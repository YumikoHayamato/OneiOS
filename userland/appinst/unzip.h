// Minimal, safe .ipa (zip) extractor for appinst: store + deflate, no zip64, rejects absolute paths and "..".
// Uses iOS's libz for inflate. See unzip.c.
#ifndef ONEIOS_APPINST_UNZIP_H
#define ONEIOS_APPINST_UNZIP_H

// Extract `zip` into directory `dest` (created if missing). Returns 0 on success, -1 on error with a message
// written to `err` (size `errsz`). Entries that escape `dest` abort the whole extraction.
int unzip_extract(const char *zip, const char *dest, char *err, int errsz);

#endif
