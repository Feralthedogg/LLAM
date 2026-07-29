/**
 * @file tests/test_external_doorbell_cases.inc
 * @brief Portable external-driver readiness doorbell lifecycle tests.
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

static bool external_doorbell_native_ready(
    const llam_external_doorbell_t *doorbell) {
    if (doorbell == NULL || !doorbell->initialized) {
        return false;
    }
#if LLAM_PLATFORM_WINDOWS
    return WaitForSingleObject((HANDLE)doorbell->handle, 0U) ==
           WAIT_OBJECT_0;
#else
    {
        struct pollfd pfd;

        pfd.fd = doorbell->read_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        return poll(&pfd, 1, 0) == 1 &&
               (pfd.revents & POLLIN) != 0;
    }
#endif
}

static int external_doorbell_check_native_contract(
    const llam_external_doorbell_t *doorbell) {
#if LLAM_PLATFORM_WINDOWS
    if (doorbell == NULL || doorbell->handle == NULL) {
        return fail_msg("external doorbell did not create a Windows event");
    }
#else
    int read_status;
    int read_descriptor;
    int write_status;
    int write_descriptor;

    if (doorbell == NULL || doorbell->read_fd < 0 ||
        doorbell->write_fd < 0) {
        return fail_msg("external doorbell did not create POSIX descriptors");
    }
#if LLAM_RUNTIME_BACKEND_LINUX
    if (doorbell->read_fd != doorbell->write_fd) {
        return fail_msg("Linux external doorbell did not use one eventfd");
    }
#else
    if (doorbell->read_fd == doorbell->write_fd) {
        return fail_msg("kqueue platform external doorbell did not use a pipe");
    }
#endif
    read_status = fcntl(doorbell->read_fd, F_GETFL);
    write_status = fcntl(doorbell->write_fd, F_GETFL);
    read_descriptor = fcntl(doorbell->read_fd, F_GETFD);
    write_descriptor = fcntl(doorbell->write_fd, F_GETFD);
    if (read_status < 0 || write_status < 0 ||
        (read_status & O_NONBLOCK) == 0 ||
        (write_status & O_NONBLOCK) == 0 ||
        read_descriptor < 0 || write_descriptor < 0 ||
        (read_descriptor & FD_CLOEXEC) == 0 ||
        (write_descriptor & FD_CLOEXEC) == 0) {
        return fail_msg(
            "external doorbell descriptors were not nonblocking CLOEXEC");
    }
#endif
    return 0;
}

static int external_doorbell_consume_one_native_token(
    llam_external_doorbell_t *doorbell) {
#if LLAM_PLATFORM_WINDOWS
    if (WaitForSingleObject((HANDLE)doorbell->handle, 0U) !=
            WAIT_OBJECT_0 ||
        !ResetEvent((HANDLE)doorbell->handle) ||
        WaitForSingleObject((HANDLE)doorbell->handle, 0U) != WAIT_TIMEOUT) {
        return fail_msg("Windows external doorbell was not manual-reset");
    }
#elif LLAM_RUNTIME_BACKEND_LINUX
    uint64_t value = 0U;
    ssize_t rc;

    do {
        rc = read(doorbell->read_fd, &value, sizeof(value));
    } while (rc < 0 && errno == EINTR);
    if (rc != (ssize_t)sizeof(value) || value != 1U) {
        return fail_msg("coalesced eventfd did not contain exactly one token");
    }
    errno = 0;
    if (read(doorbell->read_fd, &value, sizeof(value)) >= 0 ||
        (errno != EAGAIN && errno != EWOULDBLOCK)) {
        return fail_msg("coalesced eventfd contained an extra token");
    }
#else
    unsigned char tokens[2] = {0U, 0U};
    ssize_t rc;

    do {
        rc = read(doorbell->read_fd, tokens, sizeof(tokens));
    } while (rc < 0 && errno == EINTR);
    if (rc != 1 || tokens[0] != 1U) {
        return fail_msg("coalesced readiness pipe did not contain one token");
    }
    errno = 0;
    if (read(doorbell->read_fd, tokens, sizeof(tokens)) >= 0 ||
        (errno != EAGAIN && errno != EWOULDBLOCK)) {
        return fail_msg("coalesced readiness pipe contained an extra token");
    }
#endif
    return 0;
}

static int exercise_external_doorbell_coalescing(void) {
    llam_external_doorbell_t doorbell;
    int rc = 1;

    memset(&doorbell, 0, sizeof(doorbell));
    if (llam_external_doorbell_init(&doorbell) != 0) {
        return fail_errno("external doorbell initialization failed");
    }
    if (external_doorbell_check_native_contract(&doorbell) != 0) {
        goto done;
    }
    if (llam_external_doorbell_signal(&doorbell) != 0 ||
        llam_external_doorbell_signal(&doorbell) != 0 ||
        atomic_load_explicit(&doorbell.pending, memory_order_acquire) != 1U ||
        !external_doorbell_native_ready(&doorbell)) {
        rc = fail_msg("adjacent external doorbell signals did not coalesce");
        goto done;
    }
    if (external_doorbell_consume_one_native_token(&doorbell) != 0) {
        goto done;
    }
    llam_external_doorbell_drain(&doorbell);
    if (atomic_load_explicit(&doorbell.pending, memory_order_acquire) != 0U ||
        external_doorbell_native_ready(&doorbell)) {
        rc = fail_msg("external doorbell drain did not clear readiness");
        goto done;
    }
    rc = 0;

done:
    llam_external_doorbell_destroy(&doorbell);
    return rc;
}

static int exercise_external_doorbell_race_rearm(void) {
    llam_runtime_t runtime;
    llam_shard_t shard;
    llam_task_t task;
    int rc = 1;

    memset(&runtime, 0, sizeof(runtime));
    memset(&shard, 0, sizeof(shard));
    memset(&task, 0, sizeof(task));
    runtime.shards = &shard;
    runtime.active_shards = 1U;
    runtime.external_driver.enabled = true;
    shard.runtime = &runtime;
    shard.id = 0U;
    shard.event_fd = -1;
    atomic_init(&runtime.stop_requested, false);
    atomic_init(&runtime.fatal_errno, 0);
    atomic_init(&runtime.live_tasks, 1U);
    atomic_init(&runtime.overflow_depth, 0U);
    atomic_init(&shard.inject_depth, 0U);
    atomic_init(&shard.norm_depth, 0U);
    atomic_init(&shard.timer_count, 0U);
    atomic_init(&shard.current, NULL);
    if (pthread_mutex_init(&shard.lock, NULL) != 0) {
        return fail_errno("external doorbell race shard lock init failed");
    }
    shard.lock_initialized = true;
    if (llam_external_doorbell_init(
            &runtime.external_driver.doorbell) != 0) {
        pthread_mutex_destroy(&shard.lock);
        return fail_errno("external doorbell race init failed");
    }

    if (llam_external_doorbell_signal(
            &runtime.external_driver.doorbell) != 0) {
        rc = fail_errno("external doorbell race priming signal failed");
        goto done;
    }
    pthread_mutex_lock(&shard.lock);
    llam_queue_push_tail(&shard.hot_q, &task);
    pthread_mutex_unlock(&shard.lock);
    /*
     * This producer observes pending=1 after the native token was already
     * published. A consumer reset after this point would lose the wake unless
     * rearm re-checks scheduler readiness after clearing pending.
     */
    llam_kick_shard(&shard);
    llam_external_doorbell_drain(&runtime.external_driver.doorbell);
    if (external_doorbell_native_ready(&runtime.external_driver.doorbell)) {
        rc = fail_msg("external doorbell race setup did not clear native state");
        goto done;
    }
    llam_external_doorbell_rearm(&runtime);
    if (!external_doorbell_native_ready(&runtime.external_driver.doorbell) ||
        atomic_load_explicit(&runtime.external_driver.doorbell.pending,
                             memory_order_acquire) != 1U) {
        rc = fail_msg("external doorbell lost a reset-window producer");
        goto done;
    }
    rc = 0;

