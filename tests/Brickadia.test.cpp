// Tests for the Brickadia VM changes described in BRICKADIA_CHANGES.md.
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "lstate.h"

#include "Luau/Common.h"

#include "doctest.h"
#include "ScopedFlags.h"

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include <memory>
#include <stdlib.h>
#include <string.h>
#include <string>

LUAU_FASTFLAG(LuauIntegerLibrary)
LUAU_FASTFLAG(LuauIntegerType2)
LUAU_FASTFLAG(LuauIntegerFastcalls)

// internal functions, declared in lgc.h - not exposed via lua.h
void luaC_validate(lua_State* L);

using StateRef = std::unique_ptr<lua_State, void (*)(lua_State*)>;

constexpr int kOptimizationLevels = 3;

static StateRef newState()
{
    StateRef state(luaL_newstate(), lua_close);
    luaL_openlibs(state.get());
    return state;
}

static std::string topMessage(lua_State* L)
{
    const char* message = lua_tostring(L, -1);
    return message ? message : "<non-string error>";
}

static void loadChunk(lua_State* L, const char* source, int level)
{
    lua_CompileOptions options = {};
    options.optimizationLevel = level;
    options.debugLevel = 1;

    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(source, strlen(source), &options, &bytecodeSize);
    const int loadStatus = luau_load(L, "=brickadia", bytecode, bytecodeSize, 0);
    free(bytecode);
    REQUIRE_MESSAGE(loadStatus == 0, topMessage(L));
}

// Runs the chunk at every optimization level so inlining and folding can't hide a VM path.
static void runScript(lua_State* L, const char* source)
{
    for (int level = 0; level < kOptimizationLevels; ++level)
    {
        loadChunk(L, source, level);
        const int status = lua_pcall(L, 0, 0, 0);
        REQUIRE_MESSAGE(status == 0, "O" << level << ": " << topMessage(L));
    }
}

// The production path: a sandboxed environment resumed as a thread, where pcall takes its fast path.
static void runSandboxed(lua_State* GL, const char* source)
{
    for (int level = 0; level < kOptimizationLevels; ++level)
    {
        lua_State* L = lua_newthread(GL);
        luaL_sandboxthread(L);
        loadChunk(L, source, level);
        const int status = lua_resume(L, nullptr, 0);
        REQUIRE_MESSAGE(status == LUA_OK, "O" << level << ": " << topMessage(L));
        lua_pop(GL, 1);
    }
}

static int sDeadHandle = 0;
static int sLiveHandle = 0;
static int sPrethrowHits = 0;

static int taggedIndex(lua_State* L)
{
    lua_pushfstring(L, "%s!", luaL_checkstring(L, 2));
    return 1;
}

static int taggedNewIndex(lua_State* L)
{
    lua_getglobal(L, "Written");
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_settable(L, -3);
    return 0;
}

static int taggedNamecall(lua_State* L)
{
    lua_pushstring(L, "tagged namecall");
    return 1;
}

static int wideIndex(lua_State* L)
{
    lua_pushfstring(L, "wide:%s", luaL_checkstring(L, 2));
    return 1;
}

static int wideNamecall(lua_State* L)
{
    lua_pushstring(L, "wide namecall");
    return 1;
}

static int rootedIndex(lua_State* L)
{
    lua_pushfstring(L, "rooted:%s", luaL_checkstring(L, 2));
    return 1;
}

static int vectorIndexError(lua_State* L)
{
    luaL_error(L, "vector type metatable must not shadow component access");
    return 0;
}

static int livenessCheck(void* p)
{
    return p != &sDeadHandle;
}

static int userdataEqReturnsDead(lua_State* L)
{
    lua_getglobal(L, "Dead");
    return 1;
}

static void prethrowHit(void*)
{
    ++sPrethrowHits;
}

static int prethrowHits(lua_State* L)
{
    lua_pushinteger(L, sPrethrowHits);
    return 1;
}

static int throwWithPrethrow(lua_State* L)
{
    lua_setprethrow(L, prethrowHit, nullptr);
    luaL_error(L, "boom");
    return 0;
}

static int clearThenThrow(lua_State* L)
{
    lua_setprethrow(L, prethrowHit, nullptr);
    lua_clearprethrow(L);
    luaL_error(L, "cleared");
    return 0;
}

// The caught inner error must not fire the handler, and the handler must be back afterwards.
static int pcallThenThrow(lua_State* L)
{
    lua_setprethrow(L, prethrowHit, nullptr);
    const int hitsBefore = sPrethrowHits;

    lua_getglobal(L, "failing");
    const int status = lua_pcall(L, 0, 0, 0);
    lua_pop(L, 1);

    if (status == 0 || sPrethrowHits != hitsBefore)
    {
        lua_clearprethrow(L);
        luaL_error(L, "inner error was not contained");
    }

    luaL_error(L, "outer");
    return 0;
}

