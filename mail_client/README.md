# SMTP Mail Client with STARTTLS

This directory contains three small SMTP client examples written in C:

- `mail_client.c` connects to a local SMTP server at `127.0.0.1:1025` without
  encryption.
- `mail_client_ssl.c` connects to Gmail on port 587, upgrades the connection
  with STARTTLS, authenticates with `AUTH LOGIN`, and sends a message.
- `mail_client_ssl_images.c` uses the same STARTTLS flow and adds an image as a
  MIME attachment.

## TLS behavior

The STARTTLS client uses OpenSSL and requires TLS 1.2 or newer. The exact TLS
version is negotiated with the SMTP server, so a connection may use TLS 1.2 or
TLS 1.3 depending on the installed OpenSSL library and server configuration.
The current program does not print the negotiated version.

The client verifies both the server certificate and the certificate hostname
for `smtp.gmail.com` using the system trust store.

## Build

Install a C compiler and the OpenSSL development package, then compile the TLS
client:

```bash
cc -Wall -Wextra -o mail_client_ssl mail_client_ssl.c -lssl -lcrypto
cc -Wall -Wextra -o mail_client_ssl_images mail_client_ssl_images.c -lssl -lcrypto
```

To compile the local, unencrypted example:

```bash
cc -Wall -Wextra -o mail_client mail_client.c
```

## Run the local client with Mailpit

`mail_client.c` connects to `127.0.0.1:1025`, so a local SMTP server must be
running before you start the client. You can run Mailpit with Podman:

```bash
podman run --rm -p 1025:1025 -p 8025:8025 axllent/mailpit
```

Or with Docker:

```bash
docker run --rm -p 1025:1025 -p 8025:8025 axllent/mailpit
```

In another terminal, run the client:

```bash
./mail_client
```

Open <http://127.0.0.1:8025> to view the captured message. Mailpit captures the
message locally and does not deliver it to the real recipient address.

## Configuration

Set the Gmail account credentials before running the STARTTLS client:

```bash
export SMTP_USERNAME="your-account@gmail.com"
export SMTP_PASSWORD="your-app-password"
export SMTP_FROM="your-account@gmail.com"
```

`SMTP_USERNAME` and `SMTP_PASSWORD` are required. `SMTP_FROM` is optional and
defaults to `SMTP_USERNAME`. Gmail accounts with two-step verification normally
need an app password rather than the account password.

The recipient, subject, headers, and message body are currently defined in
`mail_client_ssl.c`. Update them before compiling if you want to send a
different message.

Do not commit credentials or include them directly in the source code.

## Run

```bash
./mail_client_ssl
```

To send an image attachment:

```bash
./mail_client_ssl_images photo.jpg
```

The program prints the responses returned by the SMTP server. It does not print
the SMTP commands, credentials, or message body sent by the client.

## SMTP response codes

The main response codes in the sample output are:

| Code | Meaning |
| --- | --- |
| `220` | The SMTP service is ready, or the server is ready to start TLS. |
| `250` | The requested command completed successfully. |
| `334` | The server is requesting the next authentication value. |
| `235` | Authentication succeeded. |
| `354` | The server is ready to receive the message data. |
| `221` | The server is closing the connection. |

Lines such as `250-STARTTLS`, `250-AUTH`, and `250-SIZE` advertise server
capabilities. A hyphen after the status code means that more response lines
follow; a space marks the final line of that response.

## Example server output

```bash
zhuang@fedora:mail_client$ ./mail_client_ssl 
220 smtp.gmail.com ESMTP d9443c01a7336-2e49f74d093sm13051525ad.83 - gsmtp
250-smtp.gmail.com at your service, [49.73.179.227]
250-SIZE 35882577
250-8BITMIME
250-STARTTLS
250-ENHANCEDSTATUSCODES
250-PIPELINING
250-CHUNKING
250 SMTPUTF8
220 2.0.0 Ready to start TLS
250-smtp.gmail.com at your service, [49.73.179.227]
250-SIZE 35882577
250-8BITMIME
250-AUTH LOGIN PLAIN XOAUTH2 PLAIN-CLIENTTOKEN OAUTHBEARER XOAUTH
250-ENHANCEDSTATUSCODES
250-PIPELINING
250-CHUNKING
250 SMTPUTF8
334 VXNlcm5hbWU6
334 UGFzc3dvcmQ6
235 2.7.0 Accepted
250 2.1.0 OK d9443c01a7336-2e49f74d093sm13051525ad.83 - gsmtp
250 2.1.5 OK d9443c01a7336-2e49f74d093sm13051525ad.83 - gsmtp
354 Go ahead d9443c01a7336-2e49f74d093sm13051525ad.83 - gsmtp
250 2.0.0 OK  1790990086 d9443c01a7336-2e49f74d093sm13051525ad.83 - gsmtp
221 2.0.0 closing connection d9443c01a7336-2e49f74d093sm13051525ad.83 - gsmtp
```
