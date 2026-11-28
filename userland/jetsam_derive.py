#!/usr/bin/env python3
"""jetsam_derive: a jetsam properties list sized for a smaller RAM, derived from the OS image's own.

    python3 -I userland/jetsam_derive.py V54.plist --out-dir DIR           # -> DIR/com.apple.jetsamproperties.V54O.plist
    python3 -I userland/jetsam_derive.py V54.plist -o FILE --factor 0.375  # another factor, another file name
    python3 -I userland/jetsam_derive.py V54.plist --verify V54O.plist     # is that file what these options make?

V54.plist is the file the OS image carries (24A446: `oneios-tool image apfs-copy os.img
/System/Library/LaunchDaemons/com.apple.jetsamproperties.V54.plist V54.plist`; the image is only read). Its limits are
made for an iPhone with 12 GB; the native VM has 4352-4608 MiB. The output is the same list with the memory limits
made smaller, to be named with `xnuloader-crosvm --jetsam-product V54O` (docs/native.md, "A smaller list for this VM"). It
does nothing until launchd can find the file: no os.img of the image set has it, and no other way of putting it under
/System/Library/LaunchDaemons has been run (the same section says what exists). The binary list is written by Python's
plistlib, which launchd has not read yet (2358 objects, where Apple's file has 5096: plistlib writes each equal string and
number once); `--format xml` is the fallback.

Standard library only (written for Python 3.11 or later; run on 3.14.5 only). Deterministic: the same input and options
give the same bytes (a binary property list with sorted keys), on one interpreter. Nothing here contains a file of Apple's; the tests build their list from nothing.

The file is `Version4 > CATEGORY > Override|Default > LABEL > KEY` (CATEGORY: Daemon, XPCService, Extension, App,
SystemApp, Angel, ...; the LABEL `Global` is a category's default). The keys, by what they are:

  limits, in MB      `ActiveHardMemoryLimit`, `ActiveSoftMemoryLimit`, `InactiveHardMemoryLimit`,
                     `InactiveSoftMemoryLimit` and the named ones of the same shape (`Games_EntitledHardMemoryLimit`,
                     `PosterProvider_*SoftMemoryLimit`, `Siri*SoftMemoryLimit`, `WidgetRenderer_*SoftMemoryLimit`,
                     `ExternalExtension_*HardMemoryLimit`): every key that ends in `HardMemoryLimit` or
                     `SoftMemoryLimit`. Hard: the kernel kills the process at the limit (per-process-limit). Soft: the
                     process is a first choice when memory runs short (highwater). -1 is no limit.
  bands              `JetsamPriority` (backboardd 170, SpringBoard 160, ...). Never changed.
  not memory in MB   `ThreadLimit`, `HardPortLimit`, `SoftPortLimit`, `LogicalWritesLimit`, `OSLogRateLimit`,
                     `MallocMaxMagazines`: counts and rates. In `System/Global`: `KernelHighWaterMark` 78 and
                     `UserReclaimableLimit` 62 (their meaning was not read, docs/native.md; the sizes look like
                     percentages) and `DarkBootSystemUIHardLimit` 250 (SystemUI's, named `...HardLimit`). And
                     `ConclaveMemoryLimit` (5-280; the exclave side, which this machine has not; its unit was not
                     read). None is changed.
  the rest           `RecoverOnCrash`, `DisableAllowed`, `EnergyEfficiencyMode`, `FreezerEligible`, `Malloc*`, ... Never changed.

The rules for a limit (the defaults are what the options name):

  1. -1 (or any value of 0 or less) is no limit: it stays. An entry with no limit key stays.
  2. At or below THRESHOLD (200 MB, about 5 % of the VM's RAM): it stays. A process capped there cannot sink the guest
     by itself, so a lower cap buys little; and a hard cap is a kill, in a VM whose footprints carry what a phone keeps
     out of a process (docs/native.md: GPU blobs; the last session's kills hit limits of 15 and 14 MB with footprints of
     15-46 and 16-20 MiB). 82 % of the limits are there (1433 of 1733 in 24A446's), among them, by the names of their
     extension points, those of third parties' keyboards (`com.apple.keyboard-service` 177), Safari web extensions (180)
     and notification service extensions (165). Not read: whether Apple's lists for phones with less RAM lower these
     tiers (the image has only V54).
  3. Above it: FACTOR times the limit (0.4; the VM's 4352-4608 MiB against the phone's 12 GiB are 0.354-0.375, so 0.4 is
     the next tenth up), rounded to a whole MB with halves up (exact arithmetic, no float rounding), but never below FLOOR
     (200 MB) and never above the old value. So 201-500 MB become 200, and above that it is 0.4 times (2048 -> 819).
     FLOOR >= THRESHOLD is required: then a larger limit never becomes smaller than one that was smaller, and an
     entry's Active limit stays at least its Inactive one when it was.
  4. The limits of a protected entry stay whole: the display chain and the UI (PROTECTED below), and every entry with
     `RecoverOnCrash` (backboardd, SpringBoard, runningboardd, configd: their crash restarts the guest,
     docs/native.md).
  5. RAISED: the `InactiveHardMemoryLimit` of the two processes the last session's per-process-limit kills hit is
     at least 64 MB (`max(old, 64)`; their Active limits follow rule 3). iconservicesagent had 15 MB and footprints of
     15-46 MiB (6 kills), MapKit's snapshot service 14 MB and 16-20 MiB (3 kills). 48 MB would leave 2 MiB over the
     largest footprint seen, 64 leaves 18.

`--threshold 64 --floor 64` is the stricter variant: every limit above 64 MB is cut (601 keys instead of 285 in 24A446's).

Exit status: 0 done (or --verify: the same); 1 --verify: not the same; 2 a bad option or input; 3 a protected or raised
label that the list does not have (not the list these rules were written for; --lenient makes it a warning); 4 the
result broke a rule (a bug).
"""
import argparse
import copy
import os
import plistlib
import re
import sys
from fractions import Fraction

