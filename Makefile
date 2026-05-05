CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g -fno-omit-frame-pointer
CPPFLAGS ?= -Iinclude -Isrc/internal -Isrc -D_GNU_SOURCE
LDLIBS ?= -pthread -luring
OBJDIR ?= object
CLEAN_DIRS = \
	$(OBJDIR) \
	build \
	CMakeFiles \
	cmake-build-* \
	scripts/bench_tokio_compare/target
CLEAN_FILES = \
	demo \
	stress \
	bench \
	demo.exe \
	stress.exe \
	bench.exe \
	CMakeCache.txt \
	cmake_install.cmake \
	compile_commands.json \
	perf.data \
	perf.data.old
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
else
HOST_PLATFORM := posix
endif
endif

RUNTIME_PRIV_HDRS = \
	include/llam/nm_platform.h \
	include/llam/nm_runtime.h \
	include/llam/platform.h \
	include/llam/runtime.h \
	src/internal/runtime_platform.h \
	src/internal/nm_internal.h \
	src/internal/runtime_internal.h \
	src/internal/runtime_types.h \
	src/internal/runtime_state.h \
	src/internal/runtime_protos.h \
	src/internal/runtime_proto_core.h \
	src/internal/runtime_proto_sched.h \
	src/internal/runtime_proto_io.h \
	src/internal/runtime_proto_sync.h \
	src/engine/runtime_watchdog_internal.h \
	src/io/runtime_io_api_internal.h \
	src/io/runtime_io_watch_darwin_internal.h \
	src/io/runtime_io_watch_linux_internal.h

RUNTIME_COMMON_OBJS = \
	$(OBJDIR)/src/core/runtime.o \
	$(OBJDIR)/src/core/runtime_util.o \
	$(OBJDIR)/src/core/runtime_io_udata.o \
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
	$(OBJDIR)/src/core/runtime_io_object_alloc.o \
	$(OBJDIR)/src/core/runtime_wait_timer_alloc.o \
	$(OBJDIR)/src/core/runtime_trace.o \
	$(OBJDIR)/src/core/runtime_wake.o \
	$(OBJDIR)/src/core/runtime_platform.o \
	$(OBJDIR)/src/core/runtime_safepoint.o \
	$(OBJDIR)/src/core/runtime_wait.o \
	$(OBJDIR)/src/core/runtime_task_stack.o \
	$(OBJDIR)/src/core/runtime_reinject.o \
	$(OBJDIR)/src/core/runtime_wait_tracking.o \
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
	$(OBJDIR)/src/core/runtime_llam_api.o \
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
	$(OBJDIR)/src/core/runtime_mutex.o \
	$(OBJDIR)/src/core/runtime_cond.o \
	$(OBJDIR)/src/core/runtime_channel_cache.o \
	$(OBJDIR)/src/core/runtime_channel.o \
	$(OBJDIR)/src/io/runtime_io_api.o \
	$(OBJDIR)/src/io/runtime_io_api_direct.o \
	$(OBJDIR)/src/io/runtime_io_api_direct_tuning.o \
	$(OBJDIR)/src/io/runtime_io_api_issue.o \
	$(OBJDIR)/src/io/runtime_io_api_blocking_ops.o \
	$(OBJDIR)/src/io/runtime_io_api_public.o \
	$(OBJDIR)/src/core/runtime_debug.o \
	$(OBJDIR)/src/io/runtime_io_watch.o

ifeq ($(HOST_PLATFORM),linux)
LDLIBS += -lm
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
RUNTIME_OBJS += \
	$(OBJDIR)/src/io/runtime_io_watch_linux_prelude.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_state.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_lookup.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_migration_live.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_migration_rehome.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_control.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_submit.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_cqe.o \
	$(OBJDIR)/src/io/runtime_io_watch_linux_worker.o
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
	$(OBJDIR)/src/io/runtime_io_watch_darwin_state.o \
	$(OBJDIR)/src/io/runtime_io_watch_darwin_migration_live.o \
	$(OBJDIR)/src/io/runtime_io_watch_darwin_migration_rehome.o \
	$(OBJDIR)/src/io/runtime_io_watch_darwin_control.o \
	$(OBJDIR)/src/io/runtime_io_watch_darwin_completion.o \
	$(OBJDIR)/src/io/runtime_io_watch_darwin_events.o \
	$(OBJDIR)/src/io/runtime_io_watch_darwin_worker.o
