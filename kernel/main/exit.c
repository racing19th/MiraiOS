#include <sched/process.h>
#include <mm/pagemap.h>
#include <mm/heap.h>
#include <mm/physpaging.h>
#include <mm/mmap.h>
#include <mm/memset.h>
#include <fs/fs.h>
#include <arch/tlb.h>
#include <userspace.h>
#include <modules.h>
#include <sched/readyqueue.h>
#include <panic.h>
#include <sched/pgrp.h>
#include <sched/signal.h>
#include <sched/queue.h>

static void unlinkChild(struct Process *proc) {
	struct Process *parent = proc->parent;
	acquireSpinlock(&parent->lock);
	if (parent->children == proc) {
		parent->children = proc->nextChild;
	} else { //not first, prevChild should be valid
		proc->prevChild->nextChild = proc->nextChild;
	}
	releaseSpinlock(&parent->lock);
}

static bool checkFilter(pid_t filter, struct Process *child) {
	if (!filter) {
		return true;
	}
	if (filter > 0 && child->pid == filter) {
		return true;
	}
	//TODO check for group id
	return false;
}

void removeProcess(struct Process *proc) { //remove zombie
	//delete root page table
	deallocPhysPage(proc->addressSpace);

	//delete main thread
	//deallocThread(proc->mainThread); already done on kthreadExit
	leaveGroup(proc);
	unlinkChild(proc);
	procHTDel(proc);
	kfree(proc);
}

void exitProcess(struct Process *proc) { //can also be called on failed fork
	proc->state = PROCSTATE_FINISHED;

	//Orphan any children
	acquireSpinlock(&initProcess.lock);
	struct Process *child = proc->children;
	while (child) {
		acquireSpinlock(&child->lock);
		struct Process *nextChild = child->nextChild;
		linkChild(&initProcess, child);
		releaseSpinlock(&child->lock);
		child = nextChild;
	}
	releaseSpinlock(&initProcess.lock);

	//destroy memory
	mmapDestroy(proc);
	
	//destroy files
	for (int i = 0; i < NROF_INLINE_FDS + proc->nrofFDs; i++) {
		sysClose(i);
	}

	//notify parent TODO send SIGCHLD
	struct Process *parent = proc->parent;
	acquireSpinlock(&parent->lock);
	//TODO add support for multiple threads
	thread_t parentThread = parent->mainThread;
	acquireSpinlock(&parentThread->lock);
	if (parentThread->state == THREADSTATE_PIDWAIT && checkFilter(parentThread->waitPid, proc)) {
		parentThread->waitProc = proc;
		readyQueuePush(parentThread->queueEntry); //TODO
	}
	releaseSpinlock(&parentThread->lock);
	releaseSpinlock(&parent->lock);
}

//exit info already set in process
void signalExit(void) {
	thread_t curThread = getCurrentThread();
	if (curThread->process->pid == 1) {
		panic("\n[PANIC] Attempted to kill init!");
	}
	//kill all other threads
	//set current thread as mainThread
	
	threadQueueRemove(curThread);
	exitProcess(curThread->process);

	//exit thread
	curThread->detached = true;
	kthreadExit(NULL);
}

void sysExit(int exitValue) {
	struct Process *proc = getCurrentThread()->process;
	proc->exitInfo.si_status = (exitValue & SIG_STATUS_MASK) | SIG_EXITED;
	signalExit();
}

pid_t sysWaitPid(pid_t filter, void *waitStatus, int options) {
	//check if any of the current children that match the filter are status=PROCSTATE_FINISHED 
	//if so return
	pid_t ret = 0;
	bool exist = false;
	thread_t curThread = getCurrentThread();
	struct Process *proc = curThread->process;

	acquireSpinlock(&curThread->lock); //lock curThread early to prevent child exit during execution of this function
	acquireSpinlock(&proc->lock);
	struct Process *child = proc->children;
	while (child) {
		acquireSpinlock(&child->lock);
		if (checkFilter(filter, child)) {
			exist = true;
			if (child->state == PROCSTATE_FINISHED) {
				releaseSpinlock(&child->lock);
				ret = child->pid;
				break;
			}
		}
		struct Process *next = child->nextChild;
		releaseSpinlock(&child->lock);
		child = next;
	}
	releaseSpinlock(&proc->lock);
	if (ret > 0) {
		if (waitStatus) {
			//*waitStatus = child->exitValue; //TODO expand this when signals get added
			memcpy(waitStatus, &proc->exitInfo, sizeof(proc->exitInfo));
		}
		removeProcess(child);
		releaseSpinlock(&curThread->lock);
		return ret;
	}
	if (!exist) {
		releaseSpinlock(&curThread->lock);
		return -ECHILD; //child does not exist
	}
	
	//save filter
	//Wait for children to exit
	
	curThread->state = THREADSTATE_PIDWAIT;
	curThread->waitPid = filter;
	kthreadStop();

	if (waitStatus) {
		//*waitStatus = curThread->waitProc->exitValue;
		memcpy(waitStatus, &proc->exitInfo, sizeof(proc->exitInfo));
	}
	removeProcess(curThread->waitProc);

	return curThread->waitPid;
}
