CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -g -fno-omit-frame-pointer
CPPFLAGS ?= -Iinclude -Isrc/internal -Isrc -D_GNU_SOURCE
LDLIBS ?= -pthread -luring
OBJDIR ?= object
SHARED_OBJDIR ?= $(OBJDIR)-pic
PICFLAGS ?= -fPIC
LLAM_ABI_MAJOR ?= 1
LLAM_VERSION ?= 1.0.1
CLEAN_DIRS = \
	$(OBJDIR) \
	$(SHARED_OBJDIR) \
	build \
	CMakeFiles \
	cmake-build-* \
	scripts/bench_tokio_compare/target \
	build-* \
	target
CLEAN_FILES = \
	demo \
	stress \
	bench \
	server \
	server_flood \
	test_abi_contract \
	test_connect_io \
	test_runtime_core \
	test_sync_primitives \
	test_io_buffers \
	test_windows_policy \
	test_shared_load \
	libllam_runtime.a \
	demo.exe \
	stress.exe \
	bench.exe \
	server.exe \
	server_flood.exe \
	test_abi_contract.exe \
	test_connect_io.exe \
	test_runtime_core.exe \
	test_sync_primitives.exe \
	test_io_buffers.exe \
	test_windows_policy.exe \
	test_shared_load.exe \
	libllam_runtime.dylib \
	libllam_runtime.$(LLAM_ABI_MAJOR).dylib \
	libllam_runtime.so \
	libllam_runtime.so.$(LLAM_ABI_MAJOR) \
	libllam_runtime.so.$(LLAM_VERSION) \
	CMakeCache.txt \
	cmake_install.cmake \
	compile_commands.json \
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
else
HOST_PLATFORM := posix
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

RUNTIME_PRIV_HDRS = \
	include/llam/platform.h \
	include/llam/runtime.h \
	src/internal/runtime_platform.h \
	src/internal/runtime_windows_compat.h \
	src/internal/runtime_windows.h \
	src/internal/runtime_windows_iocp.h \
	src/internal/llam_internal.h \
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
	src/io/darwin/runtime_io_watch_darwin_internal.h \
	src/io/linux/runtime_io_watch_linux_internal.h \
	src/io/windows/runtime_io_watch_windows_internal.h

RUNTIME_COMMON_OBJS = \
	$(OBJDIR)/src/core/runtime.o \
	$(OBJDIR)/src/core/runtime_abi.o \
	$(OBJDIR)/src/core/runtime_errno.o \
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
	$(OBJDIR)/src/core/runtime_windows.o \
	$(OBJDIR)/src/core/runtime_safepoint.o \
	$(OBJDIR)/src/core/runtime_wait.o \
	$(OBJDIR)/src/core/runtime_task_reclaim.o \
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
	$(OBJDIR)/src/core/runtime_channel_select.o \
	$(OBJDIR)/src/core/runtime_handle.o \
	$(OBJDIR)/src/core/runtime_task_group.o \
	$(OBJDIR)/src/core/runtime_task_local.o \
	$(OBJDIR)/src/io/runtime_io_api.o \
	$(OBJDIR)/src/io/runtime_io_api_direct.o \
	$(OBJDIR)/src/io/runtime_io_api_direct_tuning.o \
	$(OBJDIR)/src/io/runtime_io_api_issue.o \
	$(OBJDIR)/src/io/runtime_io_api_blocking_ops.o \
	$(OBJDIR)/src/io/runtime_io_api_public.o \
	$(OBJDIR)/src/io/windows/runtime_windows_iocp.o \
	$(OBJDIR)/src/core/runtime_debug.o \
	$(OBJDIR)/src/io/runtime_io_watch.o

ifeq ($(HOST_PLATFORM),linux)
LDLIBS += -lm
LLAM_HAVE_IO_URING_BUF_RING_HELPERS := $(shell printf '#include <liburing.h>\nint main(void) { void *p = (void *)io_uring_setup_buf_ring; return p == 0; }\n' | $(CC) $(CPPFLAGS) $(CFLAGS) -x c - -luring -o /tmp/llam-uring-check-$$$$ >/dev/null 2>&1 && echo 1 || echo 0)
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
LDLIBS := $(filter-out -pthread -luring,$(LDLIBS)) -lws2_32 -lmswsock
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
endif
SHARED_RUNTIME_OBJS = $(patsubst $(OBJDIR)/%,$(SHARED_OBJDIR)/%,$(RUNTIME_OBJS))
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
SERVER_OBJS = \
	$(OBJDIR)/examples/server.o
SERVER_FLOOD_OBJS = \
	$(OBJDIR)/examples/server_flood.o
