#!/usr/bin/env python3
"""Fold appinst's trust-inbox CDHashes into an owner trust bundle, for route A' (docs/appinst.md).

This is the HOST side of the two-phase install: after `appinst install` stages an app in the guest, its
CDHashes sit in the guest's trust inbox (APPINST_TRUST_INBOX, one JSON per app: {"cdhashes":[...hex...],
"category":N}). Before the next guest boot the owner trust module xnuhost injects must carry those hashes, or
TXM will not trust the re-signed app. This tool rebuilds the bundle's first module (the owner module, the one
make-owner-trust-bundle.py emits) to include them, leaving every other module (Apple's originals, the App
cryptex) byte for byte.

  fold-trust-inbox.py IN_BUNDLE INBOX_DIR OUT_BUNDLE

Both bundle files are the concatenated-module format DarwinBoot's ValidTrustCaches reads (see
scripts/make-owner-trust-bundle.py). This mirrors that script's `own` module and `valid_like_the_loader` check.

NOTE: the phone has no Python; this is the reference implementation and the build-PC tool. The actual
restart-time fold on the device is the xnuhost (Rust) change described in docs/appinst.md "host wiring" — this
file defines exactly the bytes it must produce.
"""
import json
import struct
import sys
import uuid
from pathlib import Path

ENTRY = 24          # 20-byte hash + [hashType=2, 0, category, 0]
HDR = struct.Struct('<I16sI')   # version(2), uuid, count


def parse_bundle(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        raise SystemExit('not a trust bundle (bad module count)')
    starts = struct.unpack_from(f'<{count}I', data, 4)
    mods = []
    for i in range(count):
        end = starts[i + 1] if i + 1 < count else len(data)
        mods.append(data[starts[i]:end])
    return mods


def owner_hashes(module):
    version, _uuid, n = HDR.unpack_from(module)
    if version != 2 or len(module) != HDR.size + n * ENTRY:
        raise SystemExit('first module is not a complete version-2 owner module')
    out = {}
    for i in range(n):
        at = HDR.size + i * ENTRY
        h = module[at:at + 20]
        category = module[at + 22]
        out[h] = category
    return out


def build_owner(hashes):
    # hashes: dict cdhash(bytes20) -> category. DarwinBoot needs strictly ascending hashes.
    ordered = sorted(hashes)
    ident = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.adreno-metal:' + ''.join(h.hex() for h in ordered))
    body = b''.join(h + bytes([2, 0, hashes[h], 0]) for h in ordered)
    return HDR.pack(2, ident.bytes, len(ordered)) + body


def valid_like_the_loader(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', data, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24 or struct.unpack_from('<I', data, start)[0] != 2:
            return False
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4 or (entry and data[at - 24:at - 4] >= data[at:at + 20]):
                return False
    return True


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    in_bundle, inbox_dir, out_bundle = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    data = Path(in_bundle).read_bytes()
    mods = parse_bundle(data)
    hashes = owner_hashes(mods[0])

    added = 0
    for jf in sorted(inbox_dir.glob('*.json')):
        doc = json.loads(jf.read_text())
        category = int(doc.get('category', 0))
        for hexh in doc['cdhashes']:
            h = bytes.fromhex(hexh)
            if len(h) != 20:
                raise SystemExit(f'{jf.name}: a cdhash is not 20 bytes')
            if h not in hashes:
                added += 1
            hashes[h] = category

    mods[0] = build_owner(hashes)
    cursor, starts = 4 + 4 * len(mods), []
    for m in mods:
        starts.append(cursor)
        cursor += len(m)
    bundle = struct.pack(f'<{1 + len(mods)}I', len(mods), *starts) + b''.join(mods)
    if not valid_like_the_loader(bundle):
        raise SystemExit('the rebuilt bundle would be refused by the boot loader')
    Path(out_bundle).write_bytes(bundle)
    print(f'folded {added} new CDHash(es); owner module now {owner_hashes(mods[0]).__len__()} entries -> {out_bundle}')


if __name__ == '__main__':
    main()
