/* The browser runs as its own program: this is the small part of the desktop's API it uses, implemented in
 * shell.c on top of the window protocol (winproto.h): a shared-memory window the desktop shows for us. */
#pragma once
#include "gfx.h"

#define TITLE_H     0                    /* our window is the client area only: the desktop draws the frame */
#define NOFILE_USER 32

extern u32 g_accent;
#define C_ACCENT    g_accent
#define C_TEXT      RGB(0x2e, 0x34, 0x36)
#define C_TEXT_DIM  RGB(0x6b, 0x74, 0x7c)
#define C_WHITE     RGB(0xff, 0xff, 0xff)

static inline int abs_i(int v) { return v < 0 ? -v : v; }

enum { M_MOVE, M_DOWN, M_UP, M_DRAG, M_DBL, M_RDOWN, M_MDOWN };

struct win;

struct app {
    void (*draw)(struct win *w, struct rect c);
    void (*key)(struct win *w, struct input_event *e);
    void (*mouse)(struct win *w, int x, int y, int kind);
    void (*wheel)(struct win *w, int dir);
    void (*tick)(struct win *w, u32 now);
    int (*pollfd)(struct win *w, struct pollfd *p);
    int (*pollfds)(struct win *w, struct pollfd *p, int max);
    void (*fdready)(struct win *w, struct pollfd *p);
    void (*close)(struct win *w);
    int (*request_close)(struct win *w);
    int (*frame)(struct win *w, u32 now_ms);
};

struct win {
    int used;
    int x, y, w, h;                      /* x = y = 0; w, h: the buffer (client) size */
    int minw, minh;
    char title[64];
    const struct app *app;
    void *data;
    /* shell.c */
    int id, shm, dirty;
    u32 *px;
};

struct win *wm_open(const struct app *app, int w, int h, const char *title, void *data);
void wm_close(struct win *w);
void wm_set_title(struct win *w, const char *title);
void wm_invalidate(struct win *w);
void wm_invalidate_rect(int x, int y, int w, int h);
struct rect wm_client(struct win *w);
void wm_notify(const char *msg);

void clip_set(const char *text, int len);
const char *clip_get(int *len);

int ui_button(int x, int y, int w, int h, const char *label, int pressed);
void ui_panel_inset(int x, int y, int w, int h);

void browser_open(const char *url);
