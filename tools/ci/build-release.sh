#!/usr/bin/env bash
# Tests, builds and packages the receiver the same way locally and in GitHub Actions.
# Run tools/ci/setup-deps.sh first and source the env.sh it writes.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
cd "$repo"
: "${PS5_TEMPLATE:?Source the env.sh written by tools/ci/setup-deps.sh}"

# Tests that run on any Linux PC.
python3 connectd/modular/prepare-mode.py
bash connectd/modular/test.sh
(cd connect && cargo test --release --lib --features kabuki-provider)
(cd connect && cargo test --release -p librespot-connect --lib)
mkdir -p build
c++ -std=c++20 -O1 -pthread -Iplayer/src -I.deps/nlohmann/include player/tests/link.cpp player/src/link.cpp -o build/link-tests
build/link-tests

# The app, embedded into the receiver.
bash build-modular.sh

mkdir -p dist
rm -f dist/*
cp build/librespot-ps5-modular.elf dist/librespot-ps5.elf
revision=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || printf unknown)}
{
    printf 'librespot-ps5 release build\n'
    printf 'Source revision: %s\n' "$revision"
    printf 'PS5 Payload SDK: ps5-payload-dev/sdk v0.42 (through ps5-native-app-boilerplate)\n'
    printf 'Rust: %s\n' "$(rustc -V)"
} > dist/BUILD-INFO.txt
(cd dist && sha256sum librespot-ps5.elf BUILD-INFO.txt > SHA256SUMS.txt)
ls -la dist
