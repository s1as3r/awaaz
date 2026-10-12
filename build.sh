#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$(realpath "${BASH_SOURCE[0]}")")"

EXE_NAME="awaaz"
CC="${CC:-clang}"

BUILD_TYPE="Debug"
no_build_deps=false
for arg in "$@"; do
  case $arg in
    --no-build-deps|-D) no_build_deps=true ;;
    --release|-r) BUILD_TYPE="Release" ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

mkdir -p ./build

BUILD_PATH=$(realpath "./build")
SRC=$(realpath "./src")
BACE_PATH=$(realpath "./external/bace")
CIMGUI_PATH=$(realpath "./external/cimgui")
SDL_PATH=$(realpath "./external/SDL")
BACE_OBJ="$BUILD_PATH/bace.o"

build_sdl() {
  echo "== building sdl3 =="
  cmake -S "$SDL_PATH" -B "$BUILD_PATH/sdl" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DSDL_STATIC=on -DSDL_SHARED=off
  cmake --build "$BUILD_PATH/sdl" --config "$BUILD_TYPE"
}

build_cimgui() {
  echo "== building cimgui =="
  cmake -S "$CIMGUI_PATH" -B "$BUILD_PATH/cimgui" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build "$BUILD_PATH/cimgui" --config "$BUILD_TYPE"
}

build_bace() {
  echo "== building bace =="
  (cd "$BACE_PATH" && BUILD_PATH="$BUILD_PATH" ./build.sh)
}

if [[ $BUILD_TYPE == "Release" ]]; then
  OPT_FLAGS=(-O2 -DNDEBUG)
else
  OPT_FLAGS=(-g -Og)
fi

DEFINES=(
  "-D_DEFAULT_SOURCE"
)

LIBS=(
  "-L$BUILD_PATH/sdl/"
  "$BUILD_PATH/cimgui/cimgui/cimgui.a"
  "$BACE_OBJ"
  -lSDL3 -lm -lpthread -lstdc++ -lpulse -lpulse-simple
)

COMP_FLAGS=(
  "-I$BACE_PATH/include" "-I$CIMGUI_PATH/cimgui" "-I$SDL_PATH/include"
  -Wall -Wpedantic -Wextra -Werror
  -std=c11
)

if [[ $no_build_deps == false ]]; then
  echo "=== Building dependencies ($BUILD_TYPE) ==="
  build_bace
  build_cimgui
  build_sdl
fi

EXE_CMD=("$CC" "${DEFINES[@]}" "${OPT_FLAGS[@]}" "${COMP_FLAGS[@]}"
         -o "$BUILD_PATH/$EXE_NAME" "$SRC/main.c" "${LIBS[@]}")

echo "===== $EXE_NAME ($BUILD_TYPE) ====="
echo "${EXE_CMD[*]}"
"${EXE_CMD[@]}"
