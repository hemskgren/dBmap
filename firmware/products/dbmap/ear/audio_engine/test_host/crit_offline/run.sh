#!/bin/sh
# Build and run Critical Path offline AE replay (host).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../../../../../.." && pwd)
cd "$ROOT"
OUT=${OUT:-/tmp/crit_offline_replay}
CSVDIR=${CSVDIR:-/tmp/crit_offline_out}
EVIDENCE="$ROOT/firmware/products/dbmap/ear/audio_engine/test_host/crit_offline/evidence"
mkdir -p "$CSVDIR"

cc -std=c11 -Wall -Wextra -O2 -g \
  -I "$ROOT/firmware/products/dbmap/ear/audio_engine/include" \
  -I "$ROOT/firmware/products/dbmap/ear/ear_acoustic/include" \
  "$ROOT/firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c" \
  "$ROOT/firmware/products/dbmap/ear/audio_engine/ae_signature.c" \
  "$ROOT/firmware/products/dbmap/ear/audio_engine/test_host/crit_offline_replay.c" \
  -lm -o "$OUT"

echo "== dataset (labeled evidence manifest) =="
"$OUT" dataset "$EVIDENCE/manifest.csv" --out-dir "$CSVDIR"

UART=${1:-$ROOT/dbmap-usb-console-ear-001-0.8.14-after-boot.txt}
if [ -f "$UART" ]; then
  echo "== uart soak (unlabeled ambient; not Critical evidence) =="
  "$OUT" uart "$UART" --mode both --csv "$CSVDIR/uart_l0.csv" \
    --recording-id uart_l0 --origin uart --role ambient --label device_soak
fi

echo "outputs in $CSVDIR"
