#!/bin/sh
# How old each external pin of this checkout is, and how far it lags the head of the branch it comes from, Droid-VM's own
# pin of the same component (what their iPadOS 27 guest is built from) and, where there is one, the upstream repository.
#
#   scripts/pin-ages.sh [--tree DIR]      DIR: the checkout whose pins are read (default: this script's)
#
# Read from the tree: scripts/setup/get-references.sh (the reference table), kernel/pins.sh (the kernel's sources),
# scripts/droidvm/fetch-crosvm.sh and build-virgl.sh (the phone's crosvm prebuilt and the renderer built for it),
# scripts/hugepage/build.sh, build-share.sh and build-udmabuf.sh (the phone's kernel modules). A file a tree lacks gives
# "no pin in this tree" rows. Asked of GitHub with `gh api` (read only; needs `gh auth login` for the rate limit):
# each pin's date, each branch's head and date, the commits between them (compare: "+N" the head is N commits ahead of
# the pin; "-M" M commits of the pin are not in that branch).
# A row: COMPONENT  PIN date  | BRANCH head date +ahead[-behind] days | ...
set -u
TREE=$(cd "$(dirname "$0")/.." && pwd)
[ "${1:-}" = --tree ] && TREE=$2
REFS=$TREE/scripts/setup/get-references.sh
KPINS=$TREE/kernel/pins.sh
command -v gh >/dev/null 2>&1 || { echo "pin-ages: needs gh (GitHub's CLI)" >&2; exit 1; }

api() { _o=$(gh api "$@" 2>/dev/null) && printf '%s\n' "$_o"; }   # nothing on a refusal (gh prints the error's JSON)
short() { printf %.7s "$1"; }
cdate() { api "repos/$1/commits/$2" --jq '.commit.committer.date[0:10]' || echo '?'; }       # REPO REF -> YYYY-MM-DD
chead() { api "repos/$1/commits/$2" --jq '.sha + " " + .commit.committer.date[0:10]' || echo '? ?'; }   # REPO BRANCH
ccmp() { api "repos/$1/compare/$2...$3" --jq '"+\(.ahead_by)" + (if .behind_by > 0 then "-\(.behind_by)" else "" end)' || echo '+?'; }
days() { # FROM TO (YYYY-MM-DD) -> days, with GNU date; "?" otherwise
  a=$(date -d "$1" +%s 2>/dev/null) && b=$(date -d "$2" +%s 2>/dev/null) && echo $(( (b - a) / 86400 )) || echo '?'
}
table_pin() { # FILE NAME -> the commit of the row NAME (get-references.sh, pins.sh)
  [ -f "$1" ] && awk -v n="$2" '$1 == n && $3 ~ /^[0-9a-f]{40}$/ {print $3; exit}' "$1"
}
var_pin() { # FILE PATTERN -> the 40-hex commit after COMMIT= on the first line matching PATTERN (COMMIT=x, COMMIT=${2:-x})
  [ -f "$1" ] && sed -n "/$2/s/.*COMMIT=.\\{0,6\\}\\([0-9a-f]\\{40\\}\\)[^0-9a-f].*/\\1/p;/$2/s/.*COMMIT=.\\{0,6\\}\\([0-9a-f]\\{40\\}\\)\$/\\1/p" "$1" | head -1
}
TODAY=$(date +%Y-%m-%d)

