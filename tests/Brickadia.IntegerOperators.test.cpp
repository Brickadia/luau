// Tests for the native integer operators; see BRICKADIA_CHANGES.md.
#include "Brickadia.Helpers.h"
#include "ScopedFlags.h"

LUAU_FASTFLAG(LuauIntegerLibrary)
LUAU_FASTFLAG(LuauIntegerType2)
LUAU_FASTFLAG(LuauIntegerFastcalls)

TEST_SUITE_BEGIN("Brickadia");

TEST_CASE("IntegerOperators")
{
    ScopedFastFlag integerType{FFlag::LuauIntegerType2, true};
    ScopedFastFlag integerLibrary{FFlag::LuauIntegerLibrary, true};
    ScopedFastFlag integerFastcalls{FFlag::LuauIntegerFastcalls, true};

    StateRef state = newState();
    lua_State* L = state.get();

    runScript(L, R"(
        local function noinline(x: integer): integer local s, r = pcall(function(y) return y end, x) return r end
        local function errs(f, message)
            local ok, err = pcall(f)
            assert(not ok, "expected an error")
            if message then assert(string.find(tostring(err), message, 1, true), tostring(err)) end
        end

        local maxs, mins = integer.maxsigned, integer.minsigned

        -- Floor division and modulo agree with integer.idiv/integer.mod and satisfy the division identity.
        for _, c in {{-1i, 2i, -1i}, {1i, -2i, -1i}, {-1i, -2i, 0i}, {-7i, 2i, -4i}, {7i, -2i, -4i}, {7i, 2i, 3i}, {-7i, -2i, 3i}, {0i, 5i, 0i}} do
            local a, b = noinline(c[1]), noinline(c[2])
            assert(a // b == c[3])
            assert(a // b == integer.idiv(a, b))
            assert(a % b == integer.mod(a, b))
            assert((a // b) * b + a % b == a)
        end

        assert(noinline(-7i) / noinline(2i) == -3i)

        errs(function() return noinline(mins) / noinline(-1i) end, "integer overflow")
        errs(function() return noinline(mins) // noinline(-1i) end, "integer overflow")
        assert(noinline(mins) % noinline(-1i) == 0i)
        errs(function() return noinline(1i) / noinline(0i) end, "division by zero")
        errs(function() return noinline(1i) // noinline(0i) end, "division by zero")
        errs(function() return noinline(1i) % noinline(0i) end, "division by zero")

        assert(noinline(maxs) + 1i == mins)
        assert(noinline(mins) - 1i == maxs)
        assert(noinline(maxs) * 2i == -2i)
        assert(-noinline(mins) == mins)
        assert(noinline(mins) * noinline(-1i) == mins)

        assert(noinline(0i) ^ noinline(0i) == 1i)
        assert(noinline(2i) ^ noinline(63i) == mins)
        assert(noinline(2i) ^ noinline(64i) == 0i)
        assert(noinline(-1i) ^ noinline(maxs) == -1i)
        assert(noinline(-2i) ^ noinline(63i) == mins)
        errs(function() return noinline(2i) ^ noinline(-1i) end, "negative exponent")
        errs(function() return noinline(1i) ^ noinline(-1i) end, "negative exponent")

        -- Mixing integer and number errors, including the constant-operand opcodes.
        local i = noinline(3i)
        errs(function() return i + 1 end)
        errs(function() return 1 - i end)
        errs(function() return 2 / i end)
        errs(function() return i * 2 end)
        errs(function() return i ^ 2 end)
        errs(function() return i < 1 end)
        assert((i == 3) == false)

        assert(noinline(mins) < noinline(0i))
        assert(noinline(2i) > noinline(1i))
        assert(noinline(2i) >= noinline(2i))
        assert(not (noinline(2i) < noinline(1i)))

        local t = {noinline(3i), noinline(-1i), noinline(mins), noinline(maxs), noinline(0i)}
        table.sort(t)
        assert(t[1] == mins and t[2] == -1i and t[3] == 0i and t[4] == 3i and t[5] == maxs)

        local x = noinline(9i)
        x //= 2i
        assert(x == 4i)
        x ^= 3i
        assert(x == 64i)
        x %= 5i
        assert(x == 4i)

        local intval = noinline(123i)
        assert(-intval == -123i)
    )");
}

TEST_SUITE_END();
