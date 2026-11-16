#include "output.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_color_enabled = 0;
int g_json_enabled = 0;
int g_show_reported = 0;

const char *C_RESET = "";
const char *C_BOLD = "";
const char *C_CYAN = "";
const char *C_GREEN = "";
const char *C_YELLOW = "";
const char *C_RED = "";
const char *C_DIM = "";

void output_init(void) {
    if (g_color_enabled) {
        C_RESET = "\033[0m";
        C_BOLD = "\033[1m";
        C_CYAN = "\033[36m";
        C_GREEN = "\033[32m";
        C_YELLOW = "\033[33m";
        C_RED = "\033[31m";
        C_DIM = "\033[2m";
    }
}

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"') fputs("\\\"", f);
        else if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static int is_null_media(const char *s) {
    return s[0] == '\0' || strcmp(s, "null") == 0;
}

// "UTC 2026-09-16 19:20:03 (GMT-06:00)"
static const char *when(const char *utc, const char *tz) {
    static char buf[4][64];
    static int k = 0;
    k = (k + 1) % 4;
    api_format_time(utc, tz, buf[k], sizeof(buf[k]));
    return buf[k];
}

static void json_str_or_null(const char *s) {
    if (s && s[0]) json_escape(stdout, s); else printf("null");
}

// The fields newer servers add to a post, appended to its JSON object
// (existing fields are never renamed or removed: scripts depend on them).
static void post_extra_json(const api_post_t *p) {
    printf(",\"timestampTz\":");
    json_str_or_null(p->timestamp_tz);
    printf(",\"commentCount\":%d,\"reportedByMe\":%s,\"frozen\":%s",
           p->comment_count, p->reported_by_me ? "true" : "false", p->frozen ? "true" : "false");
    if (p->media_type[0]) {
        printf(",\"media\":{\"type\":");
        json_escape(stdout, p->media_type);
        printf(",\"url\":");
        json_str_or_null(p->media_url);
        printf(",\"variantUrl\":");
        json_str_or_null(p->media_variant);
        printf(",\"posterUrl\":");
        json_str_or_null(p->media_poster);
        printf(",\"loop\":%s}", p->media_loop ? "true" : "false");
    }
}

// What a post's media is, in words: "video", "GIF (plays as a loop)".
static const char *media_kind(const api_post_t *p) {
    if (p->media_loop) return "GIF (plays as a loop)";
    if (p->media_type[0]) return p->media_type;
    return "media";
}

// The text as a list or detail view shows it: folded when you reported it
// (unless --show-reported), labelled when staff hid your own post.
static const char *post_text_view(const api_post_t *p) {
    if (p->reported_by_me && !g_show_reported) return "(You reported this post. Add --show-reported to see it.)";
    return p->text;
}

void print_posts(const api_posts_result_t *result) {
    if (g_json_enabled) {
        printf("{\"posts\":[");
        for (int i = 0; i < result->count; i++) {
            const api_post_t *p = &result->posts[i];
            if (i) printf(",");
            printf("{\"id\":");
            json_escape(stdout, p->id);
            printf(",\"text\":");
            json_escape(stdout, p->text);
            printf(",\"timestamp\":");
            json_escape(stdout, p->timestamp);
            printf(",\"userID\":%d,\"userEmail\":", p->user_id);
            json_escape(stdout, p->user_email);
            printf(",\"likeCount\":%d,\"isLiked\":%s", p->like_count, p->is_liked ? "true" : "false");
            if (!is_null_media(p->media_url)) { printf(",\"mediaUrl\":"); json_escape(stdout, p->media_url); } else printf(",\"mediaUrl\":null");
            printf(",\"likes\":%d", p->like_count);
            post_extra_json(p);
            printf("}");
        }
        printf("],\"hasMore\":%s,\"totalCount\":%d}\n", result->has_more ? "true" : "false", result->total_count);
        return;
    }
    if (result->count == 0) {
        fprintf(stderr, "No posts found.\n");
        return;
    }
    printf("%s%-10s  %-40s  %s%-6s  %s%s\n", C_BOLD, "ID", "Author", C_RESET, "Likes", C_BOLD, "Text");
    printf("%s", C_DIM);
    for (int i = 0; i < 70; i++) putchar('-');
    printf("%s\n", C_RESET);
    for (int i = 0; i < result->count; i++) {
        const api_post_t *p = &result->posts[i];
        char shown[5100];
        snprintf(shown, sizeof(shown), "%s%s%s", p->frozen ? "[Hidden by a moderator] " : "",
                 post_text_view(p), (!is_null_media(p->media_url) && !(p->reported_by_me && !g_show_reported)) ? " [media]" : "");
        char text[41];
        snprintf(text, sizeof(text), "%.40s", shown);
        if (strlen(shown) > 40) strcpy(text + 37, "...");
        char id_short[11] = {0};
        strncpy(id_short, p->id, 10);
        if (strlen(p->id) > 10) strcpy(id_short + 7, "...");
        if (p->is_liked)
            printf("%-10s  %-40s  %s%*d%s  %s\n", id_short, p->user_email, C_GREEN, 6, p->like_count, C_RESET, text);
        else
            printf("%-10s  %-40s  %*d  %s\n", id_short, p->user_email, 6, p->like_count, text);
    }
}

