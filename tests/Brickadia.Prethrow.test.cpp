// Tests for the pre-throw cleanup callback; see BRICKADIA_CHANGES.md.
#include "Brickadia.Helpers.h"
#include "lstate.h"

static int sPrethrowHits = 0;

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

TEST_SUITE_END();
