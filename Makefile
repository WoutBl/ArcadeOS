# ArcadeOS Build System
#
# Targets:
#   make          - bootloader + kernel + bootable FAT32 game volume + games
#   make run      - boot in QEMU (GUI window)
#   make run-headless - boot in QEMU with serial log + monitor socket
#   make clean    - remove build artifacts
#
# The disk image is self-booting: our own two-stage BIOS bootloader
# (boot/stage1.asm + boot/stage2.asm) lives in the FAT32 reserved sectors
# and loads the kernel flat binary to 1 MiB. No GRUB, no ISO.

# Compiler and assembler settings
AS = nasm
CC = x86_64-elf-gcc
LD = x86_64-elf-ld
AR = x86_64-elf-ar
OBJCOPY = x86_64-elf-objcopy

# Flags
# Kernel: no red zone (ISRs run on the interrupted stack) and no SSE/MMX
# (context switches don't save FPU state; GCC must not emit vector code)
ASFLAGS = -f elf64
NOFPU = -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -msoft-float
CFLAGS = -c -ffreestanding -O2 -Wall -Wextra -Iinclude $(NOFPU)
LDFLAGS = -T linker.ld

# Output files
KERNEL_ELF = build/arcadeos.elf
KERNEL = arcadeos.bin
DISK = arcadeos.img
DISK_MB = 64

# Build directory
BUILD = build

# Bootloader stages (raw binaries)
STAGE1 = $(BUILD)/stage1.bin
STAGE2 = $(BUILD)/stage2.bin

# Kernel source files
C_SOURCES = src/kernel.c src/vga.c src/serial.c src/fb.c src/console_gfx.c \
            src/keyboard.c src/gamepad.c src/pci.c src/usb.c src/uhci.c src/xhci.c \
            src/ata.c src/ahci.c src/disk.c src/klog.c src/audio.c src/ac97.c src/pcspk.c src/fat32.c src/fs.c src/clock.c src/heap.c src/idt.c \
            src/string.c src/pmm.c src/paging.c src/task.c src/scheduler.c \
            src/syscall.c src/loader.c src/elf.c src/vfs.c src/devfs.c src/pipe.c

