#include <pthread.h>

#include "linux/preempt.h"

/*
 * In userspace, pthreads are preemptible and can migrate CPUs at any time.
 *
 * In the kernel, preempt_disable() logic essentially guarantees that a marked
 * critical section owns its CPU for the relevant block. This is necessary for
 * various code paths, critically including the percpu system as it allows for
 * non-atomic reads and writes to CPU-local data structures.
 *
 * The high performance userspace equivalent would be to use thread local
 * storage to replace percpu data, but that would be complicated. It should be
 * correct to instead guarantee mutual exclusion for the critical sections.
 */

static pthread_mutex_t preempt_lock;

__attribute__((constructor))
static void preempt_init(void) {
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&preempt_lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

void preempt_disable(void)
{
	pthread_mutex_lock(&preempt_lock);
}

void preempt_enable(void)
{
	pthread_mutex_unlock(&preempt_lock);
}
