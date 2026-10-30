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
#include "anim.h"

#define ROOT      "mass:/"
#define CFG_PATH  ROOT "APPS/PS5L/launcher.cfg"
#define NEU_DIR   ROOT "neutrino"
#define NEU_ELF   NEU_DIR "/neutrino.elf"
#define MAX_GAMES 256

/* Разметка в виртуальных координатах: 854x480 (16:9) или 640x480 (4:3) */
#define VH        480.0f
#define ROW_Y     56.0f
#define TILE_S    48.0f
#define TILE_L    84.0f
#define GAP       8.0f
#define ANCHOR_X  44.0f
#define RADIUS    0.13f      /* скругление = 13% от размера плитки */

#define IRX(n) extern unsigned char n[]; extern unsigned int size_##n;
IRX(iomanX_irx) IRX(fileXio_irx) IRX(usbd_irx)
IRX(bdm_irx) IRX(bdmfs_fatfs_irx) IRX(usbmass_bd_irx)

typedef struct {
    char name[64];
    char rel[160];
    char id[16];
    int  is_cd;
    GSTEXTURE icon;
    int  icon_state;               /* 0 = не грузили, 1 = есть, -1 = нет */
    float u0, v0, u1, v1;          /* квадратная область текстуры */
    Anim scale;
} Game;

typedef struct { int video, wide, debug, argv0, cwd, slash; char extra[64], extra2[64]; } Config;
static Config cfg = { 0, 1, 1, 0, 0, 0, "", "" };   /* video: 0 auto, 1 ntsc, 2 pal, 3 480p */

static GSGLOBAL *gs;
static GSFONTM  *font;
static Game  games[MAX_GAMES];
static float tile_x[MAX_GAMES];
static int   ngames;
static float VW = 854.0f, kx = 1, ky = 1, dt = 1.0f / 60.0f;
static char  padBuf[256] __attribute__((aligned(64)));

#define X(v) ((v) * kx)
#define Y(v) ((v) * ky)
#define RGBA(r, g, b, a) GS_SETREG_RGBAQ(r, g, b, (int)((a) * 128.0f), 0)

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
static u32 read_buttons(void) {
    struct padButtonStatus b;
    int st = padGetState(0, 0);
    if (st != PAD_STATE_STABLE && st != PAD_STATE_FINDCTP1) return 0;
    if (padRead(0, 0, &b) == 0) return 0;
    return 0xFFFF ^ b.btns;
}
static int select_held(void) {              /* SELECT при запуске = безопасный видеорежим */
    for (int i = 0; i < 40; i++) {
        if (read_buttons() & PAD_SELECT) return 1;
        usleep(25 * 1000);
    }
    return 0;
}

/* ---------- Настройки ---------- */
static void load_cfg(void) {
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) return;
    char line[160], key[32], val[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %31[^= \t#] = %63s", key, val) != 2) continue;
        if (!strcasecmp(key, "video"))
            cfg.video = !strcasecmp(val, "ntsc") ? 1 : !strcasecmp(val, "pal") ? 2 :
                        !strcasecmp(val, "480p") ? 3 : 0;
        else if (!strcasecmp(key, "widescreen")) cfg.wide  = atoi(val);
        else if (!strcasecmp(key, "debug"))      cfg.debug = atoi(val);
        else if (!strcasecmp(key, "argv0"))      cfg.argv0 = atoi(val);
        else if (!strcasecmp(key, "cwd"))        cfg.cwd   = atoi(val);
        else if (!strcasecmp(key, "slash"))  cfg.slash = atoi(val);
        else if (!strcasecmp(key, "extra"))  snprintf(cfg.extra,  sizeof cfg.extra,  "%s", val);
        else if (!strcasecmp(key, "extra2")) snprintf(cfg.extra2, sizeof cfg.extra2, "%s", val);
    }
    fclose(f);
}

/* ---------- Графика ---------- */
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

    font = gsKit_init_fontm();
    gsKit_fontm_upload(gs, font);
    font->Spacing = 0.8f;                     /* плотнее буквы */
    gsKit_TexManager_init(gs);

    VW = cfg.wide ? 854.0f : 640.0f;
    kx = gs->Width / VW;
    ky = gs->Height / VH;
    dt = (gs->Mode == GS_MODE_PAL) ? 1.0f / 50.0f : 1.0f / 60.0f;
}

static void text(float x, float y, float scale, u64 col, const char *s) {
    gsKit_fontm_print_scaled(gs, font, X(x), Y(y), 3, scale * ky, col, s);
}

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
static void draw_full(GSTEXTURE *t, float a) {
    if (!t->Mem || a <= 0.0f) return;
    gsKit_TexManager_bind(gs, t);
    gsKit_prim_sprite_texture(gs, t, 0, 0, 0, 0, gs->Width, gs->Height,
                              t->Width, t->Height, 1, RGBA(0x80, 0x80, 0x80, a));
}

