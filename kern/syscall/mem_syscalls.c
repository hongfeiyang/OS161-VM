
#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <kern/limits.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <lib.h>
#include <uio.h>
#include <proc.h>
#include <current.h>
#include <synch.h>
#include <copyinout.h>
#include <vfs.h>
#include <vnode.h>
#include <openfile.h>
#include <filetable.h>
#include <syscall.h>
#include <addrspace.h>
/*
 * sbrk()
 * If successful, sbrk() returns the previous break value.
 * If unsuccessful, sbrk() returns -1 and sets errno to one of the following values:
 */

int
sys_sbrk(ssize_t amount, vaddr_t *retval) {

#if OPT_SBRK
    struct addrspace *as;
    vaddr_t old_heap_end, new_heap_end;
    struct region *heap = NULL;
    struct region *above_heap = NULL; // region that is above heap. can be stack or can be a memory mapped file

    as = curproc->p_addrspace;
    if (as == NULL) {
        return ENOMEM;
    }

    heap = find_region_by_vbase(as->all_regions, as->heap_start);
    KASSERT(heap != NULL);
    above_heap = heap->next;
    KASSERT(above_heap != NULL);

    // sbrk(0) should return end of heap
    if (amount == 0) {
        *retval = heap->vtop;
        return 0;
    }

    // Calculate the new heap end, based on weather we are growing or shrinking, round up or down to the nearest page
    old_heap_end = heap->vtop;
    // make sure original is page aligned
    KASSERT(old_heap_end % PAGE_SIZE == 0);
    new_heap_end = old_heap_end + amount;
    KASSERT(new_heap_end != old_heap_end);
    if (amount > 0) {
        // round up
        new_heap_end = (new_heap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    } else {
        // round down
        new_heap_end = new_heap_end & ~(PAGE_SIZE - 1);
    }

    // make sure we did the right rounding
    KASSERT((new_heap_end - old_heap_end) / PAGE_SIZE * PAGE_SIZE == new_heap_end - old_heap_end);

    // make sure the new heap end does not go below the heap start
    if (new_heap_end < heap->vbase) {
        return ENOMEM;
    }

    // make sure heap end does not grow into the stack
    if (new_heap_end >= above_heap->vbase) {
        return ENOMEM;
    }

    heap->vtop = new_heap_end;
    heap->npages = (new_heap_end - heap->vbase) / PAGE_SIZE;
    // npages should be evenly divisible by PAGE_SIZE
    KASSERT(heap->npages * PAGE_SIZE == new_heap_end - heap->vbase);
    *retval = old_heap_end;

    return 0;
#else
    (void)amount;
    (void)retval;
    return ENOSYS;
#endif
}

int
sys_mmap(size_t length, int prot, int fd, off_t offset, vaddr_t *retval) {

#if OPT_MMAP

    if (length == 0) {
        return EINVAL;
    }

    // offset must be page aligned
    if (offset % PAGE_SIZE != 0) {
        return EINVAL;
    }

    // make sure we can open the file
    struct openfile *file;
    int result = filetable_get(curproc->p_filetable, fd, &file);
    if (result) {
        return result;
    }

    // make sure the file is open
    if (file == NULL) {
        return EBADF;
    }

    // map the file into the address space
    struct addrspace *as = curproc->p_addrspace;
    struct region *region = alloc_file_region(as, length, prot & PROT_READ, prot & PROT_WRITE, 0);
    if (region == NULL) {
        return ENOMEM;
    }
    region->fd = fd;
    region->offset = offset;

    *retval = region->vbase;
    return 0;

#else
    (void)length;
    (void)prot;
    (void)fd;
    (void)offset;
    (void)retval;
    return ENOSYS;
#endif
}

int
sys_munmap(vaddr_t addr, int *retval) {
#if OPT_MMAP

    struct addrspace *as = curproc->p_addrspace;
    struct region *region = find_region_by_vbase(as->all_regions, addr);
    if (region == NULL) {
        return EINVAL;
    }

    // make sure the region is a file region
    if (region->type != FILE_REGION) {
        return EINVAL;
    }

    // remove the region from the address space
    regions_remove_region(as->all_regions, region);

    // free the region
    region_destroy(region);

    *retval = 0;
    return 0;

#else
    (void)addr;
    (void)retval;
    return ENOSYS;
#endif
}
