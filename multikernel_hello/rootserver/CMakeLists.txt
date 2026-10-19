if(NOT DEFINED ROOTSERVER_BUILD_ID)
    set(ROOTSERVER_BUILD_ID "ccwmp25-mk-d1-v16" CACHE STRING "Marker printed by the Phase 1 rootserver")
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
# ---- Paper experiments, corrected design: x86 D1 gate pipeline on K0 ----
# (vendored unchanged in ../gateway; see gateway/PROVENANCE.txt)
set(MK_ENGINE "everparse" CACHE STRING "K0 image: raw | gateonly | everparse | cedar")
set(GW ${CMAKE_CURRENT_SOURCE_DIR}/../gateway)
target_include_directories(rootserver_hello PRIVATE ${GW}/include ${GW}/demo/include)
if(MK_KERNEL_ID STREQUAL "0")
    set(MK_REAL_ENGINE 0)
    if(MK_ENGINE STREQUAL "everparse" OR MK_ENGINE STREQUAL "cedar")
        set(MK_REAL_ENGINE 1)
    endif()
    # The three D1 PDs, compiled from the unchanged x86 sources against the
    # k0shim/microkit.h stand-in; each PD's entry points and config globals
    # get a per-PD name so they can share K0's single image.
    set(PDS ${GW}/demo/pds)
    target_sources(rootserver_hello PRIVATE gateway_k0.c
        ${PDS}/policy_gate.c ${PDS}/authenticator.c ${PDS}/protected_app.c
        ${GW}/common/pipeline.c ${GW}/common/wire.c ${GW}/common/oracle.c
        ${GW}/demo/generated/HTTP_Extract_Bearer.c ${GW}/demo/generated/HTTP_Extract_Complete.c
        ${GW}/demo/generated/HTTP_Extract_Header.c ${GW}/demo/generated/HTTP_Extract_Method.c
        ${GW}/demo/generated/HTTP_Extract_Path.c ${GW}/demo/generated/HTTP_Extract_Scan.c
        ${GW}/demo/generated/HTTP_Extract_Types.c)
    target_include_directories(rootserver_hello PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/k0shim ${GW}/demo/generated ${GW}/engines/everparse/generated)
    target_compile_definitions(rootserver_hello PRIVATE MK_ENGINE_${MK_ENGINE}=1
                               MK_ENGINE_NAME="${MK_ENGINE}" MK_REAL_ENGINE=${MK_REAL_ENGINE}
                               D1_FAULT_KIND=0)
    set_source_files_properties(${PDS}/policy_gate.c PROPERTIES COMPILE_DEFINITIONS
        "init=pg_init;notified=pg_notified;demo_config=pg_demo_config;auth_config=pg_auth_config")
    set_source_files_properties(${PDS}/authenticator.c PROPERTIES COMPILE_DEFINITIONS
        "init=au_init;notified=au_notified;auth_config=au_auth_config")
    set_source_files_properties(${PDS}/protected_app.c PROPERTIES COMPILE_DEFINITIONS
        "init=pa_init;notified=pa_notified;demo_config=pa_demo_config")
    # As x86 demo.mk: the F* extractor's write_u32_le must not clash with wire.c's.
    file(GLOB FSTAR_C ${GW}/demo/generated/*.c)
    set_source_files_properties(${FSTAR_C} PROPERTIES COMPILE_DEFINITIONS
        "write_u32_le=fstar_write_u32_le")
    if(MK_REAL_ENGINE)
        # D2: engine equivalence corpus and the F* extractor test vectors.
        add_library(gw_common STATIC EXCLUDE_FROM_ALL
            ${GW}/host_tests/test_equivalence.c ${GW}/verified/test_extractor.c)
        target_include_directories(gw_common PUBLIC ${GW}/include ${GW}/demo/generated)
        set_source_files_properties(${GW}/host_tests/test_equivalence.c PROPERTIES
            COMPILE_DEFINITIONS "main=equivalence_main")
        set_source_files_properties(${GW}/verified/test_extractor.c PROPERTIES
            COMPILE_DEFINITIONS "main=extractor_test_main;write_u32_le=fstar_write_u32_le")
        target_link_libraries(gw_common PUBLIC muslc)
        target_link_libraries(rootserver_hello PRIVATE gw_common)
    else()
        target_sources(rootserver_hello PRIVATE engine_accept_all.c)
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
