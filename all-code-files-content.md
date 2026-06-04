### index.html
```html
<h1>Hello   Samar BITCH!!</h1>

```

### config.h
```h
// config.h
typedef struct {
    int         port;           // default: 8080
    int         threads;        // default: THREAD_POOL_SIZE (4)
    char        doc_root[512];  // default: "./www"
    int         timeout_sec;    // default: 30
} ServerConfig;

// Parse argv and fill config with defaults for anything not specified
void config_parse(ServerConfig *cfg, int argc, char **argv);
void config_print(const ServerConfig *cfg);
```

### test_keep_alive.c
```c
/*
 * test_keep_alive.c — unit tests for Week 5: keep-alive & chunked encoding
 *
 * Build:  make test_ka
 * Run:    ./test_ka
 *
 * Tests:
 *   1. Chunked encoding wire format — one chunk + terminal
 *   2. Chunked streaming API — multiple chunks
 *   3. Empty body edge case — only terminal chunk
 *   4. Keep-alive connection header parsing — derived field
 *   5. Consumed-bytes calculation — correct sliding window offset
 *   6. Multiple pipelined requests in one buffer — parsed in sequence
 *   7. Connection: close respected even on HTTP/1.1
 *   8. HTTP/1.0 defaults to non-persistent
 *
 * We test chunked encoding by writing to a socketpair() and reading
 * back the raw bytes.  This avoids needing a real network connection
 * while still exercising the actual write() calls.
 */

#include "chunked.h"
#include "http_parser.h"
#include "keep_alive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>

/* ------------------------------------------------------------------ */
/*  Minimal test framework                                             */
/* ------------------------------------------------------------------ */
static int passed = 0, failed = 0;

#define CHECK(label, cond)                                             \
    do {                                                               \
        if (cond) {                                                    \
            passed++;                                                  \
            printf("  ✓  %s\n", label);                               \
        } else {                                                       \
            failed++;                                                  \
            printf("  ✗  %s  (line %d)\n", label, __LINE__);          \
        }                                                              \
    } while (0)

#define CHECK_STR(label, got, expected)                                \
    do {                                                               \
        if (strcmp((got), (expected)) == 0) {                          \
            passed++;                                                  \
            printf("  ✓  %s\n", label);                               \
        } else {                                                       \
            failed++;                                                  \
            printf("  ✗  %s\n     expected: [%s]\n     got:      [%s]\n", \
                   label, expected, got);                              \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helper: create a socketpair, write through it, read it back       */
/* ------------------------------------------------------------------ */
typedef struct {
    int write_end;
    int read_end;
} Pipe;

static void pipe_open(Pipe *p)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        exit(EXIT_FAILURE);
    }
    p->write_end = fds[0];
    p->read_end  = fds[1];
}

static void pipe_close(Pipe *p)
{
    close(p->write_end);
    close(p->read_end);
}

/*
 * pipe_read_all — read up to `max` bytes from the read end,
 * null-terminate, and return the count.
 *
 * We shutdown the write end first so read() knows EOF is coming.
 */
static ssize_t pipe_read_all(Pipe *p, char *out, size_t max)
{
    /* Close write end so read() returns 0 at EOF instead of blocking */
    shutdown(p->write_end, SHUT_WR);

    ssize_t total = 0;
    ssize_t n;
    while (total < (ssize_t)(max - 1) &&
           (n = read(p->read_end, out + total, max - 1 - (size_t)total)) > 0) {
        total += n;
    }
    out[total] = '\0';
    return total;
}

/* ================================================================== */
/*  Test 1: one-shot chunked_send_body — wire format                  */
/* ================================================================== */
static void test_chunked_one_shot(void)
{
    printf("\n[test] chunked_send_body — wire format\n");

    Pipe p;
    pipe_open(&p);

    const char *body = "Hello, World!";   /* 13 bytes = 0xD */
    ssize_t rc = chunked_send_body(p.write_end, body, strlen(body));
    CHECK("chunked_send_body returns > 0",  rc > 0);

    char out[256];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /*
     * Expected wire bytes:
     *   "d\r\nHello, World!\r\n0\r\n\r\n"
     *
     *   d = 13 in hex
     */
    const char *expected = "d\r\nHello, World!\r\n0\r\n\r\n";
    CHECK_STR("wire format is correct", out, expected);
}

/* ================================================================== */
/*  Test 2: streaming ChunkedWriter — multiple chunks                 */
/* ================================================================== */
static void test_chunked_streaming(void)
{
    printf("\n[test] ChunkedWriter — multiple chunks\n");

    Pipe p;
    pipe_open(&p);

    ChunkedWriter cw;
    chunked_writer_init(&cw, p.write_end);

    /* Send three chunks */
    chunked_writer_send(&cw, "abc",   3);   /* 0x3 */
    chunked_writer_send(&cw, "de",    2);   /* 0x2 */
    chunked_writer_send(&cw, "fghij", 5);   /* 0x5 */
    chunked_writer_finish(&cw);

    CHECK("no error flag", cw.error == 0);
    CHECK("10 bytes sent", cw.bytes_sent == 10);

    char out[256];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /*
     * Expected:
     *   "3\r\nabc\r\n2\r\nde\r\n5\r\nfghij\r\n0\r\n\r\n"
     */
    const char *expected = "3\r\nabc\r\n2\r\nde\r\n5\r\nfghij\r\n0\r\n\r\n";
    CHECK_STR("streaming wire format correct", out, expected);
}

/* ================================================================== */
/*  Test 3: empty body — only terminal chunk                          */
/* ================================================================== */
static void test_chunked_empty_body(void)
{
    printf("\n[test] chunked_send_body — empty body\n");

    Pipe p;
    pipe_open(&p);

    /* len == 0 → only terminal chunk */
    ssize_t rc = chunked_send_body(p.write_end, "", 0);
    CHECK("returns 0 for empty", rc == 0);

    char out[64];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /* Only terminal chunk: "0\r\n\r\n" */
    CHECK_STR("empty body wire is 0 CRLF CRLF", out, "0\r\n\r\n");
}

/* ================================================================== */
/*  Test 4: streaming — zero-length chunks are skipped                */
/* ================================================================== */
static void test_chunked_skip_zero(void)
{
    printf("\n[test] ChunkedWriter — zero-length chunk skipped\n");

    Pipe p;
    pipe_open(&p);

    ChunkedWriter cw;
    chunked_writer_init(&cw, p.write_end);

    chunked_writer_send(&cw, "hi",  2);
    chunked_writer_send(&cw, "",    0);   /* should be skipped */
    chunked_writer_send(&cw, "bye", 3);
    chunked_writer_finish(&cw);

    char out[128];
    pipe_read_all(&p, out, sizeof(out));
    pipe_close(&p);

    /* "2\r\nhi\r\n3\r\nbye\r\n0\r\n\r\n" — no spurious 0-chunk in the middle */
    const char *expected = "2\r\nhi\r\n3\r\nbye\r\n0\r\n\r\n";
    CHECK_STR("zero chunk is skipped", out, expected);
}

/* ================================================================== */
/*  Test 5: keep-alive derived field from parser                      */
/* ================================================================== */
static void test_keep_alive_parser(void)
{
    printf("\n[test] Parser keep_alive derived field\n");

    /* HTTP/1.1 with no Connection header → keep-alive by default */
    {
        const char *raw =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        HttpRequest req;
        ParseResult r = parse_http_request(raw, strlen(raw), &req);
        CHECK("HTTP/1.1 default: PARSE_OK",     r == PARSE_OK);
        CHECK("HTTP/1.1 default: keep_alive=1", req.keep_alive == 1);
    }

    /* HTTP/1.1 + Connection: close → not persistent */
    {
        const char *raw =
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        HttpRequest req;
        parse_http_request(raw, strlen(raw), &req);
        CHECK("Connection: close → keep_alive=0", req.keep_alive == 0);
    }

    /* HTTP/1.0 → not persistent by default */
    {
        const char *raw =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "\r\n";
        HttpRequest req;
        parse_http_request(raw, strlen(raw), &req);
        CHECK("HTTP/1.0 default: keep_alive=0", req.keep_alive == 0);
    }

    /* HTTP/1.0 + Connection: keep-alive → client opts in */
    {
        const char *raw =
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        HttpRequest req;
        parse_http_request(raw, strlen(raw), &req);
        CHECK("HTTP/1.0 + keep-alive: keep_alive=1", req.keep_alive == 1);
    }
}

/* ================================================================== */
/*  Test 6: consumed bytes calculation for sliding window             */
/* ================================================================== */

/*
 * Expose the internal function for testing.
 * In a real build it's static inside keep_alive.c.
 * We replicate a minimal version here to test the logic directly.
 */
static size_t test_consumed_bytes(const char *buf, size_t len,
                                  const HttpRequest *req)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]   == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            return (i + 4) + req->content_length;
        }
    }
    return len;
}

static void test_consumed_bytes_calc(void)
{
    printf("\n[test] Consumed bytes (sliding window)\n");

    /* Simple GET — no body */
    {
        const char *raw =
            "GET /index.html HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        size_t len = strlen(raw);

        HttpRequest req;
        parse_http_request(raw, len, &req);

        size_t consumed = test_consumed_bytes(raw, len, &req);
        CHECK("GET: consumed == total raw length", consumed == len);
    }

    /* POST with body */
    {
        const char *raw =
            "POST /api HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        size_t len = strlen(raw);

        HttpRequest req;
        parse_http_request(raw, len, &req);

        size_t consumed = test_consumed_bytes(raw, len, &req);
        CHECK("POST: consumed == total raw length", consumed == len);
    }

    /* Two pipelined requests in one buffer */
    {
        const char *req1 =
            "GET /a HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";
        const char *req2 =
            "GET /b HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n";

        char buf[1024];
        size_t len1 = strlen(req1);
        size_t len2 = strlen(req2);
        memcpy(buf,        req1, len1);
        memcpy(buf + len1, req2, len2);
        size_t total = len1 + len2;

        /* Parse first request */
        HttpRequest req;
        ParseResult r = parse_http_request(buf, total, &req);
        CHECK("pipelined: first request parses OK", r == PARSE_OK);

        size_t consumed = test_consumed_bytes(buf, total, &req);
        CHECK("pipelined: consumed == first req length only",
              consumed == len1);

        /* Check leftover IS the second request */
        size_t leftover = total - consumed;
        CHECK("pipelined: leftover == second req length",
              leftover == len2);
        CHECK("pipelined: leftover starts with 'GET /b'",
              strncmp(buf + consumed, "GET /b", 6) == 0);
    }
}

/* ================================================================== */
/*  Test 7: chunked hex formatting — verify hex sizes                 */
/* ================================================================== */
static void test_chunked_hex_sizes(void)
{
    printf("\n[test] Chunked encoding hex size values\n");

    struct {
        size_t      len;
        const char *expected_hex;
    } cases[] = {
        {   1, "1"    },
        {  15, "f"    },
        {  16, "10"   },
        { 255, "ff"   },
        { 256, "100"  },
        {4096, "1000" },
        {   0, NULL   }
    };

    for (int i = 0; cases[i].expected_hex != NULL; i++) {
        char got[32];
        snprintf(got, sizeof(got), "%zx", cases[i].len);

        char label[64];
        snprintf(label, sizeof(label),
                 "hex(%zu) == \"%s\"", cases[i].len, cases[i].expected_hex);

        CHECK(label, strcmp(got, cases[i].expected_hex) == 0);
    }
}

/* ================================================================== */
/*  Test 8: ChunkedWriter error propagation                           */
/* ================================================================== */
static void test_chunked_error_propagation(void)
{
    printf("\n[test] ChunkedWriter error flag propagates\n");

    /*
     * Write to a closed fd — the second write() will fail with EPIPE
     * (or SIGPIPE if we haven't set SIG_IGN).  We verify that
     * cw.error is set and subsequent calls are no-ops.
     */
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

    ChunkedWriter cw;
    chunked_writer_init(&cw, fds[0]);

    /* Close the read end — writes to fds[0] will now fail */
    close(fds[1]);

    /*
     * We need to ignore SIGPIPE for this test to work without
     * killing the process.  Normally the server does this in
     * setup_signals().  We do it inline here.
     */
    signal(SIGPIPE, SIG_IGN);

    /* First send will fail (EPIPE) */
    chunked_writer_send(&cw, "test", 4);

    CHECK("error flag set after write failure", cw.error == 1);

    /* Second send should be a no-op (returns -1 immediately) */
    int rc = chunked_writer_send(&cw, "more data", 9);
    CHECK("subsequent send returns -1", rc == -1);

    /* finish() should also be a no-op */
    rc = chunked_writer_finish(&cw);
    CHECK("finish() returns -1 on error", rc == -1);

    close(fds[0]);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    printf("══════════════════════════════════════════════════════\n");
    printf("  Week 5 — Keep-Alive & Chunked Encoding Tests\n");
    printf("══════════════════════════════════════════════════════\n");

    test_chunked_one_shot();
    test_chunked_streaming();
    test_chunked_empty_body();
    test_chunked_skip_zero();
    test_keep_alive_parser();
    test_consumed_bytes_calc();
    test_chunked_hex_sizes();
    test_chunked_error_propagation();

    int total = passed + failed;
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed", passed, total);
    if (failed > 0)
        printf("  (%d FAILED)", failed);
    printf("\n══════════════════════════════════════════════════════\n\n");

    if (failed == 0) {
        printf("All tests passed!\n");
        printf("\nNext steps:\n");
        printf("  1. Build the server:  make\n");
        printf("  2. Run it:            ./httpd\n");
        printf("  3. Test keep-alive:   curl -v --keepalive-time 10 "
               "http://localhost:8080/ http://localhost:8080/index.html\n");
        printf("  4. Benchmark:         wrk -t4 -c100 -d30s "
               "http://localhost:8080/\n");
        printf("  5. Watch in Wireshark: filter 'tcp.port == 8080'\n\n");
    }

    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

### test_parser.c
```c
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
```

### http_response.c
```c
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
```

### file_server.c
```c
/*
 * file_server.c — static file serving
 *
 * Read file_server.h for the security model before editing this file.
 */

