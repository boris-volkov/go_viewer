#!/bin/bash
# Build everything on Linux — plain g++, no CMake.
#   Arch:          sudo pacman -S sdl2 curl libwebsockets
#   Debian/Ubuntu: sudo apt install libsdl2-dev libcurl4-openssl-dev libwebsockets-dev
#   Fedora:        sudo dnf install SDL2-devel libcurl-devel libwebsockets-devel
set -e
cd "$(dirname "$0")"
ROOT="$PWD"

echo "== go_viewer =="
(
    cd "$ROOT/apps/go_viewer"
    g++ -std=c++17 -O2 -I../../engine \
        $(sdl2-config --cflags) \
        main.cpp \
        ../../engine/go_rules.cpp ../../engine/game_state.cpp ../../engine/analysis_state.cpp \
        ../../engine/catalog.cpp ../../engine/game_index.cpp ../../engine/renderer.cpp \
        $(sdl2-config --libs) \
        -pthread \
        -o go_viewer
)
echo "  -> apps/go_viewer/go_viewer"

echo "== ogs_client =="
(
    cd "$ROOT/apps/ogs_client"
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
)
echo "  -> apps/ogs_client/ogs_client"

echo "done."
