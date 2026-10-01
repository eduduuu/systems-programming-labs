#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>
#include <sched.h>
#include <math.h>
#include <unistd.h>

// Conditionally include PAPI
#ifdef USE_PAPI
#include <papi.h>

#define NUM_EVENTS 7
const char *EVENT_NAMES[] = {
    "PAPI_TOT_INS",
    "perf::L1-DCACHE-LOADS",
    "perf::L1-DCACHE-LOAD-MISSES",
    "L2_RQSTS:REFERENCES",
    "L2_RQSTS:MISS",
    "perf::LLC-LOADS",
    "perf::LLC-LOAD-MISSES"
};

typedef struct FunctionEvents {
    const char *name;
    long long calls;
    long long events[NUM_EVENTS];
} FunctionEvents;

FunctionEvents *func_prof[3] = {NULL};
static __thread int EventSet = PAPI_NULL;

void profile_start() {
    if (EventSet != PAPI_NULL) {
        PAPI_reset(EventSet);
        PAPI_start(EventSet);
    }
}

void profile_stop(FunctionEvents *func_prof) {
    long long values[NUM_EVENTS] = {0};
    if (EventSet != PAPI_NULL) {
        int ret = PAPI_stop(EventSet, values);
        if (ret == PAPI_OK) {
            for (int i = 0; i < NUM_EVENTS; i++) {
                func_prof->events[i] += values[i];
            }
        }
    }
    func_prof->calls++;
}
#else
// Dummy macros if PAPI is disabled
#define profile_start() do {} while(0)
#define profile_stop(x) do {} while(0)
#endif

// --- CORE DATA STRUCTURES ---

typedef struct Array {
    int *values;
    size_t size;
    size_t capacity;
} Array;

typedef struct Micro_bench {
    Array *array;
    long long searches;
    long long pushes;
    long long pops;
    unsigned short seed[3];
    int duration;
} Micro_bench;

// --- ARRAY OPERATIONS ---

int search(Array *array, int value) {
    for (int i = 0; i < array->size; i++) {
        if (array->values[i] == value)
            return 1;
    }
    return 0;
}

void push(Array *array, int value) {
    if (array->size == array->capacity)
        return;

    // INTENTIONAL BUG: We insert the value but fail to increment the size counter.
    // This causes subsequent pushes to overwrite the exact same memory index, 
    // prevents the search function from traversing the new elements, always returning not foud
    // and makes the pop function never be executed.
    array->values[array->size] = value;
    // array->size++; 
}

void pop(Array *array) {
    if (array->size == 0)
        return;

    array->values[array->size - 1] = 0;
    array->size--;
}

// --- UTILITIES ---

static inline void rand_init(unsigned short *seed) {
    seed[0] = (unsigned short)rand();
    seed[1] = (unsigned short)rand();
    seed[2] = (unsigned short)rand();
}

static inline int rand_range(int n, unsigned short *seed) {
    return (int)(erand48(seed) * n);
}

// --- THREADING & EXECUTION ---

static volatile int stop;

static void* test_random(void *p_mb_) {
    // ISOLATION: Get the OS-level thread ID. 
    // This allows `perf` to profile *only* this thread, ignoring main process overhead.
    pid_t tid = gettid();
    printf("tid %d\n", tid);
    fflush(stdout);

#ifdef USE_PAPI
    PAPI_register_thread();
    int ret = PAPI_create_eventset(&EventSet);
    if (ret == PAPI_OK) {
        PAPI_set_multiplex(EventSet);
        for (int i = 0; i < NUM_EVENTS; i++) {
            PAPI_add_named_event(EventSet, EVENT_NAMES[i]);
        }
    }
#endif

    Micro_bench *p_mb = (Micro_bench*)p_mb_;
    int op, n;

    // SYNCHRONIZATION: Spin until the main thread flags start == 0.
    // This ensures we don't start the workload before Python attaches `perf`.
    while (stop == -1) {
        sched_yield();
    }

    while (stop == 0) {
        op = rand_range(99, p_mb->seed);
        if (op < 33) {
            n = rand_range(99, p_mb->seed);
            profile_start();
            search(p_mb->array, n);
#ifdef USE_PAPI
            profile_stop(func_prof[0]);
#endif
            p_mb->searches++;
        } else if (op < 66) {
            if (p_mb->array->size == p_mb->array->capacity) continue;
            n = rand_range(99, p_mb->seed);
            profile_start();
            push(p_mb->array, n);
#ifdef USE_PAPI
            profile_stop(func_prof[1]);
#endif
            p_mb->pushes++;
        } else {
            if (p_mb->array->size == 0) continue;
            profile_start();
            pop(p_mb->array);
#ifdef USE_PAPI
            profile_stop(func_prof[2]);
#endif
            p_mb->pops++;
        }
    }

#ifdef USE_PAPI
    if (EventSet != PAPI_NULL) {
        PAPI_cleanup_eventset(EventSet);
        PAPI_destroy_eventset(&EventSet);
        EventSet = PAPI_NULL;
    }
    PAPI_unregister_thread();
#endif

    return NULL;
}

