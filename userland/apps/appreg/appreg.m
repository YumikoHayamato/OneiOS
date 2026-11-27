// SPDX-License-Identifier: MIT
// oneios-appreg: registers OneiOS's own app bundles with LaunchServices at every boot, so SpringBoard shows them.
//
//   oneios-appreg [DIR ...]      default /private/var/ngpu/apps (where the boot addon writes them) and
//                                /private/var/oneios/apps (where a Data seed made with roottweak --apps has them)
//
// Run once per boot by oneios-init (services.d/appreg.conf, bootstrap=1: it talks to lsd over XPC). The bundles stay
// where they are; nothing goes through installd. Each bundle's code is trusted because its CDHash is in the owner
// trust cache (owner-trust.json); this tool only makes LaunchServices know the bundle.
//
// LaunchServices' rules for a bundle outside /Applications and /var/containers were never tried on this guest, so
// the ways are tried in a fixed order, each logged, and the first that LaunchServices then reports as installed wins:
//   system-dict  registerApplicationDictionary: as a System app, not containerized (uicache's way for jailbreak roots)
//   url          registerApplication: with the bundle's URL
//   user-dict    registerApplicationDictionary: as a User app (userland/appinst register.m's way)
// Lines (stdout, which oneios-init keeps in log/appreg.log, and the unified log):
//   oneios-appreg: com.oneios.bench: system-dict: registered=1 installed=1 path=/private/var/ngpu/apps/OneiOSBench.app
//   oneios-appreg: done 2 of 2 apps registered (lsd up after 4 s)
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dict;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)unregisterApplication:(NSURL *)url;
- (NSArray *)allInstalledApplications;
@end

static void say(NSString *format, ...) NS_FORMAT_FUNCTION(1, 2);
static void say(NSString *format, ...) {
  va_list ap;
  va_start(ap, format);
  NSString *s = [[NSString alloc] initWithFormat:format arguments:ap];
  va_end(ap);
  printf("oneios-appreg: %s\n", s.UTF8String);
  fflush(stdout);
  NSLog(@"oneios-appreg: %@", s);
}

static id workspace(void) {
  Class cls = NSClassFromString(@"LSApplicationWorkspace");
  if (!cls) return nil;
  return [cls performSelector:@selector(defaultWorkspace)];
}

// What LaunchServices knows of the identifier now: the proxy's bundle path, or nil.
static NSString *installed_path(NSString *bundle_id) {
  Class proxy = NSClassFromString(@"LSApplicationProxy");
  if (!proxy) return nil;
  id p = [proxy performSelector:@selector(applicationProxyForIdentifier:) withObject:bundle_id];
  if (!p) return nil;
  @try {
    id installed = [p valueForKey:@"installed"];
    if (installed && ![installed boolValue]) return nil;
    NSURL *url = [p valueForKey:@"bundleURL"];
    return url.path;
  } @catch (__unused NSException *e) {
    return nil;
  }
}

// The entitlements of a signed Mach-O: the XML blob (magic 0xfade7171) of its code signature.
static NSDictionary *entitlements_of(NSString *path) {
  NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
  const uint8_t *b = data.bytes;
  NSUInteger n = data.length;
  for (NSUInteger i = 0; i + 8 < n; i += 4) {
    if (b[i] == 0xfa && b[i + 1] == 0xde && b[i + 2] == 0x71 && b[i + 3] == 0x71) {
      uint32_t len = ((uint32_t)b[i + 4] << 24) | ((uint32_t)b[i + 5] << 16) | ((uint32_t)b[i + 6] << 8) | b[i + 7];
      if (len < 8 || i + len > n) continue;
      NSData *xml = [data subdataWithRange:NSMakeRange(i + 8, len - 8)];
      id plist = [NSPropertyListSerialization propertyListWithData:xml options:0 format:NULL error:nil];
      if ([plist isKindOfClass:[NSDictionary class]]) return plist;
    }
  }
  return @{};
}

