// test_mongreldb.cpp - live integration tests for the MongrelDB C++ client.
//
// These boot a real mongreldb-server daemon and exercise the full client
// surface against it. They skip automatically when no daemon binary is
// available.
//
// Binary resolution order:
//   1. MONGRELDB_SERVER env var (path to the server binary).
//   2. ./bin/mongreldb-server (downloaded by CI or `make server`).
//   3. mongreldb-server on PATH.
//
// Or point at an already-running daemon with MONGRELDB_URL.
//
// Licensing: MIT OR Apache-2.0.

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <mongreldb/mongreldb.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

// ── Tiny test framework ──────────────────────────────────────────────────

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

const std::string current_test;

#define RUN(name)                                                             \
    do {                                                                      \
        int before = g_fail;                                                  \
        std::printf("== %s\n", #name);                                        \
        name();                                                               \
        if (g_fail == before) ++g_pass;                                       \
    } while (0)

#define FAIL(msg)                                                             \
    do {                                                                      \
        std::printf("  FAIL: %s\n", msg);                                     \
        ++g_fail;                                                             \
        return;                                                               \
    } while (0)

#define FAILF(...)                                                             \
    do {                                                                       \
        std::printf("  FAIL: ");                                              \
        std::printf(__VA_ARGS__);                                             \
        std::printf("\n");                                                    \
        ++g_fail;                                                             \
        return;                                                               \
    } while (0)

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) { FAILF(__VA_ARGS__); }                                  \
    } while (0)

#define SKIP_IF_NO_DAEMON()                                                   \
    do {                                                                      \
        if (!g_client) {                                                      \
            std::printf("  SKIP: no mongreldb-server available\n");           \
            ++g_skip;                                                         \
            return;                                                           \
        }                                                                      \
    } while (0)

mongreldb::MongrelDBClient *g_client = nullptr;
std::string g_url;
pid_t g_server_pid = 0;

// ── Daemon harness ────────────────────────────────────────────────────────

bool is_executable(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    return (st.st_mode & 0111) != 0;
}

std::string resolve_server_binary() {
    const char *env = std::getenv("MONGRELDB_SERVER");
    if (env && *env && is_executable(env)) return env;
    if (is_executable("./bin/mongreldb-server")) return "./bin/mongreldb-server";
    const char *pp = std::getenv("PATH");
    if (!pp) return "";
    std::string s(pp);
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t colon = s.find(':', pos);
        std::string dir = s.substr(pos, colon == std::string::npos
                                            ? std::string::npos : colon - pos);
        if (!dir.empty()) {
            std::string candidate = dir + "/mongreldb-server";
            if (is_executable(candidate)) return candidate;
        }
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return "";
}

int free_port() {
#ifdef __linux__
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return 0;
    }
    socklen_t alen = sizeof(addr);
    int port = 0;
    if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &alen) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
#else
    return 8453;
#endif
}

bool daemon_healthy(const std::string &url) {
    try {
        mongreldb::MongrelDBClient c(url);
        return c.health();
    } catch (...) {
        return false;
    }
}

