#!/usr/bin/env bash
# Consistent SQLite backup (safe while the backend runs). Keep 14 days.
# Cron (as user nodus):  15 2 * * *  /opt/nodus-ai/deploy/backup.sh
set -euo pipefail
DB=/opt/nodus-ai/backend/nodus.db
DEST=/var/backups/nodus
mkdir -p "$DEST"
sqlite3 "$DB" ".backup '$DEST/nodus-$(date +%F).db'"
find "$DEST" -name 'nodus-*.db' -mtime +14 -delete
