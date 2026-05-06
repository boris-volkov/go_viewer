cmake_minimum_required(VERSION 3.10)
project(go_viewer C)

# Find SDL2
find_package(SDL2 REQUIRED)

# Include directories
include_directories(${SDL2_INCLUDE_DIRS})

# Source files
set(SOURCES go_viewer.c)

# Executable
add_executable(go_viewer ${SOURCES})

# Link libraries
target_link_libraries(go_viewer ${SDL2_LIBRARIES})

# Compiler flags
if(MSVC)
    target_compile_options(go_viewer PRIVATE /W4)
else()
    target_compile_options(go_viewer PRIVATE -Wall -Wextra -pedantic)
endif()
