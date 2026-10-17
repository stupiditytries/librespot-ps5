#!/usr/bin/env bash
# Fetches and prepares everything the console build needs, from public sources at pinned
# versions, then writes DESTINATION/env.sh. Used by GitHub Actions; works the same in WSL.
# Needs: git curl unzip make ninja-build clang-18 lld-18 llvm-18 nasm python3, rustup, and
# LLVM 21 (clang-21 lld-21 llvm-21 from apt.llvm.org), which the SDK's linker wrapper prefers.
set -euo pipefail
dest=${1:?Usage: setup-deps.sh DESTINATION}
repo=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$dest"
dest=$(cd "$dest" && pwd)

boilerplate_commit=dd44bbdc75437332ed22e3ba95126733419ef25a
borealis_commit=5f08b286f3df737f3321d2247a6fe633fcead03c
ffmpeg_commit=c7b5f1537d9c52efa50fd10d106ca015ddde1818
gl_sdk_url=https://github.com/blackbearreloaded/ps5-opengl/releases/download/v1.0.0/ps5-opengl-sdk-1.0.0.tar.gz
gl_sdk_sha256=f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5
gl_library_sha256=11ab03a347cf75e143c7aa896ff0fd94714ded471a92146aa0f2aba82722ae95
json_url=https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp
json_sha256=665fa14b8af3837966949e8eb0052d583e2ac105d3438baba9951785512cf921
rust_toolchain=nightly-2026-10-03

# A checkout of exactly one commit, verified by its hash.
checkout() {
    local url=$1 commit=$2 directory=$3
    if [[ -d $directory/.git ]] && [[ $(git -C "$directory" rev-parse HEAD) == "$commit" ]]; then return; fi
    rm -rf "$directory"
    git init -q "$directory"
    git -C "$directory" fetch -q --depth 1 "$url" "$commit"
    git -C "$directory" -c advice.detachedHead=false checkout -q FETCH_HEAD
    [[ $(git -C "$directory" rev-parse HEAD) == "$commit" ]]
}

# 1. ps5-native-app-boilerplate: compiler wrapper, host tools, clean-room libc.prx, and the
#    PS5 Payload SDK v0.42 (its own script downloads and checks it).
template=$dest/ps5-native-app-boilerplate
checkout https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git "$boilerplate_commit" "$template"
bash "$template/tools/setup-native-dependencies.sh"
USE_CCACHE=0 bash "$template/tools/rebuild-libc.sh"
(cd "$template/runtime" && sha256sum --check --strict libc.prx.sha256)
sdk=$template/.deps/native/ps5-payload-sdk

# 2. PS5 OpenGL SDK 1.0.0 release.
if [[ ! -f $dest/ps5-opengl-sdk/.verified ]]; then
    curl --fail --location --retry 3 --output "$dest/ps5-opengl-sdk.tar.gz" "$gl_sdk_url"
    printf '%s  %s\n' "$gl_sdk_sha256" "$dest/ps5-opengl-sdk.tar.gz" | sha256sum --check --strict
    rm -rf "$dest/ps5-opengl-sdk"
    mkdir -p "$dest/ps5-opengl-sdk"
    tar -xzf "$dest/ps5-opengl-sdk.tar.gz" -C "$dest/ps5-opengl-sdk" --strip-components=1
    rm "$dest/ps5-opengl-sdk.tar.gz"
    touch "$dest/ps5-opengl-sdk/.verified"
fi
gl_sdk=$dest/ps5-opengl-sdk/sdk
printf '%s  %s\n' "$gl_library_sha256" "$gl_sdk/lib/libPS5OpenGL.a" | sha256sum --check --strict

# 3. Borealis, the app's UI library (no submodules are needed for the console).
checkout https://github.com/xfangfang/borealis.git "$borealis_commit" "$dest/borealis"

# 4. FFmpeg with only the H.264 decoder Canvas videos need.
if [[ ! -f $dest/ffmpeg-work/install/lib/libavcodec.a ]]; then
    checkout https://github.com/FFmpeg/FFmpeg.git "$ffmpeg_commit" "$dest/ffmpeg"
    rm -rf "$dest/ffmpeg-work"
    PS5_PAYLOAD_SDK=$sdk PLAYER_FFMPEG_SOURCE=$dest/ffmpeg PLAYER_FFMPEG_WORK=$dest/ffmpeg-work \
        PLAYER_BUILD_JOBS=$(nproc) bash "$repo/player/ps5/build-canvas-ffmpeg.sh"
fi

# 5. nlohmann/json, header only.
json=$repo/.deps/nlohmann/include/nlohmann/json.hpp
if ! printf '%s  %s\n' "$json_sha256" "$json" | sha256sum --check --strict --status 2>/dev/null; then
    mkdir -p "$(dirname "$json")"
    curl --fail --location --retry 3 --output "$json" "$json_url"
    printf '%s  %s\n' "$json_sha256" "$json" | sha256sum --check --strict
fi

# 6. The Rust nightly the receiver was developed with (it builds std for the console).
rustup toolchain install "$rust_toolchain" --profile minimal --component rust-src

cat > "$dest/env.sh" <<ENV
export PS5_TEMPLATE='$template'
export PS5_PAYLOAD_SDK='$sdk'
export GL_SDK='$gl_sdk'
export PLAYER_BOREALIS_DIR='$dest/borealis/library'
export PLAYER_FFMPEG_DIR='$dest/ffmpeg-work/install'
export PLAYER_WORK='$dest/work/player'
export WORK='$dest/work/receiver'
export RUSTUP_TOOLCHAIN='$rust_toolchain'
export USE_CCACHE=0
ENV
echo "Dependencies ready; source $dest/env.sh"
