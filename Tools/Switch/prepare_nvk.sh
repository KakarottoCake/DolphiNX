#!/usr/bin/env bash
# Extract and localize the static NVK driver supplied for the Switch port.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ZIP="${1:-${DOLPHIN_SWITCH_NVK_ZIP:-}}"
OUT="${2:-${ROOT}/build-switch/nvk}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
BIN="${DEVKITPRO}/devkitA64/bin"
LD="${BIN}/aarch64-none-elf-ld"
OBJCOPY="${BIN}/aarch64-none-elf-objcopy"
NM="${BIN}/aarch64-none-elf-nm"

if command -v bsdtar >/dev/null 2>&1; then
  BSDTAR="$(command -v bsdtar)"
elif [[ -x /c/Windows/System32/tar.exe ]]; then
  BSDTAR=/c/Windows/System32/tar.exe
else
  echo "A bsdtar-compatible archive tool is required to read ${ZIP}" >&2
  exit 1
fi

if [[ -z "${ZIP}" ]]; then
  echo "Usage: $0 <switch-nvk.zip|mesa-switch-vulkan-sdk.zip|builddir-switch.zip> [output-directory]" >&2
  echo "Alternatively set DOLPHIN_SWITCH_NVK_ZIP." >&2
  exit 1
fi

if [[ ! -f "${ZIP}" ]]; then
  echo "NVK bundle not found: ${ZIP}" >&2
  exit 1
fi

if [[ ! -d "${OUT}/lib" ]]; then
  mkdir -p "${OUT}/lib"
fi
if [[ ! -d "${OUT}/tmp" ]]; then
  mkdir -p "${OUT}/tmp"
fi
export TMPDIR="${OUT}/tmp"
STAGE="$(mktemp -d "${TMPDIR}/extract.XXXXXX")"
trap 'rm -rf -- "${STAGE}"' EXIT

LEGACY_ARCHIVES=(
  src/nouveau/vulkan/libnvk.a
  src/vulkan/wsi/libvulkan_wsi.a
  src/vulkan/runtime/libvulkan_runtime.a
  src/vulkan/runtime/libvulkan_instance.a
  src/vulkan/util/libvulkan_util.a
  src/vulkan/runtime/libvulkan_lite_runtime.a
  src/vulkan/runtime/libvulkan_lite_instance.a
  src/nouveau/nil/libnil.a
  src/nouveau/nil/liblibnil_format_table.a
  src/nouveau/compiler/libnak.a
  src/nouveau/compiler/libnak_rs.a
  src/nouveau/mme/libnouveau_mme.a
  src/nouveau/winsys/libnouveau_ws.a
  src/nouveau/headers/libnvidia_headers_c.a
  src/compiler/nir/libnir.a
  src/compiler/spirv/libvtn.a
  src/compiler/libcompiler.a
  src/compiler/rust/libcompiler_c_helpers.a
  src/util/blake3/libblake3.a
  src/util/libmesa_util.a
  src/c11/impl/libmesa_util_c11.a
  src/util/libxmlconfig.a
)

SDK_ARCHIVES=(
  libnvk.a
  libvulkan_wsi.a
  libvulkan_runtime.a
  libvulkan_instance.a
  libvulkan_util.a
  libvulkan_lite_runtime.a
  libvulkan_lite_instance.a
  libnil.a
  liblibnil_format_table.a
  libnak.a
  libnak_rs.a
  libnouveau_mme.a
  libnouveau_ws.a
  libnvidia_headers_c.a
  libnir.a
  libvtn.a
  libcompiler.a
  libcompiler_c_helpers.a
  libblake3.a
  libmesa_util.a
  libmesa_util_simd.a
  libmesa_util_c11.a
  libxmlconfig.a
)

ARCHIVE_LIST="$("${BSDTAR}" -tf "${ZIP}")"
SDK_ROOT="$(printf '%s\n' "${ARCHIVE_LIST}" | awk '
  !found && /\/share\/nvk-switch\/build-info\.txt$/ {
    sub(/\/share\/nvk-switch\/build-info\.txt$/, "");
    print;
    found = 1;
  }
