#include "todotxt_internal.h"

#include "http_request.h"
#include "http_log.h"
#include "apr_strings.h"
#include "util_script.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct {
    const char *status;
    const char *project;
    const char *context;

    char priority;
    char priority_at_or_above;
    char priority_at_or_below;

    const char *due;
    const char *due_before;
    const char *due_after;
    const char *due_on_or_before;
    const char *due_on_or_after;

    const char *threshold;
    const char *threshold_before;
    const char *threshold_after;
    const char *threshold_on_or_before;
    const char *threshold_on_or_after;

    int visible;
} todotxt_filters;

static void log_500(request_rec *r,const char *operation,apr_status_t rv,const char *detail)
{
    todotxt_server_conf *conf=todotxt_get_conf(r->server);
    ap_log_rerror(APLOG_MARK,APLOG_ERR,rv,r,
        "mod_todotxt: HTTP 500: %s; detail='%s'; uri='%s' method='%s' user='%s' db='%s'",
        operation?operation:"(unknown operation)",
        detail?detail:"(none)",
        r->uri?r->uri:"(null)",
        r->method?r->method:"(null)",
        r->user?r->user:"(null)",
        conf&&conf->db_path?conf->db_path:"(null)");
}

static const char *request_content_type(request_rec *r)
{
    return apr_table_get(r->headers_in,"Content-Type");
}

static int request_media_type_is(request_rec *r,const char *expected)
{
    const char *ct=request_content_type(r);
    size_t n=strlen(expected);
    if (!ct) return 0;
    while (*ct==' '||*ct=='\t') ++ct;
    if (strncasecmp(ct,expected,n)) return 0;
    ct+=n;
    return *ct=='\0'||*ct==';'||*ct==' '||*ct=='\t';
}

static int request_is_text_plain(request_rec *r)
{
    return request_media_type_is(r,"text/plain");
}

static int request_is_json(request_rec *r)
{
    return request_media_type_is(r,"application/json");
}

static const char *owner_for(request_rec *r,const todotxt_server_conf *conf)
{
    if (r->user&&*r->user) return r->user;
    if (conf->allow_anonymous) return conf->anonymous_user;
    return NULL;
}

static int read_body(request_rec *r,apr_size_t max,char **out,apr_size_t *len)
{
    char tmp[8192],*buf;
    apr_size_t cap=8192,used=0;
    int n;

    if (ap_setup_client_block(r,REQUEST_CHUNKED_ERROR)!=OK)
        return HTTP_BAD_REQUEST;

    buf=apr_palloc(r->pool,cap);

    while ((n=ap_get_client_block(r,tmp,sizeof(tmp)))>0) {
        if (used+(apr_size_t)n>max)
            return HTTP_REQUEST_ENTITY_TOO_LARGE;

        if (used+(apr_size_t)n+1>cap) {
            apr_size_t nc=cap*2;
            char *nb;
            while (nc<used+(apr_size_t)n+1) nc*=2;
            nb=apr_palloc(r->pool,nc);
            memcpy(nb,buf,used);
            buf=nb;
            cap=nc;
        }
        memcpy(buf+used,tmp,(apr_size_t)n);
        used+=(apr_size_t)n;
    }

    if (n<0) return HTTP_BAD_REQUEST;
    buf[used]='\0';
    *out=buf;
    *len=used;
    return OK;
}

static const char *json_escape(apr_pool_t *p,const char *s)
{
    const unsigned char *q;
    apr_size_t n=2;
    char *out,*d;

    if (!s) return "\"\"";

    for (q=(const unsigned char*)s; *q; ++q) {
        switch (*q) {
            case '"': case '\\': case '\b': case '\f':
            case '\n': case '\r': case '\t':
                n+=2; break;
            default:
                n+=(*q<0x20)?6:1; break;
        }
    }

    out=apr_palloc(p,n+1);
    d=out; *d++='"';

    for (q=(const unsigned char*)s; *q; ++q) {
        switch (*q) {
            case '"': *d++='\\';*d++='"';break;
            case '\\':*d++='\\';*d++='\\';break;
            case '\b':*d++='\\';*d++='b';break;
            case '\f':*d++='\\';*d++='f';break;
            case '\n':*d++='\\';*d++='n';break;
            case '\r':*d++='\\';*d++='r';break;
            case '\t':*d++='\\';*d++='t';break;
            default:
                if (*q<0x20) {
                    apr_snprintf(d,7,"\\u%04x",(unsigned)*q);
                    d+=6;
                } else *d++=(char)*q;
                break;
        }
    }
    *d++='"'; *d='\0';
    return out;
}

