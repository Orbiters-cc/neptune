# Shared textures, answered with tokens and published to local programs

A Windows program that shares a Direct3D 11 texture with another program (a Spout sender, as VRChat's stream camera is one) makes it with
`D3D11_RESOURCE_MISC_SHARED`, asks `IDXGIResource::GetSharedHandle` for its legacy handle, and gives that number to the receiver, which
calls `ID3D11Device::OpenSharedResource`. Neptune's host refuses both by default (`src/neptune/npt_overrides_shared_handle.c` says why).

With **`NPT_SHARED_PUBLISH=<folder>`** in the render server's environment (`src/neptune/npt_publish.c`):

- `GetSharedHandle` answers with a **token**: a made-up number, shaped as Windows' own global handles (bit 30, the worker's process, a
  count, low bits `10`). It is looked up in a table when it comes back, never dereferenced. Only for a plain `MISC_SHARED` 2D texture of one
  level in a colour format that is read as rows of pixels (RGBA8 and BGRA8 with their typeless, sRGB and X8 forms, RGB10A2, RGBA16 float and
  unorm, RGBA32 float), at most 16 live ones of at most 128 MiB. Any other gets the answer it had (`E_INVALIDARG`, handle 0): one with a
  **keyed mutex** among them, so a video player that shares that way keeps its fallback.
- `OpenSharedResource` opens a live token, on any device of the same process of the game, over the same memory. Anything else is refused as
  ever. A receiver in **another** process of the game's session cannot run beside the sender: the launcher's `virgl_test_server --no-fork`
  serves one process's Direct3D at a time (released behaviour: a second process waits for the first one's end).
- each live shared texture is **published** on a small socket, for a program of the same user outside the engine.

Without the variable, or when the folder is not one of this user's alone (mode 0700, no link), or with another backend than DXMT, nothing
of this runs: the dispatch tables are the released ones, untouched. Tests: `tests/neptune/publish/`.

## The socket

`<folder>/<process>.sock`, one for each worker of the render server (one for each process of the game that uses Direct3D; in practice one at
a time), made with that worker's first token and 0600. A worker that was killed leaves its file behind: connecting to it is refused. The
launcher gives the folder `<temporary folder>/obelisk-engine-<launcher's process>.host/shared`, which the game does not see, where the
launcher's process is the Game session record's `game.pid`. A program looks in the folder for sockets now and then (no texture: no socket).

The server speaks, the reader only reads: a reader that sends anything is disconnected, one that does not read is dropped, and neither
the game nor another reader waits for one. At most 8 readers. Messages and memory are `src/neptune/npt_publish_wire.h`, which is all a reader
needs: `HELLO`, then a `TEXTURE` with a descriptor for each live texture and for each new one, and a `GONE` as one ends.

| Field of `TEXTURE` | |
| --- | --- |
| `token` | the handle the guest was given: what a Spout sender writes into its info block |
| `order` | 1, 2, ...: in the order this worker's textures asked for their handle |
| `width`, `height`, `dxgi_format`, `stride` | of every frame; rows from the top |
| `slots`, `map_size` | of the memory behind the descriptor |

The descriptor is shared memory opened **for reading alone**: mapping it to be written, making the mapping writable and opening it again
are refused by macOS. `fstat` it, map `map_size` bytes `PROT_READ`.

## The frames

A reader never gets the game's own texture, which the game may write at any time. While somebody reads, each picture the sender finished
is copied by the GPU, on the sender's own device and immediate context, into one of four slots, behind a fence. "Finished" is a write into
the texture (CopyResource, CopySubresourceRegion(1), ResolveSubresource, UpdateSubresource, ClearRenderTargetView, or being a render target)
followed by a Flush on that device, which a Spout sender does after each frame and Neptune's swapchain at each Present. (Commands recorded
on a deferred context count when they are recorded: such a picture is the one of a Flush before.)

What a reader is promised, following the rule in `npt_publish_wire.h` (a sequence lock): **a whole picture, never half of one**, the newest one
the GPU has finished, about a millisecond after it has; a picture's number that only grows; and how many the sender finished in all (a reader
that is slower misses pictures, and knows how many). The newest whole slot is never the next one written, so a reader has at least two of
the sender's frames to read one. When no slot is free the picture is skipped and counted (`skipped`), and the game does not wait.

With no reader, nothing is copied, counted, marked or held: an override is one relaxed load of a counter. The memory (four pictures, 32 MiB
at 1920 x 1080) is there while somebody reads, and given back with the last reader.

Cost on the sender, 1920 x 1080 BGRA8, an M-series Mac, a sender that does nothing else and waits for its GPU each frame: 0.45 ms a frame with the
released server, with this one without the variable, and with it without a reader; 0.77 ms with a reader (the copy and one more command
buffer). At a 16 ms pace the calls of the sender cost the same with a reader as without.

## The end of a texture

The texture itself tells (a private data interface it holds): when its last reference goes, `GONE` is sent, `closed` in the memory is 1, and the
memory and descriptors go. A reader's own mapping stays valid until it unmaps it. A sender that restarts makes another texture: another
token. A device that is lost publishes no more frames. A worker's end closes its connections.

A DXMT texture that was opened is returned by DXMT with one reference too many (`ImportSharedTexture`), and was never freed:
`npt_publish.c` (`alone`) takes it back for the textures it opens.

## Which texture is which Spout sender

Spout's named memory is ordinary Wine named mappings, which this Wine keeps as plain files of the host while they live, 0600, in its server's
folder: `<macOS user temporary folder>/lkd-<uid>/tmp/.wine-1000/server-<dev>-<inode>/tmpmap-<8 hex digits>` (`LKD_HOST_TMP` in place of the first part
when it is set; the inode is the Wine prefix's, in hex). The file's name says nothing of the mapping; its content does:

| Mapping | File | Content |
| --- | --- | --- |
| `SpoutSenderNames` | 16 KiB (64 names; older SDKs 4 KiB, 10 or 16 names) | 256 bytes a name, NUL-terminated, empty ones zero |
| `ActiveSenderName` | 4 KiB | one name |
| the sender's info block, named after the sender | 4 KiB | 280 bytes: `u32` share handle (**the token**), `u32` width, `u32` height, `u32` DXGI format, `u32` usage, 256 bytes the sender's path, `u32` partner |

Confirmed on the engine with an owned sender: all three are there while it lives and readable by the user, and the info block's first word is the
token. What is **not** there is the info block's own name, so a name is tied to a token only where there is one of each (one live name in
`SpoutSenderNames`, one info block whose first word is a published token), which is VRChat's stream camera (`VRCSender1`). With several senders at once,
take them in `order`, as the game made them (VRChat's dolly numbers its own), or pair a new name with the info block that appeared with it. This is an
internal of Wine, not a contract; the socket's `order` is enough for one sender and does not depend on it. The render server does not see names and
cannot publish them.
