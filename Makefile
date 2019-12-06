EXECNAME=LaTe

CC=gcc
CC_EMBEDDED=x86_64-openwrt-linux-musl-gcc

SRC_DIR=src
OBJ_DIR=obj

SRC_RAWSOCK_LIB_DIR=Rawsock_lib/Rawsock_lib
OBJ_RAWSOCK_LIB_DIR=Rawsock_lib/Rawsock_lib

SRC=$(wildcard $(SRC_DIR)/*.c)
SRC_RAWSOCK_LIB=$(wildcard $(SRC_RAWSOCK_LIB_DIR)/*.c)

OBJ=$(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
OBJ_RAWSOCK_LIB=$(SRC_RAWSOCK_LIB:$(SRC_RAWSOCK_LIB_DIR)/%.c=$(OBJ_RAWSOCK_LIB_DIR)/%.o)

OBJ_CC=$(OBJ)
OBJ_CC+=$(OBJ_RAWSOCK_LIB)

CFLAGS += -Wall -O2 -Iinclude -IRawsock_lib/Rawsock_lib
#LDFLAGS += -Lexternal_lib
LDLIBS += -lpthread -lm

.PHONY: all clean

all: $(EXECNAME)

$(EXECNAME): $(OBJ_CC)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_RAWSOCK_LIB_DIR)/%.o: $(SRC_RAWSOCK_LIB_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJ_DIR)/*.o $(OBJ_RAWSOCK_LIB_DIR)/*.o
