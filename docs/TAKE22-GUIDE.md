# OneiOS take22: guida passo passo (Galaxy S25 Ultra, KernelSU)

Questa guida spiega come rifare dal sorgente la configurazione **take22** di OneiOS (iOS 27.0.1 24A446 come VM sotto
Gunyah, dentro il crosvm di Droid-VM) e come avviarla sul telefono.

## Da sapere prima

- **take22 non è mai stata provata sul telefono.** È stata costruita e controllata solo sul PC (banco QEMU). La sessione
  provata più recente è take21b.
- Le immagini del proprietario contengono ancora il **plug-in grafico di take21**: le due correzioni del plug-in
  (registrazione schermo `64cf85c`, prewarm `6b8d8b4`, ramo `ci/metal-upgrade`) non sono mai state compilate dalla CI.
  Se compili il plug-in da questo ramo, il tuo avrà le correzioni: è una cosa che non ha ancora girato da nessuna parte.
- Il proprietario avvia le VM con l'`oneios-tool` di take19. Qui trovi i sorgenti più recenti (take22), che sono
  compilati ma non provati per `vm-boot`.
- Niente in questo ramo contiene file Apple. Le parti di iOS le scarichi tu da Apple (passo 3).

## 1. Requisiti: controlla che il tuo telefono sia uguale

Quello del proprietario, misurato il 19-11-2026 (oggi il telefono non era collegato, quindi non è stato riletto):

| Cosa | Valore del proprietario | Come lo leggi tu |
|---|---|---|
| Modello | **SM-S938B** (Galaxy S25 Ultra, Snapdragon 8 Elite SM8750, nome `pa3qxeea`) | `adb shell getprop ro.product.model` |
| Firmware | **S938BXXSDDZJ1** (Android 17, One UI 9.0) | `adb shell getprop ro.build.version.incremental` |
| Kernel | **6.6.127-android15-8** (GKI android15-6.6, pagine da 4 KiB) | `adb shell uname -r` |
| Root | **KernelSU** (sul telefono del proprietario è temporaneo: si perde a ogni riavvio) | `adb shell su -c id` deve dire `uid=0` |

- Il modello **deve** essere SM-S938B (altri S938 come la U hanno hardware diverso: non provato).
- La prima parte del kernel (`6.6.127-android15-8`) deve essere uguale: i moduli del passo 4 sono compilati per questa
  KMI. Un firmware diverso può funzionare lo stesso, ma `oneios-tool` controlla il kernel prima di caricare i moduli
  e si rifiuta se non combacia: in quel caso fermati.
- Serve almeno ~12 GB di RAM sul telefono (la VM ne usa 4,3-4,6 GB) e ~20 GB liberi su `/data`.

Sul PC (Linux, oppure macOS): vedi [building.md](building.md), sezione 0. In breve: python3, Rust (`rustup` 1.98.0),
clang, libdispatch con BlocksRuntime, Android NDK 28.2, git, zstd, curl, bubblewrap (per i moduli), `adb`, `gh`, `openssl`.
Su disco servono circa 60 GB.

## 2. Scarica il ramo

```sh
git clone -b take22 https://github.com/YumikoHayamato/OneiOS
cd OneiOS
scripts/setup/get-references.sh      # i sorgenti di riferimento in external/
kernel/fetch.sh                      # i sorgenti del kernel ai commit fissati
```

## 3. Le parti di iOS da Apple (immagine di sistema)

```sh
P='profiles/ios-27.0.1-iPhone18,2.toml'
scripts/fetch/get-ipsw-cli.sh
scripts/bg.sh fetch scripts/fetch/fetch.sh "$P"   # circa 10,6 GiB dall'IPSW di Apple, in inputs/24A446/
scripts/image/kernel-inputs.sh "$P"               # kernelcache e device tree -> out/kernel-inputs/24A446/
```

Lo script controlla l'impronta SHA-256 del kernelcache: se non combacia si ferma.

## 4. I moduli del kernel Android

Servono quattro moduli di Droid-VM. Il loro sorgente (GPL) resta fuori da questo repository: gli script lo scaricano
a un commit fissato e lo compilano in un'immagine DDK (anch'essa fissata per impronta), sotto bubblewrap, senza rete.
`WORK` è una cartella tua (es. `$HOME/oneios-hugepage`).

```sh
WORK=$HOME/oneios-hugepage
scripts/hugepage/build.sh "$WORK"          # gh_hugepage_reserve -> $WORK/out/android15-6.6.ko (+ kapi_check)
scripts/hugepage/build-share.sh "$WORK"    # gunyah_host_share   -> $WORK/out/gunyah_host_share-android15-6.6.ko
scripts/hugepage/build-udmabuf.sh "$WORK"  # udmabuf_kv          -> $WORK/out/udmabuf-android15-6.6.ko
```

