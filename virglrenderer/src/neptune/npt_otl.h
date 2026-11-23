/*
 * SPDX-License-Identifier: MIT
 *
 * The render server's part of the host timeline (otl.h): what a ring decodes per presented frame, and the fences of
 * the event rings.  Off unless OBELISK_TIMELINE is set for this process.
 */

#ifndef NPT_OTL_H
#define NPT_OTL_H

#include <stdint.h>

#include "otl.h"

struct npt_context;

/* Once per context, in the process that decodes it (after the fork). */
void npt_otl_init(struct npt_context *ctx);

/* On a ring's thread, while the timeline is on: a batch of commands begins and ends, one command is about to run. */
void npt_otl_batch_begin(void);
void npt_otl_batch_end(void);
void npt_otl_command(uint32_t cmd_type);

/* The guest exported an image (a texture other processes map), and let go of an object. */
void npt_otl_export(uint32_t ctx_id, void *texture, uint64_t identity, uint64_t size, uint32_t format);
void npt_otl_release(void *object);

/* A D3D11 fence Signal was decoded, between start and now. */
void npt_otl_signal(uint32_t ctx_id, uint64_t start, void *fence, uint64_t value);

#endif /* NPT_OTL_H */