static void json_begin(request_rec *r,int status)
{
    r->status=status;
    ap_set_content_type(r,"application/json; charset=utf-8");
}

static const char *item_etag(apr_pool_t *p,apr_int64_t id,apr_int64_t revision)
{
    return apr_psprintf(p,"\"todo-%" APR_INT64_T_FMT "-%" APR_INT64_T_FMT "\"",id,revision);
}

static void set_item_etag(request_rec *r,apr_int64_t id,apr_int64_t revision)
{
    apr_table_setn(r->headers_out,"ETag",item_etag(r->pool,id,revision));
}

static void json_write_item(request_rec *r,const todotxt_server_conf *conf,const todotxt_item *item)
{
    const char *etag=item_etag(r->pool,item->id,item->revision);

    ap_rprintf(r,
        "{\"id\":%" APR_INT64_T_FMT ",\"revision\":%" APR_INT64_T_FMT ","
        "\"etag\":%s,\"raw\":%s,\"completed\":%s",
        item->id,item->revision,
        json_escape(r->pool,etag),
        json_escape(r->pool,item->raw),
        item->completed?"true":"false");

    if (conf->expose_id_extension)
        ap_rprintf(r,",\"todo_txt\":%s",
            json_escape(r->pool,todotxt_render_line(r->pool,conf,item)));

    if (item->priority) {
        char pbuf[2]={item->priority,'\0'};
        ap_rprintf(r,",\"priority\":%s",json_escape(r->pool,pbuf));
    }
    if (item->completion_date)
        ap_rprintf(r,",\"completion_date\":%s",json_escape(r->pool,item->completion_date));
    if (item->creation_date)
        ap_rprintf(r,",\"creation_date\":%s",json_escape(r->pool,item->creation_date));
    if (item->due_date)
        ap_rprintf(r,",\"due\":%s",json_escape(r->pool,item->due_date));
    if (item->threshold_date)
        ap_rprintf(r,",\"threshold\":%s",json_escape(r->pool,item->threshold_date));
    if (item->recurrence)
        ap_rprintf(r,",\"rec\":%s",json_escape(r->pool,item->recurrence));
    if (item->projects)
        ap_rprintf(r,",\"projects\":%s",json_escape(r->pool,item->projects));
    if (item->contexts)
        ap_rprintf(r,",\"contexts\":%s",json_escape(r->pool,item->contexts));

    ap_rputs("}",r);
}

static int get_storage_or_500(request_rec *r,const todotxt_storage_provider **sp_out)
{
    const todotxt_storage_provider *sp=todotxt_storage(r->server);
    if (!sp) {
        log_500(r,"storage provider lookup",0,"provider is NULL");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    *sp_out=sp;
    return OK;
}

static int bool_arg(const char *v)
{
    return v && (!strcasecmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"yes")||!strcasecmp(v,"on"));
}

static int valid_priority_arg(const char *v)
{
    return v && strlen(v)==1 && v[0]>='A' && v[0]<='Z';
}

static int validate_date_arg(const char *v)
{
    return !v || todotxt_is_date(v);
}

