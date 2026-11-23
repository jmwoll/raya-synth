# raya — build raya.c to WebAssembly with raylib + Emscripten.
#
#   make deps    fetch emsdk + raylib into deps/ and build raylib for the web
#   make         build build/index.html (+ .js/.wasm)
#   make serve   serve build/ at http://localhost:8080

RAYLIB_VERSION ?= 5.5
PORT           ?= 8080

DEPS   := deps
EMSDK  := $(DEPS)/emsdk
RAYLIB := $(DEPS)/raylib/src
BUILD  := build

# emsdk needs Python >= 3.10; the macOS system python3 is 3.9.
EMSDK_PYTHON ?= $(firstword $(wildcard $(HOME)/.local/bin/python3.12 /opt/homebrew/bin/python3 /usr/local/bin/python3) python3)
export EMSDK_PYTHON

EMCC := $(EMSDK)/upstream/emscripten/emcc

CFLAGS  := -std=c99 -Wall -Os -DPLATFORM_WEB -I$(RAYLIB)
LDFLAGS := $(RAYLIB)/libraylib.a -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 \
           --shell-file shell.html

.PHONY: all deps serve clean

all: $(BUILD)/index.html

$(BUILD)/index.html: raya.c shell.html $(RAYLIB)/libraylib.a
	@mkdir -p $(BUILD)
	$(EMCC) $(CFLAGS) raya.c -o $@ $(LDFLAGS)

$(RAYLIB)/libraylib.a:
	@echo "raylib not built yet — run 'make deps' first" && exit 1

deps:
	@mkdir -p $(DEPS)
	test -d $(EMSDK) || git clone --depth 1 https://github.com/emscripten-core/emsdk.git $(EMSDK)
	cd $(EMSDK) && ./emsdk install latest && ./emsdk activate latest
	test -d $(DEPS)/raylib || git clone --depth 1 --branch $(RAYLIB_VERSION) https://github.com/raysan5/raylib.git $(DEPS)/raylib
	$(MAKE) -C $(RAYLIB) PLATFORM=PLATFORM_WEB EMSDK_PATH=$(abspath $(EMSDK)) \
	        CC=$(abspath $(EMCC)) AR=$(abspath $(EMSDK)/upstream/emscripten/emar) -j8

serve: all
	cd $(BUILD) && $(EMSDK_PYTHON) -m http.server $(PORT)

clean:
	rm -rf $(BUILD)
