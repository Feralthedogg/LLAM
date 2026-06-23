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
TESTHOOK_OBJDIR ?= $(OBJDIR)-testhooks
SHARED_CPPFLAGS ?= $(CPPFLAGS) -DLLAM_BUILD_SHARED
PICFLAGS ?= -fPIC -fvisibility=hidden
LLAM_ABI_MAJOR ?= 2
LLAM_VERSION ?= 2.1.0
SANITIZER_TARGETS_ENABLED ?= 0
BUILD_SIGNATURE = $(OBJDIR)/.build-signature
SHARED_BUILD_SIGNATURE = $(SHARED_OBJDIR)/.build-signature
TESTHOOK_BUILD_SIGNATURE = $(TESTHOOK_OBJDIR)/.build-signature
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
	src/internal/runtime_public_active_op.h \
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
	src/core/task/task_group_internal.h \
	src/core/task/task_handle_registry_internal.h \
	src/engine/runtime_watchdog_internal.h \
	src/io/runtime_io_api_internal.h \
	src/io/watch/migration_live_finalize_template.inc \
	src/io/watch/migration_live_forward_template.inc \
	src/io/watch/rehome_accept_template.inc \
	src/io/watch/rehome_recv_template.inc \
	src/io/watch/rehome_template.inc \
	src/io/darwin/runtime_io_watch_darwin_internal.h \
	src/io/linux/runtime_io_watch_linux_internal.h \
	src/io/windows/runtime_io_watch_windows_internal.h

