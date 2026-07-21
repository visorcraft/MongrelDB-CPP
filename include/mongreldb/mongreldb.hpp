// mongreldb/mongreldb.hpp - Header-only C++17 HTTP client for MongrelDB.
//
// Talks to a running mongreldb-server daemon's JSON API over libcurl. RAII
// wrapper, std::string/std::vector/std::optional, and an exception hierarchy.
// Mirrors the surface of the PHP, Go, and other official clients.
//
// Usage:
//     #include <mongreldb/mongreldb.hpp>
//     mongreldb::MongrelDBClient db("http://127.0.0.1:8453");
//     if (db.health()) { /* ... */ }
//
// Licensing: MIT OR Apache-2.0.
// SPDX-License-Identifier: MIT OR Apache-2.0

#ifndef MONGRELDB_MONGRELDB_HPP
#define MONGRELDB_MONGRELDB_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mongreldb {

// ── Version ──────────────────────────────────────────────────────────────

inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

inline constexpr const char *kDefaultUrl = "http://127.0.0.1:8453";

// Cap on a response body size (256 MB). Bodies larger than this abort the
// transfer and throw a QueryException.
inline constexpr std::size_t kMaxResponseBytes = 268435456;

// ── Exception hierarchy ──────────────────────────────────────────────────
//
// Every non-2xx response (and any client-side failure) throws a subclass of
// MongrelDBException. Catch the base for "anything went wrong", or a specific
// subclass to branch on the category. ResponseException carries the HTTP
// status, the structured server error code, and (for constraint violations)
// the offending op index.

class MongrelDBException : public std::runtime_error {
public:
    explicit MongrelDBException(const std::string &msg)
        : std::runtime_error(msg) {}
};

// 401/403: bad or missing credentials against an auth-enabled daemon.
class AuthException : public MongrelDBException {
public:
    explicit AuthException(const std::string &msg)
        : MongrelDBException(msg) {}
};

// 404: missing table, missing schema, dropped resource.
class NotFoundException : public MongrelDBException {
public:
    explicit NotFoundException(const std::string &msg)
        : MongrelDBException(msg) {}
};

// 409: unique, foreign-key, check, or trigger violation at commit. Carries
// the structured error code and the offending op index when the server
// reports one.
class ConflictException : public MongrelDBException {
public:
    ConflictException(const std::string &msg, std::string code,
                      std::optional<std::size_t> op_index)
        : MongrelDBException(msg),
          code_(std::move(code)),
          op_index_(op_index) {}

    const std::string &code() const noexcept { return code_; }
    std::optional<std::size_t> op_index() const noexcept { return op_index_; }

private:
    std::string code_;
    std::optional<std::size_t> op_index_;
};

// Everything else: malformed request (400), server-side failure (5xx),
// transport failure, or malformed JSON.
class QueryException : public MongrelDBException {
public:
    explicit QueryException(const std::string &msg)
        : MongrelDBException(msg) {}
};

// ── Public types ─────────────────────────────────────────────────────────

// A single cell value in a result row or an input cell. Holds the variant via
// std::string plus flags for bool/int/double/null discrimination, which keeps
// the public ABI simple and copyable.
class Value {
public:
    enum class Tag { Null, Bool, Int64, Double, String, Json };

    Value() : tag_(Tag::Null) {}
    static Value boolean(bool b) {
        Value v; v.tag_ = Tag::Bool; v.b_ = b; return v;
    }
    static Value integer(std::int64_t i) {
        Value v; v.tag_ = Tag::Int64; v.i_ = i; return v;
    }
    static Value floating(double f) {
        Value v; v.tag_ = Tag::Double; v.d_ = f; return v;
    }
    static Value string(std::string s) {
        Value v; v.tag_ = Tag::String; v.s_ = std::move(s); return v;
    }
    /** Raw valid JSON for embeddings, sparse vectors, sets, arrays, or objects. */
    static Value json(std::string s) {
        Value v; v.tag_ = Tag::Json; v.s_ = std::move(s); return v;
    }

    Tag tag() const noexcept { return tag_; }
    bool is_null() const noexcept { return tag_ == Tag::Null; }

    bool as_bool() const { return ensure(Tag::Bool), b_; }
    std::int64_t as_int64() const { return ensure(Tag::Int64), i_; }
    double as_double() const { return ensure(Tag::Double), d_; }
    const std::string &as_string() const { return ensure(Tag::String), s_; }
    const std::string &as_json() const { return ensure(Tag::Json), s_; }

private:
    void ensure(Tag expected) const {
        if (tag_ != expected) {
            throw QueryException("mongreldb: value type mismatch");
        }
    }
    Tag tag_;
    bool b_ = false;
    std::int64_t i_ = 0;
    double d_ = 0.0;
    std::string s_;
};

// A column id paired with its value (used for both input cells and result cells).
struct Cell {
    std::int64_t column_id = 0;
    Value value;
};

// A result row: a vector of cells.
using Row = std::vector<Cell>;

// A result set returned by query().
struct Result {
    std::vector<Row> rows;
    bool truncated = false;
};

struct HistoryRetention {
    std::uint64_t history_retention_epochs = 0;
    std::uint64_t earliest_retained_epoch = 0;
};

