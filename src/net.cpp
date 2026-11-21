#include <winsock2.h>
#include <ws2tcpip.h>
#include "net.h"
#include <vector>
#include <algorithm>
#include "netutil.h"
#include "config.h"
#include "log.h"
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

namespace {
    SOCKET      g_sock = INVALID_SOCKET;
    sockaddr_in g_peer{};
    bool        g_havePeer = false;     // host: learned from first packet; client: from config
    uint32_t    g_seq = 0;
    uint32_t    g_lastHello = 0, g_lastPing = 0;
    uint32_t    g_lastStateSeq = 0;
    NetStats    g_stats;
    Net::Snapshot g_latest, g_prev;
    char        g_err[128] = "";
    CarInfo     g_localCar{}, g_peerCar{};
    struct TimedSnap { double sendMs, recvMs; CarState s; };
    TimedSnap   g_ring[32];
    int         g_ringHead = 0, g_ringCount = 0;
    char        g_peerCarFile[MAX_PATH] = "";
    bool        g_haveLocalCar = false, g_havePeerCar = false;
    uint32_t    g_lastCarSend = 0;

#ifdef MWT_TESTS
    // test build: a settable millisecond clock and a tap on everything SendRaw would send
    bool     g_testTickOn = false;
    uint32_t g_testTick = 0;
    void   (*g_testCapture)(const void*, int) = nullptr;
    uint32_t Tick32() { return g_testTickOn ? g_testTick : GetTickCount(); }
    bool     SockReady() { return g_sock != INVALID_SOCKET || g_testCapture; }
#else
    inline uint32_t Tick32() { return GetTickCount(); }
    inline bool     SockReady() { return g_sock != INVALID_SOCKET; }
#endif
    // QueryPerformanceCounter milliseconds, wrapped to 32 bits. (A plain double -> uint32_t cast is
    // undefined once the PC has been up for more than 49.7 days.)
    uint32_t NowMs32() { return (uint32_t)(uint64_t)Net::NowMs(); }

    void FillHeader(PacketHeader& h, PacketType t);
    void SendRaw(const void* data, int len);
    void HandleFilePacket(uint16_t type, const char* buf, int len);
    void FileTick();
    void FileReset();
    void SendCar() {
        CarPacket p{};
        FillHeader(p.h, PKT_CAR);
        p.c = g_localCar;
        SendRaw(&p, sizeof(p));
        // the same in small pieces: they get through where a big datagram is dropped
        const uint8_t* src = (const uint8_t*)&g_localCar;
        const int total = (int)sizeof(CarInfo), chunk = (int)sizeof(((CarPartPacket*)0)->data);
        const int n = (total + chunk - 1) / chunk;
        for (int i = 0; i < n; ++i) {
            CarPartPacket q{};
            FillHeader(q.h, PKT_CARPART);
            q.id = g_localCar.carKey ^ g_localCar.customHash;
            q.index = (uint8_t)i; q.count = (uint8_t)n;
            int len = total - i * chunk; if (len > chunk) len = chunk;
            q.len = (uint16_t)len;
            memcpy(q.data, src + i * chunk, len);
            SendRaw(&q, sizeof(q));
        }
        g_lastCarSend = Tick32();
    }

    // Loading screens stall the render loop (no packets) for several seconds, so be generous.
    constexpr uint32_t TIMEOUT_MS = 15000;

    void FillHeader(PacketHeader& h, PacketType t) {
        h.magic = COOP_MAGIC;
        h.version = COOP_VERSION;
        h.type = (uint16_t)t;
        h.seq = ++g_seq;
        h.timeMs = NowMs32();
    }

    bool  g_peerSaidBye = false;
    bool  g_leaving = false;   // we said bye: send nothing more (the peer would see us reconnect)
    bool  g_byeSent = false;
    bool  g_byeRecent = false; // the peer said bye: ignore its leftover packets for a moment
    uint32_t g_byeAt = 0;
    constexpr uint32_t BYE_IGNORE_MS = 2000;

    void SendRaw(const void* data, int len) {
        if (!SockReady() || !g_havePeer || g_leaving) return;
#ifdef MWT_TESTS
        if (g_testCapture) { g_testCapture(data, len); g_stats.packetsOut++; return; }
#endif
        int r = sendto(g_sock, (const char*)data, len, 0, (sockaddr*)&g_peer, sizeof(g_peer));
        if (r == len) g_stats.packetsOut++;
    }

    // A random id for this game instance travels in the last 4 bytes of the hello name field (names
    // are at most 15 characters, so they are free). A hello that carries OUR id is our own packet
    // coming back (HostIP points at this PC): such a "peer" is refused.
    uint32_t InstanceId() {
        static uint32_t id = 0;
        if (!id) { LARGE_INTEGER q; QueryPerformanceCounter(&q); id = (uint32_t)q.LowPart ^ (GetCurrentProcessId() * 2654435761u) ^ GetTickCount(); if (!id) id = 1; }
        return id;
    }
    bool g_selfBlocked = false; sockaddr_in g_selfAddr{};

