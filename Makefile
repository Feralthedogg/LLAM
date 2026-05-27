# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g -fno-omit-frame-pointer
CPPFLAGS ?= -Iinclude -Isrc/internal -Isrc -D_GNU_SOURCE
LDLIBS ?= -pthread -luring
SERVER_FLOOD_LDLIBS ?= -pthread
OBJDIR ?= object
SHARED_OBJDIR ?= $(OBJDIR)-pic
PICFLAGS ?= -fPIC
LLAM_ABI_MAJOR ?= 2
LLAM_VERSION ?= 2.0.0
SANITIZER_TARGETS_ENABLED ?= 0
BUILD_SIGNATURE = $(OBJDIR)/.build-signature
SHARED_BUILD_SIGNATURE = $(SHARED_OBJDIR)/.build-signature
CLEAN_DIRS = \
	$(OBJDIR) \
	$(SHARED_OBJDIR) \
	object-* \
	build \
	CMakeFiles \
	cmake-build-* \
	.artifacts \
	scripts/bench_tokio_compare/target \
	build-* \
	target
CLEAN_FILES = \
	demo \
	stress \
	bench \
	llam_broker \
	server \
	server_lossless \
	server_flood \
	test_abi_contract \
	test_abi_compat \
	test_connect_io \
	test_runtime_core \
	test_multi_runtime_core \
	test_runtime_api_edges \
	test_runtime_select_edges \
	test_runtime_io_dump \
	test_runtime_group_local_edges \
	test_runtime_unmanaged_join \
	test_runtime_stress \
	test_runtime_fuzz \
	test_runtime_invariants \
	test_runtime_shutdown_internal \
	test_sync_primitives \
	test_io_buffers \
	test_windows_policy \
	test_windows_runtime_smoke \
	test_windows_iocp_io \
	test_windows_iocp_dump \
	test_windows_handle_io \
	test_security_capability \
	test_shared_load \
	asan-test_runtime_api_edges \
	asan-test_runtime_core \
	asan-test_io_buffers \
	asan-test_runtime_shutdown_internal \
	asan-test_multi_runtime_core \
	asan-test_runtime_fuzz \
	asan-test_security_capability \
	noowner-test_runtime_select_edges \
	tsan-test_runtime_core \
	tsan-test_runtime_shutdown_internal \
	tsan-test_multi_runtime_core \
	tsan-test_runtime_fuzz \
	tsan-test_security_capability \
	libllam_runtime.a \
	demo.exe \
	stress.exe \
	bench.exe \
	llam_broker.exe \
	server.exe \
	server_lossless.exe \
	server_flood.exe \
	test_abi_contract.exe \
	test_abi_compat.exe \
	test_connect_io.exe \
	test_runtime_core.exe \
	test_multi_runtime_core.exe \
	test_runtime_api_edges.exe \
	test_runtime_select_edges.exe \
	test_runtime_io_dump.exe \
	test_runtime_group_local_edges.exe \
	test_runtime_unmanaged_join.exe \
	test_runtime_stress.exe \
	test_runtime_fuzz.exe \
	test_runtime_invariants.exe \
	test_runtime_shutdown_internal.exe \
	test_sync_primitives.exe \
	test_io_buffers.exe \
	test_windows_policy.exe \
	test_windows_runtime_smoke.exe \
	test_windows_iocp_io.exe \
	test_windows_iocp_dump.exe \
	test_windows_handle_io.exe \
	test_security_capability.exe \
	test_shared_load.exe \
	libllam_runtime.dylib \
	libllam_runtime.$(LLAM_ABI_MAJOR).dylib \
	libllam_runtime.so \
	libllam_runtime.so.$(LLAM_ABI_MAJOR) \
	libllam_runtime.so.$(LLAM_VERSION) \
	CMakeCache.txt \
	cmake_install.cmake \
	compile_commands.json \
	*.link-signature \
	*.plist \
	perf.data \
	perf.data.old \
	test_nm_compat_runtime
ifeq ($(OS),Windows_NT)
UNAME_S := Windows_NT
UNAME_M := $(PROCESSOR_ARCHITECTURE)
HOST_PLATFORM := windows
else
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Linux)
HOST_PLATFORM := linux
else ifeq ($(UNAME_S),Darwin)
HOST_PLATFORM := darwin
else ifneq ($(filter $(UNAME_S),FreeBSD OpenBSD NetBSD DragonFly),)
HOST_PLATFORM := bsd
else
HOST_PLATFORM := posix
endif
endif

ifeq ($(HOST_PLATFORM),darwin)
SHLIB_LINK = libllam_runtime.dylib
SHLIB_REAL = libllam_runtime.$(LLAM_ABI_MAJOR).dylib
SHLIB_LDFLAGS = -dynamiclib -Wl,-install_name,@rpath/$(SHLIB_REAL)
DL_LIBS =
else ifeq ($(HOST_PLATFORM),bsd)
SHLIB_LINK = libllam_runtime.so
SHLIB_SONAME = libllam_runtime.so.$(LLAM_ABI_MAJOR)
SHLIB_REAL = libllam_runtime.so.$(LLAM_VERSION)
SHLIB_LDFLAGS = -shared -Wl,-soname,$(SHLIB_SONAME)
DL_LIBS =
else
SHLIB_LINK = libllam_runtime.so
SHLIB_SONAME = libllam_runtime.so.$(LLAM_ABI_MAJOR)
SHLIB_REAL = libllam_runtime.so.$(LLAM_VERSION)
SHLIB_LDFLAGS = -shared -Wl,-soname,$(SHLIB_SONAME)
DL_LIBS = -ldl
endif

LLAM_PUBLIC_HDRS = \
	include/llam/io.h \
	include/llam/platform.h \
	include/llam/runtime.h

RUNTIME_PRIV_HDRS = \
	$(LLAM_PUBLIC_HDRS) \
	src/internal/runtime_platform.h \
	src/internal/runtime_windows_compat.h \
	src/internal/runtime_windows.h \
	src/internal/runtime_windows_iocp.h \
	src/internal/runtime_debug_dump_helpers.h \
	src/internal/llam_internal.h \
	src/internal/runtime_internal.h \
	src/internal/runtime_types.h \
	src/internal/runtime_public_slot.h \
	src/internal/runtime_public_slot_seal.h \
	src/internal/runtime_capability.h \
	src/internal/runtime_broker.h \
	src/internal/runtime_broker_windows_security.h \
	src/internal/runtime_broker_ring.h \
	src/internal/runtime_state.h \
	src/internal/runtime_protos.h \
	src/internal/runtime_proto_core.h \
	src/internal/runtime_proto_sched.h \
	src/internal/runtime_proto_io.h \
	src/internal/runtime_proto_sync.h \
	src/engine/runtime_watchdog_internal.h \
	src/io/runtime_io_api_internal.h \
	src/io/runtime_io_watch_migration_live_finalize_template.inc \
	src/io/runtime_io_watch_migration_live_forward_template.inc \
	src/io/runtime_io_watch_rehome_accept_template.inc \
	src/io/runtime_io_watch_rehome_recv_template.inc \
	src/io/runtime_io_watch_rehome_template.inc \
	src/io/darwin/runtime_io_watch_darwin_internal.h \
	src/io/linux/runtime_io_watch_linux_internal.h \
	src/io/windows/runtime_io_watch_windows_internal.h

