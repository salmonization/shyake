CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -pedantic -Iinclude -D_POSIX_C_SOURCE=200809L -I/opt/homebrew/include -I/opt/homebrew/opt/openssl@3/include
LDFLAGS = -L/opt/homebrew/lib -L/opt/homebrew/opt/openssl@3/lib -loqs -lcurl -lcrypto -lm

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

TARGET = $(BIN_DIR)/shyake

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TARGET)
	@$(CC) $(CFLAGS) tests/test_crypto.c src/crypto.c -o $(BIN_DIR)/test_crypto $(LDFLAGS)
	@$(BIN_DIR)/test_crypto

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
