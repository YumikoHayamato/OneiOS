"""Tests of userland/jetsam_derive.py on a list made here from nothing. No file of Apple's is used.

    python3 -I userland/tests/test_jetsam_derive.py        (from the repository's top)

With ONEIOS_V54_PLIST=FILE (the OS image's com.apple.jetsamproperties.V54.plist, read-only) the last class also checks the
rules on the real list, with its own arithmetic: `oneios-tool image apfs-copy OS_IMG
/System/Library/LaunchDaemons/com.apple.jetsamproperties.V54.plist FILE`. Without it that class is skipped.
"""
import contextlib
import copy
import io
import os
import plistlib
import re
import sys
import tempfile
import unittest
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import jetsam_derive as jd  # noqa: E402


def read(path, mode='rb'):
    with open(path, mode) as f:
        return f.read()


def write(path, data):
    with open(path, 'wb') as f:
        f.write(data)


def make():
    """A small list of the real shape, with invented numbers (labels of the protected and raised ones are the real ones)."""
    return {'Version4': {
        'Daemon': {
            'Default': {'Global': {}},
            'Override': {
                'com.example.big': {'JetsamPriority': 140, 'ActiveSoftMemoryLimit': 1000, 'InactiveHardMemoryLimit': 500,
                                    'ThreadLimit': 512, 'EnergyEfficiencyMode': 'Efficient'},
                'com.example.mid': {'ActiveSoftMemoryLimit': 300, 'InactiveHardMemoryLimit': 201},
                'com.example.edge': {'ActiveSoftMemoryLimit': 200, 'InactiveHardMemoryLimit': 199, 'ActiveHardMemoryLimit': 1},
                'com.example.small': {'ActiveSoftMemoryLimit': 64, 'InactiveHardMemoryLimit': 10, 'JetsamPriority': 180},
                'com.example.unlimited': {'ActiveSoftMemoryLimit': -1, 'ActiveHardMemoryLimit': 2048,
                                          'InactiveHardMemoryLimit': 2048, 'InactiveSoftMemoryLimit': -1},
                'com.example.nolimit': {'JetsamPriority': 100, 'DisableAllowed': False, 'FreezerEligible': True},
                'com.example.crashy': {'ActiveSoftMemoryLimit': 900, 'RecoverOnCrash': True, 'JetsamPriority': 180},
                'com.example.notcrashy': {'ActiveSoftMemoryLimit': 900, 'RecoverOnCrash': False},
                'com.example.counts': {'ThreadLimit': 512, 'HardPortLimit': 25000, 'SoftPortLimit': 3000,
                                       'LogicalWritesLimit': 1024, 'OSLogRateLimit': 128, 'MallocMaxMagazines': 65535,
                                       'ConclaveMemoryLimit': 280, 'ActiveSoftMemoryLimit': 800},
                'com.example.odd': {'ActiveSoftMemoryLimit': True, 'InactiveHardMemoryLimit': 800.0},
                'com.apple.backboardd': {'JetsamPriority': 170, 'ActiveSoftMemoryLimit': -1, 'ActiveHardMemoryLimit': -1,
                                         'InactiveHardMemoryLimit': -1, 'RecoverOnCrash': True},
                'com.apple.SpringBoard': {'JetsamPriority': 160, 'ActiveSoftMemoryLimit': 550, 'InactiveSoftMemoryLimit': 550,
                                          'InactiveHardMemoryLimit': -1, 'RecoverOnCrash': True},
                'com.apple.SwitchBoard': {'ActiveSoftMemoryLimit': 600, 'InactiveHardMemoryLimit': 600},
                'com.apple.iconservices.iconservicesagent': {'JetsamPriority': 140, 'ActiveSoftMemoryLimit': 1124,
                                                              'InactiveHardMemoryLimit': 15},
            },
        },
        'SystemApp': {
            'Default': {'Global': {}},
            'Override': {label: {'ActiveSoftMemoryLimit': 800, 'InactiveSoftMemoryLimit': 450, 'JetsamPriority': 150}
                         for label in ('com.apple.PosterBoard', 'com.apple.springboardservices',
                                       'com.apple.chrono.WidgetRenderer-Default', 'com.apple.chrono.WidgetRenderer-StandBy')},
        },
        'SystemXPCService': {
            'Default': {'Global': {}},
            'Override': {
                'com.apple.MapKit.SnapshotService': {'ActiveSoftMemoryLimit': 300, 'InactiveHardMemoryLimit': 14},
                'com.apple.springboard.SBRendererService': {'ActiveSoftMemoryLimit': 400, 'InactiveHardMemoryLimit': 60},
            },
        },
        'Extension': {
            'Default': {'Global': {}},
            'Override': {
                'com.apple.posterkit.provider': {'PosterProvider_Rendering_PhotosSoftMemoryLimit': 350,
                                                 'ActiveSoftMemoryLimit': 150},
                'com.example.poster': {'PosterProvider_Rendering_PhotosSoftMemoryLimit': 350,
                                       'PosterProvider_EditingSoftMemoryLimit': -1,
                                       'WidgetRenderer_HighMemorySoftMemoryLimit': 400, 'SiriTransientSoftMemoryLimit': 350},
            },
        },
        'App': {
            'Default': {'Global': {}},
            'Override': {'Global': {'Games_EntitledHardMemoryLimit': 8192, 'ThreadLimit': 512, 'FreezerEligible': True}},
        },
        'ExternalExtension': {
            'Default': {'Global': {}},
            'Override': {'Global': {'ExternalExtension_UIHardMemoryLimit': 400, 'ExternalExtension_NonUIHardMemoryLimit': 35,
                                    'ActiveHardMemoryLimit': 6, 'EnablePressuredExit': False}},
        },
        'System': {
            'Default': {'Global': {}},
            'Override': {'Global': {'UserReclaimableLimit': 62, 'DarkBootSystemUIHardLimit': 250, 'KernelHighWaterMark': 78}},
        },
        'Angel': {'Default': {'Global': {}}, 'Override': {'Global': {'ActiveSoftMemoryLimit': 6, 'InactiveSoftMemoryLimit': 6}}},
    }}


