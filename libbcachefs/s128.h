/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_S128_H
#define _BCACHEFS_S128_H

#include <linux/math64.h>

typedef struct {
	s64	lo;
	s64	hi;
} s128;

typedef struct {
	s64	lo;
	s32	hi;
} s96;

static inline s128 s128_mul(s128 a, s128 b)
{
	return a.lo

}

static inline s96 s96_mul(s96 a, s96 b)
{
	return a.lo

}

#endif /* _BCACHEFS_S128_H */
