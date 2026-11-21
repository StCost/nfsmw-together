// Included from game.cpp (after nis.inl).
// ---------------------------------------------------------------------------
// Career save kept by the host
//  Every player plays their own career profile. The client's save file (Documents\NFS Most Wanted\
//  <profile>\<profile>, ~64 KB, written by the game's autosave) is uploaded to the host whenever it
//  changes; the host keeps it in scripts\MWTogether-saves\<player>\<profile>.
//  When the client starts the game and connects before its profile is loaded, it asks the host for
//  that copy. If the client's own file is missing or older, the host's copy is written in its place
//  (the old file is kept as <profile>.mwt-backup) and the game then loads it as usual. The file is
//  never edited: the game checks an MD5 inside it.
// ---------------------------------------------------------------------------
#include <shlobj.h>
#include "saveutil.h"

namespace {
    enum class Restore { Idle, Requested, Done } g_restore = Restore::Idle;
    DWORD    g_restoreAt = 0, g_saveFirstTick = 0;
    uint64_t g_uploadedTime = 0;      // file time of the save the host is known to have
    uint64_t g_pendingTime = 0;       // file time of the save being uploaded right now
    char     g_saveStatus[96] = "";

    using SaveUtil::SafeName;
    using SaveUtil::FileTime64;
    std::string DocsSavePath(const char* profile) {
        char docs[MAX_PATH];
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs))) return "";
        return std::string(docs) + "\\NFS Most Wanted\\" + profile + "\\" + profile;
    }
    std::string HostDir(const char* owner) { return std::string(g_dataDir) + "MWTogether-saves\\" + owner; }

    bool ProfileLoaded() { uint8_t* fedb = nullptr; return Mem::Read(0x91CF90, fedb) && fedb && fedb[3]; }
    bool CurrentProfile(char* out, size_t n) {
        uint8_t* fedb = nullptr; uint8_t* up = nullptr; char raw[33] = {};
        if (!Mem::Read(0x91CF90, fedb) || !fedb || !fedb[3] || !Mem::Read((uintptr_t)fedb + 0x10, up) || !up) return false;
        if (!Mem::SafeRead(up, raw, 32)) return false;
        return SafeName(raw, out, n) && !strcmp(raw, out);    // only plain names (no path tricks)
    }

    // host: a client's save arrived
    void HostStore(const char* ownerRaw, const char* profileRaw, const uint8_t* d, uint32_t n, uint64_t mtime) {
        char owner[40], profile[40];
        // the profile must already be a plain name (our client sends nothing else): never guess
        if (!SafeName(ownerRaw, owner, sizeof(owner)) || !SaveUtil::PlainName(profileRaw)) {
            LOG("Career: a save with an unusable player or profile name arrived - not stored");
            return;
        }
        strcpy_s(profile, profileRaw);
        CreateDirectoryA((std::string(g_dataDir) + "MWTogether-saves").c_str(), nullptr);
        std::string dir = HostDir(owner);
        CreateDirectoryA(dir.c_str(), nullptr);
        // only the connected player's own save, and never an older one over a newer one
        char peer[40] = "";
        if (!SafeName(Net::Stats().peerName.c_str(), peer, sizeof(peer)) || _stricmp(peer, owner)) {
            LOG("Career: a save for '%s' arrived from player '%s' - not stored", owner, peer);
            return;
        }
        std::string target = dir + "\\" + profile;
        uint64_t have = FileTime64(target);
        if (have > mtime) { LOG("Career: %s's save is older than the copy we keep - kept ours", owner); return; }
        if (have && have != mtime) CopyFileA(target.c_str(), (target + ".prev").c_str(), FALSE);   // one generation back
        // one save per player: remember which profile it is
        if (SaveUtil::WriteAllAtomic(dir + "\\" + profile, d, n, mtime)) {
            WritePrivateProfileStringA("Save", "Profile", profile, (dir + "\\save.ini").c_str());
            LOG("Career: stored %s's save '%s' (%u bytes)", owner, profile, n);
            sprintf_s(g_saveStatus, "keeping %s's save", owner);
        } else LOG("Career: could not write %s's save '%s' (the copy we had is untouched)", owner, profile);
    }
    // host: the client asks for its save
    void HostSendBack(const char* ownerRaw) {
        char owner[40], profile[40] = "";
        if (Net::SendingFile()) return;                  // the request comes twice: already answering
        if (!SafeName(ownerRaw, owner, sizeof(owner))) { Net::SendNoFile(); return; }
        std::string dir = HostDir(owner);
        char raw[40] = "";
        GetPrivateProfileStringA("Save", "Profile", "", raw, sizeof(raw), (dir + "\\save.ini").c_str());
        std::vector<uint8_t> data;
        if (!SafeName(raw, profile, sizeof(profile)) || !SaveUtil::ReadAll(dir + "\\" + profile, data, FILE_MAX)) { Net::SendNoFile(); return; }
        if (Net::SendFile(owner, profile, data.data(), (uint32_t)data.size(), FileTime64(dir + "\\" + profile)))
            LOG("Career: sending %s their save '%s' back", owner, profile);
    }
    // client: the host's copy of our save arrived
    void ClientRestore(const char* ownerRaw, const char* profileRaw, const uint8_t* d, uint32_t n, uint64_t mtime) {
        char profile[40];
        g_restore = Restore::Done;
        // the host answers with the file-name form of our name; the profile must be a plain name
        if (!SaveUtil::SameOwner(ownerRaw, g_cfg.playerName.c_str()) || !SaveUtil::PlainName(profileRaw)) {
            LOG("Career: the host sent a save that isn't ours or has an unusable name - not applied");
            return;
        }
        strcpy_s(profile, profileRaw);
        if (Game::GameFlowState() != 3 || ProfileLoaded()) { LOG("Career: host's copy arrived after the profile was loaded - not applied"); return; }
        std::string path = DocsSavePath(profile);
        if (path.empty()) return;
        uint64_t mine = FileTime64(path);
        if (!SaveUtil::HostCopyIsNewer(mine, mtime)) { LOG("Career: our save '%s' is up to date", profile); return; }
        // the save file also holds this player's own settings (camera, units, controls): an existing save is
        // never replaced, only a missing one is restored ([Career] RestoreOnlyMissing, on by default)
        if (mine && g_cfg.restoreOnlyMissing) { LOG("Career: the host's copy of '%s' is newer, but we keep our own save and settings (RestoreOnlyMissing=1)", profile); return; }
        if (mine) {
            // no backup, no overwrite
            if (!CopyFileA(path.c_str(), (path + ".mwt-backup").c_str(), FALSE)) {
                LOG("Career: could not back up our save '%s' (error %lu) - the host's copy is not applied", profile, GetLastError());
                return;
            }
        } else {
            char docs[MAX_PATH];
            SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, docs);
            CreateDirectoryA((std::string(docs) + "\\NFS Most Wanted").c_str(), nullptr);
            CreateDirectoryA((std::string(docs) + "\\NFS Most Wanted\\" + profile).c_str(), nullptr);
        }
        if (SaveUtil::WriteAllAtomic(path, d, n, mtime)) {
            g_uploadedTime = mtime;
            LOG("Career: restored save '%s' from the host (%s)", profile, mine ? "ours was older; kept as .mwt-backup" : "we had none");
            strcpy_s(g_saveStatus, "save restored from the host");
        } else LOG("Career: could not write the host's copy of '%s' - our save is untouched", profile);
    }
}

