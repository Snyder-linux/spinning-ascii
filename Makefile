# Builds every spinner: each */name.c becomes */name.
#
#   make            build everything
#   make hat3d      build one spinner by name (hat3d, station, spin, ...)
#   make clean      remove the binaries

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS  := -lm

SOURCES  := $(wildcard */*.c)
BINARIES := $(SOURCES:.c=)
NAMES    := $(notdir $(BINARIES))

all: $(BINARIES)

# the brain renders on every core
brain/brain3d: LDLIBS += -pthread

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# `make hat3d` builds just that spinner
$(NAMES):
	@$(MAKE) --no-print-directory $(filter %/$@,$(BINARIES))

clean:
	rm -f $(BINARIES)

.PHONY: all clean $(NAMES)
