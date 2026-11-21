// Included from game.cpp (uses its vehicle helpers).
// ---------------------------------------------------------------------------
// Traffic sync
//  Host: owns traffic. ~20x/s sends its traffic cars nearest to the client (when the client is near).
//  Client: near the host (enter 350 m / leave 450 m) it zeroes its own traffic density (hook on the
//  ComputeDensity call at 0x439BBD), removes its own traffic cars and spawns "mirror" cars that copy
//  the host's traffic (no AI: hard-set, interpolated transforms). Far from the host it keeps its own.
//  In a race the client keeps its own traffic (race scripts own some of it). With [Traffic]
//  SharedInRaces=1 its own traffic cars are bound to the host's cars and moved like mirrors instead
//  (nothing spawned, deleted or AI-disabled); with 0 each player sees their own traffic in races.
//  [Traffic] SmoothHandover=1 (client, free roam): leaving the host, each host-car copy becomes one of
//  our own traffic cars (traffic AI, adopted by the game's traffic manager) instead of vanishing.
//  Coming near, our own traffic cars stay until they are off screen, and a host car only appears where
//  the camera can't see it (or one of our cars in / near its spot takes it over and glides there).
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t DensityCallSite  = 0x439BBD;   // call AITrafficManager::ComputeDensity (0x41EC20)
    constexpr uintptr_t ComputeDensityFn = 0x41EC20;   // returns the traffic density (float, ST0)
    bool  g_suppressTraffic = false;   // also suppresses the client's own cop spawns

    // AICopManager spawn calls (thiscall). Redirected so the client spawns no cops near the host.
    constexpr uintptr_t PatrolCallSite   = 0x4315CF, SpawnPatrolFn  = 0x430D90; // bool SpawnPatrolCar()
    constexpr uintptr_t PursuitCallSite1 = 0x42ED7C, PursuitCallSite2 = 0x42ED9B, SpawnPursuitFn = 0x42EA90; // bool SpawnPursuitCar(IPursuit*)
    bool __fastcall hk_SpawnPatrol(void* self, void*) {
        if (g_suppressTraffic) return false;
        return ((bool(__fastcall*)(void*, void*))SpawnPatrolFn)(self, nullptr);
    }
    bool __fastcall hk_SpawnPursuit(void* self, void*, void* pursuit) {
        // pursuit cops are always allowed: every player owns the cops of their own pursuit
        // (the other player sees them as mirrors)
        return ((bool(__fastcall*)(void*, void*, void*))SpawnPursuitFn)(self, nullptr, pursuit);
    }
    bool PatchCallChecked(uintptr_t site, uintptr_t expect, void* to) {
        uint8_t op = 0; int32_t rel = 0;
        if (!Mem::Read(site, op) || !Mem::Read(site + 1, rel) || op != 0xE8 || site + 5 + rel != expect) return false;
        Mem::PatchCall(site, to);
        return true;
    }
    uint32_t& GlareBits(void* iv) { return *(uint32_t*)((uint8_t*)iv + 0x9C); }

    float __fastcall hk_ComputeDensity(void* self, void* edx) {
        using Fn = float(__fastcall*)(void*, void*);
        float d = ((Fn)ComputeDensityFn)(self, edx);
        return g_suppressTraffic ? 0.0f : d;
    }

    struct MirrorSample { double sendMs; TrafficEntry e; };
    struct Mirror {
        bool     used = false;
        uint16_t id = 0;
        uint32_t model = 0;
        void*    iv = nullptr;
        MirrorSample s[4];
        int      n = 0;        // valid samples (newest at index n-1)
        double   lastSeen = 0; // local ms
        int      fails = 0;
        bool     racer = false; // a race opponent of the host: drives one of our own racers (no spawn)
        bool     bound = false; // in a race: iv is one of OUR OWN traffic cars following the host's car
                                // (never killed, its AI is left alone: unbinding just lets it drive on)
        char     name[31] = {};  // its display name on the host (race opponents)
        // free roam (SmoothHandover): one of our own cars took this host car over and glides from where
        // it was to the host car's place instead of jumping there
        bool     blendPending = false; // start the glide at the next drive (from where the car is then)
        DWORD    blendT0 = 0;    // GetTickCount when the glide started (0 = none)
        float    blendOff[3] = {};
        float    blendRot[4] = {};
    };
    Mirror g_mirrors[48];
    bool   g_trafficNear = false;
    // In a race the client never removes or replaces its own traffic: race scripts own some of those
    // cars (drag races), and removing one crashed the game (0x5FD94A). Cops and race opponents are
    // still mirrored. With [Traffic] SharedInRaces the host's traffic cars are shown by binding each one
    // to one of our own traffic cars (see BindRaceTraffic): no car is spawned, deleted or has its AI
    // switched off; our spare traffic cars keep driving on their own.
    bool   g_clientRacing = false;
    bool   SharedRaceTraffic() { return !g_cfg.isHost && g_clientRacing && g_cfg.trafficSharedInRaces; }
    DWORD  g_raceEndedTick = 0;        // the race scripts may still own cars for a moment: no culling for 8 s
    int    g_mirrorCount = 0, g_killedOwn = 0;

    Mirror* FindMirror(uint16_t id) {
        for (auto& m : g_mirrors) if (m.used && m.id == id) return &m;
        return nullptr;
    }
    bool IsMirrorCar(void* iv) {
        for (auto& m : g_mirrors) if (m.used && m.iv == iv) return true;
        return false;
    }

    using NetUtil::CLIENT_ID_BIT;   // ids of cars owned by the client (its pursuit cops), see netutil.h

    double g_lastTrafficPkt = 0;   // local ms of the owner's last traffic packet

    void OnTrafficPacket(const TrafficEntry* e, int count, double sendMs) {
        double now = Net::LocalClockMs();
        g_lastTrafficPkt = now;
        for (int i = 0; i < count; ++i) {
            // the host accepts only the client's own cops; the client accepts the host's cars
            if (!NetUtil::TrafficIdAccepted(e[i].id, g_cfg.isHost)) continue;
            bool racer = (e[i].flags & 2) != 0;
            if (!g_cfg.isHost && !racer && !g_trafficNear) continue;   // far from the host: own traffic
            if (!g_cfg.isHost && !racer && g_clientRacing && !g_cfg.trafficSharedInRaces && !(e[i].flags & 1)) continue;   // in a race: own traffic too
            Mirror* m = FindMirror(e[i].id);
            if (!m) {
                for (auto& slot : g_mirrors) if (!slot.used) { slot = Mirror{}; slot.used = true; slot.id = e[i].id; m = &slot; break; }
                if (!m) continue;
            }
            m->lastSeen = now;   // the owner still has it (its clock stands still while its game is paused)
            if (m->n && sendMs <= m->s[m->n - 1].sendMs) continue; // out of order / duplicate
            if (m->n == 4) { memmove(&m->s[0], &m->s[1], 3 * sizeof(MirrorSample)); m->n = 3; }
            m->s[m->n].sendMs = sendMs;
            m->s[m->n].e = e[i];
            m->n++;
            m->model = e[i].model;
            m->racer = racer;
        }
    }

    // GRacerInfo of a car in the current race (GRaceStatus::GetRacerInfo(ISimable*)), or null
    void* RacerInfoOf(void* iv) {
        void* rs = nullptr;
        if (!IsVehicle(iv) || !Mem::Read(0x91E000, rs) || !rs) return nullptr;
        void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
        return sim ? ((void*(__thiscall*)(void*, void*))0x5E8B20)(rs, sim) : nullptr;
    }
    void OnRacerName(uint16_t id, const char* name) {
        if (Mirror* m = FindMirror(id)) NetUtil::CopyHudName(m->name, name);        // HUD slots are 16 bytes
    }

    void RemoveMirror(Mirror& m) {
        if (m.racer) { if (m.iv) RestoreAI(m.iv); }          // our own racer: give it its AI back
        else if (m.bound) {}                                  // our own traffic car: it just drives on
        else if (m.iv && !WasRemovedByGame(m.iv) && ListContains(-1, m.iv)) KillVehicle(m.iv);   // never kill a car the game built in its place
        m = Mirror{};
    }

    void DriveMirror(Mirror& m, double renderSendMs) {
        if (!m.iv || m.n == 0) return;
        void* rb = RigidBodyOf(m.iv);
        if (!rb) return;
        const MirrorSample* a = nullptr; const MirrorSample* b = nullptr;
        for (int i = m.n - 1; i >= 0; --i) {
            if (m.s[i].sendMs <= renderSendMs) { a = &m.s[i]; b = (i + 1 < m.n) ? &m.s[i + 1] : nullptr; break; }
        }
        float pos[3], vel[3], av[3];
        Quat q;
        if (a && b) {
            double span = b->sendMs - a->sendMs;
            float u = span > 0 ? (float)((renderSendMs - a->sendMs) / span) : 1.f;
            float dt = (float)(span / 1000.0);
            float u2 = u * u, u3 = u2 * u;
            float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
            for (int i = 0; i < 3; ++i) {
                pos[i] = h00 * a->e.pos[i] + h10 * dt * a->e.linVel[i] + h01 * b->e.pos[i] + h11 * dt * b->e.linVel[i];
                vel[i] = a->e.linVel[i] + (b->e.linVel[i] - a->e.linVel[i]) * u;
                av[i]  = a->e.angVel[i] + (b->e.angVel[i] - a->e.angVel[i]) * u;
            }
            q = Nlerp(*(const Quat*)a->e.rot, *(const Quat*)b->e.rot, u);
        } else {
            const MirrorSample& n = m.s[m.n - 1];
            float ex = (float)((renderSendMs - n.sendMs) / 1000.0);
            if (ex < 0) ex = 0;
            bool stale = ex > 0.25f;   // no fresh data (the owner's game paused): hold still
            if (ex > 0.25f) ex = 0.25f;
            for (int i = 0; i < 3; ++i) { pos[i] = n.e.pos[i] + n.e.linVel[i] * ex; vel[i] = stale ? 0.f : n.e.linVel[i]; av[i] = stale ? 0.f : n.e.angVel[i]; }
            q = *(const Quat*)n.e.rot;
        }
        if (m.blendPending) {   // start the glide from where the car is right now
            m.blendPending = false;
            const float* cp = RB_GetVec(rb, RB_GetPosition);
            for (int i = 0; i < 3; ++i) m.blendOff[i] = cp[i] - pos[i];
            memcpy(m.blendRot, RB_GetVec(rb, RB_GetOrientation), 16);
            m.blendT0 = GetTickCount() | 1;
        }
        if (m.blendT0) {   // a car of ours that took this host car over: glide into its place
            constexpr DWORD BlendMs = 1500;
            DWORD el = GetTickCount() - m.blendT0;
            if (el >= BlendMs) m.blendT0 = 0;
            else {
                float k = 1.f - (float)el / BlendMs;          // 1 -> 0: how much of the old offset is left
                k = k * k * (3.f - 2.f * k);
                for (int i = 0; i < 3; ++i) pos[i] += m.blendOff[i] * k;
                q = Nlerp(q, *(const Quat*)m.blendRot, k);
            }
        }
        RB_SetVec(rb, RB_SetOrientationQuat, &q);
        RB_SetVec(rb, RB_SetPosition, pos);
        RB_SetVec(rb, RB_SetLinearVelocity, vel);
        RB_SetVec(rb, RB_SetAngularVelocity, av);
    }

    // host: stable ids for its traffic cars
    struct HostId { void* iv; uint16_t id; };
    HostId   g_hostIds[64];
    uint16_t g_nextHostId = 1;
    uint16_t HostIdFor(void* iv) {
        for (auto& h : g_hostIds) if (h.iv == iv) return NetUtil::TrafficWireId(h.id, g_cfg.isHost);
        for (auto& h : g_hostIds) {
            if (!h.iv || !ListContains(-1, h.iv)) {
                h.iv = iv; h.id = g_nextHostId;
                g_nextHostId = NetUtil::NextTrafficLocalId(g_nextHostId);
                return NetUtil::TrafficWireId(h.id, g_cfg.isHost);
            }
        }
        return 0;
    }

    uint32_t ModelKeyOf(void* iv) {
        const char* nm = VehName(iv);
        using HashFn = uint32_t(__cdecl*)(const char*);
        if (nm && nm[0] && nm[0] != '?') return ((HashFn)0x4519D0)(nm);
        return VehicleKey(iv);
    }

    // ---- smooth handover (client, free roam, [Traffic] SmoothHandover) -------------------------
    bool SmoothHandover() { return !g_cfg.isHost && g_cfg.trafficSmoothHandover; }

    // The player's camera in game coordinates. eViews[EVIEW_PLAYER1] = 0x9195E0 + 0x70: Active byte +8
    // (0x919658), Camera* +0x40 (0x919690). Camera::CurrentKey Position +0x40 / Direction +0x50 are in
    // render space; the game's own GetCameraPosition (0x40B0D0) / GetCameraDirection (0x40B140) read the
    // same fields and swizzle them to game space as (-r.y, r.z, r.x). Unknown camera: valid = false and
    // pos = the fallback (the player).
    NetUtil::CamView ReadCamera(const float* fallbackPos) {
        NetUtil::CamView c{};
        uint8_t active = 0; uintptr_t cam = 0;
        float p[3], d[3];
        if (Mem::Read(0x919658, active) && active && Mem::Read(0x919690, cam) && cam &&
            Mem::SafeRead((void*)(cam + 0x40), p, 12) && Mem::SafeRead((void*)(cam + 0x50), d, 12) &&
            NetUtil::FiniteWithin(p, 3, NetUtil::MAX_POS) && NetUtil::FiniteWithin(d, 3, 100.f)) {
            c.pos[0] = -p[1]; c.pos[1] = p[2]; c.pos[2] = p[0];
            c.dir[0] = -d[1]; c.dir[1] = d[2]; c.dir[2] = d[0];
            c.valid = true;
        } else if (fallbackPos) memcpy(c.pos, fallbackPos, 12);
        return c;
    }

    // The game's per-car screen timers, updated every frame by PVehicle (0x686060: +0x180 / +0x184,
    // from its render connection's "visible" flag; both 0 when it isn't drawn at all).
    // (Not a vehicle: counts as on screen / inactive / loading, so nothing is done to it.)
    float OnScreenS(void* iv)  { return IsVehicle(iv) ? VCall<float(__thiscall*)(void*)>(iv, 25)(iv) : 1.f; }   // IVehicle::GetOnScreenTime
    float OffScreenS(void* iv) { return IsVehicle(iv) ? VCall<float(__thiscall*)(void*)>(iv, 24)(iv) : 0.f; }   // IVehicle::GetOffScreenTime
    bool  IsActiveCar(void* iv) { return IsVehicle(iv) && VCall<bool(__thiscall*)(void*)>(iv, 34)(iv); }        // IVehicle::IsActive
    bool  IsLoadingCar(void* iv) { return !IsVehicle(iv) || VCall<bool(__thiscall*)(void*)>(iv, IV_IsLoading)(iv); }
    bool  OnScreen(void* iv) { float s = OnScreenS(iv); return !(s <= 0.f); }

    // AITrafficManager: its constructor (0x4418D0) stores the object + 0x48 in 0x92C674. Object vtable
    // 0x89221C; IAttachable at +0x2C (vtable 0x892200, slot 2 = Attach(IUnknown*) -> 0x402900 ->
    // 0x6FBF80). Its OnAttached (0x4399C0) adds an attached vehicle to its car list (+0x90); its update
    // (0x439BB0) un-spawns a listed car once 0x410B00 says it isn't needed (off screen longer than, and
    // farther than, its density tables allow) and re-uses such cars for new traffic (0x426380) - the
    // same Attach call it makes for every traffic car it builds (0x4264BB).
    void* TrafficMgrAttachable() {
        uintptr_t p = 0, vt = 0, vt2 = 0;
        if (!Mem::Read(0x92C674, p) || p < 0x48) return nullptr;
        uintptr_t mgr = p - 0x48;
        if (!Mem::Read(mgr, vt) || vt != 0x89221C || !Mem::Read(mgr + 0x2C, vt2) || vt2 != 0x892200) return nullptr;
        return (void*)(mgr + 0x2C);
    }

    // Turn one of our spawned host-car copies into a normal traffic car of ours that keeps its motion.
    // IVehicle::SetDriverClass (slot 21, 0x6876E0) stores the class, re-lists the car (0x67ECA0: traffic
    // = the AI + AI-traffic lists) and reloads its behaviours (0x6863B0 -> 0x685240), which picks the AI
    // by driver class (table 0x8F8890, read at 0x67F383: traffic -> "AIVehicleTraffic", none ->
    // "AIVehicleEmpty"). Then the traffic manager adopts it and its AI looks for its lane.
    enum { HAND_FAILED, HAND_AI, HAND_AI_MANAGED };
    int HandToTrafficAI(void* iv) {
        if (!IsVehicle(iv) || IsLoadingCar(iv)) return HAND_FAILED;
        void* rb = RigidBodyOf(iv);
        if (!rb) return HAND_FAILED;
        float pos[3], q[4], v[3], w[3];
        memcpy(pos, RB_GetVec(rb, RB_GetPosition), 12);
        memcpy(q, RB_GetVec(rb, RB_GetOrientation), 16);
        memcpy(v, RB_GetVec(rb, RB_GetLinearVelocity), 12);
        memcpy(w, RB_GetVec(rb, RB_GetAngularVelocity), 12);
        if (DriverClass(iv) != DRV_TRAFFIC)
            VCall<void(__thiscall*)(void*, int)>(iv, IV_SetDriverClass)(iv, DRV_TRAFFIC);
        if (DriverClass(iv) != DRV_TRAFFIC) return HAND_FAILED;
        if ((rb = RigidBodyOf(iv)) != nullptr) {      // the behaviour reload must not stop or move it
            RB_SetVec(rb, RB_SetOrientationQuat, q);
            RB_SetVec(rb, RB_SetPosition, pos);
            RB_SetVec(rb, RB_SetLinearVelocity, v);
            RB_SetVec(rb, RB_SetAngularVelocity, w);
        }
        bool managed = false;
        if (void* att = TrafficMgrAttachable()) {
            void* sim = VCall<void* (__thiscall*)(void*)>(iv, IV_GetSimable)(iv);
            if (sim) managed = VCall<bool(__thiscall*)(void*, void*)>(att, 2)(att, sim);
        }
        if (void* ai = VCall<void* (__thiscall*)(void*)>(iv, 0xAC / 4)(iv))     // GetAIVehiclePtr
            VCall<void(__thiscall*)(void*, int)>(ai, 0x50 / 4)(ai, 1);         // ResetDriveToNav(current lane)
        if (managed) ForgetCar(iv);   // the game's own now: it culls and re-uses it like any traffic car
        return managed ? HAND_AI_MANAGED : HAND_AI;
    }

    // Cars we let go of that we still remove ourselves once nobody sees them: copies that couldn't get
    // the traffic AI (they coast on: removed as soon as they are off screen) and traffic cars the
    // traffic manager didn't adopt (removed the way the game culls traffic: off screen for a while and
    // not close, or very far away). They stay in our vehicle cache so we hear if the game deletes one.
    struct RetiredCar { void* iv; bool ai; };
    RetiredCar g_retired[32] = {};
    bool IsRetired(void* iv) { for (auto& r : g_retired) if (r.iv && r.iv == iv) return true; return false; }
    void Retire(void* iv, bool ai) {
        if (!iv || IsRetired(iv)) return;
        for (auto& r : g_retired) if (!r.iv) { r.iv = iv; r.ai = ai; return; }
        if (!ai) KillVehicle(iv);    // no room to track it (never seen in practice): the old behaviour
    }
    int TickRetired(const NetUtil::CamView& cam) {
        int removed = 0;
        for (auto& r : g_retired) {
            if (!r.iv) continue;
            if (WasRemovedByGame(r.iv) || !IsKeptCar(r.iv) || !ListContains(-1, r.iv)) { r.iv = nullptr; continue; }
            void* rb = RigidBodyOf(r.iv);
            if (!rb) { r.iv = nullptr; continue; }
            float d = Dist(RB_GetVec(rb, RB_GetPosition), cam.pos);
            float on = OnScreenS(r.iv), off = OffScreenS(r.iv);
            bool go = r.ai ? (d >= NetUtil::HANDOVER_GONE_M || (on <= 0.f && off >= 3.f && d >= 120.f))
                           : NetUtil::OwnCarMayGo(on, off, d);
            if (go) { KillVehicle(r.iv); r.iv = nullptr; ++removed; }
        }
        return removed;
    }
    void ForgetRetired() { for (auto& r : g_retired) r.iv = nullptr; }   // the world is gone

    int g_handoverLogs = 0;   // per-car handover lines logged since the last switch (only the first few)
    int g_hoWaiting = 0;      // host cars waiting for a spot out of view (last MaintainMirrors)
    int g_hoKeptOnExpiry = 0; // host cars the host stopped sending that stayed here because they were on screen
    int g_hoRetiredGone = 0;  // let-go cars we removed once they were out of view
}

