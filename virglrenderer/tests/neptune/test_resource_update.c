/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * RESOURCE_UPDATE through the render server's own decoder and dispatch
 * (npt_context_submit_cmd -> npt_dispatch_resource_update ->
 * npt_resource_update), into a stand-in D3D11 resource, device and
 * immediate context that record what UpdateSubresource is given.
 *
 * Why: VRChat's first upload after its avatar loaded was one RESOURCE_UPDATE
 * of 70,744,960 bytes, which the server refused ("exceeds cap 67108864") and
 * then failed the guest's ring for good. The payload arrives inline in the
 * command stream (the guest copies anything larger than the ring's direct
 * size into its own upload buffer, grown to fit, and sends
 * EXECUTE_COMMAND_STREAM over it), so the cap guarded no buffer of its own:
 * it only kept byte_size + 7 from wrapping. These checks hold the server to
 * that: large updates arrive whole, a byte_size past the stream or past the
 * cap is still refused, and nothing is called for a refused one.
 *
 * Built and run by scripts/test-resource-update.sh against build/server.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "npt_context.h"
#include "npt_resource.h"
#include "npt_transport_defs.h"

struct fake {
   void **vtbl;
};

static struct fake resource, device, context;
static void *resource_vtbl[64], *device_vtbl[64], *context_vtbl[128];

static unsigned updates;
static const void *seen_payload;
static UINT seen_sub, seen_row, seen_depth;
static int seen_box;
static uint64_t seen_sum;

static uint32_t NPT_STDMETHODCALLTYPE
fake_add_ref(void *self)
{
   (void)self;
   return 1;
}

static uint32_t NPT_STDMETHODCALLTYPE
fake_release(void *self)
{
   (void)self;
   return 1;
}

static void NPT_STDMETHODCALLTYPE
fake_get_device(void *self, ID3D11Device **out)
{
   (void)self;
   *out = (ID3D11Device *)(void *)&device;
}

static void NPT_STDMETHODCALLTYPE
fake_get_immediate_context(void *self, ID3D11DeviceContext **out)
{
   (void)self;
   *out = (ID3D11DeviceContext *)(void *)&context;
}

static uint32_t expected_bytes;

static void NPT_STDMETHODCALLTYPE
fake_update_subresource(void *self, ID3D11Resource *dst, UINT sub,
                        const D3D11_BOX *box, const void *data, UINT row,
                        UINT depth)
{
   (void)self;
   (void)dst;
   updates++;
   seen_payload = data;
   seen_sub = sub;
   seen_row = row;
   seen_depth = depth;
   seen_box = box != NULL;
   /* Read every byte the guest sent: a payload cut short would be read past. */
   uint64_t sum = 0;
   for (uint32_t i = 0; i < expected_bytes; i++)
      sum += ((const uint8_t *)data)[i];
   seen_sum = sum;
}

static void
fakes_init(void)
{
   for (unsigned i = 0; i < 64; i++)
      resource_vtbl[i] = device_vtbl[i] = NULL;
   resource_vtbl[NPT_IUNKNOWN_VTBL_ADDREF] = (void *)fake_add_ref;
   resource_vtbl[NPT_IUNKNOWN_VTBL_RELEASE] = (void *)fake_release;
   resource_vtbl[NPT_VTBL_ID3D11DeviceChild_GetDevice] = (void *)fake_get_device;
   device_vtbl[NPT_IUNKNOWN_VTBL_RELEASE] = (void *)fake_release;
   device_vtbl[NPT_VTBL_ID3D11Device_GetImmediateContext] =
      (void *)fake_get_immediate_context;
   context_vtbl[NPT_IUNKNOWN_VTBL_RELEASE] = (void *)fake_release;
   context_vtbl[NPT_VTBL_ID3D11DeviceContext_UpdateSubresource] =
      (void *)fake_update_subresource;
   resource.vtbl = resource_vtbl;
   device.vtbl = device_vtbl;
   context.vtbl = context_vtbl;
}

#define RESOURCE_ID 0x5000u

/* One RESOURCE_UPDATE command as the guest's npt_dispatch_resource_update
 * writes it: the command, then byte_size bytes rounded up to 8, of which
 * `present` are in the stream. */
