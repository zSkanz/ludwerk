# L6 -- the host. This is the one module allowed to see everything. The VM moved
# into `engine/script` in the kernel, as planned: what is left here is the world it runs
# in, the project it is mounted from, and the frame loop that drives it
# (`world_host.h`).

engine_add_module(app
    LAYER 6
    SOURCES
        src/world_host.cpp
        src/luau_flags.cpp
        src/project_watch.cpp
        src/external_changes.cpp
        src/managed_world.cpp
        src/simulated_headset.cpp
        src/headset_hands.cpp
        src/manager_link.cpp
        src/manager_main.cpp
        src/admin_channel.cpp
        src/admin_client.cpp
        src/admin_server.cpp
        src/bots.cpp
        src/health_server.cpp
        src/match_keys.cpp
        src/servers_config.cpp
        src/world_manager.cpp
        src/travel_pass.cpp
        src/server_console.cpp
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
        src/require_paths.cpp
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
        src/debug_measure.cpp
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
        src/doc_summary.cpp
        src/tip.cpp
        src/content_tree.cpp
        src/content_import.cpp
        src/icons.cpp
        src/thumbnails.cpp
        src/dev_control.cpp
        src/language_packs.cpp
        src/baked_light.cpp
        src/indirect_light.cpp
        src/mesh_rays.cpp
        src/sequences.cpp
        src/timeline.cpp
        src/profile_history.cpp
        src/node_canvas.cpp
        src/graph_editor.cpp
        src/graph_panel.cpp
        src/mouths.cpp
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
if(ENG_RHI_D3D12)
    target_link_libraries(engine_app PRIVATE engine::rhi_d3d12)
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

# **Every Luau flag, by the library that reads it** (ADR 0217), in every build:
# a project's `[luau.flags]` reach a shipped game's VM as much as the editor's
# checker. Read from the vendored sources, so the lists move with the pin.
include(${CMAKE_SOURCE_DIR}/cmake/engine_luau_flags.cmake)
engine_luau_flags("${CMAKE_CURRENT_BINARY_DIR}/generated/luau_flags.gen.h")
target_include_directories(engine_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

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
    # **`assetc` is linked HERE and only here** (the compiled-assets work). It carries the
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
    target_sources(engine_app PRIVATE src/language_service.cpp src/script_check.cpp)
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
# **The repository this build came from**: where a development
# editor finds the CLI its Export window runs. Empty in the profiles that ship,
# which find theirs beside the executable.
if(ENG_DEBUG_UI)
    target_compile_definitions(engine_app PRIVATE ENG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
else()
    target_compile_definitions(engine_app PRIVATE ENG_SOURCE_DIR="")
endif()


if(ENG_PLATFORM_UWP)
    add_executable(engine_host WIN32 src/main.cpp "${CMAKE_SOURCE_DIR}/platforms/uwp/player.cpp")
    set_target_properties(engine_host PROPERTIES OUTPUT_NAME engine-host)
    target_link_libraries(engine_host PRIVATE WindowsApp)
    target_link_options(engine_host PRIVATE /APPCONTAINER)
elseif(ANDROID)
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
    # **Nor does the editor**: opened from the folder, or started by the
    # launcher, it was a console program, and a console window opened behind it
    # -- the second, empty window seen while a project loaded.
    if(WIN32 AND ENG_PROFILE MATCHES "^(player|shipping|editor)$")
        target_link_options(engine_host PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        target_compile_definitions(engine_host PRIVATE ENG_GUI_SUBSYSTEM=1)
    endif()
endif()
target_link_libraries(engine_host PRIVATE engine::app engine::warnings)

# The application's identity, embedded in the artifact rather than left beside
# it. Windows only here because a Windows icon IS a PE resource;
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
    # What `@std/net` and `@std/json` ARE, as types: the modules are native,
    # and these are their declarations, which the script editor's checker
    # reads to follow a require of one (`LanguageTree::libraryRoot`). Beside
    # the host, because the checker is the host's: read from a project's
    # `.engine/types/`, a project whose copy was a version old was a page of
    # errors about a module the engine has.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/runtime/std"
        "${CMAKE_CURRENT_BINARY_DIR}/content/runtime/std"
    COMMENT "Staging the engine's Luau modules")
add_dependencies(engine_host engine_host_runtime)

# **The OpenXR loader beside the host** (ADR 0244), where SDL looks for it
# when a game asks for a headset. Not linked: a game without XR never loads it.
if(TARGET engine_openxr_loader)
    add_dependencies(engine_host engine_openxr_loader)
    add_custom_command(TARGET engine_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:engine_openxr_loader>" "$<TARGET_FILE_DIR:engine_host>")
endif()

add_custom_command(TARGET engine_host POST_BUILD
    # The default typeface (a deliberate choice of 2026-08-20). Content
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
    # One binary, four tests that run side by side. As one it was 474 s on a
    # hosted runner, over half the 900 a test is given, and three files were
    # two thirds of it: the ground that streams, the cases that compile a
    # surface with the real compiler, and the matches played over a wire.
    SHARD_FILES
        "streaming|*field_streamer_tests.cpp"
        "shaders|*surface_compiler_tests.cpp,*graph_editor_tests.cpp"
        "network|*network_session_tests.cpp,*network_relay_tests.cpp"
    SOURCES
        tests/pointer_ownership_tests.cpp
        tests/ui_navigation_tests.cpp
        tests/ui_pointer_tests.cpp
        tests/frame_pacing_tests.cpp
        tests/frame_scheduler_tests.cpp
        tests/text_input_focus_tests.cpp
        tests/headset_hands_tests.cpp
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
        tests/external_changes_tests.cpp
        tests/admin_server_tests.cpp
        tests/health_server_tests.cpp
        tests/match_keys_tests.cpp
        tests/server_console_tests.cpp
        tests/join_request_tests.cpp
        tests/world_manager_tests.cpp
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
        tests/debug_measure_tests.cpp
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
        tests/editor_drive_tests.cpp
        tests/doc_summary_tests.cpp
        tests/tip_tests.cpp
        tests/ui_theme_tests.cpp
        tests/ui_text_tests.cpp
        tests/thumbnail_tests.cpp
        tests/script_document_tests.cpp
        tests/class_favorites_tests.cpp
        tests/script_editor_settings_tests.cpp
        tests/editor_performance_tests.cpp
        tests/script_editor_tests.cpp
        tests/script_complete_tests.cpp
        tests/language_packs_tests.cpp
        tests/indirect_light_tests.cpp
        tests/mesh_rays_tests.cpp
        tests/sequences_tests.cpp
        tests/profile_history_tests.cpp
        tests/node_canvas_tests.cpp
        tests/graph_editor_tests.cpp
        tests/graph_panel_tests.cpp
        tests/screen_split_tests.cpp
        tests/mouths_tests.cpp
)

# The wire and the world hash describe the same state: a gate that needs the
# replication module's own field applier, so it is built where the module is.
# The language service's own suite, where the service is (ADR 0093).
if(ENG_BUILD_TESTS AND ENG_DEBUG_UI)
    target_sources(engine_app_tests PRIVATE tests/language_service_tests.cpp tests/luau_flags_tests.cpp)
    # The Timeline panel, drawn against an ImGui with no window (ADR 0204).
    target_sources(engine_app_tests PRIVATE tests/timeline_panel_tests.cpp tests/profiler_panel_tests.cpp
                                           tests/performance_preferences_tests.cpp)
    target_link_libraries(engine_app_tests PRIVATE engine::imgui)
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
    target_sources(engine_app_tests PRIVATE tests/wire_hash_tests.cpp tests/network_session_tests.cpp
                                            tests/network_relay_tests.cpp)
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

    # **The compiler, for the cut-over's end-to-end case** (the compiled-assets work).
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
    # And one real face, so the glyph atlas has something in it.
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_FONT="${ENG_THIRD_PARTY_DIR}/inter/docs/font-files/InterVariable.ttf")

    # One real mesh, so the preview's RENDER half has something to draw.
    # The engine's staged content is shaders, icons and templates -- no models --
    # so a test that looked there skipped on every machine, which is the shape
    # to avoid. Borrowed from `asset`'s fixtures rather than copied: two
    # copies of one quad is two things to keep in step.
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_MESH="${CMAKE_SOURCE_DIR}/engine/asset/tests/data/quad.gltf")
    # And one model that reads an image beside itself, with two images to put
    # there: the foliage cards' own, borrowed for the same reason.
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_CARD_DIR="${CMAKE_SOURCE_DIR}/tests/screenshots/foliagecards")

    # A folder of real textures for the skipped thumbnail bench, overridable
    # because the interesting answer comes from somebody's OWN 4K art rather than
    # from a fixture. Defaults to the icon masters, which are the only sizeable
    # pictures the repository carries.
    if(NOT DEFINED ENG_TEST_THUMBNAIL_DIR)
        set(ENG_TEST_THUMBNAIL_DIR "${CMAKE_SOURCE_DIR}/icons/default/class")
    endif()
    target_compile_definitions(engine_app_tests PRIVATE
        ENG_TEST_THUMBNAIL_DIR="${ENG_TEST_THUMBNAIL_DIR}")

    # --- The shell, registered so it can report NOT having run --
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

    # The bootstrap's deliverable, still asserted: the host runs a script and prints a
    # catalog-resolved greeting.
    #
    # `--headless --frames=3 --exit` is not decoration. Since the first window the host runs a
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

    # **A long frame's warning is about that frame**: it split the
    # frame BEFORE the long one into phases, so a tick that stood still for
    # most of a second was reported as a simulation that took nothing and "the
    # rest" that took all of it. A server, because a served match is on the
    # real clock and a headless run is not.
    add_test(NAME long_frame_names_its_phases
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/serve/long_frame.luau" --serve=47914 --rhi=null)
    set_tests_properties(long_frame_names_its_phases PROPERTIES
        PASS_REGULAR_EXPRESSION "simulation and its scripts [1-9][0-9][0-9][.][0-9]+ ms"
        TIMEOUT 60)

    # The frame loop terminates on its budget and says what it ran. Separate
    # from host_boot because "the script ran" and "the loop stopped" are
    # different claims, and a hang should name which one broke.
    add_test(NAME host_headless_frames COMMAND engine_host --headless --frames=5 --exit --rhi=null)
    set_tests_properties(host_headless_frames PROPERTIES
        PASS_REGULAR_EXPRESSION "Ran 5 frames"
        TIMEOUT 60)

    # **A frame of many draws must not write past a GPU descriptor heap.**
    # Real device, debug layer asked for by name (`--gpu-debug`), because the
    # layer is what turns the out-of-bounds write into a failure instead of a
    # silent corruption -- without the SDL patch this dies on D3D12 with
    # EXECUTION ERROR #646. A machine with no device skips rather than fails.
    add_test(NAME descriptor_heap_overflow
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/descriptorheap/transparent_crowd.luau"
                --headless --frames=8 --exit --rhi=sdlgpu --gpu-debug)
    set_tests_properties(descriptor_heap_overflow PROPERTIES
        PASS_REGULAR_EXPRESSION "Ran 8 frames"
        SKIP_REGULAR_EXPRESSION "rhi.err.device_create_failed"
        TIMEOUT 120)

    # **A GPU with no depth clamping is still a GPU.** The device such a
    # phone is given, asked for by name on whatever this machine has
    # (`--gpu-least`): every pipeline clips where it clamped, and with the
    # debug layer on a pipeline that asked for a feature the device was made
    # without is a failure rather than a driver's guess.
    add_test(NAME host_least_gpu_features
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/descriptorheap/transparent_crowd.luau"
                --headless --frames=8 --exit --rhi=sdlgpu --gpu-debug --gpu-least --log-keys)
    set_tests_properties(host_least_gpu_features PROPERTIES
        PASS_REGULAR_EXPRESSION "rhi.info.no_depth_clamp.*Ran 8 frames"
        SKIP_REGULAR_EXPRESSION "rhi.err.device_create_failed"
        TIMEOUT 120)

    # **Two eyes, drawn and different** (ADR 0244), under the engine's own
    # headset (ADR 0248 §4): no runtime and no window, so it runs where the
    # GPU tests run, and skips where there is no device.
    add_test(NAME xr_simulated_eyes
        COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DSCENE=${CMAKE_SOURCE_DIR}/examples/06-scene
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/xr-simulated-eyes
                -P ${CMAKE_SOURCE_DIR}/tests/xr/run_simulated_eyes.cmake)
    set_tests_properties(xr_simulated_eyes PROPERTIES
        PASS_REGULAR_EXPRESSION "two eyes drawn, they differ, and the window mirrors the left"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # **A screen is a panel in a headset** (ADR 0246 §1): laid out against the
    # panel's canvas and drawn into the eyes; and a hand pointing at its
    # button with the trigger pulled presses it, and so does a fingertip
    # pushed through it (§2).
    add_test(NAME xr_simulated_panel
        COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DSCENE=${CMAKE_SOURCE_DIR}/tests/xr/xr_panel.luau
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/xr-simulated-panel
                -P ${CMAKE_SOURCE_DIR}/tests/xr/run_simulated_panel.cmake)
    set_tests_properties(xr_simulated_panel PROPERTIES
        PASS_REGULAR_EXPRESSION "the screen is a panel, laid out on its canvas, and a hand pointing at its button, or a fingertip poking it, presses it"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **The room seen through** (ADR 0247 §1): the eyes are clear where
    # nothing is drawn and solid where the floor is.
    add_test(NAME xr_simulated_passthrough
        COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DPROBE=$<TARGET_FILE:imgprobe>
                -DSCENE=${CMAKE_SOURCE_DIR}/tests/xr/xr_passthrough.luau
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/xr-simulated-passthrough
                -P ${CMAKE_SOURCE_DIR}/tests/xr/run_simulated_passthrough.cmake)
    set_tests_properties(xr_simulated_passthrough PROPERTIES
        PASS_REGULAR_EXPRESSION "the room shows where nothing is drawn"
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **Where the player is, to a script** (ADR 0245): `XRService` under the
    # simulated headset -- enabled, the head standing at 1.6 m -- and flat,
    # where every pose is the identity. A script then puts the head and a
    # hand (`SimulatePose`), reads the play area, recentres and sits down;
    # flat, each of those is a no-op or nil.
    add_test(NAME xr_service_simulated
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/xr/xr_service.luau" --headless --frames=50 --exit
                --rhi=null --xr=simulated)
    set_tests_properties(xr_service_simulated PROPERTIES
        PASS_REGULAR_EXPRESSION
            "\\[xr\\] enabled=true head=1.60 left=false.*\\[xr\\] simulated=true head=1.00,1.70,2.00 left=true 1.20 area=3.00x3.00 state=Focused.*\\[xr\\] recentred=-?0.00,1.70,-?0.00 look=-?0.00,-1.00 seated=0.10 space=Seated moves=true states=Focused joints=true"
        TIMEOUT 60)
    # **Moving in a headset without being sick** (ADR 0246 §3):
    # `@engine/xrcomfort` under the simulated headset -- a teleport lands on
    # the floor and never on a wall, the view fades as it moves, and a snap
    # turn turns 30 degrees each push.
    add_test(NAME xr_comfort
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/xr/xr_comfort.luau" --headless --frames=60 --exit
                --rhi=null --xr=simulated)
    set_tests_properties(xr_comfort PROPERTIES
        PASS_REGULAR_EXPRESSION
            "\\[comfort\\] landed=-0.20,-2.2[0-9] faded=1.00 walled=-0.20,-2.2[0-9] turned=-30,-60 rest=0.00"
        TIMEOUT 60)
    # **The headset's timing in the log** (what the first test on a device
    # sends back): every five seconds of frames, the display's rate and the
    # frames' own, the late and the undrawn, the eyes' passes.
    add_test(NAME xr_timing_logged
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/xr/xr_service.luau" --headless --frames=400 --exit
                --rhi=null --xr=simulated)
    set_tests_properties(xr_timing_logged PROPERTIES
        PASS_REGULAR_EXPRESSION "Headset: display 72(\\.0)? Hz, frames every 13\\.89 ms over 3[0-9][0-9] frames, 0 late, 0 not drawn"
        TIMEOUT 60)
    add_test(NAME xr_service_flat
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/tests/xr/xr_service.luau" --headless --frames=50 --exit --rhi=null)
    set_tests_properties(xr_service_flat PROPERTIES
        PASS_REGULAR_EXPRESSION
            "\\[xr\\] enabled=false head=0.00 left=false.*\\[xr\\] simulated=false head=0.00,0.00,0.00 left=false 0.00 area=nil state=None.*\\[xr\\] recentred=0.00,0.00,0.00 look=-?0.00,-1.00 seated=0.00 space=Seated moves=false states= joints=true"
        TIMEOUT 60)

    # **The render half of the frame runs headless**: a Render-rate
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

    # What a scene is handed is JSON, and text that is not is refused
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

    # The kernel's gate: ~900 Luau specs written against docs/api-design.md by authors
    # who were forbidden from reading `engine/`. That is what
    # makes them a contract rather than a transcript, and it is why they are run
    # here rather than trusted.
    #
    # `--rhi=null` because the suite tests the kernel and not the renderer: a
    # machine with no GPU should still be able to prove the engine's semantics.
    add_test(NAME conformance
        COMMAND engine_host --run-tests=${CMAKE_SOURCE_DIR}/tests/conformance --rhi=null)
    set_tests_properties(conformance PROPERTIES TIMEOUT 300)

    # The kernel's other gate: same script and same seed, twice, 10,000 ticks,
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

    # Three of the input and UI work's gate items, and all three are replays with a FACT to prove
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

    # The perf threshold (the 10k-parts / 1k-listeners churn
    # benchmark "with a CI threshold", and the 500-instance gate scene). Both
    # budgets are deliberately loose -- see `bench.h`: a CI runner's speed varies
    # by more than the regression anyone would want to catch, so this fails on a
    # catastrophe and the recorded baselines holds the real numbers.
    #
    # One repeat here rather than the default three. The gate asks whether the
    # engine still works, and three runs of a 10,000-instance scene is thirty
    # seconds of CI for a number nobody reads.
    # The streaming work's gate: "asset build determinism check in CI". Two PROCESSES rather
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

    # An answer from the compiler's cache is the bytes a compile gives, and a
    # second compile through it compiles nothing: what an export reading the
    # editor's import cache rests on. And models compiled side by side are the
    # models compiled with one worker. A tree of models with their pictures.
    add_test(NAME asset_cache
        COMMAND ${CMAKE_COMMAND}
            -DASSETC=$<TARGET_FILE:assetc>
            -DCONTENT=${CMAKE_SOURCE_DIR}/examples/37-character/content
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/asset-cache
            -P ${CMAKE_SOURCE_DIR}/tests/assetdeterminism/run_cache_gate.cmake)
    set_tests_properties(asset_cache PROPERTIES TIMEOUT 300)

    # The streaming work's gate: the scripted five-minute fly-through over `examples/05-streaming`,
    # asserting peak memory under a declared ceiling and a frame-time histogram
    # with no hitch over 33 ms.
    #
    # Eighteen thousand frames is five minutes of SIM time and about ten seconds
    # of wall clock, because the fixed 1/60 s headless step decouples the two.
    # That is the whole reason a five-minute gate is affordable at all.
    #
    # THE THIRD ASSERTION IS THE ONE THAT EARNED ITS PLACE. A streaming world once degraded from
    # 100 fps to 35 over five minutes with no hitch anywhere and no ceiling
    # breach -- a thousand instances leaked every fifteen seconds. `soak.h`
    # explains why the instance count flattening is the property that separates
    # a streamed world from a growing one.
    #
    # **Re-measured at the compiled-assets work's cut-over** (ADR 0065), which is one of that
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
            # **The revisited-place check lives here**, because this fly-through is
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

    # The flagship work's gate: the ten-minute soak over the flagship, at the length a test
    # suite can afford to run every time. **The full ten minutes is a release's
    # gate step**; this is a hundred seconds of
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
    # **Re-measured at the compiled-assets work's cut-over** (ADR 0065), and the interesting part is
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
    # **The netcode acceptance gate**:
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

    # **The same gate with a guest at the joining machine** (ADR 0208): two
    # characters predicted on one machine, each held to the plan by itself
    # at 0, 50, 150 and 300 ms -- no correction on a straight walk, stopped
    # where the authority stopped, and the guest's character answering its
    # stick in the tick the first player's does. And one run more, the
    # authority's word a metre off for the guest alone: the guest is corrected
    # and the first player's character is not moved. Real time, two minutes.
    add_test(NAME netcode_acceptance_guest
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/acceptance
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-acceptance-guest
            -DPORT=47970
            -DGUEST=1
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_netcode_acceptance.cmake)
    set_tests_properties(netcode_acceptance_guest PROPERTIES TIMEOUT 600 LABELS "netcode" RUN_SERIAL TRUE)

    # **A driven vehicle** (ADR 0213): a car its player drives, predicted on
    # that player's machine as the character is -- a straight, a corner and a
    # stop at 0, 50, 150 and 300 ms, held to no correction, to where the
    # authority stopped, and to answering the stick within a tick of the
    # character; and one run into a parked car. Real time, a minute and a half.
    add_test(NAME netcode_vehicle
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/vehicle
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-vehicle
            -DPORT=47920
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_netcode_vehicle.cmake)
    # **A timing test, and run alone** (`perf`): it measures how a car is
    # drawn tick by tick at 300 ms, which a machine building another tier
    # beside it changes. The local gate runs `perf` after every other lane;
    # in CI it is serial in its own job already.
    set_tests_properties(netcode_vehicle PROPERTIES TIMEOUT 600 LABELS "netcode;perf" RUN_SERIAL TRUE)

    # **What a breaking wall costs on the wire** (ADR 0215): fifty pieces,
    # sent as parts while they fall and not once they lie still. Fifteen
    # seconds of real time.
    add_test(NAME netcode_fracture
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/fracture
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-fracture
            -DPORT=47928
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_fracture.cmake)
    set_tests_properties(netcode_fracture PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

    # **`examples/15-multiplayer` with a second player at the joining machine**
    # (ADR 0208): the example's own test run seats a guest over a link held
    # 75 ms each way and holds both sticks for two seconds, and both racers
    # have gone the full sixteen metres on the joiner -- predicted, each from
    # its own player's intent. Ten seconds of real time.
    add_test(NAME example_multiplayer_guest
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/15-multiplayer
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/example-multiplayer-guest
            -DPORT=47980
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_example_guest.cmake)
    set_tests_properties(example_multiplayer_guest PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

    # **What a character carries reaches the other machines** (protocol 44):
    # a real server and a real joiner. The server spawns a thing from a stamp,
    # as a `Clone` and with `Instance.new`, each carrying a light of either
    # kind, a cape's chain and collider, a bone, a highlight, a trail, a beam,
    # an emitter with sequences of its own and a playing sound; the joiner says
    # what it was sent of each against what the server holds, then again after
    # the server has changed one property of each. Every one of the sixty lines
    # but the emitter's was missing before, and the emitter's differed. The
    # joiner draws nothing, so this runs where there is no GPU.
    add_test(NAME netcode_carried
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/carried
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-carried
            -DPORT=47970
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_carried.cmake)
    # Alone, as the acceptance gate is: two processes that wait on each other
    # in real time.
    set_tests_properties(netcode_carried PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

    # **A character the server animates is animated on a joiner** (ADR 0197,
    # protocol 44): a real server spawns one from a stamp and walks it, runs
    # it, makes it jump and sets the one attribute that is its attack; a real
    # joiner, which makes nothing and plays nothing, says what the animation
    # graph on ITS machine did. The speeds it read, the jump, the fall, the
    # landing, the slash and the clips' events are held, and that its graph
    # did in order what the server's did. Seventeen seconds of real time.
    # **Two first opens at once**: a never-opened project opened by
    # two hosts in the same moment, six times over, and each asked whether
    # the model's clips are there. Nothing is drawn.
    add_test(NAME two_first_opens
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/firstopen
            -DCONTENT=${CMAKE_SOURCE_DIR}/tests/netcode/animated/content
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/two-first-opens
            -P ${CMAKE_SOURCE_DIR}/tests/firstopen/run_two_first_opens.cmake)
    set_tests_properties(two_first_opens PROPERTIES TIMEOUT 120)

    add_test(NAME netcode_animated
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/animated
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-animated
            -DPORT=47980
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_animated.cmake)
    set_tests_properties(netcode_animated PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

    # **A line a server says is heard in each joiner's own language** (ADR
    # 0200, protocol 45), and is as long on both. The server resolves in the
    # project's default language, whose recording is half a second; the
    # joiner runs in another, whose recording is nine tenths. Both hold the
    # line for nine tenths and the same number of ticks, the joiner reads the
    # caption in its own text language, and a line nobody recorded is silent
    # and lasts its caption on both. Eight seconds of real time.
    add_test(NAME netcode_dubbed
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/dubbed
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-dubbed
            -DPORT=47990
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_dubbed.cmake)
    set_tests_properties(netcode_dubbed PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

    # **A sequence a server plays is seen by a joiner** (ADR 0204, protocol
    # 47): one already going is met where it has got to, one played while it
    # watches is heard event for event over the same number of ticks with the
    # lamp it keys arriving as the lamp does, a skip is heard as a skip and a
    # stop finishes nothing. Nine seconds of real time.
    add_test(NAME netcode_cutscene
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/cutscene
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-cutscene
            -DPORT=47950
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_cutscene.cmake)
    set_tests_properties(netcode_cutscene PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

    # **Somebody else at a joiner's screen is a player of the match** (ADR
    # 0205, protocol 48): one connection seats a guest, and the two are two
    # players to the authority -- two numbers, two intents a tick, two
    # senders and two addressees -- and the guest leaves by itself. See the
    # driver. Six seconds of real time.
    add_test(NAME netcode_guests
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/netcode/guests
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/netcode-guests
            -DPORT=47930
            -P ${CMAKE_SOURCE_DIR}/tests/netcode/run_guests.cmake)
    set_tests_properties(netcode_guests PROPERTIES TIMEOUT 300 LABELS "netcode" RUN_SERIAL TRUE)

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
            # omission.** The revisited-place check needs a path that doubles back, and
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
    # found three leaks that rose with distance before it was a gate
    # -- that the ground under it came in and went out, and that the far
    # ground was drawn from its files with none made again.
    #
    # Four kilometres at a metre voxel: 4 356 cells and thirty-six blocks,
    # laid in a quarter of a minute. On a real device (see the script): it
    # takes its turn with the tests that draw. The ceiling is the peak
    # measured, with headroom on the soaks' terms; the recorded baselines
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
    # Room for two flights: one that fails is flown again (see the driver).
    set_tests_properties(terrain_far_flight PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 1500)

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

        add_test(NAME capture_gate_terrain_bindings
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DSCENARIO=${CMAKE_SOURCE_DIR}/tests/rendercapture/terrain
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-terrain-bindings.jsonl
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_terrain_bindings.cmake)
        set_tests_properties(capture_gate_terrain_bindings PROPERTIES TIMEOUT 60)

        # The first renderer's gate: three camera angles by two lighting states, as six
        # consecutive frames of one stream. Every camera position and clock
        # value is a literal at a named tick -- an orbit driven by elapsed time
        # would make the golden depend on how many ticks had passed when it was
        # recorded, and a one-tick shift changes every number in a command
        # stream.
        add_test(NAME capture_gate_meshes
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DGOLDEN=${CMAKE_SOURCE_DIR}/tests/rendercapture/meshes-6frames.jsonl
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture-gate-meshes.jsonl
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/meshes
                -DFRAMES=6
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_capture_gate.cmake)
        set_tests_properties(capture_gate_meshes PROPERTIES TIMEOUT 120)

        # The input and UI work's gate: the SAME UI script captured at two resolutions, as two
        # goldens. The pair is the check -- a layout that ignored the window
        # would produce two identical streams, and the two differ in exactly the
        # way scaling predicts: the clipped strip's scissor is 512,668,704x40 at
        # 1280x720 and 256,308,352x40 at 640x360, so the scale-driven numbers
        # halve and the offset-driven height does not.
        #
        # What these goldens can and cannot see is worth knowing before trusting
        # them: `upload` records a buffer's SIZE and not its contents, so
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

        # The skinned gate. Four frames of one animated mesh beside one that
        # is not animated at all, which is what makes it able to tell "the
        # skinned path ran" from "everything went through the skinned path".
        #
        # Unlike the UI pair, this golden CAN see the thing it is about: the
        # joint palette goes up as a uniform block and `bindUniforms` records
        # contents (the half of the capture's blindness to uploads that was fixed with the rendering work), so a pose that
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
        # recorded after the look was accepted (2026-09-25). Blocking on
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

        # The rendering work's gate addition, and the one check in this file that is not a
        # golden: one scene at two `ClockTime` values, and the two frames must
        # differ. See the driver for why a golden could not make that claim --
        # and did not, for the whole of the first renderer.
        add_test(NAME clock_differential
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/clock-differential.jsonl
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/rendercapture/clockpair
                -P ${CMAKE_SOURCE_DIR}/tests/rendercapture/run_clock_differential.cmake)
        set_tests_properties(clock_differential PROPERTIES TIMEOUT 120)
    endif()

    # The contact-shadow golden, and it exists because the artifact was
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

    # **A picture drawn small in the interface is drawn from its smaller
    # levels**: a checkerboard of single texels at an icon's sizes is
    # an even grey, where it was a pattern of its own. See the driver.
    add_test(NAME ui_minify_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/uiminify
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ui-minify.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_ui_minify_gate.cmake)
    set_tests_properties(ui_minify_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **A project `ludwerk check` calls clean is clean in the editor** (ADR
    # 0093): the script pane's checker, with no pane, over a project that
    # requires by path, over every example, and over two mistakes it must say.
    # See the driver. No window and no device: it is not one of the drawn.
    add_test(NAME script_check_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/tests/scriptcheck/project
            -DEXAMPLES=${CMAKE_SOURCE_DIR}/examples
            -DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/script-check-broken
            -P ${CMAKE_SOURCE_DIR}/tests/scriptcheck/run_script_check_gate.cmake)
    set_tests_properties(script_check_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A game learns how its last run ended, and where its report is** (ADR
    # 0187): six runs in a row against one log -- none, a script error, a real
    # fault, clean again. See the driver. Nothing is drawn: no device.
    add_test(NAME last_run_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECTS=${CMAKE_SOURCE_DIR}/tests/lastrun
            -DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/last-run-gate
            -P ${CMAKE_SOURCE_DIR}/tests/lastrun/run_last_run_gate.cmake)
    set_tests_properties(last_run_gate PROPERTIES TIMEOUT 300)

    # **A start that fails leaves a trace, and a log begins at the beginning**:
    # a run refused for an option it does not have leaves a start log
    # with what ran and why it stopped, and an ordinary run's log opens on the
    # line written before anything could fail. See the driver. No device.
    add_test(NAME start_trace
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/instancestride
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/start-trace
            -P ${CMAKE_SOURCE_DIR}/tests/lastrun/run_start_trace.cmake)
    set_tests_properties(start_trace PROPERTIES TIMEOUT 120)

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
    # however low it stands.
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

    # The flagship work's graphics settings family, as a differential. See the driver for why
    # the claim is "these two must differ" rather than a golden: a preset that
    # is parsed and then reaches nothing renders exactly what it did before.
    add_test(NAME graphics_settings_differential
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/specular
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/settings-differential
            -DFRAMES=10
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
    # Four runs of the host on whatever device there is; a software one takes
    # two to three minutes for them, and a busy runner longer.
    set_tests_properties(anti_aliasing_flicker PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 480)

    # **What a temporal upscaler is for, measured** (ADR 0164): a still scene
    # at half the resolution, brought up by FSR 2 and by FSR 1, and how far
    # each is from the picture at full resolution -- see the driver.
    add_test(NAME temporal_upscale_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/upscale
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/temporal-upscale
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_upscale_gate.cmake)
    set_tests_properties(temporal_upscale_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 480)

    # **What a made frame has to be, measured** (ADR 0165): a scene drawn with
    # frame generation, and drawn again at twice the frames without, so every
    # made frame has a drawn one to be held against -- see the driver. On
    # Windows it then runs with a window, whose made frames take another path.
    add_test(NAME frame_generation_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DIMGCMP=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/framegen
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/frame-generation
            -DWINDOW=$<BOOL:${WIN32}>
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_frame_generation_gate.cmake)
    set_tests_properties(frame_generation_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 600)

    # A thing that moves has moved over the whole of its face: its
    # motion is drawn against the depth pass's depth, and must make that depth
    # to the bit -- see the driver.
    add_test(NAME motion_whole_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/motionwhole
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/motion-whole
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_motion_whole_gate.cmake)
    set_tests_properties(motion_whole_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

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
    # shadow under an orthographic camera at all: see the driver.
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
    # stop the loop: see the driver. Timed, so alone.
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

    # `BasePart.ReceivesDecals`: a part with it off is left as it was by a
    # decal that paints the floor under it and the part beside it.
    add_test(NAME decal_receives_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/decalreceives
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/decal-receives
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_decal_receives_gate.cmake)
    set_tests_properties(decal_receives_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # The thin wall (ADR 0203): a room shut on every side stays dark beside a
    # room open to the sky, with ten centimetres of wall between them.
    add_test(NAME thin_wall_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/thinwall
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/thin-wall
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_thin_wall_gate.cmake)
    set_tests_properties(thin_wall_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **What a surface reflects of the screen, and the light that bounces off
    # it** (ADR 0209): a polished floor that shows a box where it stands now
    # and not where a probe has it, a chrome ball that keeps the probe's
    # reflection to the pixel, a floor reddened at the foot of a glowing wall
    # -- by a flag and by a level that arrives while the game runs -- a flat
    # lake held against the camera mirrored in its plane, and a frame with
    # both settings off that has no pass and no copy of theirs. See the
    # driver. Asked of the graphics API the host picks and, on Windows, of
    # Vulkan too: the two were found to disagree by eight pixels of creep
    # until both were run.
    add_test(NAME screen_light_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DMIRROR=$<TARGET_FILE:imgmirror>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/screenlight
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screen-light
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screen_light_gate.cmake)
    set_tests_properties(screen_light_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)
    if(WIN32)
        add_test(NAME screen_light_gate_vulkan
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DPROBE=$<TARGET_FILE:imgprobe>
                -DMIRROR=$<TARGET_FILE:imgmirror>
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/screenlight
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/screen-light-vulkan
                -DGPU=vulkan
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_screen_light_gate.cmake)
        set_tests_properties(screen_light_gate_vulkan PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 300)
    endif()

    # **The edge of the sun's shadow is a ramp, not a comb**: the
    # shadow's filter was turned by a different angle at every pixel where it
    # had no gaps for the turn to hide, and drew a fringe of hairs along every
    # edge that only a temporal pass was hiding. See the driver.
    add_test(NAME shadow_reach_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/shadowreach
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/shadow-reach
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_shadow_reach_gate.cmake)
    set_tests_properties(shadow_reach_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    add_test(NAME shadow_edge_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/39-interior
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/shadow-edge.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_shadow_edge_gate.cmake)
    set_tests_properties(shadow_edge_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A nearly mirror floor does not show the grid of the probe it reflects**:
    # a probe's sharpest level, magnified twenty times by a polished
    # floor and read plainly, drew its own texels as a fringe of feathers; it
    # is read through a cubic. See the driver.
    add_test(NAME probe_read_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/39-interior
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/probe-read.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_probe_read_gate.cmake)
    set_tests_properties(probe_read_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **No line of sun where a room's wall meets its ceiling**: the
    # shadow map's bias leaves a band lit under whatever casts, and the
    # contact mask, which is there to close it, faded over it and let a fifth
    # of the sun through. See the driver.
    add_test(NAME ceiling_joint_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/39-interior
            -DPART=masked
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ceiling-joint.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_ceiling_joint_gate.cmake)
    # **And with no contact shadow to hide a band**, a test of its
    # own: the four runs as one test met the class's ceiling on a
    # hosted runner. See the driver.
    add_test(NAME ceiling_joint_bare_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/39-interior
            -DPART=bare
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ceiling-joint.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_ceiling_joint_gate.cmake)
    set_tests_properties(ceiling_joint_gate ceiling_joint_bare_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A cloth's shadow does not speckle from frame to frame**: the
    # rule that tells a receiver under a caster from one in the clear took a
    # sheet for its own caster, which no still picture showed and a count of
    # pixels that pop from one frame to the next did. See the driver.
    add_test(NAME cloth_shadow_flicker_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DFLICKER=$<TARGET_FILE:imgflicker>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/35-cape
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/cloth-shadow-flicker
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_cloth_shadow_flicker_gate.cmake)
    # Alone: it writes and reads back six hundred and sixty pictures, and
    # the gate it first ran beside starved two tests of the clock.
    set_tests_properties(cloth_shadow_flicker_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        RUN_SERIAL TRUE
        TIMEOUT 600)

    # The white furnace (ADR 0203): a closed room gives back what
    # radiosity says it does -- no more light than its lamp put in -- and a
    # lamp that shines every way lights the ceiling above it.
    add_test(NAME furnace_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/furnace
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/furnace
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_furnace_gate.cmake)
    set_tests_properties(furnace_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # A volume's light, kept in a file (ADR 0207): the furnace baked by
    # `--bake-irradiance`, read back as the picture the rays made with no ray
    # cast, drawn as the file's mean where no volume is worked out, and
    # refused -- said once -- for a volume that is another's or a room that
    # has changed.
    add_test(NAME baked_room_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/furnace
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/baked-room
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_baked_room_gate.cmake)
    set_tests_properties(baked_room_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # Light that changes every frame (ADR 0203): two volumes under a torch
    # that flickers are both worked out, stay ready, and are lit again in
    # turn within the frame's budget.
    add_test(NAME relight_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/relight
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_relight_gate.cmake)
    set_tests_properties(relight_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 240)

    # Light that bounces in the open (ADR 0206): a volume carried with the
    # camera lights a place, the same place far off with the camera there, and the
    # place walked to; and a box too large for one volume is tiles.
    add_test(NAME open_field_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/openfield
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/open-field
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_open_field_gate.cmake)
    set_tests_properties(open_field_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # What a sequence puts over the picture (ADR 0204): `examples/42-nightfall`
    # comes up from black behind the bars of a wider picture.
    add_test(NAME nightfall_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/42-nightfall
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/nightfall
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_nightfall_gate.cmake)
    set_tests_properties(nightfall_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 240)

    # A room that is ONE mesh (ADR 0203) is lit through the openings it has:
    # its triangles are what a volume's rays meet, not its hull.
    add_test(NAME mesh_room_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/meshroom
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/mesh-room
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_mesh_room_gate.cmake)
    set_tests_properties(mesh_room_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # **Several players at one screen** (ADR 0205): five pictures of
    # `examples/41-couch` -- two side by side, three in the window's quarters,
    # two together in one picture, the two apart across a line, and one of
    # those two views whole, which is black on the side it is not seen on
    # because nothing is lit there. See the driver.
    # **A vehicle, looked at** (ADR 0213): the example driven at full throttle,
    # and the picture its camera takes -- the car where it looks, its rear
    # tyres darker than the road under it. Two hundred kilobytes of picture.
    # **A wall broken, looked at** (ADR 0215): untouched and after a ball, the
    # hole where it struck and the rest standing as it stood.
    add_test(NAME breaking_wall_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/45-breaking-wall
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/breaking-wall
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_breaking_wall_gate.cmake)
    set_tests_properties(breaking_wall_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A cape that is cloth, in a real run** (ADR 0214): three capes stepped
    # at High and none at Low, and the pixels that pop from frame to frame
    # with the cloth and without. Alone, for the reason the shadow flicker
    # gate is: it writes and reads back six hundred and sixty pictures.
    add_test(NAME cloth_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DFLICKER=$<TARGET_FILE:imgflicker>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/46-cloth-cape
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/cloth
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_cloth_gate.cmake)
    set_tests_properties(cloth_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        RUN_SERIAL TRUE
        TIMEOUT 900)

    add_test(NAME vehicle_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/44-vehicle
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/vehicle
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_vehicle_gate.cmake)
    set_tests_properties(vehicle_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    add_test(NAME split_screen_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/41-couch
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/split-screen
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_split_screen_gate.cmake)
    set_tests_properties(split_screen_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **The editor, looked at with no window** (`--editor-shots`): its
    # interface drawn into a headless frame while a script moves a mouse over
    # it, and each picture asked what a person would look for first. The
    # Timeline opened and scrubbed, and its three popups (ADR 0204); a colour
    # in a narrow row as its swatch; a skinned mesh's shape keys and
    # rig roles while editing, where they were missing until the game had
    # played, and a LipSync's map picker; an IKControl's joint picker
    # and a running character's animation graph.
    if(ENG_DEBUG_UI)
        # And the Profiler (ADR 0210): on a game that is playing, with its two
        # switches on and each tab looked at -- which is also what holds that
        # the editor draws at all while the GPU's passes are timed --
        # and on a profile a run with no editor wrote a moment before.
        # And the Graph panel (ADR 0211): a material graph opened, a link
        # refused, the list typed into and a compiler's error on its node --
        # on a copy of the example, because that drive saves a graph with a
        # line typed wrong, and with the error it is to say named; and a
        # character's animation graph while it is edited and while it runs.
        foreach(shots "timeline|40-cinematic" "timeline-popups|40-cinematic" "properties|40-cinematic"
                      "character|37-character" "profiler|43-ambush" "profiler-capture|43-ambush"
                      "graph-surface|23-surfaces" "graph-animation|37-character" "graph-running|37-character")
            string(REPLACE "|" ";" parts "${shots}")
            list(GET parts 0 shot_name)
            list(GET parts 1 shot_project)
            set(shot_capture "")
            if(shot_name STREQUAL "profiler-capture")
                set(shot_capture -DCAPTURE=${CMAKE_CURRENT_BINARY_DIR}/editor-shots-run.profile.json)
            endif()
            if(shot_name STREQUAL "graph-surface")
                set(shot_capture -DCOPY=1 -DSAID=nothingCalledThis)
            endif()
            add_test(NAME editor_shots_${shot_name}
                COMMAND ${CMAKE_COMMAND}
                    -DHOST=$<TARGET_FILE:engine_host>
                    -DPROBE=$<TARGET_FILE:imgprobe>
                    -DPROJECT=${CMAKE_SOURCE_DIR}/examples/${shot_project}
                    -DDRIVE=${CMAKE_SOURCE_DIR}/tests/editorshots/${shot_name}.drive
                    -DCHECKS=${CMAKE_SOURCE_DIR}/tests/editorshots/${shot_name}.checks
                    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/editor-shots-${shot_name}
                    ${shot_capture}
                    -P ${CMAKE_SOURCE_DIR}/tests/editorshots/run_editor_shots.cmake)
            set_tests_properties(editor_shots_${shot_name} PROPERTIES
                SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
                TIMEOUT 300)
        endforeach()

        # **And a game's own overlay, the same way**: the panel a player opens
        # with the overlay's key, drawn open into a headless frame of a run
        # that is not the editor. The Network panel's rows a player (ADR 0208)
        # were built and compiled for a batch with nobody able to see them
        # without a window on a screen, and the first picture found one of
        # them wrong. What this holds is that the picture can be taken.
        add_test(NAME overlay_shots_game
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DPROBE=$<TARGET_FILE:imgprobe>
                -DPROJECT=${CMAKE_SOURCE_DIR}/examples/15-multiplayer
                -DDRIVE=${CMAKE_SOURCE_DIR}/tests/editorshots/overlay.drive
                -DCHECKS=${CMAKE_SOURCE_DIR}/tests/editorshots/overlay.checks
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/overlay-shots-game
                -DGAME=1
                -P ${CMAKE_SOURCE_DIR}/tests/editorshots/run_editor_shots.cmake)
        set_tests_properties(overlay_shots_game PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 300)
    endif()

    # A decal paints the foliage that stands in it, and leaves a layer with
    # `ReceivesDecals` off as it was (ADR 0185).
    add_test(NAME foliage_decal_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/foliagedecal
            -DCARDS=${CMAKE_SOURCE_DIR}/tests/screenshots/foliagecards
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/foliage-decal
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_foliage_decal_gate.cmake)
    # Two software-rendered pictures of a field that fills them: 84 s on the
    # local Linux tier, where a hosted runner is slower.
    set_tests_properties(foliage_decal_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 600)

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

    # **The terrain gallery**: the ground photographed from
    # 5 m to 2 km with the sky magenta and the ground white, and every picture
    # checked for sky seen through the ground or where the full-detail picture
    # had ground -- see the driver. The test every earlier terrain audit
    # lacked.
    #
    # Until the audit it was expected to fail -- the 2 m slab vanished
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
    # audit): ground nothing shadows, photographed at every cascade's
    # distance at a low sun and a high one, and no face to the sun darkened by
    # the map or the contact mask -- acne -- and no crease in the shading the
    # ground does not have -- the streaks of a photo at a low sun.
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

    # **A slab's shadow on itself**: a thin slab the sun meets at a low
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

    # **A crowd of one skinned mesh is drawn as runs**: nine bending bars,
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

    # **A morphed vertex lands where the file says** (ADR 0196): pillars drawn
    # with morph targets against the same pillars made in that shape with
    # none -- the same picture, lit, depth and shadow. On the graphics API the
    # host picks, and on Windows on Vulkan as well: the shader finds a vertex's
    # deltas by the vertex's number, which the two APIs count differently for
    # a mesh that does not start at nought in its buffer. See the driver.
    add_test(NAME morph_landing
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DCOMPARE=$<TARGET_FILE:imgcmp>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/morphlanding
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/morph-landing
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_morph_landing.cmake)
    set_tests_properties(morph_landing PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)
    if(WIN32)
        add_test(NAME morph_landing_vulkan
            COMMAND ${CMAKE_COMMAND}
                -DHOST=$<TARGET_FILE:engine_host>
                -DCOMPARE=$<TARGET_FILE:imgcmp>
                -DPROBE=$<TARGET_FILE:imgprobe>
                -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/morphlanding
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/morph-landing-vulkan
                -DGPU=vulkan
                -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_morph_landing.cmake)
        set_tests_properties(morph_landing_vulkan PROPERTIES
            SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
            TIMEOUT 300)
    endif()

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
    # on a line**: a flat slab 12 km across under a camera that sees
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

    # **Paint that became the ground, beside paint that has not**: the
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
    # audit): the same ground with a metallic-roughness map whose R is
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

    # **A terrain layer whose material gives off light glows** (ADR 0113,
    # amended): rock and lava with no light in the world, on the full, the
    # lean and the fast ground. See the driver.
    add_test(NAME terrain_glow
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/terrainglow
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/terrain-glow
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_terrain_glow.cmake)
    set_tests_properties(terrain_glow PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 180)

    # **A card's shadow has the card's holes**: foliage
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

    # **The CPU's rules are the shader's**: the ground as
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

    # **Every face of a block in its block's colour**: on Vulkan a
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

    # **A ViewportFrame draws in a world with no camera**: the views were
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

    # **A ViewportFrame is never a white square**, the frame it is first
    # shown least of all: seven pictures round the frame three of them are
    # made in, and no slot white in any.
    add_test(NAME viewport_frame_first_shown
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/viewportfirst
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/viewport-first
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_viewport_first_gate.cmake)
    set_tests_properties(viewport_frame_first_shown PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **Every part of an instanced run drawn where it is**: on D3D12 a
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

    # **A run of blended parts is the parts drawn one at a time**: panes of
    # glass that overlap, in three colours at depths that interleave, drawn
    # as runs and a copy at a time -- the same picture to the pixel, in fewer
    # draws. The driver is the skinned crowd's, which asks exactly that.
    add_test(NAME blended_runs
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DCOMPARE=$<TARGET_FILE:imgcmp>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/blendedruns
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/blended-runs
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_skinned_crowd.cmake)
    set_tests_properties(blended_runs PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A cape's chains are found and stepped in a real run** (ADR 0194): the
    # example's two figures, three columns each, drawn for a second and a
    # half -- six chains of thirty joints in the frame report, and no warning.
    add_test(NAME secondary_motion_steps
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/35-cape
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_secondary_motion.cmake)
    set_tests_properties(secondary_motion_steps PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **The face example's faces move** (ADR 0196): three busts, one talking
    # by its clip's weights, one moved by the script's `SetMorphWeight`, one
    # at rest -- eight morph draws in the frame report, and no warning.
    add_test(NAME shape_keys_example
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/36-face
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_face_example.cmake)
    set_tests_properties(shape_keys_example PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A device taken away underneath the host is survived** (SDL patch
    # 0004): removed for real a third of the way through the same example --
    # the host ends saying its device is gone, where three hosts on hosted
    # runners died of an access violation, and says the sentence every test
    # that draws is skipped on. Not of that class itself: it prints the
    # sentence on purpose. Its own skip word, for a machine with no device.
    add_test(NAME device_removed_survived
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/36-face
            "-DTAKEN=${ENG_DEVICE_TAKEN_AWAY}"
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_device_removed.cmake)
    set_tests_properties(device_removed_survived PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_NO_DEVICE_HERE"
        TIMEOUT ${ENG_DEVICE_TEST_TIMEOUT})
    if(ENG_TEST_GPU_LOCK)
        set_property(TEST device_removed_survived PROPERTY RESOURCE_GROUPS "gpu:1")
    endif()

    # **And taken away as a picture is being read back** (the fourth
    # sighting): the same removal with a drive whose shot falls on the frame
    # after it. The host answered "could not read the rendered image" and
    # exit 1, which no test of the class skips on; it ends as a lost device
    # does. The game's overlay, so the editor is not needed for it.
    add_test(NAME device_removed_at_a_shot
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/15-multiplayer
            "-DTAKEN=${ENG_DEVICE_TAKEN_AWAY}"
            -DDRIVE=${CMAKE_SOURCE_DIR}/tests/screenshots/device_removed_at_a_shot.drive
            -DSHOTS=${CMAKE_CURRENT_BINARY_DIR}/device-removed-at-a-shot
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_device_removed.cmake)
    set_tests_properties(device_removed_at_a_shot PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_NO_DEVICE_HERE"
        TIMEOUT ${ENG_DEVICE_TEST_TIMEOUT})
    if(ENG_TEST_GPU_LOCK)
        set_property(TEST device_removed_at_a_shot PROPERTY RESOURCE_GROUPS "gpu:1")
    endif()

    # **And taken away where no call meets it**: on the run's last frame. The
    # host ended well and said nothing. A session asks its device as it ends,
    # whatever it is about to report (`engine::app::run`), which is the one
    # place every path passes -- this is a path nobody had listed.
    add_test(NAME device_removed_unnoticed
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/examples/15-multiplayer
            "-DTAKEN=${ENG_DEVICE_TAKEN_AWAY}"
            -DFRAMES=30
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_device_removed.cmake)
    set_tests_properties(device_removed_unnoticed PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_NO_DEVICE_HERE"
        TIMEOUT ${ENG_DEVICE_TEST_TIMEOUT})
    if(ENG_TEST_GPU_LOCK)
        set_property(TEST device_removed_unnoticed PROPERTY RESOURCE_GROUPS "gpu:1")
    endif()

    # **A machine short of memory loads less of each texture, and only of the
    # ones that can spare it**: at `Low` a material's texture is on
    # the card without its two largest levels, and a sprite's -- and one a
    # sprite shares with a material -- is whole. See the driver.
    add_test(NAME texture_quality_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/texturequality
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/texture-quality
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_texture_quality.cmake)
    set_tests_properties(texture_quality_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A scene that is left takes what it alone held with it**: a game
    # that goes lobby, map, lobby, another map, lobby holds in every lobby the
    # textures and buffers the first lobby held. See the driver.
    add_test(NAME scene_release_gate
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/sceneleak
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/scene-release
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_scene_release.cmake)
    set_tests_properties(scene_release_gate PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **The report's draws by kind add up to its total**, and name the kinds
    # the scene has. See the driver.
    add_test(NAME frame_report_draws_by_kind
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/instancestride
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_draw_kinds.cmake)
    # Two seconds on a GPU, and 66 to 74 on a hosted runner that rasterises
    # in software -- where the same test beside other neighbours has been
    # seen to take three times its own time (a boot of 41 s and of 125 s, on
    # code that had not changed). 120 was a ceiling at 1.6 times the usual,
    # and it was met on 2026-10-10 by a run with nothing slower in it.
    set_tests_properties(frame_report_draws_by_kind PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 300)

    # **A part in an instanced run is lit as the same part alone**: the
    # instanced shaders built their normal matrix transposed, and a turned
    # part's face in the sun was its dark one. See the driver.
    add_test(NAME instanced_part_lit_as_alone
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROBE=$<TARGET_FILE:imgprobe>
            -DSCRIPT=${CMAKE_SOURCE_DIR}/tests/screenshots/instancednormals
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/instanced-normals.png
            -P ${CMAKE_SOURCE_DIR}/tests/screenshots/run_instanced_normals.cmake)
    set_tests_properties(instanced_part_lit_as_alone PROPERTIES
        SKIP_REGULAR_EXPRESSION "ENG_TEST_SKIP"
        TIMEOUT 120)

    # The world-building work's screenshot row, which sat PENDING because the overlay it named was an
    # ImGui panel and a panel cannot render headlessly. The grid is drawn through
    # `DebugDraw` now -- in the world, where a question about space belongs --
    # so the picture goes through the ordinary renderer and this can take it.
    #
    # A differential for the same reason the one above is: there is no reference
    # GPU and no checked-in image, so what it compares is this machine's own
    # frame with the grid on against the same frame with it off.
    # --- Every example boots, and boots CLEAN ------------------------
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
    # **What a player says it reads**: `ludwerk build` asks the player it
    # packages whether it reads the mesh format its asset compiler writes, by
    # exit code. Its own format, yes; one past it, no.
    add_test(NAME host_reads_its_mesh_format COMMAND engine_host --reads-mesh-format=3)
    add_test(NAME host_refuses_a_newer_mesh_format COMMAND engine_host --reads-mesh-format=4)
    set_tests_properties(host_refuses_a_newer_mesh_format PROPERTIES WILL_FAIL TRUE)

    # **`--record-replay` with no `--replay` refuses to start**: given alone,
    # or with the folder as a word after it, what was left was a project path
    # and the host started as a game, with a window, on a folder of scenarios
    # (2026-10-11). It says the right form and goes. Headless and five frames here, so that
    # the day it does not refuse it shows nothing, ends, and fails for the words.
    add_test(NAME host_refuses_a_recording_of_no_replay
        COMMAND engine_host --headless --rhi=null --frames=5 --exit --record-replay
                "${CMAKE_SOURCE_DIR}/tests/determinism")
    set_tests_properties(host_refuses_a_recording_of_no_replay PROPERTIES
        PASS_REGULAR_EXPRESSION "--replay=tests/determinism --record-replay"
        TIMEOUT 30)

    foreach(example 01-instances 02-meshes 03-physics-playground 06-scene 11-ocean 12-ragdoll 20-platformer
                    21-navigation 22-atmosphere 23-surfaces 24-scenes 25-gradients-and-strokes
                    26-security-cameras 27-mirrors-and-portals 28-arcade 29-meadow
                    30-interactions 31-lake-and-river 32-menus 33-options 34-settings-list
                    35-cape 37-character 38-dubbing 39-interior 40-cinematic 41-couch 42-nightfall
                    43-ambush 44-vehicle 45-breaking-wall 46-cloth-cape 47-realms 48-xr-room)
        add_test(NAME example_boot_${example}
            COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/${example}"
                    --headless --frames=20 --exit --rhi=null)
        # Five minutes, not two: a boot that decodes a sky's worth of images
        # takes ten seconds here and forty-five on a hosted Windows runner that
        # is also drawing three terrain goldens in software -- and on a slow
        # afternoon it took more than a hundred and thirty, which turned a
        # green tree red. A boot that hangs still fails.
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

    # `examples/44-vehicle` drives (ADR 0213): with a throttle given and no
    # hand on the keys, four seconds take it tens of meters. A vehicle that
    # was built and never drove -- an engine rebuilt every tick, a chassis
    # whose wheels did not reach the ground -- runs perfectly and stands
    # still, so the assertion is the distance: ten meters at least.
    add_test(NAME example_vehicle_drives
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/44-vehicle" --headless --frames=300 --exit --rhi=null
                "--scene-data={\"throttle\":1}")
    set_tests_properties(example_vehicle_drives PROPERTIES
        PASS_REGULAR_EXPRESSION "drove ([1-9][0-9]|[1-9][0-9][0-9]+) m"
        FAIL_REGULAR_EXPRESSION "\\[error\\]"
        TIMEOUT 120)

    # `examples/48-xr-room` throws (ADRs 0244 to 0246): under the simulated
    # headset its autopilot puts the right hand on the ball, closes the grip,
    # swings forward and lets go, and the ball has to land a metre away at
    # least. A grab that never held, or a throw that dropped the ball at the
    # hand's feet, runs perfectly and goes nowhere.
    add_test(NAME example_xr_room_throws
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/48-xr-room" --headless --frames=170 --exit --rhi=null
                --xr=simulated "--scene-data={\"autopilot\":true}")
    set_tests_properties(example_xr_room_throws PROPERTIES
        PASS_REGULAR_EXPRESSION "threw the ball ([1-9]|[1-9][0-9])\\.[0-9] m"
        FAIL_REGULAR_EXPRESSION "\\[error\\]"
        TIMEOUT 120)

    # `examples/38-dubbing` says its lines (ADR 0200): twenty frames of a boot
    # end before the first of them. Eleven seconds is a round and a half --
    # every line found in the language asked for, each recording's viseme
    # track and the face map read, the line nobody dubbed and the one nobody
    # recorded -- in each of the two voices, and neither may warn.
    foreach(voice en pt-BR)
        add_test(NAME example_dubbing_speaks_${voice}
            COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/38-dubbing" --headless --frames=660 --exit --rhi=null
                    --locale=en --voice-locale=${voice})
        set_tests_properties(example_dubbing_speaks_${voice} PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[error\\];\\[warn\\]"
            TIMEOUT 300)
    endforeach()

    # `examples/40-cinematic` plays its sequence to the end (ADR 0204): twenty
    # frames of a boot end before its first line. Eighteen seconds is the whole
    # of it -- the camera's cuts, three lines found and said, the lamp and the
    # door keyed, both events and the end -- in each voice, and it may not warn.
    foreach(voice en pt-BR)
        add_test(NAME example_cinematic_plays_${voice}
            COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/40-cinematic" --headless --frames=1100 --exit --rhi=null
                    --locale=en --voice-locale=${voice})
        set_tests_properties(example_cinematic_plays_${voice} PROPERTIES
            PASS_REGULAR_EXPRESSION "event DoorOpen data=100 skipped=false.*finished skipped=false at 16.5"
            FAIL_REGULAR_EXPRESSION "\\[error\\];\\[warn\\]"
            TIMEOUT 300)
    endforeach()

    # `examples/43-ambush` plays to the end (ADR 0204): a guard a cast
    # track brought is there when the blow lands and gone when the file is
    # over, the flash is what the game said the blow was, a camera whose key
    # is eight metres off behind a wall is three and a half from the hero, a
    # function bound to the camera is asked, and a sequence the script wrote
    # as a table is played after the file's. Fifteen seconds, and no warning.
    add_test(NAME example_ambush_plays
        COMMAND engine_host "${CMAKE_SOURCE_DIR}/examples/43-ambush" --headless --frames=900 --exit --rhi=null)
    set_tests_properties(example_ambush_plays PROPERTIES
        PASS_REGULAR_EXPRESSION "struck, blow=2, the flash is 1\\.9[0-9]*, the guard is there=true.*kept in sight: 3\\.[3-5] m.*the guard is gone=true.*down, by a sequence the game wrote: 2.*asked every frame"
        FAIL_REGULAR_EXPRESSION "\\[error\\];\\[warn\\]"
        TIMEOUT 300)

    # `examples/37-character`'s walker walks at the speed it is asked for:
    # a hero's own body and the staff in its hand, which collide with
    # nothing, were ground under it, and it was carried along by what it
    # carried. Measured over ten seconds on a copy of the example, with what
    # its animation graph reads and says. See the driver.
    add_test(NAME example_character_walks
        COMMAND ${CMAKE_COMMAND}
            -DHOST=$<TARGET_FILE:engine_host>
            -DPROJECT=${CMAKE_SOURCE_DIR}/examples/37-character
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/character-walks
            -P ${CMAKE_SOURCE_DIR}/tests/examples/run_character_walks.cmake)
    set_tests_properties(example_character_walks PROPERTIES TIMEOUT 300)

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

    # The flagship work's editor-seam proof, and ADR 0017's standing condition coming due: two
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

    # The replica's acceptance gate (ADR 0069): the same harness inverted -- one project
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

    # The first window's gate's reference screenshot: render a frame on a real GPU, read it
    # back, and compare against a checked-in golden with tools/imgcmp.
    #
    # This is the milestone's actual product -- from here on, any change with
    # visible output is verifiable by a machine rather than by looking at it.
    # It skips rather than fails
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
    # because it needs no GPU, and real-image comparison is the capture's own
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

    # **The UI's pixels, which nothing compared for three milestones**.
    #
    # `tests/screenshots/ui-1280x720.png` has been checked in since the input and UI work as the
    # gate record's evidence, read by no test and re-recorded by nothing -- so it
    # drifted out of date, silently, and by the time anybody looked it differed
    # from the renderer by 1,585 pixels.
    #
    # It is worth reading rather than deleting, and a capture that recorded sizes alone is the reason: `upload`
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
    # the lighting work drawing nothing at all. Every gate the milestone had was
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

    # --- The lavapipe goldens, which both documents promise -----------
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

        # The instanced path, and it is here for what happened once: that path shipped drawing
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

        # ADR 0096's looks, one image each, as they were accepted: the same scene, camera
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
    # other test waiting.
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
        screenshot_gate_sprites ui_appearance_gate ui_layouts_gate sprite_color_gate views_gate ui_minify_gate mirrors_gate ortho_gate
        subworld_gate
        surface_contract_proof_meshes surface_contract_proof_specular user_surface_proof
        graphics_settings_differential graphics_runtime_differential options_screen_gate local_shadow_differential
        terrain_gallery_holes terrain_shadow_acne mesh_shadow_acne skinned_crowd_runs
        terrain_tiling terrain_far terrain_far_plane terrain_paint_edge terrain_material_channels terrain_glow
        terrain_far_flight foliage_card_shadows terrain_rules_agree block_faces_colours chunk_grid_differential
        editor_seam replica_seam descriptor_heap_overflow triangle_sample crash_gate device_loss_survived
        decal_blend_gate decal_receives_gate thin_wall_gate mesh_room_gate furnace_gate baked_room_gate relight_gate open_field_gate split_screen_gate overlay_shots_game editor_shots_timeline editor_shots_timeline-popups editor_shots_properties editor_shots_character editor_shots_profiler editor_shots_profiler-capture editor_shots_graph-surface editor_shots_graph-animation editor_shots_graph-running foliage_decal_gate particles_gpu_gate cast_shadow_gate window_hold_gate anti_aliasing_2d
        temporal_upscale_gate frame_generation_gate motion_whole_gate morph_landing shape_keys_example texture_quality_gate scene_release_gate
        screen_light_gate shadow_edge_gate shadow_reach_gate probe_read_gate ceiling_joint_gate ceiling_joint_bare_gate
        cloth_shadow_flicker_gate vehicle_gate breaking_wall_gate cloth_gate)
    if(WIN32)
        list(APPEND drawn_tests morph_landing_vulkan screen_light_gate_vulkan)
    endif()
    # **The class is every test that says `ENG_TEST_SKIP`** -- which is how a
    # test that needs a device says there is none -- and the names above are
    # the ones that draw and never skip. `cmake/engine_device_tests.cmake` has
    # what the class shares and why: the list above was once the whole of it,
    # kept by hand, and every test that met its own ceiling on a hosted
    # runner was one the list had not been told about.
    eng_mark_device_tests(${drawn_tests})

    # **One project folder, one test at a time**: a run writes its log and its
    # caches beside the project, and two runs of one folder fight over both.
    set(shared_projects
        "examples/00-clear|capture_gate,screenshot_gate"
        "examples/05-streaming|asset_determinism,streaming_soak"
        "examples/12-ragdoll|example_boot_12-ragdoll,example_ragdoll_builds"
        "examples/44-vehicle|example_boot_44-vehicle,example_vehicle_drives,vehicle_gate"
        "examples/45-breaking-wall|example_boot_45-breaking-wall,breaking_wall_gate"
        "examples/46-cloth-cape|example_boot_46-cloth-cape,cloth_gate"
        "examples/21-navigation|example_boot_21-navigation,example_navigation_arrives"
        "examples/36-face|shape_keys_example,device_removed_survived"
        "examples/37-character|example_boot_37-character,editor_shots_character,editor_shots_graph-animation,editor_shots_graph-running"
        "examples/38-dubbing|example_boot_38-dubbing,example_dubbing_speaks_en,example_dubbing_speaks_pt-BR"
        "examples/40-cinematic|example_boot_40-cinematic,example_cinematic_plays_en,example_cinematic_plays_pt-BR,editor_shots_timeline,editor_shots_timeline-popups,editor_shots_properties"
        "examples/41-couch|example_boot_41-couch,split_screen_gate"
        "examples/39-interior|example_boot_39-interior,shadow_edge_gate,probe_read_gate,ceiling_joint_gate,ceiling_joint_bare_gate"
        "examples/35-cape|example_boot_35-cape,cloth_shadow_flicker_gate"
        "tests/screenshots/screenlight|screen_light_gate,screen_light_gate_vulkan"
        "examples/15-multiplayer|example_boot_15-multiplayer,replica_seam,overlay_shots_game,device_removed_at_a_shot,device_removed_unnoticed"
        "examples/43-ambush|example_boot_43-ambush,example_ambush_plays,editor_shots_profiler,editor_shots_profiler-capture"
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
