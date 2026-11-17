#!/bin/bash
# Apply the ESP-VoCat-S31 ES8389 driver fix (MICBIAS + single-ended MIC1P + full
# analog/ADC init) on top of the managed esp_codec_dev component.
#
# The Component Manager re-extracts managed components on dependency changes, so
# run this again after `idf.py reconfigure`/`fullclean` if the mic goes silent.
set -e
cd "$(dirname "$0")/.."
PATCH_DIR="main/boards/espressif/esp-vocat-s31/es8389_patch"
DEST="managed_components/espressif__esp_codec_dev/device/es8389"
[ -d "$DEST" ] || { echo "esp_codec_dev not installed yet; run a build first"; exit 1; }
cp "$PATCH_DIR/es8389.c" "$DEST/es8389.c"
cp "$PATCH_DIR/es8389_reg.h" "$DEST/es8389_reg.h"
echo "applied vocat es8389 driver patch to $DEST"
