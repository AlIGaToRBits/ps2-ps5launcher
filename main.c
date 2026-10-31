/* PS5L: оболочка в стиле PS5 для PS2. Этап 1 (без запуска игр).
 *
 * Переключатели сборки (если сборка падает на этих местах):
 *   -DHAVE_CDVD=0      без часов (убрать -lcdvd из Makefile)
 *   -DHAVE_POWEROFF=0  без выключения консоли (убрать -lpoweroff и poweroff_irx.c)
 */
#define NEWLIB_PORT_AWARE
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <fileXio_rpc.h>
#include <libpad.h>
#include <elf-loader.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>

#ifndef HAVE_CDVD
#define HAVE_CDVD 1
#endif
#ifndef HAVE_POWEROFF
#define HAVE_POWEROFF 1
#endif
#if HAVE_CDVD
#include <libcdvd.h>
#endif
#if HAVE_POWEROFF
#include <libpwroff.h>
#endif

#include "anim.h"
#include "font_data.h"

#define ROOT        "mass:/"
#define APP_DIR_DEF ROOT "APPS/PS5L"
#define MAX_GAMES   256
#define MAX_TRACKS  32

/* Разметка в виртуальных координатах: 854x480 (16:9) или 640x480 (4:3) */
#define VH        480.0f
#define ROW_Y     84.0f
#define TILE_S    48.0f
#define TILE_L    84.0f
#define GAP       8.0f
#define ANCHOR_X  44.0f
#define RADIUS    0.13f
#define MARGIN    44.0f

#define IRX(n) extern unsigned char n[]; extern unsigned int size_##n;
IRX(iomanX_irx) IRX(fileXio_irx) IRX(usbd_irx)
IRX(bdm_irx) IRX(bdmfs_fatfs_irx) IRX(usbmass_bd_irx)
#if HAVE_POWEROFF
IRX(poweroff_irx)
#endif

typedef struct {
    char name[64];
    char rel[160];
    char id[16];
    int  is_cd;
    GSTEXTURE icon;
    int  icon_state;               /* 0 = не грузили, 1 = есть, -1 = нет */
    float u0, v0, u1, v1;
    Anim scale;
} Game;

typedef struct {
    int  video, wide, debug, argv0, cwd, slash;
    char extra[64], extra2[64];
    int  lang;                     /* 0 = English, 1 = Русский */
    int  tz;                       /* GMT-сдвиг в минутах */
    int  clock24, volume, music_on, fx_on;
    char track[64];
} Config;
/* video: 0 auto, 1 ntsc, 2 pal, 3 480p */
static Config cfg = { 0, 1, 0, 0, 0, 0, "", "", 1, 180, 1, 80, 1, 1, "" };

static GSGLOBAL *gs;
static GSFONTM  *fontm;
static GSTEXTURE font_tex, logo_tex, lgo_tex;
static int   font_ok, logo_ok, lgo_state;
static Game  games[MAX_GAMES];
static float tile_x[MAX_GAMES];
static int   ngames;
static float VW = 854.0f, kx = 1, ky = 1, dt = 1.0f / 60.0f;
static char  padBuf[256]  __attribute__((aligned(64)));
static char  padBuf2[256] __attribute__((aligned(64)));

#define X(v) ((v) * kx)
#define Y(v) ((v) * ky)
#define RGBA(r, g, b, a) GS_SETREG_RGBAQ(r, g, b, (int)((a) * 128.0f), 0)
#define TXT(r, g, b, a)  RGBA((r) >> 1, (g) >> 1, (b) >> 1, (a))   /* цвет для текстур: 0x80 = 100% */

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float ease_out(float t) { t = 1.0f - clampf(t, 0, 1); return 1.0f - t * t * t; }
static float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3.0f - 2.0f * t); }

static int exists_file(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }
static int exists_dir(const char *p)  { DIR *d = opendir(p); if (!d) return 0; closedir(d); return 1; }

/* ---------- IOP ---------- */
static void iop_init(void) {
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
    SifLoadFileInit();
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    SifExecModuleBuffer(iomanX_irx,  size_iomanX_irx,  0, NULL, NULL);
    SifExecModuleBuffer(fileXio_irx, size_fileXio_irx, 0, NULL, NULL);
    fileXioInit();
#if HAVE_POWEROFF
    SifExecModuleBuffer(poweroff_irx, size_poweroff_irx, 0, NULL, NULL);
    poweroffInit();
#endif
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN",  0, NULL);
    SifExecModuleBuffer(usbd_irx,        size_usbd_irx,        0, NULL, NULL);
    SifExecModuleBuffer(bdm_irx,         size_bdm_irx,         0, NULL, NULL);
    SifExecModuleBuffer(bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL, NULL);
    SifExecModuleBuffer(usbmass_bd_irx,  size_usbmass_bd_irx,  0, NULL, NULL);

    for (int i = 0; i < 50; i++) {
        DIR *d = opendir(ROOT);
        if (d) { closedir(d); break; }
        usleep(100 * 1000);
    }
}

/* ---------- Геймпад ---------- */
static int pad_connected(int port) {
    int st = padGetState(port, 0);
    return st == PAD_STATE_STABLE || st == PAD_STATE_FINDCTP1;
}
static u32 read_port(int port) {
    struct padButtonStatus b;
    if (!pad_connected(port)) return 0;
    if (padRead(port, 0, &b) == 0) return 0;
    return 0xFFFF ^ b.btns;
}
static u32 read_buttons(void) { return read_port(0) | read_port(1); }

static int select_held(void) {              /* SELECT при запуске = безопасный видеорежим */
    for (int i = 0; i < 40; i++) {
        if (read_buttons() & PAD_SELECT) return 1;
        usleep(25 * 1000);
    }
    return 0;
}
static void pads_close(void) { padPortClose(0, 0); padPortClose(1, 0); padEnd(); }
static void pads_open(void)  { padInit(0); padPortOpen(0, 0, padBuf); padPortOpen(1, 0, padBuf2); }

/* повтор при удержании: 1 = сработало */
static int repeat_fire(int down, int pressed, int *held) {
    if (!down) { *held = 0; return 0; }
    if (pressed) { *held = 0; return 1; }
    ++*held;
    return *held > 18 && *held % (*held > 90 ? 3 : 6) == 0;
}

/* ---------- Настройки ---------- */
static char self_path[120] = "(none)";
static char app_dir[128]   = APP_DIR_DEF;
static char cfg_path[160]  = APP_DIR_DEF "/launcher.cfg";
static char cfg_msg[64]    = "cfg: not loaded";
static int  cfg_dirty;

static void norm_dev(const char *src, char *dst, size_t n) {     /* mass0: -> mass: */
    if (!strncasecmp(src, "mass0:", 6)) snprintf(dst, n, "mass:%s", src + 6);
    else snprintf(dst, n, "%s", src);
}
static void set_self(int argc, char *argv[]) {
    if (argc > 0 && argv && argv[0]) snprintf(self_path, sizeof self_path, "%s", argv[0]);
}
static void find_app_dir(void) {
    char tmp[128];
    if (exists_dir(APP_DIR_DEF)) return;
    norm_dev(self_path, tmp, sizeof tmp);
    char *sl = strrchr(tmp, '/');
    if (sl && sl > tmp) {
        *sl = 0;
        if (exists_dir(tmp)) snprintf(app_dir, sizeof app_dir, "%s", tmp);
    }
    snprintf(cfg_path, sizeof cfg_path, "%s/launcher.cfg", app_dir);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}