    void SendHello() {
        HelloPacket p{};
        FillHeader(p.h, PKT_HELLO);
        strncpy_s(p.name, 16, g_cfg.playerName.c_str(), _TRUNCATE);
        uint32_t id = InstanceId(); memcpy(p.name + 20, &id, 4);
        if (g_cfg.playerColor) {    // bytes 16..19 (after the 15-character name): R, G, B, marker 0xC0
            p.name[16] = (char)((g_cfg.playerColor >> 16) & 0xFF); p.name[17] = (char)((g_cfg.playerColor >> 8) & 0xFF);
            p.name[18] = (char)(g_cfg.playerColor & 0xFF); p.name[19] = (char)0xC0;
        }
        SendRaw(&p, sizeof(p));
    }

    std::string AddrToStr(const sockaddr_in& a) {
        char ip[64] = "?";
        inet_ntop(AF_INET, (void*)&a.sin_addr, ip, sizeof(ip));
        char buf[96];
        sprintf_s(buf, "%s:%u", ip, ntohs(a.sin_port));
        return buf;
    }

    void Handle(const char* buf, int len, const sockaddr_in& from) {
        if (len < (int)sizeof(PacketHeader)) return;
        const PacketHeader* h = (const PacketHeader*)buf;
        if (h->magic != COOP_MAGIC) return;
        if (h->version != COOP_VERSION) {
            sprintf_s(g_err, "peer protocol v%u != ours v%u", h->version, COOP_VERSION);
            return;
        }
        // after a bye, the leaving game may still send a few packets (and the bye's copies): don't
        // reconnect to it - only a fresh HELLO (the game started again) counts
        if (g_byeRecent) {
            if (Tick32() - g_byeAt >= BYE_IGNORE_MS) g_byeRecent = false;
            else if (h->type != PKT_HELLO) return;
        }

        if (g_selfBlocked && from.sin_addr.s_addr == g_selfAddr.sin_addr.s_addr && from.sin_port == g_selfAddr.sin_port) return;
        if (h->type == PKT_HELLO && len >= (int)sizeof(HelloPacket)) {
            uint32_t id = 0; memcpy(&id, ((const HelloPacket*)buf)->name + 20, 4);
            if (id && id == InstanceId()) {
                g_selfBlocked = true; g_selfAddr = from;
                sprintf_s(g_err, "that address is this PC itself - set the other player's IP");
                LOG("Net: %s (%s)", g_err, AddrToStr(from).c_str());
                return;
            }
        }
        if (g_cfg.isHost) {
            // Host accepts the first peer, or the same peer again (port may change after NAT rebind).
            bool same = g_havePeer && g_peer.sin_addr.s_addr == from.sin_addr.s_addr && g_peer.sin_port == from.sin_port;
            bool stale = g_havePeer && (Tick32() - g_stats.lastRecvTick > TIMEOUT_MS);
            if (!g_havePeer || stale || (!same && h->type == PKT_HELLO && !g_stats.connected)) {
                g_peer = from;
                g_havePeer = true;
                LOG("Host: peer is now %s", AddrToStr(from).c_str());
            } else if (!same) {
                return; // ignore third parties while a peer is connected
            }
        } else if (g_havePeer && from.sin_addr.s_addr != g_peer.sin_addr.s_addr) {
            return;     // the client only listens to the host it dialled
        }

        g_stats.packetsIn++;
        g_stats.lastRecvTick = Tick32();
        if (!g_stats.connected) {
            g_stats.connected = true;
            g_peerSaidBye = false;
            g_lastStateSeq = 0; // the peer may have restarted and begun counting from 1 again
            g_ringCount = 0;
            g_latest.valid = g_prev.valid = false;
            g_stats.peerAddr = AddrToStr(from);
            LOG("Connected to peer %s", g_stats.peerAddr.c_str());
        }

        switch (h->type) {
        case PKT_HELLO: {
            if (len < (int)sizeof(HelloPacket)) return;
            const HelloPacket* p = (const HelloPacket*)buf;
            char name[25]; memcpy(name, p->name, 24); name[24] = 0; name[16] = 0;   // 16..23 carry the colour and the instance id
            uint32_t color = (uint8_t)p->name[19] == 0xC0
                ? 0xFF000000u | ((uint32_t)(uint8_t)p->name[16] << 16) | ((uint32_t)(uint8_t)p->name[17] << 8) | (uint8_t)p->name[18] : 0;
            if (color != g_stats.peerColor) { g_stats.peerColor = color; LOG("Peer colour: %06X%s", color & 0xFFFFFF, color ? "" : " (game colour)"); }
            if (g_stats.peerName != name) {
                g_stats.peerName = name;
                LOG("Peer name: %s", name);
            }
            if (g_cfg.isHost) SendHello(); // reply so client learns our name
            break;
        }
        case PKT_PING: {
            if (len < (int)sizeof(PingPacket)) return;
            PingPacket pong{};
            FillHeader(pong.h, PKT_PONG);
            pong.echoTimeMs = h->timeMs;
            SendRaw(&pong, sizeof(pong));
            break;
        }
        case PKT_PONG: {
            if (len < (int)sizeof(PingPacket)) return;
            const PingPacket* p = (const PingPacket*)buf;
            int rtt = (int)(NowMs32() - p->echoTimeMs);
            if (rtt < 0 || rtt > 60000) break;   // not an echo of one of our pings
            g_stats.pingMs = (g_stats.pingMs < 0) ? rtt : (g_stats.pingMs * 3 + rtt) / 4;
            break;
        }
        case PKT_STATE: {
            if (len < (int)sizeof(StatePacket)) return;
            const StatePacket* p = (const StatePacket*)buf;
            if (!NetUtil::ValidCarState(p->s)) return;   // NaN / absurd numbers must not reach the game
            // Drop late / duplicate packets, but a big jump backwards means the peer restarted.
            if (h->seq <= g_lastStateSeq && g_lastStateSeq - h->seq < 300) return;
            g_lastStateSeq = h->seq;
            {   // interpolation history
                double recv = Net::LocalClockMs();
                if (g_ringCount > 0) {
                    const TimedSnap& last = g_ring[(g_ringHead + 31) % 32];
                    double sms = (double)p->s.simTime * 1000.0;
                    if (sms < last.sendMs - 5000 || sms > last.sendMs + 5000) g_ringCount = 0; // peer restarted / reloaded
                }
                TimedSnap& t = g_ring[g_ringHead];
                t.sendMs = (double)p->s.simTime * 1000.0;   // sender's simulation clock
                t.recvMs = recv;
                t.s = p->s;
                g_ringHead = (g_ringHead + 1) % 32;
                if (g_ringCount < 32) ++g_ringCount;
            }
            g_prev = g_latest;
            g_latest.valid = true;
            g_latest.recvTick = Tick32();
            g_latest.seq = h->seq;
            g_latest.s = p->s;
            break;
        }
        case PKT_CARPART: {
            if (len < (int)sizeof(CarPartPacket)) return;
            const CarPartPacket* q = (const CarPartPacket*)buf;
            static CarPacket asm_{}; static uint32_t asmId = 0, asmMask = 0;
            const int total = (int)sizeof(CarInfo), chunk = (int)sizeof(q->data);
            if (q->count == 0 || q->count > 8 || q->index >= q->count || q->len > chunk || q->index * chunk + q->len > total) return;
            if (q->id != asmId) { asmId = q->id; asmMask = 0; }
            memcpy((uint8_t*)&asm_.c + q->index * chunk, q->data, q->len);
            asmMask |= 1u << q->index;
            if (asmMask != (1u << q->count) - 1) return;
            asmMask = 0;
            asm_.h = q->h; asm_.h.type = PKT_CAR;
            if (g_havePeerCar && asm_.c.carKey == g_peerCar.carKey && asm_.c.customHash == g_peerCar.customHash) return;   // nothing new
            Handle((const char*)&asm_, (int)sizeof(asm_), from);                    // as if the whole packet came
            return;
        }
        case PKT_CAR: {
            if (len < (int)sizeof(CarPacket)) return;
            const CarPacket* p = (const CarPacket*)buf;
            if (!g_havePeerCar || p->c.carKey != g_peerCar.carKey || p->c.customHash != g_peerCar.customHash)
                LOG("Peer car: key %08X custom %s (hash %08X)", p->c.carKey, p->c.hasCustom ? "yes" : "no", p->c.customHash);
            if (!g_havePeerCar || p->c.carKey != g_peerCar.carKey || p->c.customHash != g_peerCar.customHash) {
                // remember it for the next launch: races load before the peer is driving
                FILE* f = nullptr;
                if (fopen_s(&f, g_peerCarFile, "wb") == 0 && f) { fwrite(&p->c, sizeof(CarInfo), 1, f); fclose(f); }
            }
            g_peerCar = p->c;
            g_havePeerCar = p->c.carKey != 0;
            break;
        }
        case PKT_TRAFFIC: {
            if (len < (int)offsetof(TrafficPacket, e)) return;
            const TrafficPacket* p = (const TrafficPacket*)buf;
            int n = p->count;
            if (n > MAX_TRAFFIC_PER_PACKET || len < (int)(offsetof(TrafficPacket, e) + n * sizeof(TrafficEntry))) return;
            if (!NetUtil::FiniteWithin(&p->simTime, 1, NetUtil::MAX_SIMTIME_S)) return;
            for (int i = 0; i < n; ++i) if (!NetUtil::ValidTrafficEntry(p->e[i])) return;
            if (Net::OnTraffic) Net::OnTraffic(p->e, n, (double)p->simTime * 1000.0);
            break;
        }
        case PKT_EVENT: {
            if (len < (int)sizeof(EventPacket)) return;
            const EventPacket* p = (const EventPacket*)buf;
            char id[41]; memcpy(id, p->eventId, 40); id[40] = 0;
            LOG("Peer started event %s (%08X)", id, p->eventHash);
            if (Net::OnEvent) Net::OnEvent(p->eventHash, id);
            break;
        }
        case PKT_FILE: case PKT_FILEACK: case PKT_FILEREQ: case PKT_NOFILE: {
            // A client talks to one host, but its socket takes datagrams from anyone: a file (it can
            // replace the career save) must come from the host's address.
            if (!g_cfg.isHost && from.sin_addr.s_addr != g_peer.sin_addr.s_addr) {
                static bool said = false;
                if (!said) { said = true; LOG("File packet from %s, which is not the host - ignored", AddrToStr(from).c_str()); }
                break;
            }
            HandleFilePacket(h->type, buf, len);
            break;
        }
        case PKT_CARHIT: {
            if (len < (int)sizeof(CarHitPacket)) return;
            const CarHitPacket* p = (const CarHitPacket*)buf;
            static uint32_t lastCarHit = 0;
            if (p->hitSeq == lastCarHit) return;
            lastCarHit = p->hitSeq;
            if (std::isfinite(p->dv[0]) && std::isfinite(p->dv[1]) && std::isfinite(p->dv[2]) && Net::OnCarHit) Net::OnCarHit(p->id, p->dv);
            break;
        }
        case PKT_HIT: {
            if (len < (int)sizeof(HitPacket)) return;
            const HitPacket* p = (const HitPacket*)buf;
            static uint32_t lastHit = 0;
            if (p->hitSeq == lastHit) return;                    // a copy of one we already applied
            lastHit = p->hitSeq;
            if (std::isfinite(p->dv[0]) && std::isfinite(p->dv[1]) && std::isfinite(p->dv[2]) && Net::OnHit) Net::OnHit(p->dv);
            break;
        }
        case PKT_NISSKIP: {
            if (len < (int)sizeof(EventPacket)) return;
            if (Net::OnNisSkip) Net::OnNisSkip(((const EventPacket*)buf)->eventHash);
            break;
        }
        case PKT_RESTART: {
            if (len < (int)sizeof(EventPacket)) return;
            const EventPacket* p = (const EventPacket*)buf;
            if (Net::OnRestart) { uint32_t seq = 0; memcpy(&seq, p->eventId, 4); Net::OnRestart(p->eventHash, seq); }
            break;
        }
        case PKT_RACERNAME: {
            if (len < (int)sizeof(RacerNamePacket)) return;
            const RacerNamePacket* p = (const RacerNamePacket*)buf;
            char nm[31]; memcpy(nm, p->name, 30); nm[30] = 0;
            if (Net::OnRacerName) Net::OnRacerName(p->id, nm);
            break;
        }
        case PKT_ROSTER: {
            if (len < (int)sizeof(RosterPacket)) return;
            if (Net::OnRoster) Net::OnRoster(*(const RosterPacket*)buf);
            break;
        }
        case PKT_BYE:
            g_byeAt = Tick32();
            g_byeRecent = true;
            g_peerSaidBye = true;
            LOG("Peer said bye");
            g_stats.connected = false;
            g_latest.valid = g_prev.valid = false;
            if (g_cfg.isHost) g_havePeer = false;
            break;
        }
    }
}

