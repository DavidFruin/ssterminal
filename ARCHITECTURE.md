# ssterminal: how it fits together

Three terminal front ends for Simple Social over one shared C library. They talk to the same `api.php` / `media.php` as the web and phone apps and hold no business logic of their own: the server decides, the clients show.

## Layout

```
ssterminal/
├── Makefile              one build for everything; `make unit`, `make test`
├── vendor/include/curl/  vendored libcurl headers (only the runtime libcurl.so.4 is needed)
├── lib/ -> lib/libss.a   the shared library, linked statically into all three
│   ├── ss_api.c/.h       every server call, the parsed structs, times, upload checks
│   ├── ss_json.c/.h      a small reader: each object's own keys only, strings decoded
│   ├── ss_config.c/.h    base_url etc. from ~/.config/simple-social-cli/config.ini (default: prod)
│   ├── ss_state.c/.h     tokens per app under ~/.simple-social-cli/<cli|wiz|tui>/
│   └── ss_utils.c/.h     url_encode and string helpers
├── cli/                  sscli: argv commands, --json output (main.c, output.c)
├── wizard/src/           sswiz: a prompt that asks for each field; prints with cli/output.c
├── tui/src/              sstui: ncurses (app.c keys, ui.c drawing, detail.c post view,
│                         net.c the calls, auth.c login/register, settings.c)
└── tests/                test_json.c, test_cli.sh, bench.sh (a local server)
```

## The library

**Requests.** `request()` POSTs `action=...&params` to the configured `api.php` and reads the whole answer into a heap buffer (nothing is cut off at a fixed size). Every request sends:
- `Authorization: Bearer <jwt>` when signed in. A 401 renews the session once with the refresh token and retries.
- `X-SS-TZ: -06:00`, this computer's offset from GMT. The server stores it with whatever you create.
- A `User-Agent` naming the tool, which the server turns into the device name.

The clients speak API version 1 (no `X-SS-Api` header): the refresh token doesn't rotate for them, and pages still carry `totalCount`, which `sscli --json` prints.

**Errors.** `api_get_last_error()` is the server's own message; `api_get_last_status()` the HTTP status; `api_get_last_code()` the server's code (`media_quota`).

**Verify token.** Password reset and registration: the token the server returns when a one-time code is entered is kept per email inside the library and sent with the last step. Both steps must happen in the same process.

**Parsing.** Posts, comments (flat or threaded; threaded lists come back flattened, each reply after its comment with `parent_id` set), notifications, users, invites, sessions, media limits. Mentions (`@[id]`) are replaced with `@email`. Likes: `likeCount`/`likedByMe` when the server sends them, otherwise counted from the `likes` list.

**Times.** The server's times are UTC. `api_format_time()` gives `UTC 2026-11-16 15:51:00 (GMT-06:00)`; the TUI's `timefmt.c` does the same in its own short forms.

## What each app adds

- **sscli:** one command per action; `--json` prints bare objects (fields are only ever added, never renamed or removed: scripts and the agent board depend on them). Notices (terms, welcome) go to stderr only.
- **sswiz:** the same actions as prompts, one field at a time, with confirmations for reports, blocks and invites.
- **sstui:** tabs (Feed, Notifications, Users, Me, Settings), a post view with threads, modals for every choice. Press `?` in it for the keys.

All three: code-first registration with the age-and-terms question, the one-time welcome, the terms prompt when the terms change, folded reported content, "Hidden by a moderator" on your own frozen content, replies, report and block, invites, devices, upload checks.

Not in the terminal (by design): staff tools (moderation is on the website), push notifications, themes, languages other than English.