done:
    llam_external_doorbell_destroy(&runtime.external_driver.doorbell);
    pthread_mutex_destroy(&shard.lock);
    return rc;
}

static int exercise_external_doorbell_destroy_once(void) {
    llam_external_doorbell_t doorbell;
#if LLAM_PLATFORM_WINDOWS
    HANDLE owned = NULL;
    HANDLE replacement = NULL;
#else
    int owned_read = -1;
    int owned_write = -1;
    int replacement = -1;
#endif

    memset(&doorbell, 0, sizeof(doorbell));
    if (llam_external_doorbell_init(&doorbell) != 0) {
        return fail_errno("external doorbell destroy fixture init failed");
    }
#if LLAM_PLATFORM_WINDOWS
    owned = (HANDLE)doorbell.handle;
#else
    owned_read = doorbell.read_fd;
    owned_write = doorbell.write_fd;
#endif
    llam_external_doorbell_destroy(&doorbell);
#if LLAM_PLATFORM_WINDOWS
    SetLastError(ERROR_SUCCESS);
    if (WaitForSingleObject(owned, 0U) != WAIT_FAILED ||
        GetLastError() != ERROR_INVALID_HANDLE) {
        return fail_msg("doorbell destroy did not close its Windows event");
    }
    replacement = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (replacement == NULL) {
        return fail_msg("replacement event creation failed");
    }
    llam_external_doorbell_destroy(&doorbell);
    if (!SetEvent(replacement) ||
        WaitForSingleObject(replacement, 0U) != WAIT_OBJECT_0) {
        CloseHandle(replacement);
        return fail_msg("second doorbell destroy closed an unrelated event");
    }
    CloseHandle(replacement);
#else
    errno = 0;
    if (fcntl(owned_read, F_GETFD) >= 0 || errno != EBADF) {
        return fail_msg("doorbell destroy did not close its read descriptor");
    }
    if (owned_write != owned_read) {
        errno = 0;
        if (fcntl(owned_write, F_GETFD) >= 0 || errno != EBADF) {
            return fail_msg(
                "doorbell destroy did not close its write descriptor");
        }
    }
    replacement = open("/dev/null", O_RDONLY);
    if (replacement < 0) {
        return fail_errno("replacement descriptor creation failed");
    }
    llam_external_doorbell_destroy(&doorbell);
    if (fcntl(replacement, F_GETFD) < 0) {
        close(replacement);
        return fail_msg(
            "second doorbell destroy closed an unrelated descriptor");
    }
    close(replacement);
#endif
    return 0;
}

static int exercise_external_doorbell_cases(void) {
    llam_abi_info_t info;

    memset(&info, 0, sizeof(info));
    if (LLAM_RUNTIME_READINESS_NONE != 0 ||
        LLAM_RUNTIME_READINESS_FD != 1 ||
        LLAM_RUNTIME_READINESS_WINDOWS_HANDLE != 2 ||
        llam_abi_get_info(&info, sizeof(info)) != 0 ||
        info.runtime_readiness_size !=
            LLAM_RUNTIME_READINESS_CURRENT_SIZE ||
        LLAM_RUNTIME_READINESS_CURRENT_SIZE !=
            sizeof(llam_runtime_readiness_t)) {
        return fail_msg("ABI did not publish runtime readiness size");
    }
    if (exercise_external_doorbell_coalescing() != 0 ||
        exercise_external_doorbell_race_rearm() != 0 ||
        exercise_external_doorbell_destroy_once() != 0) {
        return 1;
    }
    return 0;
}
