#include "hclib_sos-internal.h"

#include "hclib-locality-graph.h"

extern "C" {
#include "shmemx.h"
}

#include <map>
#include <vector>
#include <iostream>
#include <sstream>

// #define TRACE
// #define PROFILE
// #define DETAILED_PROFILING
// #define TRACING

#define SOS_HANG_WORKAROUND

static unsigned domain_ctx_id = 0;
static shmemx_domain_t *domains = NULL;
static shmemx_ctx_t *contexts = NULL;

#ifdef PROFILE
static bool disable_profiling = false;

#define START_PROFILE const unsigned long long __start_time = hclib_current_time_ns();

#if defined(TRACING)
static FILE *trace_fp = NULL;
#define END_PROFILE(funcname) { \
    if (!disable_profiling) { \
        fprintf(trace_fp, "TRACE %d : %s : %llu : %llu\n", ::shmem_my_pe(), \
                FUNC_NAMES[funcname##_lbl], __start_time, \
                hclib_current_time_ns()); \
    } \
}
#elif defined(DETAILED_PROFILING)
#define END_PROFILE(funcname) { \
    if (!disable_profiling) { \
        const unsigned long long __end_time = hclib_current_time_ns(); \
        func_counters[funcname##_lbl]++; \
        func_times[funcname##_lbl] += (__end_time - __start_time); \
        printf("%s: %llu ns\n", FUNC_NAMES[funcname##_lbl], \
                (__end_time - __start_time)); \
    } \
}
#else
#define END_PROFILE(funcname) { \
    if (!disable_profiling) { \
        const unsigned long long __end_time = hclib_current_time_ns(); \
        func_counters[funcname##_lbl]++; \
        func_times[funcname##_lbl] += (__end_time - __start_time); \
    } \
}
#endif

enum FUNC_LABELS {
    shmem_malloc_lbl = 0,
    shmem_free_lbl,
    shmem_barrier_all_lbl,
    shmem_fence_lbl,
    shmem_quiet_lbl,
    shmem_put64_lbl,
    shmem_broadcast64_lbl,
    shmem_set_lock_lbl,
    shmem_clear_lock_lbl,
    shmem_int_get_lbl,
    shmem_getmem_lbl,
    shmem_putmem_lbl,
    shmem_int_put_lbl,
    shmem_char_put_nbi_lbl,
    shmem_char_put_signal_nbi_lbl,
    shmem_int_add_lbl,
    shmem_longlong_fadd_lbl,
    shmem_int_fadd_lbl,
    shmem_int_sum_to_all_lbl,
    shmem_longlong_sum_to_all_lbl,
    shmem_longlong_max_to_all_lbl,
    shmem_longlong_p_lbl,
    shmem_longlong_put_lbl,
    shmem_int_finc_lbl,
    shmem_int_fetch_lbl,
    shmem_collect32_lbl,
    shmem_fcollect64_lbl,
    shmem_async_when_polling_lbl,
    enqueue_wait_set_lbl,
    N_FUNCS
};

const char *FUNC_NAMES[N_FUNCS] = {
    "shmem_malloc",
    "shmem_free",
    "shmem_barrier_all",
    "shmem_fence",
    "shmem_quiet",
    "shmem_put64",
    "shmem_broadcast64",
    "shmem_set_lock",
    "shmem_clear_lock",
    "shmem_int_get",
    "shmem_getmem",
    "shmem_putmem",
    "shmem_int_put",
    "shmem_char_put_nbi",
    "shmem_char_put_signal_nbi",
    "shmem_int_add",
    "shmem_longlong_fadd",
    "shmem_int_fadd",
    "shmem_int_sum_to_all",
    "shmem_longlong_sum_to_all",
    "shmem_longlong_max_to_all",
    "shmem_longlong_p",
    "shmem_longlong_put",
    "shmem_int_finc",
    "shmem_int_fetch",
    "shmem_collect32",
    "shmem_fcollect64",
    "shmem_async_when_polling",
    "enqueue_wait_set"};

unsigned long long func_counters[N_FUNCS];
unsigned long long func_times[N_FUNCS];
#else
#define START_PROFILE
#define END_PROFILE(funcname)
#endif

typedef struct _lock_context_t {
    // A future satisfied by the last attempt to lock this global lock.
    hclib_future_t *last_lock;
    /*
     * Store the promise that should be satisfied by whoever is currently in the
     * critical section once they complete it.
     */
    hclib_promise_t * volatile live;
} lock_context_t;

static int nic_locale_id;
static hclib::locale_t *nic = NULL;
static std::map<long *, lock_context_t *> lock_info;
static pthread_mutex_t lock_info_mutex = PTHREAD_MUTEX_INITIALIZER;

static hclib::wait_set_t * volatile waiting_on_head = NULL;

static int pe_to_locale_id(int pe) {
    HASSERT(pe >= 0);
    return -1 * pe - 1;
}

static int locale_id_to_pe(int locale_id) {
    HASSERT(locale_id < 0);
    return (locale_id + 1) * -1;
}

void hclib::disable_oshmem_profiling() {
#ifdef PROFILE
    disable_profiling = true;
#endif
}

void hclib::enable_oshmem_profiling() {
#ifdef PROFILE
    disable_profiling = false;
#endif
}

void hclib::reset_oshmem_profiling_data() {
#ifdef PROFILE
    memset(func_counters, 0x00, sizeof(func_counters));
    memset(func_times, 0x00, sizeof(func_times));
#endif
}

void hclib::print_oshmem_profiling_data() {
#ifdef PROFILE
    int i;
    printf("PE %d OPENSHMEM PROFILE INFO:\n", ::shmem_my_pe());
    for (i = 0; i < N_FUNCS; i++) {
        if (func_counters[i] > 0) {
            printf("  %s: %llu calls, %llu ms\n", FUNC_NAMES[i],
                    func_counters[i], func_times[i] / 1000000);
        }
    }
#endif
}

HCLIB_MODULE_INITIALIZATION_FUNC(sos_pre_initialize) {
    nic_locale_id = hclib_add_known_locale_type("Interconnect");
#ifdef PROFILE
    memset(func_counters, 0x00, sizeof(func_counters));
    memset(func_times, 0x00, sizeof(func_times));
#endif
}

static void init_sos_state(void *state, void *user_data, int tid) {
    assert(user_data == NULL);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    *domain = domains[tid];
    *ctx = contexts[tid];

#ifdef SOS_HANG_WORKAROUND
    const int npes = ::shmem_n_pes();
    const int pe = ::shmem_my_pe();
    int *tmp_buf = (int *)::shmem_malloc(sizeof(int));
    assert(tmp_buf);

    for (int i = 0; i < npes; i++) {
        if (i == pe) continue;

        ::shmemx_ctx_putmem(tmp_buf, tmp_buf, sizeof(int), i, *ctx);
    }
    ::shmem_barrier_all();

    ::shmem_free(tmp_buf);
#endif
}

static void release_sos_state(void *state, void *user_data) {
    assert(user_data == NULL);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    ::shmemx_ctx_destroy(*ctx);
    ::shmemx_domain_destroy(1, domain);
}

HCLIB_MODULE_INITIALIZATION_FUNC(sos_post_initialize) {
    int provided_thread_safety;
    const int desired_thread_safety = SHMEMX_THREAD_MULTIPLE;
    ::shmemx_init_thread(desired_thread_safety, &provided_thread_safety);
    assert(provided_thread_safety == desired_thread_safety);

#ifdef PROFILE
#ifdef TRACING
    const char *trace_dir = getenv("HIPER_TRACE_DIR");
    assert(trace_dir);
    char pe_filename[1024];
    sprintf(pe_filename, "%s/%d.trace", trace_dir, ::shmem_my_pe());
    trace_fp = fopen(pe_filename, "w");
    assert(trace_fp);
#endif
#endif
    domains = (shmemx_domain_t *)malloc(hclib_get_num_workers() * sizeof(*domains));
    assert(domains);
    contexts = (shmemx_ctx_t *)malloc(hclib_get_num_workers() * sizeof(*contexts));
    assert(contexts);

    int err = ::shmemx_domain_create(SHMEMX_THREAD_MULTIPLE,
            hclib_get_num_workers(), domains);
    assert(err == 0); 

    for (int i = 0; i < hclib_get_num_workers(); i++) {
        err = ::shmemx_ctx_create(domains[i], contexts + i);
        assert(err == 0);
    }

    domain_ctx_id = hclib_add_per_worker_module_state(
            sizeof(shmemx_domain_t) + sizeof(shmemx_ctx_t), init_sos_state,
            NULL);

    /*
     * This is only still needed because not all OpenSHMEM APIs are
     * contexts-based.
     */
    int n_nics;
    hclib::locale_t **nics = hclib::get_all_locales_of_type(nic_locale_id,
            &n_nics);
    HASSERT(n_nics == 1);
    HASSERT(nics);
    HASSERT(nic == NULL);
    nic = nics[0];
}

HCLIB_MODULE_INITIALIZATION_FUNC(sos_finalize) {
#ifdef PROFILE
#ifdef TRACING
    fclose(trace_fp);
#endif
#endif

    hclib_release_per_worker_module_state(domain_ctx_id, release_sos_state,
            NULL);
    ::shmem_finalize();
}

int hclib::shmem_my_pe() {
    return ::shmem_my_pe();
}

int hclib::shmem_n_pes() {
    return ::shmem_n_pes();
}

void *hclib::shmem_malloc(size_t size) {
    void **out_alloc = (void **)malloc(sizeof(void *));
    hclib::finish([out_alloc, size] {
        hclib::async_nb_at([size, out_alloc] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_malloc: Allocating " << size <<
                    " bytes" << std::endl;
#endif
            *out_alloc = ::shmem_malloc(size);
            END_PROFILE(shmem_malloc)
        }, nic);
    });

    void *allocated = *out_alloc;
    free(out_alloc);

    return allocated;
}

void hclib::shmem_free(void *ptr) {
    hclib::finish([ptr] {
        hclib::async_nb_at([ptr] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_free: ptr=" << ptr <<
                    std::endl;
#endif
            ::shmem_free(ptr);
            END_PROFILE(shmem_free)
        }, nic);
    });
}

void hclib::shmem_barrier_all() {
    hclib::finish([] {
        hclib::async_nb_at([] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_barrier_all" << std::endl;
#endif
            ::shmem_barrier_all();
            END_PROFILE(shmem_barrier_all)
        }, nic);
    });
}