static void OnCarHitImpl(uint16_t id, const float* dv);

bool Game::InstallTrafficHooks() {
    uint8_t op = 0; int32_t rel = 0;
    if (!Mem::Read(DensityCallSite, op) || !Mem::Read(DensityCallSite + 1, rel) || op != 0xE8 ||
        DensityCallSite + 5 + rel != ComputeDensityFn) {
        LOG("Traffic: density call site doesn't match - traffic sync disabled");
        return false;
    }
    Mem::PatchCall(DensityCallSite, (void*)&hk_ComputeDensity);
    bool c1 = PatchCallChecked(PatrolCallSite, SpawnPatrolFn, (void*)&hk_SpawnPatrol);
    bool c2 = PatchCallChecked(PursuitCallSite1, SpawnPursuitFn, (void*)&hk_SpawnPursuit);
    bool c3 = PatchCallChecked(PursuitCallSite2, SpawnPursuitFn, (void*)&hk_SpawnPursuit);
    LOG("Cops: spawn hooks %s/%s/%s", c1 ? "ok" : "FAIL", c2 ? "ok" : "FAIL", c3 ? "ok" : "FAIL");
    Net::OnTraffic = [](const TrafficEntry* e, int count, double sendMs) { if (g_coopLinked) OnTrafficPacket(e, count, sendMs); };
    Net::OnRacerName = &OnRacerName;
    Net::OnCarHit = [](uint16_t id, const float* dv) { if (!g_coopLinked) return; __try { OnCarHitImpl(id, dv); } __except (EXCEPTION_EXECUTE_HANDLER) {} };
    LOG("Traffic: hooked ComputeDensity call (client can suppress its own traffic)");
    return true;
}

