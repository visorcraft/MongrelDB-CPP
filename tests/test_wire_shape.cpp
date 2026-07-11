// test_wire_shape.cpp - Offline wire-format conformance tests.
//
// Verifies that the JSON serialization for /kit/create_table columns matches
// the daemon's expected wire shape, without needing a running server.
//
// Licensing: MIT OR Apache-2.0.

#include <mongreldb/mongreldb.hpp>
#include <cassert>
#include <cstdio>
#include <string>

using namespace mongreldb;

int main() {
    // Test 1: Basic column (no optional fields)
    {
        Column col;
        col.id = 1;
        col.name = "id";
        col.ty = "int64";
        col.primary_key = true;
        col.nullable = false;

        // Serialize and verify wire shape
        // The column struct is serialized via the JSON encoder
        std::string json = mongreldb::detail::serialize_column_json(col);
        assert(json.find("\"id\":1") != std::string::npos);
        assert(json.find("\"name\":\"id\"") != std::string::npos);
        assert(json.find("\"ty\":\"int64\"") != std::string::npos);
        assert(json.find("\"primary_key\":true") != std::string::npos);
        assert(json.find("\"nullable\":false") != std::string::npos);
        assert(json.find("enum_variants") == std::string::npos);
        assert(json.find("default_value") == std::string::npos);
        printf("PASS: basic column wire shape\n");
    }

    // Test 2: Column with enum_variants
    {
        Column col;
        col.id = 2;
        col.name = "status";
        col.ty = "varchar";
        col.primary_key = false;
        col.nullable = false;
        col.enum_variants = {"active", "inactive", "pending"};

        std::string json = mongreldb::detail::serialize_column_json(col);
        assert(json.find("\"enum_variants\":[\"active\",\"inactive\",\"pending\"]") != std::string::npos);
        assert(json.find("default_value") == std::string::npos);
        printf("PASS: enum_variants wire shape\n");
    }

    // Test 3: Column with default_value
    {
        Column col;
        col.id = 3;
        col.name = "score";
        col.ty = "float64";
        col.primary_key = false;
        col.nullable = true;
        col.default_value = "0.0";

        std::string json = mongreldb::detail::serialize_column_json(col);
        assert(json.find("\"default_value\":\"0.0\"") != std::string::npos);
        assert(json.find("enum_variants") == std::string::npos);
        printf("PASS: default_value wire shape\n");
    }

    // Test 4: top-level CHECK constraints.
    {
        Column col{1, "score", "int64", false, false, {}, std::nullopt};
        std::string constraints =
            R"({"checks":[{"id":1,"name":"score_nonneg","expr":{"Ge":[{"Col":1},{"Lit":{"Int64":0}}]}}]})";
        std::string json = mongreldb::detail::serialize_create_table_json(
            "scores", {col}, constraints);
        assert(json.find("\"constraints\":{\"checks\":[") != std::string::npos);
        assert(json.find("\"name\":\"score_nonneg\"") != std::string::npos);
        printf("PASS: CHECK constraints wire shape\n");
    }

    printf("All wire-shape tests passed.\n");
    return 0;
}
