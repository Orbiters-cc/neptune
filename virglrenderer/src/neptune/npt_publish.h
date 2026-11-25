/*
 * SPDX-License-Identifier: MIT
 *
 * Shared textures answered with a token and published to local programs (docs/shared-textures.md).
 *
 * Off unless NPT_SHARED_PUBLISH names a private folder, which the launcher gives for the games that ask for it: then
 * nothing of this file runs, and the dispatch tables are the ones that refuse every shared HANDLE
 * (npt_overrides_shared_handle.c).
 */

#ifndef NPT_PUBLISH_H
#define NPT_PUBLISH_H

#include <stdatomic.h>
#include <stdbool.h>

struct npt_dispatch_context;

/* Installs the overrides over those \p d has, when the feature is on.  Once per context, before anything is dispatched. */
void
npt_publish_install(struct npt_dispatch_context *d);

/* How many readers want frames: zero without the feature, without a shared texture and without a reader. */
extern _Atomic int npt_publish_wanted;

/* The guest wrote \p resource (a host ID3D11Resource) outside of the dispatch: a RESOURCE_UPDATE. */
void
npt_publish_wrote(void *resource);

#endif /* NPT_PUBLISH_H */
