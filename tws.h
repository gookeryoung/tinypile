#pragma once

/* Tiny, backend-agnostic mostly-lockless threadpool and scheduler

TL;DR:
- Safe multithreading made easy!
- Split your work into Jobs, submit them into a threadpool
- Supports job dependencies, jobs generating more jobs, waiting for job completion
- Don't use this API directly -- you'll want a wrapper.
  (E.g. https://github.com/fgenesis/tinypile/blob/master/tws.hh or make your own)

Design goals:
- Plain C API, KISS.
- Bring your own threading & semaphores (6 function pointers in total)
- Different thread & job types for fine-grained control
- As many debug assertions as possible to catch user error (if it says RTFM, do that)
- No memory allocations during regular operation unless you spam & overload the pool

For example code and usage information, see the end of this file.

License: WTFPL because lawyers suck. except netpoet. hi netpoet! <3

For more info, see tws.cpp.
*/

#include <stddef.h> // for size_t, intptr_t, uintptr_t

#if (defined(__GNUC__) && (__GNUC__ >= 4)) || defined(__clang__)
#define TWS_PLEASE_CHECK __attribute__((warn_unused_result))
#elif defined(_MSC_VER) && (_MSC_VER >= 1700)
#define TWS_PLEASE_CHECK _Must_inspect_result_
#else
#define TWS_PLEASE_CHECK
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Pre-defined job types.
// Values that are not used here are free for your own use.
// Each thread is specialized in one work type and can only process that specific type.
// tws_TINY is special: Use it to annotate "tiny" work units that
// are not worth to distribute to worker threads.
// See http://cbloomrants.blogspot.com/2012/11/11-08-12-job-system-task-types.html for more info.
typedef enum
{
    tws_TINY = -1,  // Treated specially to reduce overhead; any thread can run this
    tws_DEFAULT  = 0, // For "standard" CPU jobs.
    tws_WORKTYPE_USER = 1, // use this as start for your own enum
} tws_WorkType_;

// You can define your own task/job types in your code like so:
/*typdef enum
{
    TASK_IO = tws_WORKTYPE_USER,
    TASK_GPU,
    TASK_whatever
};*/

typedef unsigned char tws_WorkType;

typedef enum
{
    tws_ERR_OK                  = 0,
    tws_ERR_ALLOC_FAIL          = -1,
    tws_ERR_FUNCPTRS_INCOMPLETE = -2,
    tws_ERR_PARAM_ERROR         = -3,
    tws_ERR_THREAD_SPAWN_FAIL   = -4,
    tws_ERR_THREAD_INIT_FAIL    = -5
} tws_Error_;

typedef int tws_Error;

// --- Backend details ---

typedef struct tws_Sem    tws_Sem;    // opaque, semaphore handle
typedef struct tws_Thread tws_Thread; // opaque, thread handle

// These structs contain function pointers so that the implementation can stay backend-agnostic.
// All that the backend must support is spawning+joining threads and basic semaphore operation.
// Since you care about multithreading (you do, else you wouldn't be here!) you probably have your own
// implementation of choice already that you should be able to hook up easily.
// If not, suggestions:
//  - tws_backend.h (autodetects Win32, SDL, pthread, possibly more)
//  - If you're on windows, wrap _beginthreadex() and CreateSemaphore()
//  - C++20 (has <thread> and <semaphore> in the STL)
//  - C++11: Has <thread>, but you'd have to roll your own semaphore
//           (See https://stackoverflow.com/questions/4792449)
//  - C11: Has <threads.h> but no semaphores. Roll your own.
//  - POSIX has <pthread.h> and <semaphore.h> but it's a bit fugly across platforms
//  - SDL (http://libsdl.org/)
//  - Turf (https://github.com/preshing/turf)
typedef struct tws_ThreadFn
{
    // spawn new thread that executes run(opaque) as its body.
    // (It may be a good idea to give the thread a name based on its ID.)
    tws_Thread* (*create)(unsigned id, const void *opaque, void (*run)(const void *opaque));

    // wait for thread to finish, then delete thread and return
    void (*join)(tws_Thread *);
} tws_ThreadFn;

