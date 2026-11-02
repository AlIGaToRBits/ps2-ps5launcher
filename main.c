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
#include <malloc.h>
#include <png.h>

#ifndef HAVE_CDVD
#define HAVE_CDVD 1
#endif
#ifndef HAVE_POWEROFF
#define HAVE_POWEROFF 1
#endif
#ifndef HAVE_AUDIO
#define HAVE_AUDIO 1
#endif
#if HAVE_CDVD
#include <libcdvd.h>
#endif
#if HAVE_POWEROFF
#include <libpwroff.h>
#endif
#if HAVE_AUDIO
#include <audsrv.h>
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
#if HAVE_AUDIO
IRX(audsrv_irx)
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
    int  vtry;                     /* видеорежим на пробу при следующем запуске, -1 = нет */
} Config;
/* video: 0 auto, 1 ntsc, 2 pal, 3 480p, 4 720p, 5 1080i */
#define NVIDEO 6
static Config cfg = { 0, 1, 0, 0, 0, 0, "", "", 1, 180, 1, 80, 1, 1, "", -1 };

static GSGLOBAL *gs;
static GSFONTM  *fontm;
static GSTEXTURE logo_tex, lgo_tex;
static int   font_ok, logo_ok, lgo_state;
static int   run_video, trial_on;                    /* видеорежим этого запуска; идёт ли проба режима */
static float trial_left;
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
#if HAVE_AUDIO
    SifLoadModule("rom0:LIBSD", 0, NULL);
    SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, NULL);
#endif
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
static const char *const VID_KEY[NVIDEO] = { "auto", "ntsc", "pal", "480p", "720p", "1080i" };
static int parse_video(const char *v) {
    for (int i = 0; i < NVIDEO; i++) if (!strcasecmp(v, VID_KEY[i])) return i;
    return 0;
}
static void cfg_set(const char *k, const char *v) {
    if (!strcasecmp(k, "video"))           cfg.video    = parse_video(v);
    else if (!strcasecmp(k, "video_try"))  cfg.vtry     = parse_video(v);
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
    FILE *f = fopen(cfg_path, "w");
    if (!f) return -1;
    fprintf(f, "# PS5L launcher settings\n");
    fprintf(f, "lang=%s\n", cfg.lang ? "ru" : "en");
    fprintf(f, "video=%s\n", VID_KEY[cfg.video % NVIDEO]);
    if (cfg.vtry >= 0) fprintf(f, "video_try=%s\n", VID_KEY[cfg.vtry % NVIDEO]);
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

/* ---------- Звук (audsrv): музыка из sound/music, эффекты из sound/fx, только WAV 8/16 бит ---------- */
#define OUT_RATE 44100
enum { FX_MOVE, FX_SELECT, FX_BACK, FX_TAB, FX_OPEN, FX_INTRO, FX_COUNT };
static const char *const FX_FILE[FX_COUNT] = { "move.wav", "select.wav", "back.wav", "tab.wav", "open.wav", "intro.wav" };

static char tracks[MAX_TRACKS][48];
static int  ntracks;
static int  audio_ok;

static int cmp_track(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }
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
    qsort(tracks, ntracks, sizeof tracks[0], cmp_track);
}

typedef struct {
    FILE *f;
    long data_off;
    unsigned data_len, data_pos;
    int ch, bits, rate;
    unsigned char raw[4096];
    int raw_len, raw_pos, eof;
    int s0[2], s1[2];
    unsigned frac, step;                 /* 16.16: позиция между s0 и s1, шаг = rate / OUT_RATE */
    int loop, primed;
} Src;

