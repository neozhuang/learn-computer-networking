// A simple mail client with SMTP protocol.

#define _POSIX_C_SOURCE 200112L

#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#define HOST "smtp.gmail.com"
#define PORT "587"
#define RECIPIENT "511726295@qq.com"
#define IO_TIMEOUT_SECONDS 15
#define MIME_BOUNDARY "----mail-client-image-boundary"

struct transport {
    int fd;
    SSL *ssl;
};

static ssize_t transport_read(struct transport *transport, void *buffer,
                              size_t length)
{
    if (transport->ssl) {
        size_t bytes_read;
        int result;

        ERR_clear_error();
        errno = 0;
        result = SSL_read_ex(transport->ssl, buffer, length, &bytes_read);

        if (result == 1)
            return (ssize_t)bytes_read;

        // error
        result = SSL_get_error(transport->ssl, result);
        if (result == SSL_ERROR_ZERO_RETURN)
            fprintf(stderr, "TLS connection closed by server\n");
        else if (result == SSL_ERROR_SYSCALL && errno != 0)
            perror("SSL_read_ex");
        else
            fprintf(stderr, "SSL_read_ex failed (SSL error %d)\n", result);
        ERR_print_errors_fp(stderr);
        return result == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }

    for (;;) {
        ssize_t bytes_read = recv(transport->fd, buffer, length, 0);

        if (bytes_read >= 0)
            return bytes_read;
        if (errno != EINTR) {
            perror("recv");
            return -1;
        }
    }
}

static int transport_write_all(struct transport *transport, const char *data,
                               size_t length)
{
    while (length > 0) {
        size_t written;

        if (transport->ssl) {
            int result;

            ERR_clear_error();
            errno = 0;
            result = SSL_write_ex(transport->ssl, data, length, &written);

            if (result != 1) {
                result = SSL_get_error(transport->ssl, result);
                if (result == SSL_ERROR_SYSCALL && errno != 0)
                    perror("SSL_write_ex");
                else
                    fprintf(stderr, "SSL_write_ex failed (SSL error %d)\n",
                            result);
                ERR_print_errors_fp(stderr);
                return -1;
            }
        } else {
            ssize_t result;

            do {
                result = send(transport->fd, data, length, MSG_NOSIGNAL);
            } while (result < 0 && errno == EINTR);

            if (result <= 0) {
                if (result < 0)
                    perror("send");
                else
                    fprintf(stderr, "send returned zero\n");
                return -1;
            }
            written = (size_t)result;
        }

        data += written;
        length -= written;
    }

    return 0;
}

static int read_smtp_response(struct transport *transport)
{
    char line[1024];
    size_t length = 0;
    int code = 0;

    for (;;) {
        char ch;

        ssize_t result = transport_read(transport, &ch, 1);

        if (result == 0) {
            fprintf(stderr, "SMTP server closed the connection\n");
            return 0;
        }
        if (result < 0)
            return 0;

        if (length + 1 < sizeof line)
            line[length++] = ch;

        if (ch == '\n') {
            line[length] = '\0';
            fputs(line, stdout);
            if (length >= 4 && line[3] == ' ') {
                code = atoi(line);
                break;
            }
            length = 0;
        }
    }
    return code;
}

static int command(struct transport *transport, int expected, const char *text)
{
    int actual;

    if (transport_write_all(transport, text, strlen(text)) < 0)
        return -1;
    actual = read_smtp_response(transport);
    if (actual != expected) {
        fprintf(stderr, "Expected SMTP status %d, received %d\n", expected,
                actual);
        return -1;
    }
    return 0;
}

struct data_writer {
    struct transport *transport;
    int line_start;
};

static int write_message_data(struct data_writer *writer, const char *data,
                              size_t length)
{
    char output[1024];
    size_t used = 0;

    for (size_t i = 0; i < length; i++) {
        if (used + 2 > sizeof output) {
            if (transport_write_all(writer->transport, output, used) < 0)
                return -1;
            used = 0;
        }
        if (writer->line_start && data[i] == '.')
            output[used++] = '.';
        output[used++] = data[i];
        writer->line_start = data[i] == '\n';
    }

    return transport_write_all(writer->transport, output, used);
}

static const char *image_content_type(const char *filename)
{
    const char *extension = strrchr(filename, '.');

    if (extension && (!strcasecmp(extension, ".jpg") ||
                      !strcasecmp(extension, ".jpeg")))
        return "image/jpeg";
    if (extension && !strcasecmp(extension, ".png"))
        return "image/png";
    if (extension && !strcasecmp(extension, ".gif"))
        return "image/gif";
    return "application/octet-stream";
}

