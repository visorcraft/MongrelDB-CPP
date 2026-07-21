// test_wire_shape.cpp - Offline wire-format conformance tests.
//
// Verifies that the JSON serialization for /kit/create_table columns matches
// the daemon's expected wire shape, without needing a running server.
//
// Licensing: MIT OR Apache-2.0.

#include <mongreldb/mongreldb.hpp>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __linux__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace mongreldb;

namespace {

// ── Tiny single-threaded HTTP server for transport tests ─────────────────
//
// Accepts a fixed number of keep-alive-less connections, captures the
// request line / body, and replies with a pre-canned response.  This lets
// wire-shape tests assert the exact method, path, and body the client
// sends without depending on a real mongreldb-server daemon.

struct CapturedRequest {
    std::string method;
    std::string path;
    std::string body;
};

class MiniHttpServer {
public:
    bool start() {
#ifdef __linux__
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int yes = 1;
        if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        port_ = ntohs(addr.sin_port);

        if (listen(listen_fd_, 8) != 0) {
            close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        thread_ = std::thread([this]() { run(); });
        return true;
#else
        return false;
#endif
    }

    void stop() {
#ifdef __linux__
        stop_ = true;
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
#endif
    }

    int port() const { return port_; }

    std::vector<CapturedRequest> take_requests() {
        std::lock_guard<std::mutex> lock(mu_);
        return std::move(requests_);
    }

    void set_response(int status, const std::string &body) {
        std::lock_guard<std::mutex> lock(mu_);
        response_status_ = status;
        response_body_ = body;
    }

private:
#ifdef __linux__
    void run() {
        while (!stop_) {
            struct sockaddr_in client{};
            socklen_t len = sizeof(client);
            int fd = accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&client), &len);
            if (fd < 0) break;
            handle(fd);
            close(fd);
        }
    }

    void handle(int fd) {
        std::string buf;
        char tmp[4096];
        for (;;) {
            ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
            // HTTP request bodies used here are tiny; stop reading once we
            // have seen the header terminator.
            if (buf.find("\r\n\r\n") != std::string::npos) break;
        }

        CapturedRequest req;
        std::size_t line_end = buf.find("\r\n");
        if (line_end != std::string::npos) {
            std::string line = buf.substr(0, line_end);
            std::size_t first = line.find(' ');
            std::size_t second = line.find(' ', first + 1);
            if (first != std::string::npos && second != std::string::npos) {
                req.method = line.substr(0, first);
                req.path = line.substr(first + 1, second - first - 1);
            }
        }

        std::size_t body_start = buf.find("\r\n\r\n");
        if (body_start != std::string::npos) {
            std::size_t content_length = 0;
            std::size_t cl = buf.find("Content-Length:");
            if (cl != std::string::npos && cl < body_start) {
                std::size_t num_start = buf.find_first_of("0123456789", cl + 15);
                std::size_t num_end = buf.find_first_not_of("0123456789", num_start);
                if (num_start != std::string::npos && num_start < body_start) {
                    content_length = static_cast<std::size_t>(
                        std::strtoull(buf.substr(num_start, num_end - num_start).c_str(), nullptr, 10));
                }
            }
            std::size_t needed = body_start + 4 + content_length;
            while (buf.size() < needed) {
                ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (n == 0) break;
                buf.append(tmp, static_cast<std::size_t>(n));
            }
            req.body = buf.substr(body_start + 4, content_length);
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            requests_.push_back(std::move(req));
        }

        std::string response;
        {
            std::lock_guard<std::mutex> lock(mu_);
            response = "HTTP/1.1 " + std::to_string(response_status_) + " OK\r\n";
            response += "Content-Type: application/json\r\n";
            response += "Content-Length: " + std::to_string(response_body_.size()) + "\r\n";
            response += "Connection: close\r\n\r\n";
            response += response_body_;
        }
        send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
    }
#endif

    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::vector<CapturedRequest> requests_;
    int response_status_ = 200;
    std::string response_body_ = "{}";
};

// Parsed defaults for one column in a create_table wire-shape test.
struct ColumnWireDefaults {
    bool has_default_value = false;
    Value default_value;
    bool has_default_expr = false;
    std::string default_expr;
};

