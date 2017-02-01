#include "map.h"
#include <mm/pagemap.h>

#include <global.h>
#include <mm/paging.h>
#include <mm/physpaging.h>
#include <print.h>

#define PAGE_BIT_WIDTH		9	//The number of bits a page level covers
#define PE_MASK			0xFFFFFFFFFFFFFFF8

#define PAGE_MASK		0x0000FFFFFFFFF000


static const uintptr_t pageLevelBase[NROF_PAGE_LEVELS] = {
	0xFFFFFF0000000000,	//PT
	0xFFFFFF7F80000000,	//PDT
	0xFFFFFF7FBFC00000,	//PDPT
	0xFFFFFF7FBFDFE000	//PML4T
};

static inline void invalidateTLB(uintptr_t page) {
	asm("invlpg [%0]": :"r"(page));
}

/*
Finds the entry in the page table at a specified level and sets it to a specified value.
*/
void mmSetPageEntry(uintptr_t addr, uint8_t level, pte_t entry) {
	addr &= 0x0000FFFFFFFFFFFF;
	addr = (addr >> (PAGE_BIT_WIDTH * (level + 1)) & PE_MASK) | pageLevelBase[level];
	pte_t *entryPtr = (pte_t*)addr;
	*entryPtr = entry;
	invalidateTLB(addr);
}

/*
Finds the entry in the page table at a specified level and sets it to a specfied value if there is no existing entry there.
*/
void mmSetPageEntryIfNotExists(uintptr_t addr, uint8_t level, pte_t entry) {
	addr &= 0x0000FFFFFFFFFFFF;
	addr = (addr >> (PAGE_BIT_WIDTH * (level + 1)) & PE_MASK) | pageLevelBase[level];
	pte_t *entryPtr = (pte_t*)addr;
	if (!(*entryPtr & MMU_FLAG_PRESENT)) {
		*entryPtr = entry;
		invalidateTLB(addr);
	}
}
/*
Finds the entry in the page table at a specified level and returns it.
*/
pte_t *mmGetEntry(uintptr_t addr, uint8_t level) {
	addr &= 0x0000FFFFFFFFFFFF;
	addr = (addr >> (PAGE_BIT_WIDTH * (level + 1)) & PE_MASK) | pageLevelBase[level];
	pte_t *entryPtr = (pte_t*)addr;
	return entryPtr;
}

/*
Finds a page entry and returns it
*/
physPage_t mmGetPageEntry(uintptr_t vaddr) {
	for (int8_t i = NROF_PAGE_LEVELS - 1; i >= 0; i--) {
		pte_t *entry = mmGetEntry(vaddr, i);
		if ( !(*entry & MMU_FLAG_PRESENT)) {
			//Page entry higher does not exist
			return NULL;
		} else if (i == 0 || *entry & MMU_FLAG_SIZE) {
			return (physPage_t)(*entry & PAGE_MASK);
		}
	}
	return NULL;
}

/*
Maps a page with physical address paddr to the virtual address vaddr.
*/
void mmMapPage(uintptr_t vaddr, physPage_t paddr, uint8_t flags) {
	for (int8_t i = NROF_PAGE_LEVELS - 1; i >= 1; i--) {
		pte_t *entry = mmGetEntry(vaddr, i);
		if ( !(*entry & MMU_FLAG_PRESENT)) {
			//Page entry higher does not exist
			physPage_t page = allocCleanPhysPage();
			*entry = page | MMU_FLAG_PRESENT | (flags << 1);
		}
	}
	pte_t entry = paddr | (flags << 1) | MMU_FLAG_PRESENT;
	mmSetPageEntry(vaddr, 0, entry);
	invalidateTLB(vaddr);
	return;
}
/*
Maps a large page with physical address paddr to the virtual address vaddr.
*/
void mmMapLargePage(uintptr_t vaddr, physPage_t paddr, uint8_t flags) {
	for (int8_t i = NROF_PAGE_LEVELS - 1; i >= 2; i--) {
		pte_t *entry = mmGetEntry(vaddr, i);
		if ( !(*entry & MMU_FLAG_PRESENT)) {
			//Page entry higher does not exist
			physPage_t page = allocCleanPhysPage();
			*entry = page | MMU_FLAG_PRESENT | (flags << 1);
		}
	}
	pte_t entry = paddr | ((flags << 1) + MMU_FLAG_PRESENT + MMU_FLAG_SIZE);
	mmSetPageEntry(vaddr, 1, entry);
	invalidateTLB(vaddr);
	return;
}

/*
Unmaps a page.
*/
void mmUnmapPage(uintptr_t vaddr) {
	for (int8_t i = NROF_PAGE_LEVELS - 1; i >= 0; i--) {
		pte_t *entry = mmGetEntry(vaddr, i);
		if ( !(*entry & MMU_FLAG_PRESENT)) {
			//Page entry higher does not exist
			invalidateTLB(vaddr);
			return;
		} else if (i == 0 || *entry & MMU_FLAG_SIZE) {
			*entry = 0;
			invalidateTLB(vaddr);
			return;
		}
	}
}