')"
SDK_PACKAGE=false
HAYATOG_ROOT="$(printf '%s\n' "${ARCHIVE_LIST}" | awk '
  !found && /\/lib\/libvulkan\.a$/ {
    sub(/\/lib\/libvulkan\.a$/, "");
    print;
    found = 1;
  }
')"
HAYATOG_PACKAGE=false

echo "Extracting the required NVK archives ..."
if [[ -n "${SDK_ROOT}" ]]; then
  SDK_PACKAGE=true
  SDK_PATHS=()
  for archive in "${SDK_ARCHIVES[@]}"; do
    SDK_PATHS+=("${SDK_ROOT}/lib/${archive}")
  done
  SDK_PATHS+=("${SDK_ROOT}/share/nvk-switch/build-info.txt")
  "${BSDTAR}" -xf "${ZIP}" -C "${STAGE}" "${SDK_PATHS[@]}"
  for archive in "${SDK_ARCHIVES[@]}"; do
    cp "${STAGE}/${SDK_ROOT}/lib/${archive}" "${OUT}/lib/${archive}"
  done
  cp "${STAGE}/${SDK_ROOT}/share/nvk-switch/build-info.txt" "${OUT}/build-info.txt"
  rm -f "${OUT}/flavor.txt"
  echo "Detected packaged NVK SDK: ${SDK_ROOT}"
elif [[ -n "${HAYATOG_ROOT}" ]]; then
  HAYATOG_PACKAGE=true
  "${BSDTAR}" -xf "${ZIP}" -C "${STAGE}" "${HAYATOG_ROOT}/lib/libvulkan.a"
  cp "${STAGE}/${HAYATOG_ROOT}/lib/libvulkan.a" "${OUT}/lib/libvulkan.a"
  rm -f "${OUT}/build-info.txt"
  printf '%s\n' "hayatog" > "${OUT}/flavor.txt"
  echo "Detected HayatoG switch-nvk package: ${HAYATOG_ROOT}"
else
  ZIP_PATHS=()
  for archive in "${LEGACY_ARCHIVES[@]}"; do
    ZIP_PATHS+=("builddir-switch/${archive}")
  done
  "${BSDTAR}" -xf "${ZIP}" -C "${STAGE}" "${ZIP_PATHS[@]}"
  for archive in "${LEGACY_ARCHIVES[@]}"; do
    cp "${STAGE}/builddir-switch/${archive}" "${OUT}/lib/$(basename "${archive}")"
  done
  rm -f "${OUT}/lib/libmesa_util_simd.a" "${OUT}/build-info.txt" "${OUT}/flavor.txt"
fi

LIB="${OUT}/lib"
SIMD_ARCHIVE=()
if [[ -f "${LIB}/libmesa_util_simd.a" ]]; then
  SIMD_ARCHIVE+=("${LIB}/libmesa_util_simd.a")
fi
MERGED="${OUT}/libnvk_merged.o"
RENAMED="${OUT}/libnvk_renamed.o"
REDEFINE_MAP="${OUT}/redefine-syms.txt"
LOCAL_TMP="${OUT}/libnvk_local.o.tmp"
LOCAL="${OUT}/libnvk_local.o"

echo "Merging the static NVK driver ..."
if [[ "${HAYATOG_PACKAGE}" == true ]]; then
  "${LD}" -r \
    --allow-multiple-definition \
    --whole-archive "${LIB}/libvulkan.a" --no-whole-archive \
    -o "${MERGED}"
elif [[ "${SDK_PACKAGE}" == true ]]; then
  "${LD}" -r \
    --whole-archive "${LIB}/libnvk.a" --no-whole-archive \
    --start-group \
    "${LIB}/libvulkan_lite_runtime.a" \
    "${LIB}/libvulkan_runtime.a" \
    "${LIB}/libvulkan_lite_instance.a" \
    "${LIB}/libvulkan_instance.a" \
    "${LIB}/libvulkan_util.a" \
    "${LIB}/libvulkan_wsi.a" \
    "${LIB}/libnak.a" \
    "${LIB}/libnak_rs.a" \
    "${LIB}/libvtn.a" \
    "${LIB}/libxmlconfig.a" \
    "${LIB}/libnil.a" \
    "${LIB}/liblibnil_format_table.a" \
    "${LIB}/libnouveau_mme.a" \
    "${LIB}/libnouveau_ws.a" \
    "${LIB}/libnvidia_headers_c.a" \
    "${LIB}/libnir.a" \
    "${LIB}/libcompiler.a" \
    "${LIB}/libcompiler_c_helpers.a" \
    "${LIB}/libmesa_util.a" \
    "${SIMD_ARCHIVE[@]}" \
    "${LIB}/libblake3.a" \
    "${LIB}/libmesa_util_c11.a" \
    --end-group \
    -o "${MERGED}"
