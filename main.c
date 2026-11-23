// Slither-like para PS2 (homebrew) v2 - gsKit + libpad
// Menu, pausa, pantalla de muerte, puntaje. Sin assets: todo se dibuja con codigo.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libpad.h>

#define WORLD 1600.0f
#define MAXSEG 150
#define MINLEN 10
#define SP 7.0f
#define NAI 8
#define NFOOD 160
#define RGB(r, g, b) GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00)

typedef struct {
    float x[MAXSEG], y[MAXSEG];
    int len;
    float ang;
    u64 col, col2;
} Snake;

typedef struct { float x, y; u64 col; } Food;
typedef enum { ST_MENU, ST_PLAY, ST_PAUSE, ST_DEAD } State;

static char padBuf[256] __attribute__((aligned(64)));
static GSGLOBAL *gs;
static Snake player, ai[NAI];
static Food food[NFOOD];
static State state = ST_MENU;
static int score = 0, best = 0, boostTick = 0, dropIdx = 0;
static float SW, SH;
static u32 rng = 2463534242u;

/* ---------------- utilidades ---------------- */
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static float frand(float a, float b) { return a + (b - a) * ((float)(rnd() & 0xFFFF) / 65535.0f); }

static float wrap(float a) {
    while (a > 3.14159f) a -= 6.28318f;
    while (a < -3.14159f) a += 6.28318f;
    return a;
}

static void steer(Snake *s, float target, float rate) {
    float d = wrap(target - s->ang);
    if (d > rate) d = rate;
    if (d < -rate) d = -rate;
    s->ang += d;
}

/* ---------------- logica ---------------- */
static void spawn_snake(Snake *s, u64 col, u64 col2, int len) {
    float x = frand(250, WORLD - 250), y = frand(250, WORLD - 250);
    float c, sn;
    s->ang = frand(0, 6.28f);
    c = cosf(s->ang); sn = sinf(s->ang);
    s->len = len; s->col = col; s->col2 = col2;
    for (int i = 0; i < MAXSEG; i++) {
        s->x[i] = x - c * SP * i;
        s->y[i] = y - sn * SP * i;
    }
}

static void spawn_food(Food *f) {
    f->x = frand(20, WORLD - 20);
    f->y = frand(20, WORLD - 20);
    f->col = RGB(90 + (rnd() % 165), 90 + (rnd() % 165), 90 + (rnd() % 165));
}

static void drop_food(Snake *s) {
    for (int i = 0; i < s->len; i += 3) {
        Food *f = &food[dropIdx];
        dropIdx = (dropIdx + 1) % NFOOD;
        f->x = s->x[i]; f->y = s->y[i];
        f->col = s->col;
    }
}

static void new_ai(int i) {
    u32 r = 80 + (rnd() % 150), g = 60 + (rnd() % 150), b = 80 + (rnd() % 150);
    spawn_snake(&ai[i], RGB(r, g, b), RGB(r / 2, g / 2, b / 2), 8 + (rnd() % 20));
}

static void reset_game(void) {
    spawn_snake(&player, RGB(70, 230, 100), RGB(35, 150, 60), MINLEN);
    for (int i = 0; i < NAI; i++) new_ai(i);
    for (int i = 0; i < NFOOD; i++) spawn_food(&food[i]);
    score = 0; boostTick = 0;
}

static void move_snake(Snake *s, float speed) {
    s->x[0] += cosf(s->ang) * speed;
    s->y[0] += sinf(s->ang) * speed;
    if (s->x[0] < 0) s->x[0] = 0;
    if (s->y[0] < 0) s->y[0] = 0;
    if (s->x[0] > WORLD) s->x[0] = WORLD;
    if (s->y[0] > WORLD) s->y[0] = WORLD;
    for (int i = 1; i < s->len; i++) {
        float dx = s->x[i - 1] - s->x[i];
        float dy = s->y[i - 1] - s->y[i];
        float d2 = dx * dx + dy * dy;
        if (d2 > SP * SP) {
            float k = SP / sqrtf(d2);
            s->x[i] = s->x[i - 1] - dx * k;
            s->y[i] = s->y[i - 1] - dy * k;
        }
    }
}

static int eat(Snake *s) {
    int n = 0;
    for (int i = 0; i < NFOOD; i++) {
        float dx = food[i].x - s->x[0], dy = food[i].y - s->y[0];
        if (dx * dx + dy * dy < 144.0f) {
            if (s->len < MAXSEG) {
                s->x[s->len] = s->x[s->len - 1];
                s->y[s->len] = s->y[s->len - 1];
                s->len++;
            }
            spawn_food(&food[i]);
            n++;
        }
    }
    return n;
}

static int hits(const Snake *head, const Snake *body) {
    float hx = head->x[0], hy = head->y[0];
    for (int i = 0; i < body->len; i++) {
        float dx = body->x[i] - hx, dy = body->y[i] - hy;
        if (dx * dx + dy * dy < 49.0f) return 1;
    }
    return 0;
}

