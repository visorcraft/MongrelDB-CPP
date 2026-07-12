# Quickstart

Zero to a running MongrelDB C++ program in fifteen minutes. This guide assumes
a fresh machine and walks through installing the prerequisites, starting the
daemon, and writing, running, and understanding a complete program.

---

## 1. Prerequisites

You need three things installed: a C++17 compiler, libcurl (with headers),
CMake, and a `mongreldb-server` daemon.

### Install a C++ compiler, libcurl, and CMake

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake libcurl4-openssl-dev
```

On Fedora:

```sh
sudo dnf install gcc-c++ cmake libcurl-devel
```

On macOS, the Xcode Command Line Tools provide clang++ and libcurl; install
CMake via Homebrew (`brew install cmake`).

Verify:

```sh
c++ --version
pkg-config --modversion libcurl   # 8.x
cmake --version                   # >= 3.16
```

### Install mongreldb-server

Fetch a prebuilt server binary from the
[MongrelDB releases](https://github.com/visorcraft/MongrelDB/releases):

```sh
mkdir -p bin
curl -fsSL -o bin/mongreldb-server \
  https://github.com/visorcraft/MongrelDB/releases/download/v0.50.0/mongreldb-server-linux-x64
chmod +x bin/mongreldb-server
```

Verify it runs:

```sh
./bin/mongreldb-server --version
```

## 2. Start the daemon

By default `mongreldb-server` listens on `http://127.0.0.1:8453` and stores
data in the directory you pass as its first argument.

```sh
mkdir -p /tmp/mdb-data
/path/to/mongreldb-server /tmp/mdb-data
```

In another terminal, sanity-check it:

```sh
curl http://127.0.0.1:8453/health
# ok
```

Leave the daemon running for the rest of this guide.

## 3. Create a project and pull in the client

The client is header-only. Drop the repository into your project (or add it as
a subdirectory) and add the include path:

```sh
mkdir mdb-demo && cd mdb-demo
git clone https://github.com/visorcraft/MongrelDB-CPP.git
```

## 4. Write your first program

Create `demo.cpp`:

```cpp
#include <mongreldb/mongreldb.hpp>
#include <iostream>
#include <optional>
#include <string>

int main() {
    // 1. Connect to the daemon. Empty url falls back to http://127.0.0.1:8453.
    mongreldb::MongrelDBClient db;

    // 2. Health check before doing anything else.
    if (!db.health()) {
        std::cerr << "daemon not reachable\n";
        return 1;
    }

    // 3. Create a table. Each column has a stable numeric id, a name, a type,
    //    and flags. The first column is the primary key.
    //
    //    `status` is constrained to {"pending", "active", "closed"} via
    //    `enum_variants`; the engine rejects any other value at commit time.
    //    `amount` carries a `default_value` of "0.0" so a `put` that omits
    //    the cell still produces a valid row.
    db.create_table("orders", {
        {1, "id",       "int64",   /*primary_key=*/true,  /*nullable=*/false},
        {2, "status",   "varchar", /*primary_key=*/false, /*nullable=*/false,
            /*enum_variants=*/{"pending", "active", "closed"}},
        {3, "amount",   "float64", /*primary_key=*/false, /*nullable=*/false,
            /*default_value=*/std::optional<std::string>{"0.0"}},
    });

    // 4. Insert rows. Cells pair column id + value.
    db.put("orders", {{1, mongreldb::Value::integer(1)},
                      {2, mongreldb::Value::string("active")},
                      {3, mongreldb::Value::floating(99.5)}});
    db.put("orders", {{1, mongreldb::Value::integer(2)},
                      {2, mongreldb::Value::string("pending")},
                      {3, mongreldb::Value::floating(150.0)}});

    // 5. Query with a native index condition. The range index serves this in
    //    sub-millisecond. Projection selects only column ids 1 and 2.
    mongreldb::Condition cond;
    cond.kind = mongreldb::CondKind::Range;
    cond.column_id = 3;
    cond.lo = 100.0; cond.lo_set = true;
    auto res = db.query("orders", {cond}, {1, 2}, 100);
    std::cout << "rows: " << res.rows.size() << "\n";

    // 6. Count the rows.
    std::cout << "total rows: " << db.count("orders") << "\n"; // 2

    // 7. Trying to insert a status outside the enum set throws
    //    `ConflictException` at commit time.
    try {
        db.put("orders", {{1, mongreldb::Value::integer(3)},
                          {2, mongreldb::Value::string("cancelled")},
                          {3, mongreldb::Value::floating(0.0)}});
    } catch (const mongreldb::ConflictException &e) {
        std::cerr << "rejected: " << e.code() << " - " << e.what() << "\n";
    }

    // 8. Omitting the `amount` cell falls back to its default (0.0).
    db.put("orders", {{1, mongreldb::Value::integer(4)},
                      {2, mongreldb::Value::string("active")}});

    return 0;
}
```

