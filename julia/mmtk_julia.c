#include "mmtk_julia.h"
#include "mmtk.h"
#include <stdbool.h>
#include <stddef.h>
#include "gc.h"

extern int64_t perm_scanned_bytes;
extern void run_finalizer(jl_task_t *ct, void *o, void *ff);
extern int gc_n_threads;
extern jl_ptls_t* gc_all_tls_states;
extern jl_value_t *cmpswap_names JL_GLOBALLY_ROOTED;
extern jl_array_t *jl_global_roots_table JL_GLOBALLY_ROOTED;
extern jl_typename_t *jl_array_typename JL_GLOBALLY_ROOTED;
extern long BI_METADATA_START_ALIGNED_DOWN;
extern long BI_METADATA_END_ALIGNED_UP;
extern void gc_premark(jl_ptls_t ptls2);
extern uint64_t finalizer_rngState[JL_RNG_SIZE];
extern const unsigned pool_sizes[];
extern void mmtk_store_obj_size_c(void* obj, size_t size);
extern void jl_gc_free_array(jl_array_t *a);
extern size_t mmtk_get_obj_size(void* obj);
extern void jl_rng_split(uint64_t to[JL_RNG_SIZE], uint64_t from[JL_RNG_SIZE]);

extern void* new_mutator_iterator(void);
extern jl_ptls_t get_next_mutator_tls(void*);
extern void* close_mutator_iterator(void*);

JL_DLLEXPORT void (jl_mmtk_harness_begin)(void)
{
    jl_ptls_t ptls = jl_current_task->ptls;
    mmtk_harness_begin(ptls);
}

JL_DLLEXPORT void (jl_mmtk_harness_end)(void)
{
    mmtk_harness_end();
}

STATIC_INLINE void* alloc_default_object(jl_ptls_t ptls, size_t size, int offset) {
    int64_t delta = (-offset -(int64_t)(ptls->cursor)) & 15; // aligned to 16
    uint64_t aligned_addr = (uint64_t)ptls->cursor + delta;

    if(__unlikely(aligned_addr+size > (uint64_t)ptls->limit)) {
        jl_ptls_t ptls2 = jl_current_task->ptls;
        ptls2->mmtk_mutator_ptr->allocators.immix[0].cursor = ptls2->cursor;
        void* res = mmtk_alloc(ptls2->mmtk_mutator_ptr, size, 16, offset, 0);
        ptls2->cursor = ptls2->mmtk_mutator_ptr->allocators.immix[0].cursor;
        ptls2->limit = ptls2->mmtk_mutator_ptr->allocators.immix[0].limit;
        return res;
    } else {
        ptls->cursor = (void*) (aligned_addr+size);
        return (void*) aligned_addr;
    }
}

JL_DLLEXPORT jl_value_t *jl_mmtk_gc_alloc_default(jl_ptls_t ptls, int pool_offset,
                                                    int osize, void *ty)
{
    // safepoint
    jl_gc_safepoint();

    jl_value_t *v;
    if ((uintptr_t)ty != jl_buff_tag) {
        // v needs to be 16 byte aligned, therefore v_tagged needs to be offset accordingly to consider the size of header
        jl_taggedvalue_t *v_tagged = (jl_taggedvalue_t *)mmtk_alloc(ptls->mmtk_mutator_ptr, osize, 16, sizeof(jl_taggedvalue_t), 0); // (jl_taggedvalue_t *) alloc_default_object(ptls, osize, sizeof(jl_taggedvalue_t));
        v = jl_valueof(v_tagged);
        mmtk_post_alloc(ptls->mmtk_mutator_ptr, v, osize, 0);
    } else {
        // allocating an extra word to store the size of buffer objects
        jl_taggedvalue_t *v_tagged = (jl_taggedvalue_t *)mmtk_alloc(ptls->mmtk_mutator_ptr, osize + sizeof(jl_taggedvalue_t), 16, 0, 0); // (jl_taggedvalue_t *) alloc_default_object(ptls, osize + sizeof(jl_taggedvalue_t), 0);
        jl_value_t* v_tagged_aligned = ((jl_value_t*)((char*)(v_tagged) + sizeof(jl_taggedvalue_t)));
        v = jl_valueof(v_tagged_aligned);
        mmtk_store_obj_size_c(v, osize + sizeof(jl_taggedvalue_t));
        mmtk_post_alloc(ptls->mmtk_mutator_ptr, v, osize + sizeof(jl_taggedvalue_t), 0);
    }
    
    ptls->gc_num.allocd += osize;
    ptls->gc_num.poolalloc++;

    return v;
}

JL_DLLEXPORT jl_value_t *jl_mmtk_gc_alloc_big(jl_ptls_t ptls, size_t sz)
{
    // safepoint
    jl_gc_safepoint();

    size_t offs = offsetof(bigval_t, header);
    assert(sz >= sizeof(jl_taggedvalue_t) && "sz must include tag");
    static_assert(offsetof(bigval_t, header) >= sizeof(void*), "Empty bigval header?");
    static_assert(sizeof(bigval_t) % JL_HEAP_ALIGNMENT == 0, "");
    size_t allocsz = LLT_ALIGN(sz + offs, JL_CACHE_BYTE_ALIGNMENT);
    if (allocsz < sz) { // overflow in adding offs, size was "negative"
        assert(0 && "Error when allocating big object");
        jl_throw(jl_memory_exception);
    }

    bigval_t *v = (bigval_t*)mmtk_alloc_large(ptls->mmtk_mutator_ptr, allocsz, JL_CACHE_BYTE_ALIGNMENT, 0, 2);

    if (v == NULL) {
        assert(0 && "Allocation failed");
        jl_throw(jl_memory_exception);
    }
    v->sz = allocsz;

    ptls->gc_num.allocd += allocsz;
    ptls->gc_num.bigalloc++;

    jl_value_t *result = jl_valueof(&v->header);
    mmtk_post_alloc(ptls->mmtk_mutator_ptr, result, allocsz, 2);

    return result;
}

