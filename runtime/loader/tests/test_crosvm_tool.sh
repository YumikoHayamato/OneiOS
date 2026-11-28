#!/bin/sh
# SPDX-License-Identifier: MIT
# xnuloader-crosvm and xnuloader-qemu themselves, on the generated kernel (tests/fixture_kernel.c): --jetsam-product,
# the bench's --audio, the video decoder's --media (xnuloader-crosvm and the bench's xnuloader-qemu), and xnuloader-crosvm's
# identity options (--identity-tree, --identity-groups, --memory-class, --soc-generation, --display-corner-radius) on the generated
# identity tree.
#   tests/test_crosvm_tool.sh BUILD      (BUILD has xnuloader-crosvm, xnuloader-qemu, xnuloader-inspect and fixture-kernel)
# Without the option the tree has no jetsam-properties-product-type; with it /product has the name and the tree is
# otherwise the same (the random seed aside: the tool reads /dev/urandom); no target-type either way; a name that is no
# part of a file name is refused with status 2 and no Image written.
set -u
B=$1
T=$(mktemp -d "$B/crosvm-tool.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
checks=0 failed=0
check() {   # check WHAT COMMAND...
  what=$1; shift
  checks=$((checks + 1))
  if ! "$@"; then failed=$((failed + 1)); echo "test_crosvm_tool: FAILED: $what" >&2; fi
}
tree() { "$B/xnuloader-inspect" adt "$1" | grep -v 'random-seed = '; }

check 'the generated kernel and identity tree are written' "$B/fixture-kernel" "$T/kernel.macho" "$T/identity.adt"
check 'an Image without the option' sh -c '"$1/xnuloader-crosvm" "$2/kernel.macho" "$2/plain.img" --ram-mib 512 > "$2/plain.txt" 2>&1' sh "$B" "$T"
check 'an Image with --jetsam-product V54' sh -c '"$1/xnuloader-crosvm" "$2/kernel.macho" "$2/named.img" --ram-mib 512 --jetsam-product V54 > "$2/named.txt" 2>&1' sh "$B" "$T"
tree "$T/plain.img.adt" > "$T/plain.tree"
tree "$T/named.img.adt" > "$T/named.tree"
check 'no name in the tree without the option' sh -c '! grep -q jetsam-properties-product-type "$1"' sh "$T/plain.tree"
check 'no target-type without the option' sh -c '! grep -q target-type "$1"' sh "$T/plain.tree"
check 'no target-type with the option' sh -c '! grep -q target-type "$1"' sh "$T/named.tree"
check 'the name is in the tree with the option' grep -q '^    jetsam-properties-product-type = "V54"$' "$T/named.tree"
# the property is /product's: the lines from "  product {" to its closing brace hold it
check 'the name is a property of /product' sh -c 'sed -n "/^  product {\$/,/^  }\$/p" "$1" | grep -q "jetsam-properties-product-type = \"V54\""' sh "$T/named.tree"
grep -v 'jetsam-properties-product-type' "$T/named.tree" > "$T/named.rest"
check 'the rest of the tree is the same' cmp -s "$T/plain.tree" "$T/named.rest"
check 'the layout names the list' grep -q 'jetsam properties: /product jetsam-properties-product-type "V54": launchd reads /System/Library/LaunchDaemons/com.apple.jetsamproperties.V54.plist' "$T/named.txt"
check 'the layout says when none is named' grep -q 'jetsam properties: none named (no --jetsam-product)' "$T/plain.txt"
# the tree grows by the property alone: a 32-byte name, a 4-byte length, "V54" and its NUL
plain=$(wc -c < "$T/plain.img.adt"); named=$(wc -c < "$T/named.img.adt")
check 'the tree is 40 bytes longer' [ "$named" -eq $((plain + 40)) ]
for bad in '' '.V54' 'V54/x' 'V 54' '01234567890123456789012345678901'; do
  rm -f "$T/bad.img" "$T/bad.img.adt"
  "$B/xnuloader-crosvm" "$T/kernel.macho" "$T/bad.img" --ram-mib 512 --jetsam-product "$bad" > "$T/bad.txt" 2>&1
  status=$?
  check "the name '$bad' is refused with status 2" [ "$status" -eq 2 ]
  check "the name '$bad' is refused with the reason" grep -q -- '--jetsam-product: 1 to 31 of' "$T/bad.txt"
  check "the name '$bad' leaves no Image" sh -c '[ ! -e "$1/bad.img" ] && [ ! -e "$1/bad.img.adt" ]' sh "$T"
done
"$B/xnuloader-crosvm" "$T/kernel.macho" "$T/bad.img" --ram-mib 512 --jetsam-product > "$T/bad.txt" 2>&1
check 'the option without a name is the usage (status 2)' [ $? -eq 2 ]

# the bench's loader: the same property in board.adt, none without the option, a bad name refused before any file
mkdir "$T/q" "$T/qn" "$T/qb"
check 'the bench without the option' sh -c '"$1/xnuloader-qemu" "$2/kernel.macho" "$2/q" > "$2/q.txt" 2>&1' sh "$B" "$T"
check 'the bench with --jetsam-product V54' sh -c '"$1/xnuloader-qemu" "$2/kernel.macho" "$2/qn" --jetsam-product V54 > "$2/qn.txt" 2>&1' sh "$B" "$T"
tree "$T/q/board.adt" > "$T/q.tree"
tree "$T/qn/board.adt" > "$T/qn.tree"
check 'the bench: no name without the option' sh -c '! grep -q jetsam-properties-product-type "$1"' sh "$T/q.tree"
check 'the bench: the name is a property of /product' sh -c 'sed -n "/^  product {\$/,/^  }\$/p" "$1" | grep -q "jetsam-properties-product-type = \"V54\""' sh "$T/qn.tree"
grep -v 'jetsam-properties-product-type' "$T/qn.tree" > "$T/qn.rest"
check 'the bench: the rest of the tree is the same' cmp -s "$T/q.tree" "$T/qn.rest"
check 'the bench: the layout names the list' grep -q 'jetsam properties: /product jetsam-properties-product-type "V54"' "$T/qn.txt"
"$B/xnuloader-qemu" "$T/kernel.macho" "$T/qb" --jetsam-product 'V54/x' > "$T/qb.txt" 2>&1
check 'the bench: a bad name is refused with status 2' [ $? -eq 2 ]
check 'the bench: a bad name is refused with the reason' grep -q -- '--jetsam-product: 1 to 31 of' "$T/qb.txt"
check 'the bench: a bad name leaves no file' sh -c '[ -z "$(ls -A "$1/qb")" ]' sh "$T"

# the bench's sound card (take17, xnuloader-qemu --audio): /arm-io/virtio-snd on the first free virtio-mmio transport and
# QEMU's virtio-sound-device there with `-audiodev none`; without the option neither the node nor the device
mkdir "$T/qa"
check 'the bench with --audio' sh -c '"$1/xnuloader-qemu" "$2/kernel.macho" "$2/qa" --audio > "$2/qa.txt" 2>&1' sh "$B" "$T"
tree "$T/qa/board.adt" > "$T/qa.tree"
check 'the bench: the sound card is a node of /arm-io' sh -c 'sed -n "/^    virtio-snd {\$/,/^    }\$/p" "$1" | grep -q "compatible = \"droidvm,virtio-snd\""' sh "$T/qa.tree"
check 'the bench: no sound card without the option' sh -c '! grep -q virtio-snd "$1"' sh "$T/q.tree"
check 'the bench: QEMU gets the card on transport 0 and an audio backend that plays nothing' sh -c 'paste -s -d" " "$1" | grep -q -- "-audiodev none,id=snd0 -device virtio-sound-device,bus=virtio-mmio-bus.0,audiodev=snd0\$"' sh "$T/qa/qemu.args"
check 'the bench: the transports are modern for the card alone too' grep -q '^virtio-mmio.force-legacy=false$' "$T/qa/qemu.args"
check 'the bench: no audio device without the option' sh -c '! grep -q -e audiodev -e virtio-sound "$1"' sh "$T/q/qemu.args"
# take18, --audio-intid N,SLOT (with --audio, --shared-ram, --image): refused without --audio or --image, or with a bad form;
# the option's numbers need an image set, so only the refusals are checked here (the tree's two properties: test_loader.c)
check 'the sound card interrupt without --audio is refused' sh -c '! "$1/xnuloader-crosvm" "$2/kernel.macho" "$2/ai.img" --ram-mib 512 --shared-ram --audio-intid 44,8 > "$2/ai.txt" 2>&1 && grep -q "needs --audio and --image" "$2/ai.txt" && [ ! -e "$2/ai.img" ]' sh "$B" "$T"
check 'the sound card interrupt without a slot is refused' sh -c '! "$1/xnuloader-crosvm" "$2/kernel.macho" "$2/ai2.img" --ram-mib 512 --shared-ram --audio --audio-intid 44 > "$2/ai2.txt" 2>&1 && grep -q "N,SLOT" "$2/ai2.txt" && [ ! -e "$2/ai2.img" ]' sh "$B" "$T"

# the video decoder (take18b, xnuloader-crosvm --media): /arm-io/virtio-media-0 is bus 0 of crosvm's ECAM with crosvm's
# media_host pool, the RAM's last --media-mib MiB, the GPU's arena right below it (crosvm's pool order); the boot arguments
# start the codec archive; without the option the tree, the arena and the arguments are as before
check 'an Image with the GPU alone' sh -c '"$1/xnuloader-crosvm" "$2/kernel.macho" "$2/g.img" --ram-mib 1024 --shared-ram --gpu > "$2/g.txt" 2>&1' sh "$B" "$T"
check 'an Image with the GPU and --media' sh -c '"$1/xnuloader-crosvm" "$2/kernel.macho" "$2/gm.img" --ram-mib 1024 --shared-ram --gpu --media > "$2/gm.txt" 2>&1' sh "$B" "$T"
check 'an Image with --media --media-mib 64' sh -c '"$1/xnuloader-crosvm" "$2/kernel.macho" "$2/m64.img" --ram-mib 1024 --shared-ram --media --media-mib 64 > "$2/m64.txt" 2>&1' sh "$B" "$T"
tree "$T/g.img.adt" > "$T/g.tree"
tree "$T/gm.img.adt" > "$T/gm.tree"
tree "$T/m64.img.adt" > "$T/m64.tree"
check 'the decoder: no node without the option' sh -c '! grep -q virtio-media "$1"' sh "$T/g.tree"
check 'the decoder: the arena is the RAM last 8 MiB without it' grep -q '^      drm-host-pool = <0xbf800000 0x800000>$' "$T/g.tree"
check 'the decoder: no codec arguments without it' sh -c '! grep -q -e oneios-codec= -e droidvm-vt-paravirt= "$1"' sh "$T/g.txt"
check 'the decoder: the node is bus 0 of the ECAM' sh -c 'sed -n "/^    virtio-media-0 {\$/,/^    }\$/p" "$1" | grep -q "compatible = \"oneios,virtio-media-pci\""' sh "$T/gm.tree"
check 'the decoder: its reg is all of bus 0' sh -c 'sed -n "/^    virtio-media-0 {\$/,/^    }\$/p" "$1" | grep -q "reg = <0xf000 0x100000>"' sh "$T/gm.tree"
check 'the decoder: the pool is the RAM last 192 MiB' sh -c 'sed -n "/^    virtio-media-0 {\$/,/^    }\$/p" "$1" | grep -q "media-host-pool = <0xb4000000 0xc000000>"' sh "$T/gm.tree"
check 'the decoder: no board line (polled)' sh -c '! sed -n "/^    virtio-media-0 {\$/,/^    }\$/p" "$1" | grep -q "droidvm,intid"' sh "$T/gm.tree"
check 'the decoder: the arena moves below the pool' grep -q '^      drm-host-pool = <0xb3800000 0x800000>$' "$T/gm.tree"
check 'the decoder: the codec arguments' grep -q 'oneios-codec=1 droidvm-vt-paravirt=1$' "$T/gm.txt"
check 'the decoder: --media-mib sizes the pool' sh -c 'sed -n "/^    virtio-media-0 {\$/,/^    }\$/p" "$1" | grep -q "media-host-pool = <0xbc000000 0x4000000>"' sh "$T/m64.tree"
check 'the decoder: the layout names the pool' grep -q "video decoder: crosvm's media_host pool 64 MiB at PA 0xbc000000 (the RAM's last 64 MiB)" "$T/m64.txt"
# the bench: the same node on virt's ECAM bus 0 (no device behind it) and the same arguments; none without the option
mkdir "$T/qm"
check 'the bench with --media' sh -c '"$1/xnuloader-qemu" "$2/kernel.macho" "$2/qm" --media > "$2/qm.txt" 2>&1' sh "$B" "$T"
tree "$T/qm/board.adt" > "$T/qm.tree"
check 'the bench: the decoder node is bus 0 of virt ECAM' sh -c 'sed -n "/^    virtio-media-0 {\$/,/^    }\$/p" "$1" | grep -q "compatible = \"oneios,virtio-media-pci\""' sh "$T/qm.tree"
check 'the bench: the decoder node has no pool' sh -c '! grep -q media-host-pool "$1"' sh "$T/qm.tree"
check 'the bench: the codec arguments' grep -q 'oneios-codec=1 droidvm-vt-paravirt=1$' "$T/qm.txt"
check 'the bench: no decoder node without the option' sh -c '! grep -q virtio-media "$1"' sh "$T/q.tree"
for bad in '--media' '--media --media-mib 15' '--media --media-mib 33' '--media --media-mib 2048'; do
  rm -f "$T/bm.img" "$T/bm.img.adt"
  shared=--shared-ram
  [ "$bad" = '--media' ] && shared=
  # shellcheck disable=SC2086
  "$B/xnuloader-crosvm" "$T/kernel.macho" "$T/bm.img" --ram-mib 1024 $shared $bad > "$T/bm.txt" 2>&1
  check "the decoder: '$shared $bad' is refused with status 2" [ $? -eq 2 ]
  check "the decoder: '$shared $bad' leaves no Image" sh -c '[ ! -e "$1/bm.img" ]' sh "$T"
done

# the identity options (xnuloader-crosvm --identity-tree with --identity-groups, --memory-class, --soc-generation,
# --display-corner-radius), on the generated identity tree (tests/fixture.h fx_identity: the properties of the six groups, the
# phone's body, the exclaves, a placeholder, three children). None is on by default: the tree with no option is the tree of before
# them, --identity-tree alone copies nothing, and each group writes exactly its own properties. Bad input is refused with status 2
# and no Image (a missing file: status 1, as for the other files).
split() {   # split TREE: TREE.product has /product's lines of the dump, TREE.rest the dump without them
  sed -n '/^  product {$/,/^  }$/p' "$1" > "$1.product"
  sed '/^  product {$/,/^  }$/d' "$1" > "$1.rest"
}
mk() {      # mk NAME ARGS...: an Image of 512 MiB (a later --ram-mib wins) with the options; NAME.txt is its layout, NAME.tree its tree
  n=$1; shift
  "$B/xnuloader-crosvm" "$T/kernel.macho" "$T/$n.img" --ram-mib 512 "$@" > "$T/$n.txt" 2>&1 || return 1
  tree "$T/$n.img.adt" > "$T/$n.tree"
  split "$T/$n.tree"
}
refuse() {  # refuse NAME REASON ARGS...: status 2, the reason in the message, no Image
  n=$1 why=$2; shift 2
  rm -f "${T:?}/refused.img" "${T:?}/refused.img.adt"
  "$B/xnuloader-crosvm" "$T/kernel.macho" "$T/refused.img" --ram-mib 512 "$@" > "$T/refused.txt" 2>&1
  status=$?
  check "refused with status 2: $n" [ "$status" -eq 2 ]
  check "refused with the reason: $n" grep -qF -- "$why" "$T/refused.txt"
  check "refused without an Image: $n" sh -c '[ ! -e "$1/refused.img" ] && [ ! -e "$1/refused.img.adt" ]' sh "$T"
}
added() {   # added BASE TREE: the names of the properties that TREE's /product lines have and BASE's lack, sorted, on one line
  diff "$1" "$2" | sed -n 's/^> *\([^ ]*\) = .*/\1/p' | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'
}
kept() {    # kept BASE TREE: no line of BASE's /product is changed or gone in TREE's
  ! diff "$1" "$2" | grep -q '^<'
}
sorted() {  # sorted NAMES...: the names, sorted, on one line
  printf '%s\n' "$@" | tr ' ' '\n' | grep . | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//'
}
root_alone() {   # root_alone A B: the dumps of the roots (A.rest, B.rest) are the same but for target-type and compatible
  grep -v -e '^  target-type = ' -e '^  compatible = ' "$1" > "$1.noroot"
  grep -v -e '^  target-type = ' -e '^  compatible = ' "$2" > "$2.noroot"
  cmp -s "$1.noroot" "$2.noroot"
}
on_lines() {     # on_lines LAYOUT: how many groups of the layout are on
  grep -c 'identity group .*: on: ' "$1"
}
off_lines() {    # off_lines LAYOUT: how many groups of the layout are off
  grep -c 'identity group .*: off$' "$1"
}
split "$T/plain.tree"

# what each group writes into /product of the generated tree (the gamut is the machine's, written with the artwork group)
G_artwork="artwork-device-idiom artwork-device-subtype artwork-dynamic-displaymode graphics-featureset-class graphics-featureset-fallbacks artwork-display-gamut"
G_quality="ui-background-quality ui-weather-quality"
G_workgroup="display-workgroup-interval hid-workgroup-interval"
G_ids="udid-version product-description public-key-accelerator"
G_memclass="device-perf-memory-class"
G_root=""

# soc-generation: none by default; a name is written as it is (40 bytes of the tree: a name, a length, "H18" and its NUL)
check 'an Image with --soc-generation H18' mk soc18 --soc-generation H18
check 'an Image with --soc-generation none' mk nosoc --soc-generation none
check 'an Image with --soc-generation H17' mk soc17 --soc-generation H17
check 'soc-generation: none by default' sh -c '! grep -q soc-generation "$1"' sh "$T/plain.tree"
check 'soc-generation: H18 is a property of /arm-io' sh -c 'sed -n "/^  arm-io {\$/,/^  }\$/p" "$1" | grep -q "^    soc-generation = \"H18\"\$"' sh "$T/soc18.tree"
grep -v 'soc-generation' "$T/soc18.tree" > "$T/soc18.nosoc"
check 'soc-generation: the tree of H18 less that line is the default tree' cmp -s "$T/soc18.nosoc" "$T/plain.tree"
check 'soc-generation: the tree of H18 is 40 bytes longer than the default' \
  [ "$(wc -c < "$T/soc18.img.adt")" -eq $(( $(wc -c < "$T/plain.img.adt") + 40 )) ]
check 'soc-generation: none is the default tree' cmp -s "$T/nosoc.tree" "$T/plain.tree"
check 'soc-generation: none is as long as the default tree' [ "$(wc -c < "$T/nosoc.img.adt")" -eq "$(wc -c < "$T/plain.img.adt")" ]
check 'soc-generation: a name is written as it is' grep -q '^    soc-generation = "H17"$' "$T/soc17.tree"
check 'soc-generation: /arm-io has no compatible (Apple'"'"'s kexts would match "arm-io,t8150")' sh -c '! sed -n "/^  arm-io {\$/,/^  }\$/p" "$1" | grep -q "^    compatible"' sh "$T/soc18.tree"
check 'soc-generation: the layout names the property' grep -q 'soc-generation: /arm-io soc-generation "H18" (--soc-generation)' "$T/soc18.txt"
check 'soc-generation: the layout says none' grep -q 'soc-generation: none written (no --soc-generation)' "$T/plain.txt"

# without the options: no group is on, the layout says so; /product is its name, product-name and model, the root the machine's
check 'no identity options: the layout has no tree, six groups off, no corner radius' sh -c \
  'grep -q "identity tree: none (no --identity-tree)" "$1" && grep -q "display corner radius: none written (no --display-corner-radius)" "$1"' sh "$T/plain.txt"
check 'no identity options: six groups off, none on' sh -c '[ "$(grep -c "identity group .*: off\$" "$1")" -eq 6 ] && ! grep -q "identity group .*: on" "$1"' sh "$T/plain.txt"
check 'no identity options: /product is its name, product-name and model' sh -c '[ "$(grep -c "^    " "$1")" -eq 2 ]' sh "$T/plain.tree.product"
check 'no identity options: no target-type, the root'"'"'s compatible is its own' sh -c '! grep -q target-type "$1" && grep -q "^  compatible = \"oneios,xnuhost\"\$" "$1"' sh "$T/plain.tree.rest"
check 'no identity options: no gamut, no corner radius' sh -c '! grep -q -e artwork-display-gamut -e display-corner-radius "$1"' sh "$T/plain.tree.product"

# --identity-tree alone copies nothing: the tree is the plain Image's, and the layout says so
check 'a plain Image of 3072 MiB' mk plain3 --ram-mib 3072
check 'an Image with --identity-tree alone' mk idalone --ram-mib 3072 --identity-tree "$T/identity.adt"
split "$T/plain3.tree"
check 'identity tree alone: the tree is the plain one'"'"'s, /product and all' cmp -s "$T/plain3.tree" "$T/idalone.tree"
check 'identity tree alone: the Image'"'"'s tree is as long as the plain one'"'"'s' [ "$(wc -c < "$T/idalone.img.adt")" -eq "$(wc -c < "$T/plain3.img.adt")" ]
check 'identity tree alone: the layout says nothing is copied, six groups off' sh -c \
  'grep -q "identity tree: .*identity.adt, read; no --identity-groups: nothing is copied from it" "$1" && [ "$(grep -c "identity group .*: off\$" "$1")" -eq 6 ] && ! grep -q "identity group .*: on" "$1"' sh "$T/idalone.txt"

# each group alone, by its name and by its letter: exactly its own properties, nothing else of /product, the rest of the tree as it was
for g in artwork:a quality:b workgroup:c ids:d memclass:e root:f; do
  gname=${g%:*} gletter=${g#*:}
  eval "gwant=\$(sorted \$G_$gname)"
  check "an Image with --identity-groups $gname" mk "g_$gname" --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups "$gname"
  check "an Image with --identity-groups $gletter" mk "g_$gletter" --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups "$gletter"
  check "group $gname: the letter $gletter is the same group" cmp -s "$T/g_$gname.tree" "$T/g_$gletter.tree"
  check "group $gname: /product gains exactly its properties" [ "$(added "$T/plain3.tree.product" "$T/g_$gname.tree.product")" = "$gwant" ]
  check "group $gname: no line of /product is changed or gone" kept "$T/plain3.tree.product" "$T/g_$gname.tree.product"
  if [ "$gname" = root ]; then
    check "group $gname: the rest of the tree is the plain one's but for target-type and compatible" root_alone "$T/plain3.tree.rest" "$T/g_$gname.tree.rest"
    check "group $gname: the rest of the tree is not the plain one's" sh -c '! cmp -s "$1" "$2"' sh "$T/plain3.tree.rest" "$T/g_$gname.tree.rest"
  else
    check "group $gname: the rest of the tree is the plain one's" cmp -s "$T/plain3.tree.rest" "$T/g_$gname.tree.rest"
  fi
  check "group $gname: the layout has it on and the others off" sh -c \
    'grep -q "identity group $2 ($3): on: " "$1" && [ "$(grep -c "identity group .*: on: " "$1")" -eq 1 ] && [ "$(grep -c "identity group .*: off\$" "$1")" -eq 5 ]' sh "$T/g_$gname.txt" "$gname" "$gletter"
done

# the values: the tree's own bytes, the featureset list less APPLE2 and MTL2,2, the gamut the machine's (the tree's is SRGB), the class the VM's
check 'artwork: the values' sh -c 'grep -q "^    artwork-device-idiom = \"phone\"\$" "$1" && grep -q "^    artwork-device-subtype = 0xb34\$" "$1" && grep -q "^    artwork-dynamic-displaymode = \"0\"\$" "$1" && grep -q "^    graphics-featureset-class = \"APPLE10\"\$" "$1"' sh "$T/g_artwork.tree.product"
check 'artwork: the featureset list is less APPLE2 and MTL2,2' grep -q '^    graphics-featureset-fallbacks = "APPLE9:APPLE3v1:APPLE1:GLES2,0"$' "$T/g_artwork.tree.product"
check 'artwork: the gamut is the machine'"'"'s, not the tree'"'"'s SRGB; no scale without --display' sh -c 'grep -q "^    artwork-display-gamut = \"P3\"\$" "$1" && ! grep -q -e SRGB -e artwork-scale-factor "$1"' sh "$T/g_artwork.tree.product"
check 'quality: the values (0x64 is the letter d and two NULs for the dump)' sh -c '[ "$(grep -c "^    ui-.*-quality = \"d||\"\$" "$1")" -eq 2 ]' sh "$T/g_quality.tree.product"
check 'workgroup: the values' sh -c 'grep -q "^    display-workgroup-interval = 0x1\$" "$1" && grep -q "^    hid-workgroup-interval = 0x1\$" "$1"' sh "$T/g_workgroup.tree.product"
check 'ids: the values' sh -c 'grep -q "^    udid-version = 0x2\$" "$1" && grep -q "^    public-key-accelerator = 0x1\$" "$1" && grep -q "^    product-description = \"Fixture Phone 17\"\$" "$1"' sh "$T/g_ids.tree.product"
check 'memclass: the class is the VM'"'"'s 3072 MiB (CoreUI'"'"'s 3 GB class is 4), not the 12 GB device'"'"'s' grep -q '^    device-perf-memory-class = 0x4$' "$T/g_memclass.tree.product"
check 'root: target-type and the board name first in compatible, then the machine'"'"'s own; the model stays' sh -c \
  'grep -q "^  target-type = \"F1\"\$" "$1" && grep -q "^  compatible = \"F1AP|oneios,xnuhost\"\$" "$1" && grep -q "^  model = \"iPhone18,2\"\$" "$1"' sh "$T/g_root.tree.rest"
check 'root: /product keeps its product-name and model' sh -c 'grep -q "^    product-name = \"OneiOS guest\"\$" "$1" && grep -q "^    model = \"oneios,xnuhost\"\$" "$1"' sh "$T/g_root.tree.product"
check 'the layout names the properties of the artwork group' grep -q 'identity group artwork (a): on: 5 from the tree (artwork-device-idiom artwork-device-subtype artwork-dynamic-displaymode graphics-featureset-class graphics-featureset-fallbacks), and artwork-display-gamut "P3" (the machine'"'"'s)$' "$T/g_artwork.txt"
check 'the layout names the properties of the quality group' grep -q 'identity group quality (b): on: 2 from the tree (ui-background-quality ui-weather-quality)$' "$T/g_quality.txt"
check 'the layout names the properties of the workgroup group' grep -q 'identity group workgroup (c): on: 2 from the tree (display-workgroup-interval hid-workgroup-interval)$' "$T/g_workgroup.txt"
check 'the layout names the properties of the ids group' grep -q 'identity group ids (d): on: 3 from the tree (udid-version public-key-accelerator product-description)$' "$T/g_ids.txt"
check 'the layout names the class of the memclass group' grep -q "identity group memclass (e): on: device-perf-memory-class 4 in 4 bytes (the VM's 3072 MiB: CoreUI's 3072 MiB class, the largest its RAM holds; Apple's own is 12)$" "$T/g_memclass.txt"
check 'the layout names the root of the root group' grep -q 'identity group root (f): on: root target-type "F1", compatible "F1AP" then "oneios,xnuhost" (' "$T/g_root.txt"

# all, and the union of groups: the properties of each, the tree of the roots'
check 'an Image with --identity-groups all' mk g_all --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups all
check 'an Image with --identity-groups a,b,c,d,e,f' mk g_abcdef --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups a,b,c,d,e,f
check 'an Image with --identity-groups root,memclass,ids,workgroup,quality,artwork' mk g_rev --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups root,memclass,ids,workgroup,quality,artwork
check 'an Image with --identity-groups all,a,root' mk g_allrep --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups all,a,root
check 'all is a, b, c, d, e, f, in any order, with repeats' sh -c 'cmp -s "$1/g_all.tree" "$1/g_abcdef.tree" && cmp -s "$1/g_all.tree" "$1/g_rev.tree" && cmp -s "$1/g_all.tree" "$1/g_allrep.tree"' sh "$T"
check 'all: /product gains the properties of the five groups' [ "$(added "$T/plain3.tree.product" "$T/g_all.tree.product")" = "$(sorted $G_artwork $G_quality $G_workgroup $G_ids $G_memclass)" ]
check 'all: no line of /product is changed or gone' kept "$T/plain3.tree.product" "$T/g_all.tree.product"
check 'all: the rest of the tree is the plain one'"'"'s but for the root'"'"'s target-type and compatible' root_alone "$T/plain3.tree.rest" "$T/g_all.tree.rest"
check 'with every group: the phone'"'"'s body, the exclaves, the placeholder, the jetsam name and the children are never copied' sh -c \
  '! grep -q -e island-notch -e supports-camera-button -e supports-burnin -e has-virtualization -e display-corner-radius -e exclaves -e oled-display -e device-colors -e AAPL -e jetsam -e "iopm {" -e "camera {" -e "audio {" "$1"' sh "$T/g_all.tree.product"
check 'all: six groups on' sh -c '[ "$(grep -c "identity group .*: on: " "$1")" -eq 6 ] && ! grep -q "identity group .*: off" "$1"' sh "$T/g_all.txt"
check 'an Image with --identity-groups ids,quality' mk g_pair --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups ids,quality
check 'two groups: the properties of both and no other' [ "$(added "$T/plain3.tree.product" "$T/g_pair.tree.product")" = "$(sorted $G_quality $G_ids)" ]
check 'two groups: the rest of the tree is the plain one'"'"'s' cmp -s "$T/plain3.tree.rest" "$T/g_pair.tree.rest"
check 'an Image with a later --identity-groups: it replaces the first' mk g_twice --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups all --identity-groups ids
check 'a later --identity-groups replaces the earlier one' cmp -s "$T/g_twice.tree" "$T/g_ids.tree"

# --memory-class N (the memclass group only): N itself, 1 to the tree's own (12); the VM's class is at least 2
check 'an Image of 512 MiB with the memclass group' mk mc2 --identity-tree "$T/identity.adt" --identity-groups memclass
check 'memory class: 512 MiB are CoreUI'"'"'s class 0' grep -q '^    device-perf-memory-class = "|||"$' "$T/mc2.tree.product"   # four zero bytes, as xnuloader-inspect prints them
check 'an Image of 4608 MiB with the memclass group' mk mc4 --ram-mib 4608 --identity-tree "$T/identity.adt" --identity-groups memclass
check 'memory class: 4608 MiB are CoreUI'"'"'s 4 GB class, 3' grep -q '^    device-perf-memory-class = 0x3$' "$T/mc4.tree.product"
# take23: CoreUI's table (machine.h xl_machine_memory_class_of): the sizes of the new boot order's Images
for pair in 4352:3 5120:3 5632:3 6144:6 6656:6 7168:6 8192:8 2048:2 1536:f 1024:1 3072:4; do
  check "an Image of ${pair%%:*} MiB with the memclass group" mk "mcr${pair%%:*}" --ram-mib "${pair%%:*}" --identity-tree "$T/identity.adt" --identity-groups memclass
  check "memory class: ${pair%%:*} MiB give 0x${pair##*:}" grep -q "^    device-perf-memory-class = 0x${pair##*:}\$" "$T/mcr${pair%%:*}.tree.product"
done
# take23: --identity-omit and --display-gamut (the artwork group as the owner chose it: no featureset keys, the gamut sRGB)
check 'an Image with the artwork group, the featureset keys left out, the gamut sRGB' mk g_art_omit --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups artwork,memclass --identity-omit graphics-featureset-class,graphics-featureset-fallbacks --display-gamut sRGB
check 'omit: no graphics-featureset key in /product' sh -c '! grep -q "graphics-featureset" "$1"' sh "$T/g_art_omit.tree.product"
check 'omit: the other artwork keys are there' sh -c 'grep -q "artwork-device-idiom" "$1" && grep -q "artwork-device-subtype" "$1" && grep -q "artwork-dynamic-displaymode" "$1"' sh "$T/g_art_omit.tree.product"
check 'gamut: sRGB is written' grep -q 'artwork-display-gamut = "sRGB"' "$T/g_art_omit.tree.product"
check 'omit: the layout says what was left out' grep -q 'left out by --identity-omit where the group has them: graphics-featureset-class,graphics-featureset-fallbacks' "$T/g_art_omit.txt"
refuse '--identity-omit without groups' '--identity-omit needs --identity-groups' --identity-tree "$T/identity.adt" --identity-omit graphics-featureset-class
refuse '--identity-omit of a name no group that is on takes' 'is not a property of an identity group that is on' --identity-tree "$T/identity.adt" --identity-groups memclass --identity-omit graphics-featureset-class
refuse '--display-gamut of another word' '--display-gamut: sRGB or P3' --identity-tree "$T/identity.adt" --identity-groups artwork --display-gamut Rec2020
refuse '--display-gamut without the artwork group' '--display-gamut needs --identity-groups artwork' --identity-tree "$T/identity.adt" --identity-groups memclass --display-gamut sRGB
check 'an Image with --memory-class 8' mk mc8 --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups memclass --memory-class 8
check 'memory class: N is written' grep -q '^    device-perf-memory-class = 0x8$' "$T/mc8.tree.product"
check 'memory class: the layout says it is N' grep -q 'identity group memclass (e): on: device-perf-memory-class 8 in 4 bytes (--memory-class, 1 to 12)$' "$T/mc8.txt"
check 'memory class: N changes that one line of the tree and nothing else' sh -c '[ "$(diff "$1" "$2" | grep -c "^>")" -eq 1 ]' sh "$T/g_memclass.tree" "$T/mc8.tree"
check 'an Image with --memory-class 12 and the other groups' mk mc12 --identity-tree "$T/identity.adt" --identity-groups all --memory-class 12
check 'memory class: the tree'"'"'s own is allowed' grep -q '^    device-perf-memory-class = 0xc$' "$T/mc12.tree.product"
check 'an Image with --memory-class 1' mk mc1 --identity-tree "$T/identity.adt" --identity-groups e --memory-class 1
check 'memory class: 1 is allowed' grep -q '^    device-perf-memory-class = 0x1$' "$T/mc1.tree.product"

# --display-corner-radius PT[,WORD2]: /product display-corner-radius, two little-endian u32 (the dump shows the 8 bytes as one u64:
# the second word in its high half), nothing else changes; independent of the identity options
check 'an Image with --display-corner-radius 30' mk cr30 --display-corner-radius 30
check 'corner radius: /product gains display-corner-radius and nothing else' [ "$(added "$T/plain.tree.product" "$T/cr30.tree.product")" = display-corner-radius ]
check 'corner radius: no line of /product is changed or gone' kept "$T/plain.tree.product" "$T/cr30.tree.product"
check 'corner radius: the rest of the tree is the plain one'"'"'s' cmp -s "$T/plain.tree.rest" "$T/cr30.tree.rest"
check 'corner radius 30: u32 30 and u32 1 (the default second word)' grep -q '^    display-corner-radius = <0x10000001e>$' "$T/cr30.tree.product"
check 'corner radius: the layout names the two words' grep -q 'display corner radius: /product display-corner-radius u32 30, u32 1 (--display-corner-radius PT\[,WORD2\]; Apple.s iPhone18,2 has 62 and 1; the second word.s meaning is not verified)$' "$T/cr30.txt"
for c in '30,0:<0x1e>' '0:<0x100000000>' '0,2:<0x200000000>' '62,1:<0x10000003e>' '200,65535:<0xffff000000c8>' '1,258:<0x10200000001>' '100,7:<0x700000064>'; do
  v=${c%%:*} want=${c#*:}
  check "an Image with --display-corner-radius $v" mk "cr_$v" --display-corner-radius "$v"
  check "corner radius $v: the bytes" grep -q "^    display-corner-radius = $want\$" "$T/cr_$v.tree.product"
done
check 'an Image with every group and --display-corner-radius 30' mk cr_all --ram-mib 3072 --identity-tree "$T/identity.adt" --identity-groups all --display-corner-radius 30
check 'corner radius with the groups: the machine'"'"'s value (not the tree'"'"'s 62 and 1), once' sh -c '[ "$(grep -c "display-corner-radius" "$1")" -eq 1 ] && grep -q "^    display-corner-radius = <0x10000001e>\$" "$1"' sh "$T/cr_all.tree.product"
check 'every group without --display-corner-radius: none' sh -c '! grep -q display-corner-radius "$1"' sh "$T/g_all.tree.product"

# refused: nothing is written. Trees made here: a file that is no device tree, an empty one, a root node alone (no /product), a root
# with an empty /product (no target-type, no compatible), a root with a target-type and no compatible, and a /product whose
# device-perf-memory-class is 3 bytes
echo "not a device tree" > "$T/junk.adt"
: > "$T/empty.adt"
{ printf '\1\0\0\0\0\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\14\0\0\0device-tree\0'; } > "$T/noproduct.adt"
{ printf '\1\0\0\0\1\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\14\0\0\0device-tree\0'
  printf '\1\0\0\0\0\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\10\0\0\0product\0'; } > "$T/nomodel.adt"
{ printf '\2\0\0\0\1\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\14\0\0\0device-tree\0'
  printf 'target-type'; head -c 21 /dev/zero; printf '\3\0\0\0V54\0'
  printf '\1\0\0\0\0\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\10\0\0\0product\0'; } > "$T/nocompat.adt"
{ printf '\1\0\0\0\1\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\14\0\0\0device-tree\0'
  printf '\2\0\0\0\0\0\0\0'; printf 'name'; head -c 28 /dev/zero; printf '\10\0\0\0product\0'
  printf 'device-perf-memory-class'; head -c 8 /dev/zero; printf '\3\0\0\0\14\0\0\0'; } > "$T/badclass.adt"
check 'an Image from a tree with an empty /product' mk empty_product --identity-tree "$T/nomodel.adt"
check 'an empty /product with no group copies nothing' cmp -s "$T/empty_product.tree" "$T/plain.tree"
check 'an Image from a tree with an empty /product and every group but the root' mk empty_groups --identity-tree "$T/nomodel.adt" --identity-groups a,b,c,d,e
check 'an empty /product copies nothing: the layout says the groups have nothing, but for the gamut' sh -c \
  'grep -q "identity group artwork (a): on: nothing from the tree (it has none of them), and artwork-display-gamut \"P3\"" "$1" && grep -q "identity group ids (d): on: nothing from the tree" "$1" && grep -q "identity group memclass (e): on: nothing written" "$1"' sh "$T/empty_groups.txt"
check 'an Image from a tree with a bad class and no memclass group' mk badclass_off --identity-tree "$T/badclass.adt" --identity-groups quality
refuse 'a tree that is no device tree' 'not a device tree' --identity-tree "$T/junk.adt"
refuse 'an empty file' 'not a device tree' --identity-tree "$T/empty.adt"
refuse 'a tree without /product' 'no /product' --identity-tree "$T/noproduct.adt"
refuse 'a tree without /product and a group' 'no /product' --identity-tree "$T/noproduct.adt" --identity-groups all
refuse 'a root group and a tree with no target-type or compatible' '--identity-groups root: ' --identity-tree "$T/nomodel.adt" --identity-groups root
refuse 'every group and a tree with no target-type or compatible' '--identity-groups root: ' --identity-tree "$T/nomodel.adt" --identity-groups all
refuse 'a root group and a tree with no compatible' '--identity-groups root: ' --identity-tree "$T/nocompat.adt" --identity-groups f
refuse 'a memclass group and a class of 3 bytes' '--identity-groups memclass: ' --identity-tree "$T/badclass.adt" --identity-groups memclass
refuse 'a group the tree has not: ids,foo' 'unknown group "foo"' --identity-tree "$T/identity.adt" --identity-groups ids,foo
for bad in '' ',' 'artwork,' ',artwork' 'artwork,,ids' 'foo' 'g' 'A' 'Artwork' 'ALL' ' artwork' 'artwork ' 'art' 'artworks' 'none' 'ab' 'a;b' 'a b' 'allx' 'al' 'rootx'; do
  refuse "--identity-groups '$bad'" 'unknown group "' --identity-tree "$T/identity.adt" --identity-groups "$bad"
done
refuse '--identity-groups names the word' 'unknown group "frobnicate"' --identity-tree "$T/identity.adt" --identity-groups artwork,frobnicate,ids
refuse '--identity-groups without --identity-tree' '--identity-groups needs --identity-tree' --identity-groups artwork
refuse '--identity-groups all without --identity-tree' '--identity-groups needs --identity-tree' --identity-groups all
refuse '--memory-class alone' '--memory-class needs --identity-tree and --identity-groups memclass' --memory-class 4
refuse '--memory-class with the tree and no group' '--memory-class needs --identity-tree and --identity-groups memclass' --identity-tree "$T/identity.adt" --memory-class 4
for g in artwork quality workgroup ids root a,b,c,d,f; do
  refuse "--memory-class with the group $g" '--memory-class needs --identity-tree and --identity-groups memclass' --identity-tree "$T/identity.adt" --identity-groups "$g" --memory-class 4
done
for bad in 0 13 16 abc -3 '' 4x 99999999 ' 4' 0x4 +4 4.0; do
  refuse "--memory-class '$bad'" '--memory-class: 1 to 12' --identity-tree "$T/identity.adt" --identity-groups memclass --memory-class "$bad"
done
for bad in '' 'H 18' 'H18/x' '0123456789012345678901234567890X'; do
  refuse "--soc-generation '$bad'" '--soc-generation: 1 to 31' --soc-generation "$bad"
done
for bad in '' abc -1 201 1000 '30,' ',1' ',' '30,1,2' '30,65536' '30,-1' ' 30' '30 ' '30, 1' '0x1e' '3.5' '30,x' '+30' '300000' '30,100000'; do
  refuse "--display-corner-radius '$bad'" '--display-corner-radius PT[,WORD2]: whole decimal numbers, PT 0 to 200 points, WORD2 0 to 65535' --display-corner-radius "$bad"
done
rm -f "${T:?}/refused.img" "${T:?}/refused.img.adt"
"$B/xnuloader-crosvm" "$T/kernel.macho" "$T/refused.img" --ram-mib 512 --identity-tree "$T/none.adt" > "$T/missing.txt" 2>&1
check 'a missing tree is refused with status 1' [ $? -eq 1 ]
check 'a missing tree is refused with its name and the reason' grep -q "none.adt: No such file" "$T/missing.txt"
check 'a missing tree leaves no Image' sh -c '[ ! -e "$1/refused.img" ] && [ ! -e "$1/refused.img.adt" ]' sh "$T"
for opt in --identity-tree --identity-groups --memory-class --soc-generation --display-corner-radius; do
  rm -f "${T:?}/refused.img" "${T:?}/refused.img.adt"
  "$B/xnuloader-crosvm" "$T/kernel.macho" "$T/refused.img" --ram-mib 512 "$opt" > "$T/novalue.txt" 2>&1
  check "the option $opt without a value is the usage (status 2)" [ $? -eq 2 ]
  check "the option $opt without a value leaves no Image" sh -c '[ ! -e "$1/refused.img" ] && [ ! -e "$1/refused.img.adt" ]' sh "$T"
done
rm -f "${T:?}/refused.img" "${T:?}/refused.img.adt"
"$B/xnuloader-crosvm" "$T/kernel.macho" "$T/refused.img" --ram-mib 512 --identity-tree "$T/identity.adt" --identity-root > "$T/oldroot.txt" 2>&1
check 'the old option --identity-root is gone: the usage, status 2' [ $? -eq 2 ]
check 'the old option --identity-root leaves no Image' sh -c '[ ! -e "$1/refused.img" ] && [ ! -e "$1/refused.img.adt" ]' sh "$T"
check 'the usage names --identity-groups and --display-corner-radius, not --identity-root' sh -c \
  'grep -q -e --identity-groups "$1" && grep -q -e --display-corner-radius "$1" && ! grep -q -e --identity-root "$1"' sh "$T/oldroot.txt"

echo "crosvm tool tests: $checks checks, $failed failed"
[ "$failed" -eq 0 ]
