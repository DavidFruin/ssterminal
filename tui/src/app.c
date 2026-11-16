#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ncurses.h>
#include "app.h"
#include "ui.h"
#include "timefmt.h"
#include "ss_api.h"
#include "net.h"
#include "ss_api.h"
#include "detail.h"
#include "editor.h"
#include "shell.h"
#include "filepick.h"
#include <unistd.h>
#include "settings.h"

static const char *TAB_NAMES[TAB_COUNT] = {
    "Feed", "Notifs", "Users", "Me", "Settings"
};

const char *app_tab_name(tab_t t) {
    if (t < 0 || t >= TAB_COUNT) return "?";
    return TAB_NAMES[t];
}

/* ---------- screen stack ---------- */

/* Frames remember what to rebuild, because opening a profile from a
 * follows list overwrites the profile state behind it. */
void app_push(app_t *app, view_t view) {
    if (app->depth >= VIEW_STACK_MAX) return;
    frame_t *f = &app->stack[app->depth++];
    f->view = app->view;
    f->profile_id = app->profile_id;
    f->list_kind = app->list_kind;
    f->list_owner = app->list_owner;
    app->view = view;
}

void app_pop(app_t *app) {
    if (app->depth <= 0) { app->view = VIEW_TABS; return; }

    frame_t *f = &app->stack[--app->depth];
    app->view = f->view;
    app->status[0] = '\0';

    /* Rebuild whatever the frame was showing. */
    if (f->view == VIEW_PROFILE && f->profile_id != app->profile_id)
        net_open_profile(app, f->profile_id);
    else if (f->view == VIEW_USERLIST)
        net_open_user_list(app, f->list_owner, f->list_kind);
}

/* Everything fetched belongs to one session, so a logout clears it all
 * rather than letting the next user see the last one's feed. */
void app_reset_data(app_t *app) {
    store_clear(&app->feed.store);
    app->feed.sel = app->feed.top = app->feed.on_more = 0;
    app->feed.fetched = 0;

    cstore_clear(&app->comments);
    nstore_clear(&app->notifs);
    app->notif_sel = app->notif_top = app->notif_on_more = app->notif_new = 0;
    app->notifs_fetched = 0;

    store_clear(&app->profile_posts.store);
    app->profile_posts.sel = app->profile_posts.top = app->profile_posts.on_more = 0;
    app->profile_posts.fetched = 0;
    app->profile_id = 0;

    if (app->users) memset(app->users, 0, sizeof(*app->users));
    if (app->my_follows) memset(app->my_follows, 0, sizeof(*app->my_follows));
    if (app->list_users) memset(app->list_users, 0, sizeof(*app->list_users));
    app->users_sel = app->users_top = 0;
    app->users_fetched = 0;

    app->unseen = 0;
    app->unseen_fetched = 0;
    app->depth = 0;
    app->view = VIEW_TABS;
    app->tab = TAB_FEED;
    app->settings_sel = 0;
    app->status[0] = '\0';
    app->status_is_error = 0;
}

int app_follows(app_t *app, int id) {
    if (!app->my_follows) return 0;
    for (int i = 0; i < app->my_follows->count; i++)
        if (app->my_follows->users[i].id == id) return 1;
    return 0;
}

int app_init(app_t *app) {
    memset(app, 0, sizeof(*app));
    ss_state_init(&app->state);
    tui_cfg_load(&app->cfg);
    store_init(&app->feed.store);
    cstore_init(&app->comments);
    nstore_init(&app->notifs);
    store_init(&app->profile_posts.store);
    app->tab = TAB_FEED;
    app->view = VIEW_TABS;
    app->detail_src = -1;
    return 0;
}

void app_free(app_t *app) {
    store_free(&app->feed.store);
    cstore_free(&app->comments);
    nstore_free(&app->notifs);
    store_free(&app->profile_posts.store);
    free(app->users);
    free(app->my_follows);
    free(app->list_users);
}

void app_set_status(app_t *app, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
    app->status_is_error = 0;
}

void app_set_error(app_t *app, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, ap);
    va_end(ap);
    app->status_is_error = 1;
}

/* Delegates to the library, which knows where this tool's tokens live -
 * they used to be at a fixed shared path, but each front end now has its
 * own directory, and clearing the wrong one would both fail to log this
 * tool out and sign the others out instead. */
