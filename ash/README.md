# ash

A minimal, `st`-inspired X11 terminal emulator in C++20.

## Gentoo dependencies

```sh
emerge --ask sys-devel/gcc x11-libs/libX11 x11-libs/libXft   media-libs/fontconfig app-misc/libvterm
```

Also install `pkg-config` if it is not already available.

## Build and run

```sh
make
./ash
```

## Current scope

- X11 window with minimal black background and light-gray monospace text
- PTY-backed login shell (uses `$SHELL`, falls back to `/bin/sh`)
- libvterm for VT parsing and alternate-screen support
- basic keyboard input, resize propagation, and close handling
- starter scrollback storage callback

## Known limitations

This is a starter, not a finished `st` replacement. Scrollback storage is not yet wired into drawing, UTF-8 glyph rendering is currently a basic ASCII fallback, and mouse reporting / selection / clipboard / IME support are not implemented. `TERM=xterm-256color` is set, so compatibility should be tested; libvterm covers many common VT sequences, but this implementation still needs testing on your machine.
