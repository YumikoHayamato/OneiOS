// Mach-O helpers for appinst: thin a fat binary to the guest's arm64e (or arm64), refuse an encrypted
// (FairPlay, cryptid=1) slice, and read back the CDHash the trust cache needs. Thin 64-bit only, like
// scripts/ios/ios-sign.py. See README.md.
#ifndef ONEIOS_APPINST_MACHO_H
#define ONEIOS_APPINST_MACHO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MACHO_CDHASH_LEN 20   // sha256 truncated, as the owner trust module stores it

typedef enum {
  MACHO_OK = 0,
  MACHO_NOT_MACHO,      // not a Mach-O at all (skip it; e.g. a resource file)
  MACHO_ENCRYPTED,      // a slice has cryptid=1: a still-encrypted App Store binary; refuse
  MACHO_NO_ARM64,       // fat, but neither arm64e nor arm64 inside
  MACHO_NO_SIGNATURE,   // asked for the CDHash of a file with no code signature
  MACHO_BAD,            // malformed / truncated / I/O error
} macho_err;

// True if the file begins with a thin or fat Mach-O magic. Cheap: reads the first 4 bytes.
bool macho_is_macho(const char *path);

// Rewrite `path` in place as a thin Mach-O for the guest: pick the arm64e slice (cpusubtype 2), else the
// plain arm64 slice, drop the rest. A file that is already thin is checked and left as is. `*chose` gets a
// static string ("arm64e"/"arm64"). Returns MACHO_ENCRYPTED when the chosen slice is still encrypted, so the
// caller refuses before signing (we never strip DRM; the input must already be decrypted).
macho_err macho_thin(const char *path, const char **chose);

// CDHash of a signed thin Mach-O: sha256 of its CodeDirectory, truncated to 20 bytes, into out[20]. Call
// after re-signing with ldid. MACHO_NO_SIGNATURE if the file carries no LC_CODE_SIGNATURE.
macho_err macho_cdhash(const char *path, uint8_t out[MACHO_CDHASH_LEN]);

// Copy the primary Entitlements blob (DER slot 7, else XML slot 5) of a signed thin Mach-O into a freshly
// malloc'd XML plist, so the re-sign can carry the app's own entitlements. *xml is NULL (and MACHO_OK) when the
// file has none. Caller frees *xml. Only the XML slot is returned; MACHO_OK with *xml==NULL if only DER is present.
macho_err macho_entitlements_xml(const char *path, char **xml, size_t *len);

const char *macho_strerror(macho_err e);

#endif
