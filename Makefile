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

CC ?= cc
AR ?= ar
PYTHON ?= python3
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g -fno-omit-frame-pointer
CPPFLAGS ?= -Iinclude -Isrc/internal -Isrc -D_GNU_SOURCE
CPPFLAGS += -Itools/preempt
LDLIBS ?= -pthread -luring
SERVER_FLOOD_LDLIBS ?= -pthread
OBJDIR ?= object
SHARED_OBJDIR ?= $(OBJDIR)-pic
TESTHOOK_OBJDIR ?= $(OBJDIR)-testhooks
SHARED_CPPFLAGS ?= $(CPPFLAGS) -DLLAM_BUILD_SHARED
PICFLAGS ?= -fPIC -fvisibility=hidden
LLAM_ABI_MAJOR ?= 2
LLAM_VERSION ?= 3.0.0
SANITIZER_TARGETS_ENABLED ?= 0
LLAM_EXPERIMENTAL_ASYNC_PREEMPT ?= auto
LLAM_PREEMPT_TOOLING ?= off
PREEMPT_TOOLING_BUILD_DIR ?= build-preempt-tools
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
	scripts/__pycache__ \
	scripts/bench_tokio_compare/target \
	build-* \
	target
CLEAN_FILES = \
	example_explicit_runtime \
	example_explicit_runtime.exe \
	test_runtime_regressions \
	test_runtime_regressions.exe \
	asan-test_runtime_regressions \
	demo \
	stress \
	bench \
	cpu_spike \
	bench_magic2_cpu \
	bench_magic2_cpu.exe \
	bench_magic2_kernels \
	bench_magic2_kernels.exe \
	bench_handoff \
	test_handoff_protocol \
	bench_watch_index \
	test_io_watch_index \
	asan-test_io_watch_index \
	bench_select_prepare \
	bench_async_compare \
	test_large_select \
	asan-test_large_select \
	llam_broker \
	server \
	server_lossless \
	server_flood \
	test_lcwe_model \
	bench_lcwe_model \
	test_abi_contract \
	test_abi_compat \
	test_connect_io \
	test_runtime_core \
	test_multi_runtime_core \
	test_api_usability \
	test_scope_values \
	test_scope \
	test_cavo_lifetime \
	test_cavo_execution \
	test_scope_cpp \
	example_scoped_results \
	test_runtime_api_edges \
	test_runtime_select_edges \
	test_runtime_io_dump \
	test_runtime_group_local_edges \
	test_runtime_unmanaged_join \
	test_runtime_stress \
	test_runtime_fuzz \
	test_runtime_invariants \
	test_runtime_shutdown_internal \
	test_watchdog_liveness_internal \
	test_worker_identity_internal \
	test_legacy_lease_internal \
	test_execution_lease_internal \
	test_native_domain_internal \
	test_managed_catalog_internal \
	test_managed_foreign_internal \
	test_capsule_pool_internal \
	test_managed_scheduler_internal \
	test_managed_async_registered \
	test_managed_async_linux_x86_64 \
	test_managed_image_internal \
	test_managed_async_compiler \
	test_processor_resource_internal \
	test_processor_queue_timer_internal \
	test_processor_allocator_io_internal \
	test_foreign_detach_internal \
	test_relief_worker_internal \
	test_preempt_capabilities \
	test_default_policy_internal \
	test_magic2_internal \
	test_async_preempt_linux_x86_64 \
	test_async_preempt_multi_linux_x86_64 \
	test_async_context_linux_arm64 \
	test_linux_arm64_async_scheduler_real \
	test_async_context_darwin \
	test_darwin_arm64_async_scheduler_real \
	llam_preempt_metadata_inspect \
	test_preempt_metadata \
	test_preempt_metadata_v2 \
	test_preempt_metadata_registration \
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
	asan-test_watchdog_liveness_internal \
	asan-test_worker_identity_internal \
	asan-test_legacy_lease_internal \
	asan-test_execution_lease_internal \
	asan-test_native_domain_internal \
	asan-test_managed_catalog_internal \
	asan-test_managed_foreign_internal \
	asan-test_capsule_pool_internal \
	asan-test_managed_scheduler_internal \
	asan-test_processor_resource_internal \
	asan-test_processor_queue_timer_internal \
	asan-test_processor_allocator_io_internal \
	asan-test_foreign_detach_internal \
	asan-test_relief_worker_internal \
	asan-test_async_preempt_linux_x86_64 \
	asan-test_async_preempt_multi_linux_x86_64 \
	asan-test_preempt_metadata \
	asan-test_preempt_metadata_v2 \
	asan-test_preempt_metadata_registration \
	asan-test_preempt_capabilities \
	asan-test_default_policy_internal \
	asan-test_magic2_internal \
	asan-test_multi_runtime_core \
	asan-test_api_usability \
	asan-test_scope \
	asan-test_scope_values \
	asan-test_cavo_lifetime \
	asan-test_cavo_execution \
	asan-test_runtime_fuzz \
	asan-test_security_capability \
	noowner-test_runtime_select_edges \
	tsan-test_runtime_core \
	tsan-test_runtime_shutdown_internal \
	tsan-test_watchdog_liveness_internal \
	tsan-test_worker_identity_internal \
	tsan-test_legacy_lease_internal \
	tsan-test_execution_lease_internal \
	tsan-test_native_domain_internal \
	tsan-test_managed_catalog_internal \
	tsan-test_managed_foreign_internal \
	tsan-test_capsule_pool_internal \
	tsan-test_managed_scheduler_internal \
	tsan-test_processor_resource_internal \
	tsan-test_processor_queue_timer_internal \
	tsan-test_processor_allocator_io_internal \
	tsan-test_foreign_detach_internal \
	tsan-test_relief_worker_internal \
	tsan-test_async_preempt_linux_x86_64 \
	tsan-test_async_preempt_multi_linux_x86_64 \
	tsan-test_preempt_metadata \
	tsan-test_preempt_metadata_v2 \
	tsan-test_preempt_metadata_registration \
	tsan-test_preempt_capabilities \
	tsan-test_default_policy_internal \
	tsan-test_magic2_internal \
	tsan-test_multi_runtime_core \
	tsan-test_api_usability \
	tsan-test_runtime_fuzz \
	tsan-test_security_capability \
	libllam_runtime.a \
	demo.exe \
	stress.exe \
	bench.exe \
	cpu_spike.exe \
	llam_broker.exe \
	server.exe \
	server_lossless.exe \
	server_flood.exe \
	test_lcwe_model.exe \
	bench_lcwe_model.exe \
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
	test_watchdog_liveness_internal.exe \
	test_worker_identity_internal.exe \
	test_legacy_lease_internal.exe \
	test_processor_resource_internal.exe \
	test_processor_queue_timer_internal.exe \
	test_processor_allocator_io_internal.exe \
	test_foreign_detach_internal.exe \
	test_relief_worker_internal.exe \
	test_preempt_capabilities.exe \
	test_default_policy_internal.exe \
	test_magic2_internal.exe \
	test_async_preempt_linux_x86_64.exe \
	test_async_preempt_multi_linux_x86_64.exe \
	test_async_preempt_multi_linux_x86_64.exe \
	llam_preempt_metadata_inspect.exe \
	test_preempt_metadata.exe \
	test_preempt_metadata_v2.exe \
	test_preempt_metadata_registration.exe \
	test_sync_primitives.exe \
	test_io_buffers.exe \
	test_windows_policy.exe \
	test_windows_runtime_smoke.exe \
	test_windows_iocp_io.exe \
	test_windows_iocp_dump.exe \
	test_windows_handle_io.exe \
	test_security_capability.exe \
	test_shared_load.exe \
	asan-test_worker_identity_internal.exe \
	asan-test_legacy_lease_internal.exe \
	asan-test_async_preempt_linux_x86_64.exe \
	asan-test_async_preempt_multi_linux_x86_64.exe \
	asan-test_async_preempt_multi_linux_x86_64.exe \
	asan-test_preempt_metadata.exe \
	asan-test_preempt_metadata_v2.exe \
	asan-test_preempt_metadata_registration.exe \
	asan-test_preempt_capabilities.exe \
	asan-test_default_policy_internal.exe \
	asan-test_magic2_internal.exe \
	tsan-test_worker_identity_internal.exe \
	tsan-test_legacy_lease_internal.exe \
	tsan-test_async_preempt_linux_x86_64.exe \
	tsan-test_async_preempt_multi_linux_x86_64.exe \
	tsan-test_async_preempt_multi_linux_x86_64.exe \
	tsan-test_preempt_metadata.exe \
	tsan-test_preempt_metadata_v2.exe \
	tsan-test_preempt_metadata_registration.exe \
	tsan-test_preempt_capabilities.exe \
	tsan-test_default_policy_internal.exe \
	tsan-test_magic2_internal.exe \
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
$(error BSD support ended in LLAM 3.0.0; use versioned 2.x sources/tooling)
else
HOST_PLATFORM := posix
endif
endif
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),auto)
LLAM_EXPERIMENTAL_ASYNC_PREEMPT := 0
ifeq ($(HOST_PLATFORM),linux)
ifneq ($(filter x86_64 aarch64 arm64,$(UNAME_M)),)
LLAM_EXPERIMENTAL_ASYNC_PREEMPT := 1
endif
else ifeq ($(HOST_PLATFORM),darwin)
ifneq ($(filter arm64 aarch64,$(UNAME_M)),)
LLAM_EXPERIMENTAL_ASYNC_PREEMPT := 1
endif
endif
endif

ifeq ($(HOST_PLATFORM),darwin)
SHLIB_LINK = libllam_runtime.dylib
SHLIB_REAL = libllam_runtime.$(LLAM_ABI_MAJOR).dylib
SHLIB_LDFLAGS = -dynamiclib -Wl,-install_name,@rpath/$(SHLIB_REAL)
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
	include/llam/runtime.h \
	include/llam/scope.h \
	include/llam/executor.h