static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static short clip16(int v) { return (short)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

static int src_open(Src *s, const char *path, int loop) {
    unsigned char h[12], c[8], fmt[16];
    int have_fmt = 0;
    long fsize;
    memset(s, 0, sizeof *s);
    s->f = fopen(path, "rb");
    if (!s->f) return -1;
    if (fread(h, 1, 12, s->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto bad;
    for (;;) {
        if (fread(c, 1, 8, s->f) != 8) goto bad;
        unsigned len = rd32(c + 4);
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            unsigned tag;
            if (fread(fmt, 1, 16, s->f) != 16) goto bad;
            tag = rd16(fmt);
            s->ch = (int)rd16(fmt + 2); s->rate = (int)rd32(fmt + 4); s->bits = (int)rd16(fmt + 14);
            if ((tag != 1 && tag != 0xFFFE) || s->ch < 1 || s->ch > 2 || (s->bits != 16 && s->bits != 8) ||
                s->rate < 4000 || s->rate > 96000) goto bad;
            if (len > 16) fseek(s->f, (long)(len - 16 + (len & 1)), SEEK_CUR);
            have_fmt = 1;
        } else if (!memcmp(c, "data", 4)) {
            if (!have_fmt) goto bad;
            s->data_off = ftell(s->f);
            s->data_len = len;
            break;
        } else {
            fseek(s->f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    fseek(s->f, 0, SEEK_END);
    fsize = ftell(s->f);
    if (fsize < s->data_off) goto bad;
    if ((long)s->data_len > fsize - s->data_off || s->data_len == 0) s->data_len = (unsigned)(fsize - s->data_off);
    if (s->data_len < (unsigned)(s->ch * s->bits / 8) * 2) goto bad;
    fseek(s->f, s->data_off, SEEK_SET);
    s->loop = loop;
    s->step = (unsigned)(((unsigned long long)s->rate << 16) / OUT_RATE);
    return 0;
bad:
    fclose(s->f);
    s->f = NULL;
    return -1;
}
/* следующий исходный кадр (с зацикливанием при loop); 0 = конец */
static int src_raw(Src *s, int out[2]) {
    int fb = s->ch * (s->bits / 8);
    if (s->raw_pos + fb > s->raw_len) {
        unsigned chunk = (unsigned)(sizeof s->raw - (sizeof s->raw % (unsigned)fb));
        unsigned want = chunk, left = s->data_len - s->data_pos;
        int got = 0;
        if (want > left) want = left;
        if (want >= (unsigned)fb) got = (int)fread(s->raw, 1, want, s->f);
        got -= got % fb;
        if (got < fb) {
            if (!s->loop) return 0;
            fseek(s->f, s->data_off, SEEK_SET);
            s->data_pos = 0;
            want = chunk < s->data_len ? chunk : s->data_len;
            got = (int)fread(s->raw, 1, want, s->f);
            got -= got % fb;
            if (got < fb) return 0;
        }
        s->raw_len = got; s->raw_pos = 0; s->data_pos += (unsigned)got;
    }
    {
        const unsigned char *p = s->raw + s->raw_pos;
        if (s->bits == 16) {
            out[0] = (short)rd16(p);
            out[1] = s->ch == 2 ? (short)rd16(p + 2) : out[0];
        } else {
            out[0] = ((int)p[0] - 128) * 256;
            out[1] = s->ch == 2 ? ((int)p[1] - 128) * 256 : out[0];
        }
    }
    s->raw_pos += fb;
    return 1;
}
/* кадр на частоте OUT_RATE (линейная интерполяция); 0 = конец звука */
static int src_next(Src *s, int out[2]) {
    if (!s->primed) {
        s->primed = 1;
        s->frac = 0;
        if (!src_raw(s, s->s0)) return 0;
        if (!src_raw(s, s->s1)) { s->s1[0] = s->s0[0]; s->s1[1] = s->s0[1]; s->eof = 1; }
    }
    if (s->eof && s->frac >= 65536u) return 0;
    out[0] = s->s0[0] + (int)(((long long)(s->s1[0] - s->s0[0]) * (long long)s->frac) >> 16);
    out[1] = s->s0[1] + (int)(((long long)(s->s1[1] - s->s0[1]) * (long long)s->frac) >> 16);
    s->frac += s->step;
    while (s->frac >= 65536u && !s->eof) {
        s->frac -= 65536u;
        s->s0[0] = s->s1[0]; s->s0[1] = s->s1[1];
        if (!src_raw(s, s->s1)) { s->eof = 1; s->s1[0] = s->s0[0]; s->s1[1] = s->s0[1]; }
    }
    return 1;
}

typedef struct { short *pcm; unsigned frames; } Fx;
typedef struct { const short *p; unsigned pos, len; } Voice;
#define NVOICE 4
static Fx    fx[FX_COUNT];
static Voice voice[NVOICE];
static Src   mus;
static int   mus_open, mus_want;
static char  mus_name[64];
static float mus_gain;

static void audio_pump(void);
static void music_stop(void) {
    if (mus.f) fclose(mus.f);
    memset(&mus, 0, sizeof mus);
    mus_open = 0;
    mus_name[0] = 0;
}
static void music_play(const char *name) {
    char p[200];
    music_stop();
    if (!name || !name[0]) return;
    snprintf(p, sizeof p, "%s/sound/music/%s", app_dir, name);
    if (src_open(&mus, p, 1) == 0) {
        mus_open = 1;
        mus_gain = 0;
        snprintf(mus_name, sizeof mus_name, "%s", name);
    }
}
static void fx_load(int id) {
    char p[200];
    Src s;
    int o[2];
    unsigned n = 0, cap = OUT_RATE * 4;                 /* эффект не длиннее 4 секунд */
    short *buf;
    snprintf(p, sizeof p, "%s/sound/fx/%s", app_dir, FX_FILE[id]);
    if (src_open(&s, p, 0) < 0) return;
    buf = malloc((size_t)cap * 4);
    if (buf)
        while (n < cap && src_next(&s, o)) { buf[n * 2] = clip16(o[0]); buf[n * 2 + 1] = clip16(o[1]); n++; }
    fclose(s.f);
    if (buf && n) {
        short *nb = realloc(buf, (size_t)n * 4);          /* освободить неиспользованный запас */
        fx[id].pcm = nb ? nb : buf; fx[id].frames = n;
    } else free(buf);
}
static void sfx(int id) {
    int slot = -1, best = -1;
    if (!audio_ok || !cfg.fx_on || id < 0 || id >= FX_COUNT || !fx[id].pcm) return;
    for (int v = 0; v < NVOICE; v++) {
        if (!voice[v].p) { slot = v; break; }
        if ((int)voice[v].pos > best) { best = (int)voice[v].pos; slot = v; }
    }
    voice[slot].p = fx[id].pcm; voice[slot].pos = 0; voice[slot].len = fx[id].frames;
}
/* применить настройки музыки: включить, выключить или сменить трек */
static void audio_apply(void) {
    const char *want;
    mus_want = cfg.music_on;
    if (!audio_ok || !cfg.music_on) return;             /* выключенная музыка плавно затухнет в audio_pump */
    want = cfg.track[0] ? cfg.track : (ntracks ? tracks[0] : "");
    if (!want[0]) return;
    if (!mus_open || strcmp(mus_name, want)) music_play(want);
}
/* докармливает звуковой буфер; вызывается каждый кадр и из долгих загрузок */
static void audio_pump(void) {
#if HAVE_AUDIO
    static short buf[2048 * 2];
    int avail, frames, fg;
    float lv, mt;
    if (!audio_ok) return;
    avail = audsrv_available();
    if (avail < 2048) return;
    frames = avail / 4;
    if (frames > 2048) frames = 2048;
    lv = (float)cfg.volume / 100.0f; lv *= lv;
    mt = (mus_want && mus_open) ? lv * 0.6f : 0.0f;
    fg = (int)(lv * 256.0f);
    for (int i = 0; i < frames; i++) {
        int l = 0, r = 0, o[2], mg;
        mus_gain += (mt - mus_gain) * 0.0001f;
        mg = (int)(mus_gain * 256.0f);
        if (mus_open) {
            if (src_next(&mus, o)) { l += o[0] * mg >> 8; r += o[1] * mg >> 8; }
            else music_stop();
        }
        for (int v = 0; v < NVOICE; v++)
            if (voice[v].p) {
                l += voice[v].p[voice[v].pos * 2] * fg >> 8;
                r += voice[v].p[voice[v].pos * 2 + 1] * fg >> 8;
                if (++voice[v].pos >= voice[v].len) voice[v].p = NULL;
            }
        buf[i * 2] = clip16(l); buf[i * 2 + 1] = clip16(r);
    }
    if (!mus_want && mus_open && mus_gain < 0.002f) music_stop();
    audsrv_play_audio((const char *)buf, frames * 4);
#endif
}
static void audio_init(void) {
#if HAVE_AUDIO
    struct audsrv_fmt_t fmt;
    if (audsrv_init() != 0) return;
    fmt.bits = 16; fmt.freq = OUT_RATE; fmt.channels = 2;
    if (audsrv_set_format(&fmt) != 0) { audsrv_quit(); return; }
    audsrv_set_volume(100);
    audio_ok = 1;
    for (int i = 0; i < FX_COUNT; i++) fx_load(i);
#endif
}
static void audio_quit(void) {
#if HAVE_AUDIO
    if (!audio_ok) return;
    music_stop();
    audsrv_stop_audio();
    audsrv_quit();
    audio_ok = 0;
#endif
}

/* ---------- Графика ---------- */
static void free_tex(GSTEXTURE *t) {
    if (t->Mem) { gsKit_TexManager_free(gs, t); free(t->Mem); t->Mem = NULL; }
}
static void img_path(char *dst, size_t n, const char *name) {
    snprintf(dst, n, "%s/img/%s", app_dir, name);
}

/* Читает PNG любого вида (палитра, серый, прозрачность tRNS, 16 бит) в RGBA8. */
static unsigned char *png_decode(const char *path, int *ow, int *oh) {
    FILE *fp = fopen(path, "rb");
    unsigned char sig[8];
    png_structp png;
    png_infop info;
    unsigned char *volatile pix = NULL;
    png_bytep *volatile rows = NULL;
    png_uint_32 w, h;
    int depth, ctype, passes;
    if (!fp) return NULL;
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) { fclose(fp); return NULL; }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(pix); free(rows); fclose(fp);
        return NULL;
    }
    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);
    png_get_IHDR(png, info, &w, &h, &depth, &ctype, NULL, NULL, NULL);
    if (!w || !h || w > 4096 || h > 4096) png_error(png, "size");
    if (depth == 16) png_set_strip_16(png);
    if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    pix = malloc((size_t)w * h * 4);
    rows = malloc(h * sizeof(png_bytep));
    if (!pix || !rows) png_error(png, "mem");
    for (png_uint_32 i = 0; i < h; i++) rows[i] = pix + (size_t)i * w * 4;
    for (int pass = 0; pass < passes; pass++)
        for (png_uint_32 i = 0; i < h; i++) {
            png_read_row(png, rows[i], NULL);
            if ((i & 15) == 15) audio_pump();
        }
    png_destroy_read_struct(&png, &info, NULL);
    free(rows);
    fclose(fp);
    *ow = (int)w; *oh = (int)h;
    return pix;
}

/* PNG -> текстура GS. Большие картинки уменьшаются (экономит видеопамять), square = обрезка до квадрата. */
static int load_png_ex(GSTEXTURE *t, const char *path, int maxw, int maxh, int square) {
    int w, h;
    unsigned char *px = png_decode(path, &w, &h);
    memset(t, 0, sizeof *t);
    if (!px) return -1;
    int cx = 0, cy = 0, cw = w, ch = h;
    if (square) { cw = ch = (w < h) ? w : h; cx = (w - cw) / 2; cy = (h - ch) / 2; }
    int dw = cw, dh = ch;
    if (maxw > 0 && (cw > maxw || ch > maxh)) {
        float sc = fminf((float)maxw / cw, (float)maxh / ch);
        dw = (int)(cw * sc + 0.5f); dh = (int)(ch * sc + 0.5f);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    u32 *out = memalign(128, (size_t)dw * dh * 4);
    if (!out) { free(px); return -1; }
    float fx = (float)cw / dw, fy = (float)ch / dh;
    for (int y = 0; y < dh; y++) {
        if ((y & 7) == 7) audio_pump();
        for (int x = 0; x < dw; x++) {
            float xa = x * fx, xb = xa + fx, ya = y * fy, yb = ya + fy;
            float sr = 0, sg = 0, sb = 0, sa = 0, area = 0;
            for (int sy = (int)ya; sy < (int)ceilf(yb) && sy < ch; sy++) {
                float wy = fminf((float)(sy + 1), yb) - fmaxf((float)sy, ya);
                for (int sx = (int)xa; sx < (int)ceilf(xb) && sx < cw; sx++) {
                    float wgt = (fminf((float)(sx + 1), xb) - fmaxf((float)sx, xa)) * wy;
                    const unsigned char *p = px + ((size_t)(cy + sy) * w + cx + sx) * 4;
                    float a = p[3] * wgt;
                    sr += p[0] * a; sg += p[1] * a; sb += p[2] * a; sa += a; area += wgt;
                }
            }
            unsigned r = 0, g = 0, b = 0, a8 = 0;
            if (area > 0 && sa > 0) {
                r = (unsigned)(sr / sa + 0.5f); g = (unsigned)(sg / sa + 0.5f); b = (unsigned)(sb / sa + 0.5f);
                a8 = (unsigned)(sa / area + 0.5f);
            }
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            if (a8 > 255) a8 = 255;
            out[y * dw + x] = ((a8 * 128u + 127u) / 255u << 24) | (b << 16) | (g << 8) | r;   /* альфа GS: 0..0x80 */
        }
    }
    free(px);
    t->Width = dw; t->Height = dh;
    t->PSM = GS_PSM_CT32;
    t->Mem = out;
    t->Clut = NULL; t->Vram = 0; t->VramClut = 0;
    t->Filter = GS_FILTER_LINEAR;
    t->Delayed = 1;
    return 0;
}

/* ---------- Шрифт: исходный лист 48 px -> атлас под текущий видеорежим ---------- */
typedef struct { unsigned short x, y; unsigned char w, h; signed char ox, oy; } BGlyph;
typedef struct { float h; GSTEXTURE tex; BGlyph g[FONT_NGLYPH]; } FontCls;
static FontCls fcls[2];                       /* 0 = мелкий (17), 1 = средний (22) */

static int bake_class(FontCls *fc, const unsigned char *src, float hv) {
    const float fx = hv / FONT_SRC_EM * kx, fy = hv / FONT_SRC_EM * ky;     /* физических пикселей на пиксель листа */
    const int AW = 256, N = (int)FONT_NGLYPH;
    int order[FONT_NGLYPH], n = 0;
    memset(fc, 0, sizeof *fc);
    fc->h = hv;
    for (int i = 0; i < N; i++) {
        const FontGlyph *g = &font_glyphs[i];
        BGlyph *b = &fc->g[i];
        if (!g->sw || !g->sh) continue;
        int x0 = (int)floorf(g->xo * fx) - 1, x1 = (int)ceilf((g->xo + g->sw) * fx) + 1;
        int y0 = (int)floorf(g->yo * fy) - 1, y1 = (int)ceilf((g->yo + g->sh) * fy) + 1;
        b->ox = (signed char)x0; b->oy = (signed char)y0;
        b->w = (unsigned char)(x1 - x0); b->h = (unsigned char)(y1 - y0);
        order[n++] = i;
    }
    for (int i = 1; i < n; i++) {             /* по убыванию высоты */
        int v = order[i], j = i - 1;
        while (j >= 0 && fc->g[order[j]].h < fc->g[v].h) { order[j + 1] = order[j]; j--; }
        order[j + 1] = v;
    }
    int px = 1, py = 1, rowh = 0;
    for (int k = 0; k < n; k++) {
        BGlyph *b = &fc->g[order[k]];
        if (px + b->w + 1 > AW) { px = 1; py += rowh + 1; rowh = 0; }
        b->x = (unsigned short)px; b->y = (unsigned short)py;
        px += b->w + 1;
        if (b->h > rowh) rowh = b->h;
    }
    int used = py + rowh + 1, AH = 64;
    while (AH < used) AH <<= 1;
    if (AH > 256) return 0;
    unsigned char *al = calloc((size_t)AW * AH, 1);
    if (!al) return 0;
    for (int k = 0; k < n; k++) {             /* усреднение по площади */
        const FontGlyph *g = &font_glyphs[order[k]];
        const BGlyph *b = &fc->g[order[k]];
        for (int iy = 0; iy < b->h; iy++) {
            float ya = (b->oy + iy) / fy - g->yo, yb = (b->oy + iy + 1) / fy - g->yo;
            float yc0 = fmaxf(ya, 0), yc1 = fminf(yb, (float)g->sh);
            for (int ix = 0; ix < b->w; ix++) {
                float xa = (b->ox + ix) / fx - g->xo, xb = (b->ox + ix + 1) / fx - g->xo;
                float xc0 = fmaxf(xa, 0), xc1 = fminf(xb, (float)g->sw), sum = 0;
                if (xc1 > xc0 && yc1 > yc0)
                    for (int sy = (int)floorf(yc0); sy < (int)ceilf(yc1); sy++) {
                        float wy = fminf((float)(sy + 1), yc1) - fmaxf((float)sy, yc0);
                        const unsigned char *row = src + ((size_t)(g->sy + sy) * FONT_SRC_W + g->sx) * 4 + 3;
                        for (int sx = (int)floorf(xc0); sx < (int)ceilf(xc1); sx++)
                            sum += row[sx * 4] * (fminf((float)(sx + 1), xc1) - fmaxf((float)sx, xc0)) * wy;
                    }
                float a = sum / ((xb - xa) * (yb - ya) * 255.0f);
                al[(b->y + iy) * AW + b->x + ix] = (unsigned char)(clampf(a, 0, 1) * 255.0f + 0.5f);
            }
        }
    }
    if (gs->Interlace == GS_INTERLACED) {     /* вертикальный фильтр 1-2-1 против мерцания чересстрочной развёртки */
        for (int k = 0; k < n; k++) {
            const BGlyph *b = &fc->g[order[k]];
            for (int ix = 0; ix < b->w; ix++) {
                unsigned char *col = al + b->y * AW + b->x + ix;
                float p0 = 0, c0 = col[0];
                for (int iy = 0; iy < b->h; iy++) {
                    float nx = (iy + 1 < b->h) ? col[(iy + 1) * AW] : 0.0f;
                    col[iy * AW] = (unsigned char)(0.25f * p0 + 0.5f * c0 + 0.25f * nx + 0.5f);
                    p0 = c0; c0 = nx;
                }
            }
        }
    }
    u32 *mem = memalign(128, (size_t)AW * AH * 4);
    if (!mem) { free(al); return 0; }
    for (int i = 0; i < AW * AH; i++) {
        unsigned a = (unsigned)(al[i] * 1.15f);
        if (a > 255) a = 255;
        mem[i] = ((a * 128u + 127u) / 255u << 24) | 0x00FFFFFFu;
    }
    free(al);
    fc->tex.Width = AW; fc->tex.Height = AH;
    fc->tex.PSM = GS_PSM_CT32;
    fc->tex.Mem = mem;
    fc->tex.Filter = GS_FILTER_LINEAR;
    fc->tex.Delayed = 1;
    return 1;
}
static int font_build_all(const char *path) {
    int w, h, ok = 0;
    unsigned char *src = png_decode(path, &w, &h);
    if (!src) return 0;
    if (w == FONT_SRC_W && h == FONT_SRC_H)
        ok = bake_class(&fcls[0], src, 17.0f) && bake_class(&fcls[1], src, 22.0f);
    free(src);
    return ok;
}

static void gfx_init(void) {
    gs = gsKit_init_global();
    gs->PSM = GS_PSM_CT24;
    switch (run_video) {
    case 1: gs->Mode = GS_MODE_NTSC; gs->Interlace = GS_INTERLACED;
            gs->Field = GS_FIELD; gs->Width = 640; gs->Height = 448; break;
    case 2: gs->Mode = GS_MODE_PAL;  gs->Interlace = GS_INTERLACED;
            gs->Field = GS_FIELD; gs->Width = 640; gs->Height = 512; break;
    case 3: gs->Mode = GS_MODE_DTV_480P; gs->Interlace = GS_NONINTERLACED;
            gs->Field = GS_FRAME; gs->Width = 640; gs->Height = 480; break;
    case 4: gs->Mode = GS_MODE_DTV_720P; gs->Interlace = GS_NONINTERLACED;      /* 640x360, растягивается вдвое */
            gs->Field = GS_FRAME; gs->Width = 640; gs->Height = 360; break;
    case 5: gs->Mode = GS_MODE_DTV_1080I; gs->Interlace = GS_INTERLACED;         /* 640x540, растягивается в 3x2 */
            gs->Field = GS_FIELD; gs->Width = 640; gs->Height = 540; gs->PSM = GS_PSM_CT16; break;
    default: break;
    }
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

    VW = (cfg.wide || run_video >= 4) ? 854.0f : 640.0f;
    kx = gs->Width / VW;
    ky = gs->Height / VH;
    dt = (gs->Mode == GS_MODE_PAL) ? 1.0f / 50.0f : 1.0f / 60.0f;

    char p[160];
    img_path(p, sizeof p, "font_src.png");
    font_ok = font_build_all(p);
    if (!font_ok) {                            /* запасной встроенный шрифт (только латиница) */
        fontm = gsKit_init_fontm();
        gsKit_fontm_upload(gs, fontm);
        fontm->Spacing = 0.8f;
        cfg.lang = 0;
    }
    img_path(p, sizeof p, "logo.png");
    logo_ok = (load_png_ex(&logo_tex, p, 320, 144, 0) == 0);
    gsKit_TexManager_init(gs);
}

/* ---------- Текст ---------- */
static int glyph_idx(unsigned cp) {
    int lo = 0, hi = (int)FONT_NGLYPH - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_glyphs[mid].cp == cp) return mid;
        if (font_glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}
static int glyph_of(unsigned cp) { int i = glyph_idx(cp); return i >= 0 ? i : glyph_idx('?'); }
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
    float k = h / FONT_SRC_EM / 16.0f, w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int i = glyph_of(utf8_next(&p));
        if (i >= 0) w += font_glyphs[i].adv16 * k;
    }
    return w;
}
/* h = размер шрифта в виртуальных пикселях (17 и 22 рисуются без масштабирования),
   (x, y) = левый верх строки; возвращает ширину */
static float text(float x, float y, float h, u64 col, const char *s) {
    if (!font_ok) {
        char buf[96];
        size_t i = 0;
        for (; s[i] && i < sizeof buf - 1; i++) buf[i] = ((unsigned char)s[i] < 0x80) ? s[i] : '?';
        buf[i] = 0;
        gsKit_fontm_print_scaled(gs, fontm, X(x), Y(y), 3, h / 48.0f * ky, col, buf);
        return text_w(h, buf);
    }
    FontCls *fc = (h < 19.5f) ? &fcls[0] : &fcls[1];
    float sc = h / fc->h, k = h / FONT_SRC_EM / 16.0f, pen = x;
    float oy = floorf(Y(y) + 0.5f);
    const unsigned char *p = (const unsigned char *)s;
    gsKit_TexManager_bind(gs, &fc->tex);
    while (*p) {
        int i = glyph_of(utf8_next(&p));
        if (i < 0) continue;
        const BGlyph *b = &fc->g[i];
        if (b->w) {
            float gx = floorf(X(pen) + 0.5f) + b->ox * sc, gy = oy + b->oy * sc;
            gsKit_prim_sprite_texture(gs, &fc->tex,
                                      gx, gy, b->x, b->y,
                                      gx + b->w * sc, gy + b->h * sc, b->x + b->w, b->y + b->h,
                                      3, col);
        }
        pen += font_glyphs[i].adv16 * k;
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
        if (load_png_ex(&g->icon, p, 72, 72, 1) == 0) {
            g->u0 = 0; g->v0 = 0; g->u1 = (float)g->icon.Width; g->v1 = (float)g->icon.Height;
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
    L_RESTARTFAIL, L_SOUNDSOON, L_HINT_GAMES, L_HINT_MENU, L_HINT_CONFIRM, L_PLAY,
    L_KEEPVIDEO, L_REVERTIN, L_NOAUDIO, L_COUNT
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
    [L_NOFONT]      = { "img/font_src.png not found: English only", "Нет img/font_src.png: только английский" },
    [L_NOTRACKS]    = { "No tracks in sound/music", "Нет треков в sound/music" },
    [L_NOPOWEROFF]  = { "Power off is not available in this build", "В этой сборке выключение недоступно" },
    [L_RESTARTFAIL] = { "Could not start the launcher ELF", "Не удалось запустить ELF лаунчера" },
    [L_SOUNDSOON]   = { "Sound playback is coming in the next stage", "Воспроизведение звука появится на следующем этапе" },
    [L_HINT_GAMES]  = { "\xE2\x9C\x95 Play     L1 / R1 Tabs     START Settings", "\xE2\x9C\x95 Играть     L1 / R1 Вкладки     START Настройки" },
    [L_HINT_MENU]   = { "\xE2\x96\xB2\xE2\x96\xBC Select     \xE2\x97\x84\xE2\x96\xBA Change     \xE2\x9C\x95 OK     \xE2\x97\x8B Back",
                        "\xE2\x96\xB2\xE2\x96\xBC Выбор     \xE2\x97\x84\xE2\x96\xBA Изменить     \xE2\x9C\x95 OK     \xE2\x97\x8B Назад" },
    [L_HINT_CONFIRM]= { "\xE2\x9C\x95 Yes     \xE2\x97\x8B No", "\xE2\x9C\x95 Да     \xE2\x97\x8B Нет" },
    [L_PLAY]        = { "PS2", "PS2" },
    [L_KEEPVIDEO]   = { "Keep this video mode?", "Оставить этот видеорежим?" },
    [L_NOAUDIO]     = { "Audio is not available", "Звук недоступен" },
    [L_REVERTIN]    = { "Reverting in %d s if no answer", "Автоотмена через %d с, если не ответить" },
};
#define T(id) (tr_tab[(id)][cfg.lang ? 1 : 0])

/* ---------- Уведомление ---------- */
static char  toast_msg[96];
static float toast_t;
static void toast(const char *s) { snprintf(toast_msg, sizeof toast_msg, "%s", s); toast_t = 2.6f; }

/* ---------- Выход, перезапуск, выключение ---------- */
static void do_exit(void) { audio_quit(); pads_close(); Exit(0); }
static void do_poweroff(void) {
#if HAVE_POWEROFF
    audio_quit();
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
    audio_quit();
    pads_close();
    LoadELFFromFile(p, 1, av);
    pads_open();                                   /* сюда попадаем, только если запуск не удался */
    toast(T(L_RESTARTFAIL));
}

/* ---------- Меню настроек ---------- */
enum { PG_ROOT, PG_SCREEN, PG_SOUND, PG_TRACKS };
static int   pg, cur[4];
static int   set_open;                              /* 1 = меню открыто (или открывается) */
static float set_anim;                              /* 0..1: вращение шестерёнки и появление панели */
static int   confirm;                               /* 0 нет, 1 выход, 2 выключение */

static int page_count(int p) {
    return p == PG_ROOT ? 6 : p == PG_SCREEN ? 6 : p == PG_SOUND ? 5 : ntracks + 1;
}
static const char *page_title(int p) {
    return p == PG_ROOT ? T(L_SETTINGS) : p == PG_SCREEN ? T(L_SCREEN) : p == PG_SOUND ? T(L_SOUND) : T(L_TRACK);
}
static void track_title(const char *file, char *out, size_t n) {      /* имя файла без .wav */
    snprintf(out, n, "%s", file);
    size_t L = strlen(out);
    if (L > 4 && !strcasecmp(out + L - 4, ".wav")) out[L - 4] = 0;
}
static const char *item_label(int p, int i) {
    static const int root[]   = { L_SCREEN, L_SOUND, L_SAVE, L_RESTART, L_EXIT, L_POWEROFF };
    static const int screen[] = { L_LANGUAGE, L_TZ, L_CLOCKFMT, L_VIDEO, L_ASPECT, L_BACK };
    static const int sound[]  = { L_VOLUME, L_MUSIC, L_EFFECTS, L_TRACK, L_BACK };
    static char name[64];
    if (p == PG_TRACKS) {
        if (i >= ntracks) return T(L_BACK);
        track_title(tracks[i], name, sizeof name);
        return name;
    }
    return T(p == PG_ROOT ? root[i] : p == PG_SCREEN ? screen[i] : sound[i]);
}
/* значение справа; пустая строка = у пункта нет значения */
static void item_value(int p, int i, char *out, size_t n) {
    static const char *vid[NVIDEO] = { "Auto", "NTSC", "PAL", "480p", "720p", "1080i" };
    out[0] = 0;
    if (p == PG_SCREEN) {
        char tz[24];
        switch (i) {
        case 0: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.lang ? "Русский" : "English"); break;
        case 1: tz_str(tz, sizeof tz); snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", tz); break;
        case 2: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.clock24 ? "24" : "12 AM/PM"); break;
        case 3: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", vid[(cfg.vtry >= 0 ? cfg.vtry : cfg.video) % NVIDEO]); break;
        case 4: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.wide ? "16:9" : "4:3"); break;
        }
    } else if (p == PG_SOUND) {
        char tn[64];
        switch (i) {
        case 0: snprintf(out, n, "\xE2\x97\x84 %d%% \xE2\x96\xBA", cfg.volume); break;
        case 1: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.music_on ? T(L_ON) : T(L_OFF)); break;
        case 2: snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", cfg.fx_on ? T(L_ON) : T(L_OFF)); break;
        case 3:
            track_title(cfg.track[0] ? cfg.track : (ntracks ? tracks[0] : "-"), tn, sizeof tn);
            snprintf(out, n, "%.24s", tn);
            break;
        }
    } else if (p == PG_TRACKS) {
        if (i < ntracks && !strcmp(tracks[i], cfg.track[0] ? cfg.track : tracks[0])) snprintf(out, n, "\xE2\x96\xBA");
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
        case 3: {
            int cv = (cfg.vtry >= 0 ? cfg.vtry : cfg.video), nv2 = (cv + dir + NVIDEO) % NVIDEO;
            cfg.vtry = (nv2 == cfg.video) ? -1 : nv2;
            toast(T(L_NEEDRESTART));
            break;
        }
        case 4: cfg.wide ^= 1; toast(T(L_NEEDRESTART)); break;
        default: changed = 0;
        }
    } else if (p == PG_SOUND) {
        switch (i) {
        case 0: cfg.volume = (int)clampf((float)(cfg.volume + dir * 10), 0, 100); break;
        case 1: cfg.music_on ^= 1; audio_apply(); break;
        case 2: cfg.fx_on ^= 1; break;
        case 3:
            if (!ntracks) { toast(T(L_NOTRACKS)); changed = 0; break; }
            {
                int c = 0;
                for (int k = 0; k < ntracks; k++) if (!strcmp(tracks[k], cfg.track)) c = k;
                if (!cfg.track[0]) c = dir > 0 ? -1 : 0;
                c = ((c + dir) % ntracks + ntracks) % ntracks;
                snprintf(cfg.track, sizeof cfg.track, "%s", tracks[c]);
                audio_apply();
            }
            break;
        default: changed = 0;
        }
        if (changed && i < 3 && !audio_ok) toast(T(L_NOAUDIO));
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
    } else if (p == PG_SOUND) {
        if (i == 4) pg = PG_ROOT;
        else if (i == 3) {
            if (!ntracks) toast(T(L_NOTRACKS));
            else {
                pg = PG_TRACKS; cur[PG_TRACKS] = 0;
                for (int k = 0; k < ntracks; k++) if (!strcmp(tracks[k], cfg.track)) cur[PG_TRACKS] = k;
            }
        } else item_adjust(p, i, 1);
    } else {
        if (i >= ntracks) pg = PG_SOUND;
        else { snprintf(cfg.track, sizeof cfg.track, "%s", tracks[i]); cfg_dirty = 1; audio_apply(); }
    }
}

