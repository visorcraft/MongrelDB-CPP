// Example: atomic batch transactions with an idempotent retry in C++.
//
// Build (from the repo root):
//
//   c++ -std=c++17 -Iinclude examples/transactions.cpp \
//       $(pkg-config --cflags --libs libcurl) -o examples/transactions
//   ./examples/transactions
//
// Requires a mongreldb-server daemon running on http://127.0.0.1:8453.
//
// Creates a table, builds one batch of three puts, and commits them
// atomically. It then verifies the row count. Finally it stages a fourth
// put and commits it twice with the SAME idempotency key: the daemon
// replays the first commit's result so the second commit is a no-op. The
// table is dropped at the end.

#include <mongreldb/mongreldb.hpp>

#include <iostream>

static const char *kUrl = "http://127.0.0.1:8453";
static const char *kTable = "example_txn";
static const char *kTxnKey = "example-txn-key";

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

// Build a PUT op for the given row.
static mongreldb::Op put_op(std::int64_t id, const std::string &name, double score) {
    mongreldb::Op op;
    op.type = mongreldb::OpType::Put;
    op.table = kTable;
    op.cells = {
        {1, mongreldb::Value::integer(id)},
        {2, mongreldb::Value::string(name)},
        {3, mongreldb::Value::floating(score)},
    };
    return op;
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

        // 3. Stage three puts and commit them atomically.
        std::vector<mongreldb::Op> batch1 = {
            put_op(1, "Alice", 95.5),
            put_op(2, "Bob", 82.0),
            put_op(3, "Carol", 78.3),
        };
        db.commit(batch1);
        std::cout << "Committed transaction with 3 puts\n";

        // 4. Verify the row count.
        std::cout << "Total rows after commit: " << db.count(kTable) << "\n";

        // 5. Idempotent retry: stage a fourth put and commit twice with the
        //    same idempotency key. The second commit is replayed as a no-op.
        std::vector<mongreldb::Op> batch2 = {put_op(4, "Dave", 60.0)};
        db.commit(batch2, kTxnKey);
        std::cout << "Committed 4th put with idempotency key " << kTxnKey << "\n";

        db.commit(batch2, kTxnKey);
        std::cout << "Recommitted with same key (idempotent replay)\n";

        std::cout << "Total rows after idempotent retry: " << db.count(kTable) << "\n";

        // 6. Cleanup.
        db.drop_table(kTable);
        std::cout << "Dropped table " << kTable << "\n";
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