RUNTIME_PRIV_HDRS = \
	$(LLAM_PUBLIC_HDRS) \
	src/internal/runtime_platform.h \
	src/internal/runtime_windows_compat.h \
	src/internal/runtime_windows.h \
	src/internal/runtime_windows_iocp.h \
	src/internal/runtime_debug_dump_helpers.h \
	src/internal/llam_internal.h \
	src/internal/runtime_internal.h \
	src/internal/runtime_mix.h \
	src/internal/runtime_types.h \
	src/internal/runtime_watch_index.h \
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
	src/internal/async_context.h \
	src/internal/capsule_pool.h \
	src/internal/managed_catalog.h \
	src/internal/managed_scheduler.h \
	src/internal/managed_async_adapter.h \
	src/internal/managed_domain.h \
	src/internal/managed_image.h \
	src/internal/runtime_test_managed_foreign.h \
	src/internal/async_context_phase.h \
	src/internal/async_context_darwin.h \
	src/internal/async_context_shared.h \
	src/internal/async_context_linux_arm64.h \
	src/internal/async_context_signal.h \
	src/internal/async_context_signal_native.h \
	src/internal/async_context_darwin_x86_64.h \
	src/internal/async_signal_registry.h \
	src/internal/runtime_test_watchdog.h \
	src/internal/runtime_proto_sched.h \
	src/internal/runtime_proto_io.h \
	src/internal/runtime_proto_sync.h \
	src/core/task/task_group_internal.h \
	src/core/task/structured_internal.h \
	src/core/task/local_executor_internal.h \
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
	$(OBJDIR)/src/engine/watchdog/watchdog_deadlock.o \
	$(OBJDIR)/src/core/sched/worker_registry.o \
	$(OBJDIR)/src/core/sched/execution_lease.o \
	$(OBJDIR)/src/core/lifecycle/runtime.o \
	$(OBJDIR)/src/core/base/abi.o \
	$(OBJDIR)/src/core/base/errno.o \
	$(OBJDIR)/src/core/base/native_errno.o \
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
	$(OBJDIR)/src/core/context/async_context.o \
	$(OBJDIR)/src/core/context/async_context_shared.o \
	$(OBJDIR)/src/core/context/context_portable.o \
	$(OBJDIR)/src/core/sched/queue_base.o \
	$(OBJDIR)/src/core/sched/norm_queue_depth.o \
	$(OBJDIR)/src/core/sched/norm_queue.o \
	$(OBJDIR)/src/core/sched/core_queue.o \
	$(OBJDIR)/src/core/memory/alloc.o \
	$(OBJDIR)/src/core/memory/task_span.o \
	$(OBJDIR)/src/core/memory/allocator_quiescent.o \
	$(OBJDIR)/src/core/task/task_alloc.o \
	$(OBJDIR)/src/core/task/task_handle_registry.o \
	$(OBJDIR)/src/core/task/task_handle_claim.o \
	$(OBJDIR)/src/core/memory/io_object_alloc.o \
	$(OBJDIR)/src/core/wait/wait_timer_alloc.o \
	$(OBJDIR)/src/core/debug/trace.o \
	$(OBJDIR)/src/core/sched/wake.o \
	$(OBJDIR)/src/core/platform/platform.o \
	$(OBJDIR)/src/core/preempt/capabilities.o \
	$(OBJDIR)/src/core/preempt/native_domain.o \
	$(OBJDIR)/src/core/preempt/managed_catalog.o \
	$(OBJDIR)/src/core/api/managed_foreign.o \
	$(OBJDIR)/src/core/context/capsule_pool.o \
	$(OBJDIR)/src/core/preempt/managed_image.o \
	$(OBJDIR)/src/core/preempt/managed_image_darwin.o \
	$(OBJDIR)/src/core/preempt/managed_image_elf.o \
	$(OBJDIR)/src/core/preempt/managed_image_windows.o \
	$(OBJDIR)/src/core/sched/managed_scheduler.o \
	$(OBJDIR)/src/core/lifecycle/managed_run.o \
	$(OBJDIR)/src/core/context/managed_async_adapter.o \
	$(OBJDIR)/tools/preempt/metadata.o \
	$(OBJDIR)/tools/preempt/metadata_v2.o \
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
	$(OBJDIR)/src/engine/watchdog/watchdog_autotune.o \
	$(OBJDIR)/src/core/api/core_api.o \
	$(OBJDIR)/src/core/task/spawn.o \
	$(OBJDIR)/src/core/task/yield_join_sleep.o \
	$(OBJDIR)/src/core/api/blocking_api.o \
	$(OBJDIR)/src/core/api/magic2_api.o \
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
	$(OBJDIR)/src/core/task/task_group_drain.o \
	$(OBJDIR)/src/core/task/local_executor.o \
	$(OBJDIR)/src/core/task/local_executor_binding.o \
	$(OBJDIR)/src/core/wait/cleanup_wait.o \
	$(OBJDIR)/src/core/task/task_group_registry.o \
	$(OBJDIR)/src/core/task/task_local.o \
	$(OBJDIR)/src/core/task/structured_registry.o \
	$(OBJDIR)/src/core/task/scope_options.o \
	$(OBJDIR)/src/core/task/owned_value.o \
	$(OBJDIR)/src/core/task/scope.o \
	$(OBJDIR)/src/core/task/scope_spawn.o \
	$(OBJDIR)/src/core/task/scope_wait.o \
	$(OBJDIR)/src/core/task/future.o \
	$(OBJDIR)/src/core/task/scoped_completion.o \
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
	$(OBJDIR)/src/io/api/wait_options.o \
	$(OBJDIR)/src/io/windows/watch/iocp.o \
	$(OBJDIR)/src/core/debug/debug_dump_helpers.o \
	$(OBJDIR)/src/core/debug/debug_stats_json.o \
	$(OBJDIR)/src/core/debug/debug.o \
	$(OBJDIR)/src/io/watch/watch.o \
	$(OBJDIR)/src/io/watch/watch_index.o \
	$(OBJDIR)/src/io/watch/close.o \
	$(OBJDIR)/src/io/watch/watch_lookup.o \
	$(OBJDIR)/src/io/watch/migration.o \
	$(OBJDIR)/src/io/watch/watch_queue.o \
	$(OBJDIR)/src/io/watch/waiter.o