RUNTIME_COMMON_OBJS = \
	$(OBJDIR)/src/core/lifecycle/runtime.o \
	$(OBJDIR)/src/core/base/abi.o \
	$(OBJDIR)/src/core/base/errno.o \
	$(OBJDIR)/src/core/base/util.o \
	$(OBJDIR)/src/core/base/io_udata.o \
	$(OBJDIR)/src/core/registry/capability.o \
	$(OBJDIR)/src/core/broker/broker.o \
	$(OBJDIR)/src/core/broker/broker_lifecycle.o \
	$(OBJDIR)/src/core/broker/broker_ops.o \
	$(OBJDIR)/src/core/broker/broker_validate.o \
	$(OBJDIR)/src/core/broker/broker_buffer.o \
	$(OBJDIR)/src/core/broker/broker_descriptor.o \
	$(OBJDIR)/src/core/broker/broker_channel.o \
	$(OBJDIR)/src/core/broker/broker_revoke.o \
	$(OBJDIR)/src/core/broker/broker_task.o \
	$(OBJDIR)/src/core/broker/broker_windows_security.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_dispatch.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_ops.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_response.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_rollback.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_ring.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_windows.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_windows_fd_stubs.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_windows_pipe.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_windows_session.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_posix.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_posix_message.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_posix_socket.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_unix.o \
	$(OBJDIR)/src/core/broker/transport/broker_transport_selftest.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_buffer_grant.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_doorbell.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_dispatch.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_ops.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_queue.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_stats.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_shm.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_shm_posix.o \
	$(OBJDIR)/src/core/broker/ring/broker_ring_shm_windows.o \
	$(OBJDIR)/src/core/debug/fault.o \
	$(OBJDIR)/src/core/base/names.o \
	$(OBJDIR)/src/core/time/time.o \
	$(OBJDIR)/src/core/context/fp.o \
	$(OBJDIR)/src/core/context/stack_sample.o \
	$(OBJDIR)/src/core/context/context_portable.o \
	$(OBJDIR)/src/core/sched/queue_base.o \
	$(OBJDIR)/src/core/sched/norm_queue_depth.o \
	$(OBJDIR)/src/core/sched/norm_queue.o \
	$(OBJDIR)/src/core/sched/core_queue.o \
	$(OBJDIR)/src/core/memory/alloc.o \
	$(OBJDIR)/src/core/memory/allocator_quiescent.o \
	$(OBJDIR)/src/core/task/task_alloc.o \
	$(OBJDIR)/src/core/task/task_handle_registry.o \
	$(OBJDIR)/src/core/task/task_handle_claim.o \
	$(OBJDIR)/src/core/memory/io_object_alloc.o \
	$(OBJDIR)/src/core/wait/wait_timer_alloc.o \
	$(OBJDIR)/src/core/debug/trace.o \
	$(OBJDIR)/src/core/sched/wake.o \
	$(OBJDIR)/src/core/platform/platform.o \
	$(OBJDIR)/src/core/platform/windows_policy.o \
	$(OBJDIR)/src/core/sched/safepoint.o \
	$(OBJDIR)/src/core/wait/wait.o \
	$(OBJDIR)/src/core/task/task_reclaim.o \
	$(OBJDIR)/src/core/task/task_stack.o \
	$(OBJDIR)/src/core/sched/reinject.o \
	$(OBJDIR)/src/core/wait/wait_accounting.o \
	$(OBJDIR)/src/core/wait/wait_tracking.o \
	$(OBJDIR)/src/core/time/timer_heap.o \
	$(OBJDIR)/src/core/time/timer.o \
	$(OBJDIR)/src/core/api/timer_api.o \
	$(OBJDIR)/src/core/api/signal_api.o \
	$(OBJDIR)/src/engine/scheduler/scheduler_engine.o \
	$(OBJDIR)/src/engine/scheduler/block.o \
	$(OBJDIR)/src/io/engine/io_engine.o \
	$(OBJDIR)/src/engine/watchdog/watchdog.o \
	$(OBJDIR)/src/engine/watchdog/watchdog_probe.o \
	$(OBJDIR)/src/engine/watchdog/watchdog_merge.o \
	$(OBJDIR)/src/engine/watchdog/watchdog_rehome.o \
	$(OBJDIR)/src/engine/watchdog/watchdog_scale.o \
	$(OBJDIR)/src/engine/watchdog/watchdog_worker.o \
	$(OBJDIR)/src/core/api/core_api.o \
	$(OBJDIR)/src/core/task/spawn.o \
	$(OBJDIR)/src/core/task/yield_join_sleep.o \
	$(OBJDIR)/src/core/api/blocking_api.o \
	$(OBJDIR)/src/core/api/cancel_api.o \
	$(OBJDIR)/src/core/lifecycle/lifecycle.o \
	$(OBJDIR)/src/core/sched/scheduler.o \
	$(OBJDIR)/src/core/lifecycle/init.o \
	$(OBJDIR)/src/core/lifecycle/shutdown.o \
	$(OBJDIR)/src/core/lifecycle/run.o \
	$(OBJDIR)/src/core/sync/sync.o \
	$(OBJDIR)/src/core/sync/mutex_lifecycle.o \
	$(OBJDIR)/src/core/sync/mutex.o \
	$(OBJDIR)/src/core/sync/cond_lifecycle.o \
	$(OBJDIR)/src/core/sync/cond.o \
	$(OBJDIR)/src/core/sync/channel_cache.o \
	$(OBJDIR)/src/core/sync/channel_lifecycle.o \
	$(OBJDIR)/src/core/sync/channel_fast.o \
	$(OBJDIR)/src/core/sync/channel.o \
	$(OBJDIR)/src/core/sync/channel_select_fast.o \
	$(OBJDIR)/src/core/sync/channel_select.o \
	$(OBJDIR)/src/core/registry/handle.o \
	$(OBJDIR)/src/core/registry/owner.o \
	$(OBJDIR)/src/core/registry/registry.o \
	$(OBJDIR)/src/core/task/task_group.o \
	$(OBJDIR)/src/core/task/task_group_registry.o \
	$(OBJDIR)/src/core/task/task_local.o \
	$(OBJDIR)/src/io/api/io_api.o \
	$(OBJDIR)/src/io/api/direct.o \
	$(OBJDIR)/src/io/api/direct_tuning.o \
	$(OBJDIR)/src/io/api/issue.o \
	$(OBJDIR)/src/io/api/blocking_ops.o \
	$(OBJDIR)/src/io/api/blocking_file_ops.o \
	$(OBJDIR)/src/io/api/blocking_wrappers.o \
	$(OBJDIR)/src/io/buffer/buffer_registry.o \
	$(OBJDIR)/src/io/api/owned.o \
	$(OBJDIR)/src/io/api/datagram.o \
	$(OBJDIR)/src/io/api/handle_positional.o \
	$(OBJDIR)/src/io/api/positional.o \
	$(OBJDIR)/src/io/api/positional_util.o \
	$(OBJDIR)/src/io/api/public.o \
	$(OBJDIR)/src/io/windows/watch/iocp.o \
	$(OBJDIR)/src/core/debug/debug_dump_helpers.o \
	$(OBJDIR)/src/core/debug/debug_stats_json.o \
	$(OBJDIR)/src/core/debug/debug.o \
	$(OBJDIR)/src/io/watch/watch.o \
	$(OBJDIR)/src/io/watch/close.o \
	$(OBJDIR)/src/io/watch/watch_lookup.o \
	$(OBJDIR)/src/io/watch/migration.o \
	$(OBJDIR)/src/io/watch/watch_queue.o \
	$(OBJDIR)/src/io/watch/waiter.o

