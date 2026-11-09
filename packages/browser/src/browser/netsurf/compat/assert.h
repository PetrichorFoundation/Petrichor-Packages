#pragma once
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
void ns_assert_fail(const char *expr, const char *file, int line);
#define assert(x) ((x) ? (void)0 : ns_assert_fail(#x, __FILE__, __LINE__))
#endif
