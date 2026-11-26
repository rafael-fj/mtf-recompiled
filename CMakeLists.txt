cmake_minimum_required(VERSION 3.25)

if(WIN32)
    project(mtf-recomp VERSION 0.1.0 LANGUAGES C CXX RC)
else()
    project(mtf-recomp VERSION 0.1.0 LANGUAGES C CXX)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# The lifted game code and its glue are built from the player's own files and never committed (build/ is ignored).
set(MTF_GENERATED_DIRS "" CACHE STRING "Semicolon-separated generator output directories (build/generated/<module>-full)")
set(MTF_GLUE_DIR "${PROJECT_SOURCE_DIR}/build/generated/glue" CACHE PATH "Directory written by recomp/glue.py")
set(MTF_CPU_LEVEL "v3" CACHE STRING "x86-64 level: v3 (AVX2/FMA, default) or v2 (SSE4.2)")
set(MTF_GEN_OPT "-O2" CACHE STRING "Optimisation level of the lifted game code")
set(MTF_OUTPUT_DIR "${PROJECT_SOURCE_DIR}/build/bin" CACHE PATH "Where mtf-recomp.exe and its DLLs go (the installer uses its own)")

if(NOT MTF_GENERATED_DIRS)
    message(FATAL_ERROR "Set MTF_GENERATED_DIRS to the lifted module directories (tools/build_windows.ps1 does)")
endif()
if(NOT EXISTS "${MTF_GLUE_DIR}/modules.c" OR NOT EXISTS "${MTF_GLUE_DIR}/layout.h")
    message(FATAL_ERROR "Run recomp/glue.py before building: ${MTF_GLUE_DIR}/modules.c is missing")
endif()

if(MTF_CPU_LEVEL STREQUAL "v2")
    set(MTF_MARCH -march=x86-64-v2)
    set(MTF_CPU_DEFINE RT_CPU_LEVEL=2)
else()
    set(MTF_MARCH -march=x86-64-v3)
    set(MTF_CPU_DEFINE RT_CPU_LEVEL=3)
endif()

# Third-party headers, import libraries and DLLs (tools/make_deps.py, from MSYS2 UCRT64): SDL3, Vulkan, VMA, Opus, libvpx, zstd.
set(MTF_DEPS_DIR "${PROJECT_SOURCE_DIR}/build/toolchain/deps" CACHE PATH "Directory written by tools/make_deps.py")

# The exe names no folder of the machine that compiled it: __FILE__ and the debug info are written relative.
foreach(MTF_MAPPED_DIR IN LISTS PROJECT_SOURCE_DIR PROJECT_BINARY_DIR MTF_GLUE_DIR MTF_DEPS_DIR MTF_GENERATED_DIRS)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-ffile-prefix-map=${MTF_MAPPED_DIR}=.>")
endforeach()
if(NOT EXISTS "${MTF_DEPS_DIR}/include/SDL3/SDL.h")
    message(FATAL_ERROR "Run tools/make_deps.py first: ${MTF_DEPS_DIR} has no SDL3 headers")
endif()

# Lifted modules, about 110,000 functions. -O2 (MTF_GEN_OPT) takes the game's own threads from 0.72 to 0.61 busy cores
# in a match against -O1, for 20 more seconds of build. -fwrapv: ARM arithmetic wraps, and the lifted C must not let
# the optimiser assume signed overflow away; -fno-strict-aliasing: guest memory is read through any type. Labels and
# missing returns are artefacts of the lifting.
set(MTF_GENERATED_GLOBS)
foreach(MTF_GENERATED_DIR IN LISTS MTF_GENERATED_DIRS)
    list(APPEND MTF_GENERATED_GLOBS "${MTF_GENERATED_DIR}/*.c")
endforeach()
file(GLOB_RECURSE MTF_GENERATED_SOURCES CONFIGURE_DEPENDS ${MTF_GENERATED_GLOBS})
if(NOT MTF_GENERATED_SOURCES)
    message(FATAL_ERROR "MTF_GENERATED_DIRS contains no generated C files: ${MTF_GENERATED_DIRS}")
endif()
add_library(mtf-generated-code OBJECT ${MTF_GENERATED_SOURCES})
target_include_directories(mtf-generated-code PRIVATE "${PROJECT_SOURCE_DIR}/runtime")
target_compile_options(mtf-generated-code PRIVATE
    ${MTF_GEN_OPT} -fwrapv -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden ${MTF_MARCH})

