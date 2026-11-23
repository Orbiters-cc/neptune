/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-command helpers for RESOURCE_{UPDATE, MAP, UNMAP}.  D3D11
 * resource manipulation lives here so npt_dispatch.c can stay a
 * decode/encode shell.
 */

#ifndef NPT_RESOURCE_H
#define NPT_RESOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "npt_com.h"

struct npt_context;
struct npt_resource;

/* The largest RESOURCE_UPDATE taken.  The payload is read in place from
 * the command stream (the guest copies anything larger than the ring's
 * direct size into its upload buffer and sends EXECUTE_COMMAND_STREAM over
 * it), and the command is refused unless the whole of it is in the stream.
 * 64 MiB refused real uploads: VRChat sends one of 70,744,960 bytes after
 * an avatar loads, and the refusal failed the guest's ring for good.
 * 256 MiB is what the whole path was tested to (this repository's
 * scripts/test-resource-update.sh, a stand-in resource), 3.8 times the
 * largest upload seen, and keeps byte_size + 7, the guest's uint32_t
 * command size and its doubling upload buffer (512 MiB for a 256 MiB
 * payload) far from wrapping. */
#define NPT_MAX_RESOURCE_UPDATE_BYTES (256u << 20)

/* At most this much payload is staged in decoders' temporary buffers at
 * once, whatever the contexts: a payload not contiguous in the stream is
 * copied there, and many contexts at once must not each take the cap. */
#define NPT_RESOURCE_UPDATE_STAGING_BUDGET (512ull << 20)

/* The bytes UpdateSubresource reads from the payload for `resource`'s
 * `subresource` with the given box and pitches, from the resource's own
 * description: UINT64_MAX when the update is not one D3D11 can take (a box
 * outside the subresource, a row pitch shorter than a row, a format whose
 * layout is not known here). Exposed for the tests. */
uint64_t
npt_resource_update_footprint(void *resource, uint32_t subresource,
                              uint32_t row_pitch, uint32_t depth_pitch,
                              bool has_box,
                              uint32_t box_left, uint32_t box_top, uint32_t box_front,
                              uint32_t box_right, uint32_t box_bottom, uint32_t box_back);



/* RESOURCE_UPDATE.  `payload` points at the contiguous source bytes in
 * the decoder's stream; lifetime is the caller's responsibility. */
void
npt_resource_update(struct npt_context *ctx,
                    uint64_t resource_id, uint32_t subresource,
                    uint32_t row_pitch, uint32_t depth_pitch,
                    uint32_t byte_size,
                    bool has_box,
                    uint32_t box_left, uint32_t box_top, uint32_t box_front,
                    uint32_t box_right, uint32_t box_bottom, uint32_t box_back,
                    const void *payload);

/* RESOURCE_MAP.  read_range_{begin,end} are accepted for the D3D12
 * path (pReadRange) and ignored on D3D11.  shmem_res is the guest's
 * slot, pinned by the caller with npt_context_pin_shm_resource (NULL
 * when shmem_res_id names no mapped SHM resource). */
HRESULT
npt_resource_map(struct npt_context *ctx,
                 uint64_t context_id, uint64_t resource_id,
                 uint32_t subresource, uint32_t access_flags,
                 uint32_t api_map_flags,
                 struct npt_resource *shmem_res, uint32_t shmem_res_id,
                 uint64_t read_range_begin, uint64_t read_range_end,
                 uint64_t byte_size,
                 uint32_t mip_height, uint32_t mip_depth,
                 uint32_t shmem_offset,
                 uint32_t *out_row_pitch, uint32_t *out_depth_pitch,
                 uint32_t *out_mapped_size);

/* RESOURCE_UNMAP.  written_range_{begin,end} are accepted for the
 * D3D12 path (pWrittenRange) and ignored on D3D11.  shmem_res as for
 * RESOURCE_MAP. */
HRESULT
npt_resource_unmap(struct npt_context *ctx,
                   uint64_t context_id, uint64_t resource_id,
                   uint32_t subresource,
                   struct npt_resource *shmem_res, uint32_t shmem_res_id,
                   uint32_t shmem_offset, uint64_t byte_size,
                   uint32_t access_flags,
                   uint64_t written_range_begin,
                   uint64_t written_range_end);

#endif /* NPT_RESOURCE_H */
