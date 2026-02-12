CC = gcc
CFLAGS = -Wall -Wextra -O3 -march=native -mfpu=neon-fp-armv8 -ftree-vectorize
LDFLAGS = -lSDL2 -lSDL2_ttf -lm -ljpeg

SRC_DIR = src
BUILD_DIR = build
BIN = capturedisp

SRCS = src/main.c src/capture.c src/config.c
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN)

install: $(BIN)
	install -m 755 $(BIN) /usr/local/bin/
