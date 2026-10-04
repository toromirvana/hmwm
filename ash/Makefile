CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2 -Wall -Wextra -std=c++20
PKGS = x11 xft fontconfig vterm

all: ash

ash: ash.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $$( $(PKG_CONFIG) --cflags --libs $(PKGS) ) -lutil

clean:
	rm -f ash

.PHONY: all clean
