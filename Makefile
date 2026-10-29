# ElfDrumLXR — Phase 0 engine harness.
# Native build of the LXR-02 sound engine (Erica Synths lxr-desktop-app, GPL-3.0) for offline
# rendering and golden tests. Structure follows mechanical-dance/PHX-D01's Makefile (GPL-3.0; see
# NOTICE): the vendor tree is copied to build/src and patched there; vendor/ stays pristine.
#   make            -> build/lxr-render
#   make test       -> intrinsics unit tests
#   make B=build-f FP16=float     -> alternate build dir with __fp16 mapped to float (Phase 0 experiment)
VENDOR ?= vendor/lxr-02
B := build
SRC := $(B)/src/Core/Src
INC := $(B)/src/Core/Inc
# __fp16 has no direct gcc equivalent; _Float16 is the exact-width IEEE half type (what PHX-D01
# used with gcc on Linux). `make FP16=float` tests the plain-float mapping instead.
FP16 ?= _Float16
ENGINE := $(shell cd $(VENDOR)/Core/Src 2>/dev/null && ls Soundengine/DSPAudio/*.c Soundengine/DSPAudio/FX/*.c Soundengine/DSPAudio/FX/*/*.c \
            Soundengine/FxBus.c Soundengine/MIDI/*.c Soundengine/Sequencer/*.c | grep -v -e MIDI/Uart.c)
OBJS := $(patsubst %.c,$(B)/obj/%.o,$(ENGINE)) $(B)/obj/engine/engine.o $(B)/obj/engine/stubs.o $(B)/obj/engine/kit.o $(B)/obj/engine/seqbridge.o $(B)/obj/menu/Cc2Text.o
CFLAGS := -std=gnu11 -O2 -w -D__fp16=$(FP16) '-Dsection(x)=unused' -Ishim -Iengine -I$(INC) -I$(INC)/Soundengine -I$(SRC)

all: $(B)/lxr-render

$(B)/.patched: $(wildcard patches/*.patch) $(VENDOR)/VENDOR_OK
	rm -rf $(B)/src && mkdir -p $(B)/src && cp -R $(VENDOR)/Core $(B)/src/
	for p in patches/*.patch; do patch -s -p1 -d $(B)/src < $$p || exit 1; done
	touch $@

# Stamp file proving the vendor checkout is at the pinned commit (scripts/fetch-vendor.sh enforces it).
$(VENDOR)/VENDOR_OK: VENDOR_COMMIT scripts/fetch-vendor.sh
	./scripts/fetch-vendor.sh && touch $@

$(B)/obj/%.o: $(VENDOR)/Core/Src/%.c $(B)/.patched
	@mkdir -p $(dir $@); $(CC) $(CFLAGS) -c $(SRC)/$*.c -o $@

$(B)/obj/engine/%.o: engine/%.c engine/engine.h $(B)/.patched
	@mkdir -p $(dir $@); $(CC) $(CFLAGS) -c $< -o $@

$(B)/lxr-render: $(OBJS) $(B)/obj/tools/lxr-render.o
	$(CC) $^ -o $@ -lm

$(B)/obj/tools/%.o: tools/%.c engine/engine.h $(B)/.patched
	@mkdir -p $(dir $@); $(CC) $(CFLAGS) -c $< -o $@

test: $(B)/test_intrinsics $(B)/test_midi_parser $(B)/seq-pat
	./$(B)/test_intrinsics
	./$(B)/test_midi_parser
	./$(B)/seq-pat

# Phase 3 gate (a): .PAT round-trip (also part of `make test`).
$(B)/seq-pat: $(OBJS) $(B)/obj/tools/seq-pat.o
	@mkdir -p $(B); $(CC) $^ -o $@ -lm

# Phase 3 gate (b) reference: drives seq_tick() via the wrapper pump and logs every
# voice trigger (link-time --wrap of the firmware's trigger/automation entry points).
$(B)/seq-ref: $(OBJS) $(B)/obj/tools/seq-ref.o
	@mkdir -p $(B); $(CC) $^ -o $@ -lm \
		-Wl,--wrap=voiceControl_noteOn,--wrap=autoNode_setDestination,--wrap=autoNode_updateValue

$(B)/test_intrinsics: tests/test_intrinsics.c shim/stm32f7xx_hal.h
	@mkdir -p $(B); $(CC) -std=gnu11 -O2 -Wall -Ishim $< -o $@

$(B)/test_midi_parser: tests/test_midi_parser.c $(OBJS)
	@mkdir -p $(B)/obj; $(CC) $(CFLAGS) -c $< -o $(B)/obj/test_midi_parser.o
	$(CC) $(B)/obj/test_midi_parser.o $(OBJS) -o $@ -lm

clean:
	rm -rf $(B)
.PHONY: all test clean
