// Type checking tests for content scripts; see BRICKADIA_CHANGES.md.
#include "Fixture.h"
#include "ScopedFlags.h"

#include "doctest.h"

using namespace Luau;

TEST_SUITE_BEGIN("BrickadiaTypeInfer");

static const char* const kContentDefinitions = R"(
    export type MeshDef = { asset: string, scale: {number}? }
    export type ThingDef = { mesh: MeshDef?, count: number? }
    declare function make(thing: ThingDef): ()
    declare function makeAll(things: { [string]: ThingDef }): ()
)";

TEST_CASE_FIXTURE(BuiltinsFixture, "definition_tables_reject_unknown_fields")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    loadDefinition(kContentDefinitions);

    CheckResult result = check(R"(
        --!strict
        make({ count = 1, cuont = 2, mesh = { asset = "a", sacle = {} } })
    )");

    LUAU_REQUIRE_ERROR_COUNT(2, result);
    CHECK_EQ("Unknown field 'cuont' in ThingDef; did you mean 'count'?", toString(result.errors[0]));
    CHECK_EQ("Unknown field 'sacle' in MeshDef; did you mean 'scale'?", toString(result.errors[1]));
}

TEST_CASE_FIXTURE(BuiltinsFixture, "ordinary_annotations_keep_width_subtyping")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    loadDefinition(kContentDefinitions);

    CheckResult result = check(R"(
        --!strict
        local function take(t: { x: number }) end
        take({ x = 1, y = 2 })
    )");

    LUAU_REQUIRE_NO_ERRORS(result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "definition_tables_under_an_indexer_reject_unknown_fields")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    loadDefinition(kContentDefinitions);

    CheckResult result = check(R"(
        --!strict
        makeAll({ a = { count = 1 }, b = { cuont = 1 } })
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);
    CHECK_EQ(2, result.errors[0].location.begin.line);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "unknown_field_in_an_assignment_is_reported_once")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    loadDefinition(kContentDefinitions);

    CheckResult result = check(R"(
        --!strict
        local m: MeshDef = { asset = "a" }
        m = { asset = "b", sacle = {} }
    )");

    LUAU_REQUIRE_ERROR_COUNT(1, result);
}

TEST_CASE_FIXTURE(BuiltinsFixture, "requiring_a_module_that_returns_nothing_gives_nil")
{
    DOES_NOT_PASS_OLD_SOLVER_GUARD();
    fileResolver.source["game/A"] = "local x = 1";
    fileResolver.source["game/B"] = "local a = require(game.A)";

    CheckResult result = getFrontend().check("game/B");

    LUAU_REQUIRE_NO_ERRORS(result);
    CHECK_EQ("nil", toString(requireType("game/B", "a")));
}

TEST_SUITE_END();
