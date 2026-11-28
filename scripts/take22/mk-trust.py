#!/usr/bin/env python3
# scripts/take22/mk-trust.py (take22): add a Metal plug-in build's two CDHashes to an image set's owner trust list and bring its manifest entry
# (size, sha256, owner_cdhashes) in line. The apps' CDHashes are added afterwards by userland/apps/appsaddon.py.
#   mk-trust.py TRUST_IN MANIFEST_IN BUILD_JSON NOTE TRUST_OUT MANIFEST_OUT
import hashlib, json, sys
trust_in, manifest_in, build_json, note, trust_out, manifest_out = sys.argv[1:7]
trust = json.load(open(trust_in)); build = json.load(open(build_json))
have = set(trust['cdhashes']); added = []
for name, f in sorted(build['files'].items()):
    h = f['cdhash']
    if len(h) != 40 or any(c not in '0123456789abcdef' for c in h): sys.exit('mk-trust: bad CDHash for ' + name)
    if h in have: continue
    trust['files'].append({'adhoc': True, 'arch': f['arch'], 'cdhash': h, 'hash': 'sha256', 'identifier': f['identifier'],
                           'note': note, 'page_size': f['page_size'],
                           'path': '/private/var/ngpu/addons/NativeAdrenoMetal.bundle/' + name, 'warnings': []})
    trust['cdhashes'].append(h); have.add(h); added.append(h)
trust['cdhashes'] = sorted(set(trust['cdhashes']))
text = json.dumps(trust, indent=2, sort_keys=True) + '\n'
open(trust_out, 'w').write(text)
raw = text.encode(); sha = hashlib.sha256(raw).hexdigest()
lines = open(manifest_in).read().split('\n'); n = 0
for i, l in enumerate(lines):
    if '"path": "overlay/userland/owner-trust.json"' in l:
        for j in range(i + 1, i + 4):
            if '"sha256"' in lines[j]: lines[j] = lines[j][:lines[j].index('"sha256"')] + '"sha256": "%s",' % sha; n += 1
            if '"size"' in lines[j]: lines[j] = lines[j][:lines[j].index('"size"')] + '"size": %d' % len(raw); n += 1
    if '"owner_cdhashes"' in l:
        lines[i] = l[:l.index('"owner_cdhashes"')] + '"owner_cdhashes": %d,' % len(trust['cdhashes']); n += 1
if n != 3: sys.exit('mk-trust: the manifest has not the three lines to change (%d found)' % n)
open(manifest_out, 'w').write('\n'.join(lines))
json.load(open(manifest_out))
print('mk-trust: %d CDHashes (%d added: %s), trust sha256 %s' % (len(trust['cdhashes']), len(added), ' '.join(added), sha))
