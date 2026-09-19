#ifndef MOD_TODOTXT_H
#define MOD_TODOTXT_H

#include "httpd.h"
#include "apr_pools.h"
#include "apr_tables.h"
#include "apr_time.h"

#define MOD_TODOTXT_VERSION "2.0.1"

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
    apr_int64_t revision;
    apr_time_t created_at;
    apr_time_t updated_at;
} todotxt_item;

typedef struct {
    apr_status_t (*insert)(apr_pool_t*, server_rec*, const char*, const todotxt_item*, apr_int64_t*);
    apr_status_t (*update)(apr_pool_t*, server_rec*, const char*, const todotxt_item*, apr_int64_t, apr_int64_t*);
    apr_status_t (*get)(apr_pool_t*, server_rec*, const char*, const char*, apr_int64_t, todotxt_item**);
    apr_status_t (*remove)(apr_pool_t*, server_rec*, const char*, const char*, apr_int64_t, apr_int64_t);
    apr_status_t (*list)(apr_pool_t*, server_rec*, const char*, const char*, const char*, apr_array_header_t**);
} todotxt_storage_provider;

#endif