void app_clear_session_files(void) {
    ss_state_delete_files();
}

/* Both forms the server uses for a rejected token: api_call's own
 * "HTTP %ld" for a non-2xx response, and the literal message
 * requireAuth() sends in the body. Checked as a substring since some
 * endpoints may wrap it differently. */
static int looks_like_auth_error(const char *msg) {
    if (!msg) return 0;
    return strstr(msg, "HTTP 401") != NULL || strstr(msg, "Unauthorized") != NULL;
}

void app_session_expired(app_t *app) {
    app_clear_session_files();
    ss_state_clear(&app->state);
    api_set_jwt("");
    app->logged_out = 1;
}

/* Failures get a modal so they cannot scroll past unnoticed; transient
 * messages stay in the status line. A rejected token is different: no
 * modal loop is going to fix it, so this drops straight back to the
 * login screen with one explanatory message instead. */
static void report(app_t *app, int rc) {
    if (rc == 0 || !app->status_is_error) return;

    if (looks_like_auth_error(app->status)) {
        app_session_expired(app);
        app_set_error(app, "Session expired. Logged out.");
        ui_modal_error(app, app->status);
        return;
    }

    ui_modal_error(app, app->status);
}

static void list_move(post_list_t *pl, int delta) {
    if (pl->store.count == 0) return;

    if (pl->on_more) {
        if (delta < 0) { pl->on_more = 0; pl->sel = pl->store.count - 1; }
        return;
    }

    int next = pl->sel + delta;

    if (next >= pl->store.count) {
        if (pl->store.has_more) { pl->on_more = 1; return; }
        next = pl->store.count - 1;
    }
    if (next < 0) next = 0;
    pl->sel = next;
}

static void feed_move(app_t *app, int delta) { list_move(&app->feed, delta); }

static void feed_page(app_t *app, int dir) {
    int step = ui_body_height() / 2;
    if (step < 1) step = 1;
    feed_move(app, dir * step);
}

/* The post list on whichever screen is in front. */
static post_list_t *active_list(app_t *app) {
    if (app->view == VIEW_PROFILE) return &app->profile_posts;
    if (app->tab == TAB_ME) return &app->profile_posts;
    return &app->feed;
}

static void open_profile(app_t *app, int user_id) {
    /* From a tab this pushes; from a list it also pushes, so Esc walks
     * back the way the user came. */
    view_t from = app->view;
    app_push(app, VIEW_PROFILE);
    if (net_open_profile(app, user_id) != 0) {
        app_pop(app);
        app->view = from;
        if (app->status_is_error) ui_modal_error(app, app->status);
    }
}

static void open_user_list(app_t *app, list_kind_t kind) {
    int owner = (app->view == VIEW_PROFILE) ? app->profile_id : app->state.user.user_id;
    view_t from = app->view;
    app_push(app, VIEW_USERLIST);
    if (net_open_user_list(app, owner, kind) != 0) {
        app_pop(app);
        app->view = from;
        if (app->status_is_error) ui_modal_error(app, app->status);
    }
}

static void open_selected_profile_post(app_t *app) {
    post_list_t *pl = &app->profile_posts;
    if (pl->store.count == 0) return;

    if (pl->on_more) {
        report(app, net_load_more_profile_posts(app));
        pl->on_more = 0;
        return;
    }

    app_push(app, VIEW_POST);
    if (net_open_post(app, pl->store.posts[pl->sel].id, -1) != 0) {
        app_pop(app);
        if (app->status_is_error) ui_modal_error(app, app->status);
    }
}

/* Liking from the feed reuses net_toggle_like by pointing the detail slot
 * at the selected row first, so both copies stay in step. */
static void feed_like(app_t *app) {
    if (app->feed.store.count == 0 || app->feed.on_more) return;
    api_post_t saved = app->detail;
    int saved_src = app->detail_src;

    app->detail = app->feed.store.posts[app->feed.sel];
    app->detail_src = app->feed.sel;

    int rc = net_toggle_like(app);

    if (rc == 0) app->feed.store.posts[app->feed.sel] = app->detail;

    /* Restore whatever the detail view was holding. */
    app->detail = saved;
    app->detail_src = saved_src;

    if (rc != 0 && app->status_is_error) ui_modal_error(app, app->status);
}