ifeq ($(HOST_PLATFORM),linux)
LDLIBS += -lm -ldl
LLAM_HAVE_IO_URING_BUF_RING_HELPERS := $(shell printf '%b' '\043include <liburing.h>\012int main\050void\051 \173 struct io_uring_buf_ring *\050*p\051\050struct io_uring *, unsigned int, int, unsigned int, int *\051 = io_uring_setup_buf_ring; return p == 0; \175\012' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c - -luring -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
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
endif
ifeq ($(HOST_PLATFORM),linux)
ifeq ($(UNAME_M),x86_64)
RUNTIME_OBJS += $(OBJDIR)/src/core/context/async_context_linux_x86_64.o
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
CPPFLAGS += -DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
RUNTIME_OBJS += $(OBJDIR)/src/asm/linux/x86_64/async_preempt_trampoline.o
endif
else ifeq ($(UNAME_M),aarch64)
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
CPPFLAGS += -DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
RUNTIME_OBJS += $(OBJDIR)/src/core/context/async_context_linux_arm64.o \
	$(OBJDIR)/src/core/context/async_context_signal.o \
	$(OBJDIR)/src/asm/linux/arm64/async_context_arm64.o
endif
endif
endif
ifeq ($(HOST_PLATFORM),darwin)
ifeq ($(UNAME_M),arm64)
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
CPPFLAGS += -DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
RUNTIME_OBJS += $(OBJDIR)/src/core/context/async_context_darwin.o \
	$(OBJDIR)/src/asm/darwin/arm64/async_context_arm64.o
endif
else ifeq ($(UNAME_M),x86_64)
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
CPPFLAGS += -DLLAM_EXPERIMENTAL_ASYNC_PREEMPT=1
RUNTIME_OBJS += $(OBJDIR)/src/core/context/async_context_darwin_x86_64.o \
	$(OBJDIR)/src/asm/darwin/x86_64/async_context_x86_64.o
endif
endif
endif
SHARED_RUNTIME_OBJS = $(patsubst $(OBJDIR)/%,$(SHARED_OBJDIR)/%,$(RUNTIME_OBJS))
TESTHOOK_RUNTIME_OVERRIDE_OBJS = \
	$(TESTHOOK_OBJDIR)/src/core/memory/task_span.o \
	$(TESTHOOK_OBJDIR)/src/core/context/managed_async_adapter.o \
	$(TESTHOOK_OBJDIR)/src/core/api/managed_foreign.o \
	$(TESTHOOK_OBJDIR)/src/core/preempt/managed_catalog.o \
	$(TESTHOOK_OBJDIR)/src/core/sched/managed_scheduler.o \
	$(TESTHOOK_OBJDIR)/src/core/lifecycle/init.o \
	$(TESTHOOK_OBJDIR)/src/core/registry/capability.o \
	$(TESTHOOK_OBJDIR)/src/core/sched/norm_queue.o \
	$(TESTHOOK_OBJDIR)/src/core/broker/broker_buffer.o \
	$(TESTHOOK_OBJDIR)/src/core/broker/transport/broker_transport.o \
	$(TESTHOOK_OBJDIR)/src/core/broker/transport/broker_transport_posix_message.o \
	$(TESTHOOK_OBJDIR)/src/core/registry/registry.o \
	$(TESTHOOK_OBJDIR)/src/engine/watchdog/watchdog_rehome.o \
	$(TESTHOOK_OBJDIR)/src/engine/watchdog/watchdog_probe.o \
	$(TESTHOOK_OBJDIR)/src/engine/watchdog/watchdog_worker.o \
	$(TESTHOOK_OBJDIR)/src/io/api/issue.o \
	$(TESTHOOK_OBJDIR)/src/io/watch/watch_queue.o \
	$(TESTHOOK_OBJDIR)/src/io/watch/watch_index.o \
	$(TESTHOOK_OBJDIR)/src/core/platform/platform.o \
	$(TESTHOOK_OBJDIR)/src/core/time/timer_heap.o \
	$(TESTHOOK_OBJDIR)/src/core/time/timer.o \
	$(TESTHOOK_OBJDIR)/src/core/api/blocking_api.o \
	$(TESTHOOK_OBJDIR)/src/core/sched/scheduler.o \
	$(TESTHOOK_OBJDIR)/src/core/sched/reinject.o \
	$(TESTHOOK_OBJDIR)/src/core/sched/core_queue.o \
	$(TESTHOOK_OBJDIR)/src/core/task/yield_join_sleep.o \
	$(TESTHOOK_OBJDIR)/src/core/task/scope_spawn.o \
	$(TESTHOOK_OBJDIR)/src/core/task/task_group.o
RUNTIME_TESTHOOK_OBJS = \
	$(filter-out \
		$(OBJDIR)/src/core/memory/task_span.o \
		$(OBJDIR)/src/core/context/managed_async_adapter.o \
		$(OBJDIR)/src/core/api/managed_foreign.o \
		$(OBJDIR)/src/core/preempt/managed_catalog.o \
		$(OBJDIR)/src/core/sched/managed_scheduler.o \
		$(OBJDIR)/src/core/lifecycle/init.o \
		$(OBJDIR)/src/core/registry/capability.o \
		$(OBJDIR)/src/core/sched/norm_queue.o \
		$(OBJDIR)/src/core/broker/broker_buffer.o \
		$(OBJDIR)/src/core/broker/transport/broker_transport.o \
		$(OBJDIR)/src/core/broker/transport/broker_transport_posix_message.o \
		$(OBJDIR)/src/core/registry/registry.o \
		$(OBJDIR)/src/engine/watchdog/watchdog_rehome.o \
		$(OBJDIR)/src/engine/watchdog/watchdog_probe.o \
		$(OBJDIR)/src/engine/watchdog/watchdog_worker.o \
		$(OBJDIR)/src/io/api/issue.o \
		$(OBJDIR)/src/io/watch/watch_queue.o \
		$(OBJDIR)/src/io/watch/watch_index.o \
		$(OBJDIR)/src/core/platform/platform.o \
		$(OBJDIR)/src/core/time/timer_heap.o \
		$(OBJDIR)/src/core/time/timer.o \
		$(OBJDIR)/src/core/api/blocking_api.o \
		$(OBJDIR)/src/core/sched/scheduler.o \
		$(OBJDIR)/src/core/sched/reinject.o \
		$(OBJDIR)/src/core/sched/core_queue.o \
		$(OBJDIR)/src/core/task/yield_join_sleep.o \
		$(OBJDIR)/src/core/task/scope_spawn.o \
		$(OBJDIR)/src/core/task/task_group.o, \
		$(RUNTIME_OBJS)) \
	$(TESTHOOK_RUNTIME_OVERRIDE_OBJS)
ifneq ($(filter $(OBJDIR)/src/core/context/async_context_linux_x86_64.o,$(RUNTIME_OBJS)),)
TESTHOOK_RUNTIME_OVERRIDE_OBJS += $(TESTHOOK_OBJDIR)/src/core/context/async_context_linux_x86_64.o
TESTHOOK_RUNTIME_OVERRIDE_OBJS += $(TESTHOOK_OBJDIR)/src/core/lifecycle/run.o
TESTHOOK_RUNTIME_OVERRIDE_OBJS += $(TESTHOOK_OBJDIR)/src/core/memory/alloc.o
RUNTIME_TESTHOOK_OBJS := $(filter-out $(OBJDIR)/src/core/context/async_context_linux_x86_64.o $(OBJDIR)/src/core/lifecycle/run.o $(OBJDIR)/src/core/memory/alloc.o,$(RUNTIME_TESTHOOK_OBJS))
endif
ifneq ($(filter $(OBJDIR)/src/core/context/async_context_darwin.o $(OBJDIR)/src/core/context/async_context_darwin_x86_64.o,$(RUNTIME_OBJS)),)
TESTHOOK_RUNTIME_OVERRIDE_OBJS += $(patsubst $(OBJDIR)/%,$(TESTHOOK_OBJDIR)/%,$(filter \
	$(OBJDIR)/src/core/context/async_context_darwin.o $(OBJDIR)/src/core/context/async_context_darwin_x86_64.o,$(RUNTIME_OBJS)))
TESTHOOK_RUNTIME_OVERRIDE_OBJS += $(TESTHOOK_OBJDIR)/src/core/context/async_context_shared.o \
	$(TESTHOOK_OBJDIR)/src/core/base/errno.o
RUNTIME_TESTHOOK_OBJS := $(filter-out $(OBJDIR)/src/core/context/async_context_darwin.o \
	$(OBJDIR)/src/core/context/async_context_darwin_x86_64.o \
	$(OBJDIR)/src/core/context/async_context_shared.o $(OBJDIR)/src/core/base/errno.o,$(RUNTIME_TESTHOOK_OBJS))
endif
ifneq ($(filter $(OBJDIR)/src/core/context/async_context_signal.o,$(RUNTIME_OBJS)),)
TESTHOOK_RUNTIME_OVERRIDE_OBJS += $(TESTHOOK_OBJDIR)/src/core/context/async_context_signal.o \
	$(TESTHOOK_OBJDIR)/src/core/context/async_context_shared.o $(TESTHOOK_OBJDIR)/src/core/base/errno.o
RUNTIME_TESTHOOK_OBJS := $(filter-out $(OBJDIR)/src/core/context/async_context_signal.o \
	$(OBJDIR)/src/core/context/async_context_shared.o $(OBJDIR)/src/core/base/errno.o,$(RUNTIME_TESTHOOK_OBJS))
endif
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
$(filter-out $(OBJDIR)/examples/diagnostic_output.o,$(STRESS_OBJS)) $(OBJDIR)/examples/broker.o: $(RUNTIME_PRIV_HDRS)
BENCH_OBJS = \
	$(OBJDIR)/examples/bench.o \
	$(OBJDIR)/examples/bench_support.o \
	$(OBJDIR)/examples/bench_entry.o
CPU_SPIKE_OBJS = \
	$(OBJDIR)/examples/cpu_spike.o
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
TEST_API_USABILITY_OBJS = $(OBJDIR)/tests/test_api_usability.o
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
TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS = $(OBJDIR)/tests/test_watchdog_liveness_internal.o
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
ifeq ($(HOST_PLATFORM)-$(UNAME_M),darwin-arm64)
TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS += $(OBJDIR)/tests/fixtures/async_darwin_leaf.o
endif
ifeq ($(HOST_PLATFORM)-$(UNAME_M),linux-aarch64)
TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS += $(OBJDIR)/tests/fixtures/async_darwin_leaf.o
endif
endif
TEST_RUNTIME_REGRESSIONS_OBJS = $(OBJDIR)/tests/test_runtime_regressions.o
TEST_WORKER_IDENTITY_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_worker_identity_internal.o
TEST_LEGACY_LEASE_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_legacy_lease_internal.o
TEST_EXECUTION_LEASE_INTERNAL_OBJS = $(OBJDIR)/tests/test_execution_lease_internal.o
TEST_NATIVE_DOMAIN_INTERNAL_OBJS = $(OBJDIR)/tests/test_native_domain_internal.o
TEST_MANAGED_CATALOG_INTERNAL_OBJS = $(OBJDIR)/tests/test_managed_catalog_internal.o
TEST_MANAGED_FOREIGN_INTERNAL_OBJS = $(OBJDIR)/tests/test_managed_foreign_internal.o
TEST_CAPSULE_POOL_INTERNAL_OBJS = $(OBJDIR)/tests/test_capsule_pool_internal.o
$(TEST_MANAGED_FOREIGN_INTERNAL_OBJS): CPPFLAGS += -DLLAM_ENABLE_TEST_HOOKS=1
TEST_MANAGED_SCHEDULER_INTERNAL_OBJS = $(OBJDIR)/tests/test_managed_scheduler_internal.o
TEST_MANAGED_ASYNC_REGISTERED_OBJS = $(OBJDIR)/tests/test_managed_async_registered.o
TEST_MANAGED_ASYNC_LINUX_X86_64_OBJS = $(OBJDIR)/tests/test_managed_async_linux_x86_64.o
TEST_MANAGED_IMAGE_INTERNAL_OBJS = $(OBJDIR)/tests/test_managed_image_internal.o
TEST_MANAGED_ASYNC_COMPILER_OBJS = $(OBJDIR)/tests/test_managed_async_compiler.o
$(TEST_MANAGED_CATALOG_INTERNAL_OBJS) $(TEST_MANAGED_SCHEDULER_INTERNAL_OBJS) $(TEST_MANAGED_ASYNC_REGISTERED_OBJS) $(TEST_MANAGED_ASYNC_LINUX_X86_64_OBJS) $(TEST_MANAGED_ASYNC_COMPILER_OBJS): CPPFLAGS += -DLLAM_ENABLE_TEST_HOOKS=1
ifeq ($(HOST_PLATFORM)-$(UNAME_M),linux-x86_64)
TEST_MANAGED_ASYNC_LINUX_X86_64_OBJS += $(OBJDIR)/tests/fixtures/async_safe_leaf.o
endif
ifeq ($(HOST_PLATFORM),darwin)
MANAGED_CONSUMER_EXPORT_FLAGS = -Wl,-export_dynamic
else ifneq ($(HOST_PLATFORM),windows)
MANAGED_CONSUMER_EXPORT_FLAGS = -Wl,--export-dynamic
endif
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
ifneq ($(filter darwin-arm64 linux-aarch64,$(HOST_PLATFORM)-$(UNAME_M)),)
TEST_MANAGED_ASYNC_REGISTERED_OBJS += $(OBJDIR)/tests/fixtures/async_darwin_leaf.o
endif
endif
TEST_PROCESSOR_RESOURCE_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_processor_resource_internal.o
TEST_PROCESSOR_QUEUE_TIMER_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_processor_queue_timer_internal.o
TEST_PROCESSOR_ALLOCATOR_IO_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_processor_allocator_io_internal.o
TEST_FOREIGN_DETACH_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_foreign_detach_internal.o
TEST_RELIEF_WORKER_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_relief_worker_internal.o
TEST_PREEMPT_CAPABILITIES_OBJS = \
	$(OBJDIR)/tests/test_preempt_capabilities.o
TEST_DEFAULT_POLICY_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_default_policy_internal.o
TEST_MAGIC2_INTERNAL_OBJS = \
	$(OBJDIR)/tests/test_magic2_internal.o \
	$(OBJDIR)/tests/test_magic2_cpu_internal.o \
	$(OBJDIR)/tests/test_magic2_auto_internal.o
$(OBJDIR)/src/core/api/magic2_api.o $(SHARED_OBJDIR)/src/core/api/magic2_api.o $(TEST_MAGIC2_INTERNAL_OBJS): src/internal/runtime_magic2.h src/internal/magic2.h
TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS = \
	$(OBJDIR)/tests/test_async_preempt_linux_x86_64.o
TEST_ASYNC_PREEMPT_MULTI_LINUX_X86_64_OBJS = \
	$(OBJDIR)/tests/test_async_preempt_multi_linux_x86_64.o
TEST_ASYNC_CONTEXT_DARWIN_OBJS = $(OBJDIR)/tests/test_async_context_darwin.o
TEST_ASYNC_CONTEXT_LINUX_ARM64_OBJS = $(OBJDIR)/tests/test_async_context_linux_arm64.o
TEST_LINUX_ARM64_ASYNC_SCHEDULER_REAL_OBJS = $(OBJDIR)/tests/test_linux_arm64_async_scheduler_real.o
TEST_DARWIN_ARM64_ASYNC_SCHEDULER_REAL_OBJS = $(OBJDIR)/tests/test_darwin_arm64_async_scheduler_real.o
ifeq ($(HOST_PLATFORM),darwin)
ifeq ($(UNAME_M),arm64)
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
TEST_ASYNC_CONTEXT_DARWIN_OBJS += $(OBJDIR)/tests/fixtures/async_darwin_leaf.o \
	$(OBJDIR)/tests/fixtures/async_darwin_entry.o
TEST_DARWIN_ARM64_ASYNC_SCHEDULER_REAL_OBJS += $(OBJDIR)/tests/fixtures/async_darwin_leaf.o
endif
endif
endif
ifeq ($(HOST_PLATFORM),linux)
ifeq ($(UNAME_M),aarch64)
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
TEST_ASYNC_CONTEXT_LINUX_ARM64_OBJS += $(OBJDIR)/src/core/base/native_errno.o \
	$(OBJDIR)/src/core/context/async_context_linux_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/async_context_arm64.o \
	$(OBJDIR)/src/asm/linux/arm64/linux_context_arm64.o \
	$(OBJDIR)/tests/fixtures/async_darwin_leaf.o \
	$(OBJDIR)/tests/fixtures/async_linux_arm64_entry.o
TEST_LINUX_ARM64_ASYNC_SCHEDULER_REAL_OBJS += $(OBJDIR)/tests/fixtures/async_darwin_leaf.o
LINUX_ARM64_ASYNC_CASES = disabled repeat capture-late restore-early-exit ready-drain offload \
	startup-abort configuration isolation task-reader witness-late pending-signal observed-cooperative \
	capture-handler-tail restore-handler-tail witness-armed-return capture-armed-cancel ready-destroy \
	signal-namespace signal-blocked-start signal-owner-isolation
endif
endif
ifeq ($(UNAME_M),x86_64)
TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS += \
	$(OBJDIR)/tests/fixtures/async_safe_leaf.o
ifeq ($(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),1)
TEST_ASYNC_SIGNAL_WRAP_CPPFLAGS = -DLLAM_TEST_WRAP_SIGACTION=1
TEST_ASYNC_SIGNAL_WRAP_LDFLAGS = -Wl,--wrap=sigaction
endif
TEST_ASYNC_PREEMPT_MULTI_LINUX_X86_64_OBJS += \
	$(OBJDIR)/tests/fixtures/async_safe_leaf.o
endif
endif
PREEMPT_METADATA_OBJS = \
	$(OBJDIR)/tools/preempt/metadata.o \
	$(OBJDIR)/tools/preempt/metadata_v2.o
PREEMPT_METADATA_INSPECT_OBJS = \
	$(OBJDIR)/tools/preempt/metadata_inspect.o
TEST_PREEMPT_METADATA_OBJS = \
	$(OBJDIR)/tests/test_preempt_metadata.o
TEST_PREEMPT_METADATA_V2_OBJS = $(OBJDIR)/tests/test_preempt_metadata_v2.o
TEST_PREEMPT_METADATA_REGISTRATION_OBJS = \
	$(OBJDIR)/tests/test_preempt_metadata_registration.o
$(PREEMPT_METADATA_OBJS) $(PREEMPT_METADATA_INSPECT_OBJS) $(TEST_PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_V2_OBJS): CPPFLAGS += -Itools/preempt
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
LCWE_MODEL_CORE_OBJS = \
	$(OBJDIR)/experiments/lcwe/lcwe_model.o \
	$(OBJDIR)/experiments/lcwe/lcwe_workloads.o
LCWE_MODEL_TEST_OBJS = \
	$(OBJDIR)/experiments/lcwe/test_lcwe_model.o
LCWE_MODEL_BENCH_OBJS = \
	$(OBJDIR)/experiments/lcwe/bench_lcwe_model.o
RUNTIME_ENGINE_FRAGMENTS = $(wildcard src/engine/detail/*.inc)
EXAMPLE_SHARED_HDRS = examples/env_compat.h
BUILD_OBJS = \
	$(OBJDIR)/examples/explicit_runtime.o \
	$(TEST_RUNTIME_REGRESSIONS_OBJS) \
	$(RUNTIME_OBJS) \
	$(DEMO_OBJS) \
	$(STRESS_OBJS) \
	$(BENCH_OBJS) \
	$(CPU_SPIKE_OBJS) \
	$(BROKER_OBJS) \
	$(SERVER_OBJS) \
	$(SERVER_LOSSLESS_OBJS) \
	$(SERVER_FLOOD_OBJS) \
	$(TEST_ABI_OBJS) \
	$(TEST_ABI_COMPAT_OBJS) \
	$(TEST_CONNECT_OBJS) \
	$(TEST_RUNTIME_CORE_OBJS) \
	$(TEST_MULTI_RUNTIME_CORE_OBJS) \
	$(TEST_API_USABILITY_OBJS) \
	$(TEST_RUNTIME_API_EDGES_OBJS) \
	$(TEST_RUNTIME_SELECT_EDGES_OBJS) \
	$(TEST_RUNTIME_IO_DUMP_OBJS) \
	$(TEST_RUNTIME_GROUP_LOCAL_EDGES_OBJS) \
	$(TEST_RUNTIME_UNMANAGED_JOIN_OBJS) \
	$(TEST_RUNTIME_STRESS_OBJS) \
	$(TEST_RUNTIME_FUZZ_OBJS) \
	$(TEST_RUNTIME_INVARIANTS_OBJS) \
	$(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS) \
	$(TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS) \
	$(TEST_WORKER_IDENTITY_INTERNAL_OBJS) \
	$(TEST_LEGACY_LEASE_INTERNAL_OBJS) \
	$(TEST_EXECUTION_LEASE_INTERNAL_OBJS) \
	$(TEST_NATIVE_DOMAIN_INTERNAL_OBJS) \
	$(TEST_MANAGED_CATALOG_INTERNAL_OBJS) \
	$(TEST_MANAGED_FOREIGN_INTERNAL_OBJS) \
	$(TEST_CAPSULE_POOL_INTERNAL_OBJS) \
	$(TEST_MANAGED_SCHEDULER_INTERNAL_OBJS) \
	$(TEST_MANAGED_ASYNC_REGISTERED_OBJS) \
	$(TEST_MANAGED_ASYNC_LINUX_X86_64_OBJS) \
	$(TEST_MANAGED_IMAGE_INTERNAL_OBJS) \
	$(TEST_MANAGED_ASYNC_COMPILER_OBJS) \
	$(TEST_PROCESSOR_RESOURCE_INTERNAL_OBJS) \
	$(TEST_PROCESSOR_QUEUE_TIMER_INTERNAL_OBJS) \
	$(TEST_PROCESSOR_ALLOCATOR_IO_INTERNAL_OBJS) \
	$(TEST_FOREIGN_DETACH_INTERNAL_OBJS) \
	$(TEST_RELIEF_WORKER_INTERNAL_OBJS) \
	$(TEST_PREEMPT_CAPABILITIES_OBJS) \
	$(TEST_DEFAULT_POLICY_INTERNAL_OBJS) \
	$(TEST_MAGIC2_INTERNAL_OBJS) \
	$(TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS) \
	$(PREEMPT_METADATA_OBJS) \
	$(PREEMPT_METADATA_INSPECT_OBJS) \
	$(TEST_PREEMPT_METADATA_OBJS) \
	$(TEST_PREEMPT_METADATA_REGISTRATION_OBJS) \
	$(TEST_SYNC_OBJS) \
	$(TEST_IO_BUFFERS_OBJS) \
	$(TEST_WINDOWS_POLICY_OBJS) \
	$(TEST_WINDOWS_RUNTIME_SMOKE_OBJS) \
	$(TEST_WINDOWS_IOCP_DUMP_OBJS) \
	$(TEST_SECURITY_CAPABILITY_OBJS) \
	$(TEST_SHARED_LOAD_OBJS) \
	$(LCWE_MODEL_CORE_OBJS) \
	$(LCWE_MODEL_TEST_OBJS) \
	$(LCWE_MODEL_BENCH_OBJS)
LINK_TARGETS = \
	test_linux_arm64_async_scheduler_real \
	test_async_context_linux_arm64 \
	test_runtime_regressions \
	demo \
	stress \
	bench \
	llam_broker \
	server \
	server_lossless \
	server_flood \
	test_lcwe_model \
	bench_lcwe_model \
	test_abi_contract \
	test_abi_compat \
	test_connect_io \
	test_runtime_core \
	test_multi_runtime_core \
	test_api_usability \
	test_scope_values \
	test_scope \
	test_cavo_lifetime \
	test_cavo_execution \
	test_scope_cpp \
	example_scoped_results \
	test_runtime_api_edges \
	test_runtime_select_edges \
	test_runtime_io_dump \
	test_runtime_group_local_edges \
	test_runtime_unmanaged_join \
	test_runtime_stress \
	test_runtime_fuzz \
	test_runtime_invariants \
	test_runtime_shutdown_internal \
	test_watchdog_liveness_internal \
	test_worker_identity_internal \
	test_legacy_lease_internal \
	test_processor_resource_internal \
	test_processor_queue_timer_internal \
	test_processor_allocator_io_internal \
	test_foreign_detach_internal \
	test_relief_worker_internal \
	test_preempt_capabilities \
	test_default_policy_internal \
	test_magic2_internal \
	test_async_preempt_linux_x86_64 \
	test_async_preempt_multi_linux_x86_64 \
	llam_preempt_metadata_inspect \
	test_preempt_metadata \
	test_preempt_metadata_v2 \
	test_preempt_metadata_registration \
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

.PHONY: cpu-spike-compare
.PHONY: all clean static shared audit-shared-exports audit-production-test-hooks test test-lcwe-model lcwe-model-report test-asan test-no-owner test-tsan test-fuzz-heavy test-process-utils test-runtime-soak test-hardening require-sanitizer-target analyze-cppcheck audit-deps test-quick test-full test-soak test-license-policy check-license-policy test_legacy_lease_boundary test-processor-resource test-processor-queue-timer test-processor-allocator-io test-foreign-detach test-relief-worker test-preempt-metadata test-preempt-sdk-metadata test-preempt-package preempt-tools check_legacy_lease_boundary check package bench-matrix bench_magic2_cpu server-stress server-flood server-lossless-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux verify-windows platform-status windows-unsupported FORCE
.DEFAULT_GOAL := all

test-license-policy:
	$(PYTHON) scripts/test_license_policy.py
	$(PYTHON) scripts/test_package_license_layout.py

check-license-policy: test-license-policy
	$(PYTHON) scripts/check_license_policy.py --include-untracked

test_legacy_lease_boundary:
	$(PYTHON) scripts/test_legacy_lease_boundary.py

check_legacy_lease_boundary:
	$(PYTHON) scripts/check_legacy_lease_boundary.py --root .

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
WINDOWS_CTEST_REGEX ?= test_abi_contract|test_abi_compat|test_runtime_core|test_multi_runtime_core|test_runtime_api_edges|test_runtime_select_edges|test_runtime_group_local_edges|test_runtime_unmanaged_join|test_runtime_stress|test_runtime_fuzz|test_runtime_invariants|test_runtime_shutdown_internal|test_watchdog_liveness|test_worker_identity_internal|test_legacy_lease_internal|test_execution_lease_internal|test_processor_resource_internal|test_processor_queue_timer_internal|test_processor_allocator_io_internal|test_foreign_detach_internal|test_relief_worker_internal|test_preempt_capabilities|test_default_policy_internal|test_magic2_internal|test_preempt_metadata|test_preempt_metadata_registration|test_preempt_sdk_metadata|test_preempt_cmake_install|test_worker_identity_boundary|test_legacy_lease_boundary|check_legacy_lease_boundary|test_sync_primitives|test_windows_policy|test_windows_runtime_smoke|test_windows_iocp_io|test_windows_iocp_dump|test_windows_handle_io|test_security_capability|test_lcwe_model|test_bench_lcwe_model|llam_broker_self_test
WINDOWS_CTEST_REGEX := $(WINDOWS_CTEST_REGEX)|test_cavo_lifetime|test_cavo_execution|test_cavo_region_unbalanced|test_scope_values|test_scope|test_scope_native_io|test_scope_cpp|example_scoped_results|test_api_usability|example_explicit_runtime|test_shared_load|test_runtime_regressions|test_sdk_install
WINDOWS_CMAKE_TARGETS = \
	example_explicit_runtime \
	test_windows_async_multim_oracle \
	test_runtime_regressions \
	demo \
	stress \
	bench \
	llam_broker \
	server \
	server_lossless \
	server_flood \
	test_lcwe_model \
	bench_lcwe_model \
	test_abi_contract \
	test_abi_compat \
	test_shared_load \
	test_connect_io \
	test_runtime_core \
	test_multi_runtime_core \
	test_api_usability \
	test_scope_values \
	test_scope \
	test_cavo_lifetime \
	test_cavo_execution \
	test_scope_cpp \
	example_scoped_results \
	test_runtime_api_edges \
	test_runtime_select_edges \
	test_runtime_io_dump \
	test_runtime_group_local_edges \
	test_runtime_unmanaged_join \
	test_runtime_stress \
	test_runtime_fuzz \
	test_runtime_invariants \
	test_runtime_shutdown_internal \
	test_watchdog_liveness_internal \
	test_worker_identity_internal \
	test_legacy_lease_internal \
	test_processor_resource_internal \
	test_processor_queue_timer_internal \
	test_processor_allocator_io_internal \
	test_foreign_detach_internal \
	test_relief_worker_internal \
	test_preempt_capabilities \
	test_default_policy_internal \
	test_magic2_internal \
	test_preempt_metadata \
	test_preempt_metadata_v2 \
	test_preempt_cmake_install \
	test_preempt_sdk_metadata \
	test_preempt_metadata_registration \
	test_async_preempt_linux_x86_64 \
	test_async_preempt_multi_linux_x86_64 \
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

test: windows-cmake-test

check: windows-cmake-test check-license-policy

test-lcwe-model: windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)" --target test_lcwe_model bench_lcwe_model
	ctest --test-dir "$(WINDOWS_CMAKE_BUILD_DIR)" --output-on-failure -C "$(WINDOWS_CMAKE_CONFIG)" -R "test_lcwe_model|test_bench_lcwe_model" $(WINDOWS_CTEST_ARGS)

$(WINDOWS_CMAKE_TARGETS): windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)" --target $@

package: windows-cmake-configure
	cmake --build "$(WINDOWS_CMAKE_BUILD_DIR)" --config "$(WINDOWS_CMAKE_CONFIG)" --target llam_runtime llam_runtime_shared bench llam_preempt_metadata_inspect
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release_windows.ps1 -BuildDir "$(WINDOWS_CMAKE_BUILD_DIR)" -Configuration "$(WINDOWS_CMAKE_CONFIG)"

bench-matrix: bench
	python scripts/bench_matrix.py

audit-shared-exports audit-production-test-hooks lcwe-model-report test-asan test-no-owner test-tsan test-fuzz-heavy test-process-utils test-runtime-soak test-hardening analyze-cppcheck audit-deps cpu-spike-compare server-stress server-flood server-lossless-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux: windows-unsupported

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

all: demo stress bench llam_broker server server_lossless server_flood llam_preempt_metadata_inspect static shared

static: libllam_runtime.a

libllam_runtime.a: $(RUNTIME_OBJS)
	rm -f $@
	$(AR) rcs $@ $(RUNTIME_OBJS)

shared: $(SHLIB_LINK)

audit-shared-exports: shared
	$(PYTHON) scripts/audit_shared_exports.py ./$(SHLIB_REAL)

audit-production-test-hooks: static shared
	$(PYTHON) scripts/audit_production_test_hooks.py ./libllam_runtime.a ./$(SHLIB_REAL) || { status=$$?; test "$$status" -eq 77; }

test: test_cavo_lifetime test_cavo_execution test_scope_values test_scope test_scope_cpp example_scoped_results test_api_usability test_capsule_pool_internal test_managed_catalog_internal test_managed_foreign_internal test_managed_scheduler_internal test_native_domain_internal test_execution_lease_internal test_watchdog_liveness_internal test_handoff_protocol bench_handoff test_io_watch_index test_large_select
test: test-managed-bookkeeping
.PHONY: test-managed-bookkeeping
test-managed-bookkeeping: test_managed_scheduler_internal
	@./test_managed_scheduler_internal --async-bookkeeping; llam_test_status=$$?; \
	if [ "$$llam_test_status" -eq 77 ]; then exit 0; else exit "$$llam_test_status"; fi
test: test_runtime_regressions test_lcwe_model bench_lcwe_model test_abi_contract test_abi_compat test_connect_io test_runtime_core test_multi_runtime_core test_runtime_api_edges test_runtime_select_edges test_runtime_io_dump test_runtime_group_local_edges test_runtime_unmanaged_join test_runtime_stress test_runtime_fuzz test_runtime_invariants test_runtime_shutdown_internal test_worker_identity_internal test_legacy_lease_internal test_processor_resource_internal test_processor_queue_timer_internal test_processor_allocator_io_internal test_foreign_detach_internal test_relief_worker_internal test_preempt_capabilities test_default_policy_internal test_magic2_internal test_async_preempt_linux_x86_64 test_preempt_metadata test_preempt_metadata_v2 test_preempt_metadata_registration test_sync_primitives test_io_buffers test_windows_policy test_windows_runtime_smoke test_windows_iocp_io test_windows_iocp_dump test_windows_handle_io test_security_capability test_shared_load llam_broker server stress server_flood shared audit-shared-exports audit-production-test-hooks
	./test_execution_lease_internal
	./test_native_domain_internal
	./test_managed_catalog_internal
	./test_managed_foreign_internal
	./test_capsule_pool_internal
	./test_managed_scheduler_internal
	$(PYTHON) -B scripts/test_native_domain_boundary.py --binary ./test_native_domain_internal
	./test_handoff_protocol
	./test_io_watch_index || test $$? -eq 77
	./test_large_select || test $$? -eq 77
	HANDOFF_BENCH_BINARY=./bench_handoff $(PYTHON) -B scripts/test_handoff_model.py
	./test_lcwe_model
	LCWE_MODEL_TEST_BINARY=./bench_lcwe_model $(PYTHON) scripts/test_bench_lcwe_model.py
	$(PYTHON) scripts/test_worker_identity_boundary.py
	$(PYTHON) scripts/test_legacy_lease_boundary.py
	@if command -v bash >/dev/null 2>&1; then \
		$(PYTHON) scripts/test_llam_clang.py; \
	else \
		echo "llam-clang wrapper test skipped: bash unavailable"; \
	fi
	$(PYTHON) scripts/test_preempt_sdk_metadata.py
	$(PYTHON) scripts/test_preempt_package.py
	$(PYTHON) scripts/test_package_license_layout.py
	$(PYTHON) scripts/check_legacy_lease_boundary.py --root .
	./test_abi_contract
	./test_abi_compat
	./test_connect_io
	./test_runtime_core
	./test_runtime_regressions
	./test_multi_runtime_core
	./test_api_usability
	./test_cavo_lifetime
	./test_cavo_lifetime --io-helper
	./test_cavo_execution
	$(PYTHON) -B -c 'import subprocess; p=subprocess.run(["./test_cavo_execution", "--unbalanced"]); assert p.returncode == 125, p.returncode'
	./test_scope_values
	./test_scope
	LLAM_DIRECT_BLOCKING_IO=0 LLAM_DIRECT_BLOCKING_POLL=0 LLAM_WINDOWS_IOCP_TCP_POLLIN=1 ./test_scope --io-native
	./test_scope_cpp
	./example_scoped_results
	./example_explicit_runtime
	./example_explicit_runtime --cancel
	./test_runtime_api_edges
	./test_runtime_select_edges
	./test_runtime_io_dump
	./test_runtime_group_local_edges
	./test_runtime_unmanaged_join
	./test_runtime_stress
	./test_runtime_fuzz
	./test_runtime_invariants
	./test_runtime_shutdown_internal
	./test_watchdog_liveness_internal
	./test_watchdog_liveness_internal --lazy
	./test_watchdog_liveness_internal --wait-reference
	./test_watchdog_liveness_internal --async-dequeue || test $$? -eq 77
	./test_watchdog_liveness_internal --async-return || test $$? -eq 77
	./test_watchdog_liveness_internal --parked
	./test_watchdog_liveness_internal --async-parked || test $$? -eq 77
	./test_worker_identity_internal
	./test_legacy_lease_internal || test $$? -eq 77
	./test_processor_resource_internal
	./test_processor_queue_timer_internal || test $$? -eq 77
	./test_processor_allocator_io_internal || test $$? -eq 77
	./test_foreign_detach_internal
	./test_relief_worker_internal
	./test_preempt_capabilities
	./test_default_policy_internal
	./test_magic2_internal
	./test_async_preempt_linux_x86_64 || test $$? -eq 77
	./test_async_preempt_linux_x86_64 --signal-namespace || test $$? -eq 77
	./test_async_preempt_linux_x86_64 --signal-namespace-faults || test $$? -eq 77
	./test_async_preempt_multi_linux_x86_64 || test $$? -eq 77
	./test_async_context_linux_arm64 || test $$? -eq 77
	./test_linux_arm64_async_scheduler_real || test $$? -eq 77
	@for llam_case in $(LINUX_ARM64_ASYNC_CASES); do \
		./test_linux_arm64_async_scheduler_real --$$llam_case || exit $$?; \
	done
	./test_async_context_darwin || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --capture-late || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --restore-early-exit || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --ready-drain || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --offload || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --startup-abort || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --configuration || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --isolation || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --task-reader || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --witness-late || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --pending-signal || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --observed-cooperative || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --witness-read-refusal || test $$? -eq 77
	./test_darwin_arm64_async_scheduler_real --probe-publication || test $$? -eq 77
	./test_preempt_metadata
	./test_preempt_metadata_v2
	./test_preempt_metadata_registration
	./test_sync_primitives
	./test_io_buffers
	./test_windows_policy
	./test_windows_runtime_smoke
	./test_windows_iocp_io
	./test_windows_iocp_dump
	./test_windows_handle_io
	./test_security_capability
	./llam_broker --self-test
	$(PYTHON) scripts/test_broker_cli_parsing.py
	$(PYTHON) scripts/test_c_env_helpers.py
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
	$(PYTHON) -c 'import socket, sys; sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); sock.connect(sys.argv[1]); sock.shutdown(socket.SHUT_RDWR); sock.close()' "$$broker_sock" && \
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
	$(PYTHON) scripts/test_server_flood_cli.py ./server_flood
	$(PYTHON) scripts/test_server_flood_stats_security.py ./server_flood
	$(PYTHON) scripts/test_example_diagnostic_security.py ./server ./stress
	$(PYTHON) scripts/test_safe_output_security.py
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
	printf '3.0.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '3.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
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
	printf '3.0.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '3.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
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
	printf '3.0.0\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '3.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	case "$$(uname -s)" in \
		Darwin) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.2.dylib"; \
			printf 'not a symlink\n' > "$$tmp_dir/pkg/lib/libllam_runtime.dylib"; \
			;; \
		Linux) \
			: > "$$tmp_dir/pkg/lib/libllam_runtime.so.3.0.0"; \
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
	printf '3.0.0-rc.1\n' > "$$tmp_dir/pkg/VERSION"; \
	printf '2\n' > "$$tmp_dir/pkg/ABI_MAJOR"; \
	printf '3.0.0\n' > "$$tmp_dir/pkg/LIBRARY_VERSION"; \
	: > "$$tmp_dir/pkg/lib/libllam_runtime.so.3.0.0"; \
	ln -s libllam_runtime.so.3.0.0 "$$tmp_dir/pkg/lib/libllam_runtime.so.2"; \
	ln -s libllam_runtime.so.2 "$$tmp_dir/pkg/lib/libllam_runtime.so"; \
	sh "$$tmp_dir/pkg/install.sh" --prefix "$$tmp_dir/prefix" --force >"$$tmp_dir/install.out" 2>&1; \
	test -L "$$tmp_dir/prefix/lib/libllam_runtime.so"; \
	grep '^3.0.0-rc.1$$' "$$tmp_dir/prefix/share/llam/VERSION" >/dev/null; \
	grep '^3.0.0$$' "$$tmp_dir/prefix/share/llam/LIBRARY_VERSION" >/dev/null
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
	: > "$$tmp_dir/repo/NOTICE"; \
	: > "$$tmp_dir/repo/THIRD_PARTY_NOTICES.md"; \
	mkdir -p "$$tmp_dir/repo/third_party/magic2"; \
	cp third_party/magic2/LICENSE "$$tmp_dir/repo/third_party/magic2/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/scripts/cli_numbers.py"; \
	: > "$$tmp_dir/repo/scripts/process_utils.py"; \
	: > "$$tmp_dir/repo/scripts/safe_output.py"; \
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
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.3.0.0"; ln -s libllam_runtime.so.3.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	ln -s "$$tmp_dir/outside" "$$tmp_dir/repo/target"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
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
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
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
	: > "$$tmp_dir/repo/NOTICE"; \
	: > "$$tmp_dir/repo/THIRD_PARTY_NOTICES.md"; \
	mkdir -p "$$tmp_dir/repo/third_party/magic2"; \
	cp third_party/magic2/LICENSE "$$tmp_dir/repo/third_party/magic2/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/scripts/cli_numbers.py"; \
	: > "$$tmp_dir/repo/scripts/process_utils.py"; \
	: > "$$tmp_dir/repo/scripts/safe_output.py"; \
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
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.3.0.0"; ln -s libllam_runtime.so.3.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	chmod 666 "$$tmp_dir/repo/README.md"; \
	if (umask 000; LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target") >"$$tmp_dir/package.out" 2>&1; then \
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
	: > "$$tmp_dir/repo/NOTICE"; \
	: > "$$tmp_dir/repo/THIRD_PARTY_NOTICES.md"; \
	mkdir -p "$$tmp_dir/repo/third_party/magic2"; \
	cp third_party/magic2/LICENSE "$$tmp_dir/repo/third_party/magic2/LICENSE"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/scripts/cli_numbers.py"; \
	: > "$$tmp_dir/repo/scripts/process_utils.py"; \
	: > "$$tmp_dir/repo/scripts/safe_output.py"; \
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
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.3.0.0"; ln -s libllam_runtime.so.3.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
	: > "$$tmp_dir/repo/NOTICE"; \
	: > "$$tmp_dir/repo/THIRD_PARTY_NOTICES.md"; \
	mkdir -p "$$tmp_dir/repo/third_party/magic2"; \
	cp third_party/magic2/LICENSE "$$tmp_dir/repo/third_party/magic2/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/scripts/cli_numbers.py"; \
	: > "$$tmp_dir/repo/scripts/process_utils.py"; \
	: > "$$tmp_dir/repo/scripts/safe_output.py"; \
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
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.3.0.0"; ln -s libllam_runtime.so.3.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s libllam_runtime.so.2 "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
	: > "$$tmp_dir/repo/NOTICE"; \
	: > "$$tmp_dir/repo/THIRD_PARTY_NOTICES.md"; \
	mkdir -p "$$tmp_dir/repo/third_party/magic2"; \
	cp third_party/magic2/LICENSE "$$tmp_dir/repo/third_party/magic2/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/scripts/cli_numbers.py"; \
	: > "$$tmp_dir/repo/scripts/process_utils.py"; \
	: > "$$tmp_dir/repo/scripts/safe_output.py"; \
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
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.3.0.0"; ln -s libllam_runtime.so.3.0.0 "$$tmp_dir/repo/libllam_runtime.so.2"; ln -s README.md "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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
	: > "$$tmp_dir/repo/NOTICE"; \
	: > "$$tmp_dir/repo/THIRD_PARTY_NOTICES.md"; \
	mkdir -p "$$tmp_dir/repo/third_party/magic2"; \
	cp third_party/magic2/LICENSE "$$tmp_dir/repo/third_party/magic2/LICENSE"; \
	: > "$$tmp_dir/repo/README.md"; \
	: > "$$tmp_dir/repo/CHANGELOG.md"; \
	: > "$$tmp_dir/repo/scripts/install.sh"; \
	: > "$$tmp_dir/repo/scripts/install.ps1"; \
	: > "$$tmp_dir/repo/scripts/stress_server.py"; \
	: > "$$tmp_dir/repo/scripts/stress_server_composite.py"; \
	: > "$$tmp_dir/repo/scripts/cli_numbers.py"; \
	: > "$$tmp_dir/repo/scripts/process_utils.py"; \
	: > "$$tmp_dir/repo/scripts/safe_output.py"; \
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
		linux-*) : > "$$tmp_dir/repo/libllam_runtime.so.3.0.0"; : > "$$tmp_dir/repo/libllam_runtime.so.2"; : > "$$tmp_dir/repo/libllam_runtime.so" ;; \
	esac; \
	if LLAM_RELEASE_VERSION=ci LLAM_VERSION=3.0.0 LLAM_ABI_MAJOR=2 sh "$$tmp_dir/repo/scripts/package_release.sh" "$$package_target" >"$$tmp_dir/package.out" 2>&1; then \
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

check: test check-license-policy

ASAN_TEST_TARGETS = \
	asan-test_execution_lease_internal \
	asan-test_native_domain_internal \
	asan-test_managed_catalog_internal \
	asan-test_managed_foreign_internal \
	asan-test_capsule_pool_internal \
	asan-test_managed_scheduler_internal \
	asan-test_large_select \
	asan-test_io_watch_index \
	asan-test_runtime_regressions \
	asan-test_runtime_api_edges \
	asan-test_runtime_core \
	asan-test_io_buffers \
	asan-test_runtime_shutdown_internal \
	asan-test_watchdog_liveness_internal \
	asan-test_worker_identity_internal \
	asan-test_legacy_lease_internal \
	asan-test_processor_resource_internal \
	asan-test_processor_queue_timer_internal \
	asan-test_processor_allocator_io_internal \
	asan-test_foreign_detach_internal \
	asan-test_relief_worker_internal \
	asan-test_async_preempt_linux_x86_64 \
	asan-test_async_preempt_multi_linux_x86_64 \
	asan-test_preempt_metadata \
	asan-test_preempt_metadata_v2 \
	asan-test_preempt_metadata_registration \
	asan-test_preempt_capabilities \
	asan-test_default_policy_internal \
	asan-test_magic2_internal \
	asan-test_multi_runtime_core \
	asan-test_api_usability \
	asan-test_scope \
	asan-test_scope_values \
	asan-test_cavo_lifetime \
	asan-test_cavo_execution \
	asan-test_runtime_fuzz \
	asan-test_security_capability

NOOWNER_TEST_TARGETS = \
	noowner-test_runtime_select_edges

TSAN_TEST_TARGETS = \
	tsan-test_execution_lease_internal \
	tsan-test_native_domain_internal \
	tsan-test_managed_catalog_internal \
	tsan-test_managed_foreign_internal \
	tsan-test_capsule_pool_internal \
	tsan-test_managed_scheduler_internal \
	tsan-test_runtime_core \
	tsan-test_runtime_shutdown_internal \
	tsan-test_watchdog_liveness_internal \
	tsan-test_worker_identity_internal \
	tsan-test_legacy_lease_internal \
	tsan-test_processor_resource_internal \
	tsan-test_processor_queue_timer_internal \
	tsan-test_processor_allocator_io_internal \
	tsan-test_foreign_detach_internal \
	tsan-test_relief_worker_internal \
	tsan-test_async_preempt_linux_x86_64 \
	tsan-test_async_preempt_multi_linux_x86_64 \
	tsan-test_preempt_metadata \
	tsan-test_preempt_metadata_v2 \
	tsan-test_preempt_metadata_registration \
	tsan-test_preempt_capabilities \
	tsan-test_default_policy_internal \
	tsan-test_magic2_internal \
	tsan-test_multi_runtime_core \
	tsan-test_api_usability \
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
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_execution_lease_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_native_domain_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_managed_catalog_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_managed_foreign_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_capsule_pool_internal; \
	ASAN_OPTIONS=halt_on_error=1:handle_abort=1:allow_addr2line=1 UBSAN_OPTIONS=halt_on_error=1 LLAM_MANAGED_SCHEDULER_VERBOSE=1 ./asan-test_managed_scheduler_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_io_watch_index || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_large_select || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_api_edges; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_core; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_regressions; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_io_buffers; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_shutdown_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_watchdog_liveness_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_watchdog_liveness_internal --wait-reference; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_worker_identity_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_legacy_lease_internal || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_processor_resource_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_processor_queue_timer_internal || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_processor_allocator_io_internal || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_foreign_detach_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_relief_worker_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_async_preempt_linux_x86_64 || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_async_preempt_multi_linux_x86_64 || test $$? -eq 77; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_preempt_metadata; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_preempt_metadata_v2; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_preempt_metadata_registration; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_preempt_capabilities; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_default_policy_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_magic2_internal; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_multi_runtime_core; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_api_usability; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_scope; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_scope_values; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_cavo_lifetime; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_cavo_lifetime --io-helper; \
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_cavo_execution; \
	LLAM_RUNTIME_FUZZ_SCENARIOS=16 LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS=16 \
		ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_runtime_fuzz; \
	: "Keep the ASan broker task-race probe short; full-strength coverage stays in normal test."; \
	LLAM_SECURITY_TASK_DETACH_RACE_ROUNDS=16 \
		ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./asan-test_security_capability; \
	$(PYTHON) scripts/test_legacy_lease_boundary.py; \
	$(PYTHON) scripts/check_legacy_lease_boundary.py --root .

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
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_watchdog_liveness_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_worker_identity_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_legacy_lease_internal || test $$? -eq 77; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_processor_resource_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_processor_queue_timer_internal || test $$? -eq 77; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_processor_allocator_io_internal || test $$? -eq 77; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_foreign_detach_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_relief_worker_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_async_preempt_linux_x86_64 || test $$? -eq 77; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_async_preempt_multi_linux_x86_64 || test $$? -eq 77; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_preempt_metadata; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_preempt_metadata_v2; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_preempt_metadata_registration; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_preempt_capabilities; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_default_policy_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_magic2_internal; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_multi_runtime_core; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_api_usability; \
	LLAM_RUNTIME_FUZZ_SCENARIOS=8 LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS=8 \
		TSAN_OPTIONS=halt_on_error=1 ./tsan-test_runtime_fuzz; \
	TSAN_OPTIONS=halt_on_error=1 ./tsan-test_security_capability; \
	$(PYTHON) scripts/test_legacy_lease_boundary.py; \
	$(PYTHON) scripts/check_legacy_lease_boundary.py --root .


CPPCHECK_HOST_ARGS :=
ifeq ($(HOST_PLATFORM),linux)
CPPCHECK_HOST_ARGS += -D__linux__=1
else ifeq ($(HOST_PLATFORM),darwin)
CPPCHECK_HOST_ARGS += -D__APPLE__=1 -D__MACH__=1
endif
ifeq ($(UNAME_M),x86_64)
CPPCHECK_HOST_ARGS += -D__x86_64__=1
else ifneq ($(filter arm64 aarch64,$(UNAME_M)),)
CPPCHECK_HOST_ARGS += -D__aarch64__=1
endif
# Follow registered host translation units; foreign-only probe sources retain
# their compile-time errors. Test hooks are enabled only for the test pass.
CPPCHECK_RUNTIME_SOURCES = $(wildcard $(patsubst $(OBJDIR)/%.o,%.c,$(RUNTIME_OBJS)))
CPPCHECK_ARGS = --platform=unix64 --std=c11 --enable=warning,performance,portability \
	--error-exitcode=1 --inline-suppr \
	--suppress=preprocessorErrorDirective:src/internal/magic2.h \
	-DUINTPTR_MAX=18446744073709551615ULL -DUINT32_MAX=4294967295U \
	$(CPPCHECK_HOST_ARGS) $(CPPFLAGS)

analyze-cppcheck:
	cppcheck $(CPPCHECK_ARGS) $(CPPCHECK_RUNTIME_SOURCES) include examples
	cppcheck $(CPPCHECK_ARGS) -DLLAM_ENABLE_TEST_HOOKS=1 tests

audit-deps:
	cd scripts/bench_tokio_compare && cargo audit

test-fuzz-heavy: test_runtime_fuzz
	LLAM_RUNTIME_FUZZ_SCENARIOS=$(FUZZ_HEAVY_RUNTIME_SCENARIOS) \
	LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS=$(FUZZ_HEAVY_MULTI_RUNTIME_SCENARIOS) \
		./test_runtime_fuzz

test-lcwe-model: test_lcwe_model bench_lcwe_model
	./test_lcwe_model
	LCWE_MODEL_TEST_BINARY=./bench_lcwe_model python3 scripts/test_bench_lcwe_model.py

lcwe-model-report: test-lcwe-model
	python3 scripts/bench_lcwe_model.py \
		--binary ./bench_lcwe_model \
		--cc "$(CC)" \
		--out-dir object/lcwe-phase1

test-process-utils:
	python3 scripts/test_process_utils.py
	python3 scripts/test_bench_parsers.py
	python3 scripts/test_bench_runtime_compare.py
	python3 scripts/test_cpu_spike_compare.py
	go test scripts/cpu_spike_go.go scripts/cpu_spike_go_test.go
	python3 scripts/test_stress_server_logic.py
	python3 scripts/test_c_env_helpers.py

test-runtime-soak: test_runtime_fuzz test_multi_runtime_core test_runtime_stress test_runtime_shutdown_internal test_io_buffers
	python3 scripts/runtime_soak.py \
		--duration $(RUNTIME_SOAK_SECONDS) \
		--timeout $(RUNTIME_SOAK_TIMEOUT) \
		--seed $(RUNTIME_SOAK_SEED) \
		--fuzz-scenarios $(RUNTIME_SOAK_FUZZ_SCENARIOS) \
		--multi-fuzz-scenarios $(RUNTIME_SOAK_MULTI_FUZZ_SCENARIOS)

test-hardening: analyze-cppcheck audit-deps test-process-utils
	# Sanitizer/soak-style binaries use tight wall-clock bounds; keep them isolated.
	$(MAKE) test-asan
	$(MAKE) test-tsan
	$(MAKE) test-fuzz-heavy

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

cpu_spike: $(RUNTIME_OBJS) $(CPU_SPIKE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(CPU_SPIKE_OBJS) $(LDLIBS)

llam_broker: $(RUNTIME_OBJS) $(BROKER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(BROKER_OBJS) $(LDLIBS)

server: $(RUNTIME_OBJS) $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(SERVER_OBJS) $(LDLIBS)

server_lossless: $(RUNTIME_OBJS) $(SERVER_LOSSLESS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(SERVER_LOSSLESS_OBJS) $(LDLIBS)

server_flood: $(SERVER_FLOOD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_FLOOD_OBJS) $(SERVER_FLOOD_LDLIBS)

test_lcwe_model: $(LCWE_MODEL_CORE_OBJS) $(LCWE_MODEL_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(LCWE_MODEL_CORE_OBJS) $(LCWE_MODEL_TEST_OBJS) $(SERVER_FLOOD_LDLIBS)

bench_lcwe_model: $(LCWE_MODEL_CORE_OBJS) $(LCWE_MODEL_BENCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $(LCWE_MODEL_CORE_OBJS) $(LCWE_MODEL_BENCH_OBJS) $(SERVER_FLOOD_LDLIBS)

# Private policy experiment; production defaults and ABI are unchanged.
bench_handoff: $(RUNTIME_OBJS) $(OBJDIR)/experiments/handoff/bench_handoff.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test_handoff_protocol: $(RUNTIME_OBJS) $(OBJDIR)/experiments/handoff/verify_handoff.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/experiments/handoff/%.o: experiments/handoff/%.c $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

.PHONY: test-handoff
bench_watch_index: $(RUNTIME_OBJS) $(OBJDIR)/experiments/watch_index/bench_watch_index.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

bench_select_prepare: $(RUNTIME_OBJS) $(OBJDIR)/experiments/select_prepare/bench_select_prepare.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

ifeq ($(HOST_PLATFORM),darwin)
MANAGED_BENCH_IMAGE_SUFFIX = .dylib
MANAGED_BENCH_IMAGE_FLAGS = -dynamiclib -Wl,-undefined,dynamic_lookup -fno-register-global-dtors-with-atexit
MANAGED_BENCH_EXPORT_FLAGS = -Wl,-export_dynamic
else
MANAGED_BENCH_IMAGE_SUFFIX = .so
MANAGED_BENCH_IMAGE_FLAGS = -shared
MANAGED_BENCH_EXPORT_FLAGS = -Wl,--export-dynamic
endif
MANAGED_BENCH_IMAGE = $(OBJDIR)/managed-bench-workload$(MANAGED_BENCH_IMAGE_SUFFIX)
$(MANAGED_BENCH_IMAGE): experiments/async_compare/managed_workload.c experiments/async_compare/managed_bench.h \
	include/llam/runtime.h include/llam/io.h tools/preempt/managed_driver.py tools/preempt/managed_verify.py tools/preempt/managed_runtime_contracts.def
	@mkdir -p $(dir $@)
	$(PYTHON) -B tools/preempt/managed_driver.py --llam-managed=required $(MANAGED_BENCH_IMAGE_FLAGS) -Iinclude $< -o $@

MANAGED_PROFILE_OBJS = $(patsubst $(OBJDIR)/%,$(OBJDIR)/native-profile/%,$(RUNTIME_OBJS))
$(OBJDIR)/native-profile/%.o: %.c $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLLAM_ENABLE_ASYNC_PROFILE=1 -c -o $@ $<

$(OBJDIR)/native-profile/%.o: %.S src/internal/llam_internal.h src/internal/async_context_phase.h $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLLAM_ENABLE_ASYNC_PROFILE=1 -c -o $@ $<

bench_managed_preempt: $(MANAGED_PROFILE_OBJS) $(OBJDIR)/experiments/async_compare/bench_managed.o $(MANAGED_BENCH_IMAGE)
	$(CC) $(CFLAGS) $(MANAGED_BENCH_EXPORT_FLAGS) -o $@ $(MANAGED_PROFILE_OBJS) $(OBJDIR)/experiments/async_compare/bench_managed.o $(LDLIBS)

$(OBJDIR)/experiments/async_compare/bench_managed.o: experiments/async_compare/bench_managed.c \
	experiments/async_compare/managed_bench.h $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLLAM_ENABLE_ASYNC_PROFILE=1 -c -o $@ $<

ifeq ($(HOST_PLATFORM)-$(UNAME_M)-$(LLAM_EXPERIMENTAL_ASYNC_PREEMPT),darwin-arm64-1)
bench_async_compare: $(RUNTIME_OBJS) $(filter-out $(OBJDIR)/examples/bench_entry.o,$(BENCH_OBJS)) $(OBJDIR)/experiments/async_compare/bench_entry.o $(OBJDIR)/experiments/async_compare/bench_async.o $(OBJDIR)/tests/fixtures/async_darwin_leaf.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/experiments/async_compare/bench_entry.o: examples/bench_entry.c experiments/async_compare/bench_async.h examples/bench_internal.h $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLLAM_BENCH_ASYNC_COMPARE=1 -c -o $@ $<

$(OBJDIR)/experiments/async_compare/bench_async.o: experiments/async_compare/bench_async.c experiments/async_compare/bench_async.h tests/fixtures/async_darwin_leaf.h $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
else
bench_async_compare:
	@echo "bench_async_compare requires Darwin arm64 and LLAM_EXPERIMENTAL_ASYNC_PREEMPT=1" >&2
	@exit 2
endif

test_large_select: $(RUNTIME_OBJS) $(OBJDIR)/tests/test_large_select.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

asan-test_large_select: require-sanitizer-target $(RUNTIME_OBJS) $(OBJDIR)/tests/test_large_select.o
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(OBJDIR)/tests/test_large_select.o $(LDLIBS)

$(OBJDIR)/experiments/select_prepare/%.o: experiments/select_prepare/%.c $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test_io_watch_index: $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_io_watch_index.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

asan-test_io_watch_index: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_io_watch_index.o
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_io_watch_index.o $(LDLIBS)

$(OBJDIR)/tests/test_io_watch_index.o: tests/test_io_watch_index.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/experiments/watch_index/%.o: experiments/watch_index/%.c $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test-handoff: bench_handoff test_handoff_protocol
	./test_handoff_protocol
	HANDOFF_BENCH_BINARY=./bench_handoff $(PYTHON) -B scripts/test_handoff_model.py

test_abi_contract: $(RUNTIME_OBJS) $(TEST_ABI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_ABI_OBJS) $(LDLIBS)

test_abi_compat: $(RUNTIME_OBJS) $(TEST_ABI_COMPAT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_ABI_COMPAT_OBJS) $(LDLIBS)

test_connect_io: $(RUNTIME_OBJS) $(TEST_CONNECT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_CONNECT_OBJS) $(LDLIBS)

test_runtime_core: $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS) $(LDLIBS)

test_runtime_regressions: $(RUNTIME_OBJS) $(TEST_RUNTIME_REGRESSIONS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_REGRESSIONS_OBJS) $(LDLIBS)

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

test_watchdog_liveness_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS) $(LDLIBS)

test_runtime_shutdown_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS) $(LDLIBS)

test_worker_identity_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_WORKER_IDENTITY_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_WORKER_IDENTITY_INTERNAL_OBJS) $(LDLIBS)

test_legacy_lease_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_LEGACY_LEASE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_LEGACY_LEASE_INTERNAL_OBJS) $(LDLIBS)

test_execution_lease_internal: $(RUNTIME_OBJS) $(TEST_EXECUTION_LEASE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_EXECUTION_LEASE_INTERNAL_OBJS) $(LDLIBS)

test_native_domain_internal: $(RUNTIME_OBJS) $(TEST_NATIVE_DOMAIN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_NATIVE_DOMAIN_INTERNAL_OBJS) $(LDLIBS)

test_managed_catalog_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_CATALOG_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_CATALOG_INTERNAL_OBJS) $(LDLIBS)

test_managed_foreign_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_FOREIGN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_FOREIGN_INTERNAL_OBJS) $(LDLIBS)

test_capsule_pool_internal: $(RUNTIME_OBJS) $(TEST_CAPSULE_POOL_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_CAPSULE_POOL_INTERNAL_OBJS) $(LDLIBS)

asan-test_capsule_pool_internal tsan-test_capsule_pool_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_CAPSULE_POOL_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_CAPSULE_POOL_INTERNAL_OBJS) $(LDLIBS)

test_managed_scheduler_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_SCHEDULER_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_SCHEDULER_INTERNAL_OBJS) $(LDLIBS)

ifeq ($(HOST_PLATFORM),linux)
MANAGED_LINUX_COMPILER_EXPORT_FLAGS = -Wl,--export-dynamic
endif
$(OBJDIR)/tests/test_managed_linux_compiler.o: CPPFLAGS += -DLLAM_ENABLE_TEST_HOOKS=1
$(OBJDIR)/tests/test_managed_linux_compiler.o: $(TESTHOOK_BUILD_SIGNATURE)
test_managed_linux_compiler: $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_managed_linux_compiler.o
	$(CC) $(CFLAGS) $(MANAGED_LINUX_COMPILER_EXPORT_FLAGS) -o $@ $^ $(LDLIBS)

test_managed_async_registered: $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_ASYNC_REGISTERED_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_ASYNC_REGISTERED_OBJS) $(LDLIBS)

test_managed_async_linux_x86_64: $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_ASYNC_LINUX_X86_64_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_ASYNC_LINUX_X86_64_OBJS) $(LDLIBS)

test_managed_image_internal: $(RUNTIME_OBJS) $(TEST_MANAGED_IMAGE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) $(MANAGED_CONSUMER_EXPORT_FLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_MANAGED_IMAGE_INTERNAL_OBJS) $(LDLIBS)

test_managed_async_compiler: $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_ASYNC_COMPILER_OBJS)
	$(CC) $(CFLAGS) $(MANAGED_CONSUMER_EXPORT_FLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_ASYNC_COMPILER_OBJS) $(LDLIBS)

asan-test_managed_catalog_internal tsan-test_managed_catalog_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_CATALOG_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_CATALOG_INTERNAL_OBJS) $(LDLIBS)

asan-test_managed_foreign_internal tsan-test_managed_foreign_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_FOREIGN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_FOREIGN_INTERNAL_OBJS) $(LDLIBS)

asan-test_managed_scheduler_internal tsan-test_managed_scheduler_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_SCHEDULER_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_MANAGED_SCHEDULER_INTERNAL_OBJS) $(LDLIBS)

asan-test_native_domain_internal tsan-test_native_domain_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_NATIVE_DOMAIN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_NATIVE_DOMAIN_INTERNAL_OBJS) $(LDLIBS)

test_processor_resource_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_RESOURCE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_RESOURCE_INTERNAL_OBJS) $(LDLIBS)

test_processor_queue_timer_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_QUEUE_TIMER_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_QUEUE_TIMER_INTERNAL_OBJS) $(LDLIBS)

test_processor_allocator_io_internal: $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_ALLOCATOR_IO_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_ALLOCATOR_IO_INTERNAL_OBJS) $(LDLIBS)

test_foreign_detach_internal: $(RUNTIME_OBJS) $(TEST_FOREIGN_DETACH_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_FOREIGN_DETACH_INTERNAL_OBJS) $(LDLIBS)

test_relief_worker_internal: $(RUNTIME_OBJS) $(TEST_RELIEF_WORKER_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RELIEF_WORKER_INTERNAL_OBJS) $(LDLIBS)

test_preempt_capabilities: $(RUNTIME_OBJS) $(TEST_PREEMPT_CAPABILITIES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_PREEMPT_CAPABILITIES_OBJS) $(LDLIBS)

test_default_policy_internal: $(RUNTIME_OBJS) $(TEST_DEFAULT_POLICY_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_DEFAULT_POLICY_INTERNAL_OBJS) $(LDLIBS)

test_magic2_internal: $(RUNTIME_OBJS) $(TEST_MAGIC2_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_MAGIC2_INTERNAL_OBJS) $(LDLIBS)

bench_magic2_cpu: $(RUNTIME_OBJS) $(OBJDIR)/experiments/magic2/bench_magic2_cpu.o
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(OBJDIR)/experiments/magic2/bench_magic2_cpu.o $(LDLIBS)

bench_magic2_kernels: $(RUNTIME_OBJS) $(OBJDIR)/experiments/magic2/bench_magic2_kernels.o
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(OBJDIR)/experiments/magic2/bench_magic2_kernels.o $(LDLIBS)

$(OBJDIR)/experiments/magic2/bench_magic2_kernels.o: experiments/magic2/bench_magic2_kernels.c $(RUNTIME_PRIV_HDRS) src/internal/runtime_magic2.h src/internal/magic2.h $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/experiments/magic2/bench_magic2_cpu.o: experiments/magic2/bench_magic2_cpu.c $(RUNTIME_PRIV_HDRS) src/internal/runtime_magic2.h src/internal/magic2.h $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

asan-test_magic2_internal tsan-test_magic2_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_MAGIC2_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_MAGIC2_INTERNAL_OBJS) $(LDLIBS)

asan-test_preempt_capabilities tsan-test_preempt_capabilities: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_PREEMPT_CAPABILITIES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_PREEMPT_CAPABILITIES_OBJS) $(LDLIBS)

asan-test_default_policy_internal tsan-test_default_policy_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_DEFAULT_POLICY_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_DEFAULT_POLICY_INTERNAL_OBJS) $(LDLIBS)

test_async_preempt_linux_x86_64: $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS)
	$(CC) $(CFLAGS) $(TEST_ASYNC_SIGNAL_WRAP_LDFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS) $(LDLIBS)

test_async_preempt_multi_linux_x86_64: $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_MULTI_LINUX_X86_64_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_MULTI_LINUX_X86_64_OBJS) $(LDLIBS)

test_async_context_linux_arm64: $(TEST_ASYNC_CONTEXT_LINUX_ARM64_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_ASYNC_CONTEXT_LINUX_ARM64_OBJS) -pthread

test_linux_arm64_async_scheduler_real: $(RUNTIME_TESTHOOK_OBJS) $(TEST_LINUX_ARM64_ASYNC_SCHEDULER_REAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_LINUX_ARM64_ASYNC_SCHEDULER_REAL_OBJS) $(LDLIBS)

test_async_context_darwin: $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_CONTEXT_DARWIN_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_CONTEXT_DARWIN_OBJS) $(LDLIBS)

test_darwin_arm64_async_scheduler_real: $(RUNTIME_TESTHOOK_OBJS) $(TEST_DARWIN_ARM64_ASYNC_SCHEDULER_REAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_DARWIN_ARM64_ASYNC_SCHEDULER_REAL_OBJS) $(LDLIBS)

test: test_async_preempt_multi_linux_x86_64 test_async_context_linux_arm64 test_async_context_darwin test_darwin_arm64_async_scheduler_real test_linux_arm64_async_scheduler_real

asan-test_async_preempt_multi_linux_x86_64 tsan-test_async_preempt_multi_linux_x86_64: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_MULTI_LINUX_X86_64_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_MULTI_LINUX_X86_64_OBJS) $(LDLIBS)

llam_preempt_metadata_inspect: $(PREEMPT_METADATA_OBJS) $(PREEMPT_METADATA_INSPECT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PREEMPT_METADATA_OBJS) $(PREEMPT_METADATA_INSPECT_OBJS)

test_preempt_metadata: $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_OBJS)

test_preempt_metadata_v2: $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_V2_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_V2_OBJS)

asan-test_preempt_metadata_v2 tsan-test_preempt_metadata_v2: require-sanitizer-target $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_V2_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_V2_OBJS)

test_preempt_metadata_registration: $(RUNTIME_OBJS) $(TEST_PREEMPT_METADATA_REGISTRATION_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_PREEMPT_METADATA_REGISTRATION_OBJS) $(LDLIBS)

asan-test_preempt_metadata: $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_OBJS)

asan-test_preempt_metadata_registration tsan-test_preempt_metadata_registration: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_PREEMPT_METADATA_REGISTRATION_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_PREEMPT_METADATA_REGISTRATION_OBJS) $(LDLIBS)

tsan-test_preempt_metadata: $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_OBJS)
	$(CC) $(CFLAGS) -o $@ $(PREEMPT_METADATA_OBJS) $(TEST_PREEMPT_METADATA_OBJS)

test-preempt-metadata: test_preempt_metadata test_preempt_metadata_v2
	./test_preempt_metadata
	./test_preempt_metadata_v2
	@if command -v bash >/dev/null 2>&1; then \
		$(PYTHON) scripts/test_llam_clang.py; \
	else \
		echo "llam-clang wrapper test skipped: bash unavailable"; \
	fi
	$(PYTHON) scripts/test_preempt_sdk_metadata.py

test-preempt-sdk-metadata:
	$(PYTHON) scripts/test_preempt_sdk_metadata.py

test-preempt-package:
	$(PYTHON) scripts/test_preempt_package.py
	$(PYTHON) scripts/test_package_license_layout.py

.PHONY: test-sdk-install
SDK_TEST_BUILD_DIR ?= build
test-sdk-install:
	$(PYTHON) scripts/test_sdk_install.py --build-dir "$(SDK_TEST_BUILD_DIR)"

preempt-tools:
	@set -e; case "$(LLAM_PREEMPT_TOOLING)" in \
		off) echo "preemption tooling disabled; use LLAM_PREEMPT_TOOLING=auto or required" ;; \
		auto|required) \
			cmake -S . -B "$(PREEMPT_TOOLING_BUILD_DIR)" \
				-DLLAM_PREEMPT_TOOLING="$(LLAM_PREEMPT_TOOLING)"; \
			cmake --build "$(PREEMPT_TOOLING_BUILD_DIR)" \
				--target llam_preempt_metadata_inspect test_preempt_metadata \
					test_preempt_metadata_registration; \
			if grep -q '^LLAM_PREEMPT_PLUGIN_AVAILABLE:INTERNAL=TRUE$$' \
				"$(PREEMPT_TOOLING_BUILD_DIR)/CMakeCache.txt"; then \
				cmake --build "$(PREEMPT_TOOLING_BUILD_DIR)" --target llam_preempt_pass; \
			fi; \
			ctest --test-dir "$(PREEMPT_TOOLING_BUILD_DIR)" \
				--output-on-failure -R '^(test_preempt_metadata|test_preempt_metadata_registration|test_llam_clang_wrapper|test_preempt_sdk_metadata|test_preempt_cmake_install|test_preempt_package|test_llam_preempt_llvm_plugin)$$' ;; \
		*) echo "LLAM_PREEMPT_TOOLING must be off, auto, or required" >&2; exit 2 ;; \
	esac

test-processor-resource: test_processor_resource_internal
	./test_processor_resource_internal
	$(PYTHON) scripts/test_legacy_lease_boundary.py
	$(PYTHON) scripts/check_legacy_lease_boundary.py --root .

test-processor-queue-timer: test_processor_queue_timer_internal
	./test_processor_queue_timer_internal

test-processor-allocator-io: test_processor_allocator_io_internal
	./test_processor_allocator_io_internal || test $$? -eq 77

test-foreign-detach: test_foreign_detach_internal
	./test_foreign_detach_internal

test-relief-worker: test_relief_worker_internal
	./test_relief_worker_internal

test_sync_primitives: $(RUNTIME_OBJS) $(TEST_SYNC_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_SYNC_OBJS) $(LDLIBS)

test_io_buffers: $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS) $(LDLIBS)

asan-test_runtime_api_edges: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_API_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_API_EDGES_OBJS) $(LDLIBS)

asan-test_runtime_core tsan-test_runtime_core: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS) $(LDLIBS)

asan-test_runtime_regressions: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RUNTIME_REGRESSIONS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_REGRESSIONS_OBJS) $(LDLIBS)

noowner-test_runtime_select_edges: $(RUNTIME_OBJS) $(TEST_RUNTIME_SELECT_EDGES_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_SELECT_EDGES_OBJS) $(LDLIBS)

asan-test_io_buffers: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS) $(LDLIBS)

asan-test_runtime_shutdown_internal tsan-test_runtime_shutdown_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_RUNTIME_SHUTDOWN_INTERNAL_OBJS) $(LDLIBS)

asan-test_worker_identity_internal tsan-test_worker_identity_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_WORKER_IDENTITY_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_WORKER_IDENTITY_INTERNAL_OBJS) $(LDLIBS)

asan-test_execution_lease_internal tsan-test_execution_lease_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_EXECUTION_LEASE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_EXECUTION_LEASE_INTERNAL_OBJS) $(LDLIBS)

asan-test_legacy_lease_internal tsan-test_legacy_lease_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_LEGACY_LEASE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_LEGACY_LEASE_INTERNAL_OBJS) $(LDLIBS)

asan-test_processor_resource_internal tsan-test_processor_resource_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_RESOURCE_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_RESOURCE_INTERNAL_OBJS) $(LDLIBS)

asan-test_processor_queue_timer_internal tsan-test_processor_queue_timer_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_QUEUE_TIMER_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_QUEUE_TIMER_INTERNAL_OBJS) $(LDLIBS)

asan-test_processor_allocator_io_internal tsan-test_processor_allocator_io_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_ALLOCATOR_IO_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_PROCESSOR_ALLOCATOR_IO_INTERNAL_OBJS) $(LDLIBS)

asan-test_foreign_detach_internal tsan-test_foreign_detach_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_FOREIGN_DETACH_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_FOREIGN_DETACH_INTERNAL_OBJS) $(LDLIBS)

asan-test_relief_worker_internal tsan-test_relief_worker_internal: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_RELIEF_WORKER_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RELIEF_WORKER_INTERNAL_OBJS) $(LDLIBS)

asan-test_async_preempt_linux_x86_64 tsan-test_async_preempt_linux_x86_64: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS)
	$(CC) $(CFLAGS) $(TEST_ASYNC_SIGNAL_WRAP_LDFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_ASYNC_PREEMPT_LINUX_X86_64_OBJS) $(LDLIBS)

asan-test_watchdog_liveness_internal tsan-test_watchdog_liveness_internal: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(TEST_WATCHDOG_LIVENESS_INTERNAL_OBJS) $(LDLIBS)

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

$(OBJDIR)/experiments/lcwe/%.o: experiments/lcwe/%.c \
		experiments/lcwe/lcwe_model.h \
		experiments/lcwe/lcwe_model_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iexperiments/lcwe -c -o $@ $<

$(OBJDIR)/tools/preempt/%.o: tools/preempt/%.c tools/preempt/metadata.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itools/preempt -c -o $@ $<

$(SHARED_OBJDIR)/tools/preempt/%.o: tools/preempt/%.c tools/preempt/metadata.h
	@mkdir -p $(dir $@)
	$(CC) $(SHARED_CPPFLAGS) $(CFLAGS) $(PICFLAGS) -Itools/preempt -c -o $@ $<

$(OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(TESTHOOK_OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(TESTHOOK_OBJDIR)/src/io/%.o: src/io/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(TESTHOOK_OBJDIR)/src/engine/%.o: src/engine/%.c $(RUNTIME_PRIV_HDRS)
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

$(OBJDIR)/examples/cpu_spike.o: examples/cpu_spike.c $(LLAM_PUBLIC_HDRS)
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

$(OBJDIR)/tests/fixtures/%.o: tests/fixtures/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_security_capability.o: tests/test_security_capability.c $(RUNTIME_PRIV_HDRS) tests/test_env.h $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_runtime_shutdown_internal.o: tests/test_runtime_shutdown_internal.c $(RUNTIME_PRIV_HDRS) tests/test_env.h $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_worker_identity_internal.o: tests/test_worker_identity_internal.c $(RUNTIME_PRIV_HDRS) tests/test_env.h $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_legacy_lease_internal.o: tests/test_legacy_lease_internal.c $(RUNTIME_PRIV_HDRS) tests/test_env.h $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_async_preempt_linux_x86_64.o: tests/test_async_preempt_linux_x86_64.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_ASYNC_SIGNAL_WRAP_CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_watchdog_liveness_internal.o: tests/test_watchdog_liveness_internal.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_async_preempt_multi_linux_x86_64.o: tests/test_async_preempt_multi_linux_x86_64.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_async_context_linux_arm64.o: tests/test_async_context_linux_arm64.c tests/fixtures/async_darwin_leaf.h $(RUNTIME_PRIV_HDRS) $(BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_linux_arm64_async_scheduler_real.o: tests/test_darwin_arm64_async_scheduler_real.c tests/fixtures/async_darwin_leaf.h $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 -DLLAM_ASYNC_SHARED_TEST_LINUX=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_async_context_darwin.o: tests/test_async_context_darwin.c tests/fixtures/async_darwin_leaf.h $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_darwin_arm64_async_scheduler_real.o: tests/test_darwin_arm64_async_scheduler_real.c tests/fixtures/async_darwin_leaf.h $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_processor_resource_internal.o: tests/test_processor_resource_internal.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_processor_queue_timer_internal.o: tests/test_processor_queue_timer_internal.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -DLLAM_ENABLE_TEST_HOOKS=1 $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/test_processor_allocator_io_internal.o: tests/test_processor_allocator_io_internal.c $(RUNTIME_PRIV_HDRS) $(TESTHOOK_BUILD_SIGNATURE)
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

package: all test preempt-tools
	LLAM_PREEMPT_TOOLING="$(LLAM_PREEMPT_TOOLING)" \
	PREEMPT_TOOLING_BUILD_DIR="$(PREEMPT_TOOLING_BUILD_DIR)" \
		./scripts/package_release.sh

bench-matrix: bench
	python3 scripts/bench_matrix.py

cpu-spike-compare: cpu_spike
	python3 scripts/cpu_spike_compare.py

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

ifneq ($(HOST_PLATFORM),windows)
# Public embedding/cancellation/deadline usability contract.
test_api_usability: $(RUNTIME_OBJS) $(TEST_API_USABILITY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_API_USABILITY_OBJS) $(LDLIBS)

$(OBJDIR)/tests/test_cavo_lifetime.o $(OBJDIR)/tests/cavo_io_lifetime.o: tests/cavo_lifetime_internal.h

$(OBJDIR)/tests/test_cavo_lifetime.o: CPPFLAGS += -DLLAM_ENABLE_TEST_HOOKS=1

test_cavo_lifetime: $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_cavo_lifetime.o $(OBJDIR)/tests/cavo_io_lifetime.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

test_cavo_execution: $(RUNTIME_OBJS) $(OBJDIR)/tests/test_cavo_execution.o $(OBJDIR)/tests/cavo_local_executor.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(OBJDIR)/tests/test_cavo_execution.o $(OBJDIR)/tests/cavo_local_executor.o: tests/cavo_execution_internal.h tests/fixtures/managed/cavo_execution.h

test_scope_values: $(RUNTIME_OBJS) $(OBJDIR)/tests/test_scope_values.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

$(OBJDIR)/tests/test_scope.o: CPPFLAGS += -DLLAM_ENABLE_TEST_HOOKS=1

test_scope: $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_scope.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

test_scope_cpp: $(RUNTIME_OBJS) tests/test_scope_cpp.cpp include/llam/future.hpp $(LLAM_PUBLIC_HDRS)
	$(CXX) $(CPPFLAGS) $(filter-out -std=c11,$(CFLAGS)) -std=c++17 -o $@ tests/test_scope_cpp.cpp $(RUNTIME_OBJS) $(LDLIBS)

example_scoped_results: $(RUNTIME_OBJS) $(OBJDIR)/examples/scoped_results.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

asan-test_cavo_lifetime: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_cavo_lifetime.o $(OBJDIR)/tests/cavo_io_lifetime.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

asan-test_cavo_execution: require-sanitizer-target $(RUNTIME_OBJS) $(OBJDIR)/tests/test_cavo_execution.o $(OBJDIR)/tests/cavo_local_executor.o
	$(CC) $(CFLAGS) -o $@ $(filter %.o,$^) $(LDLIBS)

asan-test_scope: require-sanitizer-target $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_scope.o
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) $(OBJDIR)/tests/test_scope.o $(LDLIBS)

asan-test_scope_values: require-sanitizer-target $(RUNTIME_OBJS) $(OBJDIR)/tests/test_scope_values.o
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(OBJDIR)/tests/test_scope_values.o $(LDLIBS)

asan-test_api_usability tsan-test_api_usability: require-sanitizer-target $(RUNTIME_OBJS) $(TEST_API_USABILITY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_API_USABILITY_OBJS) $(LDLIBS)

.PHONY: test-platform-support
test-platform-support:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) scripts/test_platform_support.py

example_explicit_runtime: $(RUNTIME_OBJS) $(OBJDIR)/examples/explicit_runtime.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

.PHONY: test-docs-api-coverage
test-docs-api-coverage:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) scripts/test_docs_api_coverage.py

test: test-platform-support test-docs-api-coverage example_explicit_runtime

$(OBJDIR)/examples/scoped_results.o: examples/scoped_results.c $(LLAM_PUBLIC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/explicit_runtime.o: examples/explicit_runtime.c $(LLAM_PUBLIC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

all: example_explicit_runtime
endif

.PHONY: test-managed-ci-contract test-managed-benchmark-inputs
test-managed-ci-contract:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) scripts/test_managed_ci.py

test-managed-benchmark-inputs:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) scripts/test_managed_benchmark.py

ifneq ($(HOST_PLATFORM),windows)
test: test-managed-ci-contract test-managed-benchmark-inputs
endif
