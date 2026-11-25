#include "macho.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---- Mach-O / fat constants (all file fields little-endian for thin, big-endian for the fat header) ----
#define FAT_MAGIC 0xCAFEBABEu     // fat header is big-endian on disk
#define FAT_MAGIC_64 0xCAFEBABFu
#define MH_MAGIC_64 0xFEEDFACFu   // thin 64-bit, host (little) endian
#define CPU_TYPE_ARM64 0x0100000Cu
#define CPU_SUBTYPE_ARM64E 2u
#define CPU_SUBTYPE_ARM64_ALL 0u
#define CPU_SUBTYPE_MASK 0x00FFFFFFu   // low 24 bits; the high byte holds capability/PAC bits
#define LC_SEGMENT_64 0x19u
#define LC_CODE_SIGNATURE 0x1Du
#define LC_ENCRYPTION_INFO_64 0x2Cu
#define CSMAGIC_EMBEDDED_SIGNATURE 0xFADE0CC0u  // big-endian in the blob
#define CSMAGIC_CODEDIRECTORY 0xFADE0C02u
#define CSSLOT_ENTITLEMENTS 5u
#define CSMAGIC_EMBEDDED_ENTITLEMENTS 0xFADE7171u

static uint32_t rd_le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd_be32(const uint8_t *p) { return (uint32_t)p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24; }
static uint64_t rd_be64(const uint8_t *p) { return (uint64_t)rd_be32(p) << 32 | rd_be32(p + 4); }

// Read a whole file into a malloc'd buffer. Caller frees. NULL on error.
static uint8_t *slurp(const char *path, size_t *out_len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)512 * 1024 * 1024) { close(fd); return NULL; }
  size_t len = (size_t)st.st_size;
  uint8_t *buf = malloc(len);
  if (!buf) { close(fd); return NULL; }
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, buf + got, len - got);
    if (n <= 0) { free(buf); close(fd); return NULL; }
    got += (size_t)n;
  }
  close(fd);
  *out_len = len;
  return buf;
}

bool macho_is_macho(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  uint8_t m[4];
  ssize_t n = read(fd, m, 4);
  close(fd);
  if (n != 4) return false;
  uint32_t be = rd_be32(m), le = rd_le32(m);
  return be == FAT_MAGIC || be == FAT_MAGIC_64 || le == MH_MAGIC_64;
}

// Walk the load commands of a thin 64-bit image at data[off .. off+size) and report whether a slice is still
// encrypted (cryptid != 0). `*encrypted` is set; returns false on a malformed header.
static bool thin_is_encrypted(const uint8_t *data, size_t off, size_t size, bool *encrypted) {
  *encrypted = false;
  if (size < 32 || rd_le32(data + off) != MH_MAGIC_64) return false;
  uint32_t ncmds = rd_le32(data + off + 16);
  uint32_t sizeofcmds = rd_le32(data + off + 20);
  size_t at = off + 32;
  if ((size_t)sizeofcmds > size - 32) return false;
  for (uint32_t i = 0; i < ncmds; i++) {
    if (at + 8 > off + size) return false;
    uint32_t cmd = rd_le32(data + at), cmdsize = rd_le32(data + at + 4);
    if (cmdsize < 8 || at + cmdsize > off + size) return false;
    if (cmd == LC_ENCRYPTION_INFO_64) {
      if (cmdsize < 20) return false;
      uint32_t cryptid = rd_le32(data + at + 16);
      if (cryptid != 0) *encrypted = true;
    }
    at += cmdsize;
  }
  return true;
}

