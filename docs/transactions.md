# Transactions

MongrelDB commits every write through a single atomic transaction endpoint
(`POST /kit/txn`). This guide covers the two ways to use it - a one-shot single
op, and a staged batch - plus idempotency keys for safe retries and
constraint-violation handling.

The engine enforces `UNIQUE`, foreign-key, check, and trigger constraints at
**commit time**. A violation aborts the entire batch: no op in the batch
becomes visible.

---

## Single puts vs. batch transactions

### Single op: `put`

`put` is a convenience wrapper that sends a one-op transaction. Use it when a
write is independent and you do not need atomicity across multiple rows.

```cpp
db.put("orders", {{1, mongreldb::Value::integer(1)},
                  {2, mongreldb::Value::string("Alice")},
                  {3, mongreldb::Value::floating(99.5)}});
```

`upsert`, `del`, and `delete_by_pk` are the same shape: single-op transactions.

### Batch: `commit`

When several writes must succeed or fail together, stage them in a
`std::vector<Op>` and commit once. All ops go to the server in a single HTTP
request and commit atomically.

```cpp
std::vector<mongreldb::Op> ops(3);
ops[0].type = mongreldb::OpType::Put;
ops[0].table = "orders";
ops[0].cells = {{1, mongreldb::Value::integer(10)},
                {2, mongreldb::Value::string("Dave")},
                {3, mongreldb::Value::floating(50.0)}};
ops[1].type = mongreldb::OpType::Put;
ops[1].table = "orders";
ops[1].cells = {{1, mongreldb::Value::integer(11)},
                {2, mongreldb::Value::string("Eve")},
                {3, mongreldb::Value::floating(75.0)}};
ops[2].type = mongreldb::OpType::DeleteByPk;
ops[2].table = "orders";
ops[2].pk_value = mongreldb::Value::integer(2);

db.commit(ops); // atomic - all or nothing
```

`OpType::Upsert` takes an additional `update_cells` vector applied on a
primary-key conflict. An empty `update_cells` means "do nothing on conflict".

## Idempotency keys for safe retries

Networks drop requests and daemons crash after committing but before replying.
An idempotency key makes a commit safe to retry: the daemon remembers the key
and replays the **original** result on a duplicate commit, even across
restarts.

Pass the key as the last argument to `commit` (or `put` / `upsert`):

```cpp
mongreldb::Op op;
op.type = mongreldb::OpType::Put;
op.table = "charges";
op.cells = {{1, mongreldb::Value::string(order_id)},
            {2, mongreldb::Value::floating(199.0)}};

// Use a stable, business-meaningful key derived from the request. On a retry
// with the same key the daemon returns the first commit's result instead of
// inserting a second row.
db.commit({op}, "charge:" + order_id);
```

Rules for keys:

- Any non-empty string works. Prefer content-derived, globally-unique values
  (e.g. `"charge:" + order_id`).
- An empty string disables idempotency - a retry will commit again.
- The key scopes the **entire batch**, not individual ops. Reuse the exact
  same ops and key together when retrying.

## Handling constraint violations

Constraint violations arrive as HTTP 409, thrown as `ConflictException`. It
carries the structured server error `code()` and (when the server reports one)
the offending `op_index()`:

```cpp
try {
    db.commit(ops);
} catch (const mongreldb::ConflictException &e) {
    std::cerr << "constraint " << e.code();
    if (e.op_index()) {
        std::cerr << " at op " << *e.op_index();
    }
    std::cerr << ": " << e.what() << "\n";
    // The engine already rolled back the whole batch. Nothing to undo.
}
```

Structured codes you will commonly see:

| code | Meaning |
|------|---------|
| `UNIQUE_VIOLATION` | A unique/PK constraint rejected the commit |
| `FK_VIOLATION` | A foreign-key reference was missing |
| `CHECK_VIOLATION` | A check constraint or trigger rejected the commit |
| `NOT_FOUND` | A named resource (table, schema) does not exist |

## Rollback

There are two notions of "rollback":

1. **Server-side.** When `commit` throws `ConflictException`, the engine has
   already discarded the entire batch. Nothing was written; there is no server
   rollback to perform.
2. **Client-side.** Because ops are staged in your own `std::vector<Op>`,
   discarding them is just a matter of not calling `commit`. There is no
   transaction handle to roll back - the batch only exists once you send it.

```cpp
std::vector<mongreldb::Op> ops;
// ... stage ops ...
if (!business_rule_ok()) {
    // Don't commit. The daemon has seen nothing.
    return;
}
db.commit(ops);
```

## Summary

| Goal | Use |
|------|-----|
| One independent write | `put` / `upsert` / `del` / `delete_by_pk` |
| Several writes that must commit together | `commit` with an ops vector |
| Retry safely after a network blip | `commit` with a stable idempotency key |
| Distinguish constraint classes | Catch `ConflictException`, read `.code()` |
| Abort before sending | Don't call `commit` - the batch is local |

See [errors.md](errors.md) for the full exception hierarchy and
[queries.md](queries.md) for read patterns.