// A host traffic car shown during a race (client, SharedInRaces): not a cop, not a racer.
static bool IsRaceTrafficMirror(const Mirror& m) {
    return m.bound || (SharedRaceTraffic() && !m.racer && m.n && !(m.s[m.n - 1].e.flags & 1));
}

static int g_raceBindLogs = 0;   // binding lines logged in this race (only the first few)
static int g_raceLastBound = -1, g_raceLastWaiting = -1;   // last logged status line

// Client in a race (SharedInRaces): give every host traffic car without a car here one of OUR OWN
// traffic cars to follow. Nothing is spawned or deleted and no AI is touched, so a car the race
// script owns stays a valid car (it is only moved). A car of the same model first, then any model.
static void BindRaceTraffic() {
    bool want = false;   // only look at our cars when a host car is waiting for one
    for (auto& m : g_mirrors) if (m.used && !m.iv && m.n && IsRaceTrafficMirror(m)) { want = true; break; }
    if (!want) return;
    void* cars[64];
    NetUtil::TrafficCandidate cand[64];
    int nc = 0;
    void* me = LocalVehicle();
    void* lrb = RigidBodyOf(me);
    void* grb = RigidBodyOf(g_ghost);
    float lp[3] = {}, gp[3] = {};
    if (lrb) memcpy(lp, RB_GetVec(lrb, RB_GetPosition), 12);
    if (grb) memcpy(gp, RB_GetVec(grb, RB_GetPosition), 12);
    void** b = nullptr;
    int cnt = ListSize(VL_AITRAFFIC, &b);
    for (int i = 0; i < cnt && nc < 64; ++i) {
        void* t = ListGet(VL_AITRAFFIC, i);
        if (!t || t == g_ghost || t == me || !IsVehicle(t) || IsKeptCar(t)) continue;
        void* rb = RigidBodyOf(t);
        if (!rb) continue;
        cars[nc] = t;
        cand[nc].model = ModelKeyOf(t);
        memcpy(cand[nc].pos, RB_GetVec(rb, RB_GetPosition), 12);
        // already following a host car, or touching a player (moving it away would jolt them)
        cand[nc].skip = IsMirrorCar(t) || (lrb && Dist(lp, cand[nc].pos) < 6.f) || (grb && Dist(gp, cand[nc].pos) < 6.f);
        ++nc;
    }
    if (!nc) return;
    for (int pass = 0; pass < 2; ++pass) {           // same model for everyone first, then any model
        for (auto& m : g_mirrors) {
            if (!m.used || m.iv || !m.n || !IsRaceTrafficMirror(m)) continue;
            const TrafficEntry& e = m.s[m.n - 1].e;
            // never put a car inside a player (it would fling them): wait until it has moved on
            if ((lrb && Dist(lp, e.pos) < 6.f) || (grb && Dist(gp, e.pos) < 6.f)) continue;
            int k = NetUtil::PickTrafficPartner(m.model, e.pos, cand, nc, pass == 1);
            if (k < 0) continue;
            cand[k].skip = true;
            m.iv = cars[k];
            m.bound = true;
            if (g_raceBindLogs < 10) {
                ++g_raceBindLogs;
                LOG("Traffic: our traffic car %p (%s) now follows the host's car %u (%s model, %.0f m away)",
                    cars[k], VehName(cars[k]), (unsigned)m.id, cand[k].model == m.model ? "same" : "other", Dist(cand[k].pos, e.pos));
            }
        }
    }
}

