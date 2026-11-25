// vm.c — request (never force) the guest-VM restart that commits a staged install.
//
// A restart is where xnuhost re-reads the trust inbox and injects the new CDHashes (docs/booter.md: the guest
// can halt itself via PSCI, and xnuhost treats a halt-in-window as a restart). But in this project a restart is
// the owner's decision (see the "owner restarts" rule), so appinst does NOT halt the guest itself. It drops a
// request flag that the owner — or a host-side watcher — acts on, and the UI/CLI tells the user a restart is
// needed to finish. We write the flag and report success; the actual restart stays a human/host action.
#include "appinst.h"

#include <stdio.h>
#include <time.h>

#define APPINST_RESTART_FLAG APPINST_ROOT "/restart-requested"

int appinst_request_restart(void) {
  FILE *f = fopen(APPINST_RESTART_FLAG, "w");
  if (!f) return -1;
  fprintf(f, "appinst requested a guest-VM restart at %ld to commit staged installs.\n"
             "On the next boot xnuhost folds %s/*.json into the owner trust module and\n"
             "`appinst finalize` registers the staged apps. Restarting is the owner's call.\n",
          (long)time(NULL), APPINST_TRUST_INBOX);
  fclose(f);
  return 0;
}
