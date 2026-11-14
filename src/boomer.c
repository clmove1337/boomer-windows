// boomer for Windows: zoom into a screenshot of the desktop, pixelated.
// Drag = pan, Alt+drag = draw, wheel = zoom, F = flashlight, Ctrl+wheel = flashlight size,
// 0 = reset, Q/Esc = quit.
// `boomer --daemon` stays in the background and pops up instantly on Win+Shift+Z.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <math.h>
#include <string.h>
#include <shellapi.h>
#include <limits.h>

static int sw, sh, sx, sy;           // virtual screen size/origin
static HDC hdcImg, hdcDim, hdcBack;  // screenshot, darkened screenshot, back buffer
static DWORD *img, *dim;
static int daemon;
static int dim_brightness = -1;     // integer multiplier used for the cached pixels
static double scale = 1;             // zoom
static double vx, vy;                // image point shown at window (0,0)
static int mx, my;                   // cursor in client coords
static int dragging, flashlight, closing; // closing = animating back to 1:1 before hiding
static int drawing;                 // gesture mode selected on button-down
static int repaint_pending;         // coalesce input updates until the next display frame
static double shade;                 // flashlight darkness, animated 0..1 towards `flashlight`
static double radius = 200;

// One Desktop Duplication per monitor; works for fullscreen games where GDI sees black.
typedef struct {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGIOutputDuplication *dup;
    ID3D11Texture2D *stage;
    int x, y, fresh;
} Dup;
static Dup dups[16];
static int ndups;
// D3D device per GPU, kept across popups: creating one takes ~170ms, the rest ~10ms.
static struct { LUID luid; ID3D11Device *dev; ID3D11DeviceContext *ctx; int used; } devs[8];
static int ndevs;

static void drop_dev(int i) {
    ID3D11DeviceContext_Release(devs[i].ctx);
    ID3D11Device_Release(devs[i].dev);
    devs[i] = devs[--ndevs];
}

static int get_device(IDXGIAdapter1 *a, ID3D11Device **dev, ID3D11DeviceContext **ctx) {
    DXGI_ADAPTER_DESC1 ad;
    int i;
    if (FAILED(IDXGIAdapter1_GetDesc1(a, &ad))) return 0;
    for (i = 0; i < ndevs; i++)
        if (!memcmp(&devs[i].luid, &ad.AdapterLuid, sizeof(LUID))) break;
    if (i < ndevs && ID3D11Device_GetDeviceRemovedReason(devs[i].dev) != S_OK) { // driver reset
        drop_dev(i);
        i = ndevs;
    }
    if (i == ndevs) {
        if (ndevs == 8 || FAILED(D3D11CreateDevice((IDXGIAdapter *)a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0,
                                                   D3D11_SDK_VERSION, &devs[i].dev, NULL, &devs[i].ctx)))
            return 0;
        devs[i].luid = ad.AdapterLuid;
        ndevs++;
    }
    devs[i].used = 1;
    *dev = devs[i].dev;
    *ctx = devs[i].ctx;
    return 1;
}

static HDC make_dib(int w, int h, DWORD **bits) {
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bitmap;
    HGDIOBJ old;
    *bits = NULL;
    if (!dc) return NULL;
    bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
    old = bitmap ? SelectObject(dc, bitmap) : NULL;
    if (!old || old == HGDI_ERROR) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        *bits = NULL;
        return NULL;
    }
    return dc;
}

static void free_dib(HDC dc) {
    if (!dc) return;
    HGDIOBJ b = GetCurrentObject(dc, OBJ_BITMAP);
    DeleteDC(dc);
    DeleteObject(b);
}

static void free_bufs(void) {
    free_dib(hdcImg); free_dib(hdcDim); free_dib(hdcBack);
    hdcImg = hdcDim = hdcBack = NULL;
    img = dim = NULL;
    sw = sh = 0;
    repaint_pending = 0;
}

// Follow the current monitor layout: allocate buffers and size the window to it.
// Buffers are always free here (hide frees them). Returns 0 if they could not be allocated.
static int fit_screen(HWND w) {
    DWORD *back;
    sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    hdcImg = make_dib(sw, sh, &img);
    hdcBack = make_dib(sw, sh, &back);
    if (!hdcImg || !hdcBack) { free_bufs(); return 0; }
    SetStretchBltMode(hdcBack, COLORONCOLOR); // nearest neighbour = pixelated
    sx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    sy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetWindowPos(w, HWND_TOPMOST, sx, sy, sw, sh, SWP_NOACTIVATE);
    return 1;
}

