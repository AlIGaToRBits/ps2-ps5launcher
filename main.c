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
#include "anim.h"

#define ROOT      "mass:/"     /* для PCSX2 поменяйте на "host:" */
#define MAX_GAMES 256
#define DT        (1.0f / 60.0f)

/* Разметка под 640x448 (кадр PS5 1280x720 в уменьшенном масштабе) */
#define ROW_Y     52
#define TILE_S    40
#define TILE_L    72
#define GAP       6
#define ANCHOR_X  40

/* IRX-модули, встроенные в ELF через bin2c (см. Makefile) */
#define IRX(n) extern unsigned char n[]; extern unsigned int size_##n;
IRX(iomanX_irx) IRX(fileXio_irx) IRX(usbd_irx)
IRX(bdm_irx) IRX(bdmfs_fatfs_irx) IRX(usbmass_bd_irx)

typedef struct {
    char name[64];
    char rel[160];          /* "DVD/SLUS_209.46.Name.iso" */
    char id[16];            /* "SLUS_209.46" */
    int  is_cd;
    GSTEXTURE icon;
    int  icon_state;        /* 0 = не грузили, 1 = есть, -1 = нет файла */
    Anim scale;             /* 0 = маленькая, 1 = выбранная */
} Game;

static GSGLOBAL *gs;
static GSFONTM  *font;
static Game  games[MAX_GAMES];
static float tile_x[MAX_GAMES];
static int   ngames;
static char  padBuf[256] __attribute__((aligned(64)));

/* ---------- IOP: USB + геймпад ---------- */
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

    for (int i = 0; i < 50; i++) {          /* ждём, пока флешка подключится (до ~5 с) */
        DIR *d = opendir(ROOT);
        if (d) { closedir(d); break; }
        usleep(100 * 1000);
    }
}

/* ---------- Графика ---------- */
static void gfx_init(void) {
    gs = gsKit_init_global();
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
    gsKit_fontm_upload(gs, font);   /* шрифт кладём в VRAM до запуска менеджера текстур */
    gsKit_TexManager_init(gs);
}

/* Альфа на GS: 0x80 = полностью непрозрачно */
#define RGBA(r, g, b, a) GS_SETREG_RGBAQ(r, g, b, (int)((a) * 128.0f), 0)

static int load_png(GSTEXTURE *t, const char *path) {
    memset(t, 0, sizeof *t);
    t->Delayed = 1;                 /* держим в RAM, в VRAM грузит менеджер текстур */
    if (gsKit_texture_png(gs, t, (char *)path) < 0) { t->Mem = NULL; return -1; }
    t->Filter = GS_FILTER_LINEAR;
    return 0;
}
static void free_tex(GSTEXTURE *t) {
    if (t->Mem) { gsKit_TexManager_free(gs, t); free(t->Mem); t->Mem = NULL; }
}
static void draw_tex(GSTEXTURE *t, float x, float y, float w, float h, float a) {
    if (!t->Mem || a <= 0.0f) return;
    gsKit_TexManager_bind(gs, t);
    gsKit_prim_sprite_texture(gs, t, x, y, 0, 0, x + w, y + h,
                              t->Width, t->Height, 1, RGBA(0x80, 0x80, 0x80, a));
}
static void frame_rect(float x, float y, float w, float h, float th, u64 c) {
    gsKit_prim_sprite(gs, x - th, y - th, x + w + th, y,          2, c);
    gsKit_prim_sprite(gs, x - th, y + h,  x + w + th, y + h + th, 2, c);
    gsKit_prim_sprite(gs, x - th, y,      x,          y + h,      2, c);
    gsKit_prim_sprite(gs, x + w,  y,      x + w + th, y + h,      2, c);
}

/* ---------- Поиск игр ---------- */
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
        /* Имя в стиле OPL: SLUS_209.46.Name.iso */
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

