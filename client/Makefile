CC ?= gcc

# detect OS and architecture
OS   := $(shell uname -s)
ARCH := $(shell uname -m)

# resolve prefix for headers / libraries
ifeq ($(OS), Darwin)
    ifeq ($(ARCH), arm64)
        BREW_PREFIX := /opt/homebrew
    else
        BREW_PREFIX := /usr/local
    endif
    SYS_INC  := $(BREW_PREFIX)/include \
                $(BREW_PREFIX)/opt/openssl@3/include
    SYS_LIB  := $(BREW_PREFIX)/lib \
                $(BREW_PREFIX)/opt/openssl@3/lib
    SHARED_EXT := dylib
else ifeq ($(OS), Linux)
    SYS_INC  := /usr/include \
                /usr/local/include
    SYS_LIB  := /usr/lib \
                /usr/local/lib
    SHARED_EXT := so
else
    $(error Unsupported OS: $(OS))
endif

CFLAGS = -Wall -Wextra -std=c11 -pedantic \
         -D_POSIX_C_SOURCE=200809L -fPIC \
         -Iinclude \
         -Isrc/lib/vendor/cJSON \
         $(addprefix -I, $(SYS_INC))

LDFLAGS = $(addprefix -L, $(SYS_LIB)) \
          -loqs -lcurl -lcrypto -lm

LIB_DIR     = src/lib
CLI_DIR     = src/cli
OBJ_DIR     = obj
BIN_DIR     = bin
LIB_OUT_DIR = lib

LIB_SRCS = $(wildcard $(LIB_DIR)/*.c) \
           $(wildcard $(LIB_DIR)/vendor/cJSON/*.c)
LIB_OBJS = $(patsubst $(LIB_DIR)/%.c, \
               $(OBJ_DIR)/lib/%.o, $(LIB_SRCS))

CLI_SRCS = $(wildcard $(CLI_DIR)/*.c)
CLI_OBJS = $(patsubst $(CLI_DIR)/%.c, \
               $(OBJ_DIR)/cli/%.o, $(CLI_SRCS))

STATIC_LIB = $(LIB_OUT_DIR)/libshyake.a
SHARED_LIB = $(LIB_OUT_DIR)/libshyake.$(SHARED_EXT)
CLI_TARGET = $(BIN_DIR)/shyake

.PHONY: all clean test

all: $(STATIC_LIB) $(SHARED_LIB) $(CLI_TARGET)

$(STATIC_LIB): $(LIB_OBJS)
	@mkdir -p $(LIB_OUT_DIR)
	ar rcs $@ $(LIB_OBJS)

$(SHARED_LIB): $(LIB_OBJS)
	@mkdir -p $(LIB_OUT_DIR)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

$(CLI_TARGET): $(CLI_OBJS) $(STATIC_LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(CLI_OBJS) $(STATIC_LIB) -o $@ $(LDFLAGS)

$(OBJ_DIR)/lib/%.o: $(LIB_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/cli/%.o: $(CLI_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

test: $(STATIC_LIB)
	@mkdir -p $(BIN_DIR)
	@$(CC) $(CFLAGS) tests/test_crypto.c $(STATIC_LIB) \
		-o $(BIN_DIR)/test_crypto $(LDFLAGS)
	@$(BIN_DIR)/test_crypto

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) $(LIB_OUT_DIR)
