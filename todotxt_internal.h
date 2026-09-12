#ifndef TODOTXT_INTERNAL_H
#define TODOTXT_INTERNAL_H

#include "mod_todotxt.h"
#include "http_config.h"
#include "http_protocol.h"

#define TODOTXT_STORAGE_PROVIDER_GROUP "mod_todotxt_storage"
#define TODOTXT_STORAGE_PROVIDER_VERSION "2"

typedef struct {
    int enabled;
    int allow_anonymous;
    int read_only;

    int enable_due;
    int enable_rec;
    int enable_threshold;
    int create_next_on_complete;

    const char *base_path;
    const char *db_path;
    const char *storage_provider;
    const char *anonymous_user;

    apr_size_t max_request_bytes;
    int max_batch_items;
} todotxt_server_conf;

extern module AP_MODULE_DECLARE_DATA todotxt_module;

todotxt_server_conf *todotxt_get_conf(server_rec *s);
const todotxt_storage_provider *todotxt_storage(server_rec *s);

apr_status_t todotxt_sqlite_register_provider(apr_pool_t *p);
apr_status_t todotxt_sqlite_init_server(apr_pool_t *p, server_rec *s);

int todotxt_api_handler(request_rec *r);

apr_status_t todotxt_parse_line(
    apr_pool_t *p,
    const todotxt_server_conf *conf,
    const char *owner,
    const char *line,
    todotxt_item **out,
    const char **error);

const char *todotxt_complete_line(
    apr_pool_t *p,
    const char *line,
    apr_time_t now);

const char *todotxt_recurrence_next_line(
    apr_pool_t *p,
    const todotxt_item *completed_item,
    apr_time_t now);

#endif