DEFAULT_NAME = 'V54O'
DEFAULT_FACTOR = Fraction(2, 5)
DEFAULT_THRESHOLD = 200
DEFAULT_FLOOR = 200

LIMIT_KEY = re.compile(r'(Hard|Soft)MemoryLimit$')
RAISE_KEY = 'InactiveHardMemoryLimit'
# xnuloader-crosvm --jetsam-product: 1 to 31 of A-Z a-z 0-9 . _ -, not starting with a dot (xl_machine_jetsam_product_ok)
NAME_OK = re.compile(r'[A-Za-z0-9_-][A-Za-z0-9._-]{0,30}\Z')

# label -> why its limits stay (rule 4). In any category: a label of two categories is protected in both. Apart from
# backboardd and SpringBoard (RecoverOnCrash) the choice is by name: SBRendererService, PosterBoard and
# WidgetRenderer-Default are in the repo's Metal client list (userland/roottweak/tweaks.py), the rest are SpringBoard's
# neighbours by their names only.
PROTECTED = {
    'com.apple.backboardd': 'the display server (limits -1, RecoverOnCrash)',
    'com.apple.SpringBoard': 'the home screen (RecoverOnCrash)',
    'com.apple.SwitchBoard': 'SpringBoard family (band 160)',
    'com.apple.springboardservices': 'SpringBoard family',
    'com.apple.springboard.SBRendererService': 'draws SpringBoard\'s surfaces, a GPU client',
    'com.apple.PosterBoard': 'lock screen and wallpaper renderer, a GPU client',
    'com.apple.posterkit.provider': 'the wallpaper providers\' extension point (the PosterProvider_* limits)',
    'com.apple.chrono.WidgetRenderer-Default': 'draws the widgets, a GPU client',
    'com.apple.chrono.WidgetRenderer-StandBy': 'draws the widgets (StandBy)',
}
# label -> the least MB its InactiveHardMemoryLimit gets (rule 5)
RAISED = {
    'com.apple.iconservices.iconservicesagent': 64,
    'com.apple.MapKit.SnapshotService': 64,
}


