"""Tests of userland/apps/appsaddon.py: the apps merged into the Metal GPU addon, the kernel's rules for an addon, the
owner trust list and the image set's manifest.

    python3 -I userland/apps/test_appsaddon.py
"""
import hashlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'tests'))
import appsaddon  # noqa: E402
import fakeroot  # noqa: E402


def metal_addon():
    """An addon like userland/metal/build.sh's: the bundle's files and the .publish marker."""
    plugin, _ = fakeroot.signed_macho('com.oneios.NativeAdrenoMetal')
    return appsaddon.write_addon([
        ('addons/NativeAdrenoMetal.bundle/Info.plist', 0o644, b'<plist/>'),
        ('addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal', 0o755, plugin),
        ('addons/NativeAdrenoMetal.publish', 0o644, b'1\n'),
    ])


def kernel_accepts(blob):
    """The kernel's validation pass (kern_exec.c native_gpu_addon_install), written again from the C, independently of
    appsaddon.read_addon: True when the kernel would write the addon."""
    if len(blob) < 24 or blob[:8] != b'NGPUADD2':
        return False
    count, = struct.unpack_from('<I', blob, 8)
    total, = struct.unpack_from('<Q', blob, 16)
    if total != len(blob) or not count or count > 4096:
        return False
    at = 24
    for _ in range(count):
        if len(blob) - at < 24:
            return False
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        name_padded, size_padded = (name_bytes + 7) & ~7, (packed + 7) & ~7
        name = blob[at:at + name_bytes]
        if (name_bytes < 2 or name_bytes > 160 or len(blob) - at < name_padded or name[-1] != 0 or name.find(b'\0') != name_bytes - 1
                or name[0] == ord('/') or mode & ~0o777 or size > 0x7FFFFFFF or not packed or packed > 0x7FFFFFFF
                or len(blob) - at - name_padded < size_padded):
            return False
        if b'..' in name[:-1]:
            return False
        at += name_padded + size_padded
    return at == len(blob)


