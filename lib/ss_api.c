#include "ss_api.h"
#include "ss_json.h"
#include "ss_config.h"
#include "ss_utils.h"
#include <curl/curl.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

// ============================================================ state ======

static char g_last_error[512] = {0};
static int g_last_status = 0;
static char g_last_code[64] = {0};
static char g_jwt[1024] = {0};
static int g_user_id = 0;
static char g_refresh[1024] = {0};
static char g_user_agent[128] = "simple-social-cli";
static void (*g_token_refreshed_cb)(const char *jwt) = NULL;
// Guards against a refresh recursing into itself: the refresh call goes
// through request() too, and its own 401 must not trigger another.
static int g_refreshing = 0;

const char *const api_report_reasons[] = {
    "spam", "harassment", "hate", "sexual", "violence", "self_harm", "illegal", "other", NULL
};

void api_set_jwt(const char *jwt) {
    if (jwt) { strncpy(g_jwt, jwt, sizeof(g_jwt) - 1); g_jwt[sizeof(g_jwt) - 1] = '\0'; }
    else g_jwt[0] = '\0';
}

void api_set_user_id(int user_id) { g_user_id = user_id; }

void api_set_user_agent(const char *ua) {
    if (!ua || !ua[0]) return;
    strncpy(g_user_agent, ua, sizeof(g_user_agent) - 1);
    g_user_agent[sizeof(g_user_agent) - 1] = '\0';
}

void api_set_refresh_token(const char *refresh) {
    if (!refresh) { g_refresh[0] = '\0'; return; }
    strncpy(g_refresh, refresh, sizeof(g_refresh) - 1);
    g_refresh[sizeof(g_refresh) - 1] = '\0';
}

const char *api_get_refresh_token(void) { return g_refresh; }
void api_set_token_refreshed_cb(void (*cb)(const char *jwt)) { g_token_refreshed_cb = cb; }
const char *api_get_last_error(void) { return g_last_error; }
int api_get_last_status(void) { return g_last_status; }
const char *api_get_last_code(void) { return g_last_code; }

int api_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return 0;
}

void api_cleanup(void) { curl_global_cleanup(); }

static void set_error(const char *msg) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg ? msg : "");
}

// ============================================================ times ======

void api_local_gmt_offset(char *out, int out_size) {
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    long off = lt.tm_gmtoff / 60;   // minutes east of GMT
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    snprintf(out, out_size, "%c%02ld:%02ld", sign, off / 60, off % 60);
}

void api_format_time(const char *utc, const char *tz, char *out, int out_size) {
    if (!utc || !utc[0] || strcmp(utc, "Unknown") == 0) { snprintf(out, out_size, "UTC (unknown)"); return; }
    if (tz && tz[0]) snprintf(out, out_size, "UTC %s (GMT%s)", utc, tz);
    else snprintf(out, out_size, "UTC %s", utc);
}

void api_site_url(const char *path, char *out, int out_size) {
    if (!path) path = "";
    if (strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        snprintf(out, out_size, "%s", path);
        return;
    }
    const char *base = config_get_base_url();
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    const char *slash = strchr(host, '/');
    int origin_len = slash ? (int)(slash - base) : (int)strlen(base);
    snprintf(out, out_size, "%.*s%s%s", origin_len, base, path[0] == '/' ? "" : "/", path);
}

// ============================================================ params =====

// A growable "a=1&b=2" string. Values are URL-encoded exactly as given
// (invite codes are case-sensitive and full of symbols).
typedef struct { char *s; size_t len, cap; } params_t;

static void pp_raw(params_t *p, const char *text) {
    size_t n = strlen(text);
    if (p->len + n + 2 > p->cap) {
        size_t cap = p->cap ? p->cap : 256;
        while (p->len + n + 2 > cap) cap *= 2;
        char *s = realloc(p->s, cap);
        if (!s) return;
        p->s = s;
        p->cap = cap;
    }
    memcpy(p->s + p->len, text, n);
    p->len += n;
    p->s[p->len] = '\0';
}

static void pp_add(params_t *p, const char *key, const char *val) {
    if (!val) return;
    char *enc = url_encode(val);
    if (!enc) return;
    if (p->len) pp_raw(p, "&");
    pp_raw(p, key);
    pp_raw(p, "=");
    pp_raw(p, enc);
    free(enc);
}

static void pp_addi(params_t *p, const char *key, long long val) {
    char num[32];
    snprintf(num, sizeof(num), "%lld", val);
    pp_add(p, key, num);
}

static const char *pp_str(params_t *p) { return p->s ? p->s : ""; }
static void pp_free(params_t *p) { free(p->s); p->s = NULL; p->len = p->cap = 0; }

// ============================================================ http =======

struct buf { char *data; size_t used, size; };

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct buf *b = userdata;
    size_t n = size * nmemb;
    if (b->used + n + 1 > b->size) {
        size_t cap = b->size ? b->size : 8192;
        while (b->used + n + 1 > cap) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return 0;
        b->data = d;
        b->size = cap;
    }
    memcpy(b->data + b->used, ptr, n);
    b->used += n;
    b->data[b->used] = '\0';
    return n;
}

static void media_endpoint(char *out, int out_size) {
    snprintf(out, out_size, "%s", config_get_base_url());
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    strncat(out, "/media.php", out_size - strlen(out) - 1);
}