static void cfg_set(const char *k, const char *v) {
    if (!strcasecmp(k, "video"))
        cfg.video = !strcasecmp(v, "ntsc") ? 1 : !strcasecmp(v, "pal") ? 2 : !strcasecmp(v, "480p") ? 3 : 0;
    else if (!strcasecmp(k, "widescreen")) cfg.wide     = atoi(v);
    else if (!strcasecmp(k, "debug"))      cfg.debug    = atoi(v);
    else if (!strcasecmp(k, "argv0"))      cfg.argv0    = atoi(v);
    else if (!strcasecmp(k, "cwd"))        cfg.cwd      = atoi(v);
    else if (!strcasecmp(k, "slash"))      cfg.slash    = atoi(v);
    else if (!strcasecmp(k, "extra"))      snprintf(cfg.extra,  sizeof cfg.extra,  "%s", v);
    else if (!strcasecmp(k, "extra2"))     snprintf(cfg.extra2, sizeof cfg.extra2, "%s", v);
    else if (!strcasecmp(k, "lang"))       cfg.lang     = !strcasecmp(v, "en") ? 0 : 1;
    else if (!strcasecmp(k, "tz"))         cfg.tz       = (int)clampf((float)atoi(v), -720, 840);
    else if (!strcasecmp(k, "clock24"))    cfg.clock24  = atoi(v) != 0;
    else if (!strcasecmp(k, "volume"))     cfg.volume   = (int)clampf((float)atoi(v), 0, 100);
    else if (!strcasecmp(k, "music"))      cfg.music_on = atoi(v) != 0;
    else if (!strcasecmp(k, "effects"))    cfg.fx_on    = atoi(v) != 0;
    else if (!strcasecmp(k, "track"))      snprintf(cfg.track, sizeof cfg.track, "%s", v);
}
static int parse_cfg(FILE *f) {
    char line[200];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3;                                   /* пропустить BOM */
        p = trim(p);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        cfg_set(trim(p), trim(eq + 1));
        n++;
    }
    return n;
}
static void load_cfg(void) {
    char cand[3][160];
    int opened = 0, got = 0, which = -1;
    snprintf(cand[0], sizeof cand[0], "%s", cfg_path);
    snprintf(cand[1], sizeof cand[1], "%s", APP_DIR_DEF "/launcher.cfg");
    snprintf(cand[2], sizeof cand[2], "%slauncher.cfg", ROOT);
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(cand[i], "r");
        if (!f) continue;
        opened++;
        int n = parse_cfg(f);
        fclose(f);
        if (n) { got = n; which = i; break; }
    }
    if (got)         snprintf(cfg_msg, sizeof cfg_msg, "cfg: OK, %d keys, path %d", got, which);
    else if (opened) snprintf(cfg_msg, sizeof cfg_msg, "cfg: opened, 0 keys read");
    else             snprintf(cfg_msg, sizeof cfg_msg, "cfg: NOT FOUND (defaults)");
}
static int save_cfg(void) {
    static const char *vid[] = { "auto", "ntsc", "pal", "480p" };
    FILE *f = fopen(cfg_path, "w");
    if (!f) return -1;
    fprintf(f, "# PS5L launcher settings\n");
    fprintf(f, "lang=%s\n", cfg.lang ? "ru" : "en");
    fprintf(f, "video=%s\n", vid[cfg.video & 3]);
    fprintf(f, "widescreen=%d\n", cfg.wide);
    fprintf(f, "tz=%d\n", cfg.tz);
    fprintf(f, "clock24=%d\n", cfg.clock24);
    fprintf(f, "volume=%d\n", cfg.volume);
    fprintf(f, "music=%d\n", cfg.music_on);
    fprintf(f, "effects=%d\n", cfg.fx_on);
    fprintf(f, "track=%s\n", cfg.track);
    fprintf(f, "debug=%d\n", cfg.debug);
    fprintf(f, "argv0=%d\ncwd=%d\nslash=%d\n", cfg.argv0, cfg.cwd, cfg.slash);
    fprintf(f, "extra=%s\nextra2=%s\n", cfg.extra, cfg.extra2);
    int bad = ferror(f);
    fclose(f);
    if (!bad) cfg_dirty = 0;
    return bad ? -1 : 0;
}

/* ---------- Графика ---------- */
static int load_png(GSTEXTURE *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->Delayed = 1;
    if (gsKit_texture_png(gs, t, (char *)path) < 0) { t->Mem = NULL; return -1; }
    t->Filter = GS_FILTER_LINEAR;
    return 0;
}
static void free_tex(GSTEXTURE *t) {
    if (t->Mem) { gsKit_TexManager_free(gs, t); free(t->Mem); t->Mem = NULL; }
}
static void img_path(char *dst, size_t n, const char *name) {
    snprintf(dst, n, "%s/img/%s", app_dir, name);
}

static void gfx_init(void) {
    gs = gsKit_init_global();
    switch (cfg.video) {
    case 1: gs->Mode = GS_MODE_NTSC; gs->Interlace = GS_INTERLACED;
            gs->Field = GS_FIELD; gs->Width = 640; gs->Height = 448; break;
    case 2: gs->Mode = GS_MODE_PAL;  gs->Interlace = GS_INTERLACED;
            gs->Field = GS_FIELD; gs->Width = 640; gs->Height = 512; break;
    case 3: gs->Mode = GS_MODE_DTV_480P; gs->Interlace = GS_NONINTERLACED;
            gs->Field = GS_FRAME; gs->Width = 640; gs->Height = 480; break;
    default: break;
    }
    gs->PSM = GS_PSM_CT24;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->PrimAlphaEnable = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
                D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    char p[160];
    img_path(p, sizeof p, "font.png");
    font_ok = (load_png(&font_tex, p) == 0);
    if (!font_ok) {                            /* запасной встроенный шрифт (только латиница) */
        fontm = gsKit_init_fontm();
        gsKit_fontm_upload(gs, fontm);
        fontm->Spacing = 0.8f;
        cfg.lang = 0;
    }
    img_path(p, sizeof p, "logo.png");
    logo_ok = (load_png(&logo_tex, p) == 0);
    gsKit_TexManager_init(gs);

    VW = cfg.wide ? 854.0f : 640.0f;
    kx = gs->Width / VW;
    ky = gs->Height / VH;
    dt = (gs->Mode == GS_MODE_PAL) ? 1.0f / 50.0f : 1.0f / 60.0f;
}

