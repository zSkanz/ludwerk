# L2 -- the network seam (architecture.md §2 `net_api`, ADR 0012).
#
# v1 ships primitives only, and M3 needs exactly one of them: the WebSocket
# CLIENT the engine dials the dev server with (ADR 0035). Nothing here is bound
# to Luau and no game script can reach it (R17); `@std/net` in the game VM is M7.
#
# The protocol half is separated from the socket half so that every rule RFC
# 6455 states is tested against the RFC's own published vectors rather than
# against our output.

engine_add_module(net_api
    LAYER 2
    INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include
    SOURCES
        src/async_client.cpp
        src/digest.cpp
        src/enet_transport.cpp
        src/http.cpp
        src/memory_transport.cpp
        src/tcp.cpp
        src/websocket.cpp
        src/websocket_protocol.cpp
    DEPS
        core
)

# The only platform library the sockets need, and it is Windows-only: POSIX
# sockets live in libc. Linked PUBLIC because a static library carries no
# transitive link edges of its own -- whoever links net_api needs it too.
if(WIN32)
    target_link_libraries(engine_net_api PUBLIC ws2_32)
endif()

# **`https://` is the platform's** (ADR 0063): one backend per target, each the
# TLS its operating system keeps patched. Linux loads the system's OpenSSL at
# the first request (`dl`), so no OpenSSL is needed to build and a machine
# without one fails that request rather than refusing to start.
if(WIN32)
    target_sources(engine_net_api PRIVATE src/https_winhttp.cpp)
    target_link_libraries(engine_net_api PUBLIC winhttp)
elseif(ANDROID)
    # The Java stack, through the platform's JNI environment.
    target_sources(engine_net_api PRIVATE src/https_android.cpp)
    target_link_libraries(engine_net_api PRIVATE engine::platform)
elseif(APPLE)
    enable_language(OBJCXX)
    target_sources(engine_net_api PRIVATE src/https_apple.mm)
    set_source_files_properties(src/https_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(engine_net_api PUBLIC "-framework Foundation")
else()
    target_sources(engine_net_api PRIVATE src/https_openssl.cpp)
    target_link_libraries(engine_net_api PUBLIC ${CMAKE_DL_LIBS})
endif()

# A test may trust a root of its own on the platforms that let it (Linux), and
# only a build with tests carries the hook: a player build cannot be told to
# trust anything the operating system does not.
target_compile_definitions(engine_net_api PRIVATE ENG_NET_TEST_HOOKS=$<BOOL:${ENG_BUILD_TESTS}>)

# ENet, PRIVATE: `transport.h` exposes a factory and no ENet type, so nothing
# that links `net_api` needs its headers -- which is the R17 rule holding in the
# build graph rather than only in the header.
target_link_libraries(engine_net_api PRIVATE enet::enet)

engine_add_module_tests(net
    MODULES
        net_api
    SOURCES
        tests/async_client_tests.cpp
        tests/digest_tests.cpp
        tests/http_tests.cpp
        tests/https_tests.cpp
        tests/memory_transport_tests.cpp
        tests/transport_tests.cpp
        tests/websocket_client_tests.cpp
        tests/websocket_protocol_tests.cpp
)

if(ENG_BUILD_TESTS)
    target_compile_definitions(engine_net_tests PRIVATE
        ENG_TEST_CATALOG="${CMAKE_SOURCE_DIR}/i18n/en.json")
    # The HTTPS tests' own TLS server: a certificate made at run time, and
    # Schannel serving it (`tests/tls_test_server.h`).
    if(WIN32)
        target_link_libraries(engine_net_tests PRIVATE crypt32 ncrypt secur32)
    else()
        target_link_libraries(engine_net_tests PRIVATE ${CMAKE_DL_LIBS})
    endif()
endif()
