# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSE.

cmake_minimum_required(VERSION 3.20)

if(CMAKE_SYSTEM_NAME MATCHES "^(FreeBSD|OpenBSD|NetBSD|DragonFly)$" OR
   (NOT CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_NAME MATCHES "^(FreeBSD|OpenBSD|NetBSD|DragonFly)$"))
    message(FATAL_ERROR "BSD support ended in LLAM 3.0.0; use versioned 2.x sources/tooling for legacy BSD builds.")
endif()

project(llam VERSION 3.0.0 LANGUAGES C ASM)

set(LLAM_ABI_VERSION_MAJOR 2)
set(LLAM_ABI_VERSION_MINOR 3)
set(LLAM_LIBRARY_VERSION "${PROJECT_VERSION}")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckSymbolExists)
find_package(Python3 COMPONENTS Interpreter)
find_program(LLAM_BASH_EXECUTABLE bash)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
option(LLAM_ENABLE_WINDOWS_BACKEND "Enable the native Windows 10/11 backend when building on Windows" ON)
set(LLAM_NATIVE_ASYNC_DEFAULT OFF)
if((CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|aarch64|arm64") OR
   (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64" AND
    (NOT CMAKE_OSX_ARCHITECTURES OR CMAKE_OSX_ARCHITECTURES MATCHES "^(aarch64|arm64)$")) OR
   (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64" AND
    CMAKE_C_COMPILER_ID STREQUAL "GNU"))
    set(LLAM_NATIVE_ASYNC_DEFAULT ON)
endif()
option(LLAM_EXPERIMENTAL_ASYNC_PREEMPT
    "Build native async backends; explicit ON also enables experimental platform adapters" ${LLAM_NATIVE_ASYNC_DEFAULT})
set(LLAM_MANAGED_TOOLING "off" CACHE STRING "Build LLVM 23.1.2 managed C tooling (off, auto, required)")
set_property(CACHE LLAM_MANAGED_TOOLING PROPERTY STRINGS off auto required)
if(NOT LLAM_MANAGED_TOOLING MATCHES "^(off|auto|required)$")
    message(FATAL_ERROR "LLAM_MANAGED_TOOLING must be off, auto, or required")
endif()
option(LLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE
    "Build the test-only Windows x64 native scheduler async-restore oracle" OFF)
set(LLAM_PREEMPT_TOOLING "off" CACHE STRING
    "Build the optional compiler metadata tooling: off, auto, or required")
set_property(CACHE LLAM_PREEMPT_TOOLING PROPERTY STRINGS off auto required)
string(TOLOWER "${LLAM_PREEMPT_TOOLING}" LLAM_PREEMPT_TOOLING_MODE)
if(NOT LLAM_PREEMPT_TOOLING_MODE MATCHES "^(off|auto|required)$")
    message(FATAL_ERROR "LLAM_PREEMPT_TOOLING must be off, auto, or required")
endif()
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

set(LLAM_SYSTEM_IS_KQUEUE FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(LLAM_SYSTEM_IS_KQUEUE TRUE)
endif()

set(LLAM_RUNTIME_SOURCES
    src/core/lifecycle/runtime.c
    src/core/base/abi.c
    src/core/base/errno.c
    src/core/memory/alloc.c
    src/core/memory/task_span.c
    src/core/memory/allocator_quiescent.c
    src/core/api/core_api.c
    src/core/api/blocking_api.c
    src/core/api/magic2_api.c
    src/core/registry/capability.c
    src/core/broker/broker.c
    src/core/broker/broker_lifecycle.c
    src/core/broker/broker_ops.c
    src/core/broker/broker_validate.c
    src/core/broker/broker_buffer.c
    src/core/broker/broker_descriptor.c
    src/core/broker/broker_channel.c
    src/core/broker/broker_revoke.c
    src/core/broker/broker_task.c
    src/core/broker/broker_windows_security.c
    src/core/broker/transport/broker_transport_dispatch.c
    src/core/broker/transport/broker_transport_ops.c
    src/core/broker/transport/broker_transport_response.c
    src/core/broker/transport/broker_transport_rollback.c
    src/core/broker/transport/broker_transport_ring.c
    src/core/broker/transport/broker_transport.c
    src/core/broker/transport/broker_transport_windows.c
    src/core/broker/transport/broker_transport_windows_fd_stubs.c
    src/core/broker/transport/broker_transport_windows_pipe.c
    src/core/broker/transport/broker_transport_windows_session.c
    src/core/broker/transport/broker_transport_posix.c
    src/core/broker/transport/broker_transport_posix_message.c
    src/core/broker/transport/broker_transport_posix_socket.c
    src/core/broker/transport/broker_transport_unix.c
    src/core/broker/transport/broker_transport_selftest.c
    src/core/broker/ring/broker_ring.c
    src/core/broker/ring/broker_ring_buffer_grant.c
    src/core/broker/ring/broker_ring_doorbell.c
    src/core/broker/ring/broker_ring_dispatch.c
    src/core/broker/ring/broker_ring_ops.c
    src/core/broker/ring/broker_ring_queue.c
    src/core/broker/ring/broker_ring_stats.c
    src/core/broker/ring/broker_ring_shm.c
    src/core/broker/ring/broker_ring_shm_posix.c
    src/core/broker/ring/broker_ring_shm_windows.c
    src/core/api/cancel_api.c
    src/core/context/context_portable.c
    src/core/debug/debug.c
    src/core/debug/debug_dump_helpers.c
    src/core/debug/debug_stats_json.c
    src/core/debug/fault.c
    src/core/context/fp.c
    src/core/memory/io_object_alloc.c
    src/core/base/io_udata.c
    src/core/lifecycle/init.c
    src/core/lifecycle/lifecycle.c
    src/core/sync/channel_cache.c
    src/core/sync/channel_lifecycle.c
    src/core/sync/channel_fast.c
    src/core/sync/channel.c
    src/core/sync/channel_select_fast.c
    src/core/sync/channel_select.c
    src/core/sync/cond_lifecycle.c
    src/core/sync/cond.c
    src/core/registry/handle.c
    src/core/registry/owner.c
    src/core/registry/registry.c
    src/core/sync/mutex_lifecycle.c
    src/core/sync/mutex.c
    src/core/base/names.c
    src/core/sched/norm_queue_depth.c
    src/core/sched/norm_queue.c
    src/core/platform/platform.c
    src/core/preempt/capabilities.c
    src/core/preempt/native_domain.c
    src/core/preempt/managed_catalog.c
    src/core/api/managed_foreign.c
    src/core/context/capsule_pool.c
    src/core/preempt/managed_image.c
    src/core/preempt/managed_image_darwin.c
    src/core/preempt/managed_image_elf.c
    src/core/preempt/managed_image_windows.c
    tools/preempt/metadata.c
    tools/preempt/metadata_v2.c
    src/core/sched/queue_base.c
    src/core/sched/core_queue.c
    src/core/sched/reinject.c
    src/core/sched/safepoint.c
    src/core/lifecycle/run.c
    src/core/lifecycle/managed_run.c
    src/core/sched/scheduler.c
    src/core/sched/managed_scheduler.c
    src/core/context/managed_async_adapter.c
    src/core/sched/worker_registry.c
    src/core/sched/execution_lease.c
    src/engine/watchdog/watchdog_deadlock.c
    src/core/lifecycle/shutdown.c
    src/core/context/stack_sample.c
    src/core/context/async_context.c
    src/core/context/async_context_shared.c
    src/core/sync/sync.c
    src/core/task/spawn.c
    src/core/task/task_alloc.c
    src/core/task/task_handle_registry.c
    src/core/task/task_handle_claim.c
    src/core/task/task_group.c
    src/core/task/task_group_registry.c
    src/core/task/task_local.c
    src/core/task/task_reclaim.c
    src/core/task/task_stack.c
    src/core/wait/wait_accounting.c
    src/core/time/timer_heap.c
    src/core/time/timer.c
    src/core/api/timer_api.c
    src/core/api/signal_api.c
    src/core/time/time.c
    src/core/debug/trace.c
    src/core/base/util.c
    src/core/wait/wait.c
    src/core/wait/wait_timer_alloc.c
    src/core/wait/wait_tracking.c
    src/core/sched/wake.c
    src/core/platform/windows_policy.c
    src/core/task/yield_join_sleep.c
    src/engine/scheduler/block.c
    src/engine/scheduler/scheduler_engine.c
    src/engine/watchdog/watchdog.c
    src/engine/watchdog/watchdog_autotune.c
    src/engine/watchdog/watchdog_merge.c
    src/engine/watchdog/watchdog_probe.c
    src/engine/watchdog/watchdog_rehome.c
    src/engine/watchdog/watchdog_scale.c
    src/engine/watchdog/watchdog_worker.c
    src/io/api/io_api.c
    src/io/api/blocking_ops.c
    src/io/api/direct.c
    src/io/api/direct_tuning.c
    src/io/api/issue.c
    src/io/buffer/buffer_registry.c
    src/io/api/blocking_file_ops.c
    src/io/api/blocking_wrappers.c
    src/io/api/datagram.c
    src/io/api/handle_positional.c
    src/io/api/owned.c
    src/io/api/positional.c
    src/io/api/positional_util.c
    src/io/api/public.c
    src/io/api/wait_options.c
    src/io/engine/io_engine.c
    src/io/watch/watch.c
    src/io/watch/watch_index.c
    src/io/watch/close.c
    src/io/watch/watch_lookup.c
    src/io/watch/migration.c
    src/io/watch/watch_queue.c
    src/io/watch/waiter.c
    src/io/windows/watch/iocp.c
)

if(LLAM_SYSTEM_IS_KQUEUE)
    list(APPEND LLAM_RUNTIME_SOURCES
        src/io/darwin/watch/darwin_state.c
        src/io/darwin/watch/darwin_migration_live.c
        src/io/darwin/watch/darwin_migration_rehome.c
        src/io/darwin/watch/darwin_control.c
        src/io/darwin/watch/darwin_completion.c
        src/io/darwin/watch/events.c
        src/io/darwin/watch/darwin_worker.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/io/linux/watch/prelude.c
        src/io/linux/watch/linux_state.c
        src/io/linux/watch/linux_lookup.c
        src/io/linux/watch/linux_migration_live.c
        src/io/linux/watch/linux_migration_rehome.c
        src/io/linux/watch/linux_control.c
        src/io/linux/watch/linux_submit.c
        src/io/linux/watch/cqe.c
        src/io/linux/watch/linux_worker.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/io/windows/watch/windows_state.c
        src/io/windows/watch/socket.c
        src/io/windows/watch/pool.c
        src/io/windows/watch/windows_control.c
        src/io/windows/watch/windows_submit.c
        src/io/windows/watch/windows_completion.c
        src/io/windows/watch/fallback.c
        src/io/windows/watch/windows_watch.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND LLAM_TARGET_PROCESSOR MATCHES "arm64|aarch64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/core/context/context_arm64.c
        src/asm/darwin/arm64/darwin_context_arm64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/darwin/x86_64/darwin_context_x86_64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/linux/x86_64/linux_context_x86_64.S
        src/asm/linux/x86_64/wake_syscalls_x86_64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/core/context/context_arm64.c
        src/asm/linux/arm64/linux_context_arm64.S
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MSVC)
    enable_language(ASM_MASM)
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/windows/x86_64/context_x86_64.asm
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LLAM_RUNTIME_SOURCES
        src/asm/windows/x86_64/windows_context_x86_64.S
    )
endif()

set(LLAM_PREEMPT_PLUGIN_ACTIVE FALSE)
set(LLAM_MANAGED_PLUGIN_ACTIVE FALSE)
if(NOT LLAM_MANAGED_TOOLING STREQUAL "off")
    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        enable_language(CXX)
        find_package(LLVM 23.1.2 EXACT CONFIG QUIET)
        if(LLVM_FOUND)
            set(LLAM_MANAGED_PASS_SOURCES
                tools/preempt/llam_managed_pass.cpp
                tools/preempt/llam_managed_machine.cpp)
            if(TARGET LLVM)
                add_library(llam_managed_pass MODULE ${LLAM_MANAGED_PASS_SOURCES})
                target_link_libraries(llam_managed_pass PRIVATE LLVM)
            elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLVM_ENABLE_PLUGINS)
                # Official ELF tools may link LLVM statically and export their
                # plugin API. Use LLVM's module rules without embedding a second
                # LLVM registry in the plugin.
                list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
                include(AddLLVM)
                add_llvm_library(llam_managed_pass MODULE BUILDTREE_ONLY
                    ${LLAM_MANAGED_PASS_SOURCES})
            endif()
        endif()
        if(TARGET llam_managed_pass)
            set_target_properties(llam_managed_pass PROPERTIES PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
            target_include_directories(llam_managed_pass PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
            target_include_directories(llam_managed_pass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
            separate_arguments(LLAM_MANAGED_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
            target_compile_options(llam_managed_pass PRIVATE ${LLAM_MANAGED_LLVM_DEFINITIONS})
            set(LLAM_MANAGED_PLUGIN_ACTIVE TRUE)
            find_program(LLAM_MANAGED_LLC llc HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
            if(NOT LLAM_MANAGED_LLC)
                message(FATAL_ERROR "Managed tooling needs LLVM 23.1.2 llc")
            endif()
        endif()
    endif()
    if(NOT LLAM_MANAGED_PLUGIN_ACTIVE)
        if(LLAM_MANAGED_TOOLING STREQUAL "required")
            message(FATAL_ERROR "Managed tooling needs LLVM 23.1.2 development files, loadable pass plugins, and C++17")
        endif()
        message(STATUS "Managed tooling unavailable; no managed compiler profile is advertised")
    endif()
endif()
if(NOT LLAM_PREEMPT_TOOLING_MODE STREQUAL "off")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
        find_package(LLVM CONFIG QUIET)
        if(LLVM_FOUND)
            include(CheckLanguage)
            check_language(CXX)
            if(CMAKE_CXX_COMPILER)
                find_program(LLAM_PREEMPT_CLANG_EXECUTABLE
                    NAMES clang-${LLVM_VERSION_MAJOR} clang)
                find_program(LLAM_PREEMPT_OBJCOPY_EXECUTABLE objcopy)
                if(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required" AND
                   (NOT LLAM_BASH_EXECUTABLE OR
                    NOT LLAM_PREEMPT_CLANG_EXECUTABLE OR
                    NOT LLAM_PREEMPT_OBJCOPY_EXECUTABLE))
                    message(FATAL_ERROR
                        "LLAM_PREEMPT_TOOLING=required needs bash, matching clang, and objcopy")
                endif()
                if(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required")
                    execute_process(
                        COMMAND "${LLAM_PREEMPT_CLANG_EXECUTABLE}" -dumpversion
                        RESULT_VARIABLE LLAM_PREEMPT_CLANG_VERSION_RESULT
                        OUTPUT_VARIABLE LLAM_PREEMPT_CLANG_VERSION
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                    )
                    string(REGEX MATCH "^[0-9]+" LLAM_PREEMPT_CLANG_MAJOR
                        "${LLAM_PREEMPT_CLANG_VERSION}")
                    if(NOT LLAM_PREEMPT_CLANG_VERSION_RESULT EQUAL 0 OR
                       NOT LLAM_PREEMPT_CLANG_MAJOR STREQUAL LLVM_VERSION_MAJOR)
                        message(FATAL_ERROR
                            "LLAM_PREEMPT_TOOLING=required needs clang matching LLVM ${LLVM_VERSION_MAJOR}")
                    endif()
                endif()
                enable_language(CXX)
                set(CMAKE_CXX_STANDARD 17)
                set(CMAKE_CXX_STANDARD_REQUIRED ON)
                list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
                include(AddLLVM OPTIONAL)
                if(COMMAND add_llvm_pass_plugin)
                    add_llvm_pass_plugin(llam_preempt_pass
                        tools/preempt/llam_preempt_pass.cpp
                    )
                    target_include_directories(llam_preempt_pass PRIVATE
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt
                    )
                    target_include_directories(llam_preempt_pass SYSTEM PRIVATE
                        ${LLVM_INCLUDE_DIRS}
                    )
                    separate_arguments(LLAM_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
                    target_compile_options(llam_preempt_pass PRIVATE ${LLAM_LLVM_DEFINITIONS})
                    set(LLAM_PREEMPT_PLUGIN_ACTIVE TRUE)
                elseif(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required")
                    message(FATAL_ERROR "LLVM was found but AddLLVM pass-plugin support is unavailable")
                else()
                    message(STATUS "LLVM pass-plugin build support unavailable; optional preemption tooling is off")
                endif()
            elseif(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required")
                message(FATAL_ERROR "LLAM_PREEMPT_TOOLING=required needs a C++ compiler")
            else()
                message(STATUS "C++ compiler not found; optional preemption tooling is off")
            endif()
        elseif(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required")
            message(FATAL_ERROR "LLAM_PREEMPT_TOOLING=required needs LLVM development files")
        else()
            message(STATUS "LLVM development files not found; optional preemption tooling is off")
        endif()
    elseif(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required")
        message(FATAL_ERROR "LLAM preemption compiler metadata tooling currently supports Linux x86-64 only")
    else()
        message(STATUS "Preemption compiler metadata tooling is unsupported on this target; optional mode is off")
    endif()
endif()
set(LLAM_PREEMPT_PLUGIN_AVAILABLE "${LLAM_PREEMPT_PLUGIN_ACTIVE}" CACHE INTERNAL
    "Whether the optional LLAM LLVM preemption pass is available" FORCE)
set(LLAM_PREEMPT_DRIVER_AVAILABLE FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64" AND LLAM_BASH_EXECUTABLE)
    set(LLAM_PREEMPT_DRIVER_AVAILABLE TRUE)
endif()
if(LLAM_PREEMPT_TOOLING_MODE STREQUAL "required" AND NOT LLAM_PREEMPT_DRIVER_AVAILABLE)
    message(FATAL_ERROR "LLAM_PREEMPT_TOOLING=required needs the Linux x86-64 bash driver")
endif()
set(LLAM_PREEMPT_PLUGIN_STATUS "unavailable")
if(LLAM_PREEMPT_PLUGIN_ACTIVE)
    set(LLAM_PREEMPT_PLUGIN_STATUS "available")
endif()
set(LLAM_PREEMPT_DRIVER_STATUS "unavailable")
if(LLAM_PREEMPT_DRIVER_AVAILABLE)
    set(LLAM_PREEMPT_DRIVER_STATUS "${CMAKE_INSTALL_BINDIR}/llam-clang")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/preempt-sdk")
configure_file(
    cmake/llam-preempt-tooling.conf.in
    ${CMAKE_CURRENT_BINARY_DIR}/preempt-sdk/tooling.conf
    @ONLY
)

add_library(llam_preempt_metadata STATIC
    tools/preempt/metadata.c tools/preempt/metadata_v2.c)
target_include_directories(llam_preempt_metadata PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt
)
set_target_properties(llam_preempt_metadata PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_executable(llam_preempt_metadata_inspect tools/preempt/metadata_inspect.c)
target_link_libraries(llam_preempt_metadata_inspect PRIVATE llam_preempt_metadata)
add_executable(test_preempt_metadata tests/test_preempt_metadata.c)
target_link_libraries(test_preempt_metadata PRIVATE llam_preempt_metadata)
add_executable(test_preempt_metadata_v2 tests/test_preempt_metadata_v2.c)
target_link_libraries(test_preempt_metadata_v2 PRIVATE llam_preempt_metadata)
add_executable(test_preempt_metadata_registration
    tests/test_preempt_metadata_registration.c
)
target_include_directories(test_preempt_metadata_registration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt
)
target_link_libraries(test_preempt_metadata_registration PRIVATE llam_runtime)

set(LLAM_ASYNC_PREEMPT_ACTIVE FALSE)
set(LLAM_DARWIN_ASYNC_MECHANISM FALSE)
set(LLAM_LINUX_ARM64_ASYNC_MECHANISM FALSE)
if(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
        set(LLAM_ASYNC_PREEMPT_ACTIVE TRUE)
        add_compile_definitions(LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
        list(APPEND LLAM_RUNTIME_SOURCES
            src/core/context/async_context_linux_x86_64.c
            src/asm/linux/x86_64/async_preempt_trampoline.S
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           LLAM_TARGET_PROCESSOR MATCHES "aarch64|arm64")
        # Private signal frontend, default-OFF. Compilation does not qualify a
        # runtime profile or promote any public capability.
        set(LLAM_LINUX_ARM64_ASYNC_MECHANISM TRUE)
        add_compile_definitions(LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
        list(APPEND LLAM_RUNTIME_SOURCES
            src/core/context/async_context_linux_arm64.c
            src/core/context/async_context_signal.c
            src/asm/linux/arm64/async_context_arm64.S
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
           LLAM_TARGET_PROCESSOR MATCHES "arm64|aarch64")
        # Private one-primary qualification, not public/default-ON support.
        set(LLAM_DARWIN_ASYNC_MECHANISM TRUE)
        add_compile_definitions(LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
        list(APPEND LLAM_RUNTIME_SOURCES
            src/core/context/async_context_darwin.c
            src/asm/darwin/arm64/async_context_arm64.S
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
           LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
        # Explicit experimental build only; native Intel qualification remains
        # separate from compilation or execution under Rosetta.
        add_compile_definitions(LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
        list(APPEND LLAM_RUNTIME_SOURCES
            src/core/context/async_context_darwin_x86_64.c
            src/asm/darwin/x86_64/async_context_x86_64.S)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND
           LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64|AMD64" AND NOT MSVC)
        # Default-off Win64 backend and private PE metadata collector.
        add_compile_definitions(LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
        list(APPEND LLAM_RUNTIME_SOURCES
            src/core/context/windows_async_lifetime.c
            src/core/context/windows_async_pe_metadata.c
            src/asm/windows/x86_64/windows_async_transition.S
        )
    else()
        message(STATUS "LLAM_EXPERIMENTAL_ASYNC_PREEMPT is unsupported on this target; async test will skip")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64")
    # The disabled Linux implementation provides only its internal ENOTSUP stub.
    list(APPEND LLAM_RUNTIME_SOURCES
        src/core/context/async_context_linux_x86_64.c
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # liburing exposes sigset_t-dependent declarations only when the normal
    # GNU/POSIX feature namespace is visible. Match the runtime targets here so
    # the configure probe does not misclassify current buffer-ring helpers as
    # unavailable under strict C11 mode.
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    set(CMAKE_REQUIRED_LIBRARIES uring)
    check_symbol_exists(io_uring_setup_buf_ring "liburing.h" LLAM_HAVE_IO_URING_SETUP_BUF_RING)
    unset(CMAKE_REQUIRED_DEFINITIONS)
    unset(CMAKE_REQUIRED_LIBRARIES)
endif()

add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})
add_library(llam_runtime_shared SHARED ${LLAM_RUNTIME_SOURCES})
add_library(llam_runtime_testhooks STATIC ${LLAM_RUNTIME_SOURCES})
add_library(llam_runtime_profile OBJECT EXCLUDE_FROM_ALL ${LLAM_RUNTIME_SOURCES})
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

set_target_properties(llam_runtime_testhooks
    PROPERTIES
        OUTPUT_NAME llam_runtime_testhooks
        POSITION_INDEPENDENT_CODE ON
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_target_properties(llam_runtime_shared
        PROPERTIES
            RUNTIME_OUTPUT_NAME llam_runtime
            ARCHIVE_OUTPUT_NAME llam_runtime_shared
            LIBRARY_OUTPUT_NAME llam_runtime_shared
    )
endif()

foreach(LLAM_TARGET llam_runtime llam_runtime_shared llam_runtime_testhooks llam_runtime_profile)
    target_include_directories(${LLAM_TARGET}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt
    )

    target_compile_definitions(${LLAM_TARGET} PRIVATE _GNU_SOURCE)

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_definitions(${LLAM_TARGET} PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
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
target_compile_definitions(llam_runtime_testhooks PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_compile_definitions(llam_runtime_profile PRIVATE LLAM_ENABLE_ASYNC_PROFILE=1)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_sources(llam_runtime_testhooks PRIVATE src/core/context/windows_async_probe.c)
    if(NOT LLAM_EXPERIMENTAL_ASYNC_PREEMPT AND NOT MSVC)
        # Keep the old oracle build available when production async is absent.
        target_sources(llam_runtime_testhooks PRIVATE
            src/asm/windows/x86_64/windows_async_transition.S
        )
    endif()
endif()

find_package(Threads REQUIRED)
target_link_libraries(llam_runtime PUBLIC Threads::Threads)
target_link_libraries(llam_runtime_shared PUBLIC Threads::Threads)
target_link_libraries(llam_runtime_testhooks PUBLIC Threads::Threads)
target_link_libraries(llam_runtime_profile PUBLIC Threads::Threads)
if(CMAKE_DL_LIBS)
    target_link_libraries(llam_runtime PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(llam_runtime_shared PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(llam_runtime_testhooks PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(llam_runtime_profile PUBLIC ${CMAKE_DL_LIBS})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(llam_runtime PUBLIC uring m)
    target_link_libraries(llam_runtime_shared PUBLIC uring m)
    target_link_libraries(llam_runtime_testhooks PUBLIC uring m)
    target_link_libraries(llam_runtime_profile PUBLIC uring m)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(llam_runtime PUBLIC ws2_32 mswsock synchronization advapi32)
    target_link_libraries(llam_runtime_shared PUBLIC ws2_32 mswsock synchronization advapi32)
    target_link_libraries(llam_runtime_testhooks PUBLIC ws2_32 mswsock synchronization advapi32)
    target_link_libraries(llam_runtime_profile PUBLIC ws2_32 mswsock synchronization advapi32)
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
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(bench PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
endif()
target_link_libraries(bench PRIVATE llam_runtime)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    add_executable(bench_async_compare EXCLUDE_FROM_ALL
        examples/bench.c examples/bench_support.c examples/bench_entry.c
        experiments/async_compare/bench_async.c tests/fixtures/async_darwin_leaf.S)
    target_include_directories(bench_async_compare PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
    target_compile_definitions(bench_async_compare PRIVATE
        LLAM_BENCH_ASYNC_COMPARE=1 _GNU_SOURCE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
    target_link_libraries(bench_async_compare PRIVATE llam_runtime)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(cpu_spike
        examples/cpu_spike.c
    )
    target_compile_definitions(cpu_spike PRIVATE _GNU_SOURCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_definitions(cpu_spike PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
    endif()
    target_link_libraries(cpu_spike PRIVATE llam_runtime)
endif()

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
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(server_lossless PRIVATE _WIN32_WINNT=0x0A00)
endif()
target_link_libraries(server_lossless PRIVATE llam_runtime)

add_executable(server_flood ${LLAM_POSIX_EXAMPLE_EXCLUDE}
    examples/server_flood.c
    examples/server_flood_stats.c
    examples/server_flood_stats_open.c
)
target_compile_definitions(server_flood PRIVATE _GNU_SOURCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_compile_definitions(server_flood PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_compile_definitions(server_flood PRIVATE _WIN32_WINNT=0x0A00)
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(server_flood PRIVATE Threads::Threads m)
endif()

enable_testing()

set(LLAM_LCWE_MODEL_SOURCES
    experiments/lcwe/lcwe_model.c
    experiments/lcwe/lcwe_workloads.c
)

add_executable(test_lcwe_model
    ${LLAM_LCWE_MODEL_SOURCES}
    experiments/lcwe/test_lcwe_model.c
)
add_executable(bench_lcwe_model
    ${LLAM_LCWE_MODEL_SOURCES}
    experiments/lcwe/bench_lcwe_model.c
)
foreach(LLAM_LCWE_MODEL_TARGET test_lcwe_model bench_lcwe_model)
    target_include_directories(${LLAM_LCWE_MODEL_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/experiments/lcwe
    )
    target_compile_definitions(${LLAM_LCWE_MODEL_TARGET} PRIVATE _GNU_SOURCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_definitions(${LLAM_LCWE_MODEL_TARGET} PRIVATE
            _XOPEN_SOURCE=700
            _DARWIN_C_SOURCE
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(${LLAM_LCWE_MODEL_TARGET} PRIVATE
            _WIN32_WINNT=0x0A00
        )
    endif()
endforeach()

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

add_executable(test_api_usability tests/test_api_usability.c)
target_link_libraries(test_api_usability PRIVATE llam_runtime)
target_include_directories(test_api_usability PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)

add_executable(test_runtime_api_edges ${LLAM_POSIX_TEST_EXCLUDE}
    tests/test_runtime_api_edges.c
)
target_include_directories(test_runtime_api_edges PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
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

add_executable(test_runtime_regressions tests/test_runtime_regressions.c)
target_include_directories(test_runtime_regressions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_runtime_regressions PRIVATE llam_runtime)

add_executable(test_runtime_shutdown_internal
    tests/test_runtime_shutdown_internal.c
)
target_include_directories(test_runtime_shutdown_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(test_runtime_shutdown_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LLAM_HAVE_IO_URING_SETUP_BUF_RING)
    target_compile_definitions(test_runtime_shutdown_internal PRIVATE LLAM_HAVE_IO_URING_BUF_RING_HELPERS=1)
endif()
target_link_libraries(test_runtime_shutdown_internal PRIVATE llam_runtime_testhooks)

add_executable(test_watchdog_liveness_internal tests/test_watchdog_liveness_internal.c)
target_include_directories(test_watchdog_liveness_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
target_compile_definitions(test_watchdog_liveness_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_watchdog_liveness_internal PRIVATE llam_runtime_testhooks)
if(LLAM_DARWIN_ASYNC_MECHANISM OR LLAM_LINUX_ARM64_ASYNC_MECHANISM)
    target_sources(test_watchdog_liveness_internal PRIVATE tests/fixtures/async_darwin_leaf.S)
endif()

add_executable(test_worker_identity_internal
    tests/test_worker_identity_internal.c
)
target_include_directories(test_worker_identity_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(test_worker_identity_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_worker_identity_internal PRIVATE llam_runtime_testhooks)

add_executable(test_legacy_lease_internal
    tests/test_legacy_lease_internal.c
)
target_include_directories(test_legacy_lease_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(test_legacy_lease_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_legacy_lease_internal PRIVATE llam_runtime_testhooks)

add_executable(test_execution_lease_internal tests/test_execution_lease_internal.c)
target_include_directories(test_execution_lease_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_execution_lease_internal PRIVATE llam_runtime)
add_executable(test_native_domain_internal tests/test_native_domain_internal.c)
target_include_directories(test_native_domain_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_native_domain_internal PRIVATE llam_runtime)
add_executable(test_managed_scheduler_internal tests/test_managed_scheduler_internal.c)
target_include_directories(test_managed_scheduler_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_managed_scheduler_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_managed_scheduler_internal PRIVATE llam_runtime_testhooks)
add_executable(test_managed_catalog_internal tests/test_managed_catalog_internal.c)
add_executable(test_managed_foreign_internal tests/test_managed_foreign_internal.c)
add_executable(test_capsule_pool_internal tests/test_capsule_pool_internal.c)
target_include_directories(test_capsule_pool_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_capsule_pool_internal PRIVATE llam_runtime)
target_include_directories(test_managed_foreign_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_managed_foreign_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_managed_foreign_internal PRIVATE llam_runtime_testhooks)
target_include_directories(test_managed_catalog_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(test_managed_catalog_internal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
target_compile_definitions(test_managed_catalog_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_managed_catalog_internal PRIVATE llam_runtime_testhooks)
add_executable(test_managed_async_registered tests/test_managed_async_registered.c)
target_include_directories(test_managed_async_registered PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_managed_async_registered PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_managed_async_registered PRIVATE llam_runtime_testhooks)
add_executable(test_managed_linux_compiler tests/test_managed_linux_compiler.c)
target_include_directories(test_managed_linux_compiler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
target_compile_definitions(test_managed_linux_compiler PRIVATE LLAM_ENABLE_TEST_HOOKS=1 _GNU_SOURCE)
target_link_libraries(test_managed_linux_compiler PRIVATE llam_runtime_testhooks)
set_target_properties(test_managed_linux_compiler PROPERTIES ENABLE_EXPORTS TRUE)
add_executable(test_managed_async_linux_x86_64 tests/test_managed_async_linux_x86_64.c)
target_include_directories(test_managed_async_linux_x86_64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
if(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    target_compile_definitions(test_managed_async_linux_x86_64 PRIVATE LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
endif()
target_compile_definitions(test_managed_async_linux_x86_64 PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_managed_async_linux_x86_64 PRIVATE llam_runtime_testhooks)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    target_sources(test_managed_async_linux_x86_64 PRIVATE tests/fixtures/async_safe_leaf.S)
    if(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
        target_compile_definitions(test_managed_async_linux_x86_64 PRIVATE LLAM_TEST_WRAP_CPU_COUNT=1)
        target_link_options(test_managed_async_linux_x86_64 PRIVATE "-Wl,--wrap=llam_count_allowed_cpus")
    endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
    add_executable(test_windows_join_internal tests/test_windows_join_internal.c)
    target_include_directories(test_windows_join_internal PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
    target_compile_definitions(test_windows_join_internal PRIVATE
        LLAM_ENABLE_WINDOWS_BACKEND=1 _WIN32_WINNT=0x0A00)
    add_test(NAME test_windows_join_internal COMMAND test_windows_join_internal)
    set_tests_properties(test_windows_join_internal PROPERTIES TIMEOUT 10)
    add_executable(test_managed_windows_registered tests/test_managed_windows_registered.c)
    target_include_directories(test_managed_windows_registered PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
    target_compile_definitions(test_managed_windows_registered PRIVATE LLAM_ENABLE_TEST_HOOKS=1
        LLAM_ENABLE_WINDOWS_BACKEND=1 LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1 _WIN32_WINNT=0x0A00)
    target_link_libraries(test_managed_windows_registered PRIVATE llam_runtime_testhooks)
    add_test(NAME test_managed_windows_registered COMMAND test_managed_windows_registered)
    set_tests_properties(test_managed_windows_registered PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    add_executable(test_managed_windows_compiler tests/test_managed_windows_compiler.c)
    target_compile_definitions(test_managed_windows_compiler PRIVATE
        LLAM_ENABLE_WINDOWS_BACKEND=1 _WIN32_WINNT=0x0A00 LLAM_USE_SHARED=1)
    target_link_libraries(test_managed_windows_compiler PRIVATE llam_runtime_shared)
endif()
if(LLAM_DARWIN_ASYNC_MECHANISM OR LLAM_LINUX_ARM64_ASYNC_MECHANISM)
    target_sources(test_managed_async_registered PRIVATE tests/fixtures/async_darwin_leaf.S)
endif()

add_executable(test_processor_resource_internal
    tests/test_processor_resource_internal.c
)
target_include_directories(test_processor_resource_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(test_processor_resource_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_processor_resource_internal PRIVATE llam_runtime_testhooks)

add_executable(test_processor_queue_timer_internal
    tests/test_processor_queue_timer_internal.c
)
target_include_directories(test_processor_queue_timer_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(test_processor_queue_timer_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_processor_queue_timer_internal PRIVATE llam_runtime_testhooks)

add_executable(test_processor_allocator_io_internal
    tests/test_processor_allocator_io_internal.c
)
target_include_directories(test_processor_allocator_io_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(test_processor_allocator_io_internal PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_processor_allocator_io_internal PRIVATE llam_runtime_testhooks)

add_executable(test_foreign_detach_internal
    tests/test_foreign_detach_internal.c
)
target_include_directories(test_foreign_detach_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_foreign_detach_internal PRIVATE llam_runtime)

add_executable(test_relief_worker_internal
    tests/test_relief_worker_internal.c
)
target_include_directories(test_relief_worker_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_relief_worker_internal PRIVATE llam_runtime)

add_executable(test_preempt_capabilities
    tests/test_preempt_capabilities.c
)
target_include_directories(test_preempt_capabilities PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_preempt_capabilities PRIVATE llam_runtime)

add_executable(test_default_policy_internal
    tests/test_default_policy_internal.c
)
target_include_directories(test_default_policy_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_default_policy_internal PRIVATE llam_runtime)

add_executable(test_magic2_internal
    tests/test_magic2_internal.c
    tests/test_magic2_cpu_internal.c
    tests/test_magic2_auto_internal.c
)
target_include_directories(test_magic2_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(test_magic2_internal PRIVATE llam_runtime)

add_executable(bench_magic2_cpu EXCLUDE_FROM_ALL
    experiments/magic2/bench_magic2_cpu.c
)
target_include_directories(bench_magic2_cpu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bench_magic2_cpu PRIVATE llam_runtime)

add_executable(bench_magic2_kernels EXCLUDE_FROM_ALL
    experiments/magic2/bench_magic2_kernels.c
)
target_include_directories(bench_magic2_kernels PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bench_magic2_kernels PRIVATE llam_runtime)

add_executable(bench_handoff EXCLUDE_FROM_ALL experiments/handoff/bench_handoff.c)
add_executable(bench_select_prepare EXCLUDE_FROM_ALL experiments/select_prepare/bench_select_prepare.c)
add_executable(test_large_select tests/test_large_select.c)
foreach(LLAM_SELECT_TARGET bench_select_prepare test_large_select)
    target_include_directories(${LLAM_SELECT_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
    target_link_libraries(${LLAM_SELECT_TARGET} PRIVATE llam_runtime)
endforeach()
add_test(NAME test_large_select COMMAND test_large_select)
set_tests_properties(test_large_select PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
add_executable(bench_watch_index EXCLUDE_FROM_ALL experiments/watch_index/bench_watch_index.c)
add_executable(test_io_watch_index tests/test_io_watch_index.c)
foreach(LLAM_WATCH_TARGET bench_watch_index test_io_watch_index)
    target_include_directories(${LLAM_WATCH_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
endforeach()
target_link_libraries(bench_watch_index PRIVATE llam_runtime)
target_compile_definitions(test_io_watch_index PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_io_watch_index PRIVATE llam_runtime_testhooks)
add_test(NAME test_io_watch_index COMMAND test_io_watch_index)
set_tests_properties(test_io_watch_index PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
add_executable(test_handoff_protocol experiments/handoff/verify_handoff.c)
foreach(LLAM_HANDOFF_TARGET bench_handoff test_handoff_protocol)
    target_include_directories(${LLAM_HANDOFF_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
    target_link_libraries(${LLAM_HANDOFF_TARGET} PRIVATE llam_runtime)
endforeach()
add_test(NAME test_handoff_protocol COMMAND test_handoff_protocol)
set_tests_properties(test_handoff_protocol PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
if(Python3_FOUND)
    add_test(NAME test_handoff_model
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_handoff_model.py)
endif()

add_executable(test_async_preempt_linux_x86_64
    tests/test_async_preempt_linux_x86_64.c
)
target_include_directories(test_async_preempt_linux_x86_64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(LLAM_ASYNC_PREEMPT_ACTIVE)
    target_sources(test_async_preempt_linux_x86_64 PRIVATE
        tests/fixtures/async_safe_leaf.S
    )
    target_compile_definitions(test_async_preempt_linux_x86_64 PRIVATE LLAM_TEST_WRAP_SIGACTION=1)
    target_link_options(test_async_preempt_linux_x86_64 PRIVATE "-Wl,--wrap=sigaction")
endif()
target_compile_definitions(test_async_preempt_linux_x86_64 PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_async_preempt_linux_x86_64 PRIVATE llam_runtime_testhooks)

add_executable(test_async_preempt_multi_linux_x86_64
    tests/test_async_preempt_multi_linux_x86_64.c
)
target_include_directories(test_async_preempt_multi_linux_x86_64 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(LLAM_ASYNC_PREEMPT_ACTIVE)
    target_sources(test_async_preempt_multi_linux_x86_64 PRIVATE tests/fixtures/async_safe_leaf.S)
endif()
target_compile_definitions(test_async_preempt_multi_linux_x86_64 PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_async_preempt_multi_linux_x86_64 PRIVATE llam_runtime_testhooks)

add_executable(test_async_context_linux_arm64 tests/test_async_context_linux_arm64.c)
if(LLAM_LINUX_ARM64_ASYNC_MECHANISM)
    # Deliberately independent of scheduler/I/O activation: link the actual
    # context switch and signal transport, not a replacement test switch.
    target_sources(test_async_context_linux_arm64 PRIVATE
        src/core/context/async_context_linux_arm64.c
        src/asm/linux/arm64/async_context_arm64.S
        src/asm/linux/arm64/linux_context_arm64.S
        tests/fixtures/async_darwin_leaf.S
        tests/fixtures/async_linux_arm64_entry.S)
    target_link_libraries(test_async_context_linux_arm64 PRIVATE Threads::Threads)
endif()

add_executable(test_async_context_darwin tests/test_async_context_darwin.c)
target_include_directories(test_async_context_darwin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(LLAM_DARWIN_ASYNC_MECHANISM)
    target_sources(test_async_context_darwin PRIVATE
        tests/fixtures/async_darwin_leaf.S tests/fixtures/async_darwin_entry.S)
endif()
target_compile_definitions(test_async_context_darwin PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_async_context_darwin PRIVATE llam_runtime_testhooks)


add_executable(test_darwin_arm64_async_scheduler_real tests/test_darwin_arm64_async_scheduler_real.c)
target_include_directories(test_darwin_arm64_async_scheduler_real PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_darwin_arm64_async_scheduler_real PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
if(LLAM_DARWIN_ASYNC_MECHANISM)
    target_sources(test_darwin_arm64_async_scheduler_real PRIVATE tests/fixtures/async_darwin_leaf.S)
endif()
target_link_libraries(test_darwin_arm64_async_scheduler_real PRIVATE llam_runtime_testhooks)

add_executable(test_linux_arm64_async_scheduler_real tests/test_darwin_arm64_async_scheduler_real.c)
target_include_directories(test_linux_arm64_async_scheduler_real PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(test_linux_arm64_async_scheduler_real PRIVATE
    LLAM_ENABLE_TEST_HOOKS=1 LLAM_ASYNC_SHARED_TEST_LINUX=1)
if(LLAM_LINUX_ARM64_ASYNC_MECHANISM)
    target_sources(test_linux_arm64_async_scheduler_real PRIVATE tests/fixtures/async_darwin_leaf.S)
endif()
target_link_libraries(test_linux_arm64_async_scheduler_real PRIVATE llam_runtime_testhooks)

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
target_compile_definitions(test_security_capability PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_link_libraries(test_security_capability PRIVATE llam_runtime_testhooks)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Short, explicit A/B/C/D IOCP experiments. The C/D modes share one target;
    # set LLAM_BENCH_MINIMAL=1 for C and leave it unset for validated D.
    add_executable(bench_windows_iocp_raw EXCLUDE_FROM_ALL
        experiments/windows/bench_iocp_raw.c
    )
    target_link_libraries(bench_windows_iocp_raw PRIVATE ws2_32)

    add_executable(bench_windows_iocp_engine EXCLUDE_FROM_ALL
        experiments/windows/bench_iocp_engine.c
    )
    target_include_directories(bench_windows_iocp_engine PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(bench_windows_iocp_engine PRIVATE
        _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND LLAM_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(bench_windows_iocp_engine PRIVATE llam_runtime_testhooks)

    add_executable(bench_windows_iocp_full EXCLUDE_FROM_ALL
        experiments/windows/bench_iocp_full.c
    )
    target_link_libraries(bench_windows_iocp_full PRIVATE llam_runtime)

    add_executable(bench_windows_context EXCLUDE_FROM_ALL
        experiments/windows/bench_context_windows.c
    )
    target_link_libraries(bench_windows_context PRIVATE llam_runtime)

    if(LLAM_WINDOWS_ASYNC_SCHEDULER_ORACLE AND
       LLAM_TARGET_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        enable_language(CXX)
        add_executable(test_windows_async_scheduler_oracle
            experiments/windows/llam_async_scheduler_oracle.c
            experiments/windows/llam_async_leaf.S
            experiments/windows/llam_async_native_oracle.cpp
        )
        target_include_directories(test_windows_async_scheduler_oracle PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_definitions(test_windows_async_scheduler_oracle PRIVATE
            _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
            LLAM_ENABLE_TEST_HOOKS=1
        )
        target_link_libraries(test_windows_async_scheduler_oracle PRIVATE
            llam_runtime_testhooks
        )
        set_target_properties(test_windows_async_scheduler_oracle PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
        )
        add_test(NAME test_windows_async_scheduler_oracle
                 COMMAND test_windows_async_scheduler_oracle)
        set_tests_properties(test_windows_async_scheduler_oracle PROPERTIES
            TIMEOUT 10 SKIP_RETURN_CODE 77
        )

        if(LLAM_EXPERIMENTAL_ASYNC_PREEMPT AND NOT MSVC)
            add_executable(test_windows_async_public_oracle
                experiments/windows/llam_async_public_oracle.c
                experiments/windows/llam_async_leaf.S
            )
            target_include_directories(test_windows_async_public_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_public_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_public_oracle PRIVATE
                llam_runtime
            )
            foreach(LLAM_WINDOWS_PUBLIC_MODE valid negatives)
                add_test(NAME test_windows_async_public_${LLAM_WINDOWS_PUBLIC_MODE}
                         COMMAND test_windows_async_public_oracle)
                set_tests_properties(
                    test_windows_async_public_${LLAM_WINDOWS_PUBLIC_MODE}
                    PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77
                    ENVIRONMENT "LLAM_ASYNC_PUBLIC_MODE=${LLAM_WINDOWS_PUBLIC_MODE}"
                )
            endforeach()

            add_executable(test_windows_async_config_oracle
                experiments/windows/llam_async_config_oracle.c
                experiments/windows/llam_async_leaf.S
            )
            target_include_directories(test_windows_async_config_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_config_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_ENABLE_TEST_HOOKS=1 LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_config_oracle PRIVATE
                llam_runtime_testhooks
            )
            foreach(LLAM_WINDOWS_CONFIG_MODE spawn-first enable-first enable-destroy run-first foreign-trylock start-failure delayed-controller public-spawn-first public-enable-first public-enable-destroy public-managed-refusal)
                add_test(NAME test_windows_async_config_${LLAM_WINDOWS_CONFIG_MODE}
                         COMMAND test_windows_async_config_oracle)
                set_tests_properties(
                    test_windows_async_config_${LLAM_WINDOWS_CONFIG_MODE}
                    PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77
                    ENVIRONMENT "LLAM_ASYNC_CONFIG_MODE=${LLAM_WINDOWS_CONFIG_MODE}"
                )
            endforeach()
            set_tests_properties(test_windows_async_config_delayed-controller
                                 PROPERTIES TIMEOUT 20)

            add_executable(test_windows_async_lifetime_oracle
                experiments/windows/llam_async_scheduler_oracle.c
                experiments/windows/llam_async_leaf.S
                experiments/windows/llam_async_native_oracle.cpp
            )
            target_include_directories(test_windows_async_lifetime_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_lifetime_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_ENABLE_TEST_HOOKS=1 LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
                LLAM_WINDOWS_ASYNC_LIFETIME_ORACLE=1
            )
            target_link_libraries(test_windows_async_lifetime_oracle PRIVATE
                llam_runtime_testhooks
            )
            set_target_properties(test_windows_async_lifetime_oracle PROPERTIES
                CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
            )
            add_test(NAME test_windows_async_lifetime_oracle
                     COMMAND test_windows_async_lifetime_oracle)
            set_tests_properties(test_windows_async_lifetime_oracle PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77
            )
            foreach(LLAM_WINDOWS_LIFETIME_MODE peer taskscan stopdeferred)
                add_test(NAME test_windows_async_lifetime_${LLAM_WINDOWS_LIFETIME_MODE}
                         COMMAND test_windows_async_lifetime_oracle)
                set_tests_properties(
                    test_windows_async_lifetime_${LLAM_WINDOWS_LIFETIME_MODE}
                    PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77
                    ENVIRONMENT "LLAM_LIFETIME_MODE=${LLAM_WINDOWS_LIFETIME_MODE}"
                )
            endforeach()

            add_executable(test_windows_async_lifetime_never_started
                experiments/windows/llam_async_lifetime_never_started.c
            )
            target_include_directories(test_windows_async_lifetime_never_started PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_lifetime_never_started PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_lifetime_never_started PRIVATE
                llam_runtime_testhooks
            )
            add_test(NAME test_windows_async_lifetime_never_started
                     COMMAND test_windows_async_lifetime_never_started)
            set_tests_properties(test_windows_async_lifetime_never_started PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77
            )

            add_executable(test_windows_async_lifetime_refusal_race
                experiments/windows/llam_async_lifetime_refusal_race.c
            )
            target_include_directories(test_windows_async_lifetime_refusal_race PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_lifetime_refusal_race PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_lifetime_refusal_race PRIVATE
                llam_runtime_testhooks
            )
            add_test(NAME test_windows_async_lifetime_refusal_race
                     COMMAND test_windows_async_lifetime_refusal_race)
            set_tests_properties(test_windows_async_lifetime_refusal_race PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77
            )

            add_executable(test_windows_async_lifetime_ready_race
                experiments/windows/llam_async_lifetime_ready_race.c
            )
            target_include_directories(test_windows_async_lifetime_ready_race PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_lifetime_ready_race PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1 LLAM_ENABLE_TEST_HOOKS=1
            )
            target_link_libraries(test_windows_async_lifetime_ready_race PRIVATE
                llam_runtime_testhooks
            )
            add_test(NAME test_windows_async_lifetime_ready_race
                     COMMAND test_windows_async_lifetime_ready_race)
            set_tests_properties(test_windows_async_lifetime_ready_race PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77
            )

            add_executable(test_windows_async_lifetime_failfast
                experiments/windows/llam_async_lifetime_failfast_parent.c
            )
            add_test(NAME test_windows_async_lifetime_failfast
                     COMMAND test_windows_async_lifetime_failfast
                             $<TARGET_FILE:test_windows_async_lifetime_oracle>)
            set_tests_properties(test_windows_async_lifetime_failfast PROPERTIES
                TIMEOUT 20 SKIP_RETURN_CODE 77
            )

            add_executable(test_windows_async_safe_pc_oracle
                experiments/windows/llam_async_safe_pc_oracle.c
                experiments/windows/llam_async_safe_regions.S
            )
            target_include_directories(test_windows_async_safe_pc_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_safe_pc_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_safe_pc_oracle PRIVATE
                llam_runtime
            )
            add_test(NAME test_windows_async_safe_pc_oracle
                     COMMAND test_windows_async_safe_pc_oracle)
            set_tests_properties(test_windows_async_safe_pc_oracle PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77
            )
            add_test(NAME test_windows_async_safe_pc_empty
                     COMMAND test_windows_async_safe_pc_oracle)
            set_tests_properties(test_windows_async_safe_pc_empty PROPERTIES
                TIMEOUT 10 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_SAFEPC_EMPTY=1"
            )

            add_executable(test_windows_async_transaction_oracle
                experiments/windows/llam_async_transaction_oracle.c
                experiments/windows/llam_async_leaf.S
                experiments/windows/llam_async_dual_leaf.S
            )
            target_include_directories(test_windows_async_transaction_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_transaction_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_transaction_oracle PRIVATE
                llam_runtime
            )
            add_test(NAME test_windows_async_transaction_oracle
                     COMMAND test_windows_async_transaction_oracle)
            set_tests_properties(test_windows_async_transaction_oracle PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
            )
            add_test(NAME test_windows_async_transaction_stop_gate
                     COMMAND test_windows_async_transaction_oracle)
            set_tests_properties(test_windows_async_transaction_stop_gate PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_TRANSACTION_STOP_GATE=1"
            )
            add_test(NAME test_windows_async_transaction_chain
                     COMMAND test_windows_async_transaction_oracle)
            set_tests_properties(test_windows_async_transaction_chain PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_TRANSACTION_CHAIN=1"
            )
            add_test(NAME test_windows_async_transaction_unsafe_witness
                     COMMAND test_windows_async_transaction_oracle)
            set_tests_properties(test_windows_async_transaction_unsafe_witness PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_TRANSACTION_UNSAFE_WITNESS=1"
            )
            add_test(NAME test_windows_async_transaction_automatic
                     COMMAND test_windows_async_transaction_oracle)
            set_tests_properties(test_windows_async_transaction_automatic PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_TRANSACTION_AUTOMATIC=1"
            )
            add_test(NAME test_windows_async_transaction_auto_stop
                     COMMAND test_windows_async_transaction_oracle)
            set_tests_properties(test_windows_async_transaction_auto_stop PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_TRANSACTION_AUTOMATIC_STOP=1"
            )

            add_executable(test_windows_async_auto_refusal
                experiments/windows/llam_async_auto_refusal_oracle.c
                experiments/windows/llam_async_dual_leaf.S
            )
            target_include_directories(test_windows_async_auto_refusal PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_auto_refusal PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_auto_refusal PRIVATE
                llam_runtime
            )
            add_test(NAME test_windows_async_auto_unsafe
                     COMMAND test_windows_async_auto_refusal)
            set_tests_properties(test_windows_async_auto_unsafe PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
            )
            add_test(NAME test_windows_async_auto_empty
                     COMMAND test_windows_async_auto_refusal)
            set_tests_properties(test_windows_async_auto_empty PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_AUTO_EMPTY=1"
            )
            add_test(NAME test_windows_async_auto_no_preempt
                     COMMAND test_windows_async_auto_refusal)
            set_tests_properties(test_windows_async_auto_no_preempt PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_AUTO_NO_PREEMPT=1"
            )

            add_library(test_windows_async_image SHARED
                experiments/windows/llam_async_image_leaf.c
                experiments/windows/llam_async_image_leaf.S
            )
            add_library(test_windows_async_image_absent SHARED
                experiments/windows/llam_async_image_leaf.c
                experiments/windows/llam_async_image_leaf.S
            )
            target_compile_definitions(test_windows_async_image_absent PRIVATE
                LLAM_PE_METADATA_ABSENT=1
            )
            add_library(test_windows_async_image_bad SHARED
                experiments/windows/llam_async_image_leaf.c
                experiments/windows/llam_async_image_leaf.S
            )
            target_compile_definitions(test_windows_async_image_bad PRIVATE
                LLAM_PE_METADATA_BAD_MAGIC=1
            )
            foreach(LLAM_PE_BAD_VARIANT bad_version bad_length bad_rel overlap)
                add_library(test_windows_async_image_${LLAM_PE_BAD_VARIANT} SHARED
                    experiments/windows/llam_async_image_leaf.c
                    experiments/windows/llam_async_image_leaf.S
                )
                string(TOUPPER ${LLAM_PE_BAD_VARIANT} LLAM_PE_BAD_MACRO)
                target_compile_definitions(
                    test_windows_async_image_${LLAM_PE_BAD_VARIANT} PRIVATE
                    LLAM_PE_METADATA_${LLAM_PE_BAD_MACRO}=1
                )
            endforeach()
            add_executable(test_windows_async_pe_metadata_oracle
                experiments/windows/llam_async_pe_metadata_oracle.c
            )
            target_include_directories(test_windows_async_pe_metadata_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_pe_metadata_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_pe_metadata_oracle PRIVATE
                llam_runtime
            )
            # The compiler-certified fixture is built separately with the
            # pinned LLVM/LLD profile; never synthesize a hand-authored LLPM
            # replacement in this runtime integration oracle.
            add_executable(test_windows_async_certified_runtime_oracle
                EXCLUDE_FROM_ALL
                experiments/windows/llam_async_certified_runtime_oracle.c
            )
            target_include_directories(
                test_windows_async_certified_runtime_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(
                test_windows_async_certified_runtime_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_ENABLE_TEST_HOOKS=1 LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(
                test_windows_async_certified_runtime_oracle PRIVATE
                llam_runtime_testhooks
            )
            set(LLAM_WINDOWS_ASYNC_CERTIFIED_DLL "" CACHE FILEPATH
                "Optional pinned LLVM/LLD-generated Win64 LLPM fixture DLL")
            add_executable(test_windows_async_multim_oracle EXCLUDE_FROM_ALL
                experiments/windows/llam_async_multim_oracle.c)
            target_include_directories(test_windows_async_multim_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_compile_definitions(test_windows_async_multim_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_ENABLE_TEST_HOOKS=1 LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1)
            target_link_libraries(test_windows_async_multim_oracle PRIVATE
                llam_runtime_testhooks)
            if(LLAM_WINDOWS_ASYNC_CERTIFIED_DLL)
                foreach(LLAM_MM_MODE positive overlap migration owners3 delayed repeat stop destroy startup-stop alloc1 create2 dup0 dup2 probe2)
                    add_test(NAME test_windows_async_multim_${LLAM_MM_MODE}
                        COMMAND test_windows_async_multim_oracle ${LLAM_MM_MODE}
                                ${LLAM_WINDOWS_ASYNC_CERTIFIED_DLL})
                    set_tests_properties(test_windows_async_multim_${LLAM_MM_MODE}
                        PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
                endforeach()
                foreach(LLAM_CERTIFIED_MODE positive mixed ready-stop ready-destroy capture-late disabled ordinary precommit)
                    add_test(
                        NAME test_windows_async_certified_${LLAM_CERTIFIED_MODE}
                        COMMAND test_windows_async_certified_runtime_oracle
                                ${LLAM_CERTIFIED_MODE}
                                ${LLAM_WINDOWS_ASYNC_CERTIFIED_DLL}
                    )
                    set_tests_properties(
                        test_windows_async_certified_${LLAM_CERTIFIED_MODE}
                        PROPERTIES TIMEOUT 12 SKIP_RETURN_CODE 77
                    )
                endforeach()
            endif()
            add_dependencies(test_windows_async_pe_metadata_oracle
                test_windows_async_image
                test_windows_async_image_absent
                test_windows_async_image_bad
                test_windows_async_image_bad_version
                test_windows_async_image_bad_length
                test_windows_async_image_bad_rel
                test_windows_async_image_overlap
            )
            add_test(NAME test_windows_async_pe_positive
                     COMMAND test_windows_async_pe_metadata_oracle positive
                             $<TARGET_FILE:test_windows_async_image>)
            add_test(NAME test_windows_async_pe_absent
                     COMMAND test_windows_async_pe_metadata_oracle absent
                             $<TARGET_FILE:test_windows_async_image_absent>)
            add_test(NAME test_windows_async_pe_malformed
                     COMMAND test_windows_async_pe_metadata_oracle malformed
                             $<TARGET_FILE:test_windows_async_image_bad>)
            add_test(NAME test_windows_async_pe_bad_version
                     COMMAND test_windows_async_pe_metadata_oracle unsupported
                             $<TARGET_FILE:test_windows_async_image_bad_version>)
            add_test(NAME test_windows_async_pe_bad_length
                     COMMAND test_windows_async_pe_metadata_oracle malformed
                             $<TARGET_FILE:test_windows_async_image_bad_length>)
            add_test(NAME test_windows_async_pe_bad_rel
                     COMMAND test_windows_async_pe_metadata_oracle malformed
                             $<TARGET_FILE:test_windows_async_image_bad_rel>)
            add_test(NAME test_windows_async_pe_overlap
                     COMMAND test_windows_async_pe_metadata_oracle malformed
                             $<TARGET_FILE:test_windows_async_image_overlap>)
            add_test(NAME test_windows_async_pe_invalid_handle
                     COMMAND test_windows_async_pe_metadata_oracle invalid
                             $<TARGET_FILE:test_windows_async_image>)
            add_test(NAME test_windows_async_pe_pin
                     COMMAND test_windows_async_pe_metadata_oracle pin
                             $<TARGET_FILE:test_windows_async_image>)
            add_test(NAME test_windows_async_pe_handoff
                     COMMAND test_windows_async_pe_metadata_oracle handoff
                             $<TARGET_FILE:test_windows_async_image>)
            set_tests_properties(
                test_windows_async_pe_positive
                test_windows_async_pe_absent
                test_windows_async_pe_malformed
                test_windows_async_pe_bad_version
                test_windows_async_pe_bad_length
                test_windows_async_pe_bad_rel
                test_windows_async_pe_overlap
                test_windows_async_pe_invalid_handle
                test_windows_async_pe_pin
                test_windows_async_pe_handoff
                PROPERTIES TIMEOUT 12 SKIP_RETURN_CODE 77
            )
            add_executable(test_windows_async_image_pin
                experiments/windows/llam_async_image_pin_oracle.c
            )
            target_include_directories(test_windows_async_image_pin PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_image_pin PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
            )
            target_link_libraries(test_windows_async_image_pin PRIVATE
                llam_runtime
            )
            add_dependencies(test_windows_async_image_pin
                test_windows_async_image)
            add_test(NAME test_windows_async_image_pin
                     COMMAND test_windows_async_image_pin
                             $<TARGET_FILE:test_windows_async_image>)
            set_tests_properties(test_windows_async_image_pin PROPERTIES
                TIMEOUT 12 SKIP_RETURN_CODE 77
            )

            add_executable(test_windows_async_destroy_oracle
                experiments/windows/llam_async_destroy_oracle.c
                experiments/windows/llam_async_leaf.S
            )
            target_include_directories(test_windows_async_destroy_oracle PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
                ${CMAKE_CURRENT_SOURCE_DIR}/src
            )
            target_compile_definitions(test_windows_async_destroy_oracle PRIVATE
                _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
                LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1 LLAM_ENABLE_TEST_HOOKS=1
            )
            target_link_libraries(test_windows_async_destroy_oracle PRIVATE
                llam_runtime_testhooks
            )
            foreach(LLAM_WINDOWS_DESTROY_PHASE RANGE 0 4)
                add_test(NAME test_windows_async_destroy_phase_${LLAM_WINDOWS_DESTROY_PHASE}
                         COMMAND test_windows_async_destroy_oracle)
                set_tests_properties(
                    test_windows_async_destroy_phase_${LLAM_WINDOWS_DESTROY_PHASE}
                    PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
                    ENVIRONMENT "LLAM_ASYNC_DESTROY_PHASE=${LLAM_WINDOWS_DESTROY_PHASE}"
                )
            endforeach()
        endif()

        add_executable(test_windows_async_two_capsules_oracle
            experiments/windows/llam_async_two_capsules_oracle.c
            experiments/windows/llam_async_pair_leaf.S
        )
        target_include_directories(test_windows_async_two_capsules_oracle PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_definitions(test_windows_async_two_capsules_oracle PRIVATE
            _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND
            LLAM_ENABLE_TEST_HOOKS=1
        )
        target_link_libraries(test_windows_async_two_capsules_oracle PRIVATE
            llam_runtime_testhooks
        )
        add_test(NAME test_windows_async_two_capsules_oracle
                 COMMAND test_windows_async_two_capsules_oracle)
        set_tests_properties(test_windows_async_two_capsules_oracle PROPERTIES
            TIMEOUT 10 SKIP_RETURN_CODE 77
        )
        foreach(LLAM_WINDOWS_ASYNC_STOP_PHASE capture1 gate postrestore)
            add_test(NAME test_windows_async_two_capsules_stop_${LLAM_WINDOWS_ASYNC_STOP_PHASE}
                     COMMAND test_windows_async_two_capsules_oracle)
            set_tests_properties(
                test_windows_async_two_capsules_stop_${LLAM_WINDOWS_ASYNC_STOP_PHASE}
                PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77
                ENVIRONMENT "LLAM_PAIR_STOP_PHASE=${LLAM_WINDOWS_ASYNC_STOP_PHASE}"
            )
        endforeach()
    endif()

    add_executable(test_windows_iocp_io
        tests/test_windows_iocp_io.c
    )
    target_include_directories(test_windows_iocp_io PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_windows_iocp_io PRIVATE _GNU_SOURCE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    target_link_libraries(test_windows_iocp_io PRIVATE llam_runtime)

    add_executable(test_windows_handle_io
        tests/test_windows_handle_io.c
    )
    target_include_directories(test_windows_handle_io PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_windows_handle_io PRIVATE _GNU_SOURCE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    target_link_libraries(test_windows_handle_io PRIVATE llam_runtime)

    add_executable(test_windows_iocp_dump
        tests/test_windows_iocp_dump.c
    )
    target_include_directories(test_windows_iocp_dump PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(test_windows_iocp_dump PRIVATE _GNU_SOURCE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    target_link_libraries(test_windows_iocp_dump PRIVATE llam_runtime)
endif()

add_executable(test_shared_load
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
        test_api_usability
        test_runtime_api_edges
        test_runtime_select_edges
        test_runtime_io_dump
        test_runtime_group_local_edges
        test_runtime_unmanaged_join
        test_runtime_stress
        test_runtime_fuzz
        test_runtime_invariants
        test_runtime_regressions
        test_runtime_shutdown_internal
        test_watchdog_liveness_internal
        test_worker_identity_internal
        test_legacy_lease_internal
        test_execution_lease_internal
        test_native_domain_internal
        test_managed_scheduler_internal
        test_managed_catalog_internal
        test_managed_foreign_internal
        test_capsule_pool_internal
        test_managed_async_registered
        test_managed_linux_compiler
        test_managed_async_linux_x86_64
        test_processor_resource_internal
        test_processor_queue_timer_internal
        test_processor_allocator_io_internal
        test_foreign_detach_internal
        test_relief_worker_internal
        test_preempt_capabilities
        test_default_policy_internal
        test_magic2_internal
        bench_handoff
        bench_select_prepare
        bench_watch_index
        test_handoff_protocol
        test_large_select
        test_io_watch_index
        test_async_preempt_linux_x86_64
        test_async_preempt_multi_linux_x86_64
        test_async_context_linux_arm64
        test_async_context_darwin
        test_darwin_arm64_async_scheduler_real
        test_linux_arm64_async_scheduler_real
        test_preempt_metadata
        test_preempt_metadata_v2
        test_preempt_metadata_registration
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
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_compile_definitions(${LLAM_TEST_TARGET} PRIVATE _WIN32_WINNT=0x0A00 LLAM_ENABLE_WINDOWS_BACKEND)
    endif()
endforeach()

add_test(NAME test_abi_contract COMMAND test_abi_contract)
add_test(NAME test_abi_compat COMMAND test_abi_compat)
add_test(NAME test_runtime_core COMMAND test_runtime_core)
add_test(NAME test_multi_runtime_core COMMAND test_multi_runtime_core)
add_test(NAME test_api_usability COMMAND test_api_usability)
set_tests_properties(test_api_usability PROPERTIES TIMEOUT 30)
add_test(NAME test_runtime_select_edges COMMAND test_runtime_select_edges)
add_test(NAME test_runtime_group_local_edges COMMAND test_runtime_group_local_edges)
add_test(NAME test_runtime_unmanaged_join COMMAND test_runtime_unmanaged_join)
add_test(NAME test_runtime_stress COMMAND test_runtime_stress)
add_test(NAME test_runtime_fuzz COMMAND test_runtime_fuzz)
add_test(NAME test_runtime_invariants COMMAND test_runtime_invariants)
add_test(NAME test_runtime_regressions COMMAND test_runtime_regressions)
set_tests_properties(test_runtime_regressions PROPERTIES TIMEOUT 30)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_sdk_install
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_sdk_install.py
            --cmake ${CMAKE_COMMAND} --build-dir ${CMAKE_CURRENT_BINARY_DIR} --config $<CONFIG>)
endif()
add_test(NAME test_runtime_shutdown_internal COMMAND test_runtime_shutdown_internal)
add_test(NAME test_watchdog_liveness_internal COMMAND test_watchdog_liveness_internal)
add_test(NAME test_watchdog_liveness_lazy COMMAND test_watchdog_liveness_internal --lazy)
add_test(NAME test_watchdog_wait_reference COMMAND test_watchdog_liveness_internal --wait-reference)
set_tests_properties(test_watchdog_wait_reference PROPERTIES TIMEOUT 20)
add_test(NAME test_watchdog_liveness_async_dequeue COMMAND test_watchdog_liveness_internal --async-dequeue)
add_test(NAME test_watchdog_liveness_async_return COMMAND test_watchdog_liveness_internal --async-return)
add_test(NAME test_watchdog_liveness_parked COMMAND test_watchdog_liveness_internal --parked)
add_test(NAME test_watchdog_liveness_async_parked COMMAND test_watchdog_liveness_internal --async-parked)
set_tests_properties(test_watchdog_liveness_internal test_watchdog_liveness_lazy
    test_watchdog_liveness_async_dequeue test_watchdog_liveness_async_return
    test_watchdog_liveness_parked test_watchdog_liveness_async_parked
    PROPERTIES TIMEOUT 12 SKIP_RETURN_CODE 77)
add_test(NAME test_worker_identity_internal COMMAND test_worker_identity_internal)
add_test(NAME test_legacy_lease_internal COMMAND test_legacy_lease_internal)
add_test(NAME test_execution_lease_internal COMMAND test_execution_lease_internal)
add_test(NAME test_native_domain_internal COMMAND test_native_domain_internal)
add_test(NAME test_managed_scheduler_internal COMMAND test_managed_scheduler_internal)
add_test(NAME test_managed_scheduler_async_affine COMMAND test_managed_scheduler_internal --async-affine)
add_test(NAME test_managed_scheduler_async_bookkeeping COMMAND test_managed_scheduler_internal --async-bookkeeping)
set_tests_properties(test_managed_scheduler_async_bookkeeping PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
set_tests_properties(test_managed_scheduler_async_affine PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
add_test(NAME test_managed_catalog_internal COMMAND test_managed_catalog_internal)
add_test(NAME test_managed_foreign_internal COMMAND test_managed_foreign_internal)
add_test(NAME test_capsule_pool_internal COMMAND test_capsule_pool_internal)
set_tests_properties(test_capsule_pool_internal PROPERTIES TIMEOUT 20)
set_tests_properties(test_managed_foreign_internal PROPERTIES TIMEOUT 30)
set_tests_properties(test_managed_scheduler_internal test_managed_catalog_internal PROPERTIES TIMEOUT 20)
add_test(NAME test_managed_async_registered COMMAND test_managed_async_registered)
add_test(NAME test_managed_async_linux_x86_64 COMMAND test_managed_async_linux_x86_64)
add_test(NAME test_managed_async_linux_x86_64_multi_p COMMAND test_managed_async_linux_x86_64 --multi-p)
add_test(NAME test_managed_async_linux_x86_64_oversubscribed_p COMMAND test_managed_async_linux_x86_64 --oversubscribed-p)
add_test(NAME test_managed_async_linux_x86_64_dynamic COMMAND test_managed_async_linux_x86_64 --dynamic)
foreach(LLAM_LINUX_REGISTERED_MODE auto startup-abort native-affine native-affine-auto)
    string(REPLACE "-" "_" LLAM_LINUX_REGISTERED_NAME "${LLAM_LINUX_REGISTERED_MODE}")
    add_test(NAME test_managed_async_linux_x86_64_${LLAM_LINUX_REGISTERED_NAME}
        COMMAND test_managed_async_linux_x86_64 --${LLAM_LINUX_REGISTERED_MODE})
    set_tests_properties(test_managed_async_linux_x86_64_${LLAM_LINUX_REGISTERED_NAME}
        PROPERTIES TIMEOUT 75 SKIP_RETURN_CODE 77)
endforeach()
set_tests_properties(test_managed_async_linux_x86_64 test_managed_async_linux_x86_64_multi_p
    test_managed_async_linux_x86_64_oversubscribed_p test_managed_async_linux_x86_64_dynamic
    PROPERTIES TIMEOUT 35 SKIP_RETURN_CODE 77)
add_test(NAME test_managed_async_registered_auto COMMAND test_managed_async_registered --auto)
add_test(NAME test_managed_async_registered_multi_p COMMAND test_managed_async_registered --multi-p)
add_test(NAME test_managed_async_registered_dynamic COMMAND test_managed_async_registered --dynamic)
add_test(NAME test_managed_async_registered_startup_abort COMMAND test_managed_async_registered --startup-abort)
set_tests_properties(test_managed_async_registered test_managed_async_registered_auto test_managed_async_registered_multi_p
    test_managed_async_registered_startup_abort test_managed_async_registered_dynamic PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
set_tests_properties(test_native_domain_internal PROPERTIES TIMEOUT 12)
if(Python3_FOUND)
    add_test(NAME test_native_domain_boundary
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_native_domain_boundary.py
            --binary $<TARGET_FILE:test_native_domain_internal>)
    set_tests_properties(test_native_domain_boundary PROPERTIES TIMEOUT 180)
endif()
set_tests_properties(test_execution_lease_internal PROPERTIES TIMEOUT 20)
set_tests_properties(test_legacy_lease_internal PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME test_processor_resource_internal COMMAND test_processor_resource_internal)
add_test(NAME test_processor_queue_timer_internal COMMAND test_processor_queue_timer_internal)
set_tests_properties(test_processor_queue_timer_internal PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME test_processor_allocator_io_internal COMMAND test_processor_allocator_io_internal)
set_tests_properties(test_processor_allocator_io_internal PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME test_foreign_detach_internal COMMAND test_foreign_detach_internal)
add_test(NAME test_relief_worker_internal COMMAND test_relief_worker_internal)
add_test(NAME test_preempt_capabilities COMMAND test_preempt_capabilities)
add_test(NAME test_default_policy_internal COMMAND test_default_policy_internal)
add_test(NAME test_magic2_internal COMMAND test_magic2_internal)
set_tests_properties(test_magic2_internal PROPERTIES TIMEOUT 45)
add_test(NAME test_async_preempt_linux_x86_64 COMMAND test_async_preempt_linux_x86_64)
add_test(NAME test_async_preempt_xstate_policy COMMAND test_async_preempt_linux_x86_64 --frame-only)
set_tests_properties(test_async_preempt_xstate_policy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 10)
set_tests_properties(test_async_preempt_linux_x86_64 PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME test_async_preempt_linux_signal_namespace
    COMMAND test_async_preempt_linux_x86_64 --signal-namespace)
set_tests_properties(test_async_preempt_linux_signal_namespace PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 35)
add_test(NAME test_async_preempt_linux_signal_namespace_faults
    COMMAND test_async_preempt_linux_x86_64 --signal-namespace-faults)
set_tests_properties(test_async_preempt_linux_signal_namespace_faults PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 35)
add_test(NAME test_async_preempt_multi_linux_x86_64 COMMAND test_async_preempt_multi_linux_x86_64)
set_tests_properties(test_async_preempt_multi_linux_x86_64 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
add_test(NAME test_async_context_linux_arm64 COMMAND test_async_context_linux_arm64)
set_tests_properties(test_async_context_linux_arm64 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 12)
add_test(NAME test_async_context_darwin COMMAND test_async_context_darwin)
set_tests_properties(test_async_context_darwin PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 10)
add_test(NAME test_darwin_arm64_async_scheduler_real COMMAND test_darwin_arm64_async_scheduler_real)
add_test(NAME test_linux_arm64_async_scheduler_real COMMAND test_linux_arm64_async_scheduler_real)
set_tests_properties(test_linux_arm64_async_scheduler_real PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 12)
foreach(LLAM_SIGNAL_CASE disabled repeat capture-late restore-early-exit ready-drain offload
        startup-abort configuration isolation task-reader witness-late pending-signal observed-cooperative
        capture-handler-tail restore-handler-tail witness-armed-return capture-armed-cancel ready-destroy
        signal-namespace signal-blocked-start signal-owner-isolation)
    string(REPLACE "-" "_" LLAM_SIGNAL_CASE_NAME "${LLAM_SIGNAL_CASE}")
    add_test(NAME test_linux_arm64_async_${LLAM_SIGNAL_CASE_NAME}
        COMMAND test_linux_arm64_async_scheduler_real --${LLAM_SIGNAL_CASE})
    set_tests_properties(test_linux_arm64_async_${LLAM_SIGNAL_CASE_NAME}
        PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 12)
endforeach()
add_test(NAME test_darwin_arm64_async_ready_destroy
    COMMAND test_darwin_arm64_async_scheduler_real --ready-destroy)
set_tests_properties(test_darwin_arm64_async_ready_destroy PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 12)
add_test(NAME test_darwin_arm64_async_scheduler_disabled COMMAND test_darwin_arm64_async_scheduler_real --disabled)
add_test(NAME test_darwin_arm64_async_scheduler_repeat COMMAND test_darwin_arm64_async_scheduler_real --repeat)
add_test(NAME test_darwin_arm64_async_capture_late COMMAND test_darwin_arm64_async_scheduler_real --capture-late)
add_test(NAME test_darwin_arm64_async_restore_early_exit COMMAND test_darwin_arm64_async_scheduler_real --restore-early-exit)
add_test(NAME test_darwin_arm64_async_ready_drain COMMAND test_darwin_arm64_async_scheduler_real --ready-drain)
add_test(NAME test_darwin_arm64_async_offload COMMAND test_darwin_arm64_async_scheduler_real --offload)
add_test(NAME test_darwin_arm64_async_startup_abort COMMAND test_darwin_arm64_async_scheduler_real --startup-abort)
add_test(NAME test_darwin_arm64_async_configuration COMMAND test_darwin_arm64_async_scheduler_real --configuration)
add_test(NAME test_darwin_arm64_async_isolation COMMAND test_darwin_arm64_async_scheduler_real --isolation)
add_test(NAME test_darwin_arm64_async_task_reader COMMAND test_darwin_arm64_async_scheduler_real --task-reader)
add_test(NAME test_darwin_arm64_async_witness_late COMMAND test_darwin_arm64_async_scheduler_real --witness-late)
add_test(NAME test_darwin_arm64_async_pending_signal COMMAND test_darwin_arm64_async_scheduler_real --pending-signal)
add_test(NAME test_darwin_arm64_async_witness_read_refusal
    COMMAND test_darwin_arm64_async_scheduler_real --witness-read-refusal)
add_test(NAME test_darwin_arm64_async_probe_publication
    COMMAND test_darwin_arm64_async_scheduler_real --probe-publication)
add_test(NAME test_darwin_arm64_async_observed_cooperative COMMAND test_darwin_arm64_async_scheduler_real --observed-cooperative)
set_tests_properties(test_darwin_arm64_async_scheduler_real test_darwin_arm64_async_scheduler_disabled
    test_darwin_arm64_async_scheduler_repeat test_darwin_arm64_async_capture_late
    test_darwin_arm64_async_restore_early_exit test_darwin_arm64_async_ready_drain
    test_darwin_arm64_async_offload test_darwin_arm64_async_startup_abort
    test_darwin_arm64_async_configuration test_darwin_arm64_async_isolation
    test_darwin_arm64_async_task_reader test_darwin_arm64_async_witness_late
    test_darwin_arm64_async_pending_signal
    test_darwin_arm64_async_witness_read_refusal
    test_darwin_arm64_async_probe_publication
    test_darwin_arm64_async_observed_cooperative
    PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 12)
if(LLAM_DARWIN_ASYNC_MECHANISM)
    foreach(LLAM_DARWIN_FAILURE neon general resume)
        add_test(NAME test_async_context_darwin_fail_${LLAM_DARWIN_FAILURE}
            COMMAND test_async_context_darwin --fail-${LLAM_DARWIN_FAILURE})
        set_tests_properties(test_async_context_darwin_fail_${LLAM_DARWIN_FAILURE}
            PROPERTIES TIMEOUT 10)
    endforeach()
endif()
add_test(NAME test_preempt_metadata COMMAND test_preempt_metadata)
add_test(NAME test_preempt_metadata_v2 COMMAND test_preempt_metadata_v2)
add_executable(test_managed_image_internal tests/test_managed_image_internal.c)
target_include_directories(test_managed_image_internal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt)
target_link_libraries(test_managed_image_internal PRIVATE llam_runtime)
target_compile_definitions(test_managed_image_internal PRIVATE _GNU_SOURCE)
add_executable(test_managed_async_compiler tests/test_managed_async_compiler.c)
target_include_directories(test_managed_async_compiler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
target_compile_definitions(test_managed_async_compiler PRIVATE LLAM_ENABLE_TEST_HOOKS=1)
target_compile_definitions(test_managed_async_compiler PRIVATE _GNU_SOURCE)
target_link_libraries(test_managed_async_compiler PRIVATE llam_runtime_testhooks)
foreach(LLAM_MANAGED_CONSUMER test_managed_image_internal test_managed_async_compiler)
    if(APPLE)
        target_compile_definitions(${LLAM_MANAGED_CONSUMER} PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
        target_link_options(${LLAM_MANAGED_CONSUMER} PRIVATE -Wl,-export_dynamic)
    elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        target_link_options(${LLAM_MANAGED_CONSUMER} PRIVATE -Wl,--export-dynamic)
    endif()
endforeach()
if(LLAM_MANAGED_PLUGIN_ACTIVE AND Python3_FOUND)
    add_test(NAME test_managed_compiler
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_managed_compiler.py
            --plugin $<TARGET_FILE:llam_managed_pass> --llvm-bin ${LLVM_TOOLS_BINARY_DIR})
    set_tests_properties(test_managed_compiler PROPERTIES TIMEOUT 180)
    add_test(NAME test_managed_runtime_contracts
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_managed_runtime_contracts.py
            --plugin $<TARGET_FILE:llam_managed_pass> --llvm-bin ${LLVM_TOOLS_BINARY_DIR})
    set_tests_properties(test_managed_runtime_contracts PROPERTIES TIMEOUT 180)
    if(UNIX AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME test_managed_sdk
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_managed_sdk.py
                --build-dir ${CMAKE_CURRENT_BINARY_DIR} --llvm-bin ${LLVM_TOOLS_BINARY_DIR}
                --runtime $<TARGET_FILE:llam_runtime_shared> --cmake ${CMAKE_COMMAND}
                --config $<CONFIG>)
        set_tests_properties(test_managed_sdk PROPERTIES TIMEOUT 180)
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(LLAM_MANAGED_IMAGE_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/managed-image-fixture${CMAKE_SHARED_LIBRARY_SUFFIX}")
        set(LLAM_MANAGED_AFFINE_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/managed-affine-fixture${CMAKE_SHARED_LIBRARY_SUFFIX}")
        set(LLAM_MANAGED_IMAGE_LINK_FLAGS -shared)
        set(LLAM_MANAGED_IMAGE_TARGET_FLAGS)
        if(CMAKE_C_COMPILER_TARGET)
            list(APPEND LLAM_MANAGED_IMAGE_TARGET_FLAGS "--target=${CMAKE_C_COMPILER_TARGET}")
        elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
            list(APPEND LLAM_MANAGED_IMAGE_TARGET_FLAGS -arch ${CMAKE_OSX_ARCHITECTURES})
        endif()
        if(APPLE)
            # Keep the loader destructor in the native dyld teardown table;
            # no managed task calls an implicit __cxa_atexit registration stub.
            set(LLAM_MANAGED_IMAGE_LINK_FLAGS -dynamiclib -Wl,-undefined,dynamic_lookup
                -fno-register-global-dtors-with-atexit)
        endif()
        foreach(LLAM_FIXTURE_KIND image affine)
            set(LLAM_FIXTURE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/managed-${LLAM_FIXTURE_KIND}-fixture${CMAKE_SHARED_LIBRARY_SUFFIX}")
            set(LLAM_FIXTURE_DEFINITIONS)
            if(LLAM_FIXTURE_KIND STREQUAL "affine")
                set(LLAM_FIXTURE_DEFINITIONS -DLLAM_MANAGED_AFFINE_FIXTURE=1)
            endif()
            add_custom_command(OUTPUT "${LLAM_FIXTURE_OUTPUT}"
                COMMAND ${CMAKE_COMMAND} -E env
                    "LLAM_LLVM_BIN=${LLVM_TOOLS_BINARY_DIR}"
                    "LLAM_MANAGED_PLUGIN=$<TARGET_FILE:llam_managed_pass>"
                    "PYTHONDONTWRITEBYTECODE=1"
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt/managed_driver.py
                    --llam-managed=required ${LLAM_MANAGED_IMAGE_TARGET_FLAGS}
                    ${LLAM_MANAGED_IMAGE_LINK_FLAGS} ${LLAM_FIXTURE_DEFINITIONS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/managed/image_library.c
                    -o "${LLAM_FIXTURE_OUTPUT}"
                DEPENDS llam_managed_pass tests/fixtures/managed/image_library.c
                    tests/fixtures/managed/cpu_workload.h
                    tools/preempt/managed_driver.py tools/preempt/managed_verify.py
                    tools/preempt/managed_runtime_contracts.def VERBATIM)
        endforeach()
        add_custom_target(managed_image_fixture DEPENDS "${LLAM_MANAGED_IMAGE_FIXTURE}" "${LLAM_MANAGED_AFFINE_FIXTURE}")
        set(LLAM_MANAGED_BENCH_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/managed-bench-workload${CMAKE_SHARED_LIBRARY_SUFFIX}")
        add_custom_command(OUTPUT "${LLAM_MANAGED_BENCH_IMAGE}"
            COMMAND ${CMAKE_COMMAND} -E env
                "LLAM_LLVM_BIN=${LLVM_TOOLS_BINARY_DIR}"
                "LLAM_MANAGED_PLUGIN=$<TARGET_FILE:llam_managed_pass>"
                "PYTHONDONTWRITEBYTECODE=1"
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/preempt/managed_driver.py
                --llam-managed=required ${LLAM_MANAGED_IMAGE_TARGET_FLAGS} ${LLAM_MANAGED_IMAGE_LINK_FLAGS}
                -I${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/experiments/async_compare/managed_workload.c
                -o "${LLAM_MANAGED_BENCH_IMAGE}"
            DEPENDS llam_managed_pass experiments/async_compare/managed_workload.c
                experiments/async_compare/managed_bench.h include/llam/runtime.h include/llam/io.h
                tools/preempt/managed_driver.py tools/preempt/managed_verify.py
                tools/preempt/managed_runtime_contracts.def VERBATIM)
        add_custom_target(managed_bench_image DEPENDS "${LLAM_MANAGED_BENCH_IMAGE}")
        add_executable(bench_managed_preempt EXCLUDE_FROM_ALL experiments/async_compare/bench_managed.c)
        if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(bench_managed_preempt PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
        target_include_directories(bench_managed_preempt PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/internal)
        target_compile_definitions(bench_managed_preempt PRIVATE LLAM_ENABLE_ASYNC_PROFILE=1 _GNU_SOURCE)
        if(APPLE)
            target_compile_definitions(bench_managed_preempt PRIVATE _XOPEN_SOURCE=700 _DARWIN_C_SOURCE)
        endif()
        target_link_libraries(bench_managed_preempt PRIVATE llam_runtime_profile)
        set_target_properties(bench_managed_preempt PROPERTIES ENABLE_EXPORTS TRUE)
        add_dependencies(bench_managed_preempt managed_bench_image)
        add_dependencies(test_managed_image_internal managed_image_fixture)
        add_dependencies(test_managed_async_compiler managed_image_fixture)
        add_dependencies(test_managed_linux_compiler managed_image_fixture)
        add_test(NAME test_managed_linux_compiler COMMAND test_managed_linux_compiler "${LLAM_MANAGED_IMAGE_FIXTURE}")
        add_test(NAME test_managed_linux_compiler_affine COMMAND test_managed_linux_compiler "${LLAM_MANAGED_AFFINE_FIXTURE}" --affine)
        set_tests_properties(test_managed_linux_compiler test_managed_linux_compiler_affine
            PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        add_test(NAME test_managed_image_internal COMMAND test_managed_image_internal "${LLAM_MANAGED_IMAGE_FIXTURE}")
        set_tests_properties(test_managed_image_internal PROPERTIES TIMEOUT 20)
        add_test(NAME test_managed_async_compiler COMMAND test_managed_async_compiler "${LLAM_MANAGED_IMAGE_FIXTURE}")
        add_test(NAME test_managed_async_compiler_affine COMMAND test_managed_async_compiler "${LLAM_MANAGED_AFFINE_FIXTURE}" --affine)
        add_test(NAME test_managed_async_compiler_cleanup COMMAND test_managed_async_compiler "${LLAM_MANAGED_AFFINE_FIXTURE}" --cleanup)
        set_tests_properties(test_managed_async_compiler_cleanup PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
        add_test(NAME test_managed_async_compiler_refill COMMAND test_managed_async_compiler "${LLAM_MANAGED_AFFINE_FIXTURE}" --refill)
        add_test(NAME test_managed_async_compiler_budget COMMAND test_managed_async_compiler "${LLAM_MANAGED_IMAGE_FIXTURE}" --budget)
        set_tests_properties(test_managed_async_compiler test_managed_async_compiler_affine
            test_managed_async_compiler_refill test_managed_async_compiler_budget
            PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
    endif()
endif()
add_test(NAME test_preempt_metadata_registration COMMAND test_preempt_metadata_registration)
if(Python3_FOUND AND LLAM_BASH_EXECUTABLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_test(NAME test_llam_clang_wrapper
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_llam_clang.py)
endif()
if(Python3_FOUND)
    set(LLAM_PREEMPT_INSTALL_TEST_ARGS)
    if(LLAM_PREEMPT_PLUGIN_ACTIVE)
        list(APPEND LLAM_PREEMPT_INSTALL_TEST_ARGS --expect-plugin)
    endif()
    if(LLAM_PREEMPT_DRIVER_AVAILABLE)
        list(APPEND LLAM_PREEMPT_INSTALL_TEST_ARGS --expect-driver)
    endif()
    if(LLAM_PREEMPT_CLANG_EXECUTABLE)
        list(APPEND LLAM_PREEMPT_INSTALL_TEST_ARGS --clang ${LLAM_PREEMPT_CLANG_EXECUTABLE})
    endif()
    add_test(NAME test_preempt_sdk_metadata
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_preempt_sdk_metadata.py)
    add_test(NAME test_preempt_cmake_install
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_preempt_cmake_install.py
                --cmake ${CMAKE_COMMAND}
                --build-dir ${CMAKE_CURRENT_BINARY_DIR}
                --mode ${LLAM_PREEMPT_TOOLING_MODE}
                ${LLAM_PREEMPT_INSTALL_TEST_ARGS})
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        add_test(NAME test_preempt_package
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_preempt_package.py)
    endif()
endif()
if(LLAM_PREEMPT_PLUGIN_ACTIVE AND Python3_FOUND AND LLAM_BASH_EXECUTABLE AND
   LLAM_PREEMPT_CLANG_EXECUTABLE AND LLAM_PREEMPT_OBJCOPY_EXECUTABLE)
    add_test(NAME test_llam_preempt_llvm_plugin
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_preempt_llvm_plugin.py
                --plugin $<TARGET_FILE:llam_preempt_pass>
                --inspector $<TARGET_FILE:llam_preempt_metadata_inspect>
                --driver ${CMAKE_CURRENT_SOURCE_DIR}/tools/llam-clang
                --clang ${LLAM_PREEMPT_CLANG_EXECUTABLE}
                --llvm-major ${LLVM_VERSION_MAJOR})
    set_tests_properties(test_llam_preempt_llvm_plugin PROPERTIES SKIP_RETURN_CODE 77)
elseif(LLAM_PREEMPT_PLUGIN_ACTIVE AND Python3_FOUND)
    message(STATUS "Matching clang, bash, or objcopy unavailable; LLVM plugin smoke test will be unavailable")
endif()
add_test(NAME test_sync_primitives COMMAND test_sync_primitives)
add_test(NAME test_windows_policy COMMAND test_windows_policy)
add_test(NAME test_windows_runtime_smoke COMMAND test_windows_runtime_smoke)
add_test(NAME test_security_capability COMMAND test_security_capability)
add_test(NAME test_lcwe_model COMMAND test_lcwe_model)
# Python-backed source-audit tests are registered when an interpreter is
# available.  The Make targets use $(PYTHON) so callers can explicitly select
# a supported interpreter; CMake keeps the existing optional-Python behavior.
if(Python3_FOUND)
    add_test(
        NAME test_worker_identity_boundary
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_worker_identity_boundary.py
    )
    add_test(
        NAME test_legacy_lease_boundary
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_legacy_lease_boundary.py
    )
    add_test(
        NAME check_legacy_lease_boundary
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_legacy_lease_boundary.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(
        NAME audit_production_test_hooks
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_production_test_hooks.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                $<TARGET_FILE:llam_runtime>
                $<TARGET_FILE:llam_runtime_shared>
    )
    # POSIX builds use nm/llvm-nm.  A platform without either tool reports a
    # deliberate CTest skip (77), while normal Debug/Release builds audit both
    # production static and shared artifacts.
    set_tests_properties(audit_production_test_hooks PROPERTIES SKIP_RETURN_CODE 77)
    add_test(
        NAME test_bench_lcwe_model
        COMMAND ${CMAKE_COMMAND} -E env
                "LCWE_MODEL_TEST_BINARY=$<TARGET_FILE:bench_lcwe_model>"
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_bench_lcwe_model.py
    )
    add_test(
        NAME test_cpu_spike_compare
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_cpu_spike_compare.py
    )
endif()
add_test(NAME llam_broker_self_test COMMAND llam_broker --self-test)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_test(NAME test_windows_iocp_io COMMAND test_windows_iocp_io)
    add_test(NAME test_windows_iocp_dump COMMAND test_windows_iocp_dump)
    add_test(NAME test_windows_handle_io COMMAND test_windows_handle_io)
endif()
add_test(NAME test_shared_load COMMAND test_shared_load $<TARGET_FILE:llam_runtime_shared>)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_test(NAME test_runtime_api_edges COMMAND test_runtime_api_edges)
    add_test(NAME test_connect_io COMMAND test_connect_io)
    add_test(NAME test_runtime_io_dump COMMAND test_runtime_io_dump)
    add_test(NAME test_io_buffers COMMAND test_io_buffers)
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
    set(LLAM_PC_PRIVATE_LIBS "-pthread -luring -lm -ldl")
endif()

file(RELATIVE_PATH LLAM_PC_PREFIX_RELATIVE
    "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(LLAM_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
else()
    set(LLAM_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(LLAM_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(LLAM_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
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

install(
    TARGETS llam_preempt_metadata_inspect
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT PreemptTools
)
if(LLAM_PREEMPT_DRIVER_AVAILABLE OR (LLAM_MANAGED_PLUGIN_ACTIVE AND UNIX))
    install(
        PROGRAMS tools/llam-clang
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT PreemptTools
    )
endif()
install(
    FILES tools/preempt/metadata.h
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam/preempt
    COMPONENT PreemptTools
)
install(FILES tools/preempt/metadata_v2.h
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam/preempt COMPONENT PreemptTools)
if(LLAM_MANAGED_PLUGIN_ACTIVE)
    install(TARGETS llam_managed_pass
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/llam COMPONENT PreemptTools
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/llam COMPONENT PreemptTools)
    install(PROGRAMS tools/llam-managed-clang
        DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PreemptTools)
    install(FILES tools/preempt/managed_driver.py tools/preempt/managed_verify.py
        tools/preempt/managed_runtime_contracts.def
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/llam COMPONENT PreemptTools)
endif()
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/preempt-sdk/tooling.conf
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam/preempt
    COMPONENT PreemptTools
)
if(LLAM_PREEMPT_PLUGIN_ACTIVE)
    install(
        TARGETS llam_preempt_pass
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/llam
        COMPONENT PreemptTools
    )
endif()

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
    FILES README.md LICENSE NOTICE THIRD_PARTY_NOTICES.md CHANGELOG.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam
    COMPONENT PreemptTools
)

install(
    FILES third_party/magic2/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/llam/third_party/magic2
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

add_executable(example_explicit_runtime examples/explicit_runtime.c)
target_link_libraries(example_explicit_runtime PRIVATE llam_runtime)
if(MSVC)
    target_compile_options(example_explicit_runtime PRIVATE /W4 /WX)
else()
    target_compile_options(example_explicit_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME example_explicit_runtime COMMAND example_explicit_runtime)
add_test(NAME example_explicit_runtime_cancel COMMAND example_explicit_runtime --cancel)
set_tests_properties(example_explicit_runtime example_explicit_runtime_cancel PROPERTIES TIMEOUT 30)
if(Python3_Interpreter_FOUND AND UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME test_platform_support COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_platform_support.py)
    set_tests_properties(test_platform_support PROPERTIES TIMEOUT 180)
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME test_managed_ci_contract
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_managed_ci.py)
    add_test(NAME test_managed_benchmark_inputs
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/test_managed_benchmark.py)
    set_tests_properties(test_managed_ci_contract test_managed_benchmark_inputs
        PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 30)
endif()