void hclib::shmem_fence() {
    hclib::finish([] {
        hclib::async_nb_at([] {
            START_PROFILE
            ::shmem_fence();
            END_PROFILE(shmem_fence)
        }, nic);
    });
}

void hclib::shmem_quiet() {
    hclib::finish([] {
        hclib::async_nb_at([] {
            START_PROFILE
            ::shmem_quiet();
            END_PROFILE(shmem_quiet)
        }, nic);
    });
}

void hclib::shmem_put64(void *dest, const void *source, size_t nelems, int pe) {
    hclib::finish([dest, source, nelems, pe] {
        hclib::async_nb_at([dest, source, nelems, pe] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_put64: dest=" << dest <<
                    " source=" << source << " nelems=" << nelems << " pe=" <<
                    pe << std::endl;
#endif
            ::shmem_put64(dest, source, nelems, pe);
            END_PROFILE(shmem_put64)
        }, nic);
    });
}

void hclib::shmem_broadcast64(void *dest, const void *source, size_t nelems,
        int PE_root, int PE_start, int logPE_stride, int PE_size, long *pSync) {
    hclib::finish([dest, source, nelems, PE_root, PE_start, logPE_stride, PE_size, pSync] {
        hclib::async_nb_at([dest, source, nelems, PE_root, PE_start, logPE_stride, PE_size, pSync] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_broadcast64: dest=" <<
                    dest << " source=" << source << " nelems=" << nelems <<
                    " PE_root=" << PE_root << " PE_start=" << PE_start <<
                    " logPE_stride=" << logPE_stride << " PE_size=" <<
                    PE_size << std::endl;
#endif
            ::shmem_broadcast64(dest, source, nelems, PE_root, PE_start,
                logPE_stride, PE_size, pSync);
            END_PROFILE(shmem_broadcast64)
        }, nic);
    });
}