/* ---------- Скруглённые прямоугольники ---------- */
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
        if (L < 5 || strcasecmp(n + L - 4, ".iso")) continue;
        Game *g = &games[ngames++];
        memset(g, 0, sizeof *g);
        snprintf(g->rel, sizeof g->rel, "%s/%s", sub, n);
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
    for (int i = 0; i < ngames; i++)
        if ((i < sel - 12 || i > sel + 16) && games[i].icon_state == 1) {
            free_tex(&games[i].icon); games[i].icon_state = 0;
        }
    static const int order[] = {0, 1, -1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, -2, -3};
    for (unsigned k = 0; k < sizeof order / sizeof *order; k++) {
        int i = sel + order[k];
        if (i < 0 || i >= ngames || games[i].icon_state) continue;
        games[i].icon_state = (load_art(&games[i]) == 0) ? 1 : -1;
        return;
    }
}

/* ---------- Экран с текстом (проверка запуска / ошибки) ---------- */
static void info_screen(const char *const *lines, int n) {
    gsKit_clear(gs, RGBA(0x08, 0x0C, 0x18, 1));
    for (int i = 0; i < n; i++)
        text(40, 40 + i * 28, 0.5f, RGBA(0xFF, 0xFF, 0xFF, 1), lines[i]);
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
    gsKit_TexManager_nextFrame(gs);
}
static int exists_file(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }
static int exists_dir(const char *p)  { DIR *d = opendir(p); if (!d) return 0; closedir(d); return 1; }

/* ---------- Запуск через Neutrino ---------- */
static void launch(const Game *g) {
    static char a_dvd[220], a_cwd[64], iso[200];
    static char *args[8];
    int n = 0;
    if (cfg.slash) snprintf(a_dvd, sizeof a_dvd, "-dvd=mass:/%s", g->rel);
    else           snprintf(a_dvd, sizeof a_dvd, "-dvd=mass:%s",  g->rel);
    snprintf(a_cwd, sizeof a_cwd, "-cwd=%s", NEU_DIR);
    snprintf(iso,   sizeof iso,   ROOT "%s", g->rel);
    if (cfg.argv0) args[n++] = NEU_ELF;
    args[n++] = "-bsd=usb";
    args[n++] = a_dvd;
    args[n++] = g->is_cd ? "-mt=cd" : "-mt=dvd";
    if (cfg.cwd) args[n++] = a_cwd;
    if (cfg.extra[0])  args[n++] = cfg.extra;
    if (cfg.extra2[0]) args[n++] = cfg.extra2;

    int okElf = exists_file(NEU_ELF);
    int okMod = exists_dir(NEU_DIR "/modules");
    int okCfg = exists_dir(NEU_DIR "/config");
    int okIso = exists_file(iso);

    if (cfg.debug || !(okElf && okMod && okCfg && okIso)) {
        char l[16][100];
        const char *lp[16];
        int k = 0;
        snprintf(l[k++], 100, "LAUNCH CHECK");
        snprintf(l[k++], 100, "neutrino.elf : %s", okElf ? "OK" : "NOT FOUND");
        snprintf(l[k++], 100, "modules/     : %s", okMod ? "OK" : "NOT FOUND");
        snprintf(l[k++], 100, "config/      : %s", okCfg ? "OK" : "NOT FOUND");
        snprintf(l[k++], 100, "ISO          : %s", okIso ? "OK" : "NOT FOUND");
        snprintf(l[k++], 100, "argv0=%d  cwd=%d", cfg.argv0, cfg.cwd);
        for (int i = 0; i < n; i++) snprintf(l[k++], 100, "arg%d: %.80s", i, args[i]);
        snprintf(l[k++], 100, "(X) Launch     (O) Back");
        for (int i = 0; i < k; i++) lp[i] = l[i];

        u32 prev = 0xFFFF;
        for (;;) {
            info_screen(lp, k);
            u32 b = read_buttons(), p = b & ~prev;
            prev = b;
            if (p & PAD_CROSS) break;
            if (p & PAD_CIRCLE) return;
        }
    }

    padPortClose(0, 0);
    padEnd();
    LoadELFFromFile(NEU_ELF, n, args);

    /* Сюда попадаем, только если ELF не запустился */
    padInit(0);
    padPortOpen(0, 0, padBuf);
    const char *err[] = { "LoadELFFromFile FAILED", "Check " NEU_ELF };
    for (int i = 0; i < 240; i++) info_screen(err, 2);
}

