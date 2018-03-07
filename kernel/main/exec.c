#include <sched/process.h>
#include <sched/readyqueue.h>

#include <stdint.h>
#include <errno.h>
#include <mm/paging.h>
#include <fs/fs.h>
#include <print.h>
#include <userspace.h>
#include <mm/memset.h>
#include <mm/heap.h>
#include <sched/elf.h>
#include <modules.h>

struct Process initProcess;

extern void uthreadInit(thread_t thread, void *start, uint64_t arg1, uint64_t arg2, void *userspaceStackpointer);
extern void initExecRetThread(thread_t thread, void *start, uint64_t arg1, uint64_t arg2, void *userspaceStackpointer);

union elfBuf {
	struct ElfHeader header;
	struct ElfPHEntry phEntry;

};

static int checkElfHeader(struct ElfHeader *header) {
	if (memcmp(&header->magic[0], "\x7f""ELF", 4)) {
		return -EINVAL;
	}
	if (header->type != 2) {
		return -EINVAL;
	}
	if (header->machine != 0x3E) {
		return -EINVAL;
	}
	if (header->phentSize != sizeof(struct ElfPHEntry)) {
		return -EINVAL;
	}
	return 0;
}

static int elfLoad(struct File *f, struct ElfPHEntry *entry) {
	int error = fsSeek(f, entry->pOffset, SEEK_SET);
	if (error) return error;

	void *vaddr = (void *)entry->pVAddr;
	error = validateUserPointer(vaddr, entry->pMemSz);
	if (error) return error;

	pageFlags_t flags = PAGE_FLAG_INUSE | PAGE_FLAG_CLEAN | PAGE_FLAG_USER;
	if (entry->flags & PHFLAG_EXEC) {
		flags |= PAGE_FLAG_EXEC;
	}
	//if (entry->flags & PHFLAG_WRITE) {
		flags |= PAGE_FLAG_WRITE;
	//}
	//readable flag is ignored
	allocPageAt(vaddr, entry->pMemSz, flags);

	error = fsRead(f, vaddr, entry->pFileSz);
	if (error != (int)entry->pFileSz) {
		if (error >= 0) {
			error = -EINVAL;
		}
		goto deallocP;
	}

	return 0;

	deallocP:
	deallocPages(vaddr, entry->pMemSz);
	return error;
}

static int execCommon(thread_t mainThread, const char *fileName, void **startAddr, void **sp) {
	int error;
	//Open the executable
	struct File f;
	struct Inode *inode = getInodeFromPath(rootDir, fileName);
	if (!inode) {
		error = -ENOENT;
		goto ret;
	}
	error = fsOpen(inode, &f);
	if (error) goto ret;

	//Get the ELF header
	struct ElfHeader header;
	ssize_t read = fsRead(&f, &header, sizeof(struct ElfHeader));
	if (read != sizeof(struct ElfHeader)) {
		//file too small or error occured during read
		error = -EINVAL;
		if (read < 0) {
			error = read;
		}
		goto closef;
	}
	error = checkElfHeader(&header);
	if (error) goto closef;

	//count nrof load commands
	unsigned int nrofLoads = 1; //Set to 1 to allocate stack as well
	struct ElfPHEntry phEntry;
	error = fsSeek(&f, header.phOff, SEEK_SET);
	if (error) goto closef;
	for (unsigned int i = 0; i < header.phnum; i++) {
		fsRead(&f, &phEntry, sizeof(struct ElfPHEntry));
		if (phEntry.type == PHTYPE_LOAD) {
			nrofLoads++;
		}
	}

	//create process memory struct
	struct Process *proc = mainThread->process;
	proc->nrofMemEntries = nrofLoads;
	proc->memEntries = kmalloc(sizeof(struct MemoryEntry) * nrofLoads);
	if (!proc->memEntries) {
		error = -ENOMEM;
		goto closef;
	}
	memset(proc->memEntries, 0, sizeof(struct MemoryEntry) * nrofLoads);

	//loop over all program header entries
	uintptr_t highestAddr = 0;
	uintptr_t end;
	nrofLoads = 0;
	for (unsigned int i = 0; i < header.phnum; i++) {
		error = fsSeek(&f, header.phOff + (i * sizeof(struct ElfPHEntry)), SEEK_SET);
		if (error) goto freeMemStruct;
		fsRead(&f, &phEntry, sizeof(struct ElfPHEntry));
		switch (phEntry.type) {
			case PHTYPE_NULL:
				break;
			case PHTYPE_LOAD:
				error = elfLoad(&f, &phEntry);
				proc->memEntries[nrofLoads].vaddr = (void *)(phEntry.pVAddr);
				proc->memEntries[nrofLoads].size = phEntry.pMemSz;
				proc->memEntries[nrofLoads].flags = phEntry.flags & ~MEM_FLAG_SHARED;

				end = (uintptr_t)(proc->memEntries[nrofLoads].vaddr) + proc->memEntries[nrofLoads].size;
				if (end > highestAddr) {
					highestAddr = end;
				}

				nrofLoads++;
				break;
			default:
				error = -EINVAL;
		}
		if (error) goto freeMemStruct;
	}
	*startAddr = (void *)header.entryAddr;

	//allocate stack
	if (highestAddr & (PAGE_SIZE - 1)) {
		highestAddr &= ~(PAGE_SIZE - 1);
		highestAddr += PAGE_SIZE;
	}
	
	*sp = (void *)(highestAddr + THREAD_STACK_SIZE); //stack pointer = program break
	proc->brkEntry = &proc->memEntries[nrofLoads];

	proc->memEntries[nrofLoads].vaddr = (void *)highestAddr;
	proc->memEntries[nrofLoads].size = THREAD_STACK_SIZE;
	proc->memEntries[nrofLoads].flags = MEM_FLAG_WRITE;
	allocPageAt(proc->memEntries[nrofLoads].vaddr, proc->memEntries[nrofLoads].size,
		PAGE_FLAG_INUSE | PAGE_FLAG_CLEAN | PAGE_FLAG_USER | PAGE_FLAG_WRITE);

	return 0;
	
	freeMemStruct:
	kfree(proc->memEntries);
	closef:
	ret:
	return error;
}

