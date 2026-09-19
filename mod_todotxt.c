#include "todotxt_internal.h"

#include "ap_provider.h"
#include "http_log.h"
#include "apr_strings.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void *create_server_conf(apr_pool_t *p, server_rec *s)
{
    todotxt_server_conf *conf = apr_pcalloc(p, sizeof(*conf));
    (void)s;

    conf->enabled = 0;
    conf->allow_anonymous = 0;
    conf->read_only = 0;
    conf->enable_due = 1;
    conf->enable_rec = 1;
    conf->enable_threshold = 1;
    conf->create_next_on_complete = 1;
    conf->expose_id_extension = 0;
    conf->id_extension_field = "id";
    conf->base_path = "/";
    conf->db_path = "/var/lib/httpd/todotxt.db";
    conf->storage_provider = "sqlite";
    conf->anonymous_user = "anonymous";
    conf->max_request_bytes = 1024 * 1024;
    conf->max_batch_items = 1000;
    return conf;
}

todotxt_server_conf *todotxt_get_conf(server_rec *s)
{
    return ap_get_module_config(s->module_config, &todotxt_module);
}

const todotxt_storage_provider *todotxt_storage(server_rec *s)
{
    todotxt_server_conf *conf = todotxt_get_conf(s);
    if (!conf || !conf->storage_provider) return NULL;
    return ap_lookup_provider(TODOTXT_STORAGE_PROVIDER_GROUP,
                              conf->storage_provider,
                              TODOTXT_STORAGE_PROVIDER_VERSION);
}

#define FLAG_FN(name, field) \
    static const char *name(cmd_parms *cmd, void *dummy, int flag) \
    { (void)dummy; todotxt_get_conf(cmd->server)->field = flag; return NULL; }

FLAG_FN(cmd_engine, enabled)
FLAG_FN(cmd_anon, allow_anonymous)
FLAG_FN(cmd_ro, read_only)
FLAG_FN(cmd_due, enable_due)
FLAG_FN(cmd_rec, enable_rec)
FLAG_FN(cmd_threshold, enable_threshold)
FLAG_FN(cmd_next, create_next_on_complete)
FLAG_FN(cmd_id_extension, expose_id_extension)

static const char *cmd_base(cmd_parms *cmd, void *dummy, const char *arg)
{
    char *v; size_t n; (void)dummy;
    if (!arg || arg[0] != '/') return "TodoTxtBasePath must start with /";
    v = apr_pstrdup(cmd->pool, arg);
    n = strlen(v);
    while (n > 1 && v[n-1] == '/') v[--n] = '\0';
    todotxt_get_conf(cmd->server)->base_path = v;
    return NULL;
}

static const char *cmd_store(cmd_parms *cmd, void *dummy, const char *arg)
{
    todotxt_server_conf *conf = todotxt_get_conf(cmd->server);
    (void)dummy;
    if (strncasecmp(arg, "sqlite:", 7))
        return "TodoTxtStore currently supports sqlite:/path/to/file.db";
    if (!arg[7]) return "TodoTxtStore SQLite path cannot be empty";
    conf->storage_provider = "sqlite";
    conf->db_path = apr_pstrdup(cmd->pool, arg + 7);
    return NULL;
}

static const char *cmd_anon_user(cmd_parms *cmd, void *dummy, const char *arg)
{
    (void)dummy; todotxt_get_conf(cmd->server)->anonymous_user = arg; return NULL;
}