static void close_dups(void) {
    for (int i = 0; i < ndups; i++) {
        if (dups[i].stage) ID3D11Texture2D_Release(dups[i].stage);
        IDXGIOutputDuplication_Release(dups[i].dup);
        ID3D11DeviceContext_Release(dups[i].ctx);
        ID3D11Device_Release(dups[i].dev);
    }
    ndups = 0;
}

static void open_dups(void) {
    IDXGIFactory1 *f;
    IDXGIAdapter1 *a;
    close_dups();
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return;
    for (int i = 0; i < ndevs; i++) devs[i].used = 0;
    for (UINT i = 0; SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, i, &a)); i++) {
        ID3D11Device *dev = NULL;
        ID3D11DeviceContext *ctx;
        IDXGIOutput *o;
        for (UINT j = 0; ndups < 16 && SUCCEEDED(IDXGIAdapter1_EnumOutputs(a, j, &o)); j++) {
            DXGI_OUTPUT_DESC od;
            IDXGIOutput1 *o1;
            if (FAILED(IDXGIOutput_GetDesc(o, &od))) { IDXGIOutput_Release(o); continue; }
            // shortcut: rotated monitors are left to the GDI capture; rotate the frame if anyone needs it.
            // Device only for GPUs with a monitor: one on a laptop's idle dGPU would keep it powered.
            if (od.AttachedToDesktop && od.Rotation <= DXGI_MODE_ROTATION_IDENTITY &&
                (dev || get_device(a, &dev, &ctx)) &&
                SUCCEEDED(IDXGIOutput_QueryInterface(o, &IID_IDXGIOutput1, (void **)&o1))) {
                Dup *d = &dups[ndups];
                HRESULT hr = IDXGIOutput1_DuplicateOutput(o1, (IUnknown *)dev, &d->dup);
                if (SUCCEEDED(hr)) {
                    d->dev = dev; ID3D11Device_AddRef(dev);
                    d->ctx = ctx; ID3D11DeviceContext_AddRef(ctx);
                    d->stage = NULL;
                    d->x = od.DesktopCoordinates.left;
                    d->y = od.DesktopCoordinates.top;
                    d->fresh = 1;
                    ndups++;
                }
                IDXGIOutput1_Release(o1);
            }
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    // GPU lost its monitors (unplugged): let it power down. Backwards, as drop_dev swaps in the last entry.
    for (int i = ndevs; i-- > 0;)
        if (!devs[i].used) drop_dev(i);
}

// Clamp a tw x th frame at (x0,y0) to a bw x bh buffer; returns 0 if nothing of it is inside.
// The frame can be a different size than the desktop mid mode-switch, or lie outside
// our buffer if a monitor was added while open (layout is re-read on next popup).
static int clip(int x0, int y0, int tw, int th, int bw, int bh, int *w, int *h) {
    *w = min(tw, bw - x0);
    *h = min(th, bh - y0);
    return x0 >= 0 && y0 >= 0 && *w > 0 && *h > 0;
}

// Copy one screenshot per output; returns 1 on success.
static int copy_frame(Dup *d, ID3D11Texture2D *tex) {
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE map;
    int x0 = d->x - sx, y0 = d->y - sy, w, h;
    ID3D11Texture2D_GetDesc(tex, &td);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM || !clip(x0, y0, td.Width, td.Height, sw, sh, &w, &h)) return 0;
    if (!d->stage) {
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->stage))) return 0;
    }
    ID3D11DeviceContext_CopyResource(d->ctx, (ID3D11Resource *)d->stage, (ID3D11Resource *)tex);
    if (FAILED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->stage, 0, D3D11_MAP_READ, 0, &map))) return 0;
    for (int y = 0; y < h; y++) {
        DWORD *dst = img + (size_t)(y0 + y) * sw + x0;
        BYTE *src = (BYTE *)map.pData + (size_t)y * map.RowPitch;
        memcpy(dst, src, (size_t)w * 4);
    }
    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->stage, 0);
    return 1;
}

