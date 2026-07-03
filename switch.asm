; ArcadeOS – Low-level context switch
;
; void switch_task(uint32_t* old_esp_ptr, uint32_t new_esp)
;
; Saves callee-saved registers on the current stack, stores ESP
; into *old_esp_ptr, loads new_esp, restores registers, and returns.
; The 'ret' instruction pops the return address from the new stack,
; effectively resuming the new task where it left off.

[BITS 32]

global switch_task
switch_task:
    ; Save callee-saved registers on the CURRENT stack
    push ebp
    push ebx
    push esi
    push edi

    ; Stack layout now:
    ;   [ESP+ 0] = EDI
    ;   [ESP+ 4] = ESI
    ;   [ESP+ 8] = EBX
    ;   [ESP+12] = EBP
    ;   [ESP+16] = return address
    ;   [ESP+20] = arg1: old_esp_ptr  (uint32_t*)
    ;   [ESP+24] = arg2: new_esp      (uint32_t)

    ; Save current ESP → *old_esp_ptr
    mov eax, [esp + 20]     ; eax = old_esp_ptr
    mov [eax], esp          ; *old_esp_ptr = current ESP

    ; Load the new task's stack pointer
    mov eax, [esp + 24]     ; eax = new_esp (still reading from OLD stack!)
    mov esp, eax            ; Switch stacks!

    ; Restore callee-saved registers from the NEW stack
    pop edi
    pop esi
    pop ebx
    pop ebp

    ; Return to wherever the new task left off
    ; (for a new task, this jumps to task_bootstrap)
    ret
