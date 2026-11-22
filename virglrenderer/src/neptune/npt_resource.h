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

/* Hard cap on per-call payload byte_size.  Bounded so the byte_size+7
 * alignment math can't wrap to 0 on a hostile value and read stale
 * bytes.  It sizes no buffer: the payload is read in place from the
 * command stream (the guest's upload buffer, grown to fit, behind
 * EXECUTE_COMMAND_STREAM's range check), and only where it is in the
 * stream (npt_cs_decoder_get_blob_storage, or the temp pool's own 1 GiB
 * bound).  64 MiB refused real uploads: VRChat sends one of 70,744,960
 * bytes after an avatar loads, and the refusal fails the guest's ring
 * for good.  1 GiB is the largest a 16384 x 16384 RGBA8 subresource
 * takes, keeps byte_size+7 and the guest's uint32_t command size and
 * doubling upload buffer clear of wrapping, and equals the temp pool's
 * bound. */
#define NPT_MAX_RESOURCE_UPDATE_BYTES (1u << 30)

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