// ---- smooth handover (client, free roam) ----------------------------------------------------------
// A host car copy we no longer need. With keepUnseen false only a car on screen is kept (an unseen one
// is removed right away as before). A kept traffic car drives on as our own traffic car (traffic AI);
// a cop, or a car that can't get the AI, coasts on and is removed once it is off screen.
static void LetGoOfMirror(Mirror& m, bool keepUnseen, int& handed, int& managed, int& coasting) {
    if (m.racer || m.bound || !m.iv || WasRemovedByGame(m.iv) || !ListContains(-1, m.iv) ||
        (!keepUnseen && !OnScreen(m.iv))) { RemoveMirror(m); return; }
    void* iv = m.iv;
    bool cop = m.n && (m.s[m.n - 1].e.flags & 1);
    int r = cop ? HAND_FAILED : HandToTrafficAI(iv);
    if (r == HAND_FAILED) { Retire(iv, false); ++coasting; }
    else {
        ++handed;
        if (r == HAND_AI_MANAGED) ++managed; else Retire(iv, true);
        if (g_handoverLogs < 8) {
            ++g_handoverLogs;
            LOG("Traffic: the host's car %u (%s) drives on as our own traffic car %p (%s)", (unsigned)m.id, VehName(iv), iv,
                r == HAND_AI_MANAGED ? "traffic AI, the game culls it" : "traffic AI, we remove it once out of view");
        }
    }
    m = Mirror{};
}

// Leaving the host's traffic (or the host went away): none of the host's cars around us vanish.
static void ReleaseHostTraffic(const char* why) {
    int handed = 0, managed = 0, coasting = 0, freed = 0;
    for (auto& m : g_mirrors) {
        if (!m.used || m.racer) continue;
        if (m.bound) { ++freed; RemoveMirror(m); continue; }     // our own car: it just drives on
        LetGoOfMirror(m, true, handed, managed, coasting);
    }
    if (handed || coasting || freed)
        LOG("Traffic: %s - %d of the host's cars drive on as our traffic (%d taken over by the game's traffic manager), "
            "%d coast on until out of view, %d of our own cars stop following the host's", why, handed, managed, coasting, freed);
}