static void mmtk_sweep_malloced_arrays(void) JL_NOTSAFEPOINT
{
    void* iter = new_mutator_iterator();
    jl_ptls_t ptls2 = get_next_mutator_tls(iter);
    while(ptls2 != NULL) {
        mallocarray_t *ma = ptls2->heap.mallocarrays;
        mallocarray_t **pma = &ptls2->heap.mallocarrays;
        while (ma != NULL) {
            mallocarray_t *nxt = ma->next;
            if (!mmtk_object_is_managed_by_mmtk(ma->a)) {
                pma = &ma->next;
                ma = nxt;
                continue;
            }
            if (mmtk_is_live_object(ma->a)) {
                pma = &ma->next;
            }
            else {
                *pma = nxt;
                assert(ma->a->flags.how == 2);
                jl_gc_free_array(ma->a);
                ma->next = ptls2->heap.mafreelist;
                ptls2->heap.mafreelist = ma;
            }
            ma = nxt;
        }
        ptls2 = get_next_mutator_tls(iter);
    }
    gc_sweep_sysimg();
    close_mutator_iterator(iter);
}

extern void mark_metadata_scanned(jl_value_t* obj);
extern int8_t check_metadata_scanned(jl_value_t* obj);

int8_t object_has_been_scanned(jl_value_t* obj)
{
    uintptr_t tag = (uintptr_t)jl_typeof(obj);
    jl_datatype_t *vt = (jl_datatype_t*)tag;

    if (jl_object_in_image((jl_value_t *)obj)) {
        jl_taggedvalue_t *o = jl_astaggedvalue(obj);
        return o->bits.gc == GC_MARKED;
    }

    if (vt == jl_symbol_type) {
        return 1;
    };

    return 0;
}

void mark_object_as_scanned(jl_value_t* obj) {
    if (jl_object_in_image((jl_value_t *)obj)) {
        jl_taggedvalue_t *o = jl_astaggedvalue(obj);
        o->bits.gc = GC_MARKED;
    }
}

void mmtk_wait_in_a_safepoint(void) {
    jl_ptls_t ptls = jl_current_task->ptls;
    jl_gc_safepoint_(ptls);
}

void mmtk_exit_from_safepoint(int8_t old_state) {
    jl_ptls_t ptls = jl_current_task->ptls;
    jl_gc_state_set(ptls, old_state, JL_GC_STATE_WAITING);
}

// all threads pass here and if there is another thread doing GC,
// it will block until GC is done
// that thread simply exits from block_for_gc without executing finalizers
// when executing finalizers do not let another thread do GC (set a variable such that while that variable is true, no GC can be done)
int8_t set_gc_initial_state(jl_ptls_t ptls) 
{
    int8_t old_state = jl_atomic_load_relaxed(&((jl_ptls_t)ptls)->gc_state);
    jl_atomic_store_release(&((jl_ptls_t)ptls)->gc_state, JL_GC_STATE_WAITING);
    if (!jl_safepoint_start_gc()) {
        jl_gc_state_set((jl_ptls_t)ptls, old_state, JL_GC_STATE_WAITING);
        return -1;
    }
    return old_state;
}

void set_gc_final_state(int8_t old_state) 
{
    jl_ptls_t ptls = jl_current_task->ptls;
    jl_safepoint_end_gc();
    jl_gc_state_set(ptls, old_state, JL_GC_STATE_WAITING);
}

void set_gc_old_state(int8_t old_state) 
{
    jl_ptls_t ptls = jl_current_task->ptls;
    jl_atomic_store_release(&ptls->gc_state, old_state);
}

void wait_for_the_world(void)
{
    gc_n_threads = jl_atomic_load_acquire(&jl_n_threads);
    gc_all_tls_states = jl_atomic_load_relaxed(&jl_all_tls_states);
    assert(gc_n_threads);
    if (gc_n_threads > 1)
        jl_wake_libuv();
    for (int i = 0; i < gc_n_threads; i++) {
        jl_ptls_t ptls2 = gc_all_tls_states[i];
        if (ptls2 == NULL)
            continue;
        // This acquire load pairs with the release stores
        // in the signal handler of safepoint so we are sure that
        // all the stores on those threads are visible.
        // We're currently also using atomic store release in mutator threads
        // (in jl_gc_state_set), but we may want to use signals to flush the
        // memory operations on those threads lazily instead.
        while (!jl_atomic_load_relaxed(&ptls2->gc_state) || !jl_atomic_load_acquire(&ptls2->gc_state))
            jl_cpu_pause(); // yield?
    }
}

size_t get_lo_size(jl_value_t* obj) 
{
    jl_taggedvalue_t *v = jl_astaggedvalue(obj);
    // bigval_header: but we cannot access the function here. So use container_of instead.
    bigval_t* hdr = container_of(v, bigval_t, header);
    return hdr->sz;
}

void set_jl_last_err(int e) 
{
    errno = e;
}

int get_jl_last_err(void) 
{
    gc_n_threads = jl_atomic_load_acquire(&jl_n_threads);
    gc_all_tls_states = jl_atomic_load_relaxed(&jl_all_tls_states);
    for (int t_i = 0; t_i < gc_n_threads; t_i++) {
        jl_ptls_t ptls = gc_all_tls_states[t_i];
        ptls->cursor = 0;
        ptls->limit = 0;
    }
    return errno;
}

