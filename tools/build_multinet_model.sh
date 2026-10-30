#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${ROOT}/.build-multinet"
ESP_SR_VERSION="${ESP_SR_VERSION:-2.1.5}"
# ESP-SR 2.1.5 is published in the ESP Component Registry, but does not have
# a matching v2.1.5 Git tag. This is the exact source commit from that package.
ESP_SR_COMMIT="${ESP_SR_COMMIT:-dec03a644fc1b4b412851f425f2a91f1083d1e0b}"
PARTITION_BYTES=$((6 * 1024 * 1024))

rm -rf "${WORK}"
mkdir -p "${WORK}/esp-sr" "${WORK}/output"

echo "Fetching ESP-SR ${ESP_SR_VERSION} at ${ESP_SR_COMMIT}"
git -C "${WORK}/esp-sr" init
git -C "${WORK}/esp-sr" remote add origin https://github.com/espressif/esp-sr.git
git -C "${WORK}/esp-sr" lfs install --local
git -C "${WORK}/esp-sr" fetch --depth=1 origin "${ESP_SR_COMMIT}"
git -C "${WORK}/esp-sr" checkout --detach FETCH_HEAD
git -C "${WORK}/esp-sr" lfs pull

ACTUAL_COMMIT="$(git -C "${WORK}/esp-sr" rev-parse HEAD)"
if [[ "${ACTUAL_COMMIT}" != "${ESP_SR_COMMIT}" ]]; then
  echo "ERROR: checked out ${ACTUAL_COMMIT}, expected ${ESP_SR_COMMIT}" >&2
  exit 1
fi

MANIFEST="${WORK}/esp-sr/idf_component.yml"
if [[ ! -f "${MANIFEST}" ]] || ! grep -Eq '^version:[[:space:]]*"2\.1\.5"$' "${MANIFEST}"; then
  echo "ERROR: source commit does not identify itself as ESP-SR ${ESP_SR_VERSION}" >&2
  exit 1
fi

MODEL_SOURCE="${WORK}/esp-sr/model/multinet_model"
for model in mn6_cn fst; do
  if [[ ! -d "${MODEL_SOURCE}/${model}" ]]; then
    echo "ERROR: ESP-SR ${ESP_SR_VERSION} is missing model ${model}" >&2
    exit 1
  fi
  if grep -RIl '^version https://git-lfs.github.com/spec/v1$' "${MODEL_SOURCE}/${model}" | grep -q .; then
    echo "ERROR: Git LFS pointer remains in model ${model}; model data was not downloaded" >&2
    exit 1
  fi
done

# movemodel.py selects the official pre-trained MultiNet6 Chinese model and FST.
printf '%s\n' 'CONFIG_SR_MN_CN_MULTINET6_QUANT=y' > "${WORK}/sdkconfig"
MOVEMODEL="$(find "${WORK}/esp-sr" -maxdepth 3 -name movemodel.py -print -quit)"
if [[ -z "${MOVEMODEL}" ]]; then
  echo "ERROR: movemodel.py was not found in ESP-SR ${ESP_SR_VERSION}" >&2
  exit 1
fi

python3 "${MOVEMODEL}" \
  -d1 "${WORK}/sdkconfig" \
  -d2 "${WORK}/esp-sr" \
  -d3 "${WORK}/build"

MODEL_DIR="${WORK}/build/srmodels"
for model in mn6_cn fst; do
  if [[ ! -d "${MODEL_DIR}/${model}" ]]; then
    echo "ERROR: model packer did not include ${model}" >&2
    find "${WORK}/build" -maxdepth 4 -type f -print || true
    exit 1
  fi
done

MODEL_BIN="$(find "${WORK}/build" -name srmodels.bin -type f -print -quit)"
if [[ -z "${MODEL_BIN}" ]]; then
  echo "ERROR: ESP-SR did not produce srmodels.bin" >&2
  find "${WORK}/build" -maxdepth 4 -type f -print || true
  exit 1
fi

MODEL_SIZE="$(stat -c '%s' "${MODEL_BIN}")"
if (( MODEL_SIZE == 0 || MODEL_SIZE > PARTITION_BYTES )); then
  echo "ERROR: srmodels.bin is ${MODEL_SIZE} bytes; expected 1..6291456 bytes" >&2
  exit 1
fi

cp "${MODEL_BIN}" "${WORK}/output/srmodels.bin"
cat > "${WORK}/output/model-info.txt" <<INFO
ESP-SR component version: ${ESP_SR_VERSION}
ESP-SR source commit: ${ESP_SR_COMMIT}
Model selection: CONFIG_SR_MN_CN_MULTINET6_QUANT=y
Included models: mn6_cn + fst
Target: ESP32-S3
Model partition: 6 MiB
Binary size: ${MODEL_SIZE} bytes
Wake phrase is configured at runtime by the firmware; this binary is generic and does not contain a phrase-specific trained wakeword.
INFO

mkdir -p "${ROOT}/output"
cp "${WORK}/output/srmodels.bin" "${ROOT}/output/srmodels.bin"
cp "${WORK}/output/model-info.txt" "${ROOT}/output/model-info.txt"
echo "Created ${ROOT}/output/srmodels.bin (${MODEL_SIZE} bytes)"
