# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20)

project(llam C ASM)

set(LLAM_ABI_VERSION_MAJOR 2)
set(LLAM_ABI_VERSION_MINOR 0)
set(LLAM_LIBRARY_VERSION "${LLAM_ABI_VERSION_MAJOR}.${LLAM_ABI_VERSION_MINOR}.0")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckSymbolExists)
find_package(Python3 COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(LLAM_ENABLE_WINDOWS_BACKEND "Enable the native Windows 10/11 backend when building on Windows" ON)
add_compile_options($<$<COMPILE_LANG_AND_ID:C,MSVC>:/experimental:c11atomics>)

set(LLAM_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES LLAM_OSX_ARCH_COUNT)
    if(LLAM_OSX_ARCH_COUNT EQUAL 1)
        list(GET CMAKE_OSX_ARCHITECTURES 0 LLAM_TARGET_PROCESSOR)
    else()
        message(FATAL_ERROR "LLAM does not support universal macOS CMake builds yet; configure one CMAKE_OSX_ARCHITECTURES value at a time.")
    endif()
endif()

set(LLAM_SYSTEM_IS_BSD FALSE)
if(CMAKE_SYSTEM_NAME MATCHES "^(FreeBSD|OpenBSD|NetBSD|DragonFly)$")
    set(LLAM_SYSTEM_IS_BSD TRUE)
endif()
set(LLAM_BSD_FEATURE_DEFINITIONS _BSD_SOURCE)

set(LLAM_SYSTEM_IS_KQUEUE FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR LLAM_SYSTEM_IS_BSD)
    set(LLAM_SYSTEM_IS_KQUEUE TRUE)
endif()

set(LLAM_RUNTIME_SOURCES
    src/core/runtime.c
    src/core/runtime_abi.c
    src/core/runtime_errno.c
    src/core/runtime_alloc.c
    src/core/runtime_allocator_quiescent.c
    src/core/runtime_api.c
    src/core/runtime_blocking_api.c
    src/core/runtime_capability.c
    src/core/runtime_broker.c
    src/core/runtime_broker_ops.c
    src/core/runtime_broker_validate.c
    src/core/runtime_broker_buffer.c
    src/core/runtime_broker_descriptor.c
    src/core/runtime_broker_channel.c
    src/core/runtime_broker_revoke.c
    src/core/runtime_broker_task.c
    src/core/runtime_broker_windows_security.c
    src/core/runtime_broker_transport_dispatch.c
    src/core/runtime_broker_transport_ops.c
    src/core/runtime_broker_transport_response.c
    src/core/runtime_broker_transport_rollback.c
    src/core/runtime_broker_transport_ring.c
    src/core/runtime_broker_transport.c
    src/core/runtime_broker_transport_windows.c
    src/core/runtime_broker_transport_windows_fd_stubs.c
    src/core/runtime_broker_transport_windows_pipe.c
    src/core/runtime_broker_transport_windows_session.c
    src/core/runtime_broker_transport_posix.c
    src/core/runtime_broker_transport_posix_message.c
    src/core/runtime_broker_transport_posix_socket.c
    src/core/runtime_broker_transport_unix.c
    src/core/runtime_broker_transport_selftest.c
    src/core/runtime_broker_ring.c
    src/core/runtime_broker_ring_buffer_grant.c
    src/core/runtime_broker_ring_doorbell.c
    src/core/runtime_broker_ring_dispatch.c
    src/core/runtime_broker_ring_stats.c
    src/core/runtime_broker_ring_shm.c
    src/core/runtime_broker_ring_shm_posix.c
    src/core/runtime_broker_ring_shm_windows.c
    src/core/runtime_cancel_api.c
    src/core/runtime_context_portable.c
    src/core/runtime_debug.c
    src/core/runtime_debug_dump_helpers.c
    src/core/runtime_debug_stats_json.c
    src/core/runtime_fault.c
    src/core/runtime_fp.c
    src/core/runtime_io_object_alloc.c
    src/core/runtime_io_udata.c
    src/core/runtime_init.c
    src/core/runtime_lifecycle.c
    src/core/runtime_channel_cache.c
    src/core/runtime_channel_lifecycle.c
    src/core/runtime_channel_fast.c
    src/core/runtime_channel.c
    src/core/runtime_channel_select_fast.c
    src/core/runtime_channel_select.c
    src/core/runtime_cond_lifecycle.c
    src/core/runtime_cond.c
    src/core/runtime_handle.c
    src/core/runtime_owner.c
    src/core/runtime_registry.c
    src/core/runtime_mutex_lifecycle.c
    src/core/runtime_mutex.c
    src/core/runtime_names.c
    src/core/runtime_norm_queue.c
    src/core/runtime_platform.c
    src/core/runtime_queue_base.c
    src/core/runtime_queue.c
    src/core/runtime_reinject.c
    src/core/runtime_safepoint.c
    src/core/runtime_run.c
    src/core/runtime_scheduler.c
    src/core/runtime_shutdown.c
    src/core/runtime_stack_sample.c
    src/core/runtime_sync.c
    src/core/runtime_spawn.c
    src/core/runtime_task_alloc.c
    src/core/runtime_task_handle_registry.c
    src/core/runtime_task_group.c
    src/core/runtime_task_local.c
    src/core/runtime_task_reclaim.c
    src/core/runtime_task_stack.c
    src/core/runtime_timer_heap.c
    src/core/runtime_timer.c
    src/core/runtime_time.c
    src/core/runtime_trace.c
    src/core/runtime_util.c
    src/core/runtime_wait.c
    src/core/runtime_wait_timer_alloc.c
    src/core/runtime_wait_tracking.c
    src/core/runtime_wake.c
    src/core/runtime_windows.c
    src/core/runtime_yield_join_sleep.c
    src/engine/runtime_block.c
    src/engine/runtime_engine.c
    src/engine/runtime_watchdog.c
    src/engine/runtime_watchdog_merge.c
    src/engine/runtime_watchdog_probe.c
    src/engine/runtime_watchdog_rehome.c
    src/engine/runtime_watchdog_scale.c
    src/engine/runtime_watchdog_worker.c
    src/io/runtime_io_api.c
    src/io/runtime_io_api_blocking_ops.c
    src/io/runtime_io_api_direct.c
    src/io/runtime_io_api_direct_tuning.c
    src/io/runtime_io_api_issue.c
    src/io/runtime_io_buffer_registry.c
    src/io/runtime_io_api_blocking_file_ops.c
    src/io/runtime_io_api_handle_positional.c
    src/io/runtime_io_api_owned.c
    src/io/runtime_io_api_positional.c
    src/io/runtime_io_api_positional_util.c
    src/io/runtime_io_api_public.c
    src/io/runtime_io_engine.c
    src/io/runtime_io_watch.c
    src/io/runtime_io_watch_close.c
    src/io/runtime_io_watch_lookup.c
    src/io/runtime_io_watch_migration.c
    src/io/runtime_io_watch_queue.c
    src/io/runtime_io_watch_waiter.c
    src/io/windows/runtime_windows_iocp.c
)

if(LLAM_SYSTEM_IS_KQUEUE)
    list(APPEND LLAM_RUNTIME_SOURCES
        src/io/darwin/runtime_io_watch_darwin_state.c
        src/io/darwin/runtime_io_watch_darwin_migration_live.c
        src/io/darwin/runtime_io_watch_darwin_migration_rehome.c
        src/io/darwin/runtime_io_watch_darwin_control.c
        src/io/darwin/runtime_io_watch_darwin_completion.c
        src/io/darwin/runtime_io_watch_darwin_events.c
        src/io/darwin/runtime_io_watch_darwin_worker.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/io/linux/runtime_io_watch_linux_prelude.c
        src/io/linux/runtime_io_watch_linux_state.c
        src/io/linux/runtime_io_watch_linux_lookup.c
        src/io/linux/runtime_io_watch_linux_migration_live.c
        src/io/linux/runtime_io_watch_linux_migration_rehome.c
        src/io/linux/runtime_io_watch_linux_control.c
        src/io/linux/runtime_io_watch_linux_submit.c
        src/io/linux/runtime_io_watch_linux_cqe.c
        src/io/linux/runtime_io_watch_linux_worker.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/io/windows/runtime_io_watch_windows_state.c
        src/io/windows/runtime_io_watch_windows_socket.c
        src/io/windows/runtime_io_watch_windows_pool.c
        src/io/windows/runtime_io_watch_windows_control.c
        src/io/windows/runtime_io_watch_windows_submit.c
        src/io/windows/runtime_io_watch_windows_completion.c
        src/io/windows/runtime_io_watch_windows_fallback.c
        src/io/windows/runtime_io_watch_windows.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND LLAM_TARGET_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/core/runtime_context_arm64.c
        src/asm/darwin/arm64/context_arm64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/darwin/x86_64/context_x86_64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/linux/x86_64/context_x86_64.S
        src/asm/linux/x86_64/runtime_linux_x86_64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/core/runtime_context_arm64.c
        src/asm/linux/arm64/context_arm64.S
    )
elseif(LLAM_SYSTEM_IS_BSD AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/linux/x86_64/context_x86_64.S
    )
elseif(LLAM_SYSTEM_IS_BSD AND LLAM_TARGET_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/core/runtime_context_arm64.c
        src/asm/linux/arm64/context_arm64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MSVC)
    enable_language(ASM_MASM)
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/windows/x86_64/context_x86_64.asm
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/windows/x86_64/context_x86_64.S
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_REQUIRED_LIBRARIES uring)
    check_symbol_exists(io_uring_setup_buf_ring "liburing.h" LLAM_HAVE_IO_URING_SETUP_BUF_RING)
    unset(CMAKE_REQUIRED_LIBRARIES)
