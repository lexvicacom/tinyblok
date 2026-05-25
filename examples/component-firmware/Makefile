IDF_EXPORT     ?=
IDF_PATH       ?=
DEFAULT_IDF_PATH ?= $(HOME)/esp-idf-v6.0.1
IDF_PY         ?= idf.py
PORT           ?=
TINYBLOK_REPO  ?= $(abspath ../..)

-include $(TINYBLOK_REPO)/local.mk
-include local.mk

.DEFAULT_GOAL := build

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

.PHONY: check-idf build flash monitor flash-monitor clean fullclean menuconfig

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

build: check-idf
	@$(IDF) build

flash: check-idf
	@$(IDF) $(PORT_ARG) flash

monitor: check-idf
	@echo "(exit with Ctrl+])"
	@$(IDF) $(PORT_ARG) monitor

flash-monitor: check-idf
	@echo "(exit with Ctrl+])"
	@$(IDF) $(PORT_ARG) flash monitor

clean: check-idf
	@$(IDF) clean

fullclean: check-idf
	@$(IDF) fullclean

menuconfig: check-idf
	@$(IDF) menuconfig
