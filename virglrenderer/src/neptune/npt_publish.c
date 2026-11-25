/*
 * SPDX-License-Identifier: MIT
 *
 * Shared textures answered with a token and published to local programs.
 * virglrenderer/docs/shared-textures.md says what for and how; npt_publish_wire.h is
 * what a reader sees.
 *
 * Why a token is safe where a HANDLE is not (npt_overrides_shared_handle.c
 * refuses those): the number the guest gets is made up here.  It is looked
 * up in this file's table when it comes back, and never dereferenced; no
 * host pointer leaves, and none is taken.
 *
 * The frames: the host library's shared texture is the guest's to write
 * at any time, so a local program is never handed its memory.  While
 * somebody reads, each picture the sender finished (a write into the
 * texture, then a Flush: Spout's senders flush after each, and Neptune's
 * swapchain at each Present) is copied by the GPU, on the sender's own
 * device and immediate context, into one of a few slots of a shared memory
 * of ours, behind a fence.  The reader maps that memory without write
 * access, and a sequence number says which slot holds which whole frame.
 * With no reader nothing is copied, counted, or held.
 *
 * Threads: the overrides run on the guest's ring threads; one thread of
 * this file's, started with the first token, serves the sockets and looks
 * at the fences.  It never calls into Direct3D but for a fence's value, and
 * the host library is never called with the table's lock held, since it
 * releases textures (and so calls unpublish, below) on threads of its own.
 */

#include "npt_publish.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "npt_com.h"
#include "npt_common.h"
#include "npt_context.h"
#include "npt_library.h"
#include "npt_overrides.h"
#include "npt_publish_wire.h"
#include "npt_shared.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11device.h"
#include "neptune-protocol/npt_protocol_host_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_host_idxgiresource.h"

_Atomic int npt_publish_wanted;

#ifndef __APPLE__

/* The descriptor behind a HANDLE is another on Linux; nothing asks for it
 * there. */
void
npt_publish_install(UNUSED struct npt_dispatch_context *d)
{
}

void
npt_publish_wrote(UNUSED void *resource)
{
}

#else

#define MAX_TEXTURES 16
#define MAX_READERS 8
#define MAX_SLOT_BYTES (128u << 20)

/* d3d11.h */
#define MISC_SHARED 0x2u
#define MISC_SHARED_KEYEDMUTEX 0x100u
#define MISC_SHARED_NTHANDLE 0x800u

/* The private data that calls back as a texture goes, and the one by which a
 * texture opened from a token is known for it. */
static const GUID guid_gone = { 0x6f1d1f0eu, 0x77a1, 0x4c0b, { 0x9d, 0x55, 0x0b, 0x1e, 0x6a, 0x2f, 0x33, 0x01 } };
static const GUID guid_token = { 0x6f1d1f0eu, 0x77a1, 0x4c0b, { 0x9d, 0x55, 0x0b, 0x1e, 0x6a, 0x2f, 0x33, 0x02 } };

typedef HRESULT(NPT_STDMETHODCALLTYPE *PFN_SetPrivateData)(void *, const GUID *, UINT, const void *);
typedef HRESULT(NPT_STDMETHODCALLTYPE *PFN_GetPrivateData)(void *, const GUID *, UINT *, void *);
/* ID3D11DeviceChild: GetDevice 3, GetPrivateData 4, SetPrivateData 5, SetPrivateDataInterface 6 */
#define VTBL_GetPrivateData 4
#define VTBL_SetPrivateData 5

/* The shared memory of one texture's frames, and the textures over its
 * slots on the sender's device. */
struct frames {
   int fd; /* for the readers: no write access */
   int rw;
   void *map;
   size_t size, slot_size;
   /* made by the sender's ring at the first picture */
   void *slot[NPT_PUBLISH_SLOTS];
   void *fence;
   uint64_t value;                        /* signalled last */
   uint64_t pending[NPT_PUBLISH_SLOTS];   /* the value whose frame the slot waits for, 0 */
   uint64_t frame_of[NPT_PUBLISH_SLOTS];  /* the frame the slot holds or waits for */
   bool failed;
};

struct texture {
   struct list_head head;
   uint32_t token, order;
   uint64_t created_ns;
   void *texture;          /* ID3D11Texture2D: no reference, valid until unpublished; never called through without one */
   void *context;          /* its device's immediate context, for identity alone */
   struct npt_darwin_shared_texture desc; /* its fd is ours */
   struct frames *frames;  /* while somebody reads */
   uint64_t count;         /* pictures since the first reader */
   bool dirty;             /* written since the last Flush: holds a reference on the texture */
   bool bound;             /* a render target of the context now */
   bool busy;              /* a ring copies it, with the lock dropped */
   bool drop_frames;       /* asked while busy */
};

struct gone {
   void **vtbl;
   _Atomic uint32_t refs;
   struct texture *t;
};

struct reader {
   int fd;
   pid_t pid;
};

static struct {
   mtx_t lock;
   bool enabled, started;
   char dir[sizeof(((struct sockaddr_un *)0)->sun_path) - 24];
   struct list_head textures; /* live */
   int count;
   uint32_t minted, orders;
   int wake[2];
   int listener;
   struct reader readers[MAX_READERS]; /* the thread's alone */
   int reader_count;
   _Atomic int dirty; /* textures that a Flush has to look at */
   uint32_t gone_tokens[64], new_tokens[64];
   int gone_count, new_count;
} pub = { .listener = -1 };

static void
wake_thread(void)
{
   const char c = 0;
   if (pub.started)
      (void)!write(pub.wake[1], &c, 1);
}