static int send_message_data(struct transport *transport, const char *headers,
                             const char *body, FILE *image,
                             const char *filename)
{
    struct data_writer writer = {transport, 1};
    unsigned char input[57];
    char encoded[78];
    char attachment_headers[512];
    size_t count;
    int length;

    length = snprintf(attachment_headers, sizeof attachment_headers,
                      "--%s\r\n"
                      "Content-Type: text/plain; charset=UTF-8\r\n"
                      "Content-Transfer-Encoding: 7bit\r\n\r\n"
                      "%s\r\n"
                      "--%s\r\n"
                      "Content-Type: %s; name=\"%s\"\r\n"
                      "Content-Disposition: attachment; filename=\"%s\"\r\n"
                      "Content-Transfer-Encoding: base64\r\n\r\n",
                      MIME_BOUNDARY, body, MIME_BOUNDARY,
                      image_content_type(filename), filename, filename);
    if (length < 0 || (size_t)length >= sizeof attachment_headers) {
        fprintf(stderr, "Attachment name is too long\n");
        return -1;
    }

    if (write_message_data(&writer, headers, strlen(headers)) < 0 ||
        write_message_data(&writer, attachment_headers, (size_t)length) < 0)
        return -1;

    while ((count = fread(input, 1, sizeof input, image)) > 0) {
        length = EVP_EncodeBlock((unsigned char *)encoded, input, (int)count);
        encoded[length++] = '\r';
        encoded[length++] = '\n';
        if (write_message_data(&writer, encoded, (size_t)length) < 0)
            return -1;
    }
    if (ferror(image)) {
        perror("fread");
        return -1;
    }

    length = snprintf(attachment_headers, sizeof attachment_headers,
                      "--%s--\r\n", MIME_BOUNDARY);
    if (write_message_data(&writer, attachment_headers, (size_t)length) < 0 ||
        transport_write_all(transport, ".\r\n", 3) < 0)
        return -1;

    if (read_smtp_response(transport) != 250) {
        fprintf(stderr, "SMTP server rejected the message body\n");
        return -1;
    }
    return 0;
}

static int base64_line(char *output, size_t output_size, const char *input)
{
    size_t input_length = strlen(input);
    size_t encoded_length;

    if (input_length > INT_MAX)
        return -1;

    encoded_length = 4 * ((input_length + 2) / 3);

    /* Base64 data + "\r\n" + '\0' */
    if (encoded_length + 3 > output_size)
        return -1;

    if (EVP_EncodeBlock((unsigned char *)output,
                        (const unsigned char *)input,
                        (int)input_length) < 0)
        return -1;

    output[encoded_length] = '\r';
    output[encoded_length + 1] = '\n';
    output[encoded_length + 2] = '\0';

    return 0;
}