# What make() becomes under the defaults (factor 0.4, threshold 200, floor 200; the protected and the raised as built in).
def want():
    w = make()
    d = w['Version4']['Daemon']['Override']
    d['com.example.big'].update(ActiveSoftMemoryLimit=400, InactiveHardMemoryLimit=200)              # 0.4 x 1000; 0.4 x 500 = 200
    d['com.example.mid'].update(ActiveSoftMemoryLimit=200, InactiveHardMemoryLimit=200)              # 120 -> the floor; 201 -> 200
    d['com.example.unlimited'].update(ActiveHardMemoryLimit=819, InactiveHardMemoryLimit=819)        # 819.2, the -1 stay
    d['com.example.notcrashy']['ActiveSoftMemoryLimit'] = 360
    d['com.example.counts']['ActiveSoftMemoryLimit'] = 320
    d['com.apple.iconservices.iconservicesagent'].update(ActiveSoftMemoryLimit=450, InactiveHardMemoryLimit=64)   # 449.6; raised
    w['Version4']['SystemXPCService']['Override']['com.apple.MapKit.SnapshotService'].update(
        ActiveSoftMemoryLimit=200, InactiveHardMemoryLimit=64)
    e = w['Version4']['Extension']['Override']['com.example.poster']
    e.update(PosterProvider_Rendering_PhotosSoftMemoryLimit=200, WidgetRenderer_HighMemorySoftMemoryLimit=200,
             SiriTransientSoftMemoryLimit=200)
    w['Version4']['App']['Override']['Global']['Games_EntitledHardMemoryLimit'] = 3277               # 3276.8
    w['Version4']['ExternalExtension']['Override']['Global']['ExternalExtension_UIHardMemoryLimit'] = 200
    return w


