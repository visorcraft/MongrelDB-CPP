<p align="center">
  <img src="assets/mongrel.png" alt="MongrelDB logo" width="250" />
</p>

<h1 align="center">MongrelDB C++ Client</h1>

<p align="center">
  <b>Header-only C++17 HTTP client for MongrelDB - embedded+server database with SQL, vector search, full-text search, and AI-native retrieval.</b>
  <br />
  RAII wrapper, std::string/std::vector/std::optional, and an exception hierarchy. Built on libcurl. No external runtime dependencies beyond libc and libcurl. Also bundles the engine's C ABI header for native embedding.
</p>

<p align="center">
  <a href="https://github.com/visorcraft/MongrelDB-CPP/actions/workflows/ci.yml"><img src="https://github.com/visorcraft/MongrelDB-CPP/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/visorcraft/MongrelDB/releases"><img src="https://img.shields.io/badge/server-v0.46.2-blue.svg" alt="MongrelDB server" /></a>
  <a href="#license"><img src="https://img.shields.io/badge/license-MIT%20OR%20Apache--2.0-blue.svg" alt="License" /></a>
</p>

## Package

| Surface | Package | Install |
|---|---|---|
| C++ client | `MongrelDB-CPP` | header-only; build from source with CMake + libcurl |

## Requirements

