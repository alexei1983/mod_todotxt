#include "todotxt_internal.h"
#include "apr_strings.h"

#include <string.h>
#include <ctype.h>

typedef struct {
    const char *p;
    const char *end;
    apr_pool_t *pool;
} json_cursor;

static void skip_ws(json_cursor *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n'))
        ++c->p;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

static char *append_utf8(char *d, unsigned cp)
{
    if (cp <= 0x7F) {
        *d++ = (char)cp;
    } else if (cp <= 0x7FF) {
        *d++ = (char)(0xC0 | (cp >> 6));
        *d++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *d++ = (char)(0xE0 | (cp >> 12));
        *d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *d++ = (char)(0x80 | (cp & 0x3F));
    }
    return d;
}

static apr_status_t parse_json_string(json_cursor *c, const char **value_out, const char **error)
{
    apr_size_t max_len;
    char *out, *d;

    skip_ws(c);
    if (c->p >= c->end || *c->p != '"') {
        if (error) *error = "expected JSON string";
        return APR_EINVAL;
    }

    ++c->p;
    max_len = (apr_size_t)(c->end - c->p) * 3 + 1;
    out = apr_palloc(c->pool, max_len);
    d = out;

    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;

        if (ch == '"') {
            *d = '\0';
            *value_out = out;
            return APR_SUCCESS;
        }

        if (ch < 0x20) {
            if (error) *error = "unescaped control character in JSON string";
            return APR_EINVAL;
        }

        if (ch != '\\') {
            *d++ = (char)ch;
            continue;
        }

        if (c->p >= c->end) {
            if (error) *error = "incomplete JSON escape";
            return APR_EINVAL;
        }

        ch = (unsigned char)*c->p++;
        switch (ch) {
            case '"': *d++='"'; break;
            case '\\': *d++='\\'; break;
            case '/': *d++='/'; break;
            case 'b': *d++='\b'; break;
            case 'f': *d++='\f'; break;
            case 'n': *d++='\n'; break;
            case 'r': *d++='\r'; break;
            case 't': *d++='\t'; break;
            case 'u': {
                int a,b,e,f;
                unsigned cp;
                if (c->end - c->p < 4) {
                    if (error) *error = "incomplete \\u escape";
                    return APR_EINVAL;
                }
                a=hex_value(c->p[0]); b=hex_value(c->p[1]);
                e=hex_value(c->p[2]); f=hex_value(c->p[3]);
                if (a<0||b<0||e<0||f<0) {
                    if (error) *error = "invalid \\u escape";
                    return APR_EINVAL;
                }
                cp=((unsigned)a<<12)|((unsigned)b<<8)|((unsigned)e<<4)|(unsigned)f;
                c->p += 4;
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    if (error) *error = "UTF-16 surrogate escapes are not supported; send UTF-8 directly";
                    return APR_EINVAL;
                }
                d=append_utf8(d,cp);
                break;
            }
            default:
                if (error) *error = "invalid JSON escape";
                return APR_EINVAL;
        }
    }

    if (error) *error = "unterminated JSON string";
    return APR_EINVAL;
}

static apr_status_t parse_string_or_null(json_cursor *c, const char **value_out, const char **error)
{
    skip_ws(c);
    if ((c->end-c->p)>=4 && !strncmp(c->p,"null",4)) {
        c->p += 4;
        *value_out = NULL;
        return APR_SUCCESS;
    }
    return parse_json_string(c,value_out,error);
}

static apr_status_t assign_patch(todotxt_patch *patch, const char *name, const char *value, const char **error)
{
    if (!strcmp(name,"priority")) { patch->has_priority=1; patch->priority=value; }
    else if (!strcmp(name,"due")) { patch->has_due=1; patch->due=value; }
    else if (!strcmp(name,"threshold") || !strcmp(name,"t")) { patch->has_threshold=1; patch->threshold=value; }
    else if (!strcmp(name,"rec") || !strcmp(name,"recurrence")) { patch->has_recurrence=1; patch->recurrence=value; }
    else if (!strcmp(name,"projects")) { patch->has_projects=1; patch->projects=value; }
    else if (!strcmp(name,"contexts")) { patch->has_contexts=1; patch->contexts=value; }
    else if (!strcmp(name,"creation_date")) { patch->has_creation_date=1; patch->creation_date=value; }
    else {
        if (error) *error = "unsupported PATCH field";
        return APR_EINVAL;
    }
    return APR_SUCCESS;
}