// A column definition passed to create_table().
struct Column {
    std::int64_t id = 0;
    std::string name;
    std::string ty;            // "int64", "varchar", "float64", "bool", ...
    bool primary_key = false;
    bool nullable = false;
    // Optional enum-like variant list forwarded to the engine. Empty
    // (default) means the column is not constrained to a fixed variant set
    // and the wire payload omits the "enum_variants" key entirely.
    std::vector<std::string> enum_variants;
    // Optional default-value expression for the column. Unset (default)
    // means no default and the wire payload omits the "default_value"
    // key entirely. This legacy field always sends a JSON string.
    std::optional<std::string> default_value;
    // Raw JSON scalar for static defaults. Caller must provide valid scalar
    // JSON. Takes precedence over the legacy string-only default_value field.
    std::optional<std::string> default_value_json;
    // Dynamic default discriminator: "now" or "uuid". Takes precedence over
    // both default-value fields.
    std::optional<std::string> default_expr;
    // Portable EmbeddingSource JSON. Unset means application-supplied vectors.
    std::optional<std::string> embedding_source_json;
};

enum class IndexKind { Bitmap, Fm, Ann, LearnedRange, MinHash, Sparse };
enum class AnnQuantization { BinarySign, Dense };

struct Index {
    std::string name;
    std::int64_t column_id = 0;
    IndexKind kind = IndexKind::Bitmap;
    std::optional<std::string> predicate;
    std::size_t ann_m = 16;
    std::size_t ann_ef_construction = 64;
    std::size_t ann_ef_search = 64;
    AnnQuantization ann_quantization = AnnQuantization::BinarySign;
    std::size_t minhash_permutations = 128;
    std::size_t minhash_bands = 32;
    std::size_t learned_range_epsilon = 16;
};

// A staged operation in a transaction.
enum class OpType { Put, Upsert, Delete, DeleteByPk };

struct Op {
    OpType type = OpType::Put;
    std::string table;
    std::vector<Cell> cells;            // Put / Upsert
    std::vector<Cell> update_cells;     // Upsert only (empty = DO NOTHING)
    std::int64_t row_id = 0;            // Delete
    Value pk_value;                     // DeleteByPk
};

// A query condition. Maps to a native engine index. Unused fields are ignored
// for a given kind.
enum class CondKind { Pk, BitmapEq, Range, FmContains, IsNull, IsNotNull };

struct Condition {
    CondKind kind = CondKind::Pk;
    std::int64_t column_id = 0;
    // Range bounds (kind == Range). is_*_set controls open-ended ranges.
    double lo = 0.0, hi = 0.0;
    bool lo_set = false, hi_set = false;
    bool lo_inclusive = true, hi_inclusive = true;
    // PK / BitmapEq value or FmContains pattern as a string.
    std::string str_value;
    // PK value as an integer (used when str_value is empty).
    std::int64_t int_value = 0;
    // Complete externally-tagged JsonCondition object for variable-length
    // ANN, sparse, MinHash, bitmap-in, and FM-all queries.
    std::optional<std::string> condition_json;
};

// ── Client ──────────────────────────────────────────────────────────────

class MongrelDBClient {
public:
    // Construct a client for the daemon at url (empty -> kDefaultUrl).
    explicit MongrelDBClient(const std::string &url = "");
    // Bearer token auth (--auth-token mode).
    MongrelDBClient(const std::string &url, const std::string &token);
    // HTTP Basic auth (--auth-users mode). Tag dispatch picks this overload.
    struct BasicAuth {
        std::string username, password;
    };
    MongrelDBClient(const std::string &url, BasicAuth auth);

    ~MongrelDBClient();
    MongrelDBClient(const MongrelDBClient &) = delete;
    MongrelDBClient &operator=(const MongrelDBClient &) = delete;
    MongrelDBClient(MongrelDBClient &&) noexcept;
    MongrelDBClient &operator=(MongrelDBClient &&) noexcept;

    // Per-request timeout in seconds (default 30).
    void set_timeout(long seconds);

    // ── Health & tables ──────────────────────────────────────────────────
    bool health();
    std::vector<std::string> table_names();
    HistoryRetention history_retention();
    HistoryRetention set_history_retention_epochs(std::uint64_t epochs);
    std::uint64_t history_retention_epochs();
    std::uint64_t earliest_retained_epoch();
    std::int64_t create_table(const std::string &name,
                              const std::vector<Column> &columns);
    std::int64_t create_table(const std::string &name,
                              const std::vector<Column> &columns,
                              const std::string &constraints_json);
    std::int64_t create_table(const std::string &name,
                              const std::vector<Column> &columns,
                              const std::string &constraints_json,
                              const std::vector<Index> &indexes);
    void drop_table(const std::string &name);
    std::int64_t count(const std::string &table);

    // ── CRUD (single-op transactions) ───────────────────────────────────
    void put(const std::string &table, const std::vector<Cell> &cells,
             const std::string &idempotency_key = "");
    void upsert(const std::string &table, const std::vector<Cell> &cells,
                const std::vector<Cell> &update_cells = {},
                const std::string &idempotency_key = "");
    void del(const std::string &table, std::int64_t row_id);
    void delete_by_pk(const std::string &table, const Value &pk);

    // ── Batch transactions ──────────────────────────────────────────────
    void commit(const std::vector<Op> &ops,
                const std::string &idempotency_key = "");

    // ── Query ───────────────────────────────────────────────────────────
    Result query(const std::string &table,
                 const std::vector<Condition> &conditions = {},
                 const std::vector<std::int64_t> &projection = {},
                 std::int64_t limit = 0,
                 std::int64_t offset = 0);

    // ── SQL ─────────────────────────────────────────────────────────────
    // Executes a SQL statement. The client sends `format:"json"` with every
    // request. For DDL/DML the daemon replies with a status body; for SELECT it
    // returns a JSON array of rows. Either way this returns the raw body.
    std::string sql(const std::string &statement);

    // ── Schema ──────────────────────────────────────────────────────────
    std::string schema();
    std::string schema_for(const std::string &table);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mongreldb

// The implementation is included inline so the library is header-only.
#include "mongreldb_impl.hpp"

#endif // MONGRELDB_MONGRELDB_HPP
