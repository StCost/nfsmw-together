// Included from game.cpp (after minimap.inl).
// ---------------------------------------------------------------------------
// World map (WorldMapMain) icon for remote players
//  When the map opens, WorldMap::AddPlayerIcon (0x530F40, thiscall, called from 0x55B335 and 0x55BB90)
//  adds the local player's arrow. We call it, then repeat its steps for each remote-player car using a
//  spare FEng object of the map package:
//   obj  = 0x5B8550(pkg = 0x516AF0(cFEng, [map+0x10]), FEngHash(name)), must have [obj+0x18] == 1
//   0x579840(&pos, &dir, ISimable)              world -> map coords + heading
//   world = pos;  0x511D90(map, &pos)            map coords -> screen coords (in place)
//   angle = 0x45DED0(dir.y, dir.x) & 0xFFFF;  rot = angle * [0x894958]
//   item  = 0x652AD0(0x38);  0x52F500(item, 1, obj, &pos, &world, rot, 0)
//   link (item+4) at the tail of the list map+0x154 (head sentinel) / map+0x158 (tail)
// ---------------------------------------------------------------------------
namespace {
    constexpr uintptr_t AddPlayerIconFn = 0x530F40;
    constexpr uintptr_t AddPlayerIconCall1 = 0x55B335, AddPlayerIconCall2 = 0x55BB90;

    uint32_t FEngHash(const char* s) { uint32_t h = 0xFFFFFFFF; for (; *s; ++s) h = h * 33 + (uint8_t)*s; return h; }

    void* FindMapObject(void* map, const char* name) {
        void* pkgName = nullptr;
        Mem::Read((uintptr_t)map + 0x10, pkgName);
        void* fe = nullptr;
        Mem::Read(0x91CADC, fe);
        if (!pkgName || !fe) return nullptr;
        void* pkg = ((void*(__thiscall*)(void*, void*))0x516AF0)(fe, pkgName);
        if (!pkg) return nullptr;
        void* obj = ((void*(__thiscall*)(void*, uint32_t))0x5B8550)(pkg, FEngHash(name));
        int type = 0;
        if (!obj || !Mem::Read((uintptr_t)obj + 0x18, type) || type != 1) return nullptr;
        return obj;
    }

    void AddMapIcon(void* map, void* obj, void* sim) {
        float pos[2] = {}, dir[2] = {};
        ((void(__cdecl*)(float*, float*, void*))0x579840)(pos, dir, sim);
        float world[2] = { pos[0], pos[1] };
        ((void(__thiscall*)(void*, float*))0x511D90)(map, pos);
        uint32_t ang = ((uint32_t(__cdecl*)(float, float))0x45DED0)(dir[1], dir[0]) & 0xFFFF;
        float k = 0;
        Mem::Read(0x894958, k);
        float rot = (float)(int)ang * k;
        void* item = ((void*(__cdecl*)(int))0x652AD0)(0x38);
        if (!item) return;
        void* res = ((void*(__thiscall*)(void*, int, void*, float*, float*, float, int))0x52F500)(item, 1, obj, pos, world, rot, 0);
        if (!res) return;
        uint8_t* node = (uint8_t*)res + 4;
        uint8_t* head = (uint8_t*)map + 0x154;
        uint8_t* tail = *(uint8_t**)((uint8_t*)map + 0x158);
        *(uint8_t**)tail = node;               // tail->next = node
        *(uint8_t**)(head + 4) = node;         // head.prev  = node
        *(uint8_t**)node = head;               // node->next = head
        *(uint8_t**)(node + 4) = tail;         // node->prev = tail
    }

    const char* g_usedSpare = nullptr;   // which spare FEng object shows the remote player

