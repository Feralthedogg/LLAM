# Context switching

How a task switches, what each platform saves, and what asynchronous capture adds.

## Cooperative switches

Cooperative switches use hand-written assembly and the platform ABI's nonvolatile
state.

| Platform | Saved registers | Source |
| --- | --- | --- |
| Linux x86_64 | `rbx, rbp, r12-r15`, `rsp` | `linux_context_x86_64.S` |
| Linux aarch64 | `x19-x30, sp`, `d8-d15`, FP control and status | `linux_context_arm64.S` |
| macOS x86_64 | `rbx, rbp, r12-r15`, `rsp` | `darwin_context_x86_64.S` |
| macOS arm64 | `x19-x30, sp`, `d8-d15`, FP control and status | `darwin_context_arm64.S` |
| Windows x86_64 | Win64 nonvolatile GPRs, `XMM6-XMM15`, stack and control state | `context_x86_64.asm` (MASM), `windows_context_x86_64.S` (GNU) |

On x86_64, the fast path is approximately:

```asm
mov [rdi], rsp        ; save current SP
mov rsp, [rsi]        ; restore target SP
ret                   ; jump to target's saved return address
```

The assembly transfer itself makes no kernel call. End-to-end cost also includes
scheduling, validation, and state handling, and must be measured on the target host.

## Floating-point state

x86_64 builds preserve x87 and SSE control state across cooperative switches unless
`LLAM_FP_CONTROL_CONTEXT=0` is set. Linux x86_64 can also save extended state with
XSAVE: `LLAM_XSAVE_CONTEXT=1` detects support and tracks its mask and area size. It
is off by default because it raises per-context allocation and switch cost, and
macOS and Windows do not use it. ARM64 contexts preserve FPCR, FPSR, and the
ABI-required nonvolatile SIMD state.

## Asynchronous capture

Asynchronous interruption has a separate full-state capture and restore contract. The
cooperative register set alone cannot restore an arbitrary interrupted instruction.
Native capture and restore use the qualified platform's kernel transport and preserve
the complete admitted state. Native profiles qualify the additional full-state
requirements before admitting capture. See [Platform support](../../start/platforms.md)
and the [managed C guide](../../guides/managed-c.md).
