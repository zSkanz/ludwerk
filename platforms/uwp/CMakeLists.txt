# Isolated feasibility target. No SDL, GDK, JIT, engine service facsimiles or
# changes to the shipping targets. The pinned VM and catalog reader are real.
add_compile_definitions(WINAPI_FAMILY=WINAPI_FAMILY_APP)

# Compile the actual native backend foundation with AppContainer headers and
# libraries too. Excluded from the probe executable: its D3D11 path is unchanged.
add_library(engine_uwp_d3d12_context STATIC EXCLUDE_FROM_ALL
    "${CMAKE_SOURCE_DIR}/engine/rhi/src/d3d12/context.cpp"
    "${CMAKE_SOURCE_DIR}/engine/rhi/src/d3d12/resources.cpp"
    "${CMAKE_SOURCE_DIR}/engine/rhi/src/d3d12/pipelines.cpp"
    "${CMAKE_SOURCE_DIR}/engine/rhi/src/d3d12/descriptors.cpp"
    "${CMAKE_SOURCE_DIR}/engine/rhi/src/d3d12/commands.cpp")
target_include_directories(engine_uwp_d3d12_context PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/rhi/include" "${CMAKE_SOURCE_DIR}/engine/core/include")
target_link_libraries(engine_uwp_d3d12_context PRIVATE WindowsApp)
target_compile_definitions(engine_uwp_d3d12_context PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(engine_uwp_d3d12_context PRIVATE /W4 /WX /permissive- /utf-8)
add_executable(engine_uwp_renderer_check WIN32 EXCLUDE_FROM_ALL renderer_check.cpp)
target_link_libraries(engine_uwp_renderer_check PRIVATE engine_uwp_d3d12_context WindowsApp)
target_include_directories(engine_uwp_renderer_check PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/rhi/include" "${CMAKE_SOURCE_DIR}/engine/core/include")
target_compile_definitions(engine_uwp_renderer_check PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(engine_uwp_renderer_check PRIVATE /W4 /WX /permissive- /utf-8 /bigobj)
target_link_options(engine_uwp_renderer_check PRIVATE /APPCONTAINER)
set_target_properties(engine_uwp_renderer_check PROPERTIES OUTPUT_NAME "engine-uwp-renderer-check")
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LUAU_WERROR OFF CACHE BOOL "" FORCE)
add_subdirectory("${ENG_THIRD_PARTY_DIR}/luau" "${CMAKE_BINARY_DIR}/third_party/luau"
                 EXCLUDE_FROM_ALL SYSTEM)
add_library(engine_uwp_dmath STATIC "${CMAKE_SOURCE_DIR}/engine/core/src/dmath.cpp")
target_include_directories(engine_uwp_dmath PRIVATE "${CMAKE_SOURCE_DIR}/engine/core/include")
target_link_libraries(Luau.VM PUBLIC engine_uwp_dmath)
target_link_libraries(Luau.Compiler PUBLIC engine_uwp_dmath)

add_executable(engine_uwp_probe WIN32
    probe.cpp
    "${CMAKE_SOURCE_DIR}/engine/core/src/i18n.cpp"
    "${CMAKE_SOURCE_DIR}/engine/core/src/json.cpp"
    "${CMAKE_SOURCE_DIR}/engine/core/src/number_parse.cpp")
target_include_directories(engine_uwp_probe PRIVATE "${CMAKE_SOURCE_DIR}/engine/core/include")
target_link_libraries(engine_uwp_probe PRIVATE Luau.VM Luau.Compiler engine::build_info
    WindowsApp d3d11 dxgi d2d1 dwrite)
target_compile_definitions(engine_uwp_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(engine_uwp_probe PRIVATE /APPCONTAINER)
target_compile_options(engine_uwp_probe PRIVATE /W4 /WX /permissive- /utf-8 /bigobj)
set_target_properties(engine_uwp_probe PROPERTIES OUTPUT_NAME "engine-uwp-probe")
