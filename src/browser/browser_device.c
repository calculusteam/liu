/*
 * Device looks for the viewport presets. Sizes are the hardware's own in CSS
 * pixels (points at scale 1): bezels from the body and display dimensions,
 * the display's corner radius, the camera cutout's size and offset, and the
 * strips the system keeps at the top and bottom of an upright screen. A preset
 * without an entry here is drawn as its group's generic device.
 */
#include "browser/browser_device.h"
#include "browser/browser_viewport.h"

#include <string.h>

enum {
    KEYS_NONE = 0,
    KEYS_IPHONE_PRO,     /* action button + volume left, side button right */
    KEYS_IPHONE_PRO16,   /* the same + Camera Control */
    KEYS_IPHONE,         /* ring switch + volume left, side button right */
    KEYS_IPHONE_SE,
    KEYS_PIXEL,          /* power over volume, right */
    KEYS_GALAXY,         /* volume over power, right */
    KEYS_IPAD,           /* top button + volume on the right */
    KEYS_IPAD_MINI,
    KEYS_SURFACE,        /* power + volume on the top edge */
    KEYS_DUO,            /* volume over power, on the edge across from the hinge */
    KEYS_COUNT,
};

enum {
    MARK_LENS     = 1u << 0,   /* camera centred in the top bezel */
    MARK_SPEAKER  = 1u << 1,   /* earpiece slot, camera beside it */
    MARK_HOME     = 1u << 2,   /* home button in the bottom bezel */
    MARK_LENS_OFF = 1u << 3,   /* camera right of centre in the top bezel */
    MARK_EAR_ROW  = 1u << 4,   /* earpiece over the screen's middle, the camera
                                * and the LED flash to its right */
};

typedef struct {
    const char *name;
    u8  kind, cut, keys, marks;
    f32 bezel_x, bezel_top, bezel_bottom;
    f32 screen_r, body_r;
    f32 cut_w, cut_h, cut_y;
    f32 rim;
    u32 frame, glass;
    u8  sys;                       /* BrowserDeviceSystem */
    f32 bar_top, bar_bottom;       /* upright: status bar, home indicator / gesture bar */
    f32 bezel_hinge;               /* a folding device: the bezel on the hinge's side
                                    * (the left, upright), hinge included; 0 = bezel_x */
} Skin;

#define TITANIUM  0x8a8780u
#define MIDNIGHT  0x2f333au
#define BLACK_AL  0x2c2c2fu
#define OBSIDIAN  0x2d2e32u
#define PHANTOM   0x28292cu
#define GLACIER   0xd3d4d1u
#define SPACE_GR  0x46474bu
#define PLATINUM  0xc7c8c6u
#define SILVER    0xc9cbcdu
#define GRAPHITE  0x2a2a2du
#define GLASS     0x050506u

