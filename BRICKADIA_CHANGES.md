# Brickadia Luau VM Changes

This document describes modifications to the Luau VM for Brickadia.


## 1. Per-tag metatables for light userdata

Light userdata tags had no per-tag metatable support. Two new API functions
allow registering a metatable for a light userdata tag:

```c
void lua_setlightuserdatametatable(lua_State* L, int tag);
void lua_getlightuserdatametatable(lua_State* L, int tag);
```

These mirror `lua_setuserdatametatable` / `lua_getuserdatametatable`.
Reassignment is not supported. `tag` must be in range `[0, LUA_LUTAG_LIMIT)`.

Metamethod dispatch, `lua_getmetatable`, and JIT fallback paths all resolve
the per-tag metatable for light userdata. Tags without a registered metatable
behave as before (no metamethods).

Note that `lua_pushlightuserdata(L, p)` assigns tag 0, so setting a metatable
for tag 0 affects all untagged light userdata.


## 2. Widen userdata tag to uint16

`Udata::tag` was `uint8_t`, limiting userdata to 256 distinct tags. Brickadia's
binding generator assigns a tag per exported struct type, requiring several
thousand. The field is now `uint16_t`.

`Udata::len` was also narrowed from `int` to `uint16_t` to keep the struct the
same size (`offsetof(Udata, data)` remains 16). Maximum userdata payload is now
`UINT16_MAX - sizeof(Udata)` bytes (approximately 65500).

`LUA_UTAG_LIMIT` (default 128) is independent of the field width and can be
raised via compiler define to use the full range.


## 3. Pre-throw cleanup callback

Unreal Engine builds with exceptions disabled, so Luau uses `longjmp` for error
handling, which skips C++ destructors. Generated binding code creates
stack-allocated containers (TArray, TMap, etc.) that must be destroyed before
the jump. Two new per-thread fields on `lua_State` and
corresponding API functions allow registering a cleanup callback:

```c
void lua_setprethrow(lua_State* L, void (*fn)(void*), void* data);
void lua_clearprethrow(lua_State* L);
```

When `luaD_throw` fires, it clears the handler and then calls `fn(data)` before
the `longjmp`/`throw`, giving the caller a chance to destroy C++ objects on the
stack without leaving a dangling callback after a protected error. The fields
live directly on `lua_State` (not `global_State`) to avoid function-call overhead
through `lua_callbacks()`, since they are set on every generated function that
has non-trivial temporaries.


## 4. Native operators for 64-bit integers

Upstream's experimental `integer` type only implements equality and exposes
arithmetic through `integer.*` functions. Brickadia adds native wrapping
`+`, `-`, `*`, signed `/`, floored `//` and `%`, unary negation, and signed
ordering operators. Mixed integer/number operations remain type errors.


## 5. Liveness checks for tagged light userdata

Brickadia exposes weak handles (bricks, objects) as tagged light userdata. A
per-tag liveness callback makes dead handles falsy in boolean contexts (`if`,
`and`/`or`, `not`, `assert`, `lua_toboolean`), so `if handle then` tests
liveness directly. Everything else is unchanged: `x == nil` stays false,
`type()`, equality, and table keys behave as before, and generic `for`
termination remains nil-based.

```c
typedef int (*lua_LightUserdataLiveness)(void* p);
void lua_setlightuserdatalivenesscheck(lua_State* L, int tag, lua_LightUserdataLiveness check);
```

The callback runs inside the interpreter loop: it must not call into the VM,
error, yield, or allocate, and should be O(1). Reassignment is not supported.
Values of other types pay at most one extra predicted branch.

Native codegen must not be enabled while liveness checks are registered; its
inlined truthiness operations still treat all light userdata as truthy.
