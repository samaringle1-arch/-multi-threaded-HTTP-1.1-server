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