int main(int argc, char *argv[])
{
    int              sfd = -1, s;
    struct addrinfo  hints;
    struct addrinfo  *result, *rp;
    struct transport transport;
    char line[256];
    char headers[1024];
    const char *filename;
    FILE *image = NULL;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s IMAGE\n", argv[0]);
        return EXIT_FAILURE;
    }

    filename = strrchr(argv[1], '/');
    filename = filename ? filename + 1 : argv[1];
    if (filename[0] == '\0' || strpbrk(filename, "\r\n\"") != NULL) {
        fprintf(stderr, "Invalid image filename\n");
        return EXIT_FAILURE;
    }
    image = fopen(argv[1], "rb");
    if (!image) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        goto error;
    }

    /* Obtain address(es) matching host/port. */

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* Stream socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          /* Any protocol */

    s = getaddrinfo(HOST, PORT, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    /* getaddrinfo() returns a list of address structures.
              Try each address until we successfully connect(2).
              If socket(2) (or connect(2)) fails, we (close the socket
              and) try the next address. */

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        struct timeval timeout = {IO_TIMEOUT_SECONDS, 0};

        sfd = socket(rp->ai_family, rp->ai_socktype,
                     rp->ai_protocol);
        if (sfd == -1)
            continue;

        if (setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                       sizeof timeout) == -1 ||
            setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof timeout) == -1) {
            perror("setsockopt");
            close(sfd);
            sfd = -1;
            continue;
        }

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
            break;                  /* Success */

        perror("connect");
        close(sfd);
        sfd = -1;
    }

    freeaddrinfo(result);           /* No longer needed */

    if (rp == NULL) {               /* No address succeeded */
        fprintf(stderr, "Could not connect\n");
        exit(EXIT_FAILURE);
    }

    transport.fd = sfd;
    transport.ssl = NULL;
    if (read_smtp_response(&transport) != 220)
        goto error;

    if (command(&transport, 250, "EHLO localhost\r\n") < 0 ||
        command(&transport, 220, "STARTTLS\r\n") < 0)
        goto error;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        ERR_print_errors_fp(stderr);
        goto error;
    }

    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1 ||
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1) {
        ERR_print_errors_fp(stderr);
        goto error;
    }

    ssl = SSL_new(ctx);
    if (!ssl) {
        ERR_print_errors_fp(stderr);
        goto error;
    }

    /* SNI and certificate hostname verification. */
    if (SSL_set_tlsext_host_name(ssl, HOST) != 1 ||
        SSL_set1_host(ssl, HOST) != 1 ||
        SSL_set_fd(ssl, sfd) != 1) {
        ERR_print_errors_fp(stderr);
        goto error;
    }

    ERR_clear_error();
    errno = 0;
    s = SSL_connect(ssl);
    if (s != 1) {
        int ssl_error = SSL_get_error(ssl, s);

        if (ssl_error == SSL_ERROR_SYSCALL && errno != 0)
            perror("SSL_connect");
        else
            fprintf(stderr, "SSL_connect failed (SSL error %d)\n", ssl_error);
        ERR_print_errors_fp(stderr);
        goto error;
    }

    transport.ssl = ssl;

    // SMTP requires EHLO again after STARTTLS.
    if (command(&transport, 250, "EHLO localhost\r\n") < 0)
        goto error;

    const char *smtp_user = getenv("SMTP_USERNAME");
    const char *smtp_password = getenv("SMTP_PASSWORD");
    const char *smtp_from = getenv("SMTP_FROM");
    char auth_line[1024];

    if (!smtp_user || !smtp_password) {
        fprintf(stderr, "Please set SMTP_USERNAME and SMTP_PASSWORD\n");
        goto error;
    }
    if (!smtp_from || smtp_from[0] == '\0')
        smtp_from = smtp_user;

    /*
     * AUTH LOGIN
     * 334: server requests username/password
     * 235: authentication succeeded
     */
    if (command(&transport, 334, "AUTH LOGIN\r\n") < 0)
        goto error;

    if (base64_line(auth_line, sizeof auth_line, smtp_user) < 0 ||
        command(&transport, 334, auth_line) < 0)
        goto error;

    if (base64_line(auth_line, sizeof auth_line, smtp_password) < 0 ||
        command(&transport, 235, auth_line) < 0)
        goto error;

    s = snprintf(line, sizeof line, "MAIL FROM:<%s>\r\n", smtp_from);
    if (s < 0 || (size_t)s >= sizeof line) {
        fprintf(stderr, "SMTP_FROM is too long\n");
        goto error;
    }
    if (command(&transport, 250, line) < 0)
        goto error;
    s = snprintf(line, sizeof line, "RCPT TO:<%s>\r\n", RECIPIENT);
    if (s < 0 || (size_t)s >= sizeof line ||
        transport_write_all(&transport, line, strlen(line)) < 0)
        goto error;
    s = read_smtp_response(&transport);
    if (s != 250 && s != 251) {
        fprintf(stderr, "Recipient rejected with SMTP status %d\n", s);
        goto error;
    }
    if (command(&transport, 354, "DATA\r\n") < 0)
        goto error;

    s = snprintf(headers, sizeof headers,
                 "From: \"Neo\" <%s>\r\n"
                 "To: \"zzz\" <%s>\r\n"
                 "Date: Sat, 03 Oct 2026 15:10:43 -0500\r\n"
                 "Subject: Miss\r\n"
                 "MIME-Version: 1.0\r\n" 
                 "Content-Type: multipart/mixed; boundary=\"%s\"\r\n"
                 "\r\n",
                 smtp_from, RECIPIENT, MIME_BOUNDARY);
    if (s < 0 || (size_t)s >= sizeof headers) {
        fprintf(stderr, "Message headers are too long\n");
        goto error;
    }

    if (send_message_data(&transport, headers, "I miss you so much", image,
                          filename) < 0 ||
        command(&transport, 221, "QUIT\r\n") < 0)
        goto error;

    fclose(image);
    SSL_shutdown(ssl); /* 正常结束 TLS 会话 */
    SSL_free(ssl);     /* 释放单个 TLS 连接对象 */
    SSL_CTX_free(ctx); /* 释放 TLS 配置上下文 */
    close(sfd);        /* 关闭底层 TCP socket */
    return EXIT_SUCCESS;

error:
    if (image)
        fclose(image);
    if (ssl)
        SSL_free(ssl);
    if (ctx)
        SSL_CTX_free(ctx);
    if (sfd >= 0)
        close(sfd);
    return EXIT_FAILURE;
}