static int parse_filters(request_rec *r,todotxt_filters *f,const char **error)
{
    apr_table_t *args=NULL;
    const char *v;

    memset(f,0,sizeof(*f));
    f->status="active";
    ap_args_to_table(r,&args);

    if (!args) return OK;

    v=apr_table_get(args,"status");
    if (v) f->status=v;

    if (strcmp(f->status,"active")&&strcmp(f->status,"completed")&&strcmp(f->status,"all")) {
        if (error) *error="status must be active, completed, or all";
        return HTTP_BAD_REQUEST;
    }

    f->project=apr_table_get(args,"project");
    f->context=apr_table_get(args,"context");

    v=apr_table_get(args,"priority");
    if (v) {
        if (!valid_priority_arg(v)) { if(error)*error="priority must be A-Z"; return HTTP_BAD_REQUEST; }
        f->priority=v[0];
    }

    v=apr_table_get(args,"priority_at_or_above");
    if (v) {
        if (!valid_priority_arg(v)) { if(error)*error="priority_at_or_above must be A-Z"; return HTTP_BAD_REQUEST; }
        f->priority_at_or_above=v[0];
    }

    v=apr_table_get(args,"priority_at_or_below");
    if (v) {
        if (!valid_priority_arg(v)) { if(error)*error="priority_at_or_below must be A-Z"; return HTTP_BAD_REQUEST; }
        f->priority_at_or_below=v[0];
    }

#define DATE_ARG(field,name) \
    do { f->field=apr_table_get(args,name); \
         if (!validate_date_arg(f->field)) { if(error)*error=name " must be YYYY-MM-DD"; return HTTP_BAD_REQUEST; } \
    } while(0)

    DATE_ARG(due,"due");
    DATE_ARG(due_before,"due_before");
    DATE_ARG(due_after,"due_after");
    DATE_ARG(due_on_or_before,"due_on_or_before");
    DATE_ARG(due_on_or_after,"due_on_or_after");
    DATE_ARG(threshold,"threshold");
    DATE_ARG(threshold_before,"threshold_before");
    DATE_ARG(threshold_after,"threshold_after");
    DATE_ARG(threshold_on_or_before,"threshold_on_or_before");
    DATE_ARG(threshold_on_or_after,"threshold_on_or_after");
#undef DATE_ARG

    f->visible=bool_arg(apr_table_get(args,"visible"));
    return OK;
}

static int token_list_contains(const char *list,const char *wanted)
{
    size_t n;
    const char *s;

    if (!list || !wanted || !*wanted)
        return 0;

    n=strlen(wanted);
    s=list;

    while (*s) {
        const char *e;

        while (*s==' '||*s=='\t')
            ++s;

        if (!*s)
            break;

        e=s;
        while (*e && *e!=' ' && *e!='\t')
            ++e;

        if ((size_t)(e-s)==n &&
            !strncasecmp(s,wanted,n))
            return 1;

        s=e;
    }

    return 0;
}

static const char *today_string(apr_pool_t *p)
{
    apr_time_exp_t x;
    apr_time_exp_lt(&x,apr_time_now());
    return apr_psprintf(p,"%04d-%02d-%02d",x.tm_year+1900,x.tm_mon+1,x.tm_mday);
}

static int date_matches(const char *value,const char *exact,const char *before,const char *after,
                        const char *on_or_before,const char *on_or_after)
{
    if (!exact&&!before&&!after&&!on_or_before&&!on_or_after) return 1;
    if (!value) return 0;
    if (exact && strcmp(value,exact)) return 0;
    if (before && strcmp(value,before)>=0) return 0;
    if (after && strcmp(value,after)<=0) return 0;
    if (on_or_before && strcmp(value,on_or_before)>0) return 0;
    if (on_or_after && strcmp(value,on_or_after)<0) return 0;
    return 1;
}

static int item_matches_filters(request_rec *r,const todotxt_item *item,const todotxt_filters *f)
{
    if (f->project && !token_list_contains(item->projects,f->project)) return 0;
    if (f->context && !token_list_contains(item->contexts,f->context)) return 0;

    if (f->priority) {
        if (!item->priority || item->priority!=f->priority) return 0;
    }
    if (f->priority_at_or_above) {
        /* A is the highest priority, so A/B are at-or-above C. */
        if (!item->priority || item->priority>f->priority_at_or_above) return 0;
    }
    if (f->priority_at_or_below) {
        if (!item->priority || item->priority<f->priority_at_or_below) return 0;
    }

    if (!date_matches(item->due_date,f->due,f->due_before,f->due_after,
                      f->due_on_or_before,f->due_on_or_after)) return 0;

    if (!date_matches(item->threshold_date,f->threshold,f->threshold_before,f->threshold_after,
                      f->threshold_on_or_before,f->threshold_on_or_after)) return 0;

    if (f->visible && item->threshold_date &&
        strcmp(item->threshold_date,today_string(r->pool))>0) return 0;

    return 1;
}

