# Always list every optional source/dep here. IDF's component manager parses
# this call eagerly, so conditionals that depend on Kconfig values must be
# applied AFTER idf_component_register via target_sources / idf_component_optional_requires.
idf_component_register(
    SRCS "c/stub.c" "c/nats.c" "c/drivers.c" "c/sources.c" "c/user.c" "c/creds.c"
    INCLUDE_DIRS "c"
    PRIV_REQUIRES esp_timer esp_driver_tsens esp_wifi esp_event esp_netif nvs_flash lwip
)

if(CONFIG_TINYBLOK_NATS_TLS)
    idf_component_optional_requires(PRIVATE esp-tls mbedtls)
endif()

# Embed the .creds file as a NUL-terminated text blob accessible via the
# `_binary_nats_creds_start` / `_binary_nats_creds_end` linker symbols, and
# pull in monocypher for Ed25519 signing. Both only when creds auth is on.
if(CONFIG_TINYBLOK_NATS_AUTH_CREDS)
    target_sources(${COMPONENT_LIB} PRIVATE "c/tweetnacl.c" "c/ed25519.c")
    # TweetNaCl predates GCC 14's stricter -Wunterminated-string-initialization
    # and uses `static const u8 sigma[16] = "expand 32-byte k"` (17 chars
    # including NUL into a 16-byte array — intentional, the NUL is unwanted).
    # Suppress only on tweetnacl.c.
    set_source_files_properties(c/tweetnacl.c PROPERTIES
        COMPILE_FLAGS "-Wno-error=unterminated-string-initialization")

    set(CREDS_FILE ${CMAKE_SOURCE_DIR}/${CONFIG_TINYBLOK_NATS_CREDS_FILE})
    if(NOT EXISTS ${CREDS_FILE})
        message(FATAL_ERROR
            "TINYBLOK_NATS_AUTH_CREDS is enabled but ${CREDS_FILE} does not exist. "
            "Generate one with `nsc generate creds -a <account> -n <user>` "
            "or copy secrets/nats.creds.example as a starting point. "
            "Path is configurable via CONFIG_TINYBLOK_NATS_CREDS_FILE.")
    endif()
    target_add_binary_data(${COMPONENT_LIB} ${CREDS_FILE} TEXT
                           RENAME_TO nats_creds)
endif()

set(ZIG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/zig/main.zig)
set(GEN_SCRIPT ${CMAKE_SOURCE_DIR}/tools/gen.py)
set(PATCHBAY_EDN ${CMAKE_SOURCE_DIR}/patchbay.edn)
set(GEN_RULES ${CMAKE_CURRENT_SOURCE_DIR}/zig/rules.zig)
set(ZIG_DEPS
    ${CMAKE_CURRENT_SOURCE_DIR}/zig/main.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/zig/user.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/zig/bytes.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/zig/patchbay.zig
    ${CMAKE_CURRENT_SOURCE_DIR}/zig/kernel.zig
    ${GEN_RULES}
    ${CMAKE_CURRENT_SOURCE_DIR}/zig/tx_ring.zig
)
set(ZIG_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/zig-out)
set(ZIG_LIB ${ZIG_OUT_DIR}/libtinyblok.a)

# Match the riscv32 ABI/feature set ESP-IDF's GCC uses for the C6 HP core.
set(ZIG_TARGET "riscv32-freestanding-none")
set(ZIG_CPU "generic_rv32+m+a+c+zicsr+zifencei")

# Resolve a python interpreter once at configure time. Override with
# `cmake -DPython3_EXECUTABLE=/path/to/python` if needed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Codegen: regenerate main/zig/rules.zig whenever patchbay.edn or gen.py changes.
# rules.zig is gitignored; this is the canonical source-of-truth invocation.
add_custom_command(
    OUTPUT ${GEN_RULES}
    COMMAND ${Python3_EXECUTABLE} ${GEN_SCRIPT} ${PATCHBAY_EDN} ${GEN_RULES}
    DEPENDS ${PATCHBAY_EDN} ${GEN_SCRIPT}
    COMMENT "Generating rules.zig from patchbay.edn"
    VERBATIM
)

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
