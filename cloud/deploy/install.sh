#!/usr/bin/env bash
# Run as root with an uploaded, credential-free release archive.
set -euo pipefail
archive=${1:?release archive required}
release_id=${2:?release ID required}
[[ "$release_id" =~ ^[A-Za-z0-9._-]+$ ]] || exit 2
base=/opt/rice-cloud
release="$base/releases/$release_id"
test ! -e "$release" || { echo 'Release already exists; refusing to overwrite'; exit 2; }
id rice-cloud >/dev/null 2>&1 || useradd --system --home /var/lib/rice-cloud --shell /usr/sbin/nologin rice-cloud
install -d -m 755 "$base/releases"
install -d -m 700 /etc/rice-cloud
install -d -o rice-cloud -g rice-cloud -m 700 /var/lib/rice-cloud /var/backups/rice-cloud
mkdir "$release"
python3 - "$archive" "$release" <<'PY'
import sys, tarfile
with tarfile.open(sys.argv[1]) as archive:
    archive.extractall(sys.argv[2], filter='data')
PY
test -f "$release/server/app.py"
test -f /etc/rice-cloud/server.env || { echo 'Private server.env must be provisioned before activation'; exit 2; }
if ! python3 -c 'import ensurepip' >/dev/null 2>&1; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y python3-venv
fi
python3 -m venv "$base/venv"
"$base/venv/bin/pip" install -r "$release/server/requirements.txt"
chown -R root:root "$release"
chmod -R a+rX "$release"
chmod 600 /etc/rice-cloud/server.env
install -m 644 "$release/deploy/rice-cloud.service" /etc/systemd/system/rice-cloud.service
install -m 644 "$release/deploy/rice-cloud-backup.service" /etc/systemd/system/rice-cloud-backup.service
install -m 644 "$release/deploy/rice-cloud-backup.timer" /etc/systemd/system/rice-cloud-backup.timer
previous=''
if [[ -L "$base/current" ]]; then
    candidate=$(readlink -f "$base/current" || true)
    if [[ "$candidate" == "$base/releases/"* && -d "$candidate" ]]; then previous="$candidate"; fi
fi
caddy_backup="/etc/caddy/Caddyfile.before-rice-$release_id"
cp -a /etc/caddy/Caddyfile "$caddy_backup"
rice_site_backup="/etc/caddy/rice-site.before-$release_id"
if [[ -f /etc/caddy/rice-cloud/site.caddy ]]; then cp -a /etc/caddy/rice-cloud/site.caddy "$rice_site_backup"; fi
restore() {
    cp -a "$caddy_backup" /etc/caddy/Caddyfile
    if [[ -f "$rice_site_backup" ]]; then cp -a "$rice_site_backup" /etc/caddy/rice-cloud/site.caddy; fi
    caddy validate --config /etc/caddy/Caddyfile && systemctl reload caddy || true
    if [[ -n "$previous" ]]; then
        ln -sfn "$previous" "$base/current-rollback"
        mv -Tf "$base/current-rollback" "$base/current"
        systemctl restart rice-cloud || true
    else
        systemctl stop rice-cloud || true
        if [[ -L "$base/current" ]]; then unlink "$base/current"; fi
    fi
}
trap restore ERR
ln -sfn "$release" "$base/current-new"
mv -Tf "$base/current-new" "$base/current"
systemctl daemon-reload
systemctl restart rice-cloud
for attempt in $(seq 1 30); do
    if curl --fail --silent http://127.0.0.1:8796/api/health >/dev/null; then break; fi
    sleep 1
done
curl --fail --silent http://127.0.0.1:8796/api/health >/dev/null
# Keep an independently managed site block so later updates are unambiguous.
install -d -m 755 /etc/caddy/rice-cloud
install -m 644 "$release/deploy/Caddyfile.rice" /etc/caddy/rice-cloud/site.caddy
if ! grep -Fxq 'import /etc/caddy/rice-cloud/site.caddy' /etc/caddy/Caddyfile; then
    if grep -q 'admin.riceplant.achantalabs.com' /etc/caddy/Caddyfile; then
        echo 'Existing rice hostname configuration requires inspection'; false
    fi
    printf '\nimport /etc/caddy/rice-cloud/site.caddy\n' >> /etc/caddy/Caddyfile
fi
caddy validate --config /etc/caddy/Caddyfile
systemctl reload caddy
systemctl enable rice-cloud
systemctl start rice-cloud-backup.service
systemctl enable --now rice-cloud-backup.timer
trap - ERR
echo "Rice cloud service installed: $release_id. Verify public HTTPS before flashing devices."
