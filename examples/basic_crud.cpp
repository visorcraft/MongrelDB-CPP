// Example: basic CRUD operations with the MongrelDB C++ client.
//
// Build (from the repo root), linking libcurl and the client source:
//
//   c++ -std=c++17 -Iinclude examples/basic_crud.cpp src/mongreldb.cpp
//       $(pkg-config --cflags --libs libcurl) -o examples/basic_crud
//   ./examples/basic_crud
//
// Requires a mongreldb-server daemon running on http://127.0.0.1:8453, or
// point MONGRELDB_URL at a running daemon.
//
// Creates a table, inserts three rows, counts them, queries all rows,
// upserts (updates) one row by primary key, deletes one row, then drops
// the table. Progress is printed at every step.

#include <mongreldb/mongreldb.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

static const char *kUrlDefault = "http://127.0.0.1:8453";

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

// A simple scope guard: drops the table on destruction unless release()d.
// Guarantees cleanup even when the body throws or returns early.
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

        // 2. Create the table with a per-run unique name (millis since epoch)
        //    so concurrent or repeated runs never collide on a shared daemon.
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        std::string table = "example_crud_" + std::to_string(now);

        std::int64_t tid = db.create_table(table, schema());
        std::cout << "Created table " << table << " (id " << tid << ")\n";

        // Guaranteed cleanup: drop the table no matter how the body exits
        // (success, thrown exception, or early return).
        TableGuard guard(db, table);

        // 3. Insert three rows.
        db.put(table, row(1, "Alice", 95.5));
        db.put(table, row(2, "Bob", 82.0));
        db.put(table, row(3, "Carol", 78.3));
        std::cout << "Inserted 3 rows\n";

        // 4. Count.
        std::cout << "Total rows: " << db.count(table) << "\n";

        // 5. Query all rows (no conditions, no projection, no limit).
        mongreldb::Result res = db.query(table);
        print_result("all rows", res);

        // 6. Upsert (update) Alice's score. update_cells supplies the values
        //    written on a primary-key conflict.
        db.upsert(table, row(1, "Alice", 100.0),
                  {{2, mongreldb::Value::string("Alice")},
                   {3, mongreldb::Value::floating(100.0)}});
        std::cout << "Upserted Alice's score to 100.0\n";
        std::cout << "Total rows after upsert: " << db.count(table) << "\n";

        // 7. Delete Carol (primary key 3).
        db.delete_by_pk(table, mongreldb::Value::integer(3));
        std::cout << "Deleted Carol; remaining rows: " << db.count(table) << "\n";

        std::cout << "Dropped table " << table << "\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
