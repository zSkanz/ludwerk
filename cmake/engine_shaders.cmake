# HLSL -> SPIR-V / DXIL / MSL at build time (ADR 0006, architecture.md §8).
#
#     engine_add_shaders(<target> GLOB <pattern>... [SURFACES <pattern>...])
#
# `SURFACES` names surface shaders the engine ships (ADR 0091): each is wrapped
# by `surfacewrap` into every pass variant and compiled per stage, and lands in
# the manifest as `surface_<name>_<variant>` -- so a contract that stops
# compiling stops the build, on every backend.
#
# Attaches the shader build to an existing target: every matched `.hlsl` file is
# compiled once per backend format by the SDL_shadercross host tool, and the
# results land in `content/shaders/` **beside the target's executable**, next to
# a manifest the runtime reads to pick the blob for its active backend.
#
# Beside the executable, not under the binary dir, because that is where the
# engine looks: `platform::paths()` derives its content directory from the
# running binary's location, which is the shape a packaged build has. The
# message catalog is staged the same way for the same reason. Emitting straight
# there rather than emitting elsewhere and copying keeps one location and one
# write.
#
# Authoring convention, deliberately narrow while there is one pass to serve:
# one `.hlsl` file is one graphics pipeline, with entry points `VertexMain` and
# `FragmentMain`. Compute arrives with the milestone that needs it rather than
# as an unexercised branch here.
#
# Shared code goes in `shaders/include/*.hlsli`. shadercross emits no depfile,
# so every `.hlsli` is a dependency of every shader -- coarse, but a missed
# rebuild after a header edit is a far worse failure than a few extra
# recompiles of a handful of files.

