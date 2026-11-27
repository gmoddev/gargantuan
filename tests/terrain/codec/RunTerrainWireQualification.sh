#!/usr/bin/env bash
# Copyright (c) 2026 Team Fireworks
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

TerrainSourceRoot=${1:?Supply an absolute source root and use a private build directory}
TerrainFlags=(-std=c++23 -O1 -g1 -fno-pie -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wextra -Wpedantic -Werror "-I$TerrainSourceRoot/include")
TerrainLink=(-no-pie -pthread)
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

g++ --version
for TerrainFile in src/terrain/Field.cpp src/terrain/ChunkCodec.cpp src/terrain/Manifest.cpp src/filesystem/ProjectIdentity.cpp; do
    TerrainObject=${TerrainFile##*/}
    g++ "${TerrainFlags[@]}" -c "$TerrainSourceRoot/$TerrainFile" -o "${TerrainObject%.cpp}.o"
done

g++ "${TerrainFlags[@]}" "${TerrainLink[@]}" "$TerrainSourceRoot/tests/terrain/codec/ChunkCodecTests.cpp" Field.o ChunkCodec.o -o codec
g++ "${TerrainFlags[@]}" "${TerrainLink[@]}" "$TerrainSourceRoot/tests/terrain/codec/TerrainManifestTests.cpp" Manifest.o ProjectIdentity.o -o manifest
g++ "${TerrainFlags[@]}" "${TerrainLink[@]}" "$TerrainSourceRoot/tests/terrain/codec/TerrainWireParity.cpp" Field.o ChunkCodec.o Manifest.o ProjectIdentity.o -o wire

./codec
./manifest
./wire
TerrainUniform=$(sha256sum Uniform.gtch)
TerrainMixed=$(sha256sum Mixed.gtch)
./wire "${TerrainUniform%% *}" "${TerrainMixed%% *}"
sha256sum Air.gtch Uniform.gtch Mixed.gtch Terrain.gtmf
wc -c Air.gtch Uniform.gtch Mixed.gtch Terrain.gtmf
