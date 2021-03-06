#include <fs/fs.h>
#include <fs/fd.h>
#include <fs/pipe.h>
#include <fs/devfile.h>
#include <sched/process.h>
#include <mm/heap.h>
#include <mm/memset.h>
#include <errno.h>
#include <userspace.h>
#include <modules.h>
#include <stdarg.h>

/*
Get file or pipe struct from fd

return value:
	-EBADF on error
	0 on file
	1 on read-only pipe
	3 on write-only pipe
*/
int getFileFromFD(struct Process *proc, int fd, union FilePipe *fp) {
	struct ProcessFile *pf;
	struct File *f;
	if (fd < NROF_INLINE_FDS) {
		pf = &proc->inlineFDs[fd];
	} else if (fd - NROF_INLINE_FDS < proc->nrofFDs && proc->fds[fd].flags & PROCFILE_FLAG_USED) {
		pf = &proc->fds[fd - NROF_INLINE_FDS];
	} else {
		return -EBADF;
	}

	int ret;
	if (pf->flags & PROCFILE_FLAG_PIPE) {
		fp->pipe = pf->pipe;
		ret = (pf->flags & PROCFILE_FLAG_PIPE_WRITE)? 3 : 1;
	} else {
		f = (pf->flags & PROCFILE_FLAG_SHARED)? pf->sharedFile : &pf->file;
		if (!f->inode) {
			return -EBADF;
		}
		fp->file = f;
		ret = 0;
	}

	return ret;
}

int allocateFD(struct Process *proc, struct ProcessFile **pf) {
	int fileno = 0;
	struct ProcessFile *file = NULL;

	for (unsigned int i = 0; i < NROF_INLINE_FDS; i++) {
		if ( !(proc->inlineFDs[i].flags & PROCFILE_FLAG_USED) ) {
			file = &proc->inlineFDs[i];
			fileno = i;
			break;
		}
	}
	if (!file && !proc->fds) {
		proc->fds = kmalloc(sizeof(struct ProcessFile));
		if (!proc->fds) {
			return -ENOMEM;
		}
		proc->nrofFDs = 1;
	} else if (!file) {
		//search for free entry
		for (int i = 0; i < proc->nrofFDs; i++) {
			if ( !(proc->fds[i].flags & PROCFILE_FLAG_USED)) {
				file = &proc->fds[i];
				fileno = i + NROF_INLINE_FDS;
				break;
			}
		}
		if (!file) {
			proc->nrofFDs++;
			file = krealloc(proc->fds, proc->nrofFDs * sizeof(struct ProcessFile));
			if (!file) {
				return -ENOMEM;
			}
			proc->fds = file;
			file = &proc->fds[proc->nrofFDs - 1];
			fileno = NROF_INLINE_FDS;
		}
	}
	file->flags = PROCFILE_FLAG_USED;
	if (pf) {
		*pf = file;
	}
	return fileno;
}

static struct Inode *getCwd(int dirfd, struct Process *proc) {
	if (dirfd == AT_FDCWD) {
		return proc->cwd;
	}
	struct Inode *ret = NULL;
	acquireSpinlock(&proc->fdLock);
	union FilePipe fp;
	int error = getFileFromFD(proc, dirfd, &fp);
	if (error) goto out;

	ret = fp.file->inode;

	out:
	releaseSpinlock(&proc->fdLock);
	return ret;
}

int sysWrite(int fd, const void *buffer, size_t size) {
	int error = validateUserPointer(buffer, size);
	if (error) {
		return error;
	}

	//printk("write: %d %x %x", fd, buffer, size);

	union FilePipe fp;
	error = getFileFromFD(getCurrentThread()->process, fd, &fp);
	if (error < 0) {
		return error;
	} else if (error & 1) {
		if (!(error & 2)) {
			return -EBADF;
		}
		return pipeWrite(fp.pipe, buffer, size);
	}
	error = fsWrite(fp.file, buffer, size);
	return error;
}