static void open_selected_post(app_t *app) {
    if (app->feed.store.count == 0 || app->feed.on_more) return;
    app_push(app, VIEW_POST);
    if (net_open_post(app, app->feed.store.posts[app->feed.sel].id, app->feed.sel) != 0) {
        app_pop(app);
        if (app->status_is_error) ui_modal_error(app, app->status);
    }
}

static void notif_move(app_t *app, int delta) {
    if (app->notifs.count == 0) return;

    if (app->notif_on_more) {
        if (delta < 0) { app->notif_on_more = 0; app->notif_sel = app->notifs.count - 1; }
        return;
    }

    int next = app->notif_sel + delta;
    if (next >= app->notifs.count) {
        if (app->notifs.has_more) { app->notif_on_more = 1; return; }
        next = app->notifs.count - 1;
    }
    if (next < 0) next = 0;
    app->notif_sel = next;
}

/* Opens the post a notification refers to. follow/unfollow carry no post,
 * so there is nothing to open until profiles exist. */
static void open_notif_target(app_t *app) {
    if (app->notifs.count == 0) return;

    if (app->notif_on_more) {
        report(app, net_load_more_notifs(app));
        app->notif_on_more = 0;
        return;
    }

    api_notification_t *n = &app->notifs.notifs[app->notif_sel];
    if (!n->post_id[0]) {
        /* A follow: open the person instead. */
        if (n->actor_id > 0) open_profile(app, n->actor_id);
        return;
    }
    app->pending_comment_id = n->comment_id;   /* a reply or comment: select it */

    /* -1: this did not come from the feed, so there is no row to keep in step. */
    app_push(app, VIEW_POST);
    if (net_open_post(app, n->post_id, -1) != 0) {
        app_pop(app);
        if (app->status_is_error) ui_modal_error(app, app->status);
    }
}

static void close_post(app_t *app) {
    app_pop(app);
}

/* Collects text either inline or from $EDITOR, per the config. Returns 1
 * if there is something to send. */
static int compose_text(app_t *app, const char *title, const char *send_label,
                        char *out, size_t outsz, char *attach, size_t attach_sz,
                        const char *initial) {
    if (!initial) initial = "";
    if (app->cfg.compose_mode == CFG_COMPOSE_EXTERNAL) {
        app->input_active = 1;
        int rc = shell_edit_text(initial, out, outsz);
        app->input_active = 0;

        if (rc < 0) {
            app_set_error(app, "no editor found -- set $EDITOR, or use editor = inline in %s",
                          app->cfg.path);
            ui_modal_error(app, app->status);
            return 0;
        }
        if (rc == 0) { app_set_status(app, "Nothing written, nothing sent."); return 0; }

        /* No editor box to host ^O, so ask afterwards instead. */
        if (attach && ui_confirm(app, "Attach a media file to this post?", 0)) {
            char picked[1024];
            if (filepick_run(app, picked, sizeof(picked)))
                snprintf(attach, attach_sz, "%s", picked);
        }
        return 1;
    }

    editor_t ed;
    if (editor_init(&ed, initial, 5000) != 0) {
        app_set_error(app, "out of memory");
        return 0;
    }

    int rc = editor_run(app, &ed, title, send_label, attach, attach_sz);
    if (rc != EDITOR_SUBMIT || ed.len == 0) {
        editor_free(&ed);
        if (rc == EDITOR_SUBMIT) app_set_status(app, "Empty -- nothing sent.");
        return 0;
    }

    snprintf(out, outsz, "%s", ed.buf);
    editor_free(&ed);
    return 1;
}

/* A text the server refused (a word that isn't allowed, characters it
 * doesn't take, too long: all 400s) goes back into the editor so it can be
 * fixed instead of retyped. */
static int refused_text(void) { return api_get_last_status() == 400; }

static void compose_post(app_t *app) {
    char text[5200] = {0};
    char attach[1024] = {0};
    for (;;) {
        char initial[5200];
        snprintf(initial, sizeof(initial), "%s", text);
        if (!compose_text(app, "New post", "post", text, sizeof(text), attach, sizeof(attach), initial))
            return;
        if (net_create_post(app, text, attach) == 0) return;
        if (!refused_text()) { report(app, -1); return; }
        ui_modal_error(app, app->status);
    }
}

/* Media only attaches to posts: comments and replies take none.
 * parent_id 0 = a comment on the post; otherwise a reply under it. */
