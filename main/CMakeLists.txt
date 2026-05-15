# Always list every optional source/dep here. IDF's component manager parses
# this call eagerly, so conditionals that depend on Kconfig values must be
# applied AFTER idf_component_register via target_sources / idf_component_optional_requires.
idf_component_register(
    SRCS "c/stub.c" "c/nats.c" "c/drivers.c" "c/sources.c" "c/user.c" "c/creds.c" "c/app_events.c" "c/display.c"
         "c/tinyblok_config.c" "c/tinyblok_wifi.c" "c/tinyblok_web.c" "c/tinyblok_patchbay.c" "c/tinyblok_tx_ring.c"
         "c/patchbay/pb_arena.c" "c/patchbay/pb_builtins.c" "c/patchbay/pb_eval.c"
         "c/patchbay/pb_program.c" "c/patchbay/pb_sexpr.c" "c/patchbay/pb_window_builtins.c"
    INCLUDE_DIRS "c" "c/patchbay" "c/vendor/yyjson"
    PRIV_REQUIRES esp_timer esp_driver_tsens esp_driver_i2c esp_driver_gpio esp_wifi esp_event esp_netif nvs_flash lwip
                  esp_http_server mdns
)

file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION TINYBLOK_VERSION LIMIT_COUNT 1)
if(NOT TINYBLOK_VERSION)
    set(TINYBLOK_VERSION "dev")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE TINYBLOK TINYBLOK_VERSION=\"${TINYBLOK_VERSION}\")

if(CONFIG_TINYBLOK_PATCHBAY_JSON)
    target_sources(${COMPONENT_LIB} PRIVATE "c/patchbay/pb_json.c" "c/vendor/yyjson/yyjson.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PB_ENABLE_JSON=1)
    set_source_files_properties(c/vendor/yyjson/yyjson.c PROPERTIES
        COMPILE_FLAGS "-Wno-error=maybe-uninitialized")
endif()

set(RSRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rsrc)
set(GEN_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(SETUP_HTML_HEADER ${GEN_INCLUDE_DIR}/tinyblok_setup_html.h)
set(CAPTIVE_HTML_HEADER ${GEN_INCLUDE_DIR}/tinyblok_captive_html.h)
set(PATCHBAY_EDN_HEADER ${GEN_INCLUDE_DIR}/tinyblok_patchbay_edn.h)
find_program(XXD_EXECUTABLE xxd REQUIRED)

add_custom_command(
    OUTPUT ${SETUP_HTML_HEADER}
    COMMAND ${XXD_EXECUTABLE} -i -n tinyblok_setup_html ${RSRC_DIR}/setup.html ${SETUP_HTML_HEADER}
    DEPENDS ${RSRC_DIR}/setup.html
    COMMENT "Embedding setup.html"
    VERBATIM
)

add_custom_command(
    OUTPUT ${CAPTIVE_HTML_HEADER}
    COMMAND ${XXD_EXECUTABLE} -i -n tinyblok_captive_html ${RSRC_DIR}/captive.html ${CAPTIVE_HTML_HEADER}
    DEPENDS ${RSRC_DIR}/captive.html
    COMMENT "Embedding captive.html"
    VERBATIM
)

add_custom_command(
    OUTPUT ${PATCHBAY_EDN_HEADER}
    COMMAND ${XXD_EXECUTABLE} -i -n tinyblok_patchbay_edn ${CMAKE_SOURCE_DIR}/patchbay.edn ${PATCHBAY_EDN_HEADER}
    DEPENDS ${CMAKE_SOURCE_DIR}/patchbay.edn
    COMMENT "Embedding patchbay.edn"
    VERBATIM
)

add_custom_target(tinyblok_web_assets DEPENDS ${SETUP_HTML_HEADER} ${CAPTIVE_HTML_HEADER} ${PATCHBAY_EDN_HEADER})
add_dependencies(${COMPONENT_LIB} tinyblok_web_assets)
target_include_directories(${COMPONENT_LIB} PRIVATE ${GEN_INCLUDE_DIR})
set_source_files_properties(c/tinyblok_web.c PROPERTIES
    OBJECT_DEPENDS "${SETUP_HTML_HEADER};${CAPTIVE_HTML_HEADER}")
set_source_files_properties(c/tinyblok_patchbay.c PROPERTIES
    OBJECT_DEPENDS "${PATCHBAY_EDN_HEADER}")

if(CONFIG_TINYBLOK_NATS_TLS)
    idf_component_optional_requires(PRIVATE esp-tls mbedtls)
endif()

# Runtime .creds support parses the uploaded NVS blob and signs server nonces.
if(CONFIG_TINYBLOK_NATS_CREDS_SUPPORT)
    target_sources(${COMPONENT_LIB} PRIVATE "c/tweetnacl.c" "c/ed25519.c")
    # TweetNaCl predates GCC 14's stricter -Wunterminated-string-initialization
    # and uses `static const u8 sigma[16] = "expand 32-byte k"` (17 chars
    # including NUL into a 16-byte array — intentional, the NUL is unwanted).
    # Suppress only on tweetnacl.c.
    set_source_files_properties(c/tweetnacl.c PROPERTIES
        COMPILE_FLAGS "-Wno-error=unterminated-string-initialization")

endif()