// Share a 250 ms frame-wait budget across all outputs, keeping GDI on timeout.
static int grab(ULONGLONG deadline) {
    int captured = 0;
    for (int i = 0; i < ndups; i++) {
        Dup *d = &dups[i];
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res;
        ID3D11Texture2D *tex;
        HRESULT hr;
        ULONGLONG now = GetTickCount64();
        UINT timeout = now < deadline ? (UINT)min(deadline - now, 25) : 0;
        if (!d->fresh) continue;
        hr = IDXGIOutputDuplication_AcquireNextFrame(d->dup, timeout, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) { d->fresh = 0; continue; } // retain the GDI fallback
        // 0 = no new image (cursor-only update, or the black first frame of a new duplication)
        if (fi.LastPresentTime.QuadPart &&
            SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
            GdiFlush();
            if (copy_frame(d, tex)) { d->fresh = 0; captured = 1; }
            ID3D11Texture2D_Release(tex);
        }
        IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(d->dup);
    }
    return captured;
}

static int waiting(void) {
    for (int i = 0; i < ndups; i++)
        if (dups[i].fresh) return 1;
    return 0;
}

// Map the visible part of the image to integer source/dest rects so pixels stay crisp.
static int view_rect(int imgw, int winw, double v, double s, int *src0, int *srcw, int *dst0, int *dstw) {
    // Clip in floating point first: prolonged panning can put v outside int range.
    if (!isfinite(v) || !isfinite(s) || s <= 0 || imgw <= 0 || winw <= 0) return 0;
    double left = max(floor(v), 0), right = min(ceil(v + winw / s), imgw);
    if (right <= left) return 0;
    int a = (int)left, b = (int)right;
    if (b <= a) return 0;
    *src0 = a; *srcw = b - a;
    *dst0 = (int)lround((a - v) * s);
    *dstw = (int)lround((b - v) * s) - *dst0;
    return 1;
}

static void blit(HDC dst, HDC src) {
    int x, w, dx, dw, y, h, dy, dh;
    if (view_rect(sw, sw, vx, scale, &x, &w, &dx, &dw) &&
        view_rect(sh, sh, vy, scale, &y, &h, &dy, &dh))
        StretchBlt(dst, dx, dy, dw, dh, src, x, y, w, h, SRCCOPY);
}

// Zoom to s keeping the image point under the cursor fixed.
static void zoom_to(double s) {
    double ix = vx + mx / scale, iy = vy + my / scale;
    scale = min(max(s, 0.1), 200);
    vx = ix - mx / scale;
    vy = iy - my / scale;
}

static DWORD darken(DWORD pixel, DWORD brightness) {
    return ((pixel & 0xff00ff) * brightness >> 8 & 0xff00ff) |
           ((pixel & 0x00ff00) * brightness >> 8 & 0x00ff00);
}

// Paint into the screenshot so marks naturally follow zoom/pan and the flashlight.
// The pen is roughly three screen pixels wide at the zoom where it is drawn.
static void draw_line(int x0, int y0, int x1, int y1) {
    double ax = vx + x0 / scale, ay = vy + y0 / scale;
    double bx = vx + x1 / scale, by = vy + y1 / scale;
    int width = max(1, (int)lround(3 / scale));
    if (!hdcImg || !isfinite(ax) || !isfinite(ay) || !isfinite(bx) || !isfinite(by)) return;
    if (max(ax, bx) < -width || min(ax, bx) > sw + width ||
        max(ay, by) < -width || min(ay, by) > sh + width) return;
    // Leave room for the rounded endpoints and pen width before converting to GDI coordinates.
    if (min(ax, bx) < INT_MIN / 2 || max(ax, bx) > INT_MAX / 2 ||
        min(ay, by) < INT_MIN / 2 || max(ay, by) > INT_MAX / 2) return;
    HPEN pen = CreatePen(PS_SOLID, width, RGB(255, 64, 64));
    if (!pen) return;
    HGDIOBJ old = SelectObject(hdcImg, pen);
    if (old && old != HGDI_ERROR) {
        int a = (int)lround(ax), b = (int)lround(ay);
        int c = (int)lround(bx), d = (int)lround(by);
        MoveToEx(hdcImg, a, b, NULL);
        LineTo(hdcImg, c, d);
        // LineTo excludes its final pixel; also make a click without movement visible.
        SetPixelV(hdcImg, c, d, RGB(255, 64, 64));
        SelectObject(hdcImg, old);
        dim_brightness = -1;
    }
    DeleteObject(pen);
}