void* get_obj_start_ref(jl_value_t* obj) 
{
    uintptr_t tag = (uintptr_t)jl_typeof(obj);
    jl_datatype_t *vt = (jl_datatype_t*)tag;
    void* obj_start_ref; 

    if ((uintptr_t)vt == jl_buff_tag) {
        obj_start_ref = (void*)((size_t)obj - 2*sizeof(jl_taggedvalue_t));
    } else {
        obj_start_ref = (void*)((size_t)obj - sizeof(jl_taggedvalue_t));
    }

    return obj_start_ref;
}

size_t get_so_size(jl_value_t* obj) 
{
    uintptr_t tag = (uintptr_t)jl_typeof(obj);
    jl_datatype_t *vt = (jl_datatype_t*)tag;

    if ((uintptr_t)vt == jl_buff_tag) {
        return mmtk_get_obj_size(obj);
    } else if (vt->name == jl_array_typename) {
        jl_array_t* a = (jl_array_t*) obj;
        if (a->flags.how == 0) {
            int ndimwords = jl_array_ndimwords(jl_array_ndims(a));
            int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t);
            if (mmtk_object_is_managed_by_mmtk(a->data)) {
                size_t pre_data_bytes = ((size_t)a->data - a->offset*a->elsize) - (size_t)a;
                if (pre_data_bytes > 0) { // a->data is allocated after a
                    tsz = ((size_t)a->data - a->offset*a->elsize) - (size_t)a;
                    tsz += jl_array_nbytes(a);
                }
                if (tsz + sizeof(jl_taggedvalue_t) > 2032) { // if it's too large to be inlined (a->data and a are disjoint objects)
                    tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t); // simply keep the info before data
                }
            }
            if (tsz + sizeof(jl_taggedvalue_t) > 2032) {
                printf("size greater than minimum!\n");
                mmtk_runtime_panic();
            }
            int pool_id = jl_gc_szclass(tsz + sizeof(jl_taggedvalue_t));
            int osize = jl_gc_sizeclasses[pool_id];
            return osize;
        } else if (a->flags.how == 1) {
            int ndimwords = jl_array_ndimwords(jl_array_ndims(a));
            int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t);
            if (tsz + sizeof(jl_taggedvalue_t) > 2032) {
                printf("size greater than minimum!\n");
                mmtk_runtime_panic();
            }
            int pool_id = jl_gc_szclass(tsz + sizeof(jl_taggedvalue_t));
            int osize = jl_gc_sizeclasses[pool_id];

            return osize;
        } else if (a->flags.how == 2) {
            int ndimwords = jl_array_ndimwords(jl_array_ndims(a));
            int tsz = sizeof(jl_array_t) + ndimwords*sizeof(size_t);
            if (tsz + sizeof(jl_taggedvalue_t) > 2032) {
                printf("size greater than minimum!\n");
                mmtk_runtime_panic();
            }
            int pool_id = jl_gc_szclass(tsz + sizeof(jl_taggedvalue_t));
            int osize = jl_gc_sizeclasses[pool_id];

            return osize;
        } else if (a->flags.how == 3) {
            int ndimwords = jl_array_ndimwords(jl_array_ndims(a));
            int tsz = sizeof(jl_array_t) + ndimwords * sizeof(size_t) + sizeof(void*);
            if (tsz + sizeof(jl_taggedvalue_t) > 2032) {
                printf("size greater than minimum!\n");
                mmtk_runtime_panic();
            }
            int pool_id = jl_gc_szclass(tsz + sizeof(jl_taggedvalue_t));
            int osize = jl_gc_sizeclasses[pool_id];
            return osize;
        }
    } else if (vt == jl_simplevector_type) {
        size_t l = jl_svec_len(obj);
        if (l * sizeof(void*) + sizeof(jl_svec_t) + sizeof(jl_taggedvalue_t) > 2032) {
            printf("size greater than minimum!\n");
            mmtk_runtime_panic();
        }
        int pool_id = jl_gc_szclass(l * sizeof(void*) + sizeof(jl_svec_t) + sizeof(jl_taggedvalue_t));
        int osize = jl_gc_sizeclasses[pool_id];
        return osize;
    } else if (vt == jl_module_type) {
        size_t dtsz = sizeof(jl_module_t);
        if (dtsz + sizeof(jl_taggedvalue_t) > 2032) {
            printf("size greater than minimum!\n");
            mmtk_runtime_panic();
        }
        int pool_id = jl_gc_szclass(dtsz + sizeof(jl_taggedvalue_t));
        int osize = jl_gc_sizeclasses[pool_id];
        return osize;
    } else if (vt == jl_task_type) {
        size_t dtsz = sizeof(jl_task_t);
        if (dtsz + sizeof(jl_taggedvalue_t) > 2032) {
            printf("size greater than minimum!\n");
            mmtk_runtime_panic();
        }
        int pool_id = jl_gc_szclass(dtsz + sizeof(jl_taggedvalue_t));
        int osize = jl_gc_sizeclasses[pool_id];
        return osize;
    } else if (vt == jl_string_type) {
        size_t dtsz = jl_string_len(obj) + sizeof(size_t) + 1;
        if (dtsz + sizeof(jl_taggedvalue_t) > 2032) {
            printf("size greater than minimum!\n");
            mmtk_runtime_panic();
        }
        int pool_id = jl_gc_szclass_align8(dtsz + sizeof(jl_taggedvalue_t));
        int osize = jl_gc_sizeclasses[pool_id];
        return osize;
    } else if (vt == jl_method_type) {
        size_t dtsz = sizeof(jl_method_t);
        if (dtsz + sizeof(jl_taggedvalue_t) > 2032) {
            printf("size greater than minimum!\n");
            mmtk_runtime_panic();
        }
        int pool_id = jl_gc_szclass(dtsz + sizeof(jl_taggedvalue_t));
        
        int osize = jl_gc_sizeclasses[pool_id];
        return osize;
    } else  {
        size_t dtsz = jl_datatype_size(vt);
        if (dtsz + sizeof(jl_taggedvalue_t) > 2032) {
            printf("size greater than minimum!\n");
            mmtk_runtime_panic();
        }
        int pool_id = jl_gc_szclass(dtsz + sizeof(jl_taggedvalue_t));
        int osize = jl_gc_sizeclasses[pool_id];
        return osize;
    }
    return 0;
}

