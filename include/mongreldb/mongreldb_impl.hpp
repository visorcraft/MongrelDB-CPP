// mongreldb/mongreldb_impl.hpp - Implementation for the header-only C++17 client.
//
// Included by mongreldb.hpp; do not include directly. Uses libcurl for HTTP
// and a minimal hand-rolled JSON parser/serializer, so there are no external
// dependencies beyond libc and libcurl.
//
// Licensing: MIT OR Apache-2.0.

#ifndef MONGRELDB_MONGRELDB_IMPL_HPP
#define MONGRELDB_MONGRELDB_IMPL_HPP

#include "mongreldb.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace mongreldb {

// ── JSON serialization helpers ──────────────────────────────────────────
//
// These live in `detail` (not an anonymous namespace) so that wire-shape
// conformance tests can call the column-serialization free function
// directly without spinning up a daemon. The client implementation below
// remains the only in-tree caller of every other helper here.

namespace detail {

// All helpers in this namespace are `inline` because the implementation
// header is included by more than one translation unit when a consumer
// compiles the header into their own TU alongside `src/mongreldb.cpp` (see
// the `examples/` build instructions). With `inline`, the linker dedupes
// the copies across TUs; without it, multiple-definition errors fire.
inline void json_escape(std::string &out, const std::string &s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

inline void json_value(std::string &out, const Value &v) {
    switch (v.tag()) {
        case Value::Tag::Null:   out += "null"; break;
        case Value::Tag::Bool:   out += v.as_bool() ? "true" : "false"; break;
        case Value::Tag::Int64: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(v.as_int64()));
            out += buf;
            break;
        }
        case Value::Tag::Double: {
            double d = v.as_double();
            /* NaN and Infinity have no valid JSON representation; emit null. */
            if (std::isnan(d) || std::isinf(d)) {
                out += "null";
            } else {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.17g", d);
                out += buf;
            }
            break;
        }
        case Value::Tag::String: json_escape(out, v.as_string()); break;
    }
}

inline void json_cells(std::string &out, const std::vector<Cell> &cells) {
    out.push_back('[');
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) out.push_back(',');
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(cells[i].column_id));
        out += buf;
        out.push_back(',');
        json_value(out, cells[i].value);
    }
    out.push_back(']');
}

// ── Minimal JSON parser ─────────────────────────────────────────────────
//
// A recursive-descent walker over the response body. Used to pull out the
// scalar fields the client cares about (table_id, count, truncated, error
// envelopes) and to decode the query "rows" array.

class JsonParser {
public:
    JsonParser(const std::string &s) : s_(s), i_(0), ok_(true) {}

    bool ok() const { return ok_; }

    std::size_t cursor() const { return i_; }