// One POST. Either form fields or a multipart body. *resp is always a heap
// string (possibly empty) the caller frees. Returns the HTTP status, or -1 when
// the request didn't happen at all.
static long http_post(const char *url, const char *fields, curl_mime *mime, long timeout, char **resp) {
    *resp = NULL;
    CURL *curl = curl_easy_init();
    if (!curl) { set_error("Failed to init curl"); return -1; }

    struct buf b = { calloc(1, 8192), 0, 8192 };
    if (!b.data) { curl_easy_cleanup(curl); return -1; }

    struct curl_slist *headers = NULL;
    if (fields) headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    char auth[1200];
    if (g_jwt[0]) {
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_jwt);
        headers = curl_slist_append(headers, auth);
    }
    char tz[16], tzh[40];
    api_local_gmt_offset(tz, sizeof(tz));
    snprintf(tzh, sizeof(tzh), "X-SS-TZ: %s", tz);
    headers = curl_slist_append(headers, tzh);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (fields) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
    if (mime) curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_user_agent);

    CURLcode res = curl_easy_perform(curl);
    long status = -1;
    if (res != CURLE_OK) {
        snprintf(g_last_error, sizeof(g_last_error), "HTTP error: %s", curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    *resp = b.data;
    return status;
}

// After an answer: was it a success? On failure the last error/code/status
// say why, taken from the server's own words when it gave any.
static int judge(long status, const char *resp, const char *fallback) {
    g_last_status = (int)status;
    g_last_code[0] = '\0';
    if (status < 0) return -1;
    int valid = 1;
    json_get_bool(resp, "valid", &valid);
    if (status >= 200 && status < 400 && valid) return 0;

    char msg[512] = {0};
    if (json_get_string(resp, "message", msg, sizeof(msg)) != 0 || !msg[0])
        json_get_string(resp, "error", msg, sizeof(msg));
    json_get_string(resp, "code", g_last_code, sizeof(g_last_code));
    if (msg[0]) set_error(msg);
    else if (fallback) set_error(fallback);
    else snprintf(g_last_error, sizeof(g_last_error), "HTTP %ld", status);
    return -1;
}

// Calls an action on api.php. *resp (always set, heap) holds the whole
// answer; the caller frees it. A 401 renews the session once and retries.
static int request(const char *action, const char *params, char **resp) {
    params_t f = {0};
    pp_add(&f, "action", action);
    if (params && params[0]) { pp_raw(&f, "&"); pp_raw(&f, params); }

    long status = http_post(config_get_base_url(), pp_str(&f), NULL, 30L, resp);
    if (status == 401 && !g_refreshing && g_refresh[0] && strcmp(action, "login") != 0) {
        if (api_refresh_session() == 0) {
            free(*resp);
            status = http_post(config_get_base_url(), pp_str(&f), NULL, 30L, resp);
        }
    }
    pp_free(&f);
    return judge(status, *resp ? *resp : "", NULL);
}

// request() for actions whose answer is only checked.
static int simple(const char *action, const char *params) {
    char *resp = NULL;
    int rc = request(action, params, &resp);
    free(resp);
    return rc;
}

int api_call(const char *action, const char *params, char *response, int resp_size) {
    char *resp = NULL;
    int rc = request(action, params, &resp);
    if (resp && response && resp_size > 0) {
        if ((int)strlen(resp) >= resp_size) {
            response[0] = '\0';
            set_error("The server's answer was too large to read here.");
            rc = -1;
        } else {
            strcpy(response, resp);
        }
    }
    free(resp);
    return rc;
}

int api_refresh_session(void) {
    if (!g_refresh[0]) return -1;
    params_t p = {0};
    pp_add(&p, "refreshToken", g_refresh);
    char *resp = NULL;
    g_refreshing = 1;
    int rc = request("refreshToken", pp_str(&p), &resp);
    g_refreshing = 0;
    pp_free(&p);
    char jwt[1024] = {0};
    if (rc == 0 && json_get_string(resp, "jwt", jwt, sizeof(jwt)) == 0 && jwt[0]) {
        api_set_jwt(jwt);
        if (g_token_refreshed_cb) g_token_refreshed_cb(jwt);
        free(resp);
        return 0;
    }
    free(resp);
    return -1;
}

// ============================================================ parsing ====

// Mentions are @[id] tokens in the text; the server resolves each id to the
// person's current email. Shown as @email, or "@deleted user".
static void resolve_mentions(char *text, size_t text_size, const char *item_json) {
    const char *as, *ae;
    if (json_get_array(item_json, "mentions", &as, &ae) != 0) return;
    int len = json_array_len(as, ae);
    if (len <= 0) return;

    size_t cap = text_size * 2 + 256;
    char *result = malloc(cap);
    if (!result) return;
    size_t rlen = 0;
    const char *p = text;
    while (*p && rlen < cap - 1) {
        if (p[0] == '@' && p[1] == '[') {
            const char *digits = p + 2, *q = digits;
            while (*q >= '0' && *q <= '9') q++;
            if (q > digits && *q == ']') {
                int id = atoi(digits);
                char email[256] = {0};
                int found = 0;
                for (int i = 0; i < len && !found; i++) {
                    char *m = json_array_item_dup(as, ae, i);
                    if (!m) break;
                    int mid = 0;
                    json_get_int(m, "id", &mid);
                    if (mid == id) found = json_get_string(m, "email", email, sizeof(email)) == 0;
                    free(m);
                }
                int n = snprintf(result + rlen, cap - rlen, "@%s", found ? email : "deleted user");
                if (n > 0) rlen += (size_t)n;
                if (rlen > cap - 1) rlen = cap - 1;
                p = q + 1;
                continue;
            }
        }
        result[rlen++] = *p++;
    }
    result[rlen] = '\0';
    snprintf(text, text_size, "%s", result);
    free(result);
}

static void parse_post(const char *item, api_post_t *p) {
    memset(p, 0, sizeof(*p));
    json_get_string(item, "id", p->id, sizeof(p->id));
    json_get_string(item, "text", p->text, sizeof(p->text));
    resolve_mentions(p->text, sizeof(p->text), item);
    json_get_string(item, "timestamp", p->timestamp, sizeof(p->timestamp));
    json_get_string(item, "timestampTz", p->timestamp_tz, sizeof(p->timestamp_tz));
    json_get_string(item, "mediaUrl", p->media_url, sizeof(p->media_url));
    if (strcmp(p->media_url, "null") == 0) p->media_url[0] = '\0';
    json_get_int(item, "userID", &p->user_id);
    json_get_string(item, "userEmail", p->user_email, sizeof(p->user_email));
    json_get_int(item, "commentCount", &p->comment_count);
    json_get_bool(item, "reportedByMe", &p->reported_by_me);
    json_get_bool(item, "frozen", &p->frozen);

    const char *ms, *me;
    if (json_get_nested(item, "media", &ms, &me) == 0) {
        char *m = json_copy(ms, me);
        if (m) {
            json_get_string(m, "type", p->media_type, sizeof(p->media_type));
            json_get_string(m, "variantUrl", p->media_variant, sizeof(p->media_variant));
            json_get_string(m, "posterUrl", p->media_poster, sizeof(p->media_poster));
            json_get_bool(m, "loop", &p->media_loop);
            if (!p->media_url[0]) json_get_string(m, "url", p->media_url, sizeof(p->media_url));
            free(m);
        }
    }

    // Likes: newer servers send the count and "liked by me"; older ones (and
    // version-1 answers) the whole list, which still works.
    int have_count = json_get_int(item, "likeCount", &p->like_count) == 0;
    int have_mine = json_get_bool(item, "likedByMe", &p->is_liked) == 0;
    const char *ls, *le;
    if ((!have_count || !have_mine) && json_get_array(item, "likes", &ls, &le) == 0) {
        int n = json_array_len(ls, le);
        if (!have_count) p->like_count = n;
        if (!have_mine && g_user_id > 0) {
            for (int j = 0; j < n; j++) {
                char *like = json_array_item_dup(ls, le, j);
                if (!like) break;
                int uid = 0;
                json_get_int(like, "userId", &uid);
                free(like);
                if (uid == g_user_id) { p->is_liked = 1; break; }
            }
        }
    }
}

static int parse_posts(const char *resp, api_posts_result_t *result) {
    result->count = 0;
    result->has_more = 0;
    result->total_count = 0;
    json_get_bool(resp, "hasMore", &result->has_more);
    json_get_int(resp, "totalCount", &result->total_count);
    const char *as, *ae;
    if (json_get_array(resp, "posts", &as, &ae) != 0) return -1;
    int len = json_array_len(as, ae);
    if (len > API_MAX_POSTS) len = API_MAX_POSTS;
    for (int i = 0; i < len; i++) {
        char *item = json_array_item_dup(as, ae, i);
        if (!item) break;
        parse_post(item, &result->posts[result->count++]);
        free(item);
    }
    return 0;
}

static void parse_comment(const char *item, api_comment_t *c, int parent_id) {
    memset(c, 0, sizeof(*c));
    json_get_int(item, "id", &c->id);
    json_get_int(item, "user_id", &c->user_id);
    json_get_string(item, "text", c->text, sizeof(c->text));
    resolve_mentions(c->text, sizeof(c->text), item);
    json_get_string(item, "created_at", c->created_at, sizeof(c->created_at));
    json_get_string(item, "created_tz", c->created_tz, sizeof(c->created_tz));
    json_get_string(item, "user_email", c->user_email, sizeof(c->user_email));
    if (json_get_int(item, "parent_id", &c->parent_id) != 0) c->parent_id = parent_id;
    json_get_int(item, "replyCount", &c->reply_count);
    json_get_bool(item, "deleted", &c->deleted);
    json_get_bool(item, "frozenByStaff", &c->frozen_by_staff);
    json_get_bool(item, "frozen", &c->frozen);
    json_get_bool(item, "reportedByMe", &c->reported_by_me);
}

static int parse_comments(const char *resp, api_comments_result_t *result, int threaded) {
    memset(result, 0, sizeof(*result) - sizeof(result->comments));
    result->count = 0;
    json_get_bool(resp, "hasMore", &result->has_more);
    json_get_int(resp, "totalCount", &result->total_count);
    const char *as, *ae;
    if (json_get_array(resp, "comments", &as, &ae) != 0) return -1;
    int len = json_array_len(as, ae);
    for (int i = 0; i < len; i++) {
        if (result->count >= API_MAX_COMMENTS) { result->truncated = 1; break; }
        char *item = json_array_item_dup(as, ae, i);
        if (!item) break;
        api_comment_t *top = &result->comments[result->count++];
        parse_comment(item, top, 0);
        const char *rs, *re;
        if (threaded && json_get_array(item, "replies", &rs, &re) == 0) {
            int rn = json_array_len(rs, re);
            for (int j = 0; j < rn; j++) {
                if (result->count >= API_MAX_COMMENTS) { result->truncated = 1; break; }
                char *r = json_array_item_dup(rs, re, j);
                if (!r) break;
                parse_comment(r, &result->comments[result->count++], top->id);
                free(r);
            }
        }
        free(item);
    }
    return 0;
}

static void parse_user(const char *item, api_user_t *u) {
    memset(u, 0, sizeof(*u));
    if (json_get_int(item, "id", &u->id) != 0) json_get_int(item, "userId", &u->id);
    json_get_string(item, "email", u->email, sizeof(u->email));
    if (json_get_string(item, "timestamp", u->created_at, sizeof(u->created_at)) != 0)
        json_get_string(item, "created_at", u->created_at, sizeof(u->created_at));
    if (json_get_string(item, "timestampTz", u->created_tz, sizeof(u->created_tz)) != 0)
        json_get_string(item, "created_tz", u->created_tz, sizeof(u->created_tz));
}

static int parse_users(const char *resp, const char *key, api_users_result_t *result) {
    result->count = 0;
    const char *as, *ae;
    if (json_get_array(resp, key, &as, &ae) != 0) return -1;
    int len = json_array_len(as, ae);
    if (len > 256) len = 256;
    for (int i = 0; i < len; i++) {
        char *item = json_array_item_dup(as, ae, i);
        if (!item) break;
        parse_user(item, &result->users[result->count++]);
        free(item);
    }
    return 0;
}

// Runs an action and parses a list of users out of `key`.
static int user_list(const char *action, const char *params, const char *key, api_users_result_t *result) {
    char *resp = NULL;
    int rc = request(action, params, &resp);
    if (rc == 0) rc = parse_users(resp, key, result);
    else result->count = 0;
    free(resp);
    return rc;
}

// ============================================================ account ====

int api_login(const char *email, const char *password, char *jwt_out, int jwt_size, int *user_id_out) {
    params_t p = {0};
    pp_add(&p, "email", email);
    pp_add(&p, "password", password);
    char *resp = NULL;
    int rc = request("login", pp_str(&p), &resp);
    pp_free(&p);
    if (rc != 0) {
        if (!g_last_error[0]) set_error("Login failed");
        free(resp);
        return -1;
    }
    char refresh[1024] = {0};
    if (json_get_string(resp, "refreshToken", refresh, sizeof(refresh)) == 0) api_set_refresh_token(refresh);
    if (json_get_string(resp, "jwt", jwt_out, jwt_size) != 0) {
        set_error("No JWT in response");
        free(resp);
        return -1;
    }
    json_get_int(resp, "userId", user_id_out);
    free(resp);
    return 0;
}

int api_logout(void) { return simple("logout", ""); }

int api_get_me(api_me_t *me) {
    memset(me, 0, sizeof(*me));
    char *resp = NULL;
    int rc = request("getMyInfo", "", &resp);
    if (rc == 0) {
        json_get_int(resp, "userId", &me->id);
        json_get_string(resp, "email", me->email, sizeof(me->email));
        json_get_string(resp, "created_at", me->created_at, sizeof(me->created_at));
        snprintf(me->role, sizeof(me->role), "user");
        json_get_string(resp, "role", me->role, sizeof(me->role));
        json_get_int(resp, "termsVersionAccepted", &me->terms_accepted);
        json_get_int(resp, "termsVersionCurrent", &me->terms_current);
        char path[200] = "/terms";
        json_get_string(resp, "termsUrl", path, sizeof(path));
        api_site_url(path, me->terms_url, sizeof(me->terms_url));
        json_get_string(resp, "registrationMode", me->registration_mode, sizeof(me->registration_mode));
        const char *ws, *we;
        if (json_get_nested(resp, "welcome", &ws, &we) == 0) {
            char *w = json_copy(ws, we);
            if (w) {
                me->has_welcome = 1;
                json_get_string(w, "inviterEmail", me->welcome_inviter, sizeof(me->welcome_inviter));
                json_get_bool(w, "inviterIsOwner", &me->welcome_inviter_is_owner);
                free(w);
            }
        }
    }
    free(resp);
    return rc;
}

int api_get_my_info(int *id_out, char *email_out, int email_size, char *created_out, int created_size) {
    api_me_t me;
    if (api_get_me(&me) != 0) return -1;
    *id_out = me.id;
    snprintf(email_out, email_size, "%s", me.email);
    snprintf(created_out, created_size, "%s", me.created_at);
    return 0;
}

int api_get_user_info(int user_id, char *email_out, int email_size, char *created_out, int created_size) {
    params_t p = {0};
    pp_addi(&p, "userId", user_id);
    char *resp = NULL;
    int rc = request("getUserInfo", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) {
        json_get_string(resp, "email", email_out, email_size);
        json_get_string(resp, "created_at", created_out, created_size);
    }
    free(resp);
    return rc;
}

int api_get_user_role(int user_id, char *role_out, int role_size) {
    params_t p = {0};
    pp_addi(&p, "userId", user_id);
    char *resp = NULL;
    int rc = request("getUserInfo", pp_str(&p), &resp);
    pp_free(&p);
    snprintf(role_out, role_size, "user");
    if (rc == 0) json_get_string(resp, "role", role_out, role_size);
    free(resp);
    return rc;
}

int api_get_users(api_users_result_t *result) { return user_list("getUsers", "", "users", result); }

int api_get_user_emails(int *user_ids, int count, char *out, int out_size) {
    params_t ids = {0};
    pp_raw(&ids, "[");
    for (int i = 0; i < count && i < 256; i++) {
        char num[16];
        snprintf(num, sizeof(num), "%s%d", i ? "," : "", user_ids[i]);
        pp_raw(&ids, num);
    }
    pp_raw(&ids, "]");
    params_t p = {0};
    pp_add(&p, "userIds", pp_str(&ids));
    int rc = api_call("getUserEmails", pp_str(&p), out, out_size);
    pp_free(&p);
    pp_free(&ids);
    return rc;
}

int api_accept_terms(int version) {
    params_t p = {0};
    pp_addi(&p, "version", version);
    int rc = simple("acceptTerms", pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_dismiss_welcome(void) { return simple("dismissWelcome", ""); }

// The verify token a server hands back when a one-time code is entered, kept
// per email until the last step of that reset or registration sends it.
static struct { char email[256]; char token[160]; } g_verify[8];

static void verify_key(const char *email, char *out, int out_size) {
    while (*email && isspace((unsigned char)*email)) email++;
    int n = 0;
    for (; *email && n < out_size - 1; email++) out[n++] = (char)tolower((unsigned char)*email);
    while (n > 0 && isspace((unsigned char)out[n - 1])) n--;
    out[n] = '\0';
}

static void remember_verify_token(const char *email, const char *resp) {
    char token[160] = {0};
    if (json_get_string(resp, "verifyToken", token, sizeof(token)) != 0 || !token[0]) return;
    char key[256];
    verify_key(email, key, sizeof(key));
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (strcmp(g_verify[i].email, key) == 0 || !g_verify[i].email[0]) { slot = i; break; }
    }
    snprintf(g_verify[slot].email, sizeof(g_verify[slot].email), "%s", key);
    snprintf(g_verify[slot].token, sizeof(g_verify[slot].token), "%s", token);
}

static const char *verify_token_for(const char *email) {
    char key[256];
    verify_key(email, key, sizeof(key));
    for (int i = 0; i < 8; i++) if (g_verify[i].email[0] && strcmp(g_verify[i].email, key) == 0) return g_verify[i].token;
    return NULL;
}

int api_check_invite_code(const char *code, char *inviter_out, int inviter_size, char *expires_out, int expires_size) {
    params_t p = {0};
    pp_add(&p, "inviteCode", code);
    char *resp = NULL;
    int rc = request("checkInviteCode", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) {
        if (inviter_out) json_get_string(resp, "inviterEmail", inviter_out, inviter_size);
        if (expires_out) json_get_string(resp, "expiresAt", expires_out, expires_size);
    }
    free(resp);
    return rc;
}

int api_register_send_otp(const char *email, const char *invite_code) {
    params_t p = {0};
    pp_add(&p, "email", email);
    if (invite_code && invite_code[0]) pp_add(&p, "inviteCode", invite_code);
    int rc = simple("sendRegisterOTP", pp_str(&p));
    pp_free(&p);
    return rc;
}

static int verify_code(const char *action, const char *email, const char *otp) {
    params_t p = {0};
    pp_add(&p, "email", email);
    pp_add(&p, "otp", otp);
    char *resp = NULL;
    int rc = request(action, pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) remember_verify_token(email, resp);
    free(resp);
    return rc;
}

static int finish_with_token(const char *action, const char *email, const char *password, const char *confirm) {
    params_t p = {0};
    pp_add(&p, "email", email);
    pp_add(&p, "password", password);
    pp_add(&p, "confirm", confirm);
    const char *token = verify_token_for(email);
    if (token) pp_add(&p, "verifyToken", token);
    int rc = simple(action, pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_register_verify_otp(const char *email, const char *otp) { return verify_code("verifyRegisterOTP", email, otp); }
int api_register_finish(const char *email, const char *password, const char *confirm) { return finish_with_token("finishRegister", email, password, confirm); }

int api_send_otp(const char *email) {
    params_t p = {0};
    pp_add(&p, "email", email);
    int rc = simple("sendOTP", pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_verify_otp(const char *email, const char *otp) { return verify_code("verifyOTP", email, otp); }
int api_reset_password(const char *email, const char *password, const char *confirm) { return finish_with_token("resetPassword", email, password, confirm); }

int api_delete_account(const char *password) {
    params_t p = {0};
    pp_add(&p, "password", password);
    int rc = simple("deleteAccount", pp_str(&p));
    pp_free(&p);
    return rc;
}

// ============================================================ posts ======

int api_create_post(const char *text, const char *media_url, char *post_id_out, int post_id_size) {
    params_t p = {0};
    pp_add(&p, "postText", text);
    if (media_url && media_url[0]) pp_add(&p, "mediaUrl", media_url);
    char *resp = NULL;
    int rc = request("post", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) json_get_string(resp, "postId", post_id_out, post_id_size);
    free(resp);
    return rc;
}

static int one_post_arg(const char *action, const char *post_id) {
    params_t p = {0};
    pp_add(&p, "postId", post_id);
    int rc = simple(action, pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_delete_post(const char *post_id) { return one_post_arg("deletePost", post_id); }
int api_like_post(const char *post_id) { return one_post_arg("likePost", post_id); }
int api_unlike_post(const char *post_id) { return one_post_arg("unlikePost", post_id); }

static int posts_page(const char *action, int user_id, int offset, int limit, api_posts_result_t *result) {
    params_t p = {0};
    if (user_id > 0) pp_addi(&p, "userId", user_id);
    pp_addi(&p, "offset", offset);
    pp_addi(&p, "limit", limit);
    char *resp = NULL;
    int rc = request(action, pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) rc = parse_posts(resp, result);
    else result->count = 0;
    free(resp);
    return rc;
}

int api_get_my_posts(int offset, int limit, api_posts_result_t *result) { return posts_page("getMyPosts", 0, offset, limit, result); }
int api_get_user_posts(int user_id, int offset, int limit, api_posts_result_t *result) { return posts_page("getUserPosts", user_id, offset, limit, result); }
int api_fetch_followed_posts(int offset, int limit, api_posts_result_t *result) { return posts_page("fetchFollowedPosts", 0, offset, limit, result); }

int api_get_post_by_id(const char *post_id, api_post_t *post_out) {
    params_t p = {0};
    pp_add(&p, "postId", post_id);
    char *resp = NULL;
    int rc = request("getPostById", pp_str(&p), &resp);
    pp_free(&p);
    const char *ps, *pe;
    if (rc == 0 && json_get_nested(resp, "post", &ps, &pe) == 0) {
        char *post = json_copy(ps, pe);
        if (post) { parse_post(post, post_out); free(post); }
    } else if (rc == 0) {
        rc = -1;
    }
    free(resp);
    return rc;
}

int api_get_post_likes(const char *post_id, char *out, int out_size) {
    params_t p = {0};
    pp_add(&p, "postId", post_id);
    int rc = api_call("getPostLikes", pp_str(&p), out, out_size);
    pp_free(&p);
    return rc;
}

// ============================================================ follows ====

static int one_user_arg(const char *action, int user_id) {
    params_t p = {0};
    pp_addi(&p, "userId", user_id);
    int rc = simple(action, pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_follow_user(int user_id) { return one_user_arg("followUser", user_id); }
int api_unfollow_user(int user_id) { return one_user_arg("unfollowUser", user_id); }

int api_is_following(int user_id, int *out) {
    params_t p = {0};
    pp_addi(&p, "userId", user_id);
    char *resp = NULL;
    int rc = request("isFollowing", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) json_get_bool(resp, "following", out);
    free(resp);
    return rc;
}

static int follow_list(const char *action, const char *key, int user_id, api_users_result_t *result) {
    params_t p = {0};
    pp_addi(&p, "userId", user_id);
    int rc = user_list(action, pp_str(&p), key, result);
    pp_free(&p);
    return rc;
}

int api_get_my_follows(int user_id, api_users_result_t *result) { return follow_list("getMyFollows", "follows", user_id, result); }
int api_get_my_followers(int user_id, api_users_result_t *result) { return follow_list("getMyFollowers", "followers", user_id, result); }

// ============================================================ comments ===

int api_create_reply(const char *post_id, int parent_id, const char *text, int *comment_id_out) {
    params_t p = {0};
    pp_add(&p, "postId", post_id);
    pp_add(&p, "text", text);
    if (parent_id > 0) pp_addi(&p, "parentId", parent_id);
    char *resp = NULL;
    int rc = request("createComment", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0 && comment_id_out) json_get_int(resp, "commentId", comment_id_out);
    free(resp);
    return rc;
}

int api_create_comment(const char *post_id, const char *text, int *comment_id_out) {
    return api_create_reply(post_id, 0, text, comment_id_out);
}

static int comments_page(const char *post_id, int offset, int limit, int threaded, api_comments_result_t *result) {
    params_t p = {0};
    pp_add(&p, "postId", post_id);
    pp_addi(&p, "offset", offset);
    pp_addi(&p, "limit", limit);
    if (threaded) pp_add(&p, "threaded", "1");
    char *resp = NULL;
    int rc = request("getPostComments", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0) rc = parse_comments(resp, result, threaded);
    else result->count = 0;
    free(resp);
    return rc;
}

int api_get_post_comments(const char *post_id, int offset, int limit, api_comments_result_t *result) {
    return comments_page(post_id, offset, limit, 0, result);
}

int api_get_post_comments_threaded(const char *post_id, int offset, int limit, api_comments_result_t *result) {
    return comments_page(post_id, offset, limit, 1, result);
}

int api_delete_comment(int comment_id) {
    params_t p = {0};
    pp_addi(&p, "commentId", comment_id);
    int rc = simple("deleteComment", pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_get_post_comment_counts(char post_ids[][64], int count, char *out, int out_size) {
    params_t ids = {0};
    pp_raw(&ids, "[");
    for (int i = 0; i < count; i++) {
        if (i) pp_raw(&ids, ",");
        pp_raw(&ids, "\"");
        pp_raw(&ids, post_ids[i]);
        pp_raw(&ids, "\"");
    }
    pp_raw(&ids, "]");
    params_t p = {0};
    pp_add(&p, "postIds", pp_str(&ids));
    int rc = api_call("getPostCommentCounts", pp_str(&p), out, out_size);
    pp_free(&p);
    pp_free(&ids);
    return rc;
}

// ============================================================ notifications

int api_get_notifications(int offset, api_notifications_result_t *result) {
    params_t p = {0};
    pp_addi(&p, "offset", offset);
    char *resp = NULL;
    int rc = request("getNotifications", pp_str(&p), &resp);
    pp_free(&p);
    result->count = 0;
    const char *as, *ae;
    if (rc == 0 && json_get_array(resp, "notifications", &as, &ae) == 0) {
        int len = json_array_len(as, ae);
        if (len > 256) len = 256;
        for (int i = 0; i < len; i++) {
            char *item = json_array_item_dup(as, ae, i);
            if (!item) break;
            api_notification_t *n = &result->notifications[result->count++];
            memset(n, 0, sizeof(*n));
            json_get_int(item, "id", &n->id);
            json_get_int(item, "recipient_id", &n->recipient_id);
            json_get_int(item, "actor_id", &n->actor_id);
            json_get_string(item, "actor_email", n->actor_email, sizeof(n->actor_email));
            json_get_string(item, "type", n->type, sizeof(n->type));
            json_get_string(item, "post_id", n->post_id, sizeof(n->post_id));
            json_get_string(item, "created_at", n->created_at, sizeof(n->created_at));
            json_get_string(item, "created_tz", n->created_tz, sizeof(n->created_tz));
            json_get_int(item, "comment_id", &n->comment_id);
            json_get_bool(item, "seen", &n->seen);
            free(item);
        }
    } else if (rc == 0) {
        rc = -1;
    }
    free(resp);
    return rc;
}

int api_get_unseen_notification_count(int *count_out) {
    char *resp = NULL;
    int rc = request("getUnseenNotificationCount", "", &resp);
    if (rc == 0) json_get_int(resp, "count", count_out);
    free(resp);
    return rc;
}

int api_mark_notifications_seen(void) { return simple("markNotificationsSeen", ""); }

// ============================================================ report/block

int api_report(const char *target_type, const char *target_id, const char *reason, const char *details) {
    params_t p = {0};
    pp_add(&p, "targetType", target_type);
    pp_add(&p, "targetId", target_id);
    pp_add(&p, "reason", reason);
    if (details && details[0]) pp_add(&p, "details", details);
    int rc = simple("reportContent", pp_str(&p));
    pp_free(&p);
    return rc;
}

int api_block_user(int user_id) { return one_user_arg("blockUser", user_id); }
int api_unblock_user(int user_id) { return one_user_arg("unblockUser", user_id); }
int api_get_blocked_users(api_users_result_t *result) { return user_list("getBlockedUsers", "", "users", result); }

// ============================================================ invites ====

int api_get_my_invites(api_invites_t *out) {
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    int rc = request("getMyInvites", "", &resp);
    if (rc == 0) {
        json_get_bool(resp, "unlimited", &out->unlimited);
        json_get_bool(resp, "canGenerate", &out->can_generate);
        json_get_bool(resp, "used", &out->used);
        const char *as, *ae;
        if (json_get_array(resp, "codes", &as, &ae) == 0) {
            int len = json_array_len(as, ae);
            for (int i = 0; i < len && out->count < 32; i++) {
                char *item = json_array_item_dup(as, ae, i);
                if (!item) break;
                json_get_string(item, "code", out->codes[out->count].code, sizeof(out->codes[0].code));
                json_get_string(item, "expiresAt", out->codes[out->count].expires_at, sizeof(out->codes[0].expires_at));
                out->count++;
                free(item);
            }
        }
    }
    free(resp);
    return rc;
}

int api_generate_invite_code(char *code_out, int code_size, char *expires_out, int expires_size) {
    char *resp = NULL;
    int rc = request("generateInviteCode", "", &resp);
    if (rc == 0) {
        json_get_string(resp, "code", code_out, code_size);
        if (expires_out) json_get_string(resp, "expiresAt", expires_out, expires_size);
    }
    free(resp);
    return rc;
}

int api_cancel_invite_code(const char *code) {
    params_t p = {0};
    pp_add(&p, "code", code);
    int rc = simple("cancelInviteCode", pp_str(&p));
    pp_free(&p);
    return rc;
}

// ============================================================ devices ====

int api_get_sessions(api_sessions_t *out) {
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    int rc = request("getSessions", "", &resp);
    if (rc == 0) {
        json_get_int(resp, "maxSessions", &out->max_sessions);
        const char *as, *ae;
        if (json_get_array(resp, "sessions", &as, &ae) == 0) {
            int len = json_array_len(as, ae);
            for (int i = 0; i < len && out->count < 32; i++) {
                char *item = json_array_item_dup(as, ae, i);
                if (!item) break;
                api_session_t *s = &out->sessions[out->count++];
                json_get_string(item, "id", s->id, sizeof(s->id));
                json_get_string(item, "deviceName", s->device_name, sizeof(s->device_name));
                json_get_string(item, "createdAt", s->created_at, sizeof(s->created_at));
                json_get_string(item, "createdTz", s->created_tz, sizeof(s->created_tz));
                json_get_string(item, "lastUsedAt", s->last_used_at, sizeof(s->last_used_at));
                json_get_bool(item, "isCurrent", &s->is_current);
                free(item);
            }
        }
    }
    free(resp);
    return rc;
}

int api_revoke_session(const char *session_id, int *was_current) {
    params_t p = {0};
    pp_add(&p, "sessionId", session_id);
    char *resp = NULL;
    int rc = request("revokeSession", pp_str(&p), &resp);
    pp_free(&p);
    if (rc == 0 && was_current) json_get_bool(resp, "wasCurrent", was_current);
    free(resp);
    return rc;
}

int api_revoke_all_other_sessions(int *revoked) {
    char *resp = NULL;
    int rc = request("revokeAllOtherSessions", "", &resp);
    if (rc == 0 && revoked) json_get_int(resp, "revoked", revoked);
    free(resp);
    return rc;
}

// ============================================================ media ======

int api_get_media_limits(api_media_limits_t *out) {
    memset(out, 0, sizeof(*out));
    char *resp = NULL;
    int rc = request("getMediaLimits", "", &resp);
    if (rc == 0) {
        json_get_long(resp, "storageUsedBytes", &out->storage_used);
        json_get_long(resp, "storageLimitBytes", &out->storage_limit);
        json_get_long(resp, "maxImageBytes", &out->max_image_bytes);
        json_get_long(resp, "maxVideoBytes", &out->max_video_bytes);
        json_get_long(resp, "maxAudioBytes", &out->max_audio_bytes);
        out->video_supported = out->audio_supported = 1;
        json_get_bool(resp, "videoSupported", &out->video_supported);
        json_get_bool(resp, "audioSupported", &out->audio_supported);
        json_get_int(resp, "maxSeconds", &out->max_seconds);
    }
    free(resp);
    return rc;
}

int api_check_upload(const char *path, char *msg, int msg_size) {
    api_media_limits_t lim;
    if (api_get_media_limits(&lim) != 0) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long long size = ftell(f);
    fclose(f);
    const char *dot = strrchr(path, '.');
    char ext[16] = {0};
    if (dot) for (int i = 0; dot[i + 1] && i < 15; i++) ext[i] = (char)tolower((unsigned char)dot[i + 1]);
    static const char *videos[] = {"mp4", "mov", "m4v", "webm", "mkv", "3gp", NULL};
    static const char *audios[] = {"mp3", "wav", "m4a", "ogg", "opus", "flac", "aac", "amr", "aiff", "caf", NULL};
    int video = 0, audio = 0;
    for (int i = 0; videos[i]; i++) if (!strcmp(ext, videos[i])) video = 1;
    for (int i = 0; audios[i]; i++) if (!strcmp(ext, audios[i])) audio = 1;
    if (video && !lim.video_supported) { snprintf(msg, msg_size, "Video uploads aren't available on the server right now."); return -1; }
    if (audio && !lim.audio_supported) { snprintf(msg, msg_size, "Audio uploads aren't available on the server right now."); return -1; }
    long long max = video ? lim.max_video_bytes : audio ? lim.max_audio_bytes : lim.max_image_bytes;
    if (max > 0 && size > max) {
        snprintf(msg, msg_size, "That file is too large. The limit for %s is %lld MB.", video ? "video" : audio ? "audio" : "images", max / 1048576);
        return -1;
    }
    if (lim.storage_limit > 0 && lim.storage_used + size > lim.storage_limit) {
        if (lim.storage_limit % 1073741824LL == 0)
            snprintf(msg, msg_size, "You have reached your media storage limit of %lld GB of media.", lim.storage_limit / 1073741824LL);
        else
            snprintf(msg, msg_size, "You have reached your media storage limit of %lld MB of media.", lim.storage_limit / 1048576LL);
        return -1;
    }
    return 0;
}

// media.php identifies a file from its own bytes, so the Content-Type sent
// is only a hint.
static const char *guess_mime_type(const char *filepath) {
    const char *dot = strrchr(filepath, '.');
    if (!dot) return "application/octet-stream";
    const char *e = dot + 1;
    if (!strcasecmp(e, "jpg") || !strcasecmp(e, "jpeg")) return "image/jpeg";
    if (!strcasecmp(e, "png")) return "image/png";
    if (!strcasecmp(e, "gif")) return "image/gif";
    if (!strcasecmp(e, "webp")) return "image/webp";
    if (!strcasecmp(e, "heic")) return "image/heic";
    if (!strcasecmp(e, "mov")) return "video/quicktime";
    if (!strcasecmp(e, "mp4") || !strcasecmp(e, "m4v")) return "video/mp4";
    if (!strcasecmp(e, "webm")) return "video/webm";
    if (!strcasecmp(e, "wav")) return "audio/wav";
    if (!strcasecmp(e, "mp3")) return "audio/mpeg";
    if (!strcasecmp(e, "m4a")) return "audio/mp4";
    return "application/octet-stream";
}

static long upload_once(const char *filepath, char **resp) {
    CURL *curl = curl_easy_init();   // only to build the multipart body
    if (!curl) { set_error("Failed to init curl"); *resp = NULL; return -1; }
    curl_mime *mime = curl_mime_init(curl);
    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_filedata(part, filepath);
    curl_mime_name(part, "file");
    curl_mime_type(part, guess_mime_type(filepath));
    part = curl_mime_addpart(mime);
    curl_mime_data(part, "uploadMedia", CURL_ZERO_TERMINATED);
    curl_mime_name(part, "action");
    char url[600];
    media_endpoint(url, sizeof(url));
    long status = http_post(url, NULL, mime, 120L, resp);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return status;
}

int api_upload_media_with_id(const char *filepath, char *url_out, int url_size, int *id_out) {
    char *resp = NULL;
    long status = upload_once(filepath, &resp);
    if (status == 503) {   // every conversion slot busy: one more try
        free(resp);
        sleep(3);
        status = upload_once(filepath, &resp);
    }
    int rc = judge(status, resp ? resp : "", "Upload failed");
    if (rc == 0 && json_get_string(resp, "mediaUrl", url_out, url_size) != 0) {
        set_error("Upload failed");
        rc = -1;
    }
    if (rc != 0 && status == 429 && !g_last_error[0]) set_error("Too many uploads. Try again later.");
    if (rc == 0 && id_out) {
        *id_out = 0;
        json_get_int(resp, "mediaId", id_out);
    }
    free(resp);
    return rc;
}

int api_upload_media(const char *filepath, char *url_out, int url_size) {
    return api_upload_media_with_id(filepath, url_out, url_size, NULL);
}

int api_delete_media(int media_id) {
    params_t p = {0};
    pp_add(&p, "action", "deleteMedia");
    pp_addi(&p, "mediaId", media_id);
    char url[600];
    media_endpoint(url, sizeof(url));
    char *resp = NULL;
    long status = http_post(url, pp_str(&p), NULL, 30L, &resp);
    pp_free(&p);
    int rc = judge(status, resp ? resp : "", "Delete failed");
    free(resp);
    return rc;
}
