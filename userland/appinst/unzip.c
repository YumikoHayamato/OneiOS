#include "unzip.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define EOCD_SIG 0x06054b50u
#define CD_SIG 0x02014b50u
#define LFH_SIG 0x04034b50u

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int fail(char *err, int errsz, const char *msg) {
  if (err && errsz > 0) snprintf(err, errsz, "%s", msg);
  return -1;
}

// mkdir -p for the directory part of `path` (which lives under dest). Also used for a bare dir entry.
static int mkdirs(const char *path) {
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  return 0;
}

// Reject zip-slip: no leading '/', no "." / ".." components, no backslashes.
static int name_is_safe(const char *name) {
  if (!name[0] || name[0] == '/' || strchr(name, '\\')) return 0;
  const char *p = name;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t seg = slash ? (size_t)(slash - p) : strlen(p);
    if (seg == 0 && slash) return 0;                 // "//"
    if (seg == 1 && p[0] == '.') return 0;           // "."
    if (seg == 2 && p[0] == '.' && p[1] == '.') return 0;  // ".."
    if (!slash) break;
    p = slash + 1;
  }
  return 1;
}

static int write_all(int fd, const unsigned char *buf, size_t n) {
  size_t w = 0;
  while (w < n) {
    ssize_t r = write(fd, buf + w, n - w);
    if (r <= 0) return -1;
    w += (size_t)r;
  }
  return 0;
}

// Inflate `comp[0..comp_len)` (raw deflate) writing `uncomp_len` bytes to fd.
static int inflate_to(int fd, const unsigned char *comp, size_t comp_len, uint64_t uncomp_len, char *err, int errsz) {
  z_stream zs;
  memset(&zs, 0, sizeof zs);
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return fail(err, errsz, "inflateInit failed");
  zs.next_in = (unsigned char *)comp;
  zs.avail_in = (uInt)comp_len;
  unsigned char out[64 * 1024];
  uint64_t total = 0;
  int ret;
  do {
    zs.next_out = out;
    zs.avail_out = sizeof out;
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return fail(err, errsz, "deflate stream corrupt"); }
    size_t have = sizeof out - zs.avail_out;
    if (have && write_all(fd, out, have) != 0) { inflateEnd(&zs); return fail(err, errsz, "write failed"); }
    total += have;
  } while (ret != Z_STREAM_END && zs.avail_in > 0);
  inflateEnd(&zs);
  (void)uncomp_len; (void)total;
  return 0;
}