int sysRead(int fd, void *buffer, size_t size) {
	int error = validateUserPointer(buffer, size);
	if (error) {
		return error;
	}

	//printk("read: %d %x %x", fd, buffer, size);

	union FilePipe fp;
	error = getFileFromFD(getCurrentThread()->process, fd, &fp);
	if (error < 0) {
		return error;
	} else if (error & 1) {
		if (error & 2) {
			return -EBADF;
		}
		return pipeRead(fp.pipe, buffer, size);
	}
	return fsRead(fp.file, buffer, size);
}

int sysIoctl(int fd, unsigned long request, unsigned long arg) {

	union FilePipe fp;
	int error = getFileFromFD(getCurrentThread()->process, fd, &fp);
	if (error) {
		return -EBADF;
	}
	struct File *f = fp.file;

	acquireSpinlock(&f->lock);
	acquireSpinlock(&f->inode->lock);

	struct DevFileOps *ops = f->inode->ops;
	int ret = -ENOSYS;
	if ((f->inode->type & ITYPE_MASK) == ITYPE_CHAR && ops && ops->ioctl) {
		ret = ops->ioctl(f, request, arg);
	}

	releaseSpinlock(&f->inode->lock);
	releaseSpinlock(&f->lock);

	return ret;
}

int sysOpen(int dirfd, const char *fileName, unsigned int flags) {
	//check flags
	if (flags & SYSOPEN_FLAG_DIR && flags & SYSOPEN_FLAG_WRITE) {
		return -EISDIR;
	}

	int error = validateUserString(fileName);
	if (error) return error;

	struct Process *proc = getCurrentThread()->process;
	acquireSpinlock(&proc->fdLock);
	struct ProcessFile *pf;
	int fileno = allocateFD(proc, &pf);
	releaseSpinlock(&proc->fdLock);
	if (fileno < 0) {
		return fileno;
	}

	pf->file.flags = flags & (SYSOPEN_FLAG_READ | SYSOPEN_FLAG_WRITE | SYSOPEN_FLAG_DIR | SYSOPEN_FLAG_APPEND);

	struct Inode *cwd = getCwd(dirfd, proc);
	struct Inode *inode = getInodeFromPath(cwd, fileName);
	if (!inode && flags & SYSOPEN_FLAG_CREATE) {
		//create file
		int fileNameIndex;
		struct Inode *dir = getBaseDirFromPath(cwd, &fileNameIndex, fileName);
		if (!dir) {
			return -ENOENT; //dir does not exist
		}

		//check write permission on dir
		if (!fsAccessAllowed(dir, PERM_W)) {
			return -EACCES;
		}

		uint32_t type = (flags & SYSOPEN_FLAG_DIR)? ITYPE_DIR : ITYPE_FILE;
		type |= ((flags >> SYSOPEN_PERM_SHIFT) & SYSOPEN_PERM_MASK) << CREATE_PERM_SHIFT;
		error = fsCreate(&pf->file, dir, &fileName[fileNameIndex], type);
		if (error) {
			return error;
		}
		return fileno;
	} else if (flags & SYSOPEN_FLAG_EXCL) {
		return -EEXIST;
	} else if (!inode) {
		return -ENOENT;
	}

	//check permissions
	int mode = ((flags & SYSACCESS_R)? PERM_R : 0) | ((flags & SYSACCESS_W)? PERM_W : 0);
	if (!fsAccessAllowed(inode, mode)) {
		return -EACCES;
	}

	pf->file.inode = inode;

	if (flags & SYSOPEN_FLAG_TRUNC && (inode->type & ITYPE_MASK) == ITYPE_FILE) {
		error = fsTruncate(&pf->file, 0);
		if (error) return error;
	}
	error = fsOpen(&pf->file);
	if (error) {
		return error;
	}

	return fileno;
}

int sysClose(int fd) {
	struct Process *proc = getCurrentThread()->process;
	struct ProcessFile *pf = (fd < NROF_INLINE_FDS)? &proc->inlineFDs[fd] : &proc->fds[fd - NROF_INLINE_FDS];
	if ( !(pf->flags & PROCFILE_FLAG_USED)) {
		return -EBADF;
	}

	acquireSpinlock(&proc->fdLock);
	if (pf->flags & PROCFILE_FLAG_SHARED) {
		int refCount = --pf->sharedFile->refCount;
		if (!refCount) {
			kfree(pf->sharedFile);
		}
	} else {
		fsClose(&pf->file);
	}
	
	pf->flags = 0;
	if (fd >= NROF_INLINE_FDS) {
		if ( !(--proc->nrofUsedFDs)) {
			kfree(proc->fds);
		}
	}
	releaseSpinlock(&proc->fdLock);
	return 0;
}

