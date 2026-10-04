CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O3 -Wall -Wextra -std=c++20
PKGS = x11 xft fontconfig vterm

TARGET=ash

all: ash

ash: ash.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $$( $(PKG_CONFIG) --cflags --libs $(PKGS) ) -lutil

clean:
	rm -f ash

install: $(TARGET)
	install -Dm 755 $(TARGET) /usr/local/bin/$(TARGET)


.PHONY: all clean
