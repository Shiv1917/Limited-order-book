#!/usr/bin/env bash
# Runs ob-sim twice with the same config and requires identical output hashes.
#   scripts/sim_determinism.sh [CONFIG] [BUILD_DIR]
set -euo pipefail
CONFIG=${1:-sim/configs/fixture_mm.json}
BUILD=${2:-build}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
"$BUILD/sim/ob-sim" run --config "$CONFIG" --out "$TMP/a" > /dev/null
"$BUILD/sim/ob-sim" run --config "$CONFIG" --out "$TMP/b" > /dev/null
ha=$(grep '"output_sha256"' "$TMP/a/summary.json")
hb=$(grep '"output_sha256"' "$TMP/b/summary.json")
for f in orders.csv fills.csv pnl.csv; do cmp -s "$TMP/a/$f" "$TMP/b/$f" || { echo "FAIL: $f differs"; exit 1; }; done
[ "$ha" = "$hb" ] || { echo "FAIL: output hashes differ"; exit 1; }
echo "deterministic: $ha"
