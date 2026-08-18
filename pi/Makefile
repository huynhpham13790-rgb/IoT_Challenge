CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDLIBS += -lmosquitto -lpthread
TARGET := zigbee_reader

.PHONY: all clean run

all: $(TARGET)

$(TARGET): main.c dashboard.c dashboard.h
	$(CC) $(CFLAGS) -o $@ main.c dashboard.c $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
