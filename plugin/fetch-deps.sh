#!/bin/sh
# ElfDrumLXR plugin dependencies, pinned like the firmware vendor pin (see ../scripts/fetch-vendor.sh).
# JUCE:    tag 8.0.9            (juce-framework/JUCE)
# r8brain: commit cb2abb9977efe2471979b380ed95daa56ab4fdb9 (avaneev/r8brain-free-src)
# clap-juce-extensions: commit 7adee3a1bd4684d4caa5601100e364abccff4b4d (Phase 4, optional CLAP target)
set -e
cd "$(dirname "$0")"
mkdir -p vendor
if [ ! -d vendor/JUCE/.git ]; then
  git clone --depth 1 --branch 8.0.9 https://github.com/juce-framework/JUCE.git vendor/JUCE
else
  git -C vendor/JUCE fetch -q --depth 1 origin tag 8.0.9 && git -C vendor/JUCE checkout -q 8.0.9
fi
if [ ! -d vendor/r8brain-free-src/.git ]; then
  git clone -q https://github.com/avaneev/r8brain-free-src.git vendor/r8brain-free-src
fi
git -C vendor/r8brain-free-src fetch -q origin cb2abb9977efe2471979b380ed95daa56ab4fdb9 || true
git -C vendor/r8brain-free-src checkout -q cb2abb9977efe2471979b380ed95daa56ab4fdb9
if [ ! -d vendor/clap-juce-extensions/.git ]; then
  git clone -q --recurse-submodules https://github.com/free-audio/clap-juce-extensions.git vendor/clap-juce-extensions
fi
git -C vendor/clap-juce-extensions checkout -q 7adee3a1bd4684d4caa5601100e364abccff4b4d
git -C vendor/clap-juce-extensions submodule update -q --recursive
echo "JUCE:    $(git -C vendor/JUCE describe --tags)"
echo "r8brain: $(git -C vendor/r8brain-free-src rev-parse HEAD)"
echo "clap-juce-extensions: $(git -C vendor/clap-juce-extensions rev-parse HEAD)"