ifeq ($(HOST_PLATFORM),linux)
LDLIBS += -lm
LLAM_HAVE_IO_URING_BUF_RING_HELPERS := $(shell printf '%b' '\043include <liburing.h>\012int main\050void\051 \173 void *p = \050void *\051io_uring_setup_buf_ring; return p == 0; \175\012' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c - -luring -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(LLAM_HAVE_IO_URING_BUF_RING_HELPERS),1)
CPPFLAGS += -DLLAM_HAVE_IO_URING_BUF_RING_HELPERS=1
endif
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/linux/watch/prelude.o \
	$(OBJDIR)/src/io/linux/watch/linux_state.o \
	$(OBJDIR)/src/io/linux/watch/linux_lookup.o \
	$(OBJDIR)/src/io/linux/watch/linux_migration_live.o \
	$(OBJDIR)/src/io/linux/watch/linux_migration_rehome.o \
	$(OBJDIR)/src/io/linux/watch/linux_control.o \
	$(OBJDIR)/src/io/linux/watch/linux_submit.o \
	$(OBJDIR)/src/io/linux/watch/cqe.o \
	$(OBJDIR)/src/io/linux/watch/linux_worker.o
ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/asm/linux/x86_64/linux_context_x86_64.o \
	$(OBJDIR)/src/asm/linux/x86_64/wake_syscalls_x86_64.o