static void compose_reply(app_t *app, int parent_id, const char *title) {
    char text[5200] = {0};
    for (;;) {
        char initial[5200];
        snprintf(initial, sizeof(initial), "%s", text);
        if (!compose_text(app, title, parent_id ? "reply" : "comment", text, sizeof(text), NULL, 0, initial)) return;
        if (net_create_reply(app, parent_id, text) == 0) return;
        if (!refused_text()) { report(app, -1); return; }
        ui_modal_error(app, app->status);
    }
}

static void compose_comment(app_t *app) {
    if (app->detail.frozen) { app_set_status(app, "This post is hidden by a moderator."); return; }
    compose_reply(app, 0, "New comment");
}

/* r on a comment: replies go under the top-level comment of the thread. */
static void reply_to_selected(app_t *app) {
    if (app->comments.count == 0 || app->detail_on_more) { app_set_status(app, "Select a comment to reply to."); return; }
    api_comment_t *c = &app->comments.comments[app->comment_sel];
    int parent = c->parent_id ? c->parent_id : c->id;
    api_comment_t *top = c;
    for (int i = 0; i < app->comments.count; i++) if (app->comments.comments[i].id == parent) top = &app->comments.comments[i];
    if (top->deleted || top->frozen_by_staff) { app_set_status(app, "That thread can't take new replies."); return; }
    char title[300];
    snprintf(title, sizeof(title), "Reply to %.200s", top->user_email);
    compose_reply(app, parent, title);
}

/* Reason menu, then a confirmation, then the report. */
static void report_item(app_t *app, const char *type, const char *id, const char *what) {
    static const char *const labels[] = {
        "Spam", "Harassment or bullying", "Hate", "Sexual content", "Violence",
        "Self-harm", "Illegal", "Something else"
    };
    int pick = ui_choose(app, "Why are you reporting it?", labels, 8);
    if (pick < 0) { app_set_status(app, "Not reported."); return; }
    char q[256];
    snprintf(q, sizeof(q), "Report %s to the moderators for: %s?", what, labels[pick]);
    if (!ui_confirm(app, q, 0)) { app_set_status(app, "Not reported."); return; }
    report(app, net_report(app, type, id, api_report_reasons[pick]));
}

static void report_in_post(app_t *app) {
    int has_comment = app->comments.count > 0 && !app->detail_on_more;
    api_comment_t *c = has_comment ? &app->comments.comments[app->comment_sel] : NULL;
    if (c && (c->deleted || c->frozen_by_staff || c->user_id == app->state.user.user_id)) c = NULL;
    int own_post = app->detail.user_id == app->state.user.user_id;
    if (own_post && !c) { app_set_status(app, "There is nothing here you can report."); return; }

    int choice = 0;
    if (c && !own_post) {
        static const char *const opts[] = {"The post", "The selected comment"};
        choice = ui_choose(app, "Report what?", opts, 2);
        if (choice < 0) return;
    } else if (c) {
        choice = 1;
    }
    if (choice == 0) {
        report_item(app, "post", app->detail.id, "this post");
    } else {
        char id[16];
        snprintf(id, sizeof(id), "%d", c->id);
        report_item(app, "comment", id, "this comment");
    }
}

static void invite_flow(app_t *app);
/* i on the Me tab: the member's one invite (the owner: as many as they like). */
void app_invite(app_t *app);
void app_invite(app_t *app) { invite_flow(app); }
static void invite_flow(app_t *app) {
    api_invites_t inv;
    if (api_get_my_invites(&inv) != 0) { app_set_error(app, "%s", api_get_last_error()); report(app, -1); return; }
    char msg[1024];
    size_t len = 0;
    char exp[64];
    for (int i = 0; i < inv.count && len < sizeof(msg) - 120; i++) {
        timefmt_full(inv.codes[i].expires_at, exp, sizeof(exp));
        len += snprintf(msg + len, sizeof(msg) - len, "Your code: %s  (valid until %s)\n", inv.codes[i].code, exp);
    }
    if (inv.count && !inv.unlimited) {
        snprintf(msg + len, sizeof(msg) - len, "Give it to the one person you want to invite.");
        ui_message(app, "Invite someone", msg);
        if (ui_confirm(app, "Cancel this code instead?", 0)) {
            if (api_cancel_invite_code(inv.codes[0].code) != 0) { app_set_error(app, "%s", api_get_last_error()); report(app, -1); }
            else app_set_status(app, "The code was cancelled.");
        }
        return;
    }
    if (!inv.can_generate) {
        ui_message(app, "Invite someone", "You've already used your invite.");
        return;
    }
    if (!ui_confirm(app, inv.unlimited ? "Make a new invite code? (As the owner you can make as many as you like.)"
                                       : "You can only invite one person, so choose wisely! Make your invite code now?", 0))
        return;
    char code[32] = {0}, expires[32] = {0};
    if (api_generate_invite_code(code, sizeof(code), expires, sizeof(expires)) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        report(app, -1);
        return;
    }
    timefmt_full(expires, exp, sizeof(exp));
    snprintf(msg, sizeof(msg), "Your invite code: %s\nValid until %s. It works once.\nSelect it with the mouse to copy it.", code, exp);
    ui_message(app, "Invite someone", msg);
}

