// ash.cpp — minimal st-style X11 terminal, C++20
// Build dependencies: Xlib, Xft, Fontconfig, libvterm, libutil
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <fontconfig/fontconfig.h>
#include <vterm.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr int InitialCols = 80;
constexpr int InitialRows = 24;
constexpr int ScrollbackLines = 2000;

struct Cell {
    char bytes[16]{};
    int len = 0;
    VTermScreenCell screen{};
};

Display* dpy = nullptr;
int screenNum = 0;
Window win = 0;
GC gc = 0;
XftDraw* xftDraw = nullptr;
XftFont* font = nullptr;
XftColor fg{}, bg{}, cursorColor{};
VTerm* vt = nullptr;
VTermScreen* vts = nullptr;
VTermState* vstate = nullptr;
int masterFd = -1;
pid_t childPid = -1;
bool running = true, focused = false;
int cols = InitialCols, rows = InitialRows;
int cellW = 9, cellH = 18, ascent = 14;
int scrollOffset = 0;
std::vector<std::string> history;
Atom wmDelete;

unsigned long rgb(unsigned char r, unsigned char g, unsigned char b) {
    return (static_cast<unsigned long>(r) << 16) |
           (static_cast<unsigned long>(g) << 8) | b;
}

XftColor makeColor(unsigned char r, unsigned char g, unsigned char b) {
    XftColor c{};
    XRenderColor xr{static_cast<unsigned short>(r * 257),
                    static_cast<unsigned short>(g * 257),
                    static_cast<unsigned short>(b * 257), 0xffff};
    XftColorAllocValue(dpy, DefaultVisual(dpy, screenNum),
                       DefaultColormap(dpy, screenNum), &xr, &c);
    return c;
}

void resizePty() {
    winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_xpixel = static_cast<unsigned short>(cols * cellW);
    ws.ws_ypixel = static_cast<unsigned short>(rows * cellH);
    if (masterFd >= 0) ioctl(masterFd, TIOCSWINSZ, &ws);
}

int screenDamage(VTermRect, void*)
{
	return 1;
}

int pushScrollback(int lineCols, VTermScreenCell const* cells, void*) {
    if (lineCols <= 0 || !cells) return 1;
    std::string line;
    line.reserve(static_cast<size_t>(lineCols));
    for (int i = 0; i < lineCols; ++i) {
        if (cells[i].chars[0] >= 0x20 && cells[i].chars[0] < 0x7f)
            line.push_back(static_cast<char>(cells[i].chars[0]));
        else
            line.push_back(' ');
    }
    history.push_back(std::move(line));
    if (history.size() > ScrollbackLines)
        history.erase(history.begin(), history.begin() +
                      static_cast<std::ptrdiff_t>(history.size() - ScrollbackLines));
    return 1;
}