class Rules(unittest.TestCase):
    def test_the_whole_list(self):
        got, rep = jd.derive(make())
        self.assertEqual(got, want())
        self.assertTrue(jd.identical(got, want()))
        self.assertEqual(jd.check(make(), got), [])

    def test_the_input_is_not_changed(self):
        before = make()
        jd.derive(before)
        self.assertTrue(jd.identical(before, make()))

    def test_scaling_and_rounding_in_whole_mb_with_halves_up(self):
        p = jd.Params(factor='0.4')
        self.assertEqual([jd.scale(x, p) for x in (1000, 1124, 2048, 3072, 8192, 4310)], [400, 450, 819, 1229, 3277, 1724])
        half = jd.Params(factor='1/2', threshold=0, floor=1)
        # 100.5 and 101.5: float rounding (banker's) gives 100 and 102; the rule is up
        self.assertEqual([jd.scale(x, half) for x in (201, 203, 1)], [101, 102, 1])
        third = jd.Params(factor='1/3', threshold=0, floor=1)
        self.assertEqual([jd.scale(x, third) for x in (100, 101, 2, 1)], [33, 34, 1, 1])
        for n in range(1, 4000):
            self.assertEqual(jd.rounded(n, Fraction(2, 5)), (4 * n + 5) // 10, n)

    def test_at_or_below_the_threshold_nothing_changes(self):
        got, _ = jd.derive(make())
        d = got['Version4']['Daemon']['Override']
        self.assertEqual(d['com.example.small'], make()['Version4']['Daemon']['Override']['com.example.small'])
        self.assertEqual(d['com.example.edge'], make()['Version4']['Daemon']['Override']['com.example.edge'])   # 200 and 199 stay
        self.assertEqual(got['Version4']['Angel']['Override']['Global'], {'ActiveSoftMemoryLimit': 6, 'InactiveSoftMemoryLimit': 6})
        # the stricter variant of the header
        got, _ = jd.derive(make(), jd.Params(threshold=64, floor=64))
        self.assertEqual(got['Version4']['Daemon']['Override']['com.example.edge'],
                         {'ActiveSoftMemoryLimit': 80, 'InactiveHardMemoryLimit': 80, 'ActiveHardMemoryLimit': 1})
        self.assertEqual(got['Version4']['Daemon']['Override']['com.example.mid'],
                         {'ActiveSoftMemoryLimit': 120, 'InactiveHardMemoryLimit': 80})

    def test_the_floor(self):
        p = jd.Params(threshold=64, floor=100)
        self.assertEqual([jd.scale(x, p) for x in (64, 65, 99, 100, 101, 249, 250, 251, 1000)], [64, 65, 99, 100, 100, 100, 100, 100, 400])
        got, rep = jd.derive(make())
        floored = {(c.where, c.key) for c in rep.changes if c.why == 'floor'}
        self.assertIn(('Daemon/com.example.mid', 'ActiveSoftMemoryLimit'), floored)
        self.assertIn(('Daemon/com.example.mid', 'InactiveHardMemoryLimit'), floored)
        self.assertNotIn(('Daemon/com.example.big', 'InactiveHardMemoryLimit'), floored)    # 0.4 x 500 = 200 exactly: scaled
        self.assertEqual(min(c.new for c in rep.changes if c.why != 'raised'), 200)

    def test_never_larger_and_never_a_smaller_one_above_a_larger(self):
        for p in (jd.Params(), jd.Params(threshold=64, floor=64), jd.Params(threshold=64, floor=300), jd.Params(factor='1/3', threshold=0, floor=1)):
            last = 0
            for x in range(1, 9000):
                y = jd.scale(x, p)
                self.assertLessEqual(y, x)
                self.assertGreaterEqual(y, min(x, p.floor))
                self.assertGreaterEqual(y, last, (x, p.describe()))     # monotone: an Active stays above its Inactive
                last = y

    def test_no_limit_stays_and_the_rest_of_the_entry_is_scaled(self):
        got, rep = jd.derive(make())
        minus_one = sum(1 for _, e in jd.entries(make()) for k, v in e.items() if jd.LIMIT_KEY.search(k) and v == -1)
        self.assertEqual((minus_one, rep.unlimited), (7, 7))
        e = got['Version4']['Daemon']['Override']['com.example.unlimited']
        self.assertEqual((e['ActiveSoftMemoryLimit'], e['InactiveSoftMemoryLimit']), (-1, -1))
        self.assertEqual((e['ActiveHardMemoryLimit'], e['InactiveHardMemoryLimit']), (819, 819))
        b = got['Version4']['Daemon']['Override']['com.apple.backboardd']
        self.assertEqual(b, make()['Version4']['Daemon']['Override']['com.apple.backboardd'])
        self.assertEqual(got['Version4']['Extension']['Override']['com.example.poster']['PosterProvider_EditingSoftMemoryLimit'], -1)
        self.assertEqual(got['Version4']['Daemon']['Override']['com.example.nolimit'], make()['Version4']['Daemon']['Override']['com.example.nolimit'])

    def test_the_ui_and_core_entries_stay(self):
        got, rep = jd.derive(make())
        d = got['Version4']['Daemon']['Override']
        self.assertEqual(d['com.apple.SpringBoard'], make()['Version4']['Daemon']['Override']['com.apple.SpringBoard'])
        self.assertEqual(d['com.apple.SwitchBoard']['ActiveSoftMemoryLimit'], 600)
        self.assertEqual(got['Version4']['SystemXPCService']['Override']['com.apple.springboard.SBRendererService']['ActiveSoftMemoryLimit'], 400)
        self.assertEqual(got['Version4']['Extension']['Override']['com.apple.posterkit.provider']['PosterProvider_Rendering_PhotosSoftMemoryLimit'], 350)
        # by RecoverOnCrash, not by name; False is not protected
        self.assertEqual(d['com.example.crashy']['ActiveSoftMemoryLimit'], 900)
        self.assertEqual(d['com.example.notcrashy']['ActiveSoftMemoryLimit'], 360)
        self.assertIn('com.example.crashy', rep.crash_seen)
        self.assertEqual(rep.kept_seen['com.apple.SpringBoard'], 1)

    def test_the_two_raised_ones(self):
        got, rep = jd.derive(make())
        i = got['Version4']['Daemon']['Override']['com.apple.iconservices.iconservicesagent']
        m = got['Version4']['SystemXPCService']['Override']['com.apple.MapKit.SnapshotService']
        self.assertEqual((i['ActiveSoftMemoryLimit'], i['InactiveHardMemoryLimit'], i['JetsamPriority']), (450, 64, 140))
        self.assertEqual((m['ActiveSoftMemoryLimit'], m['InactiveHardMemoryLimit']), (200, 64))
        self.assertEqual(sorted((c.where, c.old, c.new) for c in rep.changes if c.why == 'raised'),
                         [('Daemon/com.apple.iconservices.iconservicesagent', 15, 64), ('SystemXPCService/com.apple.MapKit.SnapshotService', 14, 64)])
        # it only raises: a limit that is enough stays, a protected one is raised too
        got, rep = jd.derive(make(), jd.Params(raises={'com.apple.iconservices.iconservicesagent': 10, 'com.apple.SwitchBoard': 700}))
        self.assertEqual(got['Version4']['Daemon']['Override']['com.apple.iconservices.iconservicesagent']['InactiveHardMemoryLimit'], 15)
        self.assertEqual(got['Version4']['Daemon']['Override']['com.apple.SwitchBoard'], {'ActiveSoftMemoryLimit': 600, 'InactiveHardMemoryLimit': 700})
        self.assertEqual(jd.check(make(), got, jd.Params(raises={'com.apple.iconservices.iconservicesagent': 10, 'com.apple.SwitchBoard': 700})), [])

    def test_only_memory_limits_change(self):
        got, rep = jd.derive(make())
        for path, new in jd.entries(got):
            old = dict(jd.entries(make()))[path]
            for key in old:
                if jd.LIMIT_KEY.search(key):
                    continue
                self.assertTrue(jd.identical(old[key], new[key]), (path, key))
        self.assertEqual({c.key for c in rep.changes} - {k for _, e in jd.entries(make()) for k in e if jd.LIMIT_KEY.search(k)}, set())
        # the counts, rates, bands, flags and the System keys, by name
        c = got['Version4']['Daemon']['Override']['com.example.counts']
        self.assertEqual({k: v for k, v in c.items() if k != 'ActiveSoftMemoryLimit'},
                         {k: v for k, v in make()['Version4']['Daemon']['Override']['com.example.counts'].items() if k != 'ActiveSoftMemoryLimit'})
        self.assertEqual(got['Version4']['System'], make()['Version4']['System'])
        self.assertEqual(got['Version4']['Daemon']['Override']['com.example.big']['JetsamPriority'], 140)
        self.assertEqual(got['Version4']['Daemon']['Override']['com.example.big']['EnergyEfficiencyMode'], 'Efficient')

    def test_a_limit_that_is_not_a_whole_number_is_left_and_said(self):
        got, rep = jd.derive(make())
        o = got['Version4']['Daemon']['Override']['com.example.odd']
        self.assertIs(o['ActiveSoftMemoryLimit'], True)
        self.assertEqual(o['InactiveHardMemoryLimit'], 800.0)
        self.assertIsInstance(o['InactiveHardMemoryLimit'], float)
        self.assertEqual(sorted((p[3], k) for p, k, _ in rep.odd), [('com.example.odd', 'ActiveSoftMemoryLimit'), ('com.example.odd', 'InactiveHardMemoryLimit')])

    def test_labels_that_are_not_there(self):
        plist = make()
        del plist['Version4']['Daemon']['Override']['com.apple.SwitchBoard']
        with self.assertRaises(jd.LabelError) as cm:
            jd.derive(plist)
        self.assertIn('com.apple.SwitchBoard', str(cm.exception))
        got, rep = jd.derive(plist, strict=False)
        self.assertEqual(len(rep.problems), 1)
        self.assertEqual(jd.check(plist, got), [])
        plist = make()
        del plist['Version4']['Daemon']['Override']['com.apple.iconservices.iconservicesagent']['InactiveHardMemoryLimit']
        with self.assertRaises(jd.LabelError) as cm:
            jd.derive(plist)
        self.assertIn('iconservicesagent', str(cm.exception))
        with self.assertRaises(jd.LabelError):
            jd.derive(make(), jd.Params(keep={'com.apple.nothing': 'a typo'}))

    def test_the_options_are_checked(self):
        for bad in (dict(factor=0), dict(factor='1.5'), dict(factor='-1/3'), dict(threshold=100, floor=64), dict(floor=-1), dict(floor=0),
                    dict(threshold=1.5), dict(raises={'a': 0}), dict(raises={'a': True})):
            with self.assertRaises(ValueError, msg=str(bad)):
                jd.Params(**bad)
        jd.Params(factor=1, threshold=0, floor=1)
        jd.Params(factor='2/5', threshold=64, floor=300)

    def test_the_other_factor(self):
        got, _ = jd.derive(make(), jd.Params(factor='0.5'))
        d = got['Version4']['Daemon']['Override']
        self.assertEqual((d['com.example.big']['ActiveSoftMemoryLimit'], d['com.example.big']['InactiveHardMemoryLimit']), (500, 250))
        self.assertEqual(jd.derive(make(), jd.Params(factor=1))[0]['Version4']['Daemon']['Override']['com.example.big'],
                         dict(make()['Version4']['Daemon']['Override']['com.example.big'], InactiveHardMemoryLimit=500))


class Output(unittest.TestCase):
    def test_the_same_bytes_every_time_and_whatever_the_order(self):
        a = jd.dumps(jd.derive(make())[0])
        self.assertEqual(a, jd.dumps(jd.derive(make())[0]))
        self.assertTrue(a.startswith(b'bplist00'))

        def reverse(x):
            return {k: reverse(x[k]) for k in reversed(list(x))} if isinstance(x, dict) else x
        self.assertEqual(a, jd.dumps(jd.derive(reverse(make()))[0]))
        self.assertEqual(jd.dumps(jd.derive(make())[0], 'xml'), jd.dumps(jd.derive(reverse(make()))[0], 'xml'))

    def test_what_is_written_reads_back_the_same_with_the_same_types(self):
        got, _ = jd.derive(make())
        for fmt in ('binary', 'xml'):
            back = plistlib.loads(jd.dumps(got, fmt))
            self.assertTrue(jd.identical(back, got), fmt)
        self.assertIs(plistlib.loads(jd.dumps(got))['Version4']['Daemon']['Override']['com.example.nolimit']['DisableAllowed'], False)

    def test_the_summary_names_every_changed_entry_with_old_and_new(self):
        got, rep = jd.derive(make())
        text = jd.summary(rep, jd.Params(), 'in.plist', 'out.plist', 123, 'binary')
        self.assertIn('Daemon/com.apple.iconservices.iconservicesagent: ActiveSoftMemoryLimit 1124 -> 450; InactiveHardMemoryLimit 15 -> 64 (raised)', text)
        self.assertIn('App/Global: Games_EntitledHardMemoryLimit 8192 -> 3277', text)
        self.assertIn('Daemon/com.example.mid: ActiveSoftMemoryLimit 300 -> 200 (floor); InactiveHardMemoryLimit 201 -> 200 (floor)', text)
        for path in {c.path for c in rep.changes}:
            self.assertIn(path[3], text)
        self.assertNotIn('com.apple.backboardd:', text.split('every changed entry')[1])
        short = jd.summary(rep, jd.Params(), full=False)
        self.assertNotIn('every changed entry', short)
        self.assertIn('the 15 biggest changes', short)


class Check(unittest.TestCase):
    """check() finds what a wrong result would look like."""

    def derived(self):
        return jd.derive(make())[0]

    def test_a_change_of_anything_else_is_found(self):
        def tweak(f):
            d = self.derived()
            f(d['Version4'])
            return jd.check(make(), d)
        daemon = lambda v: v['Daemon']['Override']
        self.assertTrue(tweak(lambda v: daemon(v)['com.example.big'].update(JetsamPriority=150)))
        self.assertTrue(tweak(lambda v: daemon(v)['com.example.counts'].update(ThreadLimit=100)))
        self.assertTrue(tweak(lambda v: v['System']['Override']['Global'].update(KernelHighWaterMark=30)))
        self.assertTrue(tweak(lambda v: daemon(v)['com.example.nolimit'].update(DisableAllowed=True)))
        self.assertTrue(tweak(lambda v: daemon(v)['com.example.big'].update(NewKey=1)))
        self.assertTrue(tweak(lambda v: daemon(v).pop('com.example.small')))
        self.assertTrue(tweak(lambda v: daemon(v).update({'com.example.new': {}})))
        self.assertTrue(tweak(lambda v: v.pop('Angel')))

    def test_a_limit_that_breaks_a_rule_is_found(self):
        def tweak(f):
            d = self.derived()
            f(d['Version4']['Daemon']['Override'])
            return jd.check(make(), d)
        self.assertTrue(tweak(lambda d: d['com.example.unlimited'].update(ActiveSoftMemoryLimit=500)))          # -1 became a limit
        self.assertTrue(tweak(lambda d: d['com.example.small'].update(InactiveHardMemoryLimit=5)))              # under the threshold
        self.assertTrue(tweak(lambda d: d['com.example.big'].update(ActiveSoftMemoryLimit=399)))                # not the rule's value
        self.assertTrue(tweak(lambda d: d['com.example.big'].update(ActiveSoftMemoryLimit=1000)))               # not scaled
        self.assertTrue(tweak(lambda d: d['com.apple.SpringBoard'].update(ActiveSoftMemoryLimit=220)))          # protected
        self.assertTrue(tweak(lambda d: d['com.example.crashy'].update(ActiveSoftMemoryLimit=360)))             # RecoverOnCrash
        self.assertTrue(tweak(lambda d: d['com.apple.iconservices.iconservicesagent'].update(InactiveHardMemoryLimit=15)))   # not raised
        self.assertTrue(tweak(lambda d: d['com.apple.iconservices.iconservicesagent'].update(InactiveHardMemoryLimit=100)))
        self.assertTrue(tweak(lambda d: d['com.example.odd'].update(ActiveSoftMemoryLimit=1)))                  # a bool is no limit
        self.assertTrue(tweak(lambda d: d['com.example.big'].update(ActiveSoftMemoryLimit=400.0)))              # the type changed

    def test_the_result_is_fine_and_a_list_that_was_not_scaled_is_not(self):
        self.assertEqual(jd.check(make(), self.derived()), [])
        left = jd.check(make(), make())
        self.assertIn('Daemon/Override/com.example.big ActiveSoftMemoryLimit: 1000 -> 1000, the rules give 400', left)
        self.assertIn('Daemon/Override/com.apple.iconservices.iconservicesagent InactiveHardMemoryLimit: 15 -> 15, the rules give 64', left)
        self.assertFalse([line for line in left if 'SpringBoard' in line or 'backboardd' in line or 'small' in line])

    def test_identical_tells_types_apart(self):
        self.assertFalse(jd.identical({'a': True}, {'a': 1}))
        self.assertFalse(jd.identical({'a': 1}, {'a': 1.0}))
        self.assertFalse(jd.identical([1, 2], [1]))
        self.assertTrue(jd.identical({'a': [1, {'b': False}]}, {'a': [1, {'b': False}]}))


class Command(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.top = self.tmp.name
        self.source = os.path.join(self.top, 'V54.plist')
        write(self.source, plistlib.dumps(make(), fmt=plistlib.FMT_BINARY))

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                code = jd.main(list(args))
            except SystemExit as e:        # argparse's own exit
                code = e.code
        return code, out.getvalue(), err.getvalue()

    def test_out_dir_writes_the_named_file(self):
        code, out, err = self.run_main(self.source, '--out-dir', os.path.join(self.top, 'new', 'dir'))
        self.assertEqual((code, err), (0, ''))
        path = os.path.join(self.top, 'new', 'dir', 'com.apple.jetsamproperties.V54O.plist')
        self.assertTrue(jd.identical(plistlib.loads(read(path)), want()))
        self.assertIn('Games_EntitledHardMemoryLimit 8192 -> 3277', out)
        self.assertIn(path, out)
        code, out, _ = self.run_main(self.source, '--out-dir', self.top, '--name', 'Z9.test_1')
        self.assertTrue(os.path.exists(os.path.join(self.top, 'com.apple.jetsamproperties.Z9.test_1.plist')))

    def test_a_second_run_gives_the_same_bytes(self):
        a, b = os.path.join(self.top, 'a.plist'), os.path.join(self.top, 'b.plist')
        self.run_main(self.source, '-o', a)
        self.run_main(self.source, '-o', b, '-q')
        self.assertEqual(read(a), read(b))
        self.assertEqual(read(a), jd.dumps(want()))

    def test_without_an_output_it_writes_nothing(self):
        code, out, _ = self.run_main(self.source)
        self.assertEqual(code, 0)
        self.assertIn('none written', out)
        self.assertEqual(os.listdir(self.top), ['V54.plist'])

    def test_summary_file_and_quiet(self):
        s = os.path.join(self.top, 'summary.txt')
        code, out, _ = self.run_main(self.source, '--summary', s)
        self.assertEqual(code, 0)
        self.assertIn('every changed entry', read(s, 'r'))
        self.assertNotIn('every changed entry', out)
        code, out, _ = self.run_main(self.source, '-q')
        self.assertNotIn('every changed entry', out)

    def test_xml(self):
        x = os.path.join(self.top, 'x.plist')
        self.assertEqual(self.run_main(self.source, '-o', x, '--format', 'xml')[0], 0)
        self.assertTrue(read(x).startswith(b'<?xml'))
        self.assertEqual(self.run_main(self.source, '--verify', x)[0], 0)

    def test_verify(self):
        good = os.path.join(self.top, 'good.plist')
        self.run_main(self.source, '-o', good)
        code, out, _ = self.run_main(self.source, '--verify', good)
        self.assertEqual(code, 0)
        self.assertIn('is what these options make', out)
        self.assertEqual(self.run_main(self.source, '--verify', self.source)[0], 1)              # the original is not it
        self.assertEqual(self.run_main(self.source, '--verify', good, '--factor', '1/2')[0], 1)  # nor with another factor
        bad = want()
        bad['Version4']['Daemon']['Override']['com.example.big']['JetsamPriority'] = 1
        path = os.path.join(self.top, 'bad.plist')
        write(path, plistlib.dumps(bad))
        code, out, _ = self.run_main(self.source, '--verify', path)
        self.assertEqual(code, 1)
        self.assertIn('not a memory limit', out)
        self.assertEqual(self.run_main(self.source, '--verify', os.path.join(self.top, 'missing'))[0], 2)

    def test_options(self):
        out = os.path.join(self.top, 'o.plist')
        code, text, _ = self.run_main(self.source, '-o', out, '--factor', '1/2', '--threshold', '64', '--floor', '64',
                                      '--keep', 'com.example.big', '--raise', 'com.example.small=70')
        self.assertEqual(code, 0, text)
        got = plistlib.loads(read(out))['Version4']['Daemon']['Override']
        self.assertEqual(got['com.example.big'], make()['Version4']['Daemon']['Override']['com.example.big'])      # kept
        self.assertEqual(got['com.example.mid'], {'ActiveSoftMemoryLimit': 150, 'InactiveHardMemoryLimit': 101})    # 300/2; 201/2 = 100.5
        self.assertEqual(got['com.example.small']['InactiveHardMemoryLimit'], 70)

    def test_refusals(self):
        code, _, err = self.run_main(self.source, '--name', '.hidden')
        self.assertEqual(code, 2)
        self.assertIn('--name', err)
        for name in ('', 'a' * 32, 'a/b', 'a b', 'V54O\n'):
            self.assertEqual(self.run_main(self.source, '--name', name)[0], 2, repr(name))
        self.assertEqual(self.run_main(self.source, '--name', 'a' * 31)[0], 0)
        self.assertEqual(self.run_main(self.source, '--factor', 'x')[0], 2)
        self.assertEqual(self.run_main(self.source, '--factor', '0')[0], 2)
        self.assertEqual(self.run_main(self.source, '--factor', '1.2')[0], 2)
        self.assertEqual(self.run_main(self.source, '--floor', '10')[0], 2)         # under the default threshold
        self.assertEqual(self.run_main(self.source, '--raise', 'noequals')[0], 2)
        self.assertEqual(self.run_main(self.source, '-o', 'a', '--out-dir', 'b')[0], 2)
        self.assertEqual(self.run_main(os.path.join(self.top, 'missing.plist'))[0], 2)
        notlist = os.path.join(self.top, 'notlist.plist')
        write(notlist, plistlib.dumps({'Other': {}}))
        self.assertEqual(self.run_main(notlist)[0], 2)
        junk = os.path.join(self.top, 'junk.plist')
        write(junk, b'not a plist')
        code, _, err = self.run_main(junk)
        self.assertEqual(code, 2)
        self.assertIn('junk.plist', err)

    def test_a_label_the_list_lacks_is_exit_3(self):
        plist = make()
        del plist['Version4']['Daemon']['Override']['com.apple.SwitchBoard']
        path = os.path.join(self.top, 'lacks.plist')
        write(path, plistlib.dumps(plist))
        out = os.path.join(self.top, 'lacks-out.plist')
        code, _, err = self.run_main(path, '-o', out)
        self.assertEqual(code, 3)
        self.assertIn('com.apple.SwitchBoard', err)
        self.assertFalse(os.path.exists(out))
        code, text, _ = self.run_main(path, '-o', out, '--lenient')
        self.assertEqual(code, 0)
        self.assertIn('warning: protected label com.apple.SwitchBoard is not in the list', text)
        self.assertEqual(self.run_main(self.source, '--keep', 'com.example.nothere')[0], 3)


@unittest.skipUnless(os.environ.get('ONEIOS_V54_PLIST'), 'ONEIOS_V54_PLIST is not set (the OS image\'s V54 list, read-only)')
class RealList(unittest.TestCase):
    """The rules on the image's own list, with arithmetic of this file's own (the code's helpers are not used for it)."""

    @classmethod
    def setUpClass(cls):
        cls.original = plistlib.loads(read(os.environ['ONEIOS_V54_PLIST']))
        cls.derived, cls.report = jd.derive(cls.original)

    def test_same_entries_same_keys_same_types_apart_from_the_limits(self):
        self.assertEqual(jd.check(self.original, self.derived), [])
        a, b = dict(jd.entries(self.original)), dict(jd.entries(self.derived))
        self.assertEqual(list(a), list(b))
        self.assertEqual({(p, k) for p, e in a.items() for k in e}, {(p, k) for p, e in b.items() for k in e})
        self.assertEqual(self.derived.keys(), self.original.keys())

    def test_every_difference_is_a_limit_and_follows_the_rule(self):
        limit = re.compile(r'(Hard|Soft)MemoryLimit$')
        a, b = dict(jd.entries(self.original)), dict(jd.entries(self.derived))
        raised = {'com.apple.iconservices.iconservicesagent', 'com.apple.MapKit.SnapshotService'}
        kept = set(jd.PROTECTED)
        differing = 0
        for path, old_entry in a.items():
            label = path[3]
            for key, old in old_entry.items():
                new = b[path][key]
                type_ok = type(old) is type(new)
                self.assertTrue(type_ok, (path, key))
                if old == new:
                    continue
                differing += 1
                self.assertTrue(limit.search(key), (path, key, old, new))
                self.assertIs(type(old), int)
                self.assertGreater(old, 0, 'a limit of -1 changed')
                self.assertNotIn(label, kept)
                self.assertIsNot(old_entry.get('RecoverOnCrash'), True)
                if label in raised and key == 'InactiveHardMemoryLimit':
                    self.assertEqual(new, 64)
                    continue
                self.assertGreater(old, 200, (path, key))
                self.assertEqual(new, min(old, max(200, (4 * old + 5) // 10)), (path, key, old, new))   # 0.4, halves up, floor 200
        self.assertEqual(differing, len(self.report.changes))

    def test_minus_one_is_intact_and_the_protected_are_whole(self):
        a, b = dict(jd.entries(self.original)), dict(jd.entries(self.derived))
        for path, entry in a.items():
            for key, old in entry.items():
                if type(old) is int and old <= 0:
                    self.assertEqual(b[path][key], old, (path, key))
            if path[3] in jd.PROTECTED or entry.get('RecoverOnCrash') is True:
                self.assertEqual(b[path], entry, path)
        self.assertEqual(b[('Version4', 'Daemon', 'Override', 'com.apple.backboardd')], a[('Version4', 'Daemon', 'Override', 'com.apple.backboardd')])
        self.assertEqual(b[('Version4', 'Daemon', 'Override', 'com.apple.SpringBoard')]['ActiveSoftMemoryLimit'], 550)
        self.assertEqual(self.derived['Version4']['System'], self.original['Version4']['System'])

    def test_the_two_raised_and_the_big_ones(self):
        v = self.derived['Version4']
        self.assertEqual(v['Daemon']['Override']['com.apple.iconservices.iconservicesagent']['InactiveHardMemoryLimit'], 64)
        self.assertEqual(v['SystemXPCService']['Override']['com.apple.MapKit.SnapshotService']['InactiveHardMemoryLimit'], 64)
        d = v['Daemon']['Override']
        self.assertEqual((d['com.apple.cameracaptured']['ActiveSoftMemoryLimit'], d['com.apple.videocodecd']['ActiveSoftMemoryLimit'],
                          d['com.apple.mediaplaybackd']['ActiveSoftMemoryLimit']), (1724, 1224, 920))
        self.assertEqual(v['XPCService']['Override']['com.apple.WebKit.WebContent']['ActiveHardMemoryLimit'], 819)
        self.assertEqual(v['App']['Override']['Global']['Games_EntitledHardMemoryLimit'], 3277)

    def test_two_runs_the_same_bytes(self):
        self.assertEqual(jd.dumps(jd.derive(self.original)[0]), jd.dumps(self.derived))
        self.assertEqual(jd.dumps(self.derived), jd.dumps(plistlib.loads(jd.dumps(self.derived))))


if __name__ == '__main__':
    unittest.main()
