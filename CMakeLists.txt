cmake_minimum_required(VERSION 3.14)
project(go_viewer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

set(SOURCES
    main.cpp
    go_rules.cpp
    game_state.cpp
    analysis_state.cpp
    catalog.cpp
    game_index.cpp
    renderer.cpp
)

add_executable(go_viewer ${SOURCES})
target_link_libraries(go_viewer ${SDL2_LIBRARIES})

if(MSVC)
    target_compile_options(go_viewer PRIVATE /W4 /wd4100 /wd4505)
else()
    target_compile_options(go_viewer PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