static HANDLE make_frame_timer(void) {
    // High-resolution timers require Windows 10 1803; retain the standard fallback.
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
    return timer ? timer : CreateWaitableTimerW(NULL, FALSE, NULL);
}

static void popup(HWND w) {
    HDC screen;
    POINT cursor;
    ULONGLONG deadline;
    int captured = 0;
    if (!fit_screen(w)) {
        MessageBoxW(NULL, L"Not enough memory for the screen buffers", L"boomer", MB_ICONERROR);
        if (!daemon) DestroyWindow(w);
        return;
    }
    // GDI first as a fallback for monitors DXGI can't duplicate, then DXGI on top.
    memset(img, 0, (size_t)sw * sh * sizeof(*img));
    screen = GetDC(NULL);
    if (screen) {
        captured = BitBlt(hdcImg, 0, 0, sw, sh, screen, sx, sy, SRCCOPY | CAPTUREBLT);
        ReleaseDC(NULL, screen);
    }
    open_dups();
    deadline = GetTickCount64() + 250;
    do { captured |= grab(deadline); } while (waiting() && GetTickCount64() < deadline);
    close_dups();
    if (!captured) {
        free_bufs();
        MessageBoxW(NULL, L"Could not capture the desktop. Try again after unlocking the screen.", L"boomer", MB_ICONERROR);
        if (!daemon) DestroyWindow(w);
        return;
    }
    flashlight = 0;
    shade = 0;
    dim_brightness = -1;
    scale = 1;
    vx = vy = 0;
    dragging = drawing = closing = 0;
    if (GetCursorPos(&cursor) && ScreenToClient(w, &cursor)) { mx = cursor.x; my = cursor.y; }
    repaint_pending = 1;
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
}

static void hide(HWND w) {
    if (!daemon) { DestroyWindow(w); return; }
    ShowWindow(w, SW_HIDE);
    // Don't hold screen-sized buffers while idle; fit_screen reallocates on popup.
    free_bufs();
}

