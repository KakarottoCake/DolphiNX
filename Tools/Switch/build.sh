#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${DOLPHIN_SWITCH_BUILD_DIR:-${ROOT}/build-switch}"
NVK_ZIP="${1:-${DOLPHIN_SWITCH_NVK_ZIP:-}}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITA64="${DEVKITA64:-${DEVKITPRO}/devkitA64}"
JOBS="${DOLPHIN_SWITCH_JOBS:-18}"
VERSION="${DOLPHIN_SWITCH_VERSION:-0.1.0}"
NVK_SHA256="${DOLPHIN_SWITCH_NVK_SHA256:-}"

if [[ -z "${NVK_ZIP}" ]]; then
  echo "Usage: $0 <mesa-switch-vulkan-sdk.zip|builddir-switch.zip>" >&2
  echo "Alternatively set DOLPHIN_SWITCH_NVK_ZIP." >&2
  exit 1
fi

if [[ ! -f "${DEVKITPRO}/cmake/Switch.cmake" ]]; then
  echo "A devkitPro installation with libnx was not found at ${DEVKITPRO}." >&2
  exit 1
fi

if [[ ! -f "${NVK_ZIP}" ]]; then
  echo "NVK bundle not found: ${NVK_ZIP}" >&2
  exit 1
fi

export DEVKITPRO DEVKITA64
export PATH="${DEVKITPRO}/tools/bin:${PATH}"

if [[ -n "${NVK_SHA256}" ]]; then
  if ! command -v sha256sum >/dev/null 2>&1; then
    echo "sha256sum is required when DOLPHIN_SWITCH_NVK_SHA256 is set." >&2
    exit 1
  fi
  if [[ ! "${NVK_SHA256}" =~ ^[0-9a-fA-F]{64}$ ]]; then
    echo "DOLPHIN_SWITCH_NVK_SHA256 must be a 64-character SHA-256 value." >&2
    exit 1
  fi
  ACTUAL_NVK_SHA256="$(sha256sum "${NVK_ZIP}" | awk '{print $1}')"
  if [[ "${ACTUAL_NVK_SHA256,,}" != "${NVK_SHA256,,}" ]]; then
    echo "NVK bundle checksum mismatch." >&2
    echo "Expected: ${NVK_SHA256,,}" >&2
    echo "Actual:   ${ACTUAL_NVK_SHA256,,}" >&2
    exit 1
  fi
fi

MISSING_PACKAGES=()
for package in sdl2 SDL2_ttf SDL2_image libcurl; do
  if ! PKG_CONFIG_PATH="${DEVKITPRO}/portlibs/switch/lib/pkgconfig" \
       pkg-config --exists "${package}"; then
    MISSING_PACKAGES+=("${package}")
  fi
done
if (( ${#MISSING_PACKAGES[@]} != 0 )); then
  echo "Missing Switch portlibs: ${MISSING_PACKAGES[*]}" >&2
  echo "Install the matching devkitPro switch packages before building." >&2
  exit 1
fi

if [[ ! -d "${BUILD_DIR}/tmp" ]]; then
  mkdir -p "${BUILD_DIR}/tmp"
fi
export TMPDIR="${BUILD_DIR}/tmp"

# devkitA64 uses Windows TEMP paths under MSYS2.
if command -v cygpath >/dev/null 2>&1; then
  SWITCH_TMP_WINDOWS="$(cygpath -w "${TMPDIR}")"
  export TEMP="${SWITCH_TMP_WINDOWS}"
  export TMP="${SWITCH_TMP_WINDOWS}"
else
  export TEMP="${TMPDIR}"
  export TMP="${TMPDIR}"
fi

"${ROOT}/Tools/Switch/prepare_nvk.sh" "${NVK_ZIP}" "${BUILD_DIR}/nvk"

cmake -S "${ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="${ROOT}/CMake/Toolchain-Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDISTRIBUTOR="Dolphin Switch" \
  -DDOLPHIN_SWITCH_VERSION="${VERSION}" \
  -DENABLE_LTO=ON

cmake --build "${BUILD_DIR}" --target dolphin_nro --parallel "${JOBS}"

echo "Built ${BUILD_DIR}/Binaries/dolphin.nro"
