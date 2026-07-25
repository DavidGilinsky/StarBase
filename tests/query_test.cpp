// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          tests/query_test.cpp
// Purpose:       Tests for the filter-AST to SQL compiler: comparisons, ranges,
//                set membership, logical groups, header predicates, cone search,
//                sort, injection safety, and (with a database) real results.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
// The compiler checks run always (they need a Database only for escaping, which
// works offline against a not-yet-connected handle is NOT possible -- so those
// checks are DB-gated too). Skips (exit 0) unless SB_TEST_DB_NAME is set.
// ---------------------------------------------------------------------------
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "database.hpp"
#include "query.hpp"

namespace {

using json = nlohmann::json;
namespace db = starbase::db;
namespace q = starbase::query;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++g_failures; }
}
std::string find_sql(const std::string& n) {
    for (const std::string b : {"sql/", "../sql/", "../../sql/"})
        if (std::filesystem::exists(b + n)) return b + n;
    return {};
}
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    const char* test_db = std::getenv("SB_TEST_DB_NAME");
    if (!test_db || !*test_db) { std::cout << "query_test: SB_TEST_DB_NAME not set; skipping\n"; return 0; }
    const std::string schema = find_sql("schema.sql");
    if (schema.empty()) { std::cerr << "query_test: no schema.sql\n"; return 1; }

    try {
        db::DbConfig cfg = db::DbConfig::from_env();
        cfg.database = test_db;
        db::Database d(cfg);
        d.apply_script(schema);

        auto C = [&](const json& j) { return q::compile_filter(j, d); };

        // ---- comparisons and types ----
        check(C(json::parse(R"({"field":"image_type","op":"eq","value":"light"})"))
                  == "image_type = 'light'", "string eq");
        check(C(json::parse(R"({"field":"exposure_s","op":"gte","value":300})"))
                  == "exposure_s >= 300", "numeric gte, integer literal");
        check(C(json::parse(R"({"field":"gain","op":"eq","value":"100"})"))
                  == "gain = 100", "numeric field accepts numeric string");
        check(C(json::parse(R"({"field":"filter","op":"in","value":["Ha","OIII"]})"))
                  == "filter IN ('Ha', 'OIII')", "set membership");
        check(C(json::parse(R"({"field":"exposure_s","op":"between","value":[300,600]})"))
                  == "exposure_s BETWEEN 300 AND 600", "range");
        check(C(json::parse(R"({"field":"object","op":"isnull"})")) == "object IS NULL", "isnull");
        check(C(json::parse(R"({"field":"object","op":"like","value":"NGC%"})"))
                  == "object LIKE 'NGC%'", "like");

        // ---- logical groups ----
        const std::string andq = C(json::parse(R"({"op":"and","clauses":[
            {"field":"image_type","op":"eq","value":"light"},
            {"field":"filter","op":"in","value":["Ha"]}]})"));
        check(andq == "(image_type = 'light' AND filter IN ('Ha'))", "and group");
        check(contains(C(json::parse(R"({"op":"not","clauses":[
            {"field":"image_type","op":"eq","value":"dark"}]})")), "NOT ("), "not group");

        // ---- header predicate -> EXISTS against frame_keywords ----
        const std::string kw = C(json::parse(R"({"field":"keyword:FOCTEMP","op":"lt","value":-5})"));
        check(contains(kw, "EXISTS (SELECT 1 FROM frame_keywords k"), "keyword -> EXISTS");
        check(contains(kw, "k.keyword = 'FOCTEMP'"), "keyword name in predicate");
        check(contains(kw, "CAST(k.value AS DECIMAL"), "numeric keyword compares as decimal");
        check(contains(C(json::parse(R"({"field":"keyword:SWCREATE","op":"like","value":"NINA%"})")),
                       "k.value LIKE 'NINA%'"), "string keyword compares as text");

        // ---- tag / collection membership -> correlated EXISTS ----
        const std::string tagged = C(json::parse(R"({"op":"tagged","value":"review"})"));
        check(contains(tagged, "EXISTS (SELECT 1 FROM frame_tags ft"), "tagged -> EXISTS on frame_tags");
        check(contains(tagged, "t.name = 'review'"), "tag matched by name");
        check(contains(tagged, "ft.frame_id = v_frames.frame_id"), "tag correlated on the outer frame");
        check(contains(C(json::parse(R"({"op":"tagged","value":7})")), "ft.tag_id = 7"), "tag matched by id");
        check(C(json::parse(R"({"op":"untagged","value":"review"})")).rfind("NOT ", 0) == 0, "untagged negates");
        const std::string incoll = C(json::parse(R"({"op":"in_collection","value":"NGC7000"})"));
        check(contains(incoll, "FROM collection_frames cf"), "in_collection -> EXISTS on collection_frames");
        check(contains(incoll, "c.name = 'NGC7000'"), "collection matched by name");

        // ---- cone search ----
        const std::string cone = C(json::parse(R"({"op":"cone","ra":311.4,"dec":30.7,"radius_deg":1.5})"));
        check(contains(cone, "dec_deg BETWEEN"), "cone has a dec bounding box");
        check(contains(cone, "ra_deg BETWEEN"), "cone has an ra bounding box");
        check(contains(cone, "ASIN(SQRT"), "cone has a haversine test");

        // ---- empty filter matches all ----
        check(C(json()) == "1=1", "null filter matches all");
        check(C(json::object()) == "1=1", "empty object matches all");

        // ---- sort ----
        check(q::compile_sort(json()) == "date_obs_utc DESC", "default sort");
        check(q::compile_sort(json::parse(R"([{"field":"exposure_s","dir":"asc"},
              {"field":"gain","dir":"desc"}])")) == "exposure_s ASC, gain DESC", "multi sort");

        // ---- rejection: unknown field, bad op, injection ----
        auto rejects = [&](const std::string& js, const std::string& why) {
            bool threw = false;
            try { C(json::parse(js)); } catch (const q::QueryError&) { threw = true; }
            check(threw, why);
        };
        rejects(R"({"field":"secret_col","op":"eq","value":"x"})", "unknown field rejected");
        rejects(R"({"field":"exposure_s","op":"bogus","value":1})", "unknown operator rejected");
        rejects(R"({"field":"exposure_s","op":"eq","value":"1; DROP TABLE frames"})",
                "non-numeric injection into a numeric field rejected");
        // A string value with a quote is ESCAPED, not rejected, and stays inert.
        check(contains(C(json::parse(R"({"field":"object","op":"eq","value":"a' OR '1'='1"})")),
                       "object = 'a\\' OR \\'1\\'=\\'1'"), "string injection is escaped");
        rejects(R"([1,2,3])", "non-object node rejected");

        // ---- against real (empty here) tables: the compiled SQL must execute ----
        d.query("SELECT COUNT(*) FROM v_frames WHERE " +
                C(json::parse(R"({"op":"and","clauses":[
                    {"field":"image_type","op":"eq","value":"light"},
                    {"field":"exposure_s","op":"between","value":[60,600]},
                    {"op":"cone","ra":180,"dec":0,"radius_deg":2},
                    {"field":"keyword:GAIN","op":"gte","value":100}]})")));
        check(true, "complex compiled query executes against v_frames");

    } catch (const std::exception& e) {
        std::cerr << "query_test: " << e.what() << "\n";
        return 1;
    }

    if (g_failures == 0) { std::cout << "query_test: all checks passed\n"; return 0; }
    std::cerr << "query_test: " << g_failures << " check(s) failed\n";
    return 1;
}