static const Skin g_skins[] = {
    /* name                 kind          cut              keys               marks
     *                      bezel x/top/bottom   screen_r body_r  cut w/h/y        rim  frame     glass
     *                      system, status bar, bottom bar */
    { "iPhone SE",          BDEV_PHONE,   BDEV_CUT_NONE,   KEYS_IPHONE_SE,    MARK_SPEAKER | MARK_HOME,
                            28.5f, 110.0f, 110.0f,  0.0f, 62.0f,   0, 0, 0,           3.0f, BLACK_AL, GLASS,
                            BDEV_SYS_IOS, 20.0f, 0.0f, 0.0f },
    { "iPhone XR",          BDEV_PHONE,   BDEV_CUT_NOTCH,  KEYS_IPHONE,       0,
                            36.0f, 36.0f, 36.0f,   41.5f, 77.5f,   209.0f, 31.0f, 0,  4.0f, BLACK_AL, GLASS,
                            BDEV_SYS_IOS, 48.0f, 34.0f, 0.0f },
    { "iPhone 14",          BDEV_PHONE,   BDEV_CUT_NOTCH,  KEYS_IPHONE,       0,
                            21.0f, 21.0f, 21.0f,  47.33f, 68.0f,   162.0f, 34.0f, 0,  3.5f, MIDNIGHT, GLASS,
                            BDEV_SYS_IOS, 47.0f, 34.0f, 0.0f },
    { "iPhone 14 Plus",     BDEV_PHONE,   BDEV_CUT_NOTCH,  KEYS_IPHONE,       0,
                            20.7f, 20.0f, 20.0f,  53.33f, 74.0f,   162.0f, 34.0f, 0,  3.5f, MIDNIGHT, GLASS,
                            BDEV_SYS_IOS, 47.0f, 34.0f, 0.0f },
    { "iPhone 15 Pro",      BDEV_PHONE,   BDEV_CUT_ISLAND, KEYS_IPHONE_PRO,   0,
                            16.6f, 16.6f, 16.6f,  55.0f, 71.6f,    126.0f, 37.33f, 11.33f, 3.6f, TITANIUM, GLASS,
                            BDEV_SYS_IOS, 59.0f, 34.0f, 0.0f },
    { "iPhone 15 Pro Max",  BDEV_PHONE,   BDEV_CUT_ISLAND, KEYS_IPHONE_PRO,   0,
                            16.6f, 16.6f, 16.6f,  55.0f, 71.6f,    126.0f, 37.33f, 11.33f, 3.6f, TITANIUM, GLASS,
                            BDEV_SYS_IOS, 59.0f, 34.0f, 0.0f },
    { "iPhone 16 Pro",      BDEV_PHONE,   BDEV_CUT_ISLAND, KEYS_IPHONE_PRO16, 0,
                            14.8f, 14.5f, 14.5f,  62.0f, 76.8f,    125.0f, 37.0f, 13.0f, 3.4f, TITANIUM, GLASS,
                            BDEV_SYS_IOS, 62.0f, 34.0f, 0.0f },
    { "iPhone 16 Pro Max",  BDEV_PHONE,   BDEV_CUT_ISLAND, KEYS_IPHONE_PRO16, 0,
                            14.2f, 14.2f, 14.2f,  62.0f, 76.2f,    125.0f, 37.0f, 13.5f, 3.4f, TITANIUM, GLASS,
                            BDEV_SYS_IOS, 62.0f, 34.0f, 0.0f },
    { "Pixel 8",            BDEV_PHONE,   BDEV_CUT_HOLE,   KEYS_PIXEL,        0,
                            20.8f, 24.7f, 24.7f,  38.0f, 59.0f,    19.0f, 19.0f, 12.0f, 3.0f, OBSIDIAN, GLASS,
                            BDEV_SYS_ANDROID, 44.0f, 24.0f, 0.0f },
    { "Pixel 9",            BDEV_PHONE,   BDEV_CUT_HOLE,   KEYS_PIXEL,        0,
                            21.5f, 20.5f, 20.5f,  40.0f, 61.0f,    19.0f, 19.0f, 12.0f, 3.0f, OBSIDIAN, GLASS,
                            BDEV_SYS_ANDROID, 44.0f, 24.0f, 0.0f },
    { "Pixel 9 Pro",        BDEV_PHONE,   BDEV_CUT_HOLE,   KEYS_PIXEL,        0,
                            20.5f, 20.0f, 20.0f,  42.0f, 62.0f,    19.0f, 19.0f, 12.0f, 3.0f, OBSIDIAN, GLASS,
                            BDEV_SYS_ANDROID, 44.0f, 24.0f, 0.0f },
    { "Pixel 9 Pro XL",     BDEV_PHONE,   BDEV_CUT_HOLE,   KEYS_PIXEL,        0,
                            20.4f, 20.7f, 20.7f,  44.0f, 64.0f,    20.0f, 20.0f, 12.0f, 3.0f, OBSIDIAN, GLASS,
                            BDEV_SYS_ANDROID, 44.0f, 24.0f, 0.0f },
    { "Galaxy S20 Ultra",   BDEV_PHONE,   BDEV_CUT_HOLE,   KEYS_GALAXY,       0,
                            12.4f, 22.0f, 22.0f,  26.0f, 38.0f,    16.0f, 16.0f, 10.0f, 2.6f, PHANTOM,  GLASS,
                            BDEV_SYS_ANDROID, 32.0f, 24.0f, 0.0f },
    { "Galaxy Z Fold 5",    BDEV_PHONE,   BDEV_CUT_HOLE,   KEYS_GALAXY,       0,
                            22.0f, 25.0f, 25.0f,  22.0f, 44.0f,    15.0f, 15.0f, 9.0f,  3.0f, PHANTOM,  GLASS,
                            BDEV_SYS_ANDROID, 30.0f, 24.0f, 0.0f },
    /* Folded, one screen facing out: 93.3 x 145.2 mm of black glass in a
     * Glacier bumper, the hinge along the left edge, the display's corners
     * square. The 540 x 720 screen leaves 33 on the outer side, 16 on the
     * hinge's (the 34 between the two screens when open) and 98.4 above and
     * below. */
    { "Surface Duo",        BDEV_PHONE,   BDEV_CUT_NONE,   KEYS_DUO,          MARK_EAR_ROW,
                            33.0f, 98.4f, 98.4f,   0.0f, 37.0f,    0, 0, 0,           2.6f, GLACIER,  GLASS,
                            BDEV_SYS_ANDROID, 24.0f, 24.0f, 16.0f },

    { "iPad Mini",          BDEV_TABLET,  BDEV_CUT_NONE,   KEYS_IPAD_MINI,    MARK_LENS | MARK_HOME,
                            46.0f, 139.0f, 139.0f, 0.0f, 60.0f,    0, 0, 0,           3.0f, SPACE_GR, GLASS,
                            BDEV_SYS_IOS, 20.0f, 0.0f, 0.0f },
    { "iPad Air",           BDEV_TABLET,  BDEV_CUT_NONE,   KEYS_IPAD,         MARK_LENS,
                            53.8f, 53.5f, 53.5f,  18.0f, 60.0f,    0, 0, 0,           3.0f, SPACE_GR, GLASS,
                            BDEV_SYS_IOS, 24.0f, 20.0f, 0.0f },
    { "iPad Pro",           BDEV_TABLET,  BDEV_CUT_NONE,   KEYS_IPAD,         MARK_LENS,
                            46.5f, 46.3f, 46.3f,  18.0f, 58.0f,    0, 0, 0,           3.0f, SPACE_GR, GLASS,
                            BDEV_SYS_IOS, 24.0f, 20.0f, 0.0f },
    { "Surface Pro 7",      BDEV_TABLET,  BDEV_CUT_NONE,   KEYS_SURFACE,      MARK_LENS,
                            73.0f, 83.5f, 83.5f,   4.0f, 30.0f,    0, 0, 0,           3.0f, PLATINUM, GLASS,
                            BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
    { "Asus Zenbook Fold",  BDEV_TABLET,  BDEV_CUT_NONE,   KEYS_NONE,         MARK_LENS,
                            30.0f, 34.0f, 34.0f,   8.0f, 26.0f,    0, 0, 0,           2.6f, GRAPHITE, GLASS,
                            BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },

    { "Laptop (MDPI)",      BDEV_LAPTOP,  BDEV_CUT_NONE,   KEYS_NONE,         MARK_LENS,
                            22.0f, 26.0f, 22.0f,   0.0f, 18.0f,    0, 0, 0,           2.5f, SILVER,   GLASS,
                            BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
    { "Laptop (HiDPI)",     BDEV_LAPTOP,  BDEV_CUT_NONE,   KEYS_NONE,         MARK_LENS,
                            22.0f, 26.0f, 22.0f,   0.0f, 18.0f,    0, 0, 0,           2.5f, SILVER,   GLASS,
                            BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
    { "Desktop",            BDEV_MONITOR, BDEV_CUT_NONE,   KEYS_NONE,         0,
                            22.0f, 22.0f, 30.0f,   0.0f, 10.0f,    0, 0, 0,           2.0f, GRAPHITE, GLASS,
                            BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
    { "Desktop 4K",         BDEV_MONITOR, BDEV_CUT_NONE,   KEYS_NONE,         0,
                            26.0f, 26.0f, 36.0f,   0.0f, 12.0f,    0, 0, 0,           2.0f, GRAPHITE, GLASS,
                            BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
};

/* A preset added to the table without a skin still looks like what it is. */
static const Skin g_generic[BVP_GROUP_COUNT] = {
    [BVP_GROUP_PHONE]  = { "phone",  BDEV_PHONE,  BDEV_CUT_HOLE, KEYS_PIXEL, 0,
                           18.0f, 20.0f, 20.0f, 40.0f, 58.0f, 18.0f, 18.0f, 12.0f, 3.0f, OBSIDIAN, GLASS,
                           BDEV_SYS_ANDROID, 40.0f, 24.0f, 0.0f },
    [BVP_GROUP_TABLET] = { "tablet", BDEV_TABLET, BDEV_CUT_NONE, KEYS_NONE, MARK_LENS,
                           40.0f, 40.0f, 40.0f, 18.0f, 54.0f, 0, 0, 0, 3.0f, SPACE_GR, GLASS,
                           BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
    [BVP_GROUP_LAPTOP] = { "laptop", BDEV_LAPTOP, BDEV_CUT_NONE, KEYS_NONE, MARK_LENS,
                           22.0f, 26.0f, 22.0f, 0.0f, 18.0f, 0, 0, 0, 2.5f, SILVER, GLASS,
                           BDEV_SYS_NONE, 0.0f, 0.0f, 0.0f },
};

typedef struct { u8 side; f32 at, len; } KeySpec;   /* side: 0 left, 1 right, 2 top */
enum { KEY_LEFT = 0, KEY_RIGHT, KEY_TOP };

/* Along the side as fractions of the body's height (left / right) or width
 * (top). */
static const KeySpec g_keys[KEYS_COUNT][BDEV_MAX_KEYS] = {
    [KEYS_IPHONE_PRO]   = { { KEY_LEFT, 0.175f, 0.040f }, { KEY_LEFT, 0.255f, 0.068f },
                            { KEY_LEFT, 0.345f, 0.068f }, { KEY_RIGHT, 0.290f, 0.110f } },
    [KEYS_IPHONE_PRO16] = { { KEY_LEFT, 0.175f, 0.040f }, { KEY_LEFT, 0.255f, 0.068f },
                            { KEY_LEFT, 0.345f, 0.068f }, { KEY_RIGHT, 0.290f, 0.110f },
                            { KEY_RIGHT, 0.585f, 0.075f } },
    [KEYS_IPHONE]       = { { KEY_LEFT, 0.170f, 0.030f }, { KEY_LEFT, 0.245f, 0.060f },
                            { KEY_LEFT, 0.330f, 0.060f }, { KEY_RIGHT, 0.280f, 0.100f } },
    [KEYS_IPHONE_SE]    = { { KEY_LEFT, 0.160f, 0.030f }, { KEY_LEFT, 0.235f, 0.055f },
                            { KEY_LEFT, 0.310f, 0.055f }, { KEY_RIGHT, 0.235f, 0.070f } },
    [KEYS_PIXEL]        = { { KEY_RIGHT, 0.255f, 0.065f }, { KEY_RIGHT, 0.345f, 0.125f } },
    [KEYS_GALAXY]       = { { KEY_RIGHT, 0.200f, 0.120f }, { KEY_RIGHT, 0.360f, 0.065f } },
    [KEYS_IPAD]         = { { KEY_TOP, 0.800f, 0.060f }, { KEY_RIGHT, 0.075f, 0.040f },
                            { KEY_RIGHT, 0.125f, 0.040f } },
    [KEYS_IPAD_MINI]    = { { KEY_TOP, 0.820f, 0.070f }, { KEY_RIGHT, 0.150f, 0.050f },
                            { KEY_RIGHT, 0.210f, 0.050f } },
    [KEYS_SURFACE]      = { { KEY_TOP, 0.120f, 0.050f }, { KEY_TOP, 0.200f, 0.070f } },
    [KEYS_DUO]          = { { KEY_RIGHT, 0.335f, 0.062f }, { KEY_RIGHT, 0.555f, 0.075f } },
};

static const Skin *skin_for(i32 idx) {
    const BrowserViewportPreset *p = browser_viewport_get(idx);
    if (!p || idx == BVP_RESPONSIVE) return NULL;
    for (usize i = 0; i < sizeof g_skins / sizeof g_skins[0]; i++)
        if (!strcmp(g_skins[i].name, p->name)) return &g_skins[i];
    u8 g = p->group < BVP_GROUP_COUNT ? p->group : BVP_GROUP_PHONE;
    return &g_generic[g];
}

bool browser_device_has_skin(i32 idx) {
    const Skin *s = skin_for(idx);
    return s && s >= g_skins && s < g_skins + sizeof g_skins / sizeof g_skins[0];
}

static BrowserDeviceShape shape(f32 x, f32 y, f32 w, f32 h, f32 r) {
    BrowserDeviceShape s = { x, y, w, h, { r, r, r, r } };
    return s;
}

static f32 fminf_(f32 a, f32 b) { return a < b ? a : b; }
static f32 fmaxf_(f32 a, f32 b) { return a > b ? a : b; }

/* A quarter turn counter-clockwise inside a body `bw` wide: the top edge
 * becomes the left one, which is how a phone held in landscape with its
 * camera on the left looks. */
static void turn(BrowserDeviceShape *s, f32 bw) {
    if (s->w <= 0.0f) return;
    BrowserDeviceShape t = *s;
    s->x = t.y;
    s->y = bw - (t.x + t.w);
    s->w = t.h;
    s->h = t.w;
    s->r[BDEV_TL] = t.r[BDEV_TR];
    s->r[BDEV_TR] = t.r[BDEV_BR];
    s->r[BDEV_BR] = t.r[BDEV_BL];
    s->r[BDEV_BL] = t.r[BDEV_TL];
}

static void place(BrowserDeviceShape *s, f32 ox, f32 oy, f32 ex, f32 ey, f32 k) {
    if (s->w <= 0.0f) return;
    s->x = ox + (s->x - ex) * k;
    s->y = oy + (s->y - ey) * k;
    s->w *= k;
    s->h *= k;
    for (i32 i = 0; i < 4; i++) s->r[i] *= k;
}

/* A folding device folded, one screen facing out: the hinge along the left
 * edge between the bumper's lips, its metal end caps standing a little out of
 * that edge, and the panel's corners there nearly square. */
#define FOLD_HINGE_W       10.0f
#define FOLD_LIP            6.0f
#define FOLD_KNUCKLE_AT    10.0f
#define FOLD_KNUCKLE_H     40.0f
#define FOLD_KNUCKLE_OUT    0.8f
#define FOLD_CORNER         3.0f

/* The top bezel's row on a device that has one: a grille 73 x 9, a camera
 * 25 across, a flash 27 across. */
#define EAR_ROW_SPEAKER_W  73.0f
#define EAR_ROW_SPEAKER_H   9.0f
#define EAR_ROW_LENS       25.0f
#define EAR_ROW_FLASH      27.0f

/* Landscape strips that are not an upright one turned: a turned iPhone's home
 * indicator strip, a turned Android phone's status bar. */
#define IOS_TURNED_BOTTOM   21.0f
#define ANDROID_TURNED_TOP  24.0f

/* The system's strips of the screen in the orientation it is shown in:
 * left, top, right, bottom. `status` says whether the top one is a status
 * bar with a clock and icons. */
static void system_insets(const Skin *sk, bool rotated, f32 in[4], bool *status) {
    in[0] = in[1] = in[2] = in[3] = 0.0f;
    *status = false;
    if (sk->sys == BDEV_SYS_NONE) return;
    if (!rotated || (sk->sys == BDEV_SYS_IOS && sk->kind == BDEV_TABLET)) {
        in[1] = sk->bar_top;
        in[3] = sk->bar_bottom;
        *status = in[1] > 0.0f;
        return;
    }
    if (sk->sys == BDEV_SYS_IOS) {
        /* An iPhone on its side hides the status bar; the page is kept clear
         * of the cutout's side and, alike, of the side across from it, and
         * the home indicator stays under it. */
        if (sk->cut != BDEV_CUT_NONE) in[0] = in[2] = sk->bar_top;
        if (sk->bar_bottom > 0.0f) in[3] = IOS_TURNED_BOTTOM;
        return;
    }
    /* An Android phone on its side keeps its status bar on top, does not draw
     * into the cutout's side, and keeps the gesture bar under the page. */
    in[1] = ANDROID_TURNED_TOP;
    in[3] = sk->bar_bottom;
    if (sk->cut != BDEV_CUT_NONE) in[0] = sk->bar_top;
    *status = true;
}

/* Lay `n` marks of sizes `w[] x h[]` left to right from `x`, `gap[i]` after
 * mark i, each centred on `cy`; returns where the row ends. */
static f32 row_of(BrowserDeviceShape **s, const f32 *w, const f32 *h, const f32 *gap,
                  i32 n, f32 x, f32 cy) {
    for (i32 i = 0; i < n; i++) {
        *s[i] = shape(x, cy - h[i] * 0.5f, w[i], h[i], 0.0f);
        x += w[i] + (i + 1 < n ? gap[i] : 0.0f);
    }
    return x;
}

static f32 row_width(const f32 *w, const f32 *gap, i32 n) {
    f32 t = 0.0f;
    for (i32 i = 0; i < n; i++) t += w[i] + (i + 1 < n ? gap[i] : 0.0f);
    return t;
}

/* The status bar's contents inside L->bar (unscaled, y down), in the
 * system's own sizes:
 *   iPhone with a cutout  "9:41" in 17 pt semibold centred in the strip left
 *                         of it; cellular 19.2 x 12.2, Wi-Fi 17.1 x 12.3 and
 *                         the battery 27.3 x 13 centred right of it, level
 *                         with an island's middle.
 *   iPhone without one    signal and Wi-Fi at the left, "9:41 AM" centred,
 *                         "100%" and the battery at the right, in 12 pt.
 *   iPad                  "9:41 AM  Mon Jun 10" from the left, Wi-Fi, "100%"
 *                         and the battery at the right, in 12 pt.
 *   Android               "9:41" from the left in 14 pt medium, Wi-Fi fan,
 *                         signal triangle and an upright battery at the right.
 * A strip too narrow for its row has the row scaled down to fit. */
static void status_items(BrowserDeviceLayout *L, const Skin *sk) {
    const BrowserDeviceShape *bar = &L->bar;
    f32 x0 = bar->x, x1 = bar->x + bar->w;
    f32 cy = bar->y + bar->h * 0.5f;
    bool ios = sk->sys == BDEV_SYS_IOS;
    bool ears = ios && sk->kind == BDEV_PHONE && L->cutout.w > 0.0f &&
                L->cutout.y < bar->y + bar->h &&
                L->cutout.x > x0 && L->cutout.x + L->cutout.w < x1;
    L->clock_weight = ios ? 0.30f : 0.23f;
    L->clock_left = false;
    L->percent_pt = 0.0f;

    if (ears) {
        /* An island's marks are level with its middle; beside a notch they
         * sit a point above the strip's. */
        cy = sk->cut == BDEV_CUT_ISLAND ? L->cutout.y + L->cutout.h * 0.5f : cy - 1.0f;
        f32 lw = L->cutout.x - x0, rw = x1 - (L->cutout.x + L->cutout.w);
        BrowserDeviceShape *row[] = { &L->signal, &L->wifi, &L->battery };
        f32 w[] = { 19.2f, 17.1f, 27.3f }, h[] = { 12.2f, 12.3f, 13.0f };
        f32 gap[] = { 5.0f, 5.0f };
        f32 k = fminf_(1.0f, (rw - 8.0f) / row_width(w, gap, 3));
        for (i32 i = 0; i < 3; i++) { w[i] *= k; h[i] *= k; }
        for (i32 i = 0; i < 2; i++) gap[i] *= k;
        f32 rc = L->cutout.x + L->cutout.w + rw * 0.5f;
        row_of(row, w, h, gap, 3, rc - row_width(w, gap, 3) * 0.5f, cy);
        L->clock_text = "9:41";
        L->clock_pt = fminf_(17.0f, lw * 0.30f);
        L->clock = shape(x0, cy - L->clock_pt * 0.5f, lw, L->clock_pt, 0.0f);
        return;
    }

    f32 batt_w = ios ? 24.5f : 7.6f, batt_h = ios ? 11.5f : 13.0f;
    if (ios) {
        bool phone = sk->kind == BDEV_PHONE;
        f32 inset = phone || sk->bar_bottom <= 0.0f ? 6.0f : 20.0f;
        f32 text = 12.0f;
        L->percent_pt = text;
        if (phone) {
            BrowserDeviceShape *left[] = { &L->signal, &L->wifi };
            f32 w[] = { 17.0f, 15.3f }, h[] = { 10.7f, 11.0f }, gap[] = { 4.0f };
            row_of(left, w, h, gap, 2, x0 + inset, cy);
            L->clock_text = "9:41 AM";
            L->clock = shape((x0 + x1) * 0.5f - text * 2.8f, cy - text * 0.5f,
                             text * 5.6f, text, 0.0f);
        } else {
            L->clock_text = "9:41 AM  Mon Jun 10";
            L->clock_left = true;
            L->clock = shape(x0 + inset, cy - text * 0.5f, text * 11.0f, text, 0.0f);
        }
        L->clock_pt = text;
        L->battery = shape(x1 - inset - batt_w, cy - batt_h * 0.5f, batt_w, batt_h, 0.0f);
        /* "100%" is about 2.4 of its font size wide; its box ends a few
         * points short of the battery. */
        f32 pw = text * 2.6f;
        L->percent = shape(L->battery.x - 4.0f - pw, cy - text * 0.5f, pw, text, 0.0f);
        if (!phone)
            L->wifi = shape(L->percent.x - 6.0f - 15.3f, cy - 5.5f, 15.3f, 11.0f, 0.0f);
        return;
    }

    /* Android: the clock from the start, the icons to the end. */
    f32 inset = bar->h >= 30.0f ? 22.0f : 14.0f;
    f32 text = 14.0f;
    L->clock_text = "9:41";
    L->clock_pt = text;
    L->clock_left = true;
    L->clock = shape(x0 + inset, cy - text * 0.5f, text * 3.0f, text, 0.0f);
    BrowserDeviceShape *row[] = { &L->wifi, &L->signal, &L->battery };
    f32 w[] = { 16.0f, 13.0f, batt_w }, h[] = { 12.5f, 13.0f, batt_h }, gap[] = { 4.0f, 5.0f };
    row_of(row, w, h, gap, 3, x1 - inset - row_width(w, gap, 3), cy);
}

static void grow(f32 *x0, f32 *y0, f32 *x1, f32 *y1, const BrowserDeviceShape *s) {
    if (s->w <= 0.0f) return;
    *x0 = fminf_(*x0, s->x);
    *y0 = fminf_(*y0, s->y);
    *x1 = fmaxf_(*x1, s->x + s->w);
    *y1 = fmaxf_(*y1, s->y + s->h);
}

bool browser_device_layout(i32 idx, bool rotated,
                           f32 ax, f32 ay, f32 aw, f32 ah,
                           BrowserDeviceLayout *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    out->scale = 1.0f;
    const Skin *sk = skin_for(idx);
    const BrowserViewportPreset *p = browser_viewport_get(idx);
    if (!sk || !p || p->w <= 0 || p->h <= 0) return false;
    if (!(aw > 0.0f) || !(ah > 0.0f)) return false;

    /* Built in the device's natural orientation, one unit per CSS pixel. A
     * folding device has its hinge on the left: that side's bezel is its own. */
    f32 nw = (f32)p->w, nh = (f32)p->h;
    bool folds = sk->bezel_hinge > 0.0f;
    f32 bez_l = folds ? sk->bezel_hinge : sk->bezel_x;
    f32 bw = nw + bez_l + sk->bezel_x;
    f32 bh = nh + sk->bezel_top + sk->bezel_bottom;
    BrowserDeviceLayout L = { 0 };
    L.kind = sk->kind;
    L.cut = sk->cut;
    L.rim = sk->rim;
    L.frame_rgb = sk->frame;
    L.glass_rgb = sk->glass;
    L.base_rgb = sk->kind == BDEV_MONITOR ? 0x9fa1a4u : 0xd1d3d5u;
    L.body = shape(0.0f, 0.0f, bw, bh, sk->body_r);
    if (sk->kind == BDEV_LAPTOP) {
        /* A lid: round on top, nearly square where it meets the hinge. */
        L.body.r[BDEV_BR] = L.body.r[BDEV_BL] = sk->body_r * 0.3f;
    }
    f32 gr = sk->body_r - sk->rim;
    L.glass = shape(sk->rim, sk->rim, bw - 2.0f * sk->rim, bh - 2.0f * sk->rim,
                    gr > 0.0f ? gr : 0.0f);
    if (sk->kind == BDEV_LAPTOP)
        L.glass.r[BDEV_BR] = L.glass.r[BDEV_BL] = fmaxf_(0.0f, L.body.r[BDEV_BR] - sk->rim);
    if (folds) {
        /* Square where the panel meets the hinge; the glass starts past the
         * hinge and the bumper's edge beside it. The hinge runs between the
         * bumper's lips at the top and bottom, its end caps just inside
         * them. */
        L.body.r[BDEV_TL] = L.body.r[BDEV_BL] = FOLD_CORNER;
        f32 gx = FOLD_HINGE_W + sk->rim;
        L.glass = shape(gx, sk->rim, bw - gx - sk->rim, bh - 2.0f * sk->rim, gr > 0.0f ? gr : 0.0f);
        L.glass.r[BDEV_TL] = L.glass.r[BDEV_BL] = 0.0f;
        L.hinge = shape(0.0f, FOLD_LIP, FOLD_HINGE_W, bh - 2.0f * FOLD_LIP, 0.0f);
        f32 kw = FOLD_HINGE_W + FOLD_KNUCKLE_OUT;
        L.knuckles[0] = shape(-FOLD_KNUCKLE_OUT, FOLD_KNUCKLE_AT, kw, FOLD_KNUCKLE_H, 1.5f);
        L.knuckles[1] = shape(-FOLD_KNUCKLE_OUT, bh - FOLD_KNUCKLE_AT - FOLD_KNUCKLE_H, kw,
                              FOLD_KNUCKLE_H, 1.5f);
    }
    L.screen = shape(bez_l, sk->bezel_top, nw, nh, sk->screen_r);

    f32 cx = bw * 0.5f;
    switch (sk->cut) {
    case BDEV_CUT_ISLAND:
    case BDEV_CUT_HOLE:
        L.cutout = shape(cx - sk->cut_w * 0.5f, sk->bezel_top + sk->cut_y,
                         sk->cut_w, sk->cut_h, sk->cut_h * 0.5f);
        break;
    case BDEV_CUT_NOTCH: {
        f32 nr = fminf_(sk->cut_h * 0.6f, 20.0f);
        L.cutout = shape(cx - sk->cut_w * 0.5f, sk->bezel_top, sk->cut_w, sk->cut_h, 0.0f);
        L.cutout.r[BDEV_BR] = L.cutout.r[BDEV_BL] = nr;
        break;
    }
    default: break;
    }

    if (sk->marks & (MARK_LENS | MARK_LENS_OFF)) {
        f32 d = fminf_(fmaxf_(sk->bezel_top * 0.22f, 5.0f), 10.0f);
        f32 lx = (sk->marks & MARK_LENS_OFF) ? bw * 0.78f : cx;
        L.lens = shape(lx - d * 0.5f, sk->bezel_top * 0.5f - d * 0.5f, d, d, d * 0.5f);
    }
    if (sk->marks & MARK_SPEAKER) {
        f32 sw = fminf_(64.0f, nw * 0.18f), sh = 6.0f;
        f32 sy = sk->bezel_top * 0.5f - sh * 0.5f;
        L.speaker = shape(cx - sw * 0.5f, sy, sw, sh, sh * 0.5f);
        if (!(sk->marks & MARK_LENS_OFF)) {
            f32 d = 10.0f;
            L.lens = shape(cx - sw * 0.5f - 22.0f - d * 0.5f, sy + sh * 0.5f - d * 0.5f,
                           d, d, d * 0.5f);
        }
    }
    if (sk->marks & MARK_EAR_ROW) {
        /* Level a little under the top bezel's middle: the earpiece over the
         * screen's middle, then the camera and the flash towards the outer
         * edge. */
        f32 cy = sk->bezel_top * 0.57f;
        f32 scx = bez_l + nw * 0.5f;
        L.speaker = shape(scx - EAR_ROW_SPEAKER_W * 0.5f, cy - EAR_ROW_SPEAKER_H * 0.5f,
                          EAR_ROW_SPEAKER_W, EAR_ROW_SPEAKER_H, EAR_ROW_SPEAKER_H * 0.5f);
        L.lens = shape(bw * 0.62f - EAR_ROW_LENS * 0.5f, cy - EAR_ROW_LENS * 0.5f,
                       EAR_ROW_LENS, EAR_ROW_LENS, EAR_ROW_LENS * 0.5f);
        L.flash = shape(bw * 0.80f - EAR_ROW_FLASH * 0.5f, cy - EAR_ROW_FLASH * 0.5f,
                        EAR_ROW_FLASH, EAR_ROW_FLASH, EAR_ROW_FLASH * 0.5f);
    }
    if (sk->marks & MARK_HOME) {
        f32 d = fminf_(sk->bezel_bottom * 0.62f, 70.0f);
        L.home = shape(cx - d * 0.5f, bh - sk->bezel_bottom * 0.5f - d * 0.5f, d, d, d * 0.5f);
    }

    const f32 key_out = sk->kind == BDEV_PHONE ? 2.6f : 2.0f;
    for (i32 k = 0; k < BDEV_MAX_KEYS; k++) {
        const KeySpec *ks = &g_keys[sk->keys][k];
        if (ks->len <= 0.0f) break;
        BrowserDeviceShape s;
        if (ks->side == KEY_TOP) {
            s = shape(bw * ks->at, -key_out, bw * ks->len, key_out + 1.0f, key_out * 0.8f);
        } else {
            f32 x = ks->side == KEY_LEFT ? -key_out : bw - 1.0f;
            s = shape(x, bh * ks->at, key_out + 1.0f, bh * ks->len, key_out * 0.8f);
        }
        L.keys[L.n_keys++] = s;
    }

    if (rotated) {
        turn(&L.body, bw);
        turn(&L.glass, bw);
        turn(&L.screen, bw);
        turn(&L.cutout, bw);
        turn(&L.lens, bw);
        turn(&L.flash, bw);
        turn(&L.speaker, bw);
        turn(&L.hinge, bw);
        turn(&L.knuckles[0], bw);
        turn(&L.knuckles[1], bw);
        turn(&L.home, bw);
        for (i32 k = 0; k < L.n_keys; k++) turn(&L.keys[k], bw);
    }

    /* The system's strips, the page in what is left, and the page's corners
     * rounded only as much as keeps them on the rounded display. */
    f32 in[4];
    bool status;
    system_insets(sk, rotated, in, &status);
    L.sys = sk->sys;
    const BrowserDeviceShape *s = &L.screen;
    L.page = shape(s->x + in[0], s->y + in[1], s->w - in[0] - in[2],
                   s->h - in[1] - in[3], 0.0f);
    const f32 side[4][2] = { { in[0], in[1] }, { in[2], in[1] },
                             { in[2], in[3] }, { in[0], in[3] } };
    for (i32 c = 0; c < 4; c++)
        L.page.r[c] = fmaxf_(0.0f, s->r[c] - fmaxf_(side[c][0], side[c][1]));
    if (status) {
        L.bar = shape(s->x + in[0], s->y, s->w - in[0] - in[2], in[1], 0.0f);
        status_items(&L, sk);
    }
    if (in[3] > 0.0f) {
        f32 span = s->w - in[0] - in[2];
        bool ios = sk->sys == BDEV_SYS_IOS;
        f32 w = fminf_(fmaxf_(span * (ios ? 0.34f : 0.28f), ios ? 72.0f : 60.0f),
                       ios ? 220.0f : 140.0f);
        f32 h = ios ? 5.0f : 4.0f;
        f32 y = ios ? s->y + s->h - 8.0f - h : s->y + s->h - (in[3] + h) * 0.5f;
        L.indicator = shape(s->x + in[0] + (span - w) * 0.5f, y, w, h, h * 0.5f);
    }

    /* What stands under the screen keeps standing: built after the turn. */
    f32 fw = L.body.w, fh = L.body.h;
    if (sk->kind == BDEV_LAPTOP && !rotated) {
        f32 base_w = fw * 1.14f, base_h = fmaxf_(fw * 0.026f, 8.0f);
        L.base = shape(L.body.x + (fw - base_w) * 0.5f, L.body.y + fh, base_w, base_h, 0.0f);
        L.base.r[BDEV_TL] = L.base.r[BDEV_TR] = base_h * 0.15f;
        L.base.r[BDEV_BR] = L.base.r[BDEV_BL] = base_h * 0.55f;
        f32 sw = fw * 0.13f, sh = base_h * 0.32f;
        L.scoop = shape(L.body.x + (fw - sw) * 0.5f, L.base.y, sw, sh, 0.0f);
        L.scoop.r[BDEV_BR] = L.scoop.r[BDEV_BL] = sh;
    } else if (sk->kind == BDEV_MONITOR) {
        f32 m = fminf_(fw, fh);
        f32 neck_w = m * 0.12f, neck_h = m * 0.16f;
        f32 foot_w = m * 0.46f, foot_h = fmaxf_(m * 0.035f, 6.0f);
        L.neck = shape(L.body.x + (fw - neck_w) * 0.5f, L.body.y + fh - 2.0f,
                       neck_w, neck_h + 2.0f, 0.0f);
        L.base = shape(L.body.x + (fw - foot_w) * 0.5f, L.neck.y + L.neck.h,
                       foot_w, foot_h, foot_h * 0.45f);
    }

    f32 x0 = L.body.x, y0 = L.body.y;
    f32 x1 = L.body.x + L.body.w, y1 = L.body.y + L.body.h;
    for (i32 k = 0; k < L.n_keys; k++) grow(&x0, &y0, &x1, &y1, &L.keys[k]);
    grow(&x0, &y0, &x1, &y1, &L.knuckles[0]);
    grow(&x0, &y0, &x1, &y1, &L.knuckles[1]);
    grow(&x0, &y0, &x1, &y1, &L.base);
    grow(&x0, &y0, &x1, &y1, &L.neck);
    f32 ew = x1 - x0, eh = y1 - y0;

    /* Room around the device, and a line above it for the caption when the
     * area is tall enough to spare one. */
    f32 m = fminf_(BDEV_MARGIN_PT, fminf_(aw, ah) * 0.04f);
    f32 top = m;
    bool cap = ah >= 160.0f;
    if (cap) top += BDEV_CAPTION_PT;
    f32 avail_w = aw - 2.0f * m, avail_h = ah - top - m;
    if (avail_w < 1.0f || avail_h < 1.0f) {
        m = 0.0f; top = 0.0f; cap = false;
        avail_w = aw; avail_h = ah;
    }
    f32 k = fminf_(avail_w / ew, avail_h / eh);
    /* Never magnified: a 375 px phone blown up to fill a wide pane would show
     * a device at a size it does not have. */
    if (k > 1.0f) k = 1.0f;
    f32 ox = ax + m + (avail_w - ew * k) * 0.5f;
    f32 oy = ay + top + (avail_h - eh * k) * 0.5f;

    BrowserDeviceShape *parts[] = { &L.body, &L.glass, &L.screen, &L.page, &L.bar,
                                    &L.clock, &L.signal, &L.wifi, &L.battery, &L.percent,
                                    &L.indicator, &L.cutout, &L.lens, &L.flash, &L.speaker,
                                    &L.hinge, &L.knuckles[0], &L.knuckles[1],
                                    &L.home, &L.base, &L.scoop, &L.neck };
    for (usize i = 0; i < sizeof parts / sizeof parts[0]; i++)
        place(parts[i], ox, oy, x0, y0, k);
    for (i32 i = 0; i < L.n_keys; i++) place(&L.keys[i], ox, oy, x0, y0, k);
    L.rim *= k;
    L.clock_pt *= k;
    L.percent_pt *= k;

    L.on = true;
    L.scale = k;
    L.sw = rotated ? p->h : p->w;
    L.sh = rotated ? p->w : p->h;
    L.vw = L.sw - (i32)(in[0] + in[2]);
    L.vh = L.sh - (i32)(in[1] + in[3]);
    /* The screen and the page are exactly their CSS sizes at the fit scale;
     * the bezel arithmetic above must not leave them a rounding error off. */
    L.screen.w = (f32)L.sw * k;
    L.screen.h = (f32)L.sh * k;
    L.page.w = (f32)L.vw * k;
    L.page.h = (f32)L.vh * k;
    L.ext_x = ox;
    L.ext_y = oy;
    L.ext_w = ew * k;
    L.ext_h = eh * k;
    L.cap_room = cap;
    L.cap_y = oy - BDEV_CAPTION_PT + 2.0f;
    *out = L;
    return true;
}
