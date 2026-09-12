#include "todotxt_internal.h"

#include "http_request.h"
#include "http_log.h"
#include "apr_strings.h"

#include <string.h>
#include <stdlib.h>
#include "util_script.h"

static void log_500(
    request_rec *r,
    const char *operation,
    apr_status_t rv,
    const char *detail)
{
    todotxt_server_conf *conf =
        todotxt_get_conf(r->server);

    ap_log_rerror(
        APLOG_MARK,
        APLOG_ERR,
        rv,
        r,
        "mod_todotxt: HTTP 500: %s; detail='%s'; "
        "uri='%s' method='%s' user='%s' db='%s'",
        operation ? operation : "(unknown operation)",
        detail ? detail : "(none)",
        r->uri ? r->uri : "(null)",
        r->method ? r->method : "(null)",
        r->user ? r->user : "(null)",
        conf && conf->db_path
            ? conf->db_path
            : "(null)");
}


static int request_is_text_plain(request_rec *r)
{
    const char *content_type =
        apr_table_get(
            r->headers_in,
            "Content-Type"
        );

    if (!content_type)
        return 0;

    /*
     * Accept:
     *
     * text/plain
     * text/plain; charset=utf-8
     * text/plain;charset=UTF-8
     *
     * Media types are case-insensitive.
     */
    while (*content_type == ' ' ||
           *content_type == '\t') {

        ++content_type;
    }

    if (strncasecmp(
            content_type,
            "text/plain",
            10) != 0) {

        return 0;
    }

    /*
     * Don't accidentally accept:
     *
     * text/plainthing
     */
    content_type += 10;

    return
        *content_type == '\0' ||
        *content_type == ';' ||
        *content_type == ' ' ||
        *content_type == '\t';
}

static const char *owner_for(
    request_rec *r,
    const todotxt_server_conf *conf)
{
    if (r->user && *r->user)
        return r->user;

    if (conf->allow_anonymous)
        return conf->anonymous_user;

    return NULL;
}

static int read_body(
    request_rec *r,
    apr_size_t max,
    char **out,
    apr_size_t *len)
{
    char tmp[8192];
    char *buf;
    apr_size_t cap = 8192;
    apr_size_t used = 0;
    int n;

    if (ap_setup_client_block(
            r,
            REQUEST_CHUNKED_ERROR) != OK) {

        return HTTP_BAD_REQUEST;
    }

    buf = apr_palloc(r->pool, cap);

    while ((n = ap_get_client_block(
                r,
                tmp,
                sizeof(tmp))) > 0) {

        if (used + (apr_size_t)n > max)
            return HTTP_REQUEST_ENTITY_TOO_LARGE;

        if (used + (apr_size_t)n + 1 > cap) {
            apr_size_t nc = cap * 2;
            char *nb;

            while (nc < used + (apr_size_t)n + 1)
                nc *= 2;

            nb = apr_palloc(r->pool, nc);
            memcpy(nb, buf, used);
            buf = nb;
            cap = nc;
        }

        memcpy(buf + used, tmp, (apr_size_t)n);
        used += (apr_size_t)n;
    }

    if (n < 0)
        return HTTP_BAD_REQUEST;

    buf[used] = '\0';

    *out = buf;
    *len = used;

    return OK;
}

static const char *json_escape(
    apr_pool_t *p,
    const char *s)
{
    const unsigned char *q;
    apr_size_t n = 2;
    char *out;
    char *d;

    if (!s)
        return "\"\"";

    for (q = (const unsigned char *)s; *q; ++q) {
        switch (*q) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                n += 2;
                break;

            default:
                n += (*q < 0x20) ? 6 : 1;
                break;
        }
    }

    out = apr_palloc(p, n + 1);
    d = out;

    *d++ = '"';

    for (q = (const unsigned char *)s; *q; ++q) {
        switch (*q) {
            case '"':
                *d++ = '\\';
                *d++ = '"';
                break;

            case '\\':
                *d++ = '\\';
                *d++ = '\\';
                break;

            case '\b':
                *d++ = '\\';
                *d++ = 'b';
                break;

            case '\f':
                *d++ = '\\';
                *d++ = 'f';
                break;

            case '\n':
                *d++ = '\\';
                *d++ = 'n';
                break;

            case '\r':
                *d++ = '\\';
                *d++ = 'r';
                break;

            case '\t':
                *d++ = '\\';
                *d++ = 't';
                break;

            default:
                if (*q < 0x20) {
                    apr_snprintf(
                        d,
                        7,
                        "\\u%04x",
                        (unsigned)*q);

                    d += 6;
                }
                else {
                    *d++ = (char)*q;
                }
                break;
        }
    }

    *d++ = '"';
    *d = '\0';

    return out;
}

