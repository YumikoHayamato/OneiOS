// register.m — phase 2 registration of an already-placed, now-trusted app bundle with LaunchServices, so it
// appears on SpringBoard's Home Screen. Called by `appinst finalize` after the guest restart has made the
// bundle's CDHashes valid in the trust cache.
//
// We register the bundle in place (it already lives under /var/containers/Bundle/Application/<UUID>) rather
// than going through installd's installApplication: (which re-copies and re-validates). LSApplicationWorkspace
// is a private class; we reach it through the Objective-C runtime (NSClassFromString) so nothing has to be
// linked against a private framework. If LaunchServices registration is rejected, see README "open items":
// the fallback is installd's MobileInstallation path, scaffolded below but off by default.
#import <Foundation/Foundation.h>

#include <stdio.h>
#include <string.h>

// Typed view of the private API. We never reference the class symbol directly (obtained via NSClassFromString),
// so this compiles and links without the private framework.
@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dict;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)unregisterApplication:(NSURL *)url;
- (BOOL)installApplication:(NSURL *)url withOptions:(NSDictionary *)options error:(NSError **)error;
- (void)invalidateIconCache:(id)bundleID;
@end

static LSApplicationWorkspace *workspace(void) {
  Class cls = NSClassFromString(@"LSApplicationWorkspace");
  if (!cls) return nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
  return [cls performSelector:@selector(defaultWorkspace)];
#pragma clang diagnostic pop
}

// Build the registration dictionary LaunchServices expects for a user app installed at `app_path`.
static NSDictionary *registration_dict(NSString *app_path, NSString *bundle_id) {
  NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app_path stringByAppendingPathComponent:@"Info.plist"]];
  NSMutableDictionary *d = [NSMutableDictionary dictionary];
  if (info) [d addEntriesFromDictionary:info];
  d[@"ApplicationType"] = @"User";
  d[@"CFBundleIdentifier"] = bundle_id;
  d[@"CodeInfoIdentifier"] = bundle_id;
  d[@"CompatibilityState"] = @0;
  d[@"Path"] = app_path;
  d[@"IsDeletable"] = @YES;
  d[@"HasMIDBasedSINF"] = @0;
  d[@"MinimumOSVersion"] = info[@"MinimumOSVersion"] ?: @"27.0";
  return d;
}

int appinst_register_bundle(const char *app_path_c, const char *bundle_id_c, char *err, int errsz) {
  @autoreleasepool {
    NSString *app_path = @(app_path_c);
    NSString *bundle_id = @(bundle_id_c);
    LSApplicationWorkspace *ws = workspace();
    if (!ws) { snprintf(err, errsz, "LSApplicationWorkspace unavailable"); return -1; }

    // Primary: register the bundle dictionary in place.
    @try {
      if ([ws registerApplicationDictionary:registration_dict(app_path, bundle_id)]) {
        @try { [ws invalidateIconCache:bundle_id]; } @catch (__unused id e) {}
        return 0;
      }
    } @catch (NSException *e) {
      snprintf(err, errsz, "registerApplicationDictionary threw: %s", e.reason.UTF8String);
    }

    // Secondary: register by path URL.
    @try {
      NSURL *url = [NSURL fileURLWithPath:app_path isDirectory:YES];
      if ([ws registerApplication:url]) {
        @try { [ws invalidateIconCache:bundle_id]; } @catch (__unused id e) {}
        return 0;
      }
    } @catch (NSException *e) {
      snprintf(err, errsz, "registerApplication threw: %s", e.reason.UTF8String);
    }

    if (err[0] == 0) snprintf(err, errsz, "LaunchServices refused the registration (see README open items: installd profile check)");
    return -1;
  }
}