// Our own traffic cars a host car may take over (like BindRaceTraffic): not ours to keep, active,
// not already following a host car, not touching a player. Cars we handed over earlier count too.
struct OwnTraffic { void* car[64]; NetUtil::TrafficCandidate c[64]; int n; bool built; };
static void CollectOwnTraffic(OwnTraffic& o, const float* lp, const float* gp) {
    o.n = 0; o.built = true;
    void* me = LocalVehicle();
    void** b = nullptr;
    int cnt = ListSize(VL_AITRAFFIC, &b);
    for (int i = 0; i < cnt && o.n < 64; ++i) {
        void* t = ListGet(VL_AITRAFFIC, i);
        if (!t || t == g_ghost || t == me || !IsVehicle(t) || IsMirrorCar(t) || !IsActiveCar(t)) continue;
        if (IsKeptCar(t) && !IsRetired(t)) continue;
        void* rb = RigidBodyOf(t);
        if (!rb) continue;
        o.car[o.n] = t;
        o.c[o.n].model = ModelKeyOf(t);
        memcpy(o.c[o.n].pos, RB_GetVec(rb, RB_GetPosition), 12);
        o.c[o.n].skip = (lp && Dist(lp, o.c[o.n].pos) < 6.f) || (gp && Dist(gp, o.c[o.n].pos) < 6.f);
        ++o.n;
    }
}
static bool AnyCarNear(const float* pos, float r) {
    void** b = nullptr;
    int cnt = ListSize(VL_ALL, &b);
    for (int i = 0; i < cnt; ++i) {
        void* t = ListGet(VL_ALL, i);
        void* rb = RigidBodyOf(t);
        if (rb && Dist(RB_GetVec(rb, RB_GetPosition), pos) < r) return true;
    }
    return false;
}

// A host traffic car without a car here: true = create it now (its spot is out of view or far, and
// free). Otherwise one of our own cars may become it: the one already sitting in its spot, or (spot in
// view) a nearby one, which then glides into place. Else it waits until its spot is out of view.
static bool PlaceHostCar(Mirror& m, const NetUtil::CamView& cam, const float* lp, const float* gp, OwnTraffic& own) {
    const TrafficEntry& e = m.s[m.n - 1].e;
    if (!own.built) CollectOwnTraffic(own, lp, gp);
    int k = NetUtil::PickTakeoverPartner(m.model, e.pos, own.c, own.n, NetUtil::HANDOVER_OVERLAP_M, NetUtil::HANDOVER_OVERLAP_M);
    const char* how = "in its spot";
    if (k < 0) {
        if (NetUtil::MaySpawnUnseen(cam, e.pos, lp)) return !AnyCarNear(e.pos, NetUtil::HANDOVER_OVERLAP_M);
        k = NetUtil::PickTakeoverPartner(m.model, e.pos, own.c, own.n, 25.f, 10.f);
        how = "nearby, in view";
        if (k < 0) return false;
    }
    void* car = own.car[k];
    own.c[k].skip = true;
    for (auto& r : g_retired) if (r.iv == car) { r.iv = nullptr; ForgetCar(car); }   // one we handed over earlier
    m.iv = car;
    m.bound = true;
    m.blendPending = true;
    if (g_handoverLogs < 16) {
        ++g_handoverLogs;
        LOG("Traffic: our traffic car %p (%s, %s) now shows the host's car %u (%s model, %.0f m away)", car, VehName(car), how,
            (unsigned)m.id, own.c[k].model == m.model ? "same" : "other", Dist(own.c[k].pos, e.pos));
    }
    return false;
}

// Spawn / drive / expire the mirror cars (both sides: the client mirrors the host's traffic and
// cops, the host mirrors the client's own pursuit cops).
static int MaintainMirrors(double now) {
    double renderSend = now - Net::PeerClockOffsetMs() - g_cfg.interpDelayMs;
    int spawned = 0, alive = 0, freed = 0;
    if (SharedRaceTraffic()) {
        for (auto& m : g_mirrors)   // the game removed a car we followed with: find another one
            if (m.used && m.bound && m.iv && (!ListContainsOne(VL_AITRAFFIC, m.iv) || m.iv == LocalVehicle())) { m.iv = nullptr; m.bound = false; }   // gone, or the address now holds another kind of car
        BindRaceTraffic();
    }
    // The other player's own car comes first: no mirror spawns until it exists, and when its spawn
    // failed (vehicle budget full) give back a couple of mirror cars.
    bool mayspawn = g_ghost && !g_ghostStarved;
    // SmoothHandover (client, free roam, near the host): host cars appear only where it can't be seen
    bool smooth = SmoothHandover() && !g_clientRacing && g_trafficNear;
    NetUtil::CamView cam{};
    float lpos[3] = {}, gpos[3] = {};
    const float* lp = nullptr; const float* gp = nullptr;
    OwnTraffic own; own.n = 0; own.built = false;
    if (smooth) {
        if (void* lrb = RigidBodyOf(LocalVehicle())) { memcpy(lpos, RB_GetVec(lrb, RB_GetPosition), 12); lp = lpos; }
        if (void* grb = RigidBodyOf(g_ghost)) { memcpy(gpos, RB_GetVec(grb, RB_GetPosition), 12); gp = gpos; }
        cam = ReadCamera(lp);
    }
    g_hoWaiting = 0;
    for (auto& m : g_mirrors) {
        if (!m.used) continue;
        // the owner keeps sending, just not this car: it's gone there. (If the owner sends nothing at all,
        // its game is paused - alt-tab, map, loading: keep every mirror frozen where it is.)
        // SmoothHandover: a car still on screen here drives on as our own traffic instead of vanishing.
        if (g_lastTrafficPkt - m.lastSeen > 1500) {
            if (smooth && !m.racer && !m.bound && m.iv) { int h = 0, mg = 0, co = 0; LetGoOfMirror(m, false, h, mg, co); g_hoKeptOnExpiry += h + co; }
            else RemoveMirror(m);
            continue;
        }
        if (m.iv && (WasRemovedByGame(m.iv) || !ListContains(-1, m.iv))) { m.iv = nullptr; m.bound = false; }   // the game removed it (its address may hold another car now)
        // free roam: our own car that showed this host car was un-spawned by the traffic manager (or is gone)
        if (m.bound && m.iv && !SharedRaceTraffic() && (!ListContainsOne(VL_AITRAFFIC, m.iv) || m.iv == LocalVehicle() || !IsActiveCar(m.iv))) { m.iv = nullptr; m.bound = false; }
        if (IsRaceTrafficMirror(m)) {
            // one of our own traffic cars follows the host's car (bound above): never spawn or kill here
            if (m.iv && !NisActive()) {
                DriveMirror(m, renderSend);
                GlareBits(m.iv) = m.s[m.n - 1].e.glare;
            }
            if (m.iv) ++alive;
            continue;
        }
        if (m.racer) {
            // bind to one of our own AI racers of the same model, switch its AI off and drive it
            if (!m.iv && m.n) {
                void** b = nullptr;
                int cnt = ListSize(VL_AIRACERS, &b);
                for (int i = 0; i < cnt; ++i) {
                    void* r = ListGet(VL_AIRACERS, i);
                    if (!r || r == g_ghost || IsMirrorCar(r) || !RigidBodyOf(r) || ModelKeyOf(r) != m.model) continue;
                    m.iv = r;
                    DisableAI(r);
                    LOG("Race: our racer %p (%s) now follows the host's opponent (mirror id %u)", r, VehName(r), m.id);
                    break;
                }
                if (!m.iv && !m.fails) {   // log once why it found no partner
                    m.fails = 1;
                    char ours[200] = ""; size_t L = 0;
                    for (int i = 0; i < cnt && L < 180; ++i) { void* r = ListGet(VL_AIRACERS, i); if (r) L += sprintf_s(ours + L, sizeof(ours) - L, " %s=%08X%s", VehName(r), ModelKeyOf(r), r == g_ghost ? "(ghost)" : IsMirrorCar(r) ? "(bound)" : ""); }
                    LOG("Race: no racer of model %08X for host opponent %u; ours:%s", m.model, m.id, ours);
                }
            }
            if (m.iv && !NisActive()) DriveMirror(m, renderSend);
            if (m.iv && m.name[0]) if (void* ri = RacerInfoOf(m.iv)) { const char* nm = m.name; Mem::Write((uintptr_t)ri + 0x08, nm); }
            if (m.iv) ++alive;
            continue;
        }
        if (g_ghostStarved && m.iv && freed < 2) { KillVehicle(m.iv); m.iv = nullptr; ++freed; continue; }
        if (!m.iv && m.n && mayspawn && spawned < 2 && m.fails < 3) {
            const TrafficEntry& e = m.s[m.n - 1].e;
            // never create a car inside a player (it would fling them): wait until it has moved on
            void* lrb = RigidBodyOf(LocalVehicle());
            void* grb = RigidBodyOf(g_ghost);
            if ((lrb && Dist(RB_GetVec(lrb, RB_GetPosition), e.pos) < 6.f) ||
                (grb && Dist(RB_GetVec(grb, RB_GetPosition), e.pos) < 6.f)) continue;
            // SmoothHandover: only where the player can't see it pop in (or one of our cars becomes it)
            if (smooth && !(e.flags & 1) && !PlaceHostCar(m, cam, lp, gp, own)) { if (!m.iv) ++g_hoWaiting; continue; }
            float dir[3] = { 1, 0, 0 };
            m.iv = SpawnVehicle(e.model, nullptr, e.pos, dir, g_cfg.trafficMirrorClass);
            ++spawned;
            if (!m.iv) { ++m.fails; LOG("Traffic: mirror spawn of %08X failed (%d)", e.model, m.fails); continue; }
            if (e.flags & 1) LOG("Cops: showing the other player's police car (id %u, model %08X)", (unsigned)m.id, e.model);
        }
        if (m.iv) {
            DriveMirror(m, renderSend);
            GlareBits(m.iv) = m.s[m.n - 1].e.glare;   // lights: brake/head/signals/cop light bar
            ++alive;
        }
    }
    if (freed) LOG("Traffic: removed %d mirror cars to make room for the other player's car", freed);
    g_ghostStarved = false;
    return alive;
}