static int is_mine(app_t *app, int user_id) {
    return app->state.user.user_id != 0 && user_id == app->state.user.user_id;
}

static void delete_current_post(app_t *app) {
    if (!is_mine(app, app->detail.user_id)) {
        app_set_status(app, "You can only delete your own posts.");
        return;
    }
    if (!ui_confirm(app, "Delete this post? This cannot be undone.", 1)) {
        app_set_status(app, "Not deleted.");
        return;
    }

    char id[64];
    snprintf(id, sizeof(id), "%s", app->detail.id);

    if (net_delete_post(app, id) == 0) {
        app_pop(app);
    } else if (app->status_is_error) {
        ui_modal_error(app, app->status);
    }
}

static void delete_selected_comment(app_t *app) {
    if (app->comments.count == 0 || app->detail_on_more) return;
    if (app->comment_sel < 0 || app->comment_sel >= app->comments.count) return;

    api_comment_t *c = &app->comments.comments[app->comment_sel];
    if (!is_mine(app, c->user_id)) {
        app_set_status(app, "You can only delete your own comments.");
        return;
    }
    if (!ui_confirm(app, "Delete this comment? This cannot be undone.", 1)) {
        app_set_status(app, "Not deleted.");
        return;
    }

    report(app, net_delete_comment(app, c->id));
}

/* Closes every pushed view in one step (a post opened from a profile you
 * drilled into, say) so tab/shift-tab from inside one always lands
 * somewhere visible instead of just changing app->tab under a view that
 * doesn't render it. Skips app_pop()'s per-frame rebuild since none of
 * those intermediate frames are about to be shown anyway. */
static void close_to_tabs(app_t *app) {
    app->depth = 0;
    app->view = VIEW_TABS;
    app->status[0] = '\0';
}

static void switch_tab(app_t *app, tab_t t) {
    if (t == app->tab) return;
    app->tab = t;
    app->status[0] = '\0';

    /* Re-fetch on arrival when what we hold is stale. */
    if (t == TAB_FEED) {
        int stale = !app->feed.fetched ||
            (app->cfg.feed_refresh_secs > 0 &&
             difftime(time(NULL), app->feed.fetched) >= app->cfg.feed_refresh_secs);
        if (stale) report(app, net_refresh_feed(app));
    } else if (t == TAB_NOTIFS) {
        int stale = !app->notifs_fetched ||
            (app->cfg.feed_refresh_secs > 0 &&
             difftime(time(NULL), app->notifs_fetched) >= app->cfg.feed_refresh_secs);
        if (stale) report(app, net_refresh_notifs(app));
    } else if (t == TAB_USERS) {
        int stale = !app->users_fetched ||
            (app->cfg.feed_refresh_secs > 0 &&
             difftime(time(NULL), app->users_fetched) >= app->cfg.feed_refresh_secs);
        if (stale) report(app, net_refresh_users(app));
    } else if (t == TAB_ME) {
        /* The Me tab is the profile view pointed at yourself. */
        int stale = app->profile_id != app->state.user.user_id ||
                    !app->profile_posts.fetched ||
                    (app->cfg.feed_refresh_secs > 0 &&
                     difftime(time(NULL), app->profile_posts.fetched) >= app->cfg.feed_refresh_secs);
        if (stale) report(app, net_open_profile(app, app->state.user.user_id));
    }
}

