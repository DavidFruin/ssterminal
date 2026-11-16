#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>
#include "settings.h"
#include "ui.h"
#include "ss_api.h"
#include "ss_config.h"
#include "ss_state.h"
#include <time.h>
#include "timefmt.h"

static const char *ACTION_LABEL[SETTINGS_ACTIONS] = {
    "Invite someone",
    "Blocked people",
    "Devices",
    "Terms of Use",
    "Change password",
    "Log out",
    "Delete account"
};
#define ACTION_DELETE (SETTINGS_ACTIONS - 1)

/* Media storage used, read when the tab is drawn at most once a minute. */
static long long g_used = -1, g_limit = 0;
static time_t g_storage_at = 0;

void settings_move(app_t *app, int delta) {
    app->settings_sel += delta;
    if (app->settings_sel < 0) app->settings_sel = 0;
    if (app->settings_sel >= SETTINGS_ACTIONS) app->settings_sel = SETTINGS_ACTIONS - 1;
}

void settings_draw(app_t *app, int body_top) {
    int row = body_top;
    int bottom = LINES - 4;  /* leaves room for the footer's rule + hints + status rows */

    attron(A_BOLD);
    mvaddstr(row++, 1, "Session");
    attroff(A_BOLD);

    attron(A_DIM);
    mvprintw(row++, 3, "%s  (id %d)", app->state.user.email, app->state.user.user_id);
    mvprintw(row++, 3, "token in ~/.simple-social-cli/tui/ (this tool only)");
    if (time(NULL) - g_storage_at > 60) {
        api_media_limits_t lim;
        g_storage_at = time(NULL);
        if (api_get_media_limits(&lim) == 0) { g_used = lim.storage_used; g_limit = lim.storage_limit; }
    }
    if (g_used >= 0 && g_limit > 0)
        mvprintw(row++, 3, "media      %.1f MB of %.0f MB used", g_used / 1048576.0, g_limit / 1048576.0);
    attroff(A_DIM);
    row++;

    attron(A_BOLD);
    mvaddstr(row++, 1, "Config");
    attroff(A_BOLD);

    attron(A_DIM);
    char shown[512];
    ui_utf8_take(shown, sizeof(shown), app->cfg.path, COLS - 6, 1);
    mvaddstr(row++, 3, shown);
    mvprintw(row++, 3, "api        %s", config_get_base_url());
    mvprintw(row++, 3, "composer   %s",
             app->cfg.compose_mode == CFG_COMPOSE_EXTERNAL ? "external ($EDITOR)" : "inline");
    mvprintw(row++, 3, "refresh    feed %ds, badge %ds",
             app->cfg.feed_refresh_secs, app->cfg.badge_refresh_secs);
    mvprintw(row++, 3, "page size  %d", app->cfg.page_size);
    attroff(A_DIM);
    row++;

    attron(A_BOLD);
    mvaddstr(row++, 1, "Account");
    attroff(A_BOLD);

    for (int i = 0; i < SETTINGS_ACTIONS && row <= bottom; i++, row++) {
        int selected = (i == app->settings_sel);
        int danger = (i == ACTION_DELETE);

        if (selected) { attron(COLOR_PAIR(CP_TAB_ACTIVE) | A_BOLD); mvhline(row, 0, ' ', COLS); }
        else if (danger) attron(COLOR_PAIR(CP_ERROR));

        mvprintw(row, 3, "%s", ACTION_LABEL[i]);

        if (selected) attroff(COLOR_PAIR(CP_TAB_ACTIVE) | A_BOLD);
        else if (danger) attroff(COLOR_PAIR(CP_ERROR));
    }
}

/* ---------- actions ---------- */

/* Same OTP dance as the web app's reset: code to the address on file,
 * verify it, then set the new password twice. */
static void change_password(app_t *app) {
    const char *email = app->state.user.email;

    if (!ui_confirm(app, "Email a one-time code to your address?", 0)) {
        app_set_status(app, "Cancelled.");
        return;
    }

    app_set_status(app, "Sending code...");
    ui_draw(app);
    refresh();

    if (api_send_otp(email) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        ui_modal_error(app, app->status);
        return;
    }

    char otp[64];
    if (!ui_prompt(app, "Change password", "Code emailed to you:", 0, otp, sizeof(otp))) {
        app_set_status(app, "Cancelled.");
        return;
    }

    app_set_status(app, "Checking code...");
    ui_draw(app);
    refresh();

    if (api_verify_otp(email, otp) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        ui_modal_error(app, app->status);
        return;
    }

    char pw1[256], pw2[256];
    if (!ui_prompt(app, "Change password", "New password:", 1, pw1, sizeof(pw1))) {
        app_set_status(app, "Cancelled.");
        return;
    }
    if (!ui_prompt(app, "Change password", "Again, to confirm:", 1, pw2, sizeof(pw2))) {
        memset(pw1, 0, sizeof(pw1));
        app_set_status(app, "Cancelled.");
        return;
    }

    if (strcmp(pw1, pw2) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_error(app, "those did not match -- nothing changed");
        ui_modal_error(app, app->status);
        return;
    }

    app_set_status(app, "Setting password...");
    ui_draw(app);
    refresh();

    int rc = api_reset_password(email, pw1, pw2);
    memset(pw1, 0, sizeof(pw1));
    memset(pw2, 0, sizeof(pw2));

    if (rc != 0) {
        app_set_error(app, "%s", api_get_last_error());
        ui_modal_error(app, app->status);
        return;
    }

    app_set_status(app, "Password changed.");
}