/*
 * If-Match parser. v2 emits strong ETags of the form:
 *   "todo-123-7"
 *
 * One tag or '*' is accepted. Weak tags and comma-separated lists are
 * intentionally rejected rather than ambiguously handled.
 */
static int parse_if_match(request_rec *r,apr_int64_t target_id,
                          int *present,int *wildcard,apr_int64_t *revision)
{
    const char *h=apr_table_get(r->headers_in,"If-Match");
    char *v,*p,*end;
    apr_int64_t id,rev;

    *present=0; *wildcard=0; *revision=0;
    if (!h) return OK;

    *present=1;
    while (*h==' '||*h=='\t') ++h;
    if (!strcmp(h,"*")) { *wildcard=1; return OK; }
    if (!strncmp(h,"W/",2)) return HTTP_PRECONDITION_FAILED;
    if (strchr(h,',')) return HTTP_BAD_REQUEST;

    v=apr_pstrdup(r->pool,h);
    p=v;
    while (*p==' '||*p=='\t') ++p;
    if (*p!='"') return HTTP_BAD_REQUEST;
    ++p;
    if (strncmp(p,"todo-",5)) return HTTP_PRECONDITION_FAILED;
    p+=5;

    id=apr_strtoi64(p,&end,10);
    if (end==p || *end!='-') return HTTP_BAD_REQUEST;
    p=end+1;
    rev=apr_strtoi64(p,&end,10);
    if (end==p || *end!='"') return HTTP_BAD_REQUEST;
    ++end;
    while (*end==' '||*end=='\t') ++end;
    if (*end!='\0') return HTTP_BAD_REQUEST;

    if (id!=target_id || rev<=0) return HTTP_PRECONDITION_FAILED;
    *revision=rev;
    return OK;
}

static int conflict_status(int if_match_present)
{
    return if_match_present ? HTTP_PRECONDITION_FAILED : HTTP_CONFLICT;
}

