CC ?= gcc
CFLAGS = -O3 -Wall -Wextra
LDLIBS = -lX11

TARGET = hmwm
SRC = hmwm.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
