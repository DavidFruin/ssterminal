# ssterminal

The terminal clients for [Simple Social](https://github.com/DavidFruin/ssapi), written in C, in one repo.

| Folder | Binary | PATH name | What |
|---|---|---|---|
| `cli/` | `simple-social-cli` | `sscli` | Scriptable, argv-style commands, with `--json` output |
| `wizard/` | `simple-social-cli-interactive` | `sswiz` | Wizard-style interactive prompt |
| `tui/` | `simple-social-tui` | `sstui` | Full-screen ncurses UI |
| `lib/` | `libss.a` | | The shared API library (static) all three link |

All three read the same server API. Config lives in `~/.config/simple-social-cli/config.ini`; session state is under `~/.simple-social-cli/`.

## Build and install

Install a compiler, the ncurses headers and the libcurl runtime. On Debian or Ubuntu:

```
sudo apt install build-essential libncurses-dev libcurl4 pkg-config
```

On Arch, `sudo pacman -S base-devel ncurses curl`. The libcurl dev package is not needed, because its headers are vendored in `vendor/include`.

```
git clone https://github.com/DavidFruin/ssterminal.git
cd ssterminal
make                      # all three; or: make cli | make wizard | make tui
sudo make install         # all three; or install-cli | install-wizard | install-tui
make PREFIX=$HOME/.local install   # no sudo needed
sudo make uninstall
```

Clone somewhere you can write to, such as your home directory, not `/`.

## Updating from the old repos

`simple-social-cli`, `simple-social-cli-interactive` and `simple-social-tui` are archived and replaced by this repo, with full history. The binaries and PATH names are unchanged, so existing config and sessions keep working. Clone this repo and run `sudo make install`.

## Joining

Simple Social is invite-only. Ask a member for an invite code, then register from any of the three tools: the TUI's login screen, `register` in the wizard, or

```
sscli register <invite code> <email> <password> <confirm>
```

It checks the code and shows who invited you, asks you to accept the Terms of Use and confirm you are at least 13, emails you a 6-digit code to type in, and logs you in.

Times are shown in UTC, followed by the writer's offset from GMT when the server knows it: `UTC 2026-11-16 15:51 (GMT-06:00)`.

## Tests

The tests start their own throwaway local server from an [ssapi](https://github.com/DavidFruin/ssapi) checkout, with seeded accounts and email written to a file. They never touch dev or prod.

```
make unit                                   # the JSON reader, no server needed
SSAPI=~/dev/ssapi ./tests/test_cli.sh       # the CLI, end to end
SSAPI=~/dev/ssapi python3 wizard/tests/test_wizard.py
SSAPI=~/dev/ssapi tests/bench.sh start      # a server to try the TUI against by hand
```

`tests/bench.sh` explains the seeded accounts. To run the CLI tests against another server instead, set `TEST_BASE_URL`, `TEST_EMAIL` and `TEST_PASSWORD` (only the basic cases run there).

See `ARCHITECTURE.md` for how the library and the clients fit together, and `tui/notes.md` for the TUI's working notes.