# against REPO BRANCH [LABEL]: one "| branch head date +N days" cell for the current PIN/PINDATE
against() {
  set -- "$1" "$2" "${3:-$2}"
  h=$(chead "$1" "$2"); hs=${h%% *}; hd=${h##* }
  if [ "$hs" = '?' ]; then printf ' | %s: ?' "$3"; return; fi
  if [ "$hs" = "$PIN" ]; then printf ' | %s: at its head (%s)' "$3" "$hd"; return; fi
  printf ' | %s %s %s %s, %s d' "$3" "$(short "$hs")" "$hd" "$(ccmp "$1" "$PIN" "$hs")" "$(days "$PINDATE" "$hd")"
}
# their REPO SHA LABEL: Droid-VM's own pin of the component
their() {
  [ -n "$2" ] || { printf ' | %s: ?' "$3"; return; }
  if [ "$2" = "$PIN" ]; then printf ' | %s: the same commit' "$3"; return; fi
  td=$(cdate "$1" "$2")
  printf ' | %s %s %s %s, %s d' "$3" "$(short "$2")" "$td" "$(ccmp "$1" "$PIN" "$2")" "$(days "$PINDATE" "$td")"
}
# begin LABEL REPO PIN: starts a row; false (and a full row) when the tree has no such pin
begin() {
  LABEL=$1 REPO=$2 PIN=$3
  if [ -z "$PIN" ]; then printf '%-34s no pin in this tree\n' "$LABEL"; return 1; fi
  PINDATE=$(cdate "$REPO" "$PIN")
  printf '%-34s %s %s (%s d old)' "$LABEL" "$(short "$PIN")" "$PINDATE" "$(days "$PINDATE" "$TODAY")"
}
end() { printf '\n'; }

# Droid-VM's own pins: the tree their XNU guests are built from (Droid-VM/UniXNU wip/hackintosh)
DV=Droid-VM/UniXNU DVB=wip/hackintosh
sub() { api "repos/$DV/contents/$1?ref=$DVB" --jq .sha; }   # a submodule's commit there
raw() { api -H 'Accept: application/vnd.github.raw' "repos/$DV/contents/$1?ref=$DVB"; }
DV_M2V=$(raw scripts/data/gpu-metal-vulkan-inputs.json | sed -n '/"translator"/,/}/s/.*"commit": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1)
DV_MESA=$(raw scripts/data/adreno-compiler-inputs.json | sed -n 's/.*"commit": *"\([0-9a-f]\{40\}\)".*/\1/p' | head -1)

echo "pin-ages: $TREE at $(git -C "$TREE" rev-parse --short HEAD 2>/dev/null || echo '?'), $TODAY; Droid-VM's own pins: $DV $DVB $(short "$(chead $DV $DVB)")"
echo "(+N: commits the other side is ahead of the pin; -M: commits of the pin it lacks; d: days between the two commits)"
echo
echo "== The guest's Metal stack (userland/metal)"
begin 'metal2vulkan (stack 555b13d)' steelbrain/metal2vulkan "$(table_pin "$REFS" metal2vulkan-555b13d)" && {
  their steelbrain/metal2vulkan "$DV_M2V" "Droid-VM's pin"; against steelbrain/metal2vulkan development; end; }
begin 'metal2vulkan (stack 624bab3)' steelbrain/metal2vulkan "$(table_pin "$REFS" metal2vulkan)" && {
  their steelbrain/metal2vulkan "$DV_M2V" "Droid-VM's pin"; against steelbrain/metal2vulkan development; end; }
begin 'NativeMetal (stack 555b13d)' Droid-VM/NativeMetal "$(table_pin "$REFS" NativeMetal-935c468)" && {
  their Droid-VM/NativeMetal "$(sub drivers/AdrenoMetal)" "Droid-VM's pin"; against Droid-VM/NativeMetal wip/hackintosh; against Droid-VM/NativeMetal macOS; end; }
begin 'NativeMetal (stack 624bab3)' Droid-VM/NativeMetal "$(table_pin "$REFS" NativeMetal)" && {
  their Droid-VM/NativeMetal "$(sub drivers/AdrenoMetal)" "Droid-VM's pin"; against Droid-VM/NativeMetal wip/hackintosh; end; }
printf '%-34s %s\n' '  upstream of NativeMetal' "UniXNU/AdrenoMetal main $(chead UniXNU/AdrenoMetal main | awk '{printf "%.7s %s", $1, $2}') (another history: no count)"
begin 'UniXNU scripts+patches (555b13d)' Droid-VM/UniXNU "$(table_pin "$REFS" Droid-VM-UniXNU-8789b8d)" && {
  against Droid-VM/UniXNU wip/hackintosh; end; }
begin 'mesa-droidvm (Turnip)' Droid-VM/mesa "$(table_pin "$REFS" mesa-droidvm)" && {
  their Droid-VM/mesa "$DV_MESA" "Droid-VM's pin"; against Droid-VM/mesa droidvm; against Droid-VM/mesa wip/rebase-27.0; end; }
printf '%-34s %s\n' '  upstream of Mesa, virglrenderer' 'gitlab.freedesktop.org (not asked: no GitHub API)'
begin 'NativeMobileDisplay' Droid-VM/NativeMobileDisplay "$(table_pin "$REFS" NativeMobileDisplay)" && {
  their Droid-VM/NativeMobileDisplay "$(sub drivers/GopFramebuffer)" "Droid-VM's pin"; against Droid-VM/NativeMobileDisplay wip/hackintosh; against Droid-VM/NativeMobileDisplay feat/static-refresh; end; }
begin 'NativeAdreno' Droid-VM/NativeAdreno "$(table_pin "$REFS" NativeAdreno)" && {
  their Droid-VM/NativeAdreno "$(sub drivers/Adreno740)" "Droid-VM's pin"; against Droid-VM/NativeAdreno wip/hackintosh; end; }
begin 'AdrenoPM4' UniXNU/AdrenoPM4 "$(table_pin "$REFS" AdrenoPM4)" && {
  their UniXNU/AdrenoPM4 "$(sub kits/AdrenoPM4)" "Droid-VM's pin"; against UniXNU/AdrenoPM4 main; end; }
echo
echo "== The guest kernel (kernel/pins.sh)"
begin 'Droid-VM-UniXNU (kernel base)' Droid-VM/UniXNU "$(table_pin "$KPINS" Droid-VM-UniXNU)" && {
  against Droid-VM/UniXNU wip/droidvm; against Droid-VM/UniXNU wip/hackintosh; end; }
begin 'UniXNU-droidvm (board)' Droid-VM/UniXNU-droidvm "$(table_pin "$KPINS" UniXNU-droidvm)" && {
  against Droid-VM/UniXNU-droidvm wip/hackintosh; end; }
begin 'UniXNU (upstream reference)' UniXNU/UniXNU "$(table_pin "$KPINS" UniXNU)" && { against UniXNU/UniXNU main; end; }
echo
echo "== The host side on the phone"
begin 'crosvm (source reference)' Droid-VM/crosvm "$(table_pin "$REFS" crosvm)" && {
  against Droid-VM/crosvm wip/hackintosh; against Droid-VM/crosvm droidvm; against Droid-VM/crosvm feature/dynamic-memory; end; }
begin 'crosvm prebuilt (hackintosh line)' Droid-VM/DroidVM-Prebuilt-Root "$(var_pin "$TREE/scripts/droidvm/fetch-crosvm.sh" 'hackintosh) COMMIT=')" && {
  against Droid-VM/DroidVM-Prebuilt-Root wip/hackintosh; against Droid-VM/DroidVM-Prebuilt-Root droidvm; end; }
begin 'virglrenderer (source reference)' Droid-VM/virglrenderer "$(table_pin "$REFS" virglrenderer)" && {
  against Droid-VM/virglrenderer wip/hackintosh; against Droid-VM/virglrenderer droidvm; end; }
begin 'virglrenderer (built: drm2kgsl)' Droid-VM/virglrenderer "$(var_pin "$TREE/scripts/droidvm/build-virgl.sh" '^COMMIT=')" && {
  against Droid-VM/virglrenderer wip/hackintosh; against Droid-VM/virglrenderer droidvm; end; }
echo
echo "== The phone's kernel modules"
begin 'gh-hugepage-reserve (the pool)' Droid-VM/gh-hugepage-reserve "$(var_pin "$TREE/scripts/hugepage/build.sh" '^COMMIT=')" && {
  b=$(api repos/Droid-VM/gh-hugepage-reserve --jq .default_branch); against Droid-VM/gh-hugepage-reserve "${b:-main}"; end; }
begin 'gunyah_host_mod (host share)' Droid-VM/gunyah_host_mod "$(var_pin "$TREE/scripts/hugepage/build-share.sh" '^COMMIT=')" && {
  against Droid-VM/gunyah_host_mod droidvm; against Droid-VM/gunyah_host_mod feature/dynamic-memory; end; }
begin 'gunyah_host_mod (udmabuf)' Droid-VM/gunyah_host_mod "$(var_pin "$TREE/scripts/hugepage/build-udmabuf.sh" '^COMMIT=')" && {
  against Droid-VM/gunyah_host_mod droidvm; end; }
hp=$TREE/tools/oneios-tool/crates/oneios-phone/src/haltpoll.rs
if [ -f "$hp" ]; then
  src=$(sed -n 's/.*gunyah_host_mod \([0-9a-f]\{7,40\}\).*/\1/p' "$hp" | head -1)
  full=$(api "repos/Droid-VM/gunyah_host_mod/commits/$src" --jq .sha)
  begin 'haltpoll (prebuilt .ko)' Droid-VM/gunyah_host_mod "$full" && { against Droid-VM/gunyah_host_mod droidvm; end; }
else
  printf '%-34s no pin in this tree\n' 'haltpoll (prebuilt .ko)'
fi
