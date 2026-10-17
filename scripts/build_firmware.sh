#!/usr/bin/env bash
set -euo pipefail

# Reproducible ESP32-C6 build entry point.  The ESP-IDF checkout, compiler and
# Python environment are cached under .cache/ and reused on every later build.
# Set IDF_PATH to use an existing installation (for example /opt/esp/idf in the
# project's dev container) or POOL_IDF_CACHE to put the persistent cache on a
# different disk.

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
idf_version="${POOL_IDF_VERSION:-v5.5}"
cache_root="${POOL_IDF_CACHE:-${project_root}/.cache/idf-${idf_version}}"
cached_idf="${cache_root}/esp-idf"
cached_tools="${cache_root}/tools"

use_idf="${IDF_PATH:-}"
if [[ -z "${use_idf}" && -f /opt/esp/idf/export.sh ]]; then
    use_idf=/opt/esp/idf
    export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-/opt/esp}"
fi

if [[ -z "${use_idf}" || ! -f "${use_idf}/export.sh" ]]; then
    use_idf="${cached_idf}"
fi

export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-${cached_tools}}"

if [[ ! -f "${use_idf}/export.sh" ]]; then
    mkdir -p "${cache_root}"
    echo "Installing ESP-IDF ${idf_version} once into ${cache_root}"
    git clone --branch "${idf_version}" --depth 1 --recurse-submodules \
        --shallow-submodules https://github.com/espressif/esp-idf.git "${use_idf}"
fi

if ! compgen -G "${IDF_TOOLS_PATH}/python_env/idf*_env/bin/python" >/dev/null; then
    # Handles both a first install and an existing/copy-restored IDF checkout
    # whose separately cached compiler/Python tools are absent. install.sh is
    # idempotent and downloads only missing parts.
    "${use_idf}/install.sh" esp32c6
fi

export IDF_PATH="${use_idf}"
# export.sh deliberately mutates PATH and other shell state.
# shellcheck disable=SC1091
source "${IDF_PATH}/export.sh" >/dev/null

# The only registry components used by the firmware are vendored under
# components/. Keeping component-manager disabled makes builds deterministic,
# avoids a network lookup on every fresh configuration, and works in restricted
# containers where psutil cannot inspect CMake's parent process.
export IDF_COMPONENT_MANAGER=0

# Minimal Codex/CI images may lack the two host-side build executables even
# after ESP-IDF's own installer has supplied its compiler and Python packages.
# Keep those in the same persistent environment rather than requiring apt or a
# second manual setup step.
if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
    "${IDF_PYTHON_ENV_PATH}/bin/python" -m pip install 'cmake<4' ninja
fi

cd "${project_root}"
echo "ESP-IDF: ${IDF_PATH}"
echo "Project version: ${PROJECT_VER:-derived from git}"

# CMake records the absolute Python path used when the build directory was
# configured. A copied project, or switching from a global ESP-IDF install to
# this project's cached one, otherwise makes idf.py stop and demand a manual
# fullclean. Detect that stale build metadata and clean it automatically.
if [[ -f build/CMakeCache.txt ]]; then
    configured_python="$(sed -n 's/^PYTHON:[^=]*=//p' build/CMakeCache.txt | head -n 1)"
    active_python="${IDF_PYTHON_ENV_PATH}/bin/python"
    if [[ -n "${configured_python}" && "${configured_python}" != "${active_python}" ]]; then
        echo "Build cache belongs to a different ESP-IDF Python environment; cleaning it once"
        idf.py fullclean
    fi
fi

if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32c6"$' sdkconfig; then
    idf.py set-target esp32c6
fi

exec idf.py build "$@"
