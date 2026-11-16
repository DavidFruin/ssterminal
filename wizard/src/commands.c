#include "commands.h"
#include "output.h"
#include "input.h"
#include "ss_api.h"
#include "ss_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int answer_int(const wizard_answer_t *a, int def) {
    return a->provided ? atoi(a->text) : def;
}

/* A yes/no question; only "y" or "yes" is yes. */
static int ask_yes(const char *question) {
    char buf[32];
    printf("%s [y/N] ", question);
    fflush(stdout);
    if (input_read_line(buf, sizeof(buf)) < 0) return 0;
    return buf[0] == 'y' || buf[0] == 'Y';
}

static int ask_line(const char *prompt, char *out, size_t size) {
    printf("%s", prompt);
    fflush(stdout);
    return input_read_line(out, size) < 0 ? -1 : 0;
}

static const char *when(const char *utc, const char *tz) {
    static char buf[4][64];
    static int k = 0;
    k = (k + 1) % 4;
    api_format_time(utc, tz, buf[k], sizeof(buf[k]));
    return buf[k];
}

#define TERMS_QUESTION "I agree to the Terms of Use (%s) and I'm at least 13 years old."

static void forget_session(ss_state_t *state) {
    api_logout();
    ss_state_clear(state);
    ss_state_delete_files();
}

int wizard_session_start(ss_state_t *state) {
    api_me_t me;
    if (api_get_me(&me) != 0) return -1;
    ss_state_set_user(state, me.id, me.email, me.created_at);
    api_set_user_id(me.id);

    if (me.has_welcome) {
        if (me.welcome_inviter[0] && me.welcome_inviter_is_owner)
            printf("%sWelcome to Simple Social!%s You were invited by %s, who runs Simple Social.\n", C_BOLD, C_RESET, me.welcome_inviter);
        else if (me.welcome_inviter[0])
            printf("%sWelcome to Simple Social!%s %s used their only invite on you, so use yours just as wisely.\n", C_BOLD, C_RESET, me.welcome_inviter);
        else
            printf("%sWelcome to Simple Social!%s\n", C_BOLD, C_RESET);
        api_dismiss_welcome();
    }
    if (me.terms_current > 0 && me.terms_accepted < me.terms_current) {
        printf("%sThe Terms of Use have changed.%s Read them at %s\n", C_YELLOW, C_RESET, me.terms_url);
        char q[512];
        snprintf(q, sizeof(q), TERMS_QUESTION " Accept?", me.terms_url);
        if (!ask_yes(q) || api_accept_terms(me.terms_current) != 0) {
            print_error("The Terms of Use weren't accepted, so you've been logged out.");
            forget_session(state);
            return -1;
        }
        print_success("Thank you. The Terms of Use are accepted.");
    }
    return 0;
}

/* ---------------- Auth ---------------- */

static void cmd_login(ss_state_t *state, const wizard_answer_t *answers) {
    const char *email = answers[0].text;
    const char *password = answers[1].text;

    char jwt[1024] = {0};
    int user_id = 0;
    if (api_login(email, password, jwt, sizeof(jwt), &user_id) != 0) {
        print_error(api_get_last_error());
        return;
    }
    ss_state_set_jwt(state, jwt);
    api_set_jwt(jwt);
    api_set_user_id(user_id);
    ss_state_save_jwt(state);
    // The long-lived half of the session - without it this login would
    // stop working as soon as the access token aged out.
    ss_state_set_refresh(state, api_get_refresh_token());
    ss_state_save_refresh(state);

    char msg[512];
    snprintf(msg, sizeof(msg), "Logged in as %.300s (ID: %d)", email, user_id);
    print_success(msg);
    if (wizard_session_start(state) == 0) ss_state_save_user(state);
}

static void cmd_logout(ss_state_t *state, const wizard_answer_t *answers) {
    (void)answers;
    // Revoke on the server first; removing the local files alone would
    // leave the session live and still listed as a signed-in device.
    api_logout();
    ss_state_clear(state);
    ss_state_delete_files();
    print_success("Logged out.");
}

