TARGET      := solo_stratumd
SRC_DIR     := src
SOURCE      := $(wildcard $(SRC_DIR)/*.c)
OBJS        := $(patsubst $(SRC_DIR)/%.c, $(SRC_DIR)/%.o, $(SOURCE))
DEPS        := $(OBJS:.o=.d)
CC          ?= gcc
CFLAGS      ?= -Wall -Wextra -Wshadow -Wformat-security -Wno-unused-parameter -O2 -g -std=gnu99
CPPFLAGS    ?= -I $(SRC_DIR)
LDFLAGS     ?= -g -Wl,-z,relro,-z,now
LDLIBS      := -lev -lmicrohttpd -lzmq -lcurl -ljansson -lssl -lcrypto -lpthread -lm

.PHONY: all clean deps-clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f $(SRC_DIR)/*.o $(SRC_DIR)/*.d $(TARGET)

deps-clean:
	@true

-include $(DEPS)