// Without the one-shot clear the nested throw would run this handler again, forever.
static int handlerThrows(lua_State* L)
{
    lua_setprethrow(
        L,
        [](void* data)
        {
            ++sPrethrowHits;
            luaL_error(static_cast<lua_State*>(data), "from handler");
        },
        L
    );
    luaL_error(L, "original");
    return 0;
}

enum DirectAtom : int16_t
{
    kAtomX = 1,
    kAtomBoom = 2,
    kAtomCount = 3,
};

static int sDirectHits = 0;
static int sMetaHits = 0;
static int sDirectSlotSeen = 0;
static double sDirectWritten = 0;

static int16_t directAtom(lua_State*, const char* s, size_t l)
{
    const std::string name(s, l);
    return name == "x" ? kAtomX : name == "boom" ? kAtomBoom : name == "count" ? kAtomCount : -1;
}

// Direct callbacks see the receiver at 1 and the key or arguments after it, as a metamethod would.
static void directIndex(lua_State* L, void* data, int atom, uint16_t* cachedslot, int tag)
{
    ++sDirectHits;
    sDirectSlotSeen = *cachedslot > sDirectSlotSeen ? *cachedslot : sDirectSlotSeen;
    *cachedslot = 7;

    if (atom == kAtomBoom)
        luaL_error(L, "direct boom");

    const bool sameReceiver = lua_tolightuserdatatagged(L, 1, tag) == data && strcmp(lua_tostring(L, 2), "x") == 0;
    lua_pushnumber(L, sameReceiver && data == &sLiveHandle ? atom * 100 + tag : -1);
}

static void directNewIndex(lua_State* L, void* data, int atom, uint16_t*, int tag)
{
    ++sDirectHits;
    sDirectWritten = lua_gettop(L) == 3 && atom == kAtomX && lua_tolightuserdatatagged(L, 1, tag) == data ? lua_tonumber(L, 3) : -1;
}

static int directNamecall(lua_State* L, void* data, int atom, uint16_t*, int tag)
{
    ++sDirectHits;
    lua_pushinteger(L, atom == kAtomCount && lua_tolightuserdatatagged(L, 1, tag) == data ? lua_gettop(L) - 1 : -1);
    return 1;
}

static int metaIndex(lua_State* L)
{
    ++sMetaHits;
    lua_pushfstring(L, "meta:%s", luaL_checkstring(L, 2));
    return 1;
}

static int metaNamecall(lua_State* L)
{
    ++sMetaHits;
    lua_pushstring(L, "meta call");
    return 1;
}

static const luaL_Reg kPrethrowFunctions[] = {
    {"PrethrowHits", prethrowHits},
    {"ThrowWithPrethrow", throwWithPrethrow},
    {"ClearThenThrow", clearThenThrow},
    {"PcallThenThrow", pcallThenThrow},
    {"HandlerThrows", handlerThrows},
    {nullptr, nullptr},
};

static const char* const kPrethrowScript = R"(
    local before = PrethrowHits()

    assert(not pcall(ThrowWithPrethrow))
    assert(PrethrowHits() == before + 1)

    assert(not pcall(ClearThenThrow))
    assert(PrethrowHits() == before + 1)

    local ok, err = pcall(PcallThenThrow)
    assert(not ok and string.find(err, "outer", 1, true), err)
    assert(PrethrowHits() == before + 2)

    local ok2, err2 = pcall(HandlerThrows)
    assert(not ok2 and string.find(err2, "from handler", 1, true), err2)
    assert(PrethrowHits() == before + 3)

    local co = coroutine.create(ThrowWithPrethrow)
    assert(not coroutine.resume(co))
    assert(PrethrowHits() == before + 4)
)";

TEST_SUITE_BEGIN("Brickadia");