int unzip_extract(const char *zip, const char *dest, char *err, int errsz) {
  int fd = open(zip, O_RDONLY);
  if (fd < 0) return fail(err, errsz, "cannot open .ipa");
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 22) { close(fd); return fail(err, errsz, "not a zip (too small)"); }
  size_t len = (size_t)st.st_size;
  // Map-ish: read the whole file. ipas are tens to hundreds of MB; cap at 2 GiB.
  if (len > (size_t)2 * 1024 * 1024 * 1024) { close(fd); return fail(err, errsz, ".ipa too large"); }
  unsigned char *data = malloc(len);
  if (!data) { close(fd); return fail(err, errsz, "out of memory"); }
  size_t got = 0;
  while (got < len) { ssize_t n = read(fd, data + got, len - got); if (n <= 0) break; got += (size_t)n; }
  close(fd);
  if (got != len) { free(data); return fail(err, errsz, "short read on .ipa"); }

  // Find EOCD by scanning backwards (comment up to 64 KiB).
  size_t eocd = 0; int found = 0;
  size_t scan_from = len >= 22 ? len - 22 : 0;
  size_t scan_to = (len > 22 + 65535) ? len - 22 - 65535 : 0;
  for (size_t i = scan_from + 1; i-- > scan_to;) {
    if (rd32(data + i) == EOCD_SIG) { eocd = i; found = 1; break; }
    if (i == 0) break;
  }
  if (!found) { free(data); return fail(err, errsz, "no end-of-central-directory (zip64 or corrupt?)"); }

  uint16_t entries = rd16(data + eocd + 10);
  uint32_t cd_size = rd32(data + eocd + 12);
  uint32_t cd_off = rd32(data + eocd + 16);
  if (cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) { free(data); return fail(err, errsz, "zip64 not supported"); }
  if ((size_t)cd_off + cd_size > len) { free(data); return fail(err, errsz, "central directory out of range"); }

  if (mkdir(dest, 0755) != 0 && errno != EEXIST) { free(data); return fail(err, errsz, "cannot create dest dir"); }

  size_t p = cd_off;
  for (uint16_t e = 0; e < entries; e++) {
    if (p + 46 > len || rd32(data + p) != CD_SIG) { free(data); return fail(err, errsz, "bad central directory record"); }
    uint16_t method = rd16(data + p + 10);
    uint32_t comp_size = rd32(data + p + 20);
    uint32_t uncomp_size = rd32(data + p + 24);
    uint16_t name_len = rd16(data + p + 28);
    uint16_t extra_len = rd16(data + p + 30);
    uint16_t cmt_len = rd16(data + p + 32);
    uint32_t ext_attr = rd32(data + p + 38);
    uint32_t lfh_off = rd32(data + p + 42);
    if (p + 46 + name_len > len) { free(data); return fail(err, errsz, "truncated central directory"); }
    char name[2048];
    if (name_len >= sizeof name) { free(data); return fail(err, errsz, "entry name too long"); }
    memcpy(name, data + p + 46, name_len);
    name[name_len] = 0;
    p += 46 + name_len + extra_len + cmt_len;

    int is_dir = (name_len > 0 && name[name_len - 1] == '/');
    if (!is_dir && !name_is_safe(name)) { free(data); return fail(err, errsz, "unsafe entry path in .ipa"); }
    if (is_dir) {
      // strip trailing slash for the safety check
      name[name_len - 1] = 0;
      if (name[0] && !name_is_safe(name)) { free(data); return fail(err, errsz, "unsafe dir path in .ipa"); }
    }

    char out_path[4096];
    snprintf(out_path, sizeof out_path, "%s/%s", dest, name);
    if (is_dir) {
      if (mkdir(out_path, 0755) != 0 && errno != EEXIST) { free(data); return fail(err, errsz, "mkdir failed"); }
      continue;
    }
    if (mkdirs(out_path) != 0) { free(data); return fail(err, errsz, "mkdir -p failed"); }

    // Local file header gives the real data offset (its name/extra lengths can differ from the CD's).
    if (lfh_off + 30 > len || rd32(data + lfh_off) != LFH_SIG) { free(data); return fail(err, errsz, "bad local header"); }
    uint16_t l_name = rd16(data + lfh_off + 26);
    uint16_t l_extra = rd16(data + lfh_off + 28);
    size_t data_off = (size_t)lfh_off + 30 + l_name + l_extra;
    if (data_off + comp_size > len) { free(data); return fail(err, errsz, "entry data out of range"); }

    mode_t mode = (ext_attr >> 16) & 0777;
    if (mode == 0) mode = 0644;
    int ofd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (ofd < 0) { free(data); return fail(err, errsz, "cannot create output file"); }
    int rc = 0;
    if (method == 0) {
      if (comp_size != uncomp_size) rc = fail(err, errsz, "stored entry size mismatch");
      else rc = write_all(ofd, data + data_off, comp_size) == 0 ? 0 : fail(err, errsz, "write failed");
    } else if (method == 8) {
      rc = inflate_to(ofd, data + data_off, comp_size, uncomp_size, err, errsz);
    } else {
      rc = fail(err, errsz, "unsupported compression method");
    }
    close(ofd);
    if (rc != 0) { free(data); return -1; }
    (void)uncomp_size;
  }
  free(data);
  return 0;
}