TEST_ABI_OBJS = \
	$(OBJDIR)/tests/test_abi_contract.o
TEST_CONNECT_OBJS = \
	$(OBJDIR)/tests/test_connect_io.o
TEST_RUNTIME_CORE_OBJS = \
	$(OBJDIR)/tests/test_runtime_core.o
TEST_SYNC_OBJS = \
	$(OBJDIR)/tests/test_sync_primitives.o
TEST_IO_BUFFERS_OBJS = \
	$(OBJDIR)/tests/test_io_buffers.o
TEST_WINDOWS_POLICY_OBJS = \
	$(OBJDIR)/tests/test_windows_policy.o
TEST_SHARED_LOAD_OBJS = \
	$(OBJDIR)/tests/test_shared_load.o
RUNTIME_ENGINE_FRAGMENTS = $(wildcard src/engine/detail/*.inc)
EXAMPLE_SHARED_HDRS = examples/env_compat.h

.PHONY: all clean static shared test check package bench-matrix server-stress server-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux verify-windows platform-status windows-unsupported

ifeq ($(HOST_PLATFORM),windows)

all demo stress bench server server_flood static shared test check package bench-matrix server-stress server-flood server-stress-composite server-stress-composite-quick server-stress-composite-hour verify-darwin verify-linux: windows-unsupported

platform-status:
	@echo "host platform: windows"
	@echo "Native Windows 10/11 backend is staged: scheduler/core builds through CMake with x86_64 asm context switching."
	@echo "Use CMake for native Windows builds; Makefile Windows host targets remain guarded until full IOCP request support lands."

windows-unsupported: platform-status
	@exit 2

clean:
	rm -rf $(CLEAN_DIRS)
	rm -f $(CLEAN_FILES)

verify-windows:
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_windows.ps1

else

all: demo stress bench server server_flood static shared

static: libllam_runtime.a

libllam_runtime.a: $(RUNTIME_OBJS)
	$(AR) rcs $@ $(RUNTIME_OBJS)

shared: $(SHLIB_LINK)

test: test_abi_contract test_connect_io test_runtime_core test_sync_primitives test_io_buffers test_windows_policy test_shared_load shared
	./test_abi_contract
	./test_connect_io
	./test_runtime_core
	./test_sync_primitives
	./test_io_buffers
	./test_windows_policy
	./test_shared_load ./$(SHLIB_REAL)

check: test

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

server: $(RUNTIME_OBJS) $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(SERVER_OBJS) $(LDLIBS)

server_flood: $(SERVER_FLOOD_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_FLOOD_OBJS) -pthread

test_abi_contract: $(RUNTIME_OBJS) $(TEST_ABI_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_ABI_OBJS) $(LDLIBS)

test_connect_io: $(RUNTIME_OBJS) $(TEST_CONNECT_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_CONNECT_OBJS) $(LDLIBS)

test_runtime_core: $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_RUNTIME_CORE_OBJS) $(LDLIBS)

test_sync_primitives: $(RUNTIME_OBJS) $(TEST_SYNC_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_SYNC_OBJS) $(LDLIBS)

test_io_buffers: $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_IO_BUFFERS_OBJS) $(LDLIBS)

test_windows_policy: $(RUNTIME_OBJS) $(TEST_WINDOWS_POLICY_OBJS)
	$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) $(TEST_WINDOWS_POLICY_OBJS) $(LDLIBS)

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

$(OBJDIR)/examples/demo.o: examples/demo.c include/llam/runtime.h examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo_tasks.o: examples/demo_tasks.c include/llam/runtime.h examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/demo_entry.o: examples/demo_entry.c include/llam/runtime.h examples/demo_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress.o: examples/stress.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_support.o: examples/stress_support.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_tasks.o: examples/stress_tasks.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_core_cases.o: examples/stress_core_cases.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_timeout_cases.o: examples/stress_timeout_cases.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_dynamic_cases.o: examples/stress_dynamic_cases.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_suite.o: examples/stress_suite.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/stress_entry.o: examples/stress_entry.c include/llam/runtime.h examples/stress_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench.o: examples/bench.c include/llam/runtime.h examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench_support.o: examples/bench_support.c include/llam/runtime.h examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/bench_entry.o: examples/bench_entry.c include/llam/runtime.h examples/bench_internal.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server.o: examples/server.c include/llam/runtime.h $(EXAMPLE_SHARED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/examples/server_flood.o: examples/server_flood.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/tests/%.o: tests/%.c include/llam/runtime.h
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
	powershell -NoProfile -ExecutionPolicy Bypass -File scripts/verify_windows.ps1

platform-status:
	@echo "host platform: $(HOST_PLATFORM)"

endif
