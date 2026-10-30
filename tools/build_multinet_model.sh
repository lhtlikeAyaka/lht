#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${ROOT}/.build-multinet"
ESP_SR_TAG="${ESP_SR_TAG:-v2.1.5}"
PARTITION_BYTES=$((6 * 1024 * 1024))

rm -rf "${WORK}"
mkdir -p "${WORK}/output"

git clone --depth 1 --branch "${ESP_SR_TAG}" https://github.com/espressif/esp-sr.git "${WORK}/esp-sr"
git -C "${WORK}/esp-sr" lfs install --local
git -C "${WORK}/esp-sr" lfs pull

# movemodel.py selects the requested pre-trained model and packs it into srmodels.bin.
printf '%s\n' 'CONFIG_SR_MN_CN_MULTINET6_QUANT=y' > "${WORK}/sdkconfig"
MOVEMODEL="$(find "${WORK}/esp-sr" -maxdepth 3 -name movemodel.py -print -quit)"
if [[ -z "${MOVEMODEL}" ]]; then
  echo "ERROR: movemodel.py was not found in ESP-SR ${ESP_SR_TAG}" >&2
  exit 1
fi

python3 "${MOVEMODEL}" \
  -d1 "${WORK}/sdkconfig" \
  -d2 "${WORK}/esp-sr" \
  -d3 "${WORK}/build"

MODEL_BIN="$(find "${WORK}/build" -name srmodels.bin -type f -print -quit)"
if [[ -z "${MODEL_BIN}" ]]; then
  echo "ERROR: ESP-SR did not produce srmodels.bin" >&2
  find "${WORK}/build" -maxdepth 4 -type f -print || true
  exit 1
fi

MODEL_SIZE="$(stat -c '%s' "${MODEL_BIN}")"
if (( MODEL_SIZE > PARTITION_BYTES )); then
  echo "ERROR: srmodels.bin is ${MODEL_SIZE} bytes, larger than 6 MiB model partition" >&2
  exit 1
fi

cp "${MODEL_BIN}" "${WORK}/output/srmodels.bin"
cat > "${WORK}/output/model-info.txt" <<INFO
ESP-SR tag: ${ESP_SR_TAG}
Model selection: CONFIG_SR_MN_CN_MULTINET6_QUANT=y
Expected models: mn6_cn + fst
Target: ESP32-S3
Model partition: 6 MiB
Binary size: ${MODEL_SIZE} bytes
Wake phrase is configured at runtime by the firmware; this binary is generic and does not contain a phrase-specific trained wakeword.
INFO

mkdir -p "${ROOT}/output"
cp "${WORK}/output/srmodels.bin" "${ROOT}/output/srmodels.bin"
cp "${WORK}/output/model-info.txt" "${ROOT}/output/model-info.txt"
echo "Created ${ROOT}/output/srmodels.bin (${MODEL_SIZE} bytes)"