void run_finalizer_function(jl_value_t *o, jl_value_t *ff, bool is_ptr)
{
    if (is_ptr) {
        run_finalizer(jl_current_task, (jl_value_t *)(((uintptr_t)o) | 1), (jl_value_t *)ff);
    } else {
        run_finalizer(jl_current_task, (jl_value_t *) o, (jl_value_t *)ff);
    }
}


static inline void mmtk_jl_run_finalizers_in_list(bool at_exit) {
    jl_task_t* ct = jl_current_task;
    uint8_t sticky = ct->sticky;
    mmtk_run_finalizers(at_exit);
     ct->sticky = sticky;
}

void mmtk_jl_run_pending_finalizers(void* ptls) {
    if (!((jl_ptls_t)ptls)->in_finalizer && !((jl_ptls_t)ptls)->finalizers_inhibited && ((jl_ptls_t)ptls)->locks.len == 0) {
        jl_task_t *ct = jl_current_task;
        ((jl_ptls_t)ptls)->in_finalizer = 1;
        uint64_t save_rngState[JL_RNG_SIZE];
        memcpy(&save_rngState[0], &ct->rngState[0], sizeof(save_rngState));
        jl_rng_split(ct->rngState, finalizer_rngState);
        jl_atomic_store_relaxed(&jl_gc_have_pending_finalizers, 0);
        mmtk_jl_run_finalizers_in_list(false);
        memcpy(&ct->rngState[0], &save_rngState[0], sizeof(save_rngState));
        ((jl_ptls_t)ptls)->in_finalizer = 0;
    }
}

void mmtk_jl_run_finalizers(jl_ptls_t ptls) {
    // Only disable finalizers on current thread
    // Doing this on all threads is racy (it's impossible to check
    // or wait for finalizers on other threads without dead lock).
    if (!((jl_ptls_t)ptls)->finalizers_inhibited && ((jl_ptls_t)ptls)->locks.len == 0) {
        jl_task_t *ct = jl_current_task;
        int8_t was_in_finalizer = ((jl_ptls_t)ptls)->in_finalizer;
        ((jl_ptls_t)ptls)->in_finalizer = 1;
        uint64_t save_rngState[JL_RNG_SIZE];
        memcpy(&save_rngState[0], &ct->rngState[0], sizeof(save_rngState));
        jl_rng_split(ct->rngState, finalizer_rngState);
        jl_atomic_store_relaxed(&jl_gc_have_pending_finalizers, 0);
        mmtk_jl_run_finalizers_in_list(false);
        memcpy(&ct->rngState[0], &save_rngState[0], sizeof(save_rngState));
        ((jl_ptls_t)ptls)->in_finalizer = was_in_finalizer;
    } else {
        jl_atomic_store_relaxed(&jl_gc_have_pending_finalizers, 1);
    }
}

void mmtk_jl_gc_run_all_finalizers(void) {
    mmtk_jl_run_finalizers_in_list(true);
}

// add the initial root set to mmtk roots
static void queue_roots(void)
{
    // modules
    mmtk_add_object_to_mmtk_roots(jl_main_module);

    // invisible builtin values
    if (jl_an_empty_vec_any != NULL)
        mmtk_add_object_to_mmtk_roots(jl_an_empty_vec_any);
    if (jl_module_init_order != NULL)
        mmtk_add_object_to_mmtk_roots(jl_module_init_order);
    for (size_t i = 0; i < jl_current_modules.size; i += 2) {
        if (jl_current_modules.table[i + 1] != HT_NOTFOUND) {
            mmtk_add_object_to_mmtk_roots(jl_current_modules.table[i]);
        }
    }
    mmtk_add_object_to_mmtk_roots(jl_anytuple_type_type);
    for (size_t i = 0; i < N_CALL_CACHE; i++) {
         jl_typemap_entry_t *v = jl_atomic_load_relaxed(&call_cache[i]);
         if (v != NULL)
             mmtk_add_object_to_mmtk_roots(v);
    }
    if (jl_all_methods != NULL)
        mmtk_add_object_to_mmtk_roots(jl_all_methods);

    if (_jl_debug_method_invalidation != NULL)
        mmtk_add_object_to_mmtk_roots(_jl_debug_method_invalidation);
    // if (jl_build_ids != NULL)
    //     add_object_to_mmtk_roots(jl_build_ids);

    // constants
    mmtk_add_object_to_mmtk_roots(jl_emptytuple_type);
    if (cmpswap_names != NULL)
        mmtk_add_object_to_mmtk_roots(cmpswap_names);
    mmtk_add_object_to_mmtk_roots(jl_global_roots_table);

}

// Handle the case where the stack is only partially copied.
static inline uintptr_t mmtk_gc_get_stack_addr(void *_addr, uintptr_t offset,
                                          uintptr_t lb, uintptr_t ub)
{
    uintptr_t addr = (uintptr_t)_addr;
    if (addr >= lb && addr < ub)
        return addr + offset;
    return addr;
}

