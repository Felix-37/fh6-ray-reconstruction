cmake_minimum_required(VERSION 3.20)
project(rr_forza VERSION 0.1.0 LANGUAGES C CXX)
set(RR_FORZA_VERSION "${PROJECT_VERSION}-beta")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
add_compile_options(-fms-extensions -Wall -Wno-unknown-pragmas -Wno-unused-parameter
                    -Wno-missing-field-initializers -Wno-microsoft-cast)
add_compile_options("$<$<CONFIG:Release>:-O2;-ffunction-sections;-fdata-sections>")
add_link_options("$<$<CONFIG:Release>:-Wl,--gc-sections;-s>")
add_link_options(-static)

set(TP ${CMAKE_SOURCE_DIR}/third_party)
set(GEN ${CMAKE_BINARY_DIR}/generated)

# DirectX Shader Compiler: not in the repository, fetched by tools/fetch-dxc.ps1 into third_party/dxc.
set(DXC ${TP}/dxc/bin/x64/dxc.exe)
if(NOT EXISTS ${DXC})
    message(FATAL_ERROR "DXC not found at ${DXC}.\n"
                        "Run:  powershell -ExecutionPolicy Bypass -File tools/fetch-dxc.ps1\n"
                        "(downloads the DirectX Shader Compiler release from github.com/microsoft/DirectXShaderCompiler).")
endif()

add_library(minhook STATIC
    ${TP}/minhook/src/buffer.c
    ${TP}/minhook/src/hook.c
    ${TP}/minhook/src/trampoline.c
    ${TP}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${TP}/minhook/include)
target_compile_options(minhook PRIVATE -w)

# Guide shader, compiled offline to DXIL and embedded as a byte array.
add_custom_command(
    OUTPUT ${GEN}/guides_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${DXC} -nologo -T cs_6_0 -E main -O3 -Fh ${GEN}/guides_cs.h -Vn g_guides_cs ${CMAKE_SOURCE_DIR}/shaders/guides.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/guides.hlsl
    COMMENT "dxc guides.hlsl")

# ---- Game shaders (NOT in the repository) ----
# The RTGI filter variants are the game's own DXIL with a few instructions changed by scripts/patch_rtgi*.pl,
# reassembled and signed by tools/dxil_asm.cpp with the root signature of the original container. The two
# original shaders come from each contributor's own copy of the game: in game, ReShade overlay >
# "Ray Reconstruction" > Developer > "Export game shaders", then copy both files into game-shaders/
# (game-shaders/README.md). Without them the add-on is built without those variants (GI mode "Game original" only).
set(GAME_SHADERS ${CMAKE_SOURCE_DIR}/game-shaders)
set(RTGI_TEMPORAL_CSO ${GAME_SHADERS}/shader_0x4DAF8A48.cs.cso)   # RTGI_TemporalFilter
set(RTGI_SPATIAL_CSO ${GAME_SHADERS}/shader_0x209AB6A4.cs.cso)    # RTGI_SpatialFilter_Disk
# SHA-256 of the shaders the patches were written against (Forza Horizon 6 6.440.853.0).
set(RTGI_TEMPORAL_SHA256 24d16b420d900d0eb3d2f713e3c674de998bd4963321b032dece96fc66984f30)
set(RTGI_SPATIAL_SHA256 bbcf3ee1f9cd175d15782bd7e7e3b25b58ea5228614997a44f4e7f4655a7685c)
set(RTGI_TEMPORAL_VARIANTS hist2 hist4 nofilter cap2 cap4 nofilter_iso4 nofilter_iso2 nofilter_iso1
                           orig_reset nofilter_reset nofilter_iso4_reset nofilter_iso2_reset nofilter_iso1_reset hist4_reset)