typedef struct tws_SemFn
{
    // Create a semaphore with starting count 0.
    // If your backend absolutely wants a maximum count, pass INT_MAX or some other large number.
    tws_Sem* (*create)();
    void (*destroy)(tws_Sem*);

    void (*enter)(tws_Sem*);   // Suspend calling thread until count is positive, then atomically decrease count
    void (*leave)(tws_Sem*);   // Atomically incrase count (never blocks)
} tws_SemFn;

// Optional worker thread entry point.
// Protocol:
// - At the start of the function, initialize whatever you need based on threadID, worktype, userdata.
//    E.g. Set thread priorities, assign GPU contexts, your own threadlocal variables, ...
// - When done initializing, call run(opaque).
// - run() will only return just before the threadpool is destroyed.
// - When run() returns, you can clean up whatever resources you had previously initialized.
// (This is intentionally a callback so that you can do stack allocations before run()!)
// If your init fails for some reason, don't call run(), just return.
// This will be detected and threadpool creation will fail with tws_ERR_THREAD_INIT_FAIL.
typedef void (*tws_RunThread)(int threadID, tws_WorkType worktype, void *userdata, const void *opaque, void (*run)(const void *opaque));

// Optional allocator interface. Same API as luaalloc.h.
// (Ref: https://github.com/fgenesis/tinypile/blob/master/luaalloc.h)
// Cases to handle:
//   ptr == NULL, nsize > 0:  return malloc(nsize);  // ignore osize
//   ptr != NULL, nsize == 0: free(ptr); // osize is size of allocation
//   ptr != NULL, nsize != 0: return realloc(ptr, nsize); // osize = current size
// The returned pointer must be aligned to max(atomic int64 size, pointer size).
// The allocator must be threadsafe as it might be called by multiple threads at once.
typedef void* (*tws_AllocFn)(void *allocUser, void *ptr, size_t osize, size_t nsize);

// ---- Worker function ----

typedef struct tws_Job    tws_Job;    // opaque, a thread job
typedef struct tws_Event  tws_Event;  // opaque, job completion notification

// Job work function -- main entry point of your job code
// job is a pointer to the currently running job. Never NULL.
//  - data points to a copy of the data passed to tws_newJob(). You need to know the size.
//  - You may add children to it if the job function spawns more work.
//    The job will be considered completed when all children have completed.
//  - You may add continuations that will be automatically run after
//    the job (and its children) have completed.
// ev is the (optional) event that will be notified when the job is complete.
//  - you may pass this to additional spawned continuations to make sure those
//    are finished as well before the event is signaled.
// user is the opaque pointer assigned to tws_Setup::threadUser. // TODO CHECK THIS
typedef void (*tws_JobFunc)(void *data, tws_Job *job, tws_Event *ev, void *user);

enum
{
    TWS_CONTINUATION_COST = sizeof(tws_Job*)
};

// ---- Setup config ----

typedef struct tws_Setup
{
    const tws_ThreadFn *threadFn;   // Function pointers for threads (mandatory)
    const tws_SemFn *semFn;         // Function pointers for semaphores (mandatory)

    // All memory allocation goes through this.
    // If you don't provide an allocator, a suitable one based on realloc()/free() will be used.
    tws_AllocFn allocator;
    void *allocUser;

    // Called when a thread is spawned, for each thread. Set to NULL if you don't require a custom init step.
    tws_RunThread runThread;

    // Passed to tws_RunThread and later to each tws_JobFunc called
    void *threadUser;

    const unsigned *threadsPerType; // How many threads to spawn for each work type
                                    // The index is the work type, the value the number of threads for that work type.
                                    // E.g. to spawn 1 thread each for 2 work types, pass {1,1}.

    tws_WorkType threadsPerTypeSize; // # of entries in the threadsPerType array.

    unsigned jobSpace;      // How many bytes to make available for userdata in a job.
                            // Set this to the maximum number of bytes you will typically need, but keep it as small as possible.
                            // Each continuation added to a job costs TWS_CONTINUATION_COST bytes.
                            // So this number should be the sum of user data and space needed for continuations.
                            // The actually used value is extended so that the end of the job data is also the end of a cache line;
                            // the resulting size can be retrieved in tws_MemInfo::jobSpace.
                            // If job data and number of continuations for a job exceed this space, extra memory is allocated from the heap.
                            // TL;DR if you have no idea what to put here, set this to the same value as cacheLineSize.

    unsigned cacheLineSize; // The desired alignment of most internal structs, in bytes.
                            // Should be equal to or a multiple of the CPU's L1 cache line size to avoid false sharing.
                            // Must be power of 2.
                            // Recommended: 64, unless you know your architecture is different.

    unsigned jobsPerThread; // How many in-flight jobs one thread can hold. If you push more jobs into the system than it can handle
                            // it will push jobs into internal spillover queues that are rather slow in comparison to the usual lockfree operation.
                            // Recommended: 1024 for starters. Increase as needed. Internally rounded up to a power of 2.
                            // (Required memory: threads * jobsPerThread * (jobTotalSize + sizeof(tws_Job*))
} tws_Setup;