void print_post(const api_post_t *post) {
    if (g_json_enabled) {
        printf("{\"post\":{\"id\":");
        json_escape(stdout, post->id);
        printf(",\"text\":");
        json_escape(stdout, post->text);
        printf(",\"timestamp\":");
        json_escape(stdout, post->timestamp);
        printf(",\"userID\":%d,\"userEmail\":", post->user_id);
        json_escape(stdout, post->user_email);
        printf(",\"likeCount\":%d,\"isLiked\":%s", post->like_count, post->is_liked ? "true" : "false");
        if (!is_null_media(post->media_url)) { printf(",\"mediaUrl\":"); json_escape(stdout, post->media_url); } else printf(",\"mediaUrl\":null");
        post_extra_json(post);
        printf("}}\n");
        return;
    }
    printf("%sPost:%s %s\n", C_BOLD, C_RESET, post->id);
    printf("%sAuthor:%s %s (ID: %d)\n", C_BOLD, C_RESET, post->user_email, post->user_id);
    printf("%sDate:%s   %s\n", C_BOLD, C_RESET, when(post->timestamp, post->timestamp_tz));
    if (post->frozen) {
        printf("%sHidden by a moderator.%s Only you can see this post. It can't be liked or commented on.\n", C_YELLOW, C_RESET);
    } else {
        printf("%sLikes:%s  %d", C_BOLD, C_RESET, post->like_count);
        if (post->is_liked)
            printf(" %s(you liked this)%s", C_GREEN, C_RESET);
        printf("    %sComments:%s %d\n", C_BOLD, C_RESET, post->comment_count);
    }
    int folded = post->reported_by_me && !g_show_reported;
    if (!is_null_media(post->media_url) && !folded)
        printf("%sMedia:%s  %s  %s\n", C_BOLD, C_RESET, media_kind(post), post->media_url);
    printf("\n%s%s%s\n", folded ? C_DIM : C_BOLD, post_text_view(post), C_RESET);
}

void print_users(const api_users_result_t *result) {
    if (g_json_enabled) {
        printf("{\"users\":[");
        for (int i = 0; i < result->count; i++) {
            const api_user_t *u = &result->users[i];
            if (i) printf(",");
            printf("{\"id\":%d,\"email\":", u->id);
            json_escape(stdout, u->email);
            printf(",\"created_at\":");
            json_escape(stdout, u->created_at);
            if (u->created_tz[0]) { printf(",\"created_tz\":"); json_escape(stdout, u->created_tz); }
            printf("}");
        }
        printf("]}\n");
        return;
    }
    if (result->count == 0) {
        fprintf(stderr, "No users found.\n");
        return;
    }
    printf("%s%-6s  %-40s  %s%s\n", C_BOLD, "ID", "Email", "Joined", C_RESET);
    printf("%s", C_DIM);
    for (int i = 0; i < 60; i++) putchar('-');
    printf("%s\n", C_RESET);
    for (int i = 0; i < result->count; i++) {
        const api_user_t *u = &result->users[i];
        printf("%-6d  %-40s  %s\n", u->id, u->email, when(u->created_at, u->created_tz));
    }
}