ifeq ($(UNAME_M),arm64)
RUNTIME_OBJS += $(OBJDIR)/src/core/runtime_context_arm64.o
RUNTIME_OBJS += $(OBJDIR)/src/asm/darwin/arm64/context_arm64.o
endif
else ifeq ($(HOST_PLATFORM),windows)
RUNTIME_OBJS =
LDLIBS := $(filter-out -pthread -luring,$(LDLIBS)) -lws2_32
CPPFLAGS += -D_WIN32_WINNT=0x0A00
else
RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)
LDLIBS := $(filter-out -luring,$(LDLIBS))
endif
DEMO_OBJS = \
	$(OBJDIR)/examples/demo.o \
	$(OBJDIR)/examples/demo_tasks.o \
	$(OBJDIR)/examples/demo_entry.o
STRESS_OBJS = \
	$(OBJDIR)/examples/stress.o \
	$(OBJDIR)/examples/stress_support.o \
	$(OBJDIR)/examples/stress_tasks.o \
	$(OBJDIR)/examples/stress_core_cases.o \
	$(OBJDIR)/examples/stress_timeout_cases.o \
	$(OBJDIR)/examples/stress_dynamic_cases.o \
	$(OBJDIR)/examples/stress_suite.o \
	$(OBJDIR)/examples/stress_entry.o
BENCH_OBJS = \
	$(OBJDIR)/examples/bench.o \
	$(OBJDIR)/examples/bench_support.o \
	$(OBJDIR)/examples/bench_entry.o
RUNTIME_ENGINE_FRAGMENTS = $(wildcard src/engine/detail/*.inc)
EXAMPLE_SHARED_HDRS = examples/env_compat.h

.PHONY: all clean bench-matrix verify-darwin verify-linux verify-windows platform-status windows-unsupported

ifeq ($(HOST_PLATFORM),windows)

all demo stress bench bench-matrix verify-darwin verify-linux: windows-unsupported

platform-status:
	@echo "host platform: windows"
	@echo "Native Windows 10/11 backend is scaffolded at the API/build boundary but not complete."
	@echo "Use WSL/Linux for the Linux backend today, or implement the planned IOCP/Fiber backend under a Windows platform layer."

windows-unsupported: platform-status
	@exit 2

clean:
	rm -rf $(CLEAN_DIRS)
	rm -f $(CLEAN_FILES)

verify-windows:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_windows.ps1

else

all: demo stress bench

demo: $(RUNTIME_OBJS) $(DEMO_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(DEMO_OBJS) $(LDLIBS)

stress: $(RUNTIME_OBJS) $(STRESS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(STRESS_OBJS) $(LDLIBS)

bench: $(RUNTIME_OBJS) $(BENCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(BENCH_OBJS) $(LDLIBS)

$(OBJDIR)/src/core/%.o: src/core/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/src/engine/%.o: src/engine/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/src/io/%.o: src/io/%.c $(RUNTIME_PRIV_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/src/engine/runtime_watchdog.o: $(RUNTIME_ENGINE_FRAGMENTS)

$(OBJDIR)/src/asm/linux/x86_64/%.o: src/asm/linux/x86_64/%.S src/internal/nm_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/linux/arm64/%.o: src/asm/linux/arm64/%.S src/internal/nm_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/src/asm/darwin/arm64/%.o: src/asm/darwin/arm64/%.S src/internal/nm_internal.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo.o: examples/demo.c include/llam/runtime.h include/llam/nm_runtime.h examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo_tasks.o: examples/demo_tasks.c include/llam/runtime.h include/llam/nm_runtime.h examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo_entry.o: examples/demo_entry.c include/llam/runtime.h include/llam/nm_runtime.h examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress.o: examples/stress.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_support.o: examples/stress_support.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_tasks.o: examples/stress_tasks.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_core_cases.o: examples/stress_core_cases.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_timeout_cases.o: examples/stress_timeout_cases.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_dynamic_cases.o: examples/stress_dynamic_cases.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_suite.o: examples/stress_suite.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_entry.o: examples/stress_entry.c include/llam/runtime.h include/llam/nm_runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench.o: examples/bench.c include/llam/runtime.h include/llam/nm_runtime.h examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench_support.o: examples/bench_support.c include/llam/runtime.h include/llam/nm_runtime.h examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench_entry.o: examples/bench_entry.c include/llam/runtime.h include/llam/nm_runtime.h examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(CLEAN_DIRS)
	rm -f $(CLEAN_FILES)
	find src examples -name '*.o' -delete

bench-matrix: bench
	python3 scripts/bench_matrix.py

verify-darwin: all
	./scripts/verify_darwin.sh

verify-linux: all
	./scripts/verify_linux.sh

verify-windows:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_windows.ps1

platform-status:
	@echo "host platform: $(HOST_PLATFORM)"

endif
