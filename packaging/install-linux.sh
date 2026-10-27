#!/bin/bash
# Install the SourcePawn-stack Accelerator build onto a CS:GO Legacy server.
#
#   bash install-linux.sh /path/to/server                  # real install
#   bash install-linux.sh /path/to/server --dry-run        # show what would happen
#
# Anything already there is moved to a timestamped rollback folder first, and
# every copied file is checked against checksums.txt afterwards, so a half
# installed tree is not a thing this script can leave behind.
set -euo pipefail

SERVER=${1:-}
DRY=${2:-}
[ -n "$SERVER" ] || { echo "usage: bash install-linux.sh <server root> [--dry-run]"; exit 2; }
[ -d "$SERVER/addons/sourcemod" ] || { echo "no addons/sourcemod under $SERVER - is that the server root?"; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/linux-x86/addons/sourcemod"
[ -d "$SRC" ] || { echo "missing $SRC - unpack the whole release folder, not just this script"; exit 2; }

declare -a FILES=(
	"extensions/accelerator.ext.so"
	"extensions/accelerator.autoload"
	"gamedata/accelerator.games.txt"
	"scripting/include/accelerator.inc"
	"plugins/accelerator_stack_probe.smx"
)

stamp=$(date +%Y%m%d-%H%M%S)
rollback="$SERVER/addons/sourcemod/rollback-accelerator-spstack/$stamp"

echo "server   : $SERVER"
echo "package  : $HERE"
echo "rollback : $rollback"
[ "$DRY" = "--dry-run" ] && echo "(dry run: nothing is written)"
echo

if [ "$DRY" != "--dry-run" ]; then
	mkdir -p "$rollback"
fi

replaced=0
for f in "${FILES[@]}"; do
	dest="$SERVER/addons/sourcemod/$f"
	if [ -e "$dest" ]; then
		printf 'replacing  %-45s (old size %s)\n' "$f" "$(stat -c %s "$dest")"
		if [ "$DRY" != "--dry-run" ]; then
			mkdir -p "$(dirname "$rollback/$f")"
			mv "$dest" "$rollback/$f"
		fi
		replaced=$((replaced + 1))
	else
		printf 'installing %-45s (new)\n' "$f"
	fi
	if [ "$DRY" != "--dry-run" ]; then
		mkdir -p "$(dirname "$dest")"
		cp "$SRC/$f" "$dest"
		chmod 644 "$dest"
	fi
done

if [ "$DRY" != "--dry-run" ]; then
	cd "$SERVER/addons/sourcemod"
	# Verify exactly the files this script installs. checksums.txt also covers
	# files that belong to other platforms and to the source tree, and failing on
	# those would mean failing on something the installer never claims to own.
	for f in "${FILES[@]}"; do
		expected=$(grep -F "  linux-x86/addons/sourcemod/$f" "$HERE/checksums.txt" | cut -d' ' -f1)
		if [ -z "$expected" ]; then
			echo "NO CHECKSUM ENTRY: $f"
			exit 1
		fi
		actual=$(sha256sum "$f" | cut -d' ' -f1)
		if [ "$actual" != "$expected" ]; then
			echo "CHECKSUM MISMATCH: $f"
			exit 1
		fi
	done
	echo "all ${#FILES[@]} files verified against checksums.txt"
fi

echo
echo "next steps, on the server:"
echo "  1. restart the server, or load it now:  sm exts load accelerator.ext"
echo "  2. in the server console: sm plugins list     (accelerator must appear)"
echo "  3. in the server console: sm_accel_stack     (must show at least 2 frames)"
echo "  4. only when 3 looks right: sm_accel_crash confirm   <-- this ends the server"
echo
echo "crash reports land in addons/sourcemod/data/dumps/<dump>.dmp.txt and are"
echo "copied to addons/sourcemod/data/dumps/archive/ after upload."
if [ "$replaced" != "0" ]; then
	echo
	echo "$replaced file(s) were replaced; the previous copies are in $rollback"
fi