    void skip_ws() {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' ||
                s_[i_] == '\r')) {
            ++i_;
        }
    }

    int peek() {
        skip_ws();
        return i_ < s_.size() ? static_cast<unsigned char>(s_[i_]) : -1;
    }

    void expect(char ch) {
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ch) {
            ++i_;
            return;
        }
        ok_ = false;
    }

    std::string read_string_raw() {
        // Returns the unescaped contents of a "..."'d string. Assumes peek()
        // already returned '"'.
        std::string out;
        ++i_; // skip opening quote
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\' && i_ < s_.size()) {
                char n = s_[i_++];
                switch (n) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (i_ + 4 <= s_.size()) {
                            unsigned cp = 0;
                            for (int k = 0; k < 4; ++k) {
                                char h = s_[i_++];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= h - '0';
                                else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                                else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                            }
                            // Handle UTF-16 surrogate pairs. A high surrogate
                            // (0xD800-0xDBFF) must be followed by a low
                            // surrogate (0xDC00-0xDFFF) in the next \uXXXX;
                            // combine them into a codepoint above 0xFFFF.
                            if (cp >= 0xD800 && cp <= 0xDBFF) {
                                if (i_ + 6 <= s_.size() && s_[i_] == '\\'
                                    && s_[i_ + 1] == 'u') {
                                    unsigned lo = 0;
                                    std::size_t j = i_ + 2;
                                    bool hex_ok = true;
                                    for (int k = 0; k < 4; ++k) {
                                        char h = s_[j++];
                                        lo <<= 4;
                                        if (h >= '0' && h <= '9') lo |= h - '0';
                                        else if (h >= 'a' && h <= 'f') lo |= h - 'a' + 10;
                                        else if (h >= 'A' && h <= 'F') lo |= h - 'A' + 10;
                                        else { hex_ok = false; break; }
                                    }
                                    if (hex_ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                        cp = 0x10000 + ((cp - 0xD800) << 10)
                                             + (lo - 0xDC00);
                                        i_ = j;
                                    } else {
                                        cp = 0xFFFD;  // lone high surrogate
                                    }
                                } else {
                                    cp = 0xFFFD;      // lone high surrogate
                                }
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                cp = 0xFFFD;          // lone low surrogate
                            }
                            // UTF-8 encode the codepoint.
                            if (cp < 0x80) {
                                out.push_back(static_cast<char>(cp));
                            } else if (cp < 0x800) {
                                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else if (cp < 0x10000) {
                                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            } else {
                                out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                            }
                        }
                        break;
                    default: out.push_back(n); break;
                }
            } else {
                out.push_back(c);
            }
        }
        ok_ = false;
        return out;
    }

    std::string read_number_raw() {
        std::size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' ||
                c == '.' || c == 'e' || c == 'E') {
                ++i_;
            } else {
                break;
            }
        }
        return s_.substr(start, i_ - start);
    }

    // Skip a whole value (object/array/string/number/literal).
    void skip_value() {
        int ch = peek();
        if (ch == '"') {
            (void)read_string_raw();
        } else if (ch == '{') {
            ++i_;
            skip_ws();
            if (peek() == '}') { ++i_; return; }
            for (;;) {
                if (peek() != '"') { ok_ = false; return; }
                (void)read_string_raw();
                expect(':');
                skip_value();
                if (!ok_) return;
                int n = peek();
                if (n == ',') { ++i_; continue; }
                if (n == '}') { ++i_; return; }
                ok_ = false; return;
            }
        } else if (ch == '[') {
            ++i_;
            skip_ws();
            if (peek() == ']') { ++i_; return; }
            for (;;) {
                skip_value();
                if (!ok_) return;
                int n = peek();
                if (n == ',') { ++i_; continue; }
                if (n == ']') { ++i_; return; }
                ok_ = false; return;
            }
        } else if (ch == 't' || ch == 'f' || ch == 'n') {
            while (i_ < s_.size()) {
                char c = s_[i_];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
                    ++i_;
                } else {
                    break;
                }
            }
        } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
            (void)read_number_raw();
        } else {
            ok_ = false;
        }
    }

    // Read the value at the cursor into a Value.
    Value read_value() {
        int ch = peek();
        if (ch == '"') {
            return Value::string(read_string_raw());
        }
        if (ch == '{' || ch == '[') {
            skip_value();
            return Value{}; // null for composite values (not used for cells)
        }
        if (ch == 't' || ch == 'f') {
            std::size_t start = i_;
            while (i_ < s_.size() && ((s_[i_] >= 'a' && s_[i_] <= 'z') ||
                                      (s_[i_] >= 'A' && s_[i_] <= 'Z'))) {
                ++i_;
            }
            std::string lit = s_.substr(start, i_ - start);
            return Value::boolean(lit == "true");
        }
        if (ch == 'n') {
            while (i_ < s_.size() && (s_[i_] >= 'a' && s_[i_] <= 'z')) ++i_;
            return Value{};
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            std::string num = read_number_raw();
            bool is_int = num.find_first_of(".eE") == std::string::npos;
            if (is_int) {
                return Value::integer(
                    static_cast<std::int64_t>(std::strtoll(num.c_str(), nullptr, 10)));
            }
            return Value::floating(std::strtod(num.c_str(), nullptr));
        }
        ok_ = false;
        return Value{};
    }