RUNTIME_COMMON_OBJS = \
	$(OBJDIR)/src/core/runtime.o \
	$(OBJDIR)/src/core/runtime_abi.o \
	$(OBJDIR)/src/core/runtime_errno.o \
	$(OBJDIR)/src/core/runtime_util.o \
	$(OBJDIR)/src/core/runtime_io_udata.o \
	$(OBJDIR)/src/core/runtime_capability.o \
	$(OBJDIR)/src/core/runtime_broker.o \
	$(OBJDIR)/src/core/runtime_broker_ops.o \
	$(OBJDIR)/src/core/runtime_broker_validate.o \
	$(OBJDIR)/src/core/runtime_broker_buffer.o \
	$(OBJDIR)/src/core/runtime_broker_descriptor.o \
	$(OBJDIR)/src/core/runtime_broker_channel.o \
	$(OBJDIR)/src/core/runtime_broker_revoke.o \
	$(OBJDIR)/src/core/runtime_broker_task.o \
	$(OBJDIR)/src/core/runtime_broker_windows_security.o \
	$(OBJDIR)/src/core/runtime_broker_transport_dispatch.o \
	$(OBJDIR)/src/core/runtime_broker_transport_ops.o \
	$(OBJDIR)/src/core/runtime_broker_transport_response.o \
	$(OBJDIR)/src/core/runtime_broker_transport_rollback.o \
	$(OBJDIR)/src/core/runtime_broker_transport_ring.o \
	$(OBJDIR)/src/core/runtime_broker_transport.o \
	$(OBJDIR)/src/core/runtime_broker_transport_windows.o \
	$(OBJDIR)/src/core/runtime_broker_transport_windows_fd_stubs.o \
	$(OBJDIR)/src/core/runtime_broker_transport_windows_pipe.o \
	$(OBJDIR)/src/core/runtime_broker_transport_windows_session.o \
	$(OBJDIR)/src/core/runtime_broker_transport_posix.o \
	$(OBJDIR)/src/core/runtime_broker_transport_posix_message.o \
	$(OBJDIR)/src/core/runtime_broker_transport_posix_socket.o \
	$(OBJDIR)/src/core/runtime_broker_transport_unix.o \
	$(OBJDIR)/src/core/runtime_broker_transport_selftest.o \
	$(OBJDIR)/src/core/runtime_broker_ring.o \
	$(OBJDIR)/src/core/runtime_broker_ring_buffer_grant.o \
	$(OBJDIR)/src/core/runtime_broker_ring_doorbell.o \
	$(OBJDIR)/src/core/runtime_broker_ring_dispatch.o \
	$(OBJDIR)/src/core/runtime_broker_ring_stats.o \
	$(OBJDIR)/src/core/runtime_broker_ring_shm.o \
	$(OBJDIR)/src/core/runtime_broker_ring_shm_posix.o \
	$(OBJDIR)/src/core/runtime_broker_ring_shm_windows.o \
	$(OBJDIR)/src/core/runtime_fault.o \
	$(OBJDIR)/src/core/runtime_names.o \
	$(OBJDIR)/src/core/runtime_time.o \
	$(OBJDIR)/src/core/runtime_fp.o \
	$(OBJDIR)/src/core/runtime_stack_sample.o \
	$(OBJDIR)/src/core/runtime_context_portable.o \
	$(OBJDIR)/src/core/runtime_queue_base.o \
	$(OBJDIR)/src/core/runtime_norm_queue.o \
	$(OBJDIR)/src/core/runtime_queue.o \
	$(OBJDIR)/src/core/runtime_alloc.o \
	$(OBJDIR)/src/core/runtime_allocator_quiescent.o \
	$(OBJDIR)/src/core/runtime_task_alloc.o \
	$(OBJDIR)/src/core/runtime_task_handle_registry.o \
	$(OBJDIR)/src/core/runtime_io_object_alloc.o \
	$(OBJDIR)/src/core/runtime_wait_timer_alloc.o \
	$(OBJDIR)/src/core/runtime_trace.o \
	$(OBJDIR)/src/core/runtime_wake.o \
	$(OBJDIR)/src/core/runtime_platform.o \
	$(OBJDIR)/src/core/runtime_windows.o \
	$(OBJDIR)/src/core/runtime_safepoint.o \
	$(OBJDIR)/src/core/runtime_wait.o \
	$(OBJDIR)/src/core/runtime_task_reclaim.o \
	$(OBJDIR)/src/core/runtime_task_stack.o \
	$(OBJDIR)/src/core/runtime_reinject.o \
	$(OBJDIR)/src/core/runtime_wait_tracking.o \
	$(OBJDIR)/src/core/runtime_timer_heap.o \
	$(OBJDIR)/src/core/runtime_timer.o \
	$(OBJDIR)/src/engine/runtime_engine.o \
	$(OBJDIR)/src/engine/runtime_block.o \
	$(OBJDIR)/src/io/runtime_io_engine.o \
	$(OBJDIR)/src/engine/runtime_watchdog.o \
	$(OBJDIR)/src/engine/runtime_watchdog_probe.o \
	$(OBJDIR)/src/engine/runtime_watchdog_merge.o \
	$(OBJDIR)/src/engine/runtime_watchdog_rehome.o \
	$(OBJDIR)/src/engine/runtime_watchdog_scale.o \
	$(OBJDIR)/src/engine/runtime_watchdog_worker.o \
	$(OBJDIR)/src/core/runtime_api.o \
	$(OBJDIR)/src/core/runtime_spawn.o \
	$(OBJDIR)/src/core/runtime_yield_join_sleep.o \
	$(OBJDIR)/src/core/runtime_blocking_api.o \
	$(OBJDIR)/src/core/runtime_cancel_api.o \
	$(OBJDIR)/src/core/runtime_lifecycle.o \
	$(OBJDIR)/src/core/runtime_scheduler.o \
	$(OBJDIR)/src/core/runtime_init.o \
	$(OBJDIR)/src/core/runtime_shutdown.o \
	$(OBJDIR)/src/core/runtime_run.o \
	$(OBJDIR)/src/core/runtime_sync.o \
	$(OBJDIR)/src/core/runtime_mutex_lifecycle.o \
	$(OBJDIR)/src/core/runtime_mutex.o \
	$(OBJDIR)/src/core/runtime_cond_lifecycle.o \
	$(OBJDIR)/src/core/runtime_cond.o \
	$(OBJDIR)/src/core/runtime_channel_cache.o \
	$(OBJDIR)/src/core/runtime_channel_lifecycle.o \
	$(OBJDIR)/src/core/runtime_channel_fast.o \
	$(OBJDIR)/src/core/runtime_channel.o \
	$(OBJDIR)/src/core/runtime_channel_select_fast.o \
	$(OBJDIR)/src/core/runtime_channel_select.o \
	$(OBJDIR)/src/core/runtime_handle.o \
	$(OBJDIR)/src/core/runtime_owner.o \
	$(OBJDIR)/src/core/runtime_registry.o \
	$(OBJDIR)/src/core/runtime_task_group.o \
	$(OBJDIR)/src/core/runtime_task_local.o \
	$(OBJDIR)/src/io/runtime_io_api.o \
	$(OBJDIR)/src/io/runtime_io_api_direct.o \
	$(OBJDIR)/src/io/runtime_io_api_direct_tuning.o \
	$(OBJDIR)/src/io/runtime_io_api_issue.o \
	$(OBJDIR)/src/io/runtime_io_api_blocking_ops.o \
	$(OBJDIR)/src/io/runtime_io_api_blocking_file_ops.o \
	$(OBJDIR)/src/io/runtime_io_buffer_registry.o \
	$(OBJDIR)/src/io/runtime_io_api_owned.o \
	$(OBJDIR)/src/io/runtime_io_api_handle_positional.o \
	$(OBJDIR)/src/io/runtime_io_api_positional.o \
	$(OBJDIR)/src/io/runtime_io_api_positional_util.o \
	$(OBJDIR)/src/io/runtime_io_api_public.o \
	$(OBJDIR)/src/io/windows/runtime_windows_iocp.o \
	$(OBJDIR)/src/core/runtime_debug_dump_helpers.o \
	$(OBJDIR)/src/core/runtime_debug_stats_json.o \
	$(OBJDIR)/src/core/runtime_debug.o \
	$(OBJDIR)/src/io/runtime_io_watch.o \
	$(OBJDIR)/src/io/runtime_io_watch_close.o \
	$(OBJDIR)/src/io/runtime_io_watch_lookup.o \
	$(OBJDIR)/src/io/runtime_io_watch_migration.o \
	$(OBJDIR)/src/io/runtime_io_watch_queue.o \
	$(OBJDIR)/src/io/runtime_io_watch_waiter.o

ifeq ($(HOST_PLATFORM),linux)
LDLIBS += -lm
LLAM_HAVE_IO_URING_BUF_RING_HELPERS := $(shell printf '%b' '\043include <liburing.h>\012int main\050void\051 \173 void *p = \050void *\051io_uring_setup_buf_ring; return p == 0; \175\012' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c - -luring -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(LLAM_HAVE_IO_URING_BUF_RING_HELPERS),1)
CPPFLAGS += -DLLAM_HAVE_IO_URING_BUF_RING_HELPERS=1
endif
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_prelude.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_state.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_lookup.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_migration_live.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_migration_rehome.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_control.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_submit.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_cqe.o \
	$(OBJDIR)/src/io/linux/runtime_io_watch_linux_worker.o
ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/asm/linux/x86_64/context_x86_64.o \
	$(OBJDIR)/src/asm/linux/x86_64/runtime_linux_x86_64.o