class AppsAddon(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.apps = os.path.join(self.dir, 'apps')
        self.hashes = {}
        for name, ident in (('OneiOSBench', 'com.oneios.bench'), ('OneiOS2048', 'com.oneios.game2048')):
            app = os.path.join(self.apps, name + '.app')
            os.makedirs(app)
            data, cdhash = fakeroot.signed_macho(ident, payload=name.encode())
            self.write(os.path.join(app, name), data, 0o755)
            self.write(os.path.join(app, 'Info.plist'), b'<plist/>')
            self.write(os.path.join(app, 'AppIcon60x60@3x.png'), b'\x89PNG')
            self.hashes[f'/private/var/ngpu/apps/{name}.app/{name}'] = cdhash
        data, cdhash = fakeroot.signed_macho('com.oneios.appreg')
        self.write(os.path.join(self.apps, 'oneios-appreg'), data, 0o755)
        self.hashes['/private/var/ngpu/apps/oneios-appreg'] = cdhash
        self.write(os.path.join(self.apps, 'appreg.conf'), b'arg=/private/var/ngpu/apps/oneios-appreg\n')
        self.write(os.path.join(self.apps, 'cdhashes.txt'),
                   ''.join(f'{h} arm64e pages=4096 id {p}\n' for p, h in self.hashes.items()).encode())
        self.metal = os.path.join(self.dir, 'metal.gpuaddon')
        self.write(self.metal, metal_addon())
        self.trust = os.path.join(self.dir, 'owner-trust.json')
        old = {'schema': 1, 'what': 'x', 'cdhashes': ['aa' * 20, 'bb' * 20],
               'files': [{'path': '/private/var/oneios/bin/oneios-init', 'cdhash': 'aa' * 20},
                         {'path': '/private/var/ngpu/addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal', 'cdhash': 'bb' * 20}]}
        self.write(self.trust, json.dumps(old).encode())
        self.manifest = os.path.join(self.dir, 'manifest.json')
        self.write(self.manifest, json.dumps({'tweaks': {'info': {'owner_cdhashes': 2}}, 'files': [
            {'path': 'overlay/userland/owner-trust.json', 'size': 1, 'sha256': '00', 'role': 'overlay'},
            {'path': 'trust/app.trustcache', 'size': 863, 'sha256': '82'}]}).encode())

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, path, data, mode=0o644):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
        os.chmod(path, mode)

    def build(self, *extra, expect=0):
        out = io.StringIO()
        with redirect_stdout(out), redirect_stderr(out):
            code = appsaddon.main(['build', '--apps', self.apps, '--metal', self.metal, '--out', os.path.join(self.dir, 'out.gpuaddon'),
                                   '--trust', self.trust, '--trust-out', os.path.join(self.dir, 'trust-out.json'),
                                   '--manifest', self.manifest, '--manifest-out', os.path.join(self.dir, 'manifest-out.json'), *extra])
        self.assertEqual(code, expect, out.getvalue())
        return out.getvalue()

    def load(self, name):
        with open(os.path.join(self.dir, name), 'rb') as f:
            return f.read()

    def test_the_merged_addon_keeps_the_plugin_and_adds_the_apps(self):
        self.build('--expect', os.path.join(self.apps, 'cdhashes.txt'))
        blob = self.load('out.gpuaddon')
        self.assertTrue(kernel_accepts(blob))
        entries = {n: (m, d) for n, m, d in appsaddon.read_addon(blob)}
        for n, _, d in appsaddon.read_addon(metal_addon()):
            self.assertEqual(entries[n][1], d, n)
        self.assertEqual(entries['apps/OneiOSBench.app/OneiOSBench'][0], 0o755)
        self.assertEqual(entries['apps/OneiOSBench.app/Info.plist'][0], 0o644)
        self.assertEqual(entries['apps/oneios-appreg'][0], 0o755)
        self.assertEqual(entries['oneios/services.d/appreg.conf'][1], b'arg=/private/var/ngpu/apps/oneios-appreg\n')
        self.assertTrue(entries['oneios/enabled/appreg'][1])   # never an empty file
        self.assertNotIn('apps/cdhashes.txt', entries)
        self.assertNotIn('apps/appreg.conf', entries)

    def test_every_macho_is_in_the_trust_list_with_its_guest_path(self):
        self.build()
        trust = json.loads(self.load('trust-out.json'))
        paths = {f['path']: f['cdhash'] for f in trust['files']}
        for path, cdhash in self.hashes.items():
            self.assertEqual(paths[path], cdhash)
            self.assertIn(cdhash, trust['cdhashes'])
        self.assertIn('aa' * 20, trust['cdhashes'])          # the earlier ones stay
        self.assertIn('bb' * 20, trust['cdhashes'])
        self.assertEqual(trust['cdhashes'], sorted(set(trust['cdhashes'])))
        self.assertEqual(len(trust['cdhashes']), 2 + 3)

    def test_the_manifest_names_the_new_trust_file(self):
        self.build()
        data = self.load('trust-out.json')
        manifest = json.loads(self.load('manifest-out.json'))
        entry = [e for e in manifest['files'] if e['path'] == 'overlay/userland/owner-trust.json'][0]
        self.assertEqual(entry['size'], len(data))
        self.assertEqual(entry['sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(entry['role'], 'overlay')
        self.assertEqual(manifest['tweaks']['info']['owner_cdhashes'], 5)
        self.assertEqual([e for e in manifest['files'] if e['path'] == 'trust/app.trustcache'][0]['sha256'], '82')

    def test_a_second_build_over_its_own_trust_list_changes_nothing(self):
        self.build()
        os.replace(os.path.join(self.dir, 'trust-out.json'), self.trust)
        first = self.load('out.gpuaddon')
        before = self.load('owner-trust.json')
        self.build()
        self.assertEqual(self.load('trust-out.json'), before)
        self.assertEqual(self.load('out.gpuaddon'), first)

    def test_a_rebuilt_app_replaces_its_old_hash(self):
        self.build()
        os.replace(os.path.join(self.dir, 'trust-out.json'), self.trust)
        old = self.hashes['/private/var/ngpu/apps/OneiOSBench.app/OneiOSBench']
        data, new = fakeroot.signed_macho('com.oneios.bench', payload=b'rebuilt')
        self.write(os.path.join(self.apps, 'OneiOSBench.app', 'OneiOSBench'), data, 0o755)
        self.build()
        trust = json.loads(self.load('trust-out.json'))
        self.assertIn(new, trust['cdhashes'])
        self.assertNotIn(old, trust['cdhashes'])

    def test_expect_catches_a_bundle_changed_after_the_build(self):
        data, _ = fakeroot.signed_macho('com.oneios.bench', payload=b'edited after signing')
        self.write(os.path.join(self.apps, 'OneiOSBench.app', 'OneiOSBench'), data, 0o755)
        out = self.build('--expect', os.path.join(self.apps, 'cdhashes.txt'), expect=1)
        self.assertIn('not in', out)

    def test_an_unsigned_binary_is_refused(self):
        self.write(os.path.join(self.apps, 'OneiOSBench.app', 'OneiOSBench'), b'\xcf\xfa\xed\xfe' + bytes(60), 0o755)
        self.assertIn('OneiOSBench', self.build(expect=1))
        self.assertFalse(os.path.exists(os.path.join(self.dir, 'out.gpuaddon')))

    def test_a_macho_without_the_executable_bit_still_gets_0755(self):
        os.chmod(os.path.join(self.apps, 'OneiOS2048.app', 'OneiOS2048'), 0o644)   # copied through NTFS
        self.build()
        entries = {n: m for n, m, _ in appsaddon.read_addon(self.load('out.gpuaddon'))}
        self.assertEqual(entries['apps/OneiOS2048.app/OneiOS2048'], 0o755)

    def test_an_addon_without_the_plugin_is_refused(self):
        self.write(self.metal, appsaddon.write_addon([('something/else', 0o644, b'x')]))
        self.assertIn('Metal plug-in', self.build(expect=1))

    def test_links_and_names_the_kernel_refuses(self):
        os.symlink('Info.plist', os.path.join(self.apps, 'OneiOSBench.app', 'link'))
        self.assertIn('not a plain file', self.build(expect=1))
        os.remove(os.path.join(self.apps, 'OneiOSBench.app', 'link'))
        self.write(os.path.join(self.apps, 'OneiOSBench.app', 'a..b'), b'x')
        self.assertIn('".."', self.build(expect=1))
        os.remove(os.path.join(self.apps, 'OneiOSBench.app', 'a..b'))
        self.write(os.path.join(self.apps, 'OneiOSBench.app', 'x' * 150), b'x')
        self.assertIn('name of', self.build(expect=1))

    def test_the_kernel_rules(self):
        for name, mode in (('/abs', 0o644), ('a/../b', 0o644), ('ok', 0o4755), ('x' * 160, 0o644), ('', 0o644),
                           ('oneios/bin/oneios-init', 0o755)):
            with self.assertRaises(appsaddon.AddonError, msg=name):
                appsaddon.check_entry(name, mode, 1)
        appsaddon.check_entry('x' * 159, 0o644, 1)
        appsaddon.check_entry('oneios/services.d/appreg.conf', 0o644, 1)
        self.assertTrue(kernel_accepts(appsaddon.write_addon([('x' * 159, 0o644, b'')])))
        with self.assertRaises(appsaddon.AddonError):
            appsaddon.write_addon([('a', 0o644, b'1'), ('a', 0o644, b'2')])
        with self.assertRaises(appsaddon.AddonError):
            appsaddon.write_addon([(f'f{i}', 0o644, b'') for i in range(4097)])

    def test_the_size_limit(self):
        big = os.urandom(65 << 20)   # does not compress
        with self.assertRaises(appsaddon.AddonError):
            appsaddon.write_addon([('big', 0o644, big)])

    def test_read_refuses_what_the_kernel_refuses(self):
        blob = bytearray(metal_addon())
        self.assertTrue(kernel_accepts(bytes(blob)))
        bad = bytes(blob[:-1])
        self.assertFalse(kernel_accepts(bad))
        with self.assertRaises(appsaddon.AddonError):
            appsaddon.read_addon(bad)
        bad = bytearray(blob)
        struct.pack_into('<Q', bad, 16, len(bad) + 8)
        with self.assertRaises(appsaddon.AddonError):
            appsaddon.read_addon(bytes(bad))
        # a name with ".." written by hand
        name = b'apps/../x\0'
        packed = zlib.compress(b'x')
        entry = struct.pack('<IIQQ', len(name), 0o644, 1, len(packed)) + name + bytes(-len(name) % 8) + packed + bytes(-len(packed) % 8)
        bad = b'NGPUADD2' + struct.pack('<IIQ', 1, 0, 24 + len(entry)) + entry
        self.assertFalse(kernel_accepts(bad))
        with self.assertRaises(appsaddon.AddonError):
            appsaddon.read_addon(bad)

    def test_guest_paths(self):
        self.assertEqual(appsaddon.guest_path('apps/X.app/X'), '/private/var/ngpu/apps/X.app/X')
        self.assertEqual(appsaddon.guest_path('oneios/services.d/appreg.conf'), '/private/var/oneios/services.d/appreg.conf')

    def test_list(self):
        self.build()
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(appsaddon.main(['list', os.path.join(self.dir, 'out.gpuaddon')]), 0)
        self.assertIn(self.hashes['/private/var/ngpu/apps/oneios-appreg'], out.getvalue())
        self.assertIn('/private/var/oneios/enabled/appreg', out.getvalue())

    def test_the_real_metal_addon_format_reads(self):
        """The writer of userland/metal/build.sh (its python, copied) and this reader agree."""
        files = [('addons/NativeAdrenoMetal.bundle/Info.plist', 0o644, b'<plist/>'), ('addons/NativeAdrenoMetal.publish', 0o644, b'1\n')]

        def pad(data):
            return data + bytes(-len(data) % 8)
        body = b''
        for name, mode, data in files:
            encoded = name.encode() + b'\0'
            packed = zlib.compress(data, 9)
            body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
        blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
        self.assertEqual(appsaddon.read_addon(blob), files)
        self.assertEqual(appsaddon.write_addon(files), blob)


if __name__ == '__main__':
    unittest.main()
