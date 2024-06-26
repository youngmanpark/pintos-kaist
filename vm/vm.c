/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"
#include "threads/malloc.h"
#include "vm/inspect.h"
#include "include/lib/kernel/hash.h"
#include "include/threads/vaddr.h"
#include "threads/mmu.h"
#include "string.h"
#include "userprog/process.h"

struct list frame_table;
struct lock frame_table_lock;
void destructor(struct hash_elem *e, void *aux);
/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
    vm_anon_init();
    vm_file_init();
#ifdef EFILESYS /* For project 4 */
    pagecache_init();
#endif
    register_inspect_intr();
    /* DO NOT MODIFY UPPER LINES. */
    /* TODO: Your code goes here. */
    list_init(&frame_table);
    lock_init(&frame_table_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type(struct page *page) {
    int ty = VM_TYPE(page->operations->type);
    switch (ty) {
    case VM_UNINIT:
        return VM_TYPE(page->uninit.type);
    default:
        return ty;
    }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {

    ASSERT(VM_TYPE(type) != VM_UNINIT)

    struct supplemental_page_table *spt = &thread_current()->spt;
    struct page *page;

    if (spt_find_page(spt, upage) == NULL) {
        page = calloc(1, sizeof(struct page));

        if (!page)
            return false;
        if (VM_TYPE(type) == VM_ANON)
            uninit_new(page, upage, init, type, aux, anon_initializer);
        else if (VM_TYPE(type) == VM_FILE)
            uninit_new(page, upage, init, type, aux, file_backed_initializer);

        page->writable = writable;

        return spt_insert_page(spt, page);
    }
err:
    return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
    struct page *page = NULL;
    struct hash_elem *e;

    page = malloc(sizeof(struct page));

    page->va = pg_round_down(va);
    e = hash_find(&spt->hash_spt, &page->hash_elem);

    free(page); //?다시 생각해보기

    return e != NULL ? hash_entry(e, struct page, hash_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED, struct page *page UNUSED) {
    struct hash_elem *e;

    e = hash_insert(&spt->hash_spt, &page->hash_elem);
    return e != NULL ? false : true;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {
    vm_dealloc_page(page);
    return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
    struct frame *victim = NULL;
    struct thread *curr = thread_current();

    lock_acquire(&frame_table_lock);

    for (struct list_elem *e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e)) {
        victim = list_entry(e, struct frame, frame_elem);
        if (victim->page == NULL) // frame에 할당된 페이지가 없는 경우 (page가 destroy된 경우 )
        {
            lock_release(&frame_table_lock);
            return victim;
        }
        if (pml4_is_accessed(curr->pml4, victim->page->va))
            pml4_set_accessed(curr->pml4, victim->page->va, 0);
        else {
            lock_release(&frame_table_lock);
            return victim;
        }
    }
    lock_release(&frame_table_lock);
    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
    struct frame *victim = vm_get_victim();
    /* TODO: swap out the victim and return the evicted frame. */
    if (swap_out(victim->page)) {
        victim->page = NULL;
        memset(victim->kva, 0, PGSIZE);
        victim->ref_cnt = 1;
        return victim;
    }

    return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *vm_get_frame(void) {
    struct frame *frame = NULL;

    frame = calloc(1, sizeof(struct frame));
    frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);
    frame->page = NULL;
    frame->ref_cnt = 1;
    if (frame->kva == NULL) {
        frame = vm_evict_frame();
        frame->page = NULL;
        return frame;
    }

    lock_acquire(&frame_table_lock);
    list_push_back(&frame_table, &frame->frame_elem);
    lock_release(&frame_table_lock);

    ASSERT(frame != NULL);
    ASSERT(frame->page == NULL);
    return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {
    vm_alloc_page(VM_ANON | VM_MARKER_0, pg_round_down(addr), 1);
    vm_claim_page(addr);
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {
    if (page->frame->ref_cnt == 1)
        return false;
    if (page->frame->ref_cnt > 1) {
        struct frame *new_frame = vm_get_frame();
        if (!new_frame)
            return false;
        memcpy(new_frame->kva, page->frame->kva, PGSIZE);
        lock_acquire(&frame_table_lock);
        page->frame->ref_cnt--;
        page->frame = new_frame;
        page->frame->ref_cnt = 1;
        lock_release(&frame_table_lock);
        pml4_clear_page(thread_current()->pml4, page->va);
    }
    page->writable = true;
    pml4_set_page(thread_current()->pml4, page->va, page->frame->kva, true);

    return true;
}

/* Return true on success */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED, bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
    struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
    struct page *page = spt_find_page(spt, addr);
    void *rsp = f->rsp;

    if (addr == NULL || is_kernel_vaddr(addr))
        return false;

    if (not_present) {
        rsp = user ? f->rsp : thread_current()->rsp;
        if (USER_STACK > addr && addr >= USER_STACK - (1 << 20) && addr >= rsp - 8) {
            vm_stack_growth(pg_round_down(addr));
            return true;
        }
        if (!page)
            return false;
        if (write && !page->writable)
            return false;
        return vm_do_claim_page(page);
    } else
        return vm_handle_wp(page);

    return false;
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
    destroy(page);
    free(page);
}

/* Claim the page that allocate on VA. */
bool vm_claim_page(void *va UNUSED) {
    struct page *page = NULL;
    struct thread *current = thread_current();

    /* TODO: Fill this function */
    page = spt_find_page(&current->spt, va);
    if (!page)
        return false;
    return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool vm_do_claim_page(struct page *page) {
    struct frame *frame = vm_get_frame();
    struct thread *current = thread_current();
    uint64_t pml4 = current->pml4;

    /* Set links */
    frame->page = page;
    page->frame = frame;

    /* TODO: Insert page table entry to map page's VA to frame's PA. */
    if (!pml4_set_page(pml4, page->va, frame->kva, page->writable))
        return false;

    return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED) {

    hash_init(&spt->hash_spt, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED) {

    struct hash_iterator i;

    hash_first(&i, &src->hash_spt);
    while (hash_next(&i)) {
        struct page *parent_page = hash_entry(hash_cur(&i), struct page, hash_elem);
        struct page *child_page;

        enum vm_type page_type = page_get_type(parent_page);
        void *upage = parent_page->va;
        vm_initializer *init = parent_page->uninit.init;
        struct load_aux *aux = parent_page->uninit.aux;
        bool writable = parent_page->writable;

        if (parent_page->operations->type == VM_UNINIT) {
            vm_alloc_page_with_initializer(VM_ANON, upage, writable, init, aux);
            continue;
        }
        if (parent_page->operations->type == VM_FILE) {
            // struct load_aux *file_aux = malloc(sizeof(struct load_aux));
            // file_aux=aux;
            if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable, NULL, aux))
                return false;
            child_page = spt_find_page(dst, upage);

            child_page->operations = parent_page->operations;
            child_page->frame = parent_page->frame;
            child_page->writable = false;
            child_page->parent_writable = parent_page->writable;

            lock_acquire(&frame_table_lock);
            parent_page->frame->ref_cnt++;
            lock_release(&frame_table_lock);

            pml4_set_page(thread_current()->pml4, child_page->va, child_page->frame->kva, child_page->writable);
            continue;
        }

        if (!vm_alloc_page(page_type, upage, writable))
            return false;

        child_page = spt_find_page(dst, upage);

        child_page->operations = parent_page->operations;
        child_page->frame = parent_page->frame;
        child_page->writable = false;
        child_page->parent_writable = parent_page->writable;

        lock_acquire(&frame_table_lock);
        parent_page->frame->ref_cnt++;
        lock_release(&frame_table_lock);

        pml4_set_page(thread_current()->pml4, child_page->va, child_page->frame->kva, child_page->writable);
    }
    return true;
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
    hash_clear(&spt->hash_spt, destructor);
}

void destructor(struct hash_elem *e, void *aux) {
    struct page *page = hash_entry(e, struct page, hash_elem);
    vm_dealloc_page(page);
}
/* Returns a hash value for page p. */
unsigned page_hash(const struct hash_elem *p_, void *aux UNUSED) {
    const struct page *p = hash_entry(p_, struct page, hash_elem);
    return hash_bytes(&p->va, sizeof p->va);
}

/* Returns true if page a precedes page b. */
bool page_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
    const struct page *a = hash_entry(a_, struct page, hash_elem);
    const struct page *b = hash_entry(b_, struct page, hash_elem);

    return a->va < b->va;
}
void free_frame(struct frame *frame) {
    lock_acquire(&frame_table_lock);

    if (frame->ref_cnt > 1) {
        frame->ref_cnt--;
        lock_release(&frame_table_lock);
        return;
    }

    list_remove(&frame->frame_elem);
    palloc_free_page(frame->kva);
    free(frame);

    lock_release(&frame_table_lock);
}
