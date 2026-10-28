#!/bin/sh
# Fetch the pinned Erica Synths LXR-02 firmware source into vendor/lxr-02.
# The pin lives in VENDOR_COMMIT (repo root). Idempotent: re-running with the
# pin already checked out is a no-op. vendor/ is gitignored — the firmware is
# never committed to this repo; it is fetched and patched into build/src at
# build time (see Makefile).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PIN=$(tr -d '[:space:]' < "$ROOT/VENDOR_COMMIT")
DEST="$ROOT/vendor/lxr-02"
URL="https://github.com/erica-synths/lxr-desktop-app"

if [ -d "$DEST/.git" ] && [ "$(git -C "$DEST" rev-parse HEAD 2>/dev/null || true)" = "$PIN" ]; then
    echo "vendor/lxr-02 already at $PIN"
    exit 0
fi

if [ ! -d "$DEST/.git" ]; then
    mkdir -p "$ROOT/vendor"
    # Prefer the canonical remote; fall back to a local mirror if offline.
    if ! git clone --quiet "$URL" "$DEST" 2>/dev/null; then
        LOCAL="$HOME/workspace/lxr-desktop-app"
        if [ -d "$LOCAL/.git" ]; then
            echo "remote clone failed; cloning from local mirror $LOCAL"
            git clone --quiet "$LOCAL" "$DEST"
            git -C "$DEST" remote set-url origin "$URL"
        else
            echo "error: cannot clone $URL and no local mirror found" >&2
            exit 1
        fi
    fi
fi

git -C "$DEST" fetch --quiet origin "$PIN" 2>/dev/null || true
git -C "$DEST" checkout --quiet "$PIN"
git -C "$DEST" submodule update --init --recursive >/dev/null 2>&1 || true

HEAD=$(git -C "$DEST" rev-parse HEAD)
if [ "$HEAD" != "$PIN" ]; then
    echo "error: vendor checkout is $HEAD, expected $PIN" >&2
    exit 1
fi
echo "vendor/lxr-02 at $PIN"
