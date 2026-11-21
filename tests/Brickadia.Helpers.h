// Shared helpers for the Brickadia test files; see BRICKADIA_CHANGES.md.
#pragma once

#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "doctest.h"

#include <memory>
#include <stdlib.h>
#include <string.h>
#include <string>

using StateRef = std::unique_ptr<lua_State, void (*)(lua_State*)>;

constexpr int kOptimizationLevels = 3;

inline StateRef newState()
{
    StateRef state(luaL_newstate(), lua_close);
    luaL_openlibs(state.get());
    return state;
}

inline std::string topMessage(lua_State* L)
{
    const char* message = lua_tostring(L, -1);
    return message ? message : "<non-string error>";
}

inline void loadChunk(lua_State* L, const char* source, int level)
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
inline void runScript(lua_State* L, const char* source)
{
    for (int level = 0; level < kOptimizationLevels; ++level)
    {
        loadChunk(L, source, level);
        const int status = lua_pcall(L, 0, 0, 0);
        REQUIRE_MESSAGE(status == 0, "O" << level << ": " << topMessage(L));
    }
}
