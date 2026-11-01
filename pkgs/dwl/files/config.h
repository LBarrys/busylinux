/* LBarrys dwl configuration, mirroring the niri config.
 * Colours: gruvbox (green #98971a, grey #a89984, red #cc241d)
 * Startup programs live in ~/.config/dwl/autostart.sh (run via dwl -s).
 * Built with the bar, bar-systray, simple_scratchpad, warpcursor,
 * cursortheme and attachbottom patches (new windows open on the right). */

/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }
/* appearance */
static const int sloppyfocus               = 1;  /* focus follows mouse */
static const int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
static const unsigned int borderpx         = 5;  /* niri border width 5 */
static const unsigned int snap             = 32; /* snap pixel */
static const unsigned int systrayspacing   = 2;  /* gap between and around tray icons */
static const int showsystray               = 1;  /* 0 means no systray */
static const int showbar                   = 1;  /* 0 means no bar */
static const int topbar                    = 0;  /* bar at the bottom, like yambar */
static const char *fonts[]                 = {"Fira Code:style=Medium:pixelsize=14"};
static const float rootcolor[]             = COLOR(0x000000ff);
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
static const float fullscreen_bg[]         = {0.0f, 0.0f, 0.0f, 1.0f}; /* You can also use glsl colors */
/* cursor theme for dwl itself; also exported as XCURSOR_THEME/XCURSOR_SIZE
 * so programs started from dwl use the same one */
static const char *cursor_theme            = "Breeze_Light";
static const char cursor_size[]            = "24"; /* must be a valid integer */
/* bar and window border colours (gruvbox) */
static uint32_t colors[][3]                = {
	/*                 fg          bg          border    */
	[SchemeNorm]   = { 0xebdbb2ff, 0x1d2021ff, 0xa89984ff }, /* dark bar, unfocused borders */
	[SchemeSel]    = { 0xebdbb2ff, 0x98971aff, 0x98971aff }, /* focused border */
	[SchemeUrg]    = { 0xebdbb2ff, 0xcc241dff, 0xcc241dff }, /* urgent border */
	[SchemeStatus] = { 0xebdbb2ff, 0x98971aff, 0          }, /* status text and tray, green */
};

/* tagging: two tags, shown at the left end of the bar (current one green) */
static char *tags[] = { "1", "2" };

/* scratchpad slots (Super+S hides into the current slot, Super+Shift+S
 * shows/hides it again) */
#define SCRATCHPAD_COUNT 3

/* logging */
static int log_level = WLR_ERROR;

static const Rule rules[] = {
	/* app_id             title       tags mask     isfloating   monitor */
	{ "Gimp_EXAMPLE",     NULL,       0,            1,           -1 }, /* Start on currently visible tags floating, not tiled */
	/* default/example rule: can be changed but cannot be eliminated; at least one rule must exist */
};

/* layout(s) */
static const Layout layouts[] = {
	/* symbol     arrange function */
	{ "[]=",      tile },
	{ "><>",      NULL },    /* no layout function means floating behavior */
	{ "[M]",      monocle },
};

/* monitors: HDMI-A-1 on the left, DP-1 on the right. Two windows share a
 * monitor 50/50 (niri's default column width). Refresh rates are set with
 * wlr-randr in autostart.sh, since monitor rules can't set them. */
static const MonitorRule monrules[] = {
	/* name        mfact  nmaster scale layout       rotate/reflect                x     y     tags */
	{ "HDMI-A-1",  0.5f,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   0,    0,    1 << 1 }, /* left: tag 2 */
	{ "DP-1",      0.5f,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   1920, 0,    1 << 0 }, /* right: tag 1 */
	{ NULL,        0.5f,  1,      1,    &layouts[0], WL_OUTPUT_TRANSFORM_NORMAL,   -1,   -1,   0 },
	/* default monitor rule: can be changed but cannot be eliminated; at least one monitor rule must exist */
};

/* keyboard: us + Arabic, Caps Lock switches layout */
static const struct xkb_rule_names xkb_rules = {
	.layout = "us,ara",
	.options = "grp:caps_toggle,caps:none",
};

static const int repeat_rate = 25;
static const int repeat_delay = 200;

/* Trackpad */
static const int tap_to_click = 1;
static const int tap_and_drag = 1;
static const int drag_lock = 1;
static const int natural_scrolling = 0;
static const int disable_while_typing = 1;
static const int left_handed = 0;
static const int middle_button_emulation = 0;
static const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
static const enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
/* applies to every pointer, mice included, so it can't switch off just a touchpad */
static const uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* mouse: flat profile, accel 0.5 */
static const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
static const double accel_speed = 0.5;

static const enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

#define MODKEY WLR_MODIFIER_LOGO

/* Super+N views tag N, Super+Shift+N moves the focused window to it,
 * Super+Ctrl(+Shift)+N adds/removes tag N (view/window) */
