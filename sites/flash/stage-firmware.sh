#!/usr/bin/env bash
# Copy the three .bin artifacts from an IDF build directory into
# sites/flash/firmware/<version>/ so manifest.json can resolve them when
# the site is served (locally or via GitHub Pages).
#
# Usage:
#   ./stage-firmware.sh                       # build=../../build, version=latest
#   ./stage-firmware.sh <build-dir>           # custom build dir
#   ./stage-firmware.sh <build-dir> <version> # custom build dir and version slug
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
build_dir="${1:-$here/../../build}"
version="${2:-latest}"
dest="$here/firmware/$version"

if [[ ! -d "$build_dir" ]]; then
  echo "stage-firmware: build dir not found: $build_dir" >&2
  echo "  run 'make build' from the repo root first." >&2
  exit 1
fi

bootloader="$build_dir/bootloader/bootloader.bin"
parttable="$build_dir/partition_table/partition-table.bin"
app="$build_dir/tinyblok.bin"

for f in "$bootloader" "$parttable" "$app"; do
  if [[ ! -f "$f" ]]; then
    echo "stage-firmware: missing artifact: $f" >&2
    exit 1
  fi
done

mkdir -p "$dest"
cp "$bootloader" "$dest/bootloader.bin"
cp "$parttable"  "$dest/partition-table.bin"
cp "$app"        "$dest/tinyblok.bin"

echo "staged -> $dest"
ls -lh "$dest"