set(RTGI_SPATIAL_VARIANTS assist4 assist8)
set(RTGI_HEADERS)
# Re-run CMake when files are added to or removed from game-shaders/ (the folder's timestamp changes), so
# copying the exported shaders there and running build.sh again is enough.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GAME_SHADERS})
if(EXISTS ${RTGI_TEMPORAL_CSO} AND EXISTS ${RTGI_SPATIAL_CSO})
    file(SHA256 ${RTGI_TEMPORAL_CSO} temporal_hash)
    file(SHA256 ${RTGI_SPATIAL_CSO} spatial_hash)
    if(NOT temporal_hash STREQUAL RTGI_TEMPORAL_SHA256 OR NOT spatial_hash STREQUAL RTGI_SPATIAL_SHA256)
        message(WARNING "The shaders in game-shaders/ differ from the ones the patches were written for (FH6 6.440.853.0). "
                        "If the game was updated, the patch anchors may no longer match and the build stops with "
                        "'anchor found N times'. See CONTRIBUTING.md, 'When the game updates'.")
    endif()

    add_executable(dxil_asm tools/dxil_asm.cpp)
    target_include_directories(dxil_asm PRIVATE ${TP}/dxc/inc)
    target_compile_options(dxil_asm PRIVATE -Wno-unknown-attributes)
    find_program(PERL perl REQUIRED)
    file(TO_NATIVE_PATH ${TP}/dxc/bin/x64 DXC_BIN_NATIVE)

    # Disassembly of the originals (the patch scripts anchor on its exact instructions).
    foreach(target temporal spatial)
        if(target STREQUAL "temporal")
            set(cso ${RTGI_TEMPORAL_CSO})
        else()
            set(cso ${RTGI_SPATIAL_CSO})
        endif()
        add_custom_command(
            OUTPUT ${GEN}/rtgi_${target}.orig.ll
            COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
            COMMAND ${DXC} -dumpbin ${cso} -Fc ${GEN}/rtgi_${target}.orig.ll
            DEPENDS ${cso}
            COMMENT "dxc -dumpbin ${cso}")
    endforeach()

    foreach(variant ${RTGI_TEMPORAL_VARIANTS})
        add_custom_command(
            OUTPUT ${GEN}/rtgi_temporal_${variant}.h
            COMMAND ${PERL} ${CMAKE_SOURCE_DIR}/scripts/patch_rtgi.pl ${variant} ${GEN}/rtgi_temporal.orig.ll ${GEN}/rtgi_temporal_${variant}.ll
            COMMAND $<TARGET_FILE:dxil_asm> ${DXC_BIN_NATIVE} ${GEN}/rtgi_temporal_${variant}.ll ${GEN}/rtgi_temporal_${variant}.cso
                    --parts-from ${RTGI_TEMPORAL_CSO} --header ${GEN}/rtgi_temporal_${variant}.h g_rtgi_temporal_${variant}
            DEPENDS dxil_asm ${CMAKE_SOURCE_DIR}/scripts/patch_rtgi.pl ${GEN}/rtgi_temporal.orig.ll ${RTGI_TEMPORAL_CSO}
            COMMENT "RTGI temporal filter variant ${variant}")
        list(APPEND RTGI_HEADERS ${GEN}/rtgi_temporal_${variant}.h)
    endforeach()
    # Spatial filter variants patched the same way (scripts/patch_rtgi_spatial.pl).
    foreach(variant ${RTGI_SPATIAL_VARIANTS})
        add_custom_command(
            OUTPUT ${GEN}/rtgi_spatial_${variant}.h
            COMMAND ${PERL} ${CMAKE_SOURCE_DIR}/scripts/patch_rtgi_spatial.pl ${variant} ${GEN}/rtgi_spatial.orig.ll ${GEN}/rtgi_spatial_${variant}.ll
            COMMAND $<TARGET_FILE:dxil_asm> ${DXC_BIN_NATIVE} ${GEN}/rtgi_spatial_${variant}.ll ${GEN}/rtgi_spatial_${variant}.cso
                    --parts-from ${RTGI_SPATIAL_CSO} --header ${GEN}/rtgi_spatial_${variant}.h g_rtgi_spatial_${variant}
            DEPENDS dxil_asm ${CMAKE_SOURCE_DIR}/scripts/patch_rtgi_spatial.pl ${GEN}/rtgi_spatial.orig.ll ${RTGI_SPATIAL_CSO}
            COMMENT "RTGI spatial filter variant ${variant}")
        list(APPEND RTGI_HEADERS ${GEN}/rtgi_spatial_${variant}.h)
    endforeach()
    set(VARIANT_INCLUDE ${GEN})
else()
    message(WARNING "game-shaders/ does not hold both RTGI shaders: building WITHOUT the RTGI filter variants "
                    "(only the GI mode \"Game original\" will work). See game-shaders/README.md.")
    # One-byte stubs (replace.cpp skips variants smaller than a DXIL container).
    set(VARIANT_INCLUDE ${CMAKE_BINARY_DIR}/stubs)
    foreach(variant ${RTGI_TEMPORAL_VARIANTS})
        file(WRITE ${VARIANT_INCLUDE}/rtgi_temporal_${variant}.h
             "// Stub: built without game shaders (game-shaders/README.md).\n#pragma once\nconst unsigned char g_rtgi_temporal_${variant}[] = { 0 };\n")
    endforeach()
    foreach(variant ${RTGI_SPATIAL_VARIANTS})
        file(WRITE ${VARIANT_INCLUDE}/rtgi_spatial_${variant}.h
             "// Stub: built without game shaders (game-shaders/README.md).\n#pragma once\nconst unsigned char g_rtgi_spatial_${variant}[] = { 0 };\n")
    endforeach()
endif()

add_custom_command(
    OUTPUT ${GEN}/bypass_rtgi_spatial_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${DXC} -nologo -T cs_6_6 -E main -O3 -Fh ${GEN}/bypass_rtgi_spatial_cs.h -Vn g_bypass_rtgi_spatial ${CMAKE_SOURCE_DIR}/shaders/bypass_rtgi_spatial.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/bypass_rtgi_spatial.hlsl
    COMMENT "dxc bypass_rtgi_spatial.hlsl")
