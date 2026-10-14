// ============================================================================
//  snowmask.h — per-vertex mesh flag "open to the sky from above" (for snow)
// ----------------------------------------------------------------------------
//  For GLB models (cars etc.) the baked "sky visibility" map doesn't know
//  their geometry, so it used to dump snow INSIDE closed cavities (the cabin).
//  Here, at load time, each model vertex is tested: does snow fall onto it
//  from the RIGHT direction — i.e. is there its own geometry above it
//  (a roof). A vertex with stuff above it (floor/seat under a roof) gets 0,
//  an open roof/hood/trunk gets 1. The result goes into a separate vertex
//  buffer aSnow and is multiplied into the snow in mesh.fs.
// ============================================================================
#ifndef SNOWMASK_H
#define SNOWMASK_H

// Computes the per-vertex "clearly exposed from above" factor (0/1) for the
// merged primitives. verts[p] — vertices of primitive p (3 floats each),
// indices[p] — indices (triplets).
// Returns a malloc'd array of length out_total (= sum of vcounts) or NULL.
float *snow_mask_compute(const float *const *verts, const int *vcounts,
                         const unsigned *const *indices, const int *idxcounts,
                         int nprims, int *out_total);

#endif