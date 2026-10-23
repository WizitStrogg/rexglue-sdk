set(REXSYSTEM_SOURCES
    runtime.cpp
    embedded_metadata.cpp
    achievement_manager.cpp
    achievements.cpp
    kernel_state.cpp
    xmemory.cpp
    kernel_module.cpp
    user_module.cpp
    xthread.cpp
    xevent.cpp
    xfile.cpp
    xmutant.cpp
    xsemaphore.cpp
    xtimer.cpp
    xiocompletion.cpp
    xnotifylistener.cpp
    xenumerator.cpp
    xsocket.cpp
    xsymboliclink.cpp
    xobject.cpp
    xmodule.cpp
    guest_path.cpp

    # Utilities
    util/object_table.cpp
    util/native_list.cpp
    util/xdbf_utils.cpp
    util/gameinfo_utils.cpp

    # XAM services
    xam/app_manager.cpp
    xam/content_manager.cpp
    xam/user_profile.cpp

    ppc_types.cpp
    elf_module.cpp
    entry_table.cpp
    export_resolver.cpp
    lzx.cpp
    mmio_handler.cpp
    map_parser.cpp
    module.cpp
    function_dispatcher.cpp
    gpu_plugin_loader.cpp
    thread.cpp
    thread_state.cpp
    xex_module.cpp
)

add_library(rexruntime SHARED ${REXSYSTEM_SOURCES})
add_library(rex::runtime ALIAS rexruntime)
add_library(rex::system ALIAS rexruntime)
add_library(rex::kernel ALIAS rexruntime)

# Curated export table: export exactly what the title (fable_2.exe) and the GPU
# plugin import, rather than export-all. This keeps the PE export section small
# and (importantly) guarantees the Xbox-360 kernel-stub __imp__* symbols are
# exported even when the dual-backend (D3D12+Vulkan) build's full symbol set
# would otherwise push them out of the linker's export table.
set(REXRUNTIME_DEF "${CMAKE_CURRENT_SOURCE_DIR}/rexruntime.def")
if(WIN32 AND NOT REXGLUE_USE_VULKAN)
    # The curated table contains dual-backend imports. D3D12-only builds must
    # not request exports whose Vulkan implementation is not compiled.
    file(STRINGS "${REXRUNTIME_DEF}" REXRUNTIME_EXPORT_LINES)
    list(FILTER REXRUNTIME_EXPORT_LINES EXCLUDE REGEX "@vulkan@|FLAGS_vulkan|^vma")
    list(JOIN REXRUNTIME_EXPORT_LINES "\n" REXRUNTIME_EXPORT_TEXT)
    set(REXRUNTIME_DEF "${CMAKE_CURRENT_BINARY_DIR}/rexruntime-d3d12.def")
    file(WRITE "${REXRUNTIME_DEF}" "${REXRUNTIME_EXPORT_TEXT}\n")
endif()
# lld-link needs the /DEF: flag (a bare positional .def path is "unknown file
# type"). The flag must reach lld-link (not clang++), so it is passed via
# -Xlinker; and the path must be space-free (the project path contains spaces),
# so it is made relative to the top-level build dir (the linker's working dir).
# Windows only: ELF/Mach-O builds export with default visibility.
if(WIN32)
    file(RELATIVE_PATH REXRUNTIME_DEF_REL "${CMAKE_BINARY_DIR}" "${REXRUNTIME_DEF}")
    set_target_properties(rexruntime PROPERTIES
        LINK_DEPENDS "${REXRUNTIME_DEF}"
        LINK_OPTIONS "-Xlinker;/DEF:${REXRUNTIME_DEF_REL}")
endif()

rex_target_pch(rexruntime
    <mutex>
    <unordered_map>
    <rex/math.h>
    <rex/string.h>
    <rex/memory.h>
    <rex/filesystem.h>
    <rex/thread.h>
    <rex/thread/mutex.h>
    <rex/ppc/context.h>
    <rex/system/kernel_state.h>
)

target_include_directories(rexruntime PUBLIC
    $<BUILD_INTERFACE:${REXGLUE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(rexruntime PRIVATE
    ${REXGLUE_ROOT}/thirdparty
)

target_link_libraries(rexruntime
    PUBLIC
        simde
        fmt::fmt
        spdlog::spdlog
        tomlplusplus
        SDL3::SDL3
    PRIVATE
        aes128
        mspack
        xxHash::xxhash
)

if(REXGLUE_ENABLE_TRACY)
    # TARGET_NAME is what tells install(EXPORT) this text is a target to
    # namespace. Without it the plain name survives into the installed config
    # and consumers link a bare TracyClient they have no search path for.
    target_link_libraries(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:$<TARGET_NAME:TracyClient>>)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PROFILING>)
endif()

if(REXGLUE_ENABLE_PERF_COUNTERS)
    target_compile_definitions(rexruntime PUBLIC
        $<$<NOT:$<CONFIG:Release>>:REXGLUE_ENABLE_PERF_COUNTERS>)
endif()

if(WIN32)
    target_link_libraries(rexruntime PRIVATE ws2_32 iphlpapi)
    target_link_libraries(rexruntime PUBLIC dxgi dxguid)
    target_compile_definitions(rexruntime PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(REXGLUE_USE_D3D12)
    target_link_libraries(rexruntime PUBLIC dxc-headers)
    target_compile_definitions(rexruntime PUBLIC REX_HAS_D3D12=1)
endif()
if(REXGLUE_USE_VULKAN)
    target_link_libraries(rexruntime PUBLIC
        Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator
        spirv-tools-headers
    )
    target_compile_definitions(rexruntime PUBLIC REX_HAS_VULKAN=1)
endif()
