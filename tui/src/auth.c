#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include "auth.h"
#include "ui.h"
#include "ss_api.h"
#include "ss_state.h"

#define AUTH_CHOICES 4

static const char *CHOICE[AUTH_CHOICES] = {
    "Log in",
    "Register",
    "Forgot password",
    "Quit"
};

static void busy(app_t *app, const char *msg) {
    app_set_status(app, "%s", msg);
    ui_draw(app);
    refresh();
}

void auth_draw(app_t *app) {
    int sel = app->auth_sel;
    erase();

    int top = (LINES - 12) / 2;
    if (top < 1) top = 1;

    const char *title = "Simple Social";
    int tw = ui_utf8_width(title);
    int left = (COLS - tw) / 2;
    if (left < 0) left = 0;

    attron(COLOR_PAIR(CP_PRIMARY) | A_BOLD);
    mvaddstr(top, left, title);
    attroff(COLOR_PAIR(CP_PRIMARY) | A_BOLD);

    attron(A_DIM);
    const char *sub = "no session -- this tool signs in separately from the CLI tools";
    int sw = ui_utf8_width(sub);
    int sl = (COLS - sw) / 2;
    if (sl < 0) sl = 0;
    mvaddstr(top + 1, sl, sub);
    attroff(A_DIM);

    for (int i = 0; i < AUTH_CHOICES; i++) {
        int row = top + 3 + i;
        if (row >= LINES - 3) break;  /* leaves room for the footer rule + hints + status rows */

        int w = ui_utf8_width(CHOICE[i]);
        int x = (COLS - w) / 2;
        if (x < 2) x = 2;

        if (i == sel) {
            attron(COLOR_PAIR(CP_TAB_ACTIVE) | A_BOLD);
            mvhline(row, x - 2, ' ', w + 4);
        }
        mvaddstr(row, x, CHOICE[i]);
        if (i == sel) attroff(COLOR_PAIR(CP_TAB_ACTIVE) | A_BOLD);
    }

    /* Rule above the footer text, mirroring the web footer's border-top --
     * matches the tab-bar chrome the rest of the app uses. Hints get their
     * own row so a status/error message never covers them (see ui.c's
     * draw_status for why -- same fix applied here for consistency). */
    attron(COLOR_PAIR(CP_PRIMARY) | A_DIM);
    mvhline(LINES - 3, 0, ACS_HLINE, COLS);
    attroff(COLOR_PAIR(CP_PRIMARY) | A_DIM);

    attron(A_DIM | COLOR_PAIR(CP_PRIMARY));
    mvaddstr(LINES - 2, 0, "j/k:move   enter:choose   q:quit");
    attroff(A_DIM | COLOR_PAIR(CP_PRIMARY));

    if (app->status[0]) {
        char buf[256];
        ui_utf8_take(buf, sizeof(buf), app->status, COLS - 2, 1);
        if (app->status_is_error) attron(COLOR_PAIR(CP_ERROR) | A_BOLD);
        mvaddstr(LINES - 1, 0, buf);
        if (app->status_is_error) attroff(COLOR_PAIR(CP_ERROR) | A_BOLD);
    }

    /* Stages only -- doesn't flush (see ui_draw()'s comment in ui.c for
     * why). Callers that draw nothing else on top call doupdate()
     * themselves; ui_prompt()'s loop, called while still on this screen,
     * stages its own box on top and flushes both together. */
    wnoutrefresh(stdscr);
}

/* Adopts a fresh session: store both tokens and learn who we are. Every
 * way of arriving at a new login - signing in, registering, resetting a
 * password - comes through here, so the refresh token is saved once rather
 * than at each call site. */
static int adopt_session(app_t *app, const char *jwt) {
    ss_state_set_jwt(&app->state, jwt);
    api_set_jwt(jwt);
    ss_state_save_jwt(&app->state);

    /* Without this the login lasts only as long as the access token. */
    ss_state_set_refresh(&app->state, api_get_refresh_token());
    ss_state_save_refresh(&app->state);

    int id = 0;
    char email[256] = {0}, created[32] = {0};
    if (api_get_my_info(&id, email, sizeof(email), created, sizeof(created)) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return -1;
    }

    ss_state_set_user(&app->state, id, email, created);
    api_set_user_id(id);
    ss_state_save_user(&app->state);
    return 0;
}

