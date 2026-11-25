// appinst — on-device .ipa installer for the OneiOS guest. See appinst.h for the two-phase contract and README.md.
//
// Hard rule: the input .ipa must already be DECRYPTED. appinst never strips FairPlay/DRM. A still-encrypted
// slice (cryptid=1) is detected and the install is refused.
#import <Foundation/Foundation.h>

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "appinst.h"
#include "macho.h"
#include "unzip.h"

extern char **environ;

static void logln(const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  fprintf(stderr, "appinst: ");
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

static int run(const char *const argv[]) {
  pid_t pid;
  int rc = posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ);
  if (rc != 0) { errno = rc; return -1; }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

static int mkdirs(const char *path, mode_t mode) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof tmp, "%s", path);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = 0; mkdir(tmp, mode); *p = '/'; }
  return (mkdir(tmp, mode) != 0 && errno != EEXIST) ? -1 : 0;
}

static void rm_rf(const char *path) {
  const char *const argv[] = {"/bin/rm", "-rf", path, NULL};
  (void)run(argv);
}

// Recursively chown a tree. The data container must belong to mobile (uid/gid 501) or the app cannot write it.
static void chown_r(const char *path, uid_t uid, gid_t gid) {
  lchown(path, uid, gid);
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[PATH_MAX];
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) chown_r(child, uid, gid);
    else lchown(child, uid, gid);
  }
  closedir(d);
}

// ---- find Payload/<X>.app under a directory ----
static NSString *find_dot_app(NSString *root) {
  NSString *payload = [root stringByAppendingPathComponent:@"Payload"];
  NSFileManager *fm = NSFileManager.defaultManager;
  for (NSString *name in [fm contentsOfDirectoryAtPath:payload error:nil])
    if ([name.pathExtension isEqualToString:@"app"]) return [payload stringByAppendingPathComponent:name];
  return nil;
}

// ---- recursively collect regular files that are Mach-O ----
static void collect_machos(NSString *dir, NSMutableArray<NSString *> *out) {
  NSFileManager *fm = NSFileManager.defaultManager;
  NSDirectoryEnumerator *e = [fm enumeratorAtPath:dir];
  for (NSString *rel in e) {
    NSString *full = [dir stringByAppendingPathComponent:rel];
    BOOL isdir = NO;
    if (![fm fileExistsAtPath:full isDirectory:&isdir] || isdir) continue;
    if (macho_is_macho(full.fileSystemRepresentation)) [out addObject:full];
  }
}

// ---- ldid ad-hoc sign at 4 KiB (ldid's default page size), optionally with entitlements/identifier ----
static int ldid_sign(const char *file, const char *ent_xml_path, const char *identifier) {
  char sflag[PATH_MAX + 2];
  if (ent_xml_path) snprintf(sflag, sizeof sflag, "-S%s", ent_xml_path);
  else snprintf(sflag, sizeof sflag, "-S");
  char iflag[256];
  const char *argv[6]; int n = 0;
  argv[n++] = APPINST_LDID;
  argv[n++] = sflag;
  if (identifier) { snprintf(iflag, sizeof iflag, "-I%s", identifier); argv[n++] = iflag; }
  argv[n++] = file;
  argv[n] = NULL;
  return run(argv);
}

static NSString *hex_of(const uint8_t *b, int n) {
  NSMutableString *s = [NSMutableString stringWithCapacity:n * 2];
  for (int i = 0; i < n; i++) [s appendFormat:@"%02x", b[i]];
  return s;
}

// ---- write the trust inbox JSON (owner-trust-extra shape) ----
static int write_trust_inbox(NSString *bundle_id, NSArray<NSString *> *cdhashes_hex) {
  mkdirs(APPINST_TRUST_INBOX, 0755);
  NSDictionary *doc = @{ @"cdhashes": cdhashes_hex, @"category": @0, @"bundle_id": bundle_id };
  NSError *err = nil;
  NSData *json = [NSJSONSerialization dataWithJSONObject:doc options:NSJSONWritingPrettyPrinted error:&err];
  if (!json) { logln("trust inbox json: %s", err.localizedDescription.UTF8String); return -1; }
  NSString *path = [NSString stringWithFormat:@"%s/%@.json", APPINST_TRUST_INBOX, bundle_id];
  if (![json writeToFile:path atomically:YES]) { logln("cannot write %s", path.UTF8String); return -1; }
  return 0;
}