static void draw_settings(float a) {
    gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 4, RGBA(0, 0, 0, 0.62f * a));
    int n = page_count(pg), vis = n > 7 ? 7 : n, first = 0;
    if (n > vis) { first = cur[pg] - vis / 2; if (first < 0) first = 0; if (first > n - vis) first = n - vis; }
    float rowh = 42, pw = 520, px = (VW - pw) * 0.5f, py = 50;
    float ph = 74 + vis * rowh + 14, slide = (1.0f - a) * 26;
    rr_fill(px, py + slide, pw, ph, 16, RGBA(0x12, 0x16, 0x24, 1.0f * a));
    text(px + 30, py + slide + 22, 22, TXT(255, 255, 255, a), page_title(pg));
    fill_rect(px + 28, py + slide + 62, pw - 56, 2, RGBA(255, 255, 255, 0.18f * a));
    for (int k = 0; k < vis; k++) {
        int i = first + k;
        float y = py + slide + 74 + k * rowh;
        char val[64], lab[64];
        item_value(pg, i, val, sizeof val);
        fit_text(lab, sizeof lab, item_label(pg, i), 22, pw - 120);
        if (i == cur[pg]) {
            rr_fill(px + 14, y, pw - 28, rowh - 6, 10, RGBA(255, 255, 255, 0.17f * a));
            rr_fill(px + 14, y + 6, 4, rowh - 18, 2, RGBA(255, 255, 255, 0.9f * a));
        }
        float ta = (i == cur[pg]) ? 1.0f : 0.72f;
        text(px + 34, y + 2, 22, TXT(255, 255, 255, ta * a), lab);
        if (val[0]) text_r(px + pw - 34, y + 2, 22, TXT(255, 255, 255, ta * a), val);
    }
    if (n > vis) {                                              /* полоса прокрутки */
        float th = (vis * rowh - 12) * vis / (float)n, ty = py + slide + 74 + (vis * rowh - 12 - th) * first / (float)(n - vis);
        fill_rect(px + pw - 14, ty, 3, th, RGBA(255, 255, 255, 0.45f * a));
    }
    text_c(VW * 0.5f, VH - 36, 17, TXT(255, 255, 255, 0.75f * a), T(L_HINT_MENU));
}
static void trial_keep(void) {
    cfg.video = run_video; cfg.vtry = -1; trial_on = 0; confirm = 0;
    toast(save_cfg() == 0 ? T(L_SAVED) : T(L_SAVEFAIL));
}
static void trial_revert(void) {
    cfg.vtry = -1; trial_on = 0; confirm = 0;
    do_restart();                                   /* сохранит прежний режим и перезапустит */
}
static void draw_confirm(void) {
    gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 5, RGBA(0, 0, 0, 0.55f));
    float w = 420, h = 130, x = (VW - w) * 0.5f, y = 170;
    rr_fill(x, y, w, h, 16, RGBA(0x16, 0x1A, 0x2A, 0.98f));
    if (confirm == 3) {
        char l2[96];
        snprintf(l2, sizeof l2, T(L_REVERTIN), (int)ceilf(trial_left));
        text_c(VW * 0.5f, y + 20, 22, TXT(255, 255, 255, 1), T(L_KEEPVIDEO));
        text_c(VW * 0.5f, y + 58, 17, TXT(255, 255, 255, 0.8f), l2);
        text_c(VW * 0.5f, y + 92, 17, TXT(255, 255, 255, 0.75f), T(L_HINT_CONFIRM));
        return;
    }
    text_c(VW * 0.5f, y + 28, 22, TXT(255, 255, 255, 1), confirm == 1 ? T(L_ASK_EXIT) : T(L_ASK_POWER));
    text_c(VW * 0.5f, y + 82, 17, TXT(255, 255, 255, 0.75f), T(L_HINT_CONFIRM));
}

