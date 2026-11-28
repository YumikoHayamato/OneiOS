#!/usr/bin/env python3
"""OneiOS's own apps into a boot image without a new Data seed: they ride in the GPU addon (NGPUADD2).

  python3 -I userland/apps/appsaddon.py build --apps DIR --metal ADDON --out OUT.gpuaddon
        [--trust IN.json --trust-out OUT.json] [--manifest IN.json --manifest-out OUT.json] [--expect cdhashes.txt]
  python3 -I userland/apps/appsaddon.py list ADDON          the entries of an addon, and the CDHash of each Mach-O

DIR is the output of userland/apps/build.sh: *.app bundles, oneios-appreg, appreg.conf, cdhashes.txt.

How the addon becomes files (hosted 0031, kern_exec.c native_gpu_addon_install): xnuloader-crosvm --gpu-addon puts
it in /chosen/memory-map GpuAddon; before launchd starts the kernel writes each entry NAME to /private/var/ngpu/NAME
(parents made, root-owned, the entry's mode), skipping files that are already identical, and mounts
/private/var/ngpu/addons over /System/Library/Extensions. There is ONE addon per boot, so the apps are merged into
the Metal plug-in's addon (--metal): an addon without the plug-in would hide the seed's plug-in. The kernel refuses
the whole addon when one entry breaks its rules (then nothing is written and the seed's plug-in is used), so this tool
checks the same rules first: a name of at most 159 bytes, not absolute, no "..", mode within 0777, at most 4096
entries and 64 MiB.

Entries this tool adds:
  apps/<Name>.app/...            -> /private/var/ngpu/apps/<Name>.app/...  the bundles (where oneios-appreg looks)
  apps/oneios-appreg             -> /private/var/ngpu/apps/oneios-appreg   the registrar
  oneios/services.d/appreg.conf, oneios/enabled/appreg
                                 -> /private/var/oneios/... with the kernel hook of userland/apps/kernel (the
                                    "oneios/" prefix); a kernel without it writes them under /private/var/ngpu/oneios/,
                                    harmless, and the apps are placed but not registered.
Every Mach-O's CDHash goes into the owner trust list (--trust/--trust-out) with its guest path; the image set's
manifest entry of owner-trust.json gets the new size and SHA-256 (--manifest/--manifest-out), as take18 did by hand.
"""
import argparse
import hashlib
import json
import os
import stat
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from roottweak import macho  # noqa: E402

MAGIC = b'NGPUADD2'
MAX_ENTRIES = 4096
MAX_BYTES = 64 << 20
MAX_NAME = 160          # with the NUL
INT32_MAX = 0x7FFFFFFF
GUEST_ROOT = '/private/var/ngpu/'
# names the kernel hook sends to /private/var/oneios instead (userland/apps/kernel)
ONEIOS_PREFIX = 'oneios/'
ONEIOS_ALLOWED = ('oneios/services.d/', 'oneios/enabled/')
SERVICE = 'appreg'


class AddonError(Exception):
    pass


def _pad(data):
    return data + bytes(-len(data) % 8)


def check_entry(name, mode, size):
    """The kernel's rules for one entry (native_gpu_addon_install), as an AddonError."""
    encoded = name.encode()
    if len(encoded) + 1 < 2 or len(encoded) + 1 > MAX_NAME:
        raise AddonError(f'{name}: name of {len(encoded)} bytes (the kernel takes 1..{MAX_NAME - 1})')
    if b'\0' in encoded:
        raise AddonError(f'{name!r}: NUL in the name')
    if name.startswith('/'):
        raise AddonError(f'{name}: absolute name')
    if '..' in name:
        raise AddonError(f'{name}: ".." in the name (the kernel refuses any, also inside a file name)')
    if mode & ~0o777:
        raise AddonError(f'{name}: mode {mode:o} beyond 0777')
    if size > INT32_MAX:
        raise AddonError(f'{name}: {size} bytes, over 2 GiB')
    if name.startswith(ONEIOS_PREFIX) and not name.startswith(ONEIOS_ALLOWED):
        raise AddonError(f'{name}: the kernel hook writes only {", ".join(ONEIOS_ALLOWED)} under /private/var/oneios')


def guest_path(name):
    if name.startswith(ONEIOS_ALLOWED):
        return '/private/var/' + name
    return GUEST_ROOT + name


def read_addon(blob):
    """[(name, mode, data)] of an addon, with the kernel's checks."""
    if len(blob) < 24 or blob[:8] != MAGIC:
        raise AddonError('not an NGPUADD2 addon')
    count, _, total = struct.unpack_from('<IIQ', blob, 8)
    if total != len(blob) or not count or count > MAX_ENTRIES:
        raise AddonError(f'header: {count} entries, size {total} for {len(blob)} bytes')
    at, out = 24, []
    for _ in range(count):
        if len(blob) - at < 24:
            raise AddonError('entry header out of range')
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name_padded, packed_padded = (name_bytes + 7) & ~7, (packed + 7) & ~7
        if name_bytes < 2 or name_bytes > MAX_NAME or len(blob) - at < name_padded:
            raise AddonError('name out of range')
        raw = blob[at:at + name_bytes]
        if raw[-1] != 0 or b'\0' in raw[:-1]:
            raise AddonError('name not NUL-terminated')
        name = raw[:-1].decode()
        at += name_padded
        if not packed or packed > INT32_MAX or len(blob) - at < packed_padded:
            raise AddonError(f'{name}: data out of range')
        check_entry(name, mode, size)
        data = zlib.decompress(blob[at:at + packed])
        if len(data) != size:
            raise AddonError(f'{name}: {len(data)} bytes inflated, {size} declared')
        out.append((name, mode, data))
        at += packed_padded
    if at != len(blob):
        raise AddonError('bytes after the last entry')
    return out


