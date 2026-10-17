#!/usr/bin/env bash
# Build the Borealis folder app with the PS5 Payload and OpenGL SDKs.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
if [[ -f "$repo/../.local/build-env.sh" ]];then source "$repo/../.local/build-env.sh";fi
: "${PS5_TEMPLATE:?Set PS5_TEMPLATE to ps5-native-app-boilerplate}"
: "${GL_SDK:?Set GL_SDK to the PS5 OpenGL SDK sdk folder}"
: "${PLAYER_FFMPEG_DIR:?Set PLAYER_FFMPEG_DIR to the PS5 FFmpeg static install prefix}"
: "${PLAYER_BOREALIS_DIR:?Set PLAYER_BOREALIS_DIR to borealis/library}"
sdk=${PS5_PAYLOAD_SDK:-$PS5_TEMPLATE/.deps/native/ps5-payload-sdk}
out=${PLAYER_WORK:-$repo/build/ps5}
brls=$PLAYER_BOREALIS_DIR
mkdir -p "$out/obj" "$out/stubs"
common=(-DPLAYER_CANVAS_FFMPEG -I"$PLAYER_FFMPEG_DIR/include" -O2 -DNDEBUG -ffunction-sections -fdata-sections -ffile-prefix-map="$repo"=player -ffile-prefix-map="$repo/.."=.
    -ffile-prefix-map="$brls"=/borealis -ffile-prefix-map="$PS5_TEMPLATE"=/sdk
    -I"$repo/src" -I"$repo/../.deps/nlohmann/include" -I"$GL_SDK/include"
    -I"$brls/include" -I"$brls/include/borealis/extern" -I"$brls/include/borealis/extern/nanovg"
    -I"$brls/include/borealis/extern/tinyxml2" -I"$brls/lib/extern/yoga"
    -I"$brls/lib/extern/tweeny/include" -I"$brls/lib/extern/fmt/include"
    -DBOREALIS_USE_OPENGL -DBOREALIS_USE_STD_THREAD -DYG_ENABLE_EVENTS -DGL_GLEXT_PROTOTYPES=1 '-DBRLS_RESOURCES=""')