class LabelError(Exception):
    """A protected or raised label that the list does not have."""


class Params:
    def __init__(self, factor=DEFAULT_FACTOR, threshold=DEFAULT_THRESHOLD, floor=DEFAULT_FLOOR, keep=None, raises=None):
        self.factor = Fraction(factor)
        self.threshold = threshold
        self.floor = floor
        self.keep = dict(PROTECTED if keep is None else keep)
        self.raises = dict(RAISED if raises is None else raises)
        if not 0 < self.factor <= 1:
            raise ValueError(f'factor {self.factor} is not above 0 and at most 1')
        if type(threshold) is not int or type(floor) is not int or threshold < 0 or floor < 1:
            raise ValueError('threshold is a whole number of MB, 0 or more; floor is one, 1 or more (a limit of 0 would be no limit)')
        if floor < threshold:
            raise ValueError(f'floor {floor} is under threshold {threshold}: a limit just above the threshold would end '
                             f'under one that was left alone')
        for label, mb in self.raises.items():
            if type(mb) is not int or mb <= 0:
                raise ValueError(f'{label}: {mb!r} is not a whole number of MB above 0')

    def describe(self):
        return (f'factor {self.factor} ({float(self.factor):g}), threshold {self.threshold} MB, floor {self.floor} MB, '
                f'{len(self.keep)} protected labels and the RecoverOnCrash entries, {len(self.raises)} raised')


def is_mb(value):
    """A limit's value: a whole number (a bool is an int in Python, and is no limit)."""
    return type(value) is int


def rounded(old, factor):
    """old * factor in whole MB, halves up: floor(old * factor + 1/2), in integers."""
    return (2 * old * factor.numerator + factor.denominator) // (2 * factor.denominator)


def scale(old, params):
    """The limit that `old` MB (above 0) becomes under rules 2 and 3."""
    if old <= params.threshold:
        return old
    return min(old, max(params.floor, rounded(old, params.factor)))


def entries(plist):
    """((version, category, section, label), entry) for every label entry of the list, in file order."""
    for version, body in plist.items():
        if not isinstance(body, dict):
            continue
        for category, cat in body.items():
            if not isinstance(cat, dict):
                continue
            for section, table in cat.items():
                if not isinstance(table, dict):
                    continue
                for label, entry in table.items():
                    if isinstance(entry, dict):
                        yield (version, category, section, label), entry


def protected_why(label, entry, params):
    """Why this entry's limits stay (rule 4), or None."""
    if label in params.keep:
        return params.keep[label]
    if entry.get('RecoverOnCrash') is True:
        return 'RecoverOnCrash: its crash restarts the guest'
    return None


class Change:
    __slots__ = ('path', 'key', 'old', 'new', 'why')

    def __init__(self, path, key, old, new, why):
        self.path, self.key, self.old, self.new, self.why = path, key, old, new, why

    @property
    def where(self):
        _, category, section, label = self.path
        return f'{category}/{label}' + ('' if section == 'Override' else f' [{section}]')


class Report:
    def __init__(self):
        self.changes = []
        self.entries = 0           # label entries
        self.with_limits = 0       # of them, those with a limit key
        self.unlimited = 0         # limit keys of -1 (or 0 or less): left
        self.small = 0             # limit keys at or below the threshold: left
        self.protected_keys = 0    # limit keys of protected entries: left
        self.kept_seen = {}        # protected label -> its number of entries
        self.crash_seen = set()    # labels protected by RecoverOnCrash alone
        self.raise_seen = {}       # raised label -> its number of RAISE_KEY keys
        self.odd = []              # (path, key, value) of limit keys that are not whole numbers: left
        self.problems = []         # protected or raised labels that the list lacks

    def changed_entries(self):
        return len({c.path for c in self.changes})


