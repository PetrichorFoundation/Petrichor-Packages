/* The few C library functions NetSurf's libraries need beyond Petrichor's user library. */
#include <stddef.h>
#include <stdarg.h>
#include "stdio.h"

int write(int fd, const void *buf, unsigned int n);
void exit(int status) __attribute__((noreturn));
int strncmp(const char *a, const char *b, size_t n);

struct ns_file { int fd; };
static struct ns_file f_err = {2}, f_out = {1};
FILE *stderr = &f_err, *stdout = &f_out;
int errno;

int ns_fprintf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }   /* debug output: dropped */
FILE *ns_fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int ns_fclose(FILE *f) { (void)f; return 0; }
int ns_fputs(const char *s, FILE *f) { (void)s; (void)f; return 0; }

void abort(void)
{
    write(2, "netsurf libraries: abort\n", 25);
    exit(134);
}

void ns_assert_fail(const char *expr, const char *file, int line) { (void)expr; (void)file; (void)line; abort(); }

void *malloc(size_t n);
size_t strlen(const char *s);
void *memcpy(void *d, const void *s, size_t n);

char *ns_strdup(const char *s)
{
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a) return d;
    }
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    const char *lo = base;
    while (n) {
        const char *mid = lo + (n / 2) * size;
        int c = cmp(key, mid);
        if (!c) return (void *)mid;
        if (c > 0) { lo = mid + size; n -= n / 2 + 1; }
        else n /= 2;
    }
    return NULL;
}

static unsigned long parse_ul(const char *s, char **end, int base, int *neg)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    *neg = 0;
    if (*s == '-' || *s == '+') *neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; }
    else if (base == 0) base = s[0] == '0' ? 8 : 10;
    unsigned long v = 0;
    for (;; s++) {
        int c = *s, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return v;
}

long strtol(const char *s, char **end, int base) { int neg; unsigned long v = parse_ul(s, end, base, &neg); return neg ? -(long)v : (long)v; }
unsigned long strtoul(const char *s, char **end, int base) { int neg; unsigned long v = parse_ul(s, end, base, &neg); return neg ? -v : v; }
