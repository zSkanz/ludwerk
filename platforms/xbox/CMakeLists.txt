# The optional provider has its own SDK dependency; default players never link it.
set(ENG_GDK_ROOT "" CACHE PATH "Pinned GDK Windows native directory (ADR 0188)")
if(ENG_GDK_ROOT)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT EXISTS "${ENG_GDK_ROOT}/include/XGameRuntimeInit.h")
        message(FATAL_ERROR "ENG_GDK_ROOT must name a GDK Windows native directory")
    endif()
    add_library(engine_xbox_provider SHARED xbox_provider.cpp)
    set_target_properties(engine_xbox_provider PROPERTIES OUTPUT_NAME engine_xbox)
    target_compile_features(engine_xbox_provider PRIVATE cxx_std_20)
    target_compile_definitions(engine_xbox_provider PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _GAMING_DESKTOP)
    target_include_directories(engine_xbox_provider PRIVATE "${CMAKE_SOURCE_DIR}/engine/platform/include")
    target_include_directories(engine_xbox_provider SYSTEM PRIVATE "${ENG_GDK_ROOT}/include")
    target_link_libraries(engine_xbox_provider PRIVATE
        "${ENG_GDK_ROOT}/lib/x64/xgameruntime.lib"
        "${ENG_GDK_ROOT}/lib/x64/Microsoft.Xbox.Services.143.C.lib"
        "${ENG_GDK_ROOT}/lib/x64/libHttpClient.lib"
        winhttp crypt32 bcrypt ws2_32 appnotify)
    get_filename_component(ENG_MODULE_BUILD_ROOT "${CMAKE_BINARY_DIR}" DIRECTORY)
    set(ENG_XBOX_MODULE_DIR "${ENG_MODULE_BUILD_ROOT}/modules/microsoft-gdk/2604.5.7903/windows-x64")
    add_custom_command(TARGET engine_xbox_provider POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:engine_host>/integrations/xbox"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:engine_xbox_provider>"
            "${ENG_GDK_ROOT}/bin/x64/xgameruntime.dll"
            "${ENG_GDK_ROOT}/bin/x64/libHttpClient.dll"
            "$<TARGET_FILE_DIR:engine_host>/integrations/xbox"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ENG_XBOX_MODULE_DIR}"
        # The module's directory takes the provider alone: the vendor's
        # libraries above are for running here, from this developer's own
        # SDK, and are not part of what the engine distributes (ADR 0188).
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:engine_xbox_provider>"
            "${ENG_XBOX_MODULE_DIR}")
endif()