else ifeq ($(UNAME_M),aarch64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/core/context/context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/linux_context_arm64.o
endif
else ifeq ($(HOST_PLATFORM),darwin)
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
LDLIBS := $(filter-out -luring,$(LDLIBS))
CPPFLAGS += -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/darwin/watch/darwin_state.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_migration_live.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_migration_rehome.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_control.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_completion.o \
	$(OBJDIR)/src/io/darwin/watch/events.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_worker.o
ifeq ($(UNAME_M),arm64)
RUNTIME_OBJS += $(OBJDIR)/src/core/context/context_arm64.o
RUNTIME_OBJS += $(OBJDIR)/src/asm/darwin/arm64/darwin_context_arm64.o
else ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/darwin/x86_64/darwin_context_x86_64.o
endif
else ifeq ($(HOST_PLATFORM),windows)
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
LDLIBS := $(filter-out -pthread -luring,$(LDLIBS)) -lws2_32 -lmswsock -ladvapi32
CPPFLAGS += -D_WIN32_WINNT=0x0A00 -DLLAM_ENABLE_WINDOWS_BACKEND
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/windows/watch/windows_state.o \
	$(OBJDIR)/src/io/windows/watch/socket.o \
	$(OBJDIR)/src/io/windows/watch/pool.o \
	$(OBJDIR)/src/io/windows/watch/windows_control.o \
	$(OBJDIR)/src/io/windows/watch/windows_submit.o \
	$(OBJDIR)/src/io/windows/watch/windows_completion.o \
	$(OBJDIR)/src/io/windows/watch/fallback.o \
	$(OBJDIR)/src/io/windows/watch/windows_watch.o
ifeq ($(UNAME_M),AMD64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/windows/x86_64/windows_context_x86_64.o
else ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/windows/x86_64/windows_context_x86_64.o
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
	$(OBJDIR)/src/io/darwin/watch/darwin_state.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_migration_live.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_migration_rehome.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_control.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_completion.o \
	$(OBJDIR)/src/io/darwin/watch/events.o \
	$(OBJDIR)/src/io/darwin/watch/darwin_worker.o
ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/linux/x86_64/linux_context_x86_64.o
else ifeq ($(UNAME_M),amd64)
RUNTIME_OBJS += $(OBJDIR)/src/asm/linux/x86_64/linux_context_x86_64.o
else ifeq ($(UNAME_M),aarch64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/core/context/context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/linux_context_arm64.o
else ifeq ($(UNAME_M),arm64)
RUNTIME_OBJS += \
	$(OBJDIR)/src/core/context/context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/linux_context_arm64.o
endif
endif
endif
SHARED_RUNTIME_OBJS = $(patsubst $(OBJDIR)/%,$(SHARED_OBJDIR)/%,$(RUNTIME_OBJS))
TESTHOOK_RUNTIME_OVERRIDE_OBJS = \
	$(TESTHOOK_OBJDIR)/src/core/registry/capability.o \
	$(TESTHOOK_OBJDIR)/src/core/broker/broker_buffer.o \
	$(TESTHOOK_OBJDIR)/src/core/broker/transport/broker_transport.o \
	$(TESTHOOK_OBJDIR)/src/core/registry/registry.o
RUNTIME_TESTHOOK_OBJS = \
	$(filter-out \
		$(OBJDIR)/src/core/registry/capability.o \
		$(OBJDIR)/src/core/broker/broker_buffer.o \
		$(OBJDIR)/src/core/broker/transport/broker_transport.o \
		$(OBJDIR)/src/core/registry/registry.o, \
		$(RUNTIME_OBJS)) \
	$(TESTHOOK_RUNTIME_OVERRIDE_OBJS)
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
	$(OBJDIR)/examples/server_flood_stats.o \
	$(OBJDIR)/examples/server_flood_stats_open.o
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

.PHONY: all clean static shared audit-shared-exports audit-production-test-hooks test test-asan test-no-owner test-tsan test-fuzz-heavy test-process-utils test-runtime-soak test-hardening require-sanitizer-target analyze-cppcheck audit-deps test-quick test-full test-soak check package bench-matrix server-stress server-flood server-lossless-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux verify-windows platform-status windows-unsupported FORCE
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
WINDOWS_CTEST_ARGS ?= --timeout 180
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

audit-shared-exports audit-production-test-hooks test-asan test-no-owner test-tsan test-fuzz-heavy test-process-utils test-runtime-soak test-hardening analyze-cppcheck audit-deps server-stress server-flood server-lossless-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux: windows-unsupported

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
		printf 'SHARED_CPPFLAGS=%s\n' '$(SHARED_CPPFLAGS)'; \
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
		printf 'SHARED_CPPFLAGS=%s\n' '$(SHARED_CPPFLAGS)'; \
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

$(TESTHOOK_BUILD_SIGNATURE): FORCE
	@mkdir -p $(dir $@)
	@tmp="$@.$$$$.tmp"; \
	{ \
		printf 'CC=%s\n' '$(CC)'; \
		printf 'CPPFLAGS=%s\n' '$(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1'; \
		printf 'CFLAGS=%s\n' '$(CFLAGS)'; \
		printf 'LDLIBS=%s\n' '$(LDLIBS)'; \
		printf 'TESTHOOK_OBJDIR=%s\n' '$(TESTHOOK_OBJDIR)'; \
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

$(TESTHOOK_RUNTIME_OVERRIDE_OBJS): $(TESTHOOK_BUILD_SIGNATURE)

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
	rm -f $@
	$(AR) rcs $@ $(RUNTIME_OBJS)

shared: $(SHLIB_LINK)

audit-shared-exports: shared
	python3 scripts/audit_shared_exports.py ./$(SHLIB_REAL)

audit-production-test-hooks: static
	@if command -v nm >/dev/null 2>&1; then \
		if nm -g libllam_runtime.a 2>/dev/null | grep -E 'llam_(capability|broker|runtime)_test_(force_.*(entropy|alloc)_failure|force_subject_value|buffer_free_count)' >/dev/null; then \
			echo "production static runtime exports test fault-injection hooks" >&2; \
			nm -g libllam_runtime.a 2>/dev/null | grep -E 'llam_(capability|broker|runtime)_test_(force_.*(entropy|alloc)_failure|force_subject_value|buffer_free_count)' >&2; \
			exit 1; \
		fi; \
	fi

test: test_abi_contract test_abi_compat test_connect_io test_runtime_core test_multi_runtime_core test_runtime_api_edges test_runtime_select_edges test_runtime_io_dump test_runtime_group_local_edges test_runtime_unmanaged_join test_runtime_stress test_runtime_fuzz test_runtime_invariants test_runtime_shutdown_internal test_sync_primitives test_io_buffers test_windows_policy test_windows_runtime_smoke test_windows_iocp_io test_windows_iocp_dump test_windows_handle_io test_security_capability test_shared_load llam_broker server stress server_flood shared audit-shared-exports audit-production-test-hooks
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
	python3 scripts/test_broker_cli_parsing.py
	python3 scripts/test_c_env_helpers.py
	@broker_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-broker-test.$$$$.XXXXXX")"; \
	chmod 700 "$$broker_dir"; \
	broker_sock="$$broker_dir/broker.sock"; \
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
		rmdir "$$broker_dir" >/dev/null 2>&1 || true; \
		exit "$$client_rc"; \
	fi; \
	if ! wait "$$server_pid"; then \
		cat "$$server_out" >&2; \
		rm -f "$$broker_sock" "$$server_out"; \
		rmdir "$$broker_dir" >/dev/null 2>&1 || true; \
		exit 1; \
	fi; \
	rm -f "$$broker_sock" "$$server_out"; \
	rmdir "$$broker_dir" >/dev/null 2>&1 || true
	./test_shared_load ./$(SHLIB_REAL)
	python3 scripts/test_server_flood_cli.py ./server_flood
	python3 scripts/test_server_flood_stats_security.py ./server_flood
	python3 scripts/test_example_diagnostic_security.py ./server ./stress
	python3 scripts/test_safe_output_security.py
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
	printf '2.1.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.1.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) ln -s libllam_runtime.2.dylib "$$tmp_dir/pkg/lib/libllam_runtime.dylib" ;; \
		Linux) ln -s libllam_runtime.so.2 "$$tmp_dir/pkg/lib/libllam_runtime.so" ;; \
		FreeBSD|OpenBSD|NetBSD|DragonFly) ln -s libllam_runtime.so.2 "$$tmp_dir/pkg/lib/libllam_runtime.so" ;; \
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
	printf '2.1.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.1.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.999.dylib"; \
			ln -s libllam_runtime.999.dylib "$$tmp_dir/pkg/lib/libllam_runtime.dylib"; \
			;; \
		Linux) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.so.999"; \
			ln -s libllam_runtime.so.999 "$$tmp_dir/pkg/lib/libllam_runtime.so"; \
			;; \
		FreeBSD|OpenBSD|NetBSD|DragonFly) \
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
	printf '2.1.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.1.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.2.dylib"; \
			printf 'not a symlink\n' > "$$tmp_dir/pkg/lib/libllam_runtime.dylib"; \
			;; \
		Linux) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.so.2.1.0"; \
			printf 'not a symlink\n' > "$$tmp_dir/pkg/lib/libllam_runtime.so.2"; \
			;; \
		FreeBSD|OpenBSD|NetBSD|DragonFly) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.so.2.1.0"; \
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
	printf '2.1.0-rc.1\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '2.1.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	: > "$$tmp_dir/pkg/lib/libllam_runtime.so.2.1.0"; \
	ln -s libllam_runtime.so.2.1.0 "$$tmp_dir/pkg/lib/libllam_runtime.so.2"; \
	ln -s libllam_runtime.so.2 "$$tmp_dir/pkg/lib/libllam_runtime.so"; \
	sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; \
	test -L "$$tmp_dir/prefix/lib/libllam_runtime.so"; \
	grep '^2.1.0-rc.1$$' "$$tmp_dir/prefix/share/llam/VERSION" >/dev/null; \
	grep '^2.1.0$$' "$$tmp_dir/prefix/share/llam/LIBRARY_VERSION" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-package-output-symlink.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
		linux-*|freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.1.0"; ln -s libllam_runtime.so.2.1.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/repo/target"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported package output file-component smoke host" >&2; exit 1 ;; \
	esac; \
	mkdir -p "$$tmp_dir/repo/scripts"; \
	cp scripts/package_release.sh "$$tmp_dir/repo/scripts/package_release.sh"; \
	: > "$$tmp_dir/repo/target"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
		linux-*|freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.1.0"; ln -s libllam_runtime.so.2.1.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	chmod 666 "$$tmp_dir/repo/README.md"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
		linux-*|freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.1.0"; ln -s libllam_runtime.so.2.1.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
		linux-*|freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.1.0"; ln -s libllam_runtime.so.2.1.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
		linux-*|freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.1.0"; ln -s libllam_runtime.so.2.1.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s README.md "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
		linux-*|freebsd-*|openbsd-*|netbsd-*|dragonflybsd-*) : > "$$tmp_dir/repo/libllam_runtime.so.2.1.0"; : > "$$tmp_dir/repo/libllam_runtime.so.2"; : > "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=2.1.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer checksum smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/lib"; \
	cp scripts/install.sh "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	sh scripts/test_release_fixture.sh archive-xz "$$tmp_dir/archive" "$$package" "$$tmp_dir/$$package.tar.xz"; \
	digest="$$(sh scripts/test_release_fixture.sh sha256 "$$tmp_dir/$$package.tar.xz")"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer checksum-glob smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/lib"; \
	cp scripts/install.sh "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	sh scripts/test_release_fixture.sh archive-xz "$$tmp_dir/archive" "$$package" "$$tmp_dir/$$package.tar.xz"; \
	digest="$$(sh scripts/test_release_fixture.sh sha256 "$$tmp_dir/$$package.tar.xz")"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer archive-script smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/lib"; \
	printf '#!/bin/sh\nprintf exploited > %s/marker\nexit 0\n' "$$tmp_dir" > "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	sh scripts/test_release_fixture.sh archive-xz "$$tmp_dir/archive" "$$package" "$$tmp_dir/$$package.tar.xz"; \
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer unsafe-mode smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package/include/llam" "$$tmp_dir/archive/$$package/bin" "$$tmp_dir/archive/$$package/lib"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	: > "$$tmp_dir/archive/$$package/include/llam/runtime.h"; \
	printf 'payload\n' > "$$tmp_dir/archive/$$package/bin/llam-danger"; \
	chmod 4777 "$$tmp_dir/archive/$$package/bin/llam-danger"; \
	sh scripts/test_release_fixture.sh archive-xz "$$tmp_dir/archive" "$$package" "$$tmp_dir/$$package.tar.xz"; \
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer hardlink smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	printf payload > "$$tmp_dir/archive/$$package/payload"; \
	ln "$$tmp_dir/archive/$$package/payload" "$$tmp_dir/archive/$$package/payload-hardlink"; \
	sh scripts/test_release_fixture.sh archive-xz "$$tmp_dir/archive" "$$package" "$$tmp_dir/$$package.tar.xz"; \
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a duplicate archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing duplicate archive member' "$$tmp_dir/install.out" >/dev/null
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-absolute-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer absolute-member smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	rm -f "/tmp/$$package-escape"; \
	{ \
		printf '%s\n' 'import io'; \
		printf '%s\n' 'import sys'; \
		printf '%s\n' 'import tarfile'; \
		printf '%s\n' 'from pathlib import Path'; \
		printf '%s\n' 'tmp = Path(sys.argv[1])'; \
		printf '%s\n' 'package = sys.argv[2]'; \
		printf '%s\n' 'entries = ('; \
		printf '%s\n' '    (f"{package}/install.sh", b"#!/bin/sh\nexit 0\n", 0o755),'; \
		printf '%s\n' '    (f"/tmp/{package}-escape", b"escape", 0o644),'; \
		printf '%s\n' ')'; \
		printf '%s\n' 'with tarfile.open(tmp / f"{package}.tar", "w") as archive:'; \
		printf '%s\n' '    for name, data, mode in entries:'; \
		printf '%s\n' '        info = tarfile.TarInfo(name)'; \
		printf '%s\n' '        info.size = len(data)'; \
		printf '%s\n' '        info.mode = mode'; \
		printf '%s\n' '        archive.addfile(info, io.BytesIO(data))'; \
	} > "$$tmp_dir/absolute_archive.py"; \
	python3 "$$tmp_dir/absolute_archive.py" "$$tmp_dir" "$$package"; \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted an absolute archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unsafe archive member' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "/tmp/$$package-escape"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-parent-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer parent-member smoke host" >&2; exit 1 ;; \
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
		printf '%s\n' '    (f"{package}/../escape", b"escape", 0o644),'; \
		printf '%s\n' ')'; \
		printf '%s\n' 'with tarfile.open(tmp / f"{package}.tar", "w") as archive:'; \
		printf '%s\n' '    for name, data, mode in entries:'; \
		printf '%s\n' '        info = tarfile.TarInfo(name)'; \
		printf '%s\n' '        info.size = len(data)'; \
		printf '%s\n' '        info.mode = mode'; \
		printf '%s\n' '        archive.addfile(info, io.BytesIO(data))'; \
	} > "$$tmp_dir/parent_archive.py"; \
	python3 "$$tmp_dir/parent_archive.py" "$$tmp_dir" "$$package"; \
	xz -zc "$$tmp_dir/$$package.tar" > "$$tmp_dir/$$package.tar.xz"; \
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
	if sh scripts/install.sh --version ci --target "$$package_target" --base-url "file://$$tmp_dir" --prefix "$$tmp_dir/prefix" >"$$tmp_dir/install.out" 2>&1; then \
		echo "install.sh accepted a parent-traversal archive member" >&2; \
		cat "$$tmp_dir/install.out" >&2; \
		exit 1; \
	fi; \
	grep 'refusing unsafe archive member' "$$tmp_dir/install.out" >/dev/null; \
	test ! -e "$$tmp_dir/escape"
	@tmp_dir="$$(mktemp -d "$${TMPDIR:-/tmp}/llam-install-dot-component-archive.XXXXXX")"; \
	trap 'rm -rf "$$tmp_dir"' 0 1 2 3 15; \
	case "$$(uname -s)-$$(uname -m)" in \
		Darwin-arm64) package_target=macos-aarch64 ;; \
		Darwin-x86_64) package_target=macos-x86_64 ;; \
		Linux-x86_64) package_target=linux-x86_64 ;; \
		Linux-aarch64) package_target=linux-aarch64 ;; \
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
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
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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
		FreeBSD-x86_64|FreeBSD-amd64) package_target=freebsd-x86_64 ;; \
		FreeBSD-aarch64|FreeBSD-arm64) package_target=freebsd-aarch64 ;; \
		OpenBSD-x86_64|OpenBSD-amd64) package_target=openbsd-x86_64 ;; \
		OpenBSD-aarch64|OpenBSD-arm64) package_target=openbsd-aarch64 ;; \
		NetBSD-x86_64|NetBSD-amd64) package_target=netbsd-x86_64 ;; \
		NetBSD-aarch64|NetBSD-arm64) package_target=netbsd-aarch64 ;; \
		DragonFly-x86_64|DragonFly-amd64) package_target=dragonflybsd-x86_64 ;; \
		DragonFly-aarch64|DragonFly-arm64) package_target=dragonflybsd-aarch64 ;; \
		*) echo "unsupported installer symlink-target smoke host" >&2; exit 1 ;; \
	esac; \
	package="llam-ci-$$package_target"; \
	mkdir -p "$$tmp_dir/archive/$$package"; \
	printf '%s\n' '#!/bin/sh' 'exit 0' > "$$tmp_dir/archive/$$package/install.sh"; \
	chmod +x "$$tmp_dir/archive/$$package/install.sh"; \
	ln -s 'payload target' "$$tmp_dir/archive/$$package/payload-link"; \
	sh scripts/test_release_fixture.sh archive-xz "$$tmp_dir/archive" "$$package" "$$tmp_dir/$$package.tar.xz"; \
	sh scripts/test_release_fixture.sh sha256-sidecar "$$tmp_dir/$$package.tar.xz" "$$tmp_dir/$$package.tar.xz.sha256"; \
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