static uint64_t
now_ns(void)
{
   return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static size_t
page_round(size_t n)
{
   return (n + NPT_PUBLISH_HEADER_SIZE - 1) & ~(size_t)(NPT_PUBLISH_HEADER_SIZE - 1);
}

#define STORE(p, v) atomic_store_explicit((_Atomic __typeof__(*(p)) *)(p), (v), memory_order_release)
#define LOAD(p) atomic_load_explicit((_Atomic __typeof__(*(p)) *)(p), memory_order_acquire)

/* A texture the host library opened a moment ago, which nobody else knows: its one reference is ours. DXMT's OpenSharedResource
 * returns it with one more, which nobody has and so nobody drops (ImportSharedTexture: a ref() in a pointer that counts its
 * own): each texture opened stayed for good, with its memory and its descriptor. */
static void
alone(void *opened)
{
   npt_com_add_ref(opened);
   while (npt_com_release(opened) > 1)
      ;
}

/* ---------------------------------------------------------------- frames */

static struct frames *
frames_create(const struct texture *t)
{
   struct frames *f = calloc(1, sizeof(*f));
   if (!f)
      return NULL;
   f->fd = f->rw = -1;
   f->slot_size = page_round((size_t)t->desc.stride * t->desc.height);
   f->size = NPT_PUBLISH_HEADER_SIZE + NPT_PUBLISH_SLOTS * f->slot_size;
   for (int attempt = 0; attempt < 16 && f->rw < 0; attempt++) {
      char name[32];
      snprintf(name, sizeof(name), "/npt-%x-%08x", getpid(), arc4random());
      f->rw = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
      if (f->rw < 0)
         continue;
      /* opened a second time, for reading: what a reader gets cannot be mapped to be written */
      f->fd = shm_open(name, O_RDONLY, 0);
      shm_unlink(name);
   }
   if (f->rw < 0 || f->fd < 0 || ftruncate(f->rw, (off_t)f->size) != 0)
      goto fail;
   fcntl(f->rw, F_SETFD, FD_CLOEXEC);
   fcntl(f->fd, F_SETFD, FD_CLOEXEC);
   f->map = mmap(NULL, NPT_PUBLISH_HEADER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, f->rw, 0);
   if (f->map == MAP_FAILED)
      goto fail;
   struct npt_publish_header *h = f->map;
   *h = (struct npt_publish_header){
      .magic = NPT_PUBLISH_MAGIC,
      .version = NPT_PUBLISH_VERSION,
      .token = t->token,
      .width = t->desc.width,
      .height = t->desc.height,
      .dxgi_format = t->desc.dxgi_format,
      .stride = (uint32_t)t->desc.stride,
      .slots = NPT_PUBLISH_SLOTS,
      .slot_offset = NPT_PUBLISH_HEADER_SIZE,
      .slot_size = f->slot_size,
   };
   return f;
fail:
   npt_log("publish: token 0x%x: no memory for its frames (%s)", t->token, strerror(errno));
   if (f->fd >= 0)
      close(f->fd);
   if (f->rw >= 0)
      close(f->rw);
   free(f);
   return NULL;
}

/* With the lock dropped: releases into the host library. */
static void
frames_destroy(struct frames *f, bool gone)
{
   if (!f)
      return;
   STORE(&((struct npt_publish_header *)f->map)->closed, gone ? 1u : 0u);
   for (unsigned i = 0; i < NPT_PUBLISH_SLOTS; i++)
      if (f->slot[i])
         npt_com_release(f->slot[i]);
   if (f->fence)
      npt_com_release(f->fence);
   munmap(f->map, NPT_PUBLISH_HEADER_SIZE);
   close(f->fd);
   close(f->rw);
   free(f);
}

/* The slots as textures of the sender's device: the descriptor the host
 * library itself would have made, over our memory. */
static bool
frames_open(struct frames *f, struct texture *t)
{
   ID3D11Device *device = NULL;
   void *device5 = NULL;
   NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_GetDevice, npt_com_vtable(t->texture), NPT_VTBL_ID3D11DeviceChild_GetDevice)
   (t->texture, &device);
   if (!device)
      return false;
   bool ok = NPT_SUCCEEDED(npt_com_query_interface(device, &NPT_IID_ID3D11Device5, &device5)) && device5;
   if (ok)
      ok = NPT_SUCCEEDED(NPT_COM_VTBL_FUNC(PFN_ID3D11Device5_CreateFence, npt_com_vtable(device5),
                                           NPT_VTBL_ID3D11Device5_CreateFence)(device5, 0, 0, &NPT_IID_ID3D11Fence, &f->fence)) &&
           f->fence;
   for (unsigned i = 0; ok && i < NPT_PUBLISH_SLOTS; i++) {
      struct npt_darwin_shared_texture d = t->desc;
      d.fd = f->rw;
      d.offset = NPT_PUBLISH_HEADER_SIZE + i * f->slot_size;
      d.misc_flags = MISC_SHARED;
      d.cpu_access = 0;
      ok = NPT_SUCCEEDED(NPT_COM_VTBL_FUNC(PFN_ID3D11Device_OpenSharedResource, npt_com_vtable(device),
                                           NPT_VTBL_ID3D11Device_OpenSharedResource)(device, (HANDLE)(uintptr_t)&d,
                                                                                     &NPT_IID_ID3D11Texture2D, &f->slot[i])) &&
           f->slot[i];
      if (ok)
         alone(f->slot[i]);
   }
   if (device5)
      npt_com_release(device5);
   npt_com_release(device);
   if (!ok)
      npt_log("publish: token 0x%x: its frames cannot be copied on this device", t->token);
   return ok;
}

/* ---------------------------------------------------------------- the sender's writes */

/* With the lock. The caller is in a call that uses the texture, so it lives: the reference keeps it until the Flush that
 * takes the picture. */
static void
mark(struct texture *t)
{
   if (t->dirty)
      return;
   t->dirty = true;
   npt_com_add_ref(t->texture);
   atomic_fetch_add(&pub.dirty, 1);
}