def write_addon(entries):
    """The addon of [(name, mode, data)]: the format of userland/metal/build.sh addon."""
    if not entries or len(entries) > MAX_ENTRIES:
        raise AddonError(f'{len(entries)} entries (1..{MAX_ENTRIES})')
    seen = set()
    body = b''
    for name, mode, data in entries:
        check_entry(name, mode, len(data))
        if name in seen:
            raise AddonError(f'{name}: twice')
        seen.add(name)
        encoded = name.encode() + b'\0'
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + _pad(encoded) + _pad(packed)
    blob = MAGIC + struct.pack('<IIQ', len(entries), 0, 24 + len(body)) + body
    if len(blob) > MAX_BYTES:
        raise AddonError(f'{len(blob)} bytes, over the 64 MiB the loader and the kernel take')
    return blob


def collect_apps(directory):
    """The entries for a build.sh output folder: bundles, the registrar and its service files."""
    entries = []
    bundles = sorted(n for n in os.listdir(directory) if n.endswith('.app') and os.path.isdir(os.path.join(directory, n)))
    if not bundles:
        raise AddonError(f'{directory}: no .app bundle')
    for bundle in bundles:
        top = os.path.join(directory, bundle)
        for current, dirs, files in os.walk(top):
            dirs.sort()
            for d in dirs:
                if os.path.islink(os.path.join(current, d)):
                    raise AddonError(f'{os.path.join(current, d)}: a link (the addon carries files only)')
            for f in sorted(files):
                path = os.path.join(current, f)
                st = os.lstat(path)
                if not stat.S_ISREG(st.st_mode):
                    raise AddonError(f'{path}: not a plain file (the addon carries files only)')
                rel = os.path.relpath(path, directory).replace(os.sep, '/')
                with open(path, 'rb') as fh:
                    data = fh.read()
                # the executable bit decides; a Mach-O without it (NTFS copies) is made executable anyway
                mode = 0o755 if (st.st_mode & 0o111 or macho.is_macho(data[:8])) else 0o644
                entries.append(('apps/' + rel, mode, data))
    registrar = os.path.join(directory, 'oneios-appreg')
    if os.path.isfile(registrar):
        with open(registrar, 'rb') as fh:
            entries.append(('apps/oneios-appreg', 0o755, fh.read()))
        conf = os.path.join(directory, 'appreg.conf')
        with open(conf, 'rb') as fh:
            entries.append((f'oneios/services.d/{SERVICE}.conf', 0o644, fh.read()))
        # not empty: every entry the kernel takes has packed data, and an empty file was never sent that way
        entries.append((f'oneios/enabled/{SERVICE}', 0o644, b'1\n'))
    return entries


def machos(entries):
    """[{path, cdhash, ...}] of every Mach-O among the entries (each slice), refusing unsigned ones."""
    out = []
    for name, _, data in entries:
        if not macho.is_macho(data[:8]):
            continue
        try:
            slices = macho.describe(data)
        except macho.MachOError as e:
            raise AddonError(f'{name}: {e}')
        for s in slices:
            warnings = []
            if s['arch'] != 'arm64e':
                warnings.append(f'{s["arch"]}, not arm64e')
            if s['page_size'] != 4096:
                warnings.append(f'signature pages of {s["page_size"]} bytes, not 4096')
            out.append({'path': guest_path(name), 'cdhash': s['cdhash'], 'hash': s['hash'], 'identifier': s['identifier'],
                        'arch': s['arch'], 'page_size': s['page_size'], 'adhoc': s['adhoc'], 'warnings': warnings})
    return out


def merge(base, extra):
    names = {n for n, _, _ in base}
    for n, _, _ in extra:
        if n in names:
            raise AddonError(f'{n}: in the Metal addon already')
    return list(base) + list(extra)


def update_trust(trust, files):
    """owner-trust.json with these files added (a file of the same path replaced)."""
    paths = {f['path'] for f in files}
    kept = [f for f in trust.get('files', []) if f.get('path') not in paths]
    out = dict(trust)
    out['files'] = kept + list(files)
    replaced = {f['cdhash'] for f in trust.get('files', []) if f.get('path') in paths}
    still = {f['cdhash'] for f in kept}
    listed = set(trust.get('cdhashes', [])) - (replaced - still)
    out['cdhashes'] = sorted(listed | {f['cdhash'] for f in files})
    return out


