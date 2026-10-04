#include <stddef.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "config.h"

#define NWS 3
#define MAXW 100

static Display *dpy;
static Window root, wins[NWS][MAXW];
static Atom protocols, wm_delete;
static int running = 1, ws = 0, count[NWS], focus[NWS];

static void tile(void);
static void focus_win(void);
static void workspace(int);
static void spawn(const char **);
static int locate(Window, int *);
static void remove_win(int, int);

int main(void)
{
    if (!(dpy = XOpenDisplay(NULL))) return 1;
    signal(SIGCHLD, SIG_IGN);
    root = DefaultRootWindow(dpy);
    protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
    XSelectInput(dpy, root, SubstructureRedirectMask |
                              SubstructureNotifyMask);

    for (unsigned int i = 0; i < sizeof(keys)/sizeof(*keys); i++)
        XGrabKey(dpy, XKeysymToKeycode(dpy, keys[i].keysym),
                 keys[i].mod, root, True, GrabModeAsync, GrabModeAsync);

    XEvent e;
    while (running) {
        XNextEvent(dpy, &e);

        if (e.type == MapRequest) {
            Window w = e.xmaprequest.window;
            int dummy;
            if (locate(w, &dummy) >= 0 || count[ws] == MAXW) continue;
            XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);
            XSelectInput(dpy, w, StructureNotifyMask);
            wins[ws][count[ws]++] = w;
            focus[ws] = count[ws] - 1;
            XMapWindow(dpy, w);
            tile();
            focus_win();

        } else if (e.type == UnmapNotify || e.type == DestroyNotify) {
            Window w = e.type == UnmapNotify
                     ? e.xunmap.window : e.xdestroywindow.window;
            int owner, i = locate(w, &owner);
            if (i >= 0 && (e.type == DestroyNotify || owner == ws)) {
                remove_win(owner, i);
                if (owner == ws) {
                    tile();
                    focus_win();
                }
            }

        } else if (e.type == KeyPress) {
            KeySym sym = XLookupKeysym(&e.xkey, 0);
            unsigned int mod = e.xkey.state & (MODKEY | ShiftMask);

            for (unsigned int i = 0; i < sizeof(keys)/sizeof(*keys); i++) {
                if (keys[i].keysym != sym || keys[i].mod != mod)
                    continue;

                switch (keys[i].action) {
                case CMD_TERM:   spawn(terminal); break;
                case CMD_LAUNCH: spawn(launcher); break;
                case FOCUS_NEXT:
                    if (count[ws]) focus[ws] = (focus[ws] + 1) % count[ws];
                    focus_win();
                    break;
                case FOCUS_PREV:
                    if (count[ws]) focus[ws] =
                        (focus[ws] + count[ws] - 1) % count[ws];
                    focus_win();
                    break;
                case KILL_WIN:
                    if (count[ws]) {
                        Window w = wins[ws][focus[ws]];
                        Atom *p;
                        int n;
                        if (XGetWMProtocols(dpy, w, &p, &n)) {
                            for (int j = 0; j < n; j++)
                                if (p[j] == wm_delete) {
                                    XEvent m = {0};
                                    m.xclient.type = ClientMessage;
                                    m.xclient.window = w;
                                    m.xclient.message_type = protocols;
                                    m.xclient.format = 32;
                                    m.xclient.data.l[0] = wm_delete;
                                    m.xclient.data.l[1] = CurrentTime;
                                    XSendEvent(dpy, w, False, NoEventMask, &m);
                                    break;
                                }
                            XFree(p);
                        }
                    }
                    break;
                case WS_1: workspace(0); break;
                case WS_2: workspace(1); break;
                case WS_3: workspace(2); break;
                case EXIT_WM: running = 0; break;
                }
                break;
            }
        }
    }

    XCloseDisplay(dpy);
    return 0;
}

static void spawn(const char **cmd)
{
    if (fork() == 0) {
        setsid();
        execvp(cmd[0], (char *const *)cmd);
        _exit(127);
    }
}

static int locate(Window w, int *owner)
{
    for (int s = 0; s < NWS; s++)
        for (int i = 0; i < count[s]; i++)
            if (wins[s][i] == w) {
                *owner = s;
                return i;
            }
    return -1;
}

static void remove_win(int s, int i)
{
    for (; i < count[s] - 1; i++) wins[s][i] = wins[s][i + 1];
    if (--count[s] == 0) focus[s] = 0;
    else if (focus[s] >= count[s]) focus[s] = count[s] - 1;
    else if (focus[s] > i) focus[s]--;
}

static void focus_win(void)
{
    int n = count[ws];
    if (!n) {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
        return;
    }

    for (int i = 0; i < n; i++)
        XSetWindowBorder(dpy, wins[ws][i],
            i == focus[ws] ? COLOR_FOCUSED : 0x222222);

    Window w = wins[ws][focus[ws]];
    XRaiseWindow(dpy, w);
    XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
}

static void tile(void)
{
    int n = count[ws];
    if (!n) return;

    XWindowAttributes a;
    XGetWindowAttributes(dpy, root, &a);

    if (n == 1) {
        XMoveResizeWindow(dpy, wins[ws][0], 0, 0, a.width, a.height);
    } else {
        int mw = a.width / 2;
        XMoveResizeWindow(dpy, wins[ws][0], 0, 0, mw, a.height);
        for (int i = 1; i < n; i++) {
            int y1 = (i - 1) * a.height / (n - 1);
            int y2 = i * a.height / (n - 1);
            XMoveResizeWindow(dpy, wins[ws][i], mw, y1,
                              a.width - mw, y2 - y1);
        }
    }
}

static void workspace(int s)
{
    if (s == ws || s < 0 || s >= NWS) return;
    for (int i = 0; i < count[ws]; i++) XUnmapWindow(dpy, wins[ws][i]);
    ws = s;
    for (int i = 0; i < count[ws]; i++) XMapWindow(dpy, wins[ws][i]);
    tile();
    focus_win();
}
