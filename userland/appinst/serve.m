// serve.m — a tiny localhost-only web page so the user can install an .ipa with no PC: download the (already
// decrypted) .ipa in Safari → Files, open http://127.0.0.1:8088/ in Safari, pick the file, tap Install. The
// page POSTs the file to this server, which runs phase 1 (appinst_install) and shows the result.
//
// Deliberately minimal: 127.0.0.1 bind only, one request per connection, a size cap on uploads. It is not a
// general web server and must never bind a routable address.
#import <Foundation/Foundation.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "appinst.h"

#define MAX_UPLOAD (1024u * 1024u * 1024u)   // 1 GiB cap for the in-memory multipart path; bigger → use the CLI
#define HDR_MAX (64 * 1024)

static const char *PAGE_HEAD =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>OneiOS Install</title><style>"
  "body{font:17px -apple-system,sans-serif;margin:0;background:#000;color:#eee}"
  ".wrap{max-width:560px;margin:0 auto;padding:24px 18px}"
  "h1{font-size:22px}label{display:block;margin:18px 0 8px}"
  "input[type=file]{width:100%}button{font-size:18px;padding:12px 20px;border:0;border-radius:12px;"
  "background:#0a84ff;color:#fff;margin-top:18px}.card{background:#1c1c1e;border-radius:14px;padding:18px;margin:16px 0}"
  ".warn{background:#3a2a00;color:#ffd479}.ok{background:#07310f;color:#7ee787}.err{background:#3a0d0d;color:#ff9a9a}"
  "code{color:#9ad}</style></head><body><div class=wrap><h1>OneiOS&nbsp;Install</h1>";

static const char *PAGE_FORM =
  "<div class=card><form method=post action=/install enctype=multipart/form-data>"
  "<label>Choose a decrypted .ipa (from Files)</label>"
  "<input type=file name=ipa accept=.ipa required>"
  "<button type=submit>Install</button></form></div>"
  "<div class=card>Only <b>decrypted</b> .ipa files work. appinst never strips DRM; a still-encrypted app is refused. "
  "After installing, the guest must be <b>restarted</b> (the owner's call) to finish — the new app appears on the Home Screen after the restart.</div>";

static void write_all_fd(int fd, const void *buf, size_t n) {
  const char *p = buf; size_t w = 0;
  while (w < n) { ssize_t r = write(fd, p + w, n - w); if (r <= 0) break; w += (size_t)r; }
}

static void send_response(int fd, const char *status, const char *body) {
  char hdr[256];
  int n = snprintf(hdr, sizeof hdr,
    "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
    status, strlen(body));
  write_all_fd(fd, hdr, (size_t)n);
  write_all_fd(fd, body, strlen(body));
}

static NSString *status_card(void) {
  NSFileManager *fm = NSFileManager.defaultManager;
  NSArray *pend = [fm contentsOfDirectoryAtPath:@(APPINST_PENDING) error:nil];
  NSMutableString *s = [NSMutableString string];
  NSUInteger n = 0;
  for (NSString *name in pend) if ([name.pathExtension isEqualToString:@"plist"]) n++;
  if (n) [s appendFormat:@"<div class='card warn'>%lu app(s) staged, waiting for a guest restart to finish.</div>",
                         (unsigned long)n];
  if ([fm fileExistsAtPath:@(APPINST_ROOT "/restart-requested")])
    [s appendString:@"<div class='card warn'>A restart has been requested. Ask the owner to restart the guest VM.</div>"];
  return s;
}

static void serve_index(int fd) {
  @autoreleasepool {
    NSMutableString *html = [NSMutableString stringWithUTF8String:PAGE_HEAD];
    [html appendString:status_card()];
    [html appendFormat:@"%s</div></body></html>", PAGE_FORM];
    send_response(fd, "200 OK", html.UTF8String);
  }
}

// Find needle in a byte buffer. Returns offset or -1.
static long memfind(const uint8_t *hay, size_t hlen, const uint8_t *nee, size_t nlen, size_t from) {
  if (nlen == 0 || nlen > hlen) return -1;
  for (size_t i = from; i + nlen <= hlen; i++)
    if (hay[i] == nee[0] && memcmp(hay + i, nee, nlen) == 0) return (long)i;
  return -1;
}

// Extract the first file part of a multipart/form-data body into a temp .ipa file. Returns a malloc'd path or NULL.
static char *extract_multipart(const uint8_t *body, size_t blen, const char *boundary) {
  char dash_boundary[256];
  snprintf(dash_boundary, sizeof dash_boundary, "--%s", boundary);
  size_t dlen = strlen(dash_boundary);
  long first = memfind(body, blen, (const uint8_t *)dash_boundary, dlen, 0);
  if (first < 0) return NULL;
  // part headers end at the first CRLFCRLF after the boundary
  long hdr_end = memfind(body, blen, (const uint8_t *)"\r\n\r\n", 4, (size_t)first);
  if (hdr_end < 0) return NULL;
  size_t content_start = (size_t)hdr_end + 4;
  // content ends right before the next "\r\n--boundary"
  char crlf_boundary[260];
  snprintf(crlf_boundary, sizeof crlf_boundary, "\r\n%s", dash_boundary);
  long content_end = memfind(body, blen, (const uint8_t *)crlf_boundary, strlen(crlf_boundary), content_start);
  if (content_end < 0) return NULL;
  size_t clen = (size_t)content_end - content_start;

  char *path = malloc(PATH_MAX);
  if (!path) return NULL;
  mkdir(APPINST_WORK, 0755);
  snprintf(path, PATH_MAX, "%s/upload-%d.ipa", APPINST_WORK, (int)getpid());
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { free(path); return NULL; }
  write_all_fd(fd, body + content_start, clen);
  close(fd);
  return path;
}

