#!/usr/bin/env bash
# Streams the start of NASDAQ's public ITCH 5.0 sample (2019-01-30) and
# extracts message streams for a few symbols. Only the first BYTES of the
# 4.7 GB gzip are downloaded (default 600 MB ~ 04:00-10:00 exchange time).
# Check the listing if the URL moves: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
set -euo pipefail
BYTES=${BYTES:-600000000}
URL=${URL:-"https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"}
BUILD=${BUILD:-build}
mkdir -p data/itch_raw
RAW=data/itch_raw/01302019_head.gz
if [ ! -s "$RAW" ]; then
  echo "downloading first $BYTES bytes of $URL"
  curl -fL -r "0-$((BYTES - 1))" "$URL" -o "$RAW"
fi
for sym in ${SYMBOLS:-AAPL SPY QQQ}; do
  lower=$(echo "$sym" | tr '[:upper:]' '[:lower:]')
  # gunzip complains about the truncated tail; that is expected.
  gunzip -c "$RAW" 2>/dev/null | "$BUILD/ob_replay" itch --symbol "$sym" --diff --out "data/$lower.obm" || true
done