function(engine_add_shaders target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "GLOB;SURFACES;COMPUTE")

    set(stages "vertex" "fragment")
    set(entry_vertex "VertexMain")
    set(entry_fragment "FragmentMain")

    # Backend format -> file extension and shadercross destination. The manifest
    # keys are these names, so the runtime maps its active backend to a key
    # rather than reconstructing paths.
    set(formats "spirv" "dxil" "msl")
    set(ext_spirv "spv")
    set(ext_dxil "dxil")
    set(ext_msl "msl")
    set(dest_spirv "SPIRV")
    set(dest_dxil "DXIL")
    set(dest_msl "MSL")

    if(NOT TARGET ${target})
        message(FATAL_ERROR "engine_add_shaders: '${target}' is not a target")
    endif()
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "engine_add_shaders(${target}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_GLOB)
        message(FATAL_ERROR "engine_add_shaders(${target}): GLOB requires at least one pattern")
    endif()

    if(NOT ENG_SHADER_TOOLCHAIN)
        # ADR 0032: on a host with no DirectXShaderCompiler there is no HLSL
        # front-end, so the blobs come from a Tier-1/Tier-2 host instead. Said
        # out loud, because a silently empty content/shaders would be found much
        # later and much more expensively.
        message(STATUS
            "engine: no shader toolchain on this host -- ${target} gets no compiled shaders (ADR 0032)")
        return()
    endif()

    set(sources "")
    foreach(pattern IN LISTS arg_GLOB)
        file(GLOB matched CONFIGURE_DEPENDS "${pattern}")
        list(APPEND sources ${matched})
    endforeach()
    if(NOT sources)
        message(FATAL_ERROR
            "engine_add_shaders(${target}): no .hlsl files matched ${arg_GLOB}")
    endif()
    # Sorted so the manifest is byte-identical across machines and filesystems.
    list(SORT sources)

    # Fixed by ADR 0006 ("shared code in shaders/include/*.hlsli") rather than
    # derived from the caller, so an `#include` resolves the same way no matter
    # which CMakeLists.txt asked for the compile.
    set(include_dir "${CMAKE_SOURCE_DIR}/shaders/include")
    set(include_args "")
    set(headers "")
    if(IS_DIRECTORY "${include_dir}")
        set(include_args -I "${include_dir}")
        file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${include_dir}/*.hlsli")
        list(SORT headers)
    endif()
    # **The vendored shader sources the engine's passes include** (ADR 0158):
    # SMAA's reference shader and FSR 1's portable headers, included where they
    # were vendored by a path relative to the file that includes them --
    # shadercross takes one include directory -- rather than copied beside the
    # engine's own, since they are upstream's bytes (R13). Depended on like the
    # headers above.
    set(vendor_headers "")
    foreach(vendored IN ITEMS "third_party/smaa" "third_party/fidelityfx_fsr1/ffx-fsr"
            "third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders")
        if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${vendored}")
            file(GLOB vendored_files CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/${vendored}/*.h"
                 "${CMAKE_SOURCE_DIR}/${vendored}/*.hlsl")
            list(APPEND vendor_headers ${vendored_files})
        endif()
    endforeach()

    # Resolved at configure time, because add_custom_command(OUTPUT) forbids
    # generator expressions -- $<TARGET_FILE_DIR:...> here is a configure error,
    # not a portability nicety.
    #
    # The obvious alternative, emitting under the binary dir and staging with a
    # POST_BUILD copy, is a trap: POST_BUILD only runs when the target relinks,
    # so editing only a shader would leave the staged copy stale and the engine
    # loading yesterday's blob. Emitting straight to the final location has no
    # such window.
    get_target_property(target_output_dir ${target} RUNTIME_OUTPUT_DIRECTORY)
    if(NOT target_output_dir)
        get_target_property(target_output_dir ${target} BINARY_DIR)
    endif()
    if(target_output_dir MATCHES "\\$<")
        message(FATAL_ERROR
            "engine_add_shaders(${target}): its output directory is a generator expression, "
            "which cannot be resolved here. Set a plain RUNTIME_OUTPUT_DIRECTORY on the target.")
    endif()

    set(out_dir "${target_output_dir}/content/shaders")
    set(outputs "")
    set(entries "")

    set(seen_names "")
    foreach(source IN LISTS sources)
        get_filename_component(name "${source}" NAME_WLE)

        # **A shader built on another one** -- `tonemap_graded.hlsl` includes
        # `tonemap.hlsl` whole (ADR 0096) -- depends on it as it does on the
        # shared headers: without this, editing the plain tonemap left the
        # graded one compiled from yesterday's text. Read at configure time,
        # so a NEW include line needs a reconfigure; an edit to the included
        # file does not.
        #
        # Followed through: `tonemap_graded_exact.hlsl` includes the graded
        # twin, which includes the plain one, and depends on both.
        set(siblings "")
        set(unread "${source}")
        while(unread)
            list(POP_FRONT unread reading)
            file(STRINGS "${reading}" sibling_lines REGEX "^#include \"\\.\\./src/[^\"]+\"")
            foreach(line IN LISTS sibling_lines)
                string(REGEX REPLACE "^#include \"\\.\\./src/([^\"]+)\".*" "\\1" sibling "${line}")
                set(sibling "${CMAKE_SOURCE_DIR}/shaders/src/${sibling}")
                if(NOT sibling IN_LIST siblings)
                    list(APPEND siblings "${sibling}")
                    list(APPEND unread "${sibling}")
                endif()
            endforeach()
        endwhile()

        # Output paths are flat per format, so two shaders sharing a stem in
        # different directories would silently overwrite each other's blobs.
        if(name IN_LIST seen_names)
            message(FATAL_ERROR
                "engine_add_shaders(${target}): two shaders are both named '${name}'. "
                "Shader names are the manifest's keys and must be unique across the glob.")
        endif()
        list(APPEND seen_names "${name}")

        foreach(stage IN LISTS stages)
            set(entrypoint "${entry_${stage}}")
            set(format_lines "")

            foreach(format IN LISTS formats)
                set(relative "${format}/${name}.${stage}.${ext_${format}}")
                set(output "${out_dir}/${relative}")
                add_custom_command(
                    OUTPUT "${output}"
                    COMMAND shadercross
                        "${source}"
                        -s HLSL
                        -d ${dest_${format}}
                        -t ${stage}
                        -e ${entrypoint}
                        ${include_args}
                        -o "${output}"
                    DEPENDS shadercross "${source}" ${headers} ${vendor_headers} ${siblings}
                    COMMENT "Shader ${name}.${stage} -> ${format}"
                    VERBATIM)
                list(APPEND outputs "${output}")
                list(APPEND format_lines "        \"${format}\": \"${relative}\"")
            endforeach()

            # SDL_GPU needs the resource counts at shader-creation time and the
            # input locations at pipeline-creation time; shadercross reflects
            # both out of the SPIR-V it just built. Emitting them here is what
            # keeps the runtime from hardcoding numbers that only the shader
            # source knows.
            set(reflect_relative "reflect/${name}.${stage}.json")
            set(reflect_output "${out_dir}/${reflect_relative}")
            add_custom_command(
                OUTPUT "${reflect_output}"
                COMMAND shadercross
                    "${source}"
                    -s HLSL
                    -d JSON
                    -t ${stage}
                    -e ${entrypoint}
                    ${include_args}
                    -o "${reflect_output}"
                DEPENDS shadercross "${source}" ${headers} ${vendor_headers} ${siblings}
                COMMENT "Shader ${name}.${stage} -> reflection"
                VERBATIM)
            list(APPEND outputs "${reflect_output}")

            file(RELATIVE_PATH source_relative "${CMAKE_SOURCE_DIR}" "${source}")
            string(JOIN ",\n" format_block ${format_lines})
            list(APPEND entries
"    {
      \"name\": \"${name}\",
      \"stage\": \"${stage}\",
      \"entrypoint\": \"${entrypoint}\",
      \"source\": \"${source_relative}\",
      \"reflect\": \"${reflect_relative}\",
      \"formats\": {
${format_block}
      }
    }")
        endforeach()
    endforeach()

    # --- Compute shaders (ADR 0116) --------------------------------------------
    # One stage, `ComputeMain`; the reflection carries the storage-buffer counts
    # and the thread-group size a compute pipeline is created with.
    set(compute_sources "")
    foreach(pattern IN LISTS arg_COMPUTE)
        file(GLOB matched CONFIGURE_DEPENDS "${pattern}")
        list(APPEND compute_sources ${matched})
    endforeach()
    list(SORT compute_sources)
    foreach(source IN LISTS compute_sources)
        get_filename_component(name "${source}" NAME_WLE)
        if(name IN_LIST seen_names)
            message(FATAL_ERROR "engine_add_shaders(${target}): two shaders are both named '${name}'.")
        endif()
        list(APPEND seen_names "${name}")
        set(format_lines "")
        foreach(format IN LISTS formats)
            set(relative "${format}/${name}.compute.${ext_${format}}")
            set(output "${out_dir}/${relative}")
            add_custom_command(
                OUTPUT "${output}"
                # **The target, as a define** (ADR 0164): `ENG_SHADER_SPIRV`,
                # `_DXIL` or `_MSL`, for the one thing a shader may not say the
                # same way everywhere -- an atomic on an image, which Metal's
                # language lacks. What a shader BINDS may not depend on it: the
                # reflection below is made once, with none of them defined.
                COMMAND shadercross "${source}" -s HLSL -d ${dest_${format}} -t compute -e ComputeMain
                        -DENG_SHADER_${dest_${format}} ${include_args} -o "${output}"
                DEPENDS shadercross "${source}" ${headers} ${vendor_headers}
                COMMENT "Shader ${name}.compute -> ${format}"
                VERBATIM)
            list(APPEND outputs "${output}")
            list(APPEND format_lines "        \"${format}\": \"${relative}\"")
        endforeach()
        set(reflect_relative "reflect/${name}.compute.json")
        set(reflect_output "${out_dir}/${reflect_relative}")
        add_custom_command(
            OUTPUT "${reflect_output}"
            COMMAND shadercross "${source}" -s HLSL -d JSON -t compute -e ComputeMain ${include_args}
                    -o "${reflect_output}"
            DEPENDS shadercross "${source}" ${headers} ${vendor_headers}
            COMMENT "Shader ${name}.compute -> reflection"
            VERBATIM)
        list(APPEND outputs "${reflect_output}")
        file(RELATIVE_PATH source_relative "${CMAKE_SOURCE_DIR}" "${source}")
        string(JOIN ",\n" format_block ${format_lines})
        list(APPEND entries
"    {
      \"name\": \"${name}\",
      \"stage\": \"compute\",
      \"entrypoint\": \"ComputeMain\",
      \"source\": \"${source_relative}\",
      \"reflect\": \"${reflect_relative}\",
      \"formats\": {
${format_block}
      }
    }")
    endforeach()

    # --- The engine's headers, beside the blobs (ADR 0091) -------------------
    # What the editor's surface compiler includes when it compiles a user's
    # shader at run time: `engine/surface.hlsli` and everything it builds on.
    foreach(header IN LISTS headers)
        file(RELATIVE_PATH header_relative "${include_dir}" "${header}")
        set(header_copy "${out_dir}/include/${header_relative}")
        add_custom_command(
            OUTPUT "${header_copy}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${header}" "${header_copy}"
            DEPENDS "${header}"
            COMMENT "Shader header ${header_relative} -> content"
            VERBATIM)
        list(APPEND outputs "${header_copy}")
    endforeach()

    # --- Surface shaders the engine ships (ADR 0091) -------------------------
    set(surfaces "")
    foreach(pattern IN LISTS arg_SURFACES)
        file(GLOB matched CONFIGURE_DEPENDS "${pattern}")
        list(APPEND surfaces ${matched})
    endforeach()
    list(SORT surfaces)
    set(variants "forward" "forward_instanced" "forward_blended" "depth" "depth_instanced")
    foreach(surface IN LISTS surfaces)
        get_filename_component(file_name "${surface}" NAME)
        string(REGEX REPLACE "\\.surface\\.hlsl$" "" surface_name "${file_name}")
        set(wrap_dir "${ENG_GENERATED_DIR}/surfaces/${surface_name}")
        set(wrapped "")
        foreach(variant IN LISTS variants)
            foreach(stage IN LISTS stages)
                list(APPEND wrapped "${wrap_dir}/${variant}.${stage}.hlsl")
            endforeach()
        endforeach()
        add_custom_command(
            OUTPUT ${wrapped}
            COMMAND surfacewrap "${surface}" "${wrap_dir}"
            DEPENDS surfacewrap "${surface}"
            COMMENT "Surface ${surface_name} -> wrappers"
            VERBATIM)
        # The source beside the blobs: the renderer reads its parameters from
        # it, so the layout it packs is the one the blobs were compiled with.
        set(surface_copy "${out_dir}/surfaces/${surface_name}.surface.hlsl")
        add_custom_command(
            OUTPUT "${surface_copy}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${surface}" "${surface_copy}"
            DEPENDS "${surface}"
            COMMENT "Surface ${surface_name} -> content"
            VERBATIM)
        list(APPEND outputs "${surface_copy}")

        foreach(variant IN LISTS variants)
            set(name "surface_${surface_name}_${variant}")
            if(name IN_LIST seen_names)
                message(FATAL_ERROR "engine_add_shaders(${target}): two shaders are both named '${name}'.")
            endif()
            list(APPEND seen_names "${name}")
            foreach(stage IN LISTS stages)
                set(source "${wrap_dir}/${variant}.${stage}.hlsl")
                set(entrypoint "${entry_${stage}}")
                set(format_lines "")
                foreach(format IN LISTS formats)
                    set(relative "${format}/${name}.${stage}.${ext_${format}}")
                    set(output "${out_dir}/${relative}")
                    add_custom_command(
                        OUTPUT "${output}"
                        COMMAND shadercross "${source}" -s HLSL -d ${dest_${format}} -t ${stage} -e ${entrypoint}
                                ${include_args} -o "${output}"
                        DEPENDS shadercross "${source}" "${surface}" ${headers}
                        COMMENT "Shader ${name}.${stage} -> ${format}"
                        VERBATIM)
                    list(APPEND outputs "${output}")
                    list(APPEND format_lines "        \"${format}\": \"${relative}\"")
                endforeach()
                set(reflect_relative "reflect/${name}.${stage}.json")
                set(reflect_output "${out_dir}/${reflect_relative}")
                add_custom_command(
                    OUTPUT "${reflect_output}"
                    COMMAND shadercross "${source}" -s HLSL -d JSON -t ${stage} -e ${entrypoint}
                            ${include_args} -o "${reflect_output}"
                    DEPENDS shadercross "${source}" "${surface}" ${headers}
                    COMMENT "Shader ${name}.${stage} -> reflection"
                    VERBATIM)
                list(APPEND outputs "${reflect_output}")
                file(RELATIVE_PATH source_relative "${CMAKE_SOURCE_DIR}" "${surface}")
                string(JOIN ",\n" format_block ${format_lines})
                list(APPEND entries
"    {
      \"name\": \"${name}\",
      \"stage\": \"${stage}\",
      \"entrypoint\": \"${entrypoint}\",
      \"source\": \"${source_relative}\",
      \"reflect\": \"${reflect_relative}\",
      \"formats\": {
${format_block}
      }
    }")
            endforeach()
        endforeach()
    endforeach()

    string(JOIN ",\n" entries ${entries})

    # The manifest's content is fully known at configure time, but it is written
    # into the generated tree and copied into content/ by a real build rule
    # rather than straight into place: a file with no rule that produces it is a
    # file Ninja refuses to rebuild, so deleting `content/` -- the most obvious
    # way to force a clean shader build -- would wedge the build until the next
    # configure.
    set(manifest_source "${ENG_GENERATED_DIR}/shaders/${target}-manifest.json")
    file(GENERATE OUTPUT "${manifest_source}" CONTENT
"{
  \"version\": 1,
  \"comment\": \"GENERATED by cmake/engine_shaders.cmake. Paths are relative to this file.\",
  \"shaders\": [
${entries}
  ]
}
")

    set(manifest "${out_dir}/manifest.json")
    add_custom_command(
        OUTPUT "${manifest}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${manifest_source}" "${manifest}"
        DEPENDS "${manifest_source}"
        COMMENT "Shader manifest for ${target}"
        VERBATIM)

    add_custom_target(${target}_shaders DEPENDS ${outputs} "${manifest}")
    add_dependencies(${target} ${target}_shaders)
endfunction()