def update_manifest(manifest, trust_bytes, path='overlay/userland/owner-trust.json'):
    out = json.loads(json.dumps(manifest))
    hit = [e for e in out.get('files', []) if e.get('path') == path]
    if len(hit) != 1:
        raise AddonError(f'manifest: {len(hit)} entries for {path}')
    hit[0]['size'] = len(trust_bytes)
    hit[0]['sha256'] = hashlib.sha256(trust_bytes).hexdigest()
    info = out.get('tweaks', {}).get('info')
    if isinstance(info, dict) and 'owner_cdhashes' in info:
        info['owner_cdhashes'] = len(json.loads(trust_bytes)['cdhashes'])
    return out


def read_expect(path):
    """cdhash -> identifier of a cdhashes.txt (lines '<cdhash> <arch> pages=N <identifier> <file>')."""
    out = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 4 and len(parts[0]) == 40:
                out[parts[0]] = parts[3]
    return out


def dump_json(value):
    return (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()


def cmd_build(a):
    with open(a.metal, 'rb') as f:
        base = read_addon(f.read())
    if not any(n.startswith('addons/') and n.endswith('.bundle/Info.plist') for n, _, _ in base):
        raise AddonError(f'{a.metal}: no addons/<plug-in>.bundle: an addon without the Metal plug-in hides the seed\'s')
    extra = collect_apps(a.apps)
    entries = merge(base, extra)
    blob = write_addon(entries)
    read_addon(blob)  # what the kernel will check, once more on the bytes written
    files = machos(extra)
    for f in files:
        for w in f['warnings']:
            print(f'warning: {f["path"]}: {w}', file=sys.stderr)
    if a.expect:
        expect = read_expect(a.expect)
        missing = [f for f in files if f['cdhash'] not in expect]
        if missing:
            raise AddonError('CDHashes not in ' + a.expect + ': ' + ', '.join(f'{f["path"]} {f["cdhash"]}' for f in missing))
        if len(expect) != len({f['cdhash'] for f in files}):
            raise AddonError(f'{a.expect} lists {len(expect)} CDHashes, the bundles have {len({f["cdhash"] for f in files})}')
    with open(a.out, 'wb') as f:
        f.write(blob)
    print(f'{a.out}: {len(entries)} entries ({len(base)} of the Metal addon, {len(extra)} new), {len(blob)} bytes, '
          f'sha256 {hashlib.sha256(blob).hexdigest()}')
    for f in files:
        print(f'  {f["cdhash"]} {f["arch"]} pages={f["page_size"]} {f["identifier"]} {f["path"]}')
    if a.trust:
        with open(a.trust) as f:
            trust = json.load(f)
        new = update_trust(trust, files)
        data = dump_json(new)
        with open(a.trust_out, 'wb') as f:
            f.write(data)
        print(f'{a.trust_out}: {len(new["cdhashes"])} CDHashes ({len(trust.get("cdhashes", []))} before), '
              f'{len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()}')
        if a.manifest:
            with open(a.manifest) as f:
                manifest = json.load(f)
            with open(a.manifest_out, 'wb') as f:
                f.write(dump_json(update_manifest(manifest, data)))
            print(f'{a.manifest_out}: owner-trust.json entry updated')
    return 0


def cmd_list(a):
    with open(a.addon, 'rb') as f:
        entries = read_addon(f.read())
    hashes = {f['path']: f for f in machos(entries)}
    for name, mode, data in entries:
        h = hashes.get(guest_path(name))
        print(f'{mode:04o} {len(data):9d} {guest_path(name)}' + (f'  cdhash {h["cdhash"]} {h["arch"]} pages={h["page_size"]}' if h else ''))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(prog='appsaddon', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build', help='merge the apps into the Metal addon; update the trust list and the manifest')
    b.add_argument('--apps', required=True, metavar='DIR', help='userland/apps/build.sh output')
    b.add_argument('--metal', required=True, metavar='ADDON', help='the Metal plug-in\'s NativeAdrenoMetal.gpuaddon')
    b.add_argument('--out', required=True, metavar='FILE')
    b.add_argument('--trust', metavar='JSON', help='the image set\'s overlay/userland/owner-trust.json')
    b.add_argument('--trust-out', metavar='JSON')
    b.add_argument('--manifest', metavar='JSON', help='the image set\'s manifest.json (needs --trust)')
    b.add_argument('--manifest-out', metavar='JSON')
    b.add_argument('--expect', metavar='TXT', help='cdhashes.txt of the build: every Mach-O must match it')
    l = sub.add_parser('list', help='the entries of an addon')
    l.add_argument('addon')
    a = p.parse_args(argv)
    if a.cmd == 'build':
        if bool(a.trust) != bool(a.trust_out) or bool(a.manifest) != bool(a.manifest_out) or (a.manifest and not a.trust):
            p.error('--trust needs --trust-out, --manifest needs --manifest-out and --trust')
    try:
        return cmd_build(a) if a.cmd == 'build' else cmd_list(a)
    except (AddonError, OSError, zlib.error) as e:
        print(f'appsaddon: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
