EXECNAME=LaTe

CC_EMBEDDED=x86_64-openwrt-linux-musl-gcc

SRC_DIR=src
OBJ_DIR=obj

SRC_RAWSOCK_LIB_DIR=Rawsock_lib/Rawsock_lib
OBJ_RAWSOCK_LIB_DIR=Rawsock_lib/Rawsock_lib

SRC_QPID_MODULE_DIR=src/qpid_proton
OBJ_QPID_MODULE_DIR=obj/qpid_proton

SRC=$(wildcard $(SRC_DIR)/*.c)
SRC_RAWSOCK_LIB=$(wildcard $(SRC_RAWSOCK_LIB_DIR)/*.c)
SRC_QPID_MODULE=$(wildcard $(SRC_QPID_MODULE_DIR)/*.c)

OBJ=$(SRC:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
OBJ_RAWSOCK_LIB=$(SRC_RAWSOCK_LIB:$(SRC_RAWSOCK_LIB_DIR)/%.c=$(OBJ_RAWSOCK_LIB_DIR)/%.o)
OBJ_QPID_MODULE=$(SRC_QPID_MODULE:$(SRC_QPID_MODULE_DIR)/%.c=$(OBJ_QPID_MODULE_DIR)/%.o)

OBJ_CC=$(OBJ)
OBJ_CC+=$(OBJ_RAWSOCK_LIB)
OBJ_CC+=$(OBJ_QPID_MODULE)

CFLAGS += -Wall -O2 -Iinclude -IRawsock_lib/Rawsock_lib -DAMQP_1_0_ENABLED -Iinclude/qpid_proton
#LDFLAGS += -Lexternal_lib
LDLIBS += -lpthread -lm -lqpid-proton

.PHONY: all clean

all: compilePC

compilePC: CC = gcc
compileAPU: CC = $(CC_EMBEDDED)
	
compilePCdebug: CFLAGS += -g
compilePCdebug: compilePC

compileAPUdebug: CFLAGS += -g
compileAPUdebug: compileAPU

compilePC compileAPU compilePCdebug compileAPUdebug: $(EXECNAME)

$(EXECNAME): $(OBJ_CC)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@ mkdir -p $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_RAWSOCK_LIB_DIR)/%.o: $(SRC_RAWSOCK_LIB_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_QPID_MODULE_DIR)/%.o: $(SRC_QPID_MODULE_DIR)/%.c
	@ mkdir -p $(OBJ_QPID_MODULE_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJ_DIR)/*.o $(OBJ_RAWSOCK_LIB_DIR)/*.o $(OBJ_QPID_MODULE_DIR)/*.o
	-rm -rf $(OBJ_DIR)

fullclean: clean
	$(RM) $(EXECNAME)