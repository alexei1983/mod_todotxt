# mod_todotxt 2.0.0

`mod_todotxt` is an Apache HTTP Server 2.4 module providing a multi-user
todo.txt service. Version 2.0 is VirtualHost-safe and optionally exposes the server-generated
ID of each todo.txt item.

## v2.0 additions

- JSON collection endpoint containing database IDs
- optional server-ID todo.txt extension
- configurable single server-ID extension field name
- module status endpoint
- richer list filtering
- threshold-aware visible filtering
- JSON PATCH support
- strong ETags
- `If-Match` optimistic concurrency
- SQLite schema migration adding an item revision counter

## Endpoints

With:

```apache
TodoTxtBasePath /
```

the API is:

```text
GET    /todo.txt
POST   /todo.txt

GET    /todos.json

GET    /todo/status

GET    /todo/{id}
PUT    /todo/{id}
PATCH  /todo/{id}
DELETE /todo/{id}

POST   /todo/{id}/complete
```

`GET /todo.txt?format=json` is also supported as an alias for the JSON
collection representation.

## JSON collection endpoint

```http
GET /todos.json?status=active
```

Example:

```json
[
  {
    "id": 101,
    "revision": 4,
    "etag": "\"todo-101-4\"",
    "raw": "(A) Renew certificate +IT due:2026-10-01",
    "completed": false,
    "priority": "A",
    "due": "2026-10-01",
    "projects": "IT"
  }
]
```

The JSON collection uses the same filtering parameters as `/todo.txt`.

## Optional server ID extension

By default, the server-side SQLite ID is not added to native todo.txt text:

```apache
TodoTxtIdExtension Off
```

Enable it with:

```apache
TodoTxtIdExtension On
```

The default extension field is:

```text
id
```

so text output becomes:

```text
(A) Renew certificate +IT due:2026-10-01 id:101
```

Change the field with exactly one `TAKE1` value:

```apache
TodoTxtIdExtensionField serverid
```

which produces:

```text
(A) Renew certificate +IT due:2026-10-01 serverid:101
```

The directive accepts one field name only. Allowed field-name characters are:

```text
A-Z a-z 0-9 _ - .
```

The default is `id` if the directive is omitted.

The module permits names such as:

```text
due
t
rec
```

as requested, although doing so can confuse generic todo.txt clients because
those names already have semantic meaning. The server distinguishes its own ID
projection because the value is numeric.

When `TodoTxtIdExtension On`, the configured numeric server-ID token is stripped
from incoming POST/PUT text before storage, preventing a client from persisting
or spoofing the projected server ID.

The stored `raw` value remains canonical and does not contain the projected ID.

## Status endpoint

```http
GET /todo/status
```

Example:

```json
{
  "module": "mod_todotxt",
  "version": "2.0.0",
  "user": "alex",
  "anonymous": false,
  "read_only": false,
  "max_request_bytes": 1048576,
  "max_batch_items": 1000,
  "create_next_on_complete": true,
  "extensions": {
    "due": true,
    "rec": true,
    "threshold": true,
    "server_id": {
      "enabled": true,
      "field": "id"
    }
  }
}
```

## Filtering

Filtering applies to both:

```text
GET /todo.txt
GET /todos.json
```

### Status

```text
?status=active
?status=completed
?status=all
```

### Project and context

Exact token match, case-insensitive:

```text
?project=IT
?context=phone
```

### Priority

Exact:

```text
?priority=B
```

At or above a priority:

```text
?priority_at_or_above=C
```

Because `A` is highest priority, this matches `A`, `B`, and `C`.

At or below a priority:

```text
?priority_at_or_below=C
```

This matches `C` through `Z`.

Priority filters exclude items that have no priority.

### Due-date filters

```text
?due=2026-10-01
?due_before=2026-10-01
?due_after=2026-10-01
?due_on_or_before=2026-10-01
?due_on_or_after=2026-10-01
```

`before` and `after` are strict. `on_or_before` and `on_or_after` are inclusive.

### Threshold filters

```text
?threshold=2026-09-20
?threshold_before=2026-09-20
?threshold_after=2026-09-20
?threshold_on_or_before=2026-09-20
?threshold_on_or_after=2026-09-20
```

### Threshold-aware visibility

```text
?visible=1
```

or:

```text
?visible=true
```

excludes items whose `t:` threshold is later than the server's current local
date. Items without a threshold remain visible.

