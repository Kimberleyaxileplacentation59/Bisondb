# BisonDB Wire Protocol (v2)

BisonDB speaks a minimal framed-BSON protocol over TCP. Default port: **27027**.

> 🔒 **TLS available.** Run `bisond --tls` (with a cert/key) to encrypt the transport; the
> framing and commands below are then carried inside a TLS 1.2 session, unchanged. Without
> `--tls` the socket is plain TCP (clear text) — fine for loopback dev, not for a network.
> See the [Security page](https://abdullah-masood-05.github.io/bisondb-site/reference/security)
> for the cert options and client verification modes.

**Protocol version history.** `serverStatus.protocolVersion` is **2**. v2 added the
authentication handshake and gates every non-handshake command behind it. A v1 client
(one that never authenticates) is rejected with `AuthRequired` as soon as any user exists.

## Framing

Every message (both directions) is one frame:

```
+----------------+---------------------------+
| payloadLen u32 | payload (one BSON doc)    |
| little-endian  | exactly payloadLen bytes  |
+----------------+---------------------------+
```

- `payloadLen` is the size of the BSON document (which, per BSON, also stores the same
  size in its own first four bytes).
- Valid range: `5 <= payloadLen <= 16777216` (16 MiB). A violation means the byte stream
  cannot be trusted: the server sends one final error frame if possible, then closes the
  connection.
- A well-framed but malformed BSON payload gets an error response (`ParseError` /
  `CorruptData`) and the connection **stays usable** — the stream is still in sync.
- The protocol is **strictly sequential** per connection: send one request, read one
  response, repeat. No pipelining. Clients wanting parallelism open more connections.
- BSON documents use the 11 types of BisonDB's codec: double, string, document, array,
  ObjectId, bool, UTC datetime, null, int32, int64, decimal128.

## Requests and responses

Request:  `{ "cmd": "<name>", ...arguments }`

Success:  `{ "ok": true, ...payload }`

Error:    `{ "ok": false, "error": { "code": "<code>", "message": "<text>" } }`

Error codes: `BadRequest`, `UnknownCommand`, `ParseError`, `DuplicateKey`, `NotFound`,
`CorruptData`, `TooLarge`, `Internal`, plus `ServerBusy` when the connection limit is hit
(sent immediately after accept, then the connection closes).

Authentication adds: `AuthRequired` (not authenticated / setup mode), `AuthFailed` (bad
credentials or bad/unknown token — **deliberately generic**, so it never reveals whether a
username exists), `Forbidden` (authenticated but the role lacks the capability), and
`TokenExpired` (the session token's TTL elapsed — re-authenticate).

Collection names must match `[A-Za-z0-9_][A-Za-z0-9_-]{0,127}`.

## Authentication

Auth is **on by default**. It can be disabled for local development with the server's
`--no-auth` flag (which refuses to bind to anything but loopback and warns loudly on every
startup); when disabled, the handshake is skipped and every command is allowed.

### Connection state machine

```
        ┌──────────────────┐   authenticate / authenticateToken (ok)   ┌────────────────┐
        │  UNAUTHENTICATED  │ ─────────────────────────────────────────▶│  AUTHENTICATED │
        │                   │                                           │  (roles bound) │
        │ allowed: ping,    │ ◀───────────────────────────────────────  │                │
        │ serverStatus,     │   logout / token expiry / token revoked    └────────────────┘
        │ authenticate,     │
        │ authenticateToken │   every other command → AuthRequired
        └──────────────────┘
```

- A freshly accepted connection is **UNAUTHENTICATED**. Only `ping`, `serverStatus`,
  `authenticate`, and `authenticateToken` are allowed. Everything else returns
  `AuthRequired`. (`serverStatus` in this state returns only identity + the `security`
  block; no uptime/connection/op counters.)
- After a successful `authenticate`/`authenticateToken`, the connection is **AUTHENTICATED**
  with the user's roles. **Every** subsequent command is permission-checked, and the
  session token is re-validated (expiry + revocation) on each command.
- `logout`, an expired token (`TokenExpired`), or server-side revocation (the user is
  dropped, or their password changes) drops the connection back to UNAUTHENTICATED.

### Roles and the capability table

Each command requires one capability; a connection's roles must grant it.

| Capability | `read` | `readWrite` | `admin` |
|---|:--:|:--:|:--:|
| read (`find`, `explain`, `listCollections`, `listIndexes`, `dbStats`) | ✅ | ✅ | ✅ |
| write (`insert`, `updateOne`, `deleteMany`, `createCollection`, `dropCollection`, `createIndex`, `dropIndex`, `compact`) | ❌ | ✅ | ✅ |
| admin (`createUser`, `dropUser`, `listUsers`, `shutdown`) | ❌ | ❌ | ✅ |

`ping`, `serverStatus`, `logout`, and `changePassword` need no capability (self-service:
`changePassword` lets any user change *their own* password with the old one; only an admin
can reset *another* user's). `shutdown` additionally requires a **loopback** peer.

### First-run bootstrap

On first start with an empty user store, the server either:

- **`--init-admin <user>`** — reads the password from the `BISONDB_ADMIN_PASSWORD`
  environment variable (never a CLI arg, which would leak in process lists) and creates that
  admin; or
- **setup mode** — prints a one-time **bootstrap token** to stderr. In setup mode the only
  accepted privileged command is `createUser` carrying a matching `bootstrapToken` field,
  which must create an `admin`. Once the first admin exists, setup mode ends and the token
  is void. There is **no anonymous fallback** once users exist.

The offline tool `bisonc auth create-admin --dir <dbdir> --username <u>` creates an admin
directly against the data directory with no running server (the recommended seed/recovery
path).

### Credentials & tokens (how they're stored)

- Passwords are hashed with **Argon2id** (memory-hard) and a per-user random salt; the
  plaintext is never stored or logged. The hash, salt, and KDF params live in a hidden
  system file `<dbdir>/__auth.bsd` (never listed by `listCollections`/`dbStats`).
- A successful `authenticate` issues a **256-bit session token** from the OS CSPRNG. The
  server stores only a **BLAKE2b-256 hash** of the token (never the raw value). Tokens are
  in-memory and **session-scoped**: they are lost on server restart (clients re-authenticate).
- Failed `authenticate` attempts are rate-limited per connection (small increasing delay).

### Auth commands

| Command | Arguments | Success payload |
|---|---|---|
| `authenticate` | `username, password` | `token, expiresInSec, username, roles: [string]` |
| `authenticateToken` | `token` | `username, roles: [string]` |
| `logout` | — | `{}` (invalidates the session token) |
| `createUser` | `username, password, roles: [string]` (admin) — or `bootstrapToken, username, password, roles` in setup mode | `{}` (setup-mode form also returns `token, roles` and logs the connection in) |
| `dropUser` | `username` (admin) | `dropped: bool` |
| `changePassword` | `newPassword`, `oldPassword?` (self), `username?` (admin reset) | `{}` |
| `listUsers` | — (admin) | `users: [{ username, roles, disabled }]` |

## Commands

| Command | Arguments | Success payload |
|---|---|---|
| `ping` | — | `{}` |
| `serverStatus` | — | `name, version, protocolVersion (int32, currently 2), security: { auth, tls:false, setupMode }`; authenticated callers also get `uptimeSec, connectionsCurrent, opCounters{...}` |
| `listCollections` | — | `collections: [string]` |
| `dropCollection` | `coll` | `dropped: bool` |
| `insert` | `coll, documents: [doc, ...]` | `insertedIds: [ObjectId], insertedCount` |
| `find` | `coll, filter, limit?, skip?` | `documents: [...], count` (+ truncation fields, below) |
| `deleteMany` | `coll, filter` | `deletedCount` |
| `updateOne` | `coll, filter, update: {"$set": {...}}` | `matched: bool, modified: bool` |
| `createIndex` | `coll, field` | `built: bool, docsIndexed, docsSkipped` |
| `dropIndex` | `coll, field` | `{}` |
| `listIndexes` | `coll` | `indexes: [string]` |
| `explain` | `coll, filter, limit?` | `plan: { plan, index?, docsExamined, docsReturned }` |
| `compact` | `coll` | `stats: { documents }` |
| `shutdown` | — | `{}`, then the server stops (loopback peers only) |

Notes:

- All commands in this table require an authenticated connection (see
  [Authentication](#authentication)) with a role that grants the matching capability;
  unauthenticated access returns `AuthRequired`, insufficient role returns `Forbidden`.
- `insert`: documents without `_id` get a server-generated ObjectId; a present `_id`
  must be an ObjectId and unique (`DuplicateKey` otherwise).
- Filters support `{field: literal}`, `$eq/$ne/$gt/$gte/$lt/$lte/$in`, `$and`/`$or`, and
  dotted field paths. Numbers compare numerically across int32/int64/double; other
  comparisons are type-bracketed.
- `shutdown` from a non-loopback peer returns `BadRequest`.

## find truncation (no cursors)

A single response must fit in 16 MiB. When the full result set does not fit, the server
returns as many documents as fit plus:

```
{ "ok": true, "documents": [...], "count": N, "truncated": true, "skipNext": M }
```

The client resumes with the same filter and `skip = M`, repeating until a response with
no `truncated` field arrives. This is a deliberate simplification — real server-side
cursors are out of scope; results may shift if the collection is mutated between batches.

## Example session (Python)

```python
import socket, struct

def bson_string_doc(cmd):  # build {"cmd": cmd} by hand for illustration
    key, val = b"cmd\x00", cmd.encode() + b"\x00"
    body = b"\x02" + key + struct.pack("<i", len(val)) + val
    return struct.pack("<i", 4 + len(body) + 1) + body + b"\x00"

s = socket.create_connection(("127.0.0.1", 27027))
payload = bson_string_doc("ping")
s.sendall(struct.pack("<I", len(payload)) + payload)
(length,) = struct.unpack("<I", s.recv(4))
response = b""
while len(response) < length:
    response += s.recv(length - len(response))
# response is the BSON document {"ok": true}
```

Any BSON library (e.g. `pymongo.bson`) can replace the hand-rolled encoding; the frame
is just `<u32 LE length><bson bytes>` in both directions.
