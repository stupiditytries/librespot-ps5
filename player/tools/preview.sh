#!/bin/bash
# Builds the player's PC preview on Windows (clang with Visual Studio's CMake and Ninja) and,
# given an output folder, runs it there.
#   preview.sh <output folder> [options] [steps...]
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
vs="/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake"
export PATH="$vs/Ninja:/c/Program Files/LLVM/bin:$PATH"
cmake="$vs/CMake/bin/cmake.exe"
build=$here/../build/player-preview
mkdir -p "$build"
if [[ ! -f $build/build.ninja ]]; then
    "$cmake" -S "$here" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$build.configure.log" 2>&1 ||
        { tail -30 "$build.configure.log"; exit 1; }
fi
"$cmake" --build "$build" > "$build.log" 2>&1 || { grep -E "error|FAILED|undefined" "$build.log" | head -40; exit 1; }
grep -E "warning:" "$build.log" | grep -v "/borealis/\|/glfw" | head -20 || true

if [[ $# -ge 1 ]]; then
    mkdir -p "$1"
    "$build/player_preview.exe" "$@"
fi
