# Authentication and authorization

MongrelDB supports two complementary auth models:

1. **Bearer token** - a single secret string sent as
   `Authorization: Bearer <token>`. Use this when you have one shared API
   key, a service token, or a JWT issued by an external identity provider.
2. **HTTP Basic** - a username and password pair sent as
   `Authorization: Basic <base64(user:pass)>`. Use this for MongrelDB's
   built-in user accounts.

Both are set at client construction time and reused for every request.

---

## Choosing a model

| Situation | Use |
|-----------|-----|
| Service-to-service, one shared secret | Bearer token |
| JWT or other externally-issued token | Bearer token |
| Built-in MongrelDB users and roles | HTTP Basic |
| Per-request identity (e.g. web app end user) | Basic, one client per identity |

Basic auth lets you take advantage of MongrelDB's role-based access control:
each user belongs to roles, and roles grant table-level privileges. Bearer
tokens do not carry a MongrelDB identity, so the server treats them as a
single principal whose privileges come from the token's configuration.

## Constructing an authenticated client

The C++ client takes its credentials in the constructor and stores them. There
is no way to change credentials on a live client - destroy it and build a new
one.

```cpp
// Bearer token
mongreldb::MongrelDBClient token_client(
    "http://localhost:8080",
    mongreldb::Auth::bearer("a1b2c3d4-e5f6-7890-abcd-ef1234567890"));

// HTTP Basic
mongreldb::MongrelDBClient basic_client(
    "http://localhost:8080",
    mongreldb::Auth::basic("alice", "s3cret"));
```

For an unauthenticated server (dev or single-tenant embedded use), pass an
empty auth and the client sends no `Authorization` header:

```cpp
mongreldb::MongrelDBClient db("http://localhost:8080", mongreldb::Auth::none());
```

## How credentials travel over the wire

The client attaches the same `Authorization` header to every request - GET,
POST, and DELETE alike. For Basic auth, libcurl builds the header and caches
the base64 encoding. For bearer auth, the client builds the header string
once at construction and reuses it.

Because the header is on every request, never log request headers verbatim in
production. Treat the token or password as you would any other secret.

## Users and roles

When you use Basic auth against a server with the user/role system enabled,
each user's privileges come from their role membership. Manage users and roles
through the dedicated API - the C++ client exposes the same endpoints the
other clients do:

```cpp
// Create a user
db.create_user("alice", "s3cret", {"analyst"});

// Grant a role on a table to a user
db.grant("alice", "orders", "read");

// List users
std::string users_json = db.list_users();

// Revoke and drop
db.revoke("alice", "orders", "read");
db.drop_user("alice");
```

Each call maps a non-2xx response to the matching exception - typically
`AuthException` for 401/403 (bad credentials or insufficient privilege) or
`QueryException` for malformed input. See [errors.md](errors.md).

## Securing the transport

MongrelDB listens on plain HTTP by default. For anything reachable outside
localhost, terminate TLS in front of the server (a reverse proxy or a load
balancer) and point the client at the TLS endpoint:

```cpp
mongreldb::MongrelDBClient db(
    "https://db.example.internal:443",
    mongreldb::Auth::bearer(token));
```

The client does not enforce TLS itself; it trusts whatever URL scheme you
hand it. Keep the secret out of logs, out of version control, and out of
crash dumps - rotate it on a schedule.
