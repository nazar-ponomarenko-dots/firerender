// ============================================================================
//  jobs.h — mini worker-thread pool (SDL3)
// ----------------------------------------------------------------------------
//  Why: heavy CPU phases (baking masks, chunk generation, image decoding)
//  are split into ranges and run across all cores. We do NOT touch the GPU
//  or the render context — a worker only computes data.
//  Usage:
//    jobs_init();                       // once at startup (lazy is fine too)
//    jobs_parallel_for(first, count, fn, ud);   // fn(i0, i1, ud) per slice
//  fn receives a bounded half-open range [i0, i1) and its own ud.
//  The call blocks the calling thread until all work is done. Thread-safe.
// ============================================================================
#ifndef FR_JOBS_H
#define FR_JOBS_H

#include <stddef.h>

typedef void (*JobsFn)(int i0, int i1, void *ud);

// Bring up the pool (worker count = cores - 1, minimum 1). Repeat calls are no-ops.
void jobs_init(void);

// Run count iterations in parallel: the pool cuts [first, first+count) into
// slices and calls fn from the workers. With a 1-thread pool it's just a loop.
void jobs_parallel_for(int first, int count, JobsFn fn, void *ud);

// Number of workers (including the caller? no — background threads only).
int jobs_workers(void);

#endif
