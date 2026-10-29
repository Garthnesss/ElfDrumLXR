#!/bin/sh
# Regenerates plugin/src/ParamMetadata.generated.h from the LXR-02 firmware's own
# menu tables (vendor tree, pinned by scripts/fetch-vendor.sh). Do not edit the
# generated header by hand; edit the firmware-derived inputs or this tool instead.
#
# Method (same extraction PHX-D01 proved for its web UI, reimplemented here):
#   - parameter_dtypes[] and valueNames[] are cut verbatim out of menu.c,
#   - menuPages.h / MenuText.h / Cc2Text.c (modTargets) are compiled in directly,
#   - the ParamEnums symbol names are extracted from Parameters.h by the python
#     helper below and cross-checked against the compiled enum values.
set -e
cd "$(dirname "$0")/.."
CORE=vendor/lxr-02/Core
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# --- cut the two big tables out of menu.c -------------------------------------
awk '/parameter_dtypes\[NUM_PARAMS\]/,/^};/' "$CORE/Src/menu/menu.c" | sed 's/PROGMEM//' > "$T/dtypes.inc"
awk '/^const Name valueNames/,/^};/'        "$CORE/Src/menu/menu.c" | sed 's/PROGMEM//' > "$T/names.inc"
awk '/^const ModTarg modTargets/,/^};/'     "$CORE/Src/menu/Cc2Text.c" > "$T/modtargets.inc"
awk '/^const ModTargetVoiceOffset modTargetVoiceOffsets/,/^};/' "$CORE/Src/menu/Cc2Text.c" > "$T/modoffsets.inc"
grep -q 'parameter_dtypes' "$T/dtypes.inc"
grep -q 'valueNames'       "$T/names.inc"
grep -q 'modTargets'       "$T/modtargets.inc"

# --- extract the ParamEnums symbol table (index -> name) ----------------------
python3 - "$CORE/Inc/Parameters.h" "$T/param_symbols.inc" <<'EOF'
import re, sys
src = open(sys.argv[1]).read()
body = src[src.index('enum ParamEnums'):]
body = body[body.index('{') + 1:body.index('};')]
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
body = re.sub(r'//[^\n]*', '', body)
symbols, value_of, counter = {}, {}, -1
for entry in body.split(','):
    entry = entry.strip()
    if not entry:
        continue
    m = re.match(r'^([A-Z_][A-Z0-9_]*)\s*(?:=\s*([A-Za-z0-9_]+))?$', entry)
    if not m:
        raise SystemExit('unparsed enum entry: %r' % entry)
    name, rhs = m.groups()
    counter = (value_of[rhs] if rhs and not rhs.isdigit() else int(rhs)) if rhs else counter + 1
    value_of[name] = counter
    symbols.setdefault(counter, name)   # first name wins (PAR_OSC_WAVE_DRUM1 over PAR_MOD_WHEEL alias)
n = max(symbols) + 1
# Index 1 is declared as PAR_MOD_WHEEL with PAR_OSC_WAVE_DRUM1 as its alias;
# the waveform meaning is the canonical one (the enum's own comment says so).
symbols[1] = 'PAR_OSC_WAVE_DRUM1'
# Likewise 247 is the END_OF_SOUND_PARAMETERS sentinel; PAR_ROLL aliases it
# and is the real parameter at that index.
symbols[247] = 'PAR_ROLL'
assert symbols[0] == 'PAR_NONE' and symbols[1] == 'PAR_OSC_WAVE_DRUM1', symbols.get(1)
assert symbols[234] == 'PAR_FX_TYPE' and symbols[246] == 'PAR_MORPH_KIT_DEST'
assert symbols[247] == 'PAR_ROLL', symbols.get(247)
with open(sys.argv[2], 'w') as f:
    f.write('static const char *paramSymbols[] = {\n')
    for i in range(n):
        f.write('\t"%s",\n' % symbols.get(i, ''))
    f.write('};\n')
print('param symbols: %d entries (NUM_PARAMS expected %d + terminator)' % (n, n - 1))
EOF

# --- compile and run the generator --------------------------------------------
cc -w -std=gnu11 -O1 \
   -Ishim -I"$T" \
   -I"$CORE/Inc" -I"$CORE/Inc/menu" -I"$CORE/Inc/Soundengine" -I"$CORE/Inc/Soundengine/DSPAudio" \
   -I"$CORE/Src" \
   tools/gen_pages.c -o "$T/gen_pages"
"$T/gen_pages" > plugin/src/ParamMetadata.generated.h
echo "wrote plugin/src/ParamMetadata.generated.h"
