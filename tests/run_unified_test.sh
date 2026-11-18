#!/bin/sh
# One-shot build + sample generation + test run for dr_unified.
# FLAC/MP3 samples are converted from the generated WAVs with the system ffmpeg
# (no network, no new dependencies in the repo).
set -e

cd "$(dirname "$0")/.."
OUT=tests/unified_tmp

rm -rf "$OUT"
mkdir -p "$OUT"

gcc -std=c99 -O2 -Wall -Wextra -o "$OUT/dr_unified_test" tests/dr_unified_test.c -lm

# WAVs + unsupported/corrupt fixtures come from the test itself.
"$OUT/dr_unified_test" --gen "$OUT"

# Lossless + lossy variants at both sample rates.
for rate in 48k 44k; do
    ffmpeg -y -loglevel error -i "$OUT/wav_$rate.wav" "$OUT/flac_$rate.flac"
    ffmpeg -y -loglevel error -i "$OUT/wav_$rate.wav" -codec:a libmp3lame -b:a 192k "$OUT/mp3_$rate.mp3"
done

ls -l "$OUT"

# Pin to one CPU where possible to keep the benchmark timings comparable.
if command -v taskset >/dev/null 2>&1; then
    TASKSET="taskset -c 0"
else
    TASKSET=""
fi

$TASKSET "$OUT/dr_unified_test" "$OUT"
