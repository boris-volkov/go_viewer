#!/bin/bash
cd "$(dirname "$0")"
g++ -std=c++17 -I/c/msys64/mingw64/include/SDL2 -I../../engine \
    main.cpp \
    ../../engine/analysis_state.cpp ../../engine/catalog.cpp ../../engine/game_index.cpp \
    ../../engine/game_state.cpp ../../engine/go_rules.cpp ../../engine/renderer.cpp \
    -L/c/msys64/mingw64/lib -lmingw32 -mwindows -lSDL2main -lSDL2 \
    -o go_viewer_cpp.exe