sources=("$repo"/src/*.cpp "$repo"/ps5/app/src/*.cpp "$repo/host/i18n.cpp"
    "$repo"/ps5/app/compat/*.c "$repo/ps5/runtime/heap.c" "$repo/ps5/runtime/abort.cpp" "$repo/ps5/runtime/no_exec.c"
    "$PS5_TEMPLATE/tooling/native/app_crt.cpp"
    "$brls"/lib/core/*.cpp "$brls"/lib/core/touch/*.cpp "$brls"/lib/views/*.cpp
    "$brls"/lib/views/cells/*.cpp "$brls"/lib/views/widgets/*.cpp "$brls"/lib/extern/yoga/yoga/*.cpp
    "$brls"/lib/extern/yoga/yoga/event/*.cpp "$brls/lib/extern/nanovg/nanovg.c"
    "$brls/lib/extern/tinyxml2/tinyxml2.cpp" "$brls/lib/extern/fmt/src/format.cc"
    "$brls/lib/extern/libretro-common/compat/compat_strl.c" "$brls/lib/extern/libretro-common/encodings/encoding_utf.c"
    "$brls/lib/extern/libretro-common/features/features_cpu.c")
objects=()
for source in "${sources[@]}";do
    [[ $source == "$brls/lib/core/platform.cpp" || $source == "$brls/lib/core/i18n.cpp" ]]&&continue
    name=$(printf %s "$source"|sha256sum|cut -c1-20);object="$out/obj/$name.o";objects+=("$object")
    if [[ $source == "$brls/"* && $object -nt $source && $object -nt ${BASH_SOURCE[0]} ]];then continue;fi
    case $source in
        *.cpp|*.cc) PS5_PAYLOAD_SDK="$sdk" bash "$PS5_TEMPLATE/tooling/prospero-clang18" -std=c++20 -fexceptions -frtti "${common[@]}" -c "$source" -o "$object";;
        *) PS5_PAYLOAD_SDK="$sdk" bash "$PS5_TEMPLATE/tooling/prospero-clang18" -std=c11 -DAPP_HAS_UNWINDER "${common[@]}" -c "$source" -o "$object";;
    esac
done
cp "$sdk"/target/lib/*.so "$GL_SDK/lib/libSceAgc.so" "$GL_SDK/lib/libSceAgcDriver.so" "$out/stubs/"
"$sdk/bin/prospero-lld" -z nodynamic-undefined-weak --defsym=__cxa_thread_atexit_impl=0 \
    --defsym=__dlopen=0 --defsym=__dlsym=0 --defsym=__dladdr=0 --defsym=__dlclose=0 --defsym=__dlerror=0 \
    -L "$sdk/target/lib" -L "$GL_SDK/lib" -T "$PS5_TEMPLATE/tooling/native/ps5-pie.ld" -T "$repo/ps5/app/unwind.ld" \
    --eh-frame-hdr --gc-sections --version-script "$repo/ps5/app/app-symbols.map" -e _start -u ps5_agc_gate2_run \
    --error-limit=30 -Map="$out/player.map" -o "$out/player.elf" "${objects[@]}" \
    --start-group "$PLAYER_FFMPEG_DIR/lib/libavformat.a" "$PLAYER_FFMPEG_DIR/lib/libavcodec.a" "$PLAYER_FFMPEG_DIR/lib/libswscale.a" "$PLAYER_FFMPEG_DIR/lib/libavutil.a" -lPS5OpenGL "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" --end-group \
    --as-needed "$out/stubs/libSceAgc.so" "$out/stubs/libSceAgcDriver.so" \
    "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libkernel.so" "$sdk/target/lib/libSceVideoOut.so" \
    "$sdk/target/lib/libSceSystemService.so" "$sdk/target/lib/libSceUserService.so" "$sdk/target/lib/libScePad.so" \
    "$sdk/target/lib/libSceNet.so" "$sdk/target/lib/libSceImeDialog.so" "$sdk/target/lib/libSceSysmodule.so" "$sdk/target/lib/libc.a"
if readelf -d "$out/player.elf"|grep -E 'NEEDED.*lib(ScePosixForWebKit|SceRandom|kernel_sys|kernel_web|SceAudioOut)';then
    echo 'Unexpected app import' >&2;exit 1
fi
bash "$PS5_TEMPLATE/tools/build-host-tools.sh" >/dev/null
tool="$PS5_TEMPLATE/build/host/ps5-native-tool"
"$tool" link --in "$out/player.elf" --out "$out/eboot.elf" --stub-dir "$out/stubs" --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app="$out/dist/PPSA99730"
mkdir -p "$app/sce_sys" "$app/sce_module" "$app/assets/fonts"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$repo/ps5/app/sce_sys/param.json" "$repo/ps5/app/sce_sys/icon0.png" "$repo/ps5/app/sce_sys/pic0.dds" "$app/sce_sys/"
cp "$repo/ps5/app/sce_sys/pic0.dds" "$app/sce_sys/pic1.dds"
cp "$PS5_TEMPLATE/runtime/libc.prx" "$app/sce_module/"
cp "$repo"/assets/fonts/*.ttf "$repo"/assets/fonts/*.otf "$repo/assets/fonts/OFL.txt" "$repo/assets/fonts/NotoSansCJK-OFL.txt" "$app/assets/fonts/"
mkdir -p "$app/assets/licenses"
cp "$repo/ps5/STREMIBREW-LICENSE.md" "$app/assets/licenses/"
cp "$PLAYER_FFMPEG_DIR"/licenses/* "$app/assets/licenses/"
touch "$app/.player-lock"
echo "$app"