static void json_begin(
    request_rec *r,
    int status)
{
    r->status = status;
    ap_set_content_type(
        r,
        "application/json; charset=utf-8");
}

static void json_write_item(
    request_rec *r,
    const todotxt_item *item)
{
    ap_rprintf(
        r,
        "{\"id\":%" APR_INT64_T_FMT ","
        "\"raw\":%s,"
        "\"completed\":%s",
        item->id,
        json_escape(r->pool, item->raw),
        item->completed ? "true" : "false");

    if (item->priority) {
        char pbuf[2] = { item->priority, '\0' };

        ap_rprintf(
            r,
            ",\"priority\":%s",
            json_escape(r->pool, pbuf));
    }

    if (item->completion_date) {
        ap_rprintf(
            r,
            ",\"completion_date\":%s",
            json_escape(
                r->pool,
                item->completion_date));
    }

    if (item->creation_date) {
        ap_rprintf(
            r,
            ",\"creation_date\":%s",
            json_escape(
                r->pool,
                item->creation_date));
    }

    if (item->due_date) {
        ap_rprintf(
            r,
            ",\"due\":%s",
            json_escape(
                r->pool,
                item->due_date));
    }

    if (item->threshold_date) {
        ap_rprintf(
            r,
            ",\"threshold\":%s",
            json_escape(
                r->pool,
                item->threshold_date));
    }

    if (item->recurrence) {
        ap_rprintf(
            r,
            ",\"rec\":%s",
            json_escape(
                r->pool,
                item->recurrence));
    }

    if (item->projects) {
        ap_rprintf(
            r,
            ",\"projects\":%s",
            json_escape(
                r->pool,
                item->projects));
    }

    if (item->contexts) {
        ap_rprintf(
            r,
            ",\"contexts\":%s",
            json_escape(
                r->pool,
                item->contexts));
    }

    ap_rputs("}", r);
}