static void handle_install(int fd, const char *content_type, const uint8_t *body, size_t blen) {
  @autoreleasepool {
    const char *bp = content_type ? strstr(content_type, "boundary=") : NULL;
    if (!bp) { send_response(fd, "400 Bad Request", "<p>bad upload (no boundary)</p>"); return; }
    char boundary[256];
    snprintf(boundary, sizeof boundary, "%s", bp + strlen("boundary="));
    for (char *q = boundary; *q; q++) if (*q == ';' || *q == '\r' || *q == '\n') { *q = 0; break; }

    char *ipa = extract_multipart(body, blen, boundary);
    if (!ipa) { send_response(fd, "400 Bad Request", "<p>could not read the uploaded .ipa</p>"); return; }

    int rc = appinst_install(ipa);
    unlink(ipa);
    free(ipa);

    NSMutableString *html = [NSMutableString stringWithUTF8String:PAGE_HEAD];
    if (rc == 0)
      [html appendString:@"<div class='card ok'>Staged. <b>Restart the guest VM</b> (the owner's call) to finish — "
                         "the app then appears on the Home Screen.</div>"];
    else if (rc == 2)
      [html appendString:@"<div class='card err'>Refused: the .ipa is still <b>encrypted</b> (cryptid=1). "
                         "appinst installs only decrypted .ipa files and never strips DRM.</div>"];
    else
      [html appendString:@"<div class='card err'>Install failed. See the appinst log on the console for why.</div>"];
    [html appendString:status_card()];
    [html appendFormat:@"<div class=card><a href=/ style='color:#0a84ff'>&larr; back</a></div></div></body></html>"];
    send_response(fd, "200 OK", html.UTF8String);
  }
}

// Read the full request (headers + body) up to Content-Length. Returns body buffer via out params; caller frees.
static int read_request(int fd, char *method, size_t msz, char *path, size_t psz,
                        char *ctype, size_t csz, uint8_t **body, size_t *blen) {
  size_t cap = HDR_MAX; size_t len = 0;
  uint8_t *buf = malloc(cap);
  if (!buf) return -1;
  long hdr_end = -1;
  // read until we have the header terminator
  while (hdr_end < 0) {
    if (len == cap) { if (cap >= HDR_MAX) break; }
    ssize_t n = read(fd, buf + len, cap - len);
    if (n <= 0) break;
    len += (size_t)n;
    hdr_end = memfind(buf, len, (const uint8_t *)"\r\n\r\n", 4, 0);
    if ((long)len >= (long)cap && hdr_end < 0) break;
  }
  if (hdr_end < 0) { free(buf); return -1; }

  // parse request line + content-length + content-type
  method[0] = path[0] = ctype[0] = 0;
  sscanf((char *)buf, "%15s %1023s", method, path);
  size_t content_length = 0;
  {
    // header block is buf[0..hdr_end]
    char *hdrs = strndup((char *)buf, (size_t)hdr_end);
    for (char *line = strtok(hdrs, "\r\n"); line; line = strtok(NULL, "\r\n")) {
      if (!strncasecmp(line, "Content-Length:", 15)) content_length = (size_t)strtoull(line + 15, NULL, 10);
      else if (!strncasecmp(line, "Content-Type:", 13)) snprintf(ctype, csz, "%s", line + 13 + strspn(line + 13, " "));
    }
    free(hdrs);
  }
  (void)msz; (void)psz;

  size_t body_have = len - (size_t)(hdr_end + 4);
  if (content_length > MAX_UPLOAD) { free(buf); return -2; }
  uint8_t *b = malloc(content_length ? content_length : 1);
  if (!b) { free(buf); return -1; }
  if (body_have > content_length) body_have = content_length;
  memcpy(b, buf + hdr_end + 4, body_have);
  free(buf);
  size_t got = body_have;
  while (got < content_length) {
    ssize_t n = read(fd, b + got, content_length - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  *body = b; *blen = got;
  return 0;
}

int appinst_serve(int port) {
  signal(SIGPIPE, SIG_IGN);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { perror("socket"); return 1; }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only
  if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }
  if (listen(s, 8) != 0) { perror("listen"); return 1; }
  fprintf(stderr, "appinst: serving the install page on http://127.0.0.1:%d/\n", port);

  for (;;) {
    int c = accept(s, NULL, NULL);
    if (c < 0) { if (errno == EINTR) continue; break; }
    char method[16], path[1024], ctype[1024];
    uint8_t *body = NULL; size_t blen = 0;
    int rr = read_request(c, method, sizeof method, path, sizeof path, ctype, sizeof ctype, &body, &blen);
    if (rr == -2) { send_response(c, "413 Payload Too Large", "<p>.ipa too large for the web upload — use <code>appinst install</code> from a shell.</p>"); }
    else if (rr != 0) { /* drop */ }
    else if (!strcmp(method, "POST") && !strncmp(path, "/install", 8)) handle_install(c, ctype, body, blen);
    else serve_index(c);
    free(body);
    close(c);
  }
  close(s);
  return 0;
}
