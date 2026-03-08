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
