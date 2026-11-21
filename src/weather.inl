// Included from game.cpp.
// ---------------------------------------------------------------------------
// Weather: the same rain on both PCs.
//  Rain::Update (0x758100) asks every frame for the rain target (0x7539D0) and, when it isn't raining,
//  the cloud / wetness target (0x753AC0); both are float __cdecl(float x, float y). Offline the natural
//  rain model behind them runs on the time since the game booted, so two PCs disagree. The host reports
//  its values; the client returns the host's instead of its own (call sites 0x758351 / 0x758370).
// ---------------------------------------------------------------------------
namespace {
    using RainFn = float(__cdecl*)(float, float);
    float g_myRain = 0, g_myCloud = 0;
    bool UseHostWeather(float& rain, float& cloud) {
        return g_cfg.syncWeather && !g_cfg.isHost && g_coopLinked && Net::PeerWeather(rain, cloud);
    }
    float __cdecl hk_RainTarget(float x, float y) {
        float r, c;
        if (UseHostWeather(r, c)) return r;
        g_myRain = ((RainFn)0x7539D0)(x, y);
        return g_myRain;
    }
    float __cdecl hk_CloudTarget(float x, float y) {
        float r, c;
        if (UseHostWeather(r, c)) return c;
        g_myCloud = ((RainFn)0x753AC0)(x, y);
        return g_myCloud;
    }
}

bool Game::InstallWeatherSync() {
    bool a = PatchCallChecked(0x758351, 0x7539D0, (void*)&hk_RainTarget);
    bool b = PatchCallChecked(0x758370, 0x753AC0, (void*)&hk_CloudTarget);
    LOG("Weather: sync hooks %s/%s", a ? "ok" : "FAIL", b ? "ok" : "FAIL");
    return a && b;
}

// host: send the weather a few times a second
void Game::WeatherTick(bool connected) {
    static DWORD next = 0, nextLog = 0;
    if (!g_ok || !connected || !g_cfg.syncWeather || !g_cfg.isHost || GetTickCount() < next) return;
    next = GetTickCount() + 200;
    float cloud = g_myRain > 0 ? g_myRain : g_myCloud;        // when it rains the game uses the rain value for both
    Net::SendWeather(g_myRain, cloud);
    if (GetTickCount() >= nextLog) { nextLog = GetTickCount() + 60000; LOG("Weather: sending rain %.2f cloud %.2f", g_myRain, cloud); }
}