macho_err macho_thin(const char *path, const char **chose) {
  size_t len = 0;
  uint8_t *data = slurp(path, &len);
  if (!data) return MACHO_BAD;
  macho_err rc;

  uint32_t be = rd_be32(data), le = rd_le32(data);
  if (le == MH_MAGIC_64) {
    // Already thin. Must be arm64/arm64e, and not encrypted.
    uint32_t cputype = rd_le32(data + 4), sub = rd_le32(data + 8) & CPU_SUBTYPE_MASK;
    if (cputype != CPU_TYPE_ARM64) { rc = MACHO_NO_ARM64; goto done; }
    bool enc = false;
    if (!thin_is_encrypted(data, 0, len, &enc)) { rc = MACHO_BAD; goto done; }
    if (enc) { rc = MACHO_ENCRYPTED; goto done; }
    *chose = (sub == CPU_SUBTYPE_ARM64E) ? "arm64e" : "arm64";
    rc = MACHO_OK;
    goto done;
  }
  if (be != FAT_MAGIC && be != FAT_MAGIC_64) { rc = MACHO_NOT_MACHO; goto done; }

  // Fat: pick arm64e (subtype 2) over plain arm64 (subtype 0).
  int is64 = (be == FAT_MAGIC_64);
  uint32_t nfat = rd_be32(data + 4);
  if (nfat == 0 || nfat > 32) { rc = MACHO_BAD; goto done; }
  size_t entry = is64 ? 32 : 20;
  size_t best_off = 0, best_size = 0;
  int best_rank = -1;            // 2 = arm64e, 1 = arm64, -1 = none
  const char *best_name = NULL;
  for (uint32_t i = 0; i < nfat; i++) {
    size_t e = 8 + (size_t)i * entry;
    if (e + entry > len) { rc = MACHO_BAD; goto done; }
    uint32_t cputype = rd_be32(data + e);
    uint32_t sub = rd_be32(data + e + 4) & CPU_SUBTYPE_MASK;
    uint64_t foff = is64 ? rd_be64(data + e + 8) : rd_be32(data + e + 8);
    uint64_t fsize = is64 ? rd_be64(data + e + 16) : rd_be32(data + e + 12);
    if (cputype != CPU_TYPE_ARM64) continue;
    if (foff > len || fsize > len || foff + fsize > len) { rc = MACHO_BAD; goto done; }
    int rank = (sub == CPU_SUBTYPE_ARM64E) ? 2 : (sub == CPU_SUBTYPE_ARM64_ALL ? 1 : 0);
    if (rank > best_rank) {
      best_rank = rank; best_off = (size_t)foff; best_size = (size_t)fsize;
      best_name = (sub == CPU_SUBTYPE_ARM64E) ? "arm64e" : "arm64";
    }
  }
  if (best_rank < 1) { rc = MACHO_NO_ARM64; goto done; }

  bool enc = false;
  if (!thin_is_encrypted(data, best_off, best_size, &enc)) { rc = MACHO_BAD; goto done; }
  if (enc) { rc = MACHO_ENCRYPTED; goto done; }

  // Write the chosen slice back as a thin file, preserving the mode.
  {
    struct stat st;
    mode_t mode = (stat(path, &st) == 0) ? (st.st_mode & 0777) : 0755;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.thin.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { rc = MACHO_BAD; goto done; }
    size_t w = 0;
    while (w < best_size) {
      ssize_t n = write(fd, data + best_off + w, best_size - w);
      if (n <= 0) { close(fd); unlink(tmp); rc = MACHO_BAD; goto done; }
      w += (size_t)n;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) { unlink(tmp); rc = MACHO_BAD; goto done; }
  }
  *chose = best_name;
  rc = MACHO_OK;

done:
  free(data);
  return rc;
}

// Locate the embedded signature blob (its file offset and size) from LC_CODE_SIGNATURE of a thin image.
static bool find_code_signature(const uint8_t *data, size_t len, size_t *sig_off, size_t *sig_size) {
  if (len < 32 || rd_le32(data) != MH_MAGIC_64) return false;
  uint32_t ncmds = rd_le32(data + 16);
  size_t at = 32;
  for (uint32_t i = 0; i < ncmds; i++) {
    if (at + 8 > len) return false;
    uint32_t cmd = rd_le32(data + at), cmdsize = rd_le32(data + at + 4);
    if (cmdsize < 8 || at + cmdsize > len) return false;
    if (cmd == LC_CODE_SIGNATURE && cmdsize >= 16) {
      uint32_t off = rd_le32(data + at + 8), size = rd_le32(data + at + 12);
      if ((size_t)off + size > len) return false;
      *sig_off = off; *sig_size = size;
      return true;
    }
    at += cmdsize;
  }
  return false;
}

// Within a SuperBlob at sb[0..sb_len), find the blob of a given magic (and optionally slot). Returns offset/len
// of the blob *relative to sb*, or false.
static bool superblob_find(const uint8_t *sb, size_t sb_len, uint32_t want_magic, int want_slot,
                           size_t *blob_off, size_t *blob_len) {
  if (sb_len < 12 || rd_be32(sb) != CSMAGIC_EMBEDDED_SIGNATURE) return false;
  uint32_t count = rd_be32(sb + 8);
  if ((size_t)12 + (size_t)count * 8 > sb_len) return false;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t slot = rd_be32(sb + 12 + i * 8);
    uint32_t off = rd_be32(sb + 12 + i * 8 + 4);
    if ((size_t)off + 8 > sb_len) continue;
    uint32_t magic = rd_be32(sb + off);
    uint32_t blen = rd_be32(sb + off + 4);
    if ((size_t)off + blen > sb_len) continue;
    if (magic == want_magic && (want_slot < 0 || (int)slot == want_slot)) {
      *blob_off = off; *blob_len = blen;
      return true;
    }
  }
  return false;
}