void
npt_publish_wrote(void *resource)
{
   if (!resource || !atomic_load_explicit(&npt_publish_wanted, memory_order_relaxed))
      return;
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head)
      if (t->texture == resource)
         mark(t);
   mtx_unlock(&pub.lock);
}

static void
wrote_view(void *view)
{
   if (!view || !atomic_load_explicit(&npt_publish_wanted, memory_order_relaxed))
      return;
   ID3D11Resource *resource = NULL;
   NPT_COM_VTBL_FUNC(PFN_ID3D11View_GetResource, npt_com_vtable(view), NPT_VTBL_ID3D11View_GetResource)(view, &resource);
   if (!resource)
      return;
   npt_publish_wrote(resource);
   npt_com_release(resource);
}

/* The render targets of a context are these now: a published one is written until another call says otherwise. */
static void
bound_views(void *context, UINT n, ID3D11RenderTargetView **views)
{
   if (!atomic_load_explicit(&npt_publish_wanted, memory_order_relaxed))
      return;
   void *of[8] = { 0 };
   for (UINT i = 0; views && i < n && i < ARRAY_SIZE(of); i++) {
      if (!views[i])
         continue;
      NPT_COM_VTBL_FUNC(PFN_ID3D11View_GetResource, npt_com_vtable(views[i]), NPT_VTBL_ID3D11View_GetResource)
      (views[i], (ID3D11Resource **)&of[i]);
   }
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head) {
      bool is = false;
      for (unsigned i = 0; i < ARRAY_SIZE(of); i++)
         is |= of[i] && of[i] == t->texture;
      if (is) {
         t->bound = true;
         mark(t);
      } else if (t->context == context) {
         t->bound = false;
      }
   }
   mtx_unlock(&pub.lock);
   for (unsigned i = 0; i < ARRAY_SIZE(of); i++)
      if (of[i])
         npt_com_release(of[i]);
}

/* The picture of \p t, which the caller keeps alive, into a free slot: on the sender's immediate context, so after everything
 * the sender did to it. Returns whether the GPU was told to; \p t is busy and no lock is held. */
static bool
take(struct texture *t, struct frames *f, void *context, unsigned slot, uint64_t value)
{
   void *context4 = NULL;
   if (NPT_FAILED(npt_com_query_interface(context, &NPT_IID_ID3D11DeviceContext4, &context4)) || !context4)
      return false;
   NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_CopyResource, npt_com_vtable(context), NPT_VTBL_ID3D11DeviceContext_CopyResource)
   (context, f->slot[slot], t->texture);
   HRESULT hr = NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext4_Signal, npt_com_vtable(context4),
                                  NPT_VTBL_ID3D11DeviceContext4_Signal)(context4, f->fence, value);
   npt_com_release(context4);
   return NPT_SUCCEEDED(hr);
}

/* Before the sender's Flush (its Present ends with one): the pictures it finished since the last. */
static void
take_pictures(void *context)
{
   struct texture *todo[MAX_TEXTURES];
   int n = 0;

   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head) {
      if (!t->dirty || t->busy || t->context != context)
         continue;
      t->busy = true;
      t->dirty = false; /* its reference is ours now */
      atomic_fetch_sub(&pub.dirty, 1);
      todo[n++] = t;
   }
   mtx_unlock(&pub.lock);

   for (int i = 0; i < n; i++) {
      struct texture *t = todo[i];
      void *texture = t->texture;
      mtx_lock(&pub.lock);
      struct frames *f = t->frames;
      const bool wanted = f && !f->failed && !t->drop_frames;
      mtx_unlock(&pub.lock);

      if (wanted && !f->fence && !frames_open(f, t)) {
         mtx_lock(&pub.lock);
         f->failed = true;
         mtx_unlock(&pub.lock);
      } else if (wanted) {
         struct npt_publish_header *h = f->map;
         int slot = -1;
         uint64_t value = 0, frame = 0;
         mtx_lock(&pub.lock);
         frame = ++t->count;
         STORE(&h->frames, frame);
         /* the oldest that is neither the newest whole one nor on its way */
         const uint64_t latest = LOAD(&h->latest);
         for (unsigned k = 0; k < NPT_PUBLISH_SLOTS; k++) {
            if (f->pending[k] || (latest && (latest & 0xff) == k))
               continue;
            if (slot < 0 || f->frame_of[k] < f->frame_of[slot])
               slot = (int)k;
         }
         if (slot >= 0) {
            value = ++f->value;
            STORE(&h->slot[slot].frame, (uint64_t)0);
            atomic_thread_fence(memory_order_seq_cst);
         } else {
            STORE(&h->skipped, h->skipped + 1);
         }
         mtx_unlock(&pub.lock);

         if (slot >= 0) {
            const bool told = take(t, f, context, (unsigned)slot, value);
            mtx_lock(&pub.lock);
            if (told) {
               f->pending[slot] = value;
               f->frame_of[slot] = frame;
               wake_thread();
            } else {
               f->failed = true;
               npt_log("publish: token 0x%x: a frame cannot be copied, no more are", t->token);
            }
            mtx_unlock(&pub.lock);
         }
      }

      struct frames *drop = NULL;
      bool keep = false;
      mtx_lock(&pub.lock);
      t->busy = false;
      if (t->drop_frames) {
         t->drop_frames = false;
         drop = t->frames;
         t->frames = NULL;
      }
      if (t->bound && t->frames && !t->dirty && atomic_load(&npt_publish_wanted)) {
         t->dirty = true; /* (the reference goes on) */
         atomic_fetch_add(&pub.dirty, 1);
         keep = true;
      }
      mtx_unlock(&pub.lock);
      frames_destroy(drop, false);
      /* the last reference, when the sender let go of it meanwhile: unpublish runs in here */
      if (!keep)
         npt_com_release(texture);
   }
}