private:
    const std::string &s_;
    std::size_t i_;
    bool ok_;
};

// Extract a scalar field from a top-level object. Returns true on success.
inline bool get_string(const std::string &body, const std::string &key,
                std::string &out) {
    JsonParser j(body);
    if (j.peek() != '{') return false;
    j.expect('{');
    if (j.peek() == '}') return false;
    for (;;) {
        if (j.peek() != '"') return false;
        std::string k = j.read_string_raw();
        if (!j.ok()) return false;
        j.expect(':');
        if (!j.ok()) return false;
        if (k == key && j.peek() == '"') {
            out = j.read_string_raw();
            return j.ok();
        }
        j.skip_value();
        if (!j.ok()) return false;
        int ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        return ch == '}';
    }
}

inline bool get_number(const std::string &body, const std::string &key, double &out) {
    JsonParser j(body);
    if (j.peek() != '{') return false;
    j.expect('{');
    if (j.peek() == '}') return false;
    for (;;) {
        if (j.peek() != '"') return false;
        std::string k = j.read_string_raw();
        if (!j.ok()) return false;
        j.expect(':');
        if (!j.ok()) return false;
        int ch = j.peek();
        if (k == key && (ch == '-' || (ch >= '0' && ch <= '9'))) {
            out = std::strtod(j.read_number_raw().c_str(), nullptr);
            return j.ok();
        }
        j.skip_value();
        if (!j.ok()) return false;
        ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        return ch == '}';
    }
}

inline bool get_int(const std::string &body, const std::string &key, std::int64_t &out) {
    double d = 0;
    if (!get_number(body, key, d)) return false;
    out = static_cast<std::int64_t>(d);
    return true;
}

inline bool get_uint64(const std::string &body, const std::string &key, std::uint64_t &out) {
    JsonParser j(body);
    if (j.peek() != '{') return false;
    j.expect('{');
    if (j.peek() == '}') return false;
    for (;;) {
        if (j.peek() != '"') return false;
        std::string k = j.read_string_raw();
        if (!j.ok()) return false;
        j.expect(':');
        if (!j.ok()) return false;
        int ch = j.peek();
        if (k == key && (ch >= '0' && ch <= '9')) {
            std::string raw = j.read_number_raw();
            if (!j.ok()) return false;
            char *end = nullptr;
            unsigned long long v = std::strtoull(raw.c_str(), &end, 10);
            if (end != raw.c_str() + raw.size()) return false;
            out = static_cast<std::uint64_t>(v);
            return true;
        }
        j.skip_value();
        if (!j.ok()) return false;
        ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        return ch == '}';
    }
}

inline bool get_bool(const std::string &body, const std::string &key, bool &out) {
    JsonParser j(body);
    if (j.peek() != '{') return false;
    j.expect('{');
    if (j.peek() == '}') return false;
    for (;;) {
        if (j.peek() != '"') return false;
        std::string k = j.read_string_raw();
        if (!j.ok()) return false;
        j.expect(':');
        if (!j.ok()) return false;
        int ch = j.peek();
        if (k == key && (ch == 't' || ch == 'f')) {
            Value v = j.read_value();
            out = v.as_bool();
            return j.ok();
        }
        j.skip_value();
        if (!j.ok()) return false;
        ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        return ch == '}';
    }
}

// ── HTTP plumbing ───────────────────────────────────────────────────────

inline size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    std::size_t n = size * nmemb;
    /* Cap the download: abort once the buffered body would exceed the limit so
     * an oversized response is not buffered fully. Returning a short count
     * signals an error to curl_easy_perform. */
    if (out->size() > kMaxResponseBytes ||
        n > kMaxResponseBytes - out->size()) {
        return 0;
    }
    try {
        out->append(ptr, n);
    } catch (...) {
        /* std::string::append can throw (bad_alloc) and libcurl callbacks
         * must be noexcept; returning a short count signals an error to
         * curl_easy_perform, which we then report as a network failure. */
        return 0;
    }
    return n;
}