// hashType byte lives at offset 37 of a CodeDirectory blob (after the 8-byte blob header: magic,length, then
// version,flags,hashOffset,identOffset,nSpecial,nCode,codeLimit,hashSize,hashType...).
#define CD_HASHTYPE_OFF 37
#define CS_HASHTYPE_SHA256 2

// Find the CodeDirectory the trust cache keys on: prefer hashType 2 (SHA-256), else the first CodeDirectory.
// AMFI/the trust cache store a 20-byte-truncated SHA-256; a binary ldid signed with both a SHA-1 and a SHA-256
// CodeDirectory must hash the SHA-256 one.
static bool superblob_best_codedir(const uint8_t *sb, size_t sb_len, size_t *cd_off, size_t *cd_len) {
  if (sb_len < 12 || rd_be32(sb) != CSMAGIC_EMBEDDED_SIGNATURE) return false;
  uint32_t count = rd_be32(sb + 8);
  if ((size_t)12 + (size_t)count * 8 > sb_len) return false;
  bool have_any = false;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t off = rd_be32(sb + 12 + i * 8 + 4);
    if ((size_t)off + 8 > sb_len) continue;
    if (rd_be32(sb + off) != CSMAGIC_CODEDIRECTORY) continue;
    uint32_t blen = rd_be32(sb + off + 4);
    if ((size_t)off + blen > sb_len || blen < CD_HASHTYPE_OFF + 1) continue;
    if (!have_any) { *cd_off = off; *cd_len = blen; have_any = true; }   // fallback: first CD
    if (sb[off + CD_HASHTYPE_OFF] == CS_HASHTYPE_SHA256) { *cd_off = off; *cd_len = blen; return true; }
  }
  return have_any;
}

macho_err macho_cdhash(const char *path, uint8_t out[MACHO_CDHASH_LEN]) {
  size_t len = 0;
  uint8_t *data = slurp(path, &len);
  if (!data) return MACHO_BAD;
  macho_err rc = MACHO_BAD;
  size_t so = 0, ss = 0;
  if (!find_code_signature(data, len, &so, &ss)) { rc = MACHO_NO_SIGNATURE; goto done; }
  size_t cd_off = 0, cd_len = 0;
  if (!superblob_best_codedir(data + so, ss, &cd_off, &cd_len)) { rc = MACHO_NO_SIGNATURE; goto done; }
  uint8_t full[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data + so + cd_off, (CC_LONG)cd_len, full);
  memcpy(out, full, MACHO_CDHASH_LEN);
  rc = MACHO_OK;
done:
  free(data);
  return rc;
}

macho_err macho_entitlements_xml(const char *path, char **xml, size_t *len_out) {
  *xml = NULL; *len_out = 0;
  size_t len = 0;
  uint8_t *data = slurp(path, &len);
  if (!data) return MACHO_BAD;
  macho_err rc = MACHO_OK;
  size_t so = 0, ss = 0;
  if (!find_code_signature(data, len, &so, &ss)) goto done;   // none: MACHO_OK, *xml NULL
  size_t bo = 0, bl = 0;
  if (!superblob_find(data + so, ss, CSMAGIC_EMBEDDED_ENTITLEMENTS, CSSLOT_ENTITLEMENTS, &bo, &bl)) goto done;
  if (bl <= 8) goto done;
  size_t payload = bl - 8;
  char *out = malloc(payload + 1);
  if (!out) { rc = MACHO_BAD; goto done; }
  memcpy(out, data + so + bo + 8, payload);
  out[payload] = 0;
  *xml = out; *len_out = payload;
done:
  free(data);
  return rc;
}

const char *macho_strerror(macho_err e) {
  switch (e) {
    case MACHO_OK: return "ok";
    case MACHO_NOT_MACHO: return "not a Mach-O";
    case MACHO_ENCRYPTED: return "still encrypted (cryptid=1): decrypt the .ipa first";
    case MACHO_NO_ARM64: return "no arm64e or arm64 slice";
    case MACHO_NO_SIGNATURE: return "no code signature";
    case MACHO_BAD: return "malformed or unreadable Mach-O";
  }
  return "unknown";
}