    // The map only positions icons from a base screen position stored when they were added; with the
    // world running while the map is open, refresh it (and the heading) for both players every frame.
    // Item (0x38): +0x0C FEObject*, +0x10/+0x14 base screen pos, +0x20/+0x24 map pos, +0x28 rotation, +0x2C type
    void UpdateMapItem(void* map, uint8_t* item, void* sim) {
        float p[2] = {}, d[2] = {};
        ((void(__cdecl*)(float*, float*, void*))0x579840)(p, d, sim);
        *(float*)(item + 0x20) = p[0]; *(float*)(item + 0x24) = p[1];
        ((void(__thiscall*)(void*, float*))0x511D90)(map, p);
        *(float*)(item + 0x10) = p[0]; *(float*)(item + 0x14) = p[1];
        float k = 0; Mem::Read(0x894958, k);
        float rot = (float)(int)(((uint32_t(__cdecl*)(float, float))0x45DED0)(d[1], d[0]) & 0xFFFF) * k;
        *(float*)(item + 0x28) = rot;
        ((void(__cdecl*)(void*, float))0x514E20)(*(void**)(item + 0x0C), rot);
    }
    void UpdateMapIcons(void* map) {
        void* fe = nullptr; void* pkg = nullptr;
        Mem::Read(0x91CADC, fe); Mem::Read((uintptr_t)map + 0x10, pkg);
        if (!fe || !pkg || !((bool(__thiscall*)(void*, void*))0x516B90)(fe, pkg)) return;
        void* me = LocalVehicle();
        void* mySim = IsVehicle(me) ? VCall<void* (__thiscall*)(void*)>(me, 1)(me) : nullptr;
        void* other = g_ghost;
        if (!IsVehicle(other)) { void** b = nullptr; other = ListSize(7, &b) > 0 ? ListGet(7, 0) : nullptr; }
        void* otherSim = IsVehicle(other) ? VCall<void* (__thiscall*)(void*)>(other, 1)(other) : nullptr;
        void* myObj = FindMapObject(map, "PLAYERCARINDICATOR");
        void* otherObj = g_usedSpare ? FindMapObject(map, g_usedSpare) : nullptr;
        uint8_t* head = (uint8_t*)map + 0x154;
        int guard = 0;
        for (uint8_t* node = *(uint8_t**)head; node && node != head && guard < 512; node = *(uint8_t**)node, ++guard) {
            uint8_t* item = node - 4;
            int type = *(int*)(item + 0x2C);
            void* obj = *(void**)(item + 0x0C);
            if (type != 1) continue;
            if (obj == myObj && mySim) UpdateMapItem(map, item, mySim);
            else if (obj == otherObj && otherSim) UpdateMapItem(map, item, otherSim);
        }
    }
    using MapNotifyFn = void(__thiscall*)(void*, uint32_t, void*, uint32_t, uint32_t);
    MapNotifyFn g_origMapNotify = nullptr;
    void __fastcall hk_MapNotify(void* map, void*, uint32_t msg, void* obj, uint32_t a, uint32_t b) {
        if (msg == 0xC98356BA) {   // the map's per-frame tick
            __try { UpdateMapIcons(map); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        g_origMapNotify(map, msg, obj, a, b);
    }

    void __fastcall hk_AddPlayerIcon(void* map, void*) {
        ((void(__fastcall*)(void*, void*))AddPlayerIconFn)(map, nullptr);   // the local player's arrow
        __try {
            // only the cop-car icons exist in WorldMapMain.fng besides the player's own arrow; use the last one
            // (the map shows up to 8 cops, so 7 is free unless a big pursuit is on) and paint it like the player
            static const char* spare[] = { "MMICON_COPCAR_7", "MMICON_COPCAR_6" };
            void** b = nullptr;
            int n = ListSize(7 /*REMOTE*/, &b);
            int used = 0;
            for (int i = 0; i < n && used < 1; ++i) {
                void* iv = ListGet(7, i);
                if (!IsVehicle(iv)) continue;
                void* sim = VCall<void* (__thiscall*)(void*)>(iv, 1)(iv);
                if (!sim) continue;
                for (const char* nm : spare) {
                    if (void* obj = FindMapObject(map, nm)) {
                        AddMapIcon(map, obj, sim);
                        uint32_t argb = 0xFF00FF00;   // fallback: green
                        if (void* pci = FindMapObject(map, "PLAYERCARINDICATOR")) {
                            int c[4] = {};
                            ((int* (__cdecl*)(int*, void*))0x515830)(c, pci);              // FEngGetColor
                            argb = ((uint32_t(__thiscall*)(int*))0x5B2D80)(c);              // FEColor -> ARGB
                        }
                        if (Net::Stats().peerColor) argb = Net::Stats().peerColor;      // the colour they picked
                        ((void(__cdecl*)(void*, uint32_t))0x5157E0)(obj, argb);             // FEngSetColor
                        g_usedSpare = nm;
                        LOG("World map: added remote player icon using %s", nm);
                        ++used;
                        break;
                    }
                }
                if (!used) LOG("World map: no spare icon object found in the map package");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG("World map: exception adding remote player icon");
        }
    }
}

bool Game::InstallWorldMapHook() {
    bool a = PatchCallChecked(AddPlayerIconCall1, AddPlayerIconFn, (void*)&hk_AddPlayerIcon);
    bool b = PatchCallChecked(AddPlayerIconCall2, AddPlayerIconFn, (void*)&hk_AddPlayerIcon);
    LOG("World map: remote-player icon hooks %s/%s", a ? "ok" : "FAIL", b ? "ok" : "FAIL");
    uintptr_t cur = 0;
    if (Mem::Read(0x89E734, cur) && cur == 0x55B0C0) {
        g_origMapNotify = (MapNotifyFn)0x55B0C0;
        Mem::PatchPointer((void*)0x89E734, (void*)&hk_MapNotify);
        LOG("World map: live icon positions hooked");
    } else LOG("World map: notify slot holds %08X - icons won't move while the map is open", (unsigned)cur);
    return a || b;
}
