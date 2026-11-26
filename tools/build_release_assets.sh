#!/usr/bin/env bash
# Build the MiniPAR firmware and assemble flashable release assets in dist/.
#
# The firmware version has one source: DEVICE_FIRMWARE_VERSION_STR in
# Firmware/include/app/device_config.h. It is what `hello` and INFO report
# after flashing, and it names the release tag (v<version>) and the
# application image (minipar-fw-v<version>.bin), matching the Ambit's
# ambit-fw-v<version>.bin that openJII lists from GitHub releases.
#
# Usage: tools/build_release_assets.sh [expected-version]
#   expected-version  fail unless the source version equals it (release tags)
# Env:   PIO          PlatformIO command (default: pio)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PIO="${PIO:-pio}"
ENV_NAME="esp32-c3-devkitm-1"
CONFIG="${ROOT}/Firmware/include/app/device_config.h"

VERSION=$(sed -nE 's/^#define DEVICE_FIRMWARE_VERSION_STR[[:space:]]+"([^"]+)".*/\1/p' "${CONFIG}")
if [[ -z "${VERSION}" ]]; then
  echo "DEVICE_FIRMWARE_VERSION_STR not found in ${CONFIG}" >&2
  exit 1
fi
if [[ $# -ge 1 && "$1" != "${VERSION}" ]]; then
  echo "Source firmware version ${VERSION} does not match expected $1" >&2
  exit 1
fi

${PIO} run -d "${ROOT}/Firmware" -e "${ENV_NAME}"

BUILD_DIR="${ROOT}/Firmware/.pio/build/${ENV_NAME}"
for FILE in firmware.bin bootloader.bin partitions.bin; do
  test -f "${BUILD_DIR}/${FILE}"
done

# The version string must be embedded verbatim: it is what hello and INFO report.
grep -aq "${VERSION}" "${BUILD_DIR}/firmware.bin"

# boot_app0.bin ships with the Arduino-ESP32 core, not the build; a full flash
# needs it at 0xe000.
BOOT_APP0=$(find "${PLATFORMIO_CORE_DIR:-${HOME}/.platformio}/packages" \
  -path "*framework-arduinoespressif32*" -name boot_app0.bin | head -n 1)
test -n "${BOOT_APP0}"

DIST="${ROOT}/dist"
rm -rf "${DIST}"
mkdir -p "${DIST}"
APP_NAME="minipar-fw-v${VERSION}.bin"
cp "${BUILD_DIR}/firmware.bin" "${DIST}/${APP_NAME}"
cp "${BUILD_DIR}/bootloader.bin" "${DIST}/bootloader.bin"
cp "${BUILD_DIR}/partitions.bin" "${DIST}/partitions.bin"
cp "${BOOT_APP0}" "${DIST}/boot_app0.bin"

# Flashers read names, offsets and hashes from the manifest instead of
# hardcoding them (same layout as the Ambit's release manifest).
python3 - "${DIST}" "${VERSION}" "${APP_NAME}" <<'EOF'
import hashlib, json, sys
dist, version, app_name = sys.argv[1], sys.argv[2], sys.argv[3]
def entry(file, offset):
    data = open(f"{dist}/{file}", "rb").read()
    return {"file": file, "offset": offset,
            "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
manifest = {
    "name": "minipar",
    "version": version,
    "chip": "esp32c3",
    "flash": [
        entry("bootloader.bin", "0x0"),
        entry("partitions.bin", "0x8000"),
        entry("boot_app0.bin", "0xe000"),
        entry(app_name, "0x10000"),
    ],
    "ota": {"file": app_name},
}
json.dump(manifest, open(f"{dist}/manifest.json", "w"), indent=2)
EOF

echo "version=${VERSION}"
echo "app=${APP_NAME}"
