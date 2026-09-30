#!/usr/bin/env bash
# Fetch the Telink TLSR825x SDK, build files and tc32 toolchain from atc1441/ATC_TLSR_Paper
# (MIT licensed) at a pinned commit. Only needed to rebuild the tag firmware; a prebuilt
# hanshow_serial.bin is included in the repo.
set -euo pipefail
cd "$(dirname "$0")"
COMMIT=3778d296f418c30b05310d86dafa4e3404071cb4
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
git -C "$TMP" init -q
git -C "$TMP" remote add origin https://github.com/atc1441/ATC_TLSR_Paper
git -C "$TMP" sparse-checkout set Firmware/components Firmware/make Firmware/static_src Firmware/tc32_linux
git -C "$TMP" fetch -q --depth 1 origin "$COMMIT"
git -C "$TMP" checkout -q FETCH_HEAD
for d in components make static_src tc32_linux; do
  rm -rf "$d"
  mv "$TMP/Firmware/$d" .
done
chmod -R +x tc32_linux/bin tc32_linux/tc32-elf/bin 2>/dev/null || true
echo "SDK + toolchain ready. Build with: make"
