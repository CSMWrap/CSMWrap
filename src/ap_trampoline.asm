; AP Trampoline for BIOS Proxy Helper Core
; This code is copied to 0x7000 and runs when an AP wakes from SIPI
; AP starts at CS:IP = 0x0700:0x0000 = linear 0x7000
;
; This trampoline stays in 16-bit real mode and jumps to the SeaBIOS
; 16-bit entry point, which handles the GDT load and 32-bit mode switch.

bits 16

section .rodata

; APIC MSR
%define MSR_IA32_APIC_BASE          0x1B
%define APIC_BASE_EXTD              (1 << 10)
%define APIC_BASE_EN                (1 << 11)

; AMD MTRR MSR addresses
%define MSR_SYS_CFG                 0xC0010010
%define SYS_CFG_MTRR_FIX_DRAM_EN    (1 << 18)
%define SYS_CFG_MTRR_FIX_DRAM_MOD_EN (1 << 19)
; Fixed MTRRs for conventional memory (0x00000-0x9FFFF)
%define AMD_MTRR_FIX64k_00000       0x250   ; 0x00000-0x7FFFF (512KB, 8x64KB)
%define AMD_MTRR_FIX16k_80000       0x258   ; 0x80000-0x9FFFF (128KB, 8x16KB)
%define AMD_MTRR_FIX16k_A0000       0x259   ; 0xA0000-0xBFFFF (VGA, 128KB, 8x16KB)
; Fixed MTRRs for BIOS region (0xC0000-0xFFFFF)
%define AMD_MTRR_FIX4k_C0000        0x268
%define AMD_MTRR_FIX4k_C8000        0x269
%define AMD_MTRR_FIX4k_D0000        0x26A
%define AMD_MTRR_FIX4k_D8000        0x26B
%define AMD_MTRR_FIX4k_E0000        0x26C
%define AMD_MTRR_FIX4k_E8000        0x26D
%define AMD_MTRR_FIX4k_F0000        0x26E
%define AMD_MTRR_FIX4k_F8000        0x26F
; WB_DRAM = 0x1E per segment (8 segments per MSR = 0x1E1E1E1E1E1E1E1E)
%define MTRR_WB_DRAM_LO             0x1E1E1E1E
%define MTRR_WB_DRAM_HI             0x1E1E1E1E

global ap_trampoline_start
ap_trampoline_start:
    cli
    cld

    ; Set up DS=0 so we can read from trampoline data area
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    jmp 0x0000:word (0x7000 + .reload_cs - ap_trampoline_start)
.reload_cs:

    ; ========================================
    ; AMD MTRR unlock for low memory (00000-FFFFF)
    ; Sets conventional memory and BIOS region to WB_DRAM
    ; Required for helper core to access EBDA and F-segment
    ; Skipped on Intel (PAM registers are global, already unlocked by BSP)
    ; Skipped if region is already writable (e.g., under hypervisors)
    ; ========================================

    ; First test if the BIOS region is already writable
    ; Try writing a test pattern to 0xF0000 and reading it back
    ; Use segmentation: 0xF000:0x0000 = linear 0xF0000
    ; Save and restore original value to avoid corrupting CSM code
    mov ax, 0xF000
    mov fs, ax
    mov eax, [fs:0]           ; save original value
    mov ebx, eax
    xor eax, 0xFFFFFFFF       ; flip all bits to create test pattern
    mov [fs:0], eax           ; try to write
    cmp [fs:0], eax           ; did the write succeed?
    mov [fs:0], ebx           ; restore original (regardless of result)
    je .skip_mtrr_unlock      ; Region already writable, skip MTRR unlock

    ; Region not writable - check if this is AMD
    ; On Intel, PAM registers are global so BSP unlock applies to all cores
    ; If we get here on Intel, something is wrong - halt
    mov eax, 0
    cpuid
    cmp ebx, 0x68747541      ; "Auth" (AuthenticAMD)
    jne .unlock_failed        ; Not AMD and not writable - halt

    ; AMD system with locked region - attempt MTRR unlock
    ; Enable MTRR modification: set SYS_CFG.MtrrFixDramModEn (bit 19)
    mov ecx, MSR_SYS_CFG
    rdmsr
    or eax, SYS_CFG_MTRR_FIX_DRAM_MOD_EN
    wrmsr

    ; Set conventional memory (00000-9FFFF) to WB_DRAM
    mov eax, MTRR_WB_DRAM_LO
    mov edx, MTRR_WB_DRAM_HI
    mov ecx, AMD_MTRR_FIX64k_00000      ; 0x00000-0x7FFFF
    wrmsr
    mov ecx, AMD_MTRR_FIX16k_80000      ; 0x80000-0x9FFFF (includes EBDA)
    wrmsr

    ; Set VGA region (A0000-BFFFF) to UC (MMIO)
    xor eax, eax
    xor edx, edx
    mov ecx, AMD_MTRR_FIX16k_A0000
    wrmsr

    ; Set BIOS region (C0000-FFFFF) to WB_DRAM
    mov eax, MTRR_WB_DRAM_LO
    mov edx, MTRR_WB_DRAM_HI
    mov ecx, AMD_MTRR_FIX4k_C0000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_C8000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_D0000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_D8000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_E0000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_E8000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_F0000
    wrmsr
    mov ecx, AMD_MTRR_FIX4k_F8000
    wrmsr

    ; Disable modification, enable fixed MTRR DRAM attributes
    mov ecx, MSR_SYS_CFG
    rdmsr
    and eax, ~SYS_CFG_MTRR_FIX_DRAM_MOD_EN
    or eax, SYS_CFG_MTRR_FIX_DRAM_EN
    wrmsr

    ; Verify the unlock worked by testing write again
    ; FS still points to 0xF000 from earlier
    ; Save and restore original value to avoid corrupting CSM code
    mov eax, [fs:0]           ; save original value
    mov ebx, eax
    xor eax, 0xFFFFFFFF       ; flip all bits to create test pattern
    mov [fs:0], eax           ; try to write
    cmp [fs:0], eax           ; did the write succeed?
    mov [fs:0], ebx           ; restore original (regardless of result)
    jne .unlock_failed        ; MTRR unlock didn't help - halt

