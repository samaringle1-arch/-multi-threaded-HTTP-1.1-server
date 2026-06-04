/*
 * http_parser.c — HTTP/1.1 request parser implementation
 *
 * Parsing happens in 5 steps:
 *
 *   1. Find \r\n\r\n  — the mandatory blank line that ends headers
 *   2. Parse request line — "METHOD /path?query HTTP/version"
 *   3. Parse each header line — "Name: Value\r\n"
 *   4. Derive convenience fields — keep-alive, content-length
 *   5. Point body pointer — into the raw buffer, past the blank line
 *
 * Nothing in this file touches a socket.
 */

#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  http_request_init                                                  */
/* ------------------------------------------------------------------ */
void http_request_init(HttpRequest *req)
{
    memset(req, 0, sizeof(*req));
}

/* ------------------------------------------------------------------ */
/*  str_lower_cmp — case-insensitive strcmp                            */
/* ------------------------------------------------------------------ */
static int str_lower_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 1;
        a++; b++;
    }
    return *a != *b;
}

/* ------------------------------------------------------------------ */
/*  http_get_header                                                    */
/*  Case-insensitive header lookup — HTTP/1.1 headers are case-free.  */
/* ------------------------------------------------------------------ */
const char *http_get_header(const HttpRequest *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (str_lower_cmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  parse_request_line                                                 */
/*                                                                     */
/*  Input (examples):                                                  */
/*    "GET /index.html HTTP/1.1\r\n"                                   */
/*    "POST /api/login?redirect=/ HTTP/1.1\r\n"                       */
/*    "DELETE /users/42 HTTP/1.0\r\n"                                  */
/*                                                                     */
/*  Extracts: method, path, query, version.                           */
/* ------------------------------------------------------------------ */
ParseResult parse_request_line(const char *line, size_t len,
                               HttpRequest *req)
{
    const char *p   = line;
    const char *end = line + len;

    /* --- Method --- */
    const char *sp1 = memchr(p, ' ', (size_t)(end - p));
    if (!sp1) return PARSE_ERROR;

    size_t method_len = (size_t)(sp1 - p);
    if (method_len == 0 || method_len >= MAX_METHOD_LEN)
        return PARSE_ERROR;

    memcpy(req->method, p, method_len);
    req->method[method_len] = '\0';
    p = sp1 + 1;

    /* --- Request-target (path + optional query string) --- */
    const char *sp2 = memchr(p, ' ', (size_t)(end - p));
    if (!sp2) return PARSE_ERROR;

    size_t target_len = (size_t)(sp2 - p);

    /*
     * Split the target on '?' to isolate path and query string.
     *
     *   /search?q=hello&lang=en
     *   ^path  ^query starts here (without the '?')
     */
    const char *qmark = memchr(p, '?', target_len);
    if (qmark) {
        size_t path_len  = (size_t)(qmark - p);
        size_t query_len = (size_t)(sp2   - qmark - 1);

        if (path_len  >= MAX_PATH_LEN) return PARSE_ERROR;
        if (query_len >= MAX_PATH_LEN) return PARSE_ERROR;

        memcpy(req->path,  p,        path_len);   req->path [path_len]  = '\0';
        memcpy(req->query, qmark + 1, query_len); req->query[query_len] = '\0';
    } else {
        if (target_len >= MAX_PATH_LEN) return PARSE_ERROR;
        memcpy(req->path, p, target_len);
        req->path[target_len] = '\0';
    }
    p = sp2 + 1;

    /* --- HTTP-version --- */
    size_t version_len = (size_t)(end - p);

    /* Strip trailing \r (lines may end with \r\n or just \n) */
    if (version_len > 0 && p[version_len - 1] == '\r')
        version_len--;

    if (version_len == 0 || version_len >= MAX_VERSION_LEN)
        return PARSE_ERROR;

    memcpy(req->version, p, version_len);
    req->version[version_len] = '\0';

    /*
     * Reject anything other than HTTP/1.0 and HTTP/1.1.
     * HTTP/2 and HTTP/3 are binary protocols — our parser
     * would misread them as garbage.
     */
    if (strcmp(req->version, "HTTP/1.0") != 0 &&
        strcmp(req->version, "HTTP/1.1") != 0)
        return PARSE_ERROR;

    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/*  parse_header_line                                                  */
/*                                                                     */
/*  Input (examples):                                                  */
/*    "Content-Type: application/json" + CRLF                          */
/*    "Accept: *" + CRLF                                               */
/*    "X-Custom:value-with-no-space" + CRLF                            */
/*                                                                     */
/*  RFC 7230 §3.2: field-name ":" OWS field-value OWS                 */
/*  OWS = optional whitespace (spaces and tabs)                       */
/* ------------------------------------------------------------------ */
ParseResult parse_header_line(const char *line, size_t len,
                              HttpRequest *req)
{
    if (req->header_count >= MAX_HEADERS) {
        /*
         * Silent drop is one valid strategy (nginx does this).
         * Returning PARSE_ERROR is stricter. We choose strict.
         */
        return PARSE_ERROR;
    }

    const char *colon = memchr(line, ':', len);
    if (!colon) return PARSE_ERROR;

    HttpHeader *h = &req->headers[req->header_count];

    /* Name — everything before the colon, no whitespace allowed */
    size_t name_len = (size_t)(colon - line);
    if (name_len == 0 || name_len >= MAX_HEADER_NAME) return PARSE_ERROR;
    memcpy(h->name, line, name_len);
    h->name[name_len] = '\0';

    /* Value — skip leading OWS, then copy to end of line */
    const char *value = colon + 1;
    const char *end   = line  + len;
    while (value < end && (*value == ' ' || *value == '\t'))
        value++;

    size_t value_len = (size_t)(end - value);
    /* Strip trailing \r */
    if (value_len > 0 && value[value_len - 1] == '\r')
        value_len--;

    if (value_len >= MAX_HEADER_VALUE) return PARSE_ERROR;
    memcpy(h->value, value, value_len);
    h->value[value_len] = '\0';

    req->header_count++;
    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/*  parse_http_request — main entry point                             */
/*                                                                     */
/*  Call this after EVERY recv(). Feed the entire accumulated buffer  */
/*  each time — not just the new bytes.                               */
/*                                                                     */
/*  Returns PARSE_INCOMPLETE until enough bytes have arrived.         */
/* ------------------------------------------------------------------ */
ParseResult parse_http_request(const char *raw, size_t len,
                               HttpRequest *req)
{
    http_request_init(req);

    /* ----------------------------------------------------------------
     * Step 1: Find the header/body separator — \r\n\r\n
     *
     * This is the most important check. Until we see this exact
     * four-byte sequence, we cannot know if the headers are complete.
     * Partial reads are normal — TCP does not preserve message
     * boundaries. We might see headers arrive in 3 recv() calls.
     * ---------------------------------------------------------------- */
    const char *header_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (raw[i]   == '\r' && raw[i+1] == '\n' &&
            raw[i+2] == '\r' && raw[i+3] == '\n') {
            header_end = raw + i;
            break;
        }
    }

    if (!header_end)
        return PARSE_INCOMPLETE;   /* Need more bytes */

    /* ----------------------------------------------------------------
     * Step 2: Parse the request line (first line)
     * It ends at the first \n in the buffer.
     * ---------------------------------------------------------------- */
    const char *first_lf = memchr(raw, '\n', (size_t)(header_end - raw));
    if (!first_lf)
        return PARSE_ERROR;

    ParseResult r = parse_request_line(raw,
                                       (size_t)(first_lf - raw),
                                       req);
    if (r != PARSE_OK) return r;

    /* ----------------------------------------------------------------
     * Step 3: Parse each header line
     *
     * Walk line by line through the section between the request line
     * and the blank line. Each line ends at a \n character.
     * ---------------------------------------------------------------- */
    const char *cursor = first_lf + 1;   /* Start of first header line */

    while (cursor < header_end) {
        /*
         * Bug fix: search up to header_end+2, not header_end.
         * header_end points to the \r of \r\n\r\n — the last header's
         * \n sits at header_end+1, just past a naive [cursor, header_end)
         * window. +2 brings it in range without overshooting.
         */
        const char *lf = memchr(cursor, '\n',
                                (size_t)(header_end + 2 - cursor));
        if (!lf) break;

        size_t line_len = (size_t)(lf - cursor);

        /*
         * Skip empty lines (bare \r\n or \n).
         * The blank line marking the end of headers is \r\n,
         * which appears as line_len == 1 (just the \r before \n).
         */
        if (line_len <= 1) {
            cursor = lf + 1;
            continue;
        }

        r = parse_header_line(cursor, line_len, req);
        if (r != PARSE_OK) return r;

        cursor = lf + 1;
    }

    /* ----------------------------------------------------------------
     * Step 4: Derive convenience fields
     *
     * Compute keep_alive and content_length from headers so callers
     * don't need to call http_get_header() themselves for the common
     * cases.
     * ---------------------------------------------------------------- */

    /* Content-Length */
    const char *cl_str = http_get_header(req, "Content-Length");
    if (cl_str)
        req->content_length = (size_t)strtoul(cl_str, NULL, 10);

    /*
     * Keep-alive semantics:
     *   HTTP/1.1 → persistent by default (keep_alive = 1)
     *   HTTP/1.0 → close by default      (keep_alive = 0)
     *   Either version can override with "Connection: keep-alive/close"
     */
    const char *conn = http_get_header(req, "Connection");
    if (conn) {
        req->keep_alive = (str_lower_cmp(conn, "keep-alive") == 0) ? 1 : 0;
    } else {
        req->keep_alive = (strcmp(req->version, "HTTP/1.1") == 0) ? 1 : 0;
    }

    /* ----------------------------------------------------------------
     * Step 5: Locate the body
     *
     * The body starts immediately after the \r\n\r\n separator.
     * If Content-Length says we need more bytes than have arrived,
     * report PARSE_INCOMPLETE so the caller keeps reading.
     * ---------------------------------------------------------------- */
    const char *body_start   = header_end + 4; /* skip \r\n\r\n */
    size_t      body_arrived = (size_t)((raw + len) - body_start);

    if (req->content_length > 0) {
        if (body_arrived < req->content_length)
            return PARSE_INCOMPLETE;

        req->body        = body_start;
        req->body_length = req->content_length;
    }

    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/*  http_request_print — dump all fields for debugging                */
/* ------------------------------------------------------------------ */
void http_request_print(const HttpRequest *req)
{
    printf("┌─── Parsed HTTP Request ──────────────────────────\n");
    printf("│ Method  : %s\n",   req->method);
    printf("│ Path    : %s\n",   req->path);
    printf("│ Query   : %s\n",   req->query[0] ? req->query : "(none)");
    printf("│ Version : %s\n",   req->version);
    printf("│ Keep-alive : %s\n", req->keep_alive ? "yes" : "no");
    printf("│ Headers (%d):\n",  req->header_count);
    for (int i = 0; i < req->header_count; i++) {
        printf("│   %-25s %s\n",
               req->headers[i].name, req->headers[i].value);
    }
    if (req->body_length > 0) {
        printf("│ Body (%zu bytes):\n", req->body_length);
        printf("│   %.*s\n", (int)req->body_length, req->body);
    } else {
        printf("│ Body    : (none)\n");
    }
    printf("└──────────────────────────────────────────────────\n");
}