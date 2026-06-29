cmake_minimum_required(VERSION 3.14)
project(ogs_client CXX)
set(CMAKE_CXX_STANDARD 17)

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(SDL2 REQUIRED)
find_package(CURL REQUIRED)

# libwebsockets — try pkg-config first, fall back to manual search
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LWS libwebsockets)
endif()
if(NOT LWS_FOUND)
    find_library(LWS_LIBRARIES NAMES websockets libwebsockets REQUIRED)
    find_path(LWS_INCLUDE_DIRS libwebsockets.h REQUIRED)
endif()

# ── Sources ───────────────────────────────────────────────────────────────────

add_executable(ogs_client
    main.cpp
    ogs_net.cpp
    katago.cpp
    # Reuse shared Go logic from the parent directory
    ../go_rules.cpp
    ../game_state.cpp
    ../analysis_state.cpp
    ../catalog.cpp
    ../game_index.cpp
    ../renderer.cpp
)

target_include_directories(ogs_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # json.hpp lives here
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # go_viewer.hpp, go_rules.hpp, etc.
    ${SDL2_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${LWS_INCLUDE_DIRS}
)

target_link_libraries(ogs_client PRIVATE
    ${SDL2_LIBRARIES}
    ${CURL_LIBRARIES}
    ${LWS_LIBRARIES}
)

# Windows: link against WinSock (needed by libwebsockets)
if(WIN32)
    target_link_libraries(ogs_client PRIVATE ws2_32)
endif()

# After every build, copy the exe up one level so the user can launch it from
# the ogs_client/ directory (where config.ini and log files live).
add_custom_command(TARGET ogs_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ogs_client>"
        "${CMAKE_CURRENT_SOURCE_DIR}/ogs_client.exe"
    COMMENT "Copying ogs_client.exe to source directory"
)
