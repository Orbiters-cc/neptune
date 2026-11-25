/*
 * SPDX-License-Identifier: MIT
 *
 * What the render server tells a local program of the same user about the shared textures of its guest: the messages of a
 * publishing socket and the layout of the memory they hand over.  Nothing here is the guest's: no wire format of Neptune's
 * changes.  virglrenderer/docs/shared-textures.md says how it is used; a reader needs this file alone.
 *
 * Little-endian, naturally aligned.  A reader skips message types it does not know, and what a payload holds after the
 * fields it knows.
 */

#ifndef NPT_PUBLISH_WIRE_H
#define NPT_PUBLISH_WIRE_H

#include <stdint.h>

#define NPT_PUBLISH_VERSION 1u

/* The messages, each after a header; the server speaks, the reader only reads. */
struct npt_publish_hdr {
   uint32_t type;
   uint32_t len; /* of the payload that follows */
};

enum {
   NPT_PUBLISH_HELLO = 1,   /* npt_publish_hello: once, first */
   NPT_PUBLISH_TEXTURE = 2, /* npt_publish_texture, with one descriptor (SCM_RIGHTS on the header's first byte): a live
                             * shared texture.  Those there are, after HELLO; then each new one */
   NPT_PUBLISH_GONE = 3,    /* npt_publish_gone: that texture is no more */
};

struct npt_publish_hello {
   uint32_t version;
   uint32_t server_pid; /* the render server's worker; the socket is "<server_pid>.sock" */
};

struct npt_publish_texture {
   uint32_t token;       /* the share handle the guest was given: what a Spout sender writes in its info block */
   uint32_t order;       /* 1, 2, ...: in the order this worker's textures were first asked for their handle */
   uint32_t width, height;
   uint32_t dxgi_format; /* DXGI_FORMAT_* */
   uint32_t stride;      /* bytes of a row */
   uint32_t slots;       /* NPT_PUBLISH_SLOTS */
   uint32_t reserved;
   uint64_t map_size;    /* of the descriptor's memory: mmap(PROT_READ, MAP_SHARED) at offset 0 */
   uint64_t created_ns;  /* CLOCK_UPTIME_RAW */
};

struct npt_publish_gone {
   uint32_t token;
   uint32_t reserved;
};

/* The memory: this header in the first 16 KiB, then the slots.  It is read-only for the reader: the descriptor it is
 * handed has no write access.
 *
 * A frame is the whole picture as it was at one Flush of the sender's, copied there by the GPU and complete.  To read
 * the newest one without ever taking a half-written one:
 *
 *   latest = load_acquire(h->latest);         none yet when 0
 *   i = latest & 0xff, n = latest >> 8;
 *   if load_acquire(h->slot[i].frame) != n: start again
 *   ... read the slot's pixels ...
 *   fence(acquire);
 *   if load_relaxed(h->slot[i].frame) != n: throw away what was read and start again
 *
 * A slot's `frame` is 0 from before the GPU is told to write it until its write is complete; the newest complete slot is
 * never the next one written, so a reader has at least the time of two of the sender's frames. */
#define NPT_PUBLISH_MAGIC 0x4654504eu /* 'NPTF' */
#define NPT_PUBLISH_SLOTS 4u
#define NPT_PUBLISH_HEADER_SIZE 16384u

struct npt_publish_slot {
   uint64_t frame;      /* atomic: the frame it holds whole, 0 while it is written and before */
   uint64_t written_ns; /* CLOCK_UPTIME_RAW when its frame was seen complete */
};

struct npt_publish_header {
   uint32_t magic, version;
   uint32_t token, width, height, dxgi_format, stride, slots;
   uint64_t slot_offset; /* of slot 0 */
   uint64_t slot_size;   /* slot i is at slot_offset + i * slot_size; its picture is its first stride * height bytes */
   uint64_t latest;      /* atomic: frame << 8 | slot of the newest whole frame, 0 for none */
   uint64_t frames;      /* atomic: the pictures the sender finished while somebody read, published or not */
   uint64_t skipped;     /* atomic: of those, the ones no slot was free for */
   uint32_t closed;      /* atomic: 1 once the texture is no more (GONE says so too) */
   uint32_t reserved;
   struct npt_publish_slot slot[NPT_PUBLISH_SLOTS];
};

#endif /* NPT_PUBLISH_WIRE_H */
