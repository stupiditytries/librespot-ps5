#!/bin/bash
# Builds the Connect receiver as a background program (a plain ELF loaded by hand, with no
# tile). Run on Linux or in WSL.
#   REPO      this repository as seen from WSL
#   WORK      build area (default: ~/librespot-ps5-build)
#   TEMPLATE  checkout of ps5-native-app-boilerplate, which provides the PS5 Payload SDK
#             and the compiler wrapper (default: ~/ps5-native-app-boilerplate)
set -euo pipefail
REPO=${REPO:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${WORK:-$HOME/librespot-ps5-build}
TEMPLATE=${TEMPLATE:-$HOME/ps5-native-app-boilerplate}
export RUSTUP_HOME=${RUSTUP_HOME:-$HOME/.rustup} CARGO_HOME=${CARGO_HOME:-$HOME/.cargo}
export PATH=$CARGO_HOME/bin:$PATH

sdk=$TEMPLATE/.deps/native/ps5-payload-sdk
target=$REPO/toolchain/x86_64-ps5-freebsd.json
out=$WORK/connectd
features=()
if [[ ${USB_AUTOSTART:-0} == 1 ]]; then
    out=$WORK/connectd-usb-autostart
    features+=(--features usb-autostart)
    OUTPUT=${OUTPUT:-$REPO/build/librespot-ps5-autostart.elf}
fi
if [[ ${SHELL_PCM:-0} == 1 ]]; then
    out=$WORK/connectd-shell-pcm
    features=(--features shell-pcm)
    OUTPUT=${PCM_OUTPUT:-$REPO/build/librespot-ps5-pcm-test-v8.elf}
fi
OUTPUT=${OUTPUT:-$REPO/build/librespot-ps5.elf}
mkdir -p "$out/obj" "$WORK/bin"

# 1. The Rust static library.
# The paths of this checkout and of the Rust installation are kept out of the program:
# without the --remap-path-prefix options they end up in it, in the messages for errors.
export RUSTFLAGS="--cfg libc_unstable_freebsd_version=\"11\" --cfg mio_unsupported_force_waker_pipe --remap-path-prefix=$REPO=. --remap-path-prefix=$CARGO_HOME=/cargo --remap-path-prefix=$RUSTUP_HOME=/rustup --remap-path-prefix=$WORK=/build"
bash "$REPO/toolchain/patch-rust-src.sh"
printf '#!/bin/sh\nPS5_PAYLOAD_SDK=%s exec sh %s/tooling/prospero-clang18 "$@"\n' \
    "$sdk" "$TEMPLATE" > "$WORK/bin/ps5-cc"
chmod +x "$WORK/bin/ps5-cc"
export CC_x86_64_ps5_freebsd=$WORK/bin/ps5-cc AR_x86_64_ps5_freebsd=llvm-ar-18
(cd "$REPO/connect" && CARGO_TARGET_DIR=$WORK/target-connect cargo build --release \
    --lib "${features[@]}" --target "$target" -Zbuild-std=std,panic_abort -Zjson-target-spec)
rust_lib=$WORK/target-connect/x86_64-ps5-freebsd/release/libconnect_ps5.a
# With FreeBSD 11 layouts the libc crate asks for versioned names such as stat@FBSD_1.0; the
# console's libraries export the same functions unversioned.
"$sdk/bin/prospero-nm" -u "$rust_lib" 2>/dev/null | awk '{print $NF}' | grep '@FBSD_' |
    sort -u | sed -E 's/^(.*)@FBSD_.*$/& \1/' > "$out/versioned-symbols.txt"
"$sdk/bin/prospero-objcopy" --redefine-syms="$out/versioned-symbols.txt" "$rust_lib" \
    "$out/libconnect_ps5.a"

# 2. The program: its own main, and stand-ins for the few things the Rust library needs
#    that the SDK's C library lacks or the console does differently. The console's media
#    framework is called only by an optional worker inside ShellUI. The receiver
#    itself does not own a media source; its audio output is muted.
sources=("$REPO/connectd/src/main.c" "$REPO/connectd/src/standins.c"
    "$REPO/connectd/src/compat_socket.c")
objects=()
native_libs=()
if [[ ${USB_AUTOSTART:-0} == 1 || ${SHELL_PCM:-0} == 1 ]]; then native_libs+=(-lkernel_sys -lSceLibcInternal); fi
for source in "${sources[@]}"; do
    object=$out/obj/$(basename "$source").o
    cflags=()
    if [[ ${USB_AUTOSTART:-0} == 1 || ${SHELL_PCM:-0} == 1 ]]; then cflags+=(-DUSB_AUTOSTART=1); fi
    if [[ ${SHELL_PCM:-0} == 1 ]]; then cflags+=(-DSHELL_PCM=1); fi
    "$sdk/bin/prospero-clang" -O2 -Wall "${cflags[@]}" -ffile-prefix-map="$REPO"=. -c "$source" -o "$object"
    objects+=("$object")
done
if [[ ${SHELL_PCM:-0} == 1 ]]; then
    source "$REPO/connectd/pcm-transport/build.sh"
elif [[ ${USB_AUTOSTART:-0} == 1 ]]; then
    source "$REPO/connectd/usb-transport/build.sh"
fi
"$sdk/bin/prospero-clang" -o "$out/librespot-ps5.elf" "${objects[@]}" "$out/libconnect_ps5.a" \
    -lSceNet -lSceNotification "${native_libs[@]}" \
    -lc++ -lc++abi \
    -Wl,--error-limit=0 2>&1 | tee "$out/link.log"

# Debug information is most of the file; the console does not need it.
"$sdk/bin/prospero-strip" --strip-debug "$out/librespot-ps5.elf"
ls -la "$out/librespot-ps5.elf"
readelf -d "$out/librespot-ps5.elf" | grep NEEDED
mkdir -p "$REPO/build"
cp "$out/librespot-ps5.elf" "$OUTPUT"
sha256sum "$OUTPUT"
