#!/usr/bin/env bash
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$src/build"
for test in feature process-records migration legacy bootstrap; do
    c++ -std=c++20 -O2 -Wall -Wextra -Werror "$src/$test-tests.cpp" -o "$src/build/$test-tests"
    "$src/build/$test-tests"
done
c++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined "$src/ring-tests.cpp" -o "$src/build/ring-tests"
"$src/build/ring-tests"
c++ -std=c++20 -O2 -Wall -Wextra -Werror "$src/marker-tests.cpp" -o "$src/build/marker-tests"
"$src/build/marker-tests"
c++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined "$src/socket-tests.cpp" -o "$src/build/socket-tests"
"$src/build/socket-tests"
c++ -std=c++20 -O2 -Wall -Wextra -Werror "$src/http-tests.cpp" "$src/ownership/server.cpp" "$src/ownership/stream.cpp" -o "$src/build/http-tests"
"$src/build/http-tests" "$src/build/served.wav"
c++ -std=c++20 -O2 -Wall -Wextra -Werror -pthread "$src/service-memory-tests.cpp" "$src/ownership/server.cpp" "$src/ownership/stream.cpp" -o "$src/build/service-memory-tests"
"$src/build/service-memory-tests"
