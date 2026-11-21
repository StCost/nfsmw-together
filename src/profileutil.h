#pragma once
// Fast start with several profiles: the decisions that don't need the game (tests\test_profile.cpp).
// boot.inl reads the game's profile list / loaded profile and asks these.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Profiles {
    // The boot's profile list shows only names shorter than 8 characters without a space (the list
    // setup at 0x563A26 skips the others).
    inline bool Listed(const char* name) {
        size_t n = name ? strlen(name) : 0;
        return n > 0 && n < 8 && !strchr(name, ' ');
    }
    // Position of `want` among the listed names (case-insensitive), -1 if it isn't there.
    inline int IndexOf(const std::vector<std::string>& listed, const std::string& want) {
        if (want.empty()) return -1;
        for (size_t i = 0; i < listed.size(); ++i)
            if (_stricmp(listed[i].c_str(), want.c_str()) == 0) return (int)i;
        return -1;
    }
    // Which saved profile is the loaded one: its alias is a string inside the user profile block. A
    // name counts only as a whole NUL-terminated string (not the end of a longer one). The longest
    // match wins (a profile "Al" must not hide "Alex"). Empty if none.
    inline std::string FindAlias(const uint8_t* buf, size_t n, const std::vector<std::string>& names) {
        std::string best;
        for (const std::string& s : names) {
            size_t l = s.size();
            if (!l || l + 1 > n || l <= best.size()) continue;
            for (size_t i = 0; i + l < n; ++i) {
                if ((i == 0 || buf[i - 1] == 0) && buf[i + l] == 0 && memcmp(buf + i, s.data(), l) == 0) { best = s; break; }
            }
        }
        return best;
    }
}