# Host runtime: Horizon kernel and services, NVN on Vulkan, audio, input, the settings menu.
set(MTF_RUNTIME_SOURCES
    runtime/achievements.c
    runtime/amiibo.c
    runtime/audio_effects.c
    runtime/audio_mixer.c
    runtime/audio_reverb.c
    runtime/audren.c
    runtime/boot.c
    runtime/cfg.c
    runtime/diag_watch.c
    runtime/dispatch.c
    runtime/fs_host.c
    runtime/game_host.c
    runtime/gfx_null.c
    runtime/gfx_present.c
    runtime/gfx_shader.c
    runtime/gfx_stubs.c
    runtime/glyph_draw.c
    runtime/hostfn.c
    runtime/input.c
    runtime/input_map.c
    runtime/ipc.c
    runtime/kernel.c
    runtime/ldn.c
    runtime/loader.c
    runtime/main.c
    runtime/memory.c
    runtime/motion_fusion.c
    runtime/nfp.c
    runtime/nvn2_ext.c
    runtime/nvn_api_cmd.c
    runtime/nvn_api_core.c
    runtime/nvn_api_obj.c
    runtime/nvn_ext.c
    runtime/nvn_procs.c
    runtime/nvn_tex.c
    runtime/ov_audio.c
    runtime/ov_resolution.c
    runtime/ov_audioout.c
    runtime/ov_codec.c
    runtime/ov_font.c
    runtime/ov_gfx.c
    runtime/ov_hid.c
    runtime/ov_net.c
    runtime/ov_offline.c
    runtime/ov_switch2.c
    runtime/ov_system.c
    runtime/ro.c
    runtime/romfs.c
    runtime/sha256.c
    runtime/cheats.c
    runtime/stubs.c
    runtime/svc.c
    runtime/tex_astc.c
    runtime/tex_astc_mt.c
    runtime/tex_deswizzle.c
    runtime/vk_astc.c
    runtime/vk_backend.c
    runtime/vk_caps.c
    runtime/vk_color.c
    runtime/vk_device.c
    runtime/vk_dlaa.c
    runtime/vk_framegen.c
    runtime/vk_fsr.c
    runtime/vk_smaa.c
    runtime/vk_hdr.c
    runtime/vk_headroom.c
    runtime/vk_dump.c
    runtime/vk_exec.c
    runtime/vk_exec_draw.c
    runtime/vk_exec_xfer.c
    runtime/vk_formats.c
    runtime/vk_memory.c
    runtime/vk_pipeline.c
    runtime/spirv_nan_guard.c
    runtime/vk_rec.c
    runtime/vk_sync.c
    runtime/vk_texture.c
    runtime/vk_usnap.c
    runtime/vk_vma.cpp
    runtime/perfmon.c
    runtime/platform/common.c
    # the in-game settings menu (Minus+Plus, F1, Esc) on Dear ImGui
    runtime/ui/menu.cpp
    runtime/ui/binds_page.cpp
    runtime/ui/option_rows.cpp
    runtime/ui_text.c
    runtime/update_check.c
    runtime/third_party/imgui/imgui.cpp
    runtime/third_party/imgui/imgui_draw.cpp
    runtime/third_party/imgui/imgui_tables.cpp
    runtime/third_party/imgui/imgui_widgets.cpp
    runtime/third_party/imgui/backends/imgui_impl_vulkan.cpp
)
if(WIN32)
    list(APPEND MTF_RUNTIME_SOURCES
        runtime/gfx_window_win.c
        runtime/platform/win32.c
        runtime/ui/menu_win.cpp
        runtime/stream_host.c
        runtime/stream_webrtc.cpp
        runtime/stream_hwenc.cpp
        runtime/mtf.rc
    )
else()
    list(APPEND MTF_RUNTIME_SOURCES
        runtime/gfx_window_sdl.c
        runtime/platform/linux.c
        runtime/ui/menu_sdl.cpp
        runtime/stream_linux_stub.c
    )
endif()

if(WIN32)
    add_executable(mtf-recomp ${MTF_RUNTIME_SOURCES})
else()
    add_executable(mtf-recomp
        ${MTF_RUNTIME_SOURCES}
        "${MTF_GLUE_DIR}/modules.c"
        $<TARGET_OBJECTS:mtf-generated-code>
    )
endif()

if(EXISTS "${PROJECT_SOURCE_DIR}/VERSION")
    file(READ "${PROJECT_SOURCE_DIR}/VERSION" MTF_PORT_VERSION_RAW)
    string(STRIP "${MTF_PORT_VERSION_RAW}" MTF_PORT_VERSION_STR)
else()
    set(MTF_PORT_VERSION_STR "0.1.0")
