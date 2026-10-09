#!/usr/bin/env bash
set -euo pipefail

if ! command -v arduino-cli >/dev/null; then
  brew install arduino-cli
fi

arduino-cli config init --overwrite >/dev/null
arduino-cli config add board_manager.additional_urls https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
arduino-cli core update-index
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified M5GFX ArduinoJson

cd "$(dirname "$0")/../flight_radar"
if [ ! -f secrets.h ]; then
  cp secrets.example.h secrets.h
  echo "Created flight_radar/secrets.h. Add your Wi-Fi name and password."
fi