static bool Resolve(const std::string& host, sockaddr_in& out) {
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = htons((u_short)g_cfg.port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;
    addrinfo hints{}, *res = nullptr; // allow hostnames too
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

void Net::SetDataDir(const char* dir) {
    sprintf_s(g_peerCarFile, "%sMWTogether.peercar.bin", dir);
    FILE* f = nullptr;
    if (fopen_s(&f, g_peerCarFile, "rb") == 0 && f) {
        CarInfo c{};
        if (fread(&c, sizeof(c), 1, f) == 1 && c.carKey) {
            g_peerCar = c;
            g_havePeerCar = true;
            LOG("Peer car from last session: key %08X custom %s", c.carKey, c.hasCustom ? "yes" : "no");
        }
        fclose(f);
    }
}

static DWORD WINAPI KeepAliveThread(void*);

bool Net::Init() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { strcpy_s(g_err, "WSAStartup failed"); return false; }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { sprintf_s(g_err, "socket() failed: %d", WSAGetLastError()); return false; }

    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);

    // Windows: ignore ICMP "port unreachable" resets on UDP so recvfrom doesn't keep failing
    // while the other side isn't running yet.
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL bNewBehavior = FALSE; DWORD ret = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), nullptr, 0, &ret, nullptr, nullptr);

    // Both sides bind the same fixed port. Sending first from that port makes each side's
    // firewall treat the other side's packets as replies, so no inbound rule is needed on a LAN.
    sockaddr_in local{};
    local.sin_family = AF_INET;
