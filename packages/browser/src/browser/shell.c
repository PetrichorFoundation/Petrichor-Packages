/* The browser as a separate program: a window from the desktop (shared memory, winproto.h), the event loop,
 * and the few desktop services the browser code expects (titles, notifications, the clipboard). */
#include "desk.h"
#include "winproto.h"

#define MAXW 4

u32 g_accent = RGB(0x4a, 0x90, 0xd9);
static struct win wins[MAXW];
static int next_id = 1;
static int running = 1;

static void send_msg(u32 type, u32 id, u32 a, u32 b, u32 c, u32 d, const void *payload, u32 len)
{
    struct pw_msg m = {type, id, a, b, c, d, len};
    char buf[sizeof m + 256];
    if (len > 256) len = 256;
    m.len = len;
    memcpy(buf, &m, sizeof m);
    if (len) memcpy(buf + sizeof m, payload, len);
    write(PW_FD_UP, buf, sizeof m + len);
}

/* A shared buffer of w x h pixels; returns its id (and the pixels) or -1. */
static int new_buffer(int w, int h, u32 **px)
{
    long id = shm(SHM_CREATE, (u32)w * h * 4);
    if (id <= 0) return -1;
    if (shm(SHM_MAP, id) != 0) { shm(SHM_DROP, id); return -1; }
    *px = SHM_ADDR(id);
    return (int)id;
}

struct win *wm_open(const struct app *app, int w, int h, const char *title, void *data)
{
    struct win *wn = NULL;
    for (int i = 0; i < MAXW; i++)
        if (!wins[i].used) { wn = &wins[i]; break; }
    if (!wn) return NULL;
    memset(wn, 0, sizeof *wn);
    wn->shm = new_buffer(w, h, &wn->px);
    if (wn->shm < 0) return NULL;
    wn->used = 1;
    wn->w = w;
    wn->h = h;
    wn->app = app;
    wn->data = data;
    wn->id = next_id++;
    wn->dirty = 1;
    safestrcpy(wn->title, title, sizeof wn->title);
    send_msg(PW_SHM_WINDOW, wn->id, w, h, wn->shm, 0, title, strlen(title));
    return wn;
}

void wm_close(struct win *w)
{
    if (!w->used) return;
    send_msg(PW_DESTROY, w->id, 0, 0, 0, 0, NULL, 0);
    if (w->app->close) w->app->close(w);
    shm(SHM_DROP, w->shm);
    w->used = 0;
    int any = 0;
    for (int i = 0; i < MAXW; i++) any |= wins[i].used;
    if (!any) running = 0;
}

void wm_set_title(struct win *w, const char *title)
{
    if (!strcmp(w->title, title)) return;
    safestrcpy(w->title, title, sizeof w->title);
    send_msg(PW_TITLE, w->id, 0, 0, 0, 0, title, strlen(title));
}

void wm_invalidate(struct win *w) { w->dirty = 1; }
void wm_invalidate_rect(int x, int y, int w, int h)
{
    for (int i = 0; i < MAXW; i++) if (wins[i].used) wins[i].dirty = 1;
}
struct rect wm_client(struct win *w) { return (struct rect){0, 0, w->w, w->h}; }
void wm_notify(const char *msg) { send_msg(PW_NOTIFY, 0, 0, 0, 0, 0, msg, strlen(msg)); }

/* The clipboard is shared with the desktop through a file. */
#define CLIP_FILE "/tmp/clipboard"
void clip_set(const char *text, int len)
{
    int fd = open(CLIP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    write(fd, text, len);
    close(fd);
}

const char *clip_get(int *len)
{
    static char *buf;
    struct stat st;
    if (stat(CLIP_FILE, &st) < 0 || st.size <= 0) return NULL;
    free(buf);
    buf = malloc(st.size + 1);
    int fd = open(CLIP_FILE, O_RDONLY);
    int n = fd >= 0 && buf ? read(fd, buf, st.size) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

int ui_button(int x, int y, int w, int h, const char *label, int pressed)
{
    gfx_vgrad(x, y, w, h, pressed ? RGB(0xc4, 0xc8, 0xcc) : RGB(0xfa, 0xfa, 0xfa),
              pressed ? RGB(0xe0, 0xe2, 0xe4) : RGB(0xd8, 0xdb, 0xde));
    gfx_frame(x, y, w, h, RGB(0x8a, 0x92, 0x9a));
    int tw = gfx_text_width(label);
    return gfx_text(x + (w - tw) / 2, y + (h - 16) / 2, label, C_TEXT);
}

void ui_panel_inset(int x, int y, int w, int h)
{
    gfx_fill(x, y, w, h, C_WHITE);
    gfx_frame(x, y, w, h, RGB(0x9a, 0xa2, 0xaa));
}

static struct win *by_id(u32 id)
{
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].id == (int)id) return &wins[i];
    return NULL;
}

