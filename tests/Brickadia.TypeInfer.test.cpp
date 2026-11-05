// Type checking of the Brickadia integer operators described in BRICKADIA_CHANGES.md.
#include "Fixture.h"
#include "ScopedFlags.h"

#include "doctest.h"

using namespace Luau;

LUAU_FASTFLAG(LuauIntegerType2)

TEST_SUITE_BEGIN("BrickadiaTypeInfer");

TEST_CASE_FIXTURE(BuiltinsFixture, "integer_operators_produce_integers")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    ScopedFastFlag sff{FFlag::LuauIntegerType2, true};

    CheckResult result = check(R"(
        --!strict
        local a: integer = 7i
        local b: integer = a + 1i
        local c: integer = a - b
        local d: integer = a * 2i
        local e: integer = a // 2i
        local f: integer = a % 2i
        local g: integer = a ^ 2i
        local h: integer = a / 2i
        local i: integer = -a
        local j: boolean = a < b
        local k: boolean = a >= b
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "mixing_integer_and_number_is_an_error")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    ScopedFastFlag sff{FFlag::LuauIntegerType2, true};

    CheckResult result = check(R"(
        --!strict
        local a = 7i
        local b = a + 1.5
        local c = 1.5 * a
        local d = a < 2
    )");

    LUAU_REQUIRE_ERROR_COUNT(3, result);
    CHECK_EQ(3, result.errors[0].location.begin.line);
    CHECK_EQ(4, result.errors[1].location.begin.line);
    CHECK_EQ(5, result.errors[2].location.begin.line);
}

// The integer branch must accept subtypes of integer, not only exactly-integer types.
TEST_CASE_FIXTURE(BuiltinsFixture, "integer_intersections_use_the_integer_path")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    ScopedFastFlag sff{FFlag::LuauIntegerType2, true};

    CheckResult result = check(R"(
        --!strict
        local function step<T>(x: T & integer): integer
            return x + 1i
        end
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "unary_minus_without_integer_support")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    ScopedFastFlag sff{FFlag::LuauIntegerType2, false};

    CheckResult result = check(R"(
        --!strict
        local x = -(5 :: number)
        local y = -("s" :: any)
        local z = -"s"
    )");

    LUAU_REQUIRE_ERRORS(result);
    for (const TypeError& error : result.errors)
        CHECK_EQ(4, error.location.begin.line);
    CHECK_EQ("number", toString(requireType("x")));
}

TEST_SUITE_END();
