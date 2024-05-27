/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "devices/disk.h"
#include "vm/vm.h"
#include "bitmap.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"

struct bitmap *swap_table;
struct lock swap_table_lock;
/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
    .swap_in = anon_swap_in,
    .swap_out = anon_swap_out,
    .destroy = anon_destroy,
    .type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void) {

    lock_init(&swap_table_lock);
    swap_disk = disk_get(1, 1);
    size_t swap_size = disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE);
    swap_table = bitmap_create(swap_size);
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva) {

    page->operations = &anon_ops;
    struct anon_page *anon_page = &page->anon;
}

/* Swap in the page by read contents from the swap disk. */
static bool anon_swap_in(struct page *page, void *kva) {

    size_t slot_no = page->slot_no;

    lock_acquire(&swap_table_lock);
    if (bitmap_test(swap_table, slot_no) == false) {
        lock_release(&swap_table_lock);
        return false;
    }

    for (int i = 0; i < 8; ++i)
        disk_read(swap_disk, (slot_no * 8) + i, kva + (DISK_SECTOR_SIZE * i));

    bitmap_set(swap_table, slot_no, false);
    lock_release(&swap_table_lock);

    return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool anon_swap_out(struct page *page) {

    lock_acquire(&swap_table_lock);
    size_t slot_no = bitmap_scan_and_flip(swap_table, 0, 1, false);

    if (slot_no == BITMAP_ERROR) 
        return false;
    
    for (int i = 0; i < 8; ++i) 
        disk_write(swap_disk, (slot_no * 8) + i, page->va + (DISK_SECTOR_SIZE * i));
    
    pml4_clear_page(thread_current()->pml4, page->va);

    page->slot_no = slot_no;
    lock_release(&swap_table_lock);
    return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page) {
    struct anon_page *anon_page = &page->anon;
    lock_acquire(&swap_table_lock);
    bitmap_set(swap_table, page->slot_no, true);
    lock_release(&swap_table_lock);
    if (page->frame && page->frame->page == page) {
        free_frame(page->frame);
    }
    pml4_clear_page(thread_current()->pml4, page->va);
}
