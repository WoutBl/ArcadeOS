/*
 * ArcadeOS – ELF Executable Loader
 */

#include "loader.h"
#include "elf.h"
#include "fs.h"
#include "vfs.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include "scheduler.h"
#include "vga.h"
#include "syscall.h" 
#include "heap.h"

int exec(const char* filename, char* const argv[]) {
    /*
     * Resolve the binary through the VFS so games load from any mounted
     * filesystem – the FAT32 volume at /games, the RAM fs at /, etc.
     * Paths without a leading '/' fall back to the legacy root fs.
     */
    char path[MAX_PATH];
    if (filename[0] == '/') {
        strncpy(path, filename, MAX_PATH - 1);
        path[MAX_PATH - 1] = '\0';
    } else {
        path[0] = '/';
        strncpy(path + 1, filename, MAX_PATH - 2);
        path[MAX_PATH - 1] = '\0';
    }

    vfs_node_t* node = vfs_open(path, 0);
    if (!node || !(node->flags & VFS_FLAG_FILE) || node->length == 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[EXEC] Error: File '");
        terminal_writestring(path);
        terminal_writestring("' not found!\n");
        return -1;
    }

    uint8_t* file_data = (uint8_t*)kmalloc(node->length);
    if (!file_data) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[EXEC] Error: Out of memory for ELF image!\n");
        return -1;
    }
    if (vfs_read(node, 0, node->length, file_data) != (int32_t)node->length) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[EXEC] Error: Short read loading ELF!\n");
        kfree(file_data);
        return -1;
    }

    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)file_data;
    if (!elf_verify(ehdr)) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[EXEC] Error: Invalid ELF executable format!\n");
        kfree(file_data);
        return -2;
    }
    
    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
    terminal_writestring("[EXEC] Loading ELF '");
    terminal_writestring(filename);
    terminal_writestring("'...\n");

    /*
     * Run the whole mapping phase under the KERNEL PML4.
     * exec() may be called from a task still using another process's
     * address space (SYS_SPAWN), where the user regions have been
     * overlaid by that process's pages. The kernel PML4 is the only
     * context where writes to freshly allocated physical pages
     * (including the cloned PML4 itself) are guaranteed to land on
     * the right frames. current_task->cr3 is updated too so a
     * preemption mid-exec restores the right hierarchy.
     */
    uint64_t kernel_pd_phys = (uint64_t)(uintptr_t)paging_get_kernel_pd();
    if (current_task) current_task->cr3 = kernel_pd_phys;
    asm volatile("mov %0, %%cr3" :: "r"(kernel_pd_phys) : "memory");

    /* 1. Clone the kernel's PML4 for this user process.
     *    This shares kernel tables (supervisor-only) so the kernel is
     *    always visible after a Ring 3 → Ring 0 interrupt/syscall. */
    uint64_t new_pd_phys = paging_clone_kernel_pd();
    if (new_pd_phys == 0) {
        terminal_writestring("[EXEC] Error: Out of memory for PML4!\n");
        kfree(file_data);
        return -3;
    }
    uint64_t* new_pd = (uint64_t*)(uintptr_t)new_pd_phys;

    /* 4. Process ELF Program Headers */
    Elf64_Phdr* phdr = (Elf64_Phdr*)(file_data + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            uint64_t memsz  = phdr[i].p_memsz;
            uint64_t filesz = phdr[i].p_filesz;
            uint64_t vaddr  = phdr[i].p_vaddr;
            uint64_t offset = phdr[i].p_offset;

            /* Segments must stay inside the [4 MiB, 8 MiB) app window:
             * the PMM reserves exactly this physical range so user
             * overlays can never shadow kernel allocations. */
            if (vaddr < 0x400000 || vaddr + memsz > 0x800000) {
                terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
                terminal_writestring("[EXEC] Error: segment outside app window!\n");
                kfree(file_data);
                return -2;
            }

            /*
             * W^X from the ELF program header: text/rodata segments map
             * executable but read-only; data/bss segments map writable
             * but no-execute. A buffer overflow in a game can no longer
             * place code anywhere it is allowed to run.
             */
            uint64_t flags = PTE_PRESENT | PTE_USER;
            if (phdr[i].p_flags & PF_W) flags |= PTE_READ_WRITE;
            if (!(phdr[i].p_flags & PF_X)) flags |= paging_nx_flag();

            /* Calculate page boundaries */
            uint64_t num_pages = (memsz + (vaddr & 0xFFF) + 4095) / 4096;
            uint64_t page_vaddr = vaddr & ~(uint64_t)0xFFF;

            for (uint64_t p = 0; p < num_pages; p++) {
                uint64_t phys = pmm_alloc_page();
                paging_map_page_to(new_pd, page_vaddr + p * 4096, phys, flags);

                /* Zero out memory */
                memset((void*)(uintptr_t)phys, 0, 4096);

                uint64_t current_vaddr = page_vaddr + p * 4096;
                uint64_t copy_start = 0;
                uint64_t copy_amount = 0;

                if (current_vaddr <= vaddr && current_vaddr + 4096 > vaddr) {
                    copy_start = vaddr - current_vaddr;
                    uint64_t avail = 4096 - copy_start;
                    copy_amount = (filesz < avail) ? filesz : avail;
                    memcpy((void*)(uintptr_t)(phys + copy_start), file_data + offset, copy_amount);
                } else if (current_vaddr > vaddr && current_vaddr < vaddr + filesz) {
                    uint64_t relative_offset = current_vaddr - vaddr;
                    uint64_t avail = filesz - relative_offset;
                    copy_amount = (avail > 4096) ? 4096 : avail;
                    memcpy((void*)(uintptr_t)phys, file_data + offset + relative_offset, copy_amount);
                }
            }
        }
    }

    /* The segments are copied; release the ELF file image */
    uint64_t entry_point = ehdr->e_entry;
    kfree(file_data);

    /* 5. Set up a 16 KiB User Stack ending at 0xC0000000 */
    uint64_t stack_pages = 4;
    uint64_t stack_vaddr = 0xC0000000ULL - stack_pages * 4096;
    for (uint64_t sp = 0; sp < stack_pages; sp++) {
        uint64_t stack_phys = pmm_alloc_page();
        memset((void*)(uintptr_t)stack_phys, 0, 4096);
        paging_map_page_to(new_pd, stack_vaddr + sp * 4096, stack_phys,
                           PTE_PRESENT | PTE_READ_WRITE | PTE_USER
                           | paging_nx_flag());
    }

    /* 6. Update current running task with isolated PML4 */
    if (current_task) {
        current_task->cr3 = new_pd_phys;
    }

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_writestring("[EXEC] Jumping to Ring 3 (RIP: 0x");
    terminal_writehex(entry_point);
    terminal_writestring(")...\n");

    /* 7. Wire FD 0/1/2 → /dev/tty for this process *before* dropping to Ring 3 */
    task_open_std_fds(current_task);

    /* 8. Switch MMU Context */
    asm volatile("mov %0, %%cr3" :: "r"(new_pd_phys) : "memory");

    /* 9. Copy argv strings + pointer array onto the user stack */
    uint64_t user_esp = 0xC0000000ULL;

    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }

    uint64_t argv_ptrs[32];
    for (int i = 0; i < argc; i++) {
        int len = strlen(argv[i]) + 1;
        user_esp -= len;
        memcpy((void*)(uintptr_t)user_esp, argv[i], len);
        argv_ptrs[i] = user_esp;
    }
    argv_ptrs[argc] = 0;

    user_esp &= ~(uint64_t)7; /* align to 8 bytes */

    int ptrs_size = (argc + 1) * sizeof(uint64_t);
    user_esp -= ptrs_size;
    memcpy((void*)(uintptr_t)user_esp, argv_ptrs, ptrs_size);
    uint64_t argv_array_ptr = user_esp;

    /* SysV: RSP ≡ 8 (mod 16) at function entry (as if after a call) */
    user_esp &= ~(uint64_t)15;
    user_esp -= 8;
    *(uint64_t*)(uintptr_t)user_esp = 0;   /* dummy return address */

    /* 10. Use iretq trick to drop privileges; argc/argv go in RDI/RSI
     *     (64-bit SysV passes arguments in registers, not on the stack) */
    enter_user_mode(entry_point, user_esp, (uint64_t)argc, argv_array_ptr);

    return 0;
}

