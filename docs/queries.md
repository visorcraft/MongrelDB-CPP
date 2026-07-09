# Queries

The `query` method pushes conditions down to MongrelDB's native indexes for
sub-millisecond lookups - bitmap, learned-range, FM-index full text, and more.
Each condition type maps to one specialized index; conditions are AND-ed
together.

```cpp
mongreldb::Condition cond;
cond.kind = mongreldb::CondKind::Range;
cond.column_id = 3;
cond.lo = 100.0; cond.lo_set = true;
cond.hi = 500.0; cond.hi_set = true;

auto res = db.query("orders", {cond}, {1, 2}, 100);
```

This guide covers every condition type, projection, limits and truncation, and
combining conditions.

---

## The basics

Every `query` call takes the table, a vector of conditions, a projection, a
limit, and returns a `Result`:

| Argument | Purpose |
|----------|---------|
| `conditions` (default empty) | A vector of native conditions. All are AND-ed. |
| `projection` (default empty) | Return only these column ids (empty means all columns). |
| `limit` (default 0) | Cap the number of rows. |
| return | A `Result` with `rows` and a `truncated` flag. |

The request body the client builds matches the daemon's `/kit/query` shape:

```json
{
  "table": "orders",
  "conditions": [{"range": {"column_id": 3, "lo": 100.0, "hi": 500.0}}],
  "projection": [1, 2],
  "limit": 100
}
```

Result rows and values are returned by value (in `std::vector`), so you can
keep them as long as you like.

## Condition types

Each `Condition` has a `kind` and a set of fields. Column references use the
numeric **column id**, never the column name.

### `CondKind::Pk` - exact primary-key match

The fastest lookup. Supply the primary-key value as `int_value` (for integer
PKs) or `str_value` (for string PKs).

```cpp
mongreldb::Condition cond;
cond.kind = mongreldb::CondKind::Pk;
cond.int_value = 42;
auto res = db.query("orders", {cond});
```

### `CondKind::Range` - numeric range (learned-range index)

Inclusive bounds. Leave `lo_set` / `hi_set` false for an open end.

```cpp
mongreldb::Condition cond;
cond.kind = mongreldb::CondKind::Range;
cond.column_id = 3;
cond.lo = 100.0; cond.lo_set = true;
cond.hi = 500.0; cond.hi_set = true;
auto res = db.query("orders", {cond});

// Open-ended: amount >= 100
mongreldb::Condition open_cond;
open_cond.kind = mongreldb::CondKind::Range;
open_cond.column_id = 3;
open_cond.lo = 100.0; open_cond.lo_set = true;
```

### `CondKind::BitmapEq` - equality on a bitmap-indexed column

Best for low-cardinality columns (status, category, booleans).

```cpp
mongreldb::Condition cond;
cond.kind = mongreldb::CondKind::BitmapEq;
cond.column_id = 2;
cond.str_value = "Alice";
auto res = db.query("orders", {cond});
```

### `CondKind::IsNull` / `CondKind::IsNotNull` - null checks

```cpp
mongreldb::Condition is_null;
is_null.kind = mongreldb::CondKind::IsNull;
is_null.column_id = 3;

mongreldb::Condition not_null;
not_null.kind = mongreldb::CondKind::IsNotNull;
not_null.column_id = 3;
```

### `CondKind::FmContains` - full-text substring search (FM-index)

Substring match within a column. The `str_value` becomes the on-wire `pattern`.

```cpp
mongreldb::Condition cond;
cond.kind = mongreldb::CondKind::FmContains;
cond.column_id = 2;
cond.str_value = "database performance";
auto res = db.query("documents", {cond}, {}, 10);
```

For vector similarity (`ann`), sparse match, and MinHash similarity, use SQL
or extend the condition kinds - the server supports them on the wire; this
client covers the most common index conditions. See [sql.md](sql.md) for the
ones not yet exposed as native helpers.

## Projection (column selection)

Pass a `projection` vector to restrict the columns in each returned row. Pass
an empty vector (or omit) for all columns. Projecting to only the columns you
need cuts bandwidth and decode cost.

```cpp
auto res = db.query("orders", {cond}, {1, 2}); // id and customer only
```

Returned cells are decoded into the `Value` tagged class. Check `value.tag()`
to read the right accessor:

```cpp
for (const auto &row : res.rows) {
    for (const auto &cell : row) {
        switch (cell.value.tag()) {
        case mongreldb::Value::Tag::Int64:
            std::cout << "col " << cell.column_id << " = "
                      << cell.value.as_int64() << "\n";
            break;
        case mongreldb::Value::Tag::Double:
            std::cout << "col " << cell.column_id << " = "
                      << cell.value.as_double() << "\n";
            break;
        case mongreldb::Value::Tag::String:
            std::cout << "col " << cell.column_id << " = "
                      << cell.value.as_string() << "\n";
            break;
        case mongreldb::Value::Tag::Null:
            std::cout << "col " << cell.column_id << " = null\n";
            break;
        default: break;
        }
    }
}
```

## Limit and the truncated flag

A non-zero `limit` caps the result. When the server has more matches than the
limit allows, it returns the first `limit` and sets `truncated` to true.

```cpp
auto res = db.query("orders", {cond}, {}, 100);
if (res.truncated) {
    // 100 rows came back but more exist on the server. Either raise the
    // limit, page with a range predicate on the PK, or accept the cap.
}
```

## Multiple AND conditions

Pass a vector of conditions. Every condition must match; the server intersects
the index results.

```cpp
// Customer is Alice AND amount is between 100 and 500.
mongreldb::Condition conds[2];
conds[0].kind = mongreldb::CondKind::BitmapEq;
conds[0].column_id = 2; conds[0].str_value = "Alice";
conds[1].kind = mongreldb::CondKind::Range;
conds[1].column_id = 3;
conds[1].lo = 100.0; conds[1].lo_set = true;
conds[1].hi = 500.0; conds[1].hi_set = true;

auto res = db.query("orders",
                    {conds[0], conds[1]},
                    {1, 3}, 50);
```

Because each condition targets a different specialized index, the engine can
pick the most selective one to drive the lookup and intersect the rest.

## Putting it together

A realistic combined lookup - bitmap equality + range + projection + limit +
truncation check:

```cpp
mongreldb::Result top_spenders(mongreldb::MongrelDBClient &db,
                               const std::string &customer) {
    mongreldb::Condition conds[2];
    conds[0].kind = mongreldb::CondKind::BitmapEq;
    conds[0].column_id = 2; conds[0].str_value = customer;
    conds[1].kind = mongreldb::CondKind::Range;
    conds[1].column_id = 3;
    conds[1].lo = 100.0; conds[1].lo_set = true;

    auto res = db.query("orders",
                        {conds[0], conds[1]},
                        {1, 3}, 50);
    if (res.truncated) {
        std::cerr << "warning: result capped at 50\n";
    }
    return res;
}
```

For arbitrary predicates, joins, and aggregations that the native indexes do
not cover, use SQL instead - see [sql.md](sql.md).
