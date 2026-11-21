#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstring>
#include <cstdio>
#include "input.h"
#include "config.h"
#include "log.h"
#include "mem.h"
#include "overlay.h"

// speed.exe v1.3: import address table slot for DINPUT8!DirectInput8Create (verified from the import table)
static constexpr uintptr_t IAT_DirectInput8Create = 0x89001C;

namespace {
    // GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000}
    const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

    using DI8Create_t      = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    using CreateDevice_t   = HRESULT(STDMETHODCALLTYPE*)(void* di, REFGUID, void** dev, LPUNKNOWN);
    using GetState_t       = HRESULT(STDMETHODCALLTYPE*)(void* dev, DWORD cb, LPVOID data);
    using GetData_t        = HRESULT(STDMETHODCALLTYPE*)(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags);

    DI8Create_t    o_DI8Create = nullptr;
    CreateDevice_t o_CreateDevice = nullptr;
    GetState_t     o_GetState = nullptr;
    GetData_t      o_GetData = nullptr;

    void* g_keyboard = nullptr;   // the game's keyboard device

    // tap state
    unsigned char g_dik = 0;
    bool     g_down = false;        // key currently held (for GetDeviceState)
    DWORD    g_downAt = 0;
    bool     g_pendPress = false;   // event not yet delivered via GetDeviceData
    bool     g_pendRelease = false;
    DWORD    g_seq = 0;
    bool     g_loggedState = false, g_loggedData = false;
    UINT     g_postedVk = 0;
    bool     g_postedExt = false;

