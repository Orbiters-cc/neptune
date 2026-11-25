/* SPDX-License-Identifier: MIT
 * What code that shares textures meets: shared_handles.exe video | shared_handles.exe receive NAME SECONDS
 * video: what a world's video player does: a texture on a device of its own, its shared handle, opened on the renderer's device; with
 * a keyed mutex first, then without one; and, where a handle is refused, its fallback (a copy through a staging texture).
 * receive: a Spout receiver in another process of the session (spout_sender.c's sender NAME): opens the handle of the info block on
 * its own device and looks at the pictures. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int make(ID3D11Device **device, ID3D11DeviceContext **context)
{
    D3D_FEATURE_LEVEL level;
    return SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, device, &level, context));
}
/* the first pixel of the texture, read through a staging one; 0 when it cannot be */
static UINT32 pixel(ID3D11Device *device, ID3D11DeviceContext *context, ID3D11Texture2D *t, UINT w, UINT h, int *uniform)
{
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ID3D11Texture2D *s = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    UINT32 v = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(device, &d, NULL, &s))) return 0;
    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)s, (ID3D11Resource *)t);
    if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(&v, m.pData, 4);
        if (uniform) {
            *uniform = 1;
            for (UINT y = 0; y < h; y++) {
                const UINT32 *row = (const UINT32 *)((const char *)m.pData + (size_t)y * m.RowPitch);
                for (UINT x = 0; x < w; x += 7) if (row[x] != v) *uniform = 0;
            }
        }
        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)s, 0);
    }
    ID3D11Texture2D_Release(s);
    return v;
}
static int video(void)
{
    ID3D11Device *mine, *game;
    ID3D11DeviceContext *mine_ctx, *game_ctx;
    if (!make(&mine, &mine_ctx) || !make(&game, &game_ctx)) return 3;
    static const struct { const char *what; UINT misc; } kinds[] = { { "keyed mutex", D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX }, { "plain", D3D11_RESOURCE_MISC_SHARED } };
    for (int k = 0; k < 2; k++) {
        D3D11_TEXTURE2D_DESC d = { 640, 360, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, kinds[k].misc };
        ID3D11Texture2D *t = NULL, *opened = NULL;
        IDXGIResource *r = NULL;
        ID3D11RenderTargetView *v = NULL;
        HANDLE handle = (HANDLE)(UINT_PTR)0x1234;
        HRESULT hr = ID3D11Device_CreateTexture2D(mine, &d, NULL, &t);
        if (FAILED(hr)) { printf("WITNESS video %s: CreateTexture2D 0x%08lx\n", kinds[k].what, (unsigned long)hr); continue; }
        ID3D11Device_CreateRenderTargetView(mine, (ID3D11Resource *)t, NULL, &v);
        ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource, (void **)&r);
        hr = IDXGIResource_GetSharedHandle(r, &handle);
        IDXGIResource_Release(r);
        printf("WITNESS video %s: GetSharedHandle 0x%08lx handle 0x%llx\n", kinds[k].what, (unsigned long)hr, (unsigned long long)(UINT_PTR)handle);
        int right = 0;
        for (int f = 1; f <= 60; f++) {
            float c[4] = { f / 255.0f, 10 / 255.0f, 20 / 255.0f, 1 };
            UINT32 got;
            ID3D11DeviceContext_ClearRenderTargetView(mine_ctx, v, c);
            ID3D11DeviceContext_Flush(mine_ctx);
            if (SUCCEEDED(hr)) { /* as on Windows: the renderer reads the same memory */
                if (!opened) ID3D11Device_OpenSharedResource(game, handle, &IID_ID3D11Texture2D, (void **)&opened);
                pixel(mine, mine_ctx, t, 640, 360, NULL); /* (the player's own wait for its GPU) */
                got = opened ? pixel(game, game_ctx, opened, 640, 360, NULL) : 0;
            } else { /* its fallback */
                got = pixel(mine, mine_ctx, t, 640, 360, NULL);
            }
            right += got == (0xff000000u | (UINT32)f << 16 | 10u << 8 | 20u);
        }
        printf("WITNESS video %s: %s, %d of 60 pictures right\n", kinds[k].what, SUCCEEDED(hr) ? "shared" : "fallback", right);
        if (opened) {
            HANDLE other = NULL;
            ID3D11Texture2D_QueryInterface(opened, &IID_IDXGIResource, (void **)&r);
            hr = IDXGIResource_GetSharedHandle(r, &other);
            printf("WITNESS video %s: the opened one's handle 0x%08lx %s\n", kinds[k].what, (unsigned long)hr, other == handle ? "same" : "OTHER");
            IDXGIResource_Release(r);
            ID3D11Texture2D_Release(opened);
        }
        ID3D11RenderTargetView_Release(v);
        ID3D11Texture2D_Release(t);
        ID3D11DeviceContext_Flush(mine_ctx);
        fflush(stdout);
    }
    /* a texture that is not shared, and handles that are nobody's */
    D3D11_TEXTURE2D_DESC d = { 64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *t = NULL;
    IDXGIResource *r = NULL;
    HANDLE handle = (HANDLE)(UINT_PTR)0x1234;
    ID3D11Device_CreateTexture2D(mine, &d, NULL, &t);
    ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource, (void **)&r);
    HRESULT hr = IDXGIResource_GetSharedHandle(r, &handle);
    printf("WITNESS unshared: GetSharedHandle 0x%08lx handle 0x%llx\n", (unsigned long)hr, (unsigned long long)(UINT_PTR)handle);
    static const UINT_PTR nobodys[] = { 0, 0x40000002, 0x7ffffffe, 0x4fffffff, 0x100000000ull, 0xdeadbeefcafeull, 2, 0x40000006 };
    for (unsigned k = 0; k < sizeof nobodys / sizeof *nobodys; k++) {
        ID3D11Texture2D *o = NULL;
        ID3D11Device_OpenSharedResource(game, (HANDLE)nobodys[k], &IID_ID3D11Texture2D, (void **)&o);
        UINT32 v = o ? pixel(game, game_ctx, o, 64, 64, NULL) : 0;
        printf("WITNESS nobody's 0x%llx: pixel 0x%08x\n", (unsigned long long)nobodys[k], v);
        if (o) ID3D11Texture2D_Release(o);
    }
    printf("WITNESS done\n");
    return 0;
}
static int receive(const char *name, int seconds)
{
    HANDLE info = NULL;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    for (int t = 0; t < 200 && !(info = OpenFileMappingA(FILE_MAP_READ, FALSE, name)); t++) Sleep(50);
    const unsigned char *i = info ? MapViewOfFile(info, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!i || !make(&device, &context)) { printf("WITNESS-R no sender\n"); return 3; }
    UINT32 h = 0, w = 0, hh = 0;
    for (int t = 0; t < 200 && !h; t++) { Sleep(50); memcpy(&h, i, 4); }
    memcpy(&w, i + 4, 4); memcpy(&hh, i + 8, 4);
    ID3D11Texture2D *t = NULL;
    HRESULT hr = ID3D11Device_OpenSharedResource(device, (HANDLE)(UINT_PTR)h, &IID_ID3D11Texture2D, (void **)&t);
    printf("WITNESS-R OpenSharedResource(0x%x) 0x%08lx %ux%u\n", h, (unsigned long)hr, w, hh);
    int looked = 0, whole = 0, changed = 0;
    UINT32 last = 0;
    DWORD end = GetTickCount() + seconds * 1000;
    while (t && GetTickCount() < end) {
        int uniform = 0;
        UINT32 v = pixel(device, context, t, w, hh, &uniform);
        looked++;
        whole += uniform && v;
        changed += v != last;
        last = v;
        Sleep(20);
    }
    printf("WITNESS-R looked %d times: %d whole pictures, %d changes, last 0x%08x\n", looked, whole, changed, last);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "receive")) return receive(argv[2], atoi(argv[3]));
    return video();
}
