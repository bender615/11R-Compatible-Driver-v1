CC := xcrun clang
CFLAGS := -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations
BUILD_DIR := build

USB_FRAMEWORKS := -framework IOKit -framework CoreFoundation
MIDI_FRAMEWORKS := -framework CoreMIDI
AUDIO_FRAMEWORKS := -framework AudioToolbox -framework CoreAudio

USB_TOOLS := erduplex erformat erlanes ermidi_test errig_read erstream speedcheck
ALL_TOOLS := $(USB_TOOLS) erengine ermidi_bridge erprobe ersweep gtr_live_probe ringstat
BINARIES := $(addprefix $(BUILD_DIR)/,$(ALL_TOOLS))

.PHONY: all clean assembly

all: $(BINARIES)

$(BUILD_DIR):
	mkdir -p $@

$(addprefix $(BUILD_DIR)/,$(USB_TOOLS)): $(BUILD_DIR)/%: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(USB_FRAMEWORKS) -pthread

$(BUILD_DIR)/erengine: erengine.c ERAudioRing.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(USB_FRAMEWORKS) $(MIDI_FRAMEWORKS) $(AUDIO_FRAMEWORKS) -pthread

$(BUILD_DIR)/ermidi_bridge: ermidi_bridge.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(USB_FRAMEWORKS) $(MIDI_FRAMEWORKS) -pthread

$(BUILD_DIR)/erprobe: erprobe.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(USB_FRAMEWORKS) $(MIDI_FRAMEWORKS)

$(BUILD_DIR)/ersweep: ersweep.c ERAudioRing.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/gtr_live_probe: gtr_live_probe.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/ringstat: ringstat.c ERAudioRing.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

assembly: $(BUILD_DIR)/gtr_probe_bytes.o

$(BUILD_DIR)/gtr_probe_bytes.o: gtr_probe_bytes.s | $(BUILD_DIR)
	$(CC) -arch arm64 -c $< -o $@

clean:
	$(RM) -r $(BUILD_DIR)