static void *shmem_set_lock_impl(void *arg) {
    START_PROFILE
#ifdef TRACE
    std::cerr << ::shmem_my_pe() << ": shmem_set_lock: lock=" << arg <<
        std::endl;
#endif
    ::shmem_set_lock((long *)arg);
    END_PROFILE(shmem_set_lock)
    return NULL;
}

static void shmem_clear_lock_impl(void *arg) {
    START_PROFILE
#ifdef TRACE
    std::cerr << ::shmem_my_pe() << ": shmem_clear_lock: lock=" << arg <<
        std::endl;
#endif
    ::shmem_clear_lock((long *)arg);
    END_PROFILE(shmem_clear_lock)
}

void hclib::shmem_set_lock(volatile long *lock) {
    int err = pthread_mutex_lock(&lock_info_mutex);
    HASSERT(err == 0);

    hclib_future_t *await = NULL;
    lock_context_t *ctx = NULL;

    hclib_promise_t *promise = hclib_promise_create();

    std::map<long *, lock_context_t *>::iterator found =
        lock_info.find((long *)lock);
    if (found != lock_info.end()) {
        ctx = found->second;
    } else {
        ctx = (lock_context_t *)calloc(1, sizeof(lock_context_t));

        lock_info.insert(std::pair<long *, lock_context_t *>((long *)lock, ctx));
    }

    /*
     * Launch an async at the NIC that performs the actual lock. This task's
     * execution is predicated on the last lock, which may be NULL.
     */
    await = hclib_async_future(shmem_set_lock_impl, (void *)lock,
            &ctx->last_lock, 1, nic);
    // Save ourselves as the last person to lock.
    ctx->last_lock = hclib_get_future_for_promise(promise);

    err = pthread_mutex_unlock(&lock_info_mutex);
    HASSERT(err == 0);

    hclib_future_wait(await);
    HASSERT(ctx->live == NULL);
    ctx->live = promise;
}