/* ---------------------------------------------------------------- a texture's end */

static void
unpublish(struct texture *t)
{
   mtx_lock(&pub.lock);
   list_del(&t->head);
   pub.count--;
   struct frames *f = t->frames;
   t->frames = NULL;
   if (pub.gone_count < (int)ARRAY_SIZE(pub.gone_tokens))
      pub.gone_tokens[pub.gone_count++] = t->token;
   wake_thread();
   mtx_unlock(&pub.lock);
   npt_log("publish: token 0x%x is gone", t->token);
   frames_destroy(f, true);
   close(t->desc.fd);
   free(t);
}

static HRESULT NPT_STDMETHODCALLTYPE
gone_query_interface(void *self, const GUID *iid, void **out)
{
   if (!out)
      return (HRESULT)0x80004003; /* E_POINTER */
   *out = NULL;
   if (memcmp(iid, &NPT_IID_IUnknown, sizeof(*iid)))
      return (HRESULT)0x80004002; /* E_NOINTERFACE */
   atomic_fetch_add(&((struct gone *)self)->refs, 1);
   *out = self;
   return NPT_S_OK;
}

static uint32_t NPT_STDMETHODCALLTYPE
gone_add_ref(void *self)
{
   return atomic_fetch_add(&((struct gone *)self)->refs, 1) + 1;
}

/* The texture holds the last reference, and drops it as it is destroyed: on whatever thread, with no lock of ours. */
static uint32_t NPT_STDMETHODCALLTYPE
gone_release(void *self)
{
   struct gone *g = self;
   const uint32_t left = atomic_fetch_sub(&g->refs, 1) - 1;
   if (!left) {
      if (g->t)
         unpublish(g->t);
      free(g);
   }
   return left;
}

static void *gone_vtbl[] = { (void *)gone_query_interface, (void *)gone_add_ref, (void *)gone_release };

/* ---------------------------------------------------------------- the sockets */

static bool
send_message(int fd, uint32_t type, const void *payload, uint32_t len, int pass)
{
   uint8_t buf[512];
   struct npt_publish_hdr hdr = { type, len };
   assert(sizeof(hdr) + len <= sizeof(buf));
   memcpy(buf, &hdr, sizeof(hdr));
   memcpy(buf + sizeof(hdr), payload, len);
   struct iovec iov = { buf, sizeof(hdr) + len };
   union {
      struct cmsghdr align;
      char data[CMSG_SPACE(sizeof(int))];
   } control;
   struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
   if (pass >= 0) {
      memset(&control, 0, sizeof(control));
      msg.msg_control = control.data;
      msg.msg_controllen = sizeof(control.data);
      struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(c), &pass, sizeof(int));
   }
   /* a few small messages in all: a reader whose buffer they do not fit into does not read */
   return sendmsg(fd, &msg, MSG_DONTWAIT) == (ssize_t)iov.iov_len;
}

/* "<process>.sock" in the folder: one for each worker, so that none takes another's. */
static int
listen_here(void)
{
   struct sockaddr_un sa = { .sun_family = AF_UNIX };
   const int n = snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/%d.sock", pub.dir, (int)getpid());
   if (n <= 0 || (size_t)n >= sizeof(sa.sun_path))
      return -1;
   unlink(sa.sun_path);
   const int s = socket(AF_UNIX, SOCK_STREAM, 0);
   if (s < 0)
      return -1;
   if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) || chmod(sa.sun_path, 0600) || listen(s, 8)) {
      npt_log("publish: %s: %s", sa.sun_path, strerror(errno));
      close(s);
      return -1;
   }
   fcntl(s, F_SETFL, O_NONBLOCK);
   fcntl(s, F_SETFD, FD_CLOEXEC);
   return s;
}

static pid_t
peer_pid(int fd)
{
   pid_t pid = -1;
   socklen_t len = sizeof(pid);
   return getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) ? -1 : pid;
}

static void
reader_close(int i)
{
   npt_log("publish: reader %d left", (int)pub.readers[i].pid);
   close(pub.readers[i].fd);
   pub.readers[i] = pub.readers[--pub.reader_count];
   if (atomic_fetch_sub(&npt_publish_wanted, 1) != 1)
      return;
   /* nobody reads: what the frames took is given back, by the ring that is copying one now, or here */
   struct frames *drop[MAX_TEXTURES];
   int n = 0;
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head) {
      if (!t->frames)
         continue;
      if (t->busy) {
         t->drop_frames = true;
         continue;
      }
      drop[n++] = t->frames;
      t->frames = NULL;
   }
   mtx_unlock(&pub.lock);
   while (n)
      frames_destroy(drop[--n], false);
}

/* One texture's message to one reader. */
static bool
tell_texture(int fd, uint32_t token)
{
   struct npt_publish_texture m = { 0 };
   int pass = -1;
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head) {
      if (t->token != token)
         continue;
      t->drop_frames = false; /* (what the last reader left, and a ring had yet to drop, is this one's) */
      if (!t->frames)
         t->frames = frames_create(t);
      if (!t->frames)
         break;
      m = (struct npt_publish_texture){
         .token = t->token,
         .order = t->order,
         .width = t->desc.width,
         .height = t->desc.height,
         .dxgi_format = t->desc.dxgi_format,
         .stride = (uint32_t)t->desc.stride,
         .slots = NPT_PUBLISH_SLOTS,
         .map_size = t->frames->size,
         .created_ns = t->created_ns,
      };
      pass = fcntl(t->frames->fd, F_DUPFD_CLOEXEC, 0);
   }
   mtx_unlock(&pub.lock);
   if (pass < 0)
      return true;
   const bool ok = send_message(fd, NPT_PUBLISH_TEXTURE, &m, sizeof(m), pass);
   close(pass);
   return ok;
}

