#!/bin/bash
cd "C:/Users/boris/Documents/GitHub/go_viewer"
g++ -std=c++17 -I/c/msys64/mingw64/include/SDL2 \
    analysis_state.cpp catalog.cpp game_index.cpp game_state.cpp \
    go_rules.cpp main.cpp renderer.cpp \
    -L/c/msys64/mingw64/lib -lmingw32 -mwindows -lSDL2main -lSDL2 \
    -o go_viewer_cpp.exe