void hclib::shmem_clear_lock(long *lock) {
    int err = pthread_mutex_lock(&lock_info_mutex);
    HASSERT(err == 0);

    std::map<long *, lock_context_t *>::iterator found = lock_info.find(lock);
    // Doesn't make much sense to clear a lock that hasn't been set
    HASSERT(found != lock_info.end())

    err = pthread_mutex_unlock(&lock_info_mutex);
    HASSERT(err == 0);

    hclib::finish([&] {
        hclib_async_nb(shmem_clear_lock_impl, lock, nic);
    });

    HASSERT(found->second->live);
    hclib_promise_t *live = found->second->live;
    found->second->live = NULL;
    hclib_promise_put(live, NULL);
}

void hclib::shmem_int_get(int *dest, const int *source, size_t nelems, int pe) {
    hclib::finish([dest, source, nelems, pe] {
        hclib::async_nb_at([dest, source, nelems, pe] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_int_get: dest=" << dest <<
                    " source=" << source << " nelems=" << nelems << " pe=" <<
                    pe << std::endl;
#endif
            ::shmem_int_get(dest, source, nelems, pe);
            END_PROFILE(shmem_int_get)
        }, nic);
    });
}

void hclib::shmem_getmem(void *dest, const void *source, size_t nelems, int pe) {
    void *state = hclib_get_curr_worker_module_state(domain_ctx_id);
    assert(state);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    shmemx_ctx_getmem(dest, source, nelems, pe, *ctx);
}

void hclib::shmem_putmem(void *dest, const void *source, size_t nelems, int pe) {
    void *state = hclib_get_curr_worker_module_state(domain_ctx_id);
    assert(state);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    shmemx_ctx_putmem(dest, source, nelems, pe, *ctx);
}

void hclib::shmem_int_put(int *dest, const int *source, size_t nelems, int pe) {
    hclib::finish([dest, source, nelems, pe] {
        hclib::async_nb_at([dest, source, nelems, pe] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_int_put: dest=" << dest <<
                    " source=" << source << " nelems=" << nelems << " pe=" <<
                    pe << std::endl;
#endif
            ::shmem_int_put(dest, source, nelems, pe);
            END_PROFILE(shmem_int_put)
        }, nic);
    });
}

void hclib::shmem_char_put_nbi(char *dest, const char *source, size_t nelems,
        int pe) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            START_PROFILE
            ::shmem_char_put_nbi(dest, source, nelems, pe);
            END_PROFILE(shmem_char_put_nbi)
        }, nic);
    });
}

void hclib::shmem_char_put_signal_nbi(char *dest, const char *source,
        size_t nelems, char *signal_dest, const char *signal_source,
        size_t signal_nelems, int pe) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            START_PROFILE
            ::shmem_char_put_nbi(dest, source, nelems, pe);
            ::shmem_fence();
            ::shmem_char_put_nbi(signal_dest, signal_source, signal_nelems, pe);
            END_PROFILE(shmem_char_put_signal_nbi)
        }, nic);
    });
}

