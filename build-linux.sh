#!/bin/bash
# Build Accelerator (SourcePawn-stack fork).
#
# Sources are edited on the Windows side; this mirrors them into a fast ext4
# tree. third_party keeps its own git checkout (the build resets and patches it).
set -euo pipefail
WIN="/mnt/h/code/CSGO SCRIPT"
W="$HOME/accel"
SM="$W/sourcemod"
TARGETS="${TARGETS:-x86}"

rsync -a --delete --exclude '.git' --exclude 'third_party' --exclude 'build*' \
  "$WIN/accelerator/" "$W/accelerator/"

if [ ! -d "$SM/core" ]; then
  rsync -a --exclude '.git' "$WIN/sourcemod-1.11/" "$SM/"
fi

cd "$W/accelerator"

# The .patch files were committed with LF; a Windows working tree turns them
# into CRLF (core.autocrlf) and rsync carries that over. third_party/Patch runs
# `git reset --hard` + `git apply`, so put both sides back to LF - the state CI
# builds in.
for p in patches/*.patch; do
  perl -pi -e 's/\r\n/\n/' "$p"
done
for module in third_party/breakpad third_party/zlib; do
  git -C "$module" config core.autocrlf false
  git -C "$module" reset --hard -q
done

BUILD="$W/accelerator/build-$TARGETS"
if [ "${CLEAN:-0}" = "1" ]; then
  rm -rf "$BUILD"
fi
mkdir -p "$BUILD"
cd "$BUILD"
python3 ../configure.py --sm-path="$SM" --enable-optimize --targets "$TARGETS"

STATUS=0
ambuild . > "$BUILD/build.log" 2>&1 || STATUS=$?
echo "ambuild exit=$STATUS"
grep -nE "error:|FAILED|warning:|Build failed" "$BUILD/build.log" | head -30 || true
echo "---- artifacts ----"
find "$BUILD" -name 'accelerator.ext.*' -printf '%p %s bytes\n'
exit $STATUS