/* ---------- Текст из атласа ---------- */
static const FontGlyph *find_glyph(unsigned cp) {
    int lo = 0, hi = (int)FONT_NGLYPH - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_glyphs[mid].cp == cp) return &font_glyphs[mid];
        if (font_glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
static unsigned utf8_next(const unsigned char **ps) {
    const unsigned char *s = *ps;
    unsigned c = *s++, cp;
    int n;
    if (c < 0x80) { *ps = s; return c; }
    if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *ps = s; return '?'; }
    while (n--) {
        if ((*s & 0xC0) != 0x80) { *ps = s; return '?'; }
        cp = (cp << 6) | (*s++ & 0x3F);
    }
    *ps = s;
    return cp;
}
static float text_w(float h, const char *s) {
    if (!font_ok) return (float)strlen(s) * h * 0.5f;
    float sc = h / FONT_EM, w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        const FontGlyph *g = find_glyph(utf8_next(&p));
        if (!g) g = find_glyph('?');
        if (g) w += g->adv16 * sc / 16.0f;
    }
    return w;
}
/* h = размер шрифта в виртуальных пикселях, (x, y) = левый верх строки; возвращает ширину */
static float text(float x, float y, float h, u64 col, const char *s) {
    if (!font_ok) {
        char buf[96];
        size_t i = 0;
        for (; s[i] && i < sizeof buf - 1; i++) buf[i] = ((unsigned char)s[i] < 0x80) ? s[i] : '?';
        buf[i] = 0;
        gsKit_fontm_print_scaled(gs, fontm, X(x), Y(y), 3, h / 48.0f * ky, col, buf);
        return text_w(h, buf);
    }
    float sc = h / FONT_EM, pen = x;
    const unsigned char *p = (const unsigned char *)s;
    gsKit_TexManager_bind(gs, &font_tex);
    while (*p) {
        const FontGlyph *g = find_glyph(utf8_next(&p));
        if (!g) g = find_glyph('?');
        if (!g) continue;
        if (g->w) {
            float gx = pen + g->xo * sc, gy = y + g->yo * sc;
            gsKit_prim_sprite_texture(gs, &font_tex,
                                      X(gx), Y(gy), g->x, g->y,
                                      X(gx + g->w * sc), Y(gy + g->h * sc), g->x + g->w, g->y + g->h,
                                      3, col);
        }
        pen += g->adv16 * sc / 16.0f;
    }
    return pen - x;
}
static float text_r(float xr, float y, float h, u64 col, const char *s) {   /* по правому краю */
    float w = text_w(h, s);
    text(xr - w, y, h, col, s);
    return w;
}
static float text_c(float xc, float y, float h, u64 col, const char *s) {   /* по центру */
    float w = text_w(h, s);
    text(xc - w * 0.5f, y, h, col, s);
    return w;
}
/* обрезка длинного текста с многоточием */
static void fit_text(char *dst, size_t n, const char *s, float h, float maxw) {
    snprintf(dst, n, "%s", s);
    if (text_w(h, dst) <= maxw) return;
    size_t len = strlen(dst);
    while (len > 1) {
        len--;
        while (len > 0 && ((unsigned char)dst[len] & 0xC0) == 0x80) len--;   /* не рвать UTF-8 */
        dst[len] = 0;
        char tmp[96];
        snprintf(tmp, sizeof tmp, "%s...", dst);
        if (text_w(h, tmp) <= maxw) { snprintf(dst, n, "%s", tmp); return; }
    }
}

/* ---------- Примитивы ---------- */
static void fill_rect(float x, float y, float w, float h, u64 col) {
    gsKit_prim_sprite(gs, X(x), Y(y), X(x + w), Y(y + h), 2, col);
}
static void draw_full(GSTEXTURE *t, float a) {
    if (!t->Mem || a <= 0.0f) return;
    gsKit_TexManager_bind(gs, t);
    gsKit_prim_sprite_texture(gs, t, 0, 0, 0, 0, gs->Width, gs->Height,
                              t->Width, t->Height, 1, RGBA(0x80, 0x80, 0x80, a));
}
static void draw_tex_box(GSTEXTURE *t, float x, float y, float w, float h, float a) {
    if (!t->Mem || a <= 0.0f) return;
    gsKit_TexManager_bind(gs, t);
    gsKit_prim_sprite_texture(gs, t, X(x), Y(y), 0, 0, X(x + w), Y(y + h),
                              t->Width, t->Height, 3, RGBA(0x80, 0x80, 0x80, a));
}

#define SEG  4
#define NPTS (4 * (SEG + 1))
static float cs_tab[NPTS], sn_tab[NPTS];

static void rr_init(void) {
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= SEG; i++, n++) {
            float a = ((c - 1) * 90.0f + i * 90.0f / SEG) * 3.14159265f / 180.0f;
            cs_tab[n] = cosf(a);
            sn_tab[n] = sinf(a);
        }
}
static void rr_points(float x, float y, float w, float h, float r, float *px, float *py) {
    float cx[4] = { x + w - r, x + w - r, x + r,     x + r };
    float cy[4] = { y + r,     y + h - r, y + h - r, y + r };
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= SEG; i++, n++) {
            px[n] = X(cx[c] + cs_tab[n] * r);
            py[n] = Y(cy[c] + sn_tab[n] * r);
        }
}
static void rr_fill(float x, float y, float w, float h, float r, u64 col) {
    float px[NPTS], py[NPTS];
    rr_points(x, y, w, h, r, px, py);
    float cx = X(x + w * 0.5f), cy = Y(y + h * 0.5f);
    for (int i = 0; i < NPTS; i++) {
        int j = (i + 1) % NPTS;
        gsKit_prim_triangle(gs, cx, cy, px[i], py[i], px[j], py[j], 2, col);
    }
}
static void rr_tex(GSTEXTURE *t, float x, float y, float w, float h, float r,
                   float u0, float v0, float u1, float v1, float a) {
    if (!t->Mem) return;
    float px[NPTS], py[NPTS], pu[NPTS], pv[NPTS];
    rr_points(x, y, w, h, r, px, py);
    float sx = X(x), sy = Y(y), sw = X(w), sh = Y(h);
    for (int i = 0; i < NPTS; i++) {
        pu[i] = u0 + (px[i] - sx) / sw * (u1 - u0);
        pv[i] = v0 + (py[i] - sy) / sh * (v1 - v0);
    }
    float cx = sx + sw * 0.5f, cy = sy + sh * 0.5f;
    float cu = (u0 + u1) * 0.5f, cv = (v0 + v1) * 0.5f;
    u64 col = RGBA(0x80, 0x80, 0x80, a);
    gsKit_TexManager_bind(gs, t);
    for (int i = 0; i < NPTS; i++) {
        int j = (i + 1) % NPTS;
        gsKit_prim_triangle_texture(gs, t, cx, cy, cu, cv,
                                    px[i], py[i], pu[i], pv[i],
                                    px[j], py[j], pu[j], pv[j], 2, col);
    }
}

/* Шестерёнка: 8 зубцов, вращается на ang градусов */
static void draw_gear(float cx, float cy, float R, float ang, u64 col) {
    enum { TEETH = 8, PER = 4, N = TEETH * PER };
    static const float off[PER] = { 0.00f, 0.18f, 0.42f, 0.60f };
    static const float rad[PER] = { 0.74f, 1.00f, 1.00f, 0.74f };
    float ox[N], oy[N], ix[N], iy[N];
    float a0 = ang * 3.14159265f / 180.0f, step = 6.2831853f / TEETH;
    for (int t = 0; t < TEETH; t++)
        for (int k = 0; k < PER; k++) {
            int n = t * PER + k;
            float a = a0 + (t + off[k]) * step;
            ox[n] = cx + cosf(a) * R * rad[k];  oy[n] = cy + sinf(a) * R * rad[k];
            ix[n] = cx + cosf(a) * R * 0.36f;   iy[n] = cy + sinf(a) * R * 0.36f;
        }
    for (int n = 0; n < N; n++) {
        int m = (n + 1) % N;
        gsKit_prim_triangle(gs, X(ox[n]), Y(oy[n]), X(ox[m]), Y(oy[m]), X(ix[n]), Y(iy[n]), 2, col);
        gsKit_prim_triangle(gs, X(ox[m]), Y(oy[m]), X(ix[m]), Y(iy[m]), X(ix[n]), Y(iy[n]), 2, col);
    }
}
/* Значок геймпада 30x20 */
static void draw_pad_icon(float x, float y, int on) {
    u64 col = on ? RGBA(255, 255, 255, 1) : RGBA(150, 150, 160, 0.5f);
    u64 dk  = RGBA(0x10, 0x14, 0x20, on ? 0.9f : 0.45f);
    rr_fill(x,      y + 2, 30, 14, 7, col);
    rr_fill(x + 1,  y + 8, 9,  11, 4, col);
    rr_fill(x + 20, y + 8, 9,  11, 4, col);
    fill_rect(x + 7, y + 8, 6, 2, dk);
    fill_rect(x + 9, y + 6, 2, 6, dk);
    rr_fill(x + 19, y + 7, 3, 3, 1.5f, dk);
    rr_fill(x + 23, y + 9, 3, 3, 1.5f, dk);
}

