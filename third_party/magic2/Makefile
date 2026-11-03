CC ?= cc
CXX ?= c++
MINGW_CC ?= x86_64-w64-mingw32-gcc

BUILD_DIR ?= build
SANITIZE_DIR ?= $(BUILD_DIR)/sanitize
MINGW_DIR ?= $(BUILD_DIR)/mingw

CPPFLAGS ?= -I.
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
THREAD_FLAGS ?= -pthread
TEST_WARNINGS ?= -Wno-unused-function
SANITIZE_FLAGS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZER_ENV ?= ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1

GRAPH_TEST_SOURCES := \
	tests/test_cpu_runtime.c \
	tests/test_parallel_graph.c \
	tests/test_random_graph.c \
	tests/test_security_lifetimes.c \
	tests/test_parallel_fairness.c \
	tests/test_ordinary_graph.c \
	tests/test_graph_async_overlap.c \
	tests/test_metadata_aliases.c \
	tests/test_isolation_v2.c

C_TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%,$(GRAPH_TEST_SOURCES))
CXX_TEST_BINS := $(patsubst tests/%.c,$(BUILD_DIR)/%-cxx,$(GRAPH_TEST_SOURCES))
EXAMPLE_BIN := $(BUILD_DIR)/example
IMPLEMENTATION_OBJ := $(BUILD_DIR)/magic2_impl.o
PUBLIC_CLIENT_C := $(BUILD_DIR)/public-client-c
PUBLIC_CLIENT_CXX := $(BUILD_DIR)/public-client-cxx

.PHONY: all example test test-cxx check sanitize mingw clean

all: $(EXAMPLE_BIN) $(C_TEST_BINS) $(CXX_TEST_BINS) \
	$(PUBLIC_CLIENT_C) $(PUBLIC_CLIENT_CXX)

example: $(EXAMPLE_BIN)
	@$(EXAMPLE_BIN)

test: $(C_TEST_BINS) $(PUBLIC_CLIENT_C)
	@set -e; for binary in $(C_TEST_BINS) $(PUBLIC_CLIENT_C); do "$$binary"; done

test-cxx: $(CXX_TEST_BINS) $(PUBLIC_CLIENT_CXX)
	@set -e; for binary in $(CXX_TEST_BINS) $(PUBLIC_CLIENT_CXX); do "$$binary"; done

check: test test-cxx

$(BUILD_DIR):
	@mkdir -p $@

$(SANITIZE_DIR):
	@mkdir -p $@

$(MINGW_DIR):
	@mkdir -p $@

$(EXAMPLE_BIN): examples/example.c magic2.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%: tests/%.c magic2.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%-cxx: tests/%.c magic2.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) -x c++ $< -o $@ $(LDFLAGS) $(LDLIBS)

$(IMPLEMENTATION_OBJ): magic2_impl.c magic2.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) -c $< -o $@

$(PUBLIC_CLIENT_C): tests/test_public_client.c $(IMPLEMENTATION_OBJ) magic2.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $< $(IMPLEMENTATION_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(PUBLIC_CLIENT_CXX): tests/test_public_client.c $(IMPLEMENTATION_OBJ) magic2.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) \
		-x c++ $< -x none $(IMPLEMENTATION_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

sanitize: | $(SANITIZE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $(SANITIZE_FLAGS) \
		examples/example.c -o $(SANITIZE_DIR)/example $(LDFLAGS) $(LDLIBS)
	@set -e; \
	for source in $(GRAPH_TEST_SOURCES); do \
		name=$$(basename "$$source" .c); \
		$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $(SANITIZE_FLAGS) \
			"$$source" -o "$(SANITIZE_DIR)/$$name" $(LDFLAGS) $(LDLIBS); \
		$(SANITIZER_ENV) \
			"$(SANITIZE_DIR)/$$name"; \
	done
	$(CC) $(CPPFLAGS) $(CFLAGS) $(THREAD_FLAGS) $(SANITIZE_FLAGS) \
		-c magic2_impl.c -o $(SANITIZE_DIR)/magic2_impl.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $(SANITIZE_FLAGS) \
		tests/test_public_client.c $(SANITIZE_DIR)/magic2_impl.o \
		-o $(SANITIZE_DIR)/public-client-c $(LDFLAGS) $(LDLIBS)
	$(SANITIZER_ENV) \
		$(SANITIZE_DIR)/public-client-c
	@set -e; \
	for source in $(GRAPH_TEST_SOURCES); do \
		name=$$(basename "$$source" .c)-cxx; \
		$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $(SANITIZE_FLAGS) \
			-x c++ "$$source" -o "$(SANITIZE_DIR)/$$name" $(LDFLAGS) $(LDLIBS); \
		$(SANITIZER_ENV) \
			"$(SANITIZE_DIR)/$$name"; \
	done
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(TEST_WARNINGS) $(THREAD_FLAGS) $(SANITIZE_FLAGS) \
		-x c++ tests/test_public_client.c -x none $(SANITIZE_DIR)/magic2_impl.o \
		-o $(SANITIZE_DIR)/public-client-cxx $(LDFLAGS) $(LDLIBS)
	$(SANITIZER_ENV) \
		$(SANITIZE_DIR)/public-client-cxx

mingw: | $(MINGW_DIR)
	$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
		examples/example.c -static -static-libgcc -o $(MINGW_DIR)/example.exe
	@set -e; \
	for source in $(GRAPH_TEST_SOURCES); do \
		name=$$(basename "$$source" .c); \
		$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
			"$$source" -static -static-libgcc -o "$(MINGW_DIR)/$$name.exe"; \
	done
	$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
		-c magic2_impl.c -o $(MINGW_DIR)/magic2_impl.o
	$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
		tests/test_public_client.c $(MINGW_DIR)/magic2_impl.o \
		-static -static-libgcc -o $(MINGW_DIR)/public-client.exe

clean:
	$(RM) -r $(BUILD_DIR)
