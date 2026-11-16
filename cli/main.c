#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ss_api.h"
#include "ss_config.h"
#include "ss_state.h"
#include "output.h"
#include "ss_json.h"
#include <ctype.h>

static ss_state_t state;
static api_me_t me;          // filled by auto_login()
static int me_loaded = 0;

// The website's Terms of Use and the age rule, asked at registration and
// whenever the terms change (the web and phone apps ask the same).
#define TERMS_QUESTION "I agree to the Terms of Use (%s) and I'm at least 13 years old"

// Persists an access token the library renewed on its own, so the next
// command starts with a live one instead of refreshing all over again.
static void on_token_refreshed(const char *jwt) {
    ss_state_set_jwt(&state, jwt);
    ss_state_save_jwt(&state);
}

// Identifies this tool before anything touches state or the network: it
// decides which token directory is used and what the server records as the
// device name.
static void init_client(void) {
    ss_state_set_app("cli");
    api_set_user_agent("simple-social-cli");
    api_set_token_refreshed_cb(on_token_refreshed);
}

static void auto_login(void) {
    config_t cfg;
    config_load(&cfg);
    api_init();

    // Loaded before the first request, so an aged-out access token can be
    // renewed without the user noticing.
    if (ss_state_load_refresh(&state) == 0) api_set_refresh_token(state.refresh);

    if (ss_state_load_jwt(&state) == 0) {
        api_set_jwt(state.jwt);
        if (api_get_me(&me) == 0) {
            me_loaded = 1;
            ss_state_set_user(&state, me.id, me.email, me.created_at);
            api_set_user_id(me.id);
            return;
        }
        ss_state_clear(&state);
    }
}

// Shown once per login, the first time a command runs after it, then
// cleared on the server. Same words as the website.
static void show_welcome_once(void) {
    if (!me_loaded || !me.has_welcome || g_json_enabled) return;
    if (me.welcome_inviter[0] && me.welcome_inviter_is_owner)
        fprintf(stderr, "%sWelcome to Simple Social!%s You were invited by %s, who runs Simple Social.\n", C_BOLD, C_RESET, me.welcome_inviter);
    else if (me.welcome_inviter[0])
        fprintf(stderr, "%sWelcome to Simple Social!%s %s used their only invite on you, so use yours just as wisely.\n", C_BOLD, C_RESET, me.welcome_inviter);
    else
        fprintf(stderr, "%sWelcome to Simple Social!%s\n", C_BOLD, C_RESET);
    api_dismiss_welcome();
}

// Until the current terms are accepted, every command says so on stderr
// (never on stdout, so --json output stays clean).
static void terms_notice(void) {
    if (!me_loaded || me.terms_current <= 0 || me.terms_accepted >= me.terms_current) return;
    fprintf(stderr, "%sNotice:%s the Terms of Use have changed. Read them at %s and run: simple-social-cli accept-terms\n",
            C_YELLOW, C_RESET, me.terms_url);
}

// "UTC 2026-09-16 19:20:03 (GMT-06:00)"
static const char *when(const char *utc, const char *tz) {
    static char buf[4][64];
    static int k = 0;
    k = (k + 1) % 4;
    api_format_time(utc, tz, buf[k], sizeof(buf[k]));
    return buf[k];
}

// Reads one line from stdin into out. Returns 0 on a line, -1 on end of input.
static int read_line(const char *prompt, char *out, int size) {
    if (prompt) fprintf(stderr, "%s", prompt);
    if (!fgets(out, size, stdin)) return -1;
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static int check_media_before_upload(const char *path);

static int require_auth(void) {
    if (!ss_state_is_logged_in(&state)) {
        print_error("Not logged in. Run: simple-social-cli login <email> <password>");
        return -1;
    }
    return 0;
}

// Logs in and saves the session. Returns 0 or -1 (error already shown).
static int do_login(const char *email, const char *password) {
    char jwt[1024] = {0};
    int user_id = 0;
    if (api_login(email, password, jwt, sizeof(jwt), &user_id) != 0) {
        print_error(api_get_last_error());
        return -1;
    }
    ss_state_set_jwt(&state, jwt);
    api_set_jwt(jwt);
    api_set_user_id(user_id);
    ss_state_save_jwt(&state);
    // Without this the session would still end after the access token's
    // lifetime, which is the whole thing this is meant to avoid.
    ss_state_set_refresh(&state, api_get_refresh_token());
    ss_state_save_refresh(&state);
    if (api_get_me(&me) == 0) me_loaded = 1;
    ss_state_set_user(&state, me.id ? me.id : user_id, me.email, me.created_at);
    ss_state_save_user(&state);
    return 0;
}

static int cmd_login(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: simple-social-cli login <email> <password>\n"); return 1; }
    config_t cfg;
    config_load(&cfg);
    api_init();
    if (do_login(argv[0], argv[1]) != 0) return 1;
    if (g_json_enabled) printf("{\"ok\":true,\"userId\":%d,\"email\":\"%s\"}\n", state.user.user_id, state.user.email);
    else fprintf(stderr, "Logged in as %s (ID: %d)\n", state.user.email, state.user.user_id);
    show_welcome_once();
    terms_notice();
    return 0;
}

static int cmd_logout(int argc, char **argv) {
    (void)argc; (void)argv;
    // Tell the server first: deleting the local files alone would leave the
    // session live for the rest of its 30 days, still listed as a signed-in
    // device. A failure here still clears local state - being unable to
    // reach the server shouldn't leave you stuck logged in.
    api_logout();
    ss_state_clear(&state);
    ss_state_delete_files();
    if (g_json_enabled) printf("{\"ok\":true}\n"); else fprintf(stderr, "Logged out.\n");
    return 0;
}

static int cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    if (require_auth() != 0) return 1;
    print_user_info(state.user.user_id, state.user.email, state.user.created_at);
    if (!g_json_enabled) {
        if (me_loaded) printf("%sRole:%s   %s\n", C_BOLD, C_RESET, role_label(me.role));
        api_media_limits_t lim;
        if (api_get_media_limits(&lim) == 0 && lim.storage_limit > 0)
            printf("%sMedia:%s  %.1f MB of %.0f MB used\n", C_BOLD, C_RESET,
                   lim.storage_used / 1048576.0, lim.storage_limit / 1048576.0);
    }
    return 0;
}

