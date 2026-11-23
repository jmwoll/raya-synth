# raya — build raya.c to WebAssembly with raylib + Emscripten.
# SPDX-License-Identifier: LGPL-2.1-or-later
#
#   make deps    fetch emsdk, raylib and shine (MP3 encoder) into deps/ and
#                build them for the web
#   make         build build/index.html (+ .js/.wasm)
#   make serve   serve build/ at http://localhost:8080
#   make drum-samples   render every DRUMS kit to WAV files in drum-samples/

RAYLIB_VERSION ?= 5.5
SHINE_COMMIT   ?= ab5e352
PORT           ?= 8080

DEPS   := deps
EMSDK  := $(DEPS)/emsdk
RAYLIB := $(DEPS)/raylib/src
SHINE  := $(DEPS)/shine
BUILD  := build

# emsdk needs Python >= 3.10; the macOS system python3 is 3.9.
EMSDK_PYTHON ?= $(firstword $(wildcard $(HOME)/.local/bin/python3.12 /opt/homebrew/bin/python3 /usr/local/bin/python3) python3)
export EMSDK_PYTHON

EMCC := $(EMSDK)/upstream/emscripten/emcc
EMAR := $(EMSDK)/upstream/emscripten/emar

CFLAGS  := -std=c99 -Wall -Os -DPLATFORM_WEB -I$(RAYLIB) -I$(SHINE)/src/lib
# miniaudio's Web Audio backend reads Module.HEAPF32, which newer Emscripten
# no longer exports by default. The MP3 decoder keeps large frames on the
# stack, more than Emscripten's 64 KB default.
LDFLAGS := $(RAYLIB)/libraylib.a $(SHINE)/libshine.a -sUSE_GLFW=3 -sALLOW_MEMORY_GROWTH=1 \
           -sEXPORTED_RUNTIME_METHODS=HEAPF32 -sSTACK_SIZE=1048576 \
           -sFORCE_FILESYSTEM=1 -lidbfs.js --shell-file shell.html

.PHONY: all deps serve clean drum-samples

all: $(BUILD)/index.html

$(BUILD)/index.html: raya.c shell.html $(RAYLIB)/libraylib.a $(SHINE)/libshine.a
	@mkdir -p $(BUILD)
	$(EMCC) $(CFLAGS) raya.c -o $@ $(LDFLAGS)

$(RAYLIB)/libraylib.a:
	@echo "raylib not built yet — run 'make deps' first" && exit 1

# shine: fixed-point MP3 encoder (LGPL-2), used to export tapes as .mp3
$(SHINE)/libshine.a:
	@test -d $(SHINE)/src/lib || (echo "shine not fetched yet — run 'make deps' first" && exit 1)
	@mkdir -p $(SHINE)/obj
	for f in $(SHINE)/src/lib/*.c; do $(EMCC) -O2 -c $$f -o $(SHINE)/obj/$$(basename $$f .c).o || exit 1; done
	$(EMAR) rcs $@ $(SHINE)/obj/*.o

deps:
	@mkdir -p $(DEPS)
	test -d $(EMSDK) || git clone --depth 1 https://github.com/emscripten-core/emsdk.git $(EMSDK)
	cd $(EMSDK) && ./emsdk install latest && ./emsdk activate latest
	test -d $(DEPS)/raylib || git clone --depth 1 --branch $(RAYLIB_VERSION) https://github.com/raysan5/raylib.git $(DEPS)/raylib
	$(MAKE) -C $(RAYLIB) PLATFORM=PLATFORM_WEB EMSDK_PATH=$(abspath $(EMSDK)) \
	        CC=$(abspath $(EMCC)) AR=$(abspath $(EMAR)) CUSTOM_CFLAGS=-DSUPPORT_FILEFORMAT_FLAC=1 -j8
	test -d $(SHINE) || (git clone https://github.com/toots/shine.git $(SHINE) && git -C $(SHINE) checkout -q $(SHINE_COMMIT))
	$(MAKE) $(SHINE)/libshine.a

# The drum kits are synthesized by raya.c; this writes them out as WAV files
# with a native compiler (raylib stays unlinked, only the synthesis runs).
comma := ,
UNRESOLVED := $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-undefined$(comma)dynamic_lookup,-Wl$(comma)--unresolved-symbols=ignore-all)

drum-samples: tools/render_drums.c raya.c
	@mkdir -p $(BUILD)
	cc -O2 -std=c99 -w -I$(RAYLIB) -I$(SHINE)/src/lib tools/render_drums.c -o $(BUILD)/render_drums $(UNRESOLVED) -lm
	$(BUILD)/render_drums drum-samples

serve: all
	cd $(BUILD) && $(EMSDK_PYTHON) -m http.server $(PORT)

clean:
	rm -rf $(BUILD)
