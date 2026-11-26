CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Werror
CFLAGS += -MMD -MP
CPPFLAGS += -Iinclude
LDFLAGS ?=
LDLIBS ?=

BUILD := build
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean test demo

all: $(BUILD)/wake_demo $(BUILD)/tt_tests

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/tt_tests: tests/test_all.c $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD)/wake_demo: examples/demo_main.c $(OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

test: $(BUILD)/tt_tests
	./$(BUILD)/tt_tests

demo: $(BUILD)/wake_demo
	./$(BUILD)/wake_demo

clean:
	rm -rf $(BUILD)

-include $(DEPS)
