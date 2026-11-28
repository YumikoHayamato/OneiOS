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
// What take21 showed on the phone (iOS 27.0.1, 2026-11-27): LaunchServices refuses both dictionary ways
// ("You cannot use -[LSApplicationWorkspace registerApplicationDictionary:] to register applications anymore") and
// the URL way for a bundle outside its own folders ("no install info, bundle at ... will not be registered"). The one
// registration known to work on this guest is the system apps' own: the kernel shows /private/var/staged_system_apps
// over /Applications (hosted 0035) and lsd keeps what it registered from there (0037). So each bundle is first COPIED
// into /private/var/staged_system_apps (the source stays where the addon wrote it; a copy is replaced only when the
// executable or Info.plist differs), and then these ways are tried in order, each logged, the first that
// LaunchServices then reports as installed wins:
//   apps-url     registerApplication: with /Applications/NAME.app (a system-app path now, so install info exists)
//   rebuild      _LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user: (YES, NO, NO): lsd asks installd for
//                the system apps again (uicache --all's way); asked once per run, for all bundles
//   url          registerApplication: with the bundle where the addon wrote it (take21's failing way, kept as a probe)
// ONEIOS_APPREG_STAGE=0 in the environment (appreg.conf: env=) skips the copy and the first two ways: take21's
// behaviour. ONEIOS_APPREG_REBUILD=0 leaves out only the rebuild way (it makes lsd go through every system app again). NOT TESTED on the phone when written: read log/appreg.log after the first boot.
// Lines (stdout, which oneios-init keeps in log/appreg.log, and the unified log):
//   oneios-appreg: com.oneios.bench: staged /private/var/staged_system_apps/OneiOSBench.app (copied)
//   oneios-appreg: com.oneios.bench: apps-url: registered=1 installed=1 path=/Applications/OneiOSBench.app
//   oneios-appreg: done 2 of 2 apps registered (lsd up after 4 s)
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface LSApplicationWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dict;
- (BOOL)registerApplication:(NSURL *)url;
- (BOOL)unregisterApplication:(NSURL *)url;
- (NSArray *)allInstalledApplications;
- (BOOL)_LSPrivateRebuildApplicationDatabasesForSystemApps:(BOOL)system internal:(BOOL)internal user:(BOOL)user;
@end

static NSString *const kStagedDir = @"/private/var/staged_system_apps";
static NSString *const kAppsDir = @"/Applications";

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

// Is LaunchServices' path for the identifier one of ours (the source, its staged copy, or the copy seen in /Applications)?
static BOOL is_here(NSString *now, NSArray<NSString *> *paths) {
  if (!now) return NO;
  NSString *n = now.stringByStandardizingPath;
  for (NSString *p in paths)
    if ([n isEqualToString:p.stringByStandardizingPath]) return YES;
  return NO;
}

static BOOL same_file(NSString *a, NSString *b) {
  NSData *x = [NSData dataWithContentsOfFile:a], *y = [NSData dataWithContentsOfFile:b];
  return x && y && [x isEqualToData:y];
}

// The bundle's copy in the staged system apps' folder: made when missing, replaced when its executable or Info.plist
// differs from the source. Returns the copy's name ("NAME.app") or nil; `what` says what was done.
static NSString *stage_copy(NSString *app, NSDictionary *info, NSString **what) {
  NSFileManager *fm = [NSFileManager defaultManager];
  BOOL dir = NO;
  if (![fm fileExistsAtPath:kStagedDir isDirectory:&dir] || !dir) {
    *what = @"no staged system apps folder";
    return nil;
  }
  NSString *name = app.lastPathComponent, *copy = [kStagedDir stringByAppendingPathComponent:name];
  NSString *exe = info[@"CFBundleExecutable"];
  if ([fm fileExistsAtPath:copy]) {
    if (exe && same_file([app stringByAppendingPathComponent:exe], [copy stringByAppendingPathComponent:exe]) &&
        same_file([app stringByAppendingPathComponent:@"Info.plist"], [copy stringByAppendingPathComponent:@"Info.plist"])) {
      *what = @"kept";
      return name;
    }
    NSError *e = nil;
    if (![fm removeItemAtPath:copy error:&e]) {
      *what = [NSString stringWithFormat:@"old copy not removed: %@", e.localizedDescription];
      return nil;
    }
  }
  // Copied under a temporary name and renamed, so a half-written bundle is never seen in /Applications.
  NSString *tmp = [kStagedDir stringByAppendingPathComponent:[@".oneios-" stringByAppendingString:name]];
  [fm removeItemAtPath:tmp error:nil];
  NSError *e = nil;
  if (![fm copyItemAtPath:app toPath:tmp error:&e] || ![fm moveItemAtPath:tmp toPath:copy error:&e]) {
    [fm removeItemAtPath:tmp error:nil];
    *what = [NSString stringWithFormat:@"copy failed: %@", e.localizedDescription];
    return nil;
  }
  *what = @"copied";
  return name;
}

