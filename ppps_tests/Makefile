# SPDX-License-Identifier: GPL-2.0
CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -pthread -static

SRCS = $(wildcard *.c)
BINS = $(SRCS:.c=)

all: $(BINS)

%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(BINS) *.o

.PHONY: all clean
