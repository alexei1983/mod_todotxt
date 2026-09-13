#include "todotxt_internal.h"
#include "ap_provider.h"
#include "http_log.h"
#include "apr_thread_mutex.h"
#include "apr_strings.h"
#include <sqlite3.h>
#include <string.h>

static apr_thread_mutex_t *g_mutex = NULL;

static void log_sqlite(server_rec *s,int level,sqlite3 *db,int rc,const char *db_path,const char *op)
{
    ap_log_error(APLOG_MARK,level,0,s,
        "mod_todotxt: SQLite failure: operation='%s' db='%s' sqlite_rc=%d sqlite_extended_rc=%d message='%s'",
        op?op:"(unknown)",db_path?db_path:"(null)",rc,
        db?sqlite3_extended_errcode(db):rc,
        db?sqlite3_errmsg(db):sqlite3_errstr(rc));
}

static apr_status_t open_db(server_rec *s,const char *db_path,sqlite3 **out)
{
    sqlite3 *db=NULL;
    int rc=sqlite3_open_v2(db_path,&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,NULL);
    if (rc!=SQLITE_OK) {
        log_sqlite(s,APLOG_ERR,db,rc,db_path,"sqlite3_open_v2");
        if (db) sqlite3_close(db);
        *out=NULL;
        return APR_EGENERAL;
    }
    sqlite3_extended_result_codes(db,1);
    sqlite3_busy_timeout(db,5000);
    *out=db;
    return APR_SUCCESS;
}

static apr_status_t exec_sql(server_rec *s,sqlite3 *db,const char *db_path,const char *sql,const char *op)
{
    char *err=NULL;
    int rc=sqlite3_exec(db,sql,NULL,NULL,&err);
    if (rc!=SQLITE_OK) {
        ap_log_error(APLOG_MARK,APLOG_ERR,0,s,
            "mod_todotxt: SQLite exec failure: operation='%s' db='%s' sqlite_rc=%d sqlite_extended_rc=%d message='%s'",
            op,db_path,rc,sqlite3_extended_errcode(db),err?err:sqlite3_errmsg(db));
        if (err) sqlite3_free(err);
        return APR_EGENERAL;
    }
    if (err) sqlite3_free(err);
    return APR_SUCCESS;
}

static apr_status_t ensure_revision_column(server_rec *s,sqlite3 *db,const char *db_path)
{
    sqlite3_stmt *st=NULL;
    int rc,found=0;

    rc=sqlite3_prepare_v2(db,"PRAGMA table_info(todos);",-1,&st,NULL);
    if (rc!=SQLITE_OK) {
        log_sqlite(s,APLOG_ERR,db,rc,db_path,"revision migration table_info");
        return APR_EGENERAL;
    }

    while ((rc=sqlite3_step(st))==SQLITE_ROW) {
        const unsigned char *name=sqlite3_column_text(st,1);
        if (name && !strcmp((const char*)name,"revision")) {
            found=1;
            break;
        }
    }
    sqlite3_finalize(st);

    if (rc!=SQLITE_ROW && rc!=SQLITE_DONE) {
        log_sqlite(s,APLOG_ERR,db,rc,db_path,"revision migration table_info step");
        return APR_EGENERAL;
    }
    if (found) return APR_SUCCESS;

    ap_log_error(APLOG_MARK,APLOG_NOTICE,0,s,
        "mod_todotxt: upgrading database '%s': adding revision column",db_path);

    return exec_sql(s,db,db_path,
        "ALTER TABLE todos ADD COLUMN revision INTEGER NOT NULL DEFAULT 1;",
        "add revision column");
}

