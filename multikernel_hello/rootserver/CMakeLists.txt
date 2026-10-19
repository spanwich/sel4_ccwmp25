if(NOT DEFINED ROOTSERVER_BUILD_ID)
    set(ROOTSERVER_BUILD_ID "ccwmp25-mk-c3-v16" CACHE STRING "Marker printed by the Phase 1 rootserver")
endif()

add_library(mk_channel STATIC EXCLUDE_FROM_ALL ../channel/mk_channel.c)
target_include_directories(mk_channel PUBLIC ../channel)
target_link_libraries(mk_channel PUBLIC muslc)

# lwIP 2.1.2 (projects/lwip), raw API, NO_SYS; used by K1 only.
set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../lwip)
include(${LWIP_DIR}/src/Filelists.cmake)
add_library(rs_lwip STATIC EXCLUDE_FROM_ALL
    ${lwipcore_SRCS} ${lwipcore4_SRCS} ${LWIP_DIR}/src/netif/ethernet.c)
target_include_directories(rs_lwip PUBLIC
    ${LWIP_DIR}/src/include ${CMAKE_CURRENT_SOURCE_DIR}/lwip)
target_compile_options(rs_lwip PRIVATE -w)
target_link_libraries(rs_lwip PUBLIC muslc)

add_executable(rootserver_hello EXCLUDE_FROM_ALL main.c optee_scmi.c dwmac.c net.c)
target_compile_definitions(rootserver_hello PRIVATE
    KERNEL_ID=${MK_KERNEL_ID}
    ROOTSERVER_BUILD_ID="${ROOTSERVER_BUILD_ID}"
)

if(MK_KERNEL_ID STREQUAL "0")
    if(NOT DEFINED MK_CORE1_TRAMPOLINE_PADDR)
        message(FATAL_ERROR "K0 requires MK_CORE1_TRAMPOLINE_PADDR from the final elfloader link")
    endif()
    target_compile_definitions(
        rootserver_hello PRIVATE
        MK_CORE1_TRAMPOLINE_PADDR=${MK_CORE1_TRAMPOLINE_PADDR}
    )
endif()
# ---- Paper experiments: access-control engine on K0 (vendored, ../gateway) ----
set(MK_ENGINE "none" CACHE STRING "K0 engine: none | everparse | cedar")
set(GW ${CMAKE_CURRENT_SOURCE_DIR}/../gateway)
target_include_directories(rootserver_hello PRIVATE ${GW}/include)
if(MK_KERNEL_ID STREQUAL "0")
    target_sources(rootserver_hello PRIVATE gateway_k0.c)
    target_compile_definitions(rootserver_hello PRIVATE MK_ENGINE_${MK_ENGINE}=1
                               MK_ENGINE_NAME="${MK_ENGINE}")
    if(NOT MK_ENGINE STREQUAL "none")
        add_library(gw_common STATIC EXCLUDE_FROM_ALL
            ${GW}/common/oracle.c ${GW}/common/wire.c ${GW}/host_tests/test_equivalence.c)
        target_include_directories(gw_common PUBLIC ${GW}/include)
        target_compile_definitions(gw_common PRIVATE main=equivalence_main)
        target_link_libraries(gw_common PUBLIC muslc)
        target_link_libraries(rootserver_hello PRIVATE gw_common)
    endif()
    if(MK_ENGINE STREQUAL "everparse")
        set(EP ${GW}/engines/everparse)
        add_library(gw_engine STATIC EXCLUDE_FROM_ALL
            ${EP}/engine.c ${EP}/generated/RbacPolicy.c ${EP}/generated/RbacPolicyWrapper.c
            ${EP}/verified_gate/Gate.c ${EP}/verified_gate/Partition8.c
            ${EP}/verified_gate/UniversalRate.c ${EP}/verified_policyblob/Blob.c)
        target_include_directories(gw_engine PUBLIC ${GW}/include ${EP}/generated)
        target_link_libraries(gw_engine PUBLIC muslc)
        target_link_libraries(rootserver_hello PRIVATE gw_engine)
    elseif(MK_ENGINE STREQUAL "cedar")
        set(MK_CEDAR_STATICLIB "" CACHE FILEPATH "libcedar_gateway_staticlib.a (aarch64-unknown-linux-musl)")
        target_sources(rootserver_hello PRIVATE cedar_libc_stubs.c k0_malloc.c)
        execute_process(COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
                        OUTPUT_VARIABLE LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
        target_link_libraries(rootserver_hello PRIVATE ${MK_CEDAR_STATICLIB} ${LIBGCC})
        target_link_options(rootserver_hello PRIVATE -Wl,--gc-sections)
    endif()
endif()

target_link_libraries(
    rootserver_hello
    PRIVATE
        mk_channel
        rs_lwip
        sel4
        sel4runtime
        sel4muslcsys
)


# Paper experiments: measure optimised code (-O2, asserts kept), as the x86
# host builds do. The CMake build type stays Debug for the seL4 libraries.
foreach(t rootserver_hello mk_channel rs_lwip)
    target_compile_options(${t} PRIVATE -O2)
endforeach()
foreach(t gw_engine gw_common)
    if(TARGET ${t})
        target_compile_options(${t} PRIVATE -O2)
    endif()
endforeach()
# The allocator must not be pattern-matched into calls to itself
# (GCC -O2 turns malloc+memset in calloc() back into calloc()).
set_source_files_properties(k0_malloc.c PROPERTIES COMPILE_OPTIONS "-fno-builtin")
