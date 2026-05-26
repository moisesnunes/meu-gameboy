# Makefile — Game Boy CPU unit tests
#
# Usage:
#   make          – build the test binary
#   make run      – build and run all tests
#   make clean    – remove build artefacts

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -g -I. -I..

SRCS    = test_cpu.c gb_mock.c meu-cpu.c
BIN     = test_cpu

.PHONY: all run clean

all: $(BIN)

$(BIN): $(SRCS) gb.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