#define TAGKEYS(KEY,TAG) \
	{ MODKEY,                    KEY,            view,            {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_SHIFT, KEY,            tag,             {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,KEY,toggletag,  {.ui = 1 << TAG} }

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* niri maximize-column: switch the focused monitor between tile and monocle
 * (focused window fills the screen, the others stay open behind it) */
static void
togglemonocle(const Arg *arg)
{
	if (!selmon)
		return;
	setlayout(&(Arg){.v = selmon->lt[selmon->sellt] == &layouts[2]
			? &layouts[0] : &layouts[2]});
}

/* send the focused window to the next monitor and follow it there
 * (focus and, with the warpcursor patch, the cursor) */
static void
tagmonfollow(const Arg *arg)
{
	Client *c = focustop(selmon);

	if (!c)
		return;
	setmon(c, dirtomon(arg->i), 0);
	focusclient(c, 1);
}

/* the focused monitor, kept up to date by autostart.sh from dwl's status */
#define SELMON "\"$(cat \"$XDG_RUNTIME_DIR/dwl-selmon\" 2>/dev/null)\""

/* Super+S: put the focused window in the scratchpad and hide it.
 * (simple_scratchpad's addscratchpad only marks it; this also hides the slot) */
static int scratchpad_visible[SCRATCHPAD_COUNT]; /* defined in simple_scratchpad.h */
static int scratchpad_sel;
static void addscratchpad(const Arg *arg);
static void togglescratchpad(const Arg *arg);
static void
sendscratchpad(const Arg *arg)
{
	if (!focustop(selmon))
		return;
	addscratchpad(arg);
	scratchpad_visible[scratchpad_sel] = 1; /* so the toggle hides it */
	togglescratchpad(arg);
}

static const char *termcmd[] = { "footclient", NULL };
static const char *browsercmd[] = { "firefox-esr", NULL };
static const char *fffcmd[] = { "footclient", "-e", "sh", "-ic", "fff", NULL };
/* tray icon menus (right click), shown with fuzzel on the focused monitor */
static const char *dmenucmd[] = { "/bin/sh", "-c",
	"exec fuzzel --dmenu --output=" SELMON " --placeholder='Menu: '", NULL };

static const Key keys[] = {
	/* modifier                  key                  function          argument */
	{ MODKEY,                    XKB_KEY_t,           spawn,            {.v = termcmd} },
	{ MODKEY,                    XKB_KEY_space,       spawn,            SHCMD("fuzzel --output=" SELMON) },
	{ MODKEY,                    XKB_KEY_b,           spawn,            {.v = browsercmd} },
	{ MODKEY,                    XKB_KEY_m,           spawn,            {.v = fffcmd} },
	/* cclip list prints "id<TAB>mime<TAB>preview": show only the preview
	 * (--with-nth=3), print only the id (--accept-nth=1) for cclip copy. */
	{ MODKEY,                    XKB_KEY_v,           spawn,            SHCMD("cclip list | fuzzel --dmenu --output=" SELMON " --placeholder='Clipboard: ' --with-nth=3 --accept-nth=1 | cclip copy -") },
	/* restart the bar's status text (network, clock, memory, CPU, GPU) */
	{ MODKEY,                    XKB_KEY_r,           spawn,            SHCMD("kill -HUP \"$(cat \"$XDG_RUNTIME_DIR/dwl-status.pid\")\"") },

	{ MODKEY,                    XKB_KEY_q,           killclient,       {0} },

	/* windows: next/previous in the tiling order; Super+Return swaps the
	 * focused window with the main (left) one */
	{ MODKEY,                    XKB_KEY_Left,        focusstack,       {.i = -1} },
	{ MODKEY,                    XKB_KEY_Right,       focusstack,       {.i = +1} },
	{ MODKEY,                    XKB_KEY_Return,      zoom,             {0} },

	/* monitors: focus, and send the focused window there (focus follows it) */
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Left,        focusmon,         {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Right,       focusmon,         {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Left,        tagmonfollow,     {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Right,       tagmonfollow,     {.i = WLR_DIRECTION_RIGHT} },

	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_F,           togglemonocle,    {0} },
	{ MODKEY,                    XKB_KEY_f,           togglefullscreen, {0} },

	/* scratchpad: send the focused window there / bring it back */
	{ MODKEY,                    XKB_KEY_s,           sendscratchpad,   {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_S,           togglescratchpad, {0} },

	/* tags */
	TAGKEYS(                     XKB_KEY_1,           0),
	TAGKEYS(                     XKB_KEY_2,           1),

	{ MODKEY,                    XKB_KEY_p,           spawn,            SHCMD("$HOME/.config/dwl/screenshot.sh region") },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_p,           spawn,            SHCMD("$HOME/.config/dwl/screenshot.sh screen") },

	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT, XKB_KEY_e,  quit,             {0} },

	/* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_BackSpace, quit, {0} },
	/* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
	 * do not remove them.
	 */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_F##n, chvt, {.ui = (n)} }
	CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
	CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

/* Super+drag moves (left) and resizes (right), as in niri. A tiled window
 * that is moved goes back into the tiling where it is dropped, swapping
 * places with the window under the cursor.
 * Tray icons: left click activates, right click opens the menu. */
static const Button buttons[] = {
	{ ClkClient,   MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ ClkClient,   MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ ClkClient,   MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
	{ ClkTray,     0,      BTN_LEFT,   trayactivate,   {0} },
	{ ClkTray,     0,      BTN_RIGHT,  traymenu,       {0} },
};

/* no scroll-wheel bindings (entries without a function are skipped) */
static const Axis axes[] = {
	{ 0, AxisUp, NULL, {0} },
};
