#include <sched/thread.h>

#include <stdint.h>
#include <mm/paging.h>
#include <apic.h>
#include <print.h>
#include "queue.h"

#define THREAD_STACK_SIZE	0x2000

extern void migrateMainStack(thread_t mainThread);

int createKernelThread(thread_t *thread, void *(*start)(void *), void *arg) {
	//alloc thread stack
	uintptr_t stackBottom = (uintptr_t)(allocKPages(THREAD_STACK_SIZE, PAGE_FLAG_WRITE | PAGE_FLAG_INUSE));
	if (!stackBottom) {
		return THRD_NOMEM;
	}
	struct threadInfo *thrd = (thread_t)(stackBottom + THREAD_STACK_SIZE - sizeof(struct threadInfo));
	*thread = thrd;

	thrd->state = THREADSTATE_SCHEDWAIT;
	thrd->priority = 0;
	thrd->jiffiesRemaining = 1;
	thrd->detached = false;

	kthreadInit(thrd, start, arg);

	//now push it to the scheduling queue
	pushThread(thrd);
	
	return THRD_SUCCESS;
}

int createThreadFromMain(thread_t *thread) {
	//alloc thread stack
	uintptr_t stackBottom = (uintptr_t)(allocKPages(THREAD_STACK_SIZE, PAGE_FLAG_WRITE | PAGE_FLAG_INUSE));
	if (!stackBottom) {
		return THRD_NOMEM;
	}
	struct threadInfo *thrd = (thread_t)(stackBottom + THREAD_STACK_SIZE - sizeof(struct threadInfo));
	*thread = thrd;
	migrateMainStack(thrd);

	thrd->state = THREADSTATE_RUNNING;
	thrd->priority = 0;
	thrd->jiffiesRemaining = 1;
	thrd->detached = true;

	uint32_t cpu = getCPU();
	cpuInfos[cpu].currentThread = thrd;

	//now push it to the scheduling queue
	pushThread(thrd);
	
	return THRD_SUCCESS;
}

thread_t switchThread(void) {
	uint32_t cpu = getCPU();
	thread_t oldThread = cpuInfos[cpu].currentThread;
	thread_t newThread = pullThread();
	if (!newThread || newThread == oldThread) {
		//sprint("No new threads!");
		newThread = oldThread;
		/*while (1) {
			asm ("cli");
			asm ("hlt");
		}*/
	}
	pushThread(oldThread);
	cpuInfos[cpu].currentThread = newThread;
	return newThread;
}