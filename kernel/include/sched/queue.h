#ifndef INCLUDE_SCHED_QUEUE_H
#define INCLUDE_SCHED_QUEUE_H

#include <sched/thread.h>
#include <sched/spinlock.h>

struct ThreadInfoQueue {
	struct ThreadQueueEntry *first;
	struct ThreadQueueEntry *last;
	int nrofThreads;
};

/*
Pops a thread from the front of a queue.
*/
struct ThreadQueueEntry *threadQueuePop(struct ThreadInfoQueue *queue);

/*
Pushes a thread to the back of a queue
*/
void threadQueuePush(struct ThreadInfoQueue *queue, struct ThreadInfo *thread);

/*
Pushes a thread to the front of a queue
*/
void threadQueuePushFront(struct ThreadInfoQueue *queue, struct ThreadInfo *thread);

/*
Removes a thread from a queue
*/
void threadQueueRemove(struct ThreadInfo *thread);

#endif