// Same shape as print_users, but the JSON key is "likes" - who liked a post
// isn't "every user", and every other command emits a bare object keyed by
// what it actually returned rather than a generic one.
void print_likes(const api_users_result_t *result) {
    if (g_json_enabled) {
        printf("{\"likes\":[");
        for (int i = 0; i < result->count; i++) {
            const api_user_t *u = &result->users[i];
            if (i) printf(",");
            printf("{\"id\":%d,\"email\":", u->id);
            json_escape(stdout, u->email);
            printf(",\"created_at\":");
            json_escape(stdout, u->created_at);
            printf("}");
        }
        printf("]}\n");
        return;
    }
    print_users(result);
}

void print_user_info(int id, const char *email, const char *created_at) {
    if (g_json_enabled) {
        printf("{\"id\":%d,\"email\":", id);
        json_escape(stdout, email);
        printf(",\"created_at\":");
        json_escape(stdout, created_at);
        printf("}\n");
        return;
    }
    printf("%sID:%s     %d\n", C_BOLD, C_RESET, id);
    printf("%sEmail:%s  %s\n", C_BOLD, C_RESET, email);
    printf("%sJoined:%s %s\n", C_BOLD, C_RESET, when(created_at, NULL));
}

void print_comments(const api_comments_result_t *result) {
    if (g_json_enabled) {
        printf("{\"comments\":[");
        for (int i = 0; i < result->count; i++) {
            const api_comment_t *c = &result->comments[i];
            if (i) printf(",");
            printf("{\"id\":%d,\"user_id\":%d,\"user_email\":", c->id, c->user_id);
            json_escape(stdout, c->user_email);
            printf(",\"text\":");
            json_escape(stdout, c->text);
            printf(",\"created_at\":");
            json_escape(stdout, c->created_at);
            printf(",\"created_tz\":");
            json_str_or_null(c->created_tz);
            printf(",\"parent_id\":");
            if (c->parent_id) printf("%d", c->parent_id); else printf("null");
            printf(",\"replyCount\":%d,\"deleted\":%s,\"frozenByStaff\":%s,\"frozen\":%s,\"reportedByMe\":%s}",
                   c->reply_count, c->deleted ? "true" : "false", c->frozen_by_staff ? "true" : "false",
                   c->frozen ? "true" : "false", c->reported_by_me ? "true" : "false");
        }
        printf("],\"hasMore\":%s,\"totalCount\":%d}\n", result->has_more ? "true" : "false", result->total_count);
        return;
    }
    if (result->count == 0) {
        printf("No comments.\n");
        return;
    }
    for (int i = 0; i < result->count; i++) {
        const api_comment_t *c = &result->comments[i];
        const char *indent = c->parent_id ? "      \xe2\x86\xb3 " : "  ";   // "↳" under its comment
        if (c->deleted) { printf("%s%s[%d] Comment deleted%s\n", indent, C_DIM, c->id, C_RESET); continue; }
        if (c->frozen_by_staff) { printf("%s%s[%d] Comment frozen by staff%s\n", indent, C_DIM, c->id, C_RESET); continue; }
        const char *text = c->text;
        if (c->reported_by_me && !g_show_reported) text = "(You reported this comment. Add --show-reported to see it.)";
        printf("%s%s[%d]%s %s%s (%s)%s: %s%s\n",
               indent, C_DIM, c->id, C_RESET,
               C_CYAN, c->user_email, when(c->created_at, c->created_tz), C_RESET,
               c->frozen ? "[Hidden by a moderator] " : "", text);
    }
    if (result->truncated) printf("%s(Some replies weren't shown: too many to list.)%s\n", C_DIM, C_RESET);
}

// One sentence per notification type the server sends; anything newer
// still gets a line rather than nothing.
static void notification_sentence(const api_notification_t *n, char *out, int size) {
    const char *a = n->actor_email[0] ? n->actor_email : "Someone";
    const char *t = n->type;
    if (!strcmp(t, "like")) snprintf(out, size, "%s liked your post", a);
    else if (!strcmp(t, "unlike")) snprintf(out, size, "%s unliked your post", a);
    else if (!strcmp(t, "comment")) snprintf(out, size, "%s commented on your post", a);
    else if (!strcmp(t, "reply")) snprintf(out, size, "%s replied to your comment", a);
    else if (!strcmp(t, "thread_reply")) snprintf(out, size, "%s also replied in a thread you replied in", a);
    else if (!strcmp(t, "follow")) snprintf(out, size, "%s started following you", a);
    else if (!strcmp(t, "unfollow")) snprintf(out, size, "%s unfollowed you", a);
    else if (!strcmp(t, "mention")) snprintf(out, size, "%s mentioned you", a);
    else snprintf(out, size, "%s did something", a);
}

