#include <intrin.h>
#include "overlay.h"
#include "log.h"
#include "mem.h"
#include "config.h"
#include "input.h"
#include <cstdint>
#include <algorithm>

// speed.exe v1.3: import address table slot for d3d9!Direct3DCreate9
static constexpr uintptr_t IAT_Direct3DCreate9 = 0x8904A8;

namespace {
    using Direct3DCreate9_t = IDirect3D9* (WINAPI*)(UINT);
    using CreateDevice_t    = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
    using Present_t         = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using Reset_t           = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    Direct3DCreate9_t o_Direct3DCreate9 = nullptr;
    CreateDevice_t    o_CreateDevice = nullptr;
    Present_t         o_Present = nullptr;
    uint32_t          g_frames = 0;
    Reset_t           o_Reset = nullptr;

    IDirect3DDevice9* g_dev = nullptr;
    Overlay::FrameCallback g_cb = nullptr;

    // ---- text rendering (GDI -> managed texture) ----
    constexpr int TEX_W = 1024, TEX_H = 256;
    IDirect3DTexture9* g_tex = nullptr;
    std::string g_text, g_texText;
    int  g_textW = 0, g_textH = 0;
    bool g_visible = true;
    bool g_forceShow = false;
    std::string g_shotPath;     // test command: save the next frame (without our overlay) as a BMP