typedef struct tws_MemInfo
{
    size_t cacheLineSize;   // As passed via tws_Setup::cacheLineSize
    size_t jobSpace;        // usable space in a job for user data, >= the value passed in tws_Setup
    size_t jobTotalSize;    // size of a single job in bytes, padded to specified cache line size
    size_t eventAllocSize;  // Internal size of a tws_Event, including padding to cache line size
    size_t jobMemPerThread; // raw job storage memory required by one thread
} tws_MemInfo;


// --- Threadpool control ---

// Checks your config struct and returns tws_ERR_OK (0) if it's fine,
// or an error code if there's an obvious problem.
// Optionally, pass 'mem' to fill the struct with memory usage information.
tws_Error tws_info(const tws_Setup *cfg, tws_MemInfo *mem);

// Setup the thread pool given a setup configuration.
// Returns tws_ERR_OK (0) on success or an error if failed.
tws_Error tws_init(const tws_Setup *cfg);

// Signals all pool threads to please stop ASAP. Returns once everything is stopped and cleaned up.
// Submitting new jobs from inside job functions is still possible but they may or may not be processed.
// Submitting new jobs from outside (incl. other threads) is undefined behavior.
// Any tws pointers become invalid for the outside world once this function is called.
void tws_shutdown();

// --- Job functions ---

// Create a new job.
// You can create child jobs and add continuations until the job is submitted.
// When inside a job's work function, you may add more children and continuations to the currently running job.
// 'data[0..size)' is copied into the job. If there is not enough space, a heap allocation is made.
//    If that fails too, job creation will fail and return NULL.
// 'maxcont' is the maximum number of continuations that you will ever add to this job. Must be known up-front. Adding less is ok.
// 'type' specifies which threads can run this job.
// Optionally, pass an event that will indicate when the job has completed.
// After allocating a new job, add childen and continuations as required, then submit it ASAP. Don't keep it around for later.
// You *must* submit a job eventually; not doing so is a resource leak.
// Sumbitting a job may run it immediately. More importantly, parent jobs may be run anytime wrt. their children.
// Parent-child relation is ONLY used for when to consider a job done (parent is done when all children are done);
// to express dependencies, use continuations.
tws_Job *tws_newJob(tws_JobFunc f, const void *data, size_t size, unsigned short maxcont, tws_WorkType type, tws_Job *parent, tws_Event *ev);

// Similar to tws_newJob(), but does not copy any data into the job.
// Instead, a pointer to the job data is returned in pdata.
// This function is intended for interfacing with C++ where memory is non-POD and we need an exposed pointer for placement new or similar.
tws_Job *tws_newJobNoInit(tws_JobFunc f, void **pdata, size_t size, unsigned short maxcont, tws_WorkType type, tws_Job *parent, tws_Event *ev);

// Shortcut to add an empty job.
// This is useful to set as parent for some other jobs that need to run first,
// and for registering continuations that have to run when those child jobs are done.
inline tws_Job *tws_newEmptyJob(unsigned maxcont)
{
    return tws_newJob(NULL, NULL, 0, maxcont, tws_TINY, NULL, NULL);
}