static inline uintptr_t mmtk_gc_read_stack(void *_addr, uintptr_t offset,
                                      uintptr_t lb, uintptr_t ub)
{
    uintptr_t real_addr = mmtk_gc_get_stack_addr(_addr, offset, lb, ub);
    return *(uintptr_t*)real_addr;
}

void scan_gcstack(jl_task_t *ta, closure_pointer closure, ProcessEdgeFn process_edge)
{
    void *stkbuf = ta->stkbuf;
#ifdef COPY_STACKS
    if (stkbuf && ta->copy_stack && mmtk_object_is_managed_by_mmtk(ta->stkbuf)) {
        // printf("-copystack: %p\n", &ta->stkbuf);fflush(stdout);
        process_edge(closure, &ta->stkbuf);
    }
#endif
    jl_gcframe_t *s = ta->gcstack;
    size_t nroots;
    uintptr_t offset = 0;
    uintptr_t lb = 0;
    uintptr_t ub = (uintptr_t)-1;
#ifdef COPY_STACKS
    if (stkbuf && ta->copy_stack && ta->ptls == NULL) {
        assert(ta->tid >= 0);
        jl_ptls_t ptls2 = jl_all_tls_states[ta->tid];
        ub = (uintptr_t)ptls2->stackbase;
        lb = ub - ta->copy_stack;
        offset = (uintptr_t)stkbuf - lb;
    }
#endif
    if (s) { // gc_mark_stack()
        nroots = mmtk_gc_read_stack(&s->nroots, offset, lb, ub);
        assert(nroots <= UINT32_MAX);

        uint32_t nr = nroots >> 2;

        while (1) {
            jl_value_t ***rts = (jl_value_t***)(((void**)s) + 2);
            for (uint32_t i = 0; i < nr; i++) {
                if (nroots & 1) {
                    void **slot = (void**)mmtk_gc_read_stack(&rts[i], offset, lb, ub);
                    uintptr_t real_addr = mmtk_gc_get_stack_addr(slot, offset, lb, ub);
                    // printf("-nroots&1 i = %d/%d: %p\n", i, nr, real_addr);fflush(stdout);
                    process_edge(closure, (void*)real_addr);
                }
                else {
                    uintptr_t real_addr = mmtk_gc_get_stack_addr(&rts[i], offset, lb, ub);
                    // printf("-nroots i = %d/%d: %p\n", i, nr, real_addr);fflush(stdout);
                    process_edge(closure, (void*)real_addr);
                }
            }

            jl_gcframe_t *sprev = (jl_gcframe_t*)mmtk_gc_read_stack(&s->prev, offset, lb, ub);
            if (sprev == NULL)
                break;

            s = sprev;
            uintptr_t new_nroots = mmtk_gc_read_stack(&s->nroots, offset, lb, ub);
            assert(new_nroots <= UINT32_MAX);
            nroots = (uint32_t)new_nroots;
            nr = nroots >> 2;
            continue;
        }
    }
    if (ta->excstack) { // inlining label `excstack` from mark_loop
        // if it is not managed by MMTk, nothing needs to be done because the object does not need to be scanned
        if (mmtk_object_is_managed_by_mmtk(ta->excstack)) {
            // printf("-excstack: %p\n", &ta->excstack);fflush(stdout);
            process_edge(closure, &ta->excstack);
        }
        jl_excstack_t *excstack = ta->excstack;
        size_t itr = ta->excstack->top;
        size_t bt_index = 0;
        size_t jlval_index = 0;
        while (itr > 0) {
            size_t bt_size = jl_excstack_bt_size(excstack, itr);
            jl_bt_element_t *bt_data = jl_excstack_bt_data(excstack, itr);
            for (; bt_index < bt_size; bt_index += jl_bt_entry_size(bt_data + bt_index)) {
                jl_bt_element_t *bt_entry = bt_data + bt_index;
                if (jl_bt_is_native(bt_entry))
                    continue;
                // Found an extended backtrace entry: iterate over any
                // GC-managed values inside.
                size_t njlvals = jl_bt_num_jlvals(bt_entry);
                while (jlval_index < njlvals) {
                    jl_value_t** new_obj_edge = &bt_entry[2 + jlval_index].jlvalue;
                    jlval_index += 1;
                    process_edge(closure, new_obj_edge);
                }
                jlval_index = 0;
            }

            jl_bt_element_t *stack_raw = (jl_bt_element_t *)(excstack+1);
            jl_value_t** stack_obj_edge = &stack_raw[itr-1].jlvalue;

            itr = jl_excstack_next(excstack, itr);
            bt_index = 0;
            jlval_index = 0;
            process_edge(closure, stack_obj_edge);
        }
    }
}

void root_scan_task(jl_ptls_t ptls, jl_task_t* task)
{
    // This is never accessed so just leave it uninitialized.
    closure_pointer c;

    // Scan the stack
    scan_gcstack(task, c, mmtk_process_root_edges);
    // And add the task itself
    mmtk_add_object_to_mmtk_roots(task);
}

static void jl_gc_queue_bt_buf_mmtk(jl_ptls_t ptls2)
{
    jl_bt_element_t *bt_data = ptls2->bt_data;
    jl_value_t* bt_entry_value;
    size_t bt_size = ptls2->bt_size;
    for (size_t i = 0; i < bt_size; i += jl_bt_entry_size(bt_data + i)) {
        jl_bt_element_t *bt_entry = bt_data + i;
        if (jl_bt_is_native(bt_entry))
            continue;
        size_t njlvals = jl_bt_num_jlvals(bt_entry);
        for (size_t j = 0; j < njlvals; j++) {
            bt_entry_value = jl_bt_entry_jlvalue(bt_entry, j);
            mmtk_add_object_to_mmtk_roots(bt_entry_value);
        }
    }
}