def derive(plist, params=None, strict=True):
    """(new list, Report). `plist` is not changed. With `strict`, a protected or raised label that the list does not
    have raises LabelError once the whole list is seen (the message names them all)."""
    params = params or Params()
    out = copy.deepcopy(plist)
    rep = Report()
    for path, entry in entries(out):
        label = path[3]
        rep.entries += 1
        keys = sorted(k for k in entry if LIMIT_KEY.search(k))
        rep.with_limits += bool(keys)
        why = protected_why(label, entry, params)
        if label in params.keep:
            rep.kept_seen[label] = rep.kept_seen.get(label, 0) + 1
        elif why:
            rep.crash_seen.add(label)
        for key in keys:
            old = entry[key]
            if not is_mb(old):
                rep.odd.append((path, key, old))
                continue
            if old <= 0:
                rep.unlimited += 1
                continue
            new, reason = old, None
            if not why and old > params.threshold:
                new = scale(old, params)
                reason = 'floor' if new == params.floor and rounded(old, params.factor) < params.floor else 'scaled'
            if key == RAISE_KEY and label in params.raises:
                rep.raise_seen[label] = rep.raise_seen.get(label, 0) + 1
                if params.raises[label] > new:
                    new, reason = params.raises[label], 'raised'
            if new != old:
                entry[key] = new
                rep.changes.append(Change(path, key, old, new, reason))
            elif why:
                rep.protected_keys += 1
            else:
                rep.small += 1      # at or below the threshold (or, with a floor above it, held by the floor)
    for label in params.keep:
        if label not in rep.kept_seen:
            rep.problems.append(f'protected label {label} is not in the list')
    for label in params.raises:
        if label not in rep.raise_seen:
            rep.problems.append(f'raised label {label} has no {RAISE_KEY} in the list')
    if strict and rep.problems:
        raise LabelError('; '.join(rep.problems))
    return out, rep