else
  "${LD}" -r \
    --whole-archive \
    "${LIB}/libnvk.a" \
    "${LIB}/libvulkan_wsi.a" \
    "${LIB}/libvulkan_runtime.a" \
    "${LIB}/libvulkan_instance.a" \
    "${LIB}/libvulkan_util.a" \
    --no-whole-archive \
    --start-group \
    "${LIB}/libvulkan_lite_runtime.a" \
    "${LIB}/libvulkan_lite_instance.a" \
    "${LIB}/libnil.a" \
    "${LIB}/liblibnil_format_table.a" \
    "${LIB}/libnak.a" \
    "${LIB}/libnak_rs.a" \
    "${LIB}/libnouveau_mme.a" \
    "${LIB}/libnouveau_ws.a" \
    "${LIB}/libnvidia_headers_c.a" \
    "${LIB}/libnir.a" \
    "${LIB}/libvtn.a" \
    "${LIB}/libcompiler.a" \
    "${LIB}/libcompiler_c_helpers.a" \
    "${LIB}/libblake3.a" \
    "${LIB}/libmesa_util.a" \
    "${LIB}/libmesa_util_c11.a" \
    "${LIB}/libxmlconfig.a" \
    --end-group \
    -o "${MERGED}"
fi

LOCALIZE_INPUT="${MERGED}"
if [[ "${HAYATOG_PACKAGE}" == true ]]; then
  # The packaged driver and devkitPro's SDL/EGL stack contain different generations of
  # Nouveau's C++ codegen. Give every private NVK definition a unique name before localization
  # so ELF COMDAT selection cannot discard one generation's implementation in favor of the other.
  "${NM}" -g --defined-only --format=posix "${MERGED}" |
    awk '
      $1 != "vk_icdGetInstanceProcAddr" &&
      $1 != "vk_icdNegotiateLoaderICDInterfaceVersion" &&
      $1 != "vk_icdGetPhysicalDeviceProcAddr" &&
      $1 != "__wrap_open" &&
      $1 != "__wrap_close" &&
      $1 != "__wrap_stat" &&
      $1 != "__wrap_lstat" &&
      $1 != "__wrap_vk_icdGetInstanceProcAddr" {
        print $1, "dolphin_nvk_local_" $1
      }
    ' > "${REDEFINE_MAP}"
  "${OBJCOPY}" --redefine-syms="${REDEFINE_MAP}" "${MERGED}" "${RENAMED}"
  LOCALIZE_INPUT="${RENAMED}"
fi

echo "Localizing NVK globals while retaining the Vulkan ICD entry points ..."
"${OBJCOPY}" \
  --keep-global-symbol=vk_icdGetInstanceProcAddr \
  --keep-global-symbol=vk_icdNegotiateLoaderICDInterfaceVersion \
  --keep-global-symbol=vk_icdGetPhysicalDeviceProcAddr \
  --keep-global-symbol=__wrap_open \
  --keep-global-symbol=__wrap_close \
  --keep-global-symbol=__wrap_stat \
  --keep-global-symbol=__wrap_lstat \
  --keep-global-symbol=__wrap_vk_icdGetInstanceProcAddr \
  "${LOCALIZE_INPUT}" "${LOCAL_TMP}"

if [[ ! -f "${LOCAL}" ]] || ! cmp -s "${LOCAL_TMP}" "${LOCAL}"; then
  mv -f "${LOCAL_TMP}" "${LOCAL}"
else
  rm -f "${LOCAL_TMP}"
fi
rm -f "${MERGED}" "${RENAMED}" "${REDEFINE_MAP}"

echo "Prepared ${LOCAL}"
