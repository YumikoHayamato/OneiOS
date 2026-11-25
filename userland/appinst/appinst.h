// appinst: on-device .ipa installer for the OneiOS guest. Shared paths and the two-phase contract.
//
// Why two phases: on this SPTM/TXM device the only thing that makes a code signature valid is the CDHash being
// in a trust cache, and the guest's trust cache is injected by xnuhost at boot (docs/booter.md). So a freshly
// re-signed app is NOT trusted until the next guest-VM restart. installd validates the signature at install
// time, so registering before the restart would be rejected.
//
//   Phase 1  `appinst install FILE.ipa`  (pre-restart): unzip, thin to arm64e, refuse an encrypted slice,
//            re-sign ad-hoc at 4 KiB with ldid, compute CDHashes, drop them in the trust inbox for xnuhost to
//            fold into the owner trust module, place the .app and its container, write a pending marker, then
//            ask for a guest-VM restart.
//   (restart) xnuhost reads the trust inbox and injects the new CDHashes; TXM now trusts them.
//   Phase 2  `appinst finalize`  (at boot, before SpringBoard): register each pending app with
//            installd/LaunchServices now that its signature validates, then clear the marker.
//
// `appinstd` runs finalize once, then serves the localhost install page.
#ifndef ONEIOS_APPINST_H
#define ONEIOS_APPINST_H

#define APPINST_ROOT        "/private/var/oneios/appinst"
#define APPINST_WORK        APPINST_ROOT "/work"          // temp unzip/sign scratch
#define APPINST_PENDING     APPINST_ROOT "/pending"       // <bundleid>.plist markers for phase 2
#define APPINST_LDID        "/private/var/oneios/bin/ldid"// on-device ad-hoc signer (see README)

// The trust inbox: one JSON per app, in exactly the owner-trust-extra shape make-owner-trust-bundle.py reads
// ({"cdhashes":[...hex...],"category":0}). xnuhost folds every file here into the owner trust module it injects
// at the next guest boot. This directory must live where xnuhost can read it across a restart (README "host wiring").
#define APPINST_TRUST_INBOX "/private/var/oneios/trust-inbox"

// Where third-party apps and their data live, as on stock iOS.
#define APPINST_BUNDLE_ROOT "/private/var/containers/Bundle/Application"
#define APPINST_DATA_ROOT   "/private/var/mobile/Containers/Data/Application"

#define APPINST_HTTP_PORT   8088   // localhost only

// subcommands
int appinst_install(const char *ipa_path);   // phase 1
int appinst_finalize(void);                   // phase 2 (idempotent; safe to run every boot)
int appinst_serve(int port);                  // localhost install page (serve.m)

// Register an already-placed, now-trusted bundle with installd/LaunchServices. register.m. 0 on success.
int appinst_register_bundle(const char *app_path, const char *bundle_id, char *err, int errsz);

// Ask the host to restart the guest VM (vm.c). Best-effort; returns 0 if a restart was requested, -1 if the
// caller must tell the user to restart by hand.
int appinst_request_restart(void);

#endif