// Pure time step: -1 means close, 1 means pixels changed, 0 means idle.
static int animate(double seconds) {
    double t = seconds / 0.0156, k;
    int moving = 0;
    if (shade != flashlight) { // flashlight fade in/out
        shade += (flashlight - shade) * (1 - pow(0.75, t));
        if (fabs(flashlight - shade) <= 1e-3) shade = flashlight;
        moving = 1;
    }
    if (closing) { // ease view back to 1:1, then hide
        // Scale about the one screen point that is in the same place zoomed and at 1:1,
        // so the view shrinks back into place instead of sliding: vx = c * (1 - 1/scale).
        k = pow(0.7, t);
        double s = 1 + (scale - 1) * k;
        vx *= k * scale / s;
        vy *= k * scale / s;
        scale = s;
        if (fabs(scale - 1) < 5e-3 && fabs(vx) < 1 && fabs(vy) < 1 && shade < 0.02) return -1;
        return 1;
    }
    return moving;
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_HOTKEY:
        if (!IsWindowVisible(w)) popup(w);
        return 0;
    case WM_MOUSEWHEEL: {
        if (closing || drawing) return 0;
        POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
        double f = pow(1.1, (short)HIWORD(wp) / (double)WHEEL_DELTA);
        ScreenToClient(w, &p);
        mx = p.x; my = p.y;
        if (flashlight && (LOWORD(wp) & MK_CONTROL)) {
            radius *= f;
            if (radius < 10) radius = 10;
            if (radius > sw + sh) radius = sw + sh; // bigger covers the screen anyway; keeps (int)radius valid
        } else {
            zoom_to(scale * f);
        }
        repaint_pending = 1;
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (closing) return 0;
        mx = (short)LOWORD(lp); my = (short)HIWORD(lp);
        drawing = (GetKeyState(VK_MENU) & 0x8000) != 0;
        dragging = !drawing;
        SetCapture(w);
        if (drawing) { draw_line(mx, my, mx, my); repaint_pending = 1; }
        return 0;
    case WM_LBUTTONUP:
        if (drawing) {
            draw_line(mx, my, (short)LOWORD(lp), (short)HIWORD(lp));
            repaint_pending = 1;
        }
        dragging = drawing = 0;
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        dragging = drawing = 0;
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        if (!(wp & MK_LBUTTON)) dragging = drawing = 0; // released outside
        if (drawing) draw_line(mx, my, x, y);
        if (dragging) { vx -= (x - mx) / scale; vy -= (y - my) / scale; }
        mx = x; my = y;
        if (dragging || drawing || shade > 0) repaint_pending = 1;
        return 0;
    }
    case WM_KEYDOWN:
        if (lp & 0x40000000) return 0; // auto-repeat of a held key
        if (wp == VK_ESCAPE || wp == 'Q') { // first press animates out, a second one skips it
            if (closing) { hide(w); return 0; }
            closing = 1;
            flashlight = dragging = drawing = 0;
            ReleaseCapture();
            repaint_pending = 1;
            return 0;
        }
        if (closing || drawing) return 0;
        if (wp == '0' || wp == VK_NUMPAD0) { scale = 1; vx = vy = 0; }
        if (wp == 'F') {
            if (!hdcDim) {
                hdcDim = make_dib(sw, sh, &dim);
                if (!hdcDim) {
                    MessageBoxW(w, L"Not enough memory for the flashlight", L"boomer", MB_ICONERROR);
                    return 0;
                }
            }
            flashlight = !flashlight;
        }
        repaint_pending = 1;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        if (!hdcBack) { EndPaint(w, &ps); return 0; }
        PatBlt(hdcBack, 0, 0, sw, sh, BLACKNESS);
        if (shade == 0) blit(hdcBack, hdcImg);
        if (shade > 0) { // darkened copy everywhere, then the normal image inside the circle
            HRGN r = CreateEllipticRgn(mx - (int)radius, my - (int)radius, mx + (int)radius, my + (int)radius);
            DWORD k = (DWORD)(256 - shade * 192); // 256 = 100%, 64 = 25% brightness
            // Different animation values can produce the same integer pixels.
            if (dim_brightness != (int)k) {
                GdiFlush(); // finish GDI access before touching DIB memory
                for (size_t i = 0; i < (size_t)sh * sw; i++)
                    dim[i] = darken(img[i], k);
                dim_brightness = (int)k;
            }
            blit(hdcBack, hdcDim);
            SelectClipRgn(hdcBack, r);
            blit(hdcBack, hdcImg);
            SelectClipRgn(hdcBack, NULL);
            DeleteObject(r);
        }
        BitBlt(dc, 0, 0, sw, sh, hdcBack, 0, 0, SRCCOPY);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY:
        close_dups();
        free_bufs();
        while (ndevs) drop_dev(ndevs - 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
static int self_test(void) {
    int s, sw_, d, dw;
    CHECK(view_rect(100, 100, 0, 1, &s, &sw_, &d, &dw) && s == 0 && sw_ == 100 && d == 0 && dw == 100);
    CHECK(view_rect(100, 100, 10.5, 2, &s, &sw_, &d, &dw) && s == 10 && sw_ == 51 && d == -1 && dw == 102);
    CHECK(view_rect(100, 100, -50, 1, &s, &sw_, &d, &dw) && s == 0 && sw_ == 50 && d == 50);
    CHECK(!view_rect(100, 100, 200, 1, &s, &sw_, &d, &dw));
    CHECK(!view_rect(100, 100, 1e30, 1, &s, &sw_, &d, &dw));
    CHECK(!view_rect(100, 100, -1e30, 1, &s, &sw_, &d, &dw));
    CHECK(!view_rect(100, 100, 0, 0, &s, &sw_, &d, &dw));
    // Compare packed-channel arithmetic with independent per-channel reference values.
    for (DWORD k = 64; k <= 256; k++) {
        for (DWORD value = 0; value < 256; value++) {
            DWORD r = value, g = 255 - value, b = (value * 73) & 255;
            DWORD pixel = 0xff000000 | (r << 16) | (g << 8) | b;
            DWORD expected = ((r * k / 256) << 16) | ((g * k / 256) << 8) | (b * k / 256);
            CHECK(darken(pixel, k) == expected);
        }
    }
    CHECK(clip(60, 10, 50, 50, 100, 100, &s, &d) && s == 40 && d == 50); // sticks out right
    CHECK(!clip(100, 0, 50, 50, 100, 100, &s, &d));                       // fully outside
    CHECK(!clip(-10, 0, 50, 50, 100, 100, &s, &d));                       // left of buffer
    scale = 2; vx = 10; vy = -20; mx = 200; my = 100;
    zoom_to(4);
    CHECK(fabs(vx + mx / scale - 110) < 1e-9);
    CHECK(fabs(vy + my / scale - 30) < 1e-9);
    zoom_to(1000);
    CHECK(scale == 200 && fabs(vx + mx / scale - 110) < 1e-9);
    zoom_to(0.001);
    CHECK(scale == 0.1 && fabs(vy + my / scale - 30) < 1e-9);
    // Equal elapsed time must produce the same fade at different frame rates.
    flashlight = 1; shade = 0; closing = 0;
    for (int i = 0; i < 12; i++) animate(1.0 / 60);
    double fade60 = shade;
    shade = 0;
    for (int i = 0; i < 48; i++) animate(1.0 / 240);
    CHECK(fabs(shade - fade60) < 1e-9);
    for (int rate = 30; rate <= 240; rate *= 2) {
        for (int zoomed_out = 0; zoomed_out < 2; zoomed_out++) {
            scale = zoomed_out ? 0.1 : 200; vx = 5000; vy = -3000;
            shade = 1; flashlight = 0; closing = 1;
            int result = 0;
            for (int i = 0; i < rate * 3 && result != -1; i++) result = animate(1.0 / rate);
            CHECK(result == -1 && fabs(scale - 1) < 0.005 && fabs(vx) < 1 && fabs(vy) < 1);
        }
    }
    closing = 0; shade = 0; flashlight = 0;
    CHECK(animate(1) == 0);
    // Drawing targets image coordinates, invalidates the dim cache, and clips safely.
    sw = sh = 32;
    hdcImg = make_dib(sw, sh, &img);
    CHECK(hdcImg && img);
    memset(img, 0, (size_t)sw * sh * sizeof(*img));
    scale = 2; vx = 4; vy = 6; dim_brightness = 64;
    draw_line(4, 4, 20, 4); // image (6,8) through (14,8)
    GdiFlush();
    CHECK((img[8 * sw + 6] & 0xffffff) == 0xff4040);
    CHECK((img[8 * sw + 10] & 0xffffff) == 0xff4040);
    CHECK((img[8 * sw + 14] & 0xffffff) == 0xff4040);
    CHECK(img[0] == 0 && dim_brightness == -1);
    draw_line(8, 12, 8, 12); // click without movement: image (8,12)
    GdiFlush();
    CHECK((img[12 * sw + 8] & 0xffffff) == 0xff4040);
    vx = vy = -1e30; dim_brightness = 64;
    draw_line(0, 0, 100, 100);
    CHECK(dim_brightness == 64); // off-image strokes do not touch buffers
    free_bufs();
    // Exercise both lazy allocation paths and repeated/partial cleanup.
    DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int pass = 0; pass < 100; pass++) {
        DWORD *back;
        sw = sh = 16;
        hdcImg = make_dib(sw, sh, &img);
        hdcBack = make_dib(sw, sh, &back);
        CHECK(hdcImg && hdcBack && !hdcDim && !dim);
        if (pass % 2) {
            hdcDim = make_dib(sw, sh, &dim);
            CHECK(hdcDim && dim);
        }
        free_bufs();
        free_bufs();
        CHECK(!hdcImg && !hdcDim && !hdcBack && !img && !dim);
    }
    hdcDim = make_dib(16, 16, &dim);
    CHECK(hdcDim);
    free_bufs();
    CHECK(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) == before);
    HANDLE timer = make_frame_timer();
    LARGE_INTEGER due;
    CHECK(timer);
    due.QuadPart = -100000; // 10 ms, one shot
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    CHECK(WaitForSingleObject(timer, 2000) == WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(timer, 0) == WAIT_TIMEOUT); // automatically reset, not periodic
    due.QuadPart = -10000000; // enough time to cancel deterministically
    CHECK(SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE));
    CHECK(CancelWaitableTimer(timer));
    CHECK(WaitForSingleObject(timer, 30) == WAIT_TIMEOUT);
    CloseHandle(timer);
    return 0;
}

