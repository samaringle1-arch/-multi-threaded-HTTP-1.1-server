/*
 * test_parser.c — Unit tests for the HTTP request parser
 *
 * Build:  make test
 * Run:    ./test_parser
 *
 * Why write tests before the full server?
 * Because the parser is a pure function — you can feed it exact
 * byte strings and assert exact output. No server, no network,
 * no curl needed. This is the fastest debugging loop possible.
 *
 * When a test fails, you see EXACTLY which case broke and why.
 * When all tests pass, you know the parser is correct before
 * you plug it into the server.
 *
 * Add a new test every time you find a bug. The test suite
 * becomes your regression safety net.
 */

#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Minimal test framework                                             */
/* ------------------------------------------------------------------ */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ_STR(label, got, expected)                          \
    do {                                                             \
        tests_run++;                                                 \
        if (strcmp((got), (expected)) == 0) {                        \
            tests_passed++;                                          \
            printf("  ✓  %-40s\n", label);                          \
        } else {                                                     \
            tests_failed++;                                          \
            printf("  ✗  %-40s\n", label);                          \
            printf("       expected: \"%s\"\n", expected);           \
            printf("       got:      \"%s\"\n", got);                \
        }                                                            \
    } while (0)

#define ASSERT_EQ_INT(label, got, expected)                          \
    do {                                                             \
        tests_run++;                                                 \
        if ((got) == (expected)) {                                   \
            tests_passed++;                                          \
            printf("  ✓  %-40s\n", label);                          \
        } else {                                                     \
            tests_failed++;                                          \
            printf("  ✗  %-40s\n", label);                          \
            printf("       expected: %d\n", expected);               \
            printf("       got:      %d\n", got);                    \
        }                                                            \
    } while (0)

#define ASSERT_NOT_NULL(label, ptr)                                  \
    do {                                                             \
        tests_run++;                                                 \
        if ((ptr) != NULL) {                                         \
            tests_passed++;                                          \
            printf("  ✓  %-40s\n", label);                          \
        } else {                                                     \
            tests_failed++;                                          \
            printf("  ✗  %-40s (got NULL)\n", label);               \
        }                                                            \
    } while (0)

#define ASSERT_NULL(label, ptr)                                      \
    do {                                                             \
        tests_run++;                                                 \
        if ((ptr) == NULL) {                                         \
            tests_passed++;                                          \
            printf("  ✓  %-40s\n", label);                          \
        } else {                                                     \
            tests_failed++;                                          \
            printf("  ✗  %-40s (expected NULL, got \"%s\")\n",      \
                   label, (char*)(ptr));                             \
        }                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helper — parse a raw string and return the result                 */
/* ------------------------------------------------------------------ */
static ParseResult parse_str(const char *raw, HttpRequest *req)
{
    return parse_http_request(raw, strlen(raw), req);
}

/* ================================================================== */
/*  Test cases                                                         */
/* ================================================================== */

/* ------------------------------------------------------------------ */
static void test_simple_get(void)
{
    printf("\n[test] Simple GET request\n");

    const char *raw =
        "GET /index.html HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: TestClient/1.0\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_OK",        r,                PARSE_OK);
    ASSERT_EQ_STR ("method is GET",             req.method,       "GET");
    ASSERT_EQ_STR ("path is /index.html",       req.path,         "/index.html");
    ASSERT_EQ_STR ("query is empty",            req.query,        "");
    ASSERT_EQ_STR ("version is HTTP/1.1",       req.version,      "HTTP/1.1");
    ASSERT_EQ_INT ("header_count is 2",         req.header_count, 2);
    ASSERT_EQ_INT ("keep_alive is 1 (default)", req.keep_alive,   1);
    ASSERT_EQ_INT ("body_length is 0",         (int)req.body_length, 0);
}

/* ------------------------------------------------------------------ */
static void test_get_with_query(void)
{
    printf("\n[test] GET with query string\n");

    const char *raw =
        "GET /search?q=hello+world&lang=en HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_OK",          r,          PARSE_OK);
    ASSERT_EQ_STR ("path is /search",             req.path,   "/search");
    ASSERT_EQ_STR ("query is q=hello+world&lang=en",
                                                  req.query,  "q=hello+world&lang=en");
}

/* ------------------------------------------------------------------ */
static void test_post_with_body(void)
{
    printf("\n[test] POST with body\n");

    const char *raw =
        "POST /api/login HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: 27\r\n"
        "\r\n"
        "username=alice&password=1234";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_OK",             r,                   PARSE_OK);
    ASSERT_EQ_STR ("method is POST",                 req.method,          "POST");
    ASSERT_EQ_STR ("path is /api/login",             req.path,            "/api/login");
    ASSERT_EQ_INT ("content_length is 27",          (int)req.content_length, 27);
    ASSERT_EQ_INT ("body_length is 27",             (int)req.body_length,    27);

    /* Body pointer points into the raw buffer — check first few bytes */
    ASSERT_EQ_INT ("body starts with 'u'",
                   req.body != NULL && req.body[0] == 'u', 1);
}