inline std::string url_encode_segment(const std::string &seg) {
    std::string out;
    for (unsigned char c : seg) {
        /* '/' must be percent-encoded so a table name like "a/b" cannot
         * inject an extra path segment. Only unreserved chars (RFC 3986)
         * pass through unencoded. */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Serialize a Column as a single JSON object body suitable for embedding in
// a `/kit/create_table` payload. Includes `enum_variants` when the column
// has any variants and `default_value` when one is set; otherwise those
// keys are omitted so the wire shape matches an old client. Exposed for the
// wire-shape conformance test; the client implementation below is the only
// in-tree caller.
inline std::string serialize_column_json(const Column &col) {
    std::string s = "{\"id\":";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld",
                  static_cast<long long>(col.id));
    s += buf;
    s += ",\"name\":";
    json_escape(s, col.name);
    s += ",\"ty\":";
    json_escape(s, col.ty);
    s += ",\"primary_key\":";
    s += col.primary_key ? "true" : "false";
    s += ",\"nullable\":";
    s += col.nullable ? "true" : "false";
    if (!col.enum_variants.empty()) {
        s += ",\"enum_variants\":[";
        for (std::size_t i = 0; i < col.enum_variants.size(); ++i) {
            if (i > 0) s.push_back(',');
            json_escape(s, col.enum_variants[i]);
        }
        s += "]";
    }
    if (col.default_expr.has_value()) {
        s += ",\"default_expr\":";
        json_escape(s, *col.default_expr);
    } else if (col.default_value_json.has_value()) {
        s += ",\"default_value\":";
        s += *col.default_value_json;
    } else if (col.default_value.has_value()) {
        s += ",\"default_value\":";
        json_escape(s, *col.default_value);
    }
    s.push_back('}');
    return s;
}

inline std::string serialize_create_table_json(
        const std::string &name, const std::vector<Column> &columns,
        const std::string &constraints_json = "") {
    std::string body = "{\"name\":";
    json_escape(body, name);
    body += ",\"columns\":[";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i > 0) body.push_back(',');
        body += serialize_column_json(columns[i]);
    }
    body.push_back(']');
    if (!constraints_json.empty()) {
        body += ",\"constraints\":";
        body += constraints_json;
    }
    body.push_back('}');
    return body;
}

} // namespace detail

// The client implementation below is the only in-tree consumer of the JSON
// helpers above (apart from the wire-shape test, which calls
// serialize_column_json directly). Bringing them in here keeps the
// pre-refactor call sites unqualified.
using namespace detail;

// ── Client implementation ────────────────────────────────────────────────

struct MongrelDBClient::Impl {
    std::string base_url;
    std::string token;
    std::string username;
    std::string password;
    long timeout_seconds = 30;