apr_status_t todotxt_parse_patch_json(
    apr_pool_t *p, const char *body, apr_size_t len,
    todotxt_patch **patch_out, const char **error)
{
    json_cursor c;
    todotxt_patch *patch=apr_pcalloc(p,sizeof(*patch));
    int fields=0;

    c.p=body; c.end=body+len; c.pool=p;
    skip_ws(&c);
    if (c.p>=c.end || *c.p!='{') {
        if (error) *error="PATCH body must be a JSON object";
        return APR_EINVAL;
    }
    ++c.p; skip_ws(&c);
    if (c.p<c.end && *c.p=='}') {
        if (error) *error="PATCH object must contain at least one field";
        return APR_EINVAL;
    }

    while (c.p<c.end) {
        const char *name=NULL,*value=NULL;
        apr_status_t rv=parse_json_string(&c,&name,error);
        if (rv!=APR_SUCCESS) return rv;
        skip_ws(&c);
        if (c.p>=c.end || *c.p!=':') {
            if (error) *error="expected ':' after PATCH field name";
            return APR_EINVAL;
        }
        ++c.p;
        rv=parse_string_or_null(&c,&value,error);
        if (rv!=APR_SUCCESS) return rv;
        rv=assign_patch(patch,name,value,error);
        if (rv!=APR_SUCCESS) return rv;
        ++fields; skip_ws(&c);
        if (c.p>=c.end) {
            if (error) *error="unterminated PATCH JSON object";
            return APR_EINVAL;
        }
        if (*c.p=='}') { ++c.p; break; }
        if (*c.p!=',') {
            if (error) *error="expected ',' or '}' in PATCH object";
            return APR_EINVAL;
        }
        ++c.p; skip_ws(&c);
    }

    skip_ws(&c);
    if (c.p!=c.end) {
        if (error) *error="unexpected data after PATCH JSON object";
        return APR_EINVAL;
    }
    if (!fields) {
        if (error) *error="PATCH object must contain at least one field";
        return APR_EINVAL;
    }

    if (patch->has_priority && patch->priority &&
        !(strlen(patch->priority)==1 && patch->priority[0]>='A' && patch->priority[0]<='Z')) {
        if (error) *error="priority must be A-Z or null";
        return APR_EINVAL;
    }
    if (patch->has_due && patch->due && !todotxt_is_date(patch->due)) {
        if (error) *error="due must be YYYY-MM-DD or null";
        return APR_EINVAL;
    }
    if (patch->has_threshold && patch->threshold && !todotxt_is_date(patch->threshold)) {
        if (error) *error="threshold must be YYYY-MM-DD or null";
        return APR_EINVAL;
    }
    if (patch->has_creation_date && patch->creation_date && !todotxt_is_date(patch->creation_date)) {
        if (error) *error="creation_date must be YYYY-MM-DD or null";
        return APR_EINVAL;
    }
    if (patch->has_recurrence && patch->recurrence && !todotxt_is_recurrence(patch->recurrence)) {
        if (error) *error="recurrence must be [+]N[d|w|m|y] or null";
        return APR_EINVAL;
    }

    *patch_out=patch;
    return APR_SUCCESS;
}

static int priority_token(const char *s)
{
    return s && strlen(s)==3 && s[0]=='(' && s[2]==')' && s[1]>='A' && s[1]<='Z';
}

static void add_token(apr_pool_t *p, const char **out, int *first, const char *token)
{
    if (!token || !*token) return;
    *out=*first ? apr_pstrdup(p,token) : apr_pstrcat(p,*out," ",token,NULL);
    *first=0;
}

