/**
 * @file src/core/runtime_fault.c
 * @brief Fault handling, state dump triggers, and fatal diagnostic paths.
 *
 * @details
 * Signal handlers in this file avoid allocation and stdio. Guard-page overflow
 * reporting uses a tiny append-only buffer and best-effort writes so diagnostics
 * can still be emitted when the task stack is corrupt.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "runtime_internal.h"

/** @brief Append one character to a bounded signal-safe buffer. */
static size_t llam_buf_append_char(char *buf, size_t cap, size_t offset, char ch) {
    if (offset < cap) {
        buf[offset] = ch;
    }
    return offset + 1U;
}

/** @brief Append a string to a bounded signal-safe buffer. */
static size_t llam_buf_append_str(char *buf, size_t cap, size_t offset, const char *str) {
    size_t i = 0U;

    if (str == NULL) {
        str = "(null)";
    }

    while (str[i] != '\0') {
        offset = llam_buf_append_char(buf, cap, offset, str[i]);
        i += 1U;
    }

    return offset;
}

/** @brief Append an unsigned 64-bit integer to a bounded signal-safe buffer. */
static size_t llam_buf_append_u64(char *buf, size_t cap, size_t offset, uint64_t value) {
    char digits[32];
    size_t count = 0U;

    if (value == 0U) {
        return llam_buf_append_char(buf, cap, offset, '0');
    }

    while (value > 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    while (count > 0U) {
        offset = llam_buf_append_char(buf, cap, offset, digits[count - 1U]);
        count -= 1U;
    }

    return offset;
}

/** @brief Append a uintptr_t as lowercase hexadecimal to a bounded signal-safe buffer. */
static size_t llam_buf_append_hex_uintptr(char *buf, size_t cap, size_t offset, uintptr_t value) {
    char digits[2U + sizeof(uintptr_t) * 2U];
    size_t count = 0U;

    offset = llam_buf_append_str(buf, cap, offset, "0x");
    if (value == 0U) {
        return llam_buf_append_char(buf, cap, offset, '0');
    }

    while (value > 0U && count < sizeof(digits)) {
        unsigned nibble = (unsigned)(value & 0xFU);
        digits[count++] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
        value >>= 4U;
    }

    while (count > 0U) {
        offset = llam_buf_append_char(buf, cap, offset, digits[count - 1U]);
        count -= 1U;
    }

    return offset;
}

/**
 * @brief Best-effort async-signal-safe write loop.
 *
 * @param fd  Destination file descriptor.
 * @param buf Buffer to write.
 * @param len Byte length.
 */
static void llam_async_write_best_effort(int fd, const char *buf, size_t len) {
    while (len > 0U) {
        ssize_t rc = write(fd, buf, len);

        if (rc > 0) {
            buf += (size_t)rc;
            len -= (size_t)rc;
            continue;
        }

        if (rc < 0 && errno == EINTR) {
            continue;
        }

        break;
    }
}

/**
 * @brief Check whether a SIGSEGV address is inside a task stack guard page.
 *
 * @param task       Task to inspect.
 * @param fault_addr Fault address.
 *
 * @return @c true when the address lies inside the guard page.
 */
static bool llam_fault_in_task_guard_page(const llam_task_t *task, uintptr_t fault_addr) {
    uintptr_t guard_lo;
    uintptr_t guard_hi;

    if (task == NULL || task->stack_mapping == NULL || task->stack_base == NULL) {
        return false;
    }

    guard_lo = (uintptr_t)task->stack_mapping;
    guard_hi = (uintptr_t)task->stack_base;
    return fault_addr >= guard_lo && fault_addr < guard_hi;
}

/**
 * @brief Async signal handler used to request task preemption.
 *
 * @param signo Signal number, ignored.
 */
void llam_preempt_signal_handler(int signo) {
    (void)signo;

    if (g_llam_tls_task != NULL) {
        atomic_store_explicit(&g_llam_tls_task->preempt_requested, 1U, memory_order_relaxed);
    }
}

/**
 * @brief SIGSEGV handler for task guard-page diagnostics.
 *
 * Guard-page faults are reported with task/shard state and then terminate the
 * process. Non-guard faults are restored to the default handler and re-raised.
 *
 * @param signo    Signal number.
 * @param info     Signal info containing the fault address.
 * @param ucontext Signal context, currently unused.
 */
void llam_fault_signal_handler(int signo, siginfo_t *info, void *ucontext) {
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;
    uintptr_t fault_addr = info != NULL ? (uintptr_t)info->si_addr : 0U;

    (void)ucontext;

    if (signo == SIGSEGV && task != NULL && llam_fault_in_task_guard_page(task, fault_addr)) {
        char buf[1024];
        size_t off = 0U;

        // Everything below is written using the local buffer helpers to avoid stdio in a signal handler.
        off = llam_buf_append_str(buf, sizeof(buf), off, "nm: guard page fault detected\n");
        off = llam_buf_append_str(buf, sizeof(buf), off, "  task=");
        off = llam_buf_append_u64(buf, sizeof(buf), off, task->id);
        off = llam_buf_append_str(buf, sizeof(buf), off, " shard=");
        off = llam_buf_append_u64(buf, sizeof(buf), off, shard != NULL ? shard->id : 0U);
        off = llam_buf_append_str(buf, sizeof(buf), off, " state=");
        off = llam_buf_append_str(buf, sizeof(buf), off, llam_state_name_from_id(task->state));
        off = llam_buf_append_str(buf, sizeof(buf), off, " wait=");
        off = llam_buf_append_str(buf, sizeof(buf), off, llam_wait_reason_name(task->wait_reason));
        off = llam_buf_append_char(buf, sizeof(buf), off, '\n');
        off = llam_buf_append_str(buf, sizeof(buf), off, "  fault_addr=");
        off = llam_buf_append_hex_uintptr(buf, sizeof(buf), off, fault_addr);
        off = llam_buf_append_str(buf, sizeof(buf), off, " guard=[");
        off = llam_buf_append_hex_uintptr(buf, sizeof(buf), off, (uintptr_t)task->stack_mapping);
        off = llam_buf_append_str(buf, sizeof(buf), off, ", ");
        off = llam_buf_append_hex_uintptr(buf, sizeof(buf), off, (uintptr_t)task->stack_base);
        off = llam_buf_append_str(buf, sizeof(buf), off, ") stack=[");
        off = llam_buf_append_hex_uintptr(buf, sizeof(buf), off, (uintptr_t)task->stack_base);
        off = llam_buf_append_str(buf, sizeof(buf), off, ", ");
        off = llam_buf_append_hex_uintptr(buf,
                                        sizeof(buf),
                                        off,
                                        (uintptr_t)task->stack_base + (uintptr_t)task->stack_size);
        off = llam_buf_append_str(buf, sizeof(buf), off, ")\n");
        if (shard != NULL) {
            unsigned trace_head = atomic_load_explicit(&shard->trace_head, memory_order_acquire);

            if (trace_head > 0U) {
                const llam_trace_event_t *event = &shard->trace_ring[(trace_head - 1U) % LLAM_TRACE_RING_CAP];
                unsigned kind = atomic_load_explicit(&event->kind, memory_order_acquire);
                unsigned reason = atomic_load_explicit(&event->reason, memory_order_relaxed);

                off = llam_buf_append_str(buf, sizeof(buf), off, "  last_trace=");
                off = llam_buf_append_str(buf, sizeof(buf), off, llam_trace_kind_name((llam_trace_kind_t)kind));
                off = llam_buf_append_str(buf, sizeof(buf), off, " reason=");
                off = llam_buf_append_str(buf, sizeof(buf), off, llam_wait_reason_name((llam_wait_reason_t)reason));
                off = llam_buf_append_char(buf, sizeof(buf), off, '\n');
            }
        }
        off = llam_buf_append_str(buf,
                                sizeof(buf),
                                off,
                                "  hint=fixed-size stack overflowed into guard page; increase stack_class\n");
        if (off > sizeof(buf)) {
            off = sizeof(buf);
        }
        llam_async_write_best_effort(STDERR_FILENO, buf, off);
        _exit(128 + signo);
    }

    {
        struct sigaction action;

        // Not a runtime guard-page fault: restore default behavior and re-raise.
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(signo, &action, NULL);
    }
    raise(signo);
}
