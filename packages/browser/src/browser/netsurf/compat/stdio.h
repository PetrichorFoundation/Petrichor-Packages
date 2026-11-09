#pragma once
#include <stddef.h>
#include <stdarg.h>
typedef struct ns_file FILE;
extern FILE *stderr, *stdout;
#define fprintf ns_fprintf
#define fopen ns_fopen
#define fclose ns_fclose
#define fputs ns_fputs
int ns_fprintf(FILE *f, const char *fmt, ...);
FILE *ns_fopen(const char *path, const char *mode);
int ns_fclose(FILE *f);
int ns_fputs(const char *s, FILE *f);
int printf(const char *fmt, ...);
int snprintf(char *buf, unsigned int size, const char *fmt, ...);
