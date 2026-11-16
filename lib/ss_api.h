#ifndef SS_API_H
#define SS_API_H

// Responses of any size are read whole (never cut off); this is only the
// size of the few fixed buffers callers still pass to api_call().
#define API_MAX_RESPONSE 65536
#define API_MAX_POSTS 256
#define API_MAX_COMMENTS 256
#define API_MAX_LINE 512

typedef struct {
    int id;
    char email[256];
    char created_at[32];
    char created_tz[8];      // the creator's offset from GMT ("-06:00"), "" when unknown
} api_user_t;

typedef struct {
    char id[64];
    char text[5000];
    char timestamp[32];      // UTC, "YYYY-MM-DD HH:MM:SS"
    int like_count;
    int is_liked;
    char media_url[512];
    int user_id;
    char user_email[256];
    // Newer fields (servers from 2026-10 on). All zero/empty when absent.
    char timestamp_tz[8];    // the poster's offset from GMT
    int comment_count;
    int reported_by_me;      // the viewer reported it: shown folded
    int frozen;              // hidden by staff; only ever your own post
    char media_type[16];     // image | video | audio
    char media_variant[512]; // smaller copy for feeds
    char media_poster[512];  // a video's still image
    int media_loop;          // an animated GIF converted to a looping video
} api_post_t;

typedef struct {
    int id;
    int post_id;
    int user_id;
    char text[5000];
    char created_at[32];
    char user_email[256];
    // Newer fields.
    char created_tz[8];
    int parent_id;           // 0 for a top-level comment; else the comment it replies to
    int reply_count;         // top-level comments in a threaded list
    int deleted;             // a "Comment deleted" placeholder
    int frozen_by_staff;     // a "Comment frozen by staff" placeholder
    int frozen;              // your own comment, hidden by staff
    int reported_by_me;
} api_comment_t;

typedef struct {
    int id;
    int recipient_id;
    int actor_id;
    char actor_email[256];
    char type[32];
    char post_id[64];
    char created_at[32];
    // Newer fields.
    char created_tz[8];
    int comment_id;          // the comment it is about, 0 when none
    int seen;
} api_notification_t;

typedef struct {
    api_post_t posts[API_MAX_POSTS];
    int count;
    int has_more;
    int total_count;
} api_posts_result_t;

typedef struct {
    // Threaded lists come back flattened: each top-level comment followed by
    // its replies (parent_id set), oldest reply first.
    api_comment_t comments[API_MAX_COMMENTS];
    int count;
    int has_more;
    int total_count;
    int truncated;           // more replies than fit
} api_comments_result_t;

typedef struct {
    api_user_t users[256];
    int count;
} api_users_result_t;

typedef struct {
    api_notification_t notifications[256];
    int count;
} api_notifications_result_t;

// ---- Basics -------------------------------------------------------------
int api_init(void);
void api_cleanup(void);
void api_set_jwt(const char *jwt);
void api_set_user_id(int user_id);
// Calls an action and copies the whole answer into response. Returns -1 (and
// says so in the last error) when the answer doesn't fit: never cuts it off.
int api_call(const char *action, const char *params, char *response, int resp_size);
// The last failure: its message (as the server wrote it, when it sent one),
// the HTTP status, and the server's machine-readable code ("media_quota").
const char *api_get_last_error(void);
int api_get_last_status(void);
const char *api_get_last_code(void);

// Identifies this tool to the server, which parses it into the device name
// shown in the account's device list. Set once at startup.
void api_set_user_agent(const char *ua);

// The website's address for a server path ("/terms" ->
// "https://app.davidfruin.com/terms"), from the configured API address.
void api_site_url(const char *path, char *out, int out_size);

// ---- Sessions ----------------------------------------------------------
void api_set_refresh_token(const char *refresh);
const char *api_get_refresh_token(void);
int api_refresh_session(void);
void api_set_token_refreshed_cb(void (*cb)(const char *jwt));

// ---- Account ------------------------------------------------------------
typedef struct {
    int id;
    char email[256];
    char created_at[32];
    char role[16];                 // user | moderator | admin | owner
    int terms_accepted;            // the version this account accepted (0 = never)
    int terms_current;             // the version in force now
    char terms_url[256];           // full address of the Terms of Use
    int has_welcome;               // a one-time welcome is waiting
    char welcome_inviter[256];     // who invited them ("" if that account is gone)
    int welcome_inviter_is_owner;
    char registration_mode[16];    // invite | open
} api_me_t;

int api_login(const char *email, const char *password, char *jwt_out, int jwt_size, int *user_id_out);
int api_logout(void);
int api_get_me(api_me_t *me);
int api_get_my_info(int *id_out, char *email_out, int email_size, char *created_out, int created_size);
int api_get_user_info(int user_id, char *email_out, int email_size, char *created_out, int created_size);
// role_out: user | moderator | admin | owner
int api_get_user_role(int user_id, char *role_out, int role_size);
int api_get_users(api_users_result_t *result);
int api_get_user_emails(int *user_ids, int count, char *out, int out_size);
int api_accept_terms(int version);
int api_dismiss_welcome(void);