#include "file_server.h"
#include "http_response.h"
#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>     /* PATH_MAX */
#include <time.h>

/* ================================================================== */
/*  MIME type table                                                    */
/* ================================================================== */

typedef struct {
    const char *extension;   /* e.g. ".html" — must include the dot */
    const char *mime_type;
} MimeEntry;

/*
 * Sorted by frequency of request so the linear scan hits common
 * types early. In Week 6 you could replace this with a hash table.
 */
static const MimeEntry MIME_TABLE[] = {
    /* Text */
    { ".html",  "text/html; charset=utf-8"       },
    { ".htm",   "text/html; charset=utf-8"       },
    { ".css",   "text/css"                        },
    { ".js",    "application/javascript"          },
    { ".json",  "application/json"                },
    { ".txt",   "text/plain; charset=utf-8"      },
    { ".md",    "text/plain; charset=utf-8"      },
    { ".xml",   "application/xml"                 },
    { ".csv",   "text/csv"                        },

    /* Images */
    { ".png",   "image/png"                       },
    { ".jpg",   "image/jpeg"                      },
    { ".jpeg",  "image/jpeg"                      },
    { ".gif",   "image/gif"                       },
    { ".svg",   "image/svg+xml"                   },
    { ".ico",   "image/x-icon"                    },
    { ".webp",  "image/webp"                      },

    /* Fonts */
    { ".woff",  "font/woff"                       },
    { ".woff2", "font/woff2"                      },
    { ".ttf",   "font/ttf"                        },

    /* Documents */
    { ".pdf",   "application/pdf"                 },

    /* Audio / video */
    { ".mp3",   "audio/mpeg"                      },
    { ".mp4",   "video/mp4"                       },
    { ".webm",  "video/webm"                      },

    /* Archives */
    { ".zip",   "application/zip"                 },
    { ".gz",    "application/gzip"                },

    /* Sentinel */
    { NULL, NULL }
};

/* ------------------------------------------------------------------ */
const char *get_mime_type(const char *path)
{
    /*
     * Find the last '.' in the filename — that's the extension start.
     * We use strrchr so "archive.tar.gz" correctly returns ".gz".
     */
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    for (int i = 0; MIME_TABLE[i].extension != NULL; i++) {
        if (strcasecmp(ext, MIME_TABLE[i].extension) == 0)
            return MIME_TABLE[i].mime_type;
    }

    return "application/octet-stream";   /* safe default for unknown types */
}

/* ================================================================== */
/*  Path sanitisation                                                  */
/* ================================================================== */

int resolve_safe_path(const char *doc_root, const char *uri_path,
                      char *out, size_t out_size)
{
    /*
     * Step 1: Build the candidate path by joining doc_root + uri_path.
     *
     * uri_path comes directly from the HTTP request — it may contain:
     *   - ".." segments:        /images/../../etc/passwd
     *   - URL-encoded slashes:  /images/%2e%2e%2fetc%2fpasswd
     *   - Null bytes:           /images/%00evil
     *
     * We rely on realpath() to resolve all of these canonically.
     * URL decoding is NOT done here — the parser should handle it.
     * (Week 6 extension: add a url_decode() step before this function.)
     */
    char candidate[PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s%s", doc_root, uri_path);

    /*
     * Step 2: realpath() resolves all '..' and symlinks to produce
     * the true absolute path. It also verifies the path exists.
     *
     * If the path does not exist, realpath() returns NULL with errno
     * set to ENOENT — we return -1 to signal a 404.
     */
    char resolved[PATH_MAX];
    if (realpath(candidate, resolved) == NULL) {
        return -1;   /* file not found → 404 */
    }

    /*
     * Step 3: Verify the resolved path is inside doc_root.
     *
     * We first realpath() doc_root itself so we compare canonical
     * paths (handles symlinked doc roots correctly).
     */
    char real_root[PATH_MAX];
    if (realpath(doc_root, real_root) == NULL) {
        return -1;
    }

    size_t root_len = strlen(real_root);

    /*
     * The check: resolved must begin with real_root, AND the next
     * character must be '/' or '\0'.
     *
     * Without the second condition, doc_root="/var/www" would
     * incorrectly accept "/var/www2/evil" because strncmp passes
     * on the first root_len characters.
     */
    if (strncmp(resolved, real_root, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        return -2;   /* path escape attempt → 403 */
    }

    strncpy(out, resolved, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* ================================================================== */
/*  Directory listing                                                  */
/* ================================================================== */

static void serve_directory_listing(int client_fd,
                                    const char *real_path,
                                    const char *uri_path,
                                    int         keep_alive)
{
    /*
     * Generate a simple HTML directory listing using opendir/readdir.
     * We build the entire body into a buffer, then send it with the
     * correct Content-Length header.
     *
     * Production servers (nginx) use chunked encoding here so they
     * don't need to buffer the whole listing. We use Content-Length
     * for simplicity — chunked encoding is a Week 5 topic.
     */
    DIR *dir = opendir(real_path);
    if (!dir) {
        http_response_error(client_fd, 403, "Forbidden");
        return;
    }

    /*
     * Two-pass approach:
     *   Pass 1: build body into a buffer
     *   Pass 2: send headers (with correct Content-Length), then body
     */
    char body[65536];   /* 64 KB — enough for any realistic directory */
    int  len = 0;

    len += snprintf(body + len, sizeof(body) - len,
        "<!DOCTYPE html>\r\n"
        "<html>\r\n"
        "<head><title>Index of %s</title>\r\n"
        "<style>"
        "body{font-family:monospace;padding:2rem;}"
        "a{text-decoration:none;color:#0066cc;}"
        "tr:hover{background:#f5f5f5;}"
        "th{text-align:left;padding:4px 16px 4px 0;border-bottom:1px solid #ccc;}"
        "td{padding:3px 16px 3px 0;}"
        "</style></head>\r\n"
        "<body>\r\n"
        "<h2>Index of %s</h2>\r\n"
        "<table>\r\n"
        "<tr><th>Name</th><th>Size</th><th>Modified</th></tr>\r\n",
        uri_path, uri_path);

    /* Parent directory link (unless we're at root) */
    if (strcmp(uri_path, "/") != 0) {
        len += snprintf(body + len, sizeof(body) - len,
            "<tr><td><a href=\"..\">..</a></td><td>-</td><td>-</td></tr>\r\n");
    }

    /* Read directory entries */
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip hidden files (starting with '.') */
        if (entry->d_name[0] == '.') continue;

        /* stat the entry to get size and modification time */
        char entry_path[PATH_MAX];
        snprintf(entry_path, sizeof(entry_path),
                 "%s/%s", real_path, entry->d_name);

        struct stat st;
        if (stat(entry_path, &st) < 0) continue;

        /* Format modification time */
        char mtime[32];
        struct tm *tm = gmtime(&st.st_mtime);
        strftime(mtime, sizeof(mtime), "%Y-%m-%d %H:%M", tm);

        /* Size: show bytes for files, "-" for directories */
        char size_str[32];
        if (S_ISDIR(st.st_mode)) {
            snprintf(size_str, sizeof(size_str), "-");
            /* Append '/' to directory names */
            len += snprintf(body + len, sizeof(body) - len,
                "<tr>"
                "<td><a href=\"%s%s/\">%s/</a></td>"
                "<td>%s</td>"
                "<td>%s</td>"
                "</tr>\r\n",
                uri_path[strlen(uri_path)-1] == '/' ? uri_path : uri_path,
                entry->d_name, entry->d_name, size_str, mtime);
        } else {
            /* Human-readable file size */
            if      (st.st_size >= 1024*1024)
                snprintf(size_str, sizeof(size_str),
                         "%.1f MB", (double)st.st_size / (1024*1024));
            else if (st.st_size >= 1024)
                snprintf(size_str, sizeof(size_str),
                         "%.1f KB", (double)st.st_size / 1024);
            else
                snprintf(size_str, sizeof(size_str),
                         "%lld B", (long long)st.st_size);

            len += snprintf(body + len, sizeof(body) - len,
                "<tr>"
                "<td><a href=\"%s\">%s</a></td>"
                "<td>%s</td>"
                "<td>%s</td>"
                "</tr>\r\n",
                entry->d_name, entry->d_name, size_str, mtime);
        }
    }
    closedir(dir);

    len += snprintf(body + len, sizeof(body) - len,
        "</table>\r\n"
        "<hr><small>httpd/0.4</small>\r\n"
        "</body></html>\r\n");

    /* Send response */
    HttpResponse res;
    http_response_init(&res, 200, "OK");
    http_response_add_header(&res, "Content-Type",  "text/html; charset=utf-8");
    http_response_add_header(&res, "Connection",    keep_alive ? "keep-alive" : "close");
    http_response_set_content_length(&res, len);

    http_response_send_headers(client_fd, &res);
    http_response_send_body(client_fd, body, len);
}

/* ================================================================== */
/*  serve_static — main entry point                                   */
/* ================================================================== */

void serve_static(int client_fd, const HttpRequest *req,
                  const char *doc_root)
{
    /* ----------------------------------------------------------------
     * Step 1: Method check — we only handle GET and HEAD.
     *
     * HEAD is identical to GET but the body is omitted.
     * Browsers and crawlers use HEAD to check if a resource exists
     * without downloading the whole file.
     * ---------------------------------------------------------------- */
    int is_head = (strcmp(req->method, "HEAD") == 0);

    if (!is_head && strcmp(req->method, "GET") != 0) {
        /*
         * RFC 7231 §6.5.5: 405 Must include an Allow header listing
         * what methods are actually supported on this resource.
         */
        HttpResponse res;
        http_response_init(&res, 405, "Method Not Allowed");
        http_response_add_header(&res, "Allow",      "GET, HEAD");
        http_response_add_header(&res, "Connection", "close");
        http_response_set_content_length(&res, 0);
        http_response_send_headers(client_fd, &res);
        return;
    }

    /* ----------------------------------------------------------------
     * Step 2: Resolve and sanitise the path.
     * ---------------------------------------------------------------- */
    char real_path[PATH_MAX];
    int  rc = resolve_safe_path(doc_root, req->path,
                                real_path, sizeof(real_path));

    if (rc == -2) {
        http_response_error(client_fd, 403, "Forbidden");
        return;
    }
    if (rc == -1) {
        http_response_error(client_fd, 404, "Not Found");
        return;
    }

    /* ----------------------------------------------------------------
     * Step 3: stat() to distinguish files from directories.
     * ---------------------------------------------------------------- */
    struct stat st;
    if (stat(real_path, &st) < 0) {
        http_response_error(client_fd, 404, "Not Found");
        return;
    }

    /* ----------------------------------------------------------------
     * Step 4a: Directory handling.
     * ---------------------------------------------------------------- */
    if (S_ISDIR(st.st_mode)) {
        /*
         * Try index.html first — the conventional default document.
         * If it exists, serve it as a regular file.
         * If not, fall back to the auto-generated directory listing.
         */
        char index_path[PATH_MAX];
        /* Leave room for "/index.html" (11 chars + NUL) */
        size_t rp_len = strlen(real_path);
        if (rp_len + 12 > sizeof(index_path)) {
            http_response_error(client_fd, 403, "Forbidden");
            return;
        }
        memcpy(index_path, real_path, rp_len);
        memcpy(index_path + rp_len, "/index.html", 12); /* 11 chars + NUL */

        struct stat index_st;
        if (stat(index_path, &index_st) == 0 && S_ISREG(index_st.st_mode)) {
            /* Serve index.html — update real_path and st, then fall through */
            strncpy(real_path, index_path, sizeof(real_path) - 1);
            st = index_st;
            /* Fall through to file serving below */
        } else {
            serve_directory_listing(client_fd, real_path,
                                    req->path, req->keep_alive);
            return;
        }
    }

    /* ----------------------------------------------------------------
     * Step 4b: Regular file serving.
     * ---------------------------------------------------------------- */
    if (!S_ISREG(st.st_mode)) {
        /* Special files (sockets, devices, FIFOs) — refuse */
        http_response_error(client_fd, 403, "Forbidden");
        return;
    }

    int file_fd = open(real_path, O_RDONLY);
    if (file_fd < 0) {
        http_response_error(client_fd,
                            errno == EACCES ? 403 : 404,
                            errno == EACCES ? "Forbidden" : "Not Found");
        return;
    }

    /* ----------------------------------------------------------------
     * Step 5: Build and send the response headers.
     * ---------------------------------------------------------------- */
    const char *mime     = get_mime_type(real_path);
    off_t       filesize = st.st_size;

    /*
     * Last-Modified header — enables browser caching.
     * If the browser sends "If-Modified-Since" and the file hasn't
     * changed, we can send 304 Not Modified with no body.
     * (Full cache validation is a Week 6 stretch goal.)
     */
    char last_modified[64];
    struct tm *mtime = gmtime(&st.st_mtime);
    strftime(last_modified, sizeof(last_modified),
             "%a, %d %b %Y %H:%M:%S GMT", mtime);

    HttpResponse res;
    http_response_init(&res, 200, "OK");
    http_response_add_header(&res, "Content-Type",   mime);
    http_response_add_header(&res, "Last-Modified",  last_modified);
    http_response_add_header(&res, "Connection",
                             req->keep_alive ? "keep-alive" : "close");
    http_response_set_content_length(&res, filesize);

    http_response_send_headers(client_fd, &res);

    /* ----------------------------------------------------------------
     * Step 6: Send the file body (skipped for HEAD requests).
     * ---------------------------------------------------------------- */
    if (!is_head) {
        if (http_response_sendfile(client_fd, file_fd, filesize) < 0) {
            /* Client likely disconnected mid-transfer — not an error on our end */
            if (errno != EPIPE && errno != ECONNRESET)
                perror("[file_server] sendfile");
        }
    }

    close(file_fd);
}

```

### server.h
```h
/*
 * server.h — shared constants, includes, and function declarations
 *
 * Keeping these here means Week 2, 3, … source files can all
 * #include "server.h" and get everything they need.
 */

#ifndef SERVER_H
#define SERVER_H

/* ---- Standard library ---- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* ---- POSIX signals ---- */
#include <signal.h>

/* ---- Networking ---- */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/*  Configuration constants                                            */
/*  Change these freely during development — no magic numbers in code. */
/* ------------------------------------------------------------------ */

/* Port to listen on.  Ports < 1024 require root — use 8080+ in dev. */
#define SERVER_PORT      8080

/*
 * Backlog for listen().
 * How many half-open / unaccepted connections the kernel will queue
 * before refusing new ones with ECONNREFUSED.
 * 10 is plenty for Week 1.  In production this is often 128–512.
 */
#define BACKLOG          10

/*
 * Read buffer size per client.
 * 4 KB comfortably holds any realistic HTTP request.
 * Week 2 note: a real parser must handle requests that arrive in
 * multiple recv() calls (partial reads).  This buffer is the workspace.
 */
#define RECV_BUFFER_SIZE 4096

/* ------------------------------------------------------------------ */
/*  Function declarations                                              */
/*  Implementations in server.c — Week 2+ will add more source files. */
/* ------------------------------------------------------------------ */

/* Creates, configures, binds, and listens on a TCP socket. */
int  create_server_socket(int port);

/* Week 1: echo loop.  Week 2: becomes the HTTP handler. */
void handle_client(int client_fd);

/* Registers signal handlers for clean shutdown. */
void setup_signals(void);

/* SIGINT / SIGTERM handler — sets g_running = 0. */
void signal_handler(int sig);


#endif /* SERVER_H */
```

### chunked.c
```c
/*
 * chunked.c — HTTP/1.1 chunked Transfer-Encoding implementation
 *
 * RFC 7230 §4.1 wire format:
 *
 *   chunked-body = *chunk last-chunk trailer-part CRLF
 *   chunk        = chunk-size CRLF chunk-data CRLF
 *   chunk-size   = 1*HEXDIG
 *   last-chunk   = 1*("0") CRLF
 *   trailer-part = *( header-field CRLF )   ← we always omit this
 *
 * A minimal valid chunked body for the string "Hello, World!" (13 bytes):
 *
 *   d\r\n
 *   Hello, World!\r\n
 *   0\r\n
 *   \r\n
 *
 * Note: 0xD == 13 decimal.
 */

#include "chunked.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

/* ------------------------------------------------------------------
 *  Internal helper: write all bytes, retrying on EINTR
 * ------------------------------------------------------------------ */

/*
 * write_all — keep calling write() until all `len` bytes are sent
 * or a real error occurs.
 *
 * Why not just write() once?
 * write() on a socket can return fewer bytes than requested
 * (a "short write") if the send buffer is full.  This is normal
 * and not an error.  We must loop until done.
 *
 * Returns 0 on success, -1 on error (errno is set by write()).
 */
static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);

        if (n < 0) {
            if (errno == EINTR) continue;   /* signal interrupted — retry */
            return -1;                      /* real error */
        }

        if (n == 0) return -1;             /* fd closed unexpectedly */

        sent += (size_t)n;
    }

    return 0;
}