// Asks the age-and-terms question. --agree answers it for scripts; --json
// mode never prompts, so it needs --agree.
static int ask_terms(const char *terms_url, int agreed_by_flag) {
    if (agreed_by_flag) return 0;
    if (g_json_enabled) { print_error("Add --agree to accept the Terms of Use and confirm you are at least 13."); return -1; }
    char q[512], answer[16];
    snprintf(q, sizeof(q), TERMS_QUESTION "? [y/N] ", terms_url);
    if (read_line(q, answer, sizeof(answer)) != 0 || (answer[0] != 'y' && answer[0] != 'Y')) {
        print_error("You need to agree to the Terms of Use and be at least 13 to join.");
        return -1;
    }
    return 0;
}

// The one-time code from the email: --otp, or asked for.
static int get_otp(const char *given, char *out, int size) {
    if (given) { snprintf(out, size, "%s", given); return 0; }
    if (g_json_enabled) { print_error("Add --otp <code> (the 6-digit code from the email)."); return -1; }
    return read_line("Code from the email: ", out, size);
}

// register <invite code> <email> <password> <confirm> [--otp CODE] [--agree]
// The code comes first, as on the website: it is checked (and who invited
// you shown) before anything is sent. Older form, for a server with open
// registration: register <email> <otp> <password> <confirm>, after send-otp.
static int cmd_register(int argc, char **argv) {
    const char *usage = "Usage: simple-social-cli register <invite code> <email> <password> <confirm> [--otp CODE] [--agree]\n";
    const char *otp_flag = NULL;
    int agree = 0;
    char *pos[8];
    int npos = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--otp") == 0 && i + 1 < argc) otp_flag = argv[++i];
        else if (strcmp(argv[i], "--agree") == 0) agree = 1;
        else if (npos < 8) pos[npos++] = argv[i];
    }
    if (npos < 4) { fprintf(stderr, "%s", usage); return 1; }

    config_t cfg;
    config_load(&cfg);
    api_init();
    char terms_url[256];
    api_site_url("/terms", terms_url, sizeof(terms_url));

    if (strchr(pos[0], '@') && !strchr(pos[1], '@')) {
        // The older form: the code was already sent with send-otp.
        if (ask_terms(terms_url, agree) != 0) return 1;
        if (api_register_verify_otp(pos[0], pos[1]) != 0 || api_register_finish(pos[0], pos[2], pos[3]) != 0) {
            print_error(api_get_last_error());
            return 1;
        }
    } else {
        const char *code = pos[0], *email = pos[1];
        char inviter[256] = {0}, expires[32] = {0};
        if (api_check_invite_code(code, inviter, sizeof(inviter), expires, sizeof(expires)) != 0) {
            print_error(api_get_last_error());
            return 1;
        }
        if (!g_json_enabled) fprintf(stderr, "Invited by %s.\n", inviter[0] ? inviter : "a member");
        if (ask_terms(terms_url, agree) != 0) return 1;
        // With --otp the code was already sent (send-otp <email> <invite
        // code>); sending another would replace it. Without it, send it now
        // and ask for it, which needs a person at the keyboard.
        if (!otp_flag) {
            if (g_json_enabled) { print_error("In --json mode, first run send-otp <email> <invite code>, then register again with --otp <code>."); return 1; }
            if (api_register_send_otp(email, code) != 0) { print_error(api_get_last_error()); return 1; }
            fprintf(stderr, "We emailed a 6-digit code to %s.\n", email);
        }
        char otp[32];
        if (get_otp(otp_flag, otp, sizeof(otp)) != 0) return 1;
        if (api_register_verify_otp(email, otp) != 0 || api_register_finish(email, pos[2], pos[3]) != 0) {
            print_error(api_get_last_error());
            return 1;
        }
    }
    // The terms were agreed to above: log in and record that, as the
    // website does on the new member's first sign-in.
    const char *email = strchr(pos[0], '@') && !strchr(pos[1], '@') ? pos[0] : pos[1];
    if (do_login(email, pos[2]) == 0 && me.terms_current > 0) {
        api_accept_terms(me.terms_current);
        me.terms_accepted = me.terms_current;
    }
    if (g_json_enabled) printf("{\"ok\":true}\n");
    else fprintf(stderr, "Registration complete. You're logged in as %s.\n", email);
    show_welcome_once();
    return 0;
}