void draw() {
    if (!dpy || !win || !xftDraw || !font) return;
    XSetForeground(dpy, gc, rgb(0, 0, 0));
    XFillRectangle(dpy, win, gc, 0, 0, cols * cellW, rows * cellH);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            VTermScreenCell cell{};
            vterm_screen_get_cell(vts, VTermPos{r, c}, &cell);
            char out[16]{};
            int n = 0;
            if (cell.chars[0] >= 0x20 && cell.chars[0] <= 0x7e) {
                out[0] = static_cast<char>(cell.chars[0]);
                n = 1;
            } else if (cell.chars[0] == 0) {
                out[0] = ' ';
                n = 1;
            } else {
                // Basic fallback for non-ASCII until UTF-8 rendering is added.
                out[0] = '?';
                n = 1;
            }

            unsigned char rr = 220, gg = 220, bb = 220;
            if (cell.attrs.bold) rr = gg = bb = 255;
            if (cell.fg.type == VTERM_COLOR_RGB) {
                rr = cell.fg.rgb.red;
                gg = cell.fg.rgb.green;
                bb = cell.fg.rgb.blue;
            } else if (cell.fg.type == VTERM_COLOR_INDEXED) {
                static constexpr unsigned char gray[16] =
                    {20, 45, 65, 85, 105, 125, 145, 165, 185, 195, 205, 215, 225, 235, 245, 255};
                auto idx = std::min<unsigned>(cell.fg.indexed.idx, 15);
                rr = gg = bb = gray[idx];
            }

            XftColor textColor = makeColor(rr, gg, bb);
            XftDrawStringUtf8(xftDraw, &textColor, font, c * cellW,
                              r * cellH + ascent,
                              reinterpret_cast<const FcChar8*>(out), n);
            XftColorFree(dpy, DefaultVisual(dpy, screenNum),
                         DefaultColormap(dpy, screenNum), &textColor);
        }
    }

    VTermPos cursor{};
    vterm_state_get_cursorpos(vstate, &cursor);
    if (scrollOffset == 0 && cursor.row >= 0 && cursor.row < rows &&
        cursor.col >= 0 && cursor.col < cols) {
        const int x = cursor.col * cellW, y = cursor.row * cellH;
        if (focused) {
            // Focused cursor: solid light block, with its character reversed.
            XSetForeground(dpy, gc, rgb(220, 220, 220));
            XFillRectangle(dpy, win, gc, x, y, cellW, cellH);
            VTermScreenCell cell{};
            vterm_screen_get_cell(vts, cursor, &cell);
            char ch = (cell.chars[0] >= 0x20 && cell.chars[0] <= 0x7e)
                          ? static_cast<char>(cell.chars[0]) : ' ';
            XftDrawStringUtf8(xftDraw, &bg, font, x, y + ascent,
                              reinterpret_cast<const FcChar8*>(&ch), 1);
        } else {
            XSetForeground(dpy, gc, rgb(220, 220, 220));
            XDrawRectangle(dpy, win, gc, x, y, cellW - 1, cellH - 1);
        }
    }
    XFlush(dpy);
}

void handleKey(XKeyEvent* ev) {
    KeySym sym = NoSymbol;
    char buf[64];
    int n = XLookupString(ev, buf, sizeof(buf), &sym, nullptr);
    if (sym == XK_Page_Up) {
        scrollOffset = std::min<int>(scrollOffset + rows / 2,
                                     static_cast<int>(history.size()));
        draw();
        return;
    }
    if (sym == XK_Page_Down) {
        scrollOffset = std::max(0, scrollOffset - rows / 2);
        draw();
        return;
    }
    if (n > 0 && masterFd >= 0) {
	const ssize_t written = write(masterFd, buf, static_cast<size_t>(n));
	(void)written;
        scrollOffset = 0;
        return;
    }

    const char* seq = nullptr;
    size_t len = 0;
    switch (sym) {
        case XK_Up: seq = "\033[A"; len = 3; break;
        case XK_Down: seq = "\033[B"; len = 3; break;
        case XK_Right: seq = "\033[C"; len = 3; break;
        case XK_Left: seq = "\033[D"; len = 3; break;
        case XK_Home: seq = "\033[H"; len = 3; break;
        case XK_End: seq = "\033[F"; len = 3; break;
        case XK_Delete: seq = "\033[3~"; len = 4; break;
        default: break;
    }
    if (seq && masterFd >= 0) {
        (void)write(masterFd, seq, len);
        scrollOffset = 0;
    }
}

void cleanup() {
    running = false;
    if (masterFd >= 0) close(masterFd);
    if (childPid > 0) {
        kill(childPid, SIGHUP);
        (void)waitpid(childPid, nullptr, 0);
    }
    if (xftDraw) XftDrawDestroy(xftDraw);
    if (font) XftFontClose(dpy, font);
    if (dpy) {
        XftColorFree(dpy, DefaultVisual(dpy, screenNum),
                     DefaultColormap(dpy, screenNum), &fg);
        XftColorFree(dpy, DefaultVisual(dpy, screenNum),
                     DefaultColormap(dpy, screenNum), &bg);
        XftColorFree(dpy, DefaultVisual(dpy, screenNum),
                     DefaultColormap(dpy, screenNum), &cursorColor);
        if (win) XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
    }
}

} // namespace