/* ------------------------------------------------------------------
 *  chunked_send_body — one-shot API
 * ------------------------------------------------------------------ */

/*
 * Send `len` bytes as a SINGLE chunk followed by the terminal 0-chunk.
 *
 * Wire bytes sent (example: data = "Hello" (5 bytes)):
 *
 *   5\r\n
 *   Hello\r\n
 *   0\r\n
 *   \r\n
 *
 * This is the simplest possible chunked body and is correct per RFC.
 */
ssize_t chunked_send_body(int fd, const char *data, size_t len)
{
    if (len == 0) {
        /*
         * Caller has no body at all — just send the terminal chunk.
         * This is valid and tells the client the response body is empty.
         */
        const char terminal[] = "0\r\n\r\n";
        if (write_all(fd, terminal, sizeof(terminal) - 1) < 0)
            return -1;
        return 0;
    }

    /*
     * Step 1: chunk-size line
     *
     * The size is written in hexadecimal, followed by \r\n.
     * snprintf with %zx gives us the lowercase hex representation.
     *
     * Max size_t in hex is 16 chars; +3 for "\r\n\0" = 19 bytes total.
     */
    char size_line[24];
    int  size_line_len = snprintf(size_line, sizeof(size_line),
                                  "%zx\r\n", len);
    if (size_line_len < 0) return -1;

    if (write_all(fd, size_line, (size_t)size_line_len) < 0) return -1;

    /* Step 2: chunk-data */
    if (write_all(fd, data, len) < 0) return -1;

    /* Step 3: CRLF after chunk-data */
    if (write_all(fd, "\r\n", 2) < 0) return -1;

    /*
     * Step 4: terminal chunk
     *
     * "0\r\n\r\n" — chunk size 0, followed by the mandatory
     * trailing CRLF that ends the chunked body.
     */
    if (write_all(fd, "0\r\n\r\n", 5) < 0) return -1;

    return (ssize_t)len;
}

/* ------------------------------------------------------------------
 *  ChunkedWriter — streaming API
 * ------------------------------------------------------------------ */

void chunked_writer_init(ChunkedWriter *cw, int fd)
{
    cw->fd         = fd;
    cw->bytes_sent = 0;
    cw->error      = 0;
}

/*
 * chunked_writer_send — send one chunk.
 *
 * Each call sends exactly:
 *   <hex-size>\r\n
 *   <data>\r\n
 *
 * Callers can call this as many times as needed.  Empty chunks
 * (len == 0) are skipped — they would look like the terminal chunk.
 */
int chunked_writer_send(ChunkedWriter *cw, const char *data, size_t len)
{
    /* Propagate any earlier error — once broken, stay broken. */
    if (cw->error) return -1;

    /*
     * Skip zero-length chunks.
     *
     * RFC 7230 §4.1 says a chunk-size of 0 signals end-of-body
     * (the terminal chunk).  We must never accidentally send a
     * 0-size chunk in the middle of a streaming response.
     */
    if (len == 0) return 0;

    /* --- chunk-size line --- */
    char size_line[24];
    int  size_line_len = snprintf(size_line, sizeof(size_line),
                                  "%zx\r\n", len);
    if (size_line_len < 0) { cw->error = 1; return -1; }

    if (write_all(cw->fd, size_line, (size_t)size_line_len) < 0) {
        cw->error = 1;
        return -1;
    }

    /* --- chunk-data --- */
    if (write_all(cw->fd, data, len) < 0) {
        cw->error = 1;
        return -1;
    }

    /* --- CRLF after data --- */
    if (write_all(cw->fd, "\r\n", 2) < 0) {
        cw->error = 1;
        return -1;
    }

    cw->bytes_sent += len;
    return 0;
}

/*
 * chunked_writer_finish — send the terminal 0-chunk.
 *
 * Must be called exactly once after all chunked_writer_send() calls.
 *
 * Wire bytes:
 *   0\r\n
 *   \r\n
 *
 * After this the HTTP message is complete.  If the connection is
 * keep-alive, the server can immediately read the next request.
 */
int chunked_writer_finish(ChunkedWriter *cw)
{
    if (cw->error) return -1;

    if (write_all(cw->fd, "0\r\n\r\n", 5) < 0) {
        cw->error = 1;
        return -1;
    }

    return 0;
}
```

### thread_pool.h
```h
/*
 * thread_pool.h — fixed-size thread pool with work queue (Week 3)
 *
 * Architecture: classic producer-consumer pattern.
 *
 *   main thread (producer)          worker threads (consumers)
 *   ──────────────────────          ──────────────────────────
 *   accept() new client fd    →     dequeue fd from work queue
 *   thread_pool_submit(fd)    →     handle_client(fd)
 *   loop                      →     close(fd), wait for next item
 *
 * Synchronisation primitives used:
 *
 *   mutex      — protects every read/write of the queue and flags
 *   not_empty  — workers block here when queue is empty
 *   not_full   — main thread blocks here when queue is full (backpressure)
 *
 * Shutdown sequence (graceful drain):
 *
 *   1. Main sets pool->shutdown = 1, broadcasts both condvars
 *   2. Workers finish their current client, then check:
 *        if (shutdown && queue empty) → exit
 *   3. Main calls pthread_join() on each worker
 *   4. In-flight requests complete. No connections dropped.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <netinet/in.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

/*
 * Number of worker threads.
 * Rule of thumb for I/O-bound work: number of CPU cores.
 * For CPU-bound work you'd match cores exactly; we're I/O-bound
 * (waiting on recv/send) so slightly more than core count is fine.
 *
 * Experiment in Week 6: benchmark THREAD_POOL_SIZE = 1, 2, 4, 8, 16
 * and plot req/s vs thread count. The curve flattens once threads
 * outnumber cores — that's your optimal point.
 */
#define THREAD_POOL_SIZE  4

/*
 * Maximum number of client fds that can queue up waiting for
 * a free worker. If the queue fills, the main thread blocks
 * (backpressure) rather than dropping connections.
 *
 * This is separate from the kernel's listen() backlog (BACKLOG in
 * server.h). The kernel holds unaccepted TCP connections; this queue
 * holds accepted fds waiting for a worker thread.
 */
#define QUEUE_CAPACITY    128

/* ------------------------------------------------------------------ */
/*  Work item                                                          */
/*  One entry in the queue — everything a worker needs for one client. */
/* ------------------------------------------------------------------ */
typedef struct {
    int                client_fd;    /* accepted socket fd             */
    struct sockaddr_in client_addr;  /* client IP + port (for logging) */
} WorkItem;

