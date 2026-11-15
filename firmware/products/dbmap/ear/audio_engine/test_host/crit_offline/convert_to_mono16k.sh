#!/bin/sh
# Explicit, reproducible conversion to mono 16 kHz PCM s16le WAV.
# Does NOT apply gain / normalize / AGC.
set -eu
if [ "$#" -lt 2 ]; then
  echo "usage: $0 <input_audio> <output_wav> [extra ffmpeg args...]" >&2
  exit 2
fi
IN=$1
OUT=$2
shift 2
if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg not found; install ffmpeg to convert recordings" >&2
  exit 1
fi
mkdir -p "$(dirname "$OUT")"
# -ac 1: first-channel downmix policy of ffmpeg (document if stereo source).
# For explicit left/right, pass e.g. -af "pan=mono|c0=c0" as extra args instead.
ffmpeg -y -i "$IN" -ac 1 -ar 16000 -c:a pcm_s16le -vn "$@" "$OUT"
echo "wrote $OUT (pcm_s16le mono 16000 Hz; gain unchanged by this script)"
