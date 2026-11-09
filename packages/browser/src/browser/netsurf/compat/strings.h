#pragma once
#include <stddef.h>
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *ns_strdup(const char *s);
#define strdup ns_strdup
