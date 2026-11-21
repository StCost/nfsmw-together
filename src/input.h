#pragma once

// DirectInput hooks: lets the mod press keys inside the game (the game reads the keyboard through
// DirectInput, so synthetic Windows key events are ignored). Used by AutoStart to get from the
// title screen into the career without anyone touching the menus.
namespace Input {
    bool InstallHooks();
    void TapBackground(unsigned char dik);   // test: a key press the game sees without focus (keyboard device state)              // IAT hook on DirectInput8Create; call from DllMain

    // Press-and-release a DirectInput key (DIK_* scan code) over the next few frames.
    bool Tap(unsigned char dik);      // false if not sent (game window not focused / busy)
    bool Busy();                      // a tap is in progress

    // Called once per frame from the game thread.
    void Tick();
}

// Front-end screens (FEng packages). Lets AutoStart wait for the right menu before pressing keys.
namespace FEScreens {
    bool IsPushed(const char* package);  // cFEng::IsPackagePushed (0x516B50) on cFEng::mInstance (0x91CADC)
    void Tick();                          // ~2x/s: logs when the set of open screens changes
    const char* Current();                // comma-separated open screens (from a known list)
}

// Macro: run a short key/screen sequence in any game state (same step syntax as AutoStart),
// e.g. "@EngageEventDialog.fng,ENTER". Aborts after timeoutMs.
namespace Macro {
    void Run(const char* sequence, int timeoutMs);
    void Tick();
    bool Active();
}

// Game controllers (pad.cpp): pads connected / reconnected while the game runs ([Input] GamepadHotplug)
// and keeping the player's controller bindings when the game starts without the pad
// ([Input] KeepPadBindings).
namespace Pad {
    bool InstallHooks();
    void TapBackground(unsigned char dik);   // test: a key press the game sees without focus (keyboard device state)              // call from DllMain, after the config is loaded
    void OnDeviceChange();            // WM_DEVICECHANGE on the game window
    void Tick(int gameFlowState);     // once per frame, game thread
}

// Auto-start: from the title screen, keep tapping Enter until the game starts loading the career.
namespace AutoStart {
    void Tick(int gameFlowState);
    const char* Status();
}