def identical(a, b):
    """Equal, and of the same types all the way down (True == 1 == 1.0 in Python, in a plist they differ)."""
    if type(a) is not type(b):
        return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(identical(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(identical(x, y) for x, y in zip(a, b))
    return a == b


def check(original, derived, params=None):
    """The rules, stated again from the two lists: what is broken (a sorted list; empty when all hold). Every limit above 0
    must be what rules 1 to 5 give; with the limits put back, the derived list must be the original, type for type."""
    params = params or Params()
    bad = []
    old_entries = dict(entries(original))
    undone = copy.deepcopy(derived)
    for path, entry in entries(undone):
        old_entry = old_entries.get(path)
        if old_entry is None:
            continue
        label = path[3]
        why = protected_why(label, old_entry, params)
        for key in list(entry):
            if key not in old_entry:
                continue
            old, new = old_entry[key], entry[key]
            where = f'{"/".join(path[1:])} {key}'
            if LIMIT_KEY.search(key) and is_mb(old) and old > 0:
                base = old if why or old <= params.threshold else scale(old, params)
                want = max(base, params.raises[label]) if key == RAISE_KEY and label in params.raises else base
                if not is_mb(new) or new != want:
                    bad.append(f'{where}: {old} -> {new!r}, the rules give {want}' + (f' (protected: {why})' if why else ''))
                entry[key] = old                          # put back what the rules may change
            elif not identical(old, new):
                bad.append(f'{where}: {old!r} -> {new!r}, ' + ('not a memory limit' if not LIMIT_KEY.search(key) else
                           'no limit or not a whole number: it stays'))
    if not identical(undone, original):
        bad.append('apart from the limits, the two lists differ (an entry or a key added, removed or changed)')
    return sorted(set(bad))


def dumps(plist, fmt='binary'):
    """The bytes of a list; sorted keys, so the same list is always the same bytes."""
    return plistlib.dumps(plist, fmt=plistlib.FMT_BINARY if fmt == 'binary' else plistlib.FMT_XML, sort_keys=True)


def summary(rep, params, source=None, out=None, size=None, fmt=None, full=True, top=15):
    lines = []
    if source:
        lines.append(f'source: {source}')
    if out:
        lines.append(f'output: {out}' + (f' ({size} bytes, {fmt} property list)' if size is not None and not out.startswith('none') else ''))
    lines.append(f'rules: {params.describe()}')
    lines.append('protected by label: ' + (', '.join(f'{k} x{v}' for k, v in sorted(rep.kept_seen.items())) or 'none'))
    lines.append('protected by RecoverOnCrash: ' + (', '.join(sorted(rep.crash_seen)) or 'none'))
    lines.append('raised ' + RAISE_KEY + ': ' + (', '.join(f'{k} to at least {v}' for k, v in sorted(params.raises.items())) or 'none'))
    by_reason = {}
    for c in rep.changes:
        by_reason[c.why] = by_reason.get(c.why, 0) + 1
    lines.append(f'label entries {rep.entries}; with a memory limit {rep.with_limits}; changed {rep.changed_entries()}')
    lines.append(f'limit keys changed {len(rep.changes)}: ' + ', '.join(f'{n} {w}' for w, n in sorted(by_reason.items())))
    lines.append(f'limit keys left: {rep.unlimited} without a limit (-1), {rep.small} at or below {params.threshold} MB, '
                 f'{rep.protected_keys} in protected entries')
    for path, key, old in rep.odd:
        lines.append(f'warning: {"/".join(path[1:])} {key} is {old!r}, not a whole number: left')
    for problem in rep.problems:
        lines.append(f'warning: {problem}')
    ordered = sorted(rep.changes, key=lambda c: (-(c.old - c.new), c.where, c.key))
    lines.append(f'the {min(top, len(ordered))} biggest changes (MB less):')
    for c in ordered[:top]:
        lines.append(f'  {c.old - c.new:6d}  {c.where}: {c.key} {c.old} -> {c.new}')
    if full:
        lines.append('every changed entry (old -> new):')
        grouped = {}
        for c in rep.changes:
            grouped.setdefault(c.path, []).append(c)
        for path in sorted(grouped, key=lambda p: p[1:]):
            parts = [f'{c.key} {c.old} -> {c.new}' + ('' if c.why == 'scaled' else f' ({c.why})') for c in sorted(grouped[path], key=lambda c: c.key)]
            lines.append(f'  {grouped[path][0].where}: ' + '; '.join(parts))
    return '\n'.join(lines) + '\n'


def read_list(path):
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    if not isinstance(plist, dict) or not isinstance(plist.get('Version4'), dict):
        raise ValueError(f'{path}: no Version4 dictionary: not a jetsam properties list')
    return plist


def main(argv=None):
    p = argparse.ArgumentParser(prog='jetsam_derive.py', description='\n\n'.join(__doc__.split('\n\n')[:3]),
                                formatter_class=argparse.RawDescriptionHelpFormatter,
                                epilog='The rules, the keys and the exit status: the header of this file.')
    p.add_argument('source', metavar='SOURCE', help='the OS image\'s com.apple.jetsamproperties.V54.plist')
    where = p.add_mutually_exclusive_group()
    where.add_argument('-o', '--out', metavar='FILE', help='write the list to this file')
    where.add_argument('--out-dir', metavar='DIR', help='write DIR/com.apple.jetsamproperties.NAME.plist')
    p.add_argument('--name', default=DEFAULT_NAME, help=f'the product name, for --out-dir (default {DEFAULT_NAME}): what '
                   'xnuloader-crosvm --jetsam-product is given')
    p.add_argument('--factor', default='0.4', help='the share of each limit that is kept, above 0 and at most 1: 0.4 or 2/5 (default 0.4)')
    p.add_argument('--threshold', type=int, default=DEFAULT_THRESHOLD, help=f'MB: limits at or below it stay (default {DEFAULT_THRESHOLD})')
    p.add_argument('--floor', type=int, default=DEFAULT_FLOOR, help=f'MB: no scaled limit goes below it; at least the threshold (default {DEFAULT_FLOOR})')
    p.add_argument('--keep', action='append', default=[], metavar='LABEL', help='one more label whose limits stay (repeatable)')
    p.add_argument('--raise', dest='raises', action='append', default=[], metavar='LABEL=MB',
                   help=f'one more {RAISE_KEY} to raise to at least MB, or another value for one of the two the script raises (repeatable)')
    p.add_argument('--format', choices=('binary', 'xml'), default='binary', help='the output format (default binary, as the original)')
    p.add_argument('--lenient', action='store_true', help='a protected or raised label that the list lacks is a warning, not exit 3')
    p.add_argument('--verify', metavar='DERIVED', help='write nothing: say whether DERIVED is what these options make of SOURCE')
    p.add_argument('--summary', metavar='FILE', help='write the whole summary to this file (the screen then has the counts and the biggest changes)')
    p.add_argument('-q', '--quiet', action='store_true', help='counts and the biggest changes only on the screen')
    args = p.parse_args(argv)

    if not NAME_OK.match(args.name):
        p.error('--name: 1 to 31 of A-Z a-z 0-9 . _ -, not starting with a dot (what xnuloader-crosvm --jetsam-product takes)')
    try:
        factor = Fraction(args.factor)
    except (ValueError, ZeroDivisionError):
        p.error(f'--factor {args.factor!r}: not a number (0.4 or 2/5)')
    keep = dict(PROTECTED)
    keep.update({label: 'named with --keep' for label in args.keep})
    raises = dict(RAISED)
    for item in args.raises:
        label, _, mb = item.rpartition('=')
        if not label or not mb.isdigit():
            p.error(f'--raise {item!r}: LABEL=MB')
        raises[label] = int(mb)
    try:
        params = Params(factor, args.threshold, args.floor, keep, raises)
    except ValueError as e:
        p.error(str(e))
    try:
        source = read_list(args.source)
    except Exception as e:      # a file that is not a property list fails in more than one way
        print(f'jetsam_derive: {args.source}: {e}', file=sys.stderr)
        return 2
    try:
        derived, rep = derive(source, params, strict=not args.lenient)
    except LabelError as e:
        print(f'jetsam_derive: {e}: this is not the list the rules were written for (--lenient to go on)', file=sys.stderr)
        return 3
    broken = check(source, derived, params)
    if broken:
        print('jetsam_derive: the result breaks the rules (a bug):', file=sys.stderr)
        for line in broken[:20]:
            print('  ' + line, file=sys.stderr)
        return 4
    if args.verify:
        try:
            other = read_list(args.verify)
        except Exception as e:
            print(f'jetsam_derive: {args.verify}: {e}', file=sys.stderr)
            return 2
        if not identical(other, derived):
            print(f'{args.verify} is not what these options make of {args.source}:')
            for line in (check(source, other, params) or ['it breaks no rule, it is another list'])[:20]:
                print('  ' + line)
            return 1
        print(f'{args.verify} is what these options make of {args.source}: {rep.changed_entries()} entries, {len(rep.changes)} limits changed')
        return 0
    data = dumps(derived, args.format)
    out = args.out or (os.path.join(args.out_dir, f'com.apple.jetsamproperties.{args.name}.plist') if args.out_dir else None)
    if out:
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(data)
    shown = out or 'none written (give -o or --out-dir)'
    if args.summary:
        with open(args.summary, 'w') as f:
            f.write(summary(rep, params, args.source, shown, len(data), args.format, full=True))
    sys.stdout.write(summary(rep, params, args.source, shown, len(data), args.format, full=not (args.quiet or args.summary)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