// Collect our own (non-mirror) cars of the given lists, nearest to `to` first, into a packet.
static int CollectOwned(std::initializer_list<int> lists, const float* to, TrafficEntry* out, int maxOut) {
    struct C { void* iv; float d; uint8_t flags; } cand[64]; int nc = 0;
    void* me = LocalVehicle();
    for (int list : lists) {
        void** b = nullptr;
        int cnt = ListSize(list, &b);
        for (int i = 0; i < cnt && nc < 64; ++i) {
            void* t = ListGet(list, i);
            if (!t || t == g_ghost || t == me || IsMirrorCar(t)) continue;
            void* rb = RigidBodyOf(t);
            if (!rb) continue;
            cand[nc].iv = t;
            // race opponents first, then cops, then traffic (nearest first within each)
            cand[nc].d = Dist(RB_GetVec(rb, RB_GetPosition), to) - (list == VL_AIRACERS ? 20000.f : list == VL_AICOPS ? 10000.f : 0.f);
            cand[nc].flags = list == VL_AIRACERS ? 2 : list == VL_AICOPS ? 1 : 0;
            ++nc;
        }
    }
    for (int i = 0; i < nc; ++i)
        for (int j = i + 1; j < nc; ++j)
            if (cand[j].d < cand[i].d) { C x = cand[i]; cand[i] = cand[j]; cand[j] = x; }
    int n = 0;
    for (int i = 0; i < nc && n < maxOut; ++i) {
        void* rb = RigidBodyOf(cand[i].iv);
        TrafficEntry& e = out[n];
        e.id = HostIdFor(cand[i].iv);
        if (!e.id) continue;
        e.flags = cand[i].flags;
        e.model = ModelKeyOf(cand[i].iv);
        e.glare = GlareBits(cand[i].iv);
        memcpy(e.pos, RB_GetVec(rb, RB_GetPosition), 12);
        memcpy(e.rot, RB_GetVec(rb, RB_GetOrientation), 16);
        memcpy(e.linVel, RB_GetVec(rb, RB_GetLinearVelocity), 12);
        memcpy(e.angVel, RB_GetVec(rb, RB_GetAngularVelocity), 12);
        ++n;
    }
    return n;
}

static int g_sentCount = 0;