endif()
target_include_directories(mtf-recomp PRIVATE
    "${PROJECT_SOURCE_DIR}/runtime"
    "${PROJECT_SOURCE_DIR}/runtime/third_party/imgui"
    "${MTF_GLUE_DIR}"
)
target_include_directories(mtf-recomp SYSTEM PRIVATE "${MTF_DEPS_DIR}/include" "${MTF_DEPS_DIR}/include/vma")
target_link_directories(mtf-recomp PRIVATE "${MTF_DEPS_DIR}/lib")
target_compile_options(mtf-recomp PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:-O2 -g -Wall -fno-strict-aliasing ${MTF_MARCH}>)
set_source_files_properties(runtime/vk_vma.cpp PROPERTIES COMPILE_OPTIONS "-std=c++17;-Wno-nullability-completeness")
set_source_files_properties(runtime/ui/menu.cpp runtime/ui/option_rows.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
if(WIN32)
    set_source_files_properties(runtime/ui/menu_win.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
else()
    set_source_files_properties(runtime/ui/menu.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20;-Wa,-I${PROJECT_SOURCE_DIR}/runtime/ui")
    set_source_files_properties(runtime/ui/menu_sdl.cpp PROPERTIES COMPILE_OPTIONS "-std=c++20")
endif()
set_source_files_properties(runtime/third_party/imgui/imgui.cpp runtime/third_party/imgui/imgui_draw.cpp
    runtime/third_party/imgui/imgui_tables.cpp runtime/third_party/imgui/imgui_widgets.cpp
    runtime/third_party/imgui/backends/imgui_impl_vulkan.cpp PROPERTIES COMPILE_OPTIONS "-std=c++17;-w")

if(WIN32)
    target_compile_definitions(mtf-recomp PRIVATE ${MTF_CPU_DEFINE} MTF_RUNTIME_SPLIT MTF_PORT_VERSION="${MTF_PORT_VERSION_STR}" _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    # A 16 MiB main-thread stack: the guest's boot runs on it before the first guest thread exists.
    target_link_options(mtf-recomp PRIVATE -fuse-ld=lld -Wl,--stack,16777216 -Wl,--error-limit=0 -Wl,--export-all-symbols,--out-implib,${CMAKE_CURRENT_BINARY_DIR}/libmtf-recomp.dll.a)
    target_link_libraries(mtf-recomp PRIVATE vulkan-1 SDL3 opus vpx datachannel juice srtp2 usrsctp mbedtls mbedx509 mbedcrypto everest p256m iphlpapi psapi bcrypt user32 gdi32 dwmapi imm32 winmm ole32 oleaut32 mfplat mfuuid shell32 uuid pdh rstrtmgr ws2_32 winhttp)

    add_library(mtf-game SHARED
        "${MTF_GLUE_DIR}/modules.c"
        $<TARGET_OBJECTS:mtf-generated-code>
    )
    add_dependencies(mtf-game mtf-recomp)
    target_include_directories(mtf-game PRIVATE "${PROJECT_SOURCE_DIR}/runtime" "${MTF_GLUE_DIR}")
    target_compile_definitions(mtf-game PRIVATE MTF_GAME_DLL)
    target_compile_options(mtf-game PRIVATE -O2 -g -Wall -fno-strict-aliasing ${MTF_MARCH})
    target_link_options(mtf-game PRIVATE -fuse-ld=lld -Wl,--error-limit=0)
    target_link_libraries(mtf-game PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/libmtf-recomp.dll.a")
    set_target_properties(mtf-game PROPERTIES
        PREFIX ""
        OUTPUT_NAME "mtf-game"
        RUNTIME_OUTPUT_DIRECTORY "${MTF_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MTF_OUTPUT_DIR}"
    )

    # The DLLs the executable loads, beside it (Vulkan's loader comes with the GPU driver), and llvm-mingw's C++ runtime
    # for the C++ files (the menu, ImGui, Vulkan Memory Allocator).
    file(GLOB MTF_DEPS_DLLS "${MTF_DEPS_DIR}/bin/*.dll")
    get_filename_component(MTF_CXX_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    list(APPEND MTF_DEPS_DLLS "${MTF_CXX_BIN}/../x86_64-w64-mingw32/bin/libc++.dll" "${MTF_CXX_BIN}/../x86_64-w64-mingw32/bin/libunwind.dll" "${MTF_CXX_BIN}/../x86_64-w64-mingw32/bin/libwinpthread-1.dll")
    foreach(MTF_DLL IN LISTS MTF_DEPS_DLLS)
        add_custom_command(TARGET mtf-recomp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MTF_DLL}" "${MTF_OUTPUT_DIR}/")
    endforeach()

    add_executable(mtf-updater WIN32 tools/mtf_updater.c)
    target_compile_options(mtf-updater PRIVATE -O2 -Wall)
    target_link_libraries(mtf-updater PRIVATE comctl32)
    target_link_options(mtf-updater PRIVATE -fuse-ld=lld)
    set_target_properties(mtf-updater PROPERTIES
        OUTPUT_NAME "mtf-updater"
        RUNTIME_OUTPUT_DIRECTORY "${MTF_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MTF_OUTPUT_DIR}"
    )
else()
    target_compile_definitions(mtf-recomp PRIVATE ${MTF_CPU_DEFINE} MTF_PORT_VERSION="${MTF_PORT_VERSION_STR}" _GNU_SOURCE)
    target_link_options(mtf-recomp PRIVATE -fuse-ld=lld -Wl,-rpath,$ORIGIN)
    target_link_libraries(mtf-recomp PRIVATE vulkan SDL3 opus vpx curl pthread dl m)
    file(GLOB MTF_DEPS_SOS "${MTF_DEPS_DIR}/lib/libSDL3.so*")
    foreach(MTF_SO IN LISTS MTF_DEPS_SOS)
        add_custom_command(TARGET mtf-recomp POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MTF_SO}" "${MTF_OUTPUT_DIR}/")
    endforeach()
endif()

set_target_properties(mtf-recomp PROPERTIES
    OUTPUT_NAME "mtf-recomp"
    RUNTIME_OUTPUT_DIRECTORY "${MTF_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MTF_OUTPUT_DIR}"
)
