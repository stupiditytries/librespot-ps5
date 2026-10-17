# Sourced by connectd/build.sh after sdk, REPO and out have been set.
transport="$REPO/connectd/usb-transport"
vendor="$REPO/vendor/shellui-loader"
build=$(cat "$transport/protocol.hpp" "$transport/media.hpp" "$transport/files.hpp" "$transport/worker.cpp" | sha256sum | cut -c1-16)
common=(-O2 -march=znver2 -fPIC -Wall -Wextra -Werror -std=c++20 -fno-exceptions -fno-rtti
    -fno-threadsafe-statics -ffile-prefix-map="$REPO"=. -I"$transport" -I"$vendor" "-DUSB_TRANSPORT_BUILD=0x${build}ULL")
"$sdk/bin/prospero-clang++" "${common[@]}" -shared -Wl,-e,elf_main -Wl,-Bsymbolic -Wl,--hash-style=sysv \
    "$transport/worker.cpp" -lkernel_sys -lSceLibcInternal -o "$out/usb-worker.elf"
python3 "$transport/embed.py" "$out/usb-worker.elf" "$out/usb-worker-blob.c"
"$sdk/bin/prospero-clang" -O2 -fPIC -Wall -Wextra -Wno-sign-compare -I"$vendor" \
    -ffile-prefix-map="$REPO"=. -c "$vendor/pt.c" -o "$out/obj/usb-pt.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -I"$vendor" -ffile-prefix-map="$REPO"=. \
    -c "$vendor/stable_elfldr_bridge.c" -o "$out/obj/usb-loader.o"
"$sdk/bin/prospero-clang" -O2 -fPIC -c "$out/usb-worker-blob.c" -o "$out/obj/usb-worker-blob.o"
for source in "$transport/parent.cpp" "$vendor/stable_shellui_injector.cpp" "$vendor/remote_call.cpp"; do
    object="$out/obj/usb-$(basename "$source").o"
    "$sdk/bin/prospero-clang++" "${common[@]}" -c "$source" -o "$object"
    objects+=("$object")
done
objects+=("$out/obj/usb-pt.o" "$out/obj/usb-loader.o" "$out/obj/usb-worker-blob.o")
