# Errors and exceptions

The C++ client reports failures by throwing. Every exception the client
throws derives from `MongrelDBException`, so a single catch clause is enough
to handle any client-originated failure. For finer-grained handling, catch
the specific subclass.

---

## The exception hierarchy

```
MongrelDBException              // base - any client failure
├── AuthException               // 401 or 403 - bad creds or forbidden
├── NotFoundException           // 404 - table, user, or role missing
├── ConflictException           // 409 - idempotency mismatch, etc.
└── QueryException              // 400 or 5xx - bad request or server error
```

Each subclass exposes `what()` (inherited from `std::exception`) for a
human-readable message and, where relevant, the HTTP status code that produced
it.

| Class | When it is thrown | Typical HTTP status |
|-------|-------------------|---------------------|
| `MongrelDBException` | Any client failure (base class) | - |
| `AuthException` | Bad credentials or insufficient privilege | 401, 403 |
| `NotFoundException` | Referenced table, user, or role does not exist | 404 |
| `ConflictException` | Idempotency key mismatch, version conflict | 409 |
| `QueryException` | Malformed request, SQL error, or server-side failure | 400, 5xx |

Network failures (libcurl could not reach the server, timed out, etc.) and
JSON decode failures are also thrown as `MongrelDBException`. There is no
separate network exception subclass - if you need to distinguish them, check
the message or wrap the call.

## Catching by base class

For most application code, catching the base class is enough:

```cpp
try {
    db.put("orders", {{"id", mongreldb::Value::integer(1)}}, mongreldb::OpType::Put);
    auto res = db.query("orders", {cond});
} catch (const mongreldb::MongrelDBException &e) {
    std::cerr << "mongreldb error: " << e.what() << "\n";
}
```

## Catching by subclass

Handle the cases you can recover from, and let the rest bubble up:

```cpp
try {
    auto res = db.query("missing_table", {cond});
} catch (const mongreldb::NotFoundException &e) {
    // Table does not exist. Create it and retry.
    create_orders_table(db);
    auto res = db.query("missing_table", {cond});
} catch (const mongreldb::AuthException &e) {
    // Token expired or lacks privilege. Surface to the caller.
    throw;
} catch (const mongreldb::MongrelDBException &e) {
    // Anything else.
    std::cerr << "error: " << e.what() << "\n";
    throw;
}
```

The order of catch clauses matters: subclasses first, base last.

## HTTP status mapping

The client maps the server's HTTP response code to an exception class:

- **2xx** - success, no exception.
- **401 / 403** - `AuthException`.
- **404** - `NotFoundException`.
- **409** - `ConflictException`.
- **400 and 5xx** - `QueryException`. The message includes the server's error
  text when the body is an error envelope.
- Any other non-2xx code falls through to `QueryException`.

The server's error envelope (when present) is a small JSON object with a
`message` and sometimes a `code`. The client extracts the message and puts it
into `what()`, so the most useful detail surfaces without parsing.

## Conflict vs overwrite

MongrelDB treats a `Put` with an existing primary key as an overwrite, not a
conflict. You will not see a `ConflictException` from a duplicate-key `Put`.
`ConflictException` is reserved for genuine version conflicts - for example,
an idempotency key that does not match the one stored with the row.

If you need insert-if-absent semantics, use the idempotency key: the first
`Put` with a given key wins, and a later `Put` with the same key is treated
as a safe retry rather than an overwrite.

## Idempotent retries

Network glitches and redeploys should be safe to retry. Attach an idempotency
key to any mutating call, and the server will deduplicate retries within its
retention window:

```cpp
db.put("orders", row, mongreldb::OpType::Put, "client-uuid-1234");
```

A retry with the same key returns the original result instead of creating a
second row. If the second attempt carries a different row body, the server
raises a conflict (409) and the client throws `ConflictException`.

## RAII and resource safety

The client owns its libcurl handle and buffers behind a `unique_ptr` pimpl.
Destroying a client (or letting it go out of scope) frees everything. A
client is movable but not copyable - pass it by reference, store it by value,
or move it into place. Exceptions thrown mid-operation do not leak handles.

## A complete example

```cpp
#include <mongreldb/mongreldb.hpp>
#include <iostream>

int main() {
    mongreldb::MongrelDBClient db(
        "http://localhost:8080", mongreldb::Auth::none());

    try {
        mongreldb::Condition pk;
        pk.kind = mongreldb::CondKind::Pk;
        pk.int_value = 42;
        auto res = db.query("orders", {pk});

        for (const auto &row : res.rows) {
            for (const auto &cell : row) {
                std::cout << "col " << cell.column_id << "\n";
            }
        }
    } catch (const mongreldb::NotFoundException &e) {
        std::cerr << "not found: " << e.what() << "\n";
        return 1;
    } catch (const mongreldb::MongrelDBException &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```