/* After any sign-in (and at start-up with a saved session): the one-time
 * welcome, then the Terms of Use if they changed since this account accepted
 * them. Declining logs out. Returns 1 while still signed in. */
int auth_after_login(app_t *app) {
    api_me_t me;
    if (api_get_me(&me) != 0) return 1;   /* not worth blocking the app over */

    if (me.has_welcome) {
        char msg[512];
        if (me.welcome_inviter[0] && me.welcome_inviter_is_owner)
            snprintf(msg, sizeof(msg), "You were invited by %s, who runs Simple Social.", me.welcome_inviter);
        else if (me.welcome_inviter[0])
            snprintf(msg, sizeof(msg), "%s used their only invite on you, so use yours just as wisely.", me.welcome_inviter);
        else
            snprintf(msg, sizeof(msg), "Glad you're here.");
        ui_message(app, "Welcome to Simple Social!", msg);
        api_dismiss_welcome();
    }

    if (me.terms_current > 0 && me.terms_accepted < me.terms_current) {
        char q[512];
        snprintf(q, sizeof(q), "The Terms of Use have changed. Read them at %s -- "
                 "I agree to them and I'm at least 13 years old.", me.terms_url);
        if (!ui_confirm(app, q, 0) || api_accept_terms(me.terms_current) != 0) {
            api_logout();
            app_clear_session_files();
            ss_state_clear(&app->state);
            api_set_jwt("");
            app_set_error(app, "The Terms of Use weren't accepted, so you've been logged out.");
            return 0;
        }
        app_set_status(app, "Thank you. The Terms of Use are accepted.");
    }
    return 1;
}

