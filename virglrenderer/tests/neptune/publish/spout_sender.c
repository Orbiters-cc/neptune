/* SPDX-License-Identifier: MIT
 * An owned Direct3D 11 Spout sender (the layout of the Spout2 SDK), whose every picture tells its own number:
 *   spout_sender.exe NAME WIDTH HEIGHT FRAMES copy|draw PERIOD_MS [SENDERS [HOLD_MS]]
 * copy: a picture drawn elsewhere, then CopyResource into the shared texture, then Flush, as KlakSpout through Unity does it;
 * draw: cleared in the shared texture itself, its render target. SENDERS: so many one after the other, each with a texture and
 * a handle of its own, as a sender that restarts. PERIOD_MS: 0, each frame waits for the GPU (what a frame costs); else the pace.
 * Every picture is one colour, red = n, green = n >> 8, blue = 7 n (n from 1). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}
static int by_value(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
    if (argc < 7) return 2;
    const char *name = argv[1];
    UINT w = (UINT)atoi(argv[2]), h = (UINT)atoi(argv[3]);
    int frames = atoi(argv[4]), draw = !strcmp(argv[5], "draw"), period = atoi(argv[6]);
    int senders = argc > 7 ? atoi(argv[7]) : 1, hold = argc > 8 ? atoi(argv[8]) : 0;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) return 3;
    HANDLE names = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64 * 256, "SpoutSenderNames");
    HANDLE active = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 256, "ActiveSenderName");
    HANDLE info = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 280, name);
    char *n = MapViewOfFile(names, FILE_MAP_ALL_ACCESS, 0, 0, 0), *a = MapViewOfFile(active, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    unsigned char *i = MapViewOfFile(info, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    strncpy(n, name, 255);
    strncpy(a, name, 255);
    double *ms = calloc(frames, sizeof *ms);
    for (int s = 0; s < senders; s++) {
        D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED };
        ID3D11Texture2D *shared = NULL, *source = NULL;
        ID3D11RenderTargetView *view = NULL;
        IDXGIResource *resource = NULL;
        HANDLE handle = NULL, again = NULL;
        hr = ID3D11Device_CreateTexture2D(device, &d, NULL, &shared);
        if (FAILED(hr)) { printf("WITNESS CreateTexture2D 0x%08lx\n", (unsigned long)hr); return 4; }
        ID3D11Texture2D_QueryInterface(shared, &IID_IDXGIResource, (void **)&resource);
        hr = IDXGIResource_GetSharedHandle(resource, &handle);
        HRESULT hr2 = IDXGIResource_GetSharedHandle(resource, &again);
        IDXGIResource_Release(resource);
        printf("WITNESS sender %d GetSharedHandle 0x%08lx handle 0x%llx again 0x%08lx %s\n", s, (unsigned long)hr, (unsigned long long)(UINT_PTR)handle, (unsigned long)hr2, again == handle ? "same" : "OTHER");
        fflush(stdout);
        d.MiscFlags = 0;
        ID3D11Device_CreateTexture2D(device, &d, NULL, &source);
        ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)(draw ? shared : source), NULL, &view);
        UINT32 h32 = (UINT32)(UINT_PTR)handle, fmt = DXGI_FORMAT_B8G8R8A8_UNORM, usage = 0;
        memcpy(i + 4, &w, 4); memcpy(i + 8, &h, 4); memcpy(i + 12, &fmt, 4); memcpy(i + 16, &usage, 4);
        GetModuleFileNameA(NULL, (char *)i + 20, 255);
        memcpy(i, &h32, 4);
        D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
        ID3D11Query *query = NULL;
        ID3D11Device_CreateQuery(device, &qd, &query);
        double next = now_ms(), start = next;
        for (int f = 1; f <= frames; f++) {
            double t0 = now_ms();
            float c[4] = { (f & 255) / 255.0f, ((f >> 8) & 255) / 255.0f, ((f * 7) & 255) / 255.0f, 1.0f };
            if (draw) ID3D11DeviceContext_OMSetRenderTargets(context, 1, &view, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(context, view, c);
            if (!draw) ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)shared, (ID3D11Resource *)source);
            ID3D11DeviceContext_Flush(context);
            if (!period) {
                ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);
                BOOL done = FALSE;
                while (ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query, &done, sizeof done, 0) != S_OK || !done) ;
            }
            ms[f - 1] = now_ms() - t0;
            if (period) {
                next += period;
                double left = next - now_ms();
                if (left > 0) Sleep((DWORD)left);
            }
        }
        double total = now_ms() - start;
        qsort(ms, frames, sizeof *ms, by_value);
        double sum = 0;
        for (int f = 0; f < frames; f++) sum += ms[f];
        printf("WITNESS sender %d %ux%u %d frames in %.0f ms: a frame %.3f ms mean, %.3f median, %.3f p95, %.3f worst\n", s, w, h, frames, total, sum / frames, ms[frames / 2], ms[frames * 95 / 100], ms[frames - 1]);
        fflush(stdout);
        if (hold) Sleep(hold);
        if (draw) { ID3D11RenderTargetView *none = NULL; ID3D11DeviceContext_OMSetRenderTargets(context, 1, &none, NULL); }
        ID3D11DeviceContext_Flush(context);
        ID3D11RenderTargetView_Release(view);
        ID3D11Query_Release(query);
        ID3D11Texture2D_Release(source);
        ULONG left = ID3D11Texture2D_Release(shared);
        ID3D11DeviceContext_Flush(context);
        printf("WITNESS sender %d ended, references left %lu\n", s, left);
        fflush(stdout);
        if (hold) Sleep(hold);
    }
    memset(n, 0, 256);
    printf("WITNESS done\n");
    return 0;
}