int execInit(const char *fileName) {
	int error;
	uintptr_t kernelStackBottom = (uintptr_t)allocKPages(THREAD_STACK_SIZE, PAGE_FLAG_CLEAN | PAGE_FLAG_WRITE);
	if (!kernelStackBottom) {
		error = -ENOMEM;
		goto ret;
	}
	//ThreadInfo struct is at the top of the kernel stack
	struct ThreadInfo *mainThread = (struct ThreadInfo *)(kernelStackBottom + THREAD_STACK_SIZE - sizeof(struct ThreadInfo));

	mainThread->priority = 1;
	mainThread->jiffiesRemaining = TIMESLICE_BASE << 1;
	mainThread->cpuAffinity = 1;

	mainThread->process = &initProcess;
	initProcess.mainThread = mainThread;
	initProcess.pid = 1;
	//proc->cwd = "/";

	//TODO make this arch independent
	uint64_t cr3;
	asm ("mov rax, cr3" : "=a"(cr3));
	initProcess.addressSpace = cr3;

	/*struct Inode *stdout = getInodeFromPath(rootDir, "/dev/tty1");
	error = fsOpen(stdout, &proc->inlineFDs[1]);
	if (error) {
		goto freeProcess;
	}*/

	void *start;
	void *sp;
	error = execCommon(mainThread, fileName, &start, &sp);
	if (error) goto deallocMainThread;

	uthreadInit(mainThread, start, 0, 0, sp);

	readyQueuePush(mainThread);

	return 0;

	deallocMainThread:
	deallocPages((void *)kernelStackBottom, THREAD_STACK_SIZE);
	ret:
	return error;
}

int sysExec(const char *fileName, char *const argv[] __attribute__ ((unused)), char *const envp[] __attribute__ ((unused))) {
	int error;
	int fnLen = strlen(fileName) + 1;
	if (fnLen > PAGE_SIZE) return -EINVAL;
	char namebuf[fnLen];

	error = validateUserString(fileName);
	if (error) goto ret;

	memcpy(namebuf, fileName, fnLen);

	error = fsCloseOnExec();
	if (error) goto ret;

	thread_t curThread = getCurrentThread();
	destroyProcessMem(curThread->process);
	kfree(curThread->process->memEntries);

	void *start;
	void *sp;
	error = execCommon(curThread, namebuf, &start, &sp);
	if (error) goto ret;

	initExecRetThread(curThread, start, 0, 0, sp);
	
	return 0;

	ret:
	return error;
}