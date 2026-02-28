FUSE_CFLAGS  := $(shell pkg-config --cflags fuse3)
FUSE_LIBS    := $(shell pkg-config --libs   fuse3)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -g -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=35 \
          $(FUSE_CFLAGS) -MMD -MP
LDFLAGS = $(FUSE_LIBS) -lpthread -lisal

# Main sources (everything except the auto-generated version.c)
SRC_SRCS = src/main.c src/config.c src/state.c src/alloc.c \
           src/metadata.c src/fuse_ops.c src/parity.c src/journal.c \
           src/lr_hash.c src/lr_list.c src/rebuild.c src/ctrl.c
OBJS = $(SRC_SRCS:.c=.o)
DEPS = $(OBJS:.o=.d) src/version.d

all: liveraid

liveraid: $(OBJS) src/version.o
	$(CC) -o $@ $^ $(LDFLAGS)

# Auto-generate version.c whenever any of the main objects are rebuilt.
# The recipe increments build_number.txt then writes the new version string.
src/version.c: $(OBJS)
	@n=$$(( $$(cat build_number.txt 2>/dev/null || echo 0) + 1 )); \
	 echo $$n > build_number.txt; \
	 printf 'const char lr_version[] = "v0.01.%04d";\n' $$n > $@; \
	 echo "  version  v0.01.$$(printf '%04d' $$n)"

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) src/version.o $(DEPS) src/version.c liveraid

-include $(DEPS)

.PHONY: all clean