    // Perform one HTTP request and return the response body. Throws on HTTP
    // error or transport failure.
    std::string request(const std::string &method, const std::string &path,
                        const std::string *body) {
        std::string url = base_url;
        if (path.empty() || path[0] != '/') url.push_back('/');
        url += path;

        CURL *curl = curl_easy_init();
        if (!curl) {
            throw QueryException("mongreldb: curl_easy_init failed");
        }

        std::string response;
        struct curl_slist *headers = nullptr;

        try {
            headers = curl_slist_append(headers, "Accept: application/json");
            if (!headers) throw std::bad_alloc();
            if (body) {
                headers = curl_slist_append(headers,
                                            "Content-Type: application/json");
                if (!headers) throw std::bad_alloc();
            }

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            /* Cap the response body at 256 MB. The write callback also
             * enforces this for chunked transfers where the size is unknown. */
            curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                             static_cast<curl_off_t>(kMaxResponseBytes));

            if (body) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                                 static_cast<long>(body->size()));
            }

            if (!token.empty()) {
                std::string auth_header = "Authorization: Bearer " + token;
                headers = curl_slist_append(headers, auth_header.c_str());
                if (!headers) throw std::bad_alloc();
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            } else if (!username.empty()) {
                std::string userpwd = username + ":" + password;
                curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
            }
        } catch (...) {
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            throw;
        }

        CURLcode rc = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        /* An oversized body aborts the transfer (CURLE_FILESIZE_EXCEEDED from
         * CURLOPT_MAXFILESIZE_LARGE, or CURLE_WRITE_ERROR from the write
         * callback) and is reported as a QueryException. */
        if (rc == CURLE_FILESIZE_EXCEEDED || rc == CURLE_WRITE_ERROR ||
            response.size() > kMaxResponseBytes) {
            throw QueryException("mongreldb: response body exceeds " +
                                 std::to_string(kMaxResponseBytes) + " bytes");
        }

        if (rc != CURLE_OK) {
            std::string msg = "mongreldb: network error: ";
            msg += curl_easy_strerror(rc);
            throw QueryException(msg);
        }

        if (http_code < 200 || http_code >= 300) {
            // Decode the error envelope.
            std::string message, code;
            std::optional<std::size_t> op_index;
            if (!get_string(response, "message", message)) {
                message = response;
            }
            get_string(response, "code", code);
            if (message.rfind("not found:", 0) == 0) {
                throw NotFoundException(message);
            }
            {
                double oi = -1;
                JsonParser j(response);
                // The op_index lives under error.op_index; do a best-effort
                // extraction by scanning for the key anywhere at depth 1.
                if (get_number(response, "op_index", oi) && oi >= 0) {
                    op_index = static_cast<std::size_t>(oi);
                }
            }
            switch (http_code) {
                case 401: case 403:
                    throw AuthException(message.empty()
                        ? "authentication failed" : message);
                case 404:
                    throw NotFoundException(message.empty()
                        ? "resource not found" : message);
                case 409:
                    throw ConflictException(message.empty()
                        ? "constraint violation" : message, code, op_index);
                default:
                    throw QueryException(message.empty()
                        ? "mongreldb: server error" : message);
            }
        }

        return response;
    }

    std::string get(const std::string &path) {
        return request("GET", path, nullptr);
    }
    std::string post(const std::string &path, const std::string &body) {
        return request("POST", path, &body);
    }
    std::string put(const std::string &path, const std::string &body) {
        return request("PUT", path, &body);
    }
    std::string del(const std::string &path) {
        return request("DELETE", path, nullptr);
    }
};

inline MongrelDBClient::MongrelDBClient(const std::string &url)
    : impl_(std::make_unique<Impl>()) {
    impl_->base_url = url.empty() ? kDefaultUrl : url;
    while (!impl_->base_url.empty() &&
           impl_->base_url.back() == '/') {
        impl_->base_url.pop_back();
    }
}

inline MongrelDBClient::MongrelDBClient(const std::string &url,
                                 const std::string &token)
    : MongrelDBClient(url) {
    impl_->token = token;
}

inline MongrelDBClient::MongrelDBClient(const std::string &url, BasicAuth auth)
    : MongrelDBClient(url) {
    impl_->username = std::move(auth.username);
    impl_->password = std::move(auth.password);
}

inline MongrelDBClient::~MongrelDBClient() = default;

inline MongrelDBClient::MongrelDBClient(MongrelDBClient &&) noexcept = default;
inline MongrelDBClient &MongrelDBClient::operator=(MongrelDBClient &&) noexcept = default;

inline void MongrelDBClient::set_timeout(long seconds) {
    impl_->timeout_seconds = seconds > 0 ? seconds : 30;
}

inline bool MongrelDBClient::health() {
    try {
        impl_->get("/health");
        return true;
    } catch (const QueryException &) {
        return false;
    }
}