// Decode a create_table JSON body and return a map of column name to its
// default_value/default_expr state.  This lets wire-shape tests inspect the
// actual parsed JSON instead of relying on fragile substring matches.
static std::unordered_map<std::string, ColumnWireDefaults>
extract_column_defaults(const std::string &json) {
    std::unordered_map<std::string, ColumnWireDefaults> result;
    mongreldb::detail::JsonParser j(json);
    if (j.peek() != '{') return result;
    j.expect('{');
    for (;;) {
        if (j.peek() != '"') break;
        std::string key = j.read_string_raw();
        j.expect(':');
        if (key == "columns" && j.peek() == '[') {
            j.expect('[');
            while (j.peek() == '{') {
                j.expect('{');
                ColumnWireDefaults defs;
                std::string name;
                while (j.peek() == '"') {
                    std::string col_key = j.read_string_raw();
                    j.expect(':');
                    if (col_key == "name" && j.peek() == '"') {
                        name = j.read_string_raw();
                    } else if (col_key == "default_value") {
                        defs.has_default_value = true;
                        defs.default_value = j.read_value();
                    } else if (col_key == "default_expr" && j.peek() == '"') {
                        defs.has_default_expr = true;
                        defs.default_expr = j.read_string_raw();
                    } else {
                        j.skip_value();
                    }
                    if (j.peek() == ',') j.expect(',');
                }
                j.expect('}');
                if (!name.empty()) result[name] = defs;
                if (j.peek() == ',') j.expect(',');
            }
            j.expect(']');
            break;
        }
        j.skip_value();
        if (j.peek() == ',') j.expect(',');
        else break;
    }
    return result;
}

} // namespace

#define WS_FAIL(msg)                                                            \
    do {                                                                        \
        std::printf("FAIL: %s\n", msg);                                          \
        return 1;                                                               \
    } while (0)
#define WS_CHECK(cond, msg)                                                     \
    do {                                                                        \
        if (!(cond)) WS_FAIL(msg);                                              \
    } while (0)