static void resize(struct win *w, int nw, int nh)
{
    if (nw < 100 || nh < 80 || nw > 2560 || nh > 1600 || (nw == w->w && nh == w->h)) return;
    u32 *px;
    int id = new_buffer(nw, nh, &px);
    if (id < 0) return;
    int old = w->shm;
    w->shm = id;
    w->px = px;
    w->w = nw;
    w->h = nh;
    w->dirty = 1;
    send_msg(PW_BUFFER, w->id, nw, nh, id, 0, NULL, 0);
    shm(SHM_DROP, old);                                   /* the desktop keeps its own reference until it switches */
}

static void handle(struct pw_event *e)
{
    struct win *w = by_id(e->id);
    if (!w) return;
    switch (e->type) {
    case PE_POINTER:
        if (w->app->mouse) w->app->mouse(w, e->a, e->b, e->c);
        break;
    case PE_KEY: {
        struct input_event ie = {INPUT_KEY, (u8)(e->b | IEF_DOWN), (u16)e->a, 0, 0};
        if (w->app->key) w->app->key(w, &ie);
        break;
    }
    case PE_WHEEL:
        if (w->app->wheel) w->app->wheel(w, (int)e->a);
        break;
    case PE_RESIZE:
        resize(w, e->a, e->b);
        break;
    case PE_CLOSE:
        if (!w->app->request_close || !w->app->request_close(w)) wm_close(w);
        break;
    }
}

static void paint(struct win *w)
{
    gfx_set_target(w->px, w->w, w->h);
    gfx_set_clip(0, 0, w->w, w->h);
    w->app->draw(w, wm_client(w));
    w->dirty = 0;
    send_msg(PW_DAMAGE, w->id, 0, 0, w->w, w->h, NULL, 0);
}

int main(int argc, char **argv)
{
    struct stat st;
    if (fstat(PW_FD_UP, &st) < 0 || fstat(PW_FD_DOWN, &st) < 0) {
        fprintf(2, "browser: start it from the desktop (dock, Applications menu, or a desktop terminal)\n");
        return 1;
    }
    browser_open(argc > 1 ? argv[1] : NULL);
    u8 ebuf[sizeof(struct pw_event) * 32];
    int elen = 0;
    u32 last_tick = uptime();
    while (running) {
        struct pollfd pf[32];
        struct win *owner[32];
        int n = 0;
        pf[n] = (struct pollfd){PW_FD_DOWN, POLLIN, 0};
        owner[n++] = NULL;
        int wait = 100;
        for (int i = 0; i < MAXW; i++) {
            struct win *w = &wins[i];
            if (!w->used) continue;
            if (w->app->pollfds) {
                int got = w->app->pollfds(w, &pf[n], 32 - n);
                for (int k = 0; k < got; k++) owner[n++] = w;
            }
            if (w->app->frame) {
                int ms = w->app->frame(w, uptime() * 10);
                if (ms >= 0 && ms < wait) wait = ms < 1 ? 1 : ms;
            }
            if (w->dirty) wait = 0;
        }
        for (int i = 0; i < MAXW; i++)
            if (wins[i].used && wins[i].dirty) paint(&wins[i]);
        int r = poll(pf, n, wait > 0 ? wait : 1);
        if (r > 0) {
            if (pf[0].revents & POLLIN) {
                int got = read(PW_FD_DOWN, ebuf + elen, sizeof ebuf - elen);
                if (got <= 0) break;                                        /* the desktop went away */
                elen += got;
                int pos = 0;
                while (elen - pos >= (int)sizeof(struct pw_event)) {
                    struct pw_event e;
                    memcpy(&e, ebuf + pos, sizeof e);
                    handle(&e);
                    pos += sizeof e;
                }
                memmove(ebuf, ebuf + pos, elen - pos);
                elen -= pos;
            } else if (pf[0].revents & (POLLHUP | POLLNVAL)) {
                break;
            }
            for (int i = 1; i < n; i++)
                if (pf[i].revents && owner[i]->used && owner[i]->app->fdready)
                    owner[i]->app->fdready(owner[i], &pf[i]);
        }
        u32 now = uptime();
        if (now - last_tick >= 100) {
            last_tick = now;
            for (int i = 0; i < MAXW; i++)
                if (wins[i].used && wins[i].app->tick) wins[i].app->tick(&wins[i], now);
            int st2;
            while (wait2(&st2, WNOHANG) > 0) {}
        }
    }
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used) wm_close(&wins[i]);
    return 0;
}