static int get_storage_or_500(
    request_rec *r,
    const todotxt_storage_provider **sp_out)
{
    const todotxt_storage_provider *sp =
        todotxt_storage(r->server);

    if (!sp) {
        log_500(
            r,
            "storage provider lookup",
            0,
            "provider is NULL");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    *sp_out = sp;
    return OK;
}

static int post_batch(
    request_rec *r,
    const todotxt_server_conf *conf,
    const char *owner)
{
    const todotxt_storage_provider *sp;
    char *body = NULL;
    char *save = NULL;
    char *line;
    apr_size_t len = 0;
    int rc;
    int count = 0;
    int first = 1;

    rc = get_storage_or_500(r, &sp);
    if (rc != OK)
        return rc;

    if (conf->read_only)
        return HTTP_FORBIDDEN;

    if (!request_is_text_plain(r))
        return HTTP_UNSUPPORTED_MEDIA_TYPE;

    rc = read_body(
        r,
        conf->max_request_bytes,
        &body,
        &len);

    if (rc != OK)
        return rc;

    /*
     * Validate/insert before emitting the final closing bracket.
     * Individual invalid todo lines are represented as JSON error entries.
     */
    json_begin(r, HTTP_CREATED);
    ap_rputs("[", r);

    for (line = apr_strtok(
            body,
            "\r\n",
            &save);
         line;
         line = apr_strtok(
            NULL,
            "\r\n",
            &save)) {

        todotxt_item *item = NULL;
        const char *err = NULL;
        apr_int64_t id = 0;
        apr_status_t rv;

        while (*line == ' ' || *line == '\t')
            ++line;

        if (!*line)
            continue;

        if (++count > conf->max_batch_items)
            return HTTP_REQUEST_ENTITY_TOO_LARGE;

        if (!first)
            ap_rputs(",", r);

        first = 0;

        if (todotxt_parse_line(
                r->pool,
                conf,
                owner,
                line,
                &item,
                &err) != APR_SUCCESS) {

            ap_rprintf(
                r,
                "{\"line\":%d,\"error\":%s}",
                count,
                json_escape(
                    r->pool,
                    err ? err : "invalid todo"));

            continue;
        }

        rv = sp->insert(
            r->pool,
            r->server,
            conf->db_path,
            item,
            &id);

        if (rv != APR_SUCCESS) {
            log_500(
                r,
                "POST todo.txt insert",
                rv,
                line);

            return HTTP_INTERNAL_SERVER_ERROR;
        }

        ap_rprintf(
            r,
            "{\"line\":%d,\"id\":%" APR_INT64_T_FMT "}",
            count,
            id);
    }

    ap_rputs("]\n", r);
    return OK;
}

static int list_items(
    request_rec *r,
    const todotxt_server_conf *conf,
    const char *owner)
{
    const todotxt_storage_provider *sp;
    apr_array_header_t *rows = NULL;
    apr_table_t *args = NULL;
    const char *status = "active";
    apr_status_t rv;
    int rc;
    int i;

    rc = get_storage_or_500(r, &sp);
    if (rc != OK)
        return rc;

    ap_args_to_table(r, &args);

    if (args && apr_table_get(args, "status"))
        status = apr_table_get(args, "status");

    if (strcmp(status, "active") &&
        strcmp(status, "completed") &&
        strcmp(status, "all")) {

        return HTTP_BAD_REQUEST;
    }

    rv = sp->list(
        r->pool,
        r->server,
        conf->db_path,
        owner,
        status,
        &rows);

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "GET todo.txt list",
            rv,
            status);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    ap_set_content_type(
        r,
        "text/plain; charset=utf-8");

    for (i = 0; i < rows->nelts; ++i) {
        todotxt_item *item =
            APR_ARRAY_IDX(
                rows,
                i,
                todotxt_item *);

        ap_rputs(item->raw, r);
        ap_rputs("\n", r);
    }

    return OK;
}

static int get_one(
    request_rec *r,
    const todotxt_server_conf *conf,
    const char *owner,
    apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    todotxt_item *item = NULL;
    apr_status_t rv;
    int rc;

    rc = get_storage_or_500(r, &sp);
    if (rc != OK)
        return rc;

    rv = sp->get(
        r->pool,
        r->server,
        conf->db_path,
        owner,
        id,
        &item);

    if (rv == APR_NOTFOUND)
        return HTTP_NOT_FOUND;

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "GET todo item",
            rv,
            "storage get failed");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (r->args &&
        strstr(
            r->args,
            "format=json")) {

        json_begin(r, HTTP_OK);
        json_write_item(r, item);
        ap_rputs("\n", r);
    }
    else {
        ap_set_content_type(
            r,
            "text/plain; charset=utf-8");

        ap_rputs(item->raw, r);
        ap_rputs("\n", r);
    }

    return OK;
}

static int put_one(
    request_rec *r,
    const todotxt_server_conf *conf,
    const char *owner,
    apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    char *body = NULL;
    apr_size_t len = 0;
    todotxt_item *item = NULL;
    const char *err = NULL;
    apr_status_t rv;
    int rc;

    rc = get_storage_or_500(r, &sp);
    if (rc != OK)
        return rc;

    if (conf->read_only)
        return HTTP_FORBIDDEN;

    if (!request_is_text_plain(r))
        return HTTP_UNSUPPORTED_MEDIA_TYPE;

    rc = read_body(
        r,
        conf->max_request_bytes,
        &body,
        &len);

    if (rc != OK)
        return rc;

    while (len &&
           (body[len - 1] == '\r' ||
            body[len - 1] == '\n')) {

        body[--len] = '\0';
    }

    if (strchr(body, '\n') ||
        strchr(body, '\r')) {

        return HTTP_BAD_REQUEST;
    }

    if (todotxt_parse_line(
            r->pool,
            conf,
            owner,
            body,
            &item,
            &err) != APR_SUCCESS) {

        return HTTP_BAD_REQUEST;
    }

    item->id = id;

    rv = sp->update(
        r->pool,
        r->server,
        conf->db_path,
        item);

    if (rv == APR_NOTFOUND)
        return HTTP_NOT_FOUND;

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "PUT todo item update",
            rv,
            body);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    r->status = HTTP_NO_CONTENT;
    return OK;
}

