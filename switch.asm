; ArcadeOS – Low-level context switch (x86-64)
;
; void switch_task(uint64_t* old_rsp_ptr, uint64_t new_rsp)
;   RDI = old_rsp_ptr, RSI = new_rsp
;
; Saves callee-saved registers on the current stack, stores RSP
; into *old_rsp_ptr, loads new_rsp, restores registers, and returns.
; The 'ret' instruction pops the return address from the new stack,
; effectively resuming the new task where it left off.

BITS 64

global switch_task
switch_task:
    ; Save callee-saved registers on the CURRENT stack
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save current RSP → *old_rsp_ptr
    mov [rdi], rsp

    ; Switch stacks
    mov rsp, rsi

    ; Restore callee-saved registers from the NEW stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Return to wherever the new task left off
    ; (for a new task, this jumps to task_bootstrap)
    ret