.skip_mtrr_unlock:
    ; Continue to load registers and jump to SeaBIOS
    jmp .continue_boot

.unlock_failed:
    ; Region still not writable - halt so BSP detects timeout
    hlt
    jmp .unlock_failed

.continue_boot:

    ; Hardware-disable LAPIC to prevent the legacy OS from sending IPIs
    ; (including INIT) to this core. The helper core communicates via
    ; memory mailbox only and does not need interrupt delivery.
    ; Must clear both EN (bit 11) and EXTD (bit 10) simultaneously
    ; to correctly transition from x2APIC mode to disabled state.
    ;
    ; If x2APIC is active (EXTD set), skip the disable — it may be locked
    ; (e.g. Nova Lake xAPIC deprecation) and WRMSR would #GP. A legacy OS
    ; cannot send x2APIC-mode IPIs via MMIO, so leaving the APIC enabled
    ; in x2APIC mode is safe for the helper core.
    mov ecx, MSR_IA32_APIC_BASE
    rdmsr
    test eax, APIC_BASE_EXTD
    jnz .skip_apic_disable
    and eax, ~(APIC_BASE_EN | APIC_BASE_EXTD)
    wrmsr
.skip_apic_disable:

    ; Load 32-bit values into registers for SeaBIOS
    ; (16-bit mode can still use 32-bit registers with operand size prefix)
    mov ebx, [0x7000 + trampoline_mailbox - ap_trampoline_start]
    mov esp, [0x7000 + trampoline_stack - ap_trampoline_start]
    mov esi, (0x7000 + trampoline_helper_ready - ap_trampoline_start)

    ; Far jump to SeaBIOS 16-bit entry point (segment:offset)
    jmp far [0x7000 + trampoline_target16 - ap_trampoline_start]

; --- Data area (filled in by C code) ---

align 4
trampoline_mailbox:
    dd 0

trampoline_stack:
    dd 0

; Far pointer for 16-bit jump: offset (16-bit) then segment (16-bit)
trampoline_target16:
    dw 0        ; offset
    dw 0        ; segment

trampoline_helper_ready:
    dd 0

ap_trampoline_end:

global ap_trampoline_size
ap_trampoline_size: equ (ap_trampoline_end - ap_trampoline_start)

; Export size and offsets for C code
global ap_trampoline_size_value
ap_trampoline_size_value: dd ap_trampoline_size

global ap_trampoline_mailbox_offset
ap_trampoline_mailbox_offset: dd (trampoline_mailbox - ap_trampoline_start)

global ap_trampoline_stack_offset
ap_trampoline_stack_offset: dd (trampoline_stack - ap_trampoline_start)

global ap_trampoline_target16_offset
ap_trampoline_target16_offset: dd (trampoline_target16 - ap_trampoline_start)

global ap_trampoline_helper_ready_offset
ap_trampoline_helper_ready_offset: dd (trampoline_helper_ready - ap_trampoline_start)

section .note.GNU-stack noalloc noexec nowrite progbits
