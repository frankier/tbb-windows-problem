#!/usr/bin/env bash
# Cross-build the reproducer with MSVC under Wine. This is how the expected
# results in README.md were verified on Linux; a real Windows machine only needs
# scripts/build.bat.
#
# Requires cmake, ninja, wine and an msvc-wine installation whose cl wrapper is
# in MSVC_BIN. Example:
#
#   MSVC_BIN=$HOME/msvc/bin/x64 WINEPREFIX=$HOME/msvc/wineprefix \
#       scripts/build-wine.sh
#
# Environment:
#   MSVC_BIN      directory that contains the msvc-wine cl wrapper (required)
#   WINEPREFIX    Wine prefix used for the toolchain (optional)
#   BUILD_DIR     build directory (default: <repo>/build-wine)
#   ONETBB_GIT_TAG, ONETBB_GIT_REPOSITORY   passed through to CMake

set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${MSVC_BIN:?set MSVC_BIN to the msvc-wine bin/<arch> directory}"
BUILD=${BUILD_DIR:-"$ROOT/build-wine"}

export PATH="$MSVC_BIN:$PATH"

EXTRA=()
if [[ -n "${ONETBB_GIT_TAG:-}" ]]; then
    EXTRA+=("-DONETBB_GIT_TAG=$ONETBB_GIT_TAG")
fi
if [[ -n "${ONETBB_GIT_REPOSITORY:-}" ]]; then
    EXTRA+=("-DONETBB_GIT_REPOSITORY=$ONETBB_GIT_REPOSITORY")
fi

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=cl \
    ${EXTRA[@]+"${EXTRA[@]}"}
cmake --build "$BUILD"

echo "staged: $BUILD/dist"
