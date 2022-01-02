/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _K_EYTZINGER_H
#define _K_EYTZINGER_H

/* One based indexing */
/* k = number of children */

static inline unsigned k_eytzinger_child(unsigned k, unsigned i, unsigned child)
{
	return (k * i + child) * (k - 1);
}

#endif /* _K_EYTZINGER_H */
