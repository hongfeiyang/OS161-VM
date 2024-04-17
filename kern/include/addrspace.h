/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _ADDRSPACE_H_
#define _ADDRSPACE_H_

#define OPT_COW 1
#define OPT_SBRK 1
#define OPT_MMAP 1

/*
 * Address space structure and operations.
 */

#include <vm.h>
#include "opt-dumbvm.h"

struct vnode;

/*
 * Address space - data structure associated with the virtual memory
 * space of a process.
 *
 * You write this.
 */

#define L1_BITS 11
#define L2_BITS 9
#define OFFSET_BITS 12

#define L1_INDEX(x) ((x) >> (L2_BITS + OFFSET_BITS))
#define L2_INDEX(x) (((x) >> OFFSET_BITS) & ((1 << L2_BITS) - 1))

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define YANG_VM_STACKPAGES 18

#define UNNAMED_REGION 0
#define HEAP_REGION 1
#define STACK_REGION 2
#define FILE_REGION 3

struct region {
    vaddr_t vbase;
    size_t npages;
    vaddr_t vtop; // for convenience
    unsigned int readable : 1;
    unsigned int writeable : 1;
    unsigned int executable : 1;
    struct region *next;
    struct region *prev;
    int type;
#if OPT_MMAP
    int fd;       // file descriptor
    off_t offset; // offset where the mapping starts
#endif
};

// Page Table Entry
typedef struct page_table_entry {
    paddr_t frame;
    struct lock *lock;
#if OPT_COW
    unsigned int shared : 1; // shared means it will be copy on write
    int ref_count;           // how many references to this page, for shared page this could be more than 1
#endif
} PTE;

typedef struct l2_page_table {
    PTE *entries[1 << L2_BITS];
} L2Table;
// A second-level page table has 1 << 9 = 512 entries
// Each entry is 4 bytes, so 512 * 4 = 2KB

typedef struct page_table {
    L2Table *tables[1 << L1_BITS]; // Pointers to second-level page tables
    struct lock *lock;
} PageTable;
// A first level page table has 1 << 11 = 2048 entries
// Each entry is 4 bytes, so 2048 * 4 = 8KB

typedef struct regions {
    struct region *head;
    struct region *tail;
} Regions;

struct addrspace {
#if OPT_DUMBVM
    vaddr_t as_vbase1;
    paddr_t as_pbase1;
    size_t as_npages1;
    vaddr_t as_vbase2;
    paddr_t as_pbase2;
    size_t as_npages2;
    paddr_t as_stackpbase;
#else
    Regions *all_regions;
    bool force_readwrite;
    PageTable *page_table;
    vaddr_t heap_start;
    vaddr_t stack_start;
#endif
};

/*
 * Functions in addrspace.c:
 *
 *    as_create - create a new empty address space. You need to make
 *                sure this gets called in all the right places. You
 *                may find you want to change the argument list. May
 *                return NULL on out-of-memory error.
 *
 *    as_copy   - create a new address space that is an exact copy of
 *                an old one. Probably calls as_create to get a new
 *                empty address space and fill it in, but that's up to
 *                you.
 *
 *    as_activate - make curproc's address space the one currently
 *                "seen" by the processor.
 *
 *    as_deactivate - unload curproc's address space so it isn't
 *                currently "seen" by the processor. This is used to
 *                avoid potentially "seeing" it while it's being
 *                destroyed.
 *
 *    as_destroy - dispose of an address space. You may need to change
 *                the way this works if implementing user-level threads.
 *
 *    as_define_region - set up a region of memory within the address
 *                space.
 *
 *    as_prepare_load - this is called before actually loading from an
 *                executable into the address space.
 *
 *    as_complete_load - this is called when loading from an executable
 *                is complete.
 *
 *    as_define_stack - set up the stack region in the address space.
 *                (Normally called *after* as_complete_load().) Hands
 *                back the initial stack pointer for the new process.
 *
 * Note that when using dumbvm, addrspace.c is not used and these
 * functions are found in dumbvm.c.
 */

struct addrspace *as_create(void);
int as_copy(struct addrspace *src, struct addrspace **ret);
void as_activate(void);
void as_deactivate(void);
void as_destroy(struct addrspace *);

int as_define_region(struct addrspace *as,
                     vaddr_t vaddr, size_t sz,
                     int readable,
                     int writeable,
                     int executable);
int as_prepare_load(struct addrspace *as);
int as_complete_load(struct addrspace *as);
int as_define_stack(struct addrspace *as, vaddr_t *initstackptr);

/*
 * Functions in loadelf.c
 *    load_elf - load an ELF user program executable into the current
 *               address space. Returns the entry point (initial PC)
 *               in the space pointed to by ENTRYPOINT.
 */

int load_elf(struct vnode *v, vaddr_t *entrypoint);

#if OPT_COW
void pte_inc_ref(PTE *pte);
void pte_dec_ref(PTE *pte);
#endif

PTE *new_pte(void);
void pte_destroy(PTE *pte);
PTE *pte_copy(PTE *pte);
#if OPT_COW
PTE *pte_copy_on_write(PTE *pte);
#endif

PageTable *page_table_init(void);
void page_table_destroy(PageTable *page_table);
PageTable *page_table_copy(PageTable *old);
PTE *page_table_lookup(PageTable *pt, vaddr_t addr);
int page_table_add_entry(PageTable *page_table, vaddr_t vaddr, PTE *pte);
PTE *page_table_remove_entry(PageTable *page_table, vaddr_t vaddr);

Regions *regions_create(void);
Regions *regions_copy(Regions *old);
void regions_remove_region(Regions *regions, struct region *region);
void regions_destroy(Regions *regions);
void region_destroy(struct region *region);
void regions_insert(Regions *regions, struct region *region);
struct region *find_region(Regions *regions, vaddr_t vaddr);
struct region *find_region_by_vbase(Regions *regions, vaddr_t vbase);
#if OPT_MMAP
struct region *alloc_file_region(struct addrspace *as, size_t memsize, int readable, int writeable, int executable);
#endif

#endif /* _ADDRSPACE_H_ */