void hclib::shmem_int_add(int *dest, int value, int pe) {
    hclib::finish([dest, value, pe] {
        hclib::async_nb_at([dest, value, pe] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_int_add: dest=" << dest <<
                    " value=" << value << " pe=" << pe << std::endl;
#endif
            ::shmem_int_add(dest, value, pe);
            END_PROFILE(shmem_int_add)
        }, nic);
    });
}

long long hclib::shmem_longlong_fadd(long long *target, long long value,
        int pe) {
    long long *val_ptr = (long long *)malloc(sizeof(long long));
    hclib::finish([target, value, pe, val_ptr] {
        hclib::async_nb_at([target, value, pe, val_ptr] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_longlong_fadd: target=" <<
                target << " value=" << value << " pe=" << pe << std::endl;
#endif
            const long long val = ::shmem_longlong_fadd(target, value, pe);
            *val_ptr = val;
            END_PROFILE(shmem_longlong_fadd)
        }, nic);
    });

    const long long result = *val_ptr;

    free(val_ptr);
    return result;
}

int hclib::shmem_int_fadd(int *dest, int value, int pe) {
    int *heap_fetched = (int *)malloc(sizeof(int));
    hclib::finish([dest, value, pe, heap_fetched] {
        hclib::async_nb_at([dest, value, pe, heap_fetched] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_int_fadd: dest=" <<
                dest << " value=" << value << " pe=" << pe << std::endl;
#endif
            const int fetched = ::shmem_int_fadd(dest, value, pe);
            *heap_fetched = fetched;
            END_PROFILE(shmem_int_fadd)
        }, nic);
    });

    const int fetched = *heap_fetched;
    free(heap_fetched);

    return fetched;
}

int hclib::shmem_int_swap(int *dest, int value, int pe) {
    void *state = hclib_get_curr_worker_module_state(domain_ctx_id);
    assert(state);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    return shmemx_ctx_int_swap(dest, value, pe, *ctx);
}

int hclib::shmem_int_cswap(int *dest, int cond, int value, int pe) {
    void *state = hclib_get_curr_worker_module_state(domain_ctx_id);
    assert(state);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    return shmemx_ctx_int_cswap(dest, cond, value, pe, *ctx);
}

long hclib::shmem_long_finc(long *dest, int pe) {
    void *state = hclib_get_curr_worker_module_state(domain_ctx_id);
    assert(state);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    return shmemx_ctx_long_finc(dest, pe, *ctx);
}

int hclib::shmem_int_finc(int *dest, int pe) {
    void *state = hclib_get_curr_worker_module_state(domain_ctx_id);
    assert(state);
    shmemx_domain_t *domain = (shmemx_domain_t *)state;
    shmemx_ctx_t *ctx = (shmemx_ctx_t *)(domain + 1);

    return shmemx_ctx_int_finc(dest, pe, *ctx);
}

void hclib::shmem_int_sum_to_all(int *target, int *source, int nreduce,
                          int PE_start, int logPE_stride,
                          int PE_size, int *pWrk, long *pSync) {
    hclib::finish([target, source, nreduce, PE_start, logPE_stride, PE_size,
            pWrk, pSync] {
        hclib::async_nb_at([target, source, nreduce, PE_start, logPE_stride,
            PE_size, pWrk, pSync] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_int_sum_to_all: target=" <<
                target << " source=" << source << " nreduce=" << nreduce <<
                " PE_start=" << PE_start << " logPE_stride=" << logPE_stride <<
                " PE_size=" << PE_size << std::endl;
#endif
            ::shmem_int_sum_to_all(target, source, nreduce, PE_start,
                logPE_stride, PE_size, pWrk, pSync);
            END_PROFILE(shmem_int_sum_to_all)
        }, nic);
    });
}