static uint8_t *
command(uint32_t byte_size, uint64_t present, size_t *out_size)
{
   const uint64_t aligned = ((uint64_t)byte_size + 7u) & ~(uint64_t)7u;
   const size_t size = sizeof(struct npt_cmd_resource_update) + present;
   uint8_t *buffer = calloc(1, size);
   if (!buffer)
      return NULL;
   struct npt_cmd_resource_update cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RESOURCE,
                             NPT_TRANSPORT_RESOURCE_UPDATE);
   cmd.header.object_id = RESOURCE_ID;
   cmd.header.cmd_size =
      (uint32_t)(sizeof(cmd) + (aligned <= UINT32_MAX ? aligned : 0));
   cmd.subresource = 3;
   cmd.row_pitch = 16384;
   cmd.depth_pitch = byte_size;
   cmd.byte_size = byte_size;
   memcpy(buffer, &cmd, sizeof(cmd));
   for (uint64_t i = 0; i < present && i < byte_size; i++)
      buffer[sizeof(cmd) + i] = (uint8_t)(i * 2654435761u >> 24);
   *out_size = size;
   return buffer;
}

static uint64_t
sum_of(const uint8_t *bytes, uint32_t count)
{
   uint64_t sum = 0;
   for (uint32_t i = 0; i < count; i++)
      sum += bytes[i];
   return sum;
}

static int failures;

#define CHECK(cond, what)                                                 \
   do {                                                                   \
      if (cond) {                                                         \
         printf("  ok    %s\n", what);                                    \
      } else {                                                            \
         printf("  FAIL  %s (%s:%d: %s)\n", what, __FILE__, __LINE__,     \
                #cond);                                                   \
         failures++;                                                      \
      }                                                                   \
   } while (0)

/* A fresh context per case: a refused command leaves its decoder fatal. */
static struct npt_context *
context_with_resource(void)
{
   struct npt_context *ctx = npt_context_create(1, NULL, 4, "test");
   if (ctx)
      npt_context_register_object(ctx, RESOURCE_ID, &resource,
                                  NPT_OBJECT_TYPE_ID3D11TEXTURE2D);
   return ctx;
}

static void
update_arrives_whole(uint32_t byte_size, const char *what)
{
   size_t size;
   const uint64_t aligned = ((uint64_t)byte_size + 7u) & ~(uint64_t)7u;
   uint8_t *buffer = command(byte_size, aligned, &size);
   struct npt_context *ctx = context_with_resource();
   if (!buffer || !ctx) {
      CHECK(0, "memory for the case");
      return;
   }
   updates = 0;
   expected_bytes = byte_size;
   const bool ok = npt_context_submit_cmd(ctx, buffer, size);
   char line[160];
   snprintf(line, sizeof(line), "%s: accepted", what);
   CHECK(ok, line);
   snprintf(line, sizeof(line), "%s: UpdateSubresource called once", what);
   CHECK(updates == 1, line);
   snprintf(line, sizeof(line),
            "%s: with the stream's own bytes (no copy), every byte as sent", what);
   CHECK(seen_payload == buffer + sizeof(struct npt_cmd_resource_update) &&
            seen_sum == sum_of(buffer + sizeof(struct npt_cmd_resource_update),
                               byte_size),
         line);
   snprintf(line, sizeof(line), "%s: subresource and pitches as sent", what);
   CHECK(seen_sub == 3 && seen_row == 16384 && seen_depth == byte_size &&
            !seen_box,
         line);
   npt_context_destroy(ctx);
   free(buffer);
}

static void
update_refused(uint32_t byte_size, uint64_t present, const char *what)
{
   size_t size;
   uint8_t *buffer = command(byte_size, present, &size);
   struct npt_context *ctx = context_with_resource();
   if (!buffer || !ctx) {
      CHECK(0, "memory for the case");
      return;
   }
   updates = 0;
   expected_bytes = 0;
   const bool ok = npt_context_submit_cmd(ctx, buffer, size);
   char line[160];
   snprintf(line, sizeof(line), "%s: refused, nothing called", what);
   CHECK(!ok && updates == 0, line);
   npt_context_destroy(ctx);
   free(buffer);
}

int
main(void)
{
   fakes_init();
   printf("RESOURCE_UPDATE (cap %u bytes)\n", NPT_MAX_RESOURCE_UPDATE_BYTES);
   update_arrives_whole(4096, "a small update");
   update_arrives_whole(64u << 20, "64 MiB, the old cap");
   update_arrives_whole(70744960u, "70,744,960 bytes, VRChat's upload");
   update_arrives_whole(256u << 20, "256 MiB");
   update_refused(70744960u, 70744960u - 4096u,
                  "70,744,960 bytes with 4 KiB missing from the stream");
   update_refused(NPT_MAX_RESOURCE_UPDATE_BYTES + 1u, 0,
                  "one byte over the cap");
   update_refused(UINT32_MAX, 0, "UINT32_MAX (byte_size + 7 would wrap)");
   printf(failures ? "%d FAILED\n" : "all passed\n", failures);
   return failures ? 1 : 0;
}
