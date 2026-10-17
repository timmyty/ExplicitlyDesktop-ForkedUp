#!/bin/sh
# Builds and runs the C++ unit tests with g++ or clang++ (no CMake needed).
set -e
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../Source"
out="${TMPDIR:-/tmp}/ExplicitlyTests"
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -I"$src" -I"$here" \
    -DEXPLICITLY_VECTORS_FILE="\"$here/../vectors/protocol_vectors.json\"" \
    "$here"/*.cpp "$src/StreamCrypto.cpp" "$src/PcmTap.cpp" "$src/RemoteSessions.cpp" \
    -o "$out" -pthread
"$out" "$@"