Build and run it:

```sh
c++ -std=c++17 -IMongrelDB-CPP/include demo.cpp $(pkg-config --cflags --libs libcurl) -o demo
./demo
```

You should see the row count of 2 plus the rejected-write log line.

## 5. What each part does

| Code | What it does |
|------|--------------|
| `MongrelDBClient db` | Builds an HTTP client targeting one daemon. RAII; cleans up in the destructor. |
| `db.health()` | GET `/health`; returns `true` when the daemon answers. Always check before real work. |
| `db.create_table(name, cols)` | POST `/kit/create_table`. Column `id`s are the on-wire identifiers; use them everywhere else. Per-column `enum_variants` and `default_value` constrain writes server-side. |
| `db.put(table, cells)` | Single-op transaction: POST `/kit/txn` with one `put` op. `cells` is flattened to `[col_id, val, ...]`. Cells omitted from a write fall back to the column's `default_value`. |
| `db.query(...)` | Builds a `/kit/query` body. Conditions push down to native indexes. |
| `{1, 2}` projection | Server returns only those column ids, saving bandwidth. |
| `limit = 100` | Caps the result; check `res.truncated` afterward to detect overflow. |
| `db.count(table)` | GET `/tables/{name}/count`. |

## 6. History retention and typed defaults

MongrelDB keeps a rolling MVCC history window. You can resize it and inspect
its bounds from the C++ client:

```cpp
// Requires ADMIN permission when the daemon runs with auth enabled.
auto cfg = db.set_history_retention_epochs(1024);
std::cout << "retention: " << db.history_retention_epochs() << " epochs\n";
std::cout << "earliest retained: " << db.earliest_retained_epoch() << "\n";

// Read an older version of a row via SQL:
// SELECT value FROM orders AS OF EPOCH <epoch> WHERE id = 1;
```

**Cannot restore discarded history.** The first time retention is enabled the
window starts at the *current* epoch; earlier versions may already have been
compacted. Increasing the window later cannot recreate epochs that fell outside
the previous guarantee.

For typed column defaults, use `default_value_json` for raw JSON scalars or
`default_expr` for dynamic values such as `"now"`:

```cpp
db.create_table("orders", {
    {1, "id",      "int64",   /*primary_key=*/true,  /*nullable=*/false},
    {2, "status",  "varchar", /*primary_key=*/false, /*nullable=*/false,
        /*enum_variants=*/{"pending", "active", "closed"}},
    {3, "amount",  "float64", /*primary_key=*/false, /*nullable=*/true,
        /*default_value=*/std::optional<std::string>{"0.0"}},
    {4, "label",   "varchar", /*primary_key=*/false, /*nullable=*/true,
        /*default_value_json=*/std::optional<std::string>{"\"none\""}},
    {5, "created", "timestamp_nanos", /*primary_key=*/false, /*nullable=*/false,
        /*default_expr=*/std::optional<std::string>{"now"}},
});
```

`default_value_json` is sent verbatim, so string literals must include their
JSON quotes (`"\"none\""`).  `default_expr: "now"` tells the engine to fill
in the current timestamp when the column is omitted from a write.

## 7. Common pitfalls

**Using the column name instead of the column id.** Every on-wire API uses the
numeric `id` from `create_table`, never the `name`. Conditions take the int64
`column_id`, not the string name.

**Treating a single `put` as non-transactional.** `put` is a one-op
transaction. A unique constraint violation surfaces as a `ConflictException`
(HTTP 409), not as a silent no-op.

**Letting exceptions escape into a destructor or `noexcept` context.** All
client methods throw on failure. Wrap calls in `try`/`catch` in contexts where
an exception would be fatal, and never call client methods from a destructor.

**Expecting `sql` to always return rows.** The client sends `format:"json"`
and the daemon returns JSON arrays for `SELECT`, but the body is handed back
as a raw `std::string` - you parse it yourself or re-shape it into a `Result`.
Use the native query builder when you want typed row retrieval.

**Pointing at a daemon that requires auth.** If the daemon was started with
`--auth-token` or `--auth-users`, every call throws `AuthException` unless you
construct with the token or `BasicAuth` overload. See [auth.md](auth.md).

## Next steps

- [transactions.md](transactions.md) - atomic batches, idempotency, retries
- [queries.md](queries.md) - every native index condition
- [sql.md](sql.md) - recursive CTEs, window functions, `CREATE TABLE AS SELECT`
- [auth.md](auth.md) - bearer tokens, basic auth, user/role management
- [errors.md](errors.md) - the exception hierarchy and recovery patterns
- `examples/column_constraints.cpp` - a runnable demo of `enum_variants`
  and `default_value`, including the rejection path.