static void ai_think(Snake *s) {
    float hx = s->x[0], hy = s->y[0];
    if (hx < 100 || hy < 100 || hx > WORLD - 100 || hy > WORLD - 100) {
        steer(s, atan2f(WORLD / 2 - hy, WORLD / 2 - hx), 0.08f);
        return;
    }
    float best2 = 1e12f, bx = hx, by = hy;
    for (int i = 0; i < NFOOD; i++) {
        float dx = food[i].x - hx, dy = food[i].y - hy;
        float d = dx * dx + dy * dy;
        if (d < best2) { best2 = d; bx = food[i].x; by = food[i].y; }
    }
    steer(s, atan2f(by - hy, bx - hx), 0.05f);
}

static void update_ai(void) {
    for (int i = 0; i < NAI; i++) {
        ai_think(&ai[i]);
        move_snake(&ai[i], 1.7f);
        eat(&ai[i]);
    }
}

/* ---------------- pad ---------------- */
static float padAx, padAy;
static int padAnalog;

static void init_pad(void) {
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, padBuf);
    int st;
    do { st = padGetState(0, 0); } while (st != PAD_STATE_STABLE && st != PAD_STATE_FINDCTP1);
    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
}

static u32 read_pad(void) {
    struct padButtonStatus b;
    padAnalog = 0; padAx = padAy = 0;
    if (padRead(0, 0, &b) == 0) return 0;
    if ((b.mode >> 4) == 7) {
        padAx = (float)b.ljoy_h - 128.0f;
        padAy = (float)b.ljoy_v - 128.0f;
        if (padAx * padAx + padAy * padAy > 1600.0f) padAnalog = 1;
    }
    return 0xffff ^ b.btns;
}

static void update_play(u32 pad) {
    float speed = 1.8f;
    if (padAnalog) steer(&player, atan2f(padAy, padAx), 0.07f);
    if (pad & PAD_LEFT) player.ang -= 0.07f;
    if (pad & PAD_RIGHT) player.ang += 0.07f;
    if ((pad & PAD_CROSS) && player.len > MINLEN + 2) {
        speed = 3.4f;
        if (++boostTick >= 15) { boostTick = 0; player.len--; }
    }
    move_snake(&player, speed);
    score += eat(&player);
    update_ai();
    for (int i = 0; i < NAI; i++) {
        if (hits(&player, &ai[i])) {
            drop_food(&player);
            if (score > best) best = score;
            state = ST_DEAD;
            return;
        }
        if (hits(&ai[i], &player)) { drop_food(&ai[i]); new_ai(i); }
    }
}

/* ---------------- dibujo (gsKit) ---------------- */
// Todo el dibujo vive aqui abajo: cambia colores/formas sin tocar la logica.

static void rect(float x1, float y1, float x2, float y2, u64 col) {
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > SW) x2 = SW;
    if (y2 > SH) y2 = SH;
    if (x2 <= x1 || y2 <= y1) return;
    gsKit_prim_sprite(gs, x1, y1, x2, y2, 1, col);
}

static void box(float cx, float cy, float r, u64 col) {
    rect(cx - r, cy - r, cx + r, cy + r, col);
}

static void draw_background(float camx, float camy) {
    u64 line = RGB(28, 28, 46), wall = RGB(200, 50, 60);
    gsKit_clear(gs, RGB(16, 16, 28));
    float g0 = floorf(camx / 100.0f) * 100.0f;
    for (float gx = g0; gx < camx + SW; gx += 100.0f)
        if (gx > 0 && gx < WORLD) rect(gx - camx, -camy, gx - camx + 1, WORLD - camy, line);
    g0 = floorf(camy / 100.0f) * 100.0f;
    for (float gy = g0; gy < camy + SH; gy += 100.0f)
        if (gy > 0 && gy < WORLD) rect(-camx, gy - camy, WORLD - camx, gy - camy + 1, line);
    rect(-camx - 6, -camy - 6, -camx, WORLD - camy + 6, wall);
    rect(WORLD - camx, -camy - 6, WORLD - camx + 6, WORLD - camy + 6, wall);
    rect(-camx, -camy - 6, WORLD - camx, -camy, wall);
    rect(-camx, WORLD - camy, WORLD - camx, WORLD - camy + 6, wall);
}

static void draw_food(float camx, float camy) {
    for (int i = 0; i < NFOOD; i++) box(food[i].x - camx, food[i].y - camy, 3, food[i].col);
}

