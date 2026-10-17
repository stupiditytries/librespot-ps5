#!/usr/bin/env bash
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -f "$tmp/ring-tests" "$tmp/media-tests" "$tmp/socket-tests"; rmdir "$tmp"' EXIT
c++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$src" "$src/ring-tests.cpp" -o "$tmp/ring-tests"
"$tmp/ring-tests"
c++ -std=c++20 -O2 -Wall -Wextra -Werror "$src/tests.cpp" -o "$tmp/media-tests"
"$tmp/media-tests"
c++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined "$src/socket-tests.cpp" -o "$tmp/socket-tests"
"$tmp/socket-tests"