FUZZ_HEAVY_RUNTIME_SCENARIOS ?= 2048
FUZZ_HEAVY_MULTI_RUNTIME_SCENARIOS ?= 512
RUNTIME_SOAK_SECONDS ?= 300
RUNTIME_SOAK_TIMEOUT ?= 180
RUNTIME_SOAK_SEED ?= 0x4c4c414d534f414b
RUNTIME_SOAK_FUZZ_SCENARIOS ?= 128
RUNTIME_SOAK_MULTI_FUZZ_SCENARIOS ?= 64

test-asan:
	@set -e; \
	if [ -n "$(filter -n n --just-print --dry-run --recon,$(MAKEFLAGS))" ]; then \
		$(MAKE) $(ASAN_TEST_TARGETS) \
			OBJDIR=object-asan \
			SANITIZER_TARGETS_ENABLED=1 \
			CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
			LDLIBS="$(LDLIBS) -fsanitize=address,undefined"; \
		exit 0; \
	fi; \
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
	if [ -n "$(filter -n n --just-print --dry-run --recon,$(MAKEFLAGS))" ]; then \
		$(MAKE) $(NOOWNER_TEST_TARGETS) \
			OBJDIR=object-noowner \
			CPPFLAGS="$(CPPFLAGS) -DLLAM_RUNTIME_DISABLE_OWNER_CHECKS=1"; \
		exit 0; \
	fi; \
	cleanup() { rm -f $(NOOWNER_TEST_TARGETS); }; \
	trap cleanup EXIT; \
	cleanup; \
	$(MAKE) $(NOOWNER_TEST_TARGETS) \
		OBJDIR=object-noowner \
		CPPFLAGS="$(CPPFLAGS) -DLLAM_RUNTIME_DISABLE_OWNER_CHECKS=1"; \
	./noowner-test_runtime_select_edges

