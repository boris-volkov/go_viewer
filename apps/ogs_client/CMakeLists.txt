# ── Dependencies ──────────────────────────────────────────────────────────────

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
    sound.cpp
    ogs_puzzles.cpp
)

target_include_directories(ogs_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}     # json.hpp lives here
    ${CURL_INCLUDE_DIRS}
    ${LWS_INCLUDE_DIRS}
)

target_link_libraries(ogs_client PRIVATE
    go_engine
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