void print_notifications(const api_notifications_result_t *result) {
    if (g_json_enabled) {
        printf("{\"notifications\":[");
        for (int i = 0; i < result->count; i++) {
            const api_notification_t *n = &result->notifications[i];
            if (i) printf(",");
            printf("{\"id\":%d,\"recipient_id\":%d,\"actor_id\":%d,\"actor_email\":", n->id, n->recipient_id, n->actor_id);
            json_escape(stdout, n->actor_email);
            printf(",\"type\":");
            json_escape(stdout, n->type);
            printf(",\"post_id\":");
            if (n->post_id[0]) json_escape(stdout, n->post_id); else printf("null");
            printf(",\"created_at\":");
            json_escape(stdout, n->created_at);
            printf(",\"created_tz\":");
            json_str_or_null(n->created_tz);
            printf(",\"comment_id\":");
            if (n->comment_id) printf("%d", n->comment_id); else printf("null");
            printf(",\"seen\":%s}", n->seen ? "true" : "false");
        }
        printf("]}\n");
        return;
    }
    if (result->count == 0) {
        printf("No notifications.\n");
        return;
    }
    for (int i = 0; i < result->count; i++) {
        const api_notification_t *n = &result->notifications[i];
        char sentence[400];
        notification_sentence(n, sentence, sizeof(sentence));
        printf("%s%s%s  %s%s", n->seen ? "" : C_BOLD, sentence, C_RESET, C_DIM, when(n->created_at, n->created_tz));
        if (n->post_id[0]) printf("  post %s", n->post_id);
        if (n->comment_id) printf(" comment %d", n->comment_id);
        printf("%s\n", C_RESET);
    }
}

// "Owner", "Admin", "Moderator", "User": the public badge on every profile.
const char *role_label(const char *role) {
    if (!role) return "User";
    if (!strcmp(role, "owner")) return "Owner";
    if (!strcmp(role, "admin")) return "Admin";
    if (!strcmp(role, "moderator")) return "Moderator";
    return "User";
}

void print_profile(int user_id, const char *email, const char *created_at,
                   int follower_count, int following_count, const char *role) {
    if (g_json_enabled) {
        printf("{\"id\":%d,\"email\":", user_id);
        json_escape(stdout, email);
        printf(",\"created_at\":");
        json_escape(stdout, created_at);
        printf(",\"followers\":%d,\"following\":%d,\"role\":", follower_count, following_count);
        json_escape(stdout, role && role[0] ? role : "user");
        printf("}\n");
        return;
    }
    printf("%sID:%s         %d\n", C_BOLD, C_RESET, user_id);
    printf("%sEmail:%s      %s\n", C_BOLD, C_RESET, email);
    printf("%sRole:%s       %s\n", C_BOLD, C_RESET, role_label(role));
    printf("%sJoined:%s     %s\n", C_BOLD, C_RESET, when(created_at, NULL));
    printf("%sFollowers:%s  %d\n", C_BOLD, C_RESET, follower_count);
    printf("%sFollowing:%s  %d\n", C_BOLD, C_RESET, following_count);
}

void print_success(const char *msg) {
    if (g_json_enabled) {
        printf("{\"ok\":true,\"message\":");
        json_escape(stdout, msg);
        printf("}\n");
        return;
    }
    fprintf(stderr, "%s%s%s\n", C_GREEN, msg, C_RESET);
}

void print_error(const char *msg) {
    if (g_json_enabled) {
        printf("{\"ok\":false,\"error\":");
        json_escape(stdout, msg);
        printf("}\n");
    }
    fprintf(stderr, "%sError: %s%s\n", C_RED, msg, C_RESET);
}

void print_json_error(const char *msg) {
    printf("{\"ok\":false,\"error\":");
    json_escape(stdout, msg);
    printf("}\n");
    fprintf(stderr, "%sError: %s%s\n", C_RED, msg, C_RESET);
}

void print_count(int count) {
    printf("%d\n", count);
}

void print_count_json(int count, const char *key) {
    printf("{\"%s\":%d}\n", key, count);
}