test-tsan:
	@set -e; \
	if [ -n "$(filter -n n --just-print --dry-run --recon,$(MAKEFLAGS))" ]; then \
		$(MAKE) $(TSAN_TEST_TARGETS) \
			OBJDIR=object-tsan \
			SANITIZER_TARGETS_ENABLED=1 \
			CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=thread" \
			LDLIBS="$(LDLIBS) -fsanitize=thread"; \
		exit 0; \
	fi; \
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

test-process-utils:
	python3 scripts/test_process_utils.py
	python3 scripts/test_bench_parsers.py
	python3 scripts/test_bench_runtime_compare.py
	python3 scripts/test_stress_server_logic.py
	python3 scripts/test_c_env_helpers.py

test-runtime-soak: test_runtime_fuzz test_multi_runtime_core test_runtime_stress test_runtime_shutdown_internal test_io_buffers
	python3 scripts/runtime_soak.py \
		--duration $(RUNTIME_SOAK_SECONDS) \
		--timeout $(RUNTIME_SOAK_TIMEOUT) \
		--seed $(RUNTIME_SOAK_SEED) \
		--fuzz-scenarios $(RUNTIME_SOAK_FUZZ_SCENARIOS) \
		--multi-fuzz-scenarios $(RUNTIME_SOAK_MULTI_FUZZ_SCENARIOS)

