#ifndef __LINUX_PREEMPT_H
#define __LINUX_PREEMPT_H

extern void preempt_disable(void);
extern void preempt_enable(void);

#define sched_preempt_enable_no_resched()	preempt_enable()
#define preempt_enable_no_resched()		preempt_enable()
#define preempt_check_resched()			do { } while (0)

#define preempt_disable_notrace()		preempt_disable()
#define preempt_enable_no_resched_notrace()	preempt_enable()
#define preempt_enable_notrace()		preempt_enable()
#define preemptible()				0

#endif /* __LINUX_PREEMPT_H */