/* ---------- Заставка ---------- */
typedef struct { float x, y, vx, vy, age, life, size; int blue; } Particle;
#define NPART 110
static Particle part[NPART];
static float intro_t;
static int   intro_on = 1, parts_made;

static float t_part(void) { return logo_ok ? 3.0f : 0.4f; }   /* логотип 3 с, затем 2 с частиц */

static void parts_spawn(void) {
    for (int i = 0; i < NPART; i++) {
        Particle *p = &part[i];
        float a = (float)(rand() % 6283) / 1000.0f;
        float sp = 70.0f + (float)(rand() % 220);
        p->x = VW * 0.5f + cosf(a) * 8.0f;
        p->y = VH * 0.5f + sinf(a) * 8.0f;
        p->vx = cosf(a) * sp;
        p->vy = sinf(a) * sp * 0.7f;
        p->age = -(float)(rand() % 400) / 1000.0f;       /* разный момент появления */
        p->life = 0.9f + (float)(rand() % 700) / 1000.0f;
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
    if (intro_t >= t_part() + 2.0f) intro_on = 0;
}
static float intro_cover(void) {        /* непрозрачность чёрного поверх меню */
    float t0 = t_part() + 0.1f;
    return 1.0f - smooth01((intro_t - t0) / 1.5f);
}
static void draw_intro(void) {
    float cover = intro_cover();
    if (cover > 0) gsKit_prim_sprite(gs, 0, 0, gs->Width, gs->Height, 6, RGBA(0, 0, 0, cover));
    if (logo_ok) {
        float a = smooth01((intro_t - 0.4f) / 0.8f) * (1.0f - smooth01((intro_t - 2.4f) / 0.6f));
        float w = 260, h = w * logo_tex.Height / (float)logo_tex.Width;
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
    run_video = cfg.video;
    if (select_held()) { cfg.video = 0; cfg.vtry = -1; run_video = 0; }
    else if (cfg.vtry >= 0) {            /* проба режима: прежний режим записываем заранее, на случай зависания */
        run_video = cfg.vtry; trial_on = 1; trial_left = 10.0f; cfg.vtry = -1;
        save_cfg();
    }
    gfx_init();
    if (trial_on) { intro_on = 0; confirm = 3; }
    rr_init();
    clock_init();
    srand(12345);

    scan_dir("DVD", 0);
    scan_dir("CD", 1);
    qsort(games, ngames, sizeof(Game), cmp_game);
    scan_tracks();
    audio_init();

    int sel = 0, idle = 0, bgFor = -1, lgoFor = -1, lap = 0;
    int tab = 0, src = 0, hold[4] = { 0, 0, 0, 0 }, pad_on[4] = { 0, 0, 0, 0 };
    u32 prev = 0xFFFF;
    unsigned frame = 0;
    int intro_snd = 0, music_started = 0;
    GSTEXTURE bg;
    memset(&bg, 0, sizeof bg);
    Anim cam, titleA, titleY, bgA, tabPos, tabFade, srcA[5];
    anim_init(&cam, 0, 14);
    anim_init(&titleA, 1, 10);
    anim_init(&titleY, 0, 10);
    anim_init(&bgA, 0, 9);
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
        } else if (confirm == 3) {
            if (pressed & PAD_CROSS) { sfx(FX_SELECT); trial_keep(); }
            if (pressed & PAD_CIRCLE) { sfx(FX_BACK); trial_revert(); }
        } else if (confirm) {
            if (pressed & PAD_CROSS) { int c = confirm; confirm = 0; sfx(FX_SELECT); if (c == 1) do_exit(); else do_poweroff(); }
            if (pressed & PAD_CIRCLE) { confirm = 0; sfx(FX_BACK); }
        } else if (set_open) {
            int n = page_count(pg);
            if (fU) { cur[pg] = (cur[pg] + n - 1) % n; sfx(FX_MOVE); }
            if (fD) { cur[pg] = (cur[pg] + 1) % n; sfx(FX_MOVE); }
            if (fL) { item_adjust(pg, cur[pg], -1); sfx(FX_MOVE); }
            if (fR) { item_adjust(pg, cur[pg], +1); sfx(FX_MOVE); }
            if (pressed & PAD_CROSS) { sfx(FX_SELECT); item_press(pg, cur[pg]); }
            if (pressed & PAD_CIRCLE) {
                sfx(FX_BACK);
                if (pg == PG_TRACKS) pg = PG_SOUND;
                else if (pg != PG_ROOT) pg = PG_ROOT;
                else set_open = 0;
            }
            if (pressed & PAD_START) { set_open = 0; sfx(FX_BACK); }
        } else {
            if (pressed & PAD_START) { set_open = 1; pg = PG_ROOT; sfx(FX_OPEN); }
            if (pressed & PAD_L1) { tab = (tab + 2) % 3; tabFade.v = 0; anim_to(&tabFade, 1); sfx(FX_TAB); }
            if (pressed & PAD_R1) { tab = (tab + 1) % 3; tabFade.v = 0; anim_to(&tabFade, 1); sfx(FX_TAB); }
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
                    sfx(FX_MOVE);
                    titleA.v = 0; anim_to(&titleA, 1);
                    titleY.v = 8; anim_to(&titleY, 0);
                    lgo_state = 0;
                    anim_to(&bgA, 0);
                }
                int ds = fD ? 1 : fU ? -1 : 0;
                if (ds) {
                    src = (src + ds + 5) % 5;
                    sfx(FX_MOVE);
                    sel = 0; lap = 0; idle = 0; cam.v = cam.t = 0;
                    lgo_state = 0; anim_to(&bgA, 0);
                }
                if ((pressed & PAD_CROSS) && nvis > 0) { sfx(FX_SELECT); toast(T(L_LAUNCHSOON)); }
            }
        }
        idle++;

        /* --- анимации и состояние --- */
        if (intro_on) {
            if (!intro_snd && intro_t >= 0.2f) { intro_snd = 1; sfx(FX_INTRO); }
            intro_update();
        } else {
            if (logo_ok) { free_tex(&logo_tex); logo_ok = 0; }
            if (!music_started) { music_started = 1; audio_apply(); }
        }
        if (set_open) set_anim = clampf(set_anim + dt / 0.55f, 0, 1);
        else          set_anim = clampf(set_anim - dt / 0.30f, 0, 1);
        if (toast_t > 0) toast_t -= dt;
        if (trial_on) { trial_left -= dt; if (trial_left <= 0) trial_revert(); }
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
        anim_step(&bgA, dt);

        /* --- подгрузка --- */
        if (!nvis2) anim_to(&bgA, 0);
        if (nvis2 && idle == 12) {
            if (bgFor != sel) {
                free_tex(&bg);
                if (games[sel].id[0]) {
                    char p[96];
                    snprintf(p, sizeof p, ROOT "ART/%s_BG.png", games[sel].id);
                    load_png_ex(&bg, p, 448, 252, 0);
                }
                bgFor = sel;
            }
            anim_to(&bgA, 1);
        } else if (nvis2 && idle == 14 && lgoFor != sel) {
            free_tex(&lgo_tex);
            lgo_state = 0;
            if (games[sel].id[0]) {
                char p[96];
                snprintf(p, sizeof p, ROOT "ART/%s_LGO.png", games[sel].id);
                if (load_png_ex(&lgo_tex, p, 256, 128, 0) == 0) lgo_state = 1;
            }
            lgoFor = sel;
        } else if (nvis2 && idle > 2) {
            stream_icons(sel);
        }

        /* --- отрисовка --- */
        float W = gs->Width, H = gs->Height;
        gsKit_clear(gs, RGBA(0x08, 0x0C, 0x18, 1));
        draw_full(&bg, bgA.v);

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
            float wd[3], cxs[3], total_w = 0, spacing = VW > 700 ? 56 : 30;
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
                fit_text(nm, sizeof nm, g->name, 17, 330);
                text(sx + 12, ROW_Y + TILE_S + 10 + titleY.v, 17, TXT(255, 255, 255, titleA.v), nm);

                if (lgo_state == 1 && lgoFor == sel) {                  /* логотип игры: внизу слева */
                    float bw = 330, bh = 140;
                    float sc = fminf(bw / lgo_tex.Width, bh / lgo_tex.Height);
                    float w = lgo_tex.Width * sc, h = lgo_tex.Height * sc;
                    draw_tex_box(&lgo_tex, 56, 388 - h + titleY.v, w, h, titleA.v);
                } else {
                    char big[96];
                    fit_text(big, sizeof big, g->name, 22, 520);
                    text(56, 350 + titleY.v, 22, TXT(255, 255, 255, titleA.v), big);
                }
                char sub[48];
                snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", T(L_PLAY), g->is_cd ? "CD" : "DVD");
                text(58, 394 + titleY.v, 17, TXT(200, 200, 200, titleA.v * 0.8f), sub);
            } else {
                text_c(VW * 0.5f, 250, 22, TXT(255, 255, 255, 0.85f), src == 0 ? T(L_NOGAMES) : T(L_SRCSOON));
            }

            /* список источников справа */
            for (int i = 0; i < 5; i++) {
                float a = srcA[i].v;
                float y = 214 + i * 32;
                float sz = a > 0.5f ? 22.0f : 17.0f;
                if (a > 0.02f) rr_fill(VW - MARGIN - 150, y + 1, 150 + 10, 36, 12, RGBA(255, 255, 255, 0.14f * a));
                text_r(VW - MARGIN, y, sz, TXT(255, 255, 255, 0.5f + 0.5f * a), SRC_NAME[i]);
            }
        } else {
            float a = tabFade.v;
            text_c(VW * 0.5f, 210, 22, TXT(255, 255, 255, a), tab == 1 ? T(L_APPS) : T(L_FILES));
            text_c(VW * 0.5f, 250, 17, TXT(200, 200, 200, 0.85f * a), T(L_SOON_PAGE));
        }

        if (!intro_on || intro_t < t_part() + 3.0f)
            text_c(VW * 0.5f, VH - 36, 17, TXT(255, 255, 255, 0.6f * (1.0f - fminf(set_anim * 2.0f, 1.0f))), T(L_HINT_GAMES));

        if (set_anim > 0.01f) draw_settings(smooth01((set_anim - 0.35f) / 0.65f));
        if (confirm) draw_confirm();
        if (toast_t > 0) {
            float a = clampf(toast_t / 0.4f, 0, 1);
            float w = text_w(17, toast_msg) + 44;
            rr_fill((VW - w) * 0.5f, VH - 82, w, 34, 17, RGBA(0x10, 0x14, 0x22, 0.94f * a));
            text_c(VW * 0.5f, VH - 75, 17, TXT(255, 255, 255, a), toast_msg);
        }
        if (intro_on) draw_intro();

        audio_pump();
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
        gsKit_TexManager_nextFrame(gs);
        frame++;
    }
    return 0;
}