static void task_run_app(void) {
    if (!current_task) {
        while (1) { schedule(); asm volatile("sti\nhlt"); }
    }
    
    int ret = exec(current_task->name, current_task->cmdline_args);
    if (ret != 0) {
        terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK));
        terminal_writestring("[KERNEL] Failed to load ELF. Error: ");
        terminal_writedec(ret);
        terminal_writestring("\n");
        current_task->state = TASK_DEAD;
        
        for (int i = 0; i < num_tasks; i++) {
            if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_pid == current_task->id) {
                tasks[i].state = TASK_READY;
                tasks[i].wait_pid = 0;
            }
        }
    }
    while (1) {
        schedule();
        asm volatile("sti\nhlt");
    }
}

int launch_user_app(const char* filename, char* const argv[]) {
    int idx = create_kernel_thread(task_run_app, filename);
    if (idx >= 0) {
        if (argv) {
            int argc = 0;
            while (argv[argc]) argc++;
            tasks[idx].cmdline_args = (char**)kmalloc((argc + 1) * sizeof(char*));
            for (int i = 0; i < argc; i++) {
                int len = strlen(argv[i]);
                tasks[idx].cmdline_args[i] = (char*)kmalloc(len + 1);
                strcpy(tasks[idx].cmdline_args[i], argv[i]);
            }
            tasks[idx].cmdline_args[argc] = (char*)0;
        } else {
            tasks[idx].cmdline_args = (char**)0;
        }
        return tasks[idx].id;
    }
    return -1;
}
