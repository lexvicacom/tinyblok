IDF_EXPORT := /Users/alex/esp-idf-v6.0.1/export.sh
PORT       := /dev/cu.usbmodem101
IDF        := . $(IDF_EXPORT) >/dev/null && idf.py

.PHONY: build flash monitor flash-monitor clean fullclean menuconfig

build:
	@$(IDF) build

flash: build
	@$(IDF) -p $(PORT) flash

monitor:
	@echo "(exit with Ctrl+])"
	@$(IDF) -p $(PORT) monitor

flash-monitor: build
	@echo "(exit with Ctrl+])"
	@$(IDF) -p $(PORT) flash monitor

clean:
	@$(IDF) clean

fullclean:
	@$(IDF) fullclean

menuconfig:
	@$(IDF) menuconfig
