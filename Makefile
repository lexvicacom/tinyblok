IDF_EXPORT     ?=
IDF_PATH       ?=
DEFAULT_IDF_PATH ?= $(HOME)/esp-idf-v6.0.1
IDF_PY         ?= idf.py
PORT           ?=
HOST_CC        ?= cc
NATS_HOST_PORT ?= 4223
NATS_PROTOCOL_PORT ?= 4224
HOST_BUILD_DIR ?= build/host
MONOBLOK_DIR   ?= third_party/monoblok
MONOBLOK_SRC_DIR := $(MONOBLOK_DIR)/src
MONOBLOK_PATCHBAY_DIR := $(MONOBLOK_SRC_DIR)/patchbay
MONOBLOK_YYJSON_DIR := $(MONOBLOK_DIR)/vendor/yyjson/src
PATCHBAY_JSON ?= 0

MONOBLOK_CORE_C_SRCS = \
	$(MONOBLOK_SRC_DIR)/array.c \
	$(MONOBLOK_SRC_DIR)/buf.c \
	$(MONOBLOK_SRC_DIR)/fs.c \
	$(MONOBLOK_SRC_DIR)/proto.c \
	$(MONOBLOK_SRC_DIR)/router.c \
	$(MONOBLOK_SRC_DIR)/slice.c

PATCHBAY_C_SRCS = \
	$(MONOBLOK_PATCHBAY_DIR)/pb_arena.c \
	$(MONOBLOK_PATCHBAY_DIR)/pb_eval.c \
	$(MONOBLOK_PATCHBAY_DIR)/pb_forms.c \
	$(MONOBLOK_PATCHBAY_DIR)/pb_json.c \
	$(MONOBLOK_PATCHBAY_DIR)/pb_program.c \
	$(MONOBLOK_PATCHBAY_DIR)/pb_sexpr.c \
	$(MONOBLOK_PATCHBAY_DIR)/pb_validate.c \
	$(MONOBLOK_CORE_C_SRCS)

ifeq ($(PATCHBAY_JSON),1)
PATCHBAY_C_SRCS += $(MONOBLOK_YYJSON_DIR)/yyjson.c
PATCHBAY_JSON_DEF := -DPB_ENABLE_JSON=1 -I$(MONOBLOK_YYJSON_DIR)
else
PATCHBAY_JSON_DEF := -DPB_ENABLE_JSON=0
endif

-include local.mk

ifeq ($(strip $(IDF_PATH)),)
ifneq ($(wildcard $(DEFAULT_IDF_PATH)/export.sh),)
IDF_PATH := $(DEFAULT_IDF_PATH)
endif
endif

ifeq ($(strip $(IDF_EXPORT)),)
ifneq ($(strip $(IDF_PATH)),)
IDF_EXPORT := $(IDF_PATH)/export.sh
endif
endif

ifneq ($(strip $(IDF_EXPORT)),)
IDF := . "$(IDF_EXPORT)" >/dev/null && $(IDF_PY)
else
IDF := $(IDF_PY)
endif

PORT_ARG := $(if $(strip $(PORT)),-p $(PORT),)

.PHONY: check-idf check-monoblok gen soundcheck build test nats-protocol-test nats-host-smoke flash monitor flash-monitor erase-flash-monitor clean fullclean menuconfig

check-idf:
	@if [ -n "$(IDF_EXPORT)" ]; then \
		if [ ! -f "$(IDF_EXPORT)" ]; then \
			echo "error: IDF export script not found: $(IDF_EXPORT)" >&2; \
			echo "hint: set IDF_PATH=/path/to/esp-idf or IDF_EXPORT=/path/to/esp-idf/export.sh" >&2; \
			exit 1; \
		fi; \
	elif ! command -v "$(IDF_PY)" >/dev/null 2>&1; then \
		echo "error: $(IDF_PY) not found in PATH" >&2; \
		echo "hint: run '. /path/to/esp-idf/export.sh', set IDF_PATH=/path/to/esp-idf, or pass IDF_EXPORT=/path/to/esp-idf/export.sh" >&2; \
		exit 1; \
	fi

check-monoblok:
	@if [ ! -f "$(MONOBLOK_PATCHBAY_DIR)/pb_eval.c" ]; then \
		echo "error: monoblok submodule is missing at $(MONOBLOK_DIR)" >&2; \
		echo "hint: run 'git submodule update --init --recursive' or pass MONOBLOK_DIR=/path/to/monoblok" >&2; \
		exit 1; \
	fi

gen:
	@echo "patchbay.edn is runtime-parsed; no generated rules step"

soundcheck: check-monoblok
	@mkdir -p $(HOST_BUILD_DIR)
	@echo "CC $(HOST_BUILD_DIR)/patchbay_check"
	@$(HOST_CC) -std=c17 -D_GNU_SOURCE -DTINYBLOK $(PATCHBAY_JSON_DEF) \
		-I$(MONOBLOK_SRC_DIR) -I$(MONOBLOK_PATCHBAY_DIR) \
		tests/host/patchbay_check.c $(PATCHBAY_C_SRCS) -lm -o $(HOST_BUILD_DIR)/patchbay_check
	@$(HOST_BUILD_DIR)/patchbay_check patchbay.edn

