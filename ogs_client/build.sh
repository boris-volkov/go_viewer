#!/bin/bash
# Build ogs_client on Linux.
# Arch: pacman -S sdl2 curl libwebsockets
set -e
cd "$(dirname "$0")"

g++ -std=c++17 -O2 \
    -I. -I.. \
    $(sdl2-config --cflags) \
    $(pkg-config --cflags libcurl libwebsockets) \
    main.cpp ogs_net.cpp katago.cpp \
    ../go_rules.cpp ../game_state.cpp ../analysis_state.cpp \
    ../catalog.cpp ../game_index.cpp ../renderer.cpp \
    $(sdl2-config --libs) \
    $(pkg-config --libs libcurl libwebsockets) \
    -pthread \
    -o ogs_client

echo "done: ./ogs_client"
