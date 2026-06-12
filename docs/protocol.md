# BisonDB Wire Protocol (v1)

BisonDB speaks a minimal framed-BSON protocol over TCP. Default port: **27027**.
There is **no authentication and no TLS** — run it on loopback or a trusted network only.

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

Collection names must match `[A-Za-z0-9_][A-Za-z0-9_-]{0,127}`.

## Commands

| Command | Arguments | Success payload |
|---|---|---|
| `ping` | — | `{}` |
| `serverStatus` | — | `name, version, protocolVersion (int32, currently 1), uptimeSec, connectionsCurrent, opCounters{...}` |
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