Filters can be combined:

```text
/todos.json?status=active&project=IT&priority_at_or_above=C&due_on_or_before=2026-10-01&visible=1
```

## PATCH

PATCH accepts:

```http
Content-Type: application/json
```

Supported fields:

```text
priority
due
threshold
t
rec
recurrence
projects
contexts
creation_date
```

Example:

```http
PATCH /todo/101
Content-Type: application/json

{
  "priority": "A",
  "due": "2026-10-01",
  "rec": "+1m",
  "threshold": "2026-09-25"
}
```

The response is the updated JSON item.

Use `null` to remove a field:

```json
{
  "due": null,
  "priority": null
}
```

Projects and contexts are strings containing whitespace/comma/semicolon
separated values:

```json
{
  "projects": "IT Infrastructure",
  "contexts": "computer office"
}
```

PATCH preserves the description and unknown extensions in the raw todo line.
Only fields explicitly present in the PATCH object are changed.

## ETags and If-Match

Each item has a monotonically increasing `revision`.

Individual GET responses include:

```http
ETag: "todo-101-4"
```

The JSON representation also includes:

```json
{
  "id": 101,
  "revision": 4,
  "etag": "\"todo-101-4\""
}
```

Use the ETag on mutations:

```http
PUT /todo/101
If-Match: "todo-101-4"
Content-Type: text/plain
```

```http
PATCH /todo/101
If-Match: "todo-101-4"
Content-Type: application/json
```

```http
DELETE /todo/101
If-Match: "todo-101-4"
```

```http
POST /todo/101/complete
If-Match: "todo-101-4"
```

If the revision no longer matches, the module returns:

```text
412 Precondition Failed
```

PATCH and completion also protect the read-modify-write operation internally.
If no explicit `If-Match` was supplied and another write races the operation,
the module returns:

```text
409 Conflict
```

`If-Match: *` is accepted for existence-based mutations.

Weak ETags and comma-separated If-Match lists are intentionally rejected.

## Database migration

v2.0 adds:

```sql
revision INTEGER NOT NULL DEFAULT 1
```

to `todos`.

Existing v1.x databases are upgraded automatically at Apache startup by checking
`PRAGMA table_info(todos)` and issuing:

```sql
ALTER TABLE todos
ADD COLUMN revision INTEGER NOT NULL DEFAULT 1;
```

when necessary.

Every successful PUT, PATCH, or complete operation increments the revision.

## Existing todo.txt functionality retained

The module still supports:

- completion marker `x`
- completion date
- priority `(A)` through `(Z)`
- creation date
- projects `+Project`
- contexts `@context`
- unknown/custom extension preservation
- `due:YYYY-MM-DD`
- `rec:[+]N[d|w|m|y]`
- `t:YYYY-MM-DD`
- recurring next-item creation
- newline-separated batch POST
- multi-user isolation through `request_rec->user`
- anonymous shared-list mode when explicitly enabled
- read-only mode
- VirtualHost-specific databases
- SQLite WAL
- detailed diagnostic logging

## Configuration directives

```apache
TodoTxtEngine On|Off
TodoTxtBasePath /path
TodoTxtStore sqlite:/path/to/db

TodoTxtAllowAnonymous On|Off
TodoTxtAnonymousUser anonymous
TodoTxtReadOnly On|Off

TodoTxtDueExtension On|Off
TodoTxtRecurrenceExtension On|Off
TodoTxtThresholdExtension On|Off
TodoTxtCreateNextOnComplete On|Off

TodoTxtIdExtension On|Off
TodoTxtIdExtensionField id

TodoTxtMaxRequestBytes 1048576
TodoTxtMaxBatchItems 1000
```

## Build dependencies

RHEL / Rocky / Alma:

```bash
dnf install httpd-devel sqlite-devel pkgconf-pkg-config
```

Older RHEL/CentOS:

```bash
yum install httpd-devel sqlite-devel pkgconfig
```

Ubuntu / Debian:

```bash
apt install apache2-dev libsqlite3-dev pkg-config build-essential
```

## Build

```bash
make clean
make
make install
```

## Source layout

```text
mod_todotxt.c
mod_todotxt.h
todotxt_internal.h
todotxt_api.c
todotxt_parser.c
todotxt_patch.c
todotxt_sqlite.c
Makefile
mod_todotxt.conf.example
README.md
```
