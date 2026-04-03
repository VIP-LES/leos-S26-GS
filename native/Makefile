CC ?= gcc
LEOS_SDK_DIR ?= ../external/leos-sdk
BUILD_DIR ?= build
TARGET := $(BUILD_DIR)/radio_receiver

CFLAGS += -std=c11 -O2 -Wall -Wextra -I$(LEOS_SDK_DIR)/src/leos_sx126x/include -I$(LEOS_SDK_DIR)/src/leos_sx126x -I$(LEOS_SDK_DIR)/external/sx1262/src
CFLAGS += $(shell pkg-config --cflags libgpiod 2>/dev/null)
LDFLAGS += $(shell pkg-config --libs libgpiod 2>/dev/null)

SOURCES := \
	radio_receiver.c \
	$(LEOS_SDK_DIR)/src/leos_sx126x/sx126x.c \
	$(LEOS_SDK_DIR)/src/leos_sx126x/sx126x_port_linux.c \
	$(LEOS_SDK_DIR)/external/sx1262/src/driver_sx1262.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) $(LDFLAGS)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
