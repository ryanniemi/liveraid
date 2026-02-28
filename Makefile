FUSE_CFLAGS  := $(shell pkg-config --cflags fuse3)
FUSE_LIBS    := $(shell pkg-config --libs   fuse3)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -g -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=35 \
          $(FUSE_CFLAGS) -MMD -MP
LDFLAGS = $(FUSE_LIBS) -lpthread -lisal

SRC_SRCS = src/main.c src/config.c src/state.c src/alloc.c \
           src/metadata.c src/fuse_ops.c src/parity.c src/journal.c \
           src/lr_hash.c src/lr_list.c src/rebuild.c src/ctrl.c
OBJS = $(SRC_SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

all: liveraid

liveraid: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(DEPS) liveraid

-include $(DEPS)

.PHONY: all clean
