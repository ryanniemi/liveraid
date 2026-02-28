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
	rm -f $(OBJS) src/version.o $(DEPS) src/version.c liveraid

-include $(DEPS)

.PHONY: all clean
