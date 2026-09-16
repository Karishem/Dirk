#!/usr/bin/env bash
# Generates assets/test_pattern.mp4: a synthetic clip for seek_test (see
# tools/seek_test.cpp for why this encoding is chosen -- lossless yuv420p,
# frame number burned directly into the Y plane via geq's frame-number
# variable N, no RGB round trip). Not real footage: we only ever received
# Lair.mp4.index (the frame index), never the actual Dragon's Lair video, so
# this is what VideoSource's own smoke test runs against instead.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p assets
ffmpeg -y -f lavfi -i "color=c=black:s=64x64:r=25:d=4,format=yuv420p" \
  -vf "geq=lum='mod(N,256)':cb=128:cr=128" \
  -c:v libx264 -preset veryslow -qp 0 -bf 0 -g 30 -keyint_min 30 \
  -pix_fmt yuv420p \
  assets/test_pattern.mp4