static NSMutableDictionary *base_dict(NSString *app, NSDictionary *info, NSString *bundle_id) {
  NSMutableDictionary *d = [NSMutableDictionary dictionaryWithDictionary:info];
  NSString *exe = info[@"CFBundleExecutable"];
  d[@"CFBundleIdentifier"] = bundle_id;
  d[@"CodeInfoIdentifier"] = bundle_id;
  d[@"CompatibilityState"] = @0;
  d[@"Path"] = app;
  d[@"SignerOrganization"] = @"OneiOS";
  d[@"SignerIdentity"] = @"Simulator";  // an ad hoc signature, as LaunchServices calls one
  d[@"HasMIDBasedSINF"] = @NO;
  d[@"MinimumOSVersion"] = info[@"MinimumOSVersion"] ?: @"27.0";
  if (exe) d[@"Entitlements"] = entitlements_of([app stringByAppendingPathComponent:exe]);
  return d;
}

static BOOL try_way(id ws, NSString *way, NSString *app, NSDictionary *info, NSString *bundle_id) {
  BOOL ok = NO;
  NSString *error = @"";
  @try {
    if ([way isEqualToString:@"system-dict"]) {
      NSMutableDictionary *d = base_dict(app, info, bundle_id);
      d[@"ApplicationType"] = @"System";
      d[@"IsDeletable"] = @NO;
      d[@"IsContainerized"] = @NO;
      ok = [ws registerApplicationDictionary:d];
    } else if ([way isEqualToString:@"url"]) {
      ok = [ws registerApplication:[NSURL fileURLWithPath:app isDirectory:YES]];
    } else {
      NSMutableDictionary *d = base_dict(app, info, bundle_id);
      d[@"ApplicationType"] = @"User";
      d[@"IsDeletable"] = @YES;
      d[@"IsContainerized"] = @NO;
      ok = [ws registerApplicationDictionary:d];
    }
  } @catch (NSException *e) {
    error = [NSString stringWithFormat:@" threw %@: %@", e.name, e.reason];
  }
  NSString *now = installed_path(bundle_id);
  BOOL here = now && [now.stringByStandardizingPath isEqualToString:app.stringByStandardizingPath];
  say(@"%@: %@: registered=%d installed=%d path=%@%@%@", bundle_id, way, ok, here, app,
      now && !here ? [NSString stringWithFormat:@" (LaunchServices has it at %@)", now] : @"", error);
  return here;
}

int main(int argc, char *argv[]) {
  @autoreleasepool {
    NSMutableArray *dirs = [NSMutableArray array];
    for (int i = 1; i < argc; i++) [dirs addObject:@(argv[i])];
    if (!dirs.count) [dirs addObjectsFromArray:@[ @"/private/var/ngpu/apps", @"/private/var/oneios/apps" ]];
    NSMutableArray *apps = [NSMutableArray array];
    for (NSString *dir in dirs)
      for (NSString *name in [[[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)])
        if ([name.pathExtension isEqualToString:@"app"]) [apps addObject:[dir stringByAppendingPathComponent:name]];
    if (!apps.count) {
      say(@"no app bundles in %@", [dirs componentsJoinedByString:@" "]);
      return 0;
    }
    // lsd comes up with the user session; wait for an answer from it (up to 3 minutes).
    id ws = nil;
    int waited = 0;
    for (; waited < 180; waited += 2) {
      ws = workspace();
      if (ws && installed_path(@"com.apple.Preferences")) break;
      sleep(2);
    }
    if (!ws) {
      say(@"LSApplicationWorkspace unavailable after %d s: nothing registered", waited);
      return 1;
    }
    int done = 0;
    for (NSString *app in apps) {
      NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
      NSString *bundle_id = info[@"CFBundleIdentifier"];
      if (!bundle_id) {
        say(@"%@: no CFBundleIdentifier in Info.plist; skipped", app);
        continue;
      }
      NSString *before = installed_path(bundle_id);
      if (before && [before.stringByStandardizingPath isEqualToString:app.stringByStandardizingPath]) {
        say(@"%@: already registered at %@ (kept by LaunchServices)", bundle_id, before);
        done++;
        continue;
      }
      for (NSString *way in @[ @"system-dict", @"url", @"user-dict" ]) {
        if (try_way(ws, way, app, info, bundle_id)) {
          done++;
          break;
        }
      }
    }
    say(@"done %d of %lu apps registered (lsd answered after %d s)", done, (unsigned long)apps.count, waited);
    return done == (int)apps.count ? 0 : 2;
  }
}