static void
accept_reader(void)
{
   const int fd = accept(pub.listener, NULL, NULL);
   if (fd < 0)
      return;
   uid_t uid;
   gid_t gid;
   const pid_t pid = peer_pid(fd);
   if (getpeereid(fd, &uid, &gid) || uid != geteuid() || pub.reader_count >= MAX_READERS) {
      npt_log("publish: refusing peer %d", (int)pid);
      close(fd);
      return;
   }
   const int one = 1;
   setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
   fcntl(fd, F_SETFL, O_NONBLOCK);
   fcntl(fd, F_SETFD, FD_CLOEXEC);
   npt_log("publish: reader %d", (int)pid);
   pub.readers[pub.reader_count++] = (struct reader){ .fd = fd, .pid = pid };

   uint32_t tokens[MAX_TEXTURES];
   int n = 0;
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head) {
      if (n == MAX_TEXTURES)
         break;
      tokens[n++] = t->token;
   }
   mtx_unlock(&pub.lock);
   const struct npt_publish_hello hello = { NPT_PUBLISH_VERSION, (uint32_t)getpid() };
   bool ok = send_message(fd, NPT_PUBLISH_HELLO, &hello, sizeof(hello), -1);
   for (int i = 0; ok && i < n; i++)
      ok = tell_texture(fd, tokens[i]);
   /* from here on the sender's writes are looked at */
   atomic_fetch_add(&npt_publish_wanted, 1);
   if (!ok)
      reader_close(pub.reader_count - 1);
}

/* The frames that have become whole. Returns whether any is still on its way. */
static bool
publish_whole(void)
{
   bool waiting = false;
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head) {
      struct frames *f = t->frames;
      if (!f || !f->fence)
         continue;
      uint64_t done = 0;
      bool asked = false;
      for (unsigned k = 0; k < NPT_PUBLISH_SLOTS; k++) {
         if (!f->pending[k])
            continue;
         if (!asked) {
            done = NPT_COM_VTBL_FUNC(PFN_ID3D11Fence_GetCompletedValue, npt_com_vtable(f->fence),
                                     NPT_VTBL_ID3D11Fence_GetCompletedValue)(f->fence);
            asked = true;
         }
         if (done == UINT64_MAX) { /* the device is lost: no frame comes any more */
            f->failed = true;
            f->pending[k] = 0;
            continue;
         }
         if (done < f->pending[k]) {
            waiting = true;
            continue;
         }
         f->pending[k] = 0;
         struct npt_publish_header *h = f->map;
         h->slot[k].written_ns = now_ns();
         STORE(&h->slot[k].frame, f->frame_of[k]);
         if (f->frame_of[k] > LOAD(&h->latest) >> 8)
            STORE(&h->latest, f->frame_of[k] << 8 | k);
      }
   }
   mtx_unlock(&pub.lock);
   return waiting;
}

static int
publish_thread(UNUSED void *arg)
{
   u_thread_setname("npt-publish");
   bool waiting = false;
   while (true) {
      struct pollfd fds[2 + MAX_READERS];
      fds[0] = (struct pollfd){ .fd = pub.wake[0], .events = POLLIN };
      fds[1] = (struct pollfd){ .fd = pub.listener, .events = POLLIN };
      const int readers = pub.reader_count;
      for (int i = 0; i < readers; i++)
         fds[2 + i] = (struct pollfd){ .fd = pub.readers[i].fd, .events = POLLIN };
      const int polled = poll(fds, (nfds_t)(2 + readers), waiting ? 1 : -1);
      if (polled < 0) {
         if (errno == EINTR || errno == EAGAIN)
            continue;
         npt_log("publish: poll: %s; nothing is published any more", strerror(errno));
         return 0;
      }
      waiting = publish_whole();

      /* a reader says nothing: its end, and anything it sends, end it */
      for (int i = readers - 1; i >= 0; i--)
         if (fds[2 + i].revents)
            reader_close(i);

      if (fds[0].revents) {
         char buf[64];
         while (read(pub.wake[0], buf, sizeof(buf)) > 0)
            ;
         uint32_t gone[ARRAY_SIZE(pub.gone_tokens)], made[ARRAY_SIZE(pub.new_tokens)];
         mtx_lock(&pub.lock);
         const int gone_count = pub.gone_count, new_count = pub.new_count;
         memcpy(gone, pub.gone_tokens, sizeof(gone));
         memcpy(made, pub.new_tokens, sizeof(made));
         pub.gone_count = pub.new_count = 0;
         mtx_unlock(&pub.lock);
         /* (a texture made and gone meanwhile is told as gone alone, which a reader ignores) */
         for (int i = pub.reader_count - 1; i >= 0; i--) {
            bool ok = true;
            for (int k = 0; ok && k < new_count; k++)
               ok = tell_texture(pub.readers[i].fd, made[k]);
            for (int k = 0; ok && k < gone_count; k++) {
               const struct npt_publish_gone m = { .token = gone[k] };
               ok = send_message(pub.readers[i].fd, NPT_PUBLISH_GONE, &m, sizeof(m), -1);
            }
            if (!ok) {
               npt_log("publish: reader %d does not read, dropping it", (int)pub.readers[i].pid);
               reader_close(i);
            }
         }
      }
      if (fds[1].revents)
         accept_reader();
   }
   return 0;
}

/* With the lock, at the first token. */
static bool
start_thread(void)
{
   if (pub.started)
      return true;
   if (pipe(pub.wake))
      return false;
   for (int i = 0; i < 2; i++) {
      fcntl(pub.wake[i], F_SETFL, O_NONBLOCK);
      fcntl(pub.wake[i], F_SETFD, FD_CLOEXEC);
   }
   pub.listener = listen_here();
   thrd_t thread;
   if (pub.listener < 0 || thrd_create(&thread, publish_thread, NULL) != thrd_success) {
      npt_log("publish: no socket to publish at");
      for (int i = 0; i < 2; i++)
         close(pub.wake[i]);
      if (pub.listener >= 0)
         close(pub.listener);
      pub.listener = -1;
      return false;
   }
   thrd_detach(thread);
   pub.started = true;
   npt_log("publish: shared textures at %s/%d.sock", pub.dir, (int)getpid());
   return true;
}