static int post_batch(request_rec *r,const todotxt_server_conf *conf,const char *owner)
{
    const todotxt_storage_provider *sp;
    char *body=NULL,*save=NULL,*line;
    apr_size_t len=0;
    int rc,count=0,first=1;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    if (conf->read_only) return HTTP_FORBIDDEN;
    if (!request_is_text_plain(r)) return HTTP_UNSUPPORTED_MEDIA_TYPE;
    rc=read_body(r,conf->max_request_bytes,&body,&len); if (rc!=OK) return rc;

    json_begin(r,HTTP_CREATED);
    ap_rputs("[",r);

    for (line=apr_strtok(body,"\r\n",&save); line; line=apr_strtok(NULL,"\r\n",&save)) {
        todotxt_item *item=NULL;
        const char *err=NULL,*canonical;
        apr_int64_t id=0;
        apr_status_t rv;

        while (*line==' '||*line=='\t') ++line;
        if (!*line) continue;
        if (++count>conf->max_batch_items) return HTTP_REQUEST_ENTITY_TOO_LARGE;

        if (!first) ap_rputs(",",r);
        first=0;

        canonical=todotxt_strip_server_id_extension(r->pool,conf,line);

        if (todotxt_parse_line(r->pool,conf,owner,canonical,&item,&err)!=APR_SUCCESS) {
            ap_rprintf(r,"{\"line\":%d,\"error\":%s}",count,
                       json_escape(r->pool,err?err:"invalid todo"));
            continue;
        }

        rv=sp->insert(r->pool,r->server,conf->db_path,item,&id);
        if (rv!=APR_SUCCESS) {
            log_500(r,"POST todo.txt insert",rv,canonical);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        ap_rprintf(r,
            "{\"line\":%d,\"id\":%" APR_INT64_T_FMT ",\"revision\":1,\"etag\":%s}",
            count,id,json_escape(r->pool,item_etag(r->pool,id,1)));
    }

    ap_rputs("]\n",r);
    return OK;
}

static int list_items(request_rec *r,const todotxt_server_conf *conf,const char *owner,int json_mode)
{
    const todotxt_storage_provider *sp;
    apr_array_header_t *rows=NULL;
    todotxt_filters filters;
    const char *filter_error=NULL;
    apr_status_t rv;
    int rc,i,first=1;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    rc=parse_filters(r,&filters,&filter_error);
    if (rc!=OK) {
        ap_log_rerror(APLOG_MARK,APLOG_INFO,0,r,"mod_todotxt: filter rejected: %s",
                      filter_error?filter_error:"invalid filter");
        return rc;
    }

    rv=sp->list(r->pool,r->server,conf->db_path,owner,filters.status,&rows);
    if (rv!=APR_SUCCESS) {
        log_500(r,"GET list",rv,filters.status);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (json_mode) {
        json_begin(r,HTTP_OK);
        ap_rputs("[",r);
    } else {
        ap_set_content_type(r,"text/plain; charset=utf-8");
    }

    for (i=0;i<rows->nelts;++i) {
        todotxt_item *item=APR_ARRAY_IDX(rows,i,todotxt_item*);
        if (!item_matches_filters(r,item,&filters)) continue;

        if (json_mode) {
            if (!first) ap_rputs(",",r);
            first=0;
            json_write_item(r,conf,item);
        } else {
            ap_rputs(todotxt_render_line(r->pool,conf,item),r);
            ap_rputs("\n",r);
        }
    }

    if (json_mode) ap_rputs("]\n",r);
    return OK;
}

static int status_endpoint(request_rec *r,const todotxt_server_conf *conf,const char *owner)
{
    json_begin(r,HTTP_OK);
    ap_rprintf(r,
        "{\"module\":\"mod_todotxt\",\"version\":%s,\"user\":%s,"
        "\"anonymous\":%s,\"read_only\":%s,"
        "\"max_request_bytes\":%" APR_SIZE_T_FMT ",\"max_batch_items\":%d,"
        "\"create_next_on_complete\":%s,"
        "\"extensions\":{\"due\":%s,\"rec\":%s,\"threshold\":%s,"
        "\"server_id\":{\"enabled\":%s,\"field\":%s}}}\n",
        json_escape(r->pool,MOD_TODOTXT_VERSION),
        json_escape(r->pool,owner),
        (r->user&&*r->user)?"false":"true",
        conf->read_only?"true":"false",
        conf->max_request_bytes,
        conf->max_batch_items,
        conf->create_next_on_complete?"true":"false",
        conf->enable_due?"true":"false",
        conf->enable_rec?"true":"false",
        conf->enable_threshold?"true":"false",
        conf->expose_id_extension?"true":"false",
        json_escape(r->pool,conf->id_extension_field?conf->id_extension_field:"id"));
    return OK;
}

static int get_one(request_rec *r,const todotxt_server_conf *conf,const char *owner,apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    todotxt_item *item=NULL;
    apr_status_t rv;
    int rc;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    rv=sp->get(r->pool,r->server,conf->db_path,owner,id,&item);
    if (rv==APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv!=APR_SUCCESS) {
        log_500(r,"GET todo item",rv,"storage get failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    set_item_etag(r,item->id,item->revision);

    if (r->args && strstr(r->args,"format=json")) {
        json_begin(r,HTTP_OK);
        json_write_item(r,conf,item);
        ap_rputs("\n",r);
    } else {
        ap_set_content_type(r,"text/plain; charset=utf-8");
        ap_rputs(todotxt_render_line(r->pool,conf,item),r);
        ap_rputs("\n",r);
    }
    return OK;
}

static int put_one(request_rec *r,const todotxt_server_conf *conf,const char *owner,apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    char *body=NULL;
    apr_size_t len=0;
    todotxt_item *item=NULL;
    const char *err=NULL,*canonical;
    apr_status_t rv;
    apr_int64_t new_revision=0,expected=0;
    int rc,if_present,wildcard;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    if (conf->read_only) return HTTP_FORBIDDEN;
    if (!request_is_text_plain(r)) return HTTP_UNSUPPORTED_MEDIA_TYPE;

    rc=parse_if_match(r,id,&if_present,&wildcard,&expected);
    if (rc!=OK) return rc;
    if (wildcard) expected=0;

    rc=read_body(r,conf->max_request_bytes,&body,&len); if (rc!=OK) return rc;
    while (len&&(body[len-1]=='\r'||body[len-1]=='\n')) body[--len]='\0';
    if (strchr(body,'\n')||strchr(body,'\r')) return HTTP_BAD_REQUEST;

    canonical=todotxt_strip_server_id_extension(r->pool,conf,body);
    if (todotxt_parse_line(r->pool,conf,owner,canonical,&item,&err)!=APR_SUCCESS)
        return HTTP_BAD_REQUEST;

    item->id=id;
    rv=sp->update(r->pool,r->server,conf->db_path,item,expected,&new_revision);
    if (rv==APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv==APR_EBUSY) return HTTP_PRECONDITION_FAILED;
    if (rv!=APR_SUCCESS) {
        log_500(r,"PUT todo item update",rv,canonical);
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    set_item_etag(r,id,new_revision);
    r->status=HTTP_NO_CONTENT;
    return OK;
}

static int patch_one(request_rec *r,const todotxt_server_conf *conf,const char *owner,apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    todotxt_item *old=NULL,*updated=NULL;
    todotxt_patch *patch=NULL;
    char *body=NULL;
    apr_size_t len=0;
    const char *err=NULL;
    apr_status_t rv;
    apr_int64_t expected=0,new_revision=0,update_expected;
    int rc,if_present,wildcard;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    if (conf->read_only) return HTTP_FORBIDDEN;
    if (!request_is_json(r)) return HTTP_UNSUPPORTED_MEDIA_TYPE;

    rc=parse_if_match(r,id,&if_present,&wildcard,&expected);
    if (rc!=OK) return rc;

    rv=sp->get(r->pool,r->server,conf->db_path,owner,id,&old);
    if (rv==APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv!=APR_SUCCESS) {
        log_500(r,"PATCH get",rv,"storage get failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (if_present && !wildcard && expected!=old->revision)
        return HTTP_PRECONDITION_FAILED;

    rc=read_body(r,conf->max_request_bytes,&body,&len); if (rc!=OK) return rc;
    rv=todotxt_parse_patch_json(r->pool,body,len,&patch,&err);
    if (rv!=APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK,APLOG_INFO,0,r,"mod_todotxt: PATCH rejected: %s",err?err:"invalid patch");
        return HTTP_BAD_REQUEST;
    }

    if ((patch->has_due && !conf->enable_due) ||
        (patch->has_threshold && !conf->enable_threshold) ||
        (patch->has_recurrence && !conf->enable_rec)) {

        ap_log_rerror(
            APLOG_MARK,
            APLOG_INFO,
            0,
            r,
            "mod_todotxt: PATCH rejected because requested extension is %s", "disabled");

        return HTTP_BAD_REQUEST;
    }

    rv=todotxt_apply_patch(r->pool,conf,owner,old,patch,&updated,&err);
    if (rv!=APR_SUCCESS) {
        ap_log_rerror(APLOG_MARK,APLOG_INFO,0,r,"mod_todotxt: PATCH apply rejected: %s",err?err:"invalid patch result");
        return HTTP_BAD_REQUEST;
    }

    update_expected = if_present && !wildcard ? expected : old->revision;
    rv=sp->update(r->pool,r->server,conf->db_path,updated,update_expected,&new_revision);
    if (rv==APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv==APR_EBUSY) return conflict_status(if_present);
    if (rv!=APR_SUCCESS) {
        log_500(r,"PATCH update",rv,"storage update failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    updated->revision=new_revision;
    set_item_etag(r,id,new_revision);
    json_begin(r,HTTP_OK);
    json_write_item(r,conf,updated);
    ap_rputs("\n",r);
    return OK;
}

static int delete_one(request_rec *r,const todotxt_server_conf *conf,const char *owner,apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    apr_status_t rv;
    apr_int64_t expected=0;
    int rc,if_present,wildcard;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    if (conf->read_only) return HTTP_FORBIDDEN;

    rc=parse_if_match(r,id,&if_present,&wildcard,&expected);
    if (rc!=OK) return rc;
    if (wildcard) expected=0;

    rv=sp->remove(r->pool,r->server,conf->db_path,owner,id,expected);
    if (rv==APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv==APR_EBUSY) return HTTP_PRECONDITION_FAILED;
    if (rv!=APR_SUCCESS) {
        log_500(r,"DELETE todo item",rv,"storage delete failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    r->status=HTTP_NO_CONTENT;
    return OK;
}

static int complete_one(request_rec *r,const todotxt_server_conf *conf,const char *owner,apr_int64_t id)
{
    const todotxt_storage_provider *sp;
    todotxt_item *old=NULL,*done=NULL,*next=NULL;
    apr_status_t rv;
    const char *line,*err=NULL,*next_line;
    apr_int64_t next_id=0,expected=0,new_revision=0,update_expected;
    int rc,if_present,wildcard;

    rc=get_storage_or_500(r,&sp); if (rc!=OK) return rc;
    if (conf->read_only) return HTTP_FORBIDDEN;

    rc=parse_if_match(r,id,&if_present,&wildcard,&expected);
    if (rc!=OK) return rc;

    rv=sp->get(r->pool,r->server,conf->db_path,owner,id,&old);
    if (rv==APR_NOTFOUND) return HTTP_NOT_FOUND;
    if (rv!=APR_SUCCESS) {
        log_500(r,"POST complete get",rv,"storage get failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    if (if_present && !wildcard && expected!=old->revision)
        return HTTP_PRECONDITION_FAILED;

    if (old->completed) {
        set_item_etag(r,old->id,old->revision);
        json_begin(r,HTTP_OK);
        json_write_item(r,conf,old);
        ap_rputs("\n",r);
        return OK;
    }

    line=todotxt_complete_line(r->pool,old->raw,apr_time_now());
    if (!line) {
        log_500(r,"POST complete build line",APR_EGENERAL,"todotxt_complete_line returned NULL");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    rv=todotxt_parse_line(r->pool,conf,owner,line,&done,&err);
    if (rv!=APR_SUCCESS) {
        log_500(r,"POST complete parse generated line",rv,err?err:"parse failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    done->id=id;

    update_expected=if_present&&!wildcard ? expected : old->revision;
    rv=sp->update(r->pool,r->server,conf->db_path,done,update_expected,&new_revision);
    if (rv==APR_EBUSY) return conflict_status(if_present);
    if (rv!=APR_SUCCESS) {
        log_500(r,"POST complete update",rv,"storage update failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    done->revision=new_revision;

    json_begin(r,HTTP_OK);
    set_item_etag(r,id,new_revision);
    ap_rprintf(r,"{\"completed_id\":%" APR_INT64_T_FMT ",\"revision\":%" APR_INT64_T_FMT ",\"etag\":%s",
        id,new_revision,json_escape(r->pool,item_etag(r->pool,id,new_revision)));

    if (conf->create_next_on_complete && done->recurrence) {
        next_line=todotxt_recurrence_next_line(r->pool,done,apr_time_now());
        if (!next_line) {
            ap_log_rerror(APLOG_MARK,APLOG_WARNING,0,r,
                "mod_todotxt: recurrence generation returned NULL id=%" APR_INT64_T_FMT " user='%s' rec='%s'",
                id,owner,done->recurrence);
        } else {
            rv=todotxt_parse_line(r->pool,conf,owner,next_line,&next,&err);
            if (rv!=APR_SUCCESS) {
                log_500(r,"POST complete parse recurrence",rv,err?err:"parse failed");
                return HTTP_INTERNAL_SERVER_ERROR;
            }
            rv=sp->insert(r->pool,r->server,conf->db_path,next,&next_id);
            if (rv!=APR_SUCCESS) {
                log_500(r,"POST complete insert recurrence",rv,next_line);
                return HTTP_INTERNAL_SERVER_ERROR;
            }
            ap_rprintf(r,",\"next_id\":%" APR_INT64_T_FMT ",\"next_revision\":1,\"next_etag\":%s,\"next\":%s",
                next_id,
                json_escape(r->pool,item_etag(r->pool,next_id,1)),
                json_escape(r->pool,next_line));
        }
    }

    ap_rputs("}\n",r);
    return OK;
}

int todotxt_api_handler(request_rec *r)
{
    todotxt_server_conf *conf=todotxt_get_conf(r->server);
    const char *owner,*root,*json_root,*status_path,*prefix,*p;
    apr_int64_t id;
    char *end=NULL;
    int json_mode=0;

    if (!conf || !conf->enabled) return DECLINED;

    root=!strcmp(conf->base_path,"/") ? "/todo.txt" :
        apr_pstrcat(r->pool,conf->base_path,"/todo.txt",NULL);
    json_root=!strcmp(conf->base_path,"/") ? "/todos.json" :
        apr_pstrcat(r->pool,conf->base_path,"/todos.json",NULL);
    status_path=!strcmp(conf->base_path,"/") ? "/todo/status" :
        apr_pstrcat(r->pool,conf->base_path,"/todo/status",NULL);
    prefix=!strcmp(conf->base_path,"/") ? "/todo/" :
        apr_pstrcat(r->pool,conf->base_path,"/todo/",NULL);

    if (strcmp(r->uri,root) && strcmp(r->uri,json_root) &&
        strcmp(r->uri,status_path) &&
        strncmp(r->uri,prefix,strlen(prefix)))
        return DECLINED;

    ap_log_rerror(APLOG_MARK,APLOG_DEBUG,0,r,
        "mod_todotxt: handler accepted request version='%s' uri='%s' method='%s' "
        "authenticated_user='%s' base_path='%s' db='%s' provider='%s'",
        MOD_TODOTXT_VERSION,r->uri?r->uri:"(null)",r->method?r->method:"(null)",
        r->user?r->user:"(null)",conf->base_path?conf->base_path:"(null)",
        conf->db_path?conf->db_path:"(null)",
        conf->storage_provider?conf->storage_provider:"(null)");

    owner=owner_for(r,conf);
    if (!owner) return HTTP_UNAUTHORIZED;

    if (!strcmp(r->uri,status_path)) {
        if (r->method_number!=M_GET) return HTTP_METHOD_NOT_ALLOWED;
        return status_endpoint(r,conf,owner);
    }

    if (!strcmp(r->uri,json_root)) {
        if (r->method_number!=M_GET) return HTTP_METHOD_NOT_ALLOWED;
        return list_items(r,conf,owner,1);
    }

    if (!strcmp(r->uri,root)) {
        if (r->method_number==M_GET) {
            apr_table_t *args=NULL;
            ap_args_to_table(r,&args);
            if (args) {
                const char *format=apr_table_get(args,"format");
                if (format && !strcasecmp(format,"json")) json_mode=1;
            }
            return list_items(r,conf,owner,json_mode);
        }
        if (r->method_number==M_POST) return post_batch(r,conf,owner);
        return HTTP_METHOD_NOT_ALLOWED;
    }

    p=r->uri+strlen(prefix);
    if (!*p) return HTTP_NOT_FOUND;

    id=apr_strtoi64(p,&end,10);
    if (end==p || id<=0) return HTTP_BAD_REQUEST;

    if (*end=='\0') {
        if (r->method_number==M_GET) return get_one(r,conf,owner,id);
        if (r->method_number==M_PUT) return put_one(r,conf,owner,id);
        if (!strcmp(r->method,"PATCH")) return patch_one(r,conf,owner,id);
        if (r->method_number==M_DELETE) return delete_one(r,conf,owner,id);
        return HTTP_METHOD_NOT_ALLOWED;
    }

    if (!strcmp(end,"/complete")) {
        if (r->method_number==M_POST) return complete_one(r,conf,owner,id);
        return HTTP_METHOD_NOT_ALLOWED;
    }

    return HTTP_NOT_FOUND;
}
