/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/mmu.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
    /* Set up the handler */
    page->operations = &file_ops;

    struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva)
{
    struct file_page *file_page UNUSED = &page->file;
    if (page == NULL)
        return false;
    struct load_aux *aux = page->uninit.aux;

    struct file *file = aux->file;
    off_t offset = aux->offset;
    size_t page_read_bytes = aux->page_read_bytes;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    file_seek(file, offset);

    if (file_read(file, kva, page_read_bytes) != (int)page_read_bytes)
    {
        return false;
    }

    memset(kva + page_read_bytes, 0, page_zero_bytes);

    return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page)
{
    struct file_page *file_page UNUSED = &page->file;
    struct load_aux *aux = page->uninit.aux;

    if (page == NULL)
        return false;

    // 사용 되었던 페이지(dirty page)인지 체크
    if (pml4_is_dirty(thread_current()->pml4, page->va))
    {
        file_write_at(aux->file, page->va, aux->page_read_bytes, aux->offset);
        pml4_set_dirty(thread_current()->pml4, page->va, 0);
    }

    pml4_clear_page(thread_current()->pml4, page->va);
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page)
{
    struct load_aux *aux = page->uninit.aux;
    struct thread *curr = thread_current();

    if (pml4_is_dirty(curr->pml4, page->va))
    {
        file_write_at(aux->file, page->va, aux->page_read_bytes, aux->offset);
        pml4_set_dirty(curr->pml4, page->va, 0);
    }

    if (page->frame)
    {
        list_remove(&page->frame->frame_elem);
        page->frame->page = NULL;
        free(page->frame);
        page->frame = NULL;
    }
    pml4_clear_page(curr->pml4, page->va);
}

/* Do the mmap */
void *
do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset)
{
    void *upage = addr;
    struct file *file_for_map = file_reopen(file);
    size_t read_bytes = file_length(file_for_map) < length ? file_length(file_for_map) : length;
    size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;

    ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT(pg_ofs(upage) == 0);
    ASSERT(offset % PGSIZE == 0);
    while (read_bytes > 0 || zero_bytes > 0)
    {
        /* Do calculate how to fill this page.
         * We will read PAGE_READ_BYTES bytes from FILE
         * and zero the final PAGE_ZERO_BYTES bytes. */
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        /* TODO: Set up aux to pass information to the lazy_load_segment. */
        struct load_aux *aux = calloc(1, sizeof(struct load_aux));
        aux->file = file_for_map;
        aux->offset = offset;
        aux->page_read_bytes = page_read_bytes;
        aux->page_zero_bytes = page_zero_bytes;
        aux->length = length;

        if (!vm_alloc_page_with_initializer(VM_FILE, upage,
                                            writable, lazy_load_segment, aux))
            return false;

        /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
        offset += page_read_bytes;
    }
    return addr;
}

/* Do the munmap */
void do_munmap(void *addr)
{
    struct thread *curr = thread_current();
    struct page *page = spt_find_page(&curr->spt, addr);
    struct load_aux *aux = page->uninit.aux;
    int map_pg_cnt = ((aux->length) % PGSIZE == 0) ? (aux->length / PGSIZE) : (aux->length / PGSIZE + 1);

    while (map_pg_cnt != 0)
    {
        if (page)
        {
            destroy(page);
        }
        addr += PGSIZE;
        page = spt_find_page(&curr->spt, addr);
        map_pg_cnt--;
    }
}
