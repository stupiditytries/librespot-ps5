#!/usr/bin/env bash
# Compiler front end for building FFmpeg for the PS5. Compiles with the SDK's target
# settings; when asked to link (FFmpeg's configure probes do), links against the console
# modules a game process loads, so a probe only succeeds for functions the app can use.
set -euo pipefail
sdk=${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK to the PS5 Payload SDK}
arguments=()
link=true
for argument in "$@"; do
    case "$argument" in
        -c | -E | -S | -M | -MM | --version)
            link=false
            arguments+=("$argument")
            ;;
        -lm | -lpthread | -pthread) ;; # provided by the kernel and C library modules
        *) arguments+=("$argument") ;;
    esac
done
if "$link"; then
    export PATH="$sdk/bin:$PATH"
    arguments+=(-nostdlib -Wl,-e,main -Wl,--no-undefined -L "$sdk/target/lib"
        -lSceLibcInternal -lkernel -lSceNet)
fi
exec clang-18 --target=x86_64-sie-ps5 -isysroot "$sdk" -isystem "$sdk/target/include" \
    -fvisibility-nodllstorageclass=default -fno-stack-protector -fno-plt -femulated-tls \
    -ffunction-sections -fdata-sections "${arguments[@]}"