static const char *cmd_id_extension_field(cmd_parms *cmd, void *dummy, const char *arg)
{
    const unsigned char *p; (void)dummy;
    if (!arg || !*arg) return "TodoTxtIdExtensionField cannot be empty";
    for (p=(const unsigned char*)arg; *p; ++p) {
        if (!(isalnum(*p) || *p=='_' || *p=='-' || *p=='.'))
            return "TodoTxtIdExtensionField may contain only letters, digits, _, - and .";
    }
    todotxt_get_conf(cmd->server)->id_extension_field = apr_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *cmd_max_bytes(cmd_parms *cmd, void *dummy, const char *arg)
{
    apr_int64_t n = apr_atoi64(arg); (void)dummy;
    if (n < 1024 || n > 64LL*1024*1024)
        return "TodoTxtMaxRequestBytes must be 1024..67108864";
    todotxt_get_conf(cmd->server)->max_request_bytes = (apr_size_t)n;
    return NULL;
}

static const char *cmd_max_batch(cmd_parms *cmd, void *dummy, const char *arg)
{
    long n = strtol(arg, NULL, 10); (void)dummy;
    if (n < 1 || n > 100000) return "TodoTxtMaxBatchItems must be 1..100000";
    todotxt_get_conf(cmd->server)->max_batch_items = (int)n;
    return NULL;
}

static const command_rec cmds[] = {
    AP_INIT_FLAG("TodoTxtEngine", cmd_engine, NULL, RSRC_CONF, "Enable mod_todotxt"),
    AP_INIT_FLAG("TodoTxtAllowAnonymous", cmd_anon, NULL, RSRC_CONF, "Allow unauthenticated shared anonymous owner"),
    AP_INIT_FLAG("TodoTxtReadOnly", cmd_ro, NULL, RSRC_CONF, "Disable writes"),
    AP_INIT_FLAG("TodoTxtDueExtension", cmd_due, NULL, RSRC_CONF, "Enable due:YYYY-MM-DD"),
    AP_INIT_FLAG("TodoTxtRecurrenceExtension", cmd_rec, NULL, RSRC_CONF, "Enable rec:[+]N[d|b|w|m|y]"),
    AP_INIT_FLAG("TodoTxtThresholdExtension", cmd_threshold, NULL, RSRC_CONF, "Enable t:YYYY-MM-DD"),
    AP_INIT_FLAG("TodoTxtCreateNextOnComplete", cmd_next, NULL, RSRC_CONF, "Create next recurring item when completed"),
    AP_INIT_FLAG("TodoTxtIdExtension", cmd_id_extension, NULL, RSRC_CONF, "Expose the server database ID as a todo.txt extension"),
    AP_INIT_TAKE1("TodoTxtIdExtensionField", cmd_id_extension_field, NULL, RSRC_CONF, "Single extension field name used for server IDs; default id"),
    AP_INIT_TAKE1("TodoTxtBasePath", cmd_base, NULL, RSRC_CONF, "URL base path"),
    AP_INIT_TAKE1("TodoTxtStore", cmd_store, NULL, RSRC_CONF, "sqlite:/path/to/file.db"),
    AP_INIT_TAKE1("TodoTxtAnonymousUser", cmd_anon_user, NULL, RSRC_CONF, "Owner name for anonymous access"),
    AP_INIT_TAKE1("TodoTxtMaxRequestBytes", cmd_max_bytes, NULL, RSRC_CONF, "Maximum POST/PUT/PATCH body"),
    AP_INIT_TAKE1("TodoTxtMaxBatchItems", cmd_max_batch, NULL, RSRC_CONF, "Maximum batch items"),
    { NULL }
};

static int handler(request_rec *r) { return todotxt_api_handler(r); }

static int post_config(apr_pool_t *pconf, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    server_rec *srv; (void)plog; (void)ptemp;
    for (srv=s; srv; srv=srv->next) {
        todotxt_server_conf *conf = todotxt_get_conf(srv);
        if (!conf || !conf->enabled) continue;

        ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, srv,
            "mod_todotxt: post_config version='%s' server='%s' base_path='%s' db='%s' provider='%s' "
            "due=%d rec=%d threshold=%d id_extension=%d id_field='%s'",
            MOD_TODOTXT_VERSION,
            srv->server_hostname ? srv->server_hostname : "(default)",
            conf->base_path ? conf->base_path : "(null)",
            conf->db_path ? conf->db_path : "(null)",
            conf->storage_provider ? conf->storage_provider : "(null)",
            conf->enable_due, conf->enable_rec, conf->enable_threshold,
            conf->expose_id_extension,
            conf->id_extension_field ? conf->id_extension_field : "(null)");

        if (!strcmp(conf->storage_provider, "sqlite")) {
            apr_status_t rv = todotxt_sqlite_init_server(pconf, srv);
            if (rv != APR_SUCCESS) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, srv,
                    "mod_todotxt: failed to initialize SQLite storage server='%s' db='%s'",
                    srv->server_hostname ? srv->server_hostname : "(default)",
                    conf->db_path ? conf->db_path : "(null)");
                return HTTP_INTERNAL_SERVER_ERROR;
            }
        }
    }
    return OK;
}

static void register_hooks(apr_pool_t *p)
{
    apr_status_t rv = todotxt_sqlite_register_provider(p);
    if (rv != APR_SUCCESS) {
        ap_log_perror(APLOG_MARK, APLOG_CRIT, rv, p,
            "mod_todotxt: failed to register SQLite provider group='%s' name='sqlite' version='%s'",
            TODOTXT_STORAGE_PROVIDER_GROUP, TODOTXT_STORAGE_PROVIDER_VERSION);
    } else {
        ap_log_perror(APLOG_MARK, APLOG_DEBUG, 0, p,
            "mod_todotxt: registered provider group='%s' name='sqlite' version='%s'",
            TODOTXT_STORAGE_PROVIDER_GROUP, TODOTXT_STORAGE_PROVIDER_VERSION);
    }
    ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_handler(handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA todotxt_module = {
    STANDARD20_MODULE_STUFF,
    NULL, NULL, create_server_conf, NULL, cmds, register_hooks
};
