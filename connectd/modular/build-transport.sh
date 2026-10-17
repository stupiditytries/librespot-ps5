# Sourced by this candidate's build-receiver.sh; the current receiver is unchanged.
transport="$integration"
vendor="$REPO/vendor/shellui-loader"
python3 "$integration/prepare-mode.py"
build=$(cat "$transport/protocol.hpp" "$transport/media.hpp" "$transport/files.hpp" "$transport/worker.cpp" "$transport/parent.cpp" "$transport/legacy-cleanup.hpp" "$transport/service-memory.hpp" "$transport/marker.inc" "$transport/feature-worker.inc" "$transport/mode-parent.hpp" "$transport/bootstrap.hpp" "$transport"/ownership/*.{hpp,cpp} "$transport/ownership/build/mode-firmware.inc" "$transport/ring.hpp" "$transport/socket.hpp" | sha256sum | cut -c1-16)
common=(-O2 -march=znver2 -fPIC -Wall -Wextra -Werror -Wframe-larger-than=16384 -fstack-usage -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -ffile-prefix-map="$REPO"=. -ffile-prefix-map="$integration"=pcm-usbfree -I"$transport" -I"$vendor" -I"$REPO/connectd" "-DPCM_TRANSPORT_BUILD=0x${build}ULL")
"$sdk/bin/prospero-clang++" "${common[@]}" -shared -Wl,-e,elf_main -Wl,-Bsymbolic -Wl,--hash-style=sysv \
    "$transport/worker.cpp" -lkernel_sys -lSceLibcInternal -o "$out/pcm-worker.elf"
python3 "$transport/embed.py" "$out/pcm-worker.elf" "$out/pcm-worker-blob.c"
"$sdk/bin/prospero-clang" -O2 -fPIC -Wall -Wextra -Wno-sign-compare -I"$vendor" \
    -ffile-prefix-map="$REPO"=. -c "$vendor/pt.c" -o "$out/obj/pcm-pt.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -I"$vendor" -ffile-prefix-map="$REPO"=. \
    -c "$vendor/stable_elfldr_bridge.c" -o "$out/obj/pcm-loader.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -c "$out/pcm-worker-blob.c" -o "$out/obj/pcm-worker-blob.o"
for source in "$transport/parent.cpp" "$transport/ownership/server.cpp" "$transport/ownership/stream.cpp" "$vendor/stable_shellui_injector.cpp" "$vendor/remote_call.cpp"; do
    object="$out/obj/pcm-$(basename "$source").o"
    "$sdk/bin/prospero-clang++" "${common[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
objects+=("$out/obj/pcm-pt.o" "$out/obj/pcm-loader.o" "$out/obj/pcm-worker-blob.o")
