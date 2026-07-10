add_executable(go_viewer main.cpp)
target_link_libraries(go_viewer PRIVATE go_engine)

# After every build, copy the exe up one level so it can be launched from its
# source directory (and from go_viewer.bat at the repo root).
add_custom_command(TARGET go_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:go_viewer>"
        "${CMAKE_CURRENT_SOURCE_DIR}/go_viewer.exe"
    COMMENT "Copying go_viewer.exe to source directory"
)