#ifdef MWT_TESTS
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // tests talk to themselves: no firewall prompt
#else
    local.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
    local.sin_port = htons((u_short)g_cfg.port);
    if (bind(g_sock, (sockaddr*)&local, sizeof(local)) != 0) {
        int e = WSAGetLastError();
        if (g_cfg.isHost) {
            sprintf_s(g_err, "bind(port %d) failed: %d", g_cfg.port, e);
            LOG("%s", g_err);
            return false;
        }
        LOG("Client: port %d busy (%d), using a random port", g_cfg.port, e); // e.g. host on same PC
        local.sin_port = 0;
        if (bind(g_sock, (sockaddr*)&local, sizeof(local)) != 0) {
            sprintf_s(g_err, "bind failed: %d", WSAGetLastError());
            LOG("%s", g_err);
            return false;
        }
    }

    if (!g_cfg.isHost) {
        if (!Resolve(g_cfg.peerIp, g_peer)) {
            sprintf_s(g_err, "can't resolve HostIP '%s'", g_cfg.peerIp.c_str());
            LOG("%s", g_err);
            return false;
        }
        g_havePeer = true;
        LOG("Client: will connect to %s", AddrToStr(g_peer).c_str());
    } else {
        LOG("Host: listening on UDP port %d", g_cfg.port);
        if (!g_cfg.clientIp.empty()) {
            if (Resolve(g_cfg.clientIp, g_peer)) {
                g_havePeer = true;
                LOG("Host: also calling client at %s", AddrToStr(g_peer).c_str());
            } else {
                LOG("Host: can't resolve ClientIP '%s' (ignored)", g_cfg.clientIp.c_str());
            }
        }
    }
    if (HANDLE th = CreateThread(nullptr, 0, KeepAliveThread, nullptr, 0, nullptr)) CloseHandle(th);
    return true;
}