static void draw_snake(const Snake *s, float camx, float camy) {
    for (int i = s->len - 1; i > 0; i--) {
        float x = s->x[i] - camx, y = s->y[i] - camy;
        if (x < -8 || y < -8 || x > SW + 8 || y > SH + 8) continue;
        box(x, y, 4, (i & 2) ? s->col2 : s->col);
    }
    float hx = s->x[0] - camx, hy = s->y[0] - camy;
    if (hx < -10 || hy < -10 || hx > SW + 10 || hy > SH + 10) return;
    box(hx, hy, 5, s->col);
    float c = cosf(s->ang), sn = sinf(s->ang);
    float ex = hx + c * 2, ey = hy + sn * 2;
    box(ex - sn * 3, ey + c * 3, 2, RGB(255, 255, 255));
    box(ex + sn * 3, ey - c * 3, 2, RGB(255, 255, 255));
    box(ex - sn * 3 + c, ey + c * 3 + sn, 1, RGB(0, 0, 0));
    box(ex + sn * 3 + c, ey - c * 3 + sn, 1, RGB(0, 0, 0));
}

/* fuente 5x7 propia (solo mayusculas, numeros y ':') */
static const u8 FONT[37][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x00,0x04,0x00,0x00,0x00,0x04,0x00}
};

static int glyph(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ':') return 36;
    return -1;
}

static float text_w(const char *s, float px) {
    int n = 0;
    while (s[n]) n++;
    return n * 6 * px - px;
}

static void text(const char *s, float x, float y, float px, u64 col) {
    for (; *s; s++, x += 6 * px) {
        int g = glyph(*s);
        if (g < 0) continue;
        for (int r = 0; r < 7; r++) {
            u8 row = FONT[g][r];
            int c = 0;
            while (c < 5) {
                if (row & (0x10 >> c)) {
                    int c2 = c;
                    while (c2 < 5 && (row & (0x10 >> c2))) c2++;
                    rect(x + c * px, y + r * px, x + c2 * px, y + (r + 1) * px, col);
                    c = c2;
                } else c++;
            }
        }
    }
}

static void text_center(const char *s, float y, float px, u64 col) {
    text(s, (SW - text_w(s, px)) / 2, y, px, col);
}

static void draw_hud(void) {
    char buf[32];
    sprintf(buf, "PUNTAJE: %d", score);
    text(buf, 12, 10, 2, RGB(255, 255, 255));
}

static void draw_menu(void) {
    text_center("SLITHER PS2", SH * 0.20f, 6, RGB(70, 230, 100));
    text_center("PRESIONA START", SH * 0.55f, 3, RGB(255, 255, 255));
    text_center("X: TURBO  PALANCA: GIRAR", SH * 0.72f, 2, RGB(170, 170, 200));
}

static void draw_pause(void) {
    rect(SW * 0.2f, SH * 0.3f, SW * 0.8f, SH * 0.7f, RGB(10, 10, 20));
    text_center("PAUSA", SH * 0.38f, 5, RGB(255, 255, 255));
    text_center("START: SEGUIR", SH * 0.58f, 2, RGB(170, 170, 200));
}

static void draw_dead(void) {
    char buf[32];
    rect(SW * 0.15f, SH * 0.2f, SW * 0.85f, SH * 0.8f, RGB(10, 10, 20));
    text_center("PERDISTE", SH * 0.26f, 5, RGB(230, 70, 80));
    sprintf(buf, "PUNTAJE: %d", score);
    text_center(buf, SH * 0.48f, 3, RGB(255, 255, 255));
    sprintf(buf, "RECORD: %d", best);
    text_center(buf, SH * 0.60f, 3, RGB(255, 220, 90));
    text_center("START: JUGAR DE NUEVO", SH * 0.72f, 2, RGB(170, 170, 200));
}

static void render(void) {
    float camx, camy;
    if (state == ST_MENU) { camx = WORLD / 2 - SW / 2; camy = WORLD / 2 - SH / 2; }
    else { camx = player.x[0] - SW / 2; camy = player.y[0] - SH / 2; }

    draw_background(camx, camy);
    draw_food(camx, camy);
    for (int i = 0; i < NAI; i++) draw_snake(&ai[i], camx, camy);
    if (state != ST_MENU) draw_snake(&player, camx, camy);

    switch (state) {
        case ST_MENU:  draw_menu(); break;
        case ST_PLAY:  draw_hud(); break;
        case ST_PAUSE: draw_hud(); draw_pause(); break;
        case ST_DEAD:  draw_dead(); break;
    }
}

int main(void) {
    gs = gsKit_init_global();
    gs->ZBuffering = GS_SETTING_OFF;
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    SW = (float)gs->Width;
    SH = (float)gs->Height;

    init_pad();
    reset_game();

    u32 pad = 0, prev;
    while (1) {
        prev = pad;
        pad = read_pad();
        u32 pressed = pad & ~prev;

        switch (state) {
            case ST_MENU:
                update_ai();
                if (pressed & PAD_START) { reset_game(); state = ST_PLAY; }
                break;
            case ST_PLAY:
                if (pressed & PAD_START) state = ST_PAUSE;
                else update_play(pad);
                break;
            case ST_PAUSE:
                if (pressed & PAD_START) state = ST_PLAY;
                break;
            case ST_DEAD:
                if (pressed & PAD_START) { reset_game(); state = ST_PLAY; }
                break;
        }

        render();
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    return 0;
}
