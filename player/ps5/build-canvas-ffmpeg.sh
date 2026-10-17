#!/usr/bin/env bash
# Silent Canvas codec set. No network, file protocol, audio codec, or audio output.
set -euo pipefail
: "${PLAYER_FFMPEG_SOURCE:?Set PLAYER_FFMPEG_SOURCE to FFmpeg c7b5f1537d sources}"
: "${PLAYER_FFMPEG_WORK:?Set PLAYER_FFMPEG_WORK to a dedicated build directory}"
: "${PS5_PAYLOAD_SDK:?Set PS5_PAYLOAD_SDK}"
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$PLAYER_FFMPEG_WORK"
cd "$PLAYER_FFMPEG_WORK"
bash "$PLAYER_FFMPEG_SOURCE/configure" --prefix="$PLAYER_FFMPEG_WORK/install" \
 --enable-cross-compile --arch=x86_64 --target-os=freebsd --cc="$here/ffmpeg-cc.sh" --ld="$here/ffmpeg-cc.sh" \
 --ar=llvm-ar-18 --ranlib=llvm-ranlib-18 --nm=llvm-nm-18 --x86asmexe=nasm \
 --disable-autodetect --disable-everything --disable-programs --disable-doc --disable-debug \
 --disable-avdevice --disable-avfilter --disable-swresample --disable-network \
 --enable-decoder=h264 --enable-parser=h264 --enable-demuxer=mov \
 --enable-pic --enable-pthreads --disable-shared --enable-static
make -j"${PLAYER_BUILD_JOBS:-8}"
make install
mkdir -p install/licenses
cp "$PLAYER_FFMPEG_SOURCE/COPYING.LGPLv2.1" install/licenses/FFmpeg-LGPL-2.1.txt
cp "$PLAYER_FFMPEG_SOURCE/LICENSE.md" install/licenses/FFmpeg-LICENSE.md