// send-otp <email> [invite code]: sends the registration code.
static int cmd_send_otp(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli send-otp <email> [invite code]\n"); return 1; }
    config_t cfg;
    config_load(&cfg);
    api_init();
    if (api_register_send_otp(argv[0], argc > 1 ? argv[1] : NULL) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"ok\":true}\n"); else fprintf(stderr, "OTP sent to %s\n", argv[0]);
    return 0;
}

// forgot-password <email>: emails a password reset code.
static int cmd_forgot_password(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli forgot-password <email>\n"); return 1; }
    config_t cfg;
    config_load(&cfg);
    api_init();
    if (api_send_otp(argv[0]) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) printf("{\"ok\":true}\n");
    else fprintf(stderr, "If %s has an account, a code is on its way. Then run: simple-social-cli reset-password %s <code> <new password> <confirm>\n", argv[0], argv[0]);
    return 0;
}

static int cmd_reset_password(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: simple-social-cli reset-password <email> <otp> <password> <confirm>\n"); return 1; }
    config_t cfg;
    config_load(&cfg);
    api_init();
    // Both steps in one run: the token verify returns lives in this process.
    if (api_verify_otp(argv[0], argv[1]) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (api_reset_password(argv[0], argv[2], argv[3]) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"ok\":true}\n"); else fprintf(stderr, "Password reset. You can now login.\n");
    return 0;
}

static int cmd_accept_terms(int argc, char **argv) {
    (void)argc; (void)argv;
    if (require_auth() != 0) return 1;
    if (!me_loaded || me.terms_current <= 0) { print_error("Could not read the current terms version."); return 1; }
    if (me.terms_accepted >= me.terms_current) {
        if (g_json_enabled) printf("{\"ok\":true,\"version\":%d}\n", me.terms_current); else fprintf(stderr, "You have already accepted the current Terms of Use.\n");
        return 0;
    }
    if (!g_json_enabled) {
        char q[512], answer[16];
        snprintf(q, sizeof(q), TERMS_QUESTION "? [y/N] ", me.terms_url);
        if (read_line(q, answer, sizeof(answer)) != 0 || (answer[0] != 'y' && answer[0] != 'Y')) {
            print_error("Not accepted.");
            return 1;
        }
    }
    if (api_accept_terms(me.terms_current) != 0) { print_error(api_get_last_error()); return 1; }
    me.terms_accepted = me.terms_current;
    if (g_json_enabled) printf("{\"ok\":true,\"version\":%d}\n", me.terms_current); else fprintf(stderr, "Thank you. The Terms of Use are accepted.\n");
    return 0;
}

static int cmd_feed(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int limit = 25, offset = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = atoi(argv[++i]);
    }
    api_posts_result_t result;
    if (api_fetch_followed_posts(offset, limit, &result) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_posts(&result);
    return 0;
}

static int cmd_post(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli post <post_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    api_post_t post;
    if (api_get_post_by_id(argv[0], &post) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_post(&post);

    if (g_json_enabled || post.frozen) return 0;
    static api_comments_result_t comments;   // ~1.4 MB: not on the stack
    if (api_get_post_comments_threaded(argv[0], 0, 25, &comments) == 0 && comments.count > 0) {
        printf("\n%sComments:%s\n", C_BOLD, C_RESET);
        print_comments(&comments);
    }
    return 0;
}

static int cmd_create(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli create <text> [--media <file>]\n"); return 1; }
    if (require_auth() != 0) return 1;
    char *media_file = NULL;
    char *text_args[256];
    int text_count = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--media") == 0 && i + 1 < argc) { media_file = argv[++i]; }
        else if (text_count < 256) text_args[text_count++] = argv[i];
    }
    if (text_count == 0) { fprintf(stderr, "Usage: simple-social-cli create <text> [--media <file>]\n"); return 1; }
    char full_text[5000] = {0};
    size_t pos = 0;
    for (int i = 0; i < text_count; i++) {
        const char *src = text_args[i];
        size_t slen = strlen(src);
        size_t need = slen + (i > 0 ? 1 : 0);
        if (pos + need >= sizeof(full_text)) { need = sizeof(full_text) - 1 - pos; if (need == 0) break; if (i > 0 && pos + 1 < sizeof(full_text)) full_text[pos++] = ' '; size_t copy = need - (i > 0 ? 1 : 0); if (copy > slen) copy = slen; memcpy(full_text + pos, src, copy); pos += copy; full_text[pos] = '\0'; break; }
        if (i > 0) full_text[pos++] = ' ';
        memcpy(full_text + pos, src, slen);
        pos += slen;
        full_text[pos] = '\0';
    }
    char media_url[512] = {0};
    int media_id = 0;
    const char *media_url_ptr = NULL;
    if (media_file) {
        // Checked here rather than left to the server: a bad path used to
        // round-trip a full upload request just to come back as media.php's
        // generic "No file was selected", which reads like a server problem
        // rather than a typo in what was typed.
        if (access(media_file, R_OK) != 0) {
            char msg[600];
            snprintf(msg, sizeof(msg), "Media file not found or not readable: %s", media_file);
            print_error(msg);
            return 1;
        }
        if (check_media_before_upload(media_file) != 0) return 1;
        if (api_upload_media_with_id(media_file, media_url, sizeof(media_url), &media_id) != 0) {
            print_error(api_get_last_error());
            return 1;
        }
        media_url_ptr = media_url;
    }
    char post_id[64] = {0};
    if (api_create_post(full_text, media_url_ptr, post_id, sizeof(post_id)) != 0) {
        if (media_id) api_delete_media(media_id);
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) {
        if (media_url_ptr) printf("{\"postId\":\"%s\",\"mediaUrl\":\"%s\"}\n", post_id, media_url);
        else printf("{\"postId\":\"%s\"}\n", post_id);
    } else fprintf(stderr, "Created post %s\n", post_id);
    return 0;
}

static int cmd_delete_post(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli delete <post_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_delete_post(argv[0]) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"deleted\":true,\"postId\":\"%s\"}\n", argv[0]); else fprintf(stderr, "Deleted post %s\n", argv[0]);
    return 0;
}