/* ---------- Игры ---------- */
static void scan_dir(const char *sub, int is_cd) {
    char path[64];
    snprintf(path, sizeof path, ROOT "%s", sub);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ngames < MAX_GAMES) {
        const char *n = e->d_name;
        size_t L = strlen(n);
        if (L < 5 || L > 150 || strcasecmp(n + L - 4, ".iso")) continue;
        Game *g = &games[ngames++];
        memset(g, 0, sizeof *g);
        snprintf(g->rel, sizeof g->rel, "%s/%.150s", sub, n);
        g->is_cd = is_cd;
        if (L > 16 && n[4] == '_' && n[8] == '.' && n[11] == '.') {
            memcpy(g->id, n, 11); g->id[11] = 0;
            snprintf(g->name, sizeof g->name, "%.*s", (int)(L - 16), n + 12);
        } else {
            snprintf(g->name, sizeof g->name, "%.*s", (int)(L - 4), n);
        }
        anim_init(&g->scale, 0, 18);
    }
    closedir(d);
}
static int cmp_game(const void *a, const void *b) {
    return strcasecmp(((const Game *)a)->name, ((const Game *)b)->name);
}
/* Порядок поиска: _SQR (подготовленный квадрат) -> _COV (обложка) -> _ICO (диск) */
static int load_art(Game *g) {
    static const char *suf[] = { "SQR", "COV", "ICO" };
    char p[128];
    if (!g->id[0]) return -1;
    for (int k = 0; k < 3; k++) {
        snprintf(p, sizeof p, ROOT "ART/%s_%s.png", g->id, suf[k]);
        if (load_png(&g->icon, p) == 0) {
            float w = g->icon.Width, h = g->icon.Height;
            if (h > w) { float o = (h - w) * 0.5f; g->u0 = 0; g->u1 = w; g->v0 = o; g->v1 = o + w; }
            else       { float o = (w - h) * 0.5f; g->u0 = o; g->u1 = o + h; g->v0 = 0; g->v1 = h; }
            return 0;
        }
    }
    return -1;
}
static void stream_icons(int sel) {
    for (int i = 0; i < ngames; i++) {
        int d = abs(i - sel);
        if (d > ngames / 2) d = ngames - d;                 /* расстояние по кругу */
        if (d > 16 && games[i].icon_state == 1) { free_tex(&games[i].icon); games[i].icon_state = 0; }
    }
    static const int order[] = {0, 1, -1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -2, -3, -4};
    for (unsigned k = 0; k < sizeof order / sizeof *order; k++) {
        int i = ((sel + order[k]) % ngames + ngames) % ngames;
        if (games[i].icon_state) continue;
        games[i].icon_state = (load_art(&games[i]) == 0) ? 1 : -1;
        return;
    }
}

/* ---------- Часы консоли ---------- */
static int clk_ok, clk_min;          /* минуты от полуночи по времени консоли (JST) */
#define BCD(b) ((((b) >> 4) & 0x0F) * 10 + ((b) & 0x0F))
static void clock_init(void) {
#if HAVE_CDVD
    sceCdInit(SCECdINoD);
#endif
}
static void clock_update(void) {
#if HAVE_CDVD
    sceCdCLOCK ck;
    memset(&ck, 0, sizeof ck);
    if (sceCdReadClock(&ck)) {
        int h = BCD(ck.hour), m = BCD(ck.minute);
        if (h < 24 && m < 60) { clk_min = h * 60 + m; clk_ok = 1; return; }
    }
#endif
    clk_ok = 0;
}
static void clock_str(char *dst, size_t n) {
    if (!clk_ok) { snprintf(dst, n, "--:--"); return; }
    int t = ((clk_min - 9 * 60 + cfg.tz) % 1440 + 1440) % 1440;   /* RTC консоли идёт по Японии (GMT+9) */
    int h = t / 60, m = t % 60;
    if (cfg.clock24) snprintf(dst, n, "%02d:%02d", h, m);
    else snprintf(dst, n, "%d:%02d %s", (h % 12) ? (h % 12) : 12, m, h < 12 ? "AM" : "PM");
}
static void tz_str(char *dst, size_t n) {
    int a = abs(cfg.tz);
    if (a % 60) snprintf(dst, n, "GMT%c%d:%02d", cfg.tz < 0 ? '-' : '+', a / 60, a % 60);
    else        snprintf(dst, n, "GMT%c%d", cfg.tz < 0 ? '-' : '+', a / 60);
}

