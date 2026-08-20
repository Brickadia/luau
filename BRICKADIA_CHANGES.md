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

Metamethod dispatch, generic `for` iteration, `getmetatable` (including its
fastcall), `typeof`'s `__type`, `lua_getmetatable`, and JIT fallback paths all
resolve the per-tag metatable for light userdata. Tags without a registered
metatable fall back to the type-wide light userdata metatable set through
`lua_setmetatable`, as upstream does for all light userdata. The heap validator
and heap enumeration treat the per-tag metatables as roots.

The interpreter's C `__index`/`__newindex` fast paths consult only userdata and
light userdata metatables, so a type metatable on vectors or strings never
shadows their built-in field access.

The VM's internal for-loop iterator values are light userdata tagged
`LU_TAG_ITERATOR`, which upstream defines as `LUA_UTAG_LIMIT`. That only lies
above the registerable light tags while both limits are equal (128 by default),
so it is `LUA_LUTAG_LIMIT` here.

Note that `lua_pushlightuserdata(L, p)` assigns tag 0, so setting a metatable
for tag 0 affects all untagged light userdata.

## 2. Pre-throw cleanup callback

Luau errors `longjmp` (`LUA_USE_LONGJMP=1`) so modules hosting bindings don't
need C++ exceptions. Generated bindings stage C++ temporaries (TArray, TMap,
...) that must be destroyed when an error jumps out of them, so a per-thread
callback runs just before the jump:

```c
void lua_setprethrow(lua_State* L, void (*fn)(void*), void* data);
void lua_clearprethrow(lua_State* L);
```

`luaD_throw` clears the handler and then calls `fn(data)`, so it runs at most
once. `luaD_rawrunprotected` hides it for the duration of a protected region and
restores it afterwards, since an error caught inside the region never unwinds
the frame that installed it. `lua_resetthread` clears it, and new threads start
without one.

Contract for callers:

- Clear it before returning, and never keep it installed across calls that can
  run Luau code, because a nested binding would replace it. Besides the call
  functions, that includes `lua_gettable`/`lua_settable`, `lua_getfield`/
  `lua_setfield`, `luaL_tolstring`, `lua_equal`, `lua_lessthan` and
  `lua_concat` on values with metatables.
- `fn` runs at the throw site, possibly during an out-of-memory error in the
  middle of a table resize or stack reallocation, so it must not call into the
  VM (for example `lua_unref`).
- The object `fn` destroys must have no automatic destructor in any frame the
  jump crosses: `longjmp` runs those itself in code compiled with `/EH`
  (Unreal's editor builds) and skips them otherwise. Construct it in raw
  storage, and destroy it explicitly on the success path after clearing the
  handler.


## 3. Native operators for 64-bit integers

Upstream's experimental `integer` type only implements equality and exposes
arithmetic through `integer.*` functions. Brickadia adds native wrapping
`+`, `-`, `*`, `^`, signed `/`, floored `//` and `%`, unary negation, and signed
ordering operators. Mixed integer/number operations remain type errors.
`^` uses wrapping exponentiation by squaring and errors on negative exponents.

`/` truncates toward zero; `//` and `%` floor, so `(a // b) * b + a % b == a`.
Division and modulo by zero raise "division by zero". `INT64_MIN / -1` and
`INT64_MIN // -1` raise "integer overflow" (matching `integer.div` and
`integer.idiv`), while `INT64_MIN % -1` is 0.

The new type solver types them too, only with `LuauIntegerType2`: arithmetic on
two integers gives `integer`, integer comparisons give `boolean`, and unary
minus accepts `number | integer` (TypeChecker2 and the builtin type functions).
The old solver still rejects every integer operator.

## 4. Liveness checks for tagged light userdata

Brickadia exposes weak handles (bricks, objects) as tagged light userdata. A
per-tag liveness callback makes dead handles falsy everywhere Luau tests
truthiness: `if`, `while`, `repeat`, `and`/`or`, `not`, `assert`,
`lua_toboolean`, and the results of `__eq`/`__lt`/`__le`, `table.sort`
comparators and `gsub` replacement functions. So `if handle then` tests
liveness directly. Everything else is unchanged: `x == nil` stays false,
`type()`, equality, and table keys behave as before, and generic `for`
termination remains nil-based.

```c
typedef int (*lua_LightUserdataLiveness)(void* p);
void lua_setlightuserdatalivenesscheck(lua_State* L, int tag, lua_LightUserdataLiveness check);
```

The callback runs inside the interpreter loop: it must not call into the VM,
error, yield, or allocate, and should be O(1). Reassignment is not supported.
Values of other types pay at most one extra predicted branch; every light
userdata truthiness test makes one out-of-line call, even for tags without a
check.

The type checker doesn't know about liveness: in `if h then ... else ... end`
it narrows `h` to `never` in the else branch.

Native codegen must not be enabled while liveness checks are registered; its
inlined truthiness operations still treat all light userdata as truthy.
