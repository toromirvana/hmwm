#ifndef CONFIG_H
#define CONFIG_H

#include <X11/Xlib.h>
#include <X11/keysym.h>

// visual aesthetics
#define BORDER_WIDTH	1
#define COLOR_FOCUSED	0x444444

// Default Applications (pretty self explanitory.)
const char* terminal[] = { "st", NULL };
const char* launcher[] = { "dmenu_run", NULL };

#define MODKEY Mod4Mask

typedef enum {
	CMD_TERM,
	CMD_LAUNCH,
	WS_1, WS_2, WS_3,
	FOCUS_NEXT, FOCUS_PREV,
	KILL_WIN,
	EXIT_WM
} Action;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	Action action;
} KeyBind;


// Your Keyboard Config list
static const KeyBind keys[] = {
{ MODKEY,		XK_Return,	CMD_TERM },
{ MODKEY,		XK_p,		CMD_LAUNCH },
{ MODKEY,		XK_j,		FOCUS_NEXT },
{ MODKEY,		XK_k,		FOCUS_PREV },
{ MODKEY,		XK_q,		KILL_WIN },
{ MODKEY|ShiftMask,     XK_e,           EXIT_WM },
{ MODKEY,		XK_1,		WS_1 },
{ MODKEY,		XK_2,		WS_2 },
{ MODKEY,		XK_3,		WS_3 },

};
#endif
