#!/usr/bin/env bash
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -f "$tmp/native-tests" "$tmp/policy-tests"; rmdir "$tmp"' EXIT
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra "$src/tests.cpp" -o "$tmp/native-tests"
"$tmp/native-tests"
rust_compiler=${RUSTC:-$(command -v rustc || printf '%s/.cargo/bin/rustc' "$HOME")}
"$rust_compiler" --edition=2021 --test "$src/../../connect/src/usb_policy.rs" -o "$tmp/policy-tests"
"$tmp/policy-tests"