static void add_extensions(apr_pool_t *p, const char **out, int *first, const char *value, char prefix)
{
    char *copy,*save=NULL,*tok;
    if (!value || !*value) return;
    copy=apr_pstrdup(p,value);
    for (tok=apr_strtok(copy," ,;\t",&save); tok; tok=apr_strtok(NULL," ,;\t",&save)) {
        const char *clean=tok;
        if (*clean==prefix) ++clean;
        if (*clean) add_token(p,out,first,apr_psprintf(p,"%c%s",prefix,clean));
    }
}

apr_status_t todotxt_apply_patch(
    apr_pool_t *p, const todotxt_server_conf *conf, const char *owner,
    const todotxt_item *original, const todotxt_patch *patch,
    todotxt_item **updated_out, const char **error)
{
    const char *base,*out="";
    char *copy,*save=NULL,*tok;
    apr_array_header_t *tokens;
    int pos=0,i,first=1;
    todotxt_item *parsed=NULL;
    apr_status_t rv;

    if (!original || !original->raw || !patch) return APR_EINVAL;

    base=todotxt_strip_server_id_extension(p,conf,original->raw);
    copy=apr_pstrdup(p,base);
    tokens=apr_array_make(p,16,sizeof(const char*));
    for (tok=apr_strtok(copy," \t",&save); tok; tok=apr_strtok(NULL," \t",&save))
        APR_ARRAY_PUSH(tokens,const char*)=apr_pstrdup(p,tok);

    if (pos<tokens->nelts && !strcmp(APR_ARRAY_IDX(tokens,pos,const char*),"x")) {
        add_token(p,&out,&first,APR_ARRAY_IDX(tokens,pos,const char*)); ++pos;
        if (pos<tokens->nelts && todotxt_is_date(APR_ARRAY_IDX(tokens,pos,const char*))) {
            add_token(p,&out,&first,APR_ARRAY_IDX(tokens,pos,const char*)); ++pos;
        }
    }

    if (pos<tokens->nelts && priority_token(APR_ARRAY_IDX(tokens,pos,const char*))) {
        if (!patch->has_priority) add_token(p,&out,&first,APR_ARRAY_IDX(tokens,pos,const char*));
        ++pos;
    }
    if (patch->has_priority && patch->priority)
        add_token(p,&out,&first,apr_psprintf(p,"(%s)",patch->priority));

    if (pos<tokens->nelts && todotxt_is_date(APR_ARRAY_IDX(tokens,pos,const char*))) {
        if (!patch->has_creation_date) add_token(p,&out,&first,APR_ARRAY_IDX(tokens,pos,const char*));
        ++pos;
    }
    if (patch->has_creation_date && patch->creation_date)
        add_token(p,&out,&first,patch->creation_date);

    for (i=pos; i<tokens->nelts; ++i) {
        const char *t=APR_ARRAY_IDX(tokens,i,const char*);
        if (patch->has_due && !strncmp(t,"due:",4)) continue;
        if (patch->has_threshold && !strncmp(t,"t:",2)) continue;
        if (patch->has_recurrence && !strncmp(t,"rec:",4)) continue;
        if (patch->has_projects && t[0]=='+' && t[1]) continue;
        if (patch->has_contexts && t[0]=='@' && t[1]) continue;
        add_token(p,&out,&first,t);
    }

    if (patch->has_projects) add_extensions(p,&out,&first,patch->projects,'+');
    if (patch->has_contexts) add_extensions(p,&out,&first,patch->contexts,'@');
    if (patch->has_due && patch->due) add_token(p,&out,&first,apr_pstrcat(p,"due:",patch->due,NULL));
    if (patch->has_recurrence && patch->recurrence) add_token(p,&out,&first,apr_pstrcat(p,"rec:",patch->recurrence,NULL));
    if (patch->has_threshold && patch->threshold) add_token(p,&out,&first,apr_pstrcat(p,"t:",patch->threshold,NULL));

    rv=todotxt_parse_line(p,conf,owner,out,&parsed,error);
    if (rv!=APR_SUCCESS) return rv;

    parsed->id=original->id;
    parsed->revision=original->revision;
    parsed->created_at=original->created_at;
    *updated_out=parsed;
    return APR_SUCCESS;
}