/* ---------- Подгрузка картинок без подтормаживаний ---------- */
/* Не больше одной картинки за кадр, только рядом с выбранной игрой */
static void stream_icons(int sel) {
    for (int i = 0; i < ngames; i++)
        if ((i < sel - 12 || i > sel + 16) && games[i].icon_state == 1) {
            free_tex(&games[i].icon); games[i].icon_state = 0;
        }
    static const int order[] = {0, 1, -1, 2, 3, 4, 5, 6, 7, 8, 9, 10, -2, -3};
    for (unsigned k = 0; k < sizeof order / sizeof *order; k++) {
        int i = sel + order[k];
        if (i < 0 || i >= ngames || games[i].icon_state) continue;
        char p[96];
        snprintf(p, sizeof p, ROOT "ART/%s_ICO.png", games[i].id);
        games[i].icon_state = (games[i].id[0] && load_png(&games[i].icon, p) == 0) ? 1 : -1;
        return;
    }
}

/* ---------- Запуск через Neutrino ---------- */
static void launch(const Game *g) {
    static char a_dvd[200];
    snprintf(a_dvd, sizeof a_dvd, "-dvd=mass:%s", g->rel);
    char *args[] = { "-bsd=usb", a_dvd, g->is_cd ? "-mt=cd" : "-mt=dvd" };
    padPortClose(0, 0);
    padEnd();
    LoadELFFromFile(ROOT "neutrino/neutrino.elf", 3, args);
}