static void jl_gc_queue_thread_local_mmtk(jl_ptls_t ptls2)
{
    jl_task_t * task;
    task = ptls2->root_task;
    if (task != NULL) {
        root_scan_task(ptls2, task);
    }

    task = jl_atomic_load_relaxed(&ptls2->current_task);
    if (task != NULL) {
        root_scan_task(ptls2, task);
    }

    task = ptls2->next_task;
    if (task != NULL) {
        root_scan_task(ptls2, task);
    }

    task = ptls2->previous_task;
    if (task != NULL) {
        root_scan_task(ptls2, task);
    }

    if (ptls2->previous_exception) {
        mmtk_add_object_to_mmtk_roots(ptls2->previous_exception);
    }
}

static void jl_gc_queue_remset_mmtk(jl_ptls_t ptls2)
{
    size_t len = ptls2->heap.last_remset->len;
    void **items = ptls2->heap.last_remset->items;
    for (size_t i = 0; i < len; i++) {
        mmtk_add_object_to_mmtk_roots(items[i]);
    }
}

void calculate_roots(jl_ptls_t ptls)
{
    for (int t_i = 0; t_i < gc_n_threads; t_i++)
        gc_premark(gc_all_tls_states[t_i]);

    for (int t_i = 0; t_i < gc_n_threads; t_i++) {
        jl_ptls_t ptls2 = gc_all_tls_states[t_i];
        // 2.1. mark every thread local root
        jl_gc_queue_thread_local_mmtk(ptls2);
        // 2.2. mark any managed objects in the backtrace buffer
        jl_gc_queue_bt_buf_mmtk(ptls2);
        // 2.3. mark any managed objects in the backtrace buffer
        // TODO: treat these as roots for gc_heap_snapshot_record
        jl_gc_queue_remset_mmtk(ptls2);
    }

    queue_roots();
}

JL_DLLEXPORT void scan_julia_exc_obj(jl_task_t* obj, closure_pointer closure, ProcessEdgeFn process_edge) {
    jl_task_t *ta = (jl_task_t*)obj;

    if (ta->excstack) { // inlining label `excstack` from mark_loop
        // if it is not managed by MMTk, nothing needs to be done because the object does not need to be scanned
        if (mmtk_object_is_managed_by_mmtk(ta->excstack)) {
            process_edge(closure, &ta->excstack);
        }
        jl_excstack_t *excstack = ta->excstack;
        size_t itr = ta->excstack->top;
        size_t bt_index = 0;
        size_t jlval_index = 0;
        while (itr > 0) {
            size_t bt_size = jl_excstack_bt_size(excstack, itr);
            jl_bt_element_t *bt_data = jl_excstack_bt_data(excstack, itr);
            for (; bt_index < bt_size; bt_index += jl_bt_entry_size(bt_data + bt_index)) {
                jl_bt_element_t *bt_entry = bt_data + bt_index;
                if (jl_bt_is_native(bt_entry))
                    continue;
                // Found an extended backtrace entry: iterate over any
                // GC-managed values inside.
                size_t njlvals = jl_bt_num_jlvals(bt_entry);
                while (jlval_index < njlvals) {
                    jl_value_t** new_obj_edge = &bt_entry[2 + jlval_index].jlvalue;
                    jlval_index += 1;
                    process_edge(closure, new_obj_edge);
                }
                jlval_index = 0;
            }

            jl_bt_element_t *stack_raw = (jl_bt_element_t *)(excstack+1);
            jl_value_t** stack_obj_edge = &stack_raw[itr-1].jlvalue;

            itr = jl_excstack_next(excstack, itr);
            bt_index = 0;
            jlval_index = 0;
            process_edge(closure, stack_obj_edge);
        }
    }
}

#define jl_array_data_owner_addr(a) (((jl_value_t**)((char*)a + jl_array_data_owner_offset(jl_array_ndims(a)))))

JL_DLLEXPORT void* get_stackbase(int16_t tid) {
    assert(tid >= 0);
    jl_ptls_t ptls2 = jl_all_tls_states[tid];
    return ptls2->stackbase;
}

const bool PRINT_OBJ_TYPE = false;