void Net::SendBye() {
    if (g_byeSent || !SockReady()) return;
    g_byeSent = true;
    PacketHeader bye{};
    FillHeader(bye, PKT_BYE);
    for (int i = 0; i < 3; ++i) SendRaw(&bye, sizeof(bye));
    g_leaving = true;
    LOG("Sent bye to peer");
}

void Net::Shutdown() {
    if (g_sock != INVALID_SOCKET) {
        PacketHeader bye{};
        FillHeader(bye, PKT_BYE);
        SendRaw(&bye, sizeof(bye));
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    WSACleanup();
}

void Net::Poll() {
    if (g_sock == INVALID_SOCKET) return;
    char buf[1500];
    for (int i = 0; i < 256; ++i) {
        sockaddr_in from{}; int fromLen = sizeof(from);
        int r = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (r <= 0) break;
        Handle(buf, r, from);
    }
}

// While the game is paused (alt-tab, world map, loading) its frame loop stops and so does Net::Tick.
// This thread keeps pinging the peer meanwhile so neither side times out.
namespace { volatile uint32_t g_lastTickCall = 0; }
static DWORD WINAPI KeepAliveThread(void*) {
    for (;;) {
        Sleep(500);
        SOCKET s = g_sock;
        if (s == INVALID_SOCKET) return 0;
        uint32_t last = g_lastTickCall;
        if (!last || !g_stats.connected || g_leaving || Tick32() - last < 1000) continue;
        PingPacket p{};
        p.h.magic = COOP_MAGIC; p.h.version = COOP_VERSION; p.h.type = (uint16_t)PKT_PING;
        p.h.seq = 0; p.h.timeMs = NowMs32();
        sendto(s, (const char*)&p, sizeof(p), 0, (sockaddr*)&g_peer, sizeof(g_peer));
    }
}

void Net::Tick() {
    FileTick();
    if (!SockReady()) return;
    uint32_t now = Tick32();
    // we were stalled ourselves: the peer's packets are still queued, don't count that as silence
    if (g_lastTickCall && now - g_lastTickCall > 2000) g_stats.lastRecvTick = now;
    g_lastTickCall = now;

    if (g_stats.connected && now - g_stats.lastRecvTick > TIMEOUT_MS) {
        LOG("Peer timed out");
        g_stats.connected = false;
        g_stats.pingMs = -1;
        g_latest.valid = g_prev.valid = false;
    }
    // Car info is small and important: resend every 2 s (UDP may drop it).
    if (g_stats.connected && g_haveLocalCar && now - g_lastCarSend > 2000) SendCar();
    // Client always says hello; host does too while it has a ClientIP but no connection yet.
    bool sayHello = !g_cfg.isHost || (!g_stats.connected && g_havePeer);
    if (sayHello && now - g_lastHello > (g_stats.connected ? 3000u : 1000u)) {
        g_lastHello = now;
        SendHello();
    }
    if (g_stats.connected && now - g_lastPing > 1000) {
        g_lastPing = now;
        PingPacket p{};
        FillHeader(p.h, PKT_PING);
        SendRaw(&p, sizeof(p));
    }
}

void Net::SendState(const CarState& s) {
    if (!g_stats.connected && g_cfg.isHost) return; // host has nobody to send to yet
    StatePacket p{};
    FillHeader(p.h, PKT_STATE);
    p.s = s;
    SendRaw(&p, sizeof(p));
}

void Net::SetLocalCar(const CarInfo& c) {
    bool changed = !g_haveLocalCar || c.carKey != g_localCar.carKey || c.customHash != g_localCar.customHash;
    g_localCar = c;
    g_haveLocalCar = c.carKey != 0;
    if (changed && g_haveLocalCar) {
        LOG("Local car: key %08X custom %s (hash %08X)", c.carKey, c.hasCustom ? "yes" : "no", c.customHash);
        if (g_stats.connected) SendCar();
    }
}

void (*Net::OnTraffic)(const TrafficEntry*, int, double) = nullptr;
double (*Net::LocalClockMs)() = &Net::NowMs;
void (*Net::OnEvent)(uint32_t, const char*) = nullptr;
void (*Net::OnRoster)(const RosterPacket&) = nullptr;
void (*Net::OnRacerName)(uint16_t, const char*) = nullptr;
void (*Net::OnRestart)(uint32_t, uint32_t) = nullptr;
void (*Net::OnNisSkip)(uint32_t) = nullptr;

void Net::SendNisSkip(uint32_t sceneHash) {
    if (!g_stats.connected) return;
    EventPacket p{};
    FillHeader(p.h, PKT_NISSKIP);
    p.eventHash = sceneHash;
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));
}

void Net::SendRestart(uint32_t eventHash, uint32_t seq) {
    if (!g_stats.connected) return;
    EventPacket p{};
    FillHeader(p.h, PKT_RESTART);
    p.eventHash = eventHash; memcpy(p.eventId, &seq, 4);   // sequence number in the (otherwise unused) id field
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));
}

void Net::SendRacerName(uint16_t id, const char* name) {
    if (!g_stats.connected) return;
    RacerNamePacket p{};
    FillHeader(p.h, PKT_RACERNAME);
    p.id = id;
    NetUtil::CopyHudName(p.name, name);       // the HUD standings copy names into 16-byte slots
    SendRaw(&p, sizeof(p));
}