TEST_CASE("LightuserdataTagMetatables")
{
    StateRef state = newState();
    lua_State* L = state.get();

    lua_newtable(L);
    lua_setglobal(L, "Written");

    // Tag 5 has its own metatable.
    lua_newtable(L);
    lua_pushcfunction(L, taggedIndex, "taggedIndex");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, taggedNewIndex, "taggedNewIndex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, taggedNamecall, "taggedNamecall");
    lua_setfield(L, -2, "__namecall");
    lua_pushstring(L, "Tagged");
    lua_setfield(L, -2, "__type");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "TaggedMt");
    lua_setlightuserdatametatable(L, 5);

    // Tag 8's metatable is only reachable through the per-tag table, so it must be a GC root.
    lua_newtable(L);
    lua_pushcfunction(L, rootedIndex, "rootedIndex");
    lua_setfield(L, -2, "__index");
    lua_setlightuserdatametatable(L, 8);

    // Every other light userdata falls back to the type-wide metatable.
    lua_pushlightuserdatatagged(L, &sLiveHandle, 6);
    lua_newtable(L);
    lua_pushcfunction(L, wideIndex, "wideIndex");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, wideNamecall, "wideNamecall");
    lua_setfield(L, -2, "__namecall");
    lua_pushstring(L, "Wide");
    lua_setfield(L, -2, "__type");
    lua_pushvalue(L, -1);
    lua_setglobal(L, "WideMt");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);

    // A C __index on the vector type metatable must not shadow component access.
    lua_pushvector(L, 0.0f, 0.0f, 0.0f);
    lua_newtable(L);
    lua_pushcfunction(L, vectorIndexError, "vectorIndexError");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);

    lua_pushlightuserdatatagged(L, &sLiveHandle, 5);
    lua_setglobal(L, "A");
    lua_pushlightuserdatatagged(L, &sLiveHandle, 6);
    lua_setglobal(L, "B");
    lua_pushlightuserdatatagged(L, &sLiveHandle, 8);
    lua_setglobal(L, "C");

    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    luaC_validate(L);

    runScript(L, R"(
        assert(A.foo == "foo!")
        assert(A:method() == "tagged namecall")
        assert(typeof(A) == "Tagged")

        A.written = 7
        assert(Written.written == 7)

        TaggedMt.__iter = function(self) return next, {p = 1, q = 2} end
        local count = 0
        for k, v in A do count += v end
        assert(count == 3)

        local gm = (_G :: any).getmetatable
        assert(getmetatable(A) == TaggedMt)
        assert(gm(A) == TaggedMt)

        assert(B.foo == "wide:foo")
        assert(B:method() == "wide namecall")
        assert(typeof(B) == "Wide")
        assert(getmetatable(B) == WideMt)
        assert(gm(B) == WideMt)

        assert(C.foo == "rooted:foo")

        local v = vector.create(1, 2, 3)
        assert(v.x == 1 and v.Y == 2 and v.z == 3)
    )");

    lua_getlightuserdatametatable(L, 5);
    lua_getglobal(L, "TaggedMt");
    CHECK(lua_rawequal(L, -1, -2));
    lua_pop(L, 2);

    lua_getlightuserdatametatable(L, 9);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_getglobal(L, "A");
    REQUIRE(lua_getmetatable(L, -1));
    lua_getglobal(L, "TaggedMt");
    CHECK(lua_rawequal(L, -1, -2));
}

TEST_CASE("LightuserdataDirectAccess")
{
    StateRef state = newState();
    lua_State* L = state.get();
    lua_callbacks(L)->useratom = directAtom;

    // Tag 11 is registered for direct access; tag 12 has the same metatable shape without it.
    for (int tag : {11, 12})
    {
        lua_newtable(L);
        lua_pushcfunction(L, metaIndex, "metaIndex");
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, metaIndex, "metaNewIndex");
        lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, metaNamecall, "metaNamecall");
        lua_setfield(L, -2, "__namecall");
        lua_setlightuserdatametatable(L, tag);
    }

    CHECK(lua_registerlightuserdatadirectaccess(L, 11, directIndex, directNewIndex, directNamecall) == 1);
    CHECK(lua_registerlightuserdatadirectaccess(L, 13, directIndex, directNewIndex, directNamecall) == 0);

    lua_pushlightuserdatatagged(L, &sLiveHandle, 11);
    lua_setglobal(L, "Direct");
    lua_pushlightuserdatatagged(L, &sLiveHandle, 12);
    lua_setglobal(L, "Meta");

    // Tag 14's __index is cleared after registering, so only the direct access table keeps it alive.
    lua_newtable(L);
    lua_pushcfunction(L, metaIndex, "rootedDirect");
    lua_setfield(L, -2, "__index");
    lua_pushvalue(L, -1);
    lua_setlightuserdatametatable(L, 14);
    CHECK(lua_registerlightuserdatadirectaccess(L, 14, directIndex, nullptr, nullptr) == 1);
    lua_pushnil(L);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    lua_pushlightuserdatatagged(L, &sLiveHandle, 14);
    lua_setglobal(L, "Rooted");

    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    luaC_validate(L);

    sDirectHits = 0;
    sMetaHits = 0;
    sDirectSlotSeen = 0;

    // Locals throughout: from O1 a global path like Direct.x compiles to GETIMPORT, which never uses direct access.
    runScript(L, R"(
        local direct, meta, rooted = Direct, Meta, Rooted
        local function get(o) return o.x end
        for _ = 1, 3 do assert(get(direct) == 111) end
        assert(rooted.x == 114)

        direct.x = 42
        assert(direct:count(1, 2, 3) == 3)

        assert(meta.x == "meta:x")
        assert(meta:count() == "meta call")

        local ok, err = pcall(function() return direct.boom end)
        assert(not ok and string.find(err, "direct boom", 1, true), err)
        assert(get(direct) == 111)

        -- A site that sees anything else falls back to the regular path for good, and keeps working.
        -- A global so O2 can't inline it into two separate sites.
        Poly = function(o) return o.x end
        assert(Poly({ x = 5 }) == 5)
        assert(Poly(direct) == "meta:x")
    )");

    // Per run: three gets, the rooted get, one set, one call, the boom and the get after it.
    CHECK(sDirectHits == 8 * kOptimizationLevels);
    CHECK(sMetaHits == 3 * kOptimizationLevels);
    CHECK(sDirectSlotSeen == 7);
    CHECK(sDirectWritten == 42);
}