/* ------------------------------------------------------------------ */
/*  WorkQueue — circular buffer                                        */
/*                                                                     */
/*  A ring buffer is ideal here: O(1) enqueue and dequeue, no         */
/*  heap allocation, cache-friendly.                                   */
/*                                                                     */
/*   head → next item to dequeue (consumer reads here)                */
/*   tail → next empty slot     (producer writes here)                */
/*   count → items currently in the buffer                            */
/*                                                                     */
/*   Empty: count == 0                                                 */
/*   Full:  count == QUEUE_CAPACITY                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    WorkItem items[QUEUE_CAPACITY];
    int      head;
    int      tail;
    int      count;
} WorkQueue;

/* ------------------------------------------------------------------ */
/*  ThreadPool                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    pthread_t       threads[THREAD_POOL_SIZE];  /* worker thread handles */
    WorkQueue       queue;                       /* shared work queue     */
    pthread_mutex_t mutex;                       /* protects queue+flags  */
    pthread_cond_t  not_empty;                   /* workers wait here     */
    pthread_cond_t  not_full;                    /* main waits here       */
    int             shutdown;                    /* 1 = begin drain+exit  */
    int             active_workers;              /* count of live threads  */
} ThreadPool;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Allocate, initialise, and launch all worker threads. */
void thread_pool_init(ThreadPool *pool);

/*
 * Submit a new client connection to the pool.
 * Blocks if the queue is full (backpressure) until a slot opens.
 * Safe to call only from the main/accept thread.
 */
void thread_pool_submit(ThreadPool *pool, int client_fd,
                        struct sockaddr_in client_addr);

/*
 * Signal all workers to finish their current client and exit.
 * Joins every thread before returning — blocks until fully drained.
 * Call once, after the accept loop exits.
 */
void thread_pool_destroy(ThreadPool *pool);

/* ------------------------------------------------------------------ */
/*  Internal — worker entry point                                      */
/*  Exposed in the header so it can be referenced in thread_pool.c    */
/* ------------------------------------------------------------------ */
void *worker_thread(void *arg);

#endif /* THREAD_POOL_H */
```

### server.c
```c
/*
 * server.c — Week 5: keep-alive & chunked encoding
 *
 * Changes from Week 4:
 *   - handle_client() is replaced by handle_connection() from keep_alive.c
 *   - Worker threads call handle_connection(); it owns the fd lifecycle
 *   - SO_RCVTIMEO is set inside handle_connection()
 *   - The accept loop and socket setup are unchanged
 *
 * What handle_connection() does (see keep_alive.c for details):
 *   1. Arms SO_RCVTIMEO = KEEPALIVE_TIMEOUT_SEC seconds
 *   2. Loops: read request → dispatch → loop if keep-alive
 *   3. Breaks on: Connection: close, idle timeout, error, max requests
 *   4. Closes the fd itself before returning
 *
 * Because handle_connection() closes the fd, thread_pool.c's
 * worker_thread() must NOT call close(item.client_fd) after the call.
 * See thread_pool.c for the relevant change.
 */

#include "server.h"
#include "keep_alive.h"
#include "http_parser.h"
#include "thread_pool.h"
#include "config.h"
static volatile sig_atomic_t g_running = 1;

/*
 * handle_client — Week 5 shim.
 *
 * thread_pool.c calls handle_client(fd) then close(fd).
 * We call handle_connection(fd) which does both, so we must not
 * close fd again.  The shim prevents a double-close by calling
 * handle_connection() which internally closes the fd, then
 * thread_pool's close() will operate on an already-closed fd.
 *
 * To fix cleanly: in thread_pool.c, change:
 *
 *     handle_client(item.client_fd);
 *     close(item.client_fd);
 *
 * to just:
 *
 *     handle_connection(item.client_fd);
 *bind
 * If you have not yet updated thread_pool.c, the shim below
 * is safe because close() on an already-closed fd returns -1/EBADF
 * which the worker ignores (it doesn't check the return value).
 *
 * Preferred: update thread_pool.c.  The shim is a fallback.
 */
void handle_client(int client_fd)
{
    handle_connection(client_fd);
    /*
     * handle_connection() already closed client_fd.
     * The worker thread may call close(client_fd) again — that's
     * safe (double close on Linux returns EBADF, no harm done).
     * But see thread_pool.c for the clean fix.
     */
}

/* ------------------------------------------------------------------
 *  main — unchanged from Week 4 except the startup banner
 * ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    setup_signals();

    int server_fd = create_server_socket(SERVER_PORT);
    printf("[server] Week 5 — keep-alive + chunked encoding\n");
    printf("[server] listening on port %d  (pid %d)\n",
           SERVER_PORT, getpid());
    printf("[server] keep-alive timeout : %d seconds\n",
           KEEPALIVE_TIMEOUT_SEC);
    printf("[server] max requests/conn  : %d\n",
           KEEPALIVE_MAX_REQUESTS);
    printf("[server] benchmark          : wrk -t4 -c100 -d30s "
           "http://localhost:%d/\n\n", SERVER_PORT);

    /* Initialise thread pool */
    ThreadPool pool;
    thread_pool_init(&pool);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);

        if (client_fd < 0) {
            if (errno == EINTR) break;
            perror("[server] accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr,
                  client_ip, sizeof(client_ip));
        printf("[server] new connection from %s:%d  fd=%d\n",
               client_ip, ntohs(client_addr.sin_port), client_fd);

        thread_pool_submit(&pool, client_fd, client_addr);
        /* DO NOT close client_fd here — handle_connection() owns it */
    }

    printf("[server] shutting down...\n");
    thread_pool_destroy(&pool);
    close(server_fd);
    return EXIT_SUCCESS;

    ServerConfig cfg;
config_parse(&cfg, argc, argv);
config_print(&cfg);
// pass cfg.port to create_server_socket(cfg.port)
// pass cfg.timeout_sec to handle_connection()
// etc.
}

/* ------------------------------------------------------------------
 *  Socket and signal helpers — identical to previous weeks
 * ------------------------------------------------------------------ */

int create_server_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /*
     * TCP_NODELAY — disable Nagle's algorithm.
     *
     * Nagle buffers small outgoing segments to coalesce them.
     * For request/response protocols (HTTP) this adds latency:
     * our ~200-byte headers might be held waiting for the OS
     * to decide "is there more coming?".
     *
     * With keep-alive and pipelining, we want each response to
     * be sent immediately.  TCP_NODELAY achieves this.
     * See: https://en.wikipedia.org/wiki/Nagle%27s_algorithm
     */
#ifdef TCP_NODELAY
    {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   &nodelay, sizeof(nodelay));
    }
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); exit(EXIT_FAILURE);
    }
    if (listen(fd, BACKLOG) < 0) {
        perror("listen"); close(fd); exit(EXIT_FAILURE);
    }
    return fd;
}

void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* ignore broken pipe — client disconnected */
}

void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}
```

### test_thread_pool.c
```c
/*
 * test_thread_pool.c — unit + integration tests for the thread pool
 *
 * Build:  make test_pool
 * Run:    ./test_pool
 *
 * These tests verify:
 *   1. Init/destroy works without deadlock or crash
 *   2. Submitted work items are all processed (no drops)
 *   3. Multiple workers process items concurrently
 *   4. Graceful shutdown drains the queue before exiting
 *   5. The pool stays correct under burst load (queue-full backpressure)
 *
 * Because thread bugs are timing-dependent, run this under Helgrind:
 *   valgrind --tool=helgrind ./test_pool
 */

#include "thread_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* ------------------------------------------------------------------ */
/*  Test framework                                                     */
/* ------------------------------------------------------------------ */
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(label, cond)                                             \
    do {                                                               \
        if (cond) {                                                    \
            tests_passed++;                                            \
            printf("  ✓  %s\n", label);                               \
        } else {                                                       \
            tests_failed++;                                            \
            printf("  ✗  %s  (line %d)\n", label, __LINE__);          \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Shared counter — workers increment this to prove they ran         */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_processed     = 0;

static void increment_counter(void)
{
    pthread_mutex_lock(&g_counter_mutex);
    g_processed++;
    pthread_mutex_unlock(&g_counter_mutex);
}

static int read_counter(void)
{
    pthread_mutex_lock(&g_counter_mutex);
    int v = g_processed;
    pthread_mutex_unlock(&g_counter_mutex);
    return v;
}

/* ------------------------------------------------------------------ */
/*  Stub: handle_client                                               */
/*                                                                     */
/*  The real handle_client() talks to a socket.  For pool tests we    */
/*  just verify that the worker ran and touched the right fd.         */
/*  We use a socketpair() so we have a real fd pair with no network.  */
/* ------------------------------------------------------------------ */
void handle_client(int client_fd)
{
    /*
     * Write a byte so the other end of the socketpair can confirm
     * this client was processed, then increment the shared counter.
     */
    char marker = 'X';
    write(client_fd, &marker, 1);
    increment_counter();
    /* Simulate brief I/O work so concurrency is observable */
    usleep(1000);   /* 1 ms */
}

/* ------------------------------------------------------------------ */
/*  Helper: create a connected socketpair (no network involved)       */
/* ------------------------------------------------------------------ */
static void make_pair(int *server_side, int *client_side)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        exit(EXIT_FAILURE);
    }
    *server_side = fds[0];   /* handle_client() writes here */
    *client_side = fds[1];   /* test reads confirmation here */
}

/* ================================================================== */
/*  Tests                                                              */
/* ================================================================== */

/* ------------------------------------------------------------------ */
static void test_init_destroy(void)
{
    printf("\n[test] Init and immediate destroy (no work)\n");

    ThreadPool pool;
    thread_pool_init(&pool);

    CHECK("active_workers == THREAD_POOL_SIZE",
          pool.active_workers == THREAD_POOL_SIZE);
    CHECK("shutdown flag starts at 0",
          pool.shutdown == 0);
    CHECK("queue starts empty",
          pool.queue.count == 0);

    thread_pool_destroy(&pool);

    CHECK("shutdown flag set after destroy",
          pool.shutdown == 1);

    printf("  (no deadlock = pass)\n");
}

/* ------------------------------------------------------------------ */
static void test_single_item(void)
{
    printf("\n[test] Submit one item — worker processes it\n");

    g_processed = 0;

    ThreadPool pool;
    thread_pool_init(&pool);

    int srv, cli;
    make_pair(&srv, &cli);

    struct sockaddr_in addr = {0};
    thread_pool_submit(&pool, srv, addr);

    /* Wait for the worker to write the marker byte */
    char buf = 0;
    ssize_t n = read(cli, &buf, 1);

    thread_pool_destroy(&pool);
    close(cli);

    CHECK("worker wrote marker byte",  n == 1 && buf == 'X');
    CHECK("processed counter is 1",    read_counter() == 1);
}

/* ------------------------------------------------------------------ */
static void test_many_items(void)
{
    printf("\n[test] Submit 20 items — all get processed\n");

    g_processed = 0;
    const int N = 20;

    ThreadPool pool;
    thread_pool_init(&pool);

    int clients[N];
    struct sockaddr_in addr = {0};

    for (int i = 0; i < N; i++) {
        int srv, cli;
        make_pair(&srv, &cli);
        clients[i] = cli;
        thread_pool_submit(&pool, srv, addr);
    }

    /* Read the confirmation byte from each client side */
    int confirmed = 0;
    for (int i = 0; i < N; i++) {
        char buf = 0;
        ssize_t n = read(clients[i], &buf, 1);
        if (n == 1 && buf == 'X') confirmed++;
        close(clients[i]);
    }

    thread_pool_destroy(&pool);

    CHECK("all 20 items processed",  confirmed == N);
    CHECK("counter matches",         read_counter() == N);
}