int main() {
    dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "ash: cannot open X display\n");
        return 1;
    }
    screenNum = DefaultScreen(dpy);

    font = XftFontOpenName(dpy, screenNum, "monospace:size=11");
    if (!font) {
        std::fprintf(stderr, "ash: cannot load monospace font\n");
        XCloseDisplay(dpy);
        return 1;
    }
    XGlyphInfo ext{};
    XftTextExtentsUtf8(dpy, font, reinterpret_cast<const FcChar8*>("M"), 1, &ext);
    cellW = std::max(1, static_cast<int>(ext.xOff));
    cellH = font->ascent + font->descent + 2;
    ascent = font->ascent + 1;

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screenNum), 20, 20,
                              cols * cellW, rows * cellH, 0,
                              BlackPixel(dpy, screenNum), BlackPixel(dpy, screenNum));
    XStoreName(dpy, win, "ash");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask | FocusChangeMask);
    wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    gc = XCreateGC(dpy, win, 0, nullptr);
    xftDraw = XftDrawCreate(dpy, win, DefaultVisual(dpy, screenNum),
                            DefaultColormap(dpy, screenNum));
    fg = makeColor(220, 220, 220);
    bg = makeColor(0, 0, 0);
    cursorColor = makeColor(220, 220, 220);

    vt = vterm_new(rows, cols);
    if (!vt) {
        std::fprintf(stderr, "ash: vterm_new failed\n");
        cleanup();
        return 1;
    }
    vterm_set_utf8(vt, 1);
    vts = vterm_obtain_screen(vt);
    vstate = vterm_obtain_state(vt);
    vterm_screen_enable_altscreen(vts, 1);
    vterm_screen_set_damage_merge(vts, VTERM_DAMAGE_SCROLL);
    VTermScreenCallbacks callbacks{};
    callbacks.damage = screenDamage;
    callbacks.sb_pushline = pushScrollback;
    vterm_screen_set_callbacks(vts, &callbacks, nullptr);
    vterm_screen_reset(vts, 1);

    winsize ws{};
    ws.ws_row = rows;
    ws.ws_col = cols;
    ws.ws_xpixel = cols * cellW;
    ws.ws_ypixel = rows * cellH;
    int pid = forkpty(&masterFd, nullptr, nullptr, &ws);
    if (pid < 0) {
        std::perror("ash: forkpty");
        cleanup();
        return 1;
    }
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        const char* shell = std::getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        execl(shell, shell, static_cast<char*>(nullptr));
        execl("/bin/sh", "sh", static_cast<char*>(nullptr));
        _exit(127);
    }
    childPid = pid;
    int flags = fcntl(masterFd, F_GETFL, 0);
    if (flags >= 0) fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);

    XMapWindow(dpy, win);
    draw();

    while (running) {
        pollfd pfd{masterFd, POLLIN, 0};
        int pr = poll(&pfd, 1, 15);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            std::array<char, 8192> buf{};
            ssize_t n = read(masterFd, buf.data(), buf.size());
            if (n > 0) {
                vterm_input_write(vt, buf.data(), static_cast<size_t>(n));
                vterm_screen_flush_damage(vts);
                draw();
            }
        }
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case Expose:
                    draw();
                    break;
                case KeyPress:
                    handleKey(&ev.xkey);
                    break;
                case FocusIn:
                    focused = true;
                    draw();
                    break;
                case FocusOut:
                    focused = false;
                    draw();
                    break;
                case ConfigureNotify: {
                    int newCols = std::max(1, ev.xconfigure.width / cellW);
                    int newRows = std::max(1, ev.xconfigure.height / cellH);
                    if (newCols != cols || newRows != rows) {
                        cols = newCols;
                        rows = newRows;
                        vterm_set_size(vt, rows, cols);
                        resizePty();
                        draw();
                    }
                    break;
                }
                case ButtonPress:
                    if (ev.xbutton.button == Button4) {
                        scrollOffset = std::min<int>(scrollOffset + 3, history.size());
                        draw();
                    } else if (ev.xbutton.button == Button5) {
                        scrollOffset = std::max(0, scrollOffset - 3);
                        draw();
                    }
                    break;
                case ClientMessage:
                    if (static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete)
                        running = false;
                    break;
            }
        }
        int status = 0;
        if (waitpid(childPid, &status, WNOHANG) == childPid) {
            childPid = -1;
            running = false;
        }
    }

    cleanup();
    return 0;
}
