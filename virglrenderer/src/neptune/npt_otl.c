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
   uint64_t exported, export_id, copy_start, copy_end; /* CopyResource into exported images */
} since;

/* The images the guest exported and still holds: what a pointer names now. Written by the ring threads, under the lock;
 * read without it at each CopyResource (a slot is another image only once the guest let go of the first). */
#define EXPORTS 64
static struct {
   _Atomic(void *) texture;
   _Atomic(uint64_t) id;
} exports[EXPORTS];
static _Atomic(uint32_t) exports_end;
static mtx_t exports_mutex;

static uint64_t
npt_otl_export_of(const void *texture)
{
   const uint32_t end = atomic_load_explicit(&exports_end, memory_order_acquire);
   for (uint32_t i = 0; i < end; i++) {
      if (atomic_load_explicit(&exports[i].texture, memory_order_acquire) == texture)
         return atomic_load_explicit(&exports[i].id, memory_order_relaxed);
   }
   return 0;
}

void
npt_otl_export(uint32_t ctx_id, void *texture, uint64_t identity, uint64_t size, uint32_t format)
{
   const uint64_t id = otl_id();
   mtx_lock(&exports_mutex);
   const uint32_t end = atomic_load(&exports_end);
   uint32_t at = 0;
   while (at < end && atomic_load(&exports[at].texture) != texture)
      at++;
   if (at == end) { /* not exported before: a slot left free, or the next */
      at = 0;
      while (at < end && atomic_load(&exports[at].texture))
         at++;
   }
   if (at < EXPORTS) {
      atomic_store_explicit(&exports[at].id, id, memory_order_relaxed);
      atomic_store_explicit(&exports[at].texture, texture, memory_order_release);
      if (at == end)
         atomic_store_explicit(&exports_end, end + 1, memory_order_release);
   }
   mtx_unlock(&exports_mutex);
   otl_put(otl_now(), OTL_N_EXPORT, ctx_id, (uint64_t)(uintptr_t)texture, identity, size, format,
           id, 0);
}

void
npt_otl_release(void *object)
{
   if (!npt_otl_export_of(object))
      return;
   mtx_lock(&exports_mutex);
   for (uint32_t i = 0; i < EXPORTS; i++) {
      if (atomic_load(&exports[i].texture) != object)
         continue;
      otl_put(otl_now(), OTL_N_GONE, 0, (uint64_t)(uintptr_t)object, atomic_load(&exports[i].id), 0, 0,
              0, 0);
      atomic_store(&exports[i].texture, NULL);
      break;
   }
   mtx_unlock(&exports_mutex);
}

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
   if (since.exported)
      otl_put(now, OTL_N_FRAME, (uint32_t)since.exported, (uint64_t)(uintptr_t)fence, value,
              since.export_id, since.copy_start, since.copy_end, otl_id());
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
   const uint64_t id = npt_otl_export_of(args->pDstResource);
   const uint64_t start = id ? otl_now() : 0;
   original(args->_self, args->pDstResource, args->pSrcResource);
   if (!id)
      return;
   since.exported++;
   since.export_id = id;
   since.copy_start = start;
   since.copy_end = otl_now();
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

static void
npt_otl_mutex_init(void)
{
   mtx_init(&exports_mutex, mtx_plain);
}

void
npt_otl_init(UNUSED struct npt_context *ctx)
{
   static once_flag once = ONCE_FLAG_INIT;
   otl_init("npt");
   if (!otl_active())
      return;
   call_once(&once, npt_otl_mutex_init);
   /* Off, these slots stay NULL and the default dispatch runs, as it does without this file. */
   npt_query_dc_overrides.CopyResource = npt_otl_CopyResource;
   npt_query_dc_overrides.Flush = npt_otl_Flush;
}