else ifeq ($(UNAME_M),aarch64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/core/runtime_context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/context_arm64.o
endif
else ifeq ($(HOST_PLATFORM),darwin)
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
LDLIBS := $(filter-out -luring,$(LDLIBS))
CPPFLAGS += -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_state.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_migration_live.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_migration_rehome.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_control.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_completion.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_events.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_worker.o
ifeq ($(UNAME_M),arm64)
RUNTIME_OBJS += $(OBJDIR)/src/core/runtime_context_arm64.o
RUNTIME_OBJS += $(OBJDIR)/src/asm/darwin/arm64/context_arm64.o
else ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/darwin/x86_64/context_x86_64.o
endif
else ifeq ($(HOST_PLATFORM),windows)
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
LDLIBS := $(filter-out -pthread -luring,$(LDLIBS)) -lws2_32 -lmswsock -ladvapi32
CPPFLAGS += -D_WIN32_WINNT=0x0A00 -DLLAM_ENABLE_WINDOWS_BACKEND
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_state.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_socket.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_pool.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_control.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_submit.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_completion.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows_fallback.o \
	$(OBJDIR)/src/io/windows/runtime_io_watch_windows.o
ifeq ($(UNAME_M),AMD64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/windows/x86_64/context_x86_64.o
else ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/windows/x86_64/context_x86_64.o
endif
else
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
LDLIBS := $(filter-out -luring,$(LDLIBS))
ifeq ($(HOST_PLATFORM),bsd)
SERVER_FLOOD_LDLIBS += -lm
ifeq ($(UNAME_S),NetBSD)
LDLIBS += -lrt
endif
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_state.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_migration_live.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_migration_rehome.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_control.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_completion.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_events.o \
	$(OBJDIR)/src/io/darwin/runtime_io_watch_darwin_worker.o
ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/linux/x86_64/context_x86_64.o
else ifeq ($(UNAME_M),amd64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/linux/x86_64/context_x86_64.o
else ifeq ($(UNAME_M),aarch64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/core/runtime_context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/context_arm64.o
else ifeq ($(UNAME_M),arm64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/core/runtime_context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/context_arm64.o
endif
endif
endif
SHARED_RUNTIME_OBJS = $(patsubst $(OBJDIR)/%,$(SHARED_OBJDIR)/%,$(RUNTIME_OBJS))
DEMO_OBJS = \
	$(OBJDIR)/examples/demo.o \
	$(OBJDIR)/examples/demo_tasks.o \
	$(OBJDIR)/examples/demo_entry.o
STRESS_OBJS = \
	$(OBJDIR)/examples/stress.o \
	$(OBJDIR)/examples/diagnostic_output.o \
	$(OBJDIR)/examples/stress_support.o \
	$(OBJDIR)/examples/stress_tasks.o \
	$(OBJDIR)/examples/stress_core_cases.o \
	$(OBJDIR)/examples/stress_timeout_cases.o \
	$(OBJDIR)/examples/stress_dynamic_cases.o \
	$(OBJDIR)/examples/stress_suite.o \
	$(OBJDIR)/examples/stress_entry.o \
	$(OBJDIR)/examples/stress_signal_dump.o
BENCH_OBJS = \
	$(OBJDIR)/examples/bench.o \
	$(OBJDIR)/examples/bench_support.o \
	$(OBJDIR)/examples/bench_entry.o
BROKER_OBJS = \
	$(OBJDIR)/examples/broker.o
SERVER_OBJS = \
	$(OBJDIR)/examples/server.o \
	$(OBJDIR)/examples/diagnostic_output.o \
	$(OBJDIR)/examples/server_support.o
SERVER_LOSSLESS_OBJS = \
	$(OBJDIR)/examples/server_lossless.o \
	$(OBJDIR)/examples/diagnostic_output.o \
	$(OBJDIR)/examples/server_support.o
SERVER_FLOOD_OBJS = \
	$(OBJDIR)/examples/server_flood.o \
	$(OBJDIR)/examples/server_flood_stats.o
TEST_ABI_OBJS = \
	$(OBJDIR)/tests/test_abi_contract.o
TEST_ABI_COMPAT_OBJS = \
	$(OBJDIR)/tests/test_abi_compat.o
TEST_CONNECT_OBJS = \
	$(OBJDIR)/tests/test_connect_io.o
TEST_RUNTIME_CORE_OBJS = \
	$(OBJDIR)/tests/test_runtime_core.o
TEST_MULTI_RUNTIME_CORE_OBJS = \
	$(OBJDIR)/tests/test_multi_runtime_core.o
TEST_RUNTIME_API_EDGES_OBJS = \
	$(OBJDIR)/tests/test_runtime_api_edges.o
TEST_RUNTIME_SELECT_EDGES_OBJS = \
	$(OBJDIR)/tests/test_runtime_select_edges.o
TEST_RUNTIME_IO_DUMP_OBJS = \
	$(OBJDIR)/tests/test_runtime_io_dump.o
TEST_RUNTIME_GROUP_LOCAL_EDGES_OBJS = \
	$(OBJDIR)/tests/test_runtime_group_local_edges.o
TEST_RUNTIME_UNMANAGED_JOIN_OBJS = \
	$(OBJDIR)/tests/test_runtime_unmanaged_join.o
TEST_RUNTIME_STRESS_OBJS = \
	$(OBJDIR)/tests/test_runtime_stress.o
TEST_RUNTIME_FUZZ_OBJS = \
	$(OBJDIR)/tests/test_runtime_fuzz.o
TEST_RUNTIME_INVARIANTS_OBJS = \
	$(OBJDIR)/tests/test_runtime_invariants.o
TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_runtime_shutdown_internal.o
TEST_SYNC_OBJS = \
	$(OBJDIR)/tests/test_sync_primitives.o
TEST_IO_BUFFERS_OBJS = \
	$(OBJDIR)/tests/test_io_buffers.o
TEST_WINDOWS_POLICY_OBJS = \
	$(OBJDIR)/tests/test_windows_policy.o
# Keep Make parity with CMake for portable Windows diagnostics; IOCP dump
# builds everywhere and skips at runtime on non-Windows hosts.
TEST_WINDOWS_RUNTIME_SMOKE_OBJS = \
	$(OBJDIR)/tests/test_windows_runtime_smoke.o
TEST_WINDOWS_IOCP_IO_OBJS = \
	$(OBJDIR)/tests/test_windows_iocp_io.o
TEST_WINDOWS_IOCP_DUMP_OBJS = \
	$(OBJDIR)/tests/test_windows_iocp_dump.o
TEST_WINDOWS_HANDLE_IO_OBJS = \
	$(OBJDIR)/tests/test_windows_handle_io.o
TEST_SECURITY_CAPABILITY_OBJS = \
	$(OBJDIR)/tests/test_security_capability.o
TEST_SHARED_LOAD_OBJS = \
	$(OBJDIR)/tests/test_shared_load.o
RUNTIME_ENGINE_FRAGMENTS = $(wildcard src/engine/detail/*.inc)
EXAMPLE_SHARED_HDRS = examples/env_compat.h
BUILD_OBJS = \
	$(RUNTIME_OBJS) \
	$(DEMO_OBJS) \
	$(STRESS_OBJS) \
	$(BENCH_OBJS) \
	$(BROKER_OBJS) \
	$(SERVER_OBJS) \
	$(SERVER_LOSSLESS_OBJS) \
	$(SERVER_FLOOD_OBJS) \
	$(TEST_ABI_OBJS) \
	$(TEST_ABI_COMPAT_OBJS) \
	$(TEST_CONNECT_OBJS) \
	$(TEST_RUNTIME_CORE_OBJS) \
	$(TEST_MULTI_RUNTIME_CORE_OBJS) \
	$(TEST_RUNTIME_API_EDGES_OBJS) \
	$(TEST_RUNTIME_SELECT_EDGES_OBJS) \
	$(TEST_RUNTIME_IO_DUMP_OBJS) \
	$(TEST_RUNTIME_GROUP_LOCAL_EDGES_OBJS) \
	$(TEST_RUNTIME_UNMANAGED_JOIN_OBJS) \
	$(TEST_RUNTIME_STRESS_OBJS) \
	$(TEST_RUNTIME_FUZZ_OBJS) \
	$(TEST_RUNTIME_INVARIANTS_OBJS) \
	$(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS) \
	$(TEST_SYNC_OBJS) \
	$(TEST_IO_BUFFERS_OBJS) \
	$(TEST_WINDOWS_POLICY_OBJS) \
	$(TEST_WINDOWS_RUNTIME_SMOKE_OBJS) \
	$(TEST_WINDOWS_IOCP_DUMP_OBJS) \
	$(TEST_SECURITY_CAPABILITY_OBJS) \
	$(TEST_SHARED_LOAD_OBJS)
LINK_TARGETS = \
	demo \
	stress \
	bench \
	llam_broker \
	server \
	server_lossless \
	server_flood \
	test_abi_contract \
	test_abi_compat \
	test_connect_io \
	test_runtime_core \
	test_multi_runtime_core \
	test_runtime_api_edges \
	test_runtime_select_edges \
	test_runtime_io_dump \
	test_runtime_group_local_edges \
	test_runtime_unmanaged_join \
	test_runtime_stress \
	test_runtime_fuzz \
	test_runtime_invariants \
	test_runtime_shutdown_internal \
	test_sync_primitives \
	test_io_buffers \
	test_windows_policy \
	test_windows_runtime_smoke \
	test_windows_iocp_io \
	test_windows_iocp_dump \
	test_windows_handle_io \
	test_security_capability \
	test_shared_load \
	libllam_runtime.a

.PHONY: all clean static shared test test-asan test-no-owner test-tsan test-fuzz-heavy test-hardening require-sanitizer-target analyze-cppcheck audit-deps test-quick test-full test-soak check package bench-matrix server-stress server-flood server-lossless-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux verify-windows platform-status windows-unsupported FORCE
.DEFAULT_GOAL := all

require-sanitizer-target:
	@if [ "$(SANITIZER_TARGETS_ENABLED)" != "1" ]; then \
		echo "error: build sanitizer test binaries through 'make test-asan' or 'make test-tsan' so OBJDIR/CFLAGS/LDLIBS are sanitizer-scoped" >&2; \
		exit 1; \
	fi

ifeq ($(HOST_PLATFORM),windows)

WINDOWS_CMAKE_BUILD_DIR ?= build-windows-native
WINDOWS_CMAKE_CONFIG ?= Release
WINDOWS_CMAKE_ARGS ?=
WINDOWS_CTEST_ARGS ?=
WINDOWS_CTEST_REGEX ?= test_abi_contract|test_abi_compat|test_runtime_core|test_multi_runtime_core|test_runtime_api_edges|test_runtime_select_edges|test_runtime_group_local_edges|test_runtime_unmanaged_join|test_runtime_stress|test_runtime_fuzz|test_runtime_invariants|test_runtime_shutdown_internal|test_sync_primitives|test_windows_policy|test_windows_runtime_smoke|test_windows_iocp_io|test_windows_iocp_dump|test_windows_handle_io|test_security_capability|llam_broker_self_test
WINDOWS_CMAKE_TARGETS = \
	demo \
	stress \
	bench \
	llam_broker \
	server \
	server_lossless \
	server_flood \
	test_abi_contract \
	test_abi_compat \
	test_connect_io \
	test_runtime_core \
	test_multi_runtime_core \
	test_runtime_api_edges \
	test_runtime_select_edges \
	test_runtime_io_dump \
	test_runtime_group_local_edges \
	test_runtime_unmanaged_join \
	test_runtime_stress \
	test_runtime_fuzz \
	test_runtime_invariants \
	test_runtime_shutdown_internal \
	test_sync_primitives \
	test_io_buffers \
	test_windows_policy \
	test_windows_runtime_smoke \
	test_windows_iocp_io \
	test_windows_iocp_dump \
	test_windows_handle_io \
	test_security_capability \
	test_shared_load

.PHONY: windows-cmake-configure windows-cmake-build windows-cmake-test

all: windows-cmake-build

static: windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)" --target llam_runtime

shared: windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)" --target llam_runtime_shared

test check: windows-cmake-test

$(WINDOWS_CMAKE_TARGETS): windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)" --target $@

package: windows-cmake-build
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release_windows.ps1 -BuildDir "$(WINDOWS_CMAKE_BUILD_DIR)" -Configuration "$(WINDOWS_CMAKE_CONFIG)"

bench-matrix: bench
	python scripts/bench_matrix.py

server-stress server-flood server-lossless-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux: windows-unsupported

platform-status:
	@echo "host platform: windows"
	@echo "Native Windows 10/11 backend: IOCP request path, x86_64 asm context switching, and Windows policy tests are built through CMake."
	@echo "Makefile Windows targets delegate to CMake. Override WINDOWS_CMAKE_ARGS to select a generator, for example WINDOWS_CMAKE_ARGS='-G Ninja'."

windows-cmake-configure: platform-status
	cmake -S . -B "$(WINDOWS_CMAKE_BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(WINDOWS_CMAKE_CONFIG)" -DLLAM_ENABLE_WINDOWS_BACKEND=ON $(WINDOWS_CMAKE_ARGS)

windows-cmake-build: windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)"

windows-cmake-test: windows-cmake-build
	ctest --test-dir "$(WINDOWS_CMAKE_BUILD_DIR)" --output-on-failure -C "$(WINDOWS_CMAKE_CONFIG)" -R "$(WINDOWS_CTEST_REGEX)" $(WINDOWS_CTEST_ARGS)

windows-unsupported: platform-status
	@exit 2

clean:
	rm -rf $(CLEAN_DIRS)
	rm -f $(CLEAN_FILES)

verify-windows:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_windows.ps1 -Native

else

FORCE:

$(BUILD_SIGNATURE): FORCE
	@mkdir -p $(dir $@)
	@tmp="$@.$$$$.tmp"; \
	{ \
		printf 'CC=%s\n' '$(CC)'; \
		printf 'CPPFLAGS=%s\n' '$(CPPFLAGS)'; \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'LDLIBS=%s\n' '$(LDLIBS)'; \
		printf 'OBJDIR=%s\n' '$(OBJDIR)'; \
		printf 'HOST_PLATFORM=%s\n' '$(HOST_PLATFORM)'; \
		printf 'UNAME_M=%s\n' '$(UNAME_M)'; \
	} > "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

$(SHARED_BUILD_SIGNATURE): FORCE
	@mkdir -p $(dir $@)
	@tmp="$@.$$$$.tmp"; \
	{ \
		printf 'CC=%s\n' '$(CC)'; \
		printf 'CPPFLAGS=%s\n' '$(CPPFLAGS)'; \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'PICFLAGS=%s\n' '$(PICFLAGS)'; \
		printf 'LDLIBS=%s\n' '$(LDLIBS)'; \
		printf 'SHARED_OBJDIR=%s\n' '$(SHARED_OBJDIR)'; \
		printf 'HOST_PLATFORM=%s\n' '$(HOST_PLATFORM)'; \
		printf 'UNAME_M=%s\n' '$(UNAME_M)'; \
	} > "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

$(BUILD_OBJS): $(BUILD_SIGNATURE)

$(SHARED_RUNTIME_OBJS): $(SHARED_BUILD_SIGNATURE)

$(LINK_TARGETS): %: %.link-signature

$(SHLIB_REAL): $(SHLIB_REAL).link-signature

%.link-signature: FORCE
	@tmp="$@.$$$$.tmp"; \
	{ \
		printf 'target=%s\n' '$*'; \
		printf 'CC=%s\n' '$(CC)'; \
		printf 'CPPFLAGS=%s\n' '$(CPPFLAGS)'; \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'PICFLAGS=%s\n' '$(PICFLAGS)'; \
		printf 'SHLIB_LDFLAGS=%s\n' '$(SHLIB_LDFLAGS)'; \
		printf 'LDLIBS=%s\n' '$(LDLIBS)'; \
		printf 'OBJDIR=%s\n' '$(OBJDIR)'; \
		printf 'SHARED_OBJDIR=%s\n' '$(SHARED_OBJDIR)'; \
		printf 'HOST_PLATFORM=%s\n' '$(HOST_PLATFORM)'; \
		printf 'UNAME_M=%s\n' '$(UNAME_M)'; \
	} > "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

all: demo stress bench llam_broker server server_lossless server_flood static shared

static: libllam_runtime.a

libllam_runtime.a: $(RUNTIME_OBJS)
	$(AR) rcs $@ $(RUNTIME_OBJS)

shared: $(SHLIB_LINK)

test: test_abi_contract test_abi_compat test_connect_io test_runtime_core test_multi_runtime_core test_runtime_api_edges test_runtime_select_edges test_runtime_io_dump test_runtime_group_local_edges test_runtime_unmanaged_join test_runtime_stress test_runtime_fuzz test_runtime_invariants test_runtime_shutdown_internal test_sync_primitives test_io_buffers test_windows_policy test_windows_runtime_smoke test_windows_iocp_io test_windows_iocp_dump test_windows_handle_io test_security_capability test_shared_load llam_broker server stress server_flood shared
	./test_abi_contract
	./test_abi_compat
	./test_connect_io
	./test_runtime_core
	./test_multi_runtime_core
	./test_runtime_api_edges
	./test_runtime_select_edges
	./test_runtime_io_dump
	./test_runtime_group_local_edges
	./test_runtime_unmanaged_join
	./test_runtime_stress
	./test_runtime_fuzz
	./test_runtime_invariants
	./test_runtime_shutdown_internal
	./test_sync_primitives
	./test_io_buffers
	./test_windows_policy
	./test_windows_runtime_smoke
	./test_windows_iocp_io
	./test_windows_iocp_dump
	./test_windows_handle_io
	./test_security_capability
	./llam_broker --self-test
	@broker_sock="$${TMPDIR:-/tmp}/llam-broker-test.$$$$.sock"; \
	server_out="$${TMPDIR:-/tmp}/llam-broker-test.$$$$.out"; \
	rm -f "$$broker_sock" "$$server_out"; \
	./llam_broker --serve-n "$$broker_sock" 3 >"$$server_out" 2>&1 & \
	server_pid="$$!"; \
	for _i in $$(seq 1 100); do \
		if [ -S "$$broker_sock" ]; then break; fi; \
		sleep 0.02; \
	done; \
	python3 -c 'import socket, sys; sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); sock.connect(sys.argv[1]); sock.shutdown(socket.SHUT_RDWR); sock.close()' "$$broker_sock" && \
	./llam_broker --client-self-test "$$broker_sock" && \
	./llam_broker --client-self-test "$$broker_sock"; \
	client_rc="$$?"; \
	if [ "$$client_rc" -ne 0 ]; then \
		kill "$$server_pid" >/dev/null 2>&1 || true; \
		wait "$$server_pid" >/dev/null 2>&1 || true; \
		cat "$$server_out" >&2; \
		rm -f "$$broker_sock" "$$server_out"; \
		exit "$$client_rc"; \
	fi; \
	if ! wait "$$server_pid"; then \
		cat "$$server_out" >&2; \
		rm -f "$$broker_sock" "$$server_out"; \
		exit 1; \
	fi; \
	rm -f "$$broker_sock" "$$server_out"
	./test_shared_load ./$(SHLIB_REAL)
	@tmp_out="$$(mktemp "$${TMPDIR:-/tmp}/llam-server-flood-invalid.XXXXXX")"; \
	trap 'rm -f "$$tmp_out"' 0 1 2 3 15; \
	if ./server_flood --clients 8x >"$$tmp_out" 2>&1; then \
		echo "server_flood accepted invalid --clients value" >&2; \
		cat "$$tmp_out" >&2; \
		exit 1; \
	fi; \
	if ./server_flood --duration nan >"$$tmp_out" 2>&1; then \
		echo "server_flood accepted invalid --duration value" >&2; \
		cat "$$tmp_out" >&2; \
		exit 1; \
	fi
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-server-flood-stats-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	{ \
		printf '%s\n' '#!/usr/bin/env python3'; \
		printf '%s\n' 'import os, signal, socket, sys, time'; \
		printf '%s\n' 'port = int(sys.argv[-1])'; \
		printf '%s\n' 'stats = os.environ.get("LLAM_CHAT_STATS_PATH")'; \
		printf '%s\n' 'target = os.environ.get("LLAM_MALICIOUS_STATS_TARGET")'; \
		printf '%s\n' 'if stats and target:'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        os.unlink(stats)'; \
		printf '%s\n' '    except FileNotFoundError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' '    os.symlink(target, stats)'; \
		printf '%s\n' 'stop = False'; \
		printf '%s\n' 'def handle(_signum, _frame):'; \
		printf '%s\n' '    global stop'; \
		printf '%s\n' '    stop = True'; \
		printf '%s\n' 'signal.signal(signal.SIGINT, handle)'; \
		printf '%s\n' 'sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)'; \
		printf '%s\n' 'sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)'; \
		printf '%s\n' 'sock.bind(("127.0.0.1", port))'; \
		printf '%s\n' 'sock.listen(16)'; \
		printf '%s\n' 'sock.setblocking(False)'; \
		printf '%s\n' 'clients = []'; \
		printf '%s\n' 'deadline = time.time() + 5.0'; \
		printf '%s\n' 'while not stop and time.time() < deadline:'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        client, _peer = sock.accept()'; \
		printf '%s\n' '        client.setblocking(False)'; \
		printf '%s\n' '        clients.append(client)'; \
		printf '%s\n' '    except BlockingIOError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' '    for client in list(clients):'; \
		printf '%s\n' '        try:'; \
		printf '%s\n' '            data = client.recv(4096)'; \
		printf '%s\n' '            if data:'; \
		printf '%s\n' '                client.sendall(b"x\n")'; \
		printf '%s\n' '            elif data == b"":'; \
		printf '%s\n' '                clients.remove(client)'; \
		printf '%s\n' '                client.close()'; \
		printf '%s\n' '        except BlockingIOError:'; \
		printf '%s\n' '            pass'; \
		printf '%s\n' '        except OSError:'; \
		printf '%s\n' '            clients.remove(client)'; \
		printf '%s\n' '    time.sleep(0.001)'; \
		printf '%s\n' 'for client in clients:'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        client.close()'; \
		printf '%s\n' '    except OSError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' 'sock.close()'; \
	} > "$$tmp_dir/malicious_server.py"; \
	chmod +x "$$tmp_dir/malicious_server.py"; \
	printf 'server stopped; outbox_full_drops=123456 outbox_closed_drops=0 broadcast_messages_created=1 broadcast_deliveries_attempted=1 broadcast_deliveries_enqueued=1\n' > "$$tmp_dir/outside-stats"; \
	if ! LLAM_MALICIOUS_STATS_TARGET="$$tmp_dir/outside-stats" ./server_flood --server "$$tmp_dir/malicious_server.py" --clients 2 --duration 0.05 --drain-sec 0.05 --message-bytes 8 --batch 1 --target-mps 0.001 --min-delivery-mps 0 --min-delivery-ratio 0 --allow-forced-stop --allow-missing-stats >"$$tmp_dir/flood.out" 2>&1; then \
		echo "server_flood symlink stats regression failed unexpectedly" >&2; \
		cat "$$tmp_dir/flood.out" >&2; \
		exit 1; \
	fi; \
	if grep 'outbox_full_drops=123456' "$$tmp_dir/flood.out" >/dev/null; then \
		echo "server_flood accepted a symlinked stats file" >&2; \
		cat "$$tmp_dir/flood.out" >&2; \
		exit 1; \
	fi; \
	grep 'server flood stats: unavailable' "$$tmp_dir/flood.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-server-flood-stats-parent-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	{ \
		printf '%s\n' '#!/usr/bin/env python3'; \
		printf '%s\n' 'import os, signal, socket, sys, time'; \
		printf '%s\n' 'port = int(sys.argv[-1])'; \
		printf '%s\n' 'stats = os.environ.get("LLAM_CHAT_STATS_PATH")'; \
		printf '%s\n' 'target_dir = os.environ.get("LLAM_MALICIOUS_STATS_DIR")'; \
		printf '%s\n' 'if stats and target_dir:'; \
		printf '%s\n' '    parent = os.path.dirname(stats)'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        os.rmdir(parent)'; \
		printf '%s\n' '    except OSError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        os.symlink(target_dir, parent)'; \
		printf '%s\n' '    except FileExistsError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' 'stop = False'; \
		printf '%s\n' 'def handle(_signum, _frame):'; \
		printf '%s\n' '    global stop'; \
		printf '%s\n' '    stop = True'; \
		printf '%s\n' 'signal.signal(signal.SIGINT, handle)'; \
		printf '%s\n' 'sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)'; \
		printf '%s\n' 'sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)'; \
		printf '%s\n' 'sock.bind(("127.0.0.1", port))'; \
		printf '%s\n' 'sock.listen(16)'; \
		printf '%s\n' 'sock.setblocking(False)'; \
		printf '%s\n' 'clients = []'; \
		printf '%s\n' 'deadline = time.time() + 5.0'; \
		printf '%s\n' 'while not stop and time.time() < deadline:'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        client, _peer = sock.accept()'; \
		printf '%s\n' '        client.setblocking(False)'; \
		printf '%s\n' '        clients.append(client)'; \
		printf '%s\n' '    except BlockingIOError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' '    for client in list(clients):'; \
		printf '%s\n' '        try:'; \
		printf '%s\n' '            data = client.recv(4096)'; \
		printf '%s\n' '            if data:'; \
		printf '%s\n' '                client.sendall(b"x\n")'; \
		printf '%s\n' '            elif data == b"":'; \
		printf '%s\n' '                clients.remove(client)'; \
		printf '%s\n' '                client.close()'; \
		printf '%s\n' '        except BlockingIOError:'; \
		printf '%s\n' '            pass'; \
		printf '%s\n' '        except OSError:'; \
		printf '%s\n' '            clients.remove(client)'; \
		printf '%s\n' '    time.sleep(0.001)'; \
		printf '%s\n' 'for client in clients:'; \
		printf '%s\n' '    try:'; \
		printf '%s\n' '        client.close()'; \
		printf '%s\n' '    except OSError:'; \
		printf '%s\n' '        pass'; \
		printf '%s\n' 'sock.close()'; \
	} > "$$tmp_dir/malicious_server_parent.py"; \
	chmod +x "$$tmp_dir/malicious_server_parent.py"; \
	mkdir "$$tmp_dir/outside"; \
	printf 'server stopped; outbox_full_drops=654321 outbox_closed_drops=0 broadcast_messages_created=1 broadcast_deliveries_attempted=1 broadcast_deliveries_enqueued=1\n' > "$$tmp_dir/outside/stats.txt"; \
	if ! LLAM_MALICIOUS_STATS_DIR="$$tmp_dir/outside" ./server_flood --server "$$tmp_dir/malicious_server_parent.py" --clients 2 --duration 0.05 --drain-sec 0.05 --message-bytes 8 --batch 1 --target-mps 0.001 --min-delivery-mps 0 --min-delivery-ratio 0 --allow-forced-stop --allow-missing-stats >"$$tmp_dir/flood.out" 2>&1; then \
		echo "server_flood parent symlink stats regression failed unexpectedly" >&2; \
		cat "$$tmp_dir/flood.out" >&2; \
		exit 1; \
	fi; \
	if grep 'outbox_full_drops=654321' "$$tmp_dir/flood.out" >/dev/null; then \
		echo "server_flood accepted stats through a symlinked parent directory" >&2; \
		cat "$$tmp_dir/flood.out" >&2; \
		exit 1; \
	fi; \
	grep 'server flood stats: unavailable' "$$tmp_dir/flood.out" >/dev/null
	@tmp_out="$$(mktemp "$${TMPDIR:-/tmp}/llam-server-flood-dump-path.XXXXXX")"; \
	trap 'rm -f "$$tmp_out"' 0 1 2 3 15; \
	long_dump_dir="$$(python3 -c 'print("/tmp/" + "x" * 600)')"; \
	if LLAM_SERVER_FLOOD_DUMP_DIR="$$long_dump_dir" ./server_flood --clients 2 --duration 0.01 >"$$tmp_out" 2>&1; then \
		echo "server_flood accepted truncated runtime dump path" >&2; \
		cat "$$tmp_out" >&2; \
		exit 1; \
	fi; \
	grep 'dump path' "$$tmp_out" >/dev/null
	@tmp_out="$$(mktemp "$${TMPDIR:-/tmp}/llam-server-flood-tmpdir.XXXXXX")"; \
	trap 'rm -f "$$tmp_out"' 0 1 2 3 15; \
	long_tmp_dir="$$(python3 -c 'print("/tmp/" + "x" * 600)')"; \
	if TMPDIR="$$long_tmp_dir" ./server_flood --clients 2 --duration 0.01 >"$$tmp_out" 2>&1; then \
		echo "server_flood accepted truncated stats temp path" >&2; \
		cat "$$tmp_out" >&2; \
		exit 1; \
	fi; \
	grep 'stats dir path' "$$tmp_out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-server-diagnostics-symlink.XXXXXX")"; \
	trap 'if test -n "$${server_pid:-}"; then kill -TERM "$$server_pid" >/dev/null 2>&1 || true; wait "$$server_pid" >/dev/null 2>&1 || true; fi; rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	port="$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"; \
	: > "$$tmp_dir/outside-stats"; \
	: > "$$tmp_dir/outside-dump"; \
	ln -s "$$tmp_dir/outside-stats" "$$tmp_dir/stats-link"; \
	ln -s "$$tmp_dir/outside-dump" "$$tmp_dir/dump-link"; \
	LLAM_CHAT_QUIET=1 LLAM_CHAT_STATS_PATH="$$tmp_dir/stats-link" LLAM_CHAT_DUMP_ON_STOP="$$tmp_dir/dump-link" ./server "$$port" >"$$tmp_dir/server.out" 2>&1 & \
	server_pid="$$!"; \
	sleep 0.5; \
	if ! kill -INT "$$server_pid" >/dev/null 2>&1; then \
		echo "server exited before diagnostic symlink test could signal it" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi; \
	if ! wait "$$server_pid"; then \
		echo "server failed during diagnostic symlink test" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi; \
	server_pid=""; \
	if test -s "$$tmp_dir/outside-stats" || test -s "$$tmp_dir/outside-dump"; then \
		echo "server followed a diagnostic symlink path" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-server-diagnostics-hardlink.XXXXXX")"; \
	trap 'if test -n "$${server_pid:-}"; then kill -TERM "$$server_pid" >/dev/null 2>&1 || true; wait "$$server_pid" >/dev/null 2>&1 || true; fi; rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	port="$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"; \
	printf 'outside-stats\n' > "$$tmp_dir/outside-stats"; \
	printf 'outside-dump\n' > "$$tmp_dir/outside-dump"; \
	ln "$$tmp_dir/outside-stats" "$$tmp_dir/stats-hardlink"; \
	ln "$$tmp_dir/outside-dump" "$$tmp_dir/dump-hardlink"; \
	LLAM_CHAT_QUIET=1 LLAM_CHAT_STATS_PATH="$$tmp_dir/stats-hardlink" LLAM_CHAT_DUMP_ON_STOP="$$tmp_dir/dump-hardlink" ./server "$$port" >"$$tmp_dir/server.out" 2>&1 & \
	server_pid="$$!"; \
	sleep 0.5; \
	if ! kill -INT "$$server_pid" >/dev/null 2>&1; then \
		echo "server exited before diagnostic hardlink test could signal it" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi; \
	if ! wait "$$server_pid"; then \
		echo "server failed during diagnostic hardlink test" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi; \
	server_pid=""; \
	grep '^outside-stats$$' "$$tmp_dir/outside-stats" >/dev/null; \
	grep '^outside-dump$$' "$$tmp_dir/outside-dump" >/dev/null; \
	test "$$(wc -l < "$$tmp_dir/outside-stats" | tr -d ' ')" = 1; \
	test "$$(wc -l < "$$tmp_dir/outside-dump" | tr -d ' ')" = 1
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-server-diagnostics-parent-symlink.XXXXXX")"; \
	trap 'if test -n "$${server_pid:-}"; then kill -TERM "$$server_pid" >/dev/null 2>&1 || true; wait "$$server_pid" >/dev/null 2>&1 || true; fi; rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	port="$$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"; \
	mkdir "$$tmp_dir/outside"; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/diag-link"; \
	LLAM_CHAT_QUIET=1 LLAM_CHAT_STATS_PATH="$$tmp_dir/diag-link/stats.txt" LLAM_CHAT_DUMP_ON_STOP="$$tmp_dir/diag-link/dump.txt" ./server "$$port" >"$$tmp_dir/server.out" 2>&1 & \
	server_pid="$$!"; \
	sleep 0.5; \
	if ! kill -INT "$$server_pid" >/dev/null 2>&1; then \
		echo "server exited before parent diagnostic symlink test could signal it" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi; \
	if ! wait "$$server_pid"; then \
		echo "server failed during parent diagnostic symlink test" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi; \
	server_pid=""; \
	if test -e "$$tmp_dir/outside/stats.txt" || test -e "$$tmp_dir/outside/dump.txt"; then \
		echo "server followed a diagnostic parent symlink path" >&2; \
		cat "$$tmp_dir/server.out" >&2; \
		exit 1; \
	fi
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-stress-dump-parent-symlink.XXXXXX")"; \
	trap 'if test -n "$${stress_pid:-}"; then kill -TERM "$$stress_pid" >/dev/null 2>&1 || true; wait "$$stress_pid" >/dev/null 2>&1 || true; fi; rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir "$$tmp_dir/outside"; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/diag-link"; \
	LLAM_STRESS_ROUNDS=1 LLAM_STRESS_DYNAMIC_ROUNDS=1 LLAM_RUNTIME_DUMP_ON_SIGNAL="$$tmp_dir/diag-link/dump.txt" ./stress >"$$tmp_dir/stress.out" 2>&1 & \
	stress_pid="$$!"; \
	ready=0; \
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do \
		if grep 'signal dump path' "$$tmp_dir/stress.out" >/dev/null 2>&1; then \
			ready=1; \
			break; \
		fi; \
		if ! kill -0 "$$stress_pid" >/dev/null 2>&1; then \
			break; \
		fi; \
		sleep 0.05; \
	done; \
	if test "$$ready" != 1; then \
		echo "stress signal dump test did not reach signal setup" >&2; \
		cat "$$tmp_dir/stress.out" >&2; \
		exit 1; \
	fi; \
	sleep 0.05; \
	sent_signal=0; \
	for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do \
		if kill -0 "$$stress_pid" >/dev/null 2>&1; then \
			kill -USR2 "$$stress_pid" >/dev/null 2>&1 || true; \
			sent_signal=1; \
			sleep 0.05; \
		else \
			break; \
		fi; \
	done; \
	if ! wait "$$stress_pid"; then \
		echo "stress failed during parent diagnostic symlink test" >&2; \
		cat "$$tmp_dir/stress.out" >&2; \
		exit 1; \
	fi; \
	stress_pid=""; \
	test "$$sent_signal" = 1; \
	if test -e "$$tmp_dir/outside/dump.txt"; then \
		echo "stress followed a diagnostic parent symlink path" >&2; \
		cat "$$tmp_dir/stress.out" >&2; \
		exit 1; \
	fi
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-python-safe-output.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	: > "$$tmp_dir/outside-json"; \
	ln -s "$$tmp_dir/outside-json" "$$tmp_dir/result-link.json"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; from safe_output import write_text_safely; write_text_safely(Path("'"$$tmp_dir/result-link.json"'"), "x")' >"$$tmp_dir/safe-output.out" 2>&1; then \
		echo "safe_output followed a symlink output path" >&2; \
		cat "$$tmp_dir/safe-output.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output path' "$$tmp_dir/safe-output.out" >/dev/null; \
	test ! -s "$$tmp_dir/outside-json"; \
	printf 'outside\n' > "$$tmp_dir/outside-hardlink.json"; \
	ln "$$tmp_dir/outside-hardlink.json" "$$tmp_dir/result-hardlink.json"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; from safe_output import write_text_safely; write_text_safely(Path("'"$$tmp_dir/result-hardlink.json"'"), "x")' >"$$tmp_dir/safe-output-hardlink.out" 2>&1; then \
		echo "safe_output overwrote a hard-linked output path" >&2; \
		cat "$$tmp_dir/safe-output-hardlink.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing hard-linked output path' "$$tmp_dir/safe-output-hardlink.out" >/dev/null; \
	grep '^outside$$' "$$tmp_dir/outside-hardlink.json" >/dev/null; \
	mkdir "$$tmp_dir/outside-dir"; \
	ln -s "$$tmp_dir/outside-dir" "$$tmp_dir/link-dir"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; from safe_output import write_text_safely; write_text_safely(Path("'"$$tmp_dir/link-dir/result.json"'"), "x")' >"$$tmp_dir/safe-output-parent.out" 2>&1; then \
		echo "safe_output followed a symlink output directory" >&2; \
		cat "$$tmp_dir/safe-output-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/safe-output-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/result.json"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; from safe_output import write_text_safely; write_text_safely(Path("'"$$tmp_dir/link-dir/sub/result.json"'"), "x")' >"$$tmp_dir/safe-output-deep-parent.out" 2>&1; then \
		echo "safe_output followed a symlink output directory before creating a child path" >&2; \
		cat "$$tmp_dir/safe-output-deep-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/safe-output-deep-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/sub/result.json"; \
	if (cd "$$tmp_dir" && PYTHONPATH="$(CURDIR)/scripts" python3 -c 'from pathlib import Path; from safe_output import write_text_safely; write_text_safely(Path("link-dir/relative.json"), "x")') >"$$tmp_dir/safe-output-relative-parent.out" 2>&1; then \
		echo "safe_output followed a relative symlink output directory" >&2; \
		cat "$$tmp_dir/safe-output-relative-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/safe-output-relative-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/relative.json"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; from safe_output import open_binary_for_write; handle = open_binary_for_write(Path("'"$$tmp_dir/link-dir/graph/runtime.png"'")); handle.write(b"x"); handle.close()' >"$$tmp_dir/safe-output-binary-parent.out" 2>&1; then \
		echo "safe_output binary writer followed a symlink output directory" >&2; \
		cat "$$tmp_dir/safe-output-binary-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/safe-output-binary-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/graph/runtime.png"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; import safe_output; safe_output._CAN_USE_DIR_FD = False; safe_output.write_text_safely(Path("'"$$tmp_dir/link-dir/fallback.json"'"), "x")' >"$$tmp_dir/safe-output-fallback-parent.out" 2>&1; then \
		echo "safe_output fallback followed a symlink output directory" >&2; \
		cat "$$tmp_dir/safe-output-fallback-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/safe-output-fallback-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/fallback.json"; \
	if PYTHONPATH=scripts python3 -c 'from pathlib import Path; import safe_output; safe_output._CAN_USE_DIR_FD = False; safe_output.write_text_safely(Path("'"$$tmp_dir/result-link.json"'"), "x")' >"$$tmp_dir/safe-output-fallback-leaf.out" 2>&1; then \
		echo "safe_output fallback followed a symlink output path" >&2; \
		cat "$$tmp_dir/safe-output-fallback-leaf.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output path' "$$tmp_dir/safe-output-fallback-leaf.out" >/dev/null; \
	test ! -s "$$tmp_dir/outside-json"; \
	if PYTHONPATH=scripts python3 scripts/safe_output.py --prepare-dir "$$tmp_dir/link-dir/artifacts" >"$$tmp_dir/safe-output-cli-dir.out" 2>&1; then \
		echo "safe_output CLI followed a symlink artifact directory" >&2; \
		cat "$$tmp_dir/safe-output-cli-dir.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/safe-output-cli-dir.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/artifacts"; \
	if python3 scripts/run_with_timeout.py --timeout 1 --dump-on-timeout "$$tmp_dir/link-dir/dump/runtime.txt" --log "$$tmp_dir/timeout.log" -- python3 -c 'print("ok")' >"$$tmp_dir/run-timeout-dump-parent.out" 2>&1; then \
		echo "run_with_timeout followed a symlink dump output directory" >&2; \
		cat "$$tmp_dir/run-timeout-dump-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/run-timeout-dump-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/dump/runtime.txt"; \
	if LLAM_SERVER_COMPOSITE_DUMP_DIR="$$tmp_dir/link-dir/composite" PYTHONPATH=scripts python3 -c 'from pathlib import Path; import stress_server_composite as s; s.start_server(Path("/definitely/missing/llam-server"), "127.0.0.1", 0.01)' >"$$tmp_dir/composite-dump-parent.out" 2>&1; then \
		echo "stress_server_composite followed a symlink dump output directory" >&2; \
		cat "$$tmp_dir/composite-dump-parent.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink output directory' "$$tmp_dir/composite-dump-parent.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside-dir/composite"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-dest-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib" "$$tmp_dir/outside" "$$tmp_dir/prefix"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/prefix/include"; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh followed a destination include symlink" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink component in install destination' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside/llam/runtime.h"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-dest-hardlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib" "$$tmp_dir/prefix/include/llam"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	printf 'new\n' > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	printf 'outside\n' > "$$tmp_dir/outside-runtime.h"; \
	ln "$$tmp_dir/outside-runtime.h" "$$tmp_dir/prefix/include/llam/runtime.h"; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh overwrote a hard-linked destination file" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing hard-linked destination inside install prefix' "$$tmp_dir/install.out" >/dev/null; \
	grep '^outside$$' "$$tmp_dir/outside-runtime.h" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-source-hardlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	printf 'outside-secret\n' > "$$tmp_dir/outside-runtime.h"; \
	ln "$$tmp_dir/outside-runtime.h" "$$tmp_dir/pkg/include/llam/runtime.h"; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh copied a hard-linked source file from the install tree" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing hard-linked file in LLAM install tree' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/prefix/include/llam/runtime.h"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-prefix-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib" "$$tmp_dir/outside"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/prefix"; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix/" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh followed a symlink install prefix" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink component in install prefix' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside/include/llam/runtime.h"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-prefix-parent-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib" "$$tmp_dir/outside"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/link-parent"; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/link-parent/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh followed a symlink parent in the install prefix" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink component in install prefix' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/outside/prefix/include/llam/runtime.h"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-unexpected-tree-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib" "$$tmp_dir/pkg/share"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/payload.h"; \
	ln -s payload.h "$$tmp_dir/pkg/include/llam/runtime.h"; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted an unexpected archive-local include symlink" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unexpected symlink in LLAM install tree' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/prefix/include/llam/runtime.h"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-dangling-lib-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) ln -s libllam_runtime.2.dylib "$$tmp_dir/pkg/lib/libllam_runtime.dylib" ;; \
		Linux) ln -s libllam_runtime.so.2 "$$tmp_dir/pkg/lib/libllam_runtime.so" ;; \
		*) echo "unsupported installer dangling lib-link smoke host" >&2; exit 1 ;; \
	esac; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a dangling archive-local library symlink" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing dangling symlink in LLAM install tree' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/prefix/lib/libllam_runtime.dylib"; \
	test ! -e "$$tmp_dir/prefix/lib/libllam_runtime.so"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-wrong-lib-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.999.dylib"; \
			ln -s libllam_runtime.999.dylib "$$tmp_dir/pkg/lib/libllam_runtime.dylib"; \
			;; \
		Linux) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.so.999"; \
			ln -s libllam_runtime.so.999 "$$tmp_dir/pkg/lib/libllam_runtime.so"; \
			;; \
		*) echo "unsupported installer wrong lib-link smoke host" >&2; exit 1 ;; \
	esac; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted an ABI-mismatched archive-local library symlink" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unexpected symlink in LLAM install tree' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/prefix/lib/libllam_runtime.dylib"; \
	test ! -e "$$tmp_dir/prefix/lib/libllam_runtime.so"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-regular-lib-link.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.2.dylib"; \
			printf 'not a symlink\n' > "$$tmp_dir/pkg/lib/libllam_runtime.dylib"; \
			;; \
		Linux) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.so.2.0.0"; \
			printf 'not a symlink\n' > "$$tmp_dir/pkg/lib/libllam_runtime.so.2"; \
			;; \
		*) echo "unsupported installer regular lib-link smoke host" >&2; exit 1 ;; \
	esac; \
	if sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a non-symlink archive-local library link" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing non-symlink LLAM library link in install tree' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/prefix/lib/libllam_runtime.dylib"; \
	test ! -e "$$tmp_dir/prefix/lib/libllam_runtime.so.2"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-prerelease-lib-version.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	mkdir -p "$$tmp_dir/pkg/include/llam" "$$tmp_dir/pkg/lib"; \
	cp scripts/install.sh "$$tmp_dir/pkg/install.sh"; \
	: > "$$tmp_dir/pkg/include/llam/runtime.h"; \
	printf '2.0.0-rc.1\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	: > "$$tmp_dir/pkg/lib/libllam_runtime.so.2.0.0"; \
	ln -s libllam_runtime.so.2.0.0 "$$tmp_dir/pkg/lib/libllam_runtime.so.2"; \
	ln -s libllam_runtime.so.2 "$$tmp_dir/pkg/lib/libllam_runtime.so"; \
	sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; \
	test -L "$$tmp_dir/prefix/lib/libllam_runtime.so"; \
	grep '^2.0.0-rc.1$$' "$$tmp_dir/prefix/share/llam/VERSION" >/dev/null; \
	grep '^2.0.0$$' "$$tmp_dir/prefix/share/llam/LIBRARY_VERSION" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-output-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package symlink smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts" "$$tmp_dir/repo/docs" "$$tmp_dir/repo/include/llam" "$$tmp_dir/repo/examples" "$$tmp_dir/outside"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	: > "$$tmp_dir/repo/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/examples/smoke.c"; \
	: > "$$tmp_dir/repo/examples/smoke.h"; \
	: > "$$tmp_dir/repo/demo"; \
	: > "$$tmp_dir/repo/stress"; \
	: > "$$tmp_dir/repo/bench"; \
	: > "$$tmp_dir/repo/server"; \
	: > "$$tmp_dir/repo/server_lossless"; \
	: > "$$tmp_dir/repo/server_flood"; \
	: > "$$tmp_dir/repo/libllam_runtime.a"; \
	case "$$package_target" in \
		macos-*) : > "$$tmp_dir/repo/libllam_runtime.2.dylib"; ln -s libllam_runtime.2.dylib "$$tmp_dir/repo/libllam_runtime.dylib" ;; \
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.0.0"; ln -s libllam_runtime.so.2.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/repo/target"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh followed a symlink release output path" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink release output path' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-output-file-component.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package output file-component smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	: > "$$tmp_dir/repo/target"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh accepted a non-directory release output path component" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing non-directory release output path component' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-unsafe-stage-mode.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package unsafe-mode smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts" "$$tmp_dir/repo/docs" "$$tmp_dir/repo/include/llam" "$$tmp_dir/repo/examples"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	: > "$$tmp_dir/repo/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/examples/smoke.c"; \
	: > "$$tmp_dir/repo/examples/smoke.h"; \
	: > "$$tmp_dir/repo/demo"; \
	: > "$$tmp_dir/repo/stress"; \
	: > "$$tmp_dir/repo/bench"; \
	: > "$$tmp_dir/repo/server"; \
	: > "$$tmp_dir/repo/server_lossless"; \
	: > "$$tmp_dir/repo/server_flood"; \
	: > "$$tmp_dir/repo/libllam_runtime.a"; \
	case "$$package_target" in \
		macos-*) : > "$$tmp_dir/repo/libllam_runtime.2.dylib"; ln -s libllam_runtime.2.dylib "$$tmp_dir/repo/libllam_runtime.dylib" ;; \
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.0.0"; ln -s libllam_runtime.so.2.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	chmod 666 "$$tmp_dir/repo/README.md"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh accepted an unsafe release stage mode" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unsafe release stage mode' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-input-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package input smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts" "$$tmp_dir/repo/docs" "$$tmp_dir/repo/include/llam" "$$tmp_dir/repo/examples" "$$tmp_dir/outside"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	: > "$$tmp_dir/repo/LICENSE"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/examples/smoke.c"; \
	: > "$$tmp_dir/repo/examples/smoke.h"; \
	: > "$$tmp_dir/repo/demo"; \
	: > "$$tmp_dir/repo/stress"; \
	: > "$$tmp_dir/repo/bench"; \
	: > "$$tmp_dir/repo/server"; \
	: > "$$tmp_dir/repo/server_lossless"; \
	: > "$$tmp_dir/repo/server_flood"; \
	: > "$$tmp_dir/repo/libllam_runtime.a"; \
	: > "$$tmp_dir/outside/README.md"; \
	ln -s "$$tmp_dir/outside/README.md" "$$tmp_dir/repo/README.md"; \
	case "$$package_target" in \
		macos-*) : > "$$tmp_dir/repo/libllam_runtime.2.dylib"; ln -s libllam_runtime.2.dylib "$$tmp_dir/repo/libllam_runtime.dylib" ;; \
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.0.0"; ln -s libllam_runtime.so.2.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh followed a symlink release input path" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing symlink release input path' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-input-hardlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package input hardlink smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts" "$$tmp_dir/repo/docs" "$$tmp_dir/repo/include/llam" "$$tmp_dir/repo/examples"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	cp scripts/generate_sdk_metadata.sh "$$tmp_dir/repo/scripts/generate_sdk_metadata.sh"; \
	: > "$$tmp_dir/repo/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/examples/smoke.c"; \
	: > "$$tmp_dir/repo/examples/smoke.h"; \
	: > "$$tmp_dir/repo/demo"; \
	: > "$$tmp_dir/repo/stress"; \
	: > "$$tmp_dir/repo/bench"; \
	: > "$$tmp_dir/repo/server"; \
	: > "$$tmp_dir/repo/server_lossless"; \
	: > "$$tmp_dir/repo/server_flood"; \
	: > "$$tmp_dir/repo/libllam_runtime.a"; \
	printf 'outside-secret\n' > "$$tmp_dir/outside-runtime.h"; \
	ln "$$tmp_dir/outside-runtime.h" "$$tmp_dir/repo/include/llam/runtime.h"; \
	case "$$package_target" in \
		macos-*) : > "$$tmp_dir/repo/libllam_runtime.2.dylib"; ln -s libllam_runtime.2.dylib "$$tmp_dir/repo/libllam_runtime.dylib" ;; \
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.0.0"; ln -s libllam_runtime.so.2.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh copied a hard-linked release input file" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing hard-linked file in release input tree' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-liblink-target.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package liblink smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts" "$$tmp_dir/repo/docs" "$$tmp_dir/repo/include/llam" "$$tmp_dir/repo/examples"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	: > "$$tmp_dir/repo/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/examples/smoke.c"; \
	: > "$$tmp_dir/repo/examples/smoke.h"; \
	: > "$$tmp_dir/repo/demo"; \
	: > "$$tmp_dir/repo/stress"; \
	: > "$$tmp_dir/repo/bench"; \
	: > "$$tmp_dir/repo/server"; \
	: > "$$tmp_dir/repo/server_lossless"; \
	: > "$$tmp_dir/repo/server_flood"; \
	: > "$$tmp_dir/repo/libllam_runtime.a"; \
	case "$$package_target" in \
		macos-*) : > "$$tmp_dir/repo/libllam_runtime.2.dylib"; ln -s README.md "$$tmp_dir/repo/libllam_runtime.dylib" ;; \
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.0.0"; ln -s libllam_runtime.so.2.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s README.md "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh accepted an unexpected library symlink target" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unexpected release symlink target' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-regular-liblink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported package regular-liblink smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts" "$$tmp_dir/repo/docs" "$$tmp_dir/repo/include/llam" "$$tmp_dir/repo/examples"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	cp scripts/generate_sdk_metadata.sh "$$tmp_dir/repo/scripts/generate_sdk_metadata.sh"; \
	: > "$$tmp_dir/repo/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/examples/smoke.c"; \
	: > "$$tmp_dir/repo/examples/smoke.h"; \
	: > "$$tmp_dir/repo/demo"; \
	: > "$$tmp_dir/repo/stress"; \
	: > "$$tmp_dir/repo/bench"; \
	: > "$$tmp_dir/repo/server"; \
	: > "$$tmp_dir/repo/server_lossless"; \
	: > "$$tmp_dir/repo/server_flood"; \
	: > "$$tmp_dir/repo/libllam_runtime.a"; \
	case "$$package_target" in \
		macos-*) : > "$$tmp_dir/repo/libllam_runtime.2.dylib"; : > "$$tmp_dir/repo/libllam_runtime.dylib" ;; \
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.0.0"; : > "$$tmp_dir/repo/libllam_runtime.so.2"; : > "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
		echo "package_release.sh accepted non-symlink shared-library link artifacts" >&2; \
		cat "$$tmp_dir/package.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing non-symlink release library link' "$$tmp_dir/package.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-malformed-checksum.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer checksum smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/lib"; \
	cp scripts/install.sh "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	tar -C "$$tmp_dir/archive" -cJf "$$tmp_dir/$$package.tar.xz" "$$package"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		digest="$$(sha256sum "$$tmp_dir/$$package.tar.xz" | awk '{ print $$1 }')"; \
	else \
		digest="$$(shasum -a 256 "$$tmp_dir/$$package.tar.xz" | awk '{ print $$1 }')"; \
	fi; \
	{ \
		printf '%s  %s trailing-field\n' "$$digest" "$$package.tar.xz"; \
		printf '%064d  other-file\n' 0; \
	} > "$$tmp_dir/$$package.tar.xz.sha256"; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a malformed checksum sidecar" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'invalid checksum file' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-glob-checksum.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer checksum-glob smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/lib"; \
	cp scripts/install.sh "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	tar -C "$$tmp_dir/archive" -cJf "$$tmp_dir/$$package.tar.xz" "$$package"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		digest="$$(sha256sum "$$tmp_dir/$$package.tar.xz" | awk '{ print $$1 }')"; \
	else \
		digest="$$(shasum -a 256 "$$tmp_dir/$$package.tar.xz" | awk '{ print $$1 }')"; \
	fi; \
	printf '%s  *.tar.xz\n' "$$digest" > "$$tmp_dir/$$package.tar.xz.sha256"; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a glob checksum target" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'checksum target' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-archive-script.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer archive-script smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/lib"; \
	printf '#!/bin/sh\nprintf exploited > %s/marker\nexit 0\n' "$$tmp_dir" > "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	tar -C "$$tmp_dir/archive" -cJf "$$tmp_dir/$$package.tar.xz" "$$package"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; \
	if [ -f "$$tmp_dir/marker" ]; then \
		echo "install.sh executed installer code from the downloaded archive" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	test -f "$$tmp_dir/prefix/include/llam/runtime.h"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-unsafe-mode-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer unsafe-mode smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/bin" "$$tmp_dir/archive/$$package/lib"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	printf 'payload\n' > "$$tmp_dir/archive/$$package/bin/llam-danger"; \
	chmod 4777 "$$tmp_dir/archive/$$package/bin/llam-danger"; \
	tar -C "$$tmp_dir/archive" -cJf "$$tmp_dir/$$package.tar.xz" "$$package"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted an archive member with unsafe mode bits" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unsafe archive member mode' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-hardlink-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer hardlink smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	printf payload > "$$tmp_dir/archive/$$package/payload"; \
	ln "$$tmp_dir/archive/$$package/payload" "$$tmp_dir/archive/$$package/payload-hardlink"; \
	tar -C "$$tmp_dir/archive" -cJf "$$tmp_dir/$$package.tar.xz" "$$package"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a hard-link archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing hard-link archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-duplicate-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer duplicate smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	printf first > "$$tmp_dir/archive/$$package/payload"; \
	tar -C "$$tmp_dir/archive" -cf "$$tmp_dir/$$package.tar" "$$package"; \
	printf second > "$$tmp_dir/archive/$$package/payload"; \
	tar -C "$$tmp_dir/archive" -rf "$$tmp_dir/$$package.tar" "$$package/payload"; \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a duplicate archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing duplicate archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-dot-component-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer dot-component smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	printf first > "$$tmp_dir/archive/$$package/payload"; \
	tar -C "$$tmp_dir/archive" -cf "$$tmp_dir/$$package.tar" "$$package"; \
	printf second > "$$tmp_dir/archive/$$package/payload"; \
	(cd "$$tmp_dir/archive" && tar -rf "$$tmp_dir/$$package.tar" "$$package/./payload"); \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a non-canonical ./ archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing non-canonical archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-double-slash-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer double-slash smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	printf first > "$$tmp_dir/archive/$$package/payload"; \
	tar -C "$$tmp_dir/archive" -cf "$$tmp_dir/$$package.tar" "$$package"; \
	printf second > "$$tmp_dir/archive/$$package/payload"; \
	(cd "$$tmp_dir/archive" && tar -rf "$$tmp_dir/$$package.tar" "$$package//payload"); \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a non-canonical // archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing non-canonical archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-casefold-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer casefold smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	{ \
		printf '%s\n' 'import io'; \
		printf '%s\n' 'import sys'; \
		printf '%s\n' 'import tarfile'; \
		printf '%s\n' 'from pathlib import Path'; \
		printf '%s\n' 'tmp = Path(sys.argv[1])'; \
		printf '%s\n' 'package = sys.argv[2]'; \
		printf '%s\n' 'entries = ('; \
		printf '%s\n' '    (f"{package}/install.sh", b"#!/bin/sh\nexit 0\n", 0o755),'; \
		printf '%s\n' '    (f"{package}/payload", b"lower", 0o644),'; \
		printf '%s\n' '    (f"{package}/PAYLOAD", b"upper", 0o644),'; \
		printf '%s\n' ')'; \
		printf '%s\n' 'with tarfile.open(tmp / f"{package}.tar", "w") as archive:'; \
		printf '%s\n' '    for name, data, mode in entries:'; \
		printf '%s\n' '        info = tarfile.TarInfo(name)'; \
		printf '%s\n' '        info.size = len(data)'; \
		printf '%s\n' '        info.mode = mode'; \
		printf '%s\n' '        archive.addfile(info, io.BytesIO(data))'; \
	} > "$$tmp_dir/casefold_archive.py"; \
	python3 "$$tmp_dir/casefold_archive.py" "$$tmp_dir" "$$package"; \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a case-insensitive duplicate archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing case-insensitive duplicate archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-type-collision-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer type-collision smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	printf file > "$$tmp_dir/archive/$$package/collision"; \
	tar -C "$$tmp_dir/archive" -cf "$$tmp_dir/$$package.tar" "$$package"; \
	rm -f "$$tmp_dir/archive/$$package/collision"; \
	mkdir -p "$$tmp_dir/archive/$$package/collision"; \
	tar -C "$$tmp_dir/archive" -rf "$$tmp_dir/$$package.tar" "$$package/collision"; \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted an archive path type collision" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing duplicate archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-symlink-target.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		*) echo "unsupported installer symlink-target smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	ln -s 'payload target' "$$tmp_dir/archive/$$package/payload-link"; \
	tar -C "$$tmp_dir/archive" -cJf "$$tmp_dir/$$package.tar.xz" "$$package"; \
	if command -v sha256sum >/dev/null 2>&1; then \
		(cd "$$tmp_dir" && sha256sum "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	else \
		(cd "$$tmp_dir" && shasum -a 256 "$$package.tar.xz" > "$$package.tar.xz.sha256"); \
	fi; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted an unsafe archive symlink target" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unsafe archive link target' "$$tmp_dir/install.out" >/dev/null
	$(MAKE) test-no-owner

check: test

ASAN_TEST_TARGETS = \
	asan-test_runtime_api_edges \
	asan-test_runtime_core \
	asan-test_io_buffers \
	asan-test_runtime_shutdown_internal \
	asan-test_multi_runtime_core \
	asan-test_runtime_fuzz \
	asan-test_security_capability

NOOWNER_TEST_TARGETS = \
	noowner-test_runtime_select_edges

TSAN_TEST_TARGETS = \
	tsan-test_runtime_core \
	tsan-test_runtime_shutdown_internal \
	tsan-test_multi_runtime_core \
	tsan-test_runtime_fuzz \
	tsan-test_security_capability

FUZZ_HEAVY_RUNTIME_SCENARIOS ?= 512
FUZZ_HEAVY_MULTI_RUNTIME_SCENARIOS ?= 128

test-asan:
	@set -e; \
	cleanup() { rm -f $(ASAN_TEST_TARGETS); }; \
	trap cleanup EXIT; \
	cleanup; \
	$(MAKE) $(ASAN_TEST_TARGETS) \
		OBJDIR=object-asan \
		SANITIZER_TARGETS_ENABLED=1 \
		CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
		LDLIBS="$(LDLIBS) -fsanitize=address,undefined"; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_api_edges; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_core; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_io_buffers; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_shutdown_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_multi_runtime_core; \
	LLAM_RUNTIME_FUZZ_SCENARIOS=16 LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS=16 \
		ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_fuzz; \
	: "Keep the ASan broker task-race probe short; full-strength coverage stays in normal test."; \
	LLAM_SECURITY_TASK_DETACH_RACE_ROUNDS=16 \
		ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_security_capability

test-no-owner:
	@set -e; \
	cleanup() { rm -f $(NOOWNER_TEST_TARGETS); }; \
	trap cleanup EXIT; \
	cleanup; \
	$(MAKE) $(NOOWNER_TEST_TARGETS) \
		OBJDIR=object-noowner \
		CPPFLAGS="$(CPPFLAGS) -DLLAM_RUNTIME_DISABLE_OWNER_CHECKS=1"; \
	./noowner-test_runtime_select_edges

test-tsan:
	@set -e; \
	cleanup() { rm -f $(TSAN_TEST_TARGETS); }; \
	trap cleanup EXIT; \
	cleanup; \
	tsan_cflags="-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=thread"; \
	tsan_probe="$$(mktemp "$${TMPDIR:-/tmp}/llam-tsan-probe.XXXXXX.o")"; \
	if printf 'int main(void){return 0;}\n' | $(CC) $$tsan_cflags -Wno-error=tsan -x c - -c -o "$$tsan_probe" >/dev/null 2>&1; then \
		tsan_cflags="$$tsan_cflags -Wno-error=tsan"; \
	fi; \
	rm -f "$$tsan_probe"; \
	$(MAKE) $(TSAN_TEST_TARGETS) \
		OBJDIR=object-tsan \
		SANITIZER_TARGETS_ENABLED=1 \
		CFLAGS="$$tsan_cflags" \
		LDLIBS="$(LDLIBS) -fsanitize=thread"; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_runtime_core; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_runtime_shutdown_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_multi_runtime_core; \
	LLAM_RUNTIME_FUZZ_SCENARIOS=8 LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS=8 \
		TSAN_OPTIONS=halt_on_error=1 ./tsan-test_runtime_fuzz; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_security_capability

analyze-cppcheck:
	cppcheck --platform=unix64 --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr \
		-DUINTPTR_MAX=18446744073709551615ULL -DUINT32_MAX=4294967295U \
		-Iinclude -Isrc/internal -Isrc src include tests examples

audit-deps:
	cd scripts/bench_tokio_compare && cargo audit

test-fuzz-heavy: test_runtime_fuzz
	LLAM_RUNTIME_FUZZ_SCENARIOS=$(FUZZ_HEAVY_RUNTIME_SCENARIOS) \
	LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS=$(FUZZ_HEAVY_MULTI_RUNTIME_SCENARIOS) \
		./test_runtime_fuzz

test-hardening: analyze-cppcheck audit-deps test-asan test-tsan test-fuzz-heavy

test-quick: test server-stress-composite-quick

test-full: test server-stress-composite

test-soak: test server-stress-composite-hour

ifeq ($(HOST_PLATFORM),darwin)
$(SHLIB_LINK): $(SHLIB_REAL)
	ln -sf $(SHLIB_REAL) $(SHLIB_LINK)

$(SHLIB_REAL): $(SHARED_RUNTIME_OBJS)
	$(CC) $(CFLAGS) $(SHLIB_LDFLAGS) -o $@ $(SHARED_RUNTIME_OBJS) $(LDLIBS)
else
$(SHLIB_LINK): $(SHLIB_SONAME)
	ln -sf $(SHLIB_SONAME) $(SHLIB_LINK)

$(SHLIB_SONAME): $(SHLIB_REAL)
	ln -sf $(SHLIB_REAL) $(SHLIB_SONAME)

$(SHLIB_REAL): $(SHARED_RUNTIME_OBJS)
	$(CC) $(CFLAGS) $(SHLIB_LDFLAGS) -o $@ $(SHARED_RUNTIME_OBJS) $(LDLIBS)
endif

demo: $(RUNTIME_OBJS) $(DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(DEMO_OBJS) $(LDLIBS)

stress: $(RUNTIME_OBJS) $(STRESS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(STRESS_OBJS) $(LDLIBS)

bench: $(RUNTIME_OBJS) $(BENCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(BENCH_OBJS) $(LDLIBS)

llam_broker: $(RUNTIME_OBJS) $(BROKER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(BROKER_OBJS) $(LDLIBS)

server: $(RUNTIME_OBJS) $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(SERVER_OBJS) $(LDLIBS)

server_lossless: $(RUNTIME_OBJS) $(SERVER_LOSSLESS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(SERVER_LOSSLESS_OBJS) $(LDLIBS)

server_flood: $(SERVER_FLOOD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_FLOOD_OBJS) $(SERVER_FLOOD_LDLIBS)

test_abi_contract: $(RUNTIME_OBJS) $(TEST_ABI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_ABI_OBJS) $(LDLIBS)

test_abi_compat: $(RUNTIME_OBJS) $(TEST_ABI_COMPAT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_ABI_COMPAT_OBJS) $(LDLIBS)

test_connect_io: $(RUNTIME_OBJS) $(TEST_CONNECT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_CONNECT_OBJS) $(LDLIBS)

test_runtime_core: $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS) $(LDLIBS)

test_multi_runtime_core: $(RUNTIME_OBJS) $(TEST_MULTI_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_MULTI_RUNTIME_CORE_OBJS) $(LDLIBS)

test_runtime_api_edges: $(RUNTIME_OBJS) $(TEST_RUNTIME_API_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_API_EDGES_OBJS) $(LDLIBS)

test_runtime_select_edges: $(RUNTIME_OBJS) $(TEST_RUNTIME_SELECT_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_SELECT_EDGES_OBJS) $(LDLIBS)

test_runtime_io_dump: $(RUNTIME_OBJS) $(TEST_RUNTIME_IO_DUMP_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_IO_DUMP_OBJS) $(LDLIBS)

test_runtime_group_local_edges: $(RUNTIME_OBJS) $(TEST_RUNTIME_GROUP_LOCAL_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_GROUP_LOCAL_EDGES_OBJS) $(LDLIBS)

test_runtime_unmanaged_join: $(RUNTIME_OBJS) $(TEST_RUNTIME_UNMANAGED_JOIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_UNMANAGED_JOIN_OBJS) $(LDLIBS)

test_runtime_stress: $(RUNTIME_OBJS) $(TEST_RUNTIME_STRESS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_STRESS_OBJS) $(LDLIBS)

test_runtime_fuzz: $(RUNTIME_OBJS) $(TEST_RUNTIME_FUZZ_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_FUZZ_OBJS) $(LDLIBS)

test_runtime_invariants: $(RUNTIME_OBJS) $(TEST_RUNTIME_INVARIANTS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_INVARIANTS_OBJS) $(LDLIBS)

test_runtime_shutdown_internal: $(RUNTIME_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS) $(LDLIBS)

test_sync_primitives: $(RUNTIME_OBJS) $(TEST_SYNC_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_SYNC_OBJS) $(LDLIBS)

test_io_buffers: $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS) $(LDLIBS)

asan-test_runtime_api_edges: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_API_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_API_EDGES_OBJS) $(LDLIBS)

asan-test_runtime_core tsan-test_runtime_core: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS) $(LDLIBS)

noowner-test_runtime_select_edges: $(RUNTIME_OBJS) $(TEST_RUNTIME_SELECT_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_SELECT_EDGES_OBJS) $(LDLIBS)

asan-test_io_buffers: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS) $(LDLIBS)

asan-test_runtime_shutdown_internal tsan-test_runtime_shutdown_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS) $(LDLIBS)

asan-test_multi_runtime_core tsan-test_multi_runtime_core: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_MULTI_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_MULTI_RUNTIME_CORE_OBJS) $(LDLIBS)

asan-test_runtime_fuzz tsan-test_runtime_fuzz: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_FUZZ_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_FUZZ_OBJS) $(LDLIBS)

asan-test_security_capability tsan-test_security_capability: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS) $(LDLIBS)

test_windows_policy: $(RUNTIME_OBJS) $(TEST_WINDOWS_POLICY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_WINDOWS_POLICY_OBJS) $(LDLIBS)

test_windows_runtime_smoke: $(RUNTIME_OBJS) $(TEST_WINDOWS_RUNTIME_SMOKE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_WINDOWS_RUNTIME_SMOKE_OBJS) $(LDLIBS)

test_windows_iocp_io: $(RUNTIME_OBJS) $(TEST_WINDOWS_IOCP_IO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_WINDOWS_IOCP_IO_OBJS) $(LDLIBS)

test_windows_iocp_dump: $(RUNTIME_OBJS) $(TEST_WINDOWS_IOCP_DUMP_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_WINDOWS_IOCP_DUMP_OBJS) $(LDLIBS)

test_windows_handle_io: $(RUNTIME_OBJS) $(TEST_WINDOWS_HANDLE_IO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_WINDOWS_HANDLE_IO_OBJS) $(LDLIBS)

test_security_capability: $(RUNTIME_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS) $(LDLIBS)

test_shared_load: $(TEST_SHARED_LOAD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SHARED_LOAD_OBJS) $(DL_LIBS)

$(OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/engine/%.o: src/engine/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/engine/%.o: src/engine/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/io/%.o: src/io/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/io/%.o: src/io/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/io/windows/%.o: src/io/windows/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/io/windows/%.o: src/io/windows/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/engine/runtime_watchdog.o: $(RUNTIME_ENGINE_FRAGMENTS)

$(SHARED_OBJDIR)/src/engine/runtime_watchdog.o: $(RUNTIME_ENGINE_FRAGMENTS)

$(OBJDIR)/src/asm/linux/x86_64/%.o: src/asm/linux/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/linux/x86_64/%.o: src/asm/linux/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/linux/arm64/%.o: src/asm/linux/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/linux/arm64/%.o: src/asm/linux/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/darwin/arm64/%.o: src/asm/darwin/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/darwin/arm64/%.o: src/asm/darwin/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/darwin/x86_64/%.o: src/asm/darwin/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/darwin/x86_64/%.o: src/asm/darwin/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/windows/x86_64/%.o: src/asm/windows/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/windows/x86_64/%.o: src/asm/windows/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo.o: examples/demo.c $(LLAM_PUBLIC_HDRS) examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo_tasks.o: examples/demo_tasks.c $(LLAM_PUBLIC_HDRS) examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo_entry.o: examples/demo_entry.c $(LLAM_PUBLIC_HDRS) examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress.o: examples/stress.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h examples/diagnostic_output.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/diagnostic_output.o: examples/diagnostic_output.c examples/diagnostic_output.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_support.o: examples/stress_support.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_tasks.o: examples/stress_tasks.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_core_cases.o: examples/stress_core_cases.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_timeout_cases.o: examples/stress_timeout_cases.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_dynamic_cases.o: examples/stress_dynamic_cases.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_suite.o: examples/stress_suite.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_entry.o: examples/stress_entry.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_signal_dump.o: examples/stress_signal_dump.c $(LLAM_PUBLIC_HDRS) examples/stress_internal.h examples/diagnostic_output.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench.o: examples/bench.c $(LLAM_PUBLIC_HDRS) examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench_support.o: examples/bench_support.c $(LLAM_PUBLIC_HDRS) examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench_entry.o: examples/bench_entry.c $(LLAM_PUBLIC_HDRS) examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/broker.o: examples/broker.c $(LLAM_PUBLIC_HDRS) $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server.o: examples/server.c examples/server_support.h $(LLAM_PUBLIC_HDRS) $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server_lossless.o: examples/server.c examples/server_support.h $(LLAM_PUBLIC_HDRS) $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_CHAT_LOSSLESS_DEFAULT=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server_support.o: examples/server_support.c examples/server_support.h examples/diagnostic_output.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server_flood.o: examples/server_flood.c examples/server_flood_stats.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server_flood_stats.o: examples/server_flood_stats.c examples/server_flood_stats.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Some tests intentionally include private runtime headers to validate teardown,
# ownership, and backend invariants. Rebuild all test objects on private layout
# changes so internal tests cannot link against a stale object view of structs.
$(OBJDIR)/tests/%.o: tests/%.c $(RUNTIME_PRIV_HDRS) tests/test_env.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(CLEAN_DIRS)
	rm -f $(CLEAN_FILES)
	for dir in src examples tests; do \
		if [ -d "$$dir" ]; then \
			find "$$dir" -name '*.o' -delete; \
		fi; \
	done

package: all test
	./scripts/package_release.sh

bench-matrix: bench
	python3 scripts/bench_matrix.py

server-stress: server
	python3 scripts/stress_server.py --server ./server

server-flood: server server_flood
	./server_flood --server ./server --clients 16 --duration 60 --message-bytes 8 --batch 64 --target-mps 0.30 --min-delivery-mps 1.3

server-lossless-flood: server_lossless server_flood
	./server_flood --server ./server_lossless --clients 8 --duration 5 --message-bytes 8 --batch 32 --target-mps 0.02 --min-delivery-mps 0.05 --min-delivery-ratio 0.999 --fail-on-forced-stop

server-stress-composite: server server_flood
	python3 scripts/stress_server_composite.py --server ./server --server-flood ./server_flood

server-stress-composite-quick: server server_flood
	python3 scripts/stress_server_composite.py --server ./server --server-flood ./server_flood --quick

server-stress-composite-hour: server server_flood
	python3 scripts/stress_server_composite.py --server ./server --server-flood ./server_flood --soak-hour

verify-darwin: all
	./scripts/verify_darwin.sh

verify-linux: all
	./scripts/verify_linux.sh

verify-windows:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_windows.ps1 -Native

platform-status:
	@echo "host platform: $(HOST_PLATFORM)"

endif
