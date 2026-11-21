// Tests for the XXH3 string hash and hash exchange; see BRICKADIA_CHANGES.md.
#include "Brickadia.Helpers.h"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

TEST_SUITE_BEGIN("Brickadia");

TEST_CASE("StringHashExchange")
{
    StateRef state = newState();
    lua_State* L = state.get();

    // Lengths cover each XXH3 code path, including the >240 byte one.
    for (size_t length : {0, 1, 3, 4, 8, 9, 16, 17, 128, 129, 240, 241, 1000})
    {
        std::string text(length, 'a');
        for (size_t i = 0; i < length; ++i)
            text[i] = char('a' + i % 26);

        const unsigned hash = unsigned(XXH3_64bits(text.data(), text.size()));

        lua_pushlstringhashed(L, text.data(), text.size(), hash);
        lua_pushlstring(L, text.data(), text.size());
        CHECK(lua_rawequal(L, -1, -2));

        size_t outLength = 0;
        unsigned outHash = 0;
        lua_tolstringhashed(L, -1, &outLength, &outHash);
        CHECK(outLength == length);
        CHECK(outHash == hash);
        lua_pop(L, 2);
    }

    lua_pushnumber(L, 12.5);
    size_t outLength = 0;
    unsigned outHash = 0;
    const char* text = lua_tolstringhashed(L, -1, &outLength, &outHash);
    REQUIRE(text);
    CHECK(outHash == unsigned(XXH3_64bits(text, outLength)));
    CHECK(lua_tolstringhashed(L, -1, nullptr, nullptr) == text);
}

TEST_SUITE_END();