/* ---------- Тексты (EN / RU) ---------- */
enum {
    L_GAMES, L_APPS, L_FILES, L_SETTINGS, L_SCREEN, L_SOUND, L_SAVE, L_RESTART, L_EXIT, L_POWEROFF,
    L_LANGUAGE, L_TZ, L_CLOCKFMT, L_VIDEO, L_ASPECT, L_BACK, L_VOLUME, L_MUSIC, L_EFFECTS, L_TRACK,
    L_ON, L_OFF, L_UNSAVED, L_SAVED, L_SAVEFAIL, L_NOGAMES, L_SRCSOON, L_LAUNCHSOON, L_SOON_PAGE,
    L_ASK_EXIT, L_ASK_POWER, L_YES, L_NO, L_NEEDRESTART, L_NOFONT, L_NOTRACKS, L_NOPOWEROFF,
    L_RESTARTFAIL, L_SOUNDSOON, L_HINT_GAMES, L_HINT_MENU, L_HINT_CONFIRM, L_PLAY, L_COUNT
};
static const char *const tr_tab[L_COUNT][2] = {
    [L_GAMES]       = { "Games", "Игры" },
    [L_APPS]        = { "Apps", "Приложения" },
    [L_FILES]       = { "Files", "Файлы" },
    [L_SETTINGS]    = { "Settings", "Настройки" },
    [L_SCREEN]      = { "Screen settings", "Настройки экрана" },
    [L_SOUND]       = { "Sound settings", "Настройки звука" },
    [L_SAVE]        = { "Save settings", "Сохранить настройки" },
    [L_RESTART]     = { "Restart launcher", "Перезапустить лаунчер" },
    [L_EXIT]        = { "Exit", "Выход" },
    [L_POWEROFF]    = { "Power off", "Выключить консоль" },
    [L_LANGUAGE]    = { "Language", "Язык" },
    [L_TZ]          = { "Time zone", "Часовой пояс" },
    [L_CLOCKFMT]    = { "Clock format", "Формат часов" },
    [L_VIDEO]       = { "Video mode", "Видеорежим" },
    [L_ASPECT]      = { "Screen format", "Формат экрана" },
    [L_BACK]        = { "Back", "Назад" },
    [L_VOLUME]      = { "Master volume", "Общая громкость" },
    [L_MUSIC]       = { "Music / Theme", "Музыка / Тема" },
    [L_EFFECTS]     = { "Effects", "Эффекты" },
    [L_TRACK]       = { "Music track", "Выбор музыки" },
    [L_ON]          = { "On", "Вкл" },
    [L_OFF]         = { "Off", "Выкл" },
    [L_UNSAVED]     = { " (unsaved)", " (не сохранено)" },
    [L_SAVED]       = { "Settings saved", "Настройки сохранены" },
    [L_SAVEFAIL]    = { "Could not write launcher.cfg", "Не удалось записать launcher.cfg" },
    [L_NOGAMES]     = { "No games in mass:/DVD or mass:/CD", "Нет игр в mass:/DVD и mass:/CD" },
    [L_SRCSOON]     = { "This source is not supported yet", "Этот источник пока не поддерживается" },
    [L_LAUNCHSOON]  = { "Game launch will be added later", "Запуск игр будет добавлен позже" },
    [L_SOON_PAGE]   = { "This section is coming in a later update", "Раздел появится в одном из следующих обновлений" },
    [L_ASK_EXIT]    = { "Exit to the PS2 menu?", "Выйти в меню PS2?" },
    [L_ASK_POWER]   = { "Power off the console?", "Выключить консоль?" },
    [L_YES]         = { "Yes", "Да" },
    [L_NO]          = { "No", "Нет" },
    [L_NEEDRESTART] = { "Applies after restart", "Применится после перезапуска" },
    [L_NOFONT]      = { "img/font.png not found: English only", "Нет img/font.png: только английский" },
    [L_NOTRACKS]    = { "No tracks in sound/music", "Нет треков в sound/music" },
    [L_NOPOWEROFF]  = { "Power off is not available in this build", "В этой сборке выключение недоступно" },
    [L_RESTARTFAIL] = { "Could not start the launcher ELF", "Не удалось запустить ELF лаунчера" },
    [L_SOUNDSOON]   = { "Sound playback is coming in the next stage", "Воспроизведение звука появится на следующем этапе" },
    [L_HINT_GAMES]  = { "\xE2\x9C\x95 Play     L1 / R1 Tabs     START Settings", "\xE2\x9C\x95 Играть     L1 / R1 Вкладки     START Настройки" },
    [L_HINT_MENU]   = { "\xE2\x96\xB2\xE2\x96\xBC Select     \xE2\x97\x84\xE2\x96\xBA Change     \xE2\x9C\x95 OK     \xE2\x97\x8B Back",
                        "\xE2\x96\xB2\xE2\x96\xBC Выбор     \xE2\x97\x84\xE2\x96\xBA Изменить     \xE2\x9C\x95 OK     \xE2\x97\x8B Назад" },
    [L_HINT_CONFIRM]= { "\xE2\x9C\x95 Yes     \xE2\x97\x8B No", "\xE2\x9C\x95 Да     \xE2\x97\x8B Нет" },
    [L_PLAY]        = { "PS2", "PS2" },
};
#define T(id) (tr_tab[(id)][cfg.lang ? 1 : 0])

/* ---------- Уведомление ---------- */
static char  toast_msg[96];
static float toast_t;
static void toast(const char *s) { snprintf(toast_msg, sizeof toast_msg, "%s", s); toast_t = 2.6f; }

/* ---------- Выход, перезапуск, выключение ---------- */
static void do_exit(void) { pads_close(); Exit(0); }
static void do_poweroff(void) {
#if HAVE_POWEROFF
    pads_close();
    poweroffShutdown();
#else
    toast(T(L_NOPOWEROFF));
#endif
}
static void do_restart(void) {
    char p[160];
    char *av[1];
    save_cfg();
    norm_dev(self_path, p, sizeof p);
    if (!strcmp(self_path, "(none)")) snprintf(p, sizeof p, "%s/launcher.elf", app_dir);
    av[0] = p;
    pads_close();
    LoadELFFromFile(p, 1, av);
    pads_open();                                   /* сюда попадаем, только если запуск не удался */
    toast(T(L_RESTARTFAIL));
}

/* ---------- Меню настроек ---------- */
enum { PG_ROOT, PG_SCREEN, PG_SOUND };
static int   pg, cur[3];
static int   set_open;                              /* 1 = меню открыто (или открывается) */
static float set_anim;                              /* 0..1: вращение шестерёнки и появление панели */
static int   confirm;                               /* 0 нет, 1 выход, 2 выключение */
static char  tracks[MAX_TRACKS][48];
static int   ntracks;

static void scan_tracks(void) {
    char path[160];
    ntracks = 0;
    snprintf(path, sizeof path, "%s/sound/music", app_dir);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && ntracks < MAX_TRACKS) {
        size_t L = strlen(e->d_name);
        if (L < 5 || L > 47 || strcasecmp(e->d_name + L - 4, ".wav")) continue;
        snprintf(tracks[ntracks++], sizeof tracks[0], "%.47s", e->d_name);
    }
    closedir(d);
}
static int page_count(int p) { return p == PG_ROOT ? 6 : p == PG_SCREEN ? 6 : 5; }
static const char *page_title(int p) {
    return p == PG_ROOT ? T(L_SETTINGS) : p == PG_SCREEN ? T(L_SCREEN) : T(L_SOUND);
}
static const char *item_label(int p, int i) {
    static const int root[]   = { L_SCREEN, L_SOUND, L_SAVE, L_RESTART, L_EXIT, L_POWEROFF };
    static const int screen[] = { L_LANGUAGE, L_TZ, L_CLOCKFMT, L_VIDEO, L_ASPECT, L_BACK };
    static const int sound[]  = { L_VOLUME, L_MUSIC, L_EFFECTS, L_TRACK, L_BACK };
    return T(p == PG_ROOT ? root[i] : p == PG_SCREEN ? screen[i] : sound[i]);
}
/* значение справа; пустая строка = у пункта нет значения */
static void item_value(int p, int i, char *out, size_t n) {
    static const char *vid[] = { "Auto", "NTSC", "PAL", "480p" };
    out[0] = 0;
    if (p == PG_SCREEN) {
        char tz[24];
        switch (i) {
        case 0: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.lang ? "Русский" : "English"); break;
        case 1: tz_str(tz, sizeof tz); snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", tz); break;
        case 2: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.clock24 ? "24" : "12 AM/PM"); break;
        case 3: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", vid[cfg.video & 3]); break;
        case 4: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.wide ? "16:9" : "4:3"); break;
        }
    } else if (p == PG_SOUND) {
        switch (i) {
        case 0: snprintf(out, n, "\xE2\x97\x84 %d%% \xE2\x96\xBA", cfg.volume); break;
        case 1: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.music_on ? T(L_ON) : T(L_OFF)); break;
        case 2: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.fx_on ? T(L_ON) : T(L_OFF)); break;
        case 3: snprintf(out, n, "\xE2\x97\x84 %.28s \xE2\x96\xBA", cfg.track[0] ? cfg.track : "-"); break;
        }
    } else if (p == PG_ROOT && i == 2 && cfg_dirty) {
        snprintf(out, n, "%s", T(L_UNSAVED) + 1);
    }
}
static void item_adjust(int p, int i, int dir) {
    int changed = 1;
    if (p == PG_SCREEN) {
        switch (i) {
        case 0:
            if (!font_ok) { toast(T(L_NOFONT)); changed = 0; }
            else cfg.lang ^= 1;
            break;
        case 1: cfg.tz = (int)clampf((float)(cfg.tz + dir * 30), -720, 840); break;
        case 2: cfg.clock24 ^= 1; break;
        case 3: cfg.video = (cfg.video + dir + 4) & 3; toast(T(L_NEEDRESTART)); break;
        case 4: cfg.wide ^= 1; toast(T(L_NEEDRESTART)); break;
        default: changed = 0;
        }
    } else if (p == PG_SOUND) {
        switch (i) {
        case 0: cfg.volume = (int)clampf((float)(cfg.volume + dir * 10), 0, 100); break;
        case 1: cfg.music_on ^= 1; break;
        case 2: cfg.fx_on ^= 1; break;
        case 3:
            if (!ntracks) { toast(T(L_NOTRACKS)); changed = 0; break; }
            {
                int c = 0;
                for (int k = 0; k < ntracks; k++) if (!strcmp(tracks[k], cfg.track)) c = k;
                if (!cfg.track[0]) c = dir > 0 ? -1 : 0;
                c = ((c + dir) % ntracks + ntracks) % ntracks;
                snprintf(cfg.track, sizeof cfg.track, "%s", tracks[c]);
            }
            break;
        default: changed = 0;
        }
        if (changed && i != 3) toast(T(L_SOUNDSOON));
    } else changed = 0;
    if (changed) cfg_dirty = 1;
}
static void item_press(int p, int i) {
    if (p == PG_ROOT) {
        switch (i) {
        case 0: pg = PG_SCREEN; cur[PG_SCREEN] = 0; break;
        case 1: pg = PG_SOUND;  cur[PG_SOUND] = 0; scan_tracks(); break;
        case 2: toast(save_cfg() == 0 ? T(L_SAVED) : T(L_SAVEFAIL)); break;
        case 3: do_restart(); break;
        case 4: confirm = 1; break;
        case 5: confirm = 2; break;
        }
    } else if (p == PG_SCREEN) {
        if (i == 5) pg = PG_ROOT; else item_adjust(p, i, 1);
    } else {
        if (i == 4) pg = PG_ROOT; else item_adjust(p, i, 1);
    }
}