/* ---------------------------------------------------------------- tokens */

/* As Windows' own global handles look: bit 30, then this process (17 bits, so that one of an earlier worker's is never
 * another texture's) and a count (11), the low two bits 2. */
#define TOKEN_FLAG 0x40000000u
#define IS_TOKEN(h) (((h) & ~(uint64_t)0x3fffffffu) == TOKEN_FLAG && ((h) & 3) == 2)

static uint32_t
mint_locked(void)
{
   while (true) {
      const uint32_t token = TOKEN_FLAG | ((uint32_t)getpid() & 0x1ffffu) << 13 | (++pub.minted & 0x7ffu) << 2 | 2u;
      bool used = false;
      list_for_each_entry (struct texture, t, &pub.textures, head)
         used |= t->token == token;
      if (!used)
         return token;
   }
}

/* Plain colour formats that a program outside reads as rows of pixels. */
static bool
format_published(uint32_t f)
{
   switch (f) {
   case 2:            /* R32G32B32A32_FLOAT */
   case 10: case 11:  /* R16G16B16A16_FLOAT, _UNORM */
   case 24:           /* R10G10B10A2_UNORM */
   case 27: case 28: case 29: /* R8G8B8A8: typeless, _UNORM, _UNORM_SRGB */
   case 87: case 88:  /* B8G8R8A8_UNORM, B8G8R8X8_UNORM */
   case 90: case 91:  /* B8G8R8A8: typeless, _UNORM_SRGB */
   case 92: case 93:  /* B8G8R8X8: typeless, _UNORM_SRGB */
      return true;
   default:
      return false;
   }
}

static void *
identity(void *object)
{
   void *unknown = NULL;
   if (!object || NPT_FAILED(npt_com_query_interface(object, &NPT_IID_IUnknown, &unknown)) || !unknown)
      return NULL;
   npt_com_release(unknown);
   return unknown;
}

/* A token for the host texture behind \p resource (an IDXGIResource), or 0: then the call fails as it does without this
 * feature. \p why says what for the log, when it is worth a line. */
static uint32_t
publish(void *resource, PFN_IDXGIResource_GetSharedHandle get_shared, const char **why)
{
   uint32_t token = 0;
   void *texture = NULL;
   *why = NULL;
   if (NPT_FAILED(npt_com_query_interface(resource, &NPT_IID_ID3D11Texture2D, &texture)) || !texture)
      return 0;

   /* the one it has, or the one it was opened from */
   UINT size = sizeof(token);
   if (NPT_SUCCEEDED(NPT_COM_VTBL_FUNC(PFN_GetPrivateData, npt_com_vtable(texture), VTBL_GetPrivateData)(
          texture, &guid_token, &size, &token)) &&
       size == sizeof(token) && token)
      goto out;
   token = 0;

   HANDLE handle = 0;
   HRESULT hr = get_shared(resource, &handle);
   if (NPT_FAILED(hr) || !handle)
      goto out; /* not a shared texture, or one the host library made unshared */
   const struct npt_darwin_shared_texture *desc = (const struct npt_darwin_shared_texture *)(uintptr_t)handle;
   if (desc->magic != NPT_DARWIN_SHARED_TEXTURE_MAGIC || desc->version != NPT_DARWIN_SHARED_HANDLE_VERSION || desc->fd < 0)
      goto out;
   /* A keyed mutex is not shared by a token, so a texture made with one keeps the answer it had (a video player's own device,
    * for one); an NT handle never had this one. */
   if ((desc->misc_flags & (MISC_SHARED | MISC_SHARED_KEYEDMUTEX | MISC_SHARED_NTHANDLE)) != MISC_SHARED) {
      *why = "it has a keyed mutex";
      goto out;
   }
   if (!format_published(desc->dxgi_format) || desc->mip_levels != 1 || desc->array_size != 1 || desc->sample_count > 1 ||
       !desc->stride || !desc->height || desc->stride > MAX_SLOT_BYTES / desc->height) {
      *why = "its format or size is none that is published";
      goto out;
   }

   ID3D11Device *device = NULL;
   ID3D11DeviceContext *context = NULL;
   NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_GetDevice, npt_com_vtable(texture), NPT_VTBL_ID3D11DeviceChild_GetDevice)
   (texture, &device);
   if (!device)
      goto out;
   NPT_COM_VTBL_FUNC(PFN_ID3D11Device_GetImmediateContext, npt_com_vtable(device), NPT_VTBL_ID3D11Device_GetImmediateContext)
   (device, &context);
   void *context_id = identity(context);
   if (context)
      npt_com_release(context);
   npt_com_release(device);

   struct texture *t = calloc(1, sizeof(*t));
   struct gone *g = calloc(1, sizeof(*g));
   if (!t || !g || !context_id) {
      free(t);
      free(g);
      goto out;
   }
   t->desc = *desc;
   t->desc.fd = fcntl(desc->fd, F_DUPFD_CLOEXEC, 0);
   t->texture = texture;
   t->context = context_id;
   t->created_ns = now_ns();
   *g = (struct gone){ .vtbl = gone_vtbl, .refs = 1, .t = NULL };

   mtx_lock(&pub.lock);
   if (t->desc.fd >= 0 && pub.count < MAX_TEXTURES && start_thread()) {
      token = t->token = mint_locked();
      t->order = ++pub.orders;
   }
   mtx_unlock(&pub.lock);

   /* the texture tells its end from now on: g->t is set once the table has it */
   if (token &&
       (NPT_FAILED(NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_SetPrivateDataInterface, npt_com_vtable(texture),
                                     NPT_VTBL_ID3D11DeviceChild_SetPrivateDataInterface)(texture, &guid_gone,
                                                                                         (const IUnknown *)g)) ||
        NPT_FAILED(NPT_COM_VTBL_FUNC(PFN_SetPrivateData, npt_com_vtable(texture), VTBL_SetPrivateData)(
           texture, &guid_token, sizeof(token), &token))))
      token = 0;
   if (token) {
      mtx_lock(&pub.lock);
      list_addtail(&t->head, &pub.textures);
      pub.count++;
      g->t = t;
      if (pub.new_count < (int)ARRAY_SIZE(pub.new_tokens))
         pub.new_tokens[pub.new_count++] = token;
      wake_thread();
      mtx_unlock(&pub.lock);
      npt_log("publish: token 0x%x: %ux%u format %u stride %" PRIu64 " (number %u)", token, t->desc.width, t->desc.height,
              t->desc.dxgi_format, t->desc.stride, t->order);
   } else {
      *why = "there is no room for it";
      if (t->desc.fd >= 0)
         close(t->desc.fd);
      free(t);
   }
   gone_release(g); /* the texture's own reference stays */
