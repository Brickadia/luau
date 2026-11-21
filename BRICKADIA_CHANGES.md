# Brickadia Luau Changes

This document describes Brickadia's modifications to Luau. The first commit on top of upstream
restores the integer type that upstream removed in 0.747 (luau-lang/rfcs#269), adapted to each
later release and nothing else. Every section below is one further commit, which carries the
change together with its tests and its section here.

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
