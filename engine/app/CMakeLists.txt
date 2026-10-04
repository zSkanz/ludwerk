# L6 -- the host. This is the one module allowed to see everything. The VM moved
# into `engine/script` in M2, as planned: what is left here is the world it runs
# in, the project it is mounted from, and the frame loop that drives it
# (`world_host.h`).

engine_add_module(app
    LAYER 6
    SOURCES
        src/world_host.cpp
        src/world_host_sub_worlds.cpp
        src/network_session.cpp
        src/match_launcher.cpp
        src/export_runner.cpp
        src/frame_pacing.cpp
        src/frame_scheduler.cpp
        src/backends.cpp
        src/engine.cpp
        src/preserved.cpp
        src/project_config.cpp
        src/reload.cpp
        src/replay.cpp
        src/script_package.cpp
        src/script_sides.cpp
        src/script_files.cpp
        src/two_worlds.cpp
        src/bench.cpp
        src/screenshot.cpp
        src/soak.cpp
        src/ui_text.cpp
        src/ui_theme.cpp
        src/script_document.cpp
        src/script_syntax.cpp
        src/script_editor.cpp
        src/class_favorites.cpp
        src/script_editor_settings.cpp
        src/editor_performance.cpp
        src/script_editor_panel.cpp
        src/script_complete.cpp
        src/streaming_host.cpp
        src/field_streamer.cpp
        src/terrain_cells.cpp
        src/terrain_import.cpp
        src/world_ui.cpp
        src/scene_definitions.cpp
        src/debug_overlay.cpp
        src/inspector.cpp
        src/launcher.cpp
        src/launcher_run.cpp
        src/partition_cache.cpp
        src/picking.cpp
        src/preview_renderer.cpp
        src/pointer_ownership.cpp
        src/graphics_settings_file.cpp
        src/ui_navigation.cpp
        src/ui_pointer.cpp
        src/chunk_overlay.cpp
        src/reference_grid.cpp
        src/brush_overlay.cpp
        src/skeleton_overlay.cpp
        src/terrain_overlay.cpp
        src/world_panels.cpp
        src/editor.cpp
        src/editor_water.cpp
        src/command_palette.cpp
        src/editor_drive.cpp
        src/view_host.cpp
        src/number_expression.cpp
        src/content_tree.cpp
        src/content_import.cpp
        src/icons.cpp
        src/thumbnails.cpp
        src/dev_control.cpp
    DEPS
        asset
        audio
        core
        jobs
        platform
        rhi_api
        net_api
        input
        nav
        physics_api
        render
        replication_api
        scene
        script
        ui
    PUBLIC_DEPS
        engine::luau
        engine::build_info
)

# The icon ids, generated from `icons/default/theme.json`. Beside its source
# rather than under include/engine/app/, for the reason `engine_scene` gives about
# its own: "do not edit" is easier to obey when it applies to a whole folder.
target_include_directories(engine_app PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/generated>)

# The backends themselves are linked ONLY here (ADR 0023): every other module
# sees the seam. Which ones exist is a build decision, so the link list is
# assembled from the same options backends.cpp switches on.
if(ENG_RHI_SDLGPU)
    target_link_libraries(engine_app PRIVATE engine::rhi_sdlgpu)
endif()
if(ENG_RHI_CAPTURE)
    target_link_libraries(engine_app PRIVATE engine::rhi_capture)
endif()
if(ENG_RHI_NULL)
    target_link_libraries(engine_app PRIVATE engine::rhi_null)
endif()

# The replication module, by the same rule and for a stronger reason (ADR 0070,
# clause 3): with it off there is no socket code in the binary to reach, and the
# engine refuses a networked posture by name rather than failing to connect.
if(ENG_ENABLE_REPLICATION)
    target_link_libraries(engine_app PRIVATE engine::replication)
endif()

# The physics backend, for the same reason and by the same rule (ADR 0023): the
# seam is what `scene` and everything else sees, and the implementation is
# linked here alone.
if(ENG_PHYSICS_JOLT)
    target_link_libraries(engine_app PRIVATE engine::physics_jolt)
endif()
if(ENG_PHYSICS_BOX2D)
    target_link_libraries(engine_app PRIVATE engine::physics_box2d)
endif()

