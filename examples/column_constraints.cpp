// Example: column-level constraints in C++ (`enum_variants`, `default_value`).
//
// Build (from the repo root), linking libcurl and the client source:
//
//   c++ -std=c++17 -Iinclude examples/column_constraints.cpp src/mongreldb.cpp
//       $(pkg-config --cflags --libs libcurl) -o examples/column_constraints
//   ./examples/column_constraints
//
// Requires a mongreldb-server daemon running on http://127.0.0.1:8453, or
// point MONGRELDB_URL at a running daemon.
//
// Creates a single table whose schema uses both optional Column fields:
//
//   col 2 = status (varchar) constrained to {"pending","active","closed"}
//   col 3 = amount (float64) carrying a default_value of "0.0"
//
// Inserts one valid row, then proves the engine rejects an out-of-enum
// status with a ConflictException, then proves a write that omits the
// `amount` cell succeeds (the engine substitutes the default). Finally
// prints the resulting row and drops the table.

#include <mongreldb/mongreldb.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

static const char *kUrlDefault = "http://127.0.0.1:8453";

// Build the four-column schema:
//   col 1 = id (int64, primary key)
//   col 2 = status (varchar) constrained to {"pending","active","closed"}
//   col 3 = amount (float64) carrying a default_value of "0.0"
//   col 4 = note (varchar, no constraints - the control case)
static std::vector<mongreldb::Column> schema() {
    mongreldb::Column c1;
    c1.id = 1; c1.name = "id"; c1.ty = "int64";
    c1.primary_key = true; c1.nullable = false;

    mongreldb::Column c2;
    c2.id = 2; c2.name = "status"; c2.ty = "varchar";
    c2.primary_key = false; c2.nullable = false;
    c2.enum_variants = {"pending", "active", "closed"};

    mongreldb::Column c3;
    c3.id = 3; c3.name = "amount"; c3.ty = "float64";
    c3.primary_key = false; c3.nullable = false;
    c3.default_value = std::optional<std::string>{"0.0"};

    mongreldb::Column c4;
    c4.id = 4; c4.name = "note"; c4.ty = "varchar";
    c4.primary_key = false; c4.nullable = true;

    return {c1, c2, c3, c4};
}

// Build a four-cell input row. Pass std::nullopt for `amount` to omit the
// cell and let the engine's default_value fire.
static std::vector<mongreldb::Cell> row(std::int64_t id,
                                        const std::string &status,
                                        std::optional<double> amount,
                                        const std::string &note) {
    std::vector<mongreldb::Cell> cells = {
        {1, mongreldb::Value::integer(id)},
        {2, mongreldb::Value::string(status)},
        {4, mongreldb::Value::string(note)},
    };
    if (amount.has_value()) {
        cells.insert(cells.begin() + 2,
                     {3, mongreldb::Value::floating(*amount)});
    }
    return cells;
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

// Scope guard: drops the table on destruction unless release()d.
class TableGuard {
public:
    TableGuard(mongreldb::MongrelDBClient &db, const std::string &table)
        : db_(&db), table_(&table) {}
    ~TableGuard() {
        if (db_ && table_) {
            try { db_->drop_table(*table_); } catch (...) {}
        }
    }
    TableGuard(const TableGuard &) = delete;
    TableGuard &operator=(const TableGuard &) = delete;
private:
    mongreldb::MongrelDBClient *db_;
    const std::string *table_;
};

int main() {
    const char *url = std::getenv("MONGRELDB_URL");
    if (url == nullptr || url[0] == '\0') {
        url = kUrlDefault;
    }

    try {
        mongreldb::MongrelDBClient db(url);

        // 1. Health check; bail out if the daemon is unreachable.
        if (!db.health()) {
            std::cerr << "daemon not reachable at " << url << "\n";
            return 1;
        }
        std::cout << "Connected to MongrelDB\n";

        // 2. Per-run unique table name so concurrent or repeated runs never
        //    collide on a shared daemon.
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        std::string table = "example_constraints_" + std::to_string(now);

        std::int64_t tid = db.create_table(table, schema());
        std::cout << "Created table " << table << " (id " << tid << ")\n";

        // Guaranteed cleanup: drop the table no matter how the body exits.
        TableGuard guard(db, table);

        // 3. Valid insert: status is in the enum set, amount is supplied.
        db.put(table, row(1, "active", 99.5, "first"));
        std::cout << "Inserted row 1 with valid enum status 'active'\n";

        // 4. The engine rejects an out-of-enum status with a 409.
        bool rejected = false;
        try {
            db.put(table, row(2, "cancelled", 50.0, "should fail"));
        } catch (const mongreldb::ConflictException &e) {
            rejected = true;
            std::cout << "Correctly rejected bad enum value 'cancelled'"
                      << " (code=" << e.code() << ")\n";
        }
        if (!rejected) {
            std::cerr << "expected ConflictException for bad enum value\n";
            return 1;
        }

        // 5. Omitting the amount cell lets the engine apply its default.
        db.put(table, row(3, "pending", std::nullopt, "no amount supplied"));
        std::cout << "Inserted row 3 omitting amount; engine applies default\n";

        // 6. Read the table back and confirm the default landed on row 3.
        mongreldb::Result res = db.query(table, {}, {1, 2, 3, 4}, 100);
        print_result("all rows", res);

        std::cout << "Total rows: " << db.count(table) << "\n";

        std::cout << "Dropped table " << table << "\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}