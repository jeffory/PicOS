# Neurogram Share API — specification

**Status:** specification only. No server implementation is included or implied.
**Client:** `apps/nonogram/net_client.lua`
**Version:** `v1`

This document is the contract between the Neurogram app and any server that wants
to host shared puzzles. It is written so a server can be implemented from this
document alone.

## Why the server never parses a grid

The wire representation of a puzzle grid is the **share code** — the same string
a player can type by hand (see `sharecode.lua`). The server stores it as an
opaque token and never decodes it.

This is a deliberate constraint, not an omission:

- One codec. The share code is simultaneously the on-disk format
  (`/data/com.picos.nonogram/puzzles/*.json`), the typed offline format, and the
  wire format. There is one encoder, one decoder, one checksum, and therefore one
  place a bug can live.
- The client already validates. The code carries a 10-bit checksum and the client
  verifies it on decode, so a corrupted payload is caught at the point of use.
- A server that cannot read grids cannot corrupt them, and cannot disagree with
  the client about what a puzzle is.

The cost is that the server cannot compute properties of a grid — for example it
cannot verify a submitted `logic` claim, and cannot render thumbnails. Both are
accepted; `logic` is treated as author-supplied metadata (see *Trust model*).

## Transport

- HTTPS strongly recommended; plain HTTP permitted for self-hosted use.
- Base path configurable, default `/v1`.
- All request and response bodies are `application/json; charset=utf-8`.
- The client sends `X-Client: neurogram/1.0`.
- The device supplies a **bare hostname** with no scheme (a `picocalc.network.http`
  constraint), so the server must be reachable at a host/port, not behind a
  path-rewriting proxy that requires a URL prefix the client cannot express.

### Client limits the server must respect

These are properties of the device, not preferences:

| Limit | Value | Consequence for the server |
|---|---|---|
| Simultaneous HTTP connections | 8 device-wide | Do not require pipelining or parallel fetches |
| Response buffer | configurable, default modest | Keep list pages small; see `limit` |
| No streaming JSON parser | whole body buffered | A list page must fit comfortably in RAM |
| Typed share code | ≤ 127 chars | Codes above this are transferable but not typable |

A 15×15 code is 50 characters and a 20×20 is 85, so a 20-item page carrying codes
is on the order of 2–4 KB. That is the intended page size.

## Authentication

Optional. Anonymous publishing is permitted and expected for a hobby deployment.

- If configured, the client sends `Authorization: Bearer <token>`, with the token
  held in `picocalc.appconfig`.
- A server requiring auth returns `401` with an `unauthorized` error code for
  write endpoints and should still allow anonymous reads.
- The client never sends a password and has no login flow. Tokens are entered by
  hand once.

## Data model

### Puzzle object

