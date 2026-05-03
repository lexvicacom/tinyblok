idf_component_register(
    SRCS "stub.c" "nats.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_timer esp_driver_tsens esp_wifi esp_event esp_netif nvs_flash lwip
)

set(ZIG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/main.zig)
set(ZIG_DEPS
    ${CMAKE_CURRENT_SOURCE_DIR}/main.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/patchbay.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/rules.zig
)
set(ZIG_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/zig-out)
set(ZIG_LIB ${ZIG_OUT_DIR}/libtinyblok.a)

# Match the riscv32 ABI/feature set ESP-IDF's GCC uses for the C6 HP core.
set(ZIG_TARGET "riscv32-freestanding-none")
set(ZIG_CPU "generic_rv32+m+a+c+zicsr+zifencei")

add_custom_command(
    OUTPUT ${ZIG_LIB}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ZIG_OUT_DIR}
    COMMAND zig build-lib ${ZIG_SRC}
            -target ${ZIG_TARGET}
            -mcpu=${ZIG_CPU}
            -O ReleaseSmall
            -fno-stack-check
            -femit-bin=${ZIG_LIB}
            --name tinyblok
    DEPENDS ${ZIG_DEPS}
    WORKING_DIRECTORY ${ZIG_OUT_DIR}
    COMMENT "Building Zig static library (${ZIG_TARGET}, ${ZIG_CPU})"
    VERBATIM
)

add_custom_target(zig_lib DEPENDS ${ZIG_LIB})
add_dependencies(${COMPONENT_LIB} zig_lib)
target_link_libraries(${COMPONENT_LIB} PRIVATE ${ZIG_LIB})