out:
   npt_com_release(texture);
   return token;
}

/* ---------------------------------------------------------------- the overrides */

static HRESULT
publish_IDXGIResource_GetSharedHandle(UNUSED struct npt_dispatch_context *ctx,
                                      struct npt_command_IDXGIResource_GetSharedHandle *args,
                                      PFN_IDXGIResource_GetSharedHandle original)
{
   const char *why = NULL;
   const uint32_t token = args->pSharedHandle ? publish(args->_self, original, &why) : 0;
   if (why)
      npt_log("publish: a shared handle is refused: %s", why);
   if (args->pSharedHandle)
      *args->pSharedHandle = (HANDLE)token;
   args->ret = token ? NPT_S_OK : NPT_E_INVALIDARG;
   return args->ret;
}

static HRESULT
publish_ID3D11Device_OpenSharedResource(UNUSED struct npt_dispatch_context *ctx,
                                        struct npt_command_ID3D11Device_OpenSharedResource *args,
                                        PFN_ID3D11Device_OpenSharedResource original)
{
   if (args->ppResource)
      *args->ppResource = NULL;
   args->ret = NPT_E_INVALIDARG;
   if (!args->ppResource || !IS_TOKEN(args->hResource))
      return args->ret;

   const uint32_t token = (uint32_t)args->hResource;
   struct npt_darwin_shared_texture desc = { .fd = -1 };
   mtx_lock(&pub.lock);
   list_for_each_entry (struct texture, t, &pub.textures, head)
      if (t->token == token) {
         desc = t->desc;
         desc.fd = fcntl(t->desc.fd, F_DUPFD_CLOEXEC, 0);
      }
   mtx_unlock(&pub.lock);
   if (desc.fd < 0) {
      npt_log("publish: token 0x%x is no live texture's", token);
      return args->ret;
   }
   args->ret = original(args->_self, (HANDLE)(uintptr_t)&desc, args->ReturnedInterface, args->ppResource);
   close(desc.fd); /* the host library has its own */
   if (NPT_FAILED(args->ret) || !*args->ppResource)
      return args->ret;
   alone(*args->ppResource);
   void *texture = NULL;
   if (NPT_SUCCEEDED(npt_com_query_interface(*args->ppResource, &NPT_IID_ID3D11Texture2D, &texture)) && texture) {
      NPT_COM_VTBL_FUNC(PFN_SetPrivateData, npt_com_vtable(texture), VTBL_SetPrivateData)
      (texture, &guid_token, sizeof(token), &token);
      npt_com_release(texture);
   }
   npt_log("publish: token 0x%x opened", token);
   return args->ret;
}

/* The writes: each is done first, as the guest asked. */

static void
publish_DC_CopyResource(UNUSED struct npt_dispatch_context *ctx, struct npt_command_ID3D11DeviceContext_CopyResource *args,
                        PFN_ID3D11DeviceContext_CopyResource original)
{
   original(args->_self, args->pDstResource, args->pSrcResource);
   npt_publish_wrote(args->pDstResource);
}

static void
publish_DC_CopySubresourceRegion(UNUSED struct npt_dispatch_context *ctx,
                                 struct npt_command_ID3D11DeviceContext_CopySubresourceRegion *args,
                                 PFN_ID3D11DeviceContext_CopySubresourceRegion original)
{
   original(args->_self, args->pDstResource, args->DstSubresource, args->DstX, args->DstY, args->DstZ, args->pSrcResource,
            args->SrcSubresource, args->pSrcBox);
   npt_publish_wrote(args->pDstResource);
}

static void
publish_DC1_CopySubresourceRegion1(UNUSED struct npt_dispatch_context *ctx,
                                   struct npt_command_ID3D11DeviceContext1_CopySubresourceRegion1 *args,
                                   PFN_ID3D11DeviceContext1_CopySubresourceRegion1 original)
{
   original(args->_self, args->pDstResource, args->DstSubresource, args->DstX, args->DstY, args->DstZ, args->pSrcResource,
            args->SrcSubresource, args->pSrcBox, args->CopyFlags);
   npt_publish_wrote(args->pDstResource);
}

static void
publish_DC_ResolveSubresource(UNUSED struct npt_dispatch_context *ctx,
                              struct npt_command_ID3D11DeviceContext_ResolveSubresource *args,
                              PFN_ID3D11DeviceContext_ResolveSubresource original)
{
   original(args->_self, args->pDstResource, args->DstSubresource, args->pSrcResource, args->SrcSubresource, args->Format);
   npt_publish_wrote(args->pDstResource);
}

