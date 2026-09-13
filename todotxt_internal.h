#ifndef TODOTXT_INTERNAL_H
#define TODOTXT_INTERNAL_H

#include "mod_todotxt.h"
#include "http_config.h"
#include "http_protocol.h"

#define TODOTXT_STORAGE_PROVIDER_GROUP "mod_todotxt_storage"
#define TODOTXT_STORAGE_PROVIDER_VERSION "3"

typedef struct {
    int enabled;
    int allow_anonymous;
    int read_only;
    int enable_due;
    int enable_rec;
    int enable_threshold;
    int create_next_on_complete;
    int expose_id_extension;
    const char *id_extension_field;
    const char *base_path;
    const char *db_path;
    const char *storage_provider;
    const char *anonymous_user;
    apr_size_t max_request_bytes;
    int max_batch_items;
} todotxt_server_conf;

typedef struct {
    int has_priority; const char *priority;
    int has_due; const char *due;
    int has_threshold; const char *threshold;
    int has_recurrence; const char *recurrence;
    int has_projects; const char *projects;
    int has_contexts; const char *contexts;
    int has_creation_date; const char *creation_date;
} todotxt_patch;

extern module AP_MODULE_DECLARE_DATA todotxt_module;

todotxt_server_conf *todotxt_get_conf(server_rec *s);
const todotxt_storage_provider *todotxt_storage(server_rec *s);
apr_status_t todotxt_sqlite_register_provider(apr_pool_t *p);
apr_status_t todotxt_sqlite_init_server(apr_pool_t *p, server_rec *s);
int todotxt_api_handler(request_rec *r);

int todotxt_is_date(const char *s);
int todotxt_is_recurrence(const char *s);
apr_status_t todotxt_parse_line(apr_pool_t*, const todotxt_server_conf*, const char*, const char*, todotxt_item**, const char**);
const char *todotxt_complete_line(apr_pool_t*, const char*, apr_time_t);
const char *todotxt_recurrence_next_line(apr_pool_t*, const todotxt_item*, apr_time_t);
const char *todotxt_strip_server_id_extension(apr_pool_t*, const todotxt_server_conf*, const char*);
const char *todotxt_render_line(apr_pool_t*, const todotxt_server_conf*, const todotxt_item*);
apr_status_t todotxt_parse_patch_json(apr_pool_t*, const char*, apr_size_t, todotxt_patch**, const char**);
apr_status_t todotxt_apply_patch(apr_pool_t*, const todotxt_server_conf*, const char*, const todotxt_item*, const todotxt_patch*, todotxt_item**, const char**);

#endif
