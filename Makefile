FUSE_CFLAGS  := $(shell pkg-config --cflags fuse3)
FUSE_LIBS    := $(shell pkg-config --libs   fuse3)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -g -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=35 \
          $(FUSE_CFLAGS) -MMD -MP
LDFLAGS = $(FUSE_LIBS) -lpthread -lisal

BUILD_NUM := $(shell git rev-list --count HEAD 2>/dev/null || echo 0)
VERSION   := v0.01.$(shell printf '%04d' $(BUILD_NUM))

# Main sources (everything except the auto-generated version.c)
SRC_SRCS = src/main.c src/config.c src/state.c src/alloc.c \
           src/metadata.c src/fuse_ops.c src/parity.c src/journal.c \
           src/lr_hash.c src/lr_list.c src/rebuild.c src/ctrl.c
OBJS = $(SRC_SRCS:.c=.o)
DEPS = $(OBJS:.o=.d) src/version.d

all: liveraid

liveraid: $(OBJS) src/version.o
	$(CC) -o $@ $^ $(LDFLAGS)

# Regenerate version.c whenever the commit count changes or objects are rebuilt.
src/version.c: $(OBJS)
	@printf 'const char lr_version[] = "%s";\n' $(VERSION) > $@
	@echo "  version  $(VERSION)"

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) src/version.o $(DEPS) src/version.c liveraid $(TEST_BINS)

-include $(DEPS)

# ------------------------------------------------------------------ #
# Tests                                                               #
# ------------------------------------------------------------------ #

TEST_CFLAGS  = -O2 -Wall -Wextra -g -D_FILE_OFFSET_BITS=64 -Isrc
TEST_LDFLAGS = -lpthread

TEST_BINS = tests/test_alloc tests/test_hash tests/test_list \
            tests/test_state tests/test_metadata tests/test_config

tests/test_alloc: tests/test_alloc.c src/alloc.c
	$(CC) $(TEST_CFLAGS) -o $@ $^

tests/test_hash: tests/test_hash.c src/lr_hash.c
	$(CC) $(TEST_CFLAGS) -o $@ $^

tests/test_list: tests/test_list.c src/lr_list.c
	$(CC) $(TEST_CFLAGS) -o $@ $^

tests/test_state: tests/test_state.c src/state.c src/alloc.c src/lr_hash.c src/lr_list.c
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

tests/test_metadata: tests/test_metadata.c src/metadata.c src/state.c src/alloc.c src/lr_hash.c src/lr_list.c
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

tests/test_config: tests/test_config.c src/config.c
	$(CC) $(TEST_CFLAGS) -o $@ $^

test: $(TEST_BINS)
	@rc=0; for t in $(TEST_BINS); do $$t || rc=1; done; exit $$rc

.PHONY: all clean test
