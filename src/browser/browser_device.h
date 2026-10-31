/*
 * The device drawn around an emulated viewport: a phone, tablet, laptop or
 * monitor dressed as the preset it stands for (browser_viewport.h), with the
 * page on its screen.
 *
 * The screen is the preset's size. As on the hardware, the system keeps
 * strips of it: the status bar (clock, signal, battery) with the camera
 * cutout in it, and the home indicator or gesture bar; the page gets the rest
 * and nothing is drawn over it.
 *
 * One function lays the whole device out; the Metal painter draws the body,
 * the strips and their contents from the result and the native web view takes
 * the page rect and its corner radii from the same numbers, so the two halves
 * never disagree about where the page is.
 */
#ifndef BROWSER_DEVICE_H
#define BROWSER_DEVICE_H

#include "core/types.h"

typedef enum {
    BDEV_PHONE = 0,
    BDEV_TABLET,
    BDEV_LAPTOP,    /* lid + keyboard deck */
    BDEV_MONITOR,   /* head + stand */
} BrowserDeviceKind;

typedef enum {
    BDEV_CUT_NONE = 0,
    BDEV_CUT_ISLAND,   /* capsule floating under the screen's top edge */
    BDEV_CUT_NOTCH,    /* hangs from the top edge, rounded where it ends */
    BDEV_CUT_HOLE,     /* round camera hole */
} BrowserDeviceCut;

/* Whose system bars the screen carries: where the clock and the icons sit,
 * and which strips are kept when the device is turned. */
typedef enum {
    BDEV_SYS_NONE = 0,   /* a desktop browser: the page fills the screen */
    BDEV_SYS_IOS,
    BDEV_SYS_ANDROID,
} BrowserDeviceSystem;

/* Corner order of every radius array: top-left, top-right, bottom-right,
 * bottom-left — renderer_draw_rrect's order. */
enum { BDEV_TL = 0, BDEV_TR, BDEV_BR, BDEV_BL };

typedef struct {
    f32 x, y, w, h;   /* y down; w == 0 means the part is absent */
    f32 r[4];
} BrowserDeviceShape;

#define BDEV_MAX_KEYS 5

typedef struct {
    bool on;                 /* false: Responsive, nothing is drawn */
    u8   kind;               /* BrowserDeviceKind */
    u8   cut;                /* BrowserDeviceCut */
    u8   sys;                /* BrowserDeviceSystem */
    f32  scale;              /* on-screen points per CSS pixel, <= 1 */
    i32  sw, sh;             /* the screen in CSS pixels: the preset's size, turned */
    i32  vw, vh;             /* the CSS viewport the page is given: the screen
                              * less the system's strips */

    BrowserDeviceShape body;     /* outer outline: phone body, lid, monitor head */
    BrowserDeviceShape glass;    /* front glass inside the metal rim */
    BrowserDeviceShape screen;   /* the display: exactly sw x sh times scale */
    BrowserDeviceShape page;     /* the web view: exactly vw x vh times scale,
                                  * its radii those that keep it on the display */
    BrowserDeviceShape bar;      /* the status bar's strip; w == 0 when hidden */
    BrowserDeviceShape clock;    /* box the time is set in (centred, or from its
                                  * left edge when clock_left) */
    BrowserDeviceShape signal;   /* cellular bars / triangle */
    BrowserDeviceShape wifi;     /* Wi-Fi glyph */
    BrowserDeviceShape battery;  /* battery, its terminal included; upright on Android */
    BrowserDeviceShape percent;  /* box the charge ("100%") ends at the right of */
    const char *clock_text;      /* what the clock reads */
    f32  clock_pt, percent_pt;   /* font sizes in the layout's units */
    f32  clock_weight;           /* system font weight: 0.3 semibold, 0.23 medium */
    bool clock_left;
    BrowserDeviceShape indicator;/* home indicator / gesture handle */
    BrowserDeviceShape cutout;   /* in a strip, never over the page */
    BrowserDeviceShape lens;     /* front camera in the bezel */
    BrowserDeviceShape flash;    /* front LED flash in the bezel */
    BrowserDeviceShape speaker;  /* earpiece slot in the bezel */
    BrowserDeviceShape hinge;    /* a folding device's hinge, along the edge it
                                  * folds at (the left one upright) */
    BrowserDeviceShape knuckles[2]; /* the hinge's metal end caps */
    BrowserDeviceShape home;     /* home button under the screen */
    BrowserDeviceShape base;     /* laptop deck or monitor foot */
    BrowserDeviceShape scoop;    /* the deck's opening notch */
    BrowserDeviceShape neck;     /* monitor neck */
    i32  n_keys;
    BrowserDeviceShape keys[BDEV_MAX_KEYS];   /* side buttons, outside the body */

    f32  rim;                /* metal rim width, same units as the rects */
    u32  frame_rgb;          /* 0xRRGGBB: rim, keys */
    u32  glass_rgb;          /* 0xRRGGBB: bezel */
    u32  base_rgb;           /* 0xRRGGBB: deck / stand */

    /* Everything drawn, keys and stand included; the caption sits above. */
    f32  ext_x, ext_y, ext_w, ext_h;
    /* The caption's line, centred over the device when there is room. */
    f32  cap_y;
    bool cap_room;
} BrowserDeviceLayout;

/* Room kept free around the device and above it for the caption, in points. */
#define BDEV_MARGIN_PT   14.0f
#define BDEV_CAPTION_PT  18.0f

/* Lay out preset `idx` (rotated or not) inside the page area (ax, ay, aw, ah):
 * points, top-left origin, y down. The device is scaled down to fit — never
 * up — and centred. Turned, an iPhone hides its status bar and keeps a strip
 * on both long sides (the cutout is in one), an Android phone keeps its
 * status bar on top and the cutout's side clear, a tablet keeps its bars where
 * they were. Returns false for Responsive or an index out of range (out->on
 * is false and the page fills the area). */
bool browser_device_layout(i32 idx, bool rotated,
                           f32 ax, f32 ay, f32 aw, f32 ah,
                           BrowserDeviceLayout *out);

/* Whether preset `idx` has a look of its own rather than its group's
 * generic one. */
bool browser_device_has_skin(i32 idx);

#endif /* BROWSER_DEVICE_H */
