add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE nifmerge_core)
target_compile_definitions(core_tests PRIVATE NIFMERGE_ROOT="${PROJECT_SOURCE_DIR}")
add_test(NAME core_tests COMMAND core_tests)

add_executable(anim_tests anim_tests.cpp)
target_link_libraries(anim_tests PRIVATE skyanim_core)
target_compile_definitions(anim_tests PRIVATE NIFMERGE_ROOT="${PROJECT_SOURCE_DIR}")
add_test(NAME anim_tests COMMAND anim_tests)

add_executable(fbx_summary fbx_summary.cpp)
target_link_libraries(fbx_summary PRIVATE fbxsdk)

add_executable(gui_input gui_input.cpp)
target_link_libraries(gui_input PRIVATE ole32 shell32 user32)