static void draw_settings(float a) {
    gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 4, RGBA(0, 0, 0, 0.62f * a));
    int n = page_count(pg);
    float rowh = 42, pw = 520, px = (VW - pw) * 0.5f, py = 50;
    float ph = 74 + n * rowh + 14, slide = (1.0f - a) * 26;
    rr_fill(px, py + slide, pw, ph, 16, RGBA(0x12, 0x16, 0x24, 1.0f * a));
    text(px + 30, py + slide + 20, 26, TXT(255, 255, 255, a), page_title(pg));
    fill_rect(px + 28, py + slide + 62, pw - 56, 1, RGBA(255, 255, 255, 0.18f * a));
    for (int i = 0; i < n; i++) {
        float y = py + slide + 74 + i * rowh;
        char val[64];
        item_value(pg, i, val, sizeof val);
        if (i == cur[pg]) {
            rr_fill(px + 14, y, pw - 28, rowh - 6, 10, RGBA(255, 255, 255, 0.17f * a));
            rr_fill(px + 14, y + 6, 4, rowh - 18, 2, RGBA(255, 255, 255, 0.9f * a));
        }
        float ta = (i == cur[pg]) ? 1.0f : 0.72f;
        text(px + 34, y + 8, 22, TXT(255, 255, 255, ta * a), item_label(pg, i));
        if (val[0]) text_r(px + pw - 34, y + 8, 22, TXT(255, 255, 255, ta * a), val);
    }
    text_c(VW * 0.5f, VH - 36, 17, TXT(255, 255, 255, 0.7f * a), T(L_HINT_MENU));
}
static void draw_confirm(void) {
    gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 5, RGBA(0, 0, 0, 0.55f));
    float w = 420, h = 130, x = (VW - w) * 0.5f, y = 170;
    rr_fill(x, y, w, h, 16, RGBA(0x16, 0x1A, 0x2A, 0.98f));
    text_c(VW * 0.5f, y + 28, 24, TXT(255, 255, 255, 1), confirm == 1 ? T(L_ASK_EXIT) : T(L_ASK_POWER));
    text_c(VW * 0.5f, y + 82, 20, TXT(255, 255, 255, 0.75f), T(L_HINT_CONFIRM));
}

/* ---------- Заставка ---------- */
typedef struct { float x, y, vx, vy, age, life, size; int blue; } Particle;
#define NPART 110
static Particle part[NPART];
static float intro_t;
static int   intro_on = 1, parts_made;

static float t_part(void) { return logo_ok ? 2.2f : 0.5f; }

static void parts_spawn(void) {
    for (int i = 0; i < NPART; i++) {
        Particle *p = &part[i];
        float a = (float)(rand() % 6283) / 1000.0f;
        float sp = 70.0f + (float)(rand() % 220);
        p->x = VW * 0.5f + cosf(a) * 8.0f;
        p->y = VH * 0.5f + sinf(a) * 8.0f;
        p->vx = cosf(a) * sp;
        p->vy = sinf(a) * sp * 0.7f;
        p->age = -(float)(rand() % 500) / 1000.0f;       /* разный момент появления */
        p->life = 1.3f + (float)(rand() % 1000) / 1000.0f;
        p->size = 1.5f + (float)(rand() % 30) / 10.0f;
        p->blue = (rand() % 100) < 40;
    }
    parts_made = 1;
}
static void intro_update(void) {
    intro_t += dt;
    if (intro_t >= t_part() && !parts_made) parts_spawn();
    if (parts_made)
        for (int i = 0; i < NPART; i++) {
            Particle *p = &part[i];
            p->age += dt;
            if (p->age < 0) continue;
            float k = 0.9f * dt;                          /* лёгкая закрутка */
            float vx = p->vx, vy = p->vy;
            p->vx += -vy * k;  p->vy += vx * k;
            p->x += p->vx * dt;  p->y += p->vy * dt;
        }
    if (intro_t >= t_part() + 2.6f) intro_on = 0;
}
static float intro_cover(void) {        /* непрозрачность чёрного поверх меню */
    float t0 = t_part() + 0.3f;
    return 1.0f - smooth01((intro_t - t0) / 1.3f);
}
static void draw_intro(void) {
    float cover = intro_cover();
    if (cover > 0) gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 6, RGBA(0, 0, 0, cover));
    if (logo_ok) {
        float a = smooth01((intro_t - 0.4f) / 0.7f) * (1.0f - smooth01((intro_t - 2.0f) / 0.5f));
        float w = 220, h = w * logo_tex.Height / (float)logo_tex.Width;
        draw_tex_box(&logo_tex, (VW - w) * 0.5f, (VH - h) * 0.5f, w, h, a);
    }
    if (!parts_made) return;
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 2, 0, 1, 0), 0);        /* сложение цветов: свечение */
    for (int i = 0; i < NPART; i++) {
        Particle *p = &part[i];
        if (p->age < 0 || p->age > p->life) continue;
        float a = sinf(3.14159265f * p->age / p->life);
        int r = p->blue ? 70 : 255, g = p->blue ? 150 : 200, b = p->blue ? 255 : 90;
        float s = p->size * (0.6f + 0.4f * a);
        fill_rect(p->x - s * 2.2f, p->y - s * 2.2f, s * 4.4f, s * 4.4f, RGBA(r, g, b, 0.10f * a));
        fill_rect(p->x - s * 0.5f, p->y - s * 0.5f, s, s, RGBA(r, g, b, 0.95f * a));
    }
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
}

/* ---------- Главный цикл ---------- */
static const char *const SRC_NAME[5] = { "USB", "MX4SIO", "ETH", "HDD", "iLink" };

