/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 shared / presentable textures over virtio-gpu blob resources.
 *
 * Venus-shaped model: the exporter's host texture is created shared by
 * the host D3D library and exported as an fd (a dmabuf under dxvk on
 * Linux, a POSIX shm fd under d3dmetal-native on macOS); EXPORT_BLOB
 * stages {fd, size} as this context's pending blob under a guest-chosen
 * blob_id, which the guest KMD claims via RESOURCE_CREATE_BLOB(HOST3D,
 * blob_id).  That binds a VM-global virtio res_id to the export.  A
 * consumer context receives the fd through the proxy's attach-
 * forwarding (guest KMD CTX_ATTACH_RESOURCE) and OPEN_RES rebuilds the
 * host library's shared-texture descriptor around it — zero-copy, the
 * imported texture aliases the exporter's memory.  No broker: the
 * canonical fd table is the VMM-side virgl resource table, keyed by
 * res_id.
 */

#ifndef NPT_SHARED_H
#define NPT_SHARED_H

#include <stdint.h>

#include "npt_com.h" /* HRESULT */
#include "virgl_resource.h"

/* fd type of a shared-texture export: what the host D3D library backs
 * shared allocations with on this platform. */
#ifdef __APPLE__
#define NPT_SHARED_FD_TYPE VIRGL_RESOURCE_FD_SHM
#else
#define NPT_SHARED_FD_TYPE VIRGL_RESOURCE_FD_DMABUF
#endif

#ifdef __APPLE__
/* The POD both darwin backends hand through GetSharedHandle /
 * OpenSharedResource, defined here so the renderer neither links nor
 * includes a backend header.  d3dmetal's dmn_shared_texture_handle and
 * dxmt's dxmt_shared_texture_handle share this exact ABI: same 'DMTX'
 * magic, version, and field layout. */
#define NPT_DARWIN_SHARED_TEXTURE_MAGIC   0x58544D44u /* 'DMTX' */
/* Every peer that builds or reads the POD declares the same version, and
 * an import refuses a handle whose version differs rather than read a
 * layout it does not have. */
#define NPT_DARWIN_SHARED_HANDLE_VERSION  2u
struct npt_darwin_shared_texture {
   uint32_t magic;
   uint32_t version;
   int32_t  fd;            /* process-local; sent via SCM_RIGHTS, then patched */
   uint32_t width, height;
   uint32_t dxgi_format;
   uint32_t mip_levels, array_size, sample_count;
   uint32_t bind_flags, misc_flags, cpu_access;
   uint64_t stride;        /* bytesPerRow */
   uint64_t size;          /* logical stride*height */
   uint64_t offset;        /* byte offset of the surface within fd (a
                            * surface placed in a shared heap is a window
                            * into the heap's one object); 0 otherwise */
};
#endif

struct npt_context;
struct npt_cmd_shared_open_res;

/* Exporter: export the shared host texture identified by
 * \p texture_id (a guest object id in \p ctx) as an fd, stage it as
 * a pending blob under \p blob_id, and write the npt_blob_export_info
 * into the shmem window at (data_res_id, data_off). */
HRESULT
npt_shared_export_blob(struct npt_context *ctx, uint64_t texture_id,
                       uint64_t blob_id, uint32_t data_res_id,
                       uint32_t data_off);

/* Consumer: import the texture backed by virtio resource cmd->res_id
 * on the device identified by \p device_id and register the imported
 * texture in \p ctx under cmd->mint_object_id.  Waits (bounded) for
 * the resource to be attached to this context. */
HRESULT
npt_shared_open_res(struct npt_context *ctx, uint64_t device_id,
                    const struct npt_cmd_shared_open_res *cmd);

#endif /* NPT_SHARED_H */
