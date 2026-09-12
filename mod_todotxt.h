#ifndef MOD_TODOTXT_H
#define MOD_TODOTXT_H

#include "httpd.h"
#include "apr_pools.h"
#include "apr_tables.h"
#include "apr_time.h"

#define MOD_TODOTXT_VERSION "1.2.0"

typedef struct {
    apr_int64_t id;
    const char *owner;
    const char *raw;
    int completed;
    char priority;
    const char *completion_date;
    const char *creation_date;
    const char *due_date;
    const char *threshold_date;
    const char *recurrence;
    const char *projects;
    const char *contexts;
    apr_time_t created_at;
    apr_time_t updated_at;
} todotxt_item;

/*
 * Storage providers are explicitly passed both the current server_rec and
 * the configured database path. This makes providers VirtualHost-safe:
 * there is no process-global database path.
 */
typedef struct {
    apr_status_t (*insert)(
        apr_pool_t *p,
        server_rec *s,
        const char *db_path,
        const todotxt_item *item,
        apr_int64_t *id_out);

    apr_status_t (*update)(
        apr_pool_t *p,
        server_rec *s,
        const char *db_path,
        const todotxt_item *item);

    apr_status_t (*get)(
        apr_pool_t *p,
        server_rec *s,
        const char *db_path,
        const char *owner,
        apr_int64_t id,
        todotxt_item **item_out);

    apr_status_t (*remove)(
        apr_pool_t *p,
        server_rec *s,
        const char *db_path,
        const char *owner,
        apr_int64_t id);

    apr_status_t (*list)(
        apr_pool_t *p,
        server_rec *s,
        const char *db_path,
        const char *owner,
        const char *mode,
        apr_array_header_t **items_out);
} todotxt_storage_provider;

#endif
