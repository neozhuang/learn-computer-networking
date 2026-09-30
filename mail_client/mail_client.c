// Write a simple mail client.
// Use SMTP protocol.
#define _POSIX_C_SOURCE 200112L

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HOST "127.0.0.1"
#define PORT "1025"
#define BUF_SIZE 500

static int reply(FILE *in)
{
    char line [1024];
    int code = 0;

    while (fgets(line, sizeof line, in)) {
        fputs(line, stdout);
        if (strlen(line) >= 4 && line[3] == ' ') {
            code = atoi(line);
            break;
        }
    }
    return code;
}

static int command(FILE *in, FILE *out, int expected, const char * text)
{
    if (fputs(text, out) == EOF || fflush(out) == EOF)
        return -1;
    return reply(in) == expected ? 0 : -1;
}

int main(int argc, char *argv[])
{
    int              sfd, s;
    char             buf[BUF_SIZE];
    size_t           size;
    ssize_t          nread;
    struct addrinfo  hints;
    struct addrinfo  *result, *rp;
    FILE *in, *out;
    char line[256];

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
        sfd = socket(rp->ai_family, rp->ai_socktype,
                     rp->ai_protocol);
        if (sfd == -1)
            continue;

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1)
            break;                  /* Success */

        close(sfd);
    }

    freeaddrinfo(result);           /* No longer needed */

    if (rp == NULL) {               /* No address succeeded */
        fprintf(stderr, "Could not connect\n");
        exit(EXIT_FAILURE);
    }

    in = fdopen(sfd, "r");
    out = fdopen(dup(sfd), "w");
    if (!in || !out || reply(in) != 220 || 
        command(in, out, 250, "HELO localhost\r\n") < 0) {
        goto error;
    }

    snprintf(line, sizeof line, "MAIL FROM:<%s>\r\n", "zzz@qq.com");
    if (command(in, out, 250, line) < 0)
        goto error;
    snprintf(line, sizeof line, "RCPT TO:<%s>\r\n", "yql@qq.com");
    if (command(in, out, 250, line) < 0 ||
        command(in, out, 354, "DATA\r\n") < 0)
        goto error;

    const char *data = "From: \"zzz\" <zzz@qq.com>\r\n"
                       "To: \"yql\" <yql@qq.com>\r\n"
                       "Date: Tue, 15 Jan 2008 16:02:43 -0500\r\n"
                       "Subject: Lobster\r\n"
                       "\r\n"
                       "I miss you\r\n"
                       ".\r\n";
    if (command(in, out, 250, data) < 0 || command(in, out, 221, "QUIT\r\n") < 0)
        goto error;

    fclose(out);
    fclose(in);
    return EXIT_SUCCESS;

error:
    if (out) 
        fclose(out);
    if (in) 
        fclose(in);
    return EXIT_FAILURE;
}
