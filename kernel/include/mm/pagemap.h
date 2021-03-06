#ifndef INCLUDE_PAGEMAP_H
#define INCLUDE_PAGEMAP_H

#include <mm/paging.h>
#include <stdbool.h>

/*
Maps a page with physical address paddr to the virtual address vaddr.
*/
void mmMapPage(uintptr_t vaddr, physPage_t paddr, pageFlags_t flags);

/*
Maps a large page with physical address paddr to the virtual address vaddr.
*/
void mmMapLargePage(uintptr_t vaddr, physPage_t paddr, pageFlags_t flags);

/*
Unmaps a page.
*/
void mmUnmapPage(uintptr_t vaddr);

/*
Finds a page entry and returns it
*/
physPage_t mmGetPageEntry(uintptr_t vaddr);

/*
Reserves a physical page in memory
*/
void mmSetPageFlags(uintptr_t vaddr, pageFlags_t flags);

/*
Unmaps and deallocates all mapped userspace pages belonging to the current process
*/
void mmUnmapUserspace(void);

/*
Creates an empty address space for usermode, with only kernel pages mapped
*/
uintptr_t mmCreateAddressSpace(void);

/*
Enable/disable write access
*/
void mmSetWritable(void *addr, size_t size, bool write);

#endif