    void SaveBackBuffer(IDirect3DDevice9* dev, const std::string& path) {
        IDirect3DSurface9* bb = nullptr; IDirect3DSurface9* sys = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
        D3DSURFACE_DESC d; bb->GetDesc(&d);
        IDirect3DSurface9* src = bb; IDirect3DSurface9* rt = nullptr;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE) {   // resolve first
            if (SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))) {
                dev->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE); src = rt;
            }
        }
        if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
            SUCCEEDED(dev->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                if (FILE* f = fopen(path.c_str(), "wb")) {   // 32-bit BMP, top-down (X8R8G8B8 / A8R8G8B8)
                    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
                    ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32;
                    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
                    for (UINT y = 0; y < d.Height; ++y) fwrite((uint8_t*)lr.pBits + y * lr.Pitch, 4, d.Width, f);
                    fclose(f);
                    LOG("Screenshot saved: %s (%ux%u)", path.c_str(), d.Width, d.Height);
                }
                sys->UnlockRect();
            }
        }
        if (sys) sys->Release();
        if (rt) rt->Release();
        bb->Release();
    }   // a notification is up: draw even when the overlay is hidden (F8)
    // Initialised at DLL load (static constructor), before anything can call SetText.
    struct TextLock {
        CRITICAL_SECTION cs;
        TextLock() { InitializeCriticalSection(&cs); }
    } g_textLock;
    #define g_textCs (g_textLock.cs)

    bool RebuildTexture(IDirect3DDevice9* dev) {
        if (!g_tex) {
            if (FAILED(dev->CreateTexture(TEX_W, TEX_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, nullptr))) {
                g_tex = nullptr;
                return false;
            }
        }

        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = TEX_W;
        bi.bmiHeader.biHeight = -TEX_H; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bmp || !bits) {   // out of GDI handles / memory: skip this update instead of writing to null
            if (bmp) DeleteObject(bmp);
            if (dc) DeleteDC(dc);
            return false;
        }
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HFONT font = CreateFontA(18, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
        HGDIOBJ oldFont = SelectObject(dc, font);

        memset(bits, 0, TEX_W * TEX_H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT rc{ 6, 4, TEX_W - 6, TEX_H - 4 };
        DrawTextA(dc, g_texText.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);
        RECT calc{ 0, 0, TEX_W - 12, TEX_H - 8 };
        DrawTextA(dc, g_texText.c_str(), -1, &calc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);
        g_textW = (std::min)((int)calc.right + 12, TEX_W);
        g_textH = (std::min)((int)calc.bottom + 8, TEX_H);
        GdiFlush();

        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_tex->LockRect(0, &lr, nullptr, 0))) {
            const uint32_t* src = (const uint32_t*)bits;
            for (int y = 0; y < TEX_H; ++y) {
                uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < TEX_W; ++x) {
                    uint32_t c = src[y * TEX_W + x] & 0xFF; // white text, use one channel as alpha
                    dst[x] = (c << 24) | 0x00FFFFFF;
                }
            }
            g_tex->UnlockRect(0);
        }

        SelectObject(dc, oldFont);
        SelectObject(dc, oldBmp);
        DeleteObject(font);
        DeleteObject(bmp);
        DeleteDC(dc);
        return true;
    }

    struct Vtx { float x, y, z, rhw; DWORD color; float u, v; };

    void DrawOverlay(IDirect3DDevice9* dev) {
        std::string text;
        EnterCriticalSection(&g_textCs);
        text = g_text;
        LeaveCriticalSection(&g_textCs);
        if (text.empty()) return;

        if (!g_tex || text != g_texText) {
            g_texText = text;
            if (!RebuildTexture(dev)) return;
        }

        IDirect3DStateBlock9* sb = nullptr;
        if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) return;

        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        const float x0 = 10.f, y0 = 10.f;
        const float w = (float)g_textW, h = (float)g_textH;
        const float u1 = w / TEX_W, v1 = h / TEX_H;

        // background panel (texture disabled -> diffuse only)
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        const DWORD bg = 0xA0000000;
        Vtx quad[4] = {
            { x0 - .5f,     y0 - .5f,     0, 1, bg, 0, 0 },
            { x0 + w - .5f, y0 - .5f,     0, 1, bg, 0, 0 },
            { x0 - .5f,     y0 + h - .5f, 0, 1, bg, 0, 0 },
            { x0 + w - .5f, y0 + h - .5f, 0, 1, bg, 0, 0 },
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vtx));

        // text
        dev->SetTexture(0, g_tex);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        const DWORD fg = 0xFFFFFFFF;
        Vtx tq[4] = {
            { x0 - .5f,     y0 - .5f,     0, 1, fg, 0,  0  },
            { x0 + w - .5f, y0 - .5f,     0, 1, fg, u1, 0  },
            { x0 - .5f,     y0 + h - .5f, 0, 1, fg, 0,  v1 },
            { x0 + w - .5f, y0 + h - .5f, 0, 1, fg, u1, v1 },
        };
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tq, sizeof(Vtx));

        sb->Apply();
        sb->Release();
    }

    // Draw onto the back buffer right before the frame is presented, so it is always on top and
    // always on screen, no matter which offscreen targets the game used during the frame.
    void DrawOnBackBuffer(IDirect3DDevice9* dev) {
        IDirect3DSurface9 *oldRT = nullptr, *oldDS = nullptr, *bb = nullptr;
        D3DVIEWPORT9 oldVp;
        dev->GetViewport(&oldVp);
        if (FAILED(dev->GetRenderTarget(0, &oldRT))) oldRT = nullptr;
        if (FAILED(dev->GetDepthStencilSurface(&oldDS))) oldDS = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
            if (oldRT) oldRT->Release();
            if (oldDS) oldDS->Release();
            return;
        }
        dev->SetRenderTarget(0, bb);       // also resets the viewport to the full back buffer
        dev->SetDepthStencilSurface(nullptr);

        if (SUCCEEDED(dev->BeginScene())) {
            DrawOverlay(dev);
            dev->EndScene();
        }

        if (g_frames == 1 || g_frames % 3600 == 0) {
            D3DSURFACE_DESC d{}; bb->GetDesc(&d);
            LOG("Overlay: frame %u, back buffer %ux%u, text %s", g_frames, d.Width, d.Height, g_texText.empty() ? "empty" : "set");
        }

        dev->SetRenderTarget(0, oldRT ? oldRT : bb);
        dev->SetDepthStencilSurface(oldDS);
        dev->SetViewport(&oldVp);
        if (oldRT) oldRT->Release();
        if (oldDS) oldDS->Release();
        bb->Release();
    }

    Present_t g_realPresent = nullptr; // first (d3d9.dll) Present we ever saw
    Reset_t   g_realReset = nullptr;
    bool      g_inPresent = false;

    // ---- hide the Windows cursor over the game (the game draws its own) ----
    // In windowed / borderless mode the system arrow stays visible on top of the game's cursor.
    HWND    g_gameWnd = nullptr;
    WNDPROC g_origWndProc = nullptr;
    bool    g_wndUnicode = false;

    void (*g_onClose)() = nullptr;

    LRESULT CALLBACK CursorWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        // Alt+F4 / X button / game quit: let the other player know right away.
        if ((msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_ENDSESSION || (msg == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE)) && g_onClose) {
            static int logged = 0;
            if (logged++ < 4) LOG("Window: closing message %04X (w %08X l %08X) from %p", msg, (unsigned)w, (unsigned)l, _ReturnAddress());
            g_onClose();
        }
        // A device was plugged in / removed (USB or Bluetooth pad): the hot-plug check runs on the next frames.
        if (msg == WM_DEVICECHANGE) Pad::OnDeviceChange();
        // Hide only while the game is the active window; a background game window can still get
        // WM_SETCURSOR when the pointer passes over it, and must not hide the user's cursor then.
        if (msg == WM_SETCURSOR && LOWORD(l) == HTCLIENT && g_cfg.hideCursor && GetForegroundWindow() == h) {
            SetCursor(nullptr);
            return TRUE;
        }
        // Losing focus (alt-tab, another window clicked): put the normal arrow back right away.
        if ((msg == WM_ACTIVATEAPP && !w) || msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE))
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return g_wndUnicode ? CallWindowProcW(g_origWndProc, h, msg, w, l)
                            : CallWindowProcA(g_origWndProc, h, msg, w, l);
    }

    void UpdateCursor(IDirect3DDevice9* dev) {
        if (!g_gameWnd) {
            D3DDEVICE_CREATION_PARAMETERS cp{};
            if (FAILED(dev->GetCreationParameters(&cp)) || !cp.hFocusWindow) return;
            g_gameWnd = cp.hFocusWindow;
            g_wndUnicode = IsWindowUnicode(g_gameWnd) != FALSE;
            g_origWndProc = (WNDPROC)(g_wndUnicode
                ? SetWindowLongPtrW(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)&CursorWndProc)
                : SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)&CursorWndProc));
            LOG("Cursor: subclassed game window %p (orig proc %p)", g_gameWnd, g_origWndProc);
        }
        if (!g_cfg.hideCursor) return;
        // WM_SETCURSOR only arrives when the mouse moves; also clear it while the game is focused
        // and the pointer is inside its client area (e.g. right after alt-tabbing back).
        // Only when the game is foreground AND the pointer is over the game's client area AND no
        // other window sits under the pointer (e.g. an always-on-top window above the game).
        if (GetForegroundWindow() != g_gameWnd) return;
        POINT pt; RECT rc;
        if (!GetCursorPos(&pt) || WindowFromPoint(pt) != g_gameWnd) return;
        POINT cp = pt;
        if (ScreenToClient(g_gameWnd, &cp) && GetClientRect(g_gameWnd, &rc) && PtInRect(&rc, cp))
            SetCursor(nullptr);
    }

    HRESULT STDMETHODCALLTYPE hk_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
        if (g_inPresent) return g_realPresent(dev, src, dst, wnd, dirty); // someone chained back into us
        g_inPresent = true;
        UpdateCursor(dev);
        IDirect3DDevice9* gameDev = nullptr;
        Mem::Read(0x982BDC, gameDev);
        if (gameDev && gameDev != g_dev) {
            LOG("Game device changed %p -> %p", g_dev, gameDev);
            g_dev = gameDev;
        }
        if (dev == g_dev) {
            ++g_frames;
            if (g_cb) g_cb(dev);
            if (!g_shotPath.empty()) { SaveBackBuffer(dev, g_shotPath); g_shotPath.clear(); }   // before our overlay
            if (g_visible || g_forceShow) DrawOnBackBuffer(dev);
        }
        HRESULT hr = o_Present(dev, src, dst, wnd, dirty);
        g_inPresent = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
        // our texture is D3DPOOL_MANAGED and state blocks are per-frame, so nothing to release
        HRESULT hr = o_Reset(dev, pp);
        LOG("Device Reset -> 0x%08X", (unsigned)hr);
        return hr;
    }

    void HookDevice(IDirect3DDevice9* dev) {
        void** vt = *(void***)dev;
        if (vt[17] != (void*)&hk_Present) {
            void* prevP = Mem::PatchPointer(&vt[17], (void*)&hk_Present);
            if (prevP != (void*)&hk_Present) o_Present = (Present_t)prevP;
            if (!g_realPresent) g_realPresent = o_Present;
            LOG("Hooked Present in vtable %p (prev %p)", vt, prevP);
        }
        if (vt[16] != (void*)&hk_Reset) {
            void* prevR = Mem::PatchPointer(&vt[16], (void*)&hk_Reset);
            if (prevR != (void*)&hk_Reset) o_Reset = (Reset_t)prevR;
            if (!g_realReset) g_realReset = o_Reset;
            LOG("Hooked Reset in vtable %p (prev %p)", vt, prevR);
        }
    }

    HRESULT STDMETHODCALLTYPE hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                              D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
        HRESULT hr = o_CreateDevice(d3d, adapter, type, wnd, flags, pp, out);
        LOG("CreateDevice -> 0x%08X (%ux%u windowed=%d)", (unsigned)hr,
            pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
        if (SUCCEEDED(hr) && out && *out) {
            if (g_tex) { g_tex->Release(); g_tex = nullptr; } // new device: drop old texture
            g_dev = *out;
            HookDevice(g_dev);
        }
        return hr;
    }

    IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
        IDirect3D9* d3d = o_Direct3DCreate9(sdk);
        LOG("Direct3DCreate9(%u) -> %p", sdk, d3d);
        if (d3d && !o_CreateDevice) {
            void** vt = *(void***)d3d;
            o_CreateDevice = (CreateDevice_t)Mem::PatchPointer(&vt[16], (void*)&hk_CreateDevice);
        }
        return d3d;
    }
}