/* ---------- Главный цикл ---------- */
int main(int argc, char *argv[]) {
    iop_init();
    padInit(0);
    padPortOpen(0, 0, padBuf);
    load_cfg();
    if (select_held()) cfg.video = 0;
    gfx_init();
    rr_init();

    scan_dir("DVD", 0);
    scan_dir("CD", 1);
    qsort(games, ngames, sizeof(Game), cmp_game);

    int sel = 0, idle = 0, held = 0, bgFront = 0, bgFor = -1;
    u32 prev = 0xFFFF;
    unsigned frame = 0;
    GSTEXTURE bg[2];
    memset(bg, 0, sizeof bg);
    Anim cam, titleA, titleY, bgFade;
    anim_init(&cam, 0, 14);
    anim_init(&titleA, 1, 10);
    anim_init(&titleY, 0, 10);
    anim_init(&bgFade, 1, 6);

    for (;;) {
        /* --- ввод --- */
        u32 btn = read_buttons();
        u32 pressed = btn & ~prev;
        int dir = (btn & PAD_RIGHT) ? 1 : (btn & PAD_LEFT) ? -1 : 0, move = 0;
        if (dir) {
            if (pressed & (PAD_RIGHT | PAD_LEFT)) { move = dir; held = 0; }
            else if (++held > 18 && held % (held > 90 ? 3 : 6) == 0) move = dir;
        }
        prev = btn;

        if (move && ngames) {
            int ns = sel + move;
            if (ns >= 0 && ns < ngames) {
                sel = ns; idle = 0;
                titleA.v = 0; anim_to(&titleA, 1);
                titleY.v = 8; anim_to(&titleY, 0);
            }
        }
        if ((pressed & PAD_CROSS) && ngames) {
            launch(&games[sel]);
            prev = 0xFFFF;
            continue;
        }
        idle++;

        /* --- анимации --- */
        float x = 0;
        for (int i = 0; i < ngames; i++) {
            anim_to(&games[i].scale, i == sel ? 1.0f : 0.0f);
            anim_step(&games[i].scale, dt);
            tile_x[i] = x;
            x += lerpf(TILE_S, TILE_L, games[i].scale.v) + GAP;
        }
        if (ngames) anim_to(&cam, tile_x[sel]);
        anim_step(&cam, dt);
        anim_step(&titleA, dt);
        anim_step(&titleY, dt);
        anim_step(&bgFade, dt);

        /* --- подгрузка --- */
        if (ngames && idle == 12 && bgFor != sel) {
            int back = bgFront ^ 1;
            free_tex(&bg[back]);
            if (games[sel].id[0]) {
                char p[96];
                snprintf(p, sizeof p, ROOT "ART/%s_BG.png", games[sel].id);
                load_png(&bg[back], p);
            }
            bgFront = back; bgFor = sel;
            bgFade.v = 0; anim_to(&bgFade, 1);
        } else if (idle > 2) {
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
                                RGBA(0, 0, 0, 0.55f), RGBA(0, 0, 0, 0.55f), none, none);

        text(44,  16, 0.6f, RGBA(0xFF, 0xFF, 0xFF, 1),    "Games");
        text(140, 16, 0.6f, RGBA(0xFF, 0xFF, 0xFF, 0.5f), "Media");

        for (int i = 0; i < ngames; i++) {
            Game *g = &games[i];
            float s  = lerpf(TILE_S, TILE_L, g->scale.v);
            float tx = ANCHOR_X + tile_x[i] - cam.v;
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
                char ch[2] = { g->name[0], 0 };
                text(tx + s * 0.38f, ROW_Y + s * 0.3f, s / 90.0f, RGBA(0xFF, 0xFF, 0xFF, 0.7f), ch);
            }
        }

        if (ngames) {
            Game *g = &games[sel];
            float sx = ANCHOR_X + tile_x[sel] - cam.v + lerpf(TILE_S, TILE_L, g->scale.v);
            text(sx + 12, ROW_Y + TILE_S + 10 + titleY.v, 0.5f,
                 RGBA(0xFF, 0xFF, 0xFF, titleA.v), g->name);
            text(60, 280 + titleY.v, 0.9f, RGBA(0xFF, 0xFF, 0xFF, titleA.v), g->name);
            text(60, 324 + titleY.v, 0.5f, RGBA(0xC8, 0xC8, 0xC8, titleA.v * 0.8f),
                 g->is_cd ? "PS2 - CD     (X) Play" : "PS2 - DVD     (X) Play");
        } else {
            text(60, 280, 0.6f, RGBA(0xFF, 0xFF, 0xFF, 1), "No games in mass:/DVD or mass:/CD");
        }

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        gsKit_TexManager_nextFrame(gs);
        frame++;
    }
    return 0;
}