build: check-idf
	@$(IDF) build

test: soundcheck nats-protocol-test

nats-protocol-test:
	@mkdir -p $(HOST_BUILD_DIR)
	@echo "CC $(HOST_BUILD_DIR)/nats_protocol_test"
	@$(HOST_CC) -std=c11 -D_GNU_SOURCE -DCONFIG_TINYBLOK_NATS_PORT=$(NATS_PROTOCOL_PORT) \
		-Itests/host/include -Imain/c tests/host/nats_protocol_test.c main/c/nats.c main/c/app_events.c \
		-pthread -o $(HOST_BUILD_DIR)/nats_protocol_test
	@$(HOST_BUILD_DIR)/nats_protocol_test

nats-host-smoke:
	@command -v nats-server >/dev/null || { echo "error: nats-server not found in PATH" >&2; exit 1; }
	@command -v nats >/dev/null || { echo "error: nats CLI not found in PATH" >&2; exit 1; }
	@mkdir -p $(HOST_BUILD_DIR)
	@echo "CC $(HOST_BUILD_DIR)/nats_smoke"
	@$(HOST_CC) -std=c11 -D_GNU_SOURCE -DCONFIG_TINYBLOK_NATS_PORT=$(NATS_HOST_PORT) \
		-Itests/host/include -Imain/c tests/host/nats_smoke.c main/c/nats.c main/c/app_events.c \
		-o $(HOST_BUILD_DIR)/nats_smoke
	@echo "NATS host smoke on 127.0.0.1:$(NATS_HOST_PORT)"
	@log="$(HOST_BUILD_DIR)/nats-server.log"; \
	svc_log="$(HOST_BUILD_DIR)/nats-smoke.log"; \
	sub_log="$(HOST_BUILD_DIR)/nats-sub.log"; \
	nats-server -p $(NATS_HOST_PORT) -a 127.0.0.1 >"$$log" 2>&1 & \
	server_pid=$$!; \
	trap 'kill $$server_pid $$svc_pid $$sub_pid >/dev/null 2>&1 || true; wait $$server_pid $$svc_pid $$sub_pid >/dev/null 2>&1 || true' EXIT; \
	sleep 0.2; \
	$(HOST_BUILD_DIR)/nats_smoke serve >"$$svc_log" 2>&1 & \
	svc_pid=$$!; \
	sleep 0.2; \
	reply=$$(nats --no-context -s nats://127.0.0.1:$(NATS_HOST_PORT) request --raw --timeout 2s tinyblok.req.echo "smoke"); \
	test "$$reply" = "smoke" || { echo "unexpected nats reply: '$$reply'" >&2; cat "$$svc_log" >&2; exit 1; }; \
	for i in 1 2 3 4 5 6 7 8 9 10; do \
		if ! kill -0 $$svc_pid >/dev/null 2>&1; then \
			wait $$svc_pid; \
			cat "$$svc_log"; \
			break; \
		fi; \
		sleep 0.1; \
	done; \
	if kill -0 $$svc_pid >/dev/null 2>&1; then \
		echo "nats smoke service did not exit after reply" >&2; \
		cat "$$svc_log" >&2; \
		echo "nats host smoke failed; server log:" >&2; \
		tail -40 "$$log" >&2; \
		exit 1; \
	fi; \
	nats --no-context -s nats://127.0.0.1:$(NATS_HOST_PORT) subscribe --raw --count 5 --wait 2s tinyblok.host.pub >"$$sub_log" 2>&1 & \
	sub_pid=$$!; \
	sleep 0.2; \
	$(HOST_BUILD_DIR)/nats_smoke pub; \
	wait $$sub_pid; \
	pub=$$(cat "$$sub_log"); \
	expected=$$(printf "beep0\nbeep1\nbeep2\nbeep3\nbeep4"); \
	test "$$pub" = "$$expected" || { echo "unexpected pub payloads:" >&2; cat "$$sub_log" >&2; exit 1; }; \
	echo "ok nats cli observed pub batch"; \
	cat "$$sub_log"

flash: build
	@$(IDF) $(PORT_ARG) flash

monitor: check-idf
	@echo "(exit with Ctrl+])"
	@$(IDF) $(PORT_ARG) monitor

flash-monitor: build
	@echo "(exit with Ctrl+])"
	@$(IDF) $(PORT_ARG) flash monitor

erase-flash-monitor: build
	@echo "(exit with Ctrl+])"
	@$(IDF) $(PORT_ARG) erase-flash flash monitor

clean: check-idf
	@$(IDF) clean

fullclean: check-idf
	@$(IDF) fullclean

menuconfig: check-idf
	@$(IDF) menuconfig