// Background check (4x per second): something (d3d9 runtime / another mod) restores the original
// Present pointer during startup, so we re-hook when that happens. Logs hook health once a minute and re-hooks if another mod or the game
// replaced our vtable entries or switched to a new device.
static DWORD WINAPI Watchdog(LPVOID) {
    uint32_t lastFrames = 0;
    for (int tick = 0;; ++tick) {
        Sleep(250);
        IDirect3DDevice9* gameDev = nullptr;
        Mem::Read(0x982BDC, gameDev);
        void** vt = nullptr;
        if (gameDev) Mem::Read((uintptr_t)gameDev, vt);
        void* slot17 = nullptr;
        if (vt) Mem::Read((uintptr_t)&vt[17], slot17);
        bool hooked = slot17 == (void*)&hk_Present;
        if (!hooked || tick % 240 == 0) // on re-hook, and once a minute
            LOG("watchdog: frames %u (+%u)  gameDev %p  ourDev %p  vt %p  Present slot %p %s",
                g_frames, g_frames - lastFrames, gameDev, g_dev, vt, slot17, hooked ? "(ours)" : "(NOT OURS)");
        lastFrames = g_frames;
        if (gameDev && vt && !hooked && o_Present) {
            LOG("watchdog: re-hooking device %p", gameDev);
            HookDevice(gameDev);
        }
    }
    return 0;
}

