# DOOM for the Action mini game consoles
#
#   make                     build for the default board (action104)
#   make BOARD=pacman        build for another board
#   make clean
#
# Output lands in build/<board>/doom.bin
#
# Set CROSS to your arm-none-eabi prefix if it is not on PATH:
#   make CROSS=/path/to/arm-none-eabi-
#
# Flashing is a separate step, see docs/FLASHING.md:
#   python ../tools/flash.py write build/action104/doom.bin 0x08004000 --leave-halted
#   python ../tools/flash.py write ../wad/doom1_e1m1_sfx.wad 0x08090000 --leave-halted

BOARD  ?= action104
CROSS  ?= arm-none-eabi-

CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

GBA     = GBADoom
SDK     = sdk
PORT    = port
BOARDIR = boards/$(BOARD)
# Per board, so that switching BOARD really rebuilds instead of
# silently reusing the other board's objects.
BUILD   = build/$(BOARD)

# The core is a STAR-MC1: ARMv8-M Mainline, Cortex-M33 compatible.
# Soft float on purpose - the FPU is enabled but DOOM does not need it.
ARCH    = -mcpu=cortex-m33 -mthumb -mfloat-abi=soft

CFLAGS  = $(ARCH) -std=gnu11 -O2 -g3 -ffreestanding \
          -ffunction-sections -fdata-sections -fno-common -fno-builtin \
          -Wall -Wno-switch-outside-range -Wno-unused -Wno-sign-compare \
          -Wno-incompatible-pointer-types \
          -I$(GBA)/include -I$(SDK) -I$(PORT) -I$(BOARDIR) \
          -DENABLE_CLOCK_BOOST

LDFLAGS = $(ARCH) -nostartfiles --specs=nano.specs --specs=nosys.specs \
          -Wl,--gc-sections -Wl,-Map,$(BUILD)/doom.map

# Engine: skip the GBA WAD blob and GBA/Maxmod i_audio.c (replaced below).
SKIP     = $(GBA)/source/doom_iwad.c $(GBA)/source/i_audio.c
ENGINE   = $(filter-out $(SKIP), $(wildcard $(GBA)/source/*.c))
PORTSRC  = $(PORT)/i_system_console.c $(PORT)/doom_iwad_flash.c \
           $(PORT)/i_sound_console.c
SDKSRC   = $(SDK)/startup.c $(SDK)/clock.c $(SDK)/lcd.c $(SDK)/input.c \
           $(SDK)/hostbox.c $(SDK)/audio.c $(SDK)/uart.c

SRCS    = $(ENGINE) $(PORTSRC) $(SDKSRC)
OBJS    = $(patsubst %.c,$(BUILD)/%.o,$(notdir $(SRCS)))

VPATH   = $(GBA)/source:$(PORT):$(SDK)

all: $(BUILD)/doom.bin

$(BUILD):
	mkdir -p $(BUILD)

# GBADoom's main() becomes doom_main(); our startup calls its own main()
# first to bring up the clock and peripherals.
$(BUILD)/i_main.o: i_main.c | $(BUILD)
	$(CC) $(CFLAGS) -Dmain=doom_main -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/doom.elf: $(OBJS) link_doom.ld
	$(CC) $(LDFLAGS) -T link_doom.ld $(OBJS) -o $@
	$(SIZE) $@

$(BUILD)/doom.bin: $(BUILD)/doom.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf build

.PHONY: all clean
