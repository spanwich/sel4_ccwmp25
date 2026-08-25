if(NOT DEFINED ROOTSERVER_BUILD_ID)
    set(ROOTSERVER_BUILD_ID "ccwmp25-mk-phase1-v16-01" CACHE STRING "Marker printed by the Phase 1 rootserver")
endif()

add_executable(rootserver_hello EXCLUDE_FROM_ALL main.c)
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
        sel4
        sel4runtime
        sel4muslcsys
)

target_link_options(rootserver_hello PRIVATE "-Wl,--section-start=.mk_trace=0x41f000")