/*
Gets a directory entry
return values:
<0: error value (on fail)
0: end of directory
>0: file name length
*/
int sysGetDent(int fd, struct GetDent *buf) {
	int ret = validateUserPointer(buf, sizeof(struct GetDent));
	if (ret) return ret;

	union FilePipe fp;
	ret = getFileFromFD(getCurrentThread()->process, fd, &fp);
	if (ret) return -EBADF;

	acquireSpinlock(&fp.file->lock);
	ret = fsGetDent(fp.file->inode, buf, fp.file->offset);
	if (ret) {
		fp.file->offset += 1;
	}
	releaseSpinlock(&fp.file->lock);
	return ret;
}

int sysChDir(const char *path) {
	int error = validateUserString(path);
	if (error) return error;

	struct Process *proc = getCurrentThread()->process;
	struct Inode *inode = getInodeFromPath(proc->cwd, path);
	if (!inode) {
		return -ENOENT;
	}
	if (!isDir(inode)) {
		return -ENOTDIR;
	}

	inode->refCount++;
	proc->cwd->refCount--;
	proc->cwd = inode;

	return 0;
}

int sysAccess(int dirfd, const char *path, int mode) {
	if (mode & ~0xF || (mode & SYSACCESS_OK && mode & (SYSACCESS_R | SYSACCESS_W | SYSACCESS_X))) {
		return -EINVAL;
	}
	int error = validateUserString(path);
	if (error) return error;

	struct Process *proc = getCurrentThread()->process;
	struct Inode *cwd = getCwd(dirfd, proc);
	struct Inode *inode = getInodeFromPath(cwd, path);
	if (!inode) {
		return -ENOENT;
	}
	if (mode & SYSACCESS_OK) {
		return 0;
	}

	return (fsAccessAllowed(inode, mode >> 1))? 0 : -EACCES;
}

int sysSeek(int fd, int64_t offset, int whence) {
	union FilePipe fp;
	int error = getFileFromFD(getCurrentThread()->process, fd, &fp);
	if (error) {
		return -EBADF;
	}


	return fsSeek(fp.file, offset, whence);
}

int sysUnlink(int dirfd, const char *path, int flags) {
	int error = validateUserString(path);
	if (error) return error;

	struct Process *proc = getCurrentThread()->process;
	struct Inode *cwd = getCwd(dirfd, proc);

	struct Inode *inode = getInodeFromPath(cwd, path);
	if (!inode) {
		return -ENOENT;
	}
	if (isDir(inode) && !(flags & AT_REMOVEDIR)) {
		return -EISDIR;
	}

	int fileNameIndex;
	struct Inode *dir = getBaseDirFromPath(cwd, &fileNameIndex, path);

	return fsUnlink(dir, &path[fileNameIndex]);
}

int sysRename(int oldDirfd, const char *oldPath, int newDirfd, const char *newPath, int flags) {
	int error = validateUserString(oldPath);
	if (error) return error;
	error = validateUserString(newPath);
	if (error) return error;

	struct Process *proc = getCurrentThread()->process;
	struct Inode *oldDir = getCwd(oldDirfd, proc);
	struct Inode *newDir = getCwd(newDirfd, proc);

	int oldFileIndex;
	oldDir = getBaseDirFromPath(oldDir, &oldFileIndex, oldPath);
	int newFileIndex;
	newDir = getBaseDirFromPath(newDir, &newFileIndex, newPath);

	char oldNameBuf[NAME_MAX + 1];
	int len = strlen(&oldPath[oldFileIndex]);
	if (len >= NAME_MAX) {
		return -E2BIG;
	}
	memcpy(oldNameBuf, &oldPath[oldFileIndex], len + 1);

	return fsRename(newDir, &newPath[newFileIndex], oldDir, oldNameBuf, flags);
}