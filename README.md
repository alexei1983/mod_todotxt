# mod_todotxt 1.2.0 diagnostic / VirtualHost-safe build

This version fixes the storage-provider bug observed when `TodoTxtEngine On`
is configured inside a VirtualHost.

## Important fixes in 1.2.0

### Provider registration no longer depends on TodoTxtEngine

The SQLite provider is registered in the module's `register_hooks()` phase:

```text
mod_todotxt_storage / sqlite / 2
```

It is therefore available regardless of whether `TodoTxtEngine On` is placed:

- globally
- in one VirtualHost
- in several VirtualHosts

The previous design could skip provider registration when the main server had
the module disabled but a VirtualHost enabled it.

### No process-global database path

The previous storage implementation used a process-global SQLite path.

That is gone.

Every provider operation now receives:

```text
server_rec *
db_path
owner
...
```

directly from the request's VirtualHost configuration.

This permits configurations such as:

```apache
<VirtualHost *:443>
    ServerName todo-one.example.com
    TodoTxtEngine On
    TodoTxtStore sqlite:/data/todo-one.db
</VirtualHost>

<VirtualHost *:443>
    ServerName todo-two.example.com
    TodoTxtEngine On
    TodoTxtStore sqlite:/data/todo-two.db
</VirtualHost>
```

without one VirtualHost overwriting another one's database path.

### Per-server schema initialization

During `post_config`, the module walks the main server and every VirtualHost.

For every server where:

```apache
TodoTxtEngine On
```

it opens that server's configured SQLite database and ensures the schema and
indexes exist.

### Diagnostic logging retained

This build intentionally keeps verbose diagnostics.

Examples:

```text
mod_todotxt: registered provider group='mod_todotxt_storage'
name='sqlite' version='2'
```

```text
mod_todotxt: post_config version='1.2.0'
server='goodspace.org'
base_path='/'
db='/var/www/vhosts/_goodspace.org/private/todotxt.db'
provider='sqlite'
```

```text
mod_todotxt: handler entered version='1.2.0'
uri='/todo.txt'
method='GET'
authenticated_user='alex'
db='...'
provider='sqlite'
```

Every API path that can emit HTTP 500 logs a reason.

SQLite also logs:

- operation
- database path
- SQLite return code
- SQLite extended return code
- SQLite error message

For example:

```text
mod_todotxt: SQLite failure:
operation='list prepare'
db='/path/todotxt.db'
sqlite_rc=1
sqlite_extended_rc=1
message='no such table: todos'
```

## Recommended LogLevel

Apache 2.4:

```apache
LogLevel warn todotxt:debug
```

If the platform does not support per-module LogLevel:

```apache
LogLevel debug
```

## API

```text
GET    /todo.txt
POST   /todo.txt

GET    /todo/{id}
PUT    /todo/{id}
DELETE /todo/{id}

POST   /todo/{id}/complete
```

`POST /todo.txt` accepts `text/plain`, either one item or a newline-separated
batch.

## Multi-user behavior

The authenticated Apache username from:

```c
request_rec->user
```

is the owner.

Every SQL operation includes the owner in its WHERE clause where appropriate.

Anonymous mode remains disabled by default.

## Extensions

Enabled by default:

```text
due:YYYY-MM-DD
rec:[+]N[d|w|m|y]
t:YYYY-MM-DD
```

## Dependencies

No JSON library is required.

Dependencies:

- Apache 2.4 development headers
- APR/APR-util through Apache
- SQLite3 development headers
- pkg-config

RHEL/CentOS:

```bash
yum install httpd-devel sqlite-devel pkgconfig
```

or newer systems:

```bash
dnf install httpd-devel sqlite-devel pkgconf-pkg-config
```

Ubuntu/Debian:

```bash
apt install apache2-dev libsqlite3-dev pkg-config build-essential
```

## Build

```bash
make clean
make
make install
```

The diagnostic build uses:

```text
-g -O0
```

to make debugger stack traces useful.

## Verify module/provider initialization

After restart:

```bash
httpd -M | grep todotxt
```

Then examine the log:

```bash
tail -f /var/log/httpd/error_log
```

You should see initialization messages for the enabled VirtualHost.

## SQLite / SELinux

SQLite WAL mode creates:

```text
todotxt.db
todotxt.db-wal
todotxt.db-shm
```

Apache therefore needs write permission to the directory, not only to the
database file.

On SELinux systems the directory should normally have an appropriate writable
HTTP context such as:

```text
httpd_sys_rw_content_t
```

## Source layout

```text
mod_todotxt.c
    directives
    module/provider registration
    VirtualHost-aware post_config initialization

todotxt_api.c
    multi-user HTTP API
    manual JSON escaping
    detailed HTTP-500 diagnostics

todotxt_parser.c
    todo.txt parsing
    due/rec/threshold extensions
    recurrence generation

todotxt_sqlite.c
    VirtualHost-safe SQLite provider
    schema initialization
    detailed SQLite diagnostics

mod_todotxt.h
    public provider API

todotxt_internal.h
    internal configuration/API definitions
```
