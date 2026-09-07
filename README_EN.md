# CrossDesk Server

Device registration, presence, remote-session signaling, and temporary TURN credentials for [CrossDesk](https://github.com/kunkundi/crossdesk), with a built-in Web admin dashboard. Signaling uses WSS; device records and duration statistics are persisted in SQLite.

[中文](README.md) · [Release files](https://github.com/kunkundi/crossdesk-server/releases) · [Docker images](https://hub.docker.com/r/crossdesk/crossdesk-server/tags) · [Desktop client](https://github.com/kunkundi/crossdesk) · [Web client](https://github.com/kunkundi/crossdesk-web-client)

[![Build](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml/badge.svg)](https://github.com/kunkundi/crossdesk-server/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/kunkundi/crossdesk-server)](https://github.com/kunkundi/crossdesk-server/releases)
[![Docker Pulls](https://img.shields.io/docker/pulls/crossdesk/crossdesk-server)](https://hub.docker.com/r/crossdesk/crossdesk-server)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue)](LICENSE)

[Deployment](#run-services) · [Client setup](#clients) · [Admin dashboard](#admin) · [Status API](#stats) · [Maintenance](#operations) · [Build](#build)

## Components and capabilities

| Component | Responsibility |
| --- | --- |
| CrossDesk Server | WSS signaling, device identities and password verification, presence, session coordination, and temporary TURN credentials |
| Coturn | Relays media and data when clients cannot connect directly; runs in a separate container |
| SQLite | Stores device records, online/control durations, and session information |
| Web admin dashboard | Metrics, client/session lists, search, filters, sorting, session disconnection, and optional IP geolocation |

Clients try P2P connections and use TURN when needed. The current [Compose deployment](compose.yaml) and [CI](.github/workflows/build.yml) target **Linux amd64 / arm64**. Release images use Ubuntu 22.04 as their runtime environment.

## Run services

### 1. Prepare the deployment files

Use a Linux server with **Docker Engine and the Compose plugin** installed, and confirm that `docker compose version` works. Compose uses host networking; `INTERNAL_IP` must be an address the host can bind.

Download **`compose.yaml`** and **`env.example`** from the same [GitHub Release](https://github.com/kunkundi/crossdesk-server/releases) into a dedicated deployment directory. Run there:

```bash
cp env.example .env
```

The release's `env.example` pins the corresponding image tag. The source repository instead provides [`.env.example`](.env.example), which uses `latest`; choose a fixed release tag when using that file. Run all subsequent Compose commands from the configuration directory.

### 2. Edit `.env`

Complete these values in a text editor and replace every example address. Public and private IPs usually differ on cloud hosts behind NAT. They can be identical when the public IP is assigned directly to the host's interface.

| Variable | Purpose / default |
| --- | --- |
| `CROSSDESK_IMAGE` | Fixed release tag matching the deployment files |
| `EXTERNAL_IP` | Public IP used for the default TURN endpoint and generated certificate |
| `INTERNAL_IP` | Local interface IP used by Coturn for listening and relaying |
| `CROSSDESK_SERVER_PORT` | Shared WSS / HTTPS port; Compose defaults to `9099` |
| `COTURN_PORT` | STUN / TURN port; defaults to `3478` |
| `MIN_PORT` / `MAX_PORT` | TURN media port range; defaults to `50000`–`60000` |
| `COTURN_AUTH_SECRET` | Signing secret shared by signaling and Coturn; never enter it in clients |
| `COTURN_STATELESS_NONCE_SECRET` | Separately generated, persistent nonce secret |
| `CROSSDESK_DATA_DIR` | Host data/certificate directory; defaults to `/var/lib/crossdesk` |
| `CROSSDESK_LOG_DIR` | Host application-log directory; defaults to `/var/log/crossdesk` |

Run the following twice and use the two different outputs for the two secret fields above:

```bash
openssl rand -hex 32
openssl rand -hex 32
```

Optional `COTURN_PUBLIC_HOST` advertises a separate TURN hostname/IP; an empty value uses `EXTERNAL_IP`. `COTURN_CREDENTIAL_TTL_SECONDS` defaults to `3600` seconds, with a range of `60`–`86400`. `COTURN_LOG_LEVEL` defaults to `warning`, and `COTURN_MEMORY_LIMIT` to `512m`. See the [environment example](.env.example) for all options.

The signaling server issues temporary TURN usernames/passwords during client login and session negotiation. Keep server shared secrets out of clients and Web pages. Check client compatibility when upgrading the server.

### 3. Open ports and start

Allow these ports through the host firewall and cloud security group:

| Port | Protocol | Purpose |
| --- | --- | --- |
| `CROSSDESK_SERVER_PORT`, default `9099` | TCP | WSS, status API, and admin dashboard |
| `COTURN_PORT`, default `3478` | TCP / UDP | STUN / TURN |
| `MIN_PORT`–`MAX_PORT`, default `50000`–`60000` | UDP | TURN media relay |

After saving `.env`, run:

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
sudo docker compose ps
```

`crossdesk_server` creates its certificate and database on first startup; `crossdesk_coturn` waits for the certificate files. The Compose health check only verifies that the key/certificate files exist. Also verify the status API below and an actual client connection.

All connection examples below use the Compose default, **9099**. If you change it, update subsequent URLs and client settings as well. Running the binary directly without a port argument defaults to **9090**.

<a id="clients"></a>

## Certificates and client setup

### Trust your server's certificate

Default files are stored under `${CROSSDESK_DATA_DIR}/certs/`:

| File | Purpose |
| --- | --- |
| `api.crossdesk.cn_root.crt` | Root certificate distributed to clients |
| `api.crossdesk.cn_bundle.crt` | Server certificate chain |
| `api.crossdesk.cn.key` | Server private key, retained on the server |

These are **filenames expected by the program**; your server does not need the `api.crossdesk.cn` domain. The [certificate generator](docker/generate_certs.sh) includes only `EXTERNAL_IP` as an IP subject alternative name. Use that IP in clients. For a custom domain, supply a valid certificate chain covering that domain and its private key at the `bundle.crt` and `.key` paths listed above.

The [startup script](docker/start.sh) reuses existing certificates. Editing the IP in `.env` does not reissue them. Regenerating self-signed certificates replaces the root certificate, which must then be trusted again on every client. A certificate issued by a system-trusted CA usually needs no separate root import.

Copy your deployment's `api.crossdesk.cn_root.crt` to each client and import it into the system trust store:

```powershell
# Windows: administrator PowerShell; substitute the actual file path
certutil -addstore "Root" "C:\path\to\api.crossdesk.cn_root.crt"
```

```bash
# Ubuntu / Debian
sudo cp /path/to/api.crossdesk.cn_root.crt /usr/local/share/ca-certificates/
sudo update-ca-certificates
```

```bash
# macOS
sudo security add-trusted-cert -d -r trustRoot \
  -k /Library/Keychains/System.keychain /path/to/api.crossdesk.cn_root.crt
```

### Configure the controller and host

1. Open **☰ → Settings → Self-Hosted Config** in the desktop client.
2. Enter the server IP/hostname, signaling port `9099`, and relay port `3478`, then confirm. The address field takes no URL scheme, path, or port.
3. Enable the **Self-Hosted Config** checkbox and click **OK** in the parent settings window. Configure both clients to use the same server.
4. After trusting the certificate, reopen the clients and wait for a server connection. Connect using the host's currently displayed ID and password.

The current desktop client verifies certificates through the **system trust store**; there is no certificate-file picker in the self-hosting dialog. Recheck the device ID after switching servers.

- **Web:** configure `signalingUrl` (for example, `wss://203.0.113.10:9099`) and the STUN address (for example, `stun:203.0.113.10:3478`) in the [Web client's](https://github.com/kunkundi/crossdesk-web-client) `web_client.js`. Replace the example IP, trust the certificate in the browser, and keep SRTP enabled on the host.
- **Native iOS:** enter the signaling host, signaling port, and STUN/TURN port under Settings → Server and apply. Install and trust the root certificate on the device for a self-signed deployment.

<a id="admin"></a>

## Admin dashboard

Set both `ADMIN_USERNAME` and `ADMIN_PASSWORD` in `.env`, then run `sudo docker compose up -d --no-build` again. Setting only one does not enable the dashboard. Open **`https://server-address:9099/admin`**, trust the certificate, and log in with that account.

| Area / control | Action |
| --- | --- |
| Top metrics | View online devices, Web clients, active connections, and cumulative online/control durations |
| Client Presence | Defaults to online PCs; switch PC/Web, filter Online/Remote/Offline/All, search IDs, sort, and paginate |
| Details | Expand platform/version, current and cumulative durations, connection IP, location, and remote peers |
| Active Sessions → Disconnect | Confirm to disconnect the selected session; devices stay online |
| Refresh lists / Logout | Refresh manually / sign out |

While visible, the page refreshes data every 5 seconds and updates displayed durations every second. Admin sessions are held in memory, expire after 8 hours by default, and require a new login after a server restart. Dashboard login cookies require HTTPS.

**Optional geolocation:** IP lookup is disabled by default. Enable `CROSSDESK_GEOIP_LOOKUP=1` and set `CROSSDESK_GEOIP_KEY` in `.env` to send public connection IPs to IP2Location for country and state/province lookup. The map counts current online clients, including Web clients and excluding `C-*` controller identities; unresolved locations are counted separately. Disabled lookups or unavailable results leave the map without resolved distribution data.

Connection IPs and locations are held in memory as presence state, not written to device profile records. The dashboard retains its IP2Location attribution link. The China map asset, [china-provinces.json](src/admin/web/china-provinces.json), comes from `china-map-geojson@1.0.4` under the ISC license.

<details>
<summary>Advanced geolocation and frontend configuration</summary>

- Configure the endpoint with `CROSSDESK_GEOIP_SCHEME`, `CROSSDESK_GEOIP_HOST`, `CROSSDESK_GEOIP_PORT`, and `CROSSDESK_GEOIP_PATH`. Defaults are HTTPS, `api.ip2location.io`, `443`, and `/?key={key}&ip={ip}`.
- `CROSSDESK_GEOIP_TIMEOUT_MS` defaults to `1200`. Failed lookups back off from `60000` to `1800000` milliseconds, controlled by `CROSSDESK_GEOIP_FAILURE_TTL_MS` and `CROSSDESK_GEOIP_FAILURE_MAX_TTL_MS`.
- Successful results are cached. Failed lookups are deduplicated by IP and retried with backoff; retries stop once no online client uses the IP.
- Frontend source lives in [src/admin/web](src/admin/web), installed at `/crossdesk-server/admin` in the container. A custom `CROSSDESK_ADMIN_WEB_DIR` must also be explicitly passed through Compose's `environment` and its directory mounted. Adding it to `.env` alone does not pass it into the container. Restart the service after frontend changes to refresh its asset cache.

</details>

<a id="stats"></a>

## Status API

`GET /stats` and `GET /api/stats` share the WSS HTTPS port, return aggregate statistics, allow cross-origin reads, and **do not require admin login**. Check from the server with the command below. Replace the example IP and adjust the root-certificate path if you changed the data directory:

```bash
curl --fail --cacert /var/lib/crossdesk/certs/api.crossdesk.cn_root.crt \
  https://203.0.113.10:9099/stats
```

For a domain with a trusted CA certificate:

```bash
curl --fail https://your-domain.example.com:9099/stats
```

| Field | Meaning |
| --- | --- |
| `online_device_count` | Online devices, excluding `web-*` and `C-*` |
| `online_web_client_count` | Online `web-*` clients |
| `active_connection_count` | Active remote-control connections, counted separately for each host–guest pair |
| `online_duration_seconds` | Sum of the current online periods of online devices |
| `total_online_seconds` | Cumulative device online time, including current periods |
| `total_control_seconds` / `total_controlled_seconds` | Cumulative controlling / controlled time, including ongoing sessions |

Durations are in seconds. Detailed admin APIs use `/api/admin/*` and require admin login; the public status API does not return device details.

<a id="operations"></a>

## Maintenance and troubleshooting

### Logs and upgrades

Run from the configuration directory:

```bash
sudo docker compose ps
sudo docker compose logs --tail 100 crossdesk-server coturn
```

Back up before upgrading. Change the fixed image tag in `.env` and use deployment files matching that release:

```bash
sudo docker compose config -q
sudo docker compose pull
sudo docker compose up -d --no-build
```

`docker compose restart` restarts existing containers without applying changed environment variables or images. Application logs are written to `${CROSSDESK_LOG_DIR}` and stdout; view Coturn logs with `docker compose logs coturn`. Each container's stdout/stderr rotates across three 50 MB files. Application log files rotate separately; restarts create new log groups that need periodic cleanup or archival.

### Backups and migration

Back up `.env`, `compose.yaml`, and `certs/` plus `db/` under `${CROSSDESK_DATA_DIR}`. These backups include keys and the device database. This example uses the default data directory and stops services for a consistent archive; remote connections are interrupted during the backup:

```bash
sudo docker compose stop
backup_file="crossdesk-backup-$(date +%Y%m%d-%H%M%S).tar.gz"
sudo tar -czf "$backup_file" .env compose.yaml -C /var/lib/crossdesk certs db
sudo docker compose up -d --no-build
```

For migration, restore the matching files on the destination host, check mount paths and permissions, then start. A changed public IP/hostname also requires updating certificates and client addresses. For older deployments with custom paths, point `CROSSDESK_DATA_DIR` and `CROSSDESK_LOG_DIR` at the existing directories.

### Common issues

| Symptom | Check |
| --- | --- |
| Configuration validation fails | Required IPs and both secrets are set, and files belong to the same release |
| Certificate generation / database writes fail | Signaling logs and writable mount paths; the default image starts as root, so adjust ownership for the actual UID/GID only when changing container users or similar settings |
| Coturn cannot bind | `INTERNAL_IP` belongs to a host interface and the ports are available |
| Client TLS errors | System root trust, validity dates, and matching IP/hostname; editing the IP does not update existing certificates |
| Server connected, remote device offline | Both clients use the same service, the host is running, and its current ID was copied |
| P2P and relay both fail | Client relay setting/version, matching TURN shared secrets, and relay/media firewall ports |
| Dashboard disabled / login fails | Both admin variables reached the container, the container was recreated, and the browser uses HTTPS |
| No map distribution | Geolocation enabled, API key, and outbound access; lookup is disabled by default |

<a id="build"></a>

## Build from source

CI builds on **Ubuntu 22.04, amd64 / arm64**. Install Git, a C++17 toolchain, and [Xmake](https://xmake.io/guide/quick-start.html). Xmake downloads the C++ dependencies. These Linux dependency commands match the CI baseline:

```bash
sudo apt-get update
sudo apt-get install -y ca-certificates git curl unzip build-essential

git clone https://github.com/kunkundi/crossdesk-server.git
cd crossdesk-server
xmake f -c -m release -y
xmake b -vy crossdesk_server
```

Ensure `xmake --version` works before the first build. See [xmake.lua](xmake.lua) for other platform configurations; the current release workflow provides only Linux binaries and images. Select Debug with `xmake f -m debug`; `xmake r -d crossdesk_server` runs through a debugger.

### Build a local runtime image

The Dockerfile **packages an already compiled Linux binary**. Complete a Release build on Ubuntu 22.04 with the same architecture as the target image, then run from the repository root:

```bash
# amd64; replace x86_64 with arm64 for an arm64 build
install -Dm755 build/linux/x86_64/release/crossdesk_server dist/crossdesk_server
sudo docker build -f docker/dockerfile -t crossdesk-server:local .
```

Copy `.env.example` to `.env` in the repository root, complete the deployment settings above, and set `CROSSDESK_IMAGE=crossdesk-server:local`. Then run:

```bash
sudo docker compose config -q
sudo docker compose pull coturn
sudo docker compose up -d --no-build
```

Direct binary execution requires the certificate, database, and log paths defined in [main.cpp](src/main.cpp), including `/var/lib/crossdesk/certs`. `CROSSDESK_DATA_DIR` / `CROSSDESK_LOG_DIR` configure Compose host mounts; they do not override paths in the binary. The binary does not generate certificates.

### CI test image

Successful default-branch builds publish the multi-architecture `crossdesk/crossdesk-server:test` tag, which changes with subsequent builds. Test deployments can set `CROSSDESK_IMAGE=crossdesk/crossdesk-server:test` in `.env` and use the pull/start commands above. Use fixed release tags for production. See the [CI workflow](.github/workflows/build.yml) for artifacts and image publishing.

## Feedback and license

[Report issues](https://github.com/kunkundi/crossdesk-server/issues) with the server tag, both client versions, deployment method, and sanitized logs. CrossDesk Server uses [LGPL-3.0](LICENSE).