// ======================================================================================================
// Phase 1: install
// ======================================================================================================
int appinst_install(const char *ipa_path) {
  @autoreleasepool {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm fileExistsAtPath:@(ipa_path)]) { logln("no such file: %s", ipa_path); return 1; }

    mkdirs(APPINST_WORK, 0755);
    char work_c[PATH_MAX];
    snprintf(work_c, sizeof work_c, "%s/%d", APPINST_WORK, (int)getpid());
    rm_rf(work_c);
    NSString *work = @(work_c);

    char err[256];
    logln("unzipping %s", ipa_path);
    if (unzip_extract(ipa_path, work_c, err, sizeof err) != 0) { logln("unzip: %s", err); rm_rf(work_c); return 1; }

    NSString *app = find_dot_app(work);
    if (!app) { logln("no Payload/*.app in the .ipa"); rm_rf(work_c); return 1; }

    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
    NSString *bundle_id = info[@"CFBundleIdentifier"];
    NSString *exe = info[@"CFBundleExecutable"];
    if (!bundle_id || !exe) { logln("Info.plist has no CFBundleIdentifier/CFBundleExecutable"); rm_rf(work_c); return 1; }
    logln("app %@ (%@)", bundle_id, info[@"CFBundleName"] ?: exe);

    NSMutableArray<NSString *> *machos = [NSMutableArray array];
    collect_machos(app, machos);
    if (machos.count == 0) { logln("no Mach-O found in the bundle"); rm_rf(work_c); return 1; }

    NSString *main_exe = [app stringByAppendingPathComponent:exe];
    NSMutableArray<NSString *> *cdhashes = [NSMutableArray array];

    for (NSString *m in machos) {
      const char *mc = m.fileSystemRepresentation;

      // 1) thin to arm64e (fallback arm64); refuse an encrypted slice.
      const char *arch = NULL;
      macho_err te = macho_thin(mc, &arch);
      if (te == MACHO_ENCRYPTED) {
        logln("REFUSED: %s is still encrypted (cryptid=1). This .ipa is not decrypted; appinst does not strip DRM.",
              m.lastPathComponent.UTF8String);
        rm_rf(work_c); return 2;
      }
      if (te == MACHO_NOT_MACHO) continue;          // shouldn't happen (we filtered), but be safe
      if (te != MACHO_OK) { logln("thin %s: %s", m.lastPathComponent.UTF8String, macho_strerror(te)); rm_rf(work_c); return 1; }

      BOOL is_main = [m isEqualToString:main_exe];

      // 2) preserve the app's own entitlements on the main executable.
      char *ent = NULL; size_t ent_len = 0; char ent_path[PATH_MAX]; ent_path[0] = 0;
      if (is_main) {
        if (macho_entitlements_xml(mc, &ent, &ent_len) == MACHO_OK && ent) {
          snprintf(ent_path, sizeof ent_path, "%s/entitlements.plist", work_c);
          FILE *f = fopen(ent_path, "wb");
          if (f) { fwrite(ent, 1, ent_len, f); fclose(f); } else ent_path[0] = 0;
        }
        free(ent);
      }

      // 3) re-sign ad-hoc at 4 KiB (clears the platform-binary flag by definition of an ad-hoc signature).
      int sr = ldid_sign(mc, is_main && ent_path[0] ? ent_path : NULL, is_main ? bundle_id.UTF8String : NULL);
      if (sr != 0) { logln("ldid failed on %s (rc=%d). Is %s present?", m.lastPathComponent.UTF8String, sr, APPINST_LDID);
        rm_rf(work_c); return 1; }

      // 4) read back the CDHash the trust cache needs.
      uint8_t cd[MACHO_CDHASH_LEN];
      macho_err ce = macho_cdhash(mc, cd);
      if (ce != MACHO_OK) { logln("cdhash %s: %s", m.lastPathComponent.UTF8String, macho_strerror(ce)); rm_rf(work_c); return 1; }
      [cdhashes addObject:hex_of(cd, MACHO_CDHASH_LEN)];
      logln("signed %-28s %s", m.lastPathComponent.UTF8String, arch);
    }

    // 5) trust inbox (xnuhost folds these into the owner trust module at the next boot).
    if (write_trust_inbox(bundle_id, cdhashes) != 0) { rm_rf(work_c); return 1; }

    // 6) place the bundle under a fresh container UUID and pre-create the data container.
    NSString *bundle_uuid = NSUUID.UUID.UUIDString;
    NSString *data_uuid = NSUUID.UUID.UUIDString;
    NSString *dest_dir = [@(APPINST_BUNDLE_ROOT) stringByAppendingPathComponent:bundle_uuid];
    mkdirs(dest_dir.fileSystemRepresentation, 0755);
    NSString *dest_app = [dest_dir stringByAppendingPathComponent:app.lastPathComponent];
    NSError *mv = nil;
    if (![fm moveItemAtPath:app toPath:dest_app error:&mv]) { logln("place bundle: %s", mv.localizedDescription.UTF8String);
      rm_rf(work_c); return 1; }

    NSString *data_dir = [@(APPINST_DATA_ROOT) stringByAppendingPathComponent:data_uuid];
    for (NSString *sub in @[@"Documents", @"Library", @"Library/Caches", @"Library/Preferences", @"tmp", @"SystemData"])
      mkdirs([data_dir stringByAppendingPathComponent:sub].fileSystemRepresentation, 0755);
    chown_r(data_dir.fileSystemRepresentation, 501, 501);   // mobile owns its data container

    // 7) pending marker for phase 2 (registration after the restart makes the CDHashes valid).
    mkdirs(APPINST_PENDING, 0755);
    NSDictionary *marker = @{ @"bundle_id": bundle_id, @"app_path": dest_app, @"data_path": data_dir,
                              @"bundle_uuid": bundle_uuid, @"data_uuid": data_uuid };
    NSString *marker_path = [NSString stringWithFormat:@"%s/%@.plist", APPINST_PENDING, bundle_id];
    [marker writeToFile:marker_path atomically:YES];

    rm_rf(work_c);
    logln("staged %@ — %lu Mach-O(s) trusted. Restart the guest to finish.", bundle_id, (unsigned long)cdhashes.count);

    // 8) ask for the guest-VM restart that commits the trust cache.
    if (appinst_request_restart() != 0)
      logln("could not request a restart automatically — restart the guest VM to finish installing %@.", bundle_id);
    return 0;
  }
}

