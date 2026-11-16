#!/usr/bin/env bash
# Starts a throwaway local Simple Social server for testing the terminal apps.
# Nothing touches dev or prod: php -S on 127.0.0.1, its own SQLite database in
# a scratch folder, mail written to a file instead of being sent.
#
#   SSAPI=~/dev/ssapi tests/bench.sh start [dir]   # prints the base URL
#   tests/bench.sh stop [dir]
#
# Seeded accounts (password Passw0rd! for all): owner@test.local (the owner),
# alice@test.local, bob@test.local, carol@test.local. alice follows bob.
# Registration is invite-only, as on the real servers. The email codes land
# in <dir>/mail.log.
set -euo pipefail
CMD="${1:-start}"
DIR="${2:-${TMPDIR:-/tmp}/ssterminal-bench}"
PORT="${PORT:-8190}"

if [ "$CMD" = stop ]; then
  [ -f "$DIR/server.pid" ] && kill "$(cat "$DIR/server.pid")" 2>/dev/null || true
  rm -f "$DIR/server.pid"
  exit 0
fi

: "${SSAPI:?set SSAPI to an ssapi checkout}"
"$0" stop "$DIR"
rm -rf "$DIR"; mkdir -p "$DIR/private" "$DIR/public_html"
rsync -a --exclude .git --exclude vendor "$SSAPI/" "$DIR/ssapi/"
(cd "$DIR/ssapi" && composer dump-autoload -q)
cp "$SSAPI/deploy/public/api.php" "$SSAPI/deploy/public/media.php" "$DIR/public_html/"
printf 'JWT_SECRET=%s\nTERMS_VERSION=1\n' "$(openssl rand -hex 32)" > "$DIR/private/.env"
cat > "$DIR/seed.php" <<'PHPEOF'
<?php
function respond($d, $c = 200) { fwrite(STDERR, json_encode($d) . "\n"); exit(1); }
require __DIR__ . '/ssapi/config.php';
require __DIR__ . '/ssapi/schema.php';
$pdo = db();
$ins = $pdo->prepare("INSERT INTO users (email, password, posts, follows, followers, jwt, created_at, terms_version_accepted) VALUES (?, ?, '[]', '[]', '[]', '', datetime('now'), 1)");
foreach (['owner', 'alice', 'bob', 'carol'] as $n) $ins->execute(["$n@test.local", password_hash('Passw0rd!', PASSWORD_DEFAULT)]);
$pdo->exec("UPDATE users SET role = 'owner' WHERE email = 'owner@test.local'");
$pdo->exec("UPDATE users SET invited_by = 1 WHERE id > 1");
$pdo->exec("INSERT INTO follows (follower_id, followed_id, created_at) VALUES (2, 3, datetime('now'))");
PHPEOF
php "$DIR/seed.php"
: > "$DIR/mail.log"
nohup php -d sendmail_path="tee -a $DIR/mail.log >/dev/null" -S "127.0.0.1:$PORT" -t "$DIR/public_html" >"$DIR/server.log" 2>&1 &
echo $! > "$DIR/server.pid"
for _ in $(seq 50); do curl -s "http://127.0.0.1:$PORT/api.php" >/dev/null && break; sleep 0.1; done
echo "http://127.0.0.1:$PORT"