inline std::vector<std::string> MongrelDBClient::table_names() {
    std::string body = impl_->get("/tables");
    std::vector<std::string> names;
    JsonParser j(body);
    if (j.peek() != '[') return names;
    j.expect('[');
    if (j.peek() == ']') return names;
    for (;;) {
        if (j.peek() != '"') {
            throw QueryException("mongreldb: malformed table list");
        }
        names.push_back(j.read_string_raw());
        if (!j.ok()) {
            throw QueryException("mongreldb: malformed table list");
        }
        int ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        if (ch == ']') { j.expect(']'); break; }
        throw QueryException("mongreldb: malformed table list");
    }
    return names;
}

inline std::int64_t MongrelDBClient::create_table(const std::string &name,
                                           const std::vector<Column> &columns) {
    return create_table(name, columns, "");
}

inline HistoryRetention decode_history_retention(const std::string &json) {
    std::uint64_t epochs = 0, earliest = 0;
    if (!detail::get_uint64(json, "history_retention_epochs", epochs) ||
        !detail::get_uint64(json, "earliest_retained_epoch", earliest)) {
        throw QueryException("mongreldb: malformed history retention response");
    }
    return {epochs, earliest};
}

inline HistoryRetention MongrelDBClient::history_retention() {
    return decode_history_retention(impl_->get("/history/retention"));
}

inline HistoryRetention MongrelDBClient::set_history_retention_epochs(std::uint64_t epochs) {
    return decode_history_retention(impl_->put("/history/retention",
        "{\"history_retention_epochs\":" + std::to_string(epochs) + "}"));
}

inline std::uint64_t MongrelDBClient::history_retention_epochs() {
    return history_retention().history_retention_epochs;
}

inline std::uint64_t MongrelDBClient::earliest_retained_epoch() {
    return history_retention().earliest_retained_epoch;
}

inline std::int64_t MongrelDBClient::create_table(
        const std::string &name, const std::vector<Column> &columns,
        const std::string &constraints_json) {
    std::string body = detail::serialize_create_table_json(
        name, columns, constraints_json);
    std::string resp = impl_->post("/kit/create_table", body);
    std::int64_t tid = 0;
    get_int(resp, "table_id", tid);
    return tid;
}

inline void MongrelDBClient::drop_table(const std::string &name) {
    impl_->del("/tables/" + url_encode_segment(name));
}

inline std::int64_t MongrelDBClient::count(const std::string &table) {
    std::string resp = impl_->get("/tables/" +
                                  url_encode_segment(table) + "/count");
    std::int64_t n = 0;
    get_int(resp, "count", n);
    return n;
}

inline void MongrelDBClient::put(const std::string &table,
                          const std::vector<Cell> &cells,
                          const std::string &idempotency_key) {
    Op op;
    op.type = OpType::Put;
    op.table = table;
    op.cells = cells;
    commit({op}, idempotency_key);
}

inline void MongrelDBClient::upsert(const std::string &table,
                             const std::vector<Cell> &cells,
                             const std::vector<Cell> &update_cells,
                             const std::string &idempotency_key) {
    Op op;
    op.type = OpType::Upsert;
    op.table = table;
    op.cells = cells;
    op.update_cells = update_cells;
    commit({op}, idempotency_key);
}

inline void MongrelDBClient::del(const std::string &table, std::int64_t row_id) {
    Op op;
    op.type = OpType::Delete;
    op.table = table;
    op.row_id = row_id;
    commit({op});
}

inline void MongrelDBClient::delete_by_pk(const std::string &table, const Value &pk) {
    Op op;
    op.type = OpType::DeleteByPk;
    op.table = table;
    op.pk_value = pk;
    commit({op});
}

