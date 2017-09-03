#include <fs/fs.h>
#include <fs/direntry.h>
#include <fs/devfile.h>
#include <errno.h>
#include <mm/memset.h>
#include <mm/heap.h>
#include <mm/paging.h>
#include <print.h>

int fsLink(struct inode *dir, struct inode *inode, const char *name) {
	acquireSpinlock(&inode->lock);
	if (inode->nrofLinks && (inode->type & ITYPE_MASK) == ITYPE_DIR) {
		releaseSpinlock(&inode->lock);
		return -EINVAL; //hardlinks are not allowed for dirs
	}

	struct dirEntry entry;
	size_t nameLen = strlen(name);
	if (nameLen <= 31) {
		memcpy(entry.inlineName, name, nameLen);
		entry.inlineName[nameLen] = 0;
	} else {
		entry.name = kmalloc(nameLen + 1);
		if (!entry.name) {
			return -ENOMEM;
		}
		memcpy(entry.name, name, nameLen);
		entry.name[nameLen] = 0;
	}

	acquireSpinlock(&dir->lock);

	entry.nameLen = nameLen;
	entry.inode = inode;
	
	inode->nrofLinks += 1;
	struct dirEntry *pEntry = &entry;
	dirCacheAdd(&pEntry, dir);

	releaseSpinlock(&dir->lock);
	releaseSpinlock(&inode->lock);
	
	return 0;
}

static void deleteInode(struct inode *inode) {
	if (inode->ramfs) {
		//delete cache
		if (inode->cachedData) {
			if ((inode->type & ITYPE_MASK) == ITYPE_DIR) {
				dirCacheDelete(inode);
			} else if (!(inode->ramfs & RAMFS_INITRD)) {
				//file
				deallocPages(inode->cachedData, inode->cachedDataSize);
			}
		}
		//delete inode
		kfree(inode);
	} else {
		//call fs drivers
	}
}

int fsUnlink(struct inode *dir, const char *name) {
	acquireSpinlock(&dir->lock);
	struct dirEntry *entry = dirCacheLookup(dir, name);

	if (!entry) {
		releaseSpinlock(&dir->lock);
		return -ENOENT;
	}

	struct inode *inode = entry->inode;
	if (!inode->ramfs) { //temp
		printk("Error deleting inode: unimplemented");
		releaseSpinlock(&dir->lock);
		return -ENOSYS;
	}
	
	acquireSpinlock(&inode->lock);

	inode->nrofLinks--;
	if (!inode->nrofLinks) {
		if ((inode->type & ITYPE_MASK) == ITYPE_CHAR) {
			struct devFileOps *ops = inode->ops;
			if (ops && ops->del) {
				ops->del(inode);
			}
			releaseSpinlock(&inode->lock);
		} else {
			releaseSpinlock(&inode->lock);
			deleteInode(inode);
		}
	} else {
		releaseSpinlock(&inode->lock);
	}
	//delete direntry
	dirCacheRemove(entry);

	releaseSpinlock(&dir->lock);
	return 0;
}