#include "mwt_test.h"
#include "../src/profileutil.h"

TEST(profile_listed_names) {
    CHECK(Profiles::Listed("StCost"));
    CHECK(Profiles::Listed("Abcdefg"));          // 7 characters
    CHECK(!Profiles::Listed("Abcdefgh"));        // 8: not in the game's list
    CHECK(!Profiles::Listed("Ab cd"));
    CHECK(!Profiles::Listed(""));
    CHECK(!Profiles::Listed(nullptr));
}

TEST(profile_index_in_list) {
    std::vector<std::string> l = { "Second", "StCost" };
    CHECK_EQ(Profiles::IndexOf(l, "StCost"), 1);
    CHECK_EQ(Profiles::IndexOf(l, "stcost"), 1);
    CHECK_EQ(Profiles::IndexOf(l, "Second"), 0);
    CHECK_EQ(Profiles::IndexOf(l, "Other"), -1);
    CHECK_EQ(Profiles::IndexOf(l, ""), -1);
}

TEST(profile_alias_in_block) {
    const uint8_t b[] = { 1, 2, 0, 'A', 'l', 0, 9, 'x', 'A', 'l', 'e', 'x', 0, 0, 'A', 'l', 'e', 'x', 0, 7 };
    std::vector<std::string> names = { "Al", "Alex", "Bob" };
    CHECK(Profiles::FindAlias(b, sizeof(b), names) == "Alex");          // longest whole string
    std::vector<std::string> only = { "Bob" };
    CHECK(Profiles::FindAlias(b, sizeof(b), only).empty());
    const uint8_t tail[] = { 'B', 'o', 'b' };                           // no NUL after it: not a string
    CHECK(Profiles::FindAlias(tail, sizeof(tail), only).empty());
    const uint8_t inner[] = { 0, 'x', 'B', 'o', 'b', 0 };               // end of a longer string
    CHECK(Profiles::FindAlias(inner, sizeof(inner), only).empty());
    const uint8_t start[] = { 'B', 'o', 'b', 0 };
    CHECK(Profiles::FindAlias(start, sizeof(start), only) == "Bob");
}