bool Overlay::InstallHooks() {
    void** slot = (void**)IAT_Direct3DCreate9;
    if (!Mem::IsReadable(slot, 4)) { LOG("IAT slot not readable - wrong exe version?"); return false; }
    o_Direct3DCreate9 = (Direct3DCreate9_t)Mem::PatchPointer(slot, (void*)&hk_Direct3DCreate9);
    LOG("Hooked Direct3DCreate9 IAT (orig %p)", o_Direct3DCreate9);
    if (HANDLE th = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)) CloseHandle(th);
    return o_Direct3DCreate9 != nullptr;
}

void Overlay::SetFrameCallback(FrameCallback cb) { g_cb = cb; }

void Overlay::SetText(const std::string& text) {
    EnterCriticalSection(&g_textCs);
    g_text = text;
    LeaveCriticalSection(&g_textCs);
}

void Overlay::SetVisible(bool v) { g_visible = v; }
void Overlay::SetForceShow(bool v) { g_forceShow = v; }
void Overlay::Screenshot(const std::string& path) { g_shotPath = path; }
bool Overlay::IsVisible() { return g_visible; }
IDirect3DDevice9* Overlay::Device() { return g_dev; }
HWND Overlay::GameWindow() { return g_gameWnd; }
void Overlay::SetCloseCallback(void (*cb)()) { g_onClose = cb; }
