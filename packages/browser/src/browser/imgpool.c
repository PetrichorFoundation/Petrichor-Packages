/* Image decoding on worker threads.  PNG, JPEG and GIF decoding is the heaviest work a page load does on the
 * browser's own side, and img_decode touches nothing but its input and malloc, so two worker threads do it
 * while the main thread keeps drawing and answering input.  A finished job is put on the done list and one
 * byte is written to a pipe, which the main loop polls like any other file descriptor. */
#include "web.h"
#include "pthread.h"

#define NWORKERS 2

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t more = PTHREAD_COND_INITIALIZER;
static struct imgjob *todo, *todo_tail, *done;
static int wake[2] = {-1, -1}, started, busy;

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!todo)
            pthread_cond_wait(&more, &lock);
        struct imgjob *j = todo;
        todo = j->next;
        if (!todo)
            todo_tail = NULL;
        u32 gen = j->gen;
        pthread_mutex_unlock(&lock);

        j->img = gen == page_generation ? img_decode(j->buf + j->off, j->len) : NULL;   /* skip stale pages */
        free(j->buf);
        j->buf = NULL;

        pthread_mutex_lock(&lock);
        j->next = done;
        done = j;
        busy--;
        pthread_mutex_unlock(&lock);
        write(wake[1], "i", 1);
    }
    return NULL;
}

static int start(void)
{
    if (started)
        return started > 0 ? 0 : -1;
    started = -1;
    if (pipe(wake) < 0)
        return -1;
    int ok = 0;
    for (int i = 0; i < NWORKERS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) {
            pthread_detach(t);
            ok++;
        }
    }
    if (!ok)
        return -1;
    started = 1;
    return 0;
}

int imgpool_submit(int res, u8 *buf, int off, int len)
{
    if (start() < 0)
        return -1;
    struct imgjob *j = calloc(1, sizeof *j);
    if (!j)
        return -1;
    j->res = res;
    j->gen = page_generation;
    j->buf = buf;
    j->off = off;
    j->len = len;
    pthread_mutex_lock(&lock);
    if (todo_tail)
        todo_tail->next = j;
    else
        todo = j;
    todo_tail = j;
    busy++;
    pthread_cond_signal(&more);
    pthread_mutex_unlock(&lock);
    return 0;
}

int imgpool_fd(void) { return started > 0 ? wake[0] : -1; }
int imgpool_busy(void) { return busy; }

struct imgjob *imgpool_take(void)
{
    char c[64];
    pthread_mutex_lock(&lock);
    struct imgjob *j = done;
    if (j)
        done = j->next;
    if (!done) {
        struct pollfd pf = {wake[0], POLLIN, 0};
        while (poll(&pf, 1, 0) > 0 && read(wake[0], c, sizeof c) > 0)   /* drain the wake-ups */
            pf.revents = 0;
    }
    pthread_mutex_unlock(&lock);
    return j;
}