static void TrafficTickImpl(bool connected, const Net::Snapshot& peer) {
    double now = Net::LocalClockMs();
    void* me = LocalVehicle();
    void* myRb = RigidBodyOf(me);
    if (Game::GameFlowState() != 6 || !myRb || !connected || !peer.valid || !peer.s.hasCar) {
        // not in a shared world: clean up and hand traffic back to the game
        if (g_suppressTraffic) LOG("Traffic: released (not connected / not in game)");
        g_suppressTraffic = false;
        g_trafficNear = false;
        bool inWorld = Game::GameFlowState() == 6 && myRb;
        // still driving (only the host is gone / has no car): the host's cars around us stay and drive on
        if (inWorld && SmoothHandover() && !g_clientRacing) ReleaseHostTraffic("the host's traffic is gone");
        for (auto& m : g_mirrors) if (m.used) RemoveMirror(m);
        if (inWorld) TickRetired(ReadCamera(RB_GetVec(myRb, RB_GetPosition))); else ForgetRetired();
        g_mirrorCount = 0;
        return;
    }
    const float* myPos = RB_GetVec(myRb, RB_GetPosition);
    float distToPeer = Dist(myPos, peer.s.pos);
    static double nextSend = 0;
    if (nextSend - now > 1000) nextSend = now;   // the sim clock restarted (world reload)
    bool sendNow = now >= nextSend;
    if (sendNow) nextSend = now + 50; // 20 Hz

    if (g_cfg.isHost) {
        // host: send its cops + traffic nearest to the client; mirror the client's own pursuit cops
        // host: in a race its AI opponents always go too (the client's own racers follow them)
        bool inRace = false;
        { void* rs = nullptr; int mode = 0; inRace = Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1; }
        if (sendNow && (distToPeer <= 450.f || inRace)) {
            TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
            { static int lastRacers = -1; void** rb_ = nullptr; int nr = inRace ? ListSize(VL_AIRACERS, &rb_) : 0;
              if (nr != lastRacers) { lastRacers = nr; LOG("Race: sending %d racer slot(s) to the client (in race %d)", nr, (int)inRace); } }
            g_sentCount = distToPeer <= 450.f
                ? CollectOwned({ VL_AIRACERS, VL_AICOPS, VL_AITRAFFIC }, peer.s.pos, out, MAX_TRAFFIC_PER_PACKET)
                : CollectOwned({ VL_AIRACERS }, peer.s.pos, out, MAX_TRAFFIC_PER_PACKET);
            Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());
        }
        // opponents' names, every 2 s while racing (the client shows the same names)
        static double nextNames = 0;
        if (nextNames - now > 5000) nextNames = now;
        if (inRace && now >= nextNames) {
            nextNames = now + 2000;
            void** b = nullptr;
            int cnt = ListSize(VL_AIRACERS, &b);
            for (int i = 0; i < cnt; ++i) {
                void* r = ListGet(VL_AIRACERS, i);
                if (!r || r == g_ghost || IsMirrorCar(r)) continue;
                void* ri = RacerInfoOf(r);
                const char* nm = nullptr;
                if (!ri || !Mem::Read((uintptr_t)ri + 0x08, nm) || !nm || !Mem::IsReadable(nm, 2)) continue;
                char buf[31]; if (!Mem::SafeRead(nm, buf, 30)) continue; buf[30] = 0;
                if (uint16_t id = HostIdFor(r)) Net::SendRacerName(id, buf);
            }
        }
        g_mirrorCount = MaintainMirrors(now);
        return;
    }

    // ---- client ----
    bool isNear = g_trafficNear ? distToPeer < 450.f : distToPeer < 350.f;
    // not more than one switch every 15 s: each one removes and rebuilds all traffic around us (with
    // SmoothHandover it hands cars over instead, but a quick back-and-forth still churns them)
    static double lastFlip = -1e9;
    if (lastFlip > now) lastFlip = -1e9;                         // the sim clock restarted (world reload)
    if (isNear != g_trafficNear && now - lastFlip < 15000) isNear = g_trafficNear;
    if (isNear != g_trafficNear) {
        lastFlip = now;
        g_trafficNear = isNear;
        bool smoothNow = SmoothHandover() && !g_clientRacing;
        LOG("Traffic: %s the host (%.0f m) - %s", isNear ? "near" : "away from", distToPeer,
            isNear ? (smoothNow ? "mirroring host traffic (our cars stay until out of view, host cars appear out of view)" : "mirroring host traffic")
                   : (smoothNow ? "using own traffic (the host's cars around us drive on as ours)" : "using own traffic"));
        g_handoverLogs = 0;
        if (!isNear) {
            if (smoothNow) ReleaseHostTraffic("left the host's traffic");
            for (auto& m : g_mirrors) if (m.used && !m.racer) RemoveMirror(m);
        }
    }
    {
        void* rs = nullptr; int mode = 0;
        bool racing = Mem::Read(0x91E000, rs) && rs && Mem::Read((uintptr_t)rs + 0x1960, mode) && mode == 1;
        if (racing != g_clientRacing) {
            // our spawned traffic mirrors go (as before); cars bound to the host's are our own: just released
            int released = 0;
            for (auto& m : g_mirrors) {
                if (!m.used || m.racer || !m.n || (m.s[m.n - 1].e.flags & 1)) continue;
                if (m.bound) ++released;
                RemoveMirror(m);
            }
            g_clientRacing = racing;
            g_raceBindLogs = 0; g_raceLastBound = g_raceLastWaiting = -1;
            if (racing && g_cfg.trafficSharedInRaces)
                LOG("Traffic: in a race - mirroring the host's traffic with our own traffic cars (none removed, SharedInRaces=1)");
            else
                LOG("Traffic: %s", racing ? "in a race - keeping our own traffic" : "race over - sharing the host's traffic again");
            if (released) LOG("Traffic: released %d of our traffic cars that followed the host's (they drive on by themselves)", released);
            if (!racing) g_raceEndedTick = GetTickCount();
        }
    }
    if (g_clientRacing) {
        g_suppressTraffic = false;
        if (sendNow) {
            TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
            g_sentCount = CollectOwned({ VL_AICOPS }, peer.s.pos, out, 8);
            Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());
        }
        g_mirrorCount = MaintainMirrors(now);
        if (g_cfg.trafficSharedInRaces) {
            // status line whenever the numbers change (at most every 5 s)
            int bound = 0, waiting = 0;
            for (auto& m : g_mirrors) if (m.used && IsRaceTrafficMirror(m)) { if (m.iv) ++bound; else ++waiting; }
            static double nextLog = 0;
            if (nextLog - now > 10000) nextLog = now;   // the sim clock restarted (world reload)
            if ((bound != g_raceLastBound || waiting != g_raceLastWaiting) && now >= nextLog) {
                g_raceLastBound = bound; g_raceLastWaiting = waiting; nextLog = now + 5000;
                LOG("Traffic: in a race - mirroring the host's traffic (%d cars, %d of the host's without a car here)", bound, waiting);
            }
        }
        return;
    }
    g_suppressTraffic = isNear;   // no own traffic / patrol spawns near the host (pursuit spawns stay)
    bool smooth = SmoothHandover();
    NetUtil::CamView cam = smooth ? ReadCamera(myPos) : NetUtil::CamView{};
    if (smooth) g_hoRetiredGone += TickRetired(cam);
    if (!isNear) { g_sentCount = 0; g_mirrorCount = MaintainMirrors(now); return; }   // race opponents only

    // remove our own (non-mirror) traffic cars, a few per frame. Our own cops stay: they belong to
    // our pursuit and are sent to the host. SmoothHandover: only cars that are off screen (the rest
    // drive on until they leave the view; no new ones spawn here).
    int ownInView = 0;
    if (!g_raceEndedTick || GetTickCount() - g_raceEndedTick > 8000) {
        int killed = 0;
        void** b = nullptr;
        int cnt = ListSize(VL_AITRAFFIC, &b);
        for (int i = cnt - 1; i >= 0 && killed < 3; --i) {
            void* t = ListGet(VL_AITRAFFIC, i);
            if (!t || t == g_ghost || IsMirrorCar(t) || !IsVehicle(t)) continue;
            if (smooth) {
                void* rb = RigidBodyOf(t);
                float d = rb ? Dist(RB_GetVec(rb, RB_GetPosition), cam.pos) : 0.f;
                if (!NetUtil::OwnCarMayGo(OnScreenS(t), OffScreenS(t), d)) { ++ownInView; continue; }
            }
            KillVehicle(t); ++killed; ++g_killedOwn;
        }
    }
    if (sendNow) {
        TrafficEntry out[MAX_TRAFFIC_PER_PACKET];
        g_sentCount = CollectOwned({ VL_AICOPS }, peer.s.pos, out, 8);
        Net::SendTraffic(out, g_sentCount, Game::SimTimeSec());   // also when empty: tells the host our cops are gone
    }
    g_mirrorCount = MaintainMirrors(now);
    if (smooth) {
        // status whenever the numbers change (at most every 5 s)
        int shownByOurs = 0;
        for (auto& m : g_mirrors) if (m.used && m.bound && m.iv) ++shownByOurs;
        static int lastInView = -1, lastWaiting = -1, lastOurs = -1;
        static double nextLog = 0;
        if (nextLog - now > 10000) nextLog = now;   // the sim clock restarted (world reload)
        if ((ownInView != lastInView || g_hoWaiting != lastWaiting || shownByOurs != lastOurs) && now >= nextLog) {
            lastInView = ownInView; lastWaiting = g_hoWaiting; lastOurs = shownByOurs; nextLog = now + 5000;
            LOG("Traffic: handover - %d of our cars still in view, %d host cars waiting for a spot out of view, %d shown by our own cars, %d host cars kept on screen after the host dropped them, %d let-go cars removed out of view",
                ownInView, g_hoWaiting, shownByOurs, g_hoKeptOnExpiry, g_hoRetiredGone);
        }
    }
}