// Submit a job. Submit children first, then the parent.
// Allocating a job via tws_newJob() in one thread and then submitting it in another is ok but not recommended.
// Once a job is submitted it is undefined behavior to use the job pointer outside of the running job function itself.
// (Treat the job pointer as if it was free()'d)
// If ancestor is set, submit the job as a continuation of ancestor:
//   It will be started upon completion of the ancestor job.
//   If a job's max. continuation number is exceeded this will assert() and fail.
// Returns 1 when queued, executed, or registered as continuation, 0 when failed.
// Returns 0 if and only if:
//   1) ancestor == NULL and an internal memory allocation fails (this does not assert()), -OR-
//   2) ancestor != NULL and the ancestor's continuation space is full (this does assert()).
// so if you don't worry about memory you may ignore the return value and trust the asserts.
// Never pass job == NULL.
// ProTip: If you're writing a wrapper for this, make SURE this never returns 0.
//         Assert this as hard as you can, otherwise you may run into very hard to detect problems.
TWS_PLEASE_CHECK int tws_submit(tws_Job *job, tws_Job *ancestor /* = NULL */);

// --- Event functions ---
// Create an event to indicate job completion.
// An event can be submitted along one or more jobs and can be queried whether all associated jobs have finished.
// An event initially starts with a count of 0. Submitting an event increases the count by 1,
// completion of a job decreases the count by 1. An event is "done" when the count is 0.
// Avoid creating and deleting events repeatedly, re-use them if possible.
tws_Event *tws_newEvent();

// Delete a previously created event.
// Deleting an in-flight event is undefined behavior.
void tws_destroyEvent(tws_Event *ev);

// Quick check whether an event is done. Non-blocking.
int tws_isDone(const tws_Event *ev);

// Wait until an event signals completion.
// Any number of threads can be waiting on an event. All waiting threads will continue once the event is signaled.
// Set help to an array of the type of jobs the calling thread may process while waiting.
// Pass n == 0 to just idle.
// If you choose to help note that any job that the calling thread picks up must be finished
// before this can return, so the wait may last longer than intended.
void tws_wait(tws_Event *ev, tws_WorkType *help, size_t n);

// Convenience for 0 or 1 help type
inline void tws_wait0(tws_Event *ev) { tws_wait(ev, NULL, 0); } // don't help, just idle
inline void tws_wait1(tws_Event *ev, tws_WorkType help) { tws_wait(ev, &help, 1); }


// --- Promise API ---

// A promise provides an easy way of returning data from a job asynchronously.
// It's like an event except it's fully user controlled and can carry data.

/* Usage:

struct JobData
{
    tws_Promise *pr;
    // whatever else you need as input
};

// In func, calculate whatever you want to return:
static void func(void *data, tws_Job *curjob, tws_Event *ev, void *user)
{
    JobData *dat = (JobData*)data;
    MyResult *res = (MyResult*)tws_getPromiseData(dat->pr, NULL);
    int ok = calcSomething(res, dat); // assumed to return 1 on success, 0 on fail
    tws_fulfillPromise(dat->pr, ok);
}

// Launch like this:
tws_Promise *pr = tws_newPromise(sizeof(res));
JobData dat { pr, ... };
tws_Job *job = tws_newJob(func, &dat, sizeof(dat), ...)
tws_submit(job, NULL);

// Do something else in the meantime...
// Eventually, get the result:

if(tws_waitPromise(pr))
    success((MyResult*)tws_getPromiseData(pr, NULL));
else
    fail();
tws_destroyPromise(pr);
*/

typedef struct tws_Promise tws_Promise;

// Allocate a new promise with 'space' bytes for return data.
// Alignment is optional and may be set if the promise's internal memory must be aligned to a certain size.
// Alignment must be power of 2, or pass 0 if you don't care.
tws_Promise *tws_newPromise(size_t space, size_t alignment);

// Delete a previously allocated promise.
// Deleting an in-flight promise is undefined behavior.
void tws_destroyPromise(tws_Promise *pr);

