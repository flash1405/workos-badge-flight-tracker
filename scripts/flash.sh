#!/usr/bin/env bash
set -euo pipefail

FQBN=m5stack:esp32:m5stack_stopwatch
cd "$(dirname "$0")/.."

if [ ! -f flight_radar/secrets.h ]; then
  echo "Missing flight_radar/secrets.h. Run scripts/setup.sh first."
  exit 1
fi

PORT="${1:-$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1 || true)}"
if [ -z "$PORT" ]; then
  echo "Badge not found. Plug it in and hold PWR for 2 seconds until the green LED turns on."
  exit 1
fi

arduino-cli compile -b "$FQBN" --output-dir build flight_radar
arduino-cli upload -b "$FQBN" -p "$PORT" --input-dir build flight_radar
