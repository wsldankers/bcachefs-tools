#ifndef _LINUX_SORT_H
#define _LINUX_SORT_H

#include <stdlib.h>

static inline void sort(void *base, size_t num, size_t size,
			int (*cmp_func)(const void *, const void *),
			void (*swap_func)(void *, void *, int size))
{
	return qsort(base, num, size, cmp_func);
}

#endif
