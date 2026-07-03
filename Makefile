# ArcadeOS Build System
#
# Targets:
#   make          - kernel + ISO + FAT32 game volume + all game ELFs
#   make run      - boot in QEMU (GUI window)
#   make run-headless - boot in QEMU with serial log + monitor socket
#   make clean    - remove build artifacts

# Compiler and assembler settings
AS = nasm
CC = i686-elf-gcc
LD = i686-elf-ld
AR = i686-elf-ar

# Flags
ASFLAGS = -f elf32
CFLAGS = -c -ffreestanding -O2 -Wall -Wextra -Iinclude
LDFLAGS = -T linker.ld

# Output files
KERNEL = arcadeos.bin
ISO = arcadeos.iso
DISK = arcadeos.img
DISK_MB = 64

# Build directory
BUILD = build

# Kernel source files
C_SOURCES = src/kernel.c src/vga.c src/serial.c src/fb.c src/console_gfx.c \
            src/keyboard.c src/gamepad.c src/pci.c src/usb.c src/uhci.c src/xhci.c \
            src/ata.c src/fat32.c src/fs.c src/clock.c src/heap.c src/idt.c \
            src/string.c src/pmm.c src/paging.c src/task.c src/scheduler.c \
            src/syscall.c src/loader.c src/elf.c src/vfs.c src/devfs.c src/pipe.c

# Object files
C_OBJECTS = $(patsubst src/%.c,$(BUILD)/%.o,$(C_SOURCES))
ASM_OBJECTS = $(BUILD)/boot.o $(BUILD)/isr.o $(BUILD)/switch.o
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS)

# User-space libc
LIBC_SOURCES = libc/syscall.c libc/stdio.c libc/string.c libc/console.c
LIBC_OBJECTS = $(patsubst libc/%.c,$(BUILD)/libc_%.o,$(LIBC_SOURCES))

# Games / apps shipped on the FAT32 volume
APPS = $(BUILD)/launcher.elf $(BUILD)/pong.elf $(BUILD)/snake.elf $(BUILD)/breakout.elf

# User app link flags: entry = main (no crt0), fixed base at 4 MiB
APP_LDFLAGS = -Wl,-N,-Ttext=0x400000,--build-id=none,-e,main

# Default target
all: $(ISO) $(DISK)

# Create build directory
$(BUILD):
	mkdir -p $(BUILD)

# Build the kernel binary
$(KERNEL): $(BUILD) $(OBJECTS)
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJECTS)

# Assemble boot.asm
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
	$(CC) -m32 -c -ffreestanding -O2 -Wall -Wextra $< -o $@

# Create Libc Archive (static library)
$(BUILD)/libc.a: $(LIBC_OBJECTS)
	$(AR) rcs $@ $(LIBC_OBJECTS)

# Compile Ring 3 User Apps
$(BUILD)/%.elf: apps/%.c $(BUILD)/libc.a | $(BUILD)
	$(CC) -m32 -Os -s -ffreestanding -nostdlib -fno-builtin $< $(BUILD)/libc.a -o $@ $(APP_LDFLAGS)

# Create bootable ISO
$(ISO): $(KERNEL)
	mkdir -p isodir/boot/grub
	cp $(KERNEL) isodir/boot/$(KERNEL)
	printf 'set timeout=0\nset default=0\nmenuentry "ArcadeOS" {\n    multiboot /boot/$(KERNEL)\n    boot\n}\n' > isodir/boot/grub/grub.cfg
	i686-elf-grub-mkrescue -o $(ISO) isodir

# Create the FAT32 game volume with the launcher and games
$(DISK): $(APPS) tools/mkfat32.py
	python3 tools/mkfat32.py $(DISK) $(DISK_MB) $(APPS)

# Run in QEMU with a visible window
run: $(ISO) $(DISK)
	qemu-system-i386 -m 128 \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(ISO),format=raw,if=ide,index=1,media=cdrom \
		-boot d -usb \
		-serial file:serial.log \
		-no-reboot

# Run with a real DualShock 4 passed through from the host (micro-USB).
# NEEDS ROOT on macOS: the Apple HID kernel driver owns the controller's
# interface, and only a privileged process can seize it (libusb
# USBInterfaceOpenSeize). Run:  sudo make run-ds4
# While the VM runs, macOS loses the controller; unplug/replug returns it.
run-ds4: $(ISO) $(DISK)
	qemu-system-i386 -m 128 \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(ISO),format=raw,if=ide,index=1,media=cdrom \
		-boot d -usb \
		-device usb-host,vendorid=0x054c,productid=0x09cc \
		-serial file:serial.log \
		-no-reboot

# Run headless: serial log + QEMU monitor socket (for screendump/sendkey)
run-headless: $(ISO) $(DISK)
	qemu-system-i386 -m 128 \
		-drive file=$(DISK),format=raw,if=ide,index=0,media=disk \
		-drive file=$(ISO),format=raw,if=ide,index=1,media=cdrom \
		-boot d -usb \
		-serial file:serial.log \
		-display none \
		-monitor unix:qemu-monitor.sock,server,nowait \
		-no-reboot

# Clean build files
clean:
	rm -f $(OBJECTS) $(KERNEL) $(ISO) serial.log qemu-monitor.sock
	rm -rf $(BUILD) isodir

# Clean everything including the game volume
clean-all: clean
	rm -f $(DISK)

# Phony targets
.PHONY: all run run-ds4 run-headless clean clean-all