// Hitting a car the OTHER game owns (their traffic, cops, race opponents shown here as mirrors): the
// mirror is moved by the network, so only our car reacts. Measure what our car lost in the impact
// (250 ms from the first abrupt change) and send it to the owner, who pushes the real car.
static void MirrorHitTick() {
    void* lrb = RigidBodyOf(LocalVehicle());
    if (!lrb) return;
    static float prevV[3], v0[3]; static DWORD prevT = 0, t0 = 0, lastSent = 0; static uint16_t target = 0;
    const float* lv = RB_GetVec(lrb, RB_GetLinearVelocity);
    const float* lp = RB_GetVec(lrb, RB_GetPosition);
    DWORD now = GetTickCount();
    // a teleport (restart, reset to track, event start) or a cutscene is not an impact: no hits for 1 s
    {
        static float lastPos[3]; static bool have = false;
        float sp = sqrtf(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
        float dt = prevT ? (now - prevT) / 1000.f : 0.f;
        uint8_t restartPending = 0; Mem::Read(0x91CF28, restartPending);
        bool jumped = have && Dist(lastPos, lp) > sp * dt + 3.f && (int)(g_testJumpOkUntil - now) <= 0;
        if (jumped || restartPending || NisActive() || Game::RaceStage() == 1) g_noHitUntil = now + 1000;
        memcpy(lastPos, lp, 12); have = true;
    }
    if ((int)(g_noHitUntil - now) > 0) t0 = 0;
    void* grb0 = RigidBodyOf(g_ghost);
    bool playerNear = grb0 && Dist(RB_GetVec(grb0, RB_GetPosition), lp) < 6.f;    // then it is the player-to-player case
    if (!t0 && !playerNear && (int)(g_noHitUntil - now) <= 0 && prevT && now - prevT < 100 && now - lastSent > 600) {
        float d0[3] = { prevV[0] - lv[0], 0, prevV[2] - lv[2] };
        if (sqrtf(d0[0] * d0[0] + d0[2] * d0[2]) > 3.f) {
            float best = 5.5f; uint16_t id = 0;
            for (auto& m : g_mirrors) {
                if (!m.used || !m.iv) continue;
                void* rb = RigidBodyOf(m.iv);
                if (!rb) continue;
                float dd = Dist(RB_GetVec(rb, RB_GetPosition), lp);
                if (dd < best && HitPointsAt(d0, lp, RB_GetVec(rb, RB_GetPosition))) { best = dd; id = m.id; }
            }
            void* grb = RigidBodyOf(g_ghost);
            if (id && !(grb && Dist(RB_GetVec(grb, RB_GetPosition), lp) < best)) { t0 = now; target = id; memcpy(v0, prevV, 12); }
        }
    }
    if (t0 && now - t0 >= 250) {
        float dv[3] = { v0[0] - lv[0], v0[1] - lv[1], v0[2] - lv[2] };
        float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
        t0 = 0; lastSent = now;
        if (mag > 4.f) { Net::SendCarHit(target, dv); LOG("Collision: hit the other player's car %u, lost %.0f km/h - sent to them", (unsigned)target, mag * 3.6f); }
    }
    memcpy(prevV, lv, 12); prevT = now;
}

// owner side: push our real car
static void OnCarHitImpl(uint16_t id, const float* dv) {
    for (auto& h : g_hostIds) {
        if (!h.iv || (uint16_t)(g_cfg.isHost ? h.id : (h.id | CLIENT_ID_BIT)) != id) continue;
        if (!ListContains(-1, h.iv)) return;
        void* rb = RigidBodyOf(h.iv);
        if (!rb) return;
        float mag = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
        float k = 0.6f * (mag > 45.f ? 45.f / mag : 1.f);
        const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
        float nv[3] = { v[0] + dv[0] * k, v[1], v[2] + dv[2] * k };
        RB_SetVec(rb, RB_SetLinearVelocity, nv);
        LOG("Collision: the other player hit our car %u (%s), pushed by %.0f km/h", (unsigned)id, VehName(h.iv), mag * k * 3.6f);
        return;
    }
}

void Game::TrafficTick(bool connected, const Net::Snapshot& peer) {
    if (!g_ok || !g_cfg.trafficSync) return;
    __try { TrafficTickImpl(connected, peer); if (connected && GameFlowState() == 6) MirrorHitTick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Traffic: exception"); }
}

int  Game::TrafficCount() { return g_mirrorCount; }
bool Game::TrafficSuppressed() { return g_suppressTraffic; }

// Test: spawn an AI police car (copmidsize, DRIVER_COP) 25 m ahead of the local player.
// (AICopManager::SpawnPatrolCar refuses near the safe house, so we build one directly.)
static bool DebugSpawnCopImpl(char* out, size_t n) {
    __try {
        void* me = LocalVehicle();
        void* rb = RigidBodyOf(me);
        if (!rb) { strcpy_s(out, n, "no local car"); return false; }
        const float* p = RB_GetVec(rb, RB_GetPosition);
        const float* v = RB_GetVec(rb, RB_GetLinearVelocity);
        float dir[3] = { 1, 0, 0 };
        float sp = sqrtf(v[0] * v[0] + v[2] * v[2]);
        if (sp > 1.f) { dir[0] = v[0] / sp; dir[2] = v[2] / sp; }
        float pos[3] = { p[0] + dir[0] * 25.f, p[1] + 1.f, p[2] + dir[2] * 25.f };
        using HashFn = uint32_t(__cdecl*)(const char*);
        uint32_t key = ((HashFn)0x4519D0)("copmidsize");
        void* iv = SpawnVehicle(key, nullptr, pos, dir, DRV_COP);
        void** b = nullptr;
        sprintf_s(out, n, "police car %s (cops now %d)", iv ? "spawned" : "FAILED", ListSize(VL_AICOPS, &b));
        return iv != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, n, "exception"); return false; }
}
std::string Game::DebugSpawnCop() {
    if (!g_ok || GameFlowState() != 6) return "not in game";
    char buf[96] = "";
    DebugSpawnCopImpl(buf, sizeof(buf));
    LOG("DebugSpawnCop: %s", buf);
    return buf;
}