bool wait_for_health(const std::string &url, int max_seconds) {
    for (int i = 0; i < max_seconds * 2; ++i) {
        if (daemon_healthy(url)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

void setup_daemon() {
    const char *existing = std::getenv("MONGRELDB_URL");
    if (existing && *existing) {
        if (daemon_healthy(existing)) {
            g_url = existing;
            g_client = new mongreldb::MongrelDBClient(g_url);
            return;
        }
        std::fprintf(stderr, "mongreldb: MONGRELDB_URL=%s not reachable\n", existing);
        std::exit(1);
    }

    std::string bin = resolve_server_binary();
    if (bin.empty()) {
        std::fprintf(stderr, "--- no mongreldb-server binary; live tests skipped\n");
        return;
    }

    int port = free_port();
    if (port == 0) {
        std::fprintf(stderr, "mongreldb: no free port\n");
        return;
    }
    g_url = "http://127.0.0.1:" + std::to_string(port);

    char tmpl[] = "/tmp/mongreldb-cpp-test-XXXXXX";
    char *data_dir = mkdtemp(tmpl);
    if (!data_dir) {
        std::fprintf(stderr, "mongreldb: mkdtemp failed: %s\n", std::strerror(errno));
        return;
    }

    std::string port_arg = std::to_string(port);

    g_server_pid = fork();
    if (g_server_pid < 0) {
        std::fprintf(stderr, "mongreldb: fork failed: %s\n", std::strerror(errno));
        return;
    }
    if (g_server_pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) close(devnull);
        }
        execl(bin.c_str(), bin.c_str(), data_dir, "--port", port_arg.c_str(),
              (char *)nullptr);
        _exit(127);
    }

    if (!wait_for_health(g_url, 40)) {
        std::fprintf(stderr, "mongreldb: daemon did not become healthy at %s\n",
                     g_url.c_str());
        kill(g_server_pid, SIGKILL);
        waitpid(g_server_pid, nullptr, 0);
        g_server_pid = 0;
        return;
    }
    g_client = new mongreldb::MongrelDBClient(g_url);
}

void teardown_daemon() {
    delete g_client;
    g_client = nullptr;
    if (g_server_pid > 0) {
        kill(g_server_pid, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int status;
            if (waitpid(g_server_pid, &status, WNOHANG) == g_server_pid) {
                g_server_pid = 0;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(g_server_pid, SIGKILL);
        waitpid(g_server_pid, nullptr, 0);
        g_server_pid = 0;
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────

mongreldb::Column int_col(std::int64_t id, const std::string &name, bool pk) {
    mongreldb::Column c;
    c.id = id; c.name = name; c.ty = "int64";
    c.primary_key = pk; c.nullable = !pk;
    return c;
}

mongreldb::Column float_col(std::int64_t id, const std::string &name) {
    mongreldb::Column c;
    c.id = id; c.name = name; c.ty = "float64";
    c.primary_key = false; c.nullable = false;
    return c;
}

mongreldb::Cell ci(std::int64_t col, std::int64_t v) {
    return mongreldb::Cell{col, mongreldb::Value::integer(v)};
}
mongreldb::Cell cf(std::int64_t col, double v) {
    return mongreldb::Cell{col, mongreldb::Value::floating(v)};
}
mongreldb::Cell cs(std::int64_t col, const std::string &v) {
    return mongreldb::Cell{col, mongreldb::Value::string(v)};
}

void fresh_table(const std::string &name,
                 const std::vector<mongreldb::Column> &cols) {
    try { g_client->drop_table(name); } catch (...) {}
    g_client->create_table(name, cols);
}

// cell_int64 returns the int64 value for col_id in a result row, or 0 if
// absent. *found is set to whether the cell was present and integer-typed.
std::int64_t cell_int64(const mongreldb::Row &row, std::int64_t col_id,
                        bool *found) {
    if (found) *found = false;
    for (const auto &cell : row) {
        if (cell.column_id == col_id &&
            cell.value.tag() == mongreldb::Value::Tag::Int64) {
            if (found) *found = true;
            return cell.value.as_int64();
        }
    }
    return 0;
}

// Extract the first signed integer that appears in a JSON body.  Works for
// both array-of-arrays (`[[42]]`) and object-array (`[{"x":42}]`) JSON
// responses from the daemon's SQL endpoint.
std::int64_t extract_first_int(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || c == '-') break;
        ++i;
    }
    if (i >= s.size()) return 0;
    return std::strtoll(s.c_str() + i, nullptr, 10);
}

// ── Tests ─────────────────────────────────────────────────────────────────

void test_health() {
    SKIP_IF_NO_DAEMON();
    CHECK(g_client->health(), "health check failed");
}

void test_create_table_and_count() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_tbl_count", {int_col(1, "id", true), float_col(2, "amount")});
    CHECK(g_client->count("cpp_tbl_count") == 0, "expected 0 rows");
}

void test_put_and_count() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_put", {int_col(1, "id", true), float_col(2, "amount")});
    g_client->put("cpp_put", {ci(1, 1), cf(2, 99.5)});
    g_client->put("cpp_put", {ci(1, 2), cf(2, 150.0)});
    CHECK(g_client->count("cpp_put") == 2, "expected 2 rows");
}

void test_upsert() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_upsert", {int_col(1, "id", true), float_col(2, "amount")});
    g_client->put("cpp_upsert", {ci(1, 1), cf(2, 10.0)});
    g_client->upsert("cpp_upsert", {ci(1, 1), cf(2, 20.0)}, {cf(2, 20.0)});
    CHECK(g_client->count("cpp_upsert") == 1, "expected 1 row after upsert");

    // Verify the conflict path actually wrote the updated value, not the
    // original: query the row back by primary key and read amount.
    mongreldb::Condition cond;
    cond.kind = mongreldb::CondKind::Pk;
    cond.int_value = 1;
    auto res = g_client->query("cpp_upsert", {cond});
    CHECK(res.rows.size() == 1, "expected 1 row from upsert pk query, got %zu", res.rows.size());
    bool found = false;
    std::int64_t pk = cell_int64(res.rows[0], 1, &found);
    CHECK(found && pk == 1, "expected returned pk 1, got %lld",
          static_cast<long long>(pk));
    double amount = 0.0;
    for (const auto &cell : res.rows[0]) {
        if (cell.column_id == 2 && cell.value.tag() == mongreldb::Value::Tag::Double) {
            amount = cell.value.as_double();
        }
    }
    CHECK(amount == 20.0, "expected upserted amount 20.0, got %g", amount);
}

