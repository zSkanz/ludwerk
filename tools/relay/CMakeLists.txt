# tools/relay -- the relay a host behind a NAT is reached through (ADR 0178).
#
# A program of its own, and a small one: `engine::net_api` and what that links,
# with no window, no GPU and no SDL. It is what a game's maker runs on a machine
# the internet can reach -- one UDP port -- and nothing in the engine or the
# editor starts it.
#
# **It carries its messages inside it.** Every other binary reads the catalog
# from the `content/` beside it; this one is copied alone to a server, and a
# relay that printed `[i18n:missing]` for want of a folder would be a relay
# nobody could read the log of. The catalog is still the one source of the
# text (R3): the build turns `i18n/en.json` into bytes in the binary.
set(_catalog "${CMAKE_SOURCE_DIR}/i18n/en.json")
set(_embedded "${CMAKE_CURRENT_BINARY_DIR}/relay_catalog.gen.h")
add_custom_command(
    OUTPUT "${_embedded}"
    COMMAND ${CMAKE_COMMAND} -DCATALOG=${_catalog} -DOUT=${_embedded}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_catalog.cmake"
    DEPENDS "${_catalog}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_catalog.cmake"
    COMMENT "Embedding the message catalog in the relay"
    VERBATIM)

add_executable(engine_relay src/main.cpp "${_embedded}")
set_target_properties(engine_relay PROPERTIES OUTPUT_NAME engine-relay)
target_include_directories(engine_relay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(engine_relay PRIVATE engine::warnings engine::net_api engine::core)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    # Copied to a server whose C++ runtime is whatever its distribution has:
    # the relay brings its own.
    target_link_options(engine_relay PRIVATE -static-libstdc++ -static-libgcc)
endif()
