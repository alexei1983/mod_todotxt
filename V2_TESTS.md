# mod_todotxt 2.0 smoke tests

Assume:

```text
BASE=https://todo.example.com
AUTH=-u alex:password
```

## Status

```bash
curl -i $AUTH "$BASE/todo/status"
```

## Create one item

```bash
curl -i $AUTH \
  -H 'Content-Type: text/plain' \
  --data '(B) Test v2 +IT @computer due:2026-10-01 rec:+1m t:2026-09-20' \
  "$BASE/todo.txt"
```

Record the returned `id` and `etag`.

## JSON list with IDs

```bash
curl -i $AUTH "$BASE/todos.json?status=active"
```

## Native list

```bash
curl -i $AUTH "$BASE/todo.txt?status=active"
```

If `TodoTxtIdExtension On`, each line should end in the configured field,
for example:

```text
id:101
```

## Filters

```bash
curl -i $AUTH "$BASE/todos.json?project=IT"
curl -i $AUTH "$BASE/todos.json?context=computer"
curl -i $AUTH "$BASE/todos.json?priority=B"
curl -i $AUTH "$BASE/todos.json?priority_at_or_above=C"
curl -i $AUTH "$BASE/todos.json?priority_at_or_below=B"
curl -i $AUTH "$BASE/todos.json?due_on_or_before=2026-10-01"
curl -i $AUTH "$BASE/todos.json?threshold_on_or_after=2026-09-01"
curl -i $AUTH "$BASE/todos.json?visible=1"
```

## Get one and capture ETag

```bash
curl -i $AUTH "$BASE/todo/101?format=json"
```

Expected:

```http
ETag: "todo-101-1"
```

## PATCH

```bash
curl -i $AUTH \
  -X PATCH \
  -H 'Content-Type: application/json' \
  -H 'If-Match: "todo-101-1"' \
  --data '{"priority":"A","due":"2026-10-15","rec":"+2w"}' \
  "$BASE/todo/101"
```

The returned revision/ETag should increment.

## Remove a value with PATCH

```bash
curl -i $AUTH \
  -X PATCH \
  -H 'Content-Type: application/json' \
  -H 'If-Match: "todo-101-2"' \
  --data '{"threshold":null}' \
  "$BASE/todo/101"
```

## Stale If-Match

Re-use an old ETag:

```bash
curl -i $AUTH \
  -X PATCH \
  -H 'Content-Type: application/json' \
  -H 'If-Match: "todo-101-1"' \
  --data '{"priority":"B"}' \
  "$BASE/todo/101"
```

Expected:

```text
412 Precondition Failed
```

## PUT with If-Match

```bash
curl -i $AUTH \
  -X PUT \
  -H 'Content-Type: text/plain; charset=utf-8' \
  -H 'If-Match: "todo-101-3"' \
  --data '(A) Full replacement +IT due:2026-11-01' \
  "$BASE/todo/101"
```

## Complete with If-Match

```bash
curl -i $AUTH \
  -X POST \
  -H 'If-Match: "todo-101-4"' \
  "$BASE/todo/101/complete"
```

A recurring item should return both the completed item revision and the ID of
the generated next occurrence.

## DELETE with If-Match

```bash
curl -i $AUTH \
  -X DELETE \
  -H 'If-Match: "todo-102-1"' \
  "$BASE/todo/102"
```


## 2.0.1 recurrence regression tests

Repeatedly complete:

```text
(A) 2026-09-17 Daily test due:2026-09-17 rec:1d
```

Every generated active occurrence must have exactly one leading creation date.
It must never accumulate a prefix such as:

```text
(A) 2026-09-17 2026-09-18 2026-09-19 Daily test ...
```

An already-corrupted item with consecutive leading dates should be normalized
on its next recurrence.

Business-day cases:

```text
2026-09-18 Friday   + 1b -> 2026-09-21 Monday
2026-09-19 Saturday + 1b -> 2026-09-21 Monday
2026-09-21 Monday   + 5b -> 2026-09-28 Monday
```

Test both `rec:Nb` and strict `rec:+Nb`.