- **haltpoll non si compila**: è il file già pronto di Droid-VM (`gunyah-halt-poll-gki-6.6.ko`, DroidVM-Prebuilt-Root
  `f7c6ce8`). Lo scarichi a mano; `oneios-tool` controlla la sua impronta SHA-256 (scritta in
  `tools/oneios-tool/crates/oneios-phone/src/haltpoll.rs`) prima di caricarlo.
- Ogni file `.txt` in `$WORK/out/` riporta commit e `modinfo`: tienili.

## 5. oneios-tool, kernel di take22 e collezione (`kernel.kc`)

```sh
cargo build --release --manifest-path tools/oneios-tool/Cargo.toml
scripts/take22/kbuild.sh                   # -> out/kernel/native-take22/kernel.kc  (circa 25 minuti la prima volta)
```

Lo script fa i tre passi del proprietario: `oneios-tool`, `kernel/build-native.sh` con `NATIVE_OUT=take22`, poi
`kernel/prelink/build.sh`. Se libdispatch non è in `/usr`, passa `LIBDISPATCH_PREFIX=...`. I log vanno in
`out/take22-logs/`.

Il kernel di take22 contiene tra l'altro: le patch 0084-0090 (rapporto memoria, pressione, ruolo GPU, strati del display,
servizi dell'addon), il "passo zero" del volume (`oneios-audio-zero-step`), il controllo jetsam V54.

## 6. Il plug-in grafico Metal (GitHub Actions sul tuo fork)

Il plug-in si compila solo su macOS con Xcode 27, quindi lo fa la CI di GitHub. Lo fai sul **tuo** fork, con la **tua**
chiave: quella del proprietario non si condivide.

1. Fai un fork di questo repository su GitHub e, nella pagina **Actions** del fork, abilita i workflow (nei fork sono spenti).
2. Crea la tua chiave e mettila come secret del fork:
   ```sh
   openssl rand -hex 32 > ~/metal-artifact.key          # tienila privata, NON metterla nel repository
   gh secret set METAL_ARTIFACT_KEY --repo TUO_UTENTE/OneiOS < ~/metal-artifact.key
   ```
3. Fai partire la corsa: il workflow parte con un push sui rami `ci/metal-plugin`, `ci/metal-upgrade` o `ci/apps`
   (un push su `take22` **non** fa partire niente; il pulsante "Run workflow" funziona solo dal ramo principale).
   ```sh
   git remote add mio https://github.com/TUO_UTENTE/OneiOS
   git push mio take22:ci/metal-upgrade      # plug-in (stack 555b13d) + app, circa 40-60 minuti
   ```
4. Scarica e decifra i due artefatti (restano su GitHub un giorno):
   ```sh
   gh run list --repo TUO_UTENTE/OneiOS --branch ci/metal-upgrade      # prendi l'ID della corsa riuscita
   gh run download ID --repo TUO_UTENTE/OneiOS -n metal-plugin -D dl
   gh run download ID --repo TUO_UTENTE/OneiOS -n oneios-apps -D dl
   mkdir -p ci-metal ci-apps
   openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass file:$HOME/metal-artifact.key \
     -in dl/metal-plugin.tar.zst.enc | zstd -d | tar -x -C ci-metal
   openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass file:$HOME/metal-artifact.key \
     -in dl/oneios-apps.tar.zst.enc | zstd -d | tar -x -C ci-apps
   ```
   Cerca dentro `ci-metal` la cartella con `NativeAdrenoMetal.gpuaddon` e `NativeAdrenoMetal.build.json`, e dentro
   `ci-apps` quella con `OneiOS2048.app`, `OneiOSBench.app`, `oneios-appreg`, `appreg.conf`, `cdhashes.txt`.

Nota: il workflow usa il runner `xcode-27` di GitHub. Se sul tuo account quel runner non c'è, la corsa resta in coda:
non c'è un'alternativa pronta (il proprietario ha avuto corse annullate o ferme in coda, vedi sopra).

## 7. Il set di immagini, l'addon e le immagini della VM

```sh
# il set di immagini di iOS (dischi di sistema, Data store iniziale, lista di fiducia), circa 11 GB
scripts/bg.sh image scripts/image/build.sh "$P" --kernel out/kernel/native-take22/kernel.kc
scripts/image/verify.sh out/image/OneiOS_iOS27.0.1_24A446_iPhone18-2.tar.zst

# addon unico (plug-in + app) e la copia del set che si fida dei loro CDHash
M=ci-metal/CARTELLA A=ci-apps/CARTELLA NOTE="ci/metal-upgrade mia corsa" scripts/take22/mk-addon.sh
#   -> out/take22/ALL-metal-apps.gpuaddon, out/take22/set/24A446, out/take22/ALL-imageset/

# le quattro immagini della VM (argomenti di avvio e opzioni identici a quelli di take22)
scripts/take22/images.sh
#   -> out/take22/images/kernel-take22-ALL-4608-v54.img   (principale)
#      kernel-take22-ALL-4352-v54.img                     (se Android non lascia RAM per la 4608)
#      kernel-take22-ALL-4608-v54-gpuirq.img              (prova A/B, dopo la principale)
#      kernel-take22-ALL-4352-v54-loggpu.img              (diagnosi, non per giudicare la fluidità)
```