void hclib::shmem_longlong_sum_to_all(long long *target, long long *source,
                               int nreduce, int PE_start,
                               int logPE_stride, int PE_size,
                               long long *pWrk, long *pSync) {
    hclib::finish([target, source, nreduce, PE_start, logPE_stride, PE_size,
            pWrk, pSync] {
        hclib::async_nb_at([target, source, nreduce, PE_start, logPE_stride,
            PE_size, pWrk, pSync] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_longlong_sum_to_all: "
                "target=" << target << " source=" << source << " nreduce=" <<
                nreduce << " PE_start=" << PE_start << " logPE_stride=" <<
                logPE_stride << " PE_size=" << PE_size << std::endl;
#endif
            ::shmem_longlong_sum_to_all(target, source, nreduce, PE_start,
                logPE_stride, PE_size, pWrk, pSync);
            END_PROFILE(shmem_longlong_sum_to_all)
        }, nic);
    });
}

void hclib::shmem_longlong_max_to_all(long long *target, long long *source,
                               int nreduce, int PE_start,
                               int logPE_stride, int PE_size,
                               long long *pWrk, long *pSync) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            START_PROFILE
            ::shmem_longlong_max_to_all(target, source, nreduce, PE_start,
                logPE_stride, PE_size, pWrk, pSync);
            END_PROFILE(shmem_longlong_max_to_all)
        }, nic);
    });
}

void hclib::shmem_longlong_p(long long *addr, long long value, int pe) {
    hclib::finish([addr, value, pe] {
        hclib::async_nb_at([addr, value, pe] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_longlong_p: addr=" <<
                addr << " value=" << value << " pe=" << pe << std::endl;
#endif
            ::shmem_longlong_p(addr, value, pe);
            END_PROFILE(shmem_longlong_p)
        }, nic);
    });
}

void hclib::shmem_longlong_put(long long *dest, const long long *src,
                        size_t nelems, int pe) {
    hclib::finish([dest, src, nelems, pe] {
        hclib::async_nb_at([dest, src, nelems, pe] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_longlong_put: dest=" <<
                dest << " src=" << src << "nelems=" << nelems << " pe=" << pe <<
                std::endl;
#endif
            ::shmem_longlong_put(dest, src, nelems, pe);
            END_PROFILE(shmem_longlong_put)
        }, nic);
    });
}

void hclib::shmem_collect32(void *dest, const void *source, size_t nelems,
        int PE_start, int logPE_stride, int PE_size, long *pSync) {
    hclib::finish([dest, source, nelems, PE_start, logPE_stride, PE_size, pSync] {
        hclib::async_nb_at([dest, source, nelems, PE_start, logPE_stride, PE_size, pSync] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_collect32" << std::endl;
#endif
            ::shmem_collect32(dest, source, nelems, PE_start, logPE_stride,
                PE_size, pSync);
            END_PROFILE(shmem_collect32)
        }, nic);
    });
}

void hclib::shmem_fcollect64(void *dest, const void *source, size_t nelems,
        int PE_start, int logPE_stride, int PE_size, long *pSync) {
    hclib::finish([&] {
        hclib::async_nb_at([&] {
            START_PROFILE
#ifdef TRACE
            std::cerr << ::shmem_my_pe() << ": shmem_fcollect64" << std::endl;
#endif
            ::shmem_fcollect64(dest, source, nelems, PE_start, logPE_stride,
                PE_size, pSync);
            END_PROFILE(shmem_fcollect64)
        }, nic);
    });

}

std::string hclib::shmem_name() {
    std::stringstream ss;
#ifdef SHMEM_VENDOR_STRING
    ss << SHMEM_VENDOR_STRING << " v" << SHMEM_MAJOR_VERSION << "." <<
        SHMEM_MINOR_VERSION << std::endl;
#else
    ss << "Unknown Impl" << std::endl;
#endif
    return ss.str();
}