inline void MongrelDBClient::commit(const std::vector<Op> &ops,
                             const std::string &idempotency_key) {
    if (ops.empty()) return;
    std::string body = "{\"ops\":[";
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op &op = ops[i];
        if (i > 0) body.push_back(',');
        body.push_back('{');
        switch (op.type) {
            case OpType::Put:
                body += "\"put\":{\"table\":";
                json_escape(body, op.table);
                body += ",\"cells\":";
                json_cells(body, op.cells);
                body += ",\"returning\":false}";
                break;
            case OpType::Upsert:
                body += "\"upsert\":{\"table\":";
                json_escape(body, op.table);
                body += ",\"cells\":";
                json_cells(body, op.cells);
                if (!op.update_cells.empty()) {
                    body += ",\"update_cells\":";
                    json_cells(body, op.update_cells);
                }
                body += ",\"returning\":false}";
                break;
            case OpType::Delete: {
                body += "\"delete\":{\"table\":";
                json_escape(body, op.table);
                body += ",\"row_id\":";
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld",
                              static_cast<long long>(op.row_id));
                body += buf;
                body.push_back('}');
                break;
            }
            case OpType::DeleteByPk:
                body += "\"delete_by_pk\":{\"table\":";
                json_escape(body, op.table);
                body += ",\"pk\":";
                json_value(body, op.pk_value);
                body.push_back('}');
                break;
        }
        body.push_back('}');
    }
    body += "]";
    if (!idempotency_key.empty()) {
        body += ",\"idempotency_key\":";
        json_escape(body, idempotency_key);
    }
    body.push_back('}');
    impl_->post("/kit/txn", body);
}

