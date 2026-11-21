# Brickadia Luau Changes

This document describes Brickadia's modifications to Luau. The first commit on top of upstream
restores the integer type that upstream removed in 0.747 (luau-lang/rfcs#269), adapted to each
later release and nothing else. Every section below is one further commit, which carries the
change together with its tests and its section here.

## Tagged light userdata

Brickadia exposes objects, handles and components as tagged light userdata. Three VM changes
make them behave like typed objects.

### Per-tag metatables

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
so it is `LUA_LUTAG_LIMIT` here; `IrLowering`'s `ForInManualAnnotation` expects
that value in its IR.

Note that `lua_pushlightuserdata(L, p)` assigns tag 0, so setting a metatable
for tag 0 affects all untagged light userdata.

### Liveness checks

A per-tag liveness callback makes dead handles falsy everywhere Luau tests
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

### Direct access

Upstream's `lua_registeruserdatadirectaccess` lets the `GETUDATAKS`, `SETUDATAKS` and
`NAMECALLUDATA` instructions call a userdata tag's callbacks directly, with the key's atom,
instead of resolving `__index`/`__newindex`/`__namecall` through the metatable and calling
them. The same is registered per light tag:

```c
int lua_registerlightuserdatadirectaccess(lua_State* L, int tag, lua_UserdataDirectAccess get,
    lua_UserdataDirectAccess set, lua_UserdataDirectNamecall namecall);
```

The tag's metatable must be set first; as upstream, only metamethods it has are called
directly. Callbacks receive the light userdata's pointer as `data` and its tag as `utag`, and
run in the frame the metamethod would, so the receiver is at stack index 1. The per-instruction
`cachedslot` is shared with full userdata callbacks. Direct field getters remain full userdata
only, since they cannot raise errors for dead handles.

The per-tag tables for all three features live in `global_State` like upstream's per-tag
userdata tables, so the instructions' lookup is a bounds check and an index. With a raised
`LUA_LUTAG_LIMIT` (1024 in the longjmp test configuration) they make the main state allocation
larger than 32 KB, so `HugeFunctionLoadFailure` only fails allocations made by `luau_load`.

## Pre-throw cleanup callback

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

## Native operators for 64-bit integers

Upstream's `integer` type only implements equality and exposes arithmetic through
`integer.*` functions. Brickadia adds native wrapping `+`, `-`, `*`, `^`, signed `/`,
floored `//` and `%`, unary negation, and signed ordering operators. Mixed
integer/number operations remain type errors. `^` uses wrapping exponentiation by
squaring and errors on negative exponents.

`/` truncates toward zero; `//` and `%` floor, so `(a // b) * b + a % b == a`.
Division and modulo by zero raise "division by zero". `INT64_MIN / -1` and
`INT64_MIN // -1` raise "integer overflow" (matching `integer.div` and
`integer.idiv`), while `INT64_MIN % -1` is 0 and `-INT64_MIN` wraps to `INT64_MIN`.

The new type solver types them too, only with `LuauIntegerType2`: arithmetic on
two integers gives `integer`, integer comparisons give `boolean`, and unary
minus accepts `number | integer` (TypeChecker2 and the builtin type functions).
The old solver still rejects every integer operator. Upstream's tests that
expected these operators to be errors (`integers.luau`,
`negating_a_non_literal_integer_gives_an_integer`, `typecheck_unary_minus`,
`unary_op_type_function_errors`) expect the operators instead.

## XXH3 string hash

`luaS_hash` and `BytecodeBuilder::getStringHash` use the low 32 bits of
`XXH3_64bits` instead of the Lua 5.1 shift-add-xor hash (with a lookup3-style
prefix mix for strings of 32 bytes or more). XXH3 is faster at every length and
mixes the low bits used for string table and table slot selection far better.
Both include `xxhash.h` with `XXH_INLINE_ALL` from `extern/xxhash` (v0.8.3,
unmodified); hosts building Luau themselves add that include path.
String table walks compare the stored hash before length and bytes.

Bytecode compiled with the old hash still runs correctly: the compiler's hash
only seeds the predicted slot of `GETTABLEKS`/`SETTABLEKS`/`NAMECALL`/
`GETGLOBAL`/`SETGLOBAL`, which the VM re-patches on the first miss.

Table traversal order follows the hash, so it differs from upstream's. Upstream's
bytecode, IR and constant-table dumps that print hashes or slots are updated to XXH3's
output, and the `pairs` assertions in `basic.luau` and `tables.luau` check what
scripts may rely on (array part first and in order; identical tables traverse
identically) instead of upstream's exact order.

Hosts that cache a string's hash (Brickadia's wire strings cache exactly
Luau's) can pass it in both directions instead of hashing twice:

```c
void lua_pushlstringhashed(lua_State* L, const char* s, size_t l, unsigned int hash);
const char* lua_tolstringhashed(lua_State* L, int idx, size_t* len, unsigned int* hash);
```

`hash` must equal `luaS_hash(s, l)`; assert builds check it, and a wrong hash
silently breaks interning in other builds. `lua_tolstringhashed` behaves as
`lua_tolstring` and also returns the string's stored hash; either out pointer
may be NULL. Upstream's salted interning table (`LuauSaltedInterningTable`,
`lua_setpointerencodekey`) salts only the bucket index, so the stored and exchanged
hash stays the unsalted `luaS_hash`.

## Testing

Each change's own tests live in `tests/Brickadia.<Change>.test.cpp` (Luau.Conformance) or
`tests/Brickadia.<Change>.TypeInfer.test.cpp` (Luau.UnitTest), sharing `tests/Brickadia.Helpers.h`.
A glob at the end of `Sources.cmake` adds them to the test targets, so upstream's lists stay
untouched. Upstream tests whose expectations a change alters are updated in that change's commit.

`tools/brickadia-test.ps1` builds the suites and the CLIs with `LUAU_ENABLE_ASSERT`, in
upstream's configuration and in Brickadia's (`LUA_USE_LONGJMP=1`, raised light userdata tag
limit). It runs the unit and conformance tests with default flags and with `--fflags=true`, and
the golden suite, and fails on any failure or crash. The two upstream tests that rely on the VM
turning C++ exceptions thrown by callbacks into Lua errors are compiled out under
`LUA_USE_LONGJMP=1`, which doesn't do that. Pass `-FlagsSince <previous tag>` when syncing to list the flags added
and removed upstream: Brickadia enables every non-experimental `Luau*` flag, so new ones go
live without review otherwise.