    HRESULT STDMETHODCALLTYPE hk_GetState(void* dev, DWORD cb, LPVOID data) {
        HRESULT hr = o_GetState(dev, cb, data);
        if (dev == g_keyboard && cb == 256 && data) {
            if (!g_loggedState) { g_loggedState = true; LOG("Input: game polls keyboard with GetDeviceState"); }
            // (no injection here any more: keys go through SendInput, see Input::Tap)
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE hk_GetData(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags) {
        DWORD capacity = inOut ? *inOut : 0;
        HRESULT hr = o_GetData(dev, cbObj, rg, inOut, flags);
        if (dev == g_keyboard && rg && inOut && SUCCEEDED(hr) && cbObj >= sizeof(DIDEVICEOBJECTDATA_DX3) && !(flags & DIGDD_PEEK)) {
            if (!g_loggedData) { g_loggedData = true; LOG("Input: game reads keyboard with GetDeviceData"); }
            auto add = [&](BYTE value) {
                if (*inOut >= capacity) return false;
                auto* e = (DIDEVICEOBJECTDATA*)((BYTE*)rg + (*inOut) * cbObj);
                memset(e, 0, cbObj);
                e->dwOfs = g_dik;
                e->dwData = value;
                e->dwTimeStamp = GetTickCount();
                e->dwSequence = ++g_seq;
                (*inOut)++;
                return true;
            };
            (void)add;
        }
        return hr;
    }

    void HookDevice(void* dev) {
        void** vt = *(void***)dev;
        if (!o_GetState && vt[9] != (void*)&hk_GetState) {
            o_GetState = (GetState_t)Mem::PatchPointer(&vt[9], (void*)&hk_GetState);
            o_GetData  = (GetData_t)Mem::PatchPointer(&vt[10], (void*)&hk_GetData);
            LOG("Input: hooked DirectInput device vtable %p", vt);
        }
    }

    HRESULT STDMETHODCALLTYPE hk_CreateDevice(void* di, REFGUID guid, void** dev, LPUNKNOWN outer) {
        HRESULT hr = o_CreateDevice(di, guid, dev, outer);
        if (SUCCEEDED(hr) && dev && *dev) {
            bool kb = IsEqualGUID(guid, kSysKeyboard) != 0;
            if (kb) g_keyboard = *dev;
            HookDevice(*dev); // keyboard and mouse share the vtable; we filter by device pointer
            LOG("Input: CreateDevice %s -> %p", kb ? "keyboard" : "other", *dev);
        }
        return hr;
    }

    HRESULT WINAPI hk_DI8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
        HRESULT hr = o_DI8Create(inst, ver, riid, out, outer);
        if (SUCCEEDED(hr) && out && *out && !o_CreateDevice) {
            void** vt = **(void****)out;
            o_CreateDevice = (CreateDevice_t)Mem::PatchPointer(&vt[3], (void*)&hk_CreateDevice);
            LOG("Input: hooked IDirectInput8::CreateDevice");
        }
        return hr;
    }
}

bool Input::InstallHooks() {
    void** slot = (void**)IAT_DirectInput8Create;
    if (!Mem::IsReadable(slot, 4)) return false;
    o_DI8Create = (DI8Create_t)Mem::PatchPointer(slot, (void*)&hk_DI8Create);
    LOG("Input: hooked DirectInput8Create IAT (orig %p)", o_DI8Create);
    return o_DI8Create != nullptr;
}

// Keys are sent as real keyboard events (SendInput with scan codes). The game's menus read arrow keys
// from real input only: DirectInput-state injection worked for Enter but not for arrows. SendInput goes
// to whatever window is focused, so we ONLY send while the game window is in the foreground.
static void SendScan(unsigned char dik, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = dik & 0x7F;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | ((dik & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    SendInput(1, &in, sizeof(in));
}

bool Input::Tap(unsigned char dik) {
    if (Busy()) return false;
    HWND w = Overlay::GameWindow();
    if (!w || GetForegroundWindow() != w) return false;   // never type into another app
    g_dik = dik;
    g_down = true;
    g_downAt = GetTickCount();
    SendScan(dik, false);
    return true;
}

bool Input::Busy() { return g_down; }

void Input::Tick() {
    if (g_down && GetTickCount() - g_downAt > 120) {             // hold ~120 ms, then release
        g_down = false;
        SendScan(g_dik, true);                                    // release even if focus moved
    }
}

// ---------------------------------------------------------------------------
// FE screens
namespace {
    const char* kScreens[] = {
        "PressStart.fng",
        "MW_LS_Splash.fng",
        "WS_MW_LS_Splash.fng",
        "MW_LS_AttractFMV.fng",
        "WS_MW_LS_AttractFMV.fng",
        "MC_Bootup.fng",
        "MC_Main.fng",
        "MC_ProfileManager.fng",
        "LS_LangSelect.fng",
        "MainMenu.fng",
        "MainMenu_Sub.fng",
        "UI_QRModeOptions.fng",
        "Track_Select.fng",
        "Track_Options.fng",
        "Car_Select.fng",
        "Quick_Race_Brief.fng",
        "Dialog.fng",
        "GenericDialog_MED.fng",
        "GenericDialog_ThreeButton.fng",
        "InGameDialog.fng",
        "EngageEventDialog.fng",
        "GarageMain.fng",
        "MyCarsManager.fng",
        "SafeHouseRaceSheet.fng",
        "SafehouseRaceSheet.fng",
        "RapSheetMain.fng",
        "WorldMapMain.fng",
        "Options.fng",
        "Loading.fng",
        "WS_Loading.fng",
        "PC_Loading.fng",
        "Loading_Tips.fng",
        "loading_boot.fng",
        "FadeScreenNoLoadingBar.fng",
        "Pause_Main.fng",
        "PostRace_Results.fng",
        "InGamePhotoMaster.fng",
        "InGameRivalChallenge.fng",
        "InGameRivalBio.fng",
        "InGameRaceSheet.fng",
        "EngageEventDialog.fng",
        "PostRace_Pursuit.fng",
        "PostRace_MilestoneRewards.fng",
        "InGameBounty.fng",
        "Infractions.fng",
        "HUD_SingleRace.fng",
        "InGameRaceSheet.fng"
    };
    char  g_screens[512] = "";
    DWORD g_lastScreenPoll = 0;
}

static bool IsPushedRaw(const char* package) {
    __try {
        void* fe = *(void**)0x91CADC;
        if (!fe) return false;
        using Fn = bool(__thiscall*)(void*, const char*);
        return ((Fn)0x516B50)(fe, package);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FEScreens::IsPushed(const char* package) { return IsPushedRaw(package); }

void FEScreens::Tick() {
    DWORD now = GetTickCount();
    if (now - g_lastScreenPoll < 300) return;
    g_lastScreenPoll = now;
    char buf[512] = "";
    for (const char* s : kScreens) {
        if (IsPushedRaw(s)) {
            if (buf[0]) strcat_s(buf, ", ");
            strcat_s(buf, s);
        }
    }
    if (strcmp(buf, g_screens) != 0) {
        strcpy_s(g_screens, buf);
        LOG("FE screens: [%s]", g_screens);
    }
}

const char* FEScreens::Current() { return g_screens; }

// ---------------------------------------------------------------------------
namespace {
    enum class AS { Off, WaitFrontEnd, Pressing, Done, GaveUp };
    AS       g_as = AS::WaitFrontEnd;
    DWORD    g_feSince = 0, g_lastPress = 0;
    int      g_presses = 0;
    char     g_status[96] = "waiting for menu";

    // Parsed [AutoStart] Sequence. Steps:
    //   ENTER LEFT RIGHT UP DOWN ESC  - press that key (one per IntervalMs)
    //   WAIT                          - do nothing for one interval
    //   @Name.fng                     - wait until that front-end screen is open (max 30 s), then let it
    //                                   settle ~0.8 s before the next key
    struct Step { unsigned char key; char screen[48]; char name[48]; };
    Step g_steps[48];
    int  g_seqLen = -1;
    DWORD g_stepSince = 0;          // when we started waiting on the current @screen step
    DWORD g_screenSeenAt = 0;       // when the awaited screen appeared
    int   g_retries = 0;            // re-presses of the previous key while waiting for a screen

    void ParseSequence() {
        g_seqLen = 0;
        char buf[1024];
        // Target: freeroam = the configured Sequence; safehouse = stop on the "Resume Free Roam" screen;
        // race = the Quick Race test.
        const char* seq = g_cfg.autoStartSequence.c_str();
        if (!_stricmp(g_cfg.autoStartTarget.c_str(), "safehouse"))
            seq = "@WS_MW_LS_Splash.fng,ENTER,@MC_Main.fng,ENTER,@MainMenu.fng,ENTER,@MainMenu_Sub.fng,ENTER";
        else if (!_stricmp(g_cfg.autoStartTarget.c_str(), "race"))
            seq = "@WS_MW_LS_Splash.fng,ENTER,@MC_Main.fng,ENTER,@MainMenu.fng,RIGHT,RIGHT,ENTER,@MainMenu_Sub.fng,RIGHT,ENTER,WAIT,ENTER,@Track_Select.fng,ENTER,@Track_Options.fng,LEFT,DOWN,DOWN,LEFT,LEFT,ENTER,@Car_Select.fng,ENTER,@Dialog.fng,ENTER";
        strncpy_s(buf, seq, _TRUNCATE);
        char* ctx = nullptr;
        for (char* tok = strtok_s(buf, " ,;", &ctx); tok && g_seqLen < 48; tok = strtok_s(nullptr, " ,;", &ctx)) {
            Step st{};
            if (tok[0] == '@') {
                strncpy_s(st.screen, tok + 1, _TRUNCATE);
                strncpy_s(st.name, tok, _TRUNCATE);
                g_steps[g_seqLen++] = st;
                continue;
            }
            struct { const char* n; unsigned char k; } keys[] = {
                {"ENTER", DIK_RETURN}, {"LEFT", DIK_LEFT}, {"RIGHT", DIK_RIGHT}, {"UP", DIK_UP},
                {"DOWN", DIK_DOWN}, {"ESC", DIK_ESCAPE}, {"WAIT", 0} };
            bool ok = false;
            for (auto& k : keys) if (!_stricmp(tok, k.n)) { st.key = k.k; strcpy_s(st.name, k.n); ok = true; break; }
            if (ok) g_steps[g_seqLen++] = st;
            else LOG("AutoStart: unknown step '%s' ignored", tok);
        }
        LOG("AutoStart: sequence has %d steps (%s)", g_seqLen, g_cfg.autoStartSequence.c_str());
    }
}

// A sequence step's screen: the title screen is WS_MW_LS_Splash.fng with a widescreen fix and
// MW_LS_Splash.fng in the plain game - a step for either matches both.
static bool StepScreenOpen(const char* screen) {
    if (FEScreens::IsPushed(screen)) return true;
    if (!_stricmp(screen, "WS_MW_LS_Splash.fng")) return FEScreens::IsPushed("MW_LS_Splash.fng");
    if (!_stricmp(screen, "MW_LS_Splash.fng")) return FEScreens::IsPushed("WS_MW_LS_Splash.fng");
    return false;
}

void AutoStart::Tick(int flow) {
    if (!g_cfg.autoStart) { g_as = AS::Off; strcpy_s(g_status, "off"); return; }
    if (g_as == AS::Done || g_as == AS::GaveUp || g_as == AS::Off) return;

    // A human pressing Esc cancels it, so it never fights a person in the menus.
    if (g_presses > 0 && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        g_as = AS::GaveUp; strcpy_s(g_status, "cancelled (Esc)"); LOG("AutoStart: cancelled by Esc"); return;
    }

    DWORD now = GetTickCount();
    if (flow == 3) {
        if (!g_feSince) g_feSince = now;
        if (g_as == AS::WaitFrontEnd && now - g_feSince > (DWORD)g_cfg.autoStartDelayMs) g_as = AS::Pressing;
        if (g_as != AS::Pressing || Input::Busy()) return;
        if (g_seqLen < 0) ParseSequence();
        if (g_presses >= g_seqLen) {
            if (!_stricmp(g_cfg.autoStartTarget.c_str(), "safehouse")) {
                g_as = AS::Done;
                strcpy_s(g_status, "done - at the Safe House");
                LOG("AutoStart: reached the Safe House (target safehouse)");
                return;
            }
            if (now - g_lastPress < 6000) { strcpy_s(g_status, "sequence done, waiting for loading"); return; }
            // Still in the menus: retry the tail from the key that opened the last awaited screen
            // (e.g. a dialog answered "No" because a key was swallowed), up to 3 times.
            static int tailRetries = 0;
            int lastScreen = -1;
            for (int j = g_seqLen - 1; j >= 0; --j) if (g_steps[j].screen[0]) { lastScreen = j; break; }
            if (lastScreen > 0 && g_steps[lastScreen - 1].key && tailRetries < 3) {
                ++tailRetries;
                g_presses = lastScreen - 1;
                g_stepSince = g_screenSeenAt = 0;
                g_retries = 0;
                g_lastPress = now - (DWORD)g_cfg.autoStartIntervalMs;
                LOG("AutoStart: still in the menus - retrying from step %d (%d/3)", g_presses + 1, tailRetries);
                return;
            }
            g_as = AS::GaveUp;
            strcpy_s(g_status, "sequence finished, still in menu - continue manually");
            LOG("AutoStart: sequence finished but game is still in the front end");
            return;
        }
        Step& st = g_steps[g_presses];

        if (st.screen[0]) {
            // wait for a screen
            if (!g_stepSince) g_stepSince = now;
            // Already further along (a screen was skipped, or retries advanced the menus)? Jump ahead to
            // the latest later step whose screen is open.
            for (int j = g_seqLen - 1; j > g_presses; --j) {
                if (g_steps[j].screen[0] && StepScreenOpen(g_steps[j].screen)) {
                    LOG("AutoStart: %s already open - skipping ahead from step %d to %d", g_steps[j].screen, g_presses + 1, j + 1);
                    g_presses = j;
                    g_stepSince = g_screenSeenAt = 0;
                    g_retries = 0;
                    return;
                }
            }
            if (StepScreenOpen(st.screen)) {
                if (!g_screenSeenAt) g_screenSeenAt = now;
                if (now - g_screenSeenAt < 1500) return;         // let it finish animating in (dialogs ignore early keys)
                LOG("AutoStart: step %d/%d %s reached", g_presses + 1, g_seqLen, st.name);
                ++g_presses;
                g_stepSince = g_screenSeenAt = 0;
                g_retries = 0;
                g_lastPress = now - (DWORD)g_cfg.autoStartIntervalMs; // next key right away
                return;
            }
            g_screenSeenAt = 0;
            // The previous key may have been swallowed (e.g. "Press START" ignores input until its intro
            // animation ends): re-press it every 3 s while the expected screen hasn't appeared.
            if (g_presses > 0 && g_steps[g_presses - 1].key && now - g_lastPress > 3000 && g_retries < 8) {
                if (Input::Tap(g_steps[g_presses - 1].key)) {
                    g_lastPress = now;
                    ++g_retries;
                    LOG("AutoStart: %s not open yet - pressing %s again (%d)", st.screen, g_steps[g_presses - 1].name, g_retries);
                }
            }
            if (now - g_stepSince > 30000) {
                g_as = AS::GaveUp;
                sprintf_s(g_status, "gave up waiting for %s", st.screen);
                LOG("AutoStart: %s (open screens: %s)", g_status, FEScreens::Current());
                return;
            }
            sprintf_s(g_status, "step %d/%d: waiting for %s", g_presses + 1, g_seqLen, st.screen);
            return;
        }

        if (now - g_lastPress < (DWORD)g_cfg.autoStartIntervalMs) return;
        if (st.key && !Input::Tap(st.key)) {
            strcpy_s(g_status, "paused - click the game window");
            return;                       // game not focused: wait, don't advance
        }
        g_lastPress = now;
        ++g_presses;
        sprintf_s(g_status, "step %d/%d: %s", g_presses, g_seqLen, st.name);
        LOG("AutoStart: step %d/%d %s (screens: %s)", g_presses, g_seqLen, st.name, FEScreens::Current());
    } else if (flow == 4 || flow == 5 || flow == 6) {
        if (g_presses > 0 || g_feSince) {
            g_as = AS::Done;
            sprintf_s(g_status, "done (%d steps)", g_presses);
            LOG("AutoStart: game is loading/in game after %d steps - done", g_presses);
        }
    }
}

const char* AutoStart::Status() { return g_status; }


// ---------------------------------------------------------------------------
// Macro: ad-hoc key sequences in any game state
namespace {
    struct MStep { unsigned char key; char screen[48]; };
    MStep g_mSteps[16];
    int   g_mLen = 0, g_mPos = 0;
    DWORD g_mStart = 0, g_mTimeout = 0, g_mLast = 0, g_mSeen = 0;
}

void Macro::Run(const char* sequence, int timeoutMs) {
    g_mLen = g_mPos = 0;
    char buf[256];
    strncpy_s(buf, sequence, _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, " ,;", &ctx); tok && g_mLen < 16; tok = strtok_s(nullptr, " ,;", &ctx)) {
        MStep st{};
        if (tok[0] == '@') strncpy_s(st.screen, tok + 1, _TRUNCATE);
        else if (!_stricmp(tok, "ENTER")) st.key = DIK_RETURN;
        else if (!_stricmp(tok, "LEFT")) st.key = DIK_LEFT;
        else if (!_stricmp(tok, "RIGHT")) st.key = DIK_RIGHT;
        else if (!_stricmp(tok, "UP")) st.key = DIK_UP;
        else if (!_stricmp(tok, "DOWN")) st.key = DIK_DOWN;
        else if (!_stricmp(tok, "ESC")) st.key = DIK_ESCAPE;
        else continue;
        g_mSteps[g_mLen++] = st;
    }
    g_mStart = GetTickCount();
    g_mTimeout = (DWORD)timeoutMs;
    g_mSeen = 0;
    LOG("Macro: started (%s), %d steps", sequence, g_mLen);
}

bool Macro::Active() { return g_mPos < g_mLen; }

void Macro::Tick() {
    if (g_mPos >= g_mLen) return;
    DWORD now = GetTickCount();
    if (now - g_mStart > g_mTimeout) { LOG("Macro: timed out at step %d", g_mPos + 1); g_mPos = g_mLen = 0; return; }
    if (Input::Busy()) return;
    MStep& st = g_mSteps[g_mPos];
    if (st.screen[0]) {
        if (!FEScreens::IsPushed(st.screen)) { g_mSeen = 0; return; }
        if (!g_mSeen) g_mSeen = now;
        if (now - g_mSeen < 600) return;        // let the dialog finish appearing
        LOG("Macro: %s is open", st.screen);
        ++g_mPos; g_mLast = 0;
        return;
    }
    if (now - g_mLast < 500) return;
    if (!Input::Tap(st.key)) return;            // game not focused: wait
    g_mLast = now;
    ++g_mPos;
    if (g_mPos >= g_mLen) LOG("Macro: done");
}