static int delete_one(
    request_rec *r,
    const todotxt_server_conf *conf,
    const char *owner,
    apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    apr_status_t rv;
    int rc;

    rc = get_storage_or_500(r, &sp);
    if (rc != OK)
        return rc;

    if (conf->read_only)
        return HTTP_FORBIDDEN;

    rv = sp->remove(
        r->pool,
        r->server,
        conf->db_path,
        owner,
        id);

    if (rv == APR_NOTFOUND)
        return HTTP_NOT_FOUND;

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "DELETE todo item",
            rv,
            "storage delete failed");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    r->status = HTTP_NO_CONTENT;
    return OK;
}

static int complete_one(
    request_rec *r,
    const todotxt_server_conf *conf,
    const char *owner,
    apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    todotxt_item *old = NULL;
    todotxt_item *done = NULL;
    todotxt_item *next = NULL;
    apr_status_t rv;
    const char *line;
    const char *err = NULL;
    const char *next_line;
    apr_int64_t next_id = 0;
    int rc;

    rc = get_storage_or_500(r, &sp);
    if (rc != OK)
        return rc;

    if (conf->read_only)
        return HTTP_FORBIDDEN;

    rv = sp->get(
        r->pool,
        r->server,
        conf->db_path,
        owner,
        id,
        &old);

    if (rv == APR_NOTFOUND)
        return HTTP_NOT_FOUND;

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "POST complete get",
            rv,
            "storage get failed");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (old->completed) {
        json_begin(r, HTTP_OK);
        json_write_item(r, old);
        ap_rputs("\n", r);
        return OK;
    }

    line = todotxt_complete_line(
        r->pool,
        old->raw,
        apr_time_now());

    if (!line) {
        log_500(
            r,
            "POST complete build line",
            APR_EGENERAL,
            "todotxt_complete_line returned NULL");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    rv = todotxt_parse_line(
        r->pool,
        conf,
        owner,
        line,
        &done,
        &err);

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "POST complete parse generated line",
            rv,
            err ? err : "parse failed");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    done->id = id;

    rv = sp->update(
        r->pool,
        r->server,
        conf->db_path,
        done);

    if (rv != APR_SUCCESS) {
        log_500(
            r,
            "POST complete update",
            rv,
            "storage update failed");

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    json_begin(r, HTTP_OK);

    ap_rprintf(
        r,
        "{\"completed_id\":%" APR_INT64_T_FMT,
        id);

    if (conf->create_next_on_complete &&
        done->recurrence) {

        next_line =
            todotxt_recurrence_next_line(
                r->pool,
                done,
                apr_time_now());

        if (!next_line) {
            ap_log_rerror(
                APLOG_MARK,
                APLOG_WARNING,
                0,
                r,
                "mod_todotxt: recurrence generation returned NULL "
                "id=%" APR_INT64_T_FMT " user='%s' rec='%s'",
                id,
                owner,
                done->recurrence);
        }
        else {
            rv = todotxt_parse_line(
                r->pool,
                conf,
                owner,
                next_line,
                &next,
                &err);

            if (rv != APR_SUCCESS) {
                log_500(
                    r,
                    "POST complete parse recurrence",
                    rv,
                    err ? err : "parse failed");

                return HTTP_INTERNAL_SERVER_ERROR;
            }

            rv = sp->insert(
                r->pool,
                r->server,
                conf->db_path,
                next,
                &next_id);

            if (rv != APR_SUCCESS) {
                log_500(
                    r,
                    "POST complete insert recurrence",
                    rv,
                    next_line);

                return HTTP_INTERNAL_SERVER_ERROR;
            }

            ap_rprintf(
                r,
                ",\"next_id\":%" APR_INT64_T_FMT ","
                "\"next\":%s",
                next_id,
                json_escape(
                    r->pool,
                    next_line));
        }
    }

    ap_rputs("}\n", r);
    return OK;
}

