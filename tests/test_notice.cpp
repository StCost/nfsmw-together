// Notices in the centre of the screen (src/noticeutil.h).
#include "mwt_test.h"
#include "../src/noticeutil.h"
#include <cstring>

TEST(notice_short_stays_whole) {
    auto p = Notice::Split("Laptop joined");
    CHECK_EQ((int)p.size(), 1); CHECK(p[0] == "Laptop joined");
    CHECK(Notice::Split("").empty());
    std::string exact(63, 'a');
    CHECK_EQ((int)Notice::Split(exact).size(), 1);
}

TEST(notice_split_at_dash) {
    auto p = Notice::Split("No answer from the host - check the host's IP and allow UDP 47555 in this PC's firewall");
    CHECK_EQ((int)p.size(), 2);
    CHECK(p[0] == "No answer from the host");
    CHECK(p[1] == "Check the host's IP and allow UDP 47555 in this PC's firewall");
}

TEST(notice_parts_fit) {
    const char* texts[] = {
        "No answer from the host - check the host's IP, and allow UDP 47555 in the firewall on this PC",
        "Laptop is at Blacklist #12, you are at #13 - beat your rival to catch up",
        "Laptop doesn't receive your game - allow UDP 47555 in the firewall on their PC",
        "averyveryveryveryveryveryveryveryveryveryveryveryveryveryverylongwordthatneverends and more" };
    for (const char* t : texts) {
        auto p = Notice::Split(t);
        CHECK(!p.empty());
        size_t chars = 0;
        for (auto& s : p) { CHECK(s.size() <= Notice::MaxChars); CHECK(!s.empty()); chars += s.size(); }
        CHECK(chars + 3 * p.size() >= strlen(t) - 3 * 4);   // nothing but separators lost
    }
}

TEST(notice_packs_small_pieces) {
    auto p = Notice::Split("aaaa - bbbb - " + std::string(60, 'c'));
    CHECK_EQ((int)p.size(), 2);
    CHECK(p[0] == "aaaa - bbbb");
}