TEST_CASE("PrethrowCleanup")
{
    {
        StateRef state = newState();
        lua_State* L = state.get();

        lua_pushvalue(L, LUA_GLOBALSINDEX);
        luaL_register(L, nullptr, kPrethrowFunctions);
        lua_pop(L, 1);

        runScript(L, R"(failing = function() error("inner") end)");
        runScript(L, kPrethrowScript);

        // A reset thread drops its handler.
        lua_State* co = lua_newthread(L);
        lua_setprethrow(co, prethrowHit, nullptr);
        lua_resetthread(co);
        CHECK(co->prethrow == nullptr);
        CHECK(co->prethrowdata == nullptr);
    }

    {
        StateRef state = newState();
        lua_State* L = state.get();

        lua_pushvalue(L, LUA_GLOBALSINDEX);
        luaL_register(L, nullptr, kPrethrowFunctions);
        lua_pop(L, 1);

        runScript(L, R"(failing = function() error("inner") end)");
        luaL_sandbox(L);
        runSandboxed(L, kPrethrowScript);
    }
}

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

TEST_CASE("LightuserdataLiveness")
{
    StateRef state = newState();
    lua_State* L = state.get();

    lua_setlightuserdatalivenesscheck(L, 7, livenessCheck);

    lua_pushlightuserdatatagged(L, &sDeadHandle, 7);
    lua_setglobal(L, "Dead");
    lua_pushlightuserdatatagged(L, &sLiveHandle, 7);
    lua_setglobal(L, "Live");
    lua_pushlightuserdatatagged(L, &sDeadHandle, 0);
    lua_setglobal(L, "Untagged");

    // Two userdata sharing a C __eq, which the interpreter calls on its fast path.
    lua_newtable(L);
    lua_pushcfunction(L, userdataEqReturnsDead, "userdataEqReturnsDead");
    lua_setfield(L, -2, "__eq");
    for (const char* name : {"U1", "U2"})
    {
        lua_newuserdata(L, 8);
        lua_pushvalue(L, -2);
        lua_setmetatable(L, -2);
        lua_setglobal(L, name);
    }
    lua_pop(L, 1);

    runScript(L, R"(
        assert(Live)
        assert(Untagged)
        assert(not Dead)
        if Dead then error("dead handle is truthy") end

        local n = 0
        repeat n += 1 until Dead or n == 3
        assert(n == 3)

        while Dead do error("while on dead") end
        for i = 1, 3 do if Dead then error("break on dead") end end

        local five = 5
        assert((Dead or five) == 5)
        assert(rawequal(Dead and five, Dead))
        local d = Dead
        assert((if d then d else 7) == 7)
        assert(rawequal(if d then 7 else d, d))

        assert(not pcall(function() assert(Dead) end))
        assert(not pcall(function() local v = assert(Dead) end))
        local ok, err = pcall(assert, Dead, "message")
        assert(not ok and string.find(tostring(err), "message", 1, true))

        local function inv(a) return not a end
        assert(inv(Dead))

        -- Equality, nil tests and iteration are unaffected.
        assert(Dead ~= nil)
        assert((Dead == false) == false)
        local seen = 0
        for _, v in {Dead, Dead} do seen += 1 end
        assert(seen == 2)
        seen = 0
        for _, v in ipairs({Dead, Dead}) do seen += 1 end
        assert(seen == 2)

        local mt = {__eq = function() return Dead end, __lt = function() return Dead end, __le = function() return Dead end}
        local a, b = setmetatable({}, mt), setmetatable({}, mt)
        assert(not (a == b))
        assert(not (a < b))
        assert(not (a <= b))

        assert(not (U1 == U2))
        local equal = U1 == U2
        assert(equal == false)

        local t = {3, 1, 2}
        table.sort(t, function(x, y) if x < y then return Live end return Dead end)
        assert(t[1] == 1 and t[2] == 2 and t[3] == 3)

        assert(string.gsub("abc", "b", function() return Dead end) == "abc")
    )");

    lua_getglobal(L, "Dead");
    CHECK(lua_toboolean(L, -1) == 0);
    lua_getglobal(L, "Live");
    CHECK(lua_toboolean(L, -1) == 1);
}

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
