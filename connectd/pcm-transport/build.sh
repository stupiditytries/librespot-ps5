# Sourced by connectd/build.sh after sdk, REPO and out have been set.
transport="$REPO/connectd/pcm-transport"
vendor="$REPO/vendor/shellui-loader"
build=$(cat "$transport/protocol.hpp" "$transport/media.hpp" "$transport/files.hpp" "$transport/worker.cpp" "$transport/ring.hpp" "$transport/socket.hpp" | sha256sum | cut -c1-16)
common=(-O2 -march=znver2 -fPIC -Wall -Wextra -Werror -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -ffile-prefix-map="$REPO"=. -I"$transport" -I"$vendor" "-DPCM_TRANSPORT_BUILD=0x${build}ULL")
"$sdk/bin/prospero-clang++" "${common[@]}" -shared -Wl,-e,elf_main -Wl,-Bsymbolic -Wl,--hash-style=sysv \
    "$transport/worker.cpp" -lkernel_sys -lSceLibcInternal -o "$out/pcm-worker.elf"
python3 "$transport/embed.py" "$out/pcm-worker.elf" "$out/pcm-worker-blob.c"
"$sdk/bin/prospero-clang" -O2 -fPIC -Wall -Wextra -Wno-sign-compare -I"$vendor" \
    -ffile-prefix-map="$REPO"=. -c "$vendor/pt.c" -o "$out/obj/pcm-pt.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -I"$vendor" -ffile-prefix-map="$REPO"=. \
    -c "$vendor/stable_elfldr_bridge.c" -o "$out/obj/pcm-loader.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -c "$out/pcm-worker-blob.c" -o "$out/obj/pcm-worker-blob.o"
for source in "$transport/parent.cpp" "$vendor/stable_shellui_injector.cpp" "$vendor/remote_call.cpp"; do
    object="$out/obj/pcm-$(basename "$source").o"
    "$sdk/bin/prospero-clang++" "${common[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
objects+=("$out/obj/pcm-pt.o" "$out/obj/pcm-loader.o" "$out/obj/pcm-worker-blob.o")
