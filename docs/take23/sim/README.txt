The PC checks of take23's two shell blocks (oneios-tool, crates/oneios-phone/src/take23.rs: PICK and ORCH), run under dash
against fake files. They are the blocks' text taken out of take23.rs (the __NAMES__ filled in), with /sys and /proc paths
replaced by a folder of fake files; see the python lines in the analysis' history (pick.tpl, orch.sh are made that way).
  drive.sh        the orchestrator: idle, 60 % and 80 % GPU load, back to 10 %, a good and a bad round of the guest, a hot
                  sensor, the end (the cap put back, the clamp file gone)
  pick-drive.sh   the choice of the Image: MemAvailable_kB POOL_AVAIL POOL_SIZE_MAX|none PAGES_THE_MODULE_REACHES
Nothing here runs on the phone.