/* Idle timers. Suppressed while a composer or prompt owns the keyboard,
 * so a refresh can never yank the screen out from under someone typing. */
static void run_timers(app_t *app) {
    if (app->input_active) return;

    time_t now = time(NULL);

    if (app->cfg.badge_refresh_secs > 0 &&
        difftime(now, app->unseen_fetched) >= app->cfg.badge_refresh_secs) {
        net_refresh_badge(app);   /* silent: a failed badge poll is not worth a modal */
    }

    if (app->view != VIEW_TABS) return;

    if (app->tab == TAB_FEED && app->cfg.feed_refresh_secs > 0 && app->feed.fetched &&
        difftime(now, app->feed.fetched) >= app->cfg.feed_refresh_secs) {
        /* Hold position across an idle refresh rather than snapping to top. */
        int sel = app->feed.sel, top = app->feed.top;
        if (net_refresh_feed(app) == 0) {
            if (sel < app->feed.store.count) { app->feed.sel = sel; app->feed.top = top; }
        }
    }

    if (app->tab == TAB_NOTIFS && app->cfg.feed_refresh_secs > 0 && app->notifs_fetched &&
        difftime(now, app->notifs_fetched) >= app->cfg.feed_refresh_secs) {
        int sel = app->notif_sel, top = app->notif_top;
        if (net_refresh_notifs(app) == 0) {
            if (sel < app->notifs.count) { app->notif_sel = sel; app->notif_top = top; }
        }
    }
}