/** 
 * Corresponds to the function mark_loop in the original Julia GC. It
 * dispatches MMTk work for scanning internal pointers for the object obj.
 * This function follows the flow defined in the `mark` goto label in mark_loop.
 * based on the type of obj it computes the internal pointers which are passed back to mmtk in 2 different ways:
 * (1) By adding an edge to vec_internals in order to create work packets. Note that this is a Rust vector limited 
 * by a `capacity`, once the vector is full, the work is dispatched through the function dispatch_work.
 * (2) By creating the work directly through the functions `trace_slot_with_offset`, `trace_obj` and `scan_obj`. The functions 
 * respectively: trace a buffer that contains an offset which needs to be added to the object after it is loaded; trace an object 
 * directly (not an edge), specifying whether to scan the object or not; and only scan the object 
 * (necessary for boot image / non-MMTk objects)
**/
JL_DLLEXPORT void scan_julia_obj(jl_value_t* obj, closure_pointer closure, ProcessEdgeFn process_edge, ProcessOffsetEdgeFn process_offset_edge) 
{
    uintptr_t tag = (uintptr_t)jl_typeof(obj);
    jl_datatype_t *vt = (jl_datatype_t*)tag; // type of obj

    // if it is a symbol type it does not contain internal pointers 
    // but we need to dispatch the work to appropriately drop the rust vector
    if (vt == jl_symbol_type || (uintptr_t)vt == jl_buff_tag) {
        return;
    };

    if (vt == jl_simplevector_type) { // scanning a jl_simplevector_type object (inlining label `objarray_loaded` from mark_loop)
        if (PRINT_OBJ_TYPE) { printf("scan_julia_obj %p: simple vector\n", obj); fflush(stdout); }
        size_t l = jl_svec_len(obj);
        jl_value_t **data = jl_svec_data(obj);
        jl_value_t **objary_begin = data;
        jl_value_t **objary_end = data + l;
        for (; objary_begin < objary_end; objary_begin += 1) {
            process_edge(closure, objary_begin);
        }
    } else if (vt->name == jl_array_typename) { // scanning a jl_array_typename object
        if (PRINT_OBJ_TYPE) { printf("scan_julia_obj %p: array\n", obj); fflush(stdout); }
        jl_array_t *a = (jl_array_t*)obj;
        jl_array_flags_t flags = a->flags;

        if (flags.how == 1) { // julia-allocated buffer that needs to be marked
            long offset = a->offset * a->elsize;
            process_offset_edge(closure, &a->data, offset);
        }
        if (flags.how == 2) { // malloc-allocated pointer this array object manages
            // should be processed below if it contains pointers
        } else if (flags.how == 3) { // has a pointer to the object that owns the data
            jl_value_t **owner_addr = jl_array_data_owner_addr(a);
            process_edge(closure, owner_addr);
            return;
        }
        if (a->data == NULL || jl_array_len(a) == 0) {
            return;
        }
        if (flags.ptrarray) { // inlining label `objarray_loaded` from mark_loop
            if ((jl_datatype_t*)jl_tparam0(vt) == jl_symbol_type) {
                return;
            }
            size_t l = jl_array_len(a);

            jl_value_t** objary_begin = (jl_value_t**)a->data;
            jl_value_t** objary_end = objary_begin + l;

            for (; objary_begin < objary_end; objary_begin++) {
                process_edge(closure, objary_begin);
            }
        } else if (flags.hasptr) { // inlining label `objarray_loaded` from mark_loop
            jl_datatype_t *et = (jl_datatype_t*)jl_tparam0(vt);
            const jl_datatype_layout_t *layout = et->layout;
            unsigned npointers = layout->npointers;
            unsigned elsize = a->elsize / sizeof(jl_value_t*);
            size_t l = jl_array_len(a);
            jl_value_t** objary_begin = (jl_value_t**)a->data;
            jl_value_t** objary_end = objary_begin + l * elsize;
            uint8_t *obj8_begin;
            uint8_t *obj8_end;

            if (npointers == 1) { // inlining label `objarray_loaded` from mark_loop
                objary_begin += layout->first_ptr;
                for (; objary_begin < objary_end; objary_begin+=elsize) {
                    process_edge(closure, objary_begin);
                }
            } else if (layout->fielddesc_type == 0) { // inlining label `array8_loaded` from mark_loop
                obj8_begin = (uint8_t*)jl_dt_layout_ptrs(layout);
                obj8_end = obj8_begin + npointers;
                size_t elsize = ((jl_array_t*)obj)->elsize / sizeof(jl_value_t*);
                jl_value_t **begin = objary_begin;
                jl_value_t **end = objary_end;
                uint8_t *elem_begin = obj8_begin;
                uint8_t *elem_end = obj8_end;

                for (; begin < end; begin += elsize) {
                    for (; elem_begin < elem_end; elem_begin++) {
                        jl_value_t **slot = &begin[*elem_begin];
                        process_edge(closure, slot);
                    }
                    elem_begin = obj8_begin;
                }
            } else if (layout->fielddesc_type == 1) {
                uint16_t *obj16_begin;
                uint16_t *obj16_end;
                size_t elsize = ((jl_array_t*)obj)->elsize / sizeof(jl_value_t*);
                jl_value_t **begin = objary_begin;
                jl_value_t **end = objary_end;
                obj16_begin = (uint16_t*)jl_dt_layout_ptrs(layout);
                obj16_end = obj16_begin + npointers;
                for (; begin < end; begin += elsize) {
                    for (; obj16_begin < obj16_end; obj16_begin++) {
                        jl_value_t **slot = &begin[*obj16_begin];
                        process_edge(closure, slot);
                    }
                    obj16_begin = (uint16_t*)jl_dt_layout_ptrs(layout);
                }
            } else {
                assert(0 && "unimplemented");
            }
        } else { 
            return;
        }
    } else if (vt == jl_module_type) { // inlining label `module_binding` from mark_loop
        if (PRINT_OBJ_TYPE) { printf("scan_julia_obj %p: module\n", obj); fflush(stdout); }
        jl_module_t *m = (jl_module_t*)obj;
        jl_svec_t *bindings = jl_atomic_load_relaxed(&m->bindings);
        jl_binding_t **table = (jl_binding_t**)jl_svec_data(bindings);
        size_t bsize = jl_svec_len(bindings);
        jl_binding_t **begin = table + 1;
        jl_binding_t **end =  table + bsize;
        for (; begin < end; begin++) {
            jl_binding_t *b = *begin;
            if (b == (jl_binding_t*)jl_nothing)
                continue;

            if (PRINT_OBJ_TYPE) { printf(" - scan table: %p\n", begin); fflush(stdout); }
            process_edge(closure, begin);
        }

        if (PRINT_OBJ_TYPE) { printf(" - scan parent: %p\n", &m->parent); fflush(stdout); }
        process_edge(closure, &m->parent);
        if (PRINT_OBJ_TYPE) { printf(" - scan bindingkeyset: %p\n", &m->bindingkeyset); fflush(stdout); }
        process_edge(closure, &m->bindingkeyset);
        if (PRINT_OBJ_TYPE) { printf(" - scan bindings: %p\n", &m->bindings); fflush(stdout); }
        process_edge(closure, &m->bindings);

        size_t nusings = m->usings.len;
        if (nusings) {
            jl_value_t **objary_begin = (jl_value_t**)m->usings.items;
            jl_value_t **objary_end = objary_begin + nusings;

            for (; objary_begin < objary_end; objary_begin += 1) {
                jl_value_t *pnew_obj = *objary_begin;
                if (PRINT_OBJ_TYPE) { printf(" - scan usings: %p\n", objary_begin); fflush(stdout); }
                process_edge(closure, pnew_obj);
            }
        }
    } else if (vt == jl_task_type) { // scanning a jl_task_type object
            jl_task_t *ta = (jl_task_t*)obj;

            // Scan gcstacks
            scan_gcstack(ta, closure, process_edge);

            const jl_datatype_layout_t *layout = jl_task_type->layout; // inlining label `obj8_loaded` from mark_loop 
            assert(layout->fielddesc_type == 0);
            assert(layout->nfields > 0);
            uint32_t npointers = layout->npointers;
            uint8_t *obj8_begin = (uint8_t*)jl_dt_layout_ptrs(layout);
            uint8_t *obj8_end = obj8_begin + npointers;
            (void)jl_assume(obj8_begin < obj8_end);
            for (; obj8_begin < obj8_end; obj8_begin++) {
                jl_value_t **slot = &((jl_value_t**)obj)[*obj8_begin];
                process_edge(closure, slot);
            }
    } else if (vt == jl_string_type) { // scanning a jl_string_type object
        if (PRINT_OBJ_TYPE) { printf("scan_julia_obj %p: string\n", obj); fflush(stdout); }
        return;
    } else {  // scanning a jl_datatype object
        if (PRINT_OBJ_TYPE) { printf("scan_julia_obj %p: datatype\n", obj); fflush(stdout); }
        if (vt == jl_weakref_type) {
            return;
        }
        const jl_datatype_layout_t *layout = vt->layout;
        uint32_t npointers = layout->npointers;
        if (npointers == 0) {
            return;
        } else {
            assert(layout->nfields > 0 && layout->fielddesc_type != 3 && "opaque types should have been handled specially");
            if (layout->fielddesc_type == 0) { // inlining label `obj8_loaded` from mark_loop 
                uint8_t *obj8_begin;
                uint8_t *obj8_end;

                obj8_begin = (uint8_t*)jl_dt_layout_ptrs(layout);
                obj8_end = obj8_begin + npointers;

                (void)jl_assume(obj8_begin < obj8_end);
                for (; obj8_begin < obj8_end; obj8_begin++) {
                    jl_value_t **slot = &((jl_value_t**)obj)[*obj8_begin];
                    process_edge(closure, slot);
                }
            }
            else if(layout->fielddesc_type == 1) { // inlining label `obj16_loaded` from mark_loop 
                // scan obj16
                uint16_t *obj16_begin;
                uint16_t *obj16_end;
                obj16_begin = (uint16_t*)jl_dt_layout_ptrs(layout);
                obj16_end = obj16_begin + npointers;
                for (; obj16_begin < obj16_end; obj16_begin++) {
                    jl_value_t **slot = &((jl_value_t**)obj)[*obj16_begin];
                    process_edge(closure, slot);
                }
            }
            else if (layout->fielddesc_type == 2) {
                uint32_t *obj32_begin = (uint32_t*)jl_dt_layout_ptrs(layout);
                uint32_t *obj32_end = obj32_begin + npointers;
                for (; obj32_begin < obj32_end; obj32_begin++) {
                    jl_value_t **slot = &((jl_value_t**)obj)[*obj32_begin];
                    process_edge(closure, slot);
                }
            }
            else {
                // simply dispatch the work at the end of the function
                assert(layout->fielddesc_type == 3);
                mmtk_runtime_panic();
            }
        }
    }

    return;
}