static void poll_on_waits() {
    do {
        START_PROFILE
        int wait_set_list_non_empty = 1;

        hclib::wait_set_t *prev = NULL;
        hclib::wait_set_t *wait_set = waiting_on_head;

        assert(wait_set != NULL);

        while (wait_set) {
            hclib::wait_set_t *next = wait_set->next;

            bool any_complete = false;
            for (int i = 0; i < wait_set->ninfos && !any_complete; i++) {
                hclib::wait_info_t *wait_info = wait_set->infos + i;

                switch (wait_info->cmp) {
                    case SHMEM_CMP_EQ:
                        switch (wait_info->type) {
                            case hclib::integer:
                                if (*((volatile int *)wait_info->var) == wait_info->cmp_value.i) {
                                    any_complete = true;
                                }
                                break; // integer

                            default:
                                std::cerr << "Unsupported wait type " << wait_info->type << std::endl;
                                exit(1);
                        }
                        break; // SHMEM_CMP_EQ
              
                    case SHMEM_CMP_NE:
                        switch (wait_info->type) {
                            case hclib::integer:
                                if (*((volatile int *)wait_info->var) != wait_info->cmp_value.i) {
                                    any_complete = true;
                                }
                                break; // integer

                            default:
                                std::cerr << "Unsupported wait type " << wait_info->type << std::endl;
                                exit(1);
                        }
                        break; // SHMEM_CMP_NE

                    default:
                        std::cerr << "Unsupported cmp type " << wait_info->cmp << std::endl;
                        exit(1);
                }
            }

            /*
             * If a signal in the current wait_set was satisfied, trigger either
             * a downstream task or promise.
             */
            if (any_complete) {
                // Remove from singly linked list
                if (prev == NULL) {
                    /*
                     * If previous is NULL, we *may* be looking at the front of
                     * the list. It is also possible that another thread in the
                     * meantime came along and added an entry to the front of
                     * this singly-linked wait list, in which case we need to
                     * ensure we update its next rather than updating the list
                     * head. We do this by first trying to automatically update
                     * the list head to be the next of wait_set, and if we fail
                     * then we know we have a new head whose next points to
                     * wait_set and which should be updated.
                     */
                    hclib::wait_set_t *old_head = __sync_val_compare_and_swap(
                            &waiting_on_head, wait_set, wait_set->next);
                    if (old_head != wait_set) {
                        // Failed, someone else added a different head
                        assert(old_head->next == wait_set);
                        old_head->next = wait_set->next;
                        prev = old_head;
                    } else {
                        /*
                         * Success, new head is now wait_set->next. We want this
                         * polling task to exit if we just set the head to NULL.
                         * It is the responsibility of future async_when calls
                         * to restart it upon discovering a null head.
                         */
                        wait_set_list_non_empty = (wait_set->next != NULL);
                    }
                } else {
                    /*
                     * If previous is non-null, we just adjust its next link to
                     * jump over the current node.
                     */
                    assert(prev->next == wait_set);
                    prev->next = wait_set->next;
                }

                if (wait_set->task) {
                    HASSERT(wait_set->signal == NULL);
                    spawn(wait_set->task);
                } else {
                    HASSERT(wait_set->task == NULL);
                    hclib_promise_put(wait_set->signal, NULL);
                }
                free(wait_set->infos);
                free(wait_set);
            } else {
                prev = wait_set;
            }

            wait_set = next;
        }

        END_PROFILE(shmem_async_when_polling)

        if (wait_set_list_non_empty) {
            hclib::yield_at(nic);
        } else {
            // Empty list
            break;
        }
    } while (true);
}

void hclib::shmem_int_wait_until(volatile int *ivar, int cmp, int cmp_value) {
    hclib_promise_t *promise = construct_and_insert_wait_set(&ivar, cmp,
            &cmp_value, 1, integer, i, NULL);
    HASSERT(promise);

    hclib_future_wait(hclib_get_future_for_promise(promise));

    hclib_promise_free(promise);
}

void hclib::shmem_int_wait_until_any(volatile int **ivars, int cmp,
        int *cmp_values, int nwaits) {
    hclib_promise_t *promise = construct_and_insert_wait_set(ivars, cmp,
            cmp_values, nwaits, integer, i, NULL);
    HASSERT(promise);

    hclib_future_wait(hclib_get_future_for_promise(promise));

    hclib_promise_free(promise);
}

void hclib::enqueue_wait_set(hclib::wait_set_t *wait_set) {
    START_PROFILE
    wait_set->next = waiting_on_head;

    hclib::wait_set_t *old_head;
    while (1) {
        old_head = __sync_val_compare_and_swap(
                &waiting_on_head, wait_set->next, wait_set);
        if (old_head != wait_set->next) {
            wait_set->next = old_head;
        } else {
            break;
        }
    }

    if (old_head == NULL) {
        hclib::async_at([] {
            poll_on_waits();
        }, nic);
    }
    END_PROFILE(enqueue_wait_set)
}

HCLIB_REGISTER_MODULE("openshmem", sos_pre_initialize, sos_post_initialize, sos_finalize)