test-hardening: analyze-cppcheck audit-deps test-process-utils test-asan test-tsan test-fuzz-heavy

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

asan-test_security_capability tsan-test_security_capability: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS) $(LDLIBS)

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

test_security_capability: $(RUNTIME_TESTHOOK_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_SECURITY_CAPABILITY_OBJS) $(LDLIBS)

test_shared_load: $(TEST_SHARED_LOAD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SHARED_LOAD_OBJS) $(DL_LIBS)

$(OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TESTHOOK_OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/engine/%.o: src/engine/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/engine/%.o: src/engine/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/io/%.o: src/io/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/io/%.o: src/io/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/io/windows/%.o: src/io/windows/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/io/windows/%.o: src/io/windows/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/engine/watchdog/watchdog.o: $(RUNTIME_ENGINE_FRAGMENTS)

$(SHARED_OBJDIR)/src/engine/watchdog/watchdog.o: $(RUNTIME_ENGINE_FRAGMENTS)

$(OBJDIR)/src/asm/linux/x86_64/%.o: src/asm/linux/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/linux/x86_64/%.o: src/asm/linux/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/linux/arm64/%.o: src/asm/linux/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/linux/arm64/%.o: src/asm/linux/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/darwin/arm64/%.o: src/asm/darwin/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/darwin/arm64/%.o: src/asm/darwin/arm64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/darwin/x86_64/%.o: src/asm/darwin/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/darwin/x86_64/%.o: src/asm/darwin/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/windows/x86_64/%.o: src/asm/windows/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(SHARED_OBJDIR)/src/asm/windows/x86_64/%.o: src/asm/windows/x86_64/%.S src/internal/llam_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -c -o $@ $<

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

$(OBJDIR)/examples/server_flood_stats.o: examples/server_flood_stats.c examples/server_flood_stats.h examples/server_flood_stats_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server_flood_stats_open.o: examples/server_flood_stats_open.c examples/server_flood_stats_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Some tests intentionally include private runtime headers to validate teardown,
# ownership, and backend invariants. Rebuild all test objects on private layout
# changes so internal tests cannot link against a stale object view of structs.
$(OBJDIR)/tests/%.o: tests/%.c $(RUNTIME_PRIV_HDRS) tests/test_env.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_security_capability.o: tests/test_security_capability.c $(RUNTIME_PRIV_HDRS) tests/test_env.h $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

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