int main(int argc, char *argv[]) {
    iop_init();
    pads_open();
    set_self(argc, argv);
    find_app_dir();
    load_cfg();
    if (select_held()) cfg.video = 0;
    gfx_init();
    rr_init();
    clock_init();
    srand(12345);

    scan_dir("DVD", 0);
    scan_dir("CD", 1);
    qsort(games, ngames, sizeof(Game), cmp_game);

    int sel = 0, idle = 0, bgFront = 0, bgFor = -1, lgoFor = -1, lap = 0;
    int tab = 0, src = 0, hold[4] = { 0, 0, 0, 0 }, pad_on[4] = { 0, 0, 0, 0 };
    u32 prev = 0xFFFF;
    unsigned frame = 0;
    GSTEXTURE bg[2];
    memset(bg, 0, sizeof bg);
    Anim cam, titleA, titleY, bgFade, tabPos, tabFade, srcA[5];
    anim_init(&cam, 0, 14);
    anim_init(&titleA, 1, 10);
    anim_init(&titleY, 0, 10);
    anim_init(&bgFade, 1, 6);
    anim_init(&tabPos, 0, 14);
    anim_init(&tabFade, 1, 12);
    for (int i = 0; i < 5; i++) anim_init(&srcA[i], i == 0 ? 1 : 0, 14);
    clock_update();

    for (;;) {
        /* --- ввод --- */
        u32 btn = read_buttons();
        u32 pressed = btn & ~prev;
        prev = btn;
        int fL = repeat_fire(btn & PAD_LEFT,  pressed & PAD_LEFT,  &hold[0]);
        int fR = repeat_fire(btn & PAD_RIGHT, pressed & PAD_RIGHT, &hold[1]);
        int fU = repeat_fire(btn & PAD_UP,    pressed & PAD_UP,    &hold[2]);
        int fD = repeat_fire(btn & PAD_DOWN,  pressed & PAD_DOWN,  &hold[3]);
        int nvis = (src == 0) ? ngames : 0;

        if (intro_on) {
            if (pressed & (PAD_CROSS | PAD_START)) intro_t = t_part() + 3.0f;
        } else if (confirm) {
            if (pressed & PAD_CROSS) { int c = confirm; confirm = 0; if (c == 1) do_exit(); else do_poweroff(); }
            if (pressed & PAD_CIRCLE) confirm = 0;
        } else if (set_open) {
            int n = page_count(pg);
            if (fU) cur[pg] = (cur[pg] + n - 1) % n;
            if (fD) cur[pg] = (cur[pg] + 1) % n;
            if (fL) item_adjust(pg, cur[pg], -1);
            if (fR) item_adjust(pg, cur[pg], +1);
            if (pressed & PAD_CROSS) item_press(pg, cur[pg]);
            if (pressed & PAD_CIRCLE) { if (pg != PG_ROOT) pg = PG_ROOT; else set_open = 0; }
            if (pressed & PAD_START) set_open = 0;
        } else {
            if (pressed & PAD_START) { set_open = 1; pg = PG_ROOT; }
            if (pressed & PAD_L1) { tab = (tab + 2) % 3; tabFade.v = 0; anim_to(&tabFade, 1); }
            if (pressed & PAD_R1) { tab = (tab + 1) % 3; tabFade.v = 0; anim_to(&tabFade, 1); }
            if (tab == 0) {
                int move = fR ? 1 : fL ? -1 : 0;
                if (move && nvis > 0) {
                    int ns = sel + move;
                    int wrap = 0;
                    if (ns >= nvis) { ns = 0; wrap = 1; } else if (ns < 0) { ns = nvis - 1; wrap = -1; }
                    float total = 0;
                    for (int i = 0; i < nvis; i++) total += lerpf(TILE_S, TILE_L, games[i].scale.v) + GAP;
                    if (total >= VW) lap += wrap;                    /* бесшовная петля только если список длиннее экрана */
                    sel = ns; idle = 0;
                    titleA.v = 0; anim_to(&titleA, 1);
                    titleY.v = 8; anim_to(&titleY, 0);
                    lgo_state = 0;
                }
                int ds = fD ? 1 : fU ? -1 : 0;
                if (ds) {
                    src = (src + ds + 5) % 5;
                    sel = 0; lap = 0; idle = 0; cam.v = cam.t = 0;
                }
                if ((pressed & PAD_CROSS) && nvis > 0) toast(T(L_LAUNCHSOON));
            }
        }
        idle++;

        /* --- анимации и состояние --- */
        if (intro_on) intro_update();
        else if (logo_ok) { free_tex(&logo_tex); logo_ok = 0; }
        if (set_open) set_anim = clampf(set_anim + dt / 0.55f, 0, 1);
        else          set_anim = clampf(set_anim - dt / 0.30f, 0, 1);
        if (toast_t > 0) toast_t -= dt;
        if (frame % 30 == 0) clock_update();
        if (frame % 20 == 0) { pad_on[0] = pad_connected(0); pad_on[1] = pad_connected(1); }

        anim_to(&tabPos, (float)tab);
        anim_step(&tabPos, dt);
        anim_step(&tabFade, dt);
        for (int i = 0; i < 5; i++) { anim_to(&srcA[i], i == src ? 1.0f : 0.0f); anim_step(&srcA[i], dt); }

        int nvis2 = (src == 0) ? ngames : 0;
        float x = 0;
        for (int i = 0; i < ngames; i++) {
            anim_to(&games[i].scale, (i == sel && nvis2) ? 1.0f : 0.0f);
            anim_step(&games[i].scale, dt);
            tile_x[i] = x;
            x += lerpf(TILE_S, TILE_L, games[i].scale.v) + GAP;
        }
        float total = x;
        int looping = (nvis2 > 1 && total >= VW);
        if (nvis2) anim_to(&cam, tile_x[sel] + (looping ? lap * total : 0.0f));
        anim_step(&cam, dt);
        while (looping && lap > 0 && cam.v >= total) { cam.v -= total; cam.t -= total; lap--; }
        while (looping && lap < 0 && cam.v < 0)      { cam.v += total; cam.t += total; lap++; }
        if (!looping) lap = 0;
        anim_step(&titleA, dt);
        anim_step(&titleY, dt);
        anim_step(&bgFade, dt);

        /* --- подгрузка --- */
        if (nvis2 && idle == 12 && bgFor != sel) {
            int back = bgFront ^ 1;
            free_tex(&bg[back]);
            if (games[sel].id[0]) {
                char p[96];
                snprintf(p, sizeof p, ROOT "ART/%s_BG.png", games[sel].id);
                load_png(&bg[back], p);
            }
            bgFront = back; bgFor = sel;
            bgFade.v = 0; anim_to(&bgFade, 1);
        } else if (nvis2 && idle == 14 && lgoFor != sel) {
            free_tex(&lgo_tex);
            lgo_state = 0;
            if (games[sel].id[0]) {
                char p[96];
                snprintf(p, sizeof p, ROOT "ART/%s_LGO.png", games[sel].id);
                if (load_png(&lgo_tex, p) == 0) lgo_state = 1;
            }
            lgoFor = sel;
        } else if (nvis2 && idle > 2) {
            stream_icons(sel);
        }

        /* --- отрисовка --- */
        float W = gs->Width, H = gs->Height;
        gsKit_clear(gs, RGBA(0x08, 0x0C, 0x18, 1));
        draw_full(&bg[bgFront ^ 1], 1.0f - bgFade.v);
        draw_full(&bg[bgFront], bgFade.v);

        u64 dark = RGBA(0, 0, 0, 0.7f), none = RGBA(0, 0, 0, 0);
        gsKit_prim_quad_gouraud(gs, 0, 0, 0, H, X(VW * 0.6f), 0, X(VW * 0.6f), H, 1,
                                dark, dark, none, none);
        gsKit_prim_quad_gouraud(gs, 0, H, W, H, 0, Y(250), W, Y(250), 1,
                                RGBA(0, 0, 0, 0.8f), RGBA(0, 0, 0, 0.8f), none, none);
        gsKit_prim_quad_gouraud(gs, 0, 0, W, 0, 0, Y(140), W, Y(140), 1,
                                RGBA(0, 0, 0, 0.6f), RGBA(0, 0, 0, 0.6f), none, none);

        /* вкладки по центру */
        {
            const char *names[3] = { T(L_GAMES), T(L_APPS), T(L_FILES) };
            float wd[3], cxs[3], total_w = 0, spacing = 56;
            for (int i = 0; i < 3; i++) { wd[i] = text_w(22, names[i]); total_w += wd[i]; }
            total_w += spacing * 2;
            float px = (VW - total_w) * 0.5f;
            for (int i = 0; i < 3; i++) {
                cxs[i] = px + wd[i] * 0.5f;
                float act = clampf(1.0f - fabsf(tabPos.v - i), 0, 1);
                text(px, 14, 22, TXT(255, 255, 255, 0.5f + 0.5f * act), names[i]);
                px += wd[i] + spacing;
            }
            int i0 = (int)floorf(tabPos.v);
            if (i0 < 0) i0 = 0;
            if (i0 > 1) i0 = 1;
            float f = tabPos.v - i0;
            float uc = lerpf(cxs[i0], cxs[i0 + 1], f), uw = lerpf(wd[i0], wd[i0 + 1], f);
            rr_fill(uc - uw * 0.5f, 44, uw, 3, 1.5f, RGBA(255, 255, 255, 0.95f));
        }

        /* часы, шестерёнка, геймпады */
        {
            char ck[16];
            clock_str(ck, sizeof ck);
            float cw = text_r(VW - MARGIN, 14, 22, TXT(255, 255, 255, 1), ck);
            float ang = ease_out(set_anim) * 120.0f;
            draw_gear(VW - MARGIN - cw - 26, 27, 11, ang, RGBA(255, 255, 255, 0.55f + 0.45f * set_anim));
            for (int i = 0; i < 4; i++)
                draw_pad_icon(VW - MARGIN - 4 * 30 - 3 * 8 + i * 38, 52, pad_on[i]);
        }

        /* содержимое вкладки */
        if (tab == 0) {
            int nv = nvis2;
            for (int k = (looping ? -1 : 0); k <= (looping ? 1 : 0); k++)
                for (int i = 0; i < nv; i++) {
                    Game *g = &games[i];
                    float s  = lerpf(TILE_S, TILE_L, g->scale.v);
                    float tx = ANCHOR_X + tile_x[i] - cam.v + k * total;
                    if (tx > VW || tx + s < -10) continue;
                    float r = s * RADIUS;
                    if (g->scale.v > 0.02f) {
                        float pulse = 0.8f + 0.2f * sinf(frame * 0.08f);
                        rr_fill(tx - 4, ROW_Y - 4, s + 8, s + 8, r + 4,
                                RGBA(0xFF, 0xFF, 0xFF, pulse * g->scale.v));
                        rr_fill(tx - 2, ROW_Y - 2, s + 4, s + 4, r + 2,
                                RGBA(0x0C, 0x0E, 0x14, g->scale.v));
                    }
                    if (g->icon_state == 1) {
                        rr_tex(&g->icon, tx, ROW_Y, s, s, r, g->u0, g->v0, g->u1, g->v1, 1.0f);
                    } else {
                        rr_fill(tx, ROW_Y, s, s, r, RGBA(0x2A, 0x2E, 0x3A, 0.92f));
                        char ch[2] = { g->name[0] >= 32 && g->name[0] < 127 ? g->name[0] : '?', 0 };
                        text_c(tx + s * 0.5f, ROW_Y + s * 0.2f, s * 0.5f, TXT(255, 255, 255, 0.7f), ch);
                    }
                }

            if (nv) {
                Game *g = &games[sel];
                float sx = ANCHOR_X + tile_x[sel] - cam.v + lerpf(TILE_S, TILE_L, g->scale.v);
                char nm[96];
                fit_text(nm, sizeof nm, g->name, 18, 330);
                text(sx + 12, ROW_Y + TILE_S + 10 + titleY.v, 18, TXT(255, 255, 255, titleA.v), nm);

                if (lgo_state == 1 && lgoFor == sel) {                  /* логотип игры вместо названия */
                    float bw = 320, bh = 130;
                    float sc = fminf(bw / lgo_tex.Width, bh / lgo_tex.Height);
                    float w = lgo_tex.Width * sc, h = lgo_tex.Height * sc;
                    draw_tex_box(&lgo_tex, 56, 300 - h + titleY.v, w, h, titleA.v);
                } else {
                    char big[96];
                    fit_text(big, sizeof big, g->name, 38, 520);
                    text(56, 262 + titleY.v, 38, TXT(255, 255, 255, titleA.v), big);
                }
                char sub[48];
                snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", T(L_PLAY), g->is_cd ? "CD" : "DVD");
                text(58, 312 + titleY.v, 18, TXT(200, 200, 200, titleA.v * 0.8f), sub);
            } else {
                text_c(VW * 0.5f, 250, 22, TXT(255, 255, 255, 0.85f), src == 0 ? T(L_NOGAMES) : T(L_SRCSOON));
            }

            /* список источников справа */
            for (int i = 0; i < 5; i++) {
                float a = srcA[i].v;
                float y = 214 + i * 32;
                float sz = lerpf(19, 24, a);
                if (a > 0.02f) rr_fill(VW - MARGIN - 150, y + 1, 150 + 10, 36, 12, RGBA(255, 255, 255, 0.14f * a));
                text_r(VW - MARGIN, y, sz, TXT(255, 255, 255, 0.5f + 0.5f * a), SRC_NAME[i]);
            }
        } else {
            float a = tabFade.v;
            text_c(VW * 0.5f, 210, 30, TXT(255, 255, 255, a), tab == 1 ? T(L_APPS) : T(L_FILES));
            text_c(VW * 0.5f, 256, 19, TXT(200, 200, 200, 0.85f * a), T(L_SOON_PAGE));
        }

        if (!intro_on || intro_t < t_part() + 3.0f)
            text_c(VW * 0.5f, VH - 36, 17, TXT(255, 255, 255, 0.6f * (1.0f - fminf(set_anim * 2.0f, 1.0f))), T(L_HINT_GAMES));

        if (set_anim > 0.01f) draw_settings(smooth01((set_anim - 0.35f) / 0.65f));
        if (confirm) draw_confirm();
        if (toast_t > 0) {
            float a = clampf(toast_t / 0.4f, 0, 1);
            float w = text_w(19, toast_msg) + 44;
            rr_fill((VW - w) * 0.5f, VH - 82, w, 34, 17, RGBA(0x10, 0x14, 0x22, 0.94f * a));
            text_c(VW * 0.5f, VH - 77, 19, TXT(255, 255, 255, a), toast_msg);
        }
        if (intro_on) draw_intro();

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        gsKit_TexManager_nextFrame(gs);
        frame++;
    }
    return 0;
}