# Bench readback helper (reads the DLSS output and depth without changing their layouts).
add_custom_command(
    OUTPUT ${GEN}/bench_copy_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${DXC} -nologo -T cs_6_0 -E main -O3 -Fh ${GEN}/bench_copy_cs.h -Vn g_bench_copy_cs ${CMAKE_SOURCE_DIR}/shaders/bench_copy.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/bench_copy.hlsl
    COMMENT "dxc bench_copy.hlsl")
list(APPEND RTGI_HEADERS ${GEN}/bench_copy_cs.h)

# Optional adaptive sharpening of the RR output.
add_custom_command(
    OUTPUT ${GEN}/rr_sharpen_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${DXC} -nologo -T cs_6_0 -E main -O3 -Fh ${GEN}/rr_sharpen_cs.h -Vn g_rr_sharpen_cs ${CMAKE_SOURCE_DIR}/shaders/rr_sharpen.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/rr_sharpen.hlsl
    COMMENT "dxc rr_sharpen.hlsl")
list(APPEND RTGI_HEADERS ${GEN}/rr_sharpen_cs.h)

# Specular motion vectors for RR (method adapted from speedlemur's Control Ray Reconstruction, MIT).
add_custom_command(
    OUTPUT ${GEN}/specmv_cs.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
    COMMAND ${DXC} -nologo -T cs_6_0 -E main -O3 -Fh ${GEN}/specmv_cs.h -Vn g_specmv_cs ${CMAKE_SOURCE_DIR}/shaders/specmv.hlsl
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/specmv.hlsl
    COMMENT "dxc specmv.hlsl")
list(APPEND RTGI_HEADERS ${GEN}/specmv_cs.h)

# Firefly-suppressing spatial variants for the "Full RR" GI mode (K = clamp width in sigmas).
foreach(level soft medium strong range)
    set(EXTRA "")
    # soft / medium / strong: firefly suppression v2 (luminance SH over 5x5, K x neighbour mean)
    if(level STREQUAL "soft")
        set(K 4.0)
        set(EXTRA -D LUM5=1)
    elseif(level STREQUAL "medium")
        set(K 2.5)
        set(EXTRA -D LUM5=1)
    elseif(level STREQUAL "strong")
        set(K 1.6)
        set(EXTRA -D LUM5=1)
    else()
        set(K 0.0)
        set(EXTRA -D RANGE=1)
    endif()
    add_custom_command(
        OUTPUT ${GEN}/rtgi_spatial_clamp_${level}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
        COMMAND ${DXC} -nologo -T cs_6_6 -E main -O3 -D K=${K} ${EXTRA} -Fh ${GEN}/rtgi_spatial_clamp_${level}.h -Vn g_rtgi_spatial_clamp_${level} ${CMAKE_SOURCE_DIR}/shaders/rtgi_spatial_declamp.hlsl
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/rtgi_spatial_declamp.hlsl
        COMMENT "dxc rtgi_spatial_declamp.hlsl K=${K}")
    list(APPEND RTGI_HEADERS ${GEN}/rtgi_spatial_clamp_${level}.h)
endforeach()

# Version resource: ReShade's add-on list and the file properties show the version.
include(CheckLanguage)
check_language(RC)
set(VERSION_RC)
if(CMAKE_RC_COMPILER)
    enable_language(RC)
    configure_file(src/version.rc.in ${GEN}/version.rc @ONLY)
    set(VERSION_RC ${GEN}/version.rc)
endif()

# rr-forza: DLSS Ray Reconstruction add-on (guides, RTGI filter replacement, bench, frame capture).
add_library(rr_mapper SHARED
    ${GEN}/guides_cs.h
    ${RTGI_HEADERS}
    ${GEN}/bypass_rtgi_spatial_cs.h
    ${VERSION_RC}
    src/replace.cpp
    src/guides.cpp
    src/slinit_hook.cpp
    src/shader_dump.cpp
    src/mapper_main.cpp
    src/capture.cpp
    src/image.cpp
    src/sl_hooks.cpp
    src/marker_hooks.cpp
    src/settings.cpp
    src/overlay.cpp
    src/bench.cpp
    src/metrics.cpp
    src/sharpen.cpp
    src/specmv.cpp)
target_compile_definitions(rr_mapper PRIVATE "RR_FORZA_VERSION=\"${RR_FORZA_VERSION}\"")

# Offline re-analysis of a bench folder: rr_metrics [--en|--es] <rr-forza-captures\bench-...>
add_executable(rr_metrics tools/rr_metrics.cpp src/metrics.cpp)
target_include_directories(rr_metrics PRIVATE src)
target_include_directories(rr_mapper PRIVATE
    ${TP}/reshade/include
    ${TP}/imgui
    ${TP}/streamline/include
    ${VARIANT_INCLUDE}
    ${GEN}
    src)
target_link_libraries(rr_mapper PRIVATE minhook ole32 windowscodecs uuid)
set_target_properties(rr_mapper PROPERTIES PREFIX "" OUTPUT_NAME "rr-forza" SUFFIX ".addon64")