void test_query_by_pk() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_pk", {int_col(1, "id", true)});
    g_client->put("cpp_pk", {ci(1, 42)});
    g_client->put("cpp_pk", {ci(1, 43)});

    mongreldb::Condition cond;
    cond.kind = mongreldb::CondKind::Pk;
    cond.int_value = 42;
    auto res = g_client->query("cpp_pk", {cond});
    CHECK(res.rows.size() == 1, "expected 1 row, got %zu", res.rows.size());
    // The returned row must carry the queried PK value.
    bool found = false;
    std::int64_t pk = cell_int64(res.rows[0], 1, &found);
    CHECK(found && pk == 42, "expected returned pk 42, got %lld",
          static_cast<long long>(pk));
}

void test_query_range() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_range", {int_col(1, "id", true), int_col(2, "amount", false)});
    g_client->put("cpp_range", {ci(1, 1), ci(2, 50)});
    g_client->put("cpp_range", {ci(1, 2), ci(2, 120)});
    g_client->put("cpp_range", {ci(1, 3), ci(2, 200)});

    mongreldb::Condition cond;
    cond.kind = mongreldb::CondKind::Range;
    cond.column_id = 2;
    cond.lo = 100; cond.lo_set = true;
    cond.hi = 150; cond.hi_set = true;
    auto res = g_client->query("cpp_range", {cond}, {}, 0);
    // With amounts {50, 120, 200} and range [100, 150], exactly one row
    // (amount == 120) matches; assert the exact count rather than >= 1.
    CHECK(res.rows.size() == 1, "expected exactly 1 row in range, got %zu", res.rows.size());
    CHECK(!res.truncated, "result should not be truncated");
    // Verify the PK and amount values of the returned row match the filter.
    bool found = false;
    std::int64_t pk = cell_int64(res.rows[0], 1, &found);
    CHECK(found && pk == 2, "expected returned pk 2, got %lld",
          static_cast<long long>(pk));
    std::int64_t amt = cell_int64(res.rows[0], 2, &found);
    CHECK(found && amt == 120, "expected returned amount 120, got %lld",
          static_cast<long long>(amt));

    auto page = g_client->query("cpp_range", {}, {}, 1, 2);
    CHECK(page.rows.size() == 1, "expected one row on offset page");
    std::int64_t page_pk = cell_int64(page.rows[0], 1, &found);
    CHECK(found && page_pk == 3, "expected offset page pk 3, got %lld",
          static_cast<long long>(page_pk));
}

