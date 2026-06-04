/*
 * http_response.c — HTTP/1.1 response builder
 */

#include "http_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sendfile.h>    /* Linux sendfile(2) */
#include <errno.h>

/* ------------------------------------------------------------------ */
/*  http_date_now                                                      */
/*  RFC 7231 §7.1.1.1 date format: "Tue, 15 Jan 2025 12:00:00 GMT"   */
/* ------------------------------------------------------------------ */
void http_date_now(char *buf, size_t size)
{
    time_t     now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}

/* ------------------------------------------------------------------ */
/*  http_response_init                                                 */
/* ------------------------------------------------------------------ */
void http_response_init(HttpResponse *res, int status, const char *reason)
{
    memset(res, 0, sizeof(*res));
    res->status = status;
    strncpy(res->reason, reason, sizeof(res->reason) - 1);

    /* Add mandatory headers that every response must carry */
    char date[64];
    http_date_now(date, sizeof(date));
    http_response_add_header(res, "Date",   date);
    http_response_add_header(res, "Server", "httpd/0.4");
}

/* ------------------------------------------------------------------ */
/*  http_response_add_header                                           */
/* ------------------------------------------------------------------ */
void http_response_add_header(HttpResponse *res,
                              const char *name, const char *value)
{
    if (res->header_count >= MAX_RESP_HEADERS) return;

    strncpy(res->headers[res->header_count].name,
            name,  RESP_HEADER_NAME_MAX - 1);
    strncpy(res->headers[res->header_count].value,
            value, RESP_HEADER_VAL_MAX  - 1);
    res->header_count++;
}

/* ------------------------------------------------------------------ */
/*  http_response_set_content_length                                   */
/* ------------------------------------------------------------------ */
void http_response_set_content_length(HttpResponse *res, off_t length)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)length);
    http_response_add_header(res, "Content-Length", buf);
}

/* ------------------------------------------------------------------ */
/*  http_response_send_headers                                         */
/*                                                                     */
/*  Serialises: "HTTP/1.1 200 OK\r\nName: Value\r\n...\r\n"          */
/*                                                                     */
/*  Returns 0 on success, -1 on write error.                          */
/* ------------------------------------------------------------------ */
int http_response_send_headers(int fd, const HttpResponse *res)
{
    /*
     * Build the entire header block into a single buffer, then send
     * it in one write() call. This avoids multiple round-trip delays
     * that would occur if we sent each header line separately with
     * Nagle's algorithm in play.
     */
    char buf[8192];
    int  len = 0;

    /* Status line */
    len += snprintf(buf + len, sizeof(buf) - len,
                    "HTTP/1.1 %d %s\r\n", res->status, res->reason);

    /* Header fields */
    for (int i = 0; i < res->header_count; i++) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        "%s: %s\r\n",
                        res->headers[i].name,
                        res->headers[i].value);
    }

    /* Mandatory blank line separating headers from body */
    len += snprintf(buf + len, sizeof(buf) - len, "\r\n");

    ssize_t sent = write(fd, buf, len);
    return (sent == len) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  http_response_send_body                                            */
/* ------------------------------------------------------------------ */
int http_response_send_body(int fd, const char *body, size_t len)
{
    size_t  remaining = len;
    ssize_t sent;

    /*
     * Loop until all bytes are sent. send() may write fewer than
     * requested if the socket buffer is temporarily full.
     */
    while (remaining > 0) {
        sent = write(fd, body + (len - remaining), remaining);
        if (sent < 0) {
            if (errno == EINTR) continue;   /* interrupted — retry */
            return -1;
        }
        remaining -= (size_t)sent;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  http_response_sendfile                                             */
/*                                                                     */
/*  Zero-copy file transfer via sendfile(2).                          */
/*                                                                     */
/*  The kernel reads from file_fd's page cache and writes directly    */
/*  into the socket's send buffer. Your process never copies the data. */
/*                                                                     */
/*  Benchmark note: for a 1 MB file:                                  */
/*   read()+write(): ~2 copies (kernel→userspace→kernel)              */
/*   sendfile():     ~0 copies in userspace — stays in kernel space   */
/* ------------------------------------------------------------------ */
int http_response_sendfile(int client_fd, int file_fd, off_t file_size)
{
    off_t   offset    = 0;
    off_t   remaining = file_size;

    while (remaining > 0) {
        /*
         * sendfile(out_fd, in_fd, &offset, count)
         * Transfers up to 'count' bytes from in_fd to out_fd starting
         * at *offset. Updates *offset to reflect bytes transferred.
         *
         * Returns bytes sent, 0 on EOF, -1 on error.
         */
        ssize_t sent = sendfile(client_fd, file_fd, &offset,
                                (size_t)remaining);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (sent == 0) break;   /* EOF */
        remaining -= sent;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  http_response_error                                                */
/*                                                                     */
/*  One-call error response. Generates a minimal HTML body so the     */
/*  browser renders something human-readable instead of a blank page. */
/* ------------------------------------------------------------------ */
void http_response_error(int fd, int status, const char *reason)
{
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html>\r\n"
        "<html><head><title>%d %s</title></head>\r\n"
        "<body><h1>%d %s</h1></body></html>\r\n",
        status, reason, status, reason);

    HttpResponse res;
    http_response_init(&res, status, reason);
    http_response_add_header(&res, "Content-Type",   "text/html");
    http_response_add_header(&res, "Connection",     "close");
    http_response_set_content_length(&res, body_len);

    http_response_send_headers(fd, &res);
    http_response_send_body(fd, body, body_len);
}