/* ---------- Главный цикл ---------- */
int main(int argc, char *argv[]) {
    iop_init();
    gfx_init();
    padInit(0);
    padPortOpen(0, 0, padBuf);

    scan_dir("DVD", 0);
    scan_dir("CD", 1);
    qsort(games, ngames, sizeof(Game), cmp_game);

    int sel = 0, idle = 0, held = 0, bgFront = 0, bgFor = -1;
    u32 prev = 0;
    unsigned frame = 0;
    GSTEXTURE bg[2] = {0};
    Anim cam, titleA, titleY, bgFade;
    anim_init(&cam, 0, 14);
    anim_init(&titleA, 1, 10);
    anim_init(&titleY, 0, 10);
    anim_init(&bgFade, 1, 6);

    for (;;) {
        /* --- ввод с повтором при удержании --- */
        struct padButtonStatus b;
        u32 btn = (padRead(0, 0, &b) != 0) ? (0xFFFF ^ b.btns) : 0;
        u32 pressed = btn & ~prev;
        int dir = (btn & PAD_RIGHT) ? 1 : (btn & PAD_LEFT) ? -1 : 0, move = 0;
        if (dir) {
            if (pressed & (PAD_RIGHT | PAD_LEFT)) { move = dir; held = 0; }
            else if (++held > 18 && held % (held > 90 ? 3 : 6) == 0) move = dir;
        }
        prev = btn;

        if (move && ngames) {
            int ns = sel + move;
            if (ns >= 0 && ns < ngames && ns != sel) {
                sel = ns; idle = 0;
                titleA.v = 0;  anim_to(&titleA, 1);
                titleY.v = 8;  anim_to(&titleY, 0);
            }
        }
        if ((pressed & PAD_CROSS) && ngames) launch(&games[sel]);
        idle++;

        /* --- анимации и раскладка ряда --- */
        float x = 0;
        for (int i = 0; i < ngames; i++) {
            anim_to(&games[i].scale, i == sel ? 1.0f : 0.0f);
            anim_step(&games[i].scale, DT);
            tile_x[i] = x;
            x += lerpf(TILE_S, TILE_L, games[i].scale.v) + GAP;
        }
        if (ngames) anim_to(&cam, tile_x[sel]);
        anim_step(&cam, DT);
        anim_step(&titleA, DT);
        anim_step(&titleY, DT);
        anim_step(&bgFade, DT);

        /* --- фон грузим, только когда выбор не меняется 200 мс --- */
        if (ngames && idle == 12 && bgFor != sel) {
            int back = bgFront ^ 1;
            free_tex(&bg[back]);
            char p[96];
            snprintf(p, sizeof p, ROOT "ART/%s_BG.png", games[sel].id);
            if (games[sel].id[0]) load_png(&bg[back], p);
            bgFront = back; bgFor = sel;
            bgFade.v = 0; anim_to(&bgFade, 1);
        } else if (idle > 2) {
            stream_icons(sel);
        }

        /* --- отрисовка --- */
        gsKit_clear(gs, RGBA(0x08, 0x0C, 0x18, 1));
        draw_tex(&bg[bgFront ^ 1], 0, 0, 640, 448, 1.0f - bgFade.v);   /* старый фон гаснет */
        draw_tex(&bg[bgFront],     0, 0, 640, 448, bgFade.v);          /* новый проявляется */

        /* затемнение слева и снизу, чтобы читался текст */
        gsKit_prim_quad_gouraud(gs, 0, 0, 0, 448, 400, 0, 400, 448, 1,
            RGBA(0, 0, 0, 0.75f), RGBA(0, 0, 0, 0.75f), RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 0));
        gsKit_prim_quad_gouraud(gs, 0, 448, 640, 448, 0, 250, 640, 250, 1,
            RGBA(0, 0, 0, 0.8f), RGBA(0, 0, 0, 0.8f), RGBA(0, 0, 0, 0), RGBA(0, 0, 0, 0));

        /* верхняя панель */
        gsKit_fontm_print_scaled(gs, font, 32,  16, 3, 0.55f, RGBA(0xFF, 0xFF, 0xFF, 1),    "Games");
        gsKit_fontm_print_scaled(gs, font, 100, 16, 3, 0.55f, RGBA(0xFF, 0xFF, 0xFF, 0.5f), "Media");

        /* ряд иконок */
        for (int i = 0; i < ngames; i++) {
            float s  = lerpf(TILE_S, TILE_L, games[i].scale.v);
            float tx = ANCHOR_X + tile_x[i] - cam.v;
            if (tx > 640 || tx + s < 0) continue;
            if (games[i].icon_state == 1)
                draw_tex(&games[i].icon, tx, ROW_Y, s, s, 1.0f);
            else
                gsKit_prim_sprite(gs, tx, ROW_Y, tx + s, ROW_Y + s, 2, RGBA(0x30, 0x34, 0x40, 0.9f));
            if (i == sel) {
                float pulse = 0.75f + 0.25f * sinf(frame * 0.08f);
                frame_rect(tx - 2, ROW_Y - 2, s + 4, s + 4, 2, RGBA(0xFF, 0xFF, 0xFF, pulse));
            }
        }

        if (ngames) {
            Game *g = &games[sel];
            float sx = ANCHOR_X + tile_x[sel] - cam.v + lerpf(TILE_S, TILE_L, g->scale.v);
            /* подпись справа от выбранной плитки, под рядом */
            gsKit_fontm_print_scaled(gs, font, sx + 8, ROW_Y + TILE_S + 6 + titleY.v, 3, 0.5f,
                                     RGBA(0xFF, 0xFF, 0xFF, titleA.v), g->name);
            /* крупный блок слева снизу */
            gsKit_fontm_print_scaled(gs, font, 56, 300 + titleY.v, 3, 0.9f,
                                     RGBA(0xFF, 0xFF, 0xFF, titleA.v), g->name);
            gsKit_fontm_print_scaled(gs, font, 56, 340 + titleY.v, 3, 0.5f,
                                     RGBA(0xC8, 0xC8, 0xC8, titleA.v * 0.8f),
                                     g->is_cd ? "PS2 - CD   (X) Play" : "PS2 - DVD   (X) Play");
        } else {
            gsKit_fontm_print_scaled(gs, font, 56, 300, 3, 0.6f, RGBA(0xFF, 0xFF, 0xFF, 1),
                                     "No games found in mass:/DVD or mass:/CD");
        }

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        gsKit_TexManager_nextFrame(gs);
        frame++;
    }
    return 0;
}