// Registration is code-first: check the invite code, send the email code
// with it, verify that, then finish. The token verify hands back is kept
// inside the library and sent with finish, by email.
int api_check_invite_code(const char *code, char *inviter_out, int inviter_size, char *expires_out, int expires_size);
int api_register_send_otp(const char *email, const char *invite_code);
int api_register_verify_otp(const char *email, const char *otp);
int api_register_finish(const char *email, const char *password, const char *confirm);
// Password reset: the same verify-token rule.
int api_send_otp(const char *email);
int api_verify_otp(const char *email, const char *otp);
int api_reset_password(const char *email, const char *password, const char *confirm);
int api_delete_account(const char *password);

// ---- Posts --------------------------------------------------------------
int api_create_post(const char *text, const char *media_url, char *post_id_out, int post_id_size);
int api_delete_post(const char *post_id);
int api_get_my_posts(int offset, int limit, api_posts_result_t *result);
int api_get_user_posts(int user_id, int offset, int limit, api_posts_result_t *result);
int api_fetch_followed_posts(int offset, int limit, api_posts_result_t *result);
int api_get_post_by_id(const char *post_id, api_post_t *post_out);
int api_like_post(const char *post_id);
int api_unlike_post(const char *post_id);
int api_get_post_likes(const char *post_id, char *out, int out_size);

// ---- Follows ------------------------------------------------------------
int api_follow_user(int user_id);
int api_unfollow_user(int user_id);
int api_is_following(int user_id, int *out);
int api_get_my_follows(int user_id, api_users_result_t *result);
int api_get_my_followers(int user_id, api_users_result_t *result);

// ---- Comments -----------------------------------------------------------
int api_create_comment(const char *post_id, const char *text, int *comment_id_out);
// A reply to the top-level comment parent_id (0 = a top-level comment).
int api_create_reply(const char *post_id, int parent_id, const char *text, int *comment_id_out);
// The flat list (newest first, replies as ordinary comments).
int api_get_post_comments(const char *post_id, int offset, int limit, api_comments_result_t *result);
// Threads: top-level comments (newest first), each followed by its replies.
int api_get_post_comments_threaded(const char *post_id, int offset, int limit, api_comments_result_t *result);
int api_delete_comment(int comment_id);
int api_get_post_comment_counts(char post_ids[][64], int count, char *out, int out_size);

// ---- Notifications ------------------------------------------------------
int api_get_notifications(int offset, api_notifications_result_t *result);
int api_get_unseen_notification_count(int *count_out);
int api_mark_notifications_seen(void);

// ---- Report and block ---------------------------------------------------
// target_type: post | comment | user. reason: one of api_report_reasons.
extern const char *const api_report_reasons[];   // NULL-terminated
int api_report(const char *target_type, const char *target_id, const char *reason, const char *details);
int api_block_user(int user_id);
int api_unblock_user(int user_id);
int api_get_blocked_users(api_users_result_t *result);

// ---- Invites ------------------------------------------------------------
typedef struct {
    int unlimited;                 // the owner: as many codes as they like
    int can_generate;
    int used;                      // this member's one invite is spent
    int count;
    struct { char code[32]; char expires_at[32]; } codes[32];
} api_invites_t;

int api_get_my_invites(api_invites_t *out);
int api_generate_invite_code(char *code_out, int code_size, char *expires_out, int expires_size);
int api_cancel_invite_code(const char *code);

// ---- Devices ------------------------------------------------------------
typedef struct {
    char id[64];
    char device_name[128];
    char created_at[32];
    char created_tz[8];
    char last_used_at[32];
    int is_current;
} api_session_t;

typedef struct {
    api_session_t sessions[32];
    int count;
    int max_sessions;
} api_sessions_t;

int api_get_sessions(api_sessions_t *out);
int api_revoke_session(const char *session_id, int *was_current);
int api_revoke_all_other_sessions(int *revoked);

// ---- Media --------------------------------------------------------------
typedef struct {
    long long storage_used;
    long long storage_limit;
    long long max_image_bytes, max_video_bytes, max_audio_bytes;
    int video_supported, audio_supported;
    int max_seconds;
} api_media_limits_t;

int api_get_media_limits(api_media_limits_t *out);
// Uploads a file. A busy server (503) is retried once after 3 seconds. On
// failure the last error is the server's message; a full storage quota is
// status 413 with code "media_quota".
int api_upload_media(const char *filepath, char *url_out, int url_size);
int api_upload_media_with_id(const char *filepath, char *url_out, int url_size, int *id_out);
int api_delete_media(int media_id);

// ---- Times --------------------------------------------------------------
// This computer's offset from GMT as "+HH:MM"/"-HH:MM"; sent with every
// request so the server records where things were made.
void api_local_gmt_offset(char *out, int out_size);
// "UTC 2026-09-16 19:20:03 (GMT-06:00)": how every time is shown.
void api_format_time(const char *utc, const char *tz, char *out, int out_size);

#endif
