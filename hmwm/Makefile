C = gcc
CFLAGS = -O3 -Wall -Wextra
LDFLAGS = -lX11

TARGET = hmwm
SRC = hmwm.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