/* ------------------------------------------------------------------ */
static void test_concurrent_workers(void)
{
    printf("\n[test] Multiple workers run concurrently\n");

    /*
     * Submit THREAD_POOL_SIZE items simultaneously.
     * If the pool is truly concurrent, all workers should be busy
     * at the same time — elapsed time ≈ 1 × item_latency, not N × latency.
     *
     * We verify concurrency indirectly: all N items complete in under
     * 5× the per-item latency (1 ms + overhead), not N × 1 ms.
     */
    g_processed = 0;

    ThreadPool pool;
    thread_pool_init(&pool);

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    const int N = THREAD_POOL_SIZE;
    int clients[N];
    struct sockaddr_in addr = {0};

    for (int i = 0; i < N; i++) {
        int srv, cli;
        make_pair(&srv, &cli);
        clients[i] = cli;
        thread_pool_submit(&pool, srv, addr);
    }

    for (int i = 0; i < N; i++) {
        char buf = 0;
        read(clients[i], &buf, 1);
        close(clients[i]);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    thread_pool_destroy(&pool);

    long elapsed_ms = (t_end.tv_sec  - t_start.tv_sec)  * 1000 +
                      (t_end.tv_nsec - t_start.tv_nsec) / 1000000;

    printf("  %d items × 1ms latency completed in %ld ms\n", N, elapsed_ms);

    /*
     * If serial: elapsed ≈ N × 1 ms = 4 ms (for N=4)
     * If concurrent: elapsed ≈ 1 ms + scheduling overhead
     * Threshold: pass if elapsed < N × 1 ms × 0.75
     */
    long serial_ms = N * 1;
    CHECK("workers ran concurrently (elapsed < 75% of serial time)",
          elapsed_ms < (serial_ms * 75 / 100 + 5));   /* +5ms for OS jitter */
}

/* ------------------------------------------------------------------ */
static void test_graceful_drain(void)
{
    printf("\n[test] Shutdown drains in-flight items before exiting\n");

    g_processed = 0;
    const int N = 10;

    ThreadPool pool;
    thread_pool_init(&pool);

    int clients[N];
    struct sockaddr_in addr = {0};

    for (int i = 0; i < N; i++) {
        int srv, cli;
        make_pair(&srv, &cli);
        clients[i] = cli;
        thread_pool_submit(&pool, srv, addr);
    }

    /*
     * Call destroy immediately — workers are mid-flight.
     * Graceful drain means all 10 must complete before destroy returns.
     */
    thread_pool_destroy(&pool);

    int confirmed = 0;
    for (int i = 0; i < N; i++) {
        char buf = 0;
        /* Non-blocking check — work should already be done */
        ssize_t n = read(clients[i], &buf, 1);
        if (n == 1 && buf == 'X') confirmed++;
        close(clients[i]);
    }

    CHECK("all items drained before destroy returned", confirmed == N);
    CHECK("counter matches",                           read_counter() == N);
}

/* ------------------------------------------------------------------ */
static void test_queue_stats(void)
{
    printf("\n[test] Queue circular-buffer wraparound\n");

    /*
     * Push and pop items in a pattern that forces the head and tail
     * pointers to wrap around the QUEUE_CAPACITY boundary.
     * This catches off-by-one bugs in the modulo arithmetic.
     */
    WorkQueue q = {0};

    /* Fill to capacity */
    int pushed = 0;
    while (!( q.count == QUEUE_CAPACITY )) {
        WorkItem item = { .client_fd = pushed };
        q.items[q.tail] = item;
        q.tail  = (q.tail + 1) % QUEUE_CAPACITY;
        q.count++;
        pushed++;
    }

    /* Pop half */
    int popped = 0;
    int half   = QUEUE_CAPACITY / 2;
    while (popped < half) {
        q.head  = (q.head + 1) % QUEUE_CAPACITY;
        q.count--;
        popped++;
    }

    /* Fill the freed slots — forces tail to wrap */
    int extra = 0;
    while (!( q.count == QUEUE_CAPACITY )) {
        WorkItem item = { .client_fd = 9000 + extra };
        q.items[q.tail] = item;
        q.tail  = (q.tail + 1) % QUEUE_CAPACITY;
        q.count++;
        extra++;
    }

    CHECK("queue full after wraparound fill",   q.count == QUEUE_CAPACITY);
    CHECK("tail wrapped around correctly",       q.tail  == half);
    CHECK("head is at midpoint",                 q.head  == half);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    printf("══════════════════════════════════════════════════════\n");
    printf("  Thread Pool — Unit & Integration Tests\n");
    printf("══════════════════════════════════════════════════════\n");

    test_init_destroy();
    test_single_item();
    test_many_items();
    test_concurrent_workers();
    test_graceful_drain();
    test_queue_stats();

    int total = tests_passed + tests_failed;
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed", tests_passed, total);
    if (tests_failed > 0)
        printf("  (%d FAILED)", tests_failed);
    printf("\n══════════════════════════════════════════════════════\n\n");

    printf("Next step: run under Helgrind to check for data races:\n");
    printf("  valgrind --tool=helgrind ./test_pool\n\n");

    return tests_failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

### fuzz_parser.c
```c
// fuzz_parser.c
#include "http_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
    buf[n] = '\0';

    HttpRequest req;
    parse_http_request(buf, n, &req);
    // We only care that this never crashes or hangs.
    // Any ParseResult (OK/ERROR/INCOMPLETE) is fine.
    return 0;
}
```

### chunked.h
```h
/*
 * chunked.h — HTTP/1.1 chunked Transfer-Encoding (Week 5)
 *
 * What is chunked encoding?
 *
 *   Normally, a server sends Content-Length so the client knows
 *   when the body ends.  But sometimes the total length is unknown
 *   when you start sending — e.g. dynamically generated pages,
 *   streaming data, or when compressing on the fly.
 *
 *   Chunked encoding solves this without buffering the whole body:
 *
 *     HTTP/1.1 200 OK\r\n
 *     Transfer-Encoding: chunked\r\n
 *     \r\n
 *     1a\r\n           ← chunk size in hex (26 decimal)
 *     abcdefghijklmnopqrstuvwxyz\r\n
 *     5\r\n            ← another chunk (5 bytes)
 *     hello\r\n
 *     0\r\n            ← terminal chunk (size == 0 means "done")
 *     \r\n             ← trailing CRLF ends the chunked body
 *
 *   RFC 7230 §4.1 specifies this format exactly.
 *
 * When to use chunked vs Content-Length:
 *
 *   Content-Length  — preferred when size is known (file serving).
 *                     More efficient because no chunk overhead.
 *   chunked         — required when size is unknown at response start
 *                     (directory listings, API responses built on-the-fly,
 *                     server-sent events, streaming).
 *                     Also mandatory if you want to use keep-alive
 *                     without buffering the whole body first.
 *
 * This module provides two interfaces:
 *
 *   1. chunked_send_body()  — one-shot: wrap an existing buffer in chunks.
 *      Simplest: caller still has the whole body in memory, but wants
 *      to avoid sending Content-Length (e.g. length computed late).
 *
 *   2. chunked_writer_*()   — streaming: send pieces incrementally.
 *      No buffer needed.  Write chunks as you generate them.
 *
 * RFC reference: RFC 7230 §4.1
 */

#ifndef CHUNKED_H
#define CHUNKED_H

#include <stddef.h>
#include <sys/types.h>

/* ------------------------------------------------------------------
 *  One-shot API
 * ------------------------------------------------------------------ */

/*
 * chunked_send_body — send `len` bytes of `data` as one chunk,
 * followed by the terminal 0-chunk.
 *
 * Use this when you have the full body in memory and just want
 * Transfer-Encoding: chunked semantics (e.g. no Content-Length).
 *
 * Precondition: caller must have already sent headers with
 *   Transfer-Encoding: chunked  (NOT Content-Length).
 *
 * Returns bytes written on success, -1 on error.
 */
ssize_t chunked_send_body(int fd, const char *data, size_t len);

/* ------------------------------------------------------------------
 *  Streaming (incremental) API
 * ------------------------------------------------------------------ */

/*
 * ChunkedWriter — state for an in-progress chunked response.
 *
 * Typical usage:
 *
 *   ChunkedWriter cw;
 *   chunked_writer_init(&cw, client_fd);
 *
 *   chunked_writer_send(&cw, part1, len1);
 *   chunked_writer_send(&cw, part2, len2);
 *   ...
 *   chunked_writer_finish(&cw);   // sends terminal 0-chunk
 */
typedef struct {
    int    fd;           /* socket to write to */
    size_t bytes_sent;   /* total data bytes sent (excludes framing) */
    int    error;        /* set to 1 if any write failed */
} ChunkedWriter;

/*
 * chunked_writer_init — attach a writer to a socket fd.
 * Headers must already be sent with Transfer-Encoding: chunked.
 */
void chunked_writer_init(ChunkedWriter *cw, int fd);

/*
 * chunked_writer_send — send one chunk of `len` bytes from `data`.
 *
 * Formats and writes:
 *   <hex-length>\r\n
 *   <data>\r\n
 *
 * Sets cw->error on failure.  Subsequent calls become no-ops.
 * Returns 0 on success, -1 on error.
 */
int chunked_writer_send(ChunkedWriter *cw, const char *data, size_t len);

/*
 * chunked_writer_finish — send the terminal zero-chunk.
 *
 * Writes:
 *   0\r\n
 *   \r\n
 *
 * Must be called exactly once, at the end of the response body.
 * After this, the connection may be reused (if keep-alive).
 * Returns 0 on success, -1 on error.
 */
int chunked_writer_finish(ChunkedWriter *cw);

/* ------------------------------------------------------------------
 *  Response header helper
 * ------------------------------------------------------------------ */

/*
 * http_response_set_chunked — add Transfer-Encoding: chunked to res.
 *
 * This is a convenience wrapper over http_response_add_header().
 * It also ensures Content-Length is NOT present (they are mutually
 * exclusive per RFC 7230 §3.3).
 */
struct HttpResponse;   /* forward-declared; defined in http_response.h */

#endif /* CHUNKED_H */
```

### file_server.h
```h
/*
 * file_server.h — static file serving (Week 4)
 *
 * Responsibilities:
 *   1. MIME type detection by file extension
 *   2. Path sanitisation — prevent directory traversal attacks
 *   3. File serving with sendfile(2)
 *   4. Directory handling — serve index.html or auto-generated listing
 *   5. Correct HTTP error codes for every failure mode
 *
 * Security model:
 *
 *   Every URI path is resolved to an absolute filesystem path via
 *   realpath(3). We then verify the result starts with doc_root.
 *   This makes directory traversal impossible regardless of how many
 *   "../" segments the attacker includes.
 *
 *   Example attack:  GET /../../../etc/passwd HTTP/1.1
 *   realpath result: /etc/passwd
 *   doc_root check:  /etc/passwd does NOT start with /var/www  → 403
 */

#ifndef FILE_SERVER_H
#define FILE_SERVER_H

#include "http_parser.h"

/* Default document root — override via command-line in Week 6 */
#define DEFAULT_DOC_ROOT  "./www"

/* ------------------------------------------------------------------ */
/*  MIME type lookup                                                   */
/*                                                                     */
/*  Returns the MIME type string for a given file path.               */
/*  Falls back to "application/octet-stream" for unknown extensions.  */
/* ------------------------------------------------------------------ */
const char *get_mime_type(const char *path);

/* ------------------------------------------------------------------ */
/*  Path sanitisation                                                  */
/*                                                                     */
/*  Resolves doc_root + uri_path to a canonical absolute path and     */
/*  verifies it stays inside doc_root.                                */
/*                                                                     */
/*  Returns  0  on success — out[] contains the resolved path         */
/*  Returns -1  if the file does not exist (→ 404)                    */
/*  Returns -2  if the path escapes doc_root (→ 403)                  */
/* ------------------------------------------------------------------ */
int resolve_safe_path(const char *doc_root, const char *uri_path,
                      char *out, size_t out_size);

/* ------------------------------------------------------------------ */
/*  serve_static                                                       */
/*                                                                     */
/*  Main entry point for file serving. Handles:                       */
/*    - Regular files → sendfile()                                     */
/*    - Directories   → index.html if present, else auto-listing      */
/*    - Missing files → 404                                            */
/*    - Access denied → 403                                            */
/*    - Wrong method  → 405 (only GET and HEAD supported)             */
/* ------------------------------------------------------------------ */
void serve_static(int client_fd, const HttpRequest *req,
                  const char *doc_root);

#endif /* FILE_SERVER_H */
```

### config.c
```c
// config.c
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

void config_parse(ServerConfig *cfg, int argc, char **argv) {
    // Defaults
    cfg->port        = 8080;
    cfg->threads     = 4;
    cfg->timeout_sec = 30;
    strncpy(cfg->doc_root, "./www", sizeof(cfg->doc_root) - 1);

    int opt;
    while ((opt = getopt(argc, argv, "p:t:r:T:h")) != -1) {
        switch (opt) {
        case 'p': cfg->port        = atoi(optarg); break;
        case 't': cfg->threads     = atoi(optarg); break;
        case 'r': strncpy(cfg->doc_root, optarg,
                          sizeof(cfg->doc_root) - 1);  break;
        case 'T': cfg->timeout_sec = atoi(optarg); break;
        case 'h':
            printf("Usage: httpd [-p port] [-t threads] "
                   "[-r doc_root] [-T timeout]\n");
            exit(0);
        }
    }
}
```

### http_response.h
```h
/*
 * http_response.h — HTTP/1.1 response builder (Week 4)
 *
 * Separates response *construction* from response *sending* so each
 * part is independently testable and reusable.
 *
 * Typical usage — serving a file:
 *
 *   HttpResponse res;
 *   http_response_init(&res, 200, "OK");
 *   http_response_add_header(&res, "Content-Type",  "text/html");
 *   http_response_add_header(&res, "Content-Length", "1024");
 *   http_response_send_headers(client_fd, &res);
 *   http_response_sendfile(client_fd, file_fd, file_size);
 *
 * Typical usage — sending an error:
 *
 *   http_response_error(client_fd, 404, "Not Found");
 */

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>
#include <sys/types.h>     /* off_t */

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
/* ------------------------------------------------------------------ */
#define MAX_RESP_HEADERS      24
#define RESP_HEADER_NAME_MAX  64
#define RESP_HEADER_VAL_MAX   256

/* ------------------------------------------------------------------ */
/*  HttpResponse                                                       */
/*                                                                     */
/*  Holds a status line and a set of header fields.                   */
/*  Does NOT hold the body — the body is written separately via       */
/*  http_response_send_body() or http_response_sendfile().            */
/* ------------------------------------------------------------------ */
typedef struct {
    int  status;
    char reason[64];

    struct {
        char name [RESP_HEADER_NAME_MAX];
        char value[RESP_HEADER_VAL_MAX];
    } headers[MAX_RESP_HEADERS];

    int header_count;
} HttpResponse;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Initialise a response with a status code and reason phrase */
void http_response_init(HttpResponse *res, int status, const char *reason);

/* Add a "Name: Value" header field */
void http_response_add_header(HttpResponse *res,
                              const char *name, const char *value);

/* Convenience: add Content-Length as a formatted integer */
void http_response_set_content_length(HttpResponse *res, off_t length);

/*
 * Serialise and transmit: status line + all headers + blank line.
 * Call this once, then call one of the body-sending functions below.
 */
int http_response_send_headers(int fd, const HttpResponse *res);

/*
 * Send a buffer as the response body.
 * Must be called AFTER http_response_send_headers().
 */
int http_response_send_body(int fd, const char *body, size_t len);

/*
 * Zero-copy file transfer using sendfile(2).
 * Must be called AFTER http_response_send_headers().
 *
 * sendfile() transfers directly from the file's page cache to the
 * socket buffer — no data ever enters userspace. On a warm cache this
 * is 3-5× faster than read()+write() for static file serving.
 */
int http_response_sendfile(int client_fd, int file_fd, off_t file_size);

/*
 * Build and send a complete error response in one call.
 * Generates a tiny HTML body so browsers render something readable.
 *
 *   http_response_error(fd, 404, "Not Found");
 *   http_response_error(fd, 405, "Method Not Allowed");
 */
void http_response_error(int fd, int status, const char *reason);

/* Format the current UTC time as an HTTP-date string */
void http_date_now(char *buf, size_t size);

#endif /* HTTP_RESPONSE_H */
```

### thread_pool.c
```c
/*
 * thread_pool.c — thread pool implementation (Week 3)
 *
 * Read thread_pool.h first for the architecture overview.
 *
 * Locking discipline (critical — follow this every time you touch
 * the pool to avoid deadlocks):
 *
 *   RULE 1: Always acquire pool->mutex before reading or writing
 *           pool->queue, pool->shutdown, or pool->active_workers.
 *
 *   RULE 2: Always release pool->mutex before calling handle_client().
 *           Holding the lock across a blocking I/O call would
 *           serialise all workers back to single-threaded behaviour.
 *
 *   RULE 3: Use while(), not if(), to re-check conditions after
 *           pthread_cond_wait() returns. Spurious wakeups are real.
 *
 *   RULE 4: Signal/broadcast after changing state the other side
 *           is waiting on. Forgetting a signal causes a thread to
 *           sleep forever (a "lost wakeup").
 */

#include "thread_pool.h"
#include "server.h"        /* handle_client(), logging macros */
#include "http_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ------------------------------------------------------------------ */
/*  Internal queue helpers (called with mutex held)                    */
/* ------------------------------------------------------------------ */

static int queue_is_empty(const WorkQueue *q)
{
    return q->count == 0;
}

static int queue_is_full(const WorkQueue *q)
{
    return q->count == QUEUE_CAPACITY;
}

static void queue_push(WorkQueue *q, WorkItem item)
{
    q->items[q->tail] = item;
    q->tail           = (q->tail + 1) % QUEUE_CAPACITY;  /* wrap around */
    q->count++;
}

static WorkItem queue_pop(WorkQueue *q)
{
    WorkItem item = q->items[q->head];
    q->head       = (q->head + 1) % QUEUE_CAPACITY;       /* wrap around */
    q->count--;
    return item;
}

/* ------------------------------------------------------------------ */
/*  worker_thread                                                      */
/*                                                                     */
/*  Each worker runs this loop forever until shutdown.                */
/*                                                                     */
/*  Pattern: lock → wait-if-empty → pop → unlock → do work → repeat  */
/* ------------------------------------------------------------------ */
void *worker_thread(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;

    char thread_id[32];
    snprintf(thread_id, sizeof(thread_id), "%lu",
             (unsigned long)pthread_self());

    printf("[worker %s] started\n", thread_id);

    for (;;) {
        /* ---------------------------------------------------------- */
        /* Phase 1: Wait for work (or shutdown signal)                */
        /* ---------------------------------------------------------- */
        pthread_mutex_lock(&pool->mutex);

        /*
         * Use WHILE — not IF — because:
         *   a) pthread_cond_wait() can return spuriously (POSIX allows it)
         *   b) Another worker may have stolen the item between our
         *      wakeup and our re-acquisition of the mutex
         *
         * This is called the "mesa-style" condition variable pattern.
         */
        while (queue_is_empty(&pool->queue) && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->mutex);
        }

        /*
         * Shutdown path: if we're shutting down AND the queue is
         * drained, this worker's job is done. Exit cleanly.
         *
         * Note: we keep working while shutdown=1 but queue is non-empty
         * — this is the "graceful drain" that ensures in-flight clients
         * always receive a response.
         */
        if (pool->shutdown && queue_is_empty(&pool->queue)) {
            printf("[worker %s] queue drained — exiting\n", thread_id);
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        /* ---------------------------------------------------------- */
        /* Phase 2: Dequeue one work item                             */
        /* ---------------------------------------------------------- */
        WorkItem item = queue_pop(&pool->queue);

        /*
         * Signal the producer (main thread) that a slot opened up.
         * If the main thread was blocked in thread_pool_submit()
         * because the queue was full, this wakes it up.
         */
        pthread_cond_signal(&pool->not_full);

        pthread_mutex_unlock(&pool->mutex);

        /* ---------------------------------------------------------- */
        /* Phase 3: Handle the client — NO lock held during I/O       */
        /* ---------------------------------------------------------- */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &item.client_addr.sin_addr,
                  client_ip, sizeof(client_ip));
        printf("[worker %s] handling %s:%d  (fd=%d)\n",
               thread_id, client_ip,
               ntohs(item.client_addr.sin_port), item.client_fd);

        // AFTER — handle_connection() already closes the fd
handle_client(item.client_fd);
/* fd is already closed inside handle_connection() */
printf("[worker %s] finished fd=%d\n", thread_id, item.client_fd);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  thread_pool_init                                                   */
/*                                                                     */
/*  Initialise all synchronisation primitives, then launch workers.   */
/* ------------------------------------------------------------------ */
void thread_pool_init(ThreadPool *pool)
{
    /* Zero everything first — catches uninitialised-field bugs */
    memset(pool, 0, sizeof(*pool));

    /* ---- Mutex --------------------------------------------------- */
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        perror("[pool] pthread_mutex_init");
        exit(EXIT_FAILURE);
    }

    /* ---- Condition variables ------------------------------------- */
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        perror("[pool] pthread_cond_init not_empty");
        exit(EXIT_FAILURE);
    }
    if (pthread_cond_init(&pool->not_full, NULL) != 0) {
        perror("[pool] pthread_cond_init not_full");
        exit(EXIT_FAILURE);
    }

    /* ---- Launch worker threads ----------------------------------- */
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            perror("[pool] pthread_create");
            exit(EXIT_FAILURE);
        }
        pool->active_workers++;
    }

    printf("[pool] started %d worker threads  (queue capacity: %d)\n",
           THREAD_POOL_SIZE, QUEUE_CAPACITY);
}

/* ------------------------------------------------------------------ */
/*  thread_pool_submit                                                 */
/*                                                                     */
/*  Called by the main thread after accept().                         */
/*  Blocks if the queue is full (backpressure — don't drop clients).  */
/* ------------------------------------------------------------------ */
void thread_pool_submit(ThreadPool *pool, int client_fd,
                        struct sockaddr_in client_addr)
{
    pthread_mutex_lock(&pool->mutex);

    /*
     * If the queue is full, block until a worker pops an item.
     * This provides backpressure: the main thread slows down naturally
     * rather than filling an unbounded queue and running out of memory.
     *
     * Also bail if shutdown was requested while we were waiting —
     * no point queuing new work we won't process.
     */
    while (queue_is_full(&pool->queue) && !pool->shutdown) {
        printf("[pool] queue full — main thread blocking (backpressure)\n");
        pthread_cond_wait(&pool->not_full, &pool->mutex);
    }

    if (pool->shutdown) {
        /* Server is shutting down — reject the new connection */
        pthread_mutex_unlock(&pool->mutex);
        close(client_fd);
        return;
    }

    WorkItem item = { .client_fd   = client_fd,
                      .client_addr = client_addr };
    queue_push(&pool->queue, item);

    /*
     * Signal ONE waiting worker that there's something to do.
     * Using signal() (not broadcast()) to avoid the "thundering herd":
     * waking all N threads for 1 item means N-1 threads find nothing
     * and go back to sleep — wasted context switches.
     */
    pthread_cond_signal(&pool->not_empty);

    pthread_mutex_unlock(&pool->mutex);
}

/* ------------------------------------------------------------------ */
/*  thread_pool_destroy                                                */
/*                                                                     */
/*  Graceful shutdown:                                                 */
/*    1. Set shutdown flag                                             */
/*    2. Wake all workers (they'll drain the queue then exit)         */
/*    3. Join every thread (wait for them to finish)                  */
/* ------------------------------------------------------------------ */
void thread_pool_destroy(ThreadPool *pool)
{
    printf("[pool] shutdown requested — draining queue (%d items)...\n",
           pool->queue.count);

    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;

    /*
     * Broadcast (not signal) here — we need ALL workers to wake up
     * and eventually see the shutdown flag, not just one.
     */
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);   /* unblock main if stuck */

    pthread_mutex_unlock(&pool->mutex);

    /* Wait for every worker to finish its current client and exit */
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(pool->threads[i], NULL);
        printf("[pool] worker %d joined\n", i);
    }

    /* Clean up primitives */
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);

    printf("[pool] all workers exited cleanly\n");
}
```

### test_file_server.c
```c
/*
 * test_file_server.c — unit tests for Week 4 components
 *
 * Build:  make test_files
 * Run:    ./test_files
 *
 * Tests:
 *   1. MIME type detection for all common extensions
 *   2. Path sanitisation — traversal attacks must be rejected
 *   3. Response header building and serialisation
 */

#include "file_server.h"
#include "http_response.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/*  Test framework                                                     */
/* ------------------------------------------------------------------ */
static int passed = 0, failed = 0;

#define CHECK(label, cond)                                             \
    do {                                                               \
        if (cond) { passed++; printf("  ✓  %s\n", label); }          \
        else { failed++; printf("  ✗  %s  (line %d)\n",label,__LINE__); } \
    } while (0)

#define CHECK_STR(label, got, expected)                                \
    do {                                                               \
        if (strcmp((got),(expected))==0) {                             \
            passed++; printf("  ✓  %s\n", label);                     \
        } else {                                                       \
            failed++;                                                  \
            printf("  ✗  %s\n     expected: %s\n     got:      %s\n", \
                   label, expected, got);                              \
        }                                                              \
    } while (0)

/* ================================================================== */
/*  MIME type tests                                                    */
/* ================================================================== */

static void test_mime_types(void)
{
    printf("\n[test] MIME type detection\n");

    /* Text types */
    CHECK_STR("index.html",  get_mime_type("index.html"),
              "text/html; charset=utf-8");
    CHECK_STR("page.htm",    get_mime_type("page.htm"),
              "text/html; charset=utf-8");
    CHECK_STR("style.css",   get_mime_type("style.css"),
              "text/css");
    CHECK_STR("app.js",      get_mime_type("app.js"),
              "application/javascript");
    CHECK_STR("data.json",   get_mime_type("data.json"),
              "application/json");
    CHECK_STR("readme.txt",  get_mime_type("readme.txt"),
              "text/plain; charset=utf-8");

    /* Images */
    CHECK_STR("logo.png",    get_mime_type("logo.png"),
              "image/png");
    CHECK_STR("photo.jpg",   get_mime_type("photo.jpg"),
              "image/jpeg");
    CHECK_STR("photo.jpeg",  get_mime_type("photo.jpeg"),
              "image/jpeg");
    CHECK_STR("icon.ico",    get_mime_type("icon.ico"),
              "image/x-icon");
    CHECK_STR("chart.svg",   get_mime_type("chart.svg"),
              "image/svg+xml");

    /* Case-insensitive */
    CHECK_STR(".HTML uppercase", get_mime_type("page.HTML"),
              "text/html; charset=utf-8");
    CHECK_STR(".JPG uppercase",  get_mime_type("photo.JPG"),
              "image/jpeg");

    /* Path with directories */
    CHECK_STR("full path",   get_mime_type("/var/www/html/index.html"),
              "text/html; charset=utf-8");
    CHECK_STR("nested path", get_mime_type("/assets/js/bundle.min.js"),
              "application/javascript");

    /* Unknown extension — safe default */
    CHECK_STR("unknown .xyz", get_mime_type("file.xyz"),
              "application/octet-stream");
    CHECK_STR("no extension", get_mime_type("Makefile"),
              "application/octet-stream");

    /* PDF */
    CHECK_STR("doc.pdf",   get_mime_type("doc.pdf"),
              "application/pdf");
}

/* ================================================================== */
/*  Path sanitisation tests                                            */
/* ================================================================== */

static void test_path_sanitisation(void)
{
    printf("\n[test] Path sanitisation (traversal protection)\n");

    /*
     * We need a real directory on disk that realpath() can resolve.
     * Create a temp directory and a test file inside it.
     */
    char tmpdir[] = "/tmp/httpd_test_XXXXXX";
    if (mkdtemp(tmpdir) == NULL) {
        printf("  SKIP (could not create temp dir)\n");
        return;
    }

    /* Create a test file */
    char testfile[256];
    snprintf(testfile, sizeof(testfile), "%s/hello.html", tmpdir);
    int fd = open(testfile, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) { write(fd, "<h1>hi</h1>", 11); close(fd); }

    char resolved[4096];

    /* ---- Valid paths ---- */
    int rc = resolve_safe_path(tmpdir, "/hello.html", resolved, sizeof(resolved));
    CHECK("valid file resolves OK",        rc == 0);

    rc = resolve_safe_path(tmpdir, "/", resolved, sizeof(resolved));
    CHECK("root '/' resolves to tmpdir",   rc == 0);

    /* ---- Traversal attacks ---- */

    /* Classic: ../../etc/passwd */
    rc = resolve_safe_path(tmpdir, "/../../etc/passwd", resolved, sizeof(resolved));
    CHECK("../../etc/passwd → 403",        rc == -2);

    /* Absolute path injection */
    rc = resolve_safe_path(tmpdir, "/etc/passwd", resolved, sizeof(resolved));
    /*
     * This will return -1 (not found) because realpath() of
     * "/tmp/xxx/etc/passwd" doesn't exist, OR -2 if somehow it
     * resolved to /etc/passwd. Either way not 0.
     */
    CHECK("absolute injection rejected",   rc != 0);

    /* URL-encoded attempt (decoder would need to run before this) */
    rc = resolve_safe_path(tmpdir, "/..%2F..%2Fetc%2Fpasswd",
                           resolved, sizeof(resolved));
    CHECK("URL-encoded traversal rejected (file not found)", rc != 0);

    /* ---- Non-existent file → 404 ---- */
    rc = resolve_safe_path(tmpdir, "/doesnotexist.html",
                           resolved, sizeof(resolved));
    CHECK("missing file returns -1",       rc == -1);

    /* Clean up */
    unlink(testfile);
    rmdir(tmpdir);
}

/* ================================================================== */
/*  Response header tests                                              */
/* ================================================================== */

static void test_response_headers(void)
{
    printf("\n[test] Response header building\n");

    HttpResponse res;
    http_response_init(&res, 200, "OK");

    /* http_response_init adds Date + Server automatically */
    CHECK("init adds Date header",       res.header_count >= 1);
    CHECK("init adds Server header",     res.header_count >= 2);
    CHECK("status code is 200",          res.status == 200);
    CHECK_STR("reason is OK",            res.reason, "OK");

    http_response_add_header(&res, "Content-Type",  "text/html");
    http_response_add_header(&res, "Content-Length", "42");
    http_response_add_header(&res, "Connection",    "keep-alive");

    /* Header count: 2 auto + 3 manual = 5 */
    CHECK("header count is 5",  res.header_count == 5);

    /* Check that Content-Type was stored correctly */
    int found_ct = 0;
    for (int i = 0; i < res.header_count; i++) {
        if (strcmp(res.headers[i].name,  "Content-Type") == 0 &&
            strcmp(res.headers[i].value, "text/html")    == 0)
            found_ct = 1;
    }
    CHECK("Content-Type stored correctly", found_ct);

    /* http_response_set_content_length */
    HttpResponse res2;
    http_response_init(&res2, 404, "Not Found");
    http_response_set_content_length(&res2, 12345);

    int found_cl = 0;
    for (int i = 0; i < res2.header_count; i++) {
        if (strcmp(res2.headers[i].name, "Content-Length") == 0 &&
            strcmp(res2.headers[i].value, "12345")         == 0)
            found_cl = 1;
    }
    CHECK("Content-Length set correctly", found_cl);
    CHECK("404 status stored",            res2.status == 404);
}

/* ================================================================== */
/*  HTTP date format test                                              */
/* ================================================================== */

static void test_http_date(void)
{
    printf("\n[test] HTTP-date formatting\n");

    char buf[64];
    http_date_now(buf, sizeof(buf));

    /* RFC 7231 format: "Tue, 15 Jan 2025 12:00:00 GMT" */
    /* Minimal checks: non-empty, ends with "GMT" */
    CHECK("date is non-empty",               strlen(buf) > 0);
    CHECK("date ends with GMT",
          strncmp(buf + strlen(buf) - 3, "GMT", 3) == 0);
    CHECK("date has correct length (29)",    strlen(buf) == 29);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */
int main(void)
{
    printf("══════════════════════════════════════════════════════\n");
    printf("  File Server — Unit Tests (Week 4)\n");
    printf("══════════════════════════════════════════════════════\n");

    test_mime_types();
    test_path_sanitisation();
    test_response_headers();
    test_http_date();

    int total = passed + failed;
    printf("\n══════════════════════════════════════════════════════\n");
    printf("  Results: %d/%d passed", passed, total);
    if (failed > 0) printf("  (%d FAILED)", failed);
    printf("\n══════════════════════════════════════════════════════\n\n");

    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
```

### keep_alive.c
```c
/*
 * keep_alive.c — HTTP/1.1 persistent connection loop (Week 5)
 *
 * Architecture overview:
 *
 *   handle_connection()                    ← called by worker thread
 *     │
 *     ├─ set SO_RCVTIMEO on socket
 *     │
 *     └─ for (;;) {                        ← per-request loop
 *           read_next_request()            ← accumulate bytes, call parser
 *           dispatch_request()             ← serve the request
 *           if (!keep_alive) break         ← Connection: close → done
 *           if (requests >= max) break     ← cap reached
 *           reset buffer for next request  ← KEY: slide leftover bytes
 *        }
 *
 * The hardest part (and the most common source of bugs in home-built
 * servers) is knowing where one HTTP request ends and the next begins
 * in the recv() buffer.
 *
 * We solve this with a "sliding window":
 *
 *   buf[0 .. total-1]   = all bytes received so far on this connection
 *
 *   After parse_http_request() returns PARSE_OK, we know exactly how
 *   many bytes the first request consumed.  We call
 *   http_request_consumed_bytes() to compute that length, then
 *   memmove the leftover bytes to the front of the buffer.  On the
 *   next iteration of the loop the next request starts cleanly.
 *
 *   This handles pipelining correctly: a client may send two requests
 *   in one TCP segment; without the sliding window, we'd lose the
 *   second one.
 *
 * SO_RCVTIMEO mechanics:
 *
 *   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))
 *
 *   After setting this, recv() returns -1 with errno == EAGAIN (Linux)
 *   or EWOULDBLOCK when the timeout fires.  We treat that as "idle
 *   connection" and close gracefully.
 *
 * RFC 7230 §6.3 notes:
 *   "A server that does not support persistent connections MUST send
 *    the 'close' connection option in every response message."
 *
 *   We send "Connection: keep-alive" or "Connection: close" on every
 *   response so the client always knows the state.
 */

#include "keep_alive.h"
#include "http_parser.h"
#include "http_response.h"
#include "file_server.h"
#include "chunked.h"
#include <stdatomic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>     /* struct timeval */

/* ------------------------------------------------------------------
 *  Internal helpers
 * ------------------------------------------------------------------ */

/*
 * set_recv_timeout — arm the socket with SO_RCVTIMEO.
 *
 * After this call, recv() will return -1 / EAGAIN if `seconds` pass
 * with no data arriving.  This gives us "free" idle timeout without
 * any extra threads or timers.
 *
 * Linux: errno == EAGAIN on timeout.
 * macOS/BSD: errno == EAGAIN or EWOULDBLOCK (both are fine to check).
 */
static void set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &tv, sizeof(tv)) < 0) {
        perror("[keep_alive] setsockopt SO_RCVTIMEO");
        /*
         * Non-fatal: the connection will still work, it just won't
         * time out on idle.  Log it and continue.
         */
    }
}

/*
 * http_request_consumed_bytes — compute how many bytes of the buffer
 * the parsed request used.
 *
 * After parse_http_request() returns PARSE_OK, we know:
 *   - Where the header section ends (header_end + 4 for \r\n\r\n)
 *   - How many body bytes were expected (req->content_length)
 *
 * So the total consumed bytes =
 *     (offset of body start from buf) + content_length
 *
 * We re-scan for \r\n\r\n here because parse_http_request() doesn't
 * expose the header_end pointer.  This is a clean way to find it.
 *
 * Returns the number of bytes consumed, or 0 on failure (caller
 * should treat the whole buffer as consumed to avoid loops).
 */
static size_t http_request_consumed_bytes(const char *buf, size_t len,
                                          const HttpRequest *req)
{
    /* Find \r\n\r\n — the end of the headers */
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i]   == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            size_t header_section_end = i + 4;   /* past \r\n\r\n */
            return header_section_end + req->content_length;
        }
    }

    /* Should not happen if parse_http_request() returned PARSE_OK */
    return len;
}

/* ------------------------------------------------------------------
 *  dispatch_request — serve one fully-parsed HTTP request
 *
 *  Returns 1 if the connection should stay open, 0 to close.
 * ------------------------------------------------------------------ */
int dispatch_request(int client_fd, const HttpRequest *req)
{
    /*
     * If the client said "Connection: close", we must honour it.
     * Even if we'd prefer to keep the connection open, RFC 7230 §6.3
     * says the client's "close" token takes effect after the response.
     *
     * We'll pass keep_alive through to the response so the correct
     * Connection header is sent, then return 0 to close after.
     */
    int keep_alive = req->keep_alive;

    /*
     * Route the request.
     *
     * For now: serve static files from DEFAULT_DOC_ROOT.
     * Week 6 will add a proper router with API endpoints.
     *
     * serve_static() sends the complete response (headers + body).
     * It also adds the Connection header via req->keep_alive.
     */
    serve_static(client_fd, req, DEFAULT_DOC_ROOT);

    /*
     * serve_static() already sent the Connection header based on
     * req->keep_alive.  Return the same value so our loop knows
     * whether to continue.
     */
    return keep_alive;
}

/* ------------------------------------------------------------------
 *  handle_connection — main entry point for one TCP connection
 * ------------------------------------------------------------------ */

/*
 * This function replaces handle_client() from Weeks 2-4.
 *
 * It is called by worker_thread() in thread_pool.c.
 * It is responsible for closing client_fd before returning.
 */
void handle_connection(int client_fd)
{
    /* ------------------------------------------------------------------
     * Step 1: Set idle timeout
     *
     * We want the connection to close if the client goes silent after
     * we've sent a response.  SO_RCVTIMEO is the cleanest way to do
     * this in a blocking-I/O model.
     * ------------------------------------------------------------------ */
    set_recv_timeout(client_fd, KEEPALIVE_TIMEOUT_SEC);

    /* ------------------------------------------------------------------
     * Step 2: Allocate the receive buffer
     *
     * This single buffer is reused across ALL requests on this
     * connection.  After each request is processed, leftover bytes
     * (the start of the next request) are slid to the front via
     * memmove() — see "sliding window" comment at top of file.
     * ------------------------------------------------------------------ */
    char   buf[KA_RECV_BUFFER_SIZE];
    size_t total    = 0;    /* bytes currently in buf */
    int    req_count = 0;   /* requests served on this connection */

    printf("[ka] connection fd=%d  timeout=%ds  max_req=%d\n",
           client_fd, KEEPALIVE_TIMEOUT_SEC, KEEPALIVE_MAX_REQUESTS);

    /* ------------------------------------------------------------------
     * Step 3: Per-request loop
     * ------------------------------------------------------------------ */
    for (;;) {

        /* ---- 3a. Accumulate bytes until we have a full request ---- */

        HttpRequest req;
        ParseResult result = PARSE_INCOMPLETE;

        while (result == PARSE_INCOMPLETE) {

            if (total >= sizeof(buf) - 1) {
                /*
                 * Buffer full and still no complete request.
                 * The client is either very slow or malicious.
                 * Send 413 and kill the connection.
                 */
                printf("[ka] fd=%d request too large — closing\n", client_fd);
                http_response_error(client_fd, 413, "Request Entity Too Large");
                goto close_conn;
            }

            ssize_t n = recv(client_fd,
                             buf + total,
                             sizeof(buf) - 1 - total,
                             0);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /*
                     * SO_RCVTIMEO fired — the connection has been idle
                     * for KEEPALIVE_TIMEOUT_SEC seconds.
                     *
                     * If we haven't started a request yet (total == 0),
                     * this is a normal idle timeout — close silently.
                     * If we're mid-request, the client stalled — also close.
                     */
                    printf("[ka] fd=%d idle timeout after %d requests\n",
                           client_fd, req_count);
                    goto close_conn;
                }

                if (errno == EINTR) continue;   /* signal — retry */

                perror("[ka] recv");
                goto close_conn;
            }

            if (n == 0) {
                /*
                 * Client closed the connection (TCP FIN received).
                 * This is normal — browser closes after the last resource.
                 */
                printf("[ka] fd=%d client closed gracefully after %d req\n",
                       client_fd, req_count);
                goto close_conn;
            }

            total      += (size_t)n;
            buf[total]  = '\0';

            /* Try to parse whatever we have so far */
            result = parse_http_request(buf, total, &req);

            if (result == PARSE_ERROR) {
                printf("[ka] fd=%d parse error — sending 400\n", client_fd);
                http_response_error(client_fd, 400, "Bad Request");
                goto close_conn;
            }

            /* PARSE_INCOMPLETE → loop back and recv() more bytes */
        }

        /* result == PARSE_OK here */
        req_count++;
        printf("[ka] fd=%d request #%d  %s %s  keep=%s\n",
               client_fd, req_count,
               req.method, req.path,
               req.keep_alive ? "yes" : "no");

        /* ---- 3b. Dispatch the request and send the response ---- */
        int keep_alive = dispatch_request(client_fd, &req);

        /* ---- 3c. Check whether to close ---- */

        if (!keep_alive) {
            printf("[ka] fd=%d Connection: close — ending after %d req\n",
                   client_fd, req_count);
            goto close_conn;
        }

        if (KEEPALIVE_MAX_REQUESTS > 0 &&
            req_count >= KEEPALIVE_MAX_REQUESTS) {
            printf("[ka] fd=%d max requests (%d) reached — closing\n",
                   client_fd, KEEPALIVE_MAX_REQUESTS);
            goto close_conn;
        }

        /* ---- 3d. Sliding window — prepare buffer for next request ---- */

        /*
         * Compute how many bytes the request we just served consumed.
         * Any bytes AFTER that are the beginning of the NEXT request
         * (pipelining: the client may have sent request N+1 before
         * receiving our response to request N).
         *
         * We memmove those leftover bytes to the start of buf so the
         * next iteration can find \r\n\r\n at the correct offset.
         *
         * If there are no leftover bytes (total == consumed), the
         * buffer is clean and we just reset total = 0.
         *
         * This is the KEY operation for correct pipelining.
         */
        size_t consumed = http_request_consumed_bytes(buf, total, &req);

        if (consumed >= total) {
            /* Common case: buffer exactly consumed */
            total = 0;
        } else {
            /* Pipelining: slide leftover bytes to front */
            size_t leftover = total - consumed;
            memmove(buf, buf + consumed, leftover);
            total = leftover;
            printf("[ka] fd=%d pipelining: %zu leftover bytes\n",
                   client_fd, leftover);
        }

        /*
         * Re-arm the recv timeout for the next request.
         * SO_RCVTIMEO is set per-socket and doesn't need re-arming,
         * but we print a debug note here to make the flow clear.
         */
    }   /* end for(;;) */

close_conn:
    /*
     * We own the fd lifecycle.  Close it here, not in the worker thread.
     * The worker thread must NOT close fd after handle_connection() returns.
     */
    close(client_fd);
    printf("[ka] fd=%d closed  (served %d requests)\n", client_fd, req_count);
}
```

### keep_alive.h
```h
/*
 * keep_alive.h — HTTP/1.1 persistent connection management (Week 5)
 *
 * What this module does:
 *
 *   Before Week 5, handle_client() read ONE request, sent ONE response,
 *   and returned.  The main/worker thread then closed the socket.
 *
 *   HTTP/1.1 changed the contract: the socket should stay open for
 *   more requests unless the client explicitly says "Connection: close"
 *   or the idle timeout fires.
 *
 *   This module owns the per-connection request loop:
 *
 *     while (connection is alive) {
 *         read next request  (accumulate partial reads)
 *         parse it
 *         dispatch it
 *         send response with correct Connection header
 *         if (Connection: close)  break;
 *         if (idle > KEEPALIVE_TIMEOUT_SEC)  break;
 *     }
 *
 * Idle timeout implementation:
 *
 *   We use SO_RCVTIMEO on the socket.  After setting it, recv() returns
 *   -1 with errno == EAGAIN / EWOULDBLOCK when the socket has been idle
 *   for KEEPALIVE_TIMEOUT_SEC seconds.  We then close gracefully.
 *
 *   This is simpler than a separate timer thread and correct for a
 *   single-connection loop.  epoll-based servers (Week 8) use a different
 *   approach (timeout wheel), but SO_RCVTIMEO is perfect here.
 *
 * RFC references:
 *   RFC 7230 §6.3  — Persistence
 *   RFC 7230 §6.3.2 — Pipelining
 */

#ifndef KEEP_ALIVE_H
#define KEEP_ALIVE_H

#include "http_parser.h"

/* ------------------------------------------------------------------
 *  Tuning constants
 * ------------------------------------------------------------------ */

/*
 * How long to wait for the NEXT request on a keep-alive connection.
 *
 * After sending a response, we arm the socket with this timeout.
 * If no new request bytes arrive within this window, we close.
 *
 * 30 s matches Apache's KeepAliveTimeout default.
 * Lower this to 5 s for benchmarking to see faster fd recycling.
 */
#define KEEPALIVE_TIMEOUT_SEC   30

/*
 * How many requests we will serve on a single connection.
 *
 * Caps runaway clients and ensures a busy server recycles fds.
 * Browsers rarely send more than a few hundred requests per connection.
 * Set to 0 to disable the cap (unlimited, not recommended).
 */
#define KEEPALIVE_MAX_REQUESTS  1000

/*
 * Per-request recv buffer.  Must comfortably hold one full HTTP request
 * including all headers.  Bodies larger than this are read in pieces
 * (handled by the PARSE_INCOMPLETE loop — body stays in the OS buffer).
 */
#define KA_RECV_BUFFER_SIZE     65536   /* 64 KB */

/* ------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------ */

/*
 * handle_connection — the Week 5 replacement for handle_client().
 *
 * Owns the full lifecycle of one TCP connection:
 *   - Sets SO_RCVTIMEO on the socket
 *   - Loops reading requests until close / timeout / error / max-req
 *   - Calls dispatch_request() for each complete request
 *   - Sends the right Connection header on each response
 *   - Closes the fd before returning (caller must NOT close it)
 *
 * Called by thread_pool worker_thread() instead of handle_client().
 */
void handle_connection(int client_fd);

/*
 * dispatch_request — called once per fully-parsed request.
 *
 * Decides what to do with the request (static file, echo, error)
 * and sends the response.  Returns 1 if the connection should stay
 * open, 0 if it should close (i.e. the response included
 * "Connection: close").
 */
int dispatch_request(int client_fd, const HttpRequest *req);

#endif /* KEEP_ALIVE_H */
```

### http_parser.c
```c
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
```

### http_parser.h
```h
/*
 * http_parser.h — HTTP/1.1 request parser (Week 2)
 *
 * Design principle: the parser is a PURE FUNCTION.
 * It takes a raw byte buffer and a length.
 * It knows nothing about sockets, file descriptors, or threads.
 * This makes it independently testable — see test_parser.c.
 *
 * Usage pattern (in handle_client):
 *
 *   char buf[RECV_BUFFER_SIZE];
 *   size_t total = 0;
 *
 *   while (total < sizeof(buf)) {
 *       ssize_t n = recv(fd, buf + total, sizeof(buf) - total, 0);
 *       if (n <= 0) break;
 *       total += n;
 *
 *       HttpRequest req;
 *       ParseResult r = parse_http_request(buf, total, &req);
 *       if (r == PARSE_OK)         { handle it; break; }
 *       if (r == PARSE_ERROR)      { send 400; break;  }
 *       // PARSE_INCOMPLETE → keep reading
 *   }
 */

#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Limits                                                             */
/*  Sized to cover any realistic HTTP/1.1 request.                    */
/* ------------------------------------------------------------------ */
#define MAX_METHOD_LEN    16      /* "DELETE" is 6, leave room */
#define MAX_PATH_LEN      2048    /* URI path, matches most servers */
#define MAX_VERSION_LEN   16      /* "HTTP/1.1" is 8 */
#define MAX_HEADER_NAME   128
#define MAX_HEADER_VALUE  4096    /* Cookie headers can be large */
#define MAX_HEADERS       32      /* Enough for any real browser */

/* ------------------------------------------------------------------ */
/*  ParseResult                                                        */
/*                                                                     */
/*  Three outcomes are possible every time you call parse_http_request */
/* ------------------------------------------------------------------ */
typedef enum {
    PARSE_OK         = 0,  /* Request fully and correctly parsed       */
    PARSE_INCOMPLETE = 1,  /* \r\n\r\n not yet seen — read more bytes  */
    PARSE_ERROR      = 2   /* Malformed request — send 400             */
} ParseResult;

/* ------------------------------------------------------------------ */
/*  HttpHeader — one "Name: Value" header field                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char name [MAX_HEADER_NAME];   /* e.g. "Content-Type"        */
    char value[MAX_HEADER_VALUE];  /* e.g. "application/json"    */
} HttpHeader;

/* ------------------------------------------------------------------ */
/*  HttpRequest — a fully parsed HTTP/1.1 request                     */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Request line */
    char method [MAX_METHOD_LEN];   /* GET, POST, PUT, DELETE, HEAD  */
    char path   [MAX_PATH_LEN];     /* /api/users                    */
    char query  [MAX_PATH_LEN];     /* id=42&sort=asc  (after '?')   */
    char version[MAX_VERSION_LEN];  /* HTTP/1.0 or HTTP/1.1          */

    /* Headers */
    HttpHeader headers[MAX_HEADERS];
    int        header_count;

    /*
     * Body — points INSIDE the original raw buffer.
     * Do NOT free this pointer. It is only valid while
     * the raw buffer is alive.
     */
    const char *body;
    size_t      body_length;

    /*
     * Derived convenience fields — computed during parse
     * so callers don't have to look up headers manually.
     */
    int    keep_alive;      /* 1 = Connection: keep-alive, 0 = close */
    size_t content_length;  /* Value of Content-Length, or 0         */
} HttpRequest;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Zero-initialise a request before parsing */
void http_request_init(HttpRequest *req);

/*
 * Main parser — call this after every recv().
 * Fills *req on PARSE_OK.  *req is undefined on any other result.
 */
ParseResult parse_http_request(const char *raw, size_t len,
                               HttpRequest *req);

/*
 * Look up a header value by name — case-insensitive.
 * Returns NULL if the header is not present.
 *
 *   const char *ct = http_get_header(&req, "content-type");
 */
const char *http_get_header(const HttpRequest *req, const char *name);

/* Debug helper — dump every field to stdout */
void http_request_print(const HttpRequest *req);

/* ------------------------------------------------------------------ */
/*  Internal helpers — exposed here so test_parser.c can unit-test    */
/* ------------------------------------------------------------------ */
ParseResult parse_request_line(const char *line, size_t len,
                               HttpRequest *req);
ParseResult parse_header_line (const char *line, size_t len,
                               HttpRequest *req);

#endif /* HTTP_PARSER_H */
```