void Net::SendRoster(const RosterPacket& in) {
    if (!g_stats.connected) return;
    RosterPacket p = in;
    FillHeader(p.h, PKT_ROSTER);
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));
}

void Net::SendEvent(uint32_t eventHash, const char* eventId) {
    if (!g_stats.connected) return;
    EventPacket p{};
    FillHeader(p.h, PKT_EVENT);
    p.eventHash = eventHash;
    strncpy_s(p.eventId, eventId ? eventId : "", _TRUNCATE);
    for (int i = 0; i < 3; ++i) SendRaw(&p, sizeof(p));   // important + tiny: send 3 copies (UDP)
}

// Clock mapping (our ms - peer ms). The raw estimate (fastest packet in the last ~0.5 s) jumps by a
// few ms whenever that packet ages out, which would make remote cars jump. So we glide the estimate
// towards the raw value by at most 2 ms per second, and only snap on big changes (peer restarted).
static double g_offsetEst = 0, g_offsetAt = 0;
static bool   g_haveOffset = false;
double Net::PeerClockOffsetMs() {
    if (g_ringCount == 0) return g_haveOffset ? g_offsetEst : 0.0;
    double raw = 1e300;
    for (int i = 0; i < g_ringCount; ++i) {
        const TimedSnap& t = g_ring[(g_ringHead + 32 - 1 - i) % 32];
        raw = (std::min)(raw, t.recvMs - t.sendMs);
    }
    double now = Net::LocalClockMs();
    if (!g_haveOffset || fabs(raw - g_offsetEst) > 50.0) {
        g_offsetEst = raw; g_haveOffset = true;
    } else {
        double el = now - g_offsetAt;
        if (el < 0) el = 0;
        double maxStep = el * 0.002;                     // 2 ms per second
        double d = raw - g_offsetEst;
        if (d > maxStep) d = maxStep; else if (d < -maxStep) d = -maxStep;
        g_offsetEst += d;
    }
    g_offsetAt = now;
    return g_offsetEst;
}

void Net::SendTraffic(const TrafficEntry* e, int count, float simTime) {
    if (!g_stats.connected) return;
    TrafficPacket p{};
    FillHeader(p.h, PKT_TRAFFIC);
    if (count > MAX_TRAFFIC_PER_PACKET) count = MAX_TRAFFIC_PER_PACKET;
    if (count < 0 || !e) count = 0;
    p.count = (uint8_t)count;
    p.simTime = simTime;
    memcpy(p.e, e, count * sizeof(TrafficEntry));
    SendRaw(&p, (int)(offsetof(TrafficPacket, e) + count * sizeof(TrafficEntry)));
}

double Net::NowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

using NetUtil::Lerp3;
using NetUtil::NlerpQ;

bool Net::SampleRemote(double delayMs, CarState& out, double* extrapolatedMs) {
    if (g_ringCount == 0) return false;
    double target = Net::LocalClockMs() - Net::PeerClockOffsetMs() - delayMs; // in sender sim time
    // newest first
    const TimedSnap* newer = nullptr; const TimedSnap* older = nullptr;
    for (int i = 0; i < g_ringCount; ++i) {
        const TimedSnap& t = g_ring[(g_ringHead + 32 - 1 - i) % 32];
        if (t.sendMs >= target) newer = &t;
        else { older = &t; break; }
    }
    if (extrapolatedMs) *extrapolatedMs = 0;
    if (older && newer && newer->sendMs > older->sendMs) {
        double span = newer->sendMs - older->sendMs;
        float u = (float)((target - older->sendMs) / span);
        float dt = (float)(span / 1000.0);
        // cubic Hermite on position with velocities as tangents
        out = newer->s;
        for (int i = 0; i < 3; ++i)
            out.pos[i] = NetUtil::Hermite(older->s.pos[i], older->s.linVel[i], newer->s.pos[i], newer->s.linVel[i], dt, u);
        Lerp3(older->s.linVel, newer->s.linVel, u, out.linVel);
        Lerp3(older->s.angVel, newer->s.angVel, u, out.angVel);
        NlerpQ(older->s.rot, newer->s.rot, u, out.rot);
        out.speedMps = older->s.speedMps + (newer->s.speedMps - older->s.speedMps) * u;
        return true;
    }
    // Only newer samples (target before history) or only older ones (late packets): use the newest,
    // extrapolated by velocity for at most 250 ms.
    const TimedSnap& n = g_ring[(g_ringHead + 31) % 32];
    out = n.s;
    double ex = target - n.sendMs;
    if (ex > 0) {
        if (ex > 250) ex = 250;
        float e = (float)(ex / 1000.0);
        for (int i = 0; i < 3; ++i) out.pos[i] += out.linVel[i] * e;
        if (extrapolatedMs) *extrapolatedMs = ex;
    }
    return true;
}

const CarInfo* Net::PeerCar() { return g_havePeerCar ? &g_peerCar : nullptr; }

const Net::Snapshot& Net::Latest() { return g_latest; }
const Net::Snapshot& Net::Previous() { return g_prev; }
const NetStats& Net::Stats() { return g_stats; }
const char* Net::LastError() { return g_err; }

bool Net::PeerSaidBye() { return g_peerSaidBye; }

