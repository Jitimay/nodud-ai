# Nodus AI — Backend deployment

How to run the backend on a public server with HTTPS, so gateways anywhere can reach it.
For local development see `backend/README.md`.

## 1. Server

- Any small Linux VPS (1 vCPU, 1 GB RAM is plenty for SQLite + a few gateways).
- Debian 12 / Ubuntu 24.04 commands are shown. Python 3.12+.
- A domain name, e.g. `api.example.org`, with a DNS **A record** pointing to the server's IP.
- Firewall: open 22 (SSH), 80 and 443 (HTTP/HTTPS) only. Port 8000 stays private.

```bash
sudo apt update && sudo apt install -y python3 python3-venv git sqlite3 caddy
sudo useradd --system --create-home --home-dir /opt/nodus-ai --shell /usr/sbin/nologin nodus
```

## 2. Install the backend

```bash
sudo -u nodus git clone <your-repo-url> /opt/nodus-ai      # or copy the folder with rsync
cd /opt/nodus-ai/backend
sudo -u nodus python3 -m venv .venv
sudo -u nodus .venv/bin/pip install -r requirements.txt
sudo -u nodus cp .env.example .env
sudo -u nodus chmod 600 .env
sudo -u nodus nano .env
```

Fill in `.env`:

| Variable | Value |
|---|---|
| `GROQ_API_KEY` | from https://console.groq.com/keys |
| `GROQ_MODEL` | `qwen/qwen3.8-27b` (or another model you tested) |
| `GATEWAY_TOKENS` | `tcall-01:<token>` per gateway. Generate: `python3 -c "import secrets; print(secrets.token_urlsafe(32))"` |
| `ADMIN_TOKEN` | another generated token, for `/v1/admin/stats` |

Check it starts (migrations run automatically on start):

```bash
sudo -u nodus .venv/bin/python -m pytest -q
sudo -u nodus .venv/bin/uvicorn app.main:app --port 8000   # Ctrl+C after "backend up"
```

## 3. Run as a service

```bash
sudo cp /opt/nodus-ai/deploy/nodus-backend.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nodus-backend
systemctl status nodus-backend
journalctl -u nodus-backend -f      # live logs (phone numbers are masked)
```

The service restarts automatically on crash and at boot.

## 4. HTTPS with Caddy

```bash
sudo cp /opt/nodus-ai/deploy/Caddyfile /etc/caddy/Caddyfile
sudo sed -i 's/api.example.org/YOUR.DOMAIN/' /etc/caddy/Caddyfile
sudo systemctl reload caddy
curl https://YOUR.DOMAIN/health        # {"status":"ok"}
```

Caddy obtains and renews the Let's Encrypt certificate by itself.

### Gateway side (certificate pinning)

Gateways verify the server against the **ISRG Root X1** root certificate (Let's Encrypt's root, valid
until 2035). Pinning the root instead of the server certificate means the 90-day renewals never
break the gateways.

1. Download https://letsencrypt.org/certs/isrgrootx1.pem
2. In the gateway's serial console: type `ca`, paste the whole PEM, press Enter.
3. `set api_url https://YOUR.DOMAIN`, then `reboot`.

Never use `tls_insecure 1` in production: it disables the server check.

## 5. Backups

```bash
sudo mkdir -p /var/backups/nodus && sudo chown nodus /var/backups/nodus
sudo -u nodus crontab -e
# add:
15 2 * * *  /opt/nodus-ai/deploy/backup.sh
```

Restore: stop the service, copy a backup over `/opt/nodus-ai/backend/nodus.db`, start the service.
Copy backups off the server too (e.g. `rsync` to another machine): a backup on the same disk
does not survive a disk failure.

## 6. Monitoring

```bash
curl -s -H "Authorization: Bearer $ADMIN_TOKEN" https://YOUR.DOMAIN/v1/admin/stats | python3 -m json.tool
```

Shows each gateway (online/offline, signal, modem supply voltage, counters), last-24h message counts
by status, and pending replies.

**Alerting:** point a free uptime monitor (e.g. UptimeRobot, Healthchecks.io) at
`https://YOUR.DOMAIN/health`. For gateway-offline alerts, a monitor that checks
`/v1/admin/stats` for `"online": false` (keyword monitor with the admin header) works.

What to watch:

| Signal | Meaning |
|---|---|
| gateway `online: false` | Gateway lost power, Wi-Fi, or crashed |
| `modem_supply_mv` < 3600 | Power problem: battery failing or supply too weak |
| `gsm_csq` < 10 | Weak GSM signal: move the antenna/gateway |
| many `ai_error` | Groq problem: check key, quota, `journalctl` |
| `outbox_pending` growing | Gateway receives but cannot send SMS (credit? network?) |

## 7. Updating

```bash
cd /opt/nodus-ai && sudo -u nodus git pull
sudo -u nodus backend/.venv/bin/pip install -r backend/requirements.txt
sudo systemctl restart nodus-backend     # applies new migrations automatically
```

## 8. Rotating secrets

- **Groq key:** create a new key in the Groq console, put it in `.env`, restart, delete the old key.
- **Gateway token:** generate a new token, replace it in `GATEWAY_TOKENS`, restart the backend,
  then on the gateway console `set token <new>` and `reboot`. Received SMS stay queued on the
  gateway in the meantime, so nothing is lost during the switch.
- **Stolen/lost gateway:** remove its entry from `GATEWAY_TOKENS` and restart. It can no longer connect.

## 9. Moving to PostgreSQL (when you outgrow SQLite)

1. `sudo apt install postgresql`, create a database and user.
2. `.venv/bin/pip install "psycopg[binary]"`
3. `DATABASE_URL=postgresql+psycopg://nodus:PASSWORD@localhost/nodus` in `.env`.
4. Restart: migrations create the schema. (Copy old data with a small script if needed.)
5. Then you may raise uvicorn `--workers`.
