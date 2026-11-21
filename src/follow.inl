// Included from game.cpp (after stream.inl, coopwin.inl).
// ---------------------------------------------------------------------------
// Going together to a Blacklist pursuit (milestone / bounty, started from the Blacklist screen): the game
// asks "start?" (InGameDialog.fng) and then moves the player's car to the pursuit's start. The player who
// started it sends where they were moved; the other player's game loads the world there and puts their car
// behind them, a little to the left, facing the same way. The pursuit then starts on both (shared pursuits).
// ([Coop] FollowToPursuits)
// ---------------------------------------------------------------------------
namespace {
    struct FollowTarget { bool pending = false; float pos[3], fwd[3], right[3], quat[4]; DWORD loadAt = 0, moveAt = 0; };
    FollowTarget g_follow;

    void FollowSenderTick(void* me) {
        static DWORD dialogAt = 0; static float last[3]; static bool haveLast = false;
        DWORD now = GetTickCount();
        if (FEScreens::IsPushed("InGameDialog.fng")) dialogAt = now;
        void* rb = RigidBodyOf(me);
        if (!rb) { haveLast = false; return; }
        const float* p = RB_GetVec(rb, RB_GetPosition);
        if (haveLast && Dist(last, p) > 300.f && dialogAt && now - dialogAt < 6000 && !Game::CurrentEventHash()) {
            float f[3], r[3];
            VCall<void(__thiscall*)(void*, float*)>(rb, 13)(rb, f);
            VCall<void(__thiscall*)(void*, float*)>(rb, 14)(rb, r);
            const float* q = RB_GetVec(rb, RB_GetOrientation);
            Net::SendFollow(p, f, r, q);
            dialogAt = 0;
            LOG("Follow: we were moved %.0f m after the Blacklist dialog - asking the other player to come along", Dist(last, p));
        }
        memcpy(last, p, 12); haveLast = true;
    }

    void FollowReceiverTick(void* me) {
        if (!g_follow.pending) return;
        DWORD now = GetTickCount();
        void* rb = RigidBodyOf(me);
        if (!rb || Game::CurrentEventHash() || now - g_follow.loadAt > 30000) { g_follow.pending = false; return; }
        float spot[3];
        for (int i = 0; i < 3; ++i) spot[i] = g_follow.pos[i] - g_follow.fwd[i] * 8.f - g_follow.right[i] * 3.f;
        spot[1] += 0.5f;
        if (Dist(RB_GetVec(rb, RB_GetPosition), g_follow.pos) < 150.f) { g_follow.pending = false; return; }   // already there
        if ((int)(now - g_follow.moveAt) < 0) return;                       // the world there is still loading
        float zero[3] = { 0, 0, 0 };
        g_testJumpOkUntil = now + 500;
        RB_SetVec(rb, RB_SetPosition, spot); RB_SetVec(rb, RB_SetLinearVelocity, zero); RB_SetVec(rb, RB_SetAngularVelocity, zero);
        VCall<void(__thiscall*)(void*, const float*)>(rb, RB_SetOrientationQuat)(rb, g_follow.quat);
        g_follow.pending = false;
        LOG("Follow: moved behind the other player to their Blacklist pursuit");
    }
    void FollowTickSafe(bool linked) {
        __try {
            void* me = LocalVehicle();
            if (!me || Game::GameFlowState() != 6) return;
            if (linked && g_cfg.followToPursuits) FollowSenderTick(me);
            if (linked) FollowReceiverTick(me);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_follow.pending = false; }
    }
}

void Game::FollowTick(bool linked) { if (g_ok) FollowTickSafe(linked); }
void Game::InstallFollow() {
    Net::OnFollow = [](const float* pos, const float* fwd, const float* right, const float* quat) {
        if (!g_cfg.followToPursuits || !g_coopLinked || Game::GameFlowState() != 6 || Game::CurrentEventHash()) return;
        memcpy(g_follow.pos, pos, 12); memcpy(g_follow.fwd, fwd, 12); memcpy(g_follow.right, right, 12); memcpy(g_follow.quat, quat, 16);
        g_follow.loadAt = GetTickCount(); g_follow.moveAt = g_follow.loadAt + 4000; g_follow.pending = true;
        StreamHoldAt(pos, 20000);                                            // load the world there first
        LOG("Follow: the other player went to a Blacklist pursuit - loading the world there, moving in 4 s");
        { std::string m = (Net::Stats().peerName.empty() ? std::string("The other player") : Net::Stats().peerName) + " started a Blacklist pursuit - taking you there"; Game::QueueNotice(m, false); }
    };
}