static int cmd_posts(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int limit = 25, offset = 0;
    int user_id = 0;
    int has_user = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = atoi(argv[++i]);
        else if (!has_user && argv[i][0] != '-') { user_id = atoi(argv[i]); has_user = 1; }
    }
    api_posts_result_t result;
    int rc;
    if (has_user) rc = api_get_user_posts(user_id, offset, limit, &result);
    else rc = api_get_my_posts(offset, limit, &result);
    if (rc != 0) { print_error(api_get_last_error()); return 1; }
    print_posts(&result);
    return 0;
}

static int cmd_likes(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli likes <post_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    char out[16384] = {0};
    if (api_get_post_likes(argv[0], out, sizeof(out)) != 0) { print_error(api_get_last_error()); return 1; }

    // Parsed into the same struct for both modes - JSON used to print the
    // raw server response verbatim, which leaked the "valid":true envelope
    // that every other command's --json output strips.
    api_users_result_t res;
    res.count = 0;
    const char *arr_start, *arr_end;
    if (json_get_array(out, "likes", &arr_start, &arr_end) == 0 || json_get_array(out, "users", &arr_start, &arr_end) == 0) {
        int len = json_array_len(arr_start, arr_end);
        for (int i = 0; i < len && i < 256; i++) {
            const char *is, *ie;
            if (json_array_get_item(arr_start, arr_end, i, &is, &ie) != 0) break;
            int l = ie - is;
            char item[4096]; if (l >= (int)sizeof(item)) l = sizeof(item)-1; memcpy(item, is, l); item[l]='\0';
            api_user_t *u = &res.users[res.count];
            if (json_get_int(item, "userId", &u->id) != 0) json_get_int(item, "id", &u->id);
            json_get_string(item, "email", u->email, sizeof(u->email));
            if (u->email[0] == '\0') json_get_string(item, "userEmail", u->email, sizeof(u->email));
            json_get_string(item, "created_at", u->created_at, sizeof(u->created_at));
            res.count++;
        }
    }
    print_likes(&res);
    return 0;
}

static int cmd_like(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli like <post_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_like_post(argv[0]) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"liked\":true,\"postId\":\"%s\"}\n", argv[0]); else fprintf(stderr, "Liked post %s\n", argv[0]);
    return 0;
}

static int cmd_unlike(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli unlike <post_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_unlike_post(argv[0]) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"liked\":false,\"postId\":\"%s\"}\n", argv[0]); else fprintf(stderr, "Unliked post %s\n", argv[0]);
    return 0;
}

static int cmd_comments(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli comments <post_id> [--offset N] [--flat]\n"); return 1; }
    if (require_auth() != 0) return 1;
    const char *post_id = argv[0];
    int offset = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = atoi(argv[++i]);
    }
    // Threads: each comment followed by its replies. --flat is the old list.
    static api_comments_result_t result;
    int flat = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--flat") == 0) flat = 1;
    int rc = flat ? api_get_post_comments(post_id, offset, 25, &result)
                  : api_get_post_comments_threaded(post_id, offset, 25, &result);
    if (rc != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_comments(&result);
    return 0;
}

static int cmd_comment(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "Usage: simple-social-cli comment <post_id> <text>\n"); return 1; }
    if (require_auth() != 0) return 1;
    const char *post_id = argv[0];
    char text[5000] = {0};
    size_t pos = 0;
    for (int i = 1; i < argc; i++) {
        const char *src = argv[i];
        size_t slen = strlen(src);
        size_t need = slen + (i > 1 ? 1 : 0);
        if (pos + need >= sizeof(text)) { need = sizeof(text) - 1 - pos; if (need == 0) break; if (i > 1 && pos + 1 < sizeof(text)) text[pos++] = ' '; size_t copy = need - (i > 1 ? 1 : 0); if (copy > slen) copy = slen; memcpy(text + pos, src, copy); pos += copy; text[pos] = '\0'; break; }
        if (i > 1) text[pos++] = ' ';
        memcpy(text + pos, src, slen);
        pos += slen;
        text[pos] = '\0';
    }
    int comment_id = 0;
    if (api_create_comment(post_id, text, &comment_id) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"commentId\":%d,\"postId\":\"%s\"}\n", comment_id, post_id); else fprintf(stderr, "Comment %d added to %s\n", comment_id, post_id);
    return 0;
}

static int cmd_delete_comment(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli delete-comment <comment_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_delete_comment(atoi(argv[0])) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"deleted\":true,\"commentId\":%s}\n", argv[0]); else fprintf(stderr, "Deleted comment %s\n", argv[0]);
    return 0;
}

static int cmd_users(int argc, char **argv) {
    (void)argc; (void)argv;
    if (require_auth() != 0) return 1;
    api_users_result_t result;
    if (api_get_users(&result) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_users(&result);
    return 0;
}

static int cmd_profile(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int user_id = state.user.user_id;
    if (argc >= 1) user_id = atoi(argv[0]);

    char email[256] = {0};
    char created[32] = {0};
    if (api_get_user_info(user_id, email, sizeof(email), created, sizeof(created)) != 0) {
        print_error(api_get_last_error());
        return 1;
    }

    api_users_result_t followers, following;
    api_get_my_followers(user_id, &followers);
    api_get_my_follows(user_id, &following);

    char role[16] = "user";
    api_get_user_role(user_id, role, sizeof(role));
    print_profile(user_id, email, created, followers.count, following.count, role);

    // Human mode only: in --json mode this extra line used to follow the
    // JSON object and made the output unparseable.
    if (user_id != state.user.user_id && !g_json_enabled) {
        int is_following = 0;
        api_is_following(user_id, &is_following);
        printf("%sFollowing:%s  %s\n", C_BOLD, C_RESET, is_following ? "Yes" : "No");
    }
    return 0;
}

static int cmd_follow(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli follow <user_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_follow_user(atoi(argv[0])) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"following\":true,\"userId\":%s}\n", argv[0]); else fprintf(stderr, "Followed user %s\n", argv[0]);
    return 0;
}

static int cmd_unfollow(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli unfollow <user_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_unfollow_user(atoi(argv[0])) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"following\":false,\"userId\":%s}\n", argv[0]); else fprintf(stderr, "Unfollowed user %s\n", argv[0]);
    return 0;
}

static int cmd_followers(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int user_id = state.user.user_id;
    if (argc >= 1) user_id = atoi(argv[0]);
    api_users_result_t result;
    if (api_get_my_followers(user_id, &result) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_users(&result);
    return 0;
}

static int cmd_following(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int user_id = state.user.user_id;
    if (argc >= 1) user_id = atoi(argv[0]);
    api_users_result_t result;
    if (api_get_my_follows(user_id, &result) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_users(&result);
    return 0;
}

static int cmd_notifications(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int offset = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = atoi(argv[++i]);
    }
    api_notifications_result_t result;
    if (api_get_notifications(offset, &result) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    print_notifications(&result);
    return 0;
}

static int cmd_notify_count(int argc, char **argv) {
    (void)argc; (void)argv;
    if (require_auth() != 0) return 1;
    int count = 0;
    if (api_get_unseen_notification_count(&count) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) print_count_json(count, "count"); else print_count(count);
    return 0;
}

static int cmd_mark_seen(int argc, char **argv) {
    (void)argc; (void)argv;
    if (require_auth() != 0) return 1;
    if (api_mark_notifications_seen() != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    if (g_json_enabled) printf("{\"ok\":true}\n"); else fprintf(stderr, "Notifications marked as seen.\n");
    return 0;
}


static int cmd_delete_account(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli delete-account <password>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_delete_account(argv[0]) != 0) {
        print_error(api_get_last_error());
        return 1;
    }
    ss_state_clear(&state);
    if (g_json_enabled) printf("{\"deleted\":true}\n"); else fprintf(stderr, "Account deleted.\n");
    return 0;
}

static int check_media_before_upload(const char *path) {
    char msg[256];
    if (api_check_upload(path, msg, sizeof(msg)) != 0) { print_error(msg); return -1; }
    return 0;
}

// Joins argv[from..] with spaces.
static void join_args(int argc, char **argv, int from, char *out, size_t size) {
    size_t pos = 0;
    out[0] = '\0';
    for (int i = from; i < argc && pos < size - 1; i++) {
        int n = snprintf(out + pos, size - pos, "%s%s", i > from ? " " : "", argv[i]);
        if (n < 0) break;
        pos += (size_t)n;
        if (pos >= size) { pos = size - 1; break; }
    }
}

// reply <post_id> <comment_id> <text>: a reply goes under a top-level comment.
static int cmd_reply(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: simple-social-cli reply <post_id> <comment_id> <text>\n"); return 1; }
    if (require_auth() != 0) return 1;
    char text[5000];
    join_args(argc, argv, 2, text, sizeof(text));
    int id = 0;
    if (api_create_reply(argv[0], atoi(argv[1]), text, &id) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) printf("{\"commentId\":%d,\"postId\":\"%s\",\"parentId\":%d}\n", id, argv[0], atoi(argv[1]));
    else fprintf(stderr, "Reply %d added under comment %s\n", id, argv[1]);
    return 0;
}

// report post|comment|user <id> <reason> [details]
static int cmd_report(int argc, char **argv) {
    const char *usage = "Usage: simple-social-cli report post|comment|user <id> <reason> [details]\n"
                        "Reasons: spam, harassment, hate, sexual, violence, self_harm, illegal, other\n";
    if (argc < 3) { fprintf(stderr, "%s", usage); return 1; }
    if (require_auth() != 0) return 1;
    if (strcmp(argv[0], "post") && strcmp(argv[0], "comment") && strcmp(argv[0], "user")) { fprintf(stderr, "%s", usage); return 1; }
    int ok = 0;
    for (int i = 0; api_report_reasons[i]; i++) if (!strcmp(argv[2], api_report_reasons[i])) ok = 1;
    if (!ok) { fprintf(stderr, "%s", usage); return 1; }
    char details[600];
    join_args(argc, argv, 3, details, sizeof(details));
    if (api_report(argv[0], argv[1], argv[2], details) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) printf("{\"reported\":true,\"targetType\":\"%s\",\"targetId\":\"%s\"}\n", argv[0], argv[1]);
    else fprintf(stderr, "Thank you. The report was sent to the moderators.%s\n",
                 strcmp(argv[0], "user") ? " It is folded away for you now." : "");
    return 0;
}

static int cmd_block(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli block <user_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_block_user(atoi(argv[0])) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) printf("{\"blocked\":true,\"userId\":%d}\n", atoi(argv[0]));
    else fprintf(stderr, "Blocked user %s. Neither of you will see the other's posts, comments or profile.\n", argv[0]);
    return 0;
}

static int cmd_unblock(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr, "Usage: simple-social-cli unblock <user_id>\n"); return 1; }
    if (require_auth() != 0) return 1;
    if (api_unblock_user(atoi(argv[0])) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) printf("{\"blocked\":false,\"userId\":%d}\n", atoi(argv[0]));
    else fprintf(stderr, "Unblocked user %s.\n", argv[0]);
    return 0;
}

static int cmd_blocked(int argc, char **argv) {
    (void)argc; (void)argv;
    if (require_auth() != 0) return 1;
    static api_users_result_t res;
    if (api_get_blocked_users(&res) != 0) { print_error(api_get_last_error()); return 1; }
    if (!g_json_enabled && res.count == 0) { fprintf(stderr, "You haven't blocked anyone.\n"); return 0; }
    print_users(&res);
    return 0;
}

// invite                 your invite state and live code(s)
// invite --generate      make a code (members: one, ever)
// invite --cancel <code> cancel a live code
static int cmd_invite(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    int generate = 0;
    const char *cancel = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--generate")) generate = 1;
        else if (!strcmp(argv[i], "--cancel") && i + 1 < argc) cancel = argv[++i];
    }
    if (cancel) {
        if (api_cancel_invite_code(cancel) != 0) { print_error(api_get_last_error()); return 1; }
        if (g_json_enabled) printf("{\"cancelled\":true}\n"); else fprintf(stderr, "The code was cancelled.\n");
        return 0;
    }
    if (generate) {
        api_invites_t inv;
        if (!g_json_enabled && api_get_my_invites(&inv) == 0 && !inv.unlimited) {
            char answer[16];
            if (read_line("You can only invite one person, so choose wisely! Make a code? [y/N] ", answer, sizeof(answer)) != 0
                || (answer[0] != 'y' && answer[0] != 'Y')) return 1;
        }
        char code[32] = {0}, expires[32] = {0};
        if (api_generate_invite_code(code, sizeof(code), expires, sizeof(expires)) != 0) { print_error(api_get_last_error()); return 1; }
        if (g_json_enabled) printf("{\"code\":\"%s\",\"expiresAt\":\"%s\"}\n", code, expires);
        else printf("Your invite code: %s%s%s\nValid until %s. It works once.\n", C_BOLD, code, C_RESET, when(expires, NULL));
        return 0;
    }
    api_invites_t inv;
    if (api_get_my_invites(&inv) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) {
        printf("{\"unlimited\":%s,\"canGenerate\":%s,\"used\":%s,\"codes\":[", inv.unlimited ? "true" : "false",
               inv.can_generate ? "true" : "false", inv.used ? "true" : "false");
        for (int i = 0; i < inv.count; i++) printf("%s{\"code\":\"%s\",\"expiresAt\":\"%s\"}", i ? "," : "", inv.codes[i].code, inv.codes[i].expires_at);
        printf("]}\n");
        return 0;
    }
    if (inv.used && !inv.unlimited) printf("You've already used your invite.\n");
    for (int i = 0; i < inv.count; i++)
        printf("Live code: %s%s%s  (valid until %s)\n", C_BOLD, inv.codes[i].code, C_RESET, when(inv.codes[i].expires_at, NULL));
    if (inv.can_generate)
        printf("%s\nRun: simple-social-cli invite --generate\n",
               inv.unlimited ? "As the owner you can make as many codes as you like." : "You can only invite one person, so choose wisely!");
    return 0;
}

// devices                   every signed-in device
// devices --revoke <id>     sign one out
// devices --revoke-others   sign out everything but this one
static int cmd_devices(int argc, char **argv) {
    if (require_auth() != 0) return 1;
    const char *revoke = NULL;
    int others = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--revoke") && i + 1 < argc) revoke = argv[++i];
        else if (!strcmp(argv[i], "--revoke-others")) others = 1;
    }
    if (revoke) {
        int was_current = 0;
        if (api_revoke_session(revoke, &was_current) != 0) { print_error(api_get_last_error()); return 1; }
        if (was_current) { ss_state_clear(&state); ss_state_delete_files(); }
        if (g_json_enabled) printf("{\"revoked\":true,\"wasCurrent\":%s}\n", was_current ? "true" : "false");
        else fprintf(stderr, was_current ? "Signed out (that was this device).\n" : "That device is signed out.\n");
        return 0;
    }
    if (others) {
        int n = 0;
        if (api_revoke_all_other_sessions(&n) != 0) { print_error(api_get_last_error()); return 1; }
        if (g_json_enabled) printf("{\"revoked\":%d}\n", n); else fprintf(stderr, "Signed out %d other device(s).\n", n);
        return 0;
    }
    api_sessions_t ss;
    if (api_get_sessions(&ss) != 0) { print_error(api_get_last_error()); return 1; }
    if (g_json_enabled) {
        printf("{\"sessions\":[");
        for (int i = 0; i < ss.count; i++) {
            api_session_t *d = &ss.sessions[i];
            printf("%s{\"id\":\"%s\",\"deviceName\":\"%s\",\"createdAt\":\"%s\",\"createdTz\":%s%s%s,\"lastUsedAt\":\"%s\",\"isCurrent\":%s}",
                   i ? "," : "", d->id, d->device_name, d->created_at, d->created_tz[0] ? "\"" : "", d->created_tz[0] ? d->created_tz : "null",
                   d->created_tz[0] ? "\"" : "", d->last_used_at, d->is_current ? "true" : "false");
        }
        printf("],\"maxSessions\":%d}\n", ss.max_sessions);
        return 0;
    }
    for (int i = 0; i < ss.count; i++) {
        api_session_t *d = &ss.sessions[i];
        printf("%s%-34s%s %s%s%s\n    signed in %s, last active %s\n", C_BOLD, d->device_name, C_RESET,
               d->is_current ? C_GREEN : "", d->is_current ? "(this device)" : "", C_RESET,
               when(d->created_at, d->created_tz), when(d->last_used_at, NULL));
        printf("    %sid %s%s\n", C_DIM, d->id, C_RESET);
    }
    printf("%d of %d devices. Sign one out: simple-social-cli devices --revoke <id>\n", ss.count, ss.max_sessions);
    return 0;
}

static void print_help(void) {
    fprintf(stderr, "Simple Social CLI\n\n");
    fprintf(stderr, "Usage: simple-social-cli <command> [args...]\n\n");
    fprintf(stderr, "AUTH\n");
    fprintf(stderr, "  login <email> <password>           Login\n");
    fprintf(stderr, "  logout                             Logout\n");
    fprintf(stderr, "  whoami                             Show current user\n");
    fprintf(stderr, "  register <invite code> <email> <pw> <cfm> [--otp CODE] [--agree]\n");
    fprintf(stderr, "                                     Join with an invite code (asks for the\n");
    fprintf(stderr, "                                     emailed code and the terms; --json needs\n");
    fprintf(stderr, "                                     send-otp first, then --otp and --agree)\n");
    fprintf(stderr, "  send-otp <email> [invite code]     Send the registration code\n");
    fprintf(stderr, "  forgot-password <email>            Email a password reset code\n");
    fprintf(stderr, "  reset-password <email> <otp> <pw> <cfm>  Reset password\n");
    fprintf(stderr, "  accept-terms                       Accept the current Terms of Use\n\n");
    fprintf(stderr, "POSTS\n");
    fprintf(stderr, "  feed [--limit N] [--offset N]      Show feed\n");
    fprintf(stderr, "  posts [user_id] [--limit N] [--offset N]  List user posts\n");
    fprintf(stderr, "  post <post_id>                     Show post + comments\n");
    fprintf(stderr, "  create <text> [--media <file>]    Create post\n");
    fprintf(stderr, "  delete <post_id>                   Delete post\n");
    fprintf(stderr, "  like <post_id>                     Like post\n");
    fprintf(stderr, "  unlike <post_id>                   Unlike post\n");
    fprintf(stderr, "  likes <post_id>                    List likes for post\n\n");
    fprintf(stderr, "COMMENTS\n");
    fprintf(stderr, "  comments <post_id> [--offset N] [--flat]  Show comments and replies\n");
    fprintf(stderr, "  comment <post_id> <text>           Add comment\n");
    fprintf(stderr, "  reply <post_id> <comment_id> <text>  Reply to a comment\n");
    fprintf(stderr, "  delete-comment <comment_id>        Delete comment\n\n");
    fprintf(stderr, "USERS\n");
    fprintf(stderr, "  users                              List all users\n");
    fprintf(stderr, "  profile [user_id]                  Show profile\n");
    fprintf(stderr, "  follow <user_id>                   Follow user\n");
    fprintf(stderr, "  unfollow <user_id>                 Unfollow user\n");
    fprintf(stderr, "  followers [user_id]                List followers\n");
    fprintf(stderr, "  following [user_id]                List following\n\n");
    fprintf(stderr, "NOTIFICATIONS\n");
    fprintf(stderr, "  notifications [--offset N]         Show notifications\n");
    fprintf(stderr, "  notify-count                       Unseen notification count\n");
    fprintf(stderr, "  mark-seen                          Mark notifications seen\n\n");

    fprintf(stderr, "SAFETY\n");
    fprintf(stderr, "  report post|comment|user <id> <reason> [details]\n");
    fprintf(stderr, "                                     Reasons: spam, harassment, hate, sexual,\n");
    fprintf(stderr, "                                     violence, self_harm, illegal, other\n");
    fprintf(stderr, "  block <user_id> / unblock <user_id>  Block or unblock someone\n");
    fprintf(stderr, "  blocked                            People you blocked\n\n");
    fprintf(stderr, "ACCOUNT\n");
    fprintf(stderr, "  invite [--generate | --cancel <code>]  Your invite code\n");
    fprintf(stderr, "  devices [--revoke <id> | --revoke-others]  Signed-in devices\n");
    fprintf(stderr, "  delete-account <password>          Delete your account\n\n");
    fprintf(stderr, "Times are shown in UTC, with the creator's offset from GMT when known.\n\n");
    fprintf(stderr, "FLAGS\n");
    fprintf(stderr, "  --color          Colored output (auto if TTY)\n");
    fprintf(stderr, "  --json           Raw JSON output\n");
    fprintf(stderr, "  --show-reported  Show what you reported instead of folding it\n");
    fprintf(stderr, "  --help           Show this help\n");
}

typedef struct {
    const char *name;
    int min_args;
    int (*func)(int argc, char **argv);
} command_t;

static const command_t commands[] = {
    {"login",           2, cmd_login},
    {"logout",          0, cmd_logout},
    {"whoami",          0, cmd_whoami},
    {"register",        4, cmd_register},
    {"send-otp",        1, cmd_send_otp},
    {"reset-password",  4, cmd_reset_password},
    {"forgot-password", 1, cmd_forgot_password},
    {"accept-terms",    0, cmd_accept_terms},
    {"feed",            0, cmd_feed},
    {"posts",           0, cmd_posts},
    {"post",            1, cmd_post},
    {"create",          1, cmd_create},
    {"delete",          1, cmd_delete_post},
    {"like",            1, cmd_like},
    {"unlike",          1, cmd_unlike},
    {"likes",           1, cmd_likes},
    {"comments",        1, cmd_comments},
    {"comment",         2, cmd_comment},
    {"delete-comment",  1, cmd_delete_comment},
    {"users",           0, cmd_users},
    {"profile",         0, cmd_profile},
    {"follow",          1, cmd_follow},
    {"unfollow",        1, cmd_unfollow},
    {"followers",       0, cmd_followers},
    {"following",       0, cmd_following},
    {"notifications",   0, cmd_notifications},
    {"notify-count",    0, cmd_notify_count},
    {"mark-seen",       0, cmd_mark_seen},
    {"delete-account",  1, cmd_delete_account},
    {"reply",           3, cmd_reply},
    {"report",          3, cmd_report},
    {"block",           1, cmd_block},
    {"unblock",         1, cmd_unblock},
    {"blocked",         0, cmd_blocked},
    {"invite",          0, cmd_invite},
    {"devices",         0, cmd_devices},
    {NULL, 0, NULL}
};

int main(int argc, char **argv) {
    ss_state_init(&state);
    init_client();

    if (argc < 2) {
        print_help();
        return 1;
    }

    int next_arg = 1;
    while (next_arg < argc && argv[next_arg][0] == '-') {
        if (strcmp(argv[next_arg], "--help") == 0) { print_help(); return 0; }
        else if (strcmp(argv[next_arg], "--color") == 0) g_color_enabled = 1;
        else if (strcmp(argv[next_arg], "--json") == 0) g_json_enabled = 1;
        else if (strcmp(argv[next_arg], "--show-reported") == 0) g_show_reported = 1;
        else break;
        next_arg++;
    }
    if (next_arg >= argc) {
        print_help();
        return 1;
    }
    // --show-reported works anywhere on the line.
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--show-reported") == 0) g_show_reported = 1;
    if (g_json_enabled) g_color_enabled = 0;
    else if (g_color_enabled == 0 && isatty(STDERR_FILENO)) g_color_enabled = 1;
    output_init();

    const char *cmd_name = argv[next_arg];
    next_arg++;

    if (strcmp(cmd_name, "login") != 0 && strcmp(cmd_name, "register") != 0 &&
        strcmp(cmd_name, "send-otp") != 0 && strcmp(cmd_name, "reset-password") != 0 &&
        strcmp(cmd_name, "forgot-password") != 0) {
        auto_login();
        show_welcome_once();
        if (strcmp(cmd_name, "accept-terms") != 0) terms_notice();
    }

    for (int i = 0; commands[i].name; i++) {
        if (strcmp(cmd_name, commands[i].name) == 0) {
            int cmd_argc = argc - next_arg;
            char **cmd_argv = argv + next_arg;
            if (cmd_argc < commands[i].min_args) {
                fprintf(stderr, "Error: %s requires at least %d argument(s)\n",
                        cmd_name, commands[i].min_args);
                return 1;
            }
            return commands[i].func(cmd_argc, cmd_argv);
        }
    }

    fprintf(stderr, "Unknown command: %s\n", cmd_name);
    fprintf(stderr, "Run 'simple-social-cli --help' for usage.\n");
    return 1;
}