# The debug overlay (ADR 0011). Dev builds only: the option lives beside the
# dependency in third_party/CMakeLists.txt, and a shipping build declares no
# ImGui target at all -- so what is linked here IS the whole feature.
#
# SDL3 is named explicitly, for the same reason `platform` makes every consumer
# of sdl_interop.h name it. The overlay reaches the real SDL_Window and the real
# SDL_GPUDevice through two interop headers whose whole point is that crossing
# that line should be an edit to a CMakeLists, not something an #include can do
# on its own.
if(ENG_DEBUG_UI)
    # **`assetc` is linked HERE and only here** (E9 step 12). It carries the
    # basis encoder and assimp, which is exactly what
    # `engine/asset/include/engine/asset/texture.h` split the two halves apart to
    # keep out of a shipped game -- so the editor compiles content on import and
    # a shipping build still carries no encoder at all.
    #
    # The precedent is ImGui and SDL3 beside it: a dependency the tool has and
    # the product does not, named in a CMakeLists so that crossing the line is an
    # edit rather than something an `#include` can do on its own. The layering
    # gate reads real include edges and places `assetc` at 5 -- above everything
    # it includes and below `app` -- so this is an ordinary downward edge and
    # `scene`, `render` and `audio` still may not reach it.
    target_link_libraries(engine_app PRIVATE engine::imgui SDL3::SDL3-static engine_assetc_lib)

    # **Luau's own type checker, for the script editor** (ADR 0093), and only
    # here: a shipped game carries no Analysis (ADR 0002). It is the vendored
    # Luau at the pin, so this is a component of an approved dependency and not
    # a new one; `cmake/engine_luau.cmake` keeps it out of `all` so nothing else
    # compiles it.
    target_sources(engine_app PRIVATE src/language_service.cpp)
    target_link_libraries(engine_app PRIVATE Luau.Analysis)

    # **The surface shader compiler** (ADR 0091), and only here: it RUNS
    # `shadercross` rather than linking it, so the editor carries no compiler
    # library and a shipped game neither compiles nor can. The build tree's own
    # shadercross is where a dev build finds it; a package puts one beside the
    # editor, which is looked for first.
    target_sources(engine_app PRIVATE src/surface_compiler.cpp)
    if(ENG_SHADER_TOOLCHAIN)
        target_compile_definitions(engine_app PRIVATE
            ENG_DEV_SHADERCROSS="${CMAKE_BINARY_DIR}/host-tools/shadercross${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        target_compile_definitions(engine_app PRIVATE ENG_DEV_SHADERCROSS="")
    endif()
endif()

# PUBLIC rather than PRIVATE: debug_overlay.h describes a class that is inert in
# shipping, and a test asserting that has to be able to tell which build it is
# in. The header itself never reads this -- the class has the same shape in
# every profile, which is what keeps the frame loop free of #ifdefs.
target_compile_definitions(engine_app PUBLIC ENG_DEBUG_UI=$<BOOL:${ENG_DEBUG_UI}>)
# **The repository this build came from** (audit T1): where a development
# editor finds the CLI its Export window runs. Empty in the profiles that ship,
# which find theirs beside the executable.
if(ENG_DEBUG_UI)
    target_compile_definitions(engine_app PRIVATE ENG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
else()
    target_compile_definitions(engine_app PRIVATE ENG_SOURCE_DIR="")
endif()


if(ANDROID)
    # **On Android the host is the library SDL's activity loads** -- `main`,
    # and its `main` resolved by dlsym, as the triangle sample's is
    # (samples/triangle/CMakeLists.txt says why each part is so).
    # platforms/android is the Gradle project that packages it.
    add_library(engine_host SHARED src/main.cpp)
    set_target_properties(engine_host PROPERTIES OUTPUT_NAME main)
    target_link_options(engine_host PRIVATE "LINKER:--export-dynamic-symbol=main")
else()
    add_executable(engine_host src/main.cpp)
    set_target_properties(engine_host PROPERTIES OUTPUT_NAME engine-host)
    # **A game opens no console window** (ADR 0104, stage 0). The profiles a
    # player receives are Windows-subsystem programs, entered through the same
    # `main`; one started from a terminal attaches to it so what it logs is
    # still seen, and every run writes its log file either way.
    #
    # **Nor does the editor** (D507): opened from the folder, or started by the
    # launcher, it was a console program, and a console window opened behind it
    # -- the second, empty window the owner saw while a project loaded.
    if(WIN32 AND ENG_PROFILE MATCHES "^(player|shipping|editor)$")
        target_link_options(engine_host PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        target_compile_definitions(engine_host PRIVATE ENG_GUI_SUBSYSTEM=1)
    endif()
endif()
target_link_libraries(engine_host PRIVATE engine::app engine::warnings)

# The application's identity, embedded in the artifact rather than left beside
# it (roadmap M8). Windows only here because a Windows icon IS a PE resource;
# the macOS and Linux halves are a bundle's `Info.plist` and a `.desktop` entry,
# and both belong to a packaging step neither of this machine's two tiers can
# run. `app.rc` says what this is and why a game replaces it.
if(WIN32)
    target_sources(engine_host PRIVATE "${CMAKE_SOURCE_DIR}/branding/app.rc")
    # So a change to the artwork rebuilds the binary that carries it. CMake does
    # not scan an .rc for its dependencies, and an icon that silently kept the
    # previous build's bytes is exactly the regression the read-back check
    # exists to catch -- it should never have to.
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/branding/app.rc" APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/branding/icon/icon.ico")
endif()

# The catalog ships beside the binary as content, because the host resolves it
# relative to its own location rather than the source tree -- the same shape a
# packaged build uses (architecture.md §8).
#
# **The catalog is staged by a target that runs every build**, not by this
# post-build step: a post-build step runs only when the host relinks, and a
# change to `en.json` alone relinks nothing -- which left a message added in the
# same commit printing as `[i18n:missing]`. `copy_if_different` makes it a
# comparison when nothing changed.
add_custom_target(engine_host_catalog ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/i18n/en.json"
        "${CMAKE_CURRENT_BINARY_DIR}/content/i18n/en.json"
    COMMENT "Staging the engine message catalog")
add_dependencies(engine_host engine_host_catalog)

# `@engine/...` ships as content for the same reason: a project reads the same
# file its editor type-checks against, rather than a copy compiled into the
# binary that nothing can inspect. The conformance runner too: it is an entry
# script the headless runner mounts, and a real file is one the gate can analyse.
#
# **Staged every build, as the catalog is, and for the catalog's reason**: as a
# post-build step these were copied only when the host relinked, so a module
# added or edited by itself was not there to require until some C++ changed.
add_custom_target(engine_host_runtime ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/runtime/engine"
        "${CMAKE_CURRENT_BINARY_DIR}/content/runtime/engine"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/runtime/conformance"
        "${CMAKE_CURRENT_BINARY_DIR}/content/runtime/conformance"
    COMMENT "Staging the engine's Luau modules")
add_dependencies(engine_host engine_host_runtime)

add_custom_command(TARGET engine_host POST_BUILD
    # The default typeface (roadmap M7, human decision 2026-08-20). Content
    # rather than an array compiled into the binary: it is 868 KB of data the
    # engine READS, a packaged build ships it beside the executable like every
    # other asset, and a byte array nobody can open is a byte array nobody can
    # check against upstream.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/inter/docs/font-files/InterVariable.ttf"
        "$<TARGET_FILE_DIR:engine_host>/content/fonts/Inter.ttf"
    # The CODE face (ADR 0057). Cousine, which ImGui already vendors among its
    # sample assets, under the SIL OFL 1.1 -- the same licence Inter ships under
    # and one `tools/repo/licensecheck.luau` already admits. No new dependency
    # row: this is a file inside a tree the manifest already pins by commit, so
    # it cannot move without somebody bumping ImGui on purpose. Its credit is
    # declared through the manifest's `bundled` list, because
    # THIRD_PARTY_NOTICES.md is generated and a hand-added line would be
    # overwritten by the next `vendor.luau notices`.
    #
    # Inter is still the engine's typeface. A code editor set in a proportional
    # face is not a code editor, which is the whole of why there are two.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/imgui/misc/fonts/Cousine-Regular.ttf"
        "$<TARGET_FILE_DIR:engine_host>/content/fonts/Mono.ttf"
    # The editor's icon themes. A plain recursive copy and deliberately NOT
    # through `assetc`: that path sends every image to basisu for KTX2/UASTC
    # with generated mips, which is right for a world texture and wrong for a
    # sixteen-pixel glyph -- block compression smears small alpha shapes, and a
    # small alpha shape is all an icon is. `icons/README.md` says so and says
    # why.
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/icons"
        "$<TARGET_FILE_DIR:engine_host>/content/icons"
    # The project templates and the generated definitions, so the LAUNCHER can
    # make a project (ADR 0055). `ludwerk new` reads them from the installation
    # root and this reads them from content, which is the same tree copied to two
    # places by `tools/repo/package.luau` -- and the test that compares what the
    # two scaffolders produce is what keeps that from being two answers.
    #
    # **Removed first, because a copy only adds.** A file that left a template
    # stayed in the staged one, and the launcher scaffolded it: the old
    # starter's `src/scripts/main.luau` beside the new scene's `Main`, two
    # scripts in a fresh project.
    COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:engine_host>/content/templates"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/templates"
        "$<TARGET_FILE_DIR:engine_host>/content/templates"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/runtime/types/engine.d.luau"
        "$<TARGET_FILE_DIR:engine_host>/content/runtime/types/engine.d.luau"
    COMMENT "Staging the engine message catalog, the @engine modules, the conformance runner, the editor icons and the project templates")

engine_add_module_tests(app
    SOURCES
        tests/pointer_ownership_tests.cpp
        tests/ui_navigation_tests.cpp
        tests/ui_pointer_tests.cpp
        tests/frame_pacing_tests.cpp
        tests/frame_scheduler_tests.cpp
        tests/text_input_focus_tests.cpp
        tests/foliage_editor_tests.cpp
        tests/script_sides_tests.cpp
        tests/script_matrix_tests.cpp
        tests/preserved_tests.cpp
        tests/reload_state_tests.cpp
        tests/reload_tests.cpp
        tests/world_host_tests.cpp
        tests/gamepad_tests.cpp
        tests/draw_poses_tests.cpp
        tests/script_files_tests.cpp
        tests/sub_world_tests.cpp
        tests/debug_overlay_tests.cpp
        tests/inspector_tests.cpp
        tests/launcher_tests.cpp
        tests/picking_tests.cpp
        tests/reference_grid_tests.cpp
        tests/brush_overlay_tests.cpp
        tests/skeleton_overlay_tests.cpp
        tests/terrain_overlay_tests.cpp
        tests/editor_tests.cpp
        tests/content_tree_tests.cpp
        tests/content_import_tests.cpp
        tests/partition_cache_tests.cpp
        tests/soak_tests.cpp
        tests/streaming_host_tests.cpp
        tests/field_streamer_tests.cpp
        tests/terrain_cells_tests.cpp
        tests/terrain_import_tests.cpp
        tests/terrain_seam_tests.cpp
        tests/world_ui_tests.cpp
        tests/scene_definitions_tests.cpp
        tests/two_worlds_tests.cpp
        tests/project_config_tests.cpp
        tests/export_runner_tests.cpp
        tests/command_palette_tests.cpp
        tests/view_host_tests.cpp
        tests/number_expression_tests.cpp
        tests/ui_theme_tests.cpp
        tests/ui_text_tests.cpp
        tests/thumbnail_tests.cpp
        tests/script_document_tests.cpp
        tests/class_favorites_tests.cpp
        tests/script_editor_settings_tests.cpp
        tests/editor_performance_tests.cpp
        tests/script_editor_tests.cpp
        tests/script_complete_tests.cpp
)

# The wire and the world hash describe the same state: a gate that needs the
# replication module's own field applier, so it is built where the module is.
# The language service's own suite, where the service is (ADR 0093).
if(ENG_BUILD_TESTS AND ENG_DEBUG_UI)
    target_sources(engine_app_tests PRIVATE tests/language_service_tests.cpp)
    target_link_libraries(engine_app_tests PRIVATE Luau.Analysis)

    # The surface compiler against the real `shadercross` and the real headers
    # (ADR 0091). Skipped, and saying so, on a host with no shader toolchain.
    target_sources(engine_app_tests PRIVATE tests/surface_compiler_tests.cpp)
    if(ENG_SHADER_TOOLCHAIN)
        target_compile_definitions(engine_app_tests PRIVATE
            ENG_TEST_SHADERCROSS="${CMAKE_BINARY_DIR}/host-tools/shadercross${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        target_compile_definitions(engine_app_tests PRIVATE ENG_TEST_SHADERCROSS="")
    endif()
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_SHADER_INCLUDE="${CMAKE_SOURCE_DIR}/shaders/include")
    # Only where there is one: a host with no shader toolchain (macOS, ADR
    # 0032) has no shadercross target, and the test skips there.
    if(TARGET shadercross)
        add_dependencies(engine_app_tests shadercross)
    endif()
endif()

if(ENG_BUILD_TESTS AND ENG_ENABLE_REPLICATION)
    target_sources(engine_app_tests PRIVATE tests/wire_hash_tests.cpp tests/network_session_tests.cpp)
    target_link_libraries(engine_app_tests PRIVATE engine::replication)
endif()

if(ENG_BUILD_TESTS)
    # Point the tests at the real catalog rather than a fixture, so they also
    # prove every key the host raises exists in i18n/en.json.
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_CATALOG="${CMAKE_SOURCE_DIR}/i18n/en.json")

    # **The engine itself, for the match launcher's case** (ADR 0106 §5): a
    # match is separate processes, and the only way to test starting them is to
    # start some.
    if(TARGET engine_host)
        target_sources(engine_app_tests PRIVATE tests/match_launcher_tests.cpp)
        target_compile_definitions(engine_app_tests PRIVATE ENG_TEST_HOST="$<TARGET_FILE:engine_host>")
        add_dependencies(engine_app_tests engine_host)
    endif()

    # **The compiler, for the cut-over's end-to-end case** (E9 step 14).
    # `engine_app` links this PRIVATE, deliberately, so a shipping build carries
    # no encoder -- which means its headers do not reach a test through the
    # module. Named again here, behind the same flag, because the thing under
    # test is a project compiling itself and there is no way to assert that
    # without the thing that compiles it.
    if(ENG_DEBUG_UI)
        target_link_libraries(engine_app_tests PRIVATE engine_assetc_lib)
    endif()

    # One real encoded picture, so the thumbnail cache can be asserted end to
    # end -- read, decoded, resampled, uploaded -- rather than only on the paths
    # that fail. Borrowed from `asset` rather than copied: two copies of one
    # fixture is two things to keep in step for no gain.
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_IMAGE="${CMAKE_SOURCE_DIR}/engine/asset/tests/data/checker.png")

    # One real mesh, so the preview's RENDER half has something to draw (S5.16).
    # The engine's staged content is shaders, icons and templates -- no models --
    # so a test that looked there skipped on every machine, which is the shape
    # S7.9 is about. Borrowed from `asset`'s fixtures rather than copied: two
    # copies of one quad is two things to keep in step.
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_MESH="${CMAKE_SOURCE_DIR}/engine/asset/tests/data/quad.gltf")

    # A folder of real textures for the skipped thumbnail bench, overridable
    # because the interesting answer comes from somebody's OWN 4K art rather than
    # from a fixture. Defaults to the icon masters, which are the only sizeable
    # pictures the repository carries.
    if(NOT DEFINED ENG_TEST_THUMBNAIL_DIR)
        set(ENG_TEST_THUMBNAIL_DIR "${CMAKE_SOURCE_DIR}/icons/default/class")
    endif()
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_THUMBNAIL_DIR="${ENG_TEST_THUMBNAIL_DIR}")

    # --- The shell, registered so it can report NOT having run (S7.8, S7.9) --
    #
    # `on a real device, F3 flips the panel` is the only test that enters the
    # editor shell -- it draws the explorer, the properties table and every
    # widget under a real device with validation on. Where there is no display
    # it returns before it asserts, and inside the `app` suite that is a pass:
    # 456 other cases really did run, so nothing is red and nothing says the
    # shell was not entered.
    #
    # It is not hypothetical. The Linux tier has a Vulkan device through
    # lavapipe and no display at all, so this ran on exactly one of the two
    # machines the gate uses and the summary reported green on both.
    #
    # Registering the case AGAIN on its own is what fixes that: with only this
    # case in it, "did not run" and "the test skipped" are the same statement,
    # and `SKIP_REGULAR_EXPRESSION` turns the message it already prints into a
    # CTest skip. It costs one window and one device -- about a second -- and
    # buys a summary line that tells the truth about the shell on every machine.
    add_test(NAME editor_shell COMMAND engine_app_tests "--test-case=*F3 flips*")
    set_tests_properties(editor_shell PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # Gate: --version must report the pin and the ABI constants (ADR 0031).
    # Asserted on the real binary rather than in-process, so it also proves the
    # provenance header actually reached the shipped executable.
    add_test(NAME host_version COMMAND engine_host --version)
    set_tests_properties(host_version PROPERTIES
        PASS_REGULAR_EXPRESSION "Luau ${ENG_LUAU_VERSION} \\(${ENG_LUAU_COMMIT}\\)")

    add_test(NAME host_version_abi COMMAND engine_host --version)
    set_tests_properties(host_version_abi PROPERTIES
        PASS_REGULAR_EXPRESSION "vector 3-wide f32")

    # M0's deliverable, still asserted: the host runs a script and prints a
    # catalog-resolved greeting.
    #
    # `--headless --frames=3 --exit` is not decoration. Since M1 the host runs a
    # frame loop, and without a budget it runs until its window is closed --
    # which under CTest means forever. This test hung for 140 seconds locally
    # before the flags went on, and would have burned the CI job's timeout.
    #
    # `--rhi=null` for the same reason: these tests are about the host, and a
    # Linux CI runner has no Vulkan driver. Making them depend on a GPU would
    # turn "this runner has no graphics" into a red build, which is a check
    # nobody can act on. The real device is covered by the rhi tests, which skip
    # when there is none, and by the Tier-1 gate run on the dev machine.
    add_test(NAME host_boot
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/boot/boot.luau" --headless --frames=3 --exit --rhi=null)
    set_tests_properties(host_boot PROPERTIES
        PASS_REGULAR_EXPRESSION "engine initialized"
        TIMEOUT 60)

    # **A dedicated server ends on a match, not a frame count**: `--serve` runs
    # with no budget, so the only ways out are the game's own `game:Shutdown()`
    # and a stop from outside -- and either must run the close handlers, which
    # is where a match's results are saved. Port 47913 so it cannot meet a
    # developer's own server on the default.
    add_test(NAME serve_ends_on_match
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/serve/match_over.luau" --serve=47913 --rhi=null)
    set_tests_properties(serve_ends_on_match PROPERTIES
        PASS_REGULAR_EXPRESSION "server closed after the match"
        TIMEOUT 60)

    # The frame loop terminates on its budget and says what it ran. Separate
    # from host_boot because "the script ran" and "the loop stopped" are
    # different claims, and a hang should name which one broke.
    add_test(NAME host_headless_frames COMMAND engine_host --headless --frames=5 --exit --rhi=null)
    set_tests_properties(host_headless_frames PROPERTIES
        PASS_REGULAR_EXPRESSION "Ran 5 frames"
        TIMEOUT 60)

    # **D182: a frame of many draws must not write past a GPU descriptor heap.**
    # Real device, debug layer asked for by name (`--gpu-debug`), because the
    # layer is what turns the out-of-bounds write into a failure instead of a
    # silent corruption -- without the SDL patch this dies on D3D12 with
    # EXECUTION ERROR #646. A machine with no device skips rather than fails.
    add_test(NAME descriptor_heap_overflow
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/descriptorheap/transparent_crowd.luau"
                --headless --frames=8 --exit --rhi=sdlgpu --gpu-debug)
    set_tests_properties(descriptor_heap_overflow PROPERTIES
        PASS_REGULAR_EXPRESSION "Ran 8 frames"
        SKIP_REGULAR_EXPRESSION "Could not create a graphics device"
        TIMEOUT 120)

    # **The render half of the frame runs headless** (G11, K3): a Render-rate
    # context reads an injected stick and a bound render step runs.
    add_test(NAME host_headless_render_rate
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/renderrate" --headless --rhi=null --frames=60 --exit)
    set_tests_properties(host_headless_render_rate PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[renderrate\\] render=1 simulation=1 steps=true"
        TIMEOUT 60)

    # A headless run with no budget has no window to close, so it would never
    # stop. Rejected up front rather than left to a CI timeout to discover.
    add_test(NAME host_headless_needs_frames COMMAND engine_host --headless)
    set_tests_properties(host_headless_needs_frames PROPERTIES WILL_FAIL TRUE TIMEOUT 60)

    # Selecting a backend this build does not contain names what it does have.
    add_test(NAME host_unknown_backend COMMAND engine_host --rhi=nonesuch)
    set_tests_properties(host_unknown_backend PROPERTIES WILL_FAIL TRUE TIMEOUT 60)

    # D468: what a scene is handed is JSON, and text that is not is refused
    # where it was typed rather than read as nil by the scene.
    add_test(NAME host_scene_data_must_be_json
        COMMAND engine_host --headless --frames=1 --exit --rhi=null "--scene-data={level: 3")
    set_tests_properties(host_scene_data_must_be_json PROPERTIES WILL_FAIL TRUE TIMEOUT 60)

    # **A host with no script used to refuse and now opens the project browser**
    # (ADR 0055), so what this asserts moved rather than disappeared: a path that
    # is not there is still a refusal, which is the case somebody reaches by
    # typo. Nothing here can drive the browser -- it needs a window -- and what
    # would go wrong quietly is a launcher that opened for a mistyped path
    # instead of saying so.
    #
    # A path that EXISTS and holds no scripts is deliberately not this test: the
    # host warns and runs it, which it has always done, because an empty project
    # is a project somebody is part-way through making.
    add_test(NAME host_refuses_a_path_that_is_not_there
        COMMAND engine_host ${CMAKE_SOURCE_DIR}/no-such-project --rhi=null --headless --frames=1)
    set_tests_properties(host_refuses_a_path_that_is_not_there PROPERTIES WILL_FAIL TRUE TIMEOUT 60)

    # The M2 gate: ~900 Luau specs written against docs/api-design.md by authors
    # who were forbidden from reading `engine/` (MASTER_PROMPT §7). That is what
    # makes them a contract rather than a transcript, and it is why they are run
    # here rather than trusted.
    #
    # `--rhi=null` because the suite tests the kernel and not the renderer: a
    # machine with no GPU should still be able to prove the engine's semantics.
    add_test(NAME conformance
        COMMAND engine_host --run-tests=${CMAKE_SOURCE_DIR}/tests/conformance --rhi=null)
    set_tests_properties(conformance PROPERTIES TIMEOUT 300)

    # The other M2 gate: same script and same seed, twice, 10,000 ticks,
    # identical world hash at every checkpoint (ADR 0025). No `--rhi` because a
    # replay opens no device at all -- if rendering could change the hash, that
    # is the defect, not a configuration.
    #
    # This process is fresh every time CTest starts it, so running it here is
    # also architecture.md §9's cross-process leg; the in-process repeat that
    # catches leaked global state happens inside the run.
    add_test(NAME determinism
        COMMAND engine_host --replay=${CMAKE_SOURCE_DIR}/tests/determinism)
    set_tests_properties(determinism PROPERTIES TIMEOUT 300)

    # Three of M6's gate items, and all three are replays with a FACT to prove
    # rather than only a hash to compare. The scenario manifest names an
    # attribute the run has to end having set, because a hash alone cannot gate
    # any of them: three runs that all fall in the same hole agree perfectly.
    #
    #   obby      -- `examples/04-obby` driven by a recorded input stream from
    #                the menu to the finish flag. The whole stack is in the loop:
    #                input dispatch, UI layout and hit-testing, tweens driving
    #                kinematic platforms, physics, audio and animation. It is
    #                worth more than the five system gates it does not replace.
    #   audiosoak -- sixty seconds of continuous mixing with the underrun counter
    #                still zero, and the harder half: an exact, predictable
    #                number of `Ended` events off the SIM timeline. The
    #                real-device half is a windowed run recorded by hand, since
    #                only a machine with speakers can say the device opened.
    #   animation -- three clips at three speeds, their `TimePosition` driven into
    #                the world so the replay hash covers clip sampling at all.
    add_test(NAME replay_gates
        COMMAND engine_host --replay=${CMAKE_SOURCE_DIR}/tests/replay)
    set_tests_properties(replay_gates PROPERTIES TIMEOUT 300)

    # The perf threshold (roadmap M2: the 10k-parts / 1k-listeners churn
    # benchmark "with a CI threshold", and the 500-instance gate scene). Both
    # budgets are deliberately loose -- see `bench.h`: a CI runner's speed varies
    # by more than the regression anyone would want to catch, so this fails on a
    # catastrophe and `docs/perf-baselines.md` holds the real numbers.
    #
    # One repeat here rather than the default three. The gate asks whether the
    # engine still works, and three runs of a 10,000-instance scene is thirty
    # seconds of CI for a number nobody reads.
    # M7's gate: "asset build determinism check in CI". Two PROCESSES rather
    # than the in-process double build `engine_assetc_tests` already does -- see
    # the driver for what the second process can see that the second call
    # cannot. `examples/05-streaming` is the input because it is the largest
    # content tree in the repository: 289 chunks, meshes and textures.
    add_test(NAME asset_determinism
        COMMAND ${CMAKE_COMMAND}
            -DASSETC=$<TARGET_FILE:assetc>
            -DCONTENT=${CMAKE_SOURCE_DIR}/examples/05-streaming/content
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/asset-determinism
            -DGENERATOR=${CMAKE_SOURCE_DIR}/examples/05-streaming/tools/generate_world.luau
            -DREPO=${CMAKE_SOURCE_DIR}
            -DWORLD=${CMAKE_SOURCE_DIR}/examples/05-streaming/content/world
            -DMIN_FILES=290
            -P ${CMAKE_SOURCE_DIR}/tests/assetdeterminism/run_determinism_gate.cmake)
    set_tests_properties(asset_determinism PROPERTIES TIMEOUT 600)

    # M7's gate: the scripted five-minute fly-through over `examples/05-streaming`,
    # asserting peak memory under a declared ceiling and a frame-time histogram
    # with no hitch over 33 ms.
    #
    # Eighteen thousand frames is five minutes of SIM time and about ten seconds
    # of wall clock, because the fixed 1/60 s headless step decouples the two.
    # That is the whole reason a five-minute gate is affordable at all.
    #
    # THE THIRD ASSERTION IS THE ONE THAT EARNED ITS PLACE. D032 degraded from
    # 100 fps to 35 over five minutes with no hitch anywhere and no ceiling
    # breach -- a thousand instances leaked every fifteen seconds. `soak.h`
    # explains why the instance count flattening is the property that separates
    # a streamed world from a growing one.
    #
    # **Re-measured at the E9 cut-over** (ADR 0065), which is one of that
    # milestone's own gate items: BC7 and mips reached editor content for the
    # first time, and a ceiling that stays generous after the thing it measures
    # got cheaper no longer measures anything. 192 MiB against a 25 MiB peak was
    # not a backstop, it was a number nothing could reach.
    #
    # 96 MiB against 29 MiB on Windows and 25 MiB on Linux -- two platforms
    # within sixteen per cent of each other, which is what makes 3x headroom a
    # judgement rather than a guess. It is still a backstop for a leak the growth
    # check misses rather than a tight budget: a ceiling with no headroom is a
    # gate that goes red when an allocator changes, and macOS is Tier-3, so
    # nothing local can measure it before CI does.
    add_test(NAME streaming_soak
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DASSETC=$<TARGET_FILE:assetc>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/05-streaming
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/streaming-soak-project
            -DGENERATOR=${CMAKE_SOURCE_DIR}/examples/05-streaming/tools/generate_world.luau
            -DREPO=${CMAKE_SOURCE_DIR}
            -DWORLD=${CMAKE_SOURCE_DIR}/examples/05-streaming/content/world
            -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/streaming-soak.json
            -DFRAMES=18000
            -DCEILING_MB=96
                -DMIN_INSTANCES=1000
            # **D066's named successor lives here**, because this fly-through is
            # a circular orbit: the camera comes back to every place it has been,
            # several times over three hundred seconds. Eight metres is well
            # inside one chunk, so a revisit means the same chunks -- and
            # therefore the same resident set, unless something leaked.
            #
            # It replaces nothing: the quarantined quarter-against-quarter check
            # stays where it is, still measuring and still not gating, because
            # what it watches for has no other instrument on a path that does not
            # double back.
            -DRETURN_RADIUS_M=8
            -P ${CMAKE_SOURCE_DIR}/tests/streamsoak/run_soak_gate.cmake)
    set_tests_properties(streaming_soak PROPERTIES TIMEOUT 600)

    # M8's gate: the ten-minute soak over the flagship, at the length a test
    # suite can afford to run every time. **The full ten minutes is a milestone
    # gate step and is recorded in the brief**; this is a hundred seconds of
    # simulation, which is two whole autopilot cycles -- a walk leg and a flight
    # leg twice each.
    #
    # A hundred seconds is not an arbitrary length. The soak's flatten check
    # compares the second quarter of a run against the fourth, and the demo's
    # autopilot alternates between a walk and a flight every twenty-five
    # seconds -- so at this length both quarters land on the same leg, and the
    # comparison is between like and like rather than between a walk and a
    # flight.
    #
    # **Re-measured at the E9 cut-over** (ADR 0065), and the interesting part is
    # the disagreement: this said 384 MiB against 169 measured, and the peak
    # today is 47 MiB on Windows and 46 on Linux. The 169 predates ADR 0053,
    # which stopped the flagship's generator writing `*.chunk.json` for `assetc`
    # and had it write a SCENE the engine cuts up on the way to the first frame
    # -- so the number is not wrong, it is about a different program.
    #
    # 192 MiB, which is above the historical 169 rather than below it. Two
    # current measurements beat one stale one, but not by enough to bet a red
    # `main` on a platform nothing here can run: macOS is Tier-3 and only CI
    # builds it. So this halves a ceiling that measured nothing, and leaves the
    # tightening that the 47 MiB peak would justify to somebody who can watch
    # all three tiers go green.
    # **The netcode acceptance gate** (docs/briefs/netcode-acceptance.md, B):
    # a real server and a real client, the client's link held, jittered and
    # lost by the link conditioner, at 0, 50, 150 and 300 ms round trip and
    # with frames of 150 and 400 ms -- a predicted character walked a straight
    # line and stopped, held to no correction, to where the authority stopped,
    # and to a remote answering within the ping, a tick and a frame. Real time,
    # so two and a half minutes; the table is written beside it.
    add_test(NAME netcode_acceptance
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/acceptance
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-acceptance
            -DPORT=47960
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_netcode_acceptance.cmake)
    # Alone: two real-time processes held to a tick are measured, and the rest
    # of the suite beside them on a CI runner's cores is what they would measure.
    set_tests_properties(netcode_acceptance PROPERTIES TIMEOUT 600 LABELS "netcode" RUN_SERIAL TRUE)

    add_test(NAME openworld_soak
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DASSETC=$<TARGET_FILE:assetc>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/10-open-world
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/openworld-soak-project
            -DGENERATOR=${CMAKE_SOURCE_DIR}/examples/10-open-world/tools/generate_world.luau
            -DREPO=${CMAKE_SOURCE_DIR}
            # **A scene rather than a directory of chunk sources** (ADR 0053).
            # The flagship's generator used to write `content/world/*.chunk.json`
            # for `assetc` to compile; it writes a SCENE now, and the engine cuts
            # that into cells on the way to the first frame. What this gate has
            # to ensure exists therefore moved with it -- `EXISTS` answers for a
            # file exactly as it did for a directory, so the check itself did not
            # have to change.
            -DWORLD=${CMAKE_SOURCE_DIR}/examples/10-open-world/content/scenes/main.scene.json
            -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/openworld-soak.json
            -DFRAMES=6000
            -DCEILING_MB=192
            -DMIN_INSTANCES=2000
            # **No return radius here, and that is a measurement rather than an
            # omission.** D066's successor needs a path that doubles back, and
            # this one does not: over 5,939 frames the nearest an early frame
            # came to a late one was 232 m across a path spanning 835 m, because
            # a walk leg followed by a flight leg goes onward rather than round.
            # `streaming_soak` flies a circular orbit and carries the check.
            -P ${CMAKE_SOURCE_DIR}/tests/streamsoak/run_soak_gate.cmake)
    set_tests_properties(openworld_soak PROPERTIES TIMEOUT 900)

    # **The far flight** (ADR 0150): a world wider than one table laid by
    # `--import-terrain`, the far ground's files it leaves counted, and a
    # flight out and back over it, twice, under a memory ceiling. What it
    # holds is that a world is made and flown in bounded memory -- the flight
    # found three leaks that rose with distance before it was a gate (D405,
    # D406, D408) -- that the ground under it came in and went out, and that the far
    # ground was drawn from its files with none made again.
    #
    # Four kilometres at a metre voxel: 4 356 cells and thirty-six blocks,
    # laid in a quarter of a minute. On a real device (see the script): it
    # takes its turn with the tests that draw. The ceiling is the peak
    # measured, with headroom on the soaks' terms; `docs/perf-baselines.md`
    # has both.
    add_test(NAME terrain_far_flight
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/perf/farflight
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/farflight
            -DREPORT=${CMAKE_CURRENT_BINARY_DIR}/far-flight.json
            -DSIZE=4100
            -DBLOCKS=36
            -DFRAMES=4200
            -DCEILING_MB=768
            -DMIN_GROUND=2000
            -DGROWTH=15
            -P ${CMAKE_SOURCE_DIR}/tests/streamsoak/run_far_flight_gate.cmake)
    set_tests_properties(terrain_far_flight PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 900)

    add_test(NAME perf_budget
        COMMAND engine_host --bench=${CMAKE_SOURCE_DIR}/tests/bench --bench-repeats=1)
    set_tests_properties(perf_budget PROPERTIES TIMEOUT 600 LABELS perf)

    # The blocking render-regression gate (architecture.md §9): record the
    # command stream and diff it byte-for-byte against a golden. No GPU, no
    # driver, no tolerance -- so a graphics vendor's update cannot turn the
    # merge queue red, and it runs on every tier including CI.
    if(ENG_RHI_CAPTURE)
        add_test(NAME capture_gate
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/rendercapture/clear-3frames.jsonl
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-gate.jsonl
                -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/00-clear/init.luau
                -DFRAMES=3
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_capture_gate.cmake)
        set_tests_properties(capture_gate PROPERTIES TIMEOUT 60)

        # M4's gate: three camera angles by two lighting states, as six
        # consecutive frames of one stream. Every camera position and clock
        # value is a literal at a named tick -- an orbit driven by elapsed time
        # would make the golden depend on how many ticks had passed when it was
        # recorded, and a one-tick shift changes every number in a command
        # stream (M1 Finding 9, M3 Finding 15).
        add_test(NAME capture_gate_meshes
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/rendercapture/meshes-6frames.jsonl
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-gate-meshes.jsonl
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/meshes
                -DFRAMES=6
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_capture_gate.cmake)
        set_tests_properties(capture_gate_meshes PROPERTIES TIMEOUT 120)

        # M6's gate: the SAME UI script captured at two resolutions, as two
        # goldens. The pair is the check -- a layout that ignored the window
        # would produce two identical streams, and the two differ in exactly the
        # way scaling predicts: the clipped strip's scissor is 512,668,704x40 at
        # 1280x720 and 256,308,352x40 at 640x360, so the scale-driven numbers
        # halve and the offset-driven height does not.
        #
        # What these goldens can and cannot see is worth knowing before trusting
        # them: `upload` records a buffer's SIZE and not its contents (D026), so
        # the QUADS are invisible here. The rectangles themselves are asserted
        # exactly by `engine_ui_tests`, and the pixels by a screenshot in the gate
        # record. This pair covers the scissors, the viewport and the draw count.
        foreach(resolution 1280x720 640x360)
            string(REPLACE "x" ";" size ${resolution})
            list(GET size 0 ui_width)
            list(GET size 1 ui_height)
            add_test(NAME capture_gate_ui_${resolution}
                COMMAND ${CMAKE_COMMAND}
                    -DHOST=$<TARGET_FILE:engine_host>
                    -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/rendercapture/uipanel-${resolution}.jsonl
                    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-gate-ui-${resolution}.jsonl
                    -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/uipanel
                    -DFRAMES=2
                    -DWIDTH=${ui_width}
                    -DHEIGHT=${ui_height}
                    -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_capture_gate.cmake)
            set_tests_properties(capture_gate_ui_${resolution} PROPERTIES TIMEOUT 60)
        endforeach()

        # The skinned gate (M6). Four frames of one animated mesh beside one that
        # is not animated at all, which is what makes it able to tell "the
        # skinned path ran" from "everything went through the skinned path".
        #
        # Unlike the UI pair, this golden CAN see the thing it is about: the
        # joint palette goes up as a uniform block and `bindUniforms` records
        # contents (the half of D026 that was fixed at M4.5), so a pose that
        # stopped moving is four identical lines in a file the diff points at.
        add_test(NAME capture_gate_skinned
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/rendercapture/skinned-4frames.jsonl
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-gate-skinned.jsonl
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/skinned
                -DFRAMES=4
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_capture_gate.cmake)
        set_tests_properties(capture_gate_skinned PROPERTIES TIMEOUT 60)

        # ADR 0096's golden: every effect it added in one world, three frames,
        # recorded after the owner accepted the look (2026-09-25). Blocking on
        # every tier, like its neighbours -- the lavapipe images below are the
        # pixels, and they are not. This is what stops a pass silently dropping
        # out, a uniform block changing shape, or a pipeline being built every
        # frame instead of once.
        add_test(NAME capture_gate_look
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DSCENE=${CMAKE_SOURCE_DIR}/tests/look
                -DVARIANT=everything
                -DDRIVER=${CMAKE_SOURCE_DIR}/tests/rendercapture/run_capture_gate.cmake
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/rendercapture/look-everything-3frames.jsonl
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-gate-look.jsonl
                -DFRAMES=3
                -P ${CMAKE_SOURCE_DIR}/tests/look/run_look_gate.cmake)
        set_tests_properties(capture_gate_look PROPERTIES TIMEOUT 120)

        # M4.5's gate addition, and the one check in this file that is not a
        # golden: one scene at two `ClockTime` values, and the two frames must
        # differ. See the driver for why a golden could not make that claim --
        # and did not, for the whole of M4.
        add_test(NAME clock_differential
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/clock-differential.jsonl
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/clockpair
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_clock_differential.cmake)
        set_tests_properties(clock_differential PROPERTIES TIMEOUT 120)
    endif()

    # The contact-shadow golden (D051), and it exists because the artifact was
    # invisible at every framing this repository already had. A human sent a
    # close-up of a capsule and a boulder with a band of lit floor between each
    # object and its own shadow -- both hovering -- and the same objects in
    # `tests/screenshots/specular` show that gap as about one pixel.
    #
    # Frame 15 puts the sun at half past nine, which is where a detached contact
    # is most obvious: high enough that each shadow sits beside its object rather
    # than stretching to the horizon, low enough that a centimetre of detachment
    # is a centimetre of bright floor. The scene sweeps the hour with the frame
    # number, so the two ends of the range are one flag away.
    #
    # `gpu-golden` like the other two: this frame is edges and soft gradients,
    # and neither survives a move to a software rasterizer.
    add_test(NAME screenshot_gate_contact
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/contact-0930.png
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screenshot-gate-contact.png
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/contact
            -DFRAMES=15
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)
    set_tests_properties(screenshot_gate_contact PROPERTIES
        LABELS "gpu-golden"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0110: a gradient and a stroke land where the ADR says, asked of the
    # screenshot point by point (`imgprobe`) rather than against a golden.
    add_test(NAME ui_appearance_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/uiappearance
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ui-appearance.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_ui_appearance_gate.cmake)
    set_tests_properties(ui_appearance_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0128: a grid, pages, flex, a scale, a shape, a size limit, a canvas
    # group and the selection's outline, each asked what colour it is where it
    # should be and where it should not.
    add_test(NAME ui_layouts_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/uilayouts
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ui-layouts.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_ui_layouts_gate.cmake)
    set_tests_properties(ui_layouts_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0147, section 7: the ready-made options screen. Each level its
    # quality row offers is the picture of a game started at that level, no
    # two neighbouring levels are the same picture, and the screen's pages are
    # drawn where they say.
    add_test(NAME options_screen_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/optionsscreen
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/options-screen
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_options_screen_gate.cmake)
    set_tests_properties(options_screen_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # ADR 0153: a sprite's colours on the screen are the colours it was given,
    # asked of the screenshot square by square, to within one step.
    add_test(NAME sprite_color_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/spritecolor
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/sprite-color.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_sprite_color_gate.cmake)
    set_tests_properties(sprite_color_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0129: a highlight through a wall and one hidden by it, a beam and a
    # trail, each asked what colour it is where it should be and where it
    # should not.
    add_test(NAME effects_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/effects
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/effects.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_effects_gate.cmake)
    set_tests_properties(effects_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0107: two cameras draw into two textures in one frame, and a picture
    # shows each.
    add_test(NAME views_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/views
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/views.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_views_gate.cmake)
    set_tests_properties(views_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0107: a mirror shows what a mirror would, and a portal what is
    # beyond it (`@engine/views`, `Camera.ClipPlane`).
    add_test(NAME mirrors_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/mirrors
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/mirrors.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_mirrors_gate.cmake)
    set_tests_properties(mirrors_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # An orthographic camera shows its whole column, whatever the shape and
    # however low it stands (the owner's report, 2026-09-27).
    add_test(NAME ortho_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/ortho
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ortho.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_ortho_gate.cmake)
    set_tests_properties(ortho_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0107 §3: a sub-world draws its own world, and only its own.
    add_test(NAME subworld_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/subworld
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/subworld.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_subworld_gate.cmake)
    set_tests_properties(subworld_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # ADR 0091's proof: the built-in surface, written as a surface shader,
    # forced onto every static part, draws what the built-in surface draws --
    # to the bit, on this machine. Two scenes: lights, shadows and fog; and
    # meshes with their own maps.
    foreach(scene specular meshes)
        if(scene STREQUAL "meshes")
            set(scene_dir "${CMAKE_SOURCE_DIR}/tests/rendercapture/meshes")
        else()
            set(scene_dir "${CMAKE_SOURCE_DIR}/tests/screenshots/${scene}")
        endif()
        add_test(NAME surface_contract_proof_${scene}
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DIMGCMP=$<TARGET_FILE:imgcmp>
                -DSCRIPT=${scene_dir}
                -DFRAMES=30
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/surface-proof-${scene}
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_surface_proof.cmake)
        set_tests_properties(surface_contract_proof_${scene} PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 180)
    endforeach()

    # ADR 0091 end to end: a project's own surface shader, compiled by the
    # editor at run time, drawn identically to the built-in surface it copies --
    # and differently once changed, which is what shows it was used at all.
    if(ENG_DEBUG_UI AND ENG_SHADER_TOOLCHAIN)
        add_test(NAME user_surface_proof
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DIMGCMP=$<TARGET_FILE:imgcmp>
                -DSCENES=${CMAKE_SOURCE_DIR}/tests/screenshots/usersurface
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/user-surface-proof
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_user_surface_proof.cmake)
        set_tests_properties(user_surface_proof PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 240)

        # A lost graphics device is survived, reported, and the surface that
        # was on screen held back (ADR 0091).
        add_test(NAME device_loss_survived
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DSCENE=${CMAKE_SOURCE_DIR}/tests/screenshots/usersurface/surfaced
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/device-loss
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_device_loss.cmake)
        set_tests_properties(device_loss_survived PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 240)
    endif()

    # M8's graphics settings family, as a differential. See the driver for why
    # the claim is "these two must differ" rather than a golden: a preset that
    # is parsed and then reaches nothing renders exactly what it did before.
    add_test(NAME graphics_settings_differential
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/specular
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/settings-differential
            -DFRAMES=120
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_settings_differential.cmake)
    set_tests_properties(graphics_settings_differential PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # ADR 0147: a quality level a script sets through `GraphicsService` while
    # the game runs draws what a game started at that level draws, and a level
    # the command line names stands over a script's.
    add_test(NAME graphics_runtime_differential
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/graphicsruntime
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/graphics-runtime
            -DFRAMES=150
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_graphics_runtime.cmake)
    set_tests_properties(graphics_runtime_differential PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # **What temporal anti-aliasing is for, measured** (ADR 0158): a fence under
    # a camera panning a third of a pixel a frame, drawn with each mode, and
    # how much the frames pop -- TAA must flicker clearly least. A comparison
    # of this machine's own renders, so a gate wherever there is a device.
    add_test(NAME anti_aliasing_flicker
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DFLICKER=$<TARGET_FILE:imgflicker>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/flicker
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/anti-aliasing-flicker
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_flicker_gate.cmake)
    set_tests_properties(anti_aliasing_flicker PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 240)

    # Particles simulated on the GPU (ADR 0160): they collide with the terrain
    # and with what is seen, and a hundred thousand of them are a frame under
    # a sixtieth of a second -- see the driver.
    add_test(NAME particles_gpu_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCENE=${CMAKE_SOURCE_DIR}/tests/screenshots/particlesgpu
            -DMANY=${CMAKE_SOURCE_DIR}/tests/screenshots/particlesgpumany
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/particles-gpu.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_particles_gpu_gate.cmake)
    set_tests_properties(particles_gpu_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        RUN_SERIAL TRUE
        TIMEOUT 240)

    # `BasePart.CastShadow` under the sun and under a lamp, and the sun's
    # shadow under an orthographic camera at all (D536): see the driver.
    add_test(NAME cast_shadow_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/castshadow
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/cast-shadow
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_cast_shadow_gate.cmake)
    set_tests_properties(cast_shadow_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # A window the system holds -- its title bar held, on Windows -- does not
    # stop the loop (D535): see the driver. Timed, so alone.
    if(WIN32)
        add_test(NAME window_hold_gate
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DSCENE=${CMAKE_SOURCE_DIR}/tests/screenshots/windowhold
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_window_hold_gate.cmake)
        set_tests_properties(window_hold_gate PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            RUN_SERIAL TRUE
            TIMEOUT 120)
    endif()

    # A decal's three blend modes (ADR 0160): multiplied it darkens, laid over
    # glowing or added it brightens.
    add_test(NAME decal_blend_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/decalblend
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/decal-blend.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_decal_blend_gate.cmake)
    set_tests_properties(decal_blend_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **What the anti-aliasing and the upscale leave alone** (ADR 0158): the
    # screen's UI, a picture of sprites alone, and pixel art among 3D surfaces
    # -- see the driver for each claim.
    add_test(NAME anti_aliasing_2d
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCENES=${CMAKE_SOURCE_DIR}/tests/screenshots/aa2d
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/anti-aliasing-2d
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_aa_2d_gate.cmake)
    set_tests_properties(anti_aliasing_2d PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 240)

    # Local shadows, as a differential and for exactly the same reason -- see
    # the driver. `SpotLight.Shadows` and `PointLight.Shadows` were accepted,
    # read back and plumbed to `RenderLight::shadows` for three milestones while
    # nothing acted on them, and no instrument in this repository could tell.
    #
    # Not a `gpu-golden`: there is no checked-in image and no reference GPU. It
    # compares this machine's own renders against each other, which is why it is
    # a gate everywhere rather than only where a golden was recorded.
    add_test(NAME local_shadow_differential
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/localshadow
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/local-shadow-differential
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_local_shadow_gate.cmake)
    set_tests_properties(local_shadow_differential PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # **The terrain gallery** (terrain audit T0): the ground photographed from
    # 5 m to 2 km with the sky magenta and the ground white, and every picture
    # checked for sky seen through the ground or where the full-detail picture
    # had ground -- see the driver. The test every earlier terrain audit
    # lacked.
    #
    # Until the audit's T2 it was expected to fail -- the 2 m slab vanished
    # from 500 m on, and the seams were open -- and passed only on that
    # verdict. Gathered, stitched and morphed (ADR 0140), nothing shows sky
    # through it or in its place, and it is an ordinary test.
    add_test(NAME terrain_gallery_holes
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DHOLES=$<TARGET_FILE:imgholes>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terraingallery
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-gallery
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_gallery.cmake)
    set_tests_properties(terrain_gallery_holes PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **The terrain's shadow on itself, and the bend of its normal** (terrain
    # audit T3): ground nothing shadows, photographed at every cascade's
    # distance at a low sun and a high one, and no face to the sun darkened by
    # the map or the contact mask -- acne -- and no crease in the shading the
    # ground does not have -- the streaks of the owner's photo at a low sun.
    # See the driver.
    add_test(NAME terrain_shadow_acne
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSHADOW=$<TARGET_FILE:imgshadow>
            -DSTEPS=$<TARGET_FILE:imgsteps>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainshadow
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-shadow
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_shadow.cmake)
    set_tests_properties(terrain_shadow_acne PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 600)

    # **A slab's shadow on itself** (D506): a thin slab the sun meets at a low
    # angle, photographed at every cascade's distance with the sun's shadows
    # and without, and the two pictures the same -- a cascade's texel wider
    # than the slab is thick darkened its top. See the driver.
    add_test(NAME mesh_shadow_acne
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DCOMPARE=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/meshshadow
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/mesh-shadow
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_mesh_shadow.cmake)
    set_tests_properties(mesh_shadow_acne PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A crowd of one skinned mesh is drawn as runs** (H2): nine bending bars,
    # each posed by its own palette from the frame's palette buffer, against
    # the same nine drawn a copy at a time -- the same picture, and fewer
    # draws. See the driver.
    add_test(NAME skinned_crowd_runs
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DCOMPARE=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/skinnedcrowd
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/skinned-crowd
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_skinned_crowd.cmake)
    set_tests_properties(skinned_crowd_runs PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A terrain layer's repeat is broken up** (ADR 0113's amendment): the
    # built-in grass photographed from above as it repeated, as it is by
    # default and on hexagonal cells, and its autocorrelation at the tile's
    # period measured -- the first must fail the limits the others pass. See
    # the driver.
    add_test(NAME terrain_tiling
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DTILE=$<TARGET_FILE:imgtile>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terraintiling
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-tiling
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_tiling.cmake)
    set_tests_properties(terrain_tiling PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **A streamed terrain drawn whole** (ADR 0144): ground three kilometres
    # across, streamed round the camera at 300 m, against the same ground held
    # whole -- one picture, from a kilometre and from two. See the driver.
    add_test(NAME terrain_far
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainfar
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-far
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_far.cmake)
    set_tests_properties(terrain_far PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **Ground wider than the camera sees reaches the far plane, and ends there
    # on a line** (D403): a flat slab 12 km across under a camera that sees
    # five. See the driver.
    add_test(NAME terrain_far_plane
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainfarplane
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-far-plane
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_far_plane.cmake)
    set_tests_properties(terrain_far_plane PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **Paint that became the ground, beside paint that has not** (D396): the
    # ground under the paint must not show through along the line between
    # them. See the driver.
    add_test(NAME terrain_paint_edge
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSTEPS=$<TARGET_FILE:imgsteps>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainpaintedge
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-paint-edge
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_paint_edge.cmake)
    set_tests_properties(terrain_paint_edge PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **A terrain layer reads only the channels its maps promise** (terrain
    # audit TA13): the same ground with a metallic-roughness map whose R is
    # empty, and with none, drawn the same; and a normal map's up bending the
    # ground up the image. See the driver.
    add_test(NAME terrain_material_channels
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainchannels
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-channels
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_channels.cmake)
    set_tests_properties(terrain_material_channels PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **A card's shadow has the card's holes** (terrain audit T3): foliage
    # cast solid rectangles, whatever its image's alpha. See the driver.
    add_test(NAME foliage_card_shadows
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSHADOW=$<TARGET_FILE:imgshadow>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/foliagecards
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/foliage-cards
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_foliage_cards.cmake)
    set_tests_properties(foliage_card_shadows PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # **The CPU's rules are the shader's** (terrain audit T3): the ground as
    # the rules draw it, and as the CPU writes it with `ApplyRules`, agree but
    # for their edges. See the driver.
    add_test(NAME terrain_rules_agree
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainrules
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-rules
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_rules.cmake)
    set_tests_properties(terrain_rules_agree PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **Every face of a block in its block's colour** (D380): on Vulkan a
    # block's sides drew black, its palette past the 4 KiB SDL_GPU binds a
    # uniform block with. See the driver.
    add_test(NAME block_faces_colours
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/blockfaces
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/block-faces.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_block_faces.cmake)
    set_tests_properties(block_faces_colours PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **A ViewportFrame draws in a world with no camera** (G7): the views were
    # drawn only when the world had one, and a menu's previews were empty.
    add_test(NAME viewport_frame_without_world_camera
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/viewportnocamera
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/viewport-no-camera.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_viewport_no_camera.cmake)
    set_tests_properties(viewport_frame_without_world_camera PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **Every part of an instanced run drawn where it is** (D484): on D3D12 a
    # run drawn after single draws on other meshes read every instance from
    # the first, the vertex buffer views keeping the single draws' stride of
    # zero -- SDL's patch 0003. See the driver.
    add_test(NAME instanced_run_after_single_draws
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/instancestride
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/instance-stride.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_instance_stride.cmake)
    set_tests_properties(instanced_run_after_single_draws PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # E5's screenshot row, which sat PENDING because the overlay it named was an
    # ImGui panel and a panel cannot render headlessly. The grid is drawn through
    # `DebugDraw` now -- in the world, where a question about space belongs --
    # so the picture goes through the ordinary renderer and this can take it.
    #
    # A differential for the same reason the one above is: there is no reference
    # GPU and no checked-in image, so what it compares is this machine's own
    # frame with the grid on against the same frame with it off.
    # --- Every example boots, and boots CLEAN (S7.7) ------------------------
    #
    # **Five of these were booted by no gate on any machine.** An example is a
    # promise that a thing works, and the ones nothing runs are the ones that
    # quietly stop working: a renamed property, a moved asset, a class that
    # started refusing something. None of that breaks a build.
    #
    # The assertion is `FAIL_REGULAR_EXPRESSION` on the log's own error and
    # warning prefixes rather than a per-example string. That is the check that
    # actually catches decay: a missing mesh, a refused write, a script that
    # raised on tick one and left the world empty are all a `[error]` line, and
    # every one of them ends with the host still exiting zero and reporting its
    # frames. "It ran" is exactly the claim that was already true of a broken
    # example.
    #
    # `--rhi=null` because none of it needs a device: what is under test is the
    # script, the scene and the assets, and a CI runner with no Vulkan driver
    # must not turn "this machine has no graphics" into a red build.
    # **What a player says it reads** (D517): `ludwerk build` asks the player it
    # packages whether it reads the mesh format its asset compiler writes, by
    # exit code. Its own format, yes; one past it, no.
    add_test(NAME host_reads_its_mesh_format COMMAND engine_host --reads-mesh-format=3)
    add_test(NAME host_refuses_a_newer_mesh_format COMMAND engine_host --reads-mesh-format=4)
    set_tests_properties(host_refuses_a_newer_mesh_format PROPERTIES WILL_FAIL TRUE)

    foreach(example 01-instances 02-meshes 03-physics-playground 06-scene 11-ocean 12-ragdoll 20-platformer
                    21-navigation 22-atmosphere 23-surfaces 24-scenes 25-gradients-and-strokes
                    26-security-cameras 27-mirrors-and-portals 28-arcade 29-meadow
                    30-interactions 31-lake-and-river 32-menus 33-options 34-settings-list)
        add_test(NAME example_boot_${example}
            COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/${example}"
                    --headless --frames=20 --exit --rhi=null)
        # Five minutes, not two: a boot that decodes a sky's worth of images
        # takes ten seconds here and forty-five on a hosted Windows runner that
        # is also drawing three terrain goldens in software -- and on a slow
        # afternoon it took more than a hundred and thirty, which turned a
        # green tree red (the run of e3dd5513). A boot that hangs still fails.
        set_tests_properties(example_boot_${example} PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[error\\];\\[warn\\]"
            TIMEOUT 300)
    endforeach()

    # `examples/12-ragdoll` builds its characters from a RIG, and that is the
    # part worth gating: `Ragdoll:Build` reads a skeleton, so a change that broke
    # the joint lookup, the skinned loader or the profile's alias list would
    # leave the example running perfectly and empty -- six characters standing
    # there that will not fall, with nothing in the log but a warning nobody
    # reads.
    #
    # So the assertion is the COUNT. Sixteen limbs is what the shipped humanoid
    # profile produces against the shipped rig, and any other number means one of
    # the two moved. `--rhi=null` because none of that needs a device.
    add_test(NAME example_ragdoll_builds
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/12-ragdoll" --headless --frames=10 --exit --rhi=null)
    set_tests_properties(example_ragdoll_builds PROPERTIES
        PASS_REGULAR_EXPRESSION "figure 6 built 16 limb\\(s\\)"
        TIMEOUT 60)

    # `examples/21-navigation`'s walker has to ARRIVE (ADR 0089): the maze is
    # built, `FindPath` answers, and a `CharacterBody` walking the corners
    # reaches the goal. A navigation that answered a straight line through the
    # walls would leave it pressed against one, and "it ran" would still be
    # true -- so the assertion is the arrival.
    add_test(NAME example_navigation_arrives
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/21-navigation" --headless --frames=1200 --exit --rhi=null)
    set_tests_properties(example_navigation_arrives PROPERTIES
        PASS_REGULAR_EXPRESSION "reached goal 1"
        TIMEOUT 120)

    add_test(NAME chunk_grid_differential
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/screenshots/chunkgrid
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/chunk-grid-differential
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_chunk_grid_gate.cmake)
    set_tests_properties(chunk_grid_differential PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # M8's editor-seam proof, and ADR 0017's standing condition coming due: two
    # `WorldHost`s alive at once, each with its own `ScriptRuntime` -- two Luau
    # VMs -- rendered into two targets.
    #
    # Not a `gpu-golden`: there is no checked-in image and no reference GPU. What
    # it compares is this machine's own renders against each other, which is why
    # it is a gate everywhere rather than only where the golden was recorded. It
    # skips where there is no device, like every other test that needs one.
    add_test(NAME editor_seam
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DROOT=${CMAKE_SOURCE_DIR}/tests/twoworlds
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/two-worlds
            -P ${CMAKE_SOURCE_DIR}/tests/twoworlds/run_two_worlds_gate.cmake)
    set_tests_properties(editor_seam PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # N1's acceptance gate (ADR 0069): the same harness inverted -- one project
    # booted as an authority and as its replica in one process over the memory
    # transport, and the replica must draw the authority's world. Only where the
    # module is built, because without it there is no replica to be.
    if(ENG_ENABLE_REPLICATION)
        add_test(NAME replica_seam
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DPROJECT=${CMAKE_SOURCE_DIR}/examples/15-multiplayer
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/replica-seam
                -P ${CMAKE_SOURCE_DIR}/tests/twoworlds/run_replica_gate.cmake)
        set_tests_properties(replica_seam PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 180)
    endif()

    # The M1 gate's reference screenshot: render a frame on a real GPU, read it
    # back, and compare against a checked-in golden with tools/imgcmp.
    #
    # This is the milestone's actual product -- from here on, any change with
    # visible output is verifiable by a machine rather than by looking at it
    # (MASTER_PROMPT.md §8, the observation rule). It skips rather than fails
    # where there is no GPU; see the driver script for why.
    add_test(NAME screenshot_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/pulse-frame30.png
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screenshot-gate.png
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/00-clear/init.luau
            -DFRAMES=30
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)
    # Labelled `gpu-golden` and EXCLUDED from CI (`ctest -LE gpu-golden`).
    #
    # A pixel golden is tied to the GPU that recorded it. Flat colour survived
    # the move -- tolerance 2 covers a unorm rounding difference -- but geometry
    # does not: line rasterization rules differ between a discrete GPU and the
    # runner's software rasterizer, and the gate went red on Windows CI the
    # moment the frame contained edges.
    #
    # The tempting fix is to widen the tolerance until it passes on both, which
    # would leave a gate that cannot see a real change either. architecture.md
    # §9 already says the other thing: the command stream is the BLOCKING gate
    # because it needs no GPU, and real-image comparison is the agent's own
    # verification tool -- run where the golden was recorded, which the roadmap
    # explicitly sanctions ("a scripted local gate on the dev machine, recorded
    # in the gate log either way").
    #
    # It runs on every `scripts/localgate.ps1`, and this label is what keeps it
    # from becoming a gate nobody trusts.
    set_tests_properties(screenshot_gate PROPERTIES
        LABELS "gpu-golden"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **The UI's pixels, which nothing compared for three milestones** (S8.4).
    #
    # `tests/screenshots/ui-1280x720.png` has been checked in since M6 as the
    # gate record's evidence, read by no test and re-recorded by nothing -- so it
    # drifted out of date, silently, and by the time anybody looked it differed
    # from the renderer by 1,585 pixels.
    #
    # It is worth reading rather than deleting, and D026 is the reason: `upload`
    # records a buffer's SIZE and not its contents, so the capture gate beside
    # this one is structurally blind to the QUADS. The two capture goldens prove
    # the scissors, the viewport and the draw count; only pixels can say the
    # layout put anything where it belongs. Between them and `engine_ui_tests`,
    # which asserts the rectangles exactly, this is the third leg.
    add_test(NAME screenshot_gate_ui
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/ui-1280x720.png
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screenshot-gate-ui.png
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/uipanel
            -DFRAMES=2
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)
    # `gpu-golden` for the reason the other two carry it: this frame is mostly
    # text, and glyph rasterization is the first thing to differ between one GPU
    # and another.
    set_tests_properties(screenshot_gate_ui PROPERTIES
        LABELS "gpu-golden"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # The INSTANCED path's golden, and it exists because that path shipped at
    # M7.5 drawing nothing at all (D043). Every gate the milestone had was
    # satisfied: the draw-call count fell from 15,250 to 22, the command stream
    # matched its capture, and the frame got eight times faster -- because the
    # geometry was being transformed off-screen. **A command stream and a
    # counter can both be right about a frame that is empty**, and the only
    # instrument that can tell is one that looks at pixels.
    #
    # `tests/screenshots/specular` is eight identical polished spheres, which is
    # a batch of eight and therefore the instanced path, plus a floor that is
    # not. It is also the evidence scene for geometric specular antialiasing --
    # one scene, because both claims are about what a curved metal surface
    # LOOKS like.
    #
    # `gpu-golden` for the same reason `screenshot_gate` is: this frame has
    # edges, and edges do not survive a move to a software rasterizer.
    add_test(NAME screenshot_gate_instanced
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/specular-frame120.png
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screenshot-gate-instanced.png
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/specular
            -DFRAMES=120
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)
    set_tests_properties(screenshot_gate_instanced PROPERTIES
        LABELS "gpu-golden"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # The 2D layer's sprite pass (phase 3): an orthographic camera over a
    # tilemap and one of each thing a `Part2D` is drawn as. The scene says what
    # each element proves; the golden is what they looked like when checked by
    # eye. `gpu-golden` like its neighbours: the circle and the capsule are
    # anti-aliased edges.
    add_test(NAME screenshot_gate_sprites
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/sprites-frame5.png
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screenshot-gate-sprites.png
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/sprites
            -DFRAMES=5
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)
    set_tests_properties(screenshot_gate_sprites PROPERTIES
        LABELS "gpu-golden"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # --- The lavapipe goldens, which both documents promise (S7.6) -----------
    #
    # The same two scenes as the gates above, against images recorded on Mesa's
    # software rasterizer instead of on the dev machine's GPU. The comparison is
    # EXACT -- tolerance 0, zero pixels allowed to differ -- and that is not
    # optimism: lavapipe renders the same scene to the same bytes twice, which
    # was measured before these were recorded. A golden that spans two
    # rasterizers has to widen its tolerance until it cannot see a real change;
    # one that spans none does not.
    #
    # Registered only under `ENG_LAVAPIPE_GOLDENS`, which is OFF. Both
    # `architecture.md` §9 and the roadmap say this suite is non-blocking, and a
    # Mesa upgrade in the base image moves every pixel of it -- so it must not be
    # able to redden a gate anybody runs before a push. `scripts/localgate.ps1
    # -Only lavapipe` and the nightly job are what turn it on.
    if(ENG_LAVAPIPE_GOLDENS)
        add_test(NAME lavapipe_golden_meshes
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DIMGCMP=$<TARGET_FILE:imgcmp>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/lavapipe/meshes.png
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/lavapipe-meshes.png
                -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/02-meshes
                -DFRAMES=30
                -DTOLERANCE=0
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)

        # The instanced path, and it is here for D043: that path shipped drawing
        # NOTHING while every gate it had was satisfied -- the draw count fell,
        # the command stream matched its capture, the frame got faster, because
        # the geometry was being transformed off-screen. A command stream and a
        # counter can both be right about a frame that is empty.
        add_test(NAME lavapipe_golden_specular
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DIMGCMP=$<TARGET_FILE:imgcmp>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/lavapipe/specular.png
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/lavapipe-specular.png
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/specular
                -DFRAMES=120
                -DTOLERANCE=0
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)

        # The UI, and this is the one of the three where an EXACT comparison
        # earns the most: the frame is mostly glyphs, and a tolerance wide
        # enough to survive two rasterizers is wide enough to hide a character
        # rendered in the wrong place.
        add_test(NAME lavapipe_golden_ui
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DIMGCMP=$<TARGET_FILE:imgcmp>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/lavapipe/ui.png
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/lavapipe-ui.png
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/uipanel
                -DFRAMES=2
                -DTOLERANCE=0
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake)

        # ADR 0096's looks, one image each, as the owner accepted them in
        # `docs/briefs/atmosphere-post/` (2026-09-25): the same scene, camera
        # and hour, with `tests/look/goldens.txt` naming the variants. 640x360,
        # because the claim is "this look", and a smaller picture makes it
        # for a sixth of the bytes.
        file(STRINGS ${CMAKE_SOURCE_DIR}/tests/look/goldens.txt look_goldens)
        set(look_golden_tests "")
        foreach(variant IN LISTS look_goldens)
            add_test(NAME lavapipe_golden_look_${variant}
                COMMAND ${CMAKE_COMMAND}
                    -DHOST=$<TARGET_FILE:engine_host>
                    -DIMGCMP=$<TARGET_FILE:imgcmp>
                    -DSCENE=${CMAKE_SOURCE_DIR}/tests/look
                    -DVARIANT=${variant}
                    -DDRIVER=${CMAKE_SOURCE_DIR}/tests/screenshots/run_screenshot_gate.cmake
                    -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/screenshots/lavapipe/look-${variant}.png
                    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/lavapipe-look-${variant}.png
                    -DFRAMES=30
                    -DWIDTH=640
                    -DHEIGHT=360
                    -DTOLERANCE=0
                    -P ${CMAKE_SOURCE_DIR}/tests/look/run_look_gate.cmake)
            list(APPEND look_golden_tests lavapipe_golden_look_${variant})
        endforeach()

        set_tests_properties(lavapipe_golden_meshes lavapipe_golden_specular lavapipe_golden_ui ${look_golden_tests} PROPERTIES
            LABELS "lavapipe-golden"
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 300)
    endif()

    # --- Which tests may run beside which (ADR 0148) -------------------------
    #
    # The gates run ctest with as many jobs as the machine has cores; these are
    # the tests that may not simply share it.
    #
    # **Timed, so alone** (`RUN_SERIAL`): a frame ceiling or a tick budget
    # measured while twenty other processes compile shaders is a measurement
    # of the machine's load.
    #
    # `terrain_far_flight` is not one of them, though it is a soak: what it
    # asserts of time is the streaming's own share of a frame, read in the
    # thread's CPU time where the platform has it, and its frame is a software
    # rasteriser's on a runner -- five and a half minutes alone, with every
    # other test waiting (C0, `docs/briefs/streaming-cleanup.md`).
    foreach(timed IN ITEMS perf_budget streaming_soak openworld_soak)
        if(TEST ${timed})
            set_property(TEST ${timed} APPEND PROPERTY RUN_SERIAL TRUE)
        endif()
    endforeach()

    # **Pictures on a real GPU take turns, three at a time** (`RESOURCE_GROUPS
    # gpu:1` against the three slots `tests/ctest-resources.json` declares,
    # which the Windows and macOS test presets pass): each renders whole
    # frames at full resolution, the device's memory is one pool, and the
    # device-loss proof loses its own device among them. They run beside every
    # test that does not draw. Where the GPU is lavapipe -- the Linux
    # container, which is the CPU -- there is nothing to share but cores, and
    # no group is asked for.
    set(drawn_tests
        screenshot_gate screenshot_gate_contact screenshot_gate_ui screenshot_gate_instanced
        screenshot_gate_sprites ui_appearance_gate ui_layouts_gate sprite_color_gate views_gate mirrors_gate ortho_gate
        subworld_gate
        surface_contract_proof_meshes surface_contract_proof_specular user_surface_proof
        graphics_settings_differential graphics_runtime_differential options_screen_gate local_shadow_differential
        terrain_gallery_holes terrain_shadow_acne mesh_shadow_acne skinned_crowd_runs
        terrain_tiling terrain_far terrain_far_plane terrain_paint_edge terrain_material_channels
        terrain_far_flight foliage_card_shadows terrain_rules_agree block_faces_colours chunk_grid_differential
        editor_seam replica_seam descriptor_heap_overflow triangle_sample crash_gate device_loss_survived
        decal_blend_gate particles_gpu_gate cast_shadow_gate window_hold_gate anti_aliasing_2d)
    option(ENG_TEST_GPU_LOCK "Serialise the tests that draw on a real GPU" ${WIN32})
    if(APPLE)
        set(ENG_TEST_GPU_LOCK ON)
    endif()
    foreach(drawn IN LISTS drawn_tests)
        if(NOT TEST ${drawn})
            continue()
        endif()
        if(ENG_TEST_GPU_LOCK)
            set_property(TEST ${drawn} PROPERTY RESOURCE_GROUPS "gpu:1")
        endif()
        # **A timeout is a hang guard, not a budget.** Where the device is a
        # software rasteriser -- a hosted runner's WARP, the container's
        # lavapipe -- a picture is minutes of CPU, and drawn beside three
        # others it is three times that: `terrain_far` passed in 100 s alone
        # on CI and was cut at 300 s the first time it ran at `-j 4`.
        set_property(TEST ${drawn} PROPERTY TIMEOUT 900)
    endforeach()

    # **One project folder, one test at a time**: a run writes its log and its
    # caches beside the project, and two runs of one folder fight over both.
    set(shared_projects
        "examples/00-clear|capture_gate,screenshot_gate"
        "examples/05-streaming|asset_determinism,streaming_soak"
        "examples/12-ragdoll|example_boot_12-ragdoll,example_ragdoll_builds"
        "examples/21-navigation|example_boot_21-navigation,example_navigation_arrives"
        "tests/rendercapture/meshes|capture_gate_meshes,surface_contract_proof_meshes"
        "tests/rendercapture/uipanel|capture_gate_ui_1280x720,capture_gate_ui_640x360,screenshot_gate_ui"
        "tests/screenshots/specular|graphics_settings_differential,screenshot_gate_instanced,surface_contract_proof_specular"
        "tests/screenshots/usersurface|device_loss_survived,user_surface_proof")
    foreach(entry IN LISTS shared_projects)
        string(REPLACE "|" ";" parts "${entry}")
        string(REPLACE "," ";" parts "${parts}")
        list(POP_FRONT parts folder)
        foreach(test IN LISTS parts)
            if(TEST ${test})
                set_property(TEST ${test} APPEND PROPERTY RESOURCE_LOCK "project:${folder}")
            endif()
        endforeach()
    endforeach()
endif()