- **A C++17 compiler** (g++ 7+, clang++ 5+, MSVC 19.14+)
- **libcurl** (the HTTP transport). On Debian/Ubuntu install `libcurl4-openssl-dev`; on Fedora `curl-devel`; on macOS it ships with the system.
- **CMake 3.16 or newer** (to build the tests; the library itself is header-only)
- A running [`mongreldb-server`](https://github.com/visorcraft/MongrelDB) daemon

## What It Provides

- **Typed CRUD** over the Kit transaction endpoint: `put`, `upsert` (insert-or-update on PK conflict), `del` by row id, `delete_by_pk`, with idempotency keys for safe retries.
- **Query builder** that pushes conditions down to the engine's specialized indexes for sub-millisecond lookups: bitmap equality, learned-range, null checks, and FM-index full-text search. Conditions are AND-ed.
- **Idempotent batch transactions** - all operations staged locally and committed atomically, with the engine enforcing unique, foreign key, and check constraints at commit time. Idempotency keys return the original response on duplicate commits, even after a crash.
- **Full SQL access** through the DataFusion-backed `/sql` endpoint: recursive CTEs, window functions, `CREATE TABLE AS SELECT`, materialized views, and multi-statement execution.
- **Schema management**: typed table creation with optional `enum_variants` and `default_value` per column, full schema catalog, and per-table descriptors.
- **RAII and move semantics**: the client owns its libcurl state; destructor cleans up. Not copyable, movable.
- **Bundled C ABI header** (`mongreldb/mongreldb_engine.h`) - a copy of the engine's native interface, for users who want to embed the engine directly rather than talk HTTP.
- **Exception hierarchy**: `MongrelDBException` (base), `AuthException` (401/403), `NotFoundException` (404), `ConflictException` (409, with structured code and op index), `QueryException` (everything else).
- **Modern types**: `std::vector`, `std::optional`, `std::string`, `std::int64_t`. No raw pointers in the public API.

## Examples

Runnable, commented examples live in the docs:

- [Quickstart](docs/quickstart.md) - install, start the daemon, write and run a complete program.
- [Transactions](docs/transactions.md) - batch commits, idempotency keys, constraint handling.
- [Queries](docs/queries.md) - every native condition type and the index it pushes down to.
- [SQL](docs/sql.md) - recursive CTEs, window functions, advanced SQL.
- [Authentication](docs/auth.md) - bearer token, HTTP Basic, and open modes.
- [Errors](docs/errors.md) - the exception hierarchy and recovery patterns.
- `examples/column_constraints.cpp` - `enum_variants` and `default_value` columns.

## Quick Example

```cpp
#include <mongreldb/mongreldb.hpp>
#include <iostream>

int main() {
    // Connect to a running mongreldb-server daemon.
    mongreldb::MongrelDBClient db("http://127.0.0.1:8453");

    // Create a table. Column ids are stable on-wire identifiers.
    db.create_table("orders", {
        {1, "id",       "int64",   /*primary_key=*/true,  /*nullable=*/false},
        {2, "customer", "varchar", /*primary_key=*/false, /*nullable=*/false},
        {3, "amount",   "float64", /*primary_key=*/false, /*nullable=*/false},
    });

    // Insert rows (cells pair column id + value).
    db.put("orders", {{1, mongreldb::Value::integer(1)},
                      {2, mongreldb::Value::string("Alice")},
                      {3, mongreldb::Value::floating(99.50)}});
    db.put("orders", {{1, mongreldb::Value::integer(2)},
                      {2, mongreldb::Value::string("Bob")},
                      {3, mongreldb::Value::floating(150.00)}});

    // Query with a native index condition (learned-range index).
    mongreldb::Condition cond;
    cond.kind = mongreldb::CondKind::Range;
    cond.column_id = 3;
    cond.lo = 100.0; cond.lo_set = true;
    auto res = db.query("orders", {cond}, {1, 2}, 100);
    std::cout << "rows: " << res.rows.size() << "\n";

    std::cout << "count: " << db.count("orders") << "\n"; // 2

    // Run SQL.
    db.sql("UPDATE orders SET amount = 200.0 WHERE customer = 'Bob'");

    return 0;
}
```

## Authentication

```cpp
// Bearer token (--auth-token mode)
mongreldb::MongrelDBClient db("http://127.0.0.1:8453", "my-secret-token");

// HTTP Basic (--auth-users mode)
mongreldb::MongrelDBClient db("http://127.0.0.1:8453",
                              mongreldb::MongrelDBClient::BasicAuth{"admin", "s3cret"});
```

A token takes precedence over basic auth if both are supplied.

## Batch transactions

Operations are staged locally and committed atomically. The engine enforces
unique, foreign key, and check constraints at commit time.

```cpp
std::vector<mongreldb::Op> ops(3);
ops[0].type = mongreldb::OpType::Put;
ops[0].table = "orders";
ops[0].cells = {{1, mongreldb::Value::integer(10)},
                {2, mongreldb::Value::string("Dave")},
                {3, mongreldb::Value::floating(50.00)}};
ops[1].type = mongreldb::OpType::Put;
ops[1].table = "orders";
ops[1].cells = {{1, mongreldb::Value::integer(11)},
                {2, mongreldb::Value::string("Eve")},
                {3, mongreldb::Value::floating(75.00)}};
ops[2].type = mongreldb::OpType::DeleteByPk;
ops[2].table = "orders";
ops[2].pk_value = mongreldb::Value::integer(2);

try {
    db.commit(ops, "batch-1");           // atomic - all or nothing
} catch (const mongreldb::ConflictException &e) {
    std::cerr << "constraint " << e.code() << " at op "
              << e.op_index().value_or(0) << ": " << e.what() << "\n";
}
```

## Native query builder

Conditions push down to the engine's specialized indexes. Each `Condition`
targets one index; multiple conditions are AND-ed.

```cpp
// Bitmap equality (low-cardinality columns)
mongreldb::Condition bitmap;
bitmap.kind = mongreldb::CondKind::BitmapEq;
bitmap.column_id = 2; bitmap.str_value = "Alice";

// Range query (learned-range index)
mongreldb::Condition range;
range.kind = mongreldb::CondKind::Range;
range.column_id = 3; range.lo = 50.0; range.lo_set = true;
range.hi = 150.0; range.hi_set = true;

// Full-text search (FM-index)
mongreldb::Condition fts;
fts.kind = mongreldb::CondKind::FmContains;
fts.column_id = 2; fts.str_value = "database performance";

auto res = db.query("orders", {bitmap, range}, {1, 3}, 50);
if (res.truncated) {
    // result set hit the limit; more matches exist on the server
}
```

## Column constraints: enums and defaults

Each `mongreldb::Column` carries optional fields that the engine
enforces at commit time:

- `enum_variants` (`std::vector<std::string>`) - restricts a varchar column to
  a fixed variant set. The engine rejects writes outside the set.
- `default_value_json` sends a caller-validated raw JSON scalar;
  `default_expr` sends `"now"` or `"uuid"` and takes precedence. The legacy
  string `default_value` remains supported.

These fields are omitted from the wire payload when unset, so older servers
that don't recognise them still accept the request.

```cpp
db.create_table("orders", {
    {1, "id",      "int64",   /*primary_key=*/true,  /*nullable=*/false},
    {2, "status",  "varchar", /*primary_key=*/false, /*nullable=*/false,
        /*enum_variants=*/{"pending", "active", "closed"}},
    {3, "amount",  "float64", /*primary_key=*/false, /*nullable=*/true,
        /*default_value=*/std::optional<std::string>{"0.0"}},
});
```

Table CHECKs use the additive raw constraints overload:

```cpp
db.create_table("orders", columns,
    R"({"checks":[{"id":1,"name":"amount_nonneg","expr":{"Ge":[{"Col":3},{"Lit":{"Float64":0.0}}]}}]})");
```

Trying to insert `"cancelled"` into `status` throws a `ConflictException` at
commit time. A `put` that omits the `amount` cell writes the default `0.0`
instead.

## SQL

```cpp
db.sql("INSERT INTO orders (id, customer, amount) VALUES (99, 'Zoe', 999.0)");
db.sql("CREATE TABLE archive AS SELECT * FROM orders WHERE amount > 500");

// Recursive CTEs and window functions
db.sql("WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM r WHERE n<10) "
       "SELECT n FROM r");
db.sql("SELECT id, ROW_NUMBER() OVER (PARTITION BY customer ORDER BY amount DESC) "
       "FROM orders");
```

## User & role management

User, role, and permission management is performed through SQL against the
daemon's catalog. Passwords are Argon2id-hashed server-side.

```cpp
db.sql("CREATE USER admin WITH PASSWORD 's3cret-pw'");
db.sql("ALTER USER admin SET ADMIN TRUE");

db.sql("CREATE ROLE analyst");
db.sql("GRANT select ON orders TO analyst");   // table-level permission
db.sql("GRANT analyst TO alice");

db.sql("SELECT username FROM catalog.users");  // list users
db.sql("SELECT name FROM catalog.roles");      // list roles
```

## Error handling

Every failure throws a subclass of `MongrelDBException`. Catch the base for
"anything went wrong", or a specific subclass to branch on the category.

```cpp
try {
    auto body = db.schema_for("missing_table");
} catch (const mongreldb::ConflictException &e) {
    std::cerr << "constraint " << e.code() << ": " << e.what() << "\n";
} catch (const mongreldb::NotFoundException &e) {
    std::cerr << "not found: " << e.what() << "\n";
} catch (const mongreldb::AuthException &e) {
    std::cerr << "not authorized: " << e.what() << "\n";
} catch (const mongreldb::QueryException &e) {
    std::cerr << "query/server error: " << e.what() << "\n";
} catch (const mongreldb::MongrelDBException &e) {
    std::cerr << "error: " << e.what() << "\n";
}
```

## API reference

### MongrelDBClient

| Method | Description |
|--------|-------------|
| `MongrelDBClient(url)` | Construct a client (empty url defaults to `http://127.0.0.1:8453`) |
| `MongrelDBClient(url, token)` | Bearer token auth (`--auth-token` mode) |
| `MongrelDBClient(url, BasicAuth{user, pass})` | HTTP Basic auth (`--auth-users` mode) |
| `set_timeout(seconds)` | Per-request timeout (default 30) |
| `health()` | Check daemon health (returns `bool`) |
| `table_names()` | List table names (`vector<string>`) |
| `create_table(name, columns)` | Create a table (returns table id); each `Column` may set `enum_variants` and `default_value` |
| `create_table(name, columns, constraints_json)` | Create a table with native `constraints` JSON (including CHECKs) |
| `drop_table(name)` | Drop a table |
| `count(table)` | Row count |
| `put(table, cells, key)` | Insert a row |
| `upsert(table, cells, update_cells, key)` | Upsert a row |
| `del(table, row_id)` | Delete by row id |
| `delete_by_pk(table, pk)` | Delete by primary key |
| `commit(ops, key)` | Commit a batch atomically |
| `query(table, conds, proj, limit)` | Run a native query (returns `Result`) |
| `sql(statement)` | Execute SQL (returns raw body) |
| `schema()` | Full schema catalog (raw JSON) |
| `schema_for(table)` | Single-table descriptor (raw JSON) |

### Column

| Field | Type | Purpose |
|-------|------|---------|
| `id` | `int64` | Stable on-wire column identifier. |
| `name` | `string` | Human-readable name (not used on the wire). |
| `ty` | `string` | Engine type tag: `int64`, `float64`, `varchar`, `bool`, ... |
| `primary_key` | `bool` | Marks the primary key column. |
| `nullable` | `bool` | Allows NULL cells. |
| `enum_variants` | `vector<string>` | Optional. Restricts a varchar column to these values; omitted from the wire payload when empty. |
| `default_value` | `optional<string>` | Optional. Engine default-value DSL expression applied when a `put` omits the column; omitted when unset. |

### Exception hierarchy

| Class | HTTP status | Notes |
|-------|-------------|-------|
| `MongrelDBException` | - | Base; catch for "anything went wrong" |
| `AuthException` | 401, 403 | Bad/missing credentials |
| `NotFoundException` | 404 | Missing table, schema, or resource |
| `ConflictException` | 409 | Carries `.code()` and `.op_index()` |
| `QueryException` | 400, 5xx, network | Everything else |

### Bundled engine ABI

`include/mongreldb/mongreldb_engine.h` is a verbatim copy of the engine's C ABI
for users who link the engine natively (via `libmongreldb`). See the comments in
that header for the handle-based put/query/transaction/auth surface.

## Building and testing

The library is header-only; consumers only need the include path and libcurl.
The CMake project builds the tests:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run the live test suite (boots mongreldb-server itself if it can find the
# binary in this order: MONGRELDB_SERVER env var, ./bin/mongreldb-server,
# or mongreldb-server on PATH). Set MONGRELDB_URL to use an already-running
# daemon. Tests self-skip when no binary is available.
ctest --test-dir build --output-on-failure
```

Fetch a prebuilt server binary from the [MongrelDB releases](https://github.com/visorcraft/MongrelDB/releases):

```sh
mkdir -p bin
curl -fsSL -o bin/mongreldb-server \
  https://github.com/visorcraft/MongrelDB/releases/download/v0.46.2/mongreldb-server-linux-x64
chmod +x bin/mongreldb-server
```

### Using the client in your project

With CMake:

```cmake
# In your CMakeLists.txt
find_package(CURL REQUIRED)
add_subdirectory(mongreldb_cpp)
target_link_libraries(your_app PRIVATE mongreldb_cpp)
```

Or compile directly (header-only):

```sh
c++ -std=c++17 -I/path/to/MongrelDB-CPP/include your_app.cpp -lcurl -o your_app
```

## Contributing

Contributions are welcome. Please:

1. Open an issue first for non-trivial changes.
2. Add focused tests near your change - the suite must stay green.
3. Keep the code C++17, warning-clean under `-Wall -Wextra -Wpedantic`.
4. Match the existing style: 4-space indent, `snake_case` methods, RAII, no
   raw pointers in the public API.

## License

Dual-licensed under the **MIT License** or the **Apache License, Version 2.0**,
at your option. See [MIT](LICENSE-MIT) OR [Apache-2.0](LICENSE-APACHE) for the full text.

`SPDX-License-Identifier: MIT OR Apache-2.0`