static apr_status_t ensure_schema(server_rec *s,sqlite3 *db,const char *db_path)
{
    apr_status_t rv=exec_sql(s,db,db_path,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS todos("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "owner TEXT NOT NULL,"
        "raw TEXT NOT NULL,"
        "completed INTEGER NOT NULL DEFAULT 0,"
        "priority TEXT,"
        "completion_date TEXT,"
        "creation_date TEXT,"
        "due_date TEXT,"
        "threshold_date TEXT,"
        "recurrence TEXT,"
        "projects TEXT,"
        "contexts TEXT,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "revision INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE INDEX IF NOT EXISTS ix_todos_owner_completed ON todos(owner,completed,id);"
        "CREATE INDEX IF NOT EXISTS ix_todos_owner_due ON todos(owner,due_date);"
        "CREATE INDEX IF NOT EXISTS ix_todos_owner_threshold ON todos(owner,threshold_date);",
        "ensure schema");
    if (rv!=APR_SUCCESS) return rv;
    return ensure_revision_column(s,db,db_path);
}

static apr_int64_t now_seconds(void) { return (apr_int64_t)(apr_time_now()/APR_USEC_PER_SEC); }

static void bind_nullable(sqlite3_stmt *st,int idx,const char *v)
{
    if (v && *v) sqlite3_bind_text(st,idx,v,-1,SQLITE_TRANSIENT);
    else sqlite3_bind_null(st,idx);
}

static apr_status_t select_revision(server_rec *s,sqlite3 *db,const char *db_path,
                                    const char *owner,apr_int64_t id,apr_int64_t *revision_out)
{
    sqlite3_stmt *st=NULL;
    int rc=sqlite3_prepare_v2(db,"SELECT revision FROM todos WHERE owner=? AND id=?;",-1,&st,NULL);
    apr_status_t rv=APR_NOTFOUND;
    if (rc!=SQLITE_OK) {
        log_sqlite(s,APLOG_ERR,db,rc,db_path,"select revision prepare");
        return APR_EGENERAL;
    }
    sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(st,2,id);
    rc=sqlite3_step(st);
    if (rc==SQLITE_ROW) {
        *revision_out=sqlite3_column_int64(st,0);
        rv=APR_SUCCESS;
    } else if (rc==SQLITE_DONE) {
        rv=APR_NOTFOUND;
    } else {
        log_sqlite(s,APLOG_ERR,db,rc,db_path,"select revision step");
        rv=APR_EGENERAL;
    }
    sqlite3_finalize(st);
    return rv;
}

static apr_status_t sp_insert(apr_pool_t *p,server_rec *s,const char *db_path,
                              const todotxt_item *item,apr_int64_t *id_out)
{
    sqlite3 *db=NULL; sqlite3_stmt *st=NULL; apr_status_t rv=APR_EGENERAL; int rc;
    (void)p;
    if (g_mutex) apr_thread_mutex_lock(g_mutex);
    if (open_db(s,db_path,&db)!=APR_SUCCESS) goto done;

    rc=sqlite3_prepare_v2(db,
        "INSERT INTO todos(owner,raw,completed,priority,completion_date,creation_date,due_date,"
        "threshold_date,recurrence,projects,contexts,created_at,updated_at,revision)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,1);",-1,&st,NULL);
    if (rc!=SQLITE_OK) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"insert prepare"); goto done; }

    sqlite3_bind_text(st,1,item->owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(st,2,item->raw,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(st,3,item->completed);
    if (item->priority) { char b[2]={item->priority,0}; sqlite3_bind_text(st,4,b,-1,SQLITE_TRANSIENT); }
    else sqlite3_bind_null(st,4);
    bind_nullable(st,5,item->completion_date);
    bind_nullable(st,6,item->creation_date);
    bind_nullable(st,7,item->due_date);
    bind_nullable(st,8,item->threshold_date);
    bind_nullable(st,9,item->recurrence);
    bind_nullable(st,10,item->projects);
    bind_nullable(st,11,item->contexts);
    sqlite3_bind_int64(st,12,now_seconds());
    sqlite3_bind_int64(st,13,now_seconds());

    rc=sqlite3_step(st);
    if (rc!=SQLITE_DONE) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"insert step"); goto done; }
    *id_out=sqlite3_last_insert_rowid(db);
    rv=APR_SUCCESS;
