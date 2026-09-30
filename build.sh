#!/usr/bin/env bash
set -euo pipefail

EXE_NAME="awaaz"
CC="clang"
CXX="clang++"

BUILD_PATH=$(realpath "./build")
SRC=$(realpath "./src/")

BACE_PATH=$(realpath "./external/bace/")
BACE_OBJ="bace.o"

CIMGUI_PATH=$(realpath "./external/cimgui/")
SDL_PATH=$(realpath "./external/SDL/")

build_sdl() {
    echo "== building sdl3 =="
    cmake -S "$SDL_PATH" -B "$BUILD_PATH/sdl" \
        -DSDL_STATIC=on \
        -DSDL_SHARED=off

    cmake --build "$BUILD_PATH/sdl" --config Debug
}

build_cimgui() {
    echo "== building cimgui =="
    cmake -S "$CIMGUI_PATH" -B "$BUILD_PATH/cimgui"
    cmake --build "$BUILD_PATH/cimgui" --config Debug
}


build_bace() {
    echo "== building bace =="
    cd "$BACE_PATH" && BUILD_PATH=$BUILD_PATH ./build.sh
}

build_external() {
    # git submodule update --init --recursive
    (build_bace) && (build_cimgui) && (build_sdl)
}


# debug flags
DEBUG_FLAGS=(
    "-g"
)

# compile time defines
DEFINES=(
    "-D_DEFAULT_SOURCE"
)

# platform libraries
LIBS=(
    "-L$BUILD_PATH"
    "-L$BUILD_PATH/sdl/"
    "$BUILD_PATH/cimgui/cimgui/cimgui.a"
    "$BACE_OBJ"
    "-lSDL3"
    "-lm"
    "-lasound"
    "-lpthread"
    "-lstdc++"
    "-lpulse"
    "-lpulse-simple"
)

# compiler flags
COMP_FLAGS=(
    "-I$BACE_PATH/include"
    "-I$CIMGUI_PATH/cimgui"
    "-I$SDL_PATH/include"
    "-Wall"
    "-Wpedantic"
    "-Wextra"
    "-Werror"
    "-Og"
    "-std=c11"
)

# build commands
EXE_CMD=("$CC" "${DEFINES[@]}" "${DEBUG_FLAGS[@]}" "${COMP_FLAGS[@]}" \
         "-o" "$BUILD_PATH/$EXE_NAME" "$SRC/main.c" "${LIBS[@]}")

EXE_CMD_STR=$(IFS=' '; echo "${EXE_CMD[*]}")

no_build_deps=false
do_build_shaders=false
for arg in "$@"; do
  case $arg in
    --no-build-deps|-D)
      no_build_deps=true
      ;;
    --build-shaders|-s)
      do_build_shaders=true
      ;;
  esac
done


# create build directory if it doesn't exist
if [[ ! -d "$BUILD_PATH" ]]; then
    echo "Created Build Directory"
    mkdir -p "$BUILD_PATH"
fi

if [[ $no_build_deps == "false" ]]; then
    echo "=== Building dependencies ==="
    if ! build_external; then
        echo "building dependencies failed"
        exit 1
    fi
fi

if [[ $do_build_shaders == "true" ]]; then
    echo "=== building shaders ==="
    if ! build_shaders; then
        echo "building shaders failed"
        exit 1
    fi
fi

cd "$BUILD_PATH" || exit
echo "===== $EXE_NAME ====="
echo "$EXE_CMD_STR"
eval "$EXE_CMD_STR"
cd ..