// ======================================================================================================
// Phase 2: finalize (run at boot, after xnuhost injected the new CDHashes)
// ======================================================================================================
int appinst_finalize(void) {
  @autoreleasepool {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray *pend = [fm contentsOfDirectoryAtPath:@(APPINST_PENDING) error:nil];
    if (pend.count == 0) return 0;
    int failures = 0;
    for (NSString *name in pend) {
      if (![name.pathExtension isEqualToString:@"plist"]) continue;
      NSString *mp = [@(APPINST_PENDING) stringByAppendingPathComponent:name];
      NSDictionary *m = [NSDictionary dictionaryWithContentsOfFile:mp];
      NSString *app_path = m[@"app_path"], *bundle_id = m[@"bundle_id"];
      if (!app_path || !bundle_id || ![fm fileExistsAtPath:app_path]) { [fm removeItemAtPath:mp error:nil]; continue; }
      char err[256] = {0};
      logln("registering %@", bundle_id);
      if (appinst_register_bundle(app_path.fileSystemRepresentation, bundle_id.fileSystemRepresentation, err, sizeof err) == 0) {
        [fm removeItemAtPath:mp error:nil];
        logln("installed %@", bundle_id);
      } else {
        failures++;
        logln("register %@ failed: %s (left pending; will retry next boot)", bundle_id, err);
      }
    }
    return failures ? 1 : 0;
  }
}

static int usage(void) {
  fprintf(stderr,
    "usage: appinst install FILE.ipa    stage a decrypted .ipa (phase 1), then request a guest restart\n"
    "       appinst finalize            register staged apps after the restart (phase 2; run at boot)\n"
    "       appinst serve [PORT]        localhost install page (default %d)\n", APPINST_HTTP_PORT);
  return 2;
}

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  if (!strcmp(argv[1], "install") && argc == 3) return appinst_install(argv[2]);
  if (!strcmp(argv[1], "finalize") && argc == 2) return appinst_finalize();
  if (!strcmp(argv[1], "serve")) return appinst_serve(argc >= 3 ? atoi(argv[2]) : APPINST_HTTP_PORT);
  return usage();
}