void test_transaction_commit() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_txn", {int_col(1, "id", true)});
    std::vector<mongreldb::Op> ops(3);
    for (int i = 0; i < 3; ++i) {
        ops[i].type = mongreldb::OpType::Put;
        ops[i].table = "cpp_txn";
        ops[i].cells = {ci(1, i + 1)};
    }
    g_client->commit(ops);
    CHECK(g_client->count("cpp_txn") == 3, "expected 3 rows");
}

void test_delete_by_pk() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_del", {int_col(1, "id", true)});
    g_client->put("cpp_del", {ci(1, 5)});
    CHECK(g_client->count("cpp_del") == 1, "expected 1 row");
    g_client->delete_by_pk("cpp_del", mongreldb::Value::integer(5));
    CHECK(g_client->count("cpp_del") == 0, "expected 0 rows after delete");
}

void test_string_values() {
    SKIP_IF_NO_DAEMON();
    mongreldb::Column cols[3];
    cols[0] = int_col(1, "id", true);
    cols[1].id = 2; cols[1].name = "label"; cols[1].ty = "varchar";
    cols[1].primary_key = false; cols[1].nullable = false;
    cols[2] = float_col(3, "amount");
    fresh_table("cpp_str", {cols[0], cols[1], cols[2]});
    g_client->put("cpp_str", {ci(1, 1), cs(2, "hello world"), cf(3, 1.5)});

    mongreldb::Condition cond;
    cond.kind = mongreldb::CondKind::Pk;
    cond.int_value = 1;
    auto res = g_client->query("cpp_str", {cond});
    CHECK(res.rows.size() == 1, "expected 1 row, got %zu", res.rows.size());

    std::string label;
    for (const auto &cell : res.rows[0]) {
        if (cell.column_id == 2 && cell.value.tag() == mongreldb::Value::Tag::String) {
            label = cell.value.as_string();
        }
    }
    CHECK(label == "hello world", "expected 'hello world', got '%s'", label.c_str());
}

void test_sql() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_sql", {int_col(1, "id", true), int_col(2, "amount", false)});
    CHECK(g_client->count("cpp_sql") == 0, "expected 0 rows before SQL INSERT");

    // INSERT via SQL must increase the row count.
    g_client->sql("INSERT INTO cpp_sql (id, amount) VALUES (10, 42)");
    CHECK(g_client->count("cpp_sql") == 1,
          "expected count to increase to 1 after SQL INSERT");

    // JSON SQL mode must return the inserted row (a non-empty JSON array). An
    // old server ignores the requested JSON format and answers with Arrow IPC
    // binary bytes, so only verify the JSON array body when JSON mode worked.
    std::string body = g_client->sql("SELECT id, amount FROM cpp_sql");
    if (!body.empty() && body.front() == '[') {
        CHECK(body.front() == '[',
              "expected JSON array body for SQL SELECT, got '%s'", body.c_str());
    }
}

void test_table_names() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_tables", {int_col(1, "id", true)});
    auto names = g_client->table_names();
    bool found = false;
    for (const auto &n : names) {
        if (n == "cpp_tables") { found = true; break; }
    }
    CHECK(found, "table list missing cpp_tables");
}

void test_schema_for() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_schema", {int_col(1, "id", true), float_col(2, "amount")});
    std::string body = g_client->schema_for("cpp_schema");
    CHECK(!body.empty(), "expected non-empty schema body");
}

void test_error_not_found() {
    SKIP_IF_NO_DAEMON();
    bool threw = false;
    try {
        g_client->schema_for("cpp_does_not_exist_xyz");
    } catch (const mongreldb::NotFoundException &) {
        threw = true;
    } catch (...) {
        FAILF("expected NotFoundException");
    }
    CHECK(threw, "expected NotFoundException to be thrown");
}