```json
{
  "id": "p_7f3a91c2",
  "name": "NEON CAT",
  "author": "keith",
  "w": 15,
  "h": 15,
  "code": "N13GQ7M4Z0TQ...XY",
  "logic": "unique-line",
  "created": "2026-07-25T18:04:11Z",
  "plays": 128,
  "solves": 41
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | Server-assigned. `^[A-Za-z0-9_-]{1,32}$` |
| `name` | string | 1–40 chars after trimming. Client upper-cases for display |
| `author` | string | 0–24 chars. Free text, not an identity claim |
| `w`, `h` | integer | 5–32. **Advisory** — the code is authoritative |
| `code` | string | Share code. Opaque to the server. 1–512 chars |
| `logic` | string | `unique-line`, `guess`, `multi`, `unknown`. Author-supplied |
| `created` | string | RFC 3339 UTC |
| `plays`, `solves` | integer | Server-maintained counters, read-only |

`w`/`h` are duplicated outside the code purely so a client can filter and display
a list without decoding every payload. A server **must not** treat a mismatch
between `w`/`h` and the code as authoritative in either direction; the client
decodes the code and uses what it finds.

### Error envelope

Every non-2xx response uses this shape:

```json
{ "error": { "code": "invalid_request", "message": "name must be 1-40 characters" } }
```

| `code` | HTTP | Meaning |
|---|---|---|
| `invalid_request` | 400 | Malformed body or parameters |
| `unauthorized` | 401 | Auth required or token rejected |
| `not_found` | 404 | Unknown id or code |
| `duplicate` | 409 | This exact code is already published |
| `rate_limited` | 429 | See `Retry-After` |
| `too_large` | 413 | Body over the server's cap |
| `server_error` | 500 | Anything else |

`message` is human-readable and may be shown to the user verbatim. Clients branch
on `code`, never on `message`.

## Endpoints

### `GET /v1/puzzles`

Browse published puzzles, newest first by default.

| Parameter | Type | Default | Notes |
|---|---|---|---|
| `sort` | `new` \| `top` | `new` | `top` ranks by solves, ties broken by newest |
| `size` | integer | — | Exact match on the larger of `w`,`h` |
| `limit` | integer | 20 | Clamp to 1–50 |
| `cursor` | string | — | Opaque; from a previous `next` |

```json
{
  "items": [ { "...puzzle object..." } ],
  "next": "eyJvIjoxMjB9"
}
```

`next` is `null` on the last page. Cursors are opaque strings the client echoes
back verbatim; offset- or keyset-based is the server's choice.

**The list response includes `code`.** This is the one place this spec trades
bandwidth for latency deliberately: including the code means browsing and then
playing costs one request instead of two, which on a handheld over WiFi is the
difference between responsive and not.

### `GET /v1/puzzles/{id}`

Returns a single puzzle object. `404 not_found` if unknown.

### `POST /v1/puzzles`

Publish a puzzle.

```json
{
  "name": "NEON CAT",
  "author": "keith",
  "w": 15,
  "h": 15,
  "code": "N13GQ7M4Z0TQ...XY",
  "logic": "unique-line"
}
```

Requires an `Idempotency-Key` header (client-generated, ≤64 chars). Replaying the
same key with the same body must return the original `201` result rather than
creating a second puzzle — the device may retry after a timeout when the first
attempt actually succeeded.

`201 Created` with the full puzzle object and a `Location` header.

Servers **should** reject a `code` whose checksum is structurally invalid by
length or alphabet — that is a cheap sanity check requiring no grid decoding —
and **must** return `409 duplicate` when the identical code already exists.

### `POST /v1/puzzles/{id}/plays`

Report a play or a solve. Drives `sort=top`.

```json
{ "solved": true, "seconds": 842, "moves": 133 }
```

Returns `204`. Best-effort and unauthenticated: the client does not retry on
failure and ignores the response body. Treat the numbers as untrusted telemetry —
they come from a device with a user-settable clock.

### `GET /v1/puzzles/by-code/{code}`

Resolve a share code to a hosted puzzle, so a player who typed a code by hand can
pick up its name, author and play counts. `404 not_found` if the code is not
hosted. This endpoint is a convenience only — the client can always play a valid
code offline without it.

## Rate limiting

Suggested, and what the client is built to tolerate:

| Scope | Limit |
|---|---|
| Reads per IP | 60/min |
| Publishes per IP | 6/hour |
| Play reports per IP | 60/min |

On `429` the server sends `Retry-After` in seconds. The client surfaces the wait
to the user and does not auto-retry.

## Trust model

Be explicit about what this spec does *not* provide, so nobody deploys it
assuming otherwise:

- **`author` is not authenticated.** It is free text. Anyone can publish as anyone.
- **`logic` is not verified.** The server cannot decode grids, so an author can
  claim `unique-line` for an ambiguous puzzle. Clients that care must re-verify
  locally — the app ships a solver that does exactly this (`solver.lua`), and
  create mode runs it before saving.
- **Play counts are self-reported** and trivially inflatable.
- **Content is unmoderated by default.** A share code is an arbitrary bitmap, and
  a 20×20 bitmap is enough to draw something you would not want served under your
  name. A public deployment needs a moderation and takedown story; a
  `DELETE /v1/puzzles/{id}` admin endpoint is intentionally left out of the
  client-facing spec.

## Minimum viable server

To be useful to this client, a server needs only:

1. `GET /v1/puzzles` with `limit` and `cursor`
2. `GET /v1/puzzles/{id}`
3. `POST /v1/puzzles` honouring `Idempotency-Key` and returning `409` on duplicates

`plays` and `by-code` are optional. The client degrades cleanly without them:
`sort=top` falls back to `new`, and code resolution falls back to playing the
code offline.