static int do_login(app_t *app) {
    char email[256], pw[256];

    if (!ui_prompt(app, "Log in", "Email:", 0, email, sizeof(email))) return 0;
    if (!ui_prompt(app, "Log in", "Password:", 1, pw, sizeof(pw))) return 0;

    busy(app, "Logging in...");

    char jwt[STATE_MAX_JWT] = {0};
    int user_id = 0;
    int rc = api_login(email, pw, jwt, sizeof(jwt), &user_id);
    memset(pw, 0, sizeof(pw));

    if (rc != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    if (adopt_session(app, jwt) != 0) return 0;

    app_set_status(app, "Logged in as %s.", app->state.user.email);
    return 1;
}

/* Register, code first as on the website: the invite code (and who made it),
 * the age-and-terms question, the email and its code, then the password. */
static int do_register(app_t *app) {
    char code[64], inviter[256] = {0}, expires[32] = {0};
    char email[256], otp[64], pw1[256], pw2[256];

    if (!ui_prompt(app, "Register", "Invite code:", 0, code, sizeof(code))) return 0;

    busy(app, "Checking the invite code...");
    if (api_check_invite_code(code, inviter, sizeof(inviter), expires, sizeof(expires)) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    char terms_url[256], q[600];
    api_site_url("/terms", terms_url, sizeof(terms_url));
    snprintf(q, sizeof(q), "Invited by %s. I agree to the Terms of Use (%s) and I'm at least 13 years old.",
             inviter[0] ? inviter : "a member", terms_url);
    if (!ui_confirm(app, q, 0)) {
        app_set_error(app, "You need to agree to the Terms of Use and be at least 13 to join.");
        return 0;
    }

    if (!ui_prompt(app, "Register", "Email:", 0, email, sizeof(email))) return 0;

    busy(app, "Sending code...");
    if (api_register_send_otp(email, code) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    if (!ui_prompt(app, "Register", "Code emailed to you:", 0, otp, sizeof(otp))) return 0;

    busy(app, "Checking code...");
    if (api_register_verify_otp(email, otp) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    if (!ui_prompt(app, "Register", "Choose a password:", 1, pw1, sizeof(pw1))) return 0;
    if (!ui_prompt(app, "Register", "Again, to confirm:", 1, pw2, sizeof(pw2))) {
        memset(pw1, 0, sizeof(pw1));
        return 0;
    }

    if (strcmp(pw1, pw2) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_error(app, "those did not match -- start again");
        return 0;
    }

    busy(app, "Creating account...");
    int rc = api_register_finish(email, pw1, pw2);
    if (rc != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    /* Registered, so log straight in with what they just chose. */
    busy(app, "Logging in...");
    char jwt[STATE_MAX_JWT] = {0};
    int user_id = 0;
    rc = api_login(email, pw1, jwt, sizeof(jwt), &user_id);
    memset(pw1, 0, sizeof(pw1));
    memset(pw2, 0, sizeof(pw2));

    if (rc != 0) {
        app_set_error(app, "account created, but logging in failed: %s",
                      api_get_last_error());
        return 0;
    }

    if (adopt_session(app, jwt) != 0) return 0;

    /* They agreed to the terms a moment ago: record it. */
    api_me_t me;
    if (api_get_me(&me) == 0 && me.terms_current > 0) api_accept_terms(me.terms_current);

    app_set_status(app, "Welcome, %s.", app->state.user.email);
    return 1;
}

static int do_reset(app_t *app) {
    char email[256], otp[64], pw1[256], pw2[256];

    if (!ui_prompt(app, "Forgot password", "Email:", 0, email, sizeof(email))) return 0;

    busy(app, "Sending code...");
    if (api_send_otp(email) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    if (!ui_prompt(app, "Forgot password", "Code emailed to you:", 0, otp, sizeof(otp)))
        return 0;

    busy(app, "Checking code...");
    if (api_verify_otp(email, otp) != 0) {
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    if (!ui_prompt(app, "Forgot password", "New password:", 1, pw1, sizeof(pw1))) return 0;
    if (!ui_prompt(app, "Forgot password", "Again, to confirm:", 1, pw2, sizeof(pw2))) {
        memset(pw1, 0, sizeof(pw1));
        return 0;
    }

    if (strcmp(pw1, pw2) != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_error(app, "those did not match -- start again");
        return 0;
    }

    busy(app, "Setting password...");
    int rc = api_reset_password(email, pw1, pw2);
    if (rc != 0) {
        memset(pw1, 0, sizeof(pw1));
        memset(pw2, 0, sizeof(pw2));
        app_set_error(app, "%s", api_get_last_error());
        return 0;
    }

    busy(app, "Logging in...");
    char jwt[STATE_MAX_JWT] = {0};
    int user_id = 0;
    rc = api_login(email, pw1, jwt, sizeof(jwt), &user_id);
    memset(pw1, 0, sizeof(pw1));
    memset(pw2, 0, sizeof(pw2));

    if (rc != 0) {
        app_set_error(app, "password changed, but logging in failed: %s",
                      api_get_last_error());
        return 0;
    }

    if (adopt_session(app, jwt) != 0) return 0;

    app_set_status(app, "Logged in as %s.", app->state.user.email);
    return 1;
}

int auth_screen(app_t *app) {
    int sel = 0;

    /* ui_prompt calls ui_draw, which draws the tab chrome, so the menu is
     * repainted after every prompt rather than relying on ui_draw. */
    for (;;) {
        app->auth_sel = sel;
        auth_draw(app);
        doupdate();   /* auth_draw only stages the frame; nothing else follows here */

        int ch = getch();
        if (ch == ERR) continue;

        if (app->status[0] && !app->status_is_error) app->status[0] = '\0';

        switch (ch) {
            case 'q':
                return 0;

            case 'j': case KEY_DOWN: if (sel < AUTH_CHOICES - 1) sel++; break;
            case 'k': case KEY_UP:   if (sel > 0) sel--; break;

            case '\r': case '\n': case KEY_ENTER: {
                int done = 0;
                switch (sel) {
                    case 0: done = do_login(app);    break;
                    case 1: done = do_register(app); break;
                    case 2: done = do_reset(app);    break;
                    case 3: return 0;
                }
                if (done && auth_after_login(app)) return 1;
                break;
            }

            case KEY_RESIZE:
            default:
                break;
        }
    }
}