# Object files
C_OBJECTS = $(patsubst src/%.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS = $(BUILD)/boot.o $(BUILD)/isr.o $(BUILD)/switch.o
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# User-space libc
LIBC_SOURCES = libc/syscall.c libc/stdio.c libc/string.c libc/console.c
LIBC_OBJECTS = $(patsubst libc/%.c,$(BUILD)/libc_%.o,$(LIBC_SOURCES))

# Game SDK (libarcade = SDK framework + libc in one archive)
SDK_OBJECTS = $(BUILD)/sdk_arcade.o

# Games / apps shipped on the FAT32 volume
APPS = $(BUILD)/launcher.elf $(BUILD)/pong.elf $(BUILD)/snake.elf $(BUILD)/breakout.elf $(BUILD)/starcatch.elf

# User app link flags: entry = main (no crt0), fixed base at 4 MiB
APP_LDFLAGS = -Wl,-N,-Ttext=0x400000,--build-id=none,-e,main

# Default target
all: $(DISK)

# Create build directory
$(BUILD):
	mkdir -p $(BUILD)

# Link the kernel ELF, then flatten it (the bootloader jumps to byte 0)
$(KERNEL_ELF): $(BUILD) $(OBJECTS)
	$(LD) $(LDFLAGS) -o $(KERNEL_ELF) $(OBJECTS)

$(KERNEL): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $(KERNEL_ELF) $(KERNEL)

# Bootloader stages (flat real-mode binaries)
$(STAGE1): boot/stage1.asm | $(BUILD)
	$(AS) -f bin boot/stage1.asm -o $(STAGE1)

$(STAGE2): boot/stage2.asm | $(BUILD)
	$(AS) -f bin boot/stage2.asm -o $(STAGE2)

# Assemble boot.asm (kernel entry stub)
$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(AS) $(ASFLAGS) boot.asm -o $(BUILD)/boot.o

# Assemble isr.asm
$(BUILD)/isr.o: isr.asm | $(BUILD)
	$(AS) $(ASFLAGS) isr.asm -o $(BUILD)/isr.o

# Assemble switch.asm (context switch)
$(BUILD)/switch.o: switch.asm | $(BUILD)
	$(AS) $(ASFLAGS) switch.asm -o $(BUILD)/switch.o

# Compile Kernel C source files
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# Compile Libc C source files
$(BUILD)/libc_%.o: libc/%.c | $(BUILD)
	$(CC) -c -ffreestanding -O2 -Wall -Wextra $(NOFPU) $< -o $@

# Compile the SDK
$(BUILD)/sdk_arcade.o: sdk/arcade.c sdk/arcade.h | $(BUILD)
	$(CC) -c -Os -ffreestanding -Wall -Wextra $(NOFPU) sdk/arcade.c -o $@

# Create the game SDK archive (framework + libc)
$(BUILD)/libarcade.a: $(SDK_OBJECTS) $(LIBC_OBJECTS)
	$(AR) rcs $@ $(SDK_OBJECTS) $(LIBC_OBJECTS)

# Compile Ring 3 User Apps
$(BUILD)/%.elf: apps/%.c $(BUILD)/libarcade.a | $(BUILD)
	$(CC) -Os -s -ffreestanding -nostdlib -fno-builtin $(NOFPU) $< $(BUILD)/libarcade.a -o $@ $(APP_LDFLAGS)

# Create the bootable FAT32 game volume: bootloader + kernel in the
# reserved sectors, launcher + games in the root directory
$(DISK): $(STAGE1) $(STAGE2) $(KERNEL) $(APPS) tools/mkfat32.py
	python3 tools/mkfat32.py $(DISK) $(DISK_MB) $(STAGE1) $(STAGE2) $(KERNEL) $(APPS)

# Run in QEMU with a visible window
run: $(DISK)
	qemu-system-x86_64 -m 128 \
		-audiodev coreaudio,id=snd0 \
		-device AC97,audiodev=snd0 \
		-machine pcspk-audiodev=snd0 \
		-drive file=$(DISK),format=raw,if=none,id=gamedisk \
		-device ahci,id=ahci0 \
		-device ide-hd,drive=gamedisk,bus=ahci0.0 \
		-boot c -usb \
		-serial file:serial.log \
		-no-reboot

# Run with a real DualShock 4 passed through from the host (micro-USB).
# NEEDS ROOT on macOS: the Apple HID kernel driver owns the controller's
# interface, and only a privileged process can seize it (libusb
# USBInterfaceOpenSeize). Run:  sudo make run-ds4
# While the VM runs, macOS loses the controller; unplug/replug returns it.
run-ds4: $(DISK)
	qemu-system-x86_64 -m 128 \
		-audiodev coreaudio,id=snd0 \
		-device AC97,audiodev=snd0 \
		-machine pcspk-audiodev=snd0 \
		-drive file=$(DISK),format=raw,if=none,id=gamedisk \
		-device ahci,id=ahci0 \
		-device ide-hd,drive=gamedisk,bus=ahci0.0 \
		-boot c -usb \
		-device usb-host,vendorid=0x054c,productid=0x09cc \
		-serial file:serial.log \
		-no-reboot

# Run headless: serial log + QEMU monitor socket (for screendump/sendkey)
run-headless: $(DISK)
	qemu-system-x86_64 -m 128 \
		-audiodev wav,id=snd0,path=audio-out.wav \
		-device AC97,audiodev=snd0 \
		-machine pcspk-audiodev=snd0 \
		-drive file=$(DISK),format=raw,if=none,id=gamedisk \
		-device ahci,id=ahci0 \
		-device ide-hd,drive=gamedisk,bus=ahci0.0 \
		-boot c -usb \
		-serial file:serial.log \
		-display none \
		-monitor unix:qemu-monitor.sock,server,nowait \
		-no-reboot

# Legacy IDE attach: exercises the ATA PIO fallback path
run-ide: $(DISK)
	qemu-system-x86_64 -m 128 \
		-audiodev wav,id=snd0,path=audio-out.wav \
		-machine pcspk-audiodev=snd0 \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-boot c -usb \
		-serial file:serial.log \
		-display none \
		-monitor unix:qemu-monitor.sock,server,nowait \
		-no-reboot

# Clean build files
clean:
	rm -f $(OBJECTS) $(KERNEL) $(KERNEL_ELF) serial.log qemu-monitor.sock audio-out.wav arcadeos.iso
	rm -rf $(BUILD) isodir

# Clean everything including the game volume
clean-all: clean
	rm -f $(DISK)

# Phony targets
.PHONY: all run run-ds4 run-headless run-ide clean clean-all