/* ------------------------------------------------------------------ */
static void test_header_lookup(void)
{
    printf("\n[test] Case-insensitive header lookup\n");

    const char *raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: text/html\r\n"
        "X-Custom-Header: MyValue\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    HttpRequest req;
    parse_str(raw, &req);

    /* Exact case */
    ASSERT_NOT_NULL("find 'Host' exactly",           http_get_header(&req, "Host"));
    /* Lowercase */
    ASSERT_NOT_NULL("find 'host' lowercase",         http_get_header(&req, "host"));
    /* Mixed case */
    ASSERT_NOT_NULL("find 'CONTENT-TYPE' uppercase", http_get_header(&req, "CONTENT-TYPE"));
    /* Custom header */
    ASSERT_NOT_NULL("find 'x-custom-header'",        http_get_header(&req, "x-custom-header"));
    /* Missing header */
    ASSERT_NULL    ("missing header returns NULL",   http_get_header(&req, "Authorization"));

    /* Value correctness */
    const char *host = http_get_header(&req, "host");
    if (host) ASSERT_EQ_STR("Host value is example.com", host, "example.com");

    /* keep_alive derived field */
    ASSERT_EQ_INT ("keep_alive is 1", req.keep_alive, 1);
}

/* ------------------------------------------------------------------ */
static void test_connection_close(void)
{
    printf("\n[test] Connection: close disables keep-alive\n");

    const char *raw =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";

    HttpRequest req;
    parse_str(raw, &req);

    ASSERT_EQ_INT ("keep_alive is 0", req.keep_alive, 0);
}

/* ------------------------------------------------------------------ */
static void test_http10_defaults_to_close(void)
{
    printf("\n[test] HTTP/1.0 defaults to Connection: close\n");

    const char *raw =
        "GET / HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_OK",  r,              PARSE_OK);
    ASSERT_EQ_STR ("version is HTTP/1.0", req.version,    "HTTP/1.0");
    ASSERT_EQ_INT ("keep_alive is 0",     req.keep_alive, 0);
}

/* ------------------------------------------------------------------ */
static void test_incomplete_request(void)
{
    printf("\n[test] Incomplete request (no \\r\\n\\r\\n yet)\n");

    /* Only the first line — headers haven't arrived */
    const char *raw = "GET /index.html HTTP/1.1\r\nHost: localhos";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_INCOMPLETE", r, PARSE_INCOMPLETE);
}

/* ------------------------------------------------------------------ */
static void test_incomplete_body(void)
{
    printf("\n[test] Incomplete body (Content-Length not yet satisfied)\n");

    /*
     * Content-Length says 100 bytes are coming,
     * but we only have 5 body bytes so far.
     */
    const char *raw =
        "POST /upload HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "hello";   /* only 5 of 100 bytes arrived */

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_INCOMPLETE", r, PARSE_INCOMPLETE);
}

/* ------------------------------------------------------------------ */
static void test_malformed_no_method(void)
{
    printf("\n[test] Malformed request — no method\n");

    const char *raw =
        "/index.html HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_ERROR", r, PARSE_ERROR);
}

/* ------------------------------------------------------------------ */
static void test_malformed_bad_version(void)
{
    printf("\n[test] Malformed request — unsupported version\n");

    const char *raw =
        "GET / HTTP/2.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_ERROR", r, PARSE_ERROR);
}

/* ------------------------------------------------------------------ */
static void test_curl_real_request(void)
{
    printf("\n[test] Real curl GET request\n");

    /*
     * This is an exact copy of what curl sends to localhost:8080.
     * Copy-paste from your server terminal output in Week 1 Experiment 2.
     * Your parser must handle this correctly.
     */
    const char *raw =
        "GET /hello HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "User-Agent: curl/7.88.1\r\n"
        "Accept: */*\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_OK",       r,                PARSE_OK);
    ASSERT_EQ_STR ("method GET",               req.method,       "GET");
    ASSERT_EQ_STR ("path /hello",              req.path,         "/hello");
    ASSERT_EQ_INT ("3 headers parsed",         req.header_count, 3);

    const char *ua = http_get_header(&req, "user-agent");
    ASSERT_NOT_NULL("User-Agent header found", ua);
}

/* ------------------------------------------------------------------ */
static void test_delete_method(void)
{
    printf("\n[test] DELETE method\n");

    const char *raw =
        "DELETE /users/42 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    HttpRequest req;
    ParseResult r = parse_str(raw, &req);

    ASSERT_EQ_INT ("result is PARSE_OK",  r,          PARSE_OK);
    ASSERT_EQ_STR ("method is DELETE",    req.method,  "DELETE");
    ASSERT_EQ_STR ("path is /users/42",   req.path,    "/users/42");
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    printf("══════════════════════════════════════════════════════\n");
    printf("  HTTP Parser — Unit Tests\n");
    printf("══════════════════════════════════════════════════════\n");

    test_simple_get();
    test_get_with_query();
    test_post_with_body();
    test_header_lookup();
    test_connection_close();
    test_http10_defaults_to_close();
    test_incomplete_request();
    test_incomplete_body();
    test_malformed_no_method();
    test_malformed_bad_version();
    test_curl_real_request();
    test_delete_method();

    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0)
        printf("  (%d FAILED)", tests_failed);
    printf("\n══════════════════════════════════════════════════════\n\n");

    return tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}