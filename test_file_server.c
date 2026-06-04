/*
 * test_file_server.c — unit tests for Week 4 
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

    /* URL-encoded attempt  */
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