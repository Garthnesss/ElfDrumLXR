#!/usr/bin/env python3
"""Cross-check for plugin/src/ParamMetadata.generated.h (see tools/gen_pages.sh).

Fails loudly (exit 1) if:
  - any kit index 0..246 is missing or duplicated in the generated table,
  - kPanelOrder is not a permutation of 0..246,
  - a voice slice assignment is inconsistent (voice set per page, FX/global
    params carrying a voice, a voice param on the wrong page),
  - an enum/mod-target param has no value-name list or a count mismatch,
  - a range is empty, or a factory INIT byte falls outside the metadata
    range (which would make the APVTS default silently differ from INIT),
  - display names collide (DAW automation lists would be ambiguous).

The APVTS side of the cross-check (every kitNNN parameter exists and carries
the metadata display name) runs in the plugin_panel_test harness, which can
instantiate the real processor.
"""
import re
import sys

ROOT = __file__.rsplit('/tools/', 1)[0]
errors = []

src = open(f'{ROOT}/plugin/src/ParamMetadata.generated.h').read()
row_re = re.compile(
    r'\{ (\d+), "([^"]+)", "([^"]*)", "([^"]*)", "([^"]*)", (\d+), (\d+), '
    r'Control::(\w+), (-?\d+), (-?\d+), (-?\d+), (\w+|nullptr), (\d+), (\d+) \}')
rows = [dict(index=int(r[0]), symbol=r[1], name=r[2], display=r[3], section=r[4],
             page=int(r[5]), voice=int(r[6]), control=r[7], min=int(r[8]),
             max=int(r[9]), off=int(r[10]), names=r[11], nvalues=int(r[12]),
             flags=int(r[13])) for r in row_re.findall(src)]

if len(rows) != 247:
    errors.append(f'expected 247 kit rows, found {len(rows)}')
indices = sorted(r['index'] for r in rows)
if indices != list(range(247)):
    missing = sorted(set(range(247)) - set(indices))
    dup = sorted({i for i in indices if indices.count(i) > 1})
    errors.append(f'kit index coverage broken: missing={missing} duplicate={dup}')

order = [int(x) for x in re.search(r'kPanelOrder\[247\] = \{([^}]*)\}', src).group(1).split(',')]
if sorted(order) != list(range(247)):
    errors.append('kPanelOrder is not a permutation of 0..246')

names_lists = dict(re.findall(r'static const char \*const (\w+)\[\] = \{([^}]*)\};', src))
def list_len(name):
    if name == 'kModTargetNames':
        m = re.search(r'kModTargetNames\[\] = \{(.*?)\n\};', src, re.S)
        return m.group(1).count('\t"')
    return names_lists[name].count('"') // 2 if name in names_lists else -1

init = open(f'{ROOT}/factory/INIT.SND', 'rb').read()
assert len(init) == 255
init_params = init[8:]

for r in rows:
    i = r['index']
    if r['page'] <= 5 and r['voice'] != r['page']:
        errors.append(f"{i} {r['symbol']}: voice page {r['page']} but voice {r['voice']}")
    if r['page'] >= 6 and r['voice'] != 255:
        errors.append(f"{i} {r['symbol']}: FX/global page but voice {r['voice']}")
    if r['control'] in ('Enum', 'ModTarget'):
        if r['names'] == 'nullptr' or r['nvalues'] == 0:
            errors.append(f"{i} {r['symbol']}: {r['control']} without a value-name list")
        elif list_len(r['names']) != r['nvalues']:
            errors.append(f"{i} {r['symbol']}: numValues {r['nvalues']} != list length {list_len(r['names'])}")
        if r['max'] - r['min'] + 1 != r['nvalues']:
            errors.append(f"{i} {r['symbol']}: range {r['min']}..{r['max']} != {r['nvalues']} values")
    if r['control'] == 'Toggle' and (r['min'], r['max']) != (0, 1):
        errors.append(f"{i} {r['symbol']}: toggle range is {r['min']}..{r['max']}")
    if r['min'] > r['max']:
        errors.append(f"{i} {r['symbol']}: empty range {r['min']}..{r['max']}")
    if not (r['min'] <= init_params[i] <= r['max']):
        errors.append(f"{i} {r['symbol']}: INIT byte {init_params[i]} outside metadata range {r['min']}..{r['max']}")

from collections import Counter
dup_names = [k for k, v in Counter(r['display'] for r in rows).items() if v > 1]
if dup_names:
    errors.append(f'duplicate display names: {dup_names}')

slices = {v: sum(1 for r in rows if r['voice'] == v) for v in range(6)}
print(f'kit rows: {len(rows)}; voice slices: {slices}; '
      f'fx/global: {sum(1 for r in rows if r["voice"] == 255)}')
if errors:
    print('METADATA CROSS-CHECK FAILED:')
    for e in errors:
        print('  -', e)
    sys.exit(1)
print('metadata cross-check: OK (247/247 kit params, panel order, ranges vs INIT, names)')
