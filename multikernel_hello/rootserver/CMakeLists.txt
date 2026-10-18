if(NOT DEFINED ROOTSERVER_BUILD_ID)
    set(ROOTSERVER_BUILD_ID "ccwmp25-mk-n2-v16" CACHE STRING "Marker printed by the Phase 1 rootserver")
endif()

add_library(mk_channel STATIC EXCLUDE_FROM_ALL ../channel/mk_channel.c)
target_include_directories(mk_channel PUBLIC ../channel)
target_link_libraries(mk_channel PUBLIC muslc)

add_executable(rootserver_hello EXCLUDE_FROM_ALL main.c optee_scmi.c dwmac.c)
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
target_link_libraries(
    rootserver_hello
    PRIVATE
        mk_channel
        sel4
        sel4runtime
        sel4muslcsys
)