// Reset a promise to pristine state, allowing to use it again without re-allocating.
// Same rules as tws_destroyPromise() apply, don't reset an in-flight promise!
// Calling this more than once has no effect.
void tws_resetPromise(tws_Promise *pr);

// Non-blocking; returns 1 when a promise was fulfilled, 0 when it was not.
int tws_isDonePromise(const tws_Promise *pr);

// Get pointer into internal promise memory region reserved for return data.
// psize is set to capacity if passed, ignored if NULL.
// This function is needed at least twice:
//    First time to set the data (copy data into the returned pointer),
//    Second after tws_waitPromise() returns, to get a pointer to the returned data.
// You can store any data inside the promise; the memory is not touched and will not get cleared on reset.
// The returned pointer is be aligned to cache line size.
void *tws_getPromiseData(tws_Promise *pr, size_t *psize);

// Fulfill (or fail) a promise. Code is up to you and will be returned by tws_waitPromise().
// You must call this function exactly once per promise.
// You may call this function again after resetting the promise.
// Order of operations:
//  - Call tws_getPromiseData() first and copy whatever you want to return
//  - Afterwards, call this function.
void tws_fulfillPromise(tws_Promise *pr, int code);

// Wait until promise is done, and return the code passed to tws_fulfillPromise().
// After this function returns, you may use tws_getPromiseData() to get the return data.
int tws_waitPromise(tws_Promise *pr);




// --- Threadpool status - For information/debug purposes only ---

// Query current status of the lock-free queues.
// pSizes is an array with 'n' entries (usually one per thread in the pool). Can be NULL.
//   Each entry is set to the number of elements currently in the lockfree queue
//   of the corresponding thread.
//   The access to the internal queue is not synchronized in any way, take the numbers as an estimate!
// pMax receives the queue capacity (single number). Can be NULL.
//   (Same as tws_Setup::jobsPerThread passed during init.)
// Returns how many entries would be written to pSizes (= how many threads in the pool)
size_t tws_queueLevels(size_t *pSizes, size_t n, size_t *pCapacity);

// Query current status of the spillover queues.
// pSizes is an array with 'n' entries (usually one per work type in use). Can be NULL.
//   Each entry is set to the number of elements currently in the spillover queue
//   for that work type.
// Ideally pSizes is all zeros, or close to! If it's not: You're overloading the scheduler.
//   To fix:
//     - Increase tws_Setup::jobsPerThread
//     - Spawn more threads
//     - Submit less jobs from external threads (only jobs started from within jobs use the fast, lockless path)
//     - Submit less jobs in general
//   While this is not a problem it will degrade performance.
// Returns how many entries would be written to pSizes (= how many work types in use)
size_t tws_spillLevels(size_t *pSizes, tws_WorkType n);



// --- Utility functions for wrappers ---

typedef int tws_SpinLock;
void tws_atomicLock(tws_SpinLock *lock);
int tws_atomicTryLock(tws_SpinLock *lock); // attempt to lock (non-blocking), 1 when locked, 0 when not
void tws_atomicUnlock(tws_SpinLock *lock);



#ifdef __cplusplus
}
#endif