static void cmd_whoami(ss_state_t *state, const wizard_answer_t *answers) {
    (void)answers;
    print_user_info(state->user.user_id, state->user.email, state->user.created_at);
    char role[16] = "user";
    api_get_user_role(state->user.user_id, role, sizeof(role));
    printf("%sRole:%s   %s\n", C_BOLD, C_RESET, role_label(role));
    api_media_limits_t lim;
    if (api_get_media_limits(&lim) == 0 && lim.storage_limit > 0)
        printf("%sMedia:%s  %.1f MB of %.0f MB used\n", C_BOLD, C_RESET, lim.storage_used / 1048576.0, lim.storage_limit / 1048576.0);
}

/* Code-first, as on the website: the invite code is checked (and who made
 * it shown) before the email is even asked for. */
static void cmd_register(ss_state_t *state, const wizard_answer_t *answers) {
    const char *code = answers[0].text;
    char inviter[256] = {0}, expires[32] = {0};
    if (api_check_invite_code(code, inviter, sizeof(inviter), expires, sizeof(expires)) != 0) {
        print_error(api_get_last_error());
        return;
    }
    printf("Invited by %s.\n", inviter[0] ? inviter : "a member");

    char terms_url[256], q[512];
    api_site_url("/terms", terms_url, sizeof(terms_url));
    snprintf(q, sizeof(q), TERMS_QUESTION " Agree?", terms_url);
    if (!ask_yes(q)) {
        print_error("You need to agree to the Terms of Use and be at least 13 to join.");
        return;
    }

    char email[256], otp[32];
    wizard_answer_t pw[2];
    static const field_spec_t pw_fields[2] = {{"Password", FT_PASSWORD, 0, NULL}, {"Confirm Password", FT_PASSWORD, 0, NULL}};
    if (ask_line("Email: ", email, sizeof(email)) != 0 || !email[0]) { printf("Cancelled.\n"); return; }
    if (api_register_send_otp(email, code) != 0) { print_error(api_get_last_error()); return; }
    printf("We emailed a 6-digit code to %s.\n", email);
    if (ask_line("Code from the email: ", otp, sizeof(otp)) != 0) { printf("Cancelled.\n"); return; }
    if (api_register_verify_otp(email, otp) != 0) { print_error(api_get_last_error()); return; }
    if (wizard_collect(pw_fields, 2, pw) != 0) { printf("Cancelled.\n"); return; }
    if (api_register_finish(email, pw[0].text, pw[1].text) != 0) { print_error(api_get_last_error()); return; }

    /* Log the new member straight in and record the terms they just agreed to. */
    char jwt[1024] = {0};
    int user_id = 0;
    if (api_login(email, pw[0].text, jwt, sizeof(jwt), &user_id) != 0) {
        print_success("Registration complete. You can now login.");
        return;
    }
    ss_state_set_jwt(state, jwt);
    api_set_jwt(jwt);
    api_set_user_id(user_id);
    ss_state_save_jwt(state);
    ss_state_set_refresh(state, api_get_refresh_token());
    ss_state_save_refresh(state);
    api_me_t me;
    if (api_get_me(&me) == 0 && me.terms_current > 0) api_accept_terms(me.terms_current);
    print_success("Registration complete. You're logged in.");
    if (wizard_session_start(state) == 0) ss_state_save_user(state);
}

static void cmd_forgot_password(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    if (api_send_otp(answers[0].text) != 0) { print_error(api_get_last_error()); return; }
    print_success("If that email has an account, a code is on its way. Then use 'reset-password'.");
}

static void cmd_accept_terms(ss_state_t *state, const wizard_answer_t *answers) {
    (void)answers;
    api_me_t me;
    if (api_get_me(&me) != 0) { print_error(api_get_last_error()); return; }
    if (me.terms_accepted >= me.terms_current) { print_success("You have already accepted the current Terms of Use."); return; }
    wizard_session_start(state);
}

