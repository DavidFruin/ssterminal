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

## Tests

```
TEST_EMAIL=you@example.com TEST_PASSWORD='...' TEST_BASE_URL=http://127.0.0.1:8080 make test
```

They create and delete real posts, so point `TEST_BASE_URL` at a local bench or dev, never prod (the default is dev).

See `ARCHITECTURE.md` for how the library and the clients fit together, and `tui/notes.md` for the TUI's working notes.
