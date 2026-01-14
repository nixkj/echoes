#!/usr/bin/env bash
set -euo pipefail

# -----------------------------
# Functions
# -----------------------------
usage() {
  cat <<EOF
Usage: $0 -i <input_audio> -o <output_audio>

Example:
  $0 -i "XC1059929 - Hadada Ibis - Bostrychia hagedash.wav" -o hadeda.mp3
EOF
  exit 1
}

require() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "Error: required command '$1' not found"
    exit 1
  }
}

# -----------------------------
# Dependency checks
# -----------------------------
require sox
require ffmpeg

# -----------------------------
# Argument parsing
# -----------------------------
INPUT_FILE=""
OUTPUT_FILE=""

while getopts ":i:o:" opt; do
  case "$opt" in
    i) INPUT_FILE="$OPTARG" ;;
    o) OUTPUT_FILE="$OPTARG" ;;
    *) usage ;;
  esac
done

[[ -z "$INPUT_FILE" || -z "$OUTPUT_FILE" ]] && usage
[[ ! -f "$INPUT_FILE" ]] && { echo "Input file not found: $INPUT_FILE"; exit 1; }

# -----------------------------
# Derived filenames
# -----------------------------
RAW_FILE="${OUTPUT_FILE%.*}.raw"

# -----------------------------
# Audio processing
# -----------------------------
echo "Processing audio:"
echo "  Input : $INPUT_FILE"
echo "  Output: $OUTPUT_FILE"
echo "  Raw   : $RAW_FILE"

sox "$INPUT_FILE" "$OUTPUT_FILE" \
  remix - \
  pitch -900 \
  pitch 40 \
  rate 8000 \
  tremolo 15 90 \
  phaser 0.5 0.5 2 0.7 0.9 -t \
  echo 0.4 0.4 20 0.4 40 0.3 80 0.2 \
  chorus 0.3 0.3 20 0.2 0.1 1 -s \
  reverb 4 15 10 10 10 0 \
  norm -1

ffmpeg -y \
  -i "$OUTPUT_FILE" \
  -f u8 \
  -ar 8000 \
  -ac 1 \
  "$RAW_FILE"

echo "Done."
