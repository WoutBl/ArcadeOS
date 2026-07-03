/*
 * ArcadeOS – Kernel Entry Point
 *
 * Boot flow for the gaming console:
 *
 *   1. Serial + framebuffer + boot console
 *   2. GDT / IDT / PIT / keyboard, then interrupts on
 *   3. PMM → heap → paging (framebuffer identity-mapped)
 *   4. Graphics double buffer + boot splash
 *   5. VFS, devfs, RAM fs
 *   6. Scheduler, TSS, syscalls
 *   7. PCI → USB host controllers → gamepad stack
 *   8. ATA → FAT32 game volume mounted at /games
 *   9. Launch the user-space launcher (Ring 3 init process)
 */

#include "types.h"
#include "multiboot.h"
#include "serial.h"
#include "fb.h"
#include "console_gfx.h"
#include "vga.h"
#include "idt.h"
#include "keyboard.h"
#include "clock.h"
#include "pmm.h"
#include "heap.h"
#include "paging.h"
#include "task.h"
#include "scheduler.h"
#include "syscall.h"
#include "ata.h"
#include "fs.h"
#include "fat32.h"
#include "pci.h"
#include "usb.h"
#include "gamepad.h"
#include "loader.h"
#include "vfs.h"
#include "devfs.h"

/* ──────── Idle Task ──────── */
/*
 * The idle task is a permanent, unkillable kernel thread that halts the
 * CPU until the next interrupt fires.  It gives the scheduler a safe
 * fall-back when ALL user processes are blocked.
 */
static void idle_task(void) {
    while (1) {
        asm volatile("sti\nhlt"); /* Enable IRQs, then sleep */
    }
}

/* ──────── Boot splash ──────── */
static void boot_splash(void) {
    if (!gfx_ready()) return;

    int w = (int)fb_width();
    int h = (int)fb_height();

    gfx_clear(gfx_rgb(8, 10, 30));

    /* Accent bars */
    gfx_fill_rect(0, h / 2 - 60, w, 4, gfx_rgb(80, 120, 255));
    gfx_fill_rect(0, h / 2 + 44, w, 4, gfx_rgb(80, 120, 255));

    /* Title */
    const char* title = "ARCADE OS";
    int scale = 4;
    int tw = (int)strlen(title) * 8 * scale;
    gfx_draw_text((w - tw) / 2, h / 2 - 32, title,
                  gfx_rgb(255, 255, 255), GFX_TRANSPARENT, scale);

    const char* sub = "GAMING CONSOLE  v" OS_VERSION;
    int sw = (int)strlen(sub) * 8;
    gfx_draw_text((w - sw) / 2, h / 2 + 16, sub,
                  gfx_rgb(140, 160, 220), GFX_TRANSPARENT, 1);

    gfx_present();
}

void kernel_main(uint32_t magic, multiboot_info_t* mboot_info) {
    /* 1. Serial first so every boot message reaches the debug log */
    serial_init();
    serial_write("\n[BOOT] ArcadeOS starting...\n");

    /* 2. Framebuffer discovery must precede the console: terminal
     *    output renders through it when available */
    fb_init(mboot_info);

    /* 3. Initialize the boot console and virtual terminals */
    terminal_initialize();
    vterm_init_all();
    terminal_enable_cursor(0, 15);
    terminal_update_cursor();

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("ArcadeOS v" OS_VERSION " - console kernel booting\n");

    if (fb_available()) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
        terminal_writestring("[FB] Linear framebuffer: ");
        terminal_writedec(fb_width());
        terminal_writestring("x");
        terminal_writedec(fb_height());
        terminal_writestring("x");
        terminal_writedec(fb_bpp());
        terminal_writestring(" @ 0x");
        terminal_writehex(fb_phys_addr());
        terminal_writestring("\n");
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK));
        terminal_writestring("[FB] No framebuffer from bootloader - VGA text fallback\n");
    }

    /* 4. Set up GDT with known selectors (0x08=code, 0x10=data) */
    gdt_init();

    /* 5. Set up IDT (remaps PIC, installs ISR gates) */
    idt_init();

    /* 6. Configure PIT at 1000 Hz and read RTC */
    clock_init();

    /* 7. Initialize keyboard (registers IRQ1 handler) */
    keyboard_init();

    /* 8. Enable hardware interrupts */
    sti();

    /* 9. Verify boot magic (own bootloader, or GRUB if legacy-booted) */
    if (magic != ARCADEBOOT_MAGIC && magic != MULTIBOOT_MAGIC) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[BOOT] WARNING: Invalid boot magic! (0x");
        terminal_writehex(magic);
        terminal_writestring(")\n");
    }

    /* 10. Physical Memory Manager (from multiboot memory map) */
    pmm_init(mboot_info);

    /* 11. Heap allocator (backed by PMM pages) */
    heap_init();

    /* 12. Paging: identity-map RAM + the framebuffer MMIO region */
    paging_init();

    /* 13. Controller & input stack: PCI scan → USB HCs → gamepads.
     *     Runs BEFORE gfx_init so the UHCI schedule/DMA pages come from
     *     low physical memory (always identity-mapped in every address
     *     space – they are touched from syscall context). */
    pci_init();
    usb_init();
    gamepad_init();

    /* 14. Graphics double buffer + splash screen */
    gfx_init();
    boot_splash();

    /* 15. VFS + devfs + RAM filesystem at / */
    vfs_init();
    devfs_init();
    fs_initialize();
    fs_create_file("welcome.txt", "Welcome to ArcadeOS!");
    vfs_mount("/", fs_as_vfs_root());

    /* 15. Scheduler (creates task 0 for the current context) */
    scheduler_init();

    /* 16. Task State Segment and system calls */
    uint64_t kernel_stack_top = (uint64_t)(uintptr_t)kmalloc(8192) + 8192;
    tss_init(kernel_stack_top);
    syscall_init();

    /* 16. Storage: ATA disk → FAT32 game volume at /games */
    if (ata_init()) {
        if (fat32_init()) {
            vfs_mount("/games", fat32_get_root());
        }
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[DISK] No storage detected - no game volume\n");
    }

    /* 19. Register the Idle Task so the scheduler always has a fallback */
    create_kernel_thread(idle_task, "idle");

    /* 20. Boot into the user-space launcher (the console's "home screen") */
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[BOOT] Starting launcher (Ring 3)...\n");
    serial_write("[BOOT] Kernel init complete\n");

    if (fat32_available()) {
        char* init_argv[] = { "/games/LAUNCHER.ELF", (char*)0 };
        launch_user_app("/games/LAUNCHER.ELF", init_argv);
    } else {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[BOOT] No game volume - halting in boot console\n");
    }

    /* Yield the CPU indefinitely */
    while (1) {
        schedule();
        asm volatile("sti\nhlt");
    }
}
