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