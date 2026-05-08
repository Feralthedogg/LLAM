; @file src/asm/windows/x86_64/context_x86_64.asm
; @brief Windows x86-64 fiber context switch assembly for MSVC/MASM builds.
;
; @details
; This path uses the Windows x64 calling convention and preserves all
; callee-saved integer registers plus XMM6-XMM15. The C runtime still owns task
; stack allocation, so Windows does not use the OS Fiber allocator here.
;
; @copyright Copyright 2026 Feralthedogg
;
; @par License
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;     http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.
;
; rcx = context to save
; rdx = context to restore

OPTION CASEMAP:NONE

EXTERN g_llam_fp_control_context:DWORD
EXTERN g_llam_xsave_mask_lo:DWORD
EXTERN g_llam_xsave_mask_hi:DWORD
EXTERN llam_task_bootstrap:PROC
EXTERN llam_fiber_alignment_violation:PROC

LLAM_WIN_CTX_SIMD_FLAGS_OFFSET EQU 260
LLAM_WIN_CTX_SIMD_F_SKIP_SAVE EQU 1
LLAM_WIN_CTX_SIMD_F_SKIP_RESTORE EQU 2

PUBLIC llam_ctx_switch
PUBLIC llam_fiber_bootstrap

.code

llam_ctx_switch PROC
    mov r10, rdx

    test DWORD PTR [rcx + LLAM_WIN_CTX_SIMD_FLAGS_OFFSET], LLAM_WIN_CTX_SIMD_F_SKIP_SAVE
    jnz saved_simd
    mov r11, QWORD PTR [rcx + 248]
    test r11, r11
    jnz save_xsave
    cmp DWORD PTR [g_llam_fp_control_context], 0
    je saved_fp
    stmxcsr DWORD PTR [rcx + 240]
    fnstcw WORD PTR [rcx + 244]
    jmp saved_fp
save_xsave:
    mov eax, DWORD PTR [g_llam_xsave_mask_lo]
    mov edx, DWORD PTR [g_llam_xsave_mask_hi]
    xsave QWORD PTR [r11]
saved_fp:
    movdqa XMMWORD PTR [rcx + 80], xmm6
    movdqa XMMWORD PTR [rcx + 96], xmm7
    movdqa XMMWORD PTR [rcx + 112], xmm8
    movdqa XMMWORD PTR [rcx + 128], xmm9
    movdqa XMMWORD PTR [rcx + 144], xmm10
    movdqa XMMWORD PTR [rcx + 160], xmm11
    movdqa XMMWORD PTR [rcx + 176], xmm12
    movdqa XMMWORD PTR [rcx + 192], xmm13
    movdqa XMMWORD PTR [rcx + 208], xmm14
    movdqa XMMWORD PTR [rcx + 224], xmm15
    mov DWORD PTR [rcx + 256], 1

saved_simd:
    mov QWORD PTR [rcx + 0], rsp
    mov QWORD PTR [rcx + 8], rbx
    mov QWORD PTR [rcx + 16], rbp
    mov QWORD PTR [rcx + 24], rsi
    mov QWORD PTR [rcx + 32], rdi
    mov QWORD PTR [rcx + 40], r12
    mov QWORD PTR [rcx + 48], r13
    mov QWORD PTR [rcx + 56], r14
    mov QWORD PTR [rcx + 64], r15

    test DWORD PTR [r10 + LLAM_WIN_CTX_SIMD_FLAGS_OFFSET], LLAM_WIN_CTX_SIMD_F_SKIP_RESTORE
    jnz restored_simd
    cmp DWORD PTR [r10 + 256], 0
    je restored_simd
    mov r11, QWORD PTR [r10 + 248]
    test r11, r11
    jnz restore_xsave
    cmp DWORD PTR [g_llam_fp_control_context], 0
    je restored_fp
    ldmxcsr DWORD PTR [r10 + 240]
    fldcw WORD PTR [r10 + 244]
    jmp restored_fp
restore_xsave:
    mov eax, DWORD PTR [g_llam_xsave_mask_lo]
    mov edx, DWORD PTR [g_llam_xsave_mask_hi]
    xrstor QWORD PTR [r11]
restored_fp:
    movdqa xmm6, XMMWORD PTR [r10 + 80]
    movdqa xmm7, XMMWORD PTR [r10 + 96]
    movdqa xmm8, XMMWORD PTR [r10 + 112]
    movdqa xmm9, XMMWORD PTR [r10 + 128]
    movdqa xmm10, XMMWORD PTR [r10 + 144]
    movdqa xmm11, XMMWORD PTR [r10 + 160]
    movdqa xmm12, XMMWORD PTR [r10 + 176]
    movdqa xmm13, XMMWORD PTR [r10 + 192]
    movdqa xmm14, XMMWORD PTR [r10 + 208]
    movdqa xmm15, XMMWORD PTR [r10 + 224]

restored_simd:
    mov rsp, QWORD PTR [r10 + 0]
    mov rbx, QWORD PTR [r10 + 8]
    mov rbp, QWORD PTR [r10 + 16]
    mov rsi, QWORD PTR [r10 + 24]
    mov rdi, QWORD PTR [r10 + 32]
    mov r12, QWORD PTR [r10 + 40]
    mov r13, QWORD PTR [r10 + 48]
    mov r14, QWORD PTR [r10 + 56]
    mov r15, QWORD PTR [r10 + 64]
    ret
llam_ctx_switch ENDP

llam_fiber_bootstrap PROC
    mov rcx, r12
    sub rsp, 40
    test rsp, 0Fh
    jnz fiber_alignment_violation
    call llam_task_bootstrap
    add rsp, 40
    ret
fiber_alignment_violation:
    mov rcx, rsp
    and rsp, -16
    sub rsp, 32
    call llam_fiber_alignment_violation
    ud2
llam_fiber_bootstrap ENDP

END
