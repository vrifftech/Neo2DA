#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_NAME="$(basename "$ROOT_DIR")"
CMAKE_BIN="${CMAKE:-cmake}"
BUILD_DIR="$ROOT_DIR/build"
BUILD_TYPE="Release"
BUILD_WX="ON"
REQUIRE_WX="OFF"
BUILD_CLI="ON"
JOBS="${JOBS:-}"
GENERATOR=""
TARGET=""
CLEAN=0
EXTRA=()

usage() {
  cat <<USAGE
usage: ./scripts/build.sh [options] [-- extra-cmake-args...]

Options:
  --build-dir DIR        Build directory [default: ./build]
  --build-type TYPE      Debug, Release, RelWithDebInfo, or MinSizeRel [default: Release]
  --wx ON|OFF            Build the wxWidgets GUI target when wxWidgets is available [default: ON]
  --require-wx ON|OFF    Fail configure if wxWidgets is unavailable [default: OFF]
  --cli ON|OFF           Build the command-line target [default: ON]
  --jobs N               Parallel build jobs [default: cmake default]
  --target NAME          Build a specific CMake target
  --generator NAME       CMake generator, for example Ninja or "Visual Studio 17 2022"
  --clean                Delete the build directory before configuring
  -h, --help             Show this help

Examples:
  ./scripts/build.sh --wx OFF --jobs "$(nproc)"
  ./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2;;
    --build-type) BUILD_TYPE="$2"; shift 2;;
    --wx) BUILD_WX="$2"; shift 2;;
    --require-wx) REQUIRE_WX="$2"; shift 2;;
    --cli) BUILD_CLI="$2"; shift 2;;
    --jobs|--parallel) JOBS="$2"; shift 2;;
    --target) TARGET="$2"; shift 2;;
    --generator) GENERATOR="$2"; shift 2;;
    --clean) CLEAN=1; shift;;
    -h|--help) usage; exit 0;;
    --) shift; EXTRA+=("$@"); break;;
    *) EXTRA+=("$1"); shift;;
  esac
done

PROJECT_PREFIX="$(printf '%s' "$PROJECT_NAME" | tr -cd '[:alnum:]' | tr '[:lower:]' '[:upper:]')"
WX_OPTION="${PROJECT_PREFIX}_BUILD_WX_GUI"
REQUIRE_WX_OPTION="${PROJECT_PREFIX}_REQUIRE_WX_GUI"
CLI_OPTION="${PROJECT_PREFIX}_BUILD_CLI"

[[ "$CLEAN" == 1 ]] && rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

CONFIG_ARGS=(-S "$ROOT_DIR" -B "$BUILD_DIR" "-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
[[ -n "$GENERATOR" ]] && CONFIG_ARGS=(-G "$GENERATOR" "${CONFIG_ARGS[@]}")
CONFIG_ARGS+=("-D$WX_OPTION=$BUILD_WX" "-D$REQUIRE_WX_OPTION=$REQUIRE_WX" "-D$CLI_OPTION=$BUILD_CLI")
CONFIG_ARGS+=("${EXTRA[@]}")

echo "Configuring $PROJECT_NAME in $BUILD_DIR"
"$CMAKE_BIN" "${CONFIG_ARGS[@]}"

BUILD_ARGS=(--build "$BUILD_DIR" --config "$BUILD_TYPE")
[[ -n "$JOBS" ]] && BUILD_ARGS+=(--parallel "$JOBS")
[[ -n "$TARGET" ]] && BUILD_ARGS+=(--target "$TARGET")

echo "Building $PROJECT_NAME"
"$CMAKE_BIN" "${BUILD_ARGS[@]}"
