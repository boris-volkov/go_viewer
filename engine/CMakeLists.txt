find_package(SDL2 REQUIRED)

add_library(go_engine STATIC
    go_rules.cpp
    game_state.cpp
    analysis_state.cpp
    catalog.cpp
    game_index.cpp
    renderer.cpp
)

target_compile_features(go_engine PUBLIC cxx_std_17)

target_include_directories(go_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(go_engine PUBLIC ${SDL2_LIBRARIES})

if(MSVC)
    target_compile_options(go_engine PRIVATE /W4 /wd4100 /wd4505)
else()
    target_compile_options(go_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