inline Result MongrelDBClient::query(const std::string &table,
                              const std::vector<Condition> &conditions,
                              const std::vector<std::int64_t> &projection,
                              std::int64_t limit) {
    std::string body = "{\"table\":";
    json_escape(body, table);
    if (!conditions.empty()) {
        body += ",\"conditions\":[";
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) body.push_back(',');
            const Condition &c = conditions[i];
            body.push_back('{');
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(c.column_id));
            switch (c.kind) {
                case CondKind::Pk:
                    body += "\"pk\":{\"value\":";
                    if (!c.str_value.empty()) {
                        json_escape(body, c.str_value);
                    } else {
                        char b2[32];
                        std::snprintf(b2, sizeof(b2), "%lld",
                                      static_cast<long long>(c.int_value));
                        body += b2;
                    }
                    body.push_back('}');
                    break;
                case CondKind::BitmapEq:
                    body += "\"bitmap_eq\":{\"column_id\":";
                    body += buf;
                    body += ",\"value\":";
                    json_escape(body, c.str_value);
                    body.push_back('}');
                    break;
                case CondKind::Range:
                    body += "\"range\":{\"column_id\":";
                    body += buf;
                    if (c.lo_set) {
                        char b2[64];
                        std::snprintf(b2, sizeof(b2), ",\"lo\":%.17g", c.lo);
                        body += b2;
                    }
                    if (c.hi_set) {
                        char b2[64];
                        std::snprintf(b2, sizeof(b2), ",\"hi\":%.17g", c.hi);
                        body += b2;
                    }
                    body.push_back('}');
                    break;
                case CondKind::FmContains:
                    body += "\"fm_contains\":{\"column_id\":";
                    body += buf;
                    body += ",\"pattern\":";
                    json_escape(body, c.str_value);
                    body.push_back('}');
                    break;
                case CondKind::IsNull:
                    body += "\"is_null\":{\"column_id\":";
                    body += buf;
                    body.push_back('}');
                    break;
                case CondKind::IsNotNull:
                    body += "\"is_not_null\":{\"column_id\":";
                    body += buf;
                    body.push_back('}');
                    break;
            }
            body.push_back('}');
        }
        body.push_back(']');
    }
    if (!projection.empty()) {
        body += ",\"projection\":[";
        for (std::size_t i = 0; i < projection.size(); ++i) {
            if (i > 0) body.push_back(',');
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld",
                          static_cast<long long>(projection[i]));
            body += buf;
        }
        body.push_back(']');
    }
    if (limit > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(limit));
        body += ",\"limit\":";
        body += buf;
    }
    body.push_back('}');

    std::string resp = impl_->post("/kit/query", body);
    Result result;
    bool trunc = false;
    get_bool(resp, "truncated", trunc);
    result.truncated = trunc;

    // Decode the "rows" array. Each row is {"row_id":"...","cells":[c,v,...]}.
    JsonParser j(resp);
    if (j.peek() != '{') return result;
    j.expect('{');
    if (j.peek() == '}') return result;
    // Locate the "rows" array.
    bool found_rows = false;
    for (;;) {
        if (j.peek() != '"') return result;
        std::string k = j.read_string_raw();
        if (!j.ok()) return result;
        j.expect(':');
        if (!j.ok()) return result;
        if (k == "rows" && j.peek() == '[') {
            found_rows = true;
            break;
        }
        j.skip_value();
        if (!j.ok()) return result;
        int ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        return result;
    }
    if (!found_rows) return result;

    // Walk the rows array.
    j.expect('[');
    if (j.peek() == ']') return result;
    for (;;) {
        if (j.peek() != '{') {
            throw QueryException("mongreldb: malformed query response");
        }
        j.expect('{');
        Row row;
        if (j.peek() != '}') {
            for (;;) {
                if (j.peek() != '"') {
                    throw QueryException("mongreldb: malformed row");
                }
                std::string k = j.read_string_raw();
                if (!j.ok()) {
                    throw QueryException("mongreldb: malformed row");
                }
                j.expect(':');
                if (!j.ok()) {
                    throw QueryException("mongreldb: malformed row");
                }
                if (k == "cells" && j.peek() == '[') {
                    j.expect('[');
                    if (j.peek() == ']') {
                        j.expect(']');
                    } else {
                        for (;;) {
                            // column id
                            int ch0 = j.peek();
                            if (ch0 != '-' && !(ch0 >= '0' && ch0 <= '9')) {
                                throw QueryException("mongreldb: malformed cells");
                            }
                            std::string num = j.read_number_raw();
                            std::int64_t colid = static_cast<std::int64_t>(
                                std::strtoll(num.c_str(), nullptr, 10));
                            if (j.peek() == ',') j.expect(',');
                            Value v = j.read_value();
                            if (!j.ok()) {
                                throw QueryException("mongreldb: malformed cell value");
                            }
                            row.push_back(Cell{colid, std::move(v)});
                            int ch = j.peek();
                            if (ch == ',') { j.expect(','); continue; }
                            if (ch == ']') { j.expect(']'); break; }
                            throw QueryException("mongreldb: malformed cells");
                        }
                    }
                    int ch = j.peek();
                    if (ch == '}') { j.expect('}'); break; }
                    if (ch == ',') { j.expect(','); continue; }
                    throw QueryException("mongreldb: malformed row");
                } else {
                    j.skip_value();
                    if (!j.ok()) {
                        throw QueryException("mongreldb: malformed row");
                    }
                    int ch = j.peek();
                    if (ch == ',') { j.expect(','); continue; }
                    if (ch == '}') { j.expect('}'); break; }
                    throw QueryException("mongreldb: malformed row");
                }
            }
        } else {
            j.expect('}');
        }
        result.rows.push_back(std::move(row));
        int ch = j.peek();
        if (ch == ',') { j.expect(','); continue; }
        if (ch == ']') { j.expect(']'); break; }
        throw QueryException("mongreldb: malformed query response");
    }
    return result;
}

inline std::string MongrelDBClient::sql(const std::string &statement) {
    std::string body = "{\"sql\":";
    json_escape(body, statement);
    body += ",\"format\":\"json\"}";
    return impl_->post("/sql", body);
}

inline std::string MongrelDBClient::schema() {
    return impl_->get("/kit/schema");
}

inline std::string MongrelDBClient::schema_for(const std::string &table) {
    return impl_->get("/kit/schema/" + url_encode_segment(table));
}

} // namespace mongreldb

#endif // MONGRELDB_MONGRELDB_IMPL_HPP
