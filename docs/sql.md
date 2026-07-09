# SQL

MongrelDB ships a SQL surface for the cases the native index conditions do not
cover: joins, aggregations, vector similarity (ANN), sparse match, and MinHash
similarity. The C++ client exposes it through `sql()`.

```cpp
std::string rows = db.sql("SELECT id, amount FROM orders WHERE amount > 100");
```

The server returns the raw result as JSON. The client hands you that body as a
`std::string` - parse it with your favorite JSON library, or use the
`Value` / `Result` decoder helpers from [queries.md](queries.md) when you
re-shape the rows yourself.

---

## When to use SQL vs native queries

| Use native `query()` | Use `sql()` |
|-----------------------|-------------|
| Point lookups by primary key | Joins across tables |
| Range scans on a numeric column | Aggregations (COUNT, SUM, GROUP BY) |
| Bitmap equality on low-cardinality columns | Vector similarity (`ANN`) |
| FM-index substring / full-text | Sparse match, MinHash similarity |
| Anything the engine can push to a single index | Arbitrary predicates and combinations |

Native `query()` always wins on latency for the cases it covers, because the
engine skips the SQL planner and goes straight to the index. Reach for SQL when
you need expressiveness the native conditions cannot give you.

## Running a statement

`sql()` takes a SQL string and returns the raw JSON body the server produced.
DML (INSERT, UPDATE, DELETE) and DDL (CREATE TABLE, CREATE INDEX) are all
supported - the server reports affected rows or status in the returned JSON.

```cpp
// DDL
db.sql("CREATE TABLE orders (id INT PRIMARY KEY, customer TEXT, amount DOUBLE)");

// DML
db.sql("INSERT INTO orders VALUES (1, 'Alice', 250.0)");

// Read
std::string body = db.sql("SELECT id, amount FROM orders WHERE amount > 100");
```

If the server rejects the statement it returns a non-2xx status, which the
client maps to a `QueryException` (or `AuthException` for 401/403). See
[errors.md](errors.md).

## Vector similarity (ANN)

Approximate nearest neighbor search is a SQL function over a vector column.
Pass the query vector as a SQL literal and a limit.

```cpp
std::string body = db.sql(
    "SELECT id FROM items ORDER BY embedding ANN '[0.12,0.43,0.55,...]' LIMIT 10");
```

For typed access to the returned ids, re-shape the body into a `Result` or
decode the ids yourself. The server returns the ranked ids in the JSON body.

## Sparse match and MinHash similarity

Sparse match and MinHash similarity are likewise SQL functions:

```cpp
// Sparse match over a set column
db.sql("SELECT id FROM docs WHERE tokens SPARSE_MATCH '{1,4,9,12}' LIMIT 10");

// MinHash similarity for near-duplicate detection
db.sql("SELECT id FROM docs ORDER BY mh SIMILARITY '{...}' LIMIT 5");
```

## Combining with native queries

A common pattern: use `query()` to find candidate rows by index, then run a
SQL statement over a narrower set. Or run a SQL `SELECT` for the parts the
indexes cannot express, and a native `query()` for the indexed predicates.
The two paths share the same tables and the same data - pick whichever wins
for the predicate at hand.

## Error handling

`sql()` throws on any non-2xx response. Catch `QueryException` for SQL syntax
and runtime errors, and `MongrelDBException` as the catch-all base:

```cpp
try {
    std::string body = db.sql("SELECT * FROM missing_table");
} catch (const mongreldb::QueryException &e) {
    std::cerr << "query failed: " << e.what() << "\n";
} catch (const mongreldb::MongrelDBException &e) {
    std::cerr << "other error: " << e.what() << "\n";
}
```

See [errors.md](errors.md) for the full exception hierarchy and the HTTP
status mapping.