int main() {
    {
        std::string json;
        mongreldb::detail::json_value(json, Value::json("[1.0,-2.0]"));
        assert(json == "[1.0,-2.0]");
        printf("PASS: raw embedding JSON value wire shape\n");
    }

    // Test 1: Basic column (no optional fields)
    {
        Column col;
        col.id = 1;
        col.name = "id";
        col.ty = "int64";
        col.primary_key = true;
        col.nullable = false;

        // Serialize and verify wire shape
        // The column struct is serialized via the JSON encoder
        std::string json = mongreldb::detail::serialize_column_json(col);
        assert(json.find("\"id\":1") != std::string::npos);
        assert(json.find("\"name\":\"id\"") != std::string::npos);
        assert(json.find("\"ty\":\"int64\"") != std::string::npos);
        assert(json.find("\"primary_key\":true") != std::string::npos);
        assert(json.find("\"nullable\":false") != std::string::npos);
        assert(json.find("enum_variants") == std::string::npos);
        assert(json.find("default_value") == std::string::npos);
        printf("PASS: basic column wire shape\n");
    }

    // Test 2: Static-default matrix in a single create-table payload.
    // Covers string, number, boolean, explicit null, literal "now",
    // default_expr "now", and the precedence rule (default_expr wins when both
    // are set).  Each shape must round-trip through the hand-rolled serializer
    // without being re-quoted or mangled.  We decode the JSON and inspect the
    // parsed values rather than doing fragile substring searches.
    {
        auto make_col = [](std::int64_t id, const std::string &name,
                           const std::string &ty) {
            Column c;
            c.id = id;
            c.name = name;
            c.ty = ty;
            c.primary_key = false;
            c.nullable = true;
            return c;
        };

        Column c_str = make_col(1, "label", "varchar");
        c_str.default_value_json = "\"text\"";

        Column c_num = make_col(2, "attempts", "int64");
        c_num.default_value_json = "3";

        Column c_bool = make_col(3, "active", "bool");
        c_bool.default_value_json = "true";

        Column c_null = make_col(4, "optional", "varchar");
        c_null.default_value_json = "null";

        Column c_now_literal = make_col(5, "created_at", "timestamp_nanos");
        c_now_literal.default_value_json = "\"now\"";

        Column c_expr = make_col(6, "updated_at", "timestamp_nanos");
        c_expr.default_expr = "now";

        Column c_both = make_col(7, "both", "timestamp_nanos");
        c_both.default_value_json = "\"ignored\"";
        c_both.default_expr = "now";

        std::string json = mongreldb::detail::serialize_create_table_json(
            "defaults", {c_str, c_num, c_bool, c_null, c_now_literal, c_expr, c_both});

        auto defs = extract_column_defaults(json);

        const ColumnWireDefaults &str_def = defs["label"];
        WS_CHECK(str_def.has_default_value && !str_def.has_default_expr,
                 "string default should only set default_value");
        WS_CHECK(str_def.default_value.tag() == Value::Tag::String &&
                 str_def.default_value.as_string() == "text",
                 "string default not encoded as JSON string");

        const ColumnWireDefaults &num_def = defs["attempts"];
        WS_CHECK(num_def.has_default_value,
                 "numeric default missing");
        WS_CHECK(num_def.default_value.tag() == Value::Tag::Int64 &&
                 num_def.default_value.as_int64() == 3,
                 "numeric default not encoded as JSON number");

        const ColumnWireDefaults &bool_def = defs["active"];
        WS_CHECK(bool_def.has_default_value,
                 "boolean default missing");
        WS_CHECK(bool_def.default_value.tag() == Value::Tag::Bool &&
                 bool_def.default_value.as_bool(),
                 "boolean default not encoded as JSON true");

        const ColumnWireDefaults &null_def = defs["optional"];
        WS_CHECK(null_def.has_default_value && null_def.default_value.is_null(),
                 "explicit null default not encoded as JSON null");

        const ColumnWireDefaults &now_lit_def = defs["created_at"];
        WS_CHECK(now_lit_def.has_default_value,
                 "literal now default missing");
        WS_CHECK(now_lit_def.default_value.tag() == Value::Tag::String &&
                 now_lit_def.default_value.as_string() == "now",
                 "literal now default not encoded as JSON string");

        const ColumnWireDefaults &now_expr_def = defs["updated_at"];
        WS_CHECK(!now_expr_def.has_default_value && now_expr_def.has_default_expr,
                 "default_expr column must not also emit default_value");
        WS_CHECK(now_expr_def.default_expr == "now",
                 "default_expr now missing or wrong");

        const ColumnWireDefaults &both_def = defs["both"];
        WS_CHECK(!both_def.has_default_value && both_def.has_default_expr,
                 "when both default_value and default_expr are set, default_expr must win");
        WS_CHECK(both_def.default_expr == "now",
                 "precedence winner default_expr mismatch");

        printf("PASS: static-default matrix wire shape\n");
    }

    // Test 3: /history/retention transport contract.
    // Asserts exact HTTP method, path, request body key, and response-key
    // parsing for both getter methods.
    {
        MiniHttpServer server;
        if (!server.start()) {
            printf("SKIP: /history/retention transport test (no socket support)\n");
        } else {
            // The mock replies with a body that lets us verify both response
            // keys are parsed by the PUT and the two GET methods.
            server.set_response(200,
                "{\"history_retention_epochs\":42,\"earliest_retained_epoch\":1}");
            std::string url = "http://127.0.0.1:" + std::to_string(server.port());
            MongrelDBClient client(url);

            auto put_resp = client.set_history_retention_epochs(42);
            WS_CHECK(put_resp.history_retention_epochs == 42,
                     "PUT response history_retention_epochs mismatch");
            WS_CHECK(put_resp.earliest_retained_epoch == 1,
                     "PUT response earliest_retained_epoch mismatch");

            std::uint64_t epochs = client.history_retention_epochs();
            WS_CHECK(epochs == 42, "GET history_retention_epochs mismatch");

            std::uint64_t earliest = client.earliest_retained_epoch();
            WS_CHECK(earliest == 1, "GET earliest_retained_epoch mismatch");

            auto reqs = server.take_requests();
            WS_CHECK(reqs.size() == 3, "expected 3 captured requests");

            WS_CHECK(reqs[0].method == "PUT", "expected PUT method");
            WS_CHECK(reqs[0].path == "/history/retention", "expected /history/retention path");
            WS_CHECK(reqs[0].body == "{\"history_retention_epochs\":42}",
                     "PUT body must be the exact JSON object");

            WS_CHECK(reqs[1].method == "GET", "expected first GET method");
            WS_CHECK(reqs[1].path == "/history/retention",
                     "expected first GET /history/retention path");
            WS_CHECK(reqs[1].body.empty(), "GET body should be empty");

            WS_CHECK(reqs[2].method == "GET", "expected second GET method");
            WS_CHECK(reqs[2].path == "/history/retention",
                     "expected second GET /history/retention path");
            WS_CHECK(reqs[2].body.empty(), "GET body should be empty");

            server.stop();
            printf("PASS: /history/retention transport contract\n");
        }
    }

    // Test 4: Column with enum_variants
    {
        Column col;
        col.id = 2;
        col.name = "status";
        col.ty = "varchar";
        col.primary_key = false;
        col.nullable = false;
        col.enum_variants = {"active", "inactive", "pending"};

        std::string json = mongreldb::detail::serialize_column_json(col);
        assert(json.find("\"enum_variants\":[\"active\",\"inactive\",\"pending\"]") != std::string::npos);
        assert(json.find("default_value") == std::string::npos);
        printf("PASS: enum_variants wire shape\n");
    }

    // Test 5: Column with default_value
    {
        Column col;
        col.id = 3;
        col.name = "score";
        col.ty = "float64";
        col.primary_key = false;
        col.nullable = true;
        col.default_value = "0.0";

        std::string json = mongreldb::detail::serialize_column_json(col);
        assert(json.find("\"default_value\":\"0.0\"") != std::string::npos);
        assert(json.find("enum_variants") == std::string::npos);
        printf("PASS: default_value wire shape\n");
    }

    // Test 6: top-level CHECK constraints.
    {
        Column col{1, "score", "int64", false, false, {}, std::nullopt,
                   std::nullopt, std::nullopt, std::nullopt};
        std::string constraints =
            R"({"checks":[{"id":1,"name":"score_nonneg","expr":{"Ge":[{"Col":1},{"Lit":{"Int64":0}}]}}]})";
        std::string json = mongreldb::detail::serialize_create_table_json(
            "scores", {col}, constraints);
        assert(json.find("\"constraints\":{\"checks\":[") != std::string::npos);
        assert(json.find("\"name\":\"score_nonneg\"") != std::string::npos);
        printf("PASS: CHECK constraints wire shape\n");
    }

    // Test 7: all public index kinds, Dense ANN options, and model source.
    {
        Column id;
        id.id = 1;
        id.name = "id";
        id.ty = "int64";
        id.primary_key = true;
        Column embedding;
        embedding.id = 2;
        embedding.name = "embedding";
        embedding.ty = "embedding(384)";
        embedding.embedding_source_json =
            R"({"kind":"configured_model","provider_id":"docs","model_id":"model","model_version":"1"})";

        std::vector<Index> indexes;
        for (const auto kind : {IndexKind::Bitmap, IndexKind::Fm, IndexKind::Ann,
                                IndexKind::LearnedRange, IndexKind::MinHash,
                                IndexKind::Sparse}) {
            Index index;
            index.name = "idx_" + std::to_string(indexes.size());
            index.column_id = kind == IndexKind::Ann ? 2 : 1;
            index.kind = kind;
            if (kind == IndexKind::Ann) {
                index.predicate = "embedding IS NOT NULL";
                index.ann_m = 24;
                index.ann_ef_construction = 96;
                index.ann_ef_search = 48;
                index.ann_quantization = AnnQuantization::Dense;
            }
            if (kind == IndexKind::LearnedRange) index.learned_range_epsilon = 8;
            if (kind == IndexKind::MinHash) {
                index.minhash_permutations = 64;
                index.minhash_bands = 16;
            }
            indexes.push_back(index);
        }
        std::string json = mongreldb::detail::serialize_create_table_json(
            "search_docs", {id, embedding}, "", indexes);
        assert(json.find("\"embedding_source\":{\"kind\":\"configured_model\"") != std::string::npos);
        assert(json.find("\"kind\":\"bitmap\"") != std::string::npos);
        assert(json.find("\"kind\":\"fm_index\"") != std::string::npos);
        assert(json.find("\"kind\":\"ann\"") != std::string::npos);
        assert(json.find("\"quantization\":\"dense\"") != std::string::npos);
        assert(json.find("\"m\":24") != std::string::npos);
        assert(json.find("\"predicate\":\"embedding IS NOT NULL\"") != std::string::npos);
        assert(json.find("\"kind\":\"learned_range\"") != std::string::npos);
        assert(json.find("\"epsilon\":8") != std::string::npos);
        assert(json.find("\"kind\":\"minhash\"") != std::string::npos);
        assert(json.find("\"permutations\":64") != std::string::npos);
        assert(json.find("\"kind\":\"sparse\"") != std::string::npos);
        printf("PASS: all index kinds and embedding source wire shape\n");
    }

    // Test 8: error propagation — a non-2xx response surfaces as the client's
    // typed QueryException, not a silent success.
    {
        MiniHttpServer server;
        if (!server.start()) {
            printf("SKIP: error propagation test (no socket support)\n");
        } else {
            server.set_response(503,
                "{\"message\":\"server overloaded\",\"code\":\"UNAVAILABLE\"}");
            std::string url = "http://127.0.0.1:" + std::to_string(server.port());
            MongrelDBClient client(url);

            // GET path: history_retention_epochs() must throw.
            bool threw = false;
            try {
                (void)client.history_retention_epochs();
            } catch (const QueryException &) {
                threw = true;
            }
            WS_CHECK(threw,
                     "503 on GET /history/retention must throw QueryException");

            // PUT path: set_history_retention_epochs() must also throw.
            threw = false;
            try {
                (void)client.set_history_retention_epochs(99);
            } catch (const QueryException &) {
                threw = true;
            }
            WS_CHECK(threw,
                     "503 on PUT /history/retention must throw QueryException");

            server.stop();
            printf("PASS: error propagation\n");
        }
    }

    // Test 9: swappable ANN algorithms (DiskANN / IVF) and product
    // quantization serialize to the expected JSON wire shape. Mirrors the C
    // client's test_wire_shape.c Test 13 so both bindings emit byte-identical
    // /kit/create_table index payloads.
    {
        Index diskann;
        diskann.name = "ann_diskann";
        diskann.column_id = 2;
        diskann.kind = IndexKind::Ann;
        diskann.ann_quantization = AnnQuantization::Dense;
        diskann.ann_algorithm = AnnAlgorithm::Diskann;
        diskann.diskann_r = 128;
        diskann.diskann_l = 256;
        diskann.diskann_beam_width = 4;
        diskann.diskann_alpha = 130;
        std::string json = mongreldb::detail::serialize_index_json(diskann);
        assert(json.find("\"algorithm\":\"diskann\"") != std::string::npos);
        assert(json.find("\"quantization\":\"dense\"") != std::string::npos);
        assert(json.find("\"diskann\":{\"r\":128") != std::string::npos);
        assert(json.find("\"l\":256") != std::string::npos);
        assert(json.find("\"beam_width\":4") != std::string::npos);
        assert(json.find("\"alpha\":130") != std::string::npos);

        Index ivf;
        ivf.name = "ann_ivf";
        ivf.column_id = 2;
        ivf.kind = IndexKind::Ann;
        ivf.ann_quantization = AnnQuantization::Dense;
        ivf.ann_algorithm = AnnAlgorithm::Ivf;
        ivf.ivf_nlist = 512;
        ivf.ivf_nprobe = 16;
        json = mongreldb::detail::serialize_index_json(ivf);
        assert(json.find("\"algorithm\":\"ivf\"") != std::string::npos);
        assert(json.find("\"ivf\":{\"nlist\":512,\"nprobe\":16}") != std::string::npos);

        Index pq;
        pq.name = "ann_pq";
        pq.column_id = 2;
        pq.kind = IndexKind::Ann;
        pq.ann_quantization = AnnQuantization::Product;
        pq.pq_num_subvectors = 32;
        pq.pq_bits = 8;
        pq.pq_training_samples = 10000;
        pq.pq_seed = 42;
        pq.pq_rerank_factor = 3;
        json = mongreldb::detail::serialize_index_json(pq);
        assert(json.find("\"quantization\":\"product\"") != std::string::npos);
        assert(json.find("\"product\":{\"training_samples\":10000") != std::string::npos);
        assert(json.find("\"seed\":42") != std::string::npos);
        assert(json.find("\"rerank_factor\":3") != std::string::npos);
        /* Default algorithm (HNSW) is omitted to preserve wire shape. */
        assert(json.find("\"algorithm\":") == std::string::npos);

        printf("PASS: swappable ANN algorithm and product-quantization wire shape\n");
    }

    printf("All wire-shape tests passed.\n");
    return 0;
}