#ifdef MWT_TESTS
// ---------------------------------------------------------------------------------------------
// Test hooks (tests\, built with /DMWT_TESTS; not part of the plugin).
// ---------------------------------------------------------------------------------------------
void Net::Test_Reset() {
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    g_peer = sockaddr_in{};
    g_havePeer = false;
    g_seq = 0;
    g_lastHello = g_lastPing = 0;
    g_lastStateSeq = 0;
    g_stats = NetStats{};
    g_latest = Net::Snapshot{}; g_prev = Net::Snapshot{};
    g_err[0] = 0;
    g_localCar = CarInfo{}; g_peerCar = CarInfo{};
    g_ringHead = g_ringCount = 0;
    g_peerCarFile[0] = 0;
    g_haveLocalCar = g_havePeerCar = false;
    g_lastCarSend = 0;
    g_peerSaidBye = g_leaving = g_byeSent = g_byeRecent = false;
    g_byeAt = 0;
    g_lastTickCall = 0;
    g_offsetEst = g_offsetAt = 0;
    g_haveOffset = false;
    g_testTickOn = false; g_testTick = 0;
    g_testCapture = nullptr;
    Net::OnTraffic = nullptr; Net::OnEvent = nullptr; Net::OnRoster = nullptr;
    Net::OnRacerName = nullptr; Net::OnRestart = nullptr; Net::OnNisSkip = nullptr;
    Net::OnFile = nullptr; Net::OnFileRequest = nullptr; Net::OnNoFile = nullptr;
    FileReset();
    Net::LocalClockMs = &Net::NowMs;
}

void Net::Test_HandlePacket(const void* buf, int len, uint32_t fromIp, uint16_t fromPort) {
    sockaddr_in from{};
    from.sin_family = AF_INET;
    from.sin_addr.s_addr = htonl(fromIp);
    from.sin_port = htons(fromPort);
    Handle((const char*)buf, len, from);
}

void Net::Test_SetTick(bool on, uint32_t ms) { g_testTickOn = on; g_testTick = ms; }
void Net::Test_SetCapture(void (*fn)(const void*, int)) { g_testCapture = fn; }

void Net::Test_SetPeer(uint32_t ip, uint16_t port) {
    g_peer = sockaddr_in{};
    g_peer.sin_family = AF_INET;
    g_peer.sin_addr.s_addr = htonl(ip);
    g_peer.sin_port = htons(port);
    g_havePeer = true;
}

Net::TestState Net::Test_GetState() {
    TestState t{};
    t.havePeer = g_havePeer;
    t.peerIp = ntohl(g_peer.sin_addr.s_addr);
    t.peerPort = ntohs(g_peer.sin_port);
    t.lastStateSeq = g_lastStateSeq;
    t.ringCount = g_ringCount;
    t.leaving = g_leaving;
    t.socketOpen = g_sock != INVALID_SOCKET;
    return t;
}
#endif

// ---------------------------------------------------------------------------
// Small file transfer (career save backup / restore). The sender repeats all chunks, a few per
// frame, until the receiver acknowledges the file's hash or ~10 s pass.
// ---------------------------------------------------------------------------
void (*Net::OnFile)(const char*, const char*, const uint8_t*, uint32_t, uint64_t) = nullptr;
void (*Net::OnFileRequest)(const char*) = nullptr;
void (*Net::OnNoFile)() = nullptr;

