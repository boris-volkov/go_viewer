cmake_minimum_required(VERSION 3.14)
project(go_viewer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(engine)
add_subdirectory(apps/go_viewer)
add_subdirectory(apps/ogs_client)
