// Example: native query builder (range + primary-key lookups) in C++.
//
// Build (from the repo root):
//
//   c++ -std=c++17 -Iinclude examples/query_builder.cpp \
//       $(pkg-config --cflags --libs libcurl) -o examples/query_builder
//   ./examples/query_builder
//
// Requires a mongreldb-server daemon running on http://127.0.0.1:8453.
//
// Creates a table, loads five rows with varying scores, then runs two
// native queries: a range scan over score in [60, 90], and an exact
// primary-key lookup for id == 4. Results are printed, then the table is
// dropped.

#include <mongreldb/mongreldb.hpp>

#include <iostream>

static const char *kUrl = "http://127.0.0.1:8453";
static const char *kTable = "example_query";

// Build the shared three-column schema used by every example:
//   col 1 = id (int64, primary key)
//   col 2 = name (varchar)
//   col 3 = score (float64)
static std::vector<mongreldb::Column> schema() {
    return {
        {1, "id", "int64", /*primary_key=*/true, /*nullable=*/false},
        {2, "name", "varchar", /*primary_key=*/false, /*nullable=*/false},
        {3, "score", "float64", /*primary_key=*/false, /*nullable=*/false},
    };
}

// Build a three-cell input row.
static std::vector<mongreldb::Cell> row(std::int64_t id,
                                        const std::string &name,
                                        double score) {
    return {
        {1, mongreldb::Value::integer(id)},
        {2, mongreldb::Value::string(name)},
        {3, mongreldb::Value::floating(score)},
    };
}

// Pretty-print every cell of a result set.
static void print_result(const std::string &label, const mongreldb::Result &res) {
    std::cout << "  " << label << ": " << res.rows.size() << " rows\n";
    for (const auto &r : res.rows) {
        std::cout << "    { ";
        for (std::size_t j = 0; j < r.size(); ++j) {
            const auto &c = r[j];
            std::cout << "col" << c.column_id << "=";
            switch (c.value.tag()) {
                case mongreldb::Value::Tag::Int64:  std::cout << c.value.as_int64(); break;
                case mongreldb::Value::Tag::Double: std::cout << c.value.as_double(); break;
                case mongreldb::Value::Tag::String: std::cout << c.value.as_string(); break;
                case mongreldb::Value::Tag::Bool:   std::cout << (c.value.as_bool() ? "true" : "false"); break;
                default:                             std::cout << "null"; break;
            }
            if (j + 1 < r.size()) std::cout << ", ";
        }
        std::cout << " }\n";
    }
}

int main() {
    try {
        mongreldb::MongrelDBClient db(kUrl);

        // 1. Health check; bail out if the daemon is unreachable.
        if (!db.health()) {
            std::cerr << "daemon not reachable at " << kUrl << "\n";
            return 1;
        }
        std::cout << "Connected to MongrelDB\n";

        // 2. Create the table.
        std::int64_t tid = db.create_table(kTable, schema());
        std::cout << "Created table " << kTable << " (id " << tid << ")\n";

        // 3. Load five rows with varying scores.
        db.put(kTable, row(1, "Alice", 40.0));
        db.put(kTable, row(2, "Bob", 65.0));
        db.put(kTable, row(3, "Carol", 82.0));
        db.put(kTable, row(4, "Dave", 91.0));
        db.put(kTable, row(5, "Eve", 12.5));
        std::cout << "Inserted 5 rows\n";

        // 4. Range query: 60 <= score <= 90 (both inclusive).
        mongreldb::Condition range_cond;
        range_cond.kind = mongreldb::CondKind::Range;
        range_cond.column_id = 3;     // score
        range_cond.lo = 60.0;
        range_cond.hi = 90.0;
        range_cond.lo_set = true;
        range_cond.hi_set = true;
        range_cond.lo_inclusive = true;
        range_cond.hi_inclusive = true;

        mongreldb::Result res = db.query(kTable, {range_cond});
        print_result("range [60, 90] on score", res);

        // 5. Primary-key lookup: id == 4 (Dave).
        mongreldb::Condition pk_cond;
        pk_cond.kind = mongreldb::CondKind::Pk;
        pk_cond.int_value = 4;

        res = db.query(kTable, {pk_cond});
        print_result("pk == 4", res);

        // 6. Cleanup.
        db.drop_table(kTable);
        std::cout << "Dropped table " << kTable << "\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
