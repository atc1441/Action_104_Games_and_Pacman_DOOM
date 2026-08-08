CROSS  ?= arm-none-eabi-
CC      = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
SIZE    = $(CROSS)size

ARCH    = -mcpu=cortex-m33 -mthumb -mfloat-abi=soft
CFLAGS  = $(ARCH) -std=c11 -O2 -g3 -ffreestanding \
          -ffunction-sections -fdata-sections -fno-common -fno-builtin \
          -Wall -Wextra -I.
LDFLAGS = $(ARCH) -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map,build/writer.map

SRCS = fw_main.c spiflash.c

all: build/writer.bin

build:
	mkdir -p build

build/writer.elf: $(SRCS) link_writer.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) -T link_writer.ld $(SRCS) -o $@
	$(SIZE) $@

build/writer.bin: build/writer.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf build

.PHONY: all clean
