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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

namespace {
constexpr int InitialCols = 80;
constexpr int InitialRows = 24;
constexpr size_t ScrollbackLines = 5000;
constexpr double InitialFontSize = 11.0;
constexpr double MinFontSize = 6.0;
constexpr double MaxFontSize = 48.0;

Display* dpy = nullptr;
int screenNum = 0;
Window win = 0;
GC gc = 0;
XftDraw* xftDraw = nullptr;
XftFont* font = nullptr;
XftColor bg{};
VTerm* vt = nullptr;
VTermScreen* vts = nullptr;
VTermState* vstate = nullptr;
int masterFd = -1;
pid_t childPid = -1;
bool running = true;
bool cursorVisible = true;
bool altScreen = false;
int cols = InitialCols, rows = InitialRows;
int winW = 0, winH = 0;
int cellW = 9, cellH = 18, ascent = 14;
double fontSize = InitialFontSize;
int scrollOffset = 0;  // lines scrolled back into history
std::deque<std::vector<VTermScreenCell>> history;
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

int screenDamage(VTermRect, void*) { return 1; }

int setTermProp(VTermProp prop, VTermValue* val, void*) {
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        cursorVisible = val->boolean != 0;
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        altScreen = val->boolean != 0;
        scrollOffset = 0;
    }
    return 1;
}

// Called by libvterm when a line scrolls off the top of the screen.
// The cells pointer is only valid during the call, so copy them.
int pushScrollback(int lineCols, const VTermScreenCell* cells, void*) {
    if (lineCols <= 0 || !cells) return 1;
    history.emplace_back(cells, cells + lineCols);
    if (history.size() > ScrollbackLines) history.pop_front();
    // Keep the view anchored while the user is scrolled back.
    if (scrollOffset > 0)
        scrollOffset = std::min<int>(scrollOffset + 1, static_cast<int>(history.size()));
    return 1;
}

// Cell at screen position (r, c), taking the scroll offset into account.
VTermScreenCell cellAt(int r, int c) {
    VTermScreenCell cell{};
    int row = r - scrollOffset;
    if (row >= 0) {
        vterm_screen_get_cell(vts, VTermPos{row, c}, &cell);
    } else {
        size_t idx = static_cast<size_t>(static_cast<int>(history.size()) + row);
        if (idx < history.size() && c < static_cast<int>(history[idx].size()))
            cell = history[idx][static_cast<size_t>(c)];
    }
    return cell;
}

void draw() {
    if (!dpy || !win || !xftDraw || !font) return;
    XSetForeground(dpy, gc, rgb(0, 0, 0));
    XFillRectangle(dpy, win, gc, 0, 0, winW, winH);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            VTermScreenCell cell = cellAt(r, c);
            char ch = ' ';
            if (cell.chars[0] >= 0x20 && cell.chars[0] <= 0x7e)
                ch = static_cast<char>(cell.chars[0]);
            else if (cell.chars[0] != 0)
                ch = '?';  // non-ASCII fallback until UTF-8 rendering is added

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

            // Reverse video (used by vis for its cursor/selection):
            // fill the cell with the text color and draw the glyph in black.
            if (cell.attrs.reverse) {
                XSetForeground(dpy, gc, rgb(rr, gg, bb));
                XFillRectangle(dpy, win, gc, c * cellW, r * cellH, cellW, cellH);
                rr = gg = bb = 0;
            }

            if (ch == ' ') continue;
            XftColor textColor = makeColor(rr, gg, bb);
            XftDrawStringUtf8(xftDraw, &textColor, font, c * cellW,
                              r * cellH + ascent,
                              reinterpret_cast<const FcChar8*>(&ch), 1);
            XftColorFree(dpy, DefaultVisual(dpy, screenNum),
                         DefaultColormap(dpy, screenNum), &textColor);
        }
    }

    VTermPos cursor{};
    vterm_state_get_cursorpos(vstate, &cursor);
    const bool cursorInside = cursor.row >= 0 && cursor.row < rows &&
                              cursor.col >= 0 && cursor.col < cols;
    if (cursorVisible && scrollOffset == 0 && cursorInside) {
        VTermScreenCell cell{};
        vterm_screen_get_cell(vts, cursor, &cell);
        // Skip our block if the app already drew a reverse-video cursor cell.
        if (!cell.attrs.reverse) {
            const int x = cursor.col * cellW, y = cursor.row * cellH;
            XSetForeground(dpy, gc, rgb(220, 220, 220));
            XFillRectangle(dpy, win, gc, x, y, cellW, cellH);
            char ch = (cell.chars[0] >= 0x20 && cell.chars[0] <= 0x7e)
                          ? static_cast<char>(cell.chars[0]) : ' ';
            XftDrawStringUtf8(xftDraw, &bg, font, x, y + ascent,
                              reinterpret_cast<const FcChar8*>(&ch), 1);
        }
    }
    XFlush(dpy);
}