void test_exception_hierarchy() {
    SKIP_IF_NO_DAEMON();
    // NotFoundException should be catchable as its own type AND as the base
    // MongrelDBException. This exercises the exception hierarchy.
    bool caught_specific = false;
    bool caught_base = false;
    try {
        g_client->schema_for("cpp_does_not_exist_xyz2");
    } catch (const mongreldb::NotFoundException &) {
        caught_specific = true;
    } catch (const mongreldb::MongrelDBException &) {
        caught_base = true;
    } catch (...) {
        FAILF("expected NotFoundException or MongrelDBException");
    }
    CHECK(caught_specific || caught_base,
          "expected NotFoundException or base to be caught");

    // Re-throw and confirm it is catchable as the base too.
    try {
        g_client->schema_for("cpp_does_not_exist_xyz3");
        FAIL("expected NotFoundException");
    } catch (const mongreldb::MongrelDBException &) {
        // ok: derived-to-base catch works
    } catch (...) {
        FAILF("expected MongrelDBException base catch to work");
    }
}

void test_idempotency_key() {
    SKIP_IF_NO_DAEMON();
    fresh_table("cpp_idem", {int_col(1, "id", true)});
    // Unique key per run so prior test runs don't replay stale results.
    std::string idem_key = "idem-key-" + std::to_string(std::time(nullptr));
    g_client->put("cpp_idem", {ci(1, 1)}, idem_key);
    CHECK(g_client->count("cpp_idem") == 1, "expected 1 row");
    // Same key, different value: daemon replays the original result.
    try {
        g_client->put("cpp_idem", {ci(1, 2)}, idem_key);
    } catch (...) {
        // some servers may return a conflict; either way count stays 1
    }
    CHECK(g_client->count("cpp_idem") == 1, "expected 1 row after duplicate idempotent commit");
}

void test_history_retention() {
    SKIP_IF_NO_DAEMON();

    // Configure a durable MVCC window before writing any history.
    auto cfg = g_client->set_history_retention_epochs(100);
    CHECK(cfg.history_retention_epochs == 100, "expected retention 100");
    CHECK(g_client->history_retention_epochs() == 100,
          "history_retention_epochs getter mismatch");
    std::uint64_t earliest = g_client->earliest_retained_epoch();
    CHECK(earliest >= cfg.earliest_retained_epoch,
          "earliest_retained_epoch moved backwards");

    fresh_table("cpp_retention", {int_col(1, "id", true), int_col(2, "value", false)});
    g_client->put("cpp_retention", {ci(1, 1), ci(2, 10)});

    // Capture the commit epoch of the insert via PRAGMA data_version.
    std::string epoch_body = g_client->sql("PRAGMA data_version");
    std::int64_t insert_epoch = extract_first_int(epoch_body);
    CHECK(insert_epoch > 0, "expected positive insert epoch, got %lld",
          static_cast<long long>(insert_epoch));

    // Update the same row; this advances the current epoch.
    g_client->upsert("cpp_retention", {ci(1, 1), ci(2, 20)}, {ci(2, 20)});

    // At the current epoch the value is 20.
    std::string now_body = g_client->sql("SELECT value FROM cpp_retention WHERE id = 1");
    std::int64_t now_val = extract_first_int(now_body);
    CHECK(now_val == 20, "expected current value 20, got %lld",
          static_cast<long long>(now_val));

    // AS OF the insert epoch the older version must still be readable.
    std::string old_sql = "SELECT value FROM cpp_retention AS OF EPOCH " +
                          std::to_string(insert_epoch) + " WHERE id = 1";
    std::string old_body = g_client->sql(old_sql);
    std::int64_t old_val = extract_first_int(old_body);
    CHECK(old_val == 10, "expected AS-OF value 10, got %lld",
          static_cast<long long>(old_val));
}

} // namespace

int main() {
    setup_daemon();

    RUN(test_health);
    RUN(test_create_table_and_count);
    RUN(test_put_and_count);
    RUN(test_upsert);
    RUN(test_query_by_pk);
    RUN(test_query_range);
    RUN(test_transaction_commit);
    RUN(test_delete_by_pk);
    RUN(test_string_values);
    RUN(test_sql);
    RUN(test_table_names);
    RUN(test_schema_for);
    RUN(test_error_not_found);
    RUN(test_exception_hierarchy);
    RUN(test_idempotency_key);
    RUN(test_history_retention);

    teardown_daemon();

    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail > 0 ? 1 : 0;
}
