CC ?= cc
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g \
          -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror \
          -Iinclude
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR := build
CORE_SRCS := $(wildcard src/*.c)
CORE_OBJS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))

TEST_BIN := $(BUILD_DIR)/test_wake_agent
DEMO_BIN := $(BUILD_DIR)/host_demo
HERD_BIN := $(BUILD_DIR)/herd_sim

.PHONY: all test demo herd clean

all: test demo herd

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_BIN): tests/test_wake_agent.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(CORE_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_BIN)
	$(TEST_BIN)

$(DEMO_BIN): examples/host_demo.c $(CORE_OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< $(CORE_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

demo: $(DEMO_BIN)
	$(DEMO_BIN)

$(HERD_BIN): examples/herd_sim.c src/wake_policy.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< src/wake_policy.c -o $@ $(LDFLAGS) $(LDLIBS)

herd: $(HERD_BIN)
	$(HERD_BIN)

clean:
	rm -rf $(BUILD_DIR)