// Load the font at the given size and recompute the cell metrics.
bool loadFont(double size) {
    char name[64];
    std::snprintf(name, sizeof(name), "monospace:size=%g", size);
    XftFont* f = XftFontOpenName(dpy, screenNum, name);
    if (!f) return false;
    if (font) XftFontClose(dpy, font);
    font = f;
    XGlyphInfo ext{};
    XftTextExtentsUtf8(dpy, font, reinterpret_cast<const FcChar8*>("M"), 1, &ext);
    cellW = std::max(1, static_cast<int>(ext.xOff));
    cellH = font->ascent + font->descent + 2;
    ascent = font->ascent + 1;
    return true;
}

// Recompute the character grid from the pixel size and the cell size.
void updateGrid() {
    const int newCols = std::max(1, winW / cellW);
    const int newRows = std::max(1, winH / cellH);
    if (newCols != cols || newRows != rows) {
        cols = newCols;
        rows = newRows;
        vterm_set_size(vt, rows, cols);
    }
    resizePty();
    scrollOffset = std::min<int>(scrollOffset, static_cast<int>(history.size()));
    draw();
}

void changeFontSize(double delta) {
    const double newSize = std::clamp(fontSize + delta, MinFontSize, MaxFontSize);
    if (newSize == fontSize || !loadFont(newSize)) return;
    fontSize = newSize;
    updateGrid();
}

void scrollBy(int delta) {
    if (altScreen) return;
    scrollOffset = std::clamp(scrollOffset + delta, 0, static_cast<int>(history.size()));
    draw();
}

void sendToPty(const char* s, size_t len) {
    if (masterFd < 0) return;
    const ssize_t written = write(masterFd, s, len);
    (void)written;
    if (scrollOffset != 0) {
        scrollOffset = 0;
        draw();
    }
}

void handleKey(XKeyEvent* ev) {
    KeySym sym = NoSymbol;
    char buf[64];
    int n = XLookupString(ev, buf, sizeof(buf), &sym, nullptr);
    const bool ctrl = ev->state & ControlMask;
    const bool shift = ev->state & ShiftMask;

    if (ctrl && shift && sym == XK_Page_Up)   { changeFontSize(+1); return; }
    if (ctrl && shift && sym == XK_Page_Down) { changeFontSize(-1); return; }

    // Scrollback on the primary screen; full-screen apps get the keys instead.
    if (!altScreen && !ctrl) {
        if (sym == XK_Page_Up)   { scrollBy(rows / 2);  return; }
        if (sym == XK_Page_Down) { scrollBy(-rows / 2); return; }
    }

    if (n > 0) {
        sendToPty(buf, static_cast<size_t>(n));
        return;
    }

    const char* seq = nullptr;
    switch (sym) {
        case XK_Up:        seq = "\033[A";  break;
        case XK_Down:      seq = "\033[B";  break;
        case XK_Right:     seq = "\033[C";  break;
        case XK_Left:      seq = "\033[D";  break;
        case XK_Home:      seq = "\033[H";  break;
        case XK_End:       seq = "\033[F";  break;
        case XK_Delete:    seq = "\033[3~"; break;
        case XK_Page_Up:   seq = "\033[5~"; break;
        case XK_Page_Down: seq = "\033[6~"; break;
        default: break;
    }
    if (seq) sendToPty(seq, std::strlen(seq));
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
                     DefaultColormap(dpy, screenNum), &bg);
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

    if (!loadFont(fontSize)) {
        std::fprintf(stderr, "ash: cannot load monospace font\n");
        XCloseDisplay(dpy);
        return 1;
    }
    winW = cols * cellW;
    winH = rows * cellH;

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screenNum), 20, 20, winW, winH, 0,
                              BlackPixel(dpy, screenNum), BlackPixel(dpy, screenNum));
    XStoreName(dpy, win, "ash");
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask);
    wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    gc = XCreateGC(dpy, win, 0, nullptr);
    xftDraw = XftDrawCreate(dpy, win, DefaultVisual(dpy, screenNum),
                            DefaultColormap(dpy, screenNum));
    bg = makeColor(0, 0, 0);

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
    callbacks.settermprop = setTermProp;
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
                case ConfigureNotify:
                    if (ev.xconfigure.width != winW || ev.xconfigure.height != winH) {
                        winW = ev.xconfigure.width;
                        winH = ev.xconfigure.height;
                        updateGrid();
                    }
                    break;
                case ButtonPress:
                    if (ev.xbutton.button == Button4) scrollBy(3);
                    else if (ev.xbutton.button == Button5) scrollBy(-3);
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