static void
publish_DC_ClearRenderTargetView(UNUSED struct npt_dispatch_context *ctx,
                                 struct npt_command_ID3D11DeviceContext_ClearRenderTargetView *args,
                                 PFN_ID3D11DeviceContext_ClearRenderTargetView original)
{
   original(args->_self, args->pRenderTargetView, args->ColorRGBA);
   wrote_view(args->pRenderTargetView);
}

static void
publish_DC_OMSetRenderTargets(UNUSED struct npt_dispatch_context *ctx,
                              struct npt_command_ID3D11DeviceContext_OMSetRenderTargets *args,
                              PFN_ID3D11DeviceContext_OMSetRenderTargets original)
{
   original(args->_self, args->NumViews, args->ppRenderTargetViews, args->pDepthStencilView);
   if (atomic_load_explicit(&npt_publish_wanted, memory_order_relaxed))
      bound_views(identity(args->_self), args->NumViews, args->ppRenderTargetViews);
}

static void
publish_DC_OMSetRenderTargetsAndUnorderedAccessViews(
   UNUSED struct npt_dispatch_context *ctx,
   struct npt_command_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews *args,
   PFN_ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews original)
{
   original(args->_self, args->NumRTVs, args->ppRenderTargetViews, args->pDepthStencilView, args->UAVStartSlot, args->NumUAVs,
            args->ppUnorderedAccessViews, args->pUAVInitialCounts);
   if (args->NumRTVs != 0xffffffffu /* D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL */ &&
       atomic_load_explicit(&npt_publish_wanted, memory_order_relaxed))
      bound_views(identity(args->_self), args->NumRTVs, args->ppRenderTargetViews);
}

static void
publish_DC_Flush(UNUSED struct npt_dispatch_context *ctx, struct npt_command_ID3D11DeviceContext_Flush *args,
                 PFN_ID3D11DeviceContext_Flush original)
{
   if (atomic_load_explicit(&pub.dirty, memory_order_relaxed))
      take_pictures(identity(args->_self));
   original(args->_self);
}

static void
publish_DC3_Flush1(UNUSED struct npt_dispatch_context *ctx, struct npt_command_ID3D11DeviceContext3_Flush1 *args,
                   PFN_ID3D11DeviceContext3_Flush1 original)
{
   if (atomic_load_explicit(&pub.dirty, memory_order_relaxed))
      take_pictures(identity(args->_self));
   original(args->_self, args->ContextType, args->hEvent);
}

static struct npt_dispatch_idxgiresource_overrides publish_idxgiresource_overrides;
static struct npt_dispatch_id3d11device_overrides publish_id3d11device_overrides;
static struct npt_dispatch_id3d11devicecontext_overrides publish_dc_overrides;
static struct npt_dispatch_id3d11devicecontext1_overrides publish_dc1_overrides;
static struct npt_dispatch_id3d11devicecontext3_overrides publish_dc3_overrides;

static void
publish_init(void)
{
   const char *dir = getenv("NPT_SHARED_PUBLISH");
   struct stat st;
   if (!dir || !*dir)
      return;
   if (strlen(dir) >= sizeof(pub.dir) || lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
       (st.st_mode & 077)) {
      npt_log("publish: off: NPT_SHARED_PUBLISH is no folder of this user's alone");
      return;
   }
   if (npt_renderer_get_library()->backend != NPT_BACKEND_DXMT) {
      npt_log("publish: off: it is DXMT's shared textures that are published");
      return;
   }
   strcpy(pub.dir, dir);
   mtx_init(&pub.lock, mtx_plain);
   list_inithead(&pub.textures);

   publish_idxgiresource_overrides = npt_idxgiresource_overrides;
   publish_idxgiresource_overrides.GetSharedHandle = publish_IDXGIResource_GetSharedHandle;
   publish_id3d11device_overrides = npt_id3d11device_overrides;
   publish_id3d11device_overrides.OpenSharedResource = publish_ID3D11Device_OpenSharedResource;
   publish_dc_overrides = npt_query_dc_overrides;
   publish_dc_overrides.CopyResource = publish_DC_CopyResource;
   publish_dc_overrides.CopySubresourceRegion = publish_DC_CopySubresourceRegion;
   publish_dc_overrides.ResolveSubresource = publish_DC_ResolveSubresource;
   publish_dc_overrides.ClearRenderTargetView = publish_DC_ClearRenderTargetView;
   publish_dc_overrides.OMSetRenderTargets = publish_DC_OMSetRenderTargets;
   publish_dc_overrides.OMSetRenderTargetsAndUnorderedAccessViews = publish_DC_OMSetRenderTargetsAndUnorderedAccessViews;
   publish_dc_overrides.Flush = publish_DC_Flush;
   publish_dc1_overrides.CopySubresourceRegion1 = publish_DC1_CopySubresourceRegion1;
   publish_dc3_overrides.Flush1 = publish_DC3_Flush1;
   pub.enabled = true;
   npt_log("publish: shared textures are answered with tokens");
}

static pthread_once_t publish_once = PTHREAD_ONCE_INIT;

void
npt_publish_install(struct npt_dispatch_context *d)
{
   pthread_once(&publish_once, publish_init);
   if (!pub.enabled)
      return;
   d->idxgiresource_dispatch_overrides = &publish_idxgiresource_overrides;
   d->id3d11device_dispatch_overrides = &publish_id3d11device_overrides;
   d->id3d11devicecontext_dispatch_overrides = &publish_dc_overrides;
   d->id3d11devicecontext1_dispatch_overrides = &publish_dc1_overrides;
   d->id3d11devicecontext3_dispatch_overrides = &publish_dc3_overrides;
}

#endif /* __APPLE__ */
