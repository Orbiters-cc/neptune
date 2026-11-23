/*
 * SPDX-License-Identifier: MIT
 */

#define OTL_IMPLEMENTATION
#include "npt_otl.h"

#include "npt_context.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11devicecontext.h"

/* What this ring's thread decoded since its last Signal. */
static _Thread_local struct {
   uint64_t batch, busy, first;
   uint64_t commands, draws, dispatches, maps, unmaps, updates, flushes;
   uint32_t copies, targets;
} since;

void
npt_otl_batch_begin(void)
{
   since.batch = otl_now();
   if (!since.first)
      since.first = since.batch;
}

void
npt_otl_batch_end(void)
{
   since.busy += otl_now() - since.batch;
   since.batch = 0;
}

void
npt_otl_command(uint32_t cmd_type)
{
   since.commands++;
   if (cmd_type >> 24 != 255u ||
       ((cmd_type >> 8) & 0xffffu) != NPT_IFACE_ID_ID3D11DeviceContext)
      return;
   switch (cmd_type & 0xffu) {
   case NPT_METHOD_ID3D11DeviceContext_Draw:
   case NPT_METHOD_ID3D11DeviceContext_DrawIndexed:
   case NPT_METHOD_ID3D11DeviceContext_DrawInstanced:
   case NPT_METHOD_ID3D11DeviceContext_DrawIndexedInstanced:
   case NPT_METHOD_ID3D11DeviceContext_DrawAuto:
   case NPT_METHOD_ID3D11DeviceContext_DrawIndexedInstancedIndirect:
   case NPT_METHOD_ID3D11DeviceContext_DrawInstancedIndirect:
      since.draws++;
      break;
   case NPT_METHOD_ID3D11DeviceContext_Map:
      since.maps++;
      break;
   case NPT_METHOD_ID3D11DeviceContext_Unmap:
      since.unmaps++;
      break;
   case NPT_METHOD_ID3D11DeviceContext_UpdateSubresource:
      since.updates++;
      break;
   case NPT_METHOD_ID3D11DeviceContext_Flush:
      since.flushes++;
      break;
   case NPT_METHOD_ID3D11DeviceContext_CopyResource:
   case NPT_METHOD_ID3D11DeviceContext_CopySubresourceRegion:
      since.copies++;
      break;
   case NPT_METHOD_ID3D11DeviceContext_OMSetRenderTargets:
   case NPT_METHOD_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews:
      since.targets++;
      break;
   default:
      break;
   }
}

void
npt_otl_signal(uint32_t ctx_id, uint64_t start, void *fence, uint64_t value)
{
   const uint64_t now = otl_now();
   const uint64_t busy = since.busy + (since.batch ? now - since.batch : 0);
   otl_put(now, OTL_N_SIGNAL, ctx_id, start, (uint64_t)(uintptr_t)fence, value, busy,
           since.first, since.commands);
   otl_put(now, OTL_N_COUNTS, ctx_id, since.draws, since.maps, since.unmaps, since.updates,
           since.flushes, (uint64_t)since.copies << 32 | since.targets);
   otl_hist(OTL_H_A, busy);
   const uint64_t batch = since.batch;
   memset(&since, 0, sizeof(since));
   if (batch)
      since.batch = since.first = now;
}

static void
npt_otl_CopyResource(UNUSED struct npt_dispatch_context *dctx,
                     struct npt_command_ID3D11DeviceContext_CopyResource *args,
                     PFN_ID3D11DeviceContext_CopyResource original)
{
   const uint64_t start = otl_now();
   original(args->_self, args->pDstResource, args->pSrcResource);
   otl_put(otl_now(), OTL_N_COPY, 0, start, (uint64_t)(uintptr_t)args->pDstResource,
           (uint64_t)(uintptr_t)args->pSrcResource, 0, 0, 0);
}

static void
npt_otl_Flush(UNUSED struct npt_dispatch_context *dctx,
              struct npt_command_ID3D11DeviceContext_Flush *args,
              PFN_ID3D11DeviceContext_Flush original)
{
   const uint64_t start = otl_now();
   original(args->_self);
   otl_put(otl_now(), OTL_N_FLUSH, 0, start, 0, 0, 0, 0, 0);
}

void
npt_otl_init(UNUSED struct npt_context *ctx)
{
   otl_init("npt");
   if (!otl_active())
      return;
   /* Off, these slots stay NULL and the default dispatch runs, as it does without this file. */
   npt_query_dc_overrides.CopyResource = npt_otl_CopyResource;
   npt_query_dc_overrides.Flush = npt_otl_Flush;
}
