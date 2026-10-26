add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE nifmerge_core)
target_compile_definitions(core_tests PRIVATE NIFMERGE_ROOT="${PROJECT_SOURCE_DIR}")
add_test(NAME core_tests COMMAND core_tests)

add_executable(fbx_summary fbx_summary.cpp)
target_link_libraries(fbx_summary PRIVATE fbxsdk)