int todotxt_api_handler(
    request_rec *r)
{
    todotxt_server_conf *conf =
        todotxt_get_conf(r->server);

    const char *owner;
    const char *root;
    const char *prefix;
    const char *p;
    apr_int64_t id;
    char *end = NULL;

    /*
     * Module disabled for this server/vhost.
     */
    if (!conf || !conf->enabled)
        return DECLINED;

    /*
     * Work out exactly which URL space belongs to us BEFORE doing
     * authentication, logging, storage lookups, etc.
     */
    root =
        !strcmp(conf->base_path, "/")
            ? "/todo.txt"
            : apr_pstrcat(
                r->pool,
                conf->base_path,
                "/todo.txt",
                NULL);

    prefix =
        !strcmp(conf->base_path, "/")
            ? "/todo/"
            : apr_pstrcat(
                r->pool,
                conf->base_path,
                "/todo/",
                NULL);

    /*
     * This request does not belong to mod_todotxt.
     *
     * It is important that this occurs before owner_for(), otherwise
     * unrelated requests could incorrectly receive HTTP 401.
     */
    if (strcmp(r->uri, root) != 0 &&
        strncmp(r->uri, prefix, strlen(prefix)) != 0) {

        return DECLINED;
    }

    /*
     * From this point forward we know the request is actually ours.
     */
    ap_log_rerror(
        APLOG_MARK,
        APLOG_DEBUG,
        0,
        r,
        "mod_todotxt: handler accepted request "
        "version='%s' uri='%s' method='%s' "
        "authenticated_user='%s' base_path='%s' "
        "db='%s' provider='%s'",
        MOD_TODOTXT_VERSION,
        r->uri ? r->uri : "(null)",
        r->method ? r->method : "(null)",
        r->user ? r->user : "(null)",
        conf->base_path
            ? conf->base_path
            : "(null)",
        conf->db_path
            ? conf->db_path
            : "(null)",
        conf->storage_provider
            ? conf->storage_provider
            : "(null)");

    /*
     * Authentication/ownership only matters for requests that are
     * actually part of the todo.txt API.
     */
    owner = owner_for(r, conf);

    if (!owner)
        return HTTP_UNAUTHORIZED;

    /*
     * /todo.txt
     */
    if (!strcmp(r->uri, root)) {

        if (r->method_number == M_GET) {
            return list_items(
                r,
                conf,
                owner);
        }

        if (r->method_number == M_POST) {
            return post_batch(
                r,
                conf,
                owner);
        }

        return HTTP_METHOD_NOT_ALLOWED;
    }

    /*
     * Anything reaching here matched /todo/.
     */
    p = r->uri + strlen(prefix);

    /*
     * Bare /todo/ isn't a valid item URL.
     */
    if (!*p)
        return HTTP_NOT_FOUND;

    id = apr_strtoi64(
        p,
        &end,
        10);

    if (end == p || id <= 0)
        return HTTP_BAD_REQUEST;

    /*
     * /todo/{id}
     */
    if (*end == '\0') {

        if (r->method_number == M_GET) {
            return get_one(
                r,
                conf,
                owner,
                id);
        }

        if (r->method_number == M_PUT) {
            return put_one(
                r,
                conf,
                owner,
                id);
        }

        if (r->method_number == M_DELETE) {
            return delete_one(
                r,
                conf,
                owner,
                id);
        }

        return HTTP_METHOD_NOT_ALLOWED;
    }

    /*
     * /todo/{id}/complete
     */
    if (!strcmp(end, "/complete")) {

        if (r->method_number == M_POST) {
            return complete_one(
                r,
                conf,
                owner,
                id);
        }

        return HTTP_METHOD_NOT_ALLOWED;
    }

    /*
     * It started with /todo/, so it is within our URL namespace,
     * but it isn't an endpoint we implement.
     */
    return HTTP_NOT_FOUND;
}