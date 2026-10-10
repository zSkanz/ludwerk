# L2 -- the RHI seam and its backends (ADR 0005, ADR 0023).
#
# One directory, several targets. `rhi_api` is header-only and is all any module
# above L2 ever sees; the backends have no public headers at all -- their
# sources live under src/ and are reachable only through the creators in
# engine/rhi/backends.h. That, not the layer table, is what keeps "everything
# above sees only the interface" true.

engine_add_module(rhi_api
    INTERFACE
    LAYER 2
    DEPS
        core
)

# Declared on the seam rather than on each backend, because backends.h -- which
# lives in the seam -- is what reads them. A backend that is off is not
# declared, so selecting it fails to compile instead of returning null.
target_compile_definitions(engine_rhi_api INTERFACE
    ENG_RHI_NULL=$<BOOL:${ENG_RHI_NULL}>
    ENG_RHI_SDLGPU=$<BOOL:${ENG_RHI_SDLGPU}>
    ENG_RHI_CAPTURE=$<BOOL:${ENG_RHI_CAPTURE}>
    ENG_RHI_D3D12=$<BOOL:${ENG_RHI_D3D12}>
)


# Accumulated rather than written out, so the test target links exactly the
# backends this configuration built. Naming one unconditionally would turn
# switching a backend off into a confusing CMake error in an unrelated file.
set(engine_rhi_test_modules rhi_api)

# Native queue/CoreWindow foundation for the UWP player. Kept separate from
# SDL_GPU: neither its device nor its swapchain reaches a desktop window API.
# The selectable adapter is compiled only with ENG_RHI_D3D12; default builds
# retain the foundation solely for native GPU ownership tests on Windows.
if(WIN32)
    engine_add_module(rhi_d3d12
        LAYER 2
        # `device.cpp` with them: the adapter the engine selects is built
        # wherever its parts are, so the gate can run a frame through the
        # same interface a game would. It reaches a game only where
        # ENG_RHI_D3D12 makes `--rhi=d3d12` a choice.
        SOURCES src/d3d12/context.cpp src/d3d12/resources.cpp src/d3d12/pipelines.cpp src/d3d12/descriptors.cpp src/d3d12/commands.cpp
                src/d3d12/device.cpp
        DEPS core platform
        API_DEPS rhi_api
    )
    target_link_libraries(engine_rhi_d3d12 PRIVATE d3d12 dxgi)
    set_target_properties(engine_rhi_d3d12 PROPERTIES EXCLUDE_FROM_ALL TRUE)
    list(APPEND engine_rhi_test_modules rhi_d3d12)
endif()


if(ENG_RHI_NULL)
    engine_add_module(rhi_null
        LAYER 2
        SOURCES
            src/null/null_device.cpp
        DEPS
            core
        API_DEPS
            rhi_api
    )
    list(APPEND engine_rhi_test_modules rhi_null)
endif()

if(ENG_RHI_CAPTURE)
    engine_add_module(rhi_capture
        LAYER 2
        SOURCES
            src/capture/capture_device.cpp
        DEPS
            core
        API_DEPS
            rhi_api
    )
    list(APPEND engine_rhi_test_modules rhi_capture)
endif()

# The one backend that talks to a driver. It reaches SDL through platform's
# sdl_interop.h -- the access ADR 0004 grants it by name -- and links SDL3
# itself, which is the price that header charges for being included.
#
# Its include root is `interop/`, NOT `include/`. `include/` belongs to the
# seam, so anything that links rhi_api can reach every header in it -- and
# sdlgpu_interop.h, which hands out SDL_GPU handles, must not be reachable that
# way. Its own root means those handles are on the include path only for a
# target that links this backend by name (`app`'s ImGui glue, ADR 0011), while
# rhi_capture and rhi_null go on compiling with no SDL in sight. The seam's own
# headers still arrive here through API_DEPS.
if(ENG_RHI_SDLGPU)
    engine_add_module(rhi_sdlgpu
        LAYER 2
        INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/interop
        SOURCES
            src/sdlgpu/sdlgpu_device.cpp
        DEPS
            core
            platform
        API_DEPS
            rhi_api
        PRIVATE_DEPS
            SDL3::SDL3-static
    )
    list(APPEND engine_rhi_test_modules rhi_sdlgpu)
endif()

# rhi_api_tests links no backend at all -- if it ever needs one, the seam has
# grown an implementation dependency and the neutrality claim is gone. The
# others come with theirs.
set(engine_rhi_test_sources tests/rhi_api_tests.cpp)
if(WIN32)
    list(APPEND engine_rhi_test_sources tests/rhi_d3d12_context_tests.cpp)
endif()
if(ENG_RHI_NULL)
    list(APPEND engine_rhi_test_sources tests/rhi_null_tests.cpp)
endif()
if(ENG_RHI_CAPTURE AND ENG_RHI_NULL)
    list(APPEND engine_rhi_test_sources tests/rhi_capture_tests.cpp)
endif()
if(ENG_RHI_SDLGPU)
    list(APPEND engine_rhi_test_sources tests/rhi_sdlgpu_tests.cpp)
endif()

engine_add_module_tests(rhi
    MODULES ${engine_rhi_test_modules}
    SOURCES ${engine_rhi_test_sources}
)

if(ENG_BUILD_TESTS)
    target_compile_definitions(engine_rhi_tests PRIVATE
        ENG_TEST_SHADER_CONTENT="${CMAKE_BINARY_DIR}/engine/app/content/shaders")
endif()

if(WIN32 AND ENG_BUILD_TESTS)
    target_compile_definitions(engine_rhi_tests PRIVATE
        ENG_TEST_NATIVE_SHADERS="${CMAKE_BINARY_DIR}/engine/app/content/shaders/dxil")
    add_test(NAME rhi_d3d12_shaders COMMAND engine_rhi_tests "--test-case=native D3D12 shipping*")
    set_tests_properties(rhi_d3d12_shaders PROPERTIES SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP" TIMEOUT 120)
endif()

# **The SDL_GPU cases, registered again on their own so a skip is visible**
# (S7.10). Inside the `rhi` suite they cannot be: the API and null-backend cases
# really do run everywhere, so the entry is green and honest and the three that
# needed a device and did not find one are invisible inside it. With only those
# three in an entry, "did not run" and "the test skipped" are the same
# statement, and `SKIP_REGULAR_EXPRESSION` turns the message the fixture already
# prints into a CTest skip.
#
# The same shape as `editor_shell` in `engine/app/CMakeLists.txt`, and for the
# same reason: CTest counts a skip as a pass.
if(ENG_BUILD_TESTS AND ENG_RHI_SDLGPU)
    add_test(NAME rhi_sdlgpu COMMAND engine_rhi_tests --test-suite=sdlgpu)
    set_tests_properties(rhi_sdlgpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)
endif()