done:
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    if (g_mutex) apr_thread_mutex_unlock(g_mutex);
    return rv;
}

static apr_status_t sp_update(apr_pool_t *p,server_rec *s,const char *db_path,
                              const todotxt_item *item,apr_int64_t expected_revision,
                              apr_int64_t *new_revision_out)
{
    sqlite3 *db=NULL; sqlite3_stmt *st=NULL; apr_status_t rv=APR_EGENERAL; int rc,b=1;
    const char *sql;
    (void)p;
    if (new_revision_out) *new_revision_out=0;
    if (g_mutex) apr_thread_mutex_lock(g_mutex);
    if (open_db(s,db_path,&db)!=APR_SUCCESS) goto done;

    sql = expected_revision>0 ?
        "UPDATE todos SET raw=?,completed=?,priority=?,completion_date=?,creation_date=?,due_date=?,"
        "threshold_date=?,recurrence=?,projects=?,contexts=?,updated_at=?,revision=revision+1 "
        "WHERE owner=? AND id=? AND revision=?;" :
        "UPDATE todos SET raw=?,completed=?,priority=?,completion_date=?,creation_date=?,due_date=?,"
        "threshold_date=?,recurrence=?,projects=?,contexts=?,updated_at=?,revision=revision+1 "
        "WHERE owner=? AND id=?;";

    rc=sqlite3_prepare_v2(db,sql,-1,&st,NULL);
    if (rc!=SQLITE_OK) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"update prepare"); goto done; }

    sqlite3_bind_text(st,b++,item->raw,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(st,b++,item->completed);
    if (item->priority) { char pb[2]={item->priority,0}; sqlite3_bind_text(st,b++,pb,-1,SQLITE_TRANSIENT); }
    else sqlite3_bind_null(st,b++);
    bind_nullable(st,b++,item->completion_date);
    bind_nullable(st,b++,item->creation_date);
    bind_nullable(st,b++,item->due_date);
    bind_nullable(st,b++,item->threshold_date);
    bind_nullable(st,b++,item->recurrence);
    bind_nullable(st,b++,item->projects);
    bind_nullable(st,b++,item->contexts);
    sqlite3_bind_int64(st,b++,now_seconds());
    sqlite3_bind_text(st,b++,item->owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(st,b++,item->id);
    if (expected_revision>0) sqlite3_bind_int64(st,b++,expected_revision);

    rc=sqlite3_step(st);
    if (rc!=SQLITE_DONE) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"update step"); goto done; }

    if (sqlite3_changes(db)==1) {
        apr_int64_t rev=0;
        rv=select_revision(s,db,db_path,item->owner,item->id,&rev);
        if (rv==APR_SUCCESS && new_revision_out) *new_revision_out=rev;
        goto done;
    }

    if (expected_revision>0) {
        apr_int64_t current=0;
        rv=select_revision(s,db,db_path,item->owner,item->id,&current);
        if (rv==APR_SUCCESS) rv=APR_EBUSY;
    } else {
        rv=APR_NOTFOUND;
    }
done:
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    if (g_mutex) apr_thread_mutex_unlock(g_mutex);
    return rv;
}

