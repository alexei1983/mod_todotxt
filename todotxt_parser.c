#include "todotxt_internal.h"
#include "apr_strings.h"

#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdlib.h>

static int is_date10(const char *s)
{
    return s && strlen(s) == 10 &&
        isdigit((unsigned char)s[0]) &&
        isdigit((unsigned char)s[1]) &&
        isdigit((unsigned char)s[2]) &&
        isdigit((unsigned char)s[3]) &&
        s[4] == '-' &&
        isdigit((unsigned char)s[5]) &&
        isdigit((unsigned char)s[6]) &&
        s[7] == '-' &&
        isdigit((unsigned char)s[8]) &&
        isdigit((unsigned char)s[9]);
}

static int is_priority(const char *s)
{
    return s && strlen(s) == 3 &&
        s[0] == '(' && s[2] == ')' &&
        s[1] >= 'A' && s[1] <= 'Z';
}

static const char *date_today(apr_pool_t *p, apr_time_t t)
{
    apr_time_exp_t x;
    apr_time_exp_lt(&x, t);

    return apr_psprintf(
        p,
        "%04d-%02d-%02d",
        x.tm_year + 1900,
        x.tm_mon + 1,
        x.tm_mday);
}

static void append_word(
    apr_pool_t *p,
    const char **acc,
    const char *value)
{
    if (!*acc || !**acc)
        *acc = apr_pstrdup(p, value);
    else
        *acc = apr_pstrcat(p, *acc, " ", value, NULL);
}

apr_status_t todotxt_parse_line(
    apr_pool_t *p,
    const todotxt_server_conf *conf,
    const char *owner,
    const char *line,
    todotxt_item **out,
    const char **error)
{
    todotxt_item *item;
    char *copy;
    char *save = NULL;
    char *tok;

    if (!line || !*line) {
        if (error) *error = "empty todo line";
        return APR_EINVAL;
    }

    item = apr_pcalloc(p, sizeof(*item));
    item->owner = apr_pstrdup(p, owner);
    item->raw = apr_pstrdup(p, line);

    copy = apr_pstrdup(p, line);
    tok = apr_strtok(copy, " \t", &save);

    if (tok && !strcmp(tok, "x")) {
        item->completed = 1;
        tok = apr_strtok(NULL, " \t", &save);

        if (tok && is_date10(tok)) {
            item->completion_date = apr_pstrdup(p, tok);
            tok = apr_strtok(NULL, " \t", &save);
        }
    }

    if (tok && is_priority(tok)) {
        item->priority = tok[1];
        tok = apr_strtok(NULL, " \t", &save);
    }

    if (tok && is_date10(tok)) {
        item->creation_date = apr_pstrdup(p, tok);
        tok = apr_strtok(NULL, " \t", &save);
    }

    for (; tok; tok = apr_strtok(NULL, " \t", &save)) {
        if (tok[0] == '+' && tok[1]) {
            append_word(p, &item->projects, tok + 1);
        }
        else if (tok[0] == '@' && tok[1]) {
            append_word(p, &item->contexts, tok + 1);
        }
        else if (conf->enable_due && !strncmp(tok, "due:", 4)) {
            if (!is_date10(tok + 4)) {
                if (error) *error = "invalid due: date; expected YYYY-MM-DD";
                return APR_EINVAL;
            }
            item->due_date = apr_pstrdup(p, tok + 4);
        }
        else if (conf->enable_threshold && !strncmp(tok, "t:", 2)) {
            if (!is_date10(tok + 2)) {
                if (error) *error = "invalid threshold t: date; expected YYYY-MM-DD";
                return APR_EINVAL;
            }
            item->threshold_date = apr_pstrdup(p, tok + 2);
        }
        else if (conf->enable_rec && !strncmp(tok, "rec:", 4)) {
            const char *q = tok + 4;

            if (*q == '+')
                ++q;

            if (!isdigit((unsigned char)*q)) {
                if (error) *error = "invalid rec: expression";
                return APR_EINVAL;
            }

            while (isdigit((unsigned char)*q))
                ++q;

            if (!(*q == 'd' || *q == 'w' || *q == 'm' || *q == 'y') ||
                q[1] != '\0') {

                if (error)
                    *error = "rec: supports [+]N[d|w|m|y]";

                return APR_EINVAL;
            }

            item->recurrence = apr_pstrdup(p, tok + 4);
        }
    }

    *out = item;
    return APR_SUCCESS;
}