endif()

add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})
add_library(llam_runtime_shared SHARED ${LLAM_RUNTIME_SOURCES})
add_library(llam::runtime ALIAS llam_runtime)
add_library(llam::runtime_shared ALIAS llam_runtime_shared)

set_target_properties(llam_runtime
    PROPERTIES
        OUTPUT_NAME llam_runtime
        EXPORT_NAME runtime
        POSITION_INDEPENDENT_CODE ON
)

set_target_properties(llam_runtime_shared
    PROPERTIES
        OUTPUT_NAME llam_runtime
        EXPORT_NAME runtime_shared
        VERSION ${LLAM_LIBRARY_VERSION}
        SOVERSION ${LLAM_ABI_VERSION_MAJOR}
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_target_properties(llam_runtime_shared
        PROPERTIES
            RUNTIME_OUTPUT_NAME llam_runtime
            ARCHIVE_OUTPUT_NAME llam_runtime_shared
            LIBRARY_OUTPUT_NAME llam_runtime_shared
    )
endif()

foreach(LLAM_TARGET llam_runtime llam_runtime_shared)
    target_include_directories(${LLAM_TARGET}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    )

    target_compile_definitions(${LLAM_TARGET} PRIVATE _GNU_SOURCE)

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_definitions(${LLAM_TARGET} PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
    elseif(LLAM_SYSTEM_IS_BSD)
        # BSD libc headers hide MAP_ANON/MAP_ANONYMOUS behind strict
        # _XOPEN_SOURCE on some targets. Keep the default BSD namespace visible
        # so the runtime allocator can use anonymous mmap portably.
        target_compile_definitions(${LLAM_TARGET} PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(LLAM_HAVE_IO_URING_SETUP_BUF_RING)
            target_compile_definitions(${LLAM_TARGET} PRIVATE LLAM_HAVE_IO_URING_BUF_RING_HELPERS=1)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        if(NOT LLAM_ENABLE_WINDOWS_BACKEND)
            message(FATAL_ERROR "Native Windows builds require LLAM_ENABLE_WINDOWS_BACKEND=ON")
        endif()
        target_compile_definitions(${LLAM_TARGET} PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    endif()
endforeach()

target_compile_definitions(llam_runtime_shared PRIVATE LLAM_BUILD_SHARED=1)

find_package(Threads REQUIRED)
target_link_libraries(llam_runtime PUBLIC Threads::Threads)
target_link_libraries(llam_runtime_shared PUBLIC Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(llam_runtime PUBLIC uring m)
    target_link_libraries(llam_runtime_shared PUBLIC uring m)
elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    target_link_libraries(llam_runtime PUBLIC rt)
    target_link_libraries(llam_runtime_shared PUBLIC rt)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(llam_runtime PUBLIC ws2_32 mswsock synchronization advapi32)
    target_link_libraries(llam_runtime_shared PUBLIC ws2_32 mswsock synchronization advapi32)
endif()

set(LLAM_POSIX_EXAMPLE_EXCLUDE)
set(LLAM_POSIX_TEST_EXCLUDE)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(LLAM_POSIX_EXAMPLE_EXCLUDE EXCLUDE_FROM_ALL)
    set(LLAM_POSIX_TEST_EXCLUDE EXCLUDE_FROM_ALL)
endif()

add_executable(demo ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    examples/demo.c
    examples/demo_tasks.c
    examples/demo_entry.c
)
target_include_directories(demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_compile_definitions(demo PRIVATE _GNU_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(demo PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
endif()
target_link_libraries(demo PRIVATE llam_runtime)

add_executable(stress ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    examples/stress.c
    examples/diagnostic_output.c
    examples/stress_support.c
    examples/stress_tasks.c
    examples/stress_core_cases.c
    examples/stress_timeout_cases.c
    examples/stress_dynamic_cases.c
    examples/stress_suite.c
    examples/stress_entry.c
    examples/stress_signal_dump.c
)
target_include_directories(stress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_compile_definitions(stress PRIVATE _GNU_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(stress PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
endif()
target_link_libraries(stress PRIVATE llam_runtime)

add_executable(bench ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    examples/bench.c
    examples/bench_support.c
    examples/bench_entry.c
)
target_include_directories(bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_compile_definitions(bench PRIVATE _GNU_SOURCE)
if(LLAM_SYSTEM_IS_BSD)
    target_compile_definitions(bench PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(bench PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
endif()
target_link_libraries(bench PRIVATE llam_runtime)

add_executable(llam_broker
    examples/broker.c
)
target_include_directories(llam_broker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
)
target_compile_definitions(llam_broker PRIVATE _GNU_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(llam_broker PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
elseif(LLAM_SYSTEM_IS_BSD)
    target_compile_definitions(llam_broker PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(llam_broker PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
endif()
target_link_libraries(llam_broker PRIVATE llam_runtime)

set(LLAM_SERVER_SOURCES
    examples/server.c
)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LLAM_SERVER_SOURCES
        examples/diagnostic_output.c
        examples/server_support.c
    )
endif()

add_executable(server ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    ${LLAM_SERVER_SOURCES}
)
target_compile_definitions(server PRIVATE _GNU_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(server PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
elseif(LLAM_SYSTEM_IS_BSD)
    target_compile_definitions(server PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(server PRIVATE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(server PRIVATE llam_runtime)

add_executable(server_lossless ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    ${LLAM_SERVER_SOURCES}
)
target_compile_definitions(server_lossless PRIVATE _GNU_SOURCE LLAM_CHAT_LOSSLESS_DEFAULT=1)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(server_lossless PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
elseif(LLAM_SYSTEM_IS_BSD)
    target_compile_definitions(server_lossless PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(server_lossless PRIVATE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(server_lossless PRIVATE llam_runtime)

add_executable(server_flood ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    examples/server_flood.c
    examples/server_flood_stats.c
)
target_compile_definitions(server_flood PRIVATE _GNU_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(server_flood PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
elseif(LLAM_SYSTEM_IS_BSD)
    target_compile_definitions(server_flood PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(server_flood PRIVATE _WIN32_WINNT=0x0A00)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(server_flood PRIVATE Threads::Threads m)
endif()

enable_testing()

add_executable(test_abi_contract
    tests/test_abi_contract.c
)
target_link_libraries(test_abi_contract PRIVATE llam_runtime)

add_executable(test_abi_compat
    tests/test_abi_compat.c
)
target_link_libraries(test_abi_compat PRIVATE llam_runtime)

add_executable(test_connect_io ${LLAM_POSIX_TEST_EXCLUDE}
    tests/test_connect_io.c
)
target_link_libraries(test_connect_io PRIVATE llam_runtime)

add_executable(test_runtime_core
    tests/test_runtime_core.c
)
target_link_libraries(test_runtime_core PRIVATE llam_runtime)

add_executable(test_multi_runtime_core
    tests/test_multi_runtime_core.c
)
target_link_libraries(test_multi_runtime_core PRIVATE llam_runtime)

add_executable(test_runtime_api_edges ${LLAM_POSIX_TEST_EXCLUDE}
    tests/test_runtime_api_edges.c
)
target_link_libraries(test_runtime_api_edges PRIVATE llam_runtime)

add_executable(test_runtime_select_edges
    tests/test_runtime_select_edges.c
)
target_link_libraries(test_runtime_select_edges PRIVATE llam_runtime)

add_executable(test_runtime_io_dump ${LLAM_POSIX_TEST_EXCLUDE}
    tests/test_runtime_io_dump.c
)
target_link_libraries(test_runtime_io_dump PRIVATE llam_runtime)

add_executable(test_runtime_group_local_edges
    tests/test_runtime_group_local_edges.c
)
target_link_libraries(test_runtime_group_local_edges PRIVATE llam_runtime)

add_executable(test_runtime_unmanaged_join
    tests/test_runtime_unmanaged_join.c
)
target_link_libraries(test_runtime_unmanaged_join PRIVATE llam_runtime)

add_executable(test_runtime_stress
    tests/test_runtime_stress.c
)
target_link_libraries(test_runtime_stress PRIVATE llam_runtime)

add_executable(test_runtime_fuzz
    tests/test_runtime_fuzz.c
)
target_link_libraries(test_runtime_fuzz PRIVATE llam_runtime)

add_executable(test_runtime_invariants
    tests/test_runtime_invariants.c
)
target_link_libraries(test_runtime_invariants PRIVATE llam_runtime)

add_executable(test_runtime_shutdown_internal
    tests/test_runtime_shutdown_internal.c
)
target_include_directories(test_runtime_shutdown_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_runtime_shutdown_internal PRIVATE llam_runtime)

add_executable(test_sync_primitives
    tests/test_sync_primitives.c
)
target_link_libraries(test_sync_primitives PRIVATE llam_runtime)

add_executable(test_io_buffers ${LLAM_POSIX_TEST_EXCLUDE}
    tests/test_io_buffers.c
)
target_link_libraries(test_io_buffers PRIVATE llam_runtime)

add_executable(test_windows_policy
    tests/test_windows_policy.c
)
target_link_libraries(test_windows_policy PRIVATE llam_runtime)

add_executable(test_windows_runtime_smoke
    tests/test_windows_runtime_smoke.c
)
target_link_libraries(test_windows_runtime_smoke PRIVATE llam_runtime)

add_executable(test_security_capability
    tests/test_security_capability.c
)
target_link_libraries(test_security_capability PRIVATE llam_runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(test_windows_iocp_io
        tests/test_windows_iocp_io.c
    )
    target_include_directories(test_windows_iocp_io PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    )
    target_compile_definitions(test_windows_iocp_io PRIVATE _GNU_SOURCE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    target_link_libraries(test_windows_iocp_io PRIVATE llam_runtime)

    add_executable(test_windows_handle_io
        tests/test_windows_handle_io.c
    )
    target_include_directories(test_windows_handle_io PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    )
    target_compile_definitions(test_windows_handle_io PRIVATE _GNU_SOURCE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    target_link_libraries(test_windows_handle_io PRIVATE llam_runtime)

    add_executable(test_windows_iocp_dump
        tests/test_windows_iocp_dump.c
    )
    target_include_directories(test_windows_iocp_dump PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    )
    target_compile_definitions(test_windows_iocp_dump PRIVATE _GNU_SOURCE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    target_link_libraries(test_windows_iocp_dump PRIVATE llam_runtime)
endif()

add_executable(test_shared_load ${LLAM_POSIX_TEST_EXCLUDE}
    tests/test_shared_load.c
)
target_link_libraries(test_shared_load PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(test_shared_load llam_runtime_shared)

foreach(LLAM_TEST_TARGET
        test_abi_contract
        test_abi_compat
        test_connect_io
        test_runtime_core
        test_multi_runtime_core
        test_runtime_api_edges
        test_runtime_select_edges
        test_runtime_io_dump
        test_runtime_group_local_edges
        test_runtime_unmanaged_join
        test_runtime_stress
        test_runtime_fuzz
        test_runtime_invariants
        test_runtime_shutdown_internal
        test_sync_primitives
        test_io_buffers
        test_windows_policy
        test_windows_runtime_smoke
        test_security_capability
        test_shared_load)
    target_include_directories(${LLAM_TEST_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    )
    target_compile_definitions(${LLAM_TEST_TARGET} PRIVATE _GNU_SOURCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_definitions(${LLAM_TEST_TARGET} PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
    elseif(LLAM_SYSTEM_IS_BSD)
        target_compile_definitions(${LLAM_TEST_TARGET} PRIVATE ${LLAM_BSD_FEATURE_DEFINITIONS})
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(${LLAM_TEST_TARGET} PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    endif()
endforeach()

add_test(NAME test_abi_contract COMMAND test_abi_contract)
add_test(NAME test_abi_compat COMMAND test_abi_compat)
add_test(NAME test_runtime_core COMMAND test_runtime_core)
add_test(NAME test_multi_runtime_core COMMAND test_multi_runtime_core)
add_test(NAME test_runtime_select_edges COMMAND test_runtime_select_edges)
add_test(NAME test_runtime_group_local_edges COMMAND test_runtime_group_local_edges)
add_test(NAME test_runtime_unmanaged_join COMMAND test_runtime_unmanaged_join)
add_test(NAME test_runtime_stress COMMAND test_runtime_stress)
add_test(NAME test_runtime_fuzz COMMAND test_runtime_fuzz)
add_test(NAME test_runtime_invariants COMMAND test_runtime_invariants)
add_test(NAME test_runtime_shutdown_internal COMMAND test_runtime_shutdown_internal)
add_test(NAME test_sync_primitives COMMAND test_sync_primitives)
add_test(NAME test_windows_policy COMMAND test_windows_policy)
add_test(NAME test_windows_runtime_smoke COMMAND test_windows_runtime_smoke)
add_test(NAME test_security_capability COMMAND test_security_capability)
add_test(NAME llam_broker_self_test COMMAND llam_broker --self-test)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_test(NAME test_windows_iocp_io COMMAND test_windows_iocp_io)
    add_test(NAME test_windows_iocp_dump COMMAND test_windows_iocp_dump)
    add_test(NAME test_windows_handle_io COMMAND test_windows_handle_io)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_test(NAME test_runtime_api_edges COMMAND test_runtime_api_edges)
    add_test(NAME test_connect_io COMMAND test_connect_io)
    add_test(NAME test_runtime_io_dump COMMAND test_runtime_io_dump)
    add_test(NAME test_io_buffers COMMAND test_io_buffers)
    add_test(NAME test_shared_load COMMAND test_shared_load $<TARGET_FILE:llam_runtime_shared>)
    if(Python3_FOUND)
        add_test(NAME audit_shared_exports
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_shared_exports.py
                         $<TARGET_FILE:llam_runtime_shared>
                         --root ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()

set(LLAM_PC_PRIVATE_LIBS "-pthread")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LLAM_PC_PRIVATE_LIBS "-pthread -luring -lm")
elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    set(LLAM_PC_PRIVATE_LIBS "-pthread -lrt")
endif()

configure_file(
    cmake/llam.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/llam.pc
    @ONLY
)

configure_package_config_file(
    cmake/llam-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/llam-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llam
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/llam-config-version.cmake
    VERSION ${LLAM_LIBRARY_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    TARGETS llam_runtime llam_runtime_shared
    EXPORT llam-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    install(
        TARGETS demo stress bench server server_lossless server_flood
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam/docs
)

install(
    FILES README.md LICENSE CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam
)

install(
    DIRECTORY examples/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam/examples
    FILES_MATCHING
        PATTERN "*.c"
        PATTERN "*.h"
)

install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/llam.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/llam-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/llam-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llam
)

install(
    EXPORT llam-targets
    NAMESPACE llam::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llam
)
