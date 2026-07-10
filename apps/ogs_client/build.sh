#!/bin/bash
# Build ogs_client on Linux.
# Arch: pacman -S sdl2 curl libwebsockets
set -e
cd "$(dirname "$0")"

g++ -std=c++17 -O2 \
    -I. -I../../engine \
    $(sdl2-config --cflags) \
    $(pkg-config --cflags libcurl libwebsockets) \
    main.cpp ogs_net.cpp katago.cpp sound.cpp ogs_puzzles.cpp \
    ../../engine/go_rules.cpp ../../engine/game_state.cpp ../../engine/analysis_state.cpp \
    ../../engine/catalog.cpp ../../engine/game_index.cpp ../../engine/renderer.cpp \
    $(sdl2-config --libs) \
    $(pkg-config --libs libcurl libwebsockets) \
    -pthread \
    -o ogs_client

echo "done: ./ogs_client"