void app_run(app_t *app) {
    if (!app->feed.fetched) report(app, net_refresh_feed(app));
    net_refresh_badge(app);

    while (!app->quit) {
        if (app->logged_out) return;      /* main shows the login screen again */

        ui_draw(app);
        doupdate();   /* ui_draw only stages the frame; nothing else follows here */

        int ch = getch();

        if (ch == ERR) {          /* timeout tick */
            run_timers(app);
            continue;
        }

        /* Any keypress clears a stale status message so hints return. */
        if (app->status[0] && !app->status_is_error) app->status[0] = '\0';

        /* Profile view: its own post list, follow, follows/followers. */
        if (app->view == VIEW_PROFILE) {
            switch (ch) {
                case KEY_RESIZE: continue;

                case 27: case KEY_BACKSPACE: case 127: case 8:
                    app_pop(app);
                    continue;

                case 'q': app->quit = 1; continue;

                case 'j': case KEY_DOWN: list_move(&app->profile_posts, 1);  continue;
                case 'k': case KEY_UP:   list_move(&app->profile_posts, -1); continue;
                case KEY_NPAGE: case 4:
                    list_move(&app->profile_posts, ui_body_height() / 2);  continue;
                case KEY_PPAGE: case 21:
                    list_move(&app->profile_posts, -(ui_body_height() / 2)); continue;

                case 'g': case KEY_HOME:
                    app->profile_posts.sel = 0; app->profile_posts.top = 0;
                    app->profile_posts.on_more = 0; continue;
                case 'G': case KEY_END:
                    if (app->profile_posts.store.count) {
                        app->profile_posts.sel = app->profile_posts.store.count - 1;
                        app->profile_posts.on_more = 0;
                    }
                    continue;

                case '\r': case '\n': case KEY_ENTER: case ' ':
                    open_selected_profile_post(app);
                    continue;

                case 'f': report(app, net_toggle_follow(app)); continue;
                case 'w': open_user_list(app, LIST_FOLLOWS);   continue;
                case 'W': open_user_list(app, LIST_FOLLOWERS); continue;

                case 'r': report(app, net_open_profile(app, app->profile_id)); continue;
                case 'R':
                    if (app->profile_id == app->state.user.user_id) { app_set_status(app, "That's you."); continue; }
                    {
                        char id[16];
                        snprintf(id, sizeof(id), "%d", app->profile_id);
                        report_item(app, "user", id, "this person");
                    }
                    continue;
                case 'B':
                    if (app->profile_id == app->state.user.user_id) { app_set_status(app, "That's you."); continue; }
                    if (ui_confirm(app, "Block this person? Neither of you will see the other's posts, comments or profile.", 1)
                        && net_block(app, app->profile_id) == 0)
                        app_pop(app);
                    else report(app, app->status_is_error ? -1 : 0);
                    continue;
                case '?': ui_help(app); continue;
                default:  continue;
            }
        }

        /* A follows/followers list. */
        if (app->view == VIEW_USERLIST) {
            api_users_result_t *lst = app->list_users;
            int n = lst ? lst->count : 0;

            switch (ch) {
                case KEY_RESIZE: continue;

                case 27: case KEY_BACKSPACE: case 127: case 8:
                    app_pop(app);
                    continue;

                case 'q': app->quit = 1; continue;

                case 'j': case KEY_DOWN: if (app->list_sel < n - 1) app->list_sel++; continue;
                case 'k': case KEY_UP:   if (app->list_sel > 0) app->list_sel--; continue;
                case 'g': case KEY_HOME: app->list_sel = 0; continue;
                case 'G': case KEY_END:  if (n) app->list_sel = n - 1; continue;

                case '\r': case '\n': case KEY_ENTER:
                    if (n) open_profile(app, lst->users[app->list_sel].id);
                    continue;

                case '?': ui_help(app); continue;
                default:  continue;
            }
        }

        /* The post view owns most keys while it is open. */
        if (app->view == VIEW_POST) {
            switch (ch) {
                case KEY_RESIZE: break;

                case 27:            /* esc */
                case KEY_BACKSPACE:
                case 127: case 8:
                    close_post(app);
                    continue;

                case 'q':
                    app->quit = 1;
                    continue;

                case 'j': case KEY_DOWN:  detail_move_selection(app, 1);  continue;
                case 'k': case KEY_UP:    detail_move_selection(app, -1); continue;

                case KEY_NPAGE: case 4:
                    detail_scroll_by(app, ui_body_height() / 2);  continue;
                case KEY_PPAGE: case 21:
                    detail_scroll_by(app, -(ui_body_height() / 2)); continue;

                case 'g': case KEY_HOME:
                    app->detail_scroll = 0; app->comment_sel = 0;
                    app->detail_on_more = 0; continue;

                case 'l':
                    report(app, net_toggle_like(app));
                    if (app->detail_src >= 0 && app->detail_src < app->feed.store.count &&
                        strcmp(app->feed.store.posts[app->detail_src].id, app->detail.id) == 0)
                        app->feed.store.posts[app->detail_src] = app->detail;
                    continue;

                case 'o':
                    report(app, detail_open_media(app));
                    continue;

                case 'c':
                    compose_comment(app);
                    continue;

                case 'r':
                    reply_to_selected(app);
                    continue;

                case 's':
                    app->detail_show_reported = !app->detail_show_reported;
                    continue;

                case 'R':
                    report_in_post(app);
                    continue;

                case 'd':
                    delete_selected_comment(app);
                    continue;

                case 'D':
                    delete_current_post(app);
                    continue;

                case '\r': case '\n': case KEY_ENTER: case ' ':
                    if (app->detail_on_more) {
                        report(app, net_load_more_comments(app));
                        app->detail_on_more = 0;
                    }
                    continue;

                case '\t':
                    close_to_tabs(app);
                    switch_tab(app, (app->tab + 1) % TAB_COUNT);
                    continue;
                case KEY_BTAB:
                    close_to_tabs(app);
                    switch_tab(app, (app->tab + TAB_COUNT - 1) % TAB_COUNT);
                    continue;

                case '?':
                    ui_help(app);
                    continue;

                default:
                    continue;
            }
        }

        switch (ch) {
            case KEY_RESIZE:
                /* ncurses has already resized; the next draw re-lays out. */
                break;

            case 'q':
                app->quit = 1;
                break;

            case 'j': case KEY_DOWN:
                if (app->tab == TAB_SETTINGS) settings_move(app, 1);
                else if (app->tab == TAB_NOTIFS) notif_move(app, 1);
                else if (app->tab == TAB_USERS) {
                    int n = app->users ? app->users->count : 0;
                    if (app->users_sel < n - 1) app->users_sel++;
                } else list_move(active_list(app), 1);
                break;
            case 'k': case KEY_UP:
                if (app->tab == TAB_SETTINGS) settings_move(app, -1);
                else if (app->tab == TAB_NOTIFS) notif_move(app, -1);
                else if (app->tab == TAB_USERS) {
                    if (app->users_sel > 0) app->users_sel--;
                } else list_move(active_list(app), -1);
                break;
            case KEY_NPAGE: case 4:                                /* ^D */
                if (app->tab == TAB_NOTIFS) notif_move(app, ui_body_height() / 2);
                else if (app->tab == TAB_USERS) {
                    int n = app->users ? app->users->count : 0;
                    app->users_sel += ui_body_height() / 2;
                    if (app->users_sel > n - 1) app->users_sel = n ? n - 1 : 0;
                } else if (app->tab == TAB_ME) list_move(active_list(app), ui_body_height() / 2);
                else feed_page(app, 1);
                break;
            case KEY_PPAGE: case 21:                               /* ^U */
                if (app->tab == TAB_NOTIFS) notif_move(app, -(ui_body_height() / 2));
                else if (app->tab == TAB_USERS) {
                    app->users_sel -= ui_body_height() / 2;
                    if (app->users_sel < 0) app->users_sel = 0;
                } else if (app->tab == TAB_ME) list_move(active_list(app), -(ui_body_height() / 2));
                else feed_page(app, -1);
                break;

            case 'g': case KEY_HOME:
                if (app->tab == TAB_NOTIFS) {
                    app->notif_sel = 0; app->notif_top = 0; app->notif_on_more = 0;
                } else if (app->tab == TAB_USERS) {
                    app->users_sel = 0; app->users_top = 0;
                } else {
                    post_list_t *pl = active_list(app);
                    pl->sel = 0; pl->top = 0; pl->on_more = 0;
                }
                break;

            case 'G': case KEY_END:
                if (app->tab == TAB_NOTIFS) {
                    if (app->notifs.count) {
                        app->notif_sel = app->notifs.count - 1;
                        app->notif_on_more = 0;
                    }
                } else if (app->tab == TAB_USERS) {
                    int n = app->users ? app->users->count : 0;
                    if (n) app->users_sel = n - 1;
                } else {
                    post_list_t *pl = active_list(app);
                    if (pl->store.count) { pl->sel = pl->store.count - 1; pl->on_more = 0; }
                }
                break;

            case 'r':
                if (app->tab == TAB_FEED) { report(app, net_refresh_feed(app)); net_refresh_badge(app); }
                else if (app->tab == TAB_NOTIFS) report(app, net_refresh_notifs(app));
                else if (app->tab == TAB_USERS) report(app, net_refresh_users(app));
                else if (app->tab == TAB_ME) report(app, net_open_profile(app, app->state.user.user_id));
                else net_refresh_badge(app);
                break;

            case '\r': case '\n': case KEY_ENTER: case ' ':
                if (app->tab == TAB_NOTIFS) { open_notif_target(app); break; }
                if (app->tab == TAB_USERS) {
                    int n = app->users ? app->users->count : 0;
                    if (n) open_profile(app, app->users->users[app->users_sel].id);
                    break;
                }
                if (app->tab == TAB_ME) { open_selected_profile_post(app); break; }
                if (app->tab == TAB_SETTINGS) { settings_activate(app); break; }
                if (app->tab != TAB_FEED) break;
                if (app->feed.on_more) {
                    report(app, net_load_more_feed(app));
                    app->feed.on_more = 0;
                    if (app->feed.store.count) app->feed.sel = app->feed.store.count - 1;
                } else {
                    open_selected_post(app);
                }
                break;

            case 'l':
                if (app->tab == TAB_FEED) feed_like(app);
                break;

            case 'p':
                if (app->tab == TAB_FEED) compose_post(app);
                break;

            case 'w':
                if (app->tab == TAB_ME) open_user_list(app, LIST_FOLLOWS);
                break;
            case 'i':
                if (app->tab == TAB_ME) invite_flow(app);
                break;
            case 'W':
                if (app->tab == TAB_ME) open_user_list(app, LIST_FOLLOWERS);
                break;

            case '1': switch_tab(app, TAB_FEED);     break;
            case '2': switch_tab(app, TAB_NOTIFS);   break;
            case '3': switch_tab(app, TAB_USERS);    break;
            case '4': switch_tab(app, TAB_ME);       break;
            case '5': switch_tab(app, TAB_SETTINGS); break;

            case '\t':
                switch_tab(app, (app->tab + 1) % TAB_COUNT);
                break;
            case KEY_BTAB:
                switch_tab(app, (app->tab + TAB_COUNT - 1) % TAB_COUNT);
                break;

            case '?':
                ui_help(app);
                break;

            default:
                break;
        }
    }
}