void update_gc_time(uint64_t inc) {
    gc_num.total_time += inc;
}

Julia_Upcalls mmtk_upcalls = (Julia_Upcalls) {
    .scan_julia_obj = scan_julia_obj,
    .scan_julia_exc_obj = scan_julia_exc_obj,
    .get_stackbase = get_stackbase,
    .calculate_roots = calculate_roots,
    .run_finalizer_function = run_finalizer_function,
    .get_jl_last_err = get_jl_last_err,
    .set_jl_last_err = set_jl_last_err,
    .get_lo_size = get_lo_size,
    .get_so_size = get_so_size,
    .get_obj_start_ref = get_obj_start_ref,
    .wait_for_the_world = wait_for_the_world,
    .set_gc_initial_state = set_gc_initial_state,
    .set_gc_final_state = set_gc_final_state,
    .set_gc_old_state = set_gc_old_state,
    .mmtk_jl_run_finalizers = mmtk_jl_run_finalizers,
    .jl_throw_out_of_memory_error = jl_throw_out_of_memory_error,
    .mark_object_as_scanned = mark_object_as_scanned,
    .object_has_been_scanned = object_has_been_scanned,
    .sweep_malloced_array = mmtk_sweep_malloced_arrays,
    .wait_in_a_safepoint = mmtk_wait_in_a_safepoint,
    .exit_from_safepoint = mmtk_exit_from_safepoint,
    .jl_hrtime = jl_hrtime,
    .update_gc_time = update_gc_time,
};