int WINAPI WinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE prevInst, _In_ LPSTR cmd, _In_ int show) {
    MSG msg;
    WNDCLASSW wc = {0};
    HWND w;
    HANDLE frame_timer;
    LARGE_INTEGER frequency, last;
    int armed = 0, animation_active = 0;
    double frame_seconds = 1.0 / 60;

    int argc, test = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--daemon")) daemon = 1;
        else if (!wcscmp(argv[i], L"--self-test")) test = 1;
        else {
            LocalFree(argv);
            MessageBoxW(NULL, L"Usage: boomer.exe [--daemon] [--self-test]", L"boomer", MB_ICONERROR);
            return 1;
        }
    }
    LocalFree(argv);
    if (test) return self_test();
    frame_timer = make_frame_timer();
    if (!frame_timer) return 1;
    QueryPerformanceFrequency(&frequency);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // capture real pixels, not scaled

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"boomer";
    RegisterClassW(&wc);
    w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"boomer", L"boomer", WS_POPUP,
                        0, 0, 1, 1, NULL, NULL, inst, NULL);
    if (!w) { CloseHandle(frame_timer); return 1; }
    if (daemon) {
        if (!RegisterHotKey(w, 1, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, 'Z')) {
            MessageBoxW(NULL, L"Win+Shift+Z is taken (boomer already running?)", L"boomer", MB_ICONERROR);
            DestroyWindow(w);
            CloseHandle(frame_timer);
            return 1;
        }
        open_dups(); // warm up the GPU devices so the first popup is instant
        close_dups();
    } else {
        popup(w);
    }
    for (;;) {
        // Bound each batch so sustained mouse input cannot starve animation ticks.
        for (int count = 0; count < 64 && PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE); count++) {
            if (msg.message == WM_QUIT) { CloseHandle(frame_timer); return (int)msg.wParam; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        int moving = hdcBack && IsWindowVisible(w) && (repaint_pending || closing || shade != flashlight);
        if (!moving && armed) { CancelWaitableTimer(frame_timer); armed = 0; }
        if (!moving) animation_active = 0;
        if (moving && !armed) {
            if (!animation_active) {
                DWM_TIMING_INFO timing = {sizeof(timing)};
                if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &timing)) && timing.qpcRefreshPeriod)
                    frame_seconds = (double)timing.qpcRefreshPeriod / frequency.QuadPart;
                frame_seconds = min(max(frame_seconds, 1.0 / 360), 1.0 / 30);
                QueryPerformanceCounter(&last);
                animation_active = 1;
            }
            LARGE_INTEGER due, now;
            QueryPerformanceCounter(&now);
            // Include painting time in the frame interval; never busy-loop when behind.
            double remaining = frame_seconds - (double)(now.QuadPart - last.QuadPart) / frequency.QuadPart;
            due.QuadPart = -(LONGLONG)(max(remaining, 0.001) * 10000000);
            if (!SetWaitableTimer(frame_timer, &due, 0, NULL, NULL, FALSE)) {
                MessageBoxW(w, L"Could not schedule animation frames", L"boomer", MB_ICONERROR);
                DestroyWindow(w);
                CloseHandle(frame_timer);
                return 1;
            }
            armed = 1;
        }
        DWORD result = MsgWaitForMultipleObjectsEx(armed ? 1 : 0, &frame_timer, INFINITE,
                                                   QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (armed && result == WAIT_OBJECT_0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int changed = animate((double)(now.QuadPart - last.QuadPart) / frequency.QuadPart);
            last = now;
            armed = 0;
            if (changed < 0) hide(w);
            else if (changed || repaint_pending) {
                repaint_pending = 0;
                InvalidateRect(w, NULL, FALSE);
                UpdateWindow(w); // render once per tick, after coalescing input updates
            }
        } else if (result == WAIT_FAILED) {
            DestroyWindow(w);
            CloseHandle(frame_timer);
            return 1;
        }
    }
}