void run_test(Micro_bench *p_mb) {
    stop = -1;

    struct timeval start, end;
    struct timespec timeout;

    timeout.tv_sec = p_mb->duration / 1000;
    timeout.tv_nsec = (p_mb->duration % 1000) * 1000000;

#ifdef USE_PAPI
    if (!PAPI_is_initialized()) {
        PAPI_library_init(PAPI_VER_CURRENT);
        PAPI_thread_init(pthread_self);
        PAPI_multiplex_init();
    }
    for (int i = 0; i < 3; i++) {
        func_prof[i] = (FunctionEvents*)calloc(1, sizeof(FunctionEvents));
    }
    func_prof[0]->name = "searches";
    func_prof[1]->name = "pushes";
    func_prof[2]->name = "pops";
#endif

    pthread_t thread;
    pthread_create(&thread, NULL, test_random, (void*)p_mb);
    pthread_setname_np(thread, "bench_thread");

    // BLOCKING: Wait for Python script to send a newline via stdin
    // before allowing the worker thread to begin its loop.
    getchar();
    stop = 0;

    gettimeofday(&start, NULL);
    nanosleep(&timeout, NULL);
    stop = 1;

    gettimeofday(&end, NULL);
    pthread_join(thread, NULL);

    p_mb->duration = (end.tv_sec * 1000 + end.tv_usec / 1000) - (start.tv_sec * 1000 + start.tv_usec / 1000);
}

int main(int argc, char **argv) {
    int duration = 1000; // ms
    int capacity = 100000;

    Micro_bench *p_mb = (Micro_bench*)malloc(sizeof(Micro_bench));
    p_mb->duration = duration;
    p_mb->array = (Array*)malloc(sizeof(Array));
    p_mb->array->values = (int*)malloc(sizeof(int) * capacity);
    p_mb->array->capacity = capacity;
    p_mb->array->size = 0;

    srand((int)time(NULL));
    rand_init(p_mb->seed);

    p_mb->searches = 0;
    p_mb->pushes = 0;
    p_mb->pops = 0;

    run_test(p_mb);

    // Write throughput results
    char *file_name = "throughput.csv";
    int insert_header = access(file_name, F_OK);
    FILE *fptr = fopen(file_name, "a");
    if (insert_header != 0) {
        fprintf(fptr, "capacity,duration_ms,searches,pushes,pops\n");
    }
    fprintf(fptr, "%d,%d,%lld,%lld,%lld\n", capacity, p_mb->duration, p_mb->searches, p_mb->pushes, p_mb->pops);
    fclose(fptr);

#ifdef USE_PAPI
    // Write PAPI cache results if enabled
    int papi_header = access("cache_papi.csv", F_OK);
    FILE *papi_ptr = fopen("cache_papi.csv", "a");
    if (papi_header != 0) {
        fprintf(papi_ptr, "function,calls,instructions,l1_dcache_loads,l1_dcache_load_misses,l2_references,l2_misses,llc_loads,llc_load_misses\n");
    }
    for (int i = 0; i < 3; i++) {
        fprintf(papi_ptr, "%s,%lld", func_prof[i]->name, func_prof[i]->calls);
        for (int j = 0; j < NUM_EVENTS; j++) {
            fprintf(papi_ptr, ",%lld", func_prof[i]->events[j]);
        }
        fprintf(papi_ptr, "\n");
        free(func_prof[i]);
    }
    fclose(papi_ptr);
#endif

    free(p_mb->array->values);
    free(p_mb->array);
    free(p_mb);

    return 0;
}