static void cmd_send_otp(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *email = answers[0].text;
    if (api_register_send_otp(email, answers[1].provided ? answers[1].text : NULL) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "OTP sent to %s", email);
    print_success(msg);
}

static void cmd_reset_password(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *email = answers[0].text;
    const char *otp = answers[1].text;
    const char *password = answers[2].text;
    const char *confirm = answers[3].text;

    if (api_verify_otp(email, otp) != 0) {
        print_error(api_get_last_error());
        return;
    }
    if (api_reset_password(email, password, confirm) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_success("Password reset. You can now login.");
}

/* ---------------- Posts ---------------- */

static void cmd_feed(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    int limit = answer_int(&answers[0], 25);
    int offset = answer_int(&answers[1], 0);
    api_posts_result_t result;
    if (api_fetch_followed_posts(offset, limit, &result) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_posts(&result);
}

static void cmd_posts(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    int limit = answer_int(&answers[1], 25);
    int offset = answer_int(&answers[2], 0);
    api_posts_result_t result;
    int rc;
    if (answers[0].provided) rc = api_get_user_posts(atoi(answers[0].text), offset, limit, &result);
    else rc = api_get_my_posts(offset, limit, &result);
    if (rc != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_posts(&result);
}

static void cmd_post(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    api_post_t post;
    if (api_get_post_by_id(post_id, &post) != 0) {
        print_error(api_get_last_error());
        return;
    }
    /* Something you reported stays folded unless you ask to see it. */
    g_show_reported = post.reported_by_me && ask_yes("You reported this post. Show it anyway?");
    print_post(&post);
    if (!post.frozen) {
        static api_comments_result_t comments;
        if (api_get_post_comments_threaded(post_id, 0, 25, &comments) == 0 && comments.count > 0) {
            printf("\n%sComments:%s\n", C_BOLD, C_RESET);
            print_comments(&comments);
        }
    }
    g_show_reported = 0;
}

static void cmd_create(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *text = answers[0].text;
    char media_url[512] = {0};
    int media_id = 0;
    const char *media_url_ptr = NULL;

    if (answers[1].provided) {
        // Checked here rather than left to the server: a bad path used to
        // round-trip a full upload request just to come back as media.php's
        // generic "No file was selected", which reads like a server problem
        // rather than a typo in what was typed.
        if (access(answers[1].text, R_OK) != 0) {
            char msg[600];
            snprintf(msg, sizeof(msg), "Media file not found or not readable: %s", answers[1].text);
            print_error(msg);
            return;
        }
        char why[256];
        if (api_check_upload(answers[1].text, why, sizeof(why)) != 0) { print_error(why); return; }
        if (api_upload_media_with_id(answers[1].text, media_url, sizeof(media_url), &media_id) != 0) {
            print_error(api_get_last_error());
            return;
        }
        media_url_ptr = media_url;
    }

    char post_id[64] = {0};
    if (api_create_post(text, media_url_ptr, post_id, sizeof(post_id)) != 0) {
        if (media_id) api_delete_media(media_id);
        print_error(api_get_last_error());
        return;
    }

    char msg[600];
    if (media_url_ptr) snprintf(msg, sizeof(msg), "Created post %s (media: %s)", post_id, media_url);
    else snprintf(msg, sizeof(msg), "Created post %s", post_id);
    print_success(msg);
}

static void cmd_delete_post(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    if (api_delete_post(post_id) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Deleted post %s", post_id);
    print_success(msg);
}

static void cmd_like(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    if (api_like_post(post_id) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Liked post %s", post_id);
    print_success(msg);
}

static void cmd_unlike(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    if (api_unlike_post(post_id) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Unliked post %s", post_id);
    print_success(msg);
}

static void cmd_likes(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    char out[16384] = {0};
    if (api_get_post_likes(post_id, out, sizeof(out)) != 0) {
        print_error(api_get_last_error());
        return;
    }
    api_users_result_t res;
    res.count = 0;
    const char *arr_start, *arr_end;
    if (json_get_array(out, "likes", &arr_start, &arr_end) == 0 ||
        json_get_array(out, "users", &arr_start, &arr_end) == 0) {
        int len = json_array_len(arr_start, arr_end);
        for (int i = 0; i < len && i < 256; i++) {
            const char *is, *ie;
            if (json_array_get_item(arr_start, arr_end, i, &is, &ie) != 0) break;
            int l = (int)(ie - is);
            char item[4096];
            if (l >= (int)sizeof(item)) l = sizeof(item) - 1;
            memcpy(item, is, l);
            item[l] = '\0';
            api_user_t *u = &res.users[res.count];
            if (json_get_int(item, "userId", &u->id) != 0) json_get_int(item, "id", &u->id);
            json_get_string(item, "email", u->email, sizeof(u->email));
            if (u->email[0] == '\0') json_get_string(item, "userEmail", u->email, sizeof(u->email));
            json_get_string(item, "created_at", u->created_at, sizeof(u->created_at));
            res.count++;
        }
        print_users(&res);
        return;
    }
    printf("%s\n", out);
}

/* ---------------- Comments ---------------- */

static void cmd_comments(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    int offset = answer_int(&answers[1], 0);
    static api_comments_result_t result;
    if (api_get_post_comments_threaded(post_id, offset, 25, &result) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_comments(&result);
}

static void cmd_comment(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *post_id = answers[0].text;
    const char *text = answers[1].text;
    int comment_id = 0;
    if (api_create_comment(post_id, text, &comment_id) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Comment %d added to %s", comment_id, post_id);
    print_success(msg);
}

static void cmd_delete_comment(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *comment_id = answers[0].text;
    if (api_delete_comment(atoi(comment_id)) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Deleted comment %s", comment_id);
    print_success(msg);
}

/* ---------------- Users / follow ---------------- */

static void cmd_users(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    (void)answers;
    api_users_result_t result;
    if (api_get_users(&result) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_users(&result);
}

static void cmd_profile(ss_state_t *state, const wizard_answer_t *answers) {
    int user_id = answers[0].provided ? atoi(answers[0].text) : state->user.user_id;

    char email[256] = {0};
    char created[32] = {0};
    if (api_get_user_info(user_id, email, sizeof(email), created, sizeof(created)) != 0) {
        print_error(api_get_last_error());
        return;
    }

    api_users_result_t followers, following;
    api_get_my_followers(user_id, &followers);
    api_get_my_follows(user_id, &following);
    char role[16] = "user";
    api_get_user_role(user_id, role, sizeof(role));
    print_profile(user_id, email, created, followers.count, following.count, role);

    if (user_id != state->user.user_id) {
        int is_following = 0;
        api_is_following(user_id, &is_following);
        printf("%sFollowing:%s  %s\n", C_BOLD, C_RESET, is_following ? "Yes" : "No");
    }
}

static void cmd_follow(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *user_id = answers[0].text;
    if (api_follow_user(atoi(user_id)) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Followed user %s", user_id);
    print_success(msg);
}

static void cmd_unfollow(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *user_id = answers[0].text;
    if (api_unfollow_user(atoi(user_id)) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[5100];
    snprintf(msg, sizeof(msg), "Unfollowed user %s", user_id);
    print_success(msg);
}

static void cmd_followers(ss_state_t *state, const wizard_answer_t *answers) {
    int user_id = answers[0].provided ? atoi(answers[0].text) : state->user.user_id;
    api_users_result_t result;
    if (api_get_my_followers(user_id, &result) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_users(&result);
}

static void cmd_following(ss_state_t *state, const wizard_answer_t *answers) {
    int user_id = answers[0].provided ? atoi(answers[0].text) : state->user.user_id;
    api_users_result_t result;
    if (api_get_my_follows(user_id, &result) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_users(&result);
}

/* ---------------- Notifications ---------------- */

static void cmd_notifications(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    int offset = answer_int(&answers[0], 0);
    api_notifications_result_t result;
    if (api_get_notifications(offset, &result) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_notifications(&result);
}

static void cmd_notify_count(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    (void)answers;
    int count = 0;
    if (api_get_unseen_notification_count(&count) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_count(count);
}

static void cmd_mark_seen(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    (void)answers;
    if (api_mark_notifications_seen() != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_success("Notifications marked as seen.");
}

/* ---------------- Account ---------------- */

static void cmd_delete_account(ss_state_t *state, const wizard_answer_t *answers) {
    const char *password = answers[0].text;
    if (api_delete_account(password) != 0) {
        print_error(api_get_last_error());
        return;
    }
    ss_state_clear(state);
    print_success("Account deleted.");
}

/* ---------------- Replies, safety, invites, devices ---------------- */

static void cmd_reply(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    int id = 0;
    if (api_create_reply(answers[0].text, atoi(answers[1].text), answers[2].text, &id) != 0) {
        print_error(api_get_last_error());
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Reply %d added under comment %.20s", id, answers[1].text);
    print_success(msg);
}

static void cmd_report(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    const char *type = answers[0].text;
    if (strcmp(type, "post") && strcmp(type, "comment") && strcmp(type, "user")) {
        print_error("What to report must be post, comment or user.");
        return;
    }
    printf("Why? ");
    for (int i = 0; api_report_reasons[i]; i++) printf("%s%d) %s", i ? "  " : "", i + 1, api_report_reasons[i]);
    printf("\n");
    char pick[16];
    if (ask_line("Reason number: ", pick, sizeof(pick)) != 0) { printf("Cancelled.\n"); return; }
    int n = atoi(pick), count = 0;
    while (api_report_reasons[count]) count++;
    if (n < 1 || n > count) { print_error("Pick one of the numbers shown."); return; }
    if (!ask_yes("Send this report to the moderators?")) { printf("Cancelled.\n"); return; }
    if (api_report(type, answers[1].text, api_report_reasons[n - 1], answers[2].provided ? answers[2].text : NULL) != 0) {
        print_error(api_get_last_error());
        return;
    }
    print_success(strcmp(type, "user") ? "Thank you. The report was sent, and it's folded away for you now."
                                        : "Thank you. The report was sent to the moderators.");
}

static void cmd_block(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    if (!ask_yes("Neither of you will see the other's posts, comments or profile. Block?")) { printf("Cancelled.\n"); return; }
    if (api_block_user(atoi(answers[0].text)) != 0) { print_error(api_get_last_error()); return; }
    print_success("Blocked.");
}

static void cmd_unblock(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state;
    if (api_unblock_user(atoi(answers[0].text)) != 0) { print_error(api_get_last_error()); return; }
    print_success("Unblocked.");
}

static void cmd_blocked(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state; (void)answers;
    static api_users_result_t res;
    if (api_get_blocked_users(&res) != 0) { print_error(api_get_last_error()); return; }
    if (res.count == 0) { printf("You haven't blocked anyone.\n"); return; }
    print_users(&res);
}

static void cmd_invite(ss_state_t *state, const wizard_answer_t *answers) {
    (void)state; (void)answers;
    api_invites_t inv;
    if (api_get_my_invites(&inv) != 0) { print_error(api_get_last_error()); return; }
    if (inv.used && !inv.unlimited) printf("You've already used your invite.\n");
    for (int i = 0; i < inv.count; i++) {
        printf("Live code: %s%s%s  (valid until %s)\n", C_BOLD, inv.codes[i].code, C_RESET, when(inv.codes[i].expires_at, NULL));
        if (!inv.unlimited && ask_yes("Cancel this code?")) {
            if (api_cancel_invite_code(inv.codes[i].code) != 0) print_error(api_get_last_error());
            else print_success("The code was cancelled.");
            return;
        }
    }
    if (!inv.can_generate) return;
    printf("%s\n", inv.unlimited ? "As the owner you can make as many codes as you like." : "You can only invite one person, so choose wisely!");
    if (!ask_yes("Make an invite code now?")) return;
    char code[32] = {0}, expires[32] = {0};
    if (api_generate_invite_code(code, sizeof(code), expires, sizeof(expires)) != 0) { print_error(api_get_last_error()); return; }
    printf("Your invite code: %s%s%s\nValid until %s. It works once.\n", C_BOLD, code, C_RESET, when(expires, NULL));
}

static void cmd_devices(ss_state_t *state, const wizard_answer_t *answers) {
    (void)answers;
    api_sessions_t ss;
    if (api_get_sessions(&ss) != 0) { print_error(api_get_last_error()); return; }
    for (int i = 0; i < ss.count; i++) {
        api_session_t *d = &ss.sessions[i];
        printf("%d) %s%s%s %s\n   signed in %s, last active %s\n", i + 1, C_BOLD, d->device_name, C_RESET,
               d->is_current ? "(this device)" : "", when(d->created_at, d->created_tz), when(d->last_used_at, NULL));
    }
    char pick[16];
    if (ask_line("Sign out a device: its number, 'others' for all but this one, or Enter for none: ", pick, sizeof(pick)) != 0 || !pick[0]) return;
    if (!strcmp(pick, "others")) {
        int n = 0;
        if (api_revoke_all_other_sessions(&n) != 0) { print_error(api_get_last_error()); return; }
        char msg[64];
        snprintf(msg, sizeof(msg), "Signed out %d other device(s).", n);
        print_success(msg);
        return;
    }
    int k = atoi(pick);
    if (k < 1 || k > ss.count) { print_error("Pick one of the numbers shown."); return; }
    int was_current = 0;
    if (api_revoke_session(ss.sessions[k - 1].id, &was_current) != 0) { print_error(api_get_last_error()); return; }
    if (was_current) { ss_state_clear(state); ss_state_delete_files(); print_success("Signed out (that was this device)."); }
    else print_success("That device is signed out.");
}

/* ---------------- Command table ---------------- */

const command_spec_t g_commands[] = {
    {"login", "Login", "AUTH", 0,
     {{"Email", FT_TEXT, 0, NULL}, {"Password", FT_PASSWORD, 0, NULL}}, 2, cmd_login},
    {"logout", "Logout", "AUTH", 0, {{0}}, 0, cmd_logout},
    {"whoami", "Show current user", "AUTH", 1, {{0}}, 0, cmd_whoami},
    {"register", "Join with an invite code", "AUTH", 0,
     {{"Invite code", FT_TEXT, 0, NULL}}, 1, cmd_register},
    {"send-otp", "Send registration OTP", "AUTH", 0,
     {{"Email", FT_TEXT, 0, NULL}, {"Invite code", FT_TEXT, 1, "(optional)"}}, 2, cmd_send_otp},
    {"forgot-password", "Email a password reset code", "AUTH", 0,
     {{"Email", FT_TEXT, 0, NULL}}, 1, cmd_forgot_password},
    {"accept-terms", "Accept the current Terms of Use", "AUTH", 1, {{0}}, 0, cmd_accept_terms},
    {"reset-password", "Reset password via OTP", "AUTH", 0,
     {{"Email", FT_TEXT, 0, NULL}, {"OTP", FT_TEXT, 0, NULL},
      {"Password", FT_PASSWORD, 0, NULL}, {"Confirm Password", FT_PASSWORD, 0, NULL}},
     4, cmd_reset_password},

    {"feed", "Show feed", "POSTS", 1,
     {{"Limit", FT_INT, 1, "(optional, default 25)"}, {"Offset", FT_INT, 1, "(optional, default 0)"}},
     2, cmd_feed},
    {"posts", "List user posts", "POSTS", 1,
     {{"User ID", FT_INT, 1, "(optional, default: yourself)"},
      {"Limit", FT_INT, 1, "(optional, default 25)"}, {"Offset", FT_INT, 1, "(optional, default 0)"}},
     3, cmd_posts},
    {"post", "Show post + comments", "POSTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}}, 1, cmd_post},
    {"create", "Create post", "POSTS", 1,
     {{"Text", FT_TEXT, 0, NULL}, {"Media file path", FT_TEXT, 1, "(optional)"}}, 2, cmd_create},
    {"delete", "Delete post", "POSTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}}, 1, cmd_delete_post},
    {"like", "Like post", "POSTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}}, 1, cmd_like},
    {"unlike", "Unlike post", "POSTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}}, 1, cmd_unlike},
    {"likes", "List likes for post", "POSTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}}, 1, cmd_likes},

    {"comments", "Show comments", "COMMENTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}, {"Offset", FT_INT, 1, "(optional, default 0)"}}, 2, cmd_comments},
    {"comment", "Add comment", "COMMENTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}, {"Text", FT_TEXT, 0, NULL}}, 2, cmd_comment},
    {"reply", "Reply to a comment", "COMMENTS", 1,
     {{"Post ID", FT_TEXT, 0, NULL}, {"Comment ID", FT_INT, 0, NULL}, {"Text", FT_TEXT, 0, NULL}}, 3, cmd_reply},
    {"delete-comment", "Delete comment", "COMMENTS", 1,
     {{"Comment ID", FT_INT, 0, NULL}}, 1, cmd_delete_comment},

    {"users", "List all users", "USERS", 1, {{0}}, 0, cmd_users},
    {"profile", "Show profile", "USERS", 1,
     {{"User ID", FT_INT, 1, "(optional, default: yourself)"}}, 1, cmd_profile},
    {"follow", "Follow user", "USERS", 1,
     {{"User ID", FT_INT, 0, NULL}}, 1, cmd_follow},
    {"unfollow", "Unfollow user", "USERS", 1,
     {{"User ID", FT_INT, 0, NULL}}, 1, cmd_unfollow},
    {"followers", "List followers", "USERS", 1,
     {{"User ID", FT_INT, 1, "(optional, default: yourself)"}}, 1, cmd_followers},
    {"following", "List following", "USERS", 1,
     {{"User ID", FT_INT, 1, "(optional, default: yourself)"}}, 1, cmd_following},

    {"notifications", "Show notifications", "NOTIFICATIONS", 1,
     {{"Offset", FT_INT, 1, "(optional, default 0)"}}, 1, cmd_notifications},
    {"notify-count", "Unseen notification count", "NOTIFICATIONS", 1, {{0}}, 0, cmd_notify_count},
    {"mark-seen", "Mark notifications seen", "NOTIFICATIONS", 1, {{0}}, 0, cmd_mark_seen},

    {"report", "Report a post, comment or person", "SAFETY", 1,
     {{"What (post, comment or user)", FT_TEXT, 0, NULL}, {"Its ID", FT_TEXT, 0, NULL},
      {"Details", FT_TEXT, 1, "(optional)"}}, 3, cmd_report},
    {"block", "Block someone", "SAFETY", 1, {{"User ID", FT_INT, 0, NULL}}, 1, cmd_block},
    {"unblock", "Unblock someone", "SAFETY", 1, {{"User ID", FT_INT, 0, NULL}}, 1, cmd_unblock},
    {"blocked", "People you blocked", "SAFETY", 1, {{0}}, 0, cmd_blocked},

    {"invite", "Your invite code", "ACCOUNT", 1, {{0}}, 0, cmd_invite},
    {"devices", "Signed-in devices", "ACCOUNT", 1, {{0}}, 0, cmd_devices},
    {"delete-account", "Delete your account", "ACCOUNT", 1,
     {{"Password", FT_PASSWORD, 0, NULL}}, 1, cmd_delete_account},
};

const int g_command_count = sizeof(g_commands) / sizeof(g_commands[0]);