namespace {
    uint32_t Fnv(const uint8_t* p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; } return h; }

    struct OutFile {
        std::vector<uint8_t> data; uint32_t hash = 0; uint64_t mtime = 0; char owner[32] = {}, profile[32] = {};
        int next = 0, rounds = 0; bool active = false, delivered = false; uint32_t roundAt = 0;
    } g_out;
    struct InFile { std::vector<uint8_t> data; std::vector<uint8_t> have; uint32_t hash = 0, size = 0; int got = 0; bool active = false; } g_in;
    // the last file we received completely: its chunks are only acknowledged again
    bool     g_haveDone = false;
    uint32_t g_doneHash = 0, g_doneSize = 0;

    void FileReset() { g_out = OutFile{}; g_in = InFile{}; g_haveDone = false; g_doneHash = g_doneSize = 0; }

    void FileTick() {
        if (!g_out.active) return;
        if (!g_stats.connected) { LOG("File: the peer is gone - %s not delivered", g_out.profile); g_out.active = false; return; }
        int total = (int)((g_out.data.size() + FILE_CHUNK - 1) / FILE_CHUNK);
        if (g_out.next >= total) {                       // a round is done: wait a little, then repeat
            if (Tick32() - g_out.roundAt < 700) return;
            if (++g_out.rounds >= 14) { LOG("File: no acknowledgement for %s - giving up", g_out.profile); g_out.active = false; return; }
            g_out.next = 0;
        }
        for (int n = 0; n < 6 && g_out.next < total; ++n, ++g_out.next) {
            FileChunkPacket p{};
            FillHeader(p.h, PKT_FILE);
            p.fileHash = g_out.hash; p.size = (uint32_t)g_out.data.size(); p.mtime = g_out.mtime;
            p.index = (uint16_t)g_out.next; p.total = (uint16_t)total;
            memcpy(p.owner, g_out.owner, 32); memcpy(p.profile, g_out.profile, 32);
            uint32_t off = (uint32_t)g_out.next * FILE_CHUNK;
            uint32_t len = (std::min)((uint32_t)FILE_CHUNK, p.size - off);
            memcpy(p.data, g_out.data.data() + off, len);
            SendRaw(&p, (int)(offsetof(FileChunkPacket, data) + len));
        }
        if (g_out.next >= total) g_out.roundAt = Tick32();
    }

    void HandleFilePacket(uint16_t type, const char* buf, int len) {
        if (type == PKT_FILEACK) {
            if (len < (int)sizeof(FileAckPacket)) return;
            if (g_out.active && ((const FileAckPacket*)buf)->fileHash == g_out.hash) {
                g_out.active = false; g_out.delivered = true;
                LOG("File: %s delivered", g_out.profile);
            }
            return;
        }
        if (type == PKT_FILEREQ) {
            if (len < (int)sizeof(FileReqPacket)) return;
            char owner[33]; memcpy(owner, ((const FileReqPacket*)buf)->owner, 32); owner[32] = 0;
            if (Net::OnFileRequest) Net::OnFileRequest(owner);
            return;
        }
        if (type == PKT_NOFILE) { if (Net::OnNoFile) Net::OnNoFile(); return; }
        // PKT_FILE
        if (len < (int)offsetof(FileChunkPacket, data)) return;
        const FileChunkPacket* p = (const FileChunkPacket*)buf;
        if (p->size == 0 || p->size > FILE_MAX) return;
        uint32_t total = (p->size + FILE_CHUNK - 1) / FILE_CHUNK;
        if (p->total != total || p->index >= total) return;
        uint32_t off = (uint32_t)p->index * FILE_CHUNK;
        uint32_t clen = (std::min)((uint32_t)FILE_CHUNK, p->size - off);
        if (len < (int)(offsetof(FileChunkPacket, data) + clen)) return;
        auto ack = [&]() { FileAckPacket a{}; FillHeader(a.h, PKT_FILEACK); a.fileHash = p->fileHash; SendRaw(&a, sizeof(a)); };
        if (g_haveDone && p->fileHash == g_doneHash && p->size == g_doneSize) { ack(); return; }   // our ack was lost
        if (!g_in.active || g_in.hash != p->fileHash || g_in.size != p->size) {  // a new file
            g_in.active = true;
            g_in.hash = p->fileHash; g_in.size = p->size; g_in.got = 0;
            g_in.data.assign(p->size, 0); g_in.have.assign(total, 0);
        }
        if (g_in.have[p->index]) return;
        memcpy(g_in.data.data() + off, p->data, clen);
        g_in.have[p->index] = 1;
        if (++g_in.got < (int)total) return;
        g_in.active = false;                             // whole: a later chunk of it starts over
        if (Fnv(g_in.data.data(), g_in.size) != g_in.hash) { LOG("File: hash mismatch - dropped"); return; }
        g_haveDone = true; g_doneHash = g_in.hash; g_doneSize = g_in.size;
        ack();
        char owner[33], profile[33]; memcpy(owner, p->owner, 32); owner[32] = 0; memcpy(profile, p->profile, 32); profile[32] = 0;
        if (Net::OnFile) Net::OnFile(owner, profile, g_in.data.data(), g_in.size, p->mtime);
    }
}

bool Net::SendFile(const char* owner, const char* profile, const void* data, uint32_t size, uint64_t mtime) {
    if (!g_stats.connected || g_out.active || !data || !owner || !profile || !size || size > FILE_MAX) return false;
    g_out = OutFile{};
    g_out.data.assign((const uint8_t*)data, (const uint8_t*)data + size);
    g_out.hash = Fnv(g_out.data.data(), size); g_out.mtime = mtime;
    strncpy_s(g_out.owner, owner, _TRUNCATE); strncpy_s(g_out.profile, profile, _TRUNCATE);
    g_out.active = true;
    return true;
}
bool Net::SendingFile() { return g_out.active; }
bool Net::LastFileDelivered() { return !g_out.active && g_out.delivered; }
void Net::RequestFile(const char* owner) {
    FileReqPacket p{}; FillHeader(p.h, PKT_FILEREQ); strncpy_s(p.owner, owner ? owner : "", _TRUNCATE);
    for (int i = 0; i < 2; ++i) SendRaw(&p, sizeof(p));
}
void Net::SendNoFile() { PacketHeader p{}; FillHeader(p, PKT_NOFILE); for (int i = 0; i < 2; ++i) SendRaw(&p, sizeof(p)); }
void (*Net::OnHit)(const float*) = nullptr;
void Net::SendHit(const float* dv) {
    if (!g_stats.connected) return;
    static uint32_t seq = GetTickCount();       // differs after a relaunch, so the first hit isn't taken for a repeat
    HitPacket p{};
    FillHeader(p.h, PKT_HIT);
    p.hitSeq = ++seq; memcpy(p.dv, dv, sizeof(p.dv));
    for (int i = 0; i < 2; ++i) SendRaw(&p, sizeof(p));
}

void (*Net::OnCarHit)(uint16_t, const float*) = nullptr;
void Net::SendCarHit(uint16_t id, const float* dv) {
    if (!g_stats.connected) return;
    static uint32_t seq = GetTickCount();
    CarHitPacket p{};
    FillHeader(p.h, PKT_CARHIT);
    p.hitSeq = ++seq; p.id = id; memcpy(p.dv, dv, sizeof(p.dv));
    for (int i = 0; i < 2; ++i) SendRaw(&p, sizeof(p));
}