static todotxt_item *row_to_item(apr_pool_t *p,sqlite3_stmt *st)
{
    todotxt_item *it=apr_pcalloc(p,sizeof(*it));
    const unsigned char *v;
    it->id=sqlite3_column_int64(st,0);
    v=sqlite3_column_text(st,1); it->owner=apr_pstrdup(p,v?(const char*)v:"");
    v=sqlite3_column_text(st,2); it->raw=apr_pstrdup(p,v?(const char*)v:"");
    it->completed=sqlite3_column_int(st,3);
    v=sqlite3_column_text(st,4); if (v&&*v) it->priority=((const char*)v)[0];
    v=sqlite3_column_text(st,5); it->completion_date=v?apr_pstrdup(p,(const char*)v):NULL;
    v=sqlite3_column_text(st,6); it->creation_date=v?apr_pstrdup(p,(const char*)v):NULL;
    v=sqlite3_column_text(st,7); it->due_date=v?apr_pstrdup(p,(const char*)v):NULL;
    v=sqlite3_column_text(st,8); it->threshold_date=v?apr_pstrdup(p,(const char*)v):NULL;
    v=sqlite3_column_text(st,9); it->recurrence=v?apr_pstrdup(p,(const char*)v):NULL;
    v=sqlite3_column_text(st,10); it->projects=v?apr_pstrdup(p,(const char*)v):NULL;
    v=sqlite3_column_text(st,11); it->contexts=v?apr_pstrdup(p,(const char*)v):NULL;
    it->created_at=sqlite3_column_int64(st,12)*APR_USEC_PER_SEC;
    it->updated_at=sqlite3_column_int64(st,13)*APR_USEC_PER_SEC;
    it->revision=sqlite3_column_int64(st,14);
    return it;
}

static apr_status_t sp_get(apr_pool_t *p,server_rec *s,const char *db_path,
                           const char *owner,apr_int64_t id,todotxt_item **out)
{
    sqlite3 *db=NULL; sqlite3_stmt *st=NULL; apr_status_t rv=APR_NOTFOUND; int rc;
    *out=NULL;
    if (g_mutex) apr_thread_mutex_lock(g_mutex);
    if (open_db(s,db_path,&db)!=APR_SUCCESS) { rv=APR_EGENERAL; goto done; }

    rc=sqlite3_prepare_v2(db,
        "SELECT id,owner,raw,completed,priority,completion_date,creation_date,due_date,threshold_date,"
        "recurrence,projects,contexts,created_at,updated_at,revision FROM todos WHERE owner=? AND id=?;",
        -1,&st,NULL);
    if (rc!=SQLITE_OK) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"get prepare"); rv=APR_EGENERAL; goto done; }
    sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(st,2,id);
    rc=sqlite3_step(st);
    if (rc==SQLITE_ROW) { *out=row_to_item(p,st); rv=APR_SUCCESS; }
    else if (rc==SQLITE_DONE) rv=APR_NOTFOUND;
    else { log_sqlite(s,APLOG_ERR,db,rc,db_path,"get step"); rv=APR_EGENERAL; }
done:
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    if (g_mutex) apr_thread_mutex_unlock(g_mutex);
    return rv;
}

static apr_status_t sp_remove(apr_pool_t *p,server_rec *s,const char *db_path,
                              const char *owner,apr_int64_t id,apr_int64_t expected_revision)
{
    sqlite3 *db=NULL; sqlite3_stmt *st=NULL; apr_status_t rv=APR_EGENERAL; int rc;
    (void)p;
    if (g_mutex) apr_thread_mutex_lock(g_mutex);
    if (open_db(s,db_path,&db)!=APR_SUCCESS) goto done;

    rc=sqlite3_prepare_v2(db,
        expected_revision>0 ?
        "DELETE FROM todos WHERE owner=? AND id=? AND revision=?;" :
        "DELETE FROM todos WHERE owner=? AND id=?;",
        -1,&st,NULL);
    if (rc!=SQLITE_OK) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"delete prepare"); goto done; }
    sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(st,2,id);
    if (expected_revision>0) sqlite3_bind_int64(st,3,expected_revision);
    rc=sqlite3_step(st);
    if (rc!=SQLITE_DONE) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"delete step"); goto done; }
    if (sqlite3_changes(db)==1) { rv=APR_SUCCESS; goto done; }

    if (expected_revision>0) {
        apr_int64_t current=0;
        rv=select_revision(s,db,db_path,owner,id,&current);
        if (rv==APR_SUCCESS) rv=APR_EBUSY;
    } else rv=APR_NOTFOUND;
done:
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    if (g_mutex) apr_thread_mutex_unlock(g_mutex);
    return rv;
}

