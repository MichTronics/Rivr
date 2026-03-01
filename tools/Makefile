# tools/Makefile — Build host-side RIVR utilities
#
# Run from the project root:
#   make -C tools
#
# Or from within tools/:
#   make

CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wpedantic \
           -I../firmware_core -I..
LDFLAGS ?=

SRCS_DECODE := rivr_decode.c ../firmware_core/protocol.c
BIN_DECODE  := rivr_decode

.PHONY: all clean

all: $(BIN_DECODE)

$(BIN_DECODE): $(SRCS_DECODE)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

clean:
	rm -f $(BIN_DECODE)