Attenzione: il proprietario non ha costruito il set così da zero per take22; ha riusato il set di take21 (stesso
procedimento). `mk-addon.sh` si ferma se il `manifest.json` del set non ha le righe che deve cambiare.

Gli strumenti che `vm-boot` porta sul telefono (servono l'NDK):

```sh
make -C runtime/display        # build/android/displayctl (+ l'helper del display)
make -C runtime/bridges        # build/android/bridgectl, build/android/wifictl
scripts/droidvm/build-virgl.sh # libvirglrenderer.so (con le due patch di scripts/droidvm/virgl-patches)
```

Il crosvm di Droid-VM: `oneios-tool phone droidvm-crosvm` (vedi [native.md](native.md), "A VM under Gunyah").

## 8. Sul telefono: moduli e pool di memoria

Ogni volta che il telefono si riavvia (prima rifai il root di KernelSU). `T=tools/oneios-tool/target/release/oneios-tool`.

```sh
$T phone status                                                   # root e stato, sola lettura
$T phone hugepool load --ko $WORK/out/android15-6.6.ko --kapi-check $WORK/out/kapi_check   # pool da 2 MiB
$T phone hugepool grow --want 2560                                # riempie il pool (One UI si ferma e riparte, il telefono no)
$T phone hostshare load --ko $WORK/out/gunyah_host_share-android15-6.6.ko
$T phone udmabuf load --ko $WORK/out/udmabuf-android15-6.6.ko
$T phone haltpoll load --ko PERCORSO/gunyah-halt-poll-gki-6.6.ko
$T phone hugepool status                                          # controlla pool_avail
```

- Non copiare a mano gli `insmod` o gli offset del proprietario: lascia fare a `oneios-tool`, che fa i suoi controlli
  (`kapi_check` per gh_hugepage_reserve, il controllo del kernel per hostshare) e si rifiuta se qualcosa non torna.
- 2560 pagine da 2 MiB = 5 GiB: basta per l'immagine 4608. Se `grow` non arriva, usa l'immagine 4352.
- Passa sempre `--ko` (e `--kapi-check`): i percorsi predefiniti sono quelli del PC del proprietario.

## 9. Avvio

**Prima di ogni sessione, fai una copia del Data store della VM** (è il "disco" di iOS con le tue impostazioni):

```sh
$T phone pull-data --vm backup-data-$(date +%F).img
```

Poi:

```sh
I=out/take22/images
$T phone vm-boot --image $I/kernel-take22-ALL-4608-v54.img --seconds 3600 --thp --takeover --pin \
  --pin-first 2 --thermal-log --perf-log --touch-boost \
  --bridge runtime/display/build/android/displayctl --bridgectl runtime/bridges/build/android/bridgectl \
  --wifictl runtime/bridges/build/android/wifictl --virgl PERCORSO/libvirglrenderer.so \
  --out sessioni/take22
```

- La prima sessione ricompila gli shader: giudica la fluidità dalla seconda.
- Cosa guardare nei log e gli interruttori (argomenti di avvio): i commenti di `scripts/take22/images.sh` e
  [native.md](native.md). Ogni argomento diverso = un'immagine nuova con `scripts/take22/images.sh`, nessuna ricompilazione.

## 10. Sicurezza e come uscire

- **Backup**: `phone pull-data --vm` prima di ogni sessione; tieni le copie. Se il Data store si rovina, rimetti la copia.
  Dopo take22 due app restano copiate in `/private/var/staged_system_apps` nel Data store: con un'immagine più vecchia
  (take20 o prima) non partono.
- **Uscire**:
  - dal PC: `$T phone vm-boot --stop`;
  - senza PC: tieni premuto **volume giù per 10 secondi**, la VM si ferma e One UI torna;
  - da solo: alla fine dei secondi di `--seconds`.
- Se qualcosa va storto c'è il "failsafe": il telefono si riavvia in One UI. Con il root temporaneo, dopo il riavvio
  il root non c'è più: rifallo prima della sessione successiva. Un panic del kernel Android sotto carico può venire
  dall'exploit del root, non da OneiOS: leggi il pstore (`$T phone dmesg --previous`) prima di dare la colpa al codice.
- Non usare un'immagine con meno di ~1300 MiB di RAM con questi argomenti: la soglia di memoria a 600 MB deve restare
  sotto metà della RAM, altrimenti il kernel si ferma all'avvio.
- Non mettere mai nel repository la tua chiave `METAL_ARTIFACT_KEY`, gli artefatti decifrati, le immagini o file di iOS.