// The fast start waits with "Press START" while the host's copy of our save may still arrive.
static bool SaveSyncHoldsBoot() {
    if (g_cfg.isHost || !g_cfg.saveSync || g_restore == Restore::Done) return false;
    if (!g_saveFirstTick) g_saveFirstTick = GetTickCount();
    // (SaveTick ends the wait after 4 s; the second limit is for the case it isn't running)
    if (g_restore == Restore::Requested) return GetTickCount() - g_restoreAt < 6000;
    return GetTickCount() - g_saveFirstTick < 1500;      // give the connection a moment
}

static void SaveTickImpl(bool connected) {
    DWORD now = GetTickCount();
    if (!g_saveFirstTick) g_saveFirstTick = now;
    if (g_cfg.isHost) return;
    // restore: ask once, before our profile is loaded
    if (g_restore == Restore::Idle && connected && Game::GameFlowState() == 3 && !ProfileLoaded()) {
        Net::RequestFile(g_cfg.playerName.c_str());
        g_restore = Restore::Requested; g_restoreAt = now;
        LOG("Career: asking the host for our save");
    }
    if (g_restore == Restore::Requested && now - g_restoreAt > 4000) { g_restore = Restore::Done; LOG("Career: no answer from the host about our save"); }
    if (g_restore == Restore::Idle && ProfileLoaded()) g_restore = Restore::Done;
    // backup: upload our save whenever the game has written it
    static DWORD nextCheck = 0;
    if (g_pendingTime && !Net::SendingFile()) {          // the upload ended: only an acknowledged one counts
        if (Net::LastFileDelivered()) { g_uploadedTime = g_pendingTime; strcpy_s(g_saveStatus, "save backed up on the host"); }
        else LOG("Career: the host did not confirm our save - will send it again");
        g_pendingTime = 0;
    }
    if (!connected || (int)(now - nextCheck) < 0 || Net::SendingFile()) return;
    nextCheck = now + 5000;
    char profile[40];
    if (!CurrentProfile(profile, sizeof(profile))) return;
    std::string path = DocsSavePath(profile);
    uint64_t t = FileTime64(path);
    if (!t || t == g_uploadedTime) return;
    FILETIME ftNow; GetSystemTimeAsFileTime(&ftNow);
    uint64_t n64 = ((uint64_t)ftNow.dwHighDateTime << 32) | ftNow.dwLowDateTime;
    if (n64 - t < 20000000ull) return;                   // written under 2 s ago: let the game finish
    std::vector<uint8_t> data;
    if (SaveUtil::ReadAll(path, data, FILE_MAX) && Net::SendFile(g_cfg.playerName.c_str(), profile, data.data(), (uint32_t)data.size(), t)) {
        g_pendingTime = t;
        LOG("Career: sending our save '%s' to the host (%u bytes)", profile, (unsigned)data.size());
    }
}

void Game::SaveTick(bool connected) {
    if (!g_ok || !g_cfg.saveSync) return;
    SaveTickImpl(connected);
}
const char* Game::SaveStatus() { return g_saveStatus; }

void Game::InstallSaveSync() {
    char safe[40];
    if (g_cfg.saveSync && !g_cfg.isHost && !SafeName(g_cfg.playerName.c_str(), safe, sizeof(safe)))
        LOG("Career: the player name has no letters or digits the host can use as a folder name - the save can't be kept there");
    Net::OnFile = [](const char* owner, const char* profile, const uint8_t* d, uint32_t n, uint64_t mtime) {
        if (!g_cfg.saveSync) return;
        if (g_cfg.isHost) HostStore(owner, profile, d, n, mtime);
        else ClientRestore(owner, profile, d, n, mtime);
    };
    Net::OnFileRequest = [](const char* owner) { if (g_cfg.isHost && g_cfg.saveSync) HostSendBack(owner); else Net::SendNoFile(); };
    Net::OnNoFile = []() { if (g_restore == Restore::Requested) { g_restore = Restore::Done; LOG("Career: the host keeps no save for us yet"); } };
}
