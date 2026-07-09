// Example: atomic batch transactions with an idempotent retry in C++.
//
// Build (from the repo root), linking libcurl and the client source:
//
//   c++ -std=c++17 -Iinclude examples/transactions.cpp src/mongreldb.cpp
//       $(pkg-config --cflags --libs libcurl) -o examples/transactions
//   ./examples/transactions
//
// Requires a mongreldb-server daemon running on http://127.0.0.1:8453, or
// point MONGRELDB_URL at a running daemon.
//
// Creates a table, builds one batch of three puts, and commits them
// atomically. It then verifies the row count. Finally it stages a fourth
// put and commits it twice with the SAME idempotency key: the daemon
// replays the first commit's result so the second commit is a no-op. The
// table is dropped at the end (even on error).

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

        // 2. Per-run unique table name and idempotency key. A reused key
        //    replays the original result and silently drops the new batch, so
        //    the key must be unique per run too.
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        std::string table = "example_txn_" + std::to_string(now);
        std::string txn_key = "example-txn-key-" + std::to_string(now);

        // 3. Create the table.
        std::int64_t tid = db.create_table(table, schema());
        std::cout << "Created table " << table << " (id " << tid << ")\n";

        // Guaranteed cleanup: drop the table no matter how the body exits.
        TableGuard guard(db, table);

        // Build a PUT op for the given row.
        auto put_op = [&](std::int64_t id, const std::string &name, double score) {
            mongreldb::Op op;
            op.type = mongreldb::OpType::Put;
            op.table = table;
            op.cells = {
                {1, mongreldb::Value::integer(id)},
                {2, mongreldb::Value::string(name)},
                {3, mongreldb::Value::floating(score)},
            };
            return op;
        };

        // 4. Stage three puts and commit them atomically.
        std::vector<mongreldb::Op> batch1 = {
            put_op(1, "Alice", 95.5),
            put_op(2, "Bob", 82.0),
            put_op(3, "Carol", 78.3),
        };
        db.commit(batch1);
        std::cout << "Committed transaction with 3 puts\n";

        // 5. Verify the row count.
        std::cout << "Total rows after commit: " << db.count(table) << "\n";

        // 6. Idempotent retry: stage a fourth put and commit twice with the
        //    same idempotency key. The second commit is replayed as a no-op.
        std::vector<mongreldb::Op> batch2 = {put_op(4, "Dave", 60.0)};
        db.commit(batch2, txn_key);
        std::cout << "Committed 4th put with idempotency key " << txn_key << "\n";

        db.commit(batch2, txn_key);
        std::cout << "Recommitted with same key (idempotent replay)\n";

        std::cout << "Total rows after idempotent retry: " << db.count(table) << "\n";

        std::cout << "Dropped table " << table << "\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