const char *todotxt_complete_line(
    apr_pool_t *p,
    const char *line,
    apr_time_t now)
{
    if (!line)
        return NULL;

    if (!strncmp(line, "x ", 2))
        return apr_pstrdup(p, line);

    return apr_pstrcat(
        p,
        "x ",
        date_today(p, now),
        " ",
        line,
        NULL);
}

static int parse_ymd(const char *s, struct tm *t)
{
    if (!is_date10(s))
        return 0;

    memset(t, 0, sizeof(*t));

    t->tm_year = atoi(s) - 1900;
    t->tm_mon = atoi(s + 5) - 1;
    t->tm_mday = atoi(s + 8);
    t->tm_isdst = -1;

    return 1;
}

static const char *add_interval(
    apr_pool_t *p,
    const char *base,
    const char *rec)
{
    struct tm t;
    char *end = NULL;
    long n;
    char unit;
    char buf[16];
    time_t tt;
    const char *q = rec;

    if (*q == '+')
        ++q;

    n = strtol(q, &end, 10);

    if (end == q || !*end || end[1])
        return NULL;

    unit = *end;

    if (n <= 0 || !parse_ymd(base, &t))
        return NULL;

    switch (unit) {
        case 'd': t.tm_mday += (int)n; break;
        case 'w': t.tm_mday += (int)(n * 7); break;
        case 'm': t.tm_mon  += (int)n; break;
        case 'y': t.tm_year += (int)n; break;
        default: return NULL;
    }

    tt = mktime(&t);
    if (tt == (time_t)-1)
        return NULL;

    {
        struct tm *o = localtime(&tt);

        if (!o)
            return NULL;

        strftime(buf, sizeof(buf), "%Y-%m-%d", o);
    }

    return apr_pstrdup(p, buf);
}

static const char *replace_extension(
    apr_pool_t *p,
    const char *line,
    const char *prefix,
    const char *value)
{
    char *copy = apr_pstrdup(p, line);
    char *save = NULL;
    char *tok;
    const char *out = "";
    int first = 1;
    size_t n = strlen(prefix);

    for (tok = apr_strtok(copy, " \t", &save);
         tok;
         tok = apr_strtok(NULL, " \t", &save)) {

        const char *emit =
            !strncmp(tok, prefix, n)
                ? apr_pstrcat(p, prefix, value, NULL)
                : tok;

        out =
            first
                ? apr_pstrdup(p, emit)
                : apr_pstrcat(p, out, " ", emit, NULL);

        first = 0;
    }

    return out;
}

static const char *strip_completion(
    apr_pool_t *p,
    const char *line)
{
    if (strncmp(line, "x ", 2))
        return apr_pstrdup(p, line);

    line += 2;

    if (is_date10(line) && line[10] == ' ')
        line += 11;

    return apr_pstrdup(p, line);
}

const char *todotxt_recurrence_next_line(
    apr_pool_t *p,
    const todotxt_item *item,
    apr_time_t now)
{
    const char *base;
    const char *next_due;
    const char *line;
    const char *nowdate;

    if (!item || !item->recurrence)
        return NULL;

    nowdate = date_today(p, now);

    base =
        (item->recurrence[0] == '+' && item->due_date)
            ? item->due_date
            : nowdate;

    next_due = add_interval(p, base, item->recurrence);

    if (!next_due)
        return NULL;

    line = strip_completion(p, item->raw);

    if (item->due_date)
        line = replace_extension(p, line, "due:", next_due);
    else
        line = apr_pstrcat(p, line, " due:", next_due, NULL);

    /*
     * If both threshold and due exist, preserve their day-distance
     * approximately for the next occurrence.
     */
    if (item->threshold_date && item->due_date) {
        struct tm th;
        struct tm du;
        struct tm nd;
        time_t t1;
        time_t t2;
        time_t tn;

        if (parse_ymd(item->threshold_date, &th) &&
            parse_ymd(item->due_date, &du) &&
            parse_ymd(next_due, &nd)) {

            t1 = mktime(&th);
            t2 = mktime(&du);

            if (t1 != (time_t)-1 && t2 != (time_t)-1) {
                long days = (long)((t2 - t1) / 86400);

                nd.tm_mday -= (int)days;
                tn = mktime(&nd);

                if (tn != (time_t)-1) {
                    char b[16];
                    struct tm *o = localtime(&tn);

                    if (o) {
                        strftime(b, sizeof(b), "%Y-%m-%d", o);
                        line = replace_extension(p, line, "t:", b);
                    }
                }
            }
        }
    }

    return line;
}