static apr_status_t sp_list(apr_pool_t *p,server_rec *s,const char *db_path,
                            const char *owner,const char *mode,apr_array_header_t **out)
{
    sqlite3 *db=NULL; sqlite3_stmt *st=NULL; apr_status_t rv=APR_EGENERAL; int rc;
    const char *sql;
    *out=apr_array_make(p,32,sizeof(todotxt_item*));

    if (mode && !strcmp(mode,"completed"))
        sql="SELECT id,owner,raw,completed,priority,completion_date,creation_date,due_date,threshold_date,"
            "recurrence,projects,contexts,created_at,updated_at,revision FROM todos "
            "WHERE owner=? AND completed=1 ORDER BY id;";
    else if (mode && !strcmp(mode,"all"))
        sql="SELECT id,owner,raw,completed,priority,completion_date,creation_date,due_date,threshold_date,"
            "recurrence,projects,contexts,created_at,updated_at,revision FROM todos "
            "WHERE owner=? ORDER BY completed,id;";
    else
        sql="SELECT id,owner,raw,completed,priority,completion_date,creation_date,due_date,threshold_date,"
            "recurrence,projects,contexts,created_at,updated_at,revision FROM todos "
            "WHERE owner=? AND completed=0 ORDER BY id;";

    if (g_mutex) apr_thread_mutex_lock(g_mutex);
    if (open_db(s,db_path,&db)!=APR_SUCCESS) goto done;
    rc=sqlite3_prepare_v2(db,sql,-1,&st,NULL);
    if (rc!=SQLITE_OK) { log_sqlite(s,APLOG_ERR,db,rc,db_path,"list prepare"); goto done; }
    sqlite3_bind_text(st,1,owner,-1,SQLITE_TRANSIENT);
    while ((rc=sqlite3_step(st))==SQLITE_ROW)
        APR_ARRAY_PUSH(*out,todotxt_item*)=row_to_item(p,st);
    if (rc==SQLITE_DONE) rv=APR_SUCCESS;
    else { log_sqlite(s,APLOG_ERR,db,rc,db_path,"list step"); rv=APR_EGENERAL; }
done:
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
    if (g_mutex) apr_thread_mutex_unlock(g_mutex);
    return rv;
}

static todotxt_storage_provider provider={sp_insert,sp_update,sp_get,sp_remove,sp_list};

apr_status_t todotxt_sqlite_register_provider(apr_pool_t *p)
{
#if APR_HAS_THREADS
    if (!g_mutex && apr_thread_mutex_create(&g_mutex,APR_THREAD_MUTEX_DEFAULT,p)!=APR_SUCCESS)
        return APR_EGENERAL;
#endif
    ap_register_provider(p,TODOTXT_STORAGE_PROVIDER_GROUP,"sqlite",TODOTXT_STORAGE_PROVIDER_VERSION,&provider);
    return APR_SUCCESS;
}

apr_status_t todotxt_sqlite_init_server(apr_pool_t *p,server_rec *s)
{
    todotxt_server_conf *conf=todotxt_get_conf(s);
    sqlite3 *db=NULL;
    apr_status_t rv;
    (void)p;
    if (!conf || !conf->db_path) return APR_EINVAL;

    ap_log_error(APLOG_MARK,APLOG_NOTICE,0,s,
        "mod_todotxt: initializing SQLite store server='%s' db='%s'",
        s->server_hostname?s->server_hostname:"(default)",conf->db_path);

    rv=open_db(s,conf->db_path,&db);
    if (rv!=APR_SUCCESS) return rv;
    rv=ensure_schema(s,db,conf->db_path);

    if (rv==APR_SUCCESS)
        ap_log_error(APLOG_MARK,APLOG_NOTICE,0,s,
            "mod_todotxt: SQLite schema ready server='%s' db='%s' sqlite_version='%s'",
            s->server_hostname?s->server_hostname:"(default)",conf->db_path,sqlite3_libversion());

    sqlite3_close(db);
    return rv;
}
