/**
 * @file examples/stress_signal_dump.c
 * @brief Signal-driven runtime dump support for the stress executable.
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

#include "stress_internal.h"

#if !LLAM_PLATFORM_WINDOWS
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>

static atomic_bool g_stress_dump_thread_stop;
static pthread_t g_stress_dump_thread;
static bool g_stress_dump_thread_started;
static sigset_t g_stress_dump_signal_set;
static sigset_t g_stress_dump_old_signal_set;
static bool g_stress_dump_old_signal_set_valid;
static char g_stress_dump_path[512];

static void stress_write_signal_dump(void) {
    int fd = open(g_stress_dump_path, O_CREAT | O_WRONLY | O_APPEND, 0644);

    if (fd < 0) {
        return;
    }
    dprintf(fd, "[stress] runtime dump requested by signal\n");
    llam_dump_runtime_state(fd);
    (void)close(fd);
}

static void *stress_signal_dump_thread_main(void *arg) {
    (void)arg;

    for (;;) {
        int signo = 0;

        if (sigwait(&g_stress_dump_signal_set, &signo) != 0) {
            continue;
        }
        if (atomic_load_explicit(&g_stress_dump_thread_stop, memory_order_acquire)) {
            break;
        }
        stress_write_signal_dump();
    }
    return NULL;
}

void stress_setup_signal_dump(void) {
    const char *path = getenv("LLAM_RUNTIME_DUMP_ON_SIGNAL");
    int written;

    if (path == NULL || path[0] == '\0') {
        return;
    }
    written = snprintf(g_stress_dump_path, sizeof(g_stress_dump_path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(g_stress_dump_path)) {
        fprintf(stderr, "[stress] signal dump path is too long\n");
        return;
    }
    fprintf(stderr, "[stress] signal dump path=%s\n", g_stress_dump_path);
    sigemptyset(&g_stress_dump_signal_set);
    /*
     * LLAM uses SIGUSR1 internally as the cooperative preemption signal on
     * POSIX.  Keep the stress dump trigger on SIGUSR2 so enabling timeout
     * diagnostics never causes runtime worker threads to inherit SIGUSR1 as a
     * blocked signal.
     */
    sigaddset(&g_stress_dump_signal_set, SIGUSR2);
    if (pthread_sigmask(SIG_BLOCK, &g_stress_dump_signal_set, &g_stress_dump_old_signal_set) != 0) {
        fprintf(stderr, "[stress] failed to block signal dump signal\n");
        return;
    }
    g_stress_dump_old_signal_set_valid = true;
    atomic_store_explicit(&g_stress_dump_thread_stop, false, memory_order_release);
    if (pthread_create(&g_stress_dump_thread, NULL, stress_signal_dump_thread_main, NULL) != 0) {
        fprintf(stderr, "[stress] failed to start signal dump thread\n");
        if (g_stress_dump_old_signal_set_valid) {
            (void)pthread_sigmask(SIG_SETMASK, &g_stress_dump_old_signal_set, NULL);
            g_stress_dump_old_signal_set_valid = false;
        }
        return;
    }
    g_stress_dump_thread_started = true;
}

void stress_teardown_signal_dump(void) {
    if (!g_stress_dump_thread_started) {
        return;
    }
    atomic_store_explicit(&g_stress_dump_thread_stop, true, memory_order_release);
    (void)pthread_kill(g_stress_dump_thread, SIGUSR2);
    (void)pthread_join(g_stress_dump_thread, NULL);
    g_stress_dump_thread_started = false;
    if (g_stress_dump_old_signal_set_valid) {
        (void)pthread_sigmask(SIG_SETMASK, &g_stress_dump_old_signal_set, NULL);
        g_stress_dump_old_signal_set_valid = false;
    }
}
#else
void stress_setup_signal_dump(void) {
}

void stress_teardown_signal_dump(void) {
}
#endif