/*
How to use:

// --- EXAMPLE CODE BEGIN ---

// -- backend setup --
// if you need a quick & dirty ready-made backend, see tws_backend.h
static const tws_ThreadFn thfn = { <your function pointers> };
static const tws_SemFn semfn = { <your function pointers> };

// -- init threadpool --
tws_Setup ts; // This can be allocated on the stack; it's no longer needed after tws_init().
memset(&ts, 0, sizeof(ts)); // we're not using optional fields here, make sure those are cleared
ts.cacheLineSize = 64;     // <-- whatever fits your target architecture
ts.jobSpace = 64;          // <-- whatever size you need
ts.jobsPerThread = 1024;   // <-- each thread gets this many slots for jobs to queue
unsigned threads[] = {4};  // <-- might be a good idea to auto-detect this
ts.threadsPerType = &threads; // <-- Can specify more than one work type if needed, e.g. an extra disk I/O thread
ts.threadsPerTypeSize = 1;    // <-- #entries in that array
ts.threadFn = &thfn;       // link up backend (thread funcs)
ts.semFn = &semfn;         // link up backend (semaphore funcs)
if(tws_init(&ts) != tws_ERR_OK)  // start up threadpool
    gtfo("threadpool init failed");
// now ready to submit jobs

// -- worker functions --
struct ProcessInfo // for passing data to workers
{
    float *begin;
    size_t size;
};
void processChunk(void *data, tws_Job *job, tws_Event *ev, void *user)
{
    ProcessInfo *info = (ProcessInfo*)data;
    // work on info->begin[0 .. info->size)
}
void split(void *data, tws_Job *job, tws_Event *ev, void *user)
{
    ProcessInfo *info = (ProcessInfo*)data;
    const size_t CHUNK = 8*1024;
    size_t remain = info->size;
    for(size_t i = 0; remain; i += CHUNK) // split work into chunks
    {
        const size_t todo = remain < CHUNK ? remain : CHUNK; // handle incomplete sizes
        remain -= todo;
        const struct ProcessInfo info = { &work[i], todo }; // this will be stored in the job
        // Start as child of the current job
        // parameters:          (func,         data,  maxcont,  type     parent, event)
        tws_Job *ch = tws_newJob(processChunk, &info,    0,  tws_DEFAULT,  job,  NULL);
        tws_submit(ch, NULL);
    }
    // some finalization function to run after everything is processed. Could be added before or after the children in this example.
    // note that this adds itself to the event so that fin must complete before the event is signaled
    tws_Job *fin = tws_newJob(finalize, &all, sizeof(all), NULL, tws_DEFAULT, ev);
    tws_submit(fin, job); // set fin to run as continuation after root is done
}

// -- get some work done --
const size_t SZ = 1024*1024;
float work[SZ] = ...;
tws_Event *event = tws_newEvent(); // for synchronization
const struct ProcessInfo all = { &work[0], SZ };
// parameters:            (func,  data, maxcont,  type,    parent, event)
tws_Job *root = tws_newJob(split, &all,    1,  tws_DEFAULT, NULL,  event);

tws_submit(root, NULL); // Submit the root job to start the chain.
// root is done when all children are done
// Once root is done, fin is run as continuation

tws_wait1(event, tws_DEFAULT); // wait until root and fin are done; the calling thread will help working

tws_destroyEvent(event); // ideally you'd keep the event around when running this multiple times

tws_shutdown(); // whenever you're done using it

// -- In summary:
// -- This setup will launch one task to split work into smaller chunks,
// -- process these in parallel on all available threads in the pool,
// -- then run a finalization step on the data.

// --- EXAMPLE CODE END ---


Rules of thumb:

- The fast path is everything that happens in a job function:
  - Any followup jobs allocated and submitted inside of a job will use the lockless path.
  - Any job allocated outside of a job function will use the slower spillover path.
  --> Ideally, launch a single job that figures out the work that needs to be done, then adds children to itself.
      A nice side effect is that the caller can already move on and do other things while the job system adds work to itself in the background.

- If you're using an event together with a parent, and spawn child jobs from that parent, don't add the event to every child job.
  (It's not incorrect do do this, just spammy, unnecessary, and slower than it has to be.)
  The parent will be done once all children are done, and only then signal the event (and run continuations).

- Set your tws_Setup::jobSpace high enough that job data and continuations you will add fit in there.
  It is no problem if once in a while a large data block has to be added,
  but this will fallback to a heap allocation every time, which you want to avoid.
  Just pass a pointer to your data and ensure the memory stays valid while jobs work on it.
*/


/* ASSERT:
- tws_drain() to wait for all the things to finish and also reset LQ top+bottom?
- add TWS_RESTRICT
- TWS_CHECK_WARN() + add notification callback? (called when spilled, too large job is pushed, etc)

typedef enum tws_Warn
{
    TWS_WARN_JOB_SPILLED,       // job was unexpectedly spilled to (slower) backup queue
    TWS_WARN_JOB_REALLOCATED,   // could not store all data within the job; had to allocate an extra heap block
    TWS_WARN_JOB_SLOW_ALLOC,    // job had to be allocated from the slow global heap instead of the fast per-worker storage
} tws_Warn;
*/
