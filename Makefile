CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE
SRC     := src/node.c src/config.c src/net.c src/routing.c src/tcp_newreno.c
HDR     := src/common.h src/config.h src/net.h src/routing.h src/tcp_newreno.h
BIN     := node

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

clean:
	rm -f $(BIN)

.PHONY: all clean
