/* SPDX-License-Identifier: MIT
 * A native reader of the published shared textures, as a program outside the engine is one (npt_publish_wire.h alone):
 *   spout_reader FOLDER SECONDS [check] [idle]
 * Finds the workers' sockets in FOLDER, follows every texture told, and reads its newest whole frames as fast as it can. check: every
 * pixel of a frame must be spout_sender.c's colour of one picture, and the pictures' numbers only grow. idle: connects, maps, reads
 * nothing of the sockets afterwards. One line a texture as it goes or at the end; exits 1 when anything was wrong. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "npt_publish_wire.h"

struct tex {
   struct npt_publish_texture m;
   const struct npt_publish_header *h;
   int fd;
   uint64_t last, first_number, last_number, frames, torn, retried, wrong, backwards, count_off;
   double copy_ms;
   bool gone;
};
static struct tex tex[256];
static int tex_count, failures;
static struct { int fd; char path[256]; bool ended; } conn[64];
static int conn_count;
static uint8_t *copy;

static double now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
#define LOAD(p) atomic_load_explicit((const _Atomic __typeof__(*(p)) *)(p), memory_order_acquire)

static void fail(const char *what) { printf("READER FAIL %s\n", what); failures++; }

static void report(struct tex *t)
{
   printf("READER token 0x%x number %u %ux%u format %u stride %u: %llu frames read whole (pictures %llu to %llu), %llu retried, %llu torn, "
          "%llu wrong, %llu backwards; the sender finished %llu, %llu had no slot; a copy %.2f ms\n",
          t->m.token, t->m.order, t->m.width, t->m.height, t->m.dxgi_format, t->m.stride, (unsigned long long)t->frames,
          (unsigned long long)t->first_number, (unsigned long long)t->last_number, (unsigned long long)t->retried,
          (unsigned long long)t->torn, (unsigned long long)t->wrong, (unsigned long long)t->backwards,
          (unsigned long long)LOAD(&t->h->frames), (unsigned long long)LOAD(&t->h->skipped), t->frames ? t->copy_ms / t->frames : 0);
   if (t->torn || t->wrong || t->backwards)
      failures++;
}
static void add(const struct npt_publish_texture *m, int fd)
{
   struct stat st;
   struct tex *t = &tex[tex_count];
   if (fd < 0) { fail("a texture without a descriptor"); return; }
   if (fstat(fd, &st) || (uint64_t)st.st_size < m->map_size) { fail("memory smaller than told"); return; }
   void *writable = mmap(NULL, m->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (writable != MAP_FAILED) fail("the memory can be mapped to be written");
   void *p = mmap(NULL, m->map_size, PROT_READ, MAP_SHARED, fd, 0);
   if (p == MAP_FAILED) { fail("mmap"); return; }
   if (!mprotect(p, NPT_PUBLISH_HEADER_SIZE, PROT_READ | PROT_WRITE)) fail("the mapping can be made writable");
   *t = (struct tex){ .m = *m, .h = p, .fd = fd };
   if (t->h->magic != NPT_PUBLISH_MAGIC || t->h->version != NPT_PUBLISH_VERSION || t->h->token != m->token || t->h->width != m->width || t->h->height != m->height)
      fail("the header is not the texture's");
   printf("READER new token 0x%x number %u %ux%u\n", m->token, m->order, m->width, m->height);
   fflush(stdout);
   tex_count++;
}
static bool whole_message(int fd, struct npt_publish_hdr *h, uint8_t *body, int *passed)
{
   union { struct cmsghdr align; char data[CMSG_SPACE(sizeof(int))]; } control;
   struct iovec iov = { h, sizeof *h };
   struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control.data, .msg_controllen = sizeof control.data };
   *passed = -1;
   if (recvmsg(fd, &msg, MSG_WAITALL) != sizeof *h) return false;
   for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
      if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(passed, CMSG_DATA(c), sizeof(int));
   if (h->len > 4096) return false;
   return !h->len || recv(fd, body, h->len, MSG_WAITALL) == (ssize_t)h->len;
}
static void read_socket(int i)
{
   struct npt_publish_hdr h;
   uint8_t body[4096];
   int passed;
   if (!whole_message(conn[i].fd, &h, body, &passed)) { conn[i].ended = true; close(conn[i].fd); printf("READER a server ended\n"); return; }
   if (h.type == NPT_PUBLISH_TEXTURE) add((const void *)body, passed);
   else if (h.type == NPT_PUBLISH_GONE)
      for (int k = 0; k < tex_count; k++)
         if (tex[k].m.token == ((const struct npt_publish_gone *)body)->token && !tex[k].gone) { tex[k].gone = true; report(&tex[k]); munmap((void *)tex[k].h, tex[k].m.map_size); close(tex[k].fd); printf("READER gone 0x%x\n", tex[k].m.token); fflush(stdout); }
}
static void look(const char *folder)
{
   DIR *d = opendir(folder);
   struct dirent *e;
   while (d && (e = readdir(d))) {
      size_t n = strlen(e->d_name);
      char path[256];
      bool known = false;
      if (n < 6 || strcmp(e->d_name + n - 5, ".sock")) continue;
      snprintf(path, sizeof path, "%s/%s", folder, e->d_name);
      for (int i = 0; i < conn_count; i++) known |= !strcmp(conn[i].path, path);
      if (known || conn_count == 64) continue;
      struct stat st;
      if (lstat(path, &st) || (st.st_mode & 0777) != 0600) fail("a socket not this user's alone");
      struct sockaddr_un sa = { .sun_family = AF_UNIX };
      snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
      int s = socket(AF_UNIX, SOCK_STREAM, 0);
      if (connect(s, (void *)&sa, sizeof sa)) { close(s); continue; }
      conn[conn_count].fd = s;
      snprintf(conn[conn_count].path, sizeof conn[0].path, "%s", path);
      conn[conn_count++].ended = false;
   }
   if (d) closedir(d);
}
/* the newest whole frame, when there is a new one */
static void frame(struct tex *t, bool check)
{
   const uint64_t latest = LOAD(&t->h->latest);
   if (!latest || latest >> 8 == t->last) return;
   const unsigned slot = latest & 0xff;
   const uint64_t n = latest >> 8;
   const size_t bytes = (size_t)t->m.stride * t->m.height;
   if (LOAD(&t->h->slot[slot].frame) != n) { t->retried++; return; }
   const double t0 = now_ms();
   memcpy(copy, (const uint8_t *)t->h + t->h->slot_offset + slot * t->h->slot_size, bytes);
   atomic_thread_fence(memory_order_acquire);
   if (atomic_load_explicit((const _Atomic uint64_t *)&t->h->slot[slot].frame, memory_order_relaxed) != n) { t->retried++; return; }
   t->copy_ms += now_ms() - t0;
   t->last = n;
   t->frames++;
   if (!check) return;
   const uint32_t first = *(const uint32_t *)copy; /* BGRA */
   const uint32_t number = ((first >> 16) & 255) | ((first >> 8) & 255) << 8;
   bool uniform = true;
   for (uint32_t y = 0; uniform && y < t->m.height; y++) {
      const uint32_t *row = (const uint32_t *)(copy + (size_t)y * t->m.stride);
      for (uint32_t x = 0; x < t->m.width; x++) if (row[x] != first) { uniform = false; break; }
   }
   if (!uniform) t->torn++;
   else if ((first & 255) != ((number * 7) & 255) || first >> 24 != 255) t->wrong++;
   else {
      if (t->frames > 1 && number <= t->last_number) t->backwards++;
      if (!t->first_number) t->first_number = number;
      t->last_number = number;
   }
}
int main(int argc, char **argv)
{
   if (argc < 3) return 2;
   bool check = false, idle = false;
   for (int i = 3; i < argc; i++) { check |= !strcmp(argv[i], "check"); idle |= !strcmp(argv[i], "idle"); }
   copy = malloc(256u << 20);
   const double end = now_ms() + atof(argv[2]) * 1000;
   double seen = 0;
   while (now_ms() < end) {
      if (now_ms() - seen > 100) { look(argv[1]); seen = now_ms(); }
      struct pollfd p[64];
      int n = 0, at[64];
      for (int i = 0; i < conn_count; i++) if (!conn[i].ended) { at[n] = i; p[n++] = (struct pollfd){ .fd = conn[i].fd, .events = POLLIN }; }
      if (n ? poll(p, n, 1) > 0 : (usleep(5000), 0))
         for (int i = 0; i < n; i++) if (p[i].revents && !(idle && tex_count)) read_socket(at[i]);
      for (int k = 0; k < tex_count; k++) if (!tex[k].gone) frame(&tex[k], check);
   }
   for (int k = 0; k < tex_count; k++) if (!tex[k].gone) report(&tex[k]);
   printf("READER %d textures, %d failures\n", tex_count, failures);
   return failures ? 1 : 0;
}
