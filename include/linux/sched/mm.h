#ifndef _LINUX_SCHED_MM_H
#define _LINUX_SCHED_MM_H

#define PF_MEMALLOC_NOFS 0

static inline unsigned int memalloc_nofs_save(void)
{
	unsigned int flags = current->flags & PF_MEMALLOC_NOFS;
	current->flags |= PF_MEMALLOC_NOFS;
	return flags;
}

static inline void memalloc_nofs_restore(unsigned int flags)
{
	current->flags = (current->flags & ~PF_MEMALLOC_NOFS) | flags;
}

#endif /* _LINUX_SCHED_MM_H */