static void do_logout(app_t *app) {
    if (!ui_confirm(app, "Log out of this device? The CLI tools keep "
                         "their own sessions and stay signed in.", 0)) {
        app_set_status(app, "Still logged in.");
        return;
    }

    app_set_status(app, "Logging out...");
    ui_draw(app);
    refresh();

    api_logout();                 /* best effort; the local token is what matters */
    app_clear_session_files();
    ss_state_clear(&app->state);
    api_set_jwt("");

    app->logged_out = 1;          /* main decides whether to show the login screen */
}

static void delete_account(app_t *app) {
    char pw1[256], pw2[256];

    if (!ui_prompt(app, "Delete account", "Your password:", 1, pw1, sizeof(pw1))) {
        app_set_status(app, "Cancelled.");
        return;
    }
    if (!ui_prompt(app, "Delete account", "Again, to confirm:", 1, pw2, sizeof(pw2))) {
        memset(pw1, 0, sizeof(pw1));
        app_set_status(app, "Cancelled.");
        return;
    }

    if (strcmp(pw1, pw2) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_error(app, "those did not match -- nothing deleted");
        ui_modal_error(app, app->status);
        return;
    }

    /* Third gate, after the two matching entries. */
    if (!ui_confirm(app, "Delete your account and everything in it? "
                         "This cannot be undone.", 1)) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_status(app, "Not deleted.");
        return;
    }

    app_set_status(app, "Deleting account...");
    ui_draw(app);
    refresh();

    int rc = api_delete_account(pw1);
    memset(pw1, 0, sizeof(pw1));
    memset(pw2, 0, sizeof(pw2));

    if (rc != 0) {
        app_set_error(app, "%s", api_get_last_error());
        ui_modal_error(app, app->status);
        return;
    }

    app_clear_session_files();
    ss_state_clear(&app->state);

    /* Nothing left to show. */
    app->quit = 1;
    app->account_deleted = 1;
}

static void blocked_people(app_t *app) {
    static api_users_result_t res;
    if (api_get_blocked_users(&res) != 0) { app_set_error(app, "%s", api_get_last_error()); ui_modal_error(app, app->status); return; }
    if (res.count == 0) { ui_message(app, "Blocked people", "You haven't blocked anyone."); return; }
    const char *opts[64];
    int n = res.count < 64 ? res.count : 64;
    for (int i = 0; i < n; i++) opts[i] = res.users[i].email;
    int pick = ui_choose(app, "Blocked people (enter: unblock)", opts, n);
    if (pick < 0) return;
    char q[320];
    snprintf(q, sizeof(q), "Unblock %.250s?", res.users[pick].email);
    if (!ui_confirm(app, q, 0)) return;
    if (api_unblock_user(res.users[pick].id) != 0) { app_set_error(app, "%s", api_get_last_error()); ui_modal_error(app, app->status); return; }
    app->feed.fetched = 0;
    app->users_fetched = 0;
    app_set_status(app, "Unblocked %s.", res.users[pick].email);
}

static void devices(app_t *app) {
    api_sessions_t ss;
    if (api_get_sessions(&ss) != 0) { app_set_error(app, "%s", api_get_last_error()); ui_modal_error(app, app->status); return; }
    static char labels[33][200];
    const char *opts[33];
    int n = 0;
    for (int i = 0; i < ss.count && n < 32; i++, n++) {
        char last[64];
        timefmt_full(ss.sessions[i].last_used_at, last, sizeof(last));
        snprintf(labels[n], sizeof(labels[n]), "%.90s%s  last active %s", ss.sessions[i].device_name,
                 ss.sessions[i].is_current ? " (this device)" : "", last);
        opts[n] = labels[n];
    }
    snprintf(labels[n], sizeof(labels[n]), "Sign out all other devices");
    opts[n] = labels[n];
    int pick = ui_choose(app, "Devices (enter: sign out)", opts, n + 1);
    if (pick < 0) return;
    if (pick == n) {
        int count = 0;
        if (!ui_confirm(app, "Sign out every other device?", 0)) return;
        if (api_revoke_all_other_sessions(&count) != 0) { app_set_error(app, "%s", api_get_last_error()); ui_modal_error(app, app->status); return; }
        app_set_status(app, "Signed out %d other device(s).", count);
        return;
    }
    if (!ui_confirm(app, ss.sessions[pick].is_current ? "Sign out this device? (This logs you out here.)" : "Sign out that device?", 0)) return;
    int was_current = 0;
    if (api_revoke_session(ss.sessions[pick].id, &was_current) != 0) { app_set_error(app, "%s", api_get_last_error()); ui_modal_error(app, app->status); return; }
    if (was_current) {
        app_clear_session_files();
        ss_state_clear(&app->state);
        api_set_jwt("");
        app->logged_out = 1;
        return;
    }
    app_set_status(app, "That device is signed out.");
}

static void terms(app_t *app) {
    api_me_t me;
    if (api_get_me(&me) != 0) { app_set_error(app, "%s", api_get_last_error()); ui_modal_error(app, app->status); return; }
    char msg[512];
    snprintf(msg, sizeof(msg), "Read the Terms of Use at %s\n\nYou have accepted version %d; the current one is %d.",
             me.terms_url, me.terms_accepted, me.terms_current);
    ui_message(app, "Terms of Use", msg);
}

/* Invite lives on the Me tab (i); this runs the same thing from here. */
void app_invite(app_t *app);

void settings_activate(app_t *app) {
    switch (app->settings_sel) {
        case 0: app_invite(app);       break;
        case 1: blocked_people(app);   break;
        case 2: devices(app);          break;
        case 3: terms(app);            break;
        case 4: change_password(app);  break;
        case 5: do_logout(app);        break;
        case 6: delete_account(app);   break;
        default: break;
    }
}