static BOOL register_url(id ws, NSString *path, NSString **error) {
  @try {
    return [ws registerApplication:[NSURL fileURLWithPath:path isDirectory:YES]];
  } @catch (NSException *e) {
    *error = [NSString stringWithFormat:@" threw %@: %@", e.name, e.reason];
    return NO;
  }
}

int main(int argc, char *argv[]) {
  @autoreleasepool {
    NSMutableArray *dirs = [NSMutableArray array];
    for (int i = 1; i < argc; i++) [dirs addObject:@(argv[i])];
    if (!dirs.count) [dirs addObjectsFromArray:@[ @"/private/var/ngpu/apps", @"/private/var/oneios/apps" ]];
    const char *stage_env = getenv("ONEIOS_APPREG_STAGE");
    BOOL stage = !(stage_env && !strcmp(stage_env, "0"));
    const char *rebuild_env = getenv("ONEIOS_APPREG_REBUILD");
    BOOL may_rebuild = !(rebuild_env && !strcmp(rebuild_env, "0"));
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
    if (!stage) say(@"ONEIOS_APPREG_STAGE=0: no copy into %@, the url way only", kStagedDir);
    int done = 0;
    BOOL rebuilt = NO;
    for (NSString *app in apps) {
      NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:[app stringByAppendingPathComponent:@"Info.plist"]];
      NSString *bundle_id = info[@"CFBundleIdentifier"];
      if (!bundle_id) {
        say(@"%@: no CFBundleIdentifier in Info.plist; skipped", app);
        continue;
      }
      NSString *name = nil;
      if (stage) {
        NSString *what = @"";
        name = stage_copy(app, info, &what);
        say(@"%@: staged %@ (%@)", bundle_id, name ? [kStagedDir stringByAppendingPathComponent:name] : @"nothing", what);
      }
      NSString *in_apps = name ? [kAppsDir stringByAppendingPathComponent:name] : nil;
      NSString *in_staged = name ? [kStagedDir stringByAppendingPathComponent:name] : nil;
      NSMutableArray<NSString *> *ours = [NSMutableArray arrayWithObject:app];
      if (name) [ours addObjectsFromArray:@[ in_apps, in_staged ]];
      NSString *before = installed_path(bundle_id);
      // A staged copy wins over an older registration of the source path (none exists on iOS 27; kept simple).
      if (is_here(before, name ? @[ in_apps, in_staged ] : @[ app ])) {
        say(@"%@: already registered at %@ (kept by LaunchServices)", bundle_id, before);
        done++;
        continue;
      }
      NSMutableArray<NSString *> *ways = [NSMutableArray array];
      if (name) [ways addObject:@"apps-url"];
      if (name && may_rebuild) [ways addObject:@"rebuild"];
      [ways addObject:@"url"];
      for (NSString *way in ways) {
        BOOL ok = NO;
        NSString *error = @"", *path = app;
        if ([way isEqualToString:@"apps-url"]) {
          path = in_apps;
          if (![[NSFileManager defaultManager] fileExistsAtPath:[path stringByAppendingPathComponent:@"Info.plist"]]) {
            say(@"%@: apps-url: %@ is not visible (the view of the staged apps is off?): skipped", bundle_id, path);
            continue;
          }
          ok = register_url(ws, path, &error);
        } else if ([way isEqualToString:@"rebuild"]) {
          path = in_apps;
          if (rebuilt) {
            ok = YES;  // asked already in this run: only look again
          } else if (![ws respondsToSelector:@selector(_LSPrivateRebuildApplicationDatabasesForSystemApps:internal:user:)]) {
            say(@"%@: rebuild: LaunchServices has no such method here: skipped", bundle_id);
            continue;
          } else {
            @try {
              ok = [ws _LSPrivateRebuildApplicationDatabasesForSystemApps:YES internal:NO user:NO];
            } @catch (NSException *e) {
              error = [NSString stringWithFormat:@" threw %@: %@", e.name, e.reason];
            }
            rebuilt = YES;
          }
        } else {
          ok = register_url(ws, path, &error);
        }
        NSString *now = installed_path(bundle_id);
        // lsd may answer a rebuild before it has gone through installd's list: look again for up to 20 s.
        for (int i = 0; i < 10 && [way isEqualToString:@"rebuild"] && ok && !is_here(now, ours); i++) {
          sleep(2);
          now = installed_path(bundle_id);
        }
        BOOL here = is_here(now, ours);
        say(@"%@: %@: registered=%d installed=%d path=%@%@%@", bundle_id, way, ok, here, path,
            now && !here ? [NSString stringWithFormat:@" (LaunchServices has it at %@)", now] : @"", error);
        if (here) {
          done++;
          break;
        }
      }
    }
    say(@"done %d of %lu apps registered (lsd answered after %d s)", done, (unsigned long)apps.count, waited);
    return done == (int)apps.count ? 0 : 2;
  }
}
