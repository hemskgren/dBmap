# Unauthenticated access investigation — public Hub web surface

**Target:** `https://dbmap.ddns.net:8443/`  
**Date:** 2026-11-19  
**Scope:** Read-only investigation of public frontend exposure and backend authentication/authorization enforcement. No production configuration changes. No intentional destructive operations.  
**Sources:** Repository source (`web/`, `hub/bootstrap/`, `nginx/`, `policy/`, `analysis/`, `firmware-builder/`, Compose), non-destructive HTTPS probes against the public host, and sanitized live role checks with the deployment’s local admin/viewer tokens.

---

## 1. Executive summary

The publicly reachable Hub UI is a **static single-page application**. An unauthenticated visitor can download the full HTML/JS shell (including login UI, DOM ids such as `allDevices` / `login-panel`, and client-side logout logic). That is expected for this architecture and does **not** by itself mean the backend trusts the frontend.

**Protected Hub data APIs independently require a local admin or viewer bearer token**, then consult OPA (`policy/authz.rego`) before returning data. Live unauthenticated requests to device, user, group, observation, hub-status, firmware-catalog, and analysis-scratch routes returned **HTTP 401** with JSON `{"detail":"valid local access token required"}`. Invalid bearer tokens were also rejected with 401.

Frontend “Sign out” only clears in-memory JS state. There is **no server-side session store** to invalidate: authentication is shared long-lived deployment secrets (`DBMAP_ADMIN_TOKEN` / `DBMAP_VIEWER_TOKEN`). Logout therefore cannot revoke those secrets; possession of a token remains valid until rotation.

**Overall conclusion:** **PARTIALLY SECURE** — no confirmed unauthenticated access to protected Hub inventory/user/observation data within the tested scope, but residual weaknesses remain (long-lived shared tokens / non-revocable logout, intentional public device-provision endpoint, public MQTT listener, private-CA TLS).

**Investigator note (scope deviation):** During live admin/viewer role checks, an admin `POST /api/v1/users` accidentally created an active directory user `security-probe-should-fail-or-create` (`user_id` `410abc14-308b-42bf-b425-f7376bb83a95`, created `2026-11-19T17:08:14.911719Z`). Cleanup (disable) requires operator approval and was **not** performed in this task.

---

## 2. Actual application architecture

Corrected request path for this deployment:

```text
Internet
   |
   v
Nginx (TLS terminate, host-published :8443)
   |
   +---- HTTPS --> dbmap-web:443
   |                 Static SPA (web/index.html + assets)
   |                 No API, no SQLite, no MQTT client
   |
   +---- /api/v1/* (except builder/analysis prefixes) --> hub-bootstrap:8443
   |         FastAPI (hub/bootstrap/app/api.py)
   |         Auth: Bearer admin|viewer (_authenticate)
   |         Authz: OPA over opa-private (policy.evaluate / require_policy)
   |         DB: SQLite file inside hub-bootstrap (no published DB port)
   |         Special:
   |           POST /api/v1/provision          (bootstrap_token body; no OPA)
   |           GET  /api/v1/device/*           (device API token; no OPA)
   |
   +---- /api/v1/analysis/* --> analysis:8443 (optional profile; same bearer + OPA)
   |
   +---- /api/v1/firmware/{targets,builds}/* --> firmware-builder:8443
   |         Admin bearer only (hmac.compare_digest)
   |
   +---- /health --> hub-bootstrap /health (public liveness)

Separately (not via Nginx UI path):
   Mosquitto MQTT/TLS published on LAN/public :8883 (device/simulator traffic)
   OPA :8181 not host-published (closed/filtered from Internet in this probe)
```

| Component | Implementation | Notes |
| --- | --- | --- |
| Reverse proxy / TLS | `nginx/` (`default.conf.template`) | Only Compose HTTPS listener on `${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}` |
| Static web | `web/` via `dbmap-web` (`web/serve.py`, HTTPS) | Entry: `/` → `web/index.html` |
| Hub API | FastAPI `hub/bootstrap/app/main.py` → `api.py` | Bearer + OPA for human routes |
| Auth | `_authenticate` / `require_policy` in `api.py` | Timing-safe compare to configured secrets |
| Authorization | `hub/bootstrap/app/policy.py` + `policy/authz.rego` | Fail-closed if OPA unavailable |
| Session model | None for humans | Shared deployment bearer tokens; UI holds token in memory |
| Database | SQLite in `hub-bootstrap` volume | No DB port |
| WebSocket / SSE | **Not used** | UI polls HTTP APIs on a timer |
| Analysis | Optional Compose profile | Same admin/viewer tokens + OPA |
| Firmware builder | Optional profile | Admin bearer only |

References: `docs/connectivity.md`, `docs/authorization.md`, `docs/identity-and-operating-modes.md`.

---

## 3. Publicly accessible frontend inventory

### Live probes (unauthenticated)

| Path | HTTP | Notes |
| --- | --- | --- |
| `GET /` | 200 | ~310 KB HTML with embedded app JS |
| `GET /map/` | 200 | Map SPA (~55 KB) |
| `GET /espconnect/` | 200 | ESPConnect shell |
| `GET /esp-web-tools/install-button.js` | 200 | Vendored flasher helper |
| `GET /display-time.js`, `/observation-lifecycle.js`, `/usb-chip-inspect.js` | 200 | Shared UI helpers |
| `GET /display-config.js` | 200 | `window.DBMAP_DISPLAY_TIMEZONE = "Europe/Stockholm";` only |
| `GET /site.webmanifest`, favicons | 200 | Public branding assets |
| `GET /docs`, `/openapi.json`, `/redoc`, `/.env`, source maps | 404 | Not served on public Nginx surface |
| `GET /health` | 200 | `{"status":"ok","service":"hub-bootstrap"}` |

Response security headers observed on `/` and API responses (via Nginx): `X-Frame-Options: SAMEORIGIN`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: strict-origin-when-cross-origin`, `Strict-Transport-Security: max-age=31536000`, `Permissions-Policy: camera=(), microphone=(), geolocation=()`. HTML uses `Cache-Control: no-store`.

TLS certificate: private CA `CN=dBmap-local-ca`, SAN includes `dbmap.ddns.net` (and local names). Clients must trust the project CA or use insecure skip-verify (as in the initial `curl -k`).

### Classification of frontend content

**Expected public information**

- Full SPA markup and client JavaScript required to render login and (after auth) the dashboard.
- UI structure symbols (`allDevices`, `device-list`, `user-list`, `group-list`, `login-panel`, logout clearing those lists).
- Client API path strings under `/api/v1/...`.
- Public display timezone config.

**Potentially sensitive — checked, not found in public assets**

- No embedded admin/viewer tokens, MQTT passwords, CA private keys, or DB connection strings in the served HTML/JS.
- No source maps served.
- `localStorage` / `sessionStorage` used for Wi-Fi SSID cache and non-secret deployment-session metadata; bearer token is **not** persisted there (`authToken` in-memory only in `web/index.html`).

**Info disclosure that is intentional / low risk**

- Admin UI workflows and endpoint names are visible in JS (not classified as a vulnerability by itself).
- Map page can read the opener’s in-memory token via `window.dbmapGetMapSession` (`web/index.html`, `web/map/index.html`) — same-origin parent window only; not available to an unauthenticated visitor without the token.

---

## 4. Complete backend endpoint inventory

### 4.1 Hub (`hub/bootstrap/app/api.py`)

Auth helpers: `_authenticate`, `require_policy` (lines ~104–151). Device helpers: `_device_api_token`, `service.node_for_device_api_token`.

| Method | Path | Purpose | AuthN | AuthZ (OPA action) | Sensitive if authorized |
| --- | --- | --- | --- | --- | --- |
| GET | `/health` | Liveness | None | — | Low (service name) |
| GET | `/api/v1/session` | Role for UI | Bearer | `session.read` | Role only |
| GET | `/api/v1/devices` | Device summaries | Bearer | `devices.list_summary` | Device inventory |
| GET | `/api/v1/nodes` | Full node list | Bearer | `nodes.list` (admin) | Full node records |
| GET | `/api/v1/nodes/{node_id}` | Node detail | Bearer | `nodes.read_detail` | Desired/reported/status |
| GET | `/api/v1/nodes/by-chip-mac/{chip_mac}` | Lookup by MAC | Bearer | `nodes.read_detail` | Device identity |
| GET | `/api/v1/nodes/{node_id}/monitoring/events` | Device monitor events | Bearer | `nodes.read_detail` | Ops events |
| POST | `/api/v1/nodes` | Register node | Bearer | `nodes.create` | Bootstrap token issued |
| POST | `/api/v1/nodes/{node_id}/bootstrap-token` | Rotate pending bootstrap | Bearer | `nodes.bootstrap_token.rotate` | Secret |
| GET/PUT | `/api/v1/nodes/{node_id}/ownership` | Ownership | Bearer | read/update ownership | Directory linkage |
| PUT | `/api/v1/nodes/{node_id}/desired` | Desired state | Bearer | `nodes.update_desired` | Config |
| PUT | `/api/v1/nodes/{node_id}/lifecycle` | Lifecycle | Bearer | `nodes.update_lifecycle` | Admin control |
| PUT | `/api/v1/nodes/{node_id}/hardware-revision` | Hardware | Bearer | `nodes.update_hardware` | Admin |
| PUT | `/api/v1/nodes/{node_id}/installation` | Installation | Bearer | `nodes.update_installation` | Location |
| POST | `/api/v1/nodes/{node_id}/commands` | Output commands | Bearer | `nodes.send_command` | Device actuation |
| GET | `/api/v1/users`, `.../{user_id}` | User directory | Bearer | `users.read` | User records |
| POST | `/api/v1/users` | Create user | Bearer | `users.create` | Directory write |
| PUT | `/api/v1/users/{user_id}/status` | Enable/disable | Bearer | `users.update_status` | Account control |
| POST/PUT | identities routes | Link/disable IdP | Bearer | identities.* | Identity linkage |
| GET/POST/PUT/DELETE | `/api/v1/groups...` | Groups | Bearer | groups.* | Group directory |
| GET | `/api/v1/observations`, `/count` | Observations | Bearer | `observations.read` | Sensor history |
| GET | `/api/v1/hub/status` (+ monitoring, environmental-context, component details/logs/failures) | Hub ops | Bearer | `hub.status.read` / `hub.logs.read` | Infra + counts; logs may include sensitive ops data |
| GET/POST/DELETE | `/api/v1/firmware/catalog...` | Firmware catalog | Bearer | firmware.catalog.* / flash.read | Binaries + metadata |
| GET/POST/PATCH | node firmware selection / deployment-attempts | Deploy | Bearer | firmware.deploy.* | Admin flash ops |
| GET | `/api/v1/device/desired-firmware` | Device Desired | Device API bearer | Hub token hash lookup (no OPA) | Firmware ids for that node |
| GET | `/api/v1/device/ota-image` | OTA binary | Device API bearer | Same | Firmware binary |
| POST | `/api/v1/nodes/{node_id}/device-api-token` (+ revoke, recovery-provision) | Device credential ops | Bearer | device_api_token.* / recovery | Secrets |
| POST | `/api/v1/provision` | Device bootstrap exchange | **Bootstrap token in body** | No OPA | Issues MQTT + device API credentials once |

### 4.2 Analysis (`analysis/app/api.py`) — profile enabled on this host

| Method | Path | AuthN | AuthZ |
| --- | --- | --- | --- |
| GET | `/api/v1/analysis/scratch` (+ `/{object_id}`) | Bearer | `analysis.scratch.read` |
| POST/PUT/DELETE | scratch create/update/delete/clear | Bearer | write/delete (admin) |

Unauthenticated live: `GET /api/v1/analysis/scratch` → **401**.

### 4.3 Firmware builder (`firmware-builder/server.py`)

| Method | Path pattern | AuthN |
| --- | --- | --- |
| GET/POST/DELETE | `/api/v1/firmware/targets...`, `/api/v1/firmware/builds...` | Admin bearer only |

Unauthenticated live: **401** (“Admin bearer token required”).

### 4.4 Not present

- No application WebSocket or SSE endpoints in Hub/web.
- FastAPI `/docs` / OpenAPI are not exposed through Nginx (public `/docs` → HTML 404).

---

## 5. Authentication enforcement results

### Method

Unauthenticated `curl -k` against `https://dbmap.ddns.net:8443` for data-bearing routes, plus invalid bearer samples. No password guessing.

### Results (representative)

| Request | Status | Body (sanitized) |
| --- | --- | --- |
| `GET /api/v1/session` | 401 | `valid local access token required` |
| `GET /api/v1/devices` | 401 | same |
| `GET /api/v1/nodes` | 401 | same |
| `GET /api/v1/users` | 401 | same |
| `GET /api/v1/groups` | 401 | same |
| `GET /api/v1/observations` | 401 | same |
| `GET /api/v1/hub/status` | 401 | same |
| `GET /api/v1/firmware/catalog` | 401 | same |
| `GET /api/v1/hub/components/database/details` | 401 | same |
| `GET /api/v1/analysis/scratch` | 401 | same |
| `GET /api/v1/device/desired-firmware` | 401 | `device API token required` |
| `GET /api/v1/device/desired-firmware` + `Authorization: Bearer <fake>` | 401 | same message (invalid tokens not distinguished) |
| `GET /api/v1/devices` + invalid admin/viewer bearer | 401 | `valid local access token required` |
| `POST /api/v1/provision` `{}` | 422 | validation (missing `bootstrap_token`) |
| `POST /api/v1/provision` fake UUID token | 400 | `invalid or consumed bootstrap token` |
| `GET /health` | 200 | liveness only |

**Verdict:** Clearing JS arrays / hiding `#login-panel` is **not** the security boundary. Protected Hub/Analysis/builder data routes reject unauthenticated callers at the API.

Unit-test corroboration: `hub/bootstrap/tests/test_service.py` (`test_viewer_can_read_device_summaries_but_not_admin_api`), `test_identity_api.py`, `test_device_https_identity.py`, `analysis/tests/test_scratch_api.py`.

---

## 6. Authorization enforcement results

### Roles (current)

| Role | How authenticated | Capability summary |
| --- | --- | --- |
| `admin` | `DBMAP_ADMIN_TOKEN` | All OPA actions (`policy/authz.rego` admin allow) |
| `viewer` | `DBMAP_VIEWER_TOKEN` | Read session, devices summary, node detail, hub status/logs, observations, users/groups, firmware catalog read, analysis scratch read |
| Device | Per-node device API token (hash in DB) | Desired firmware / OTA for that node only |
| Ordinary `user` | **Not implemented** (no OAuth/session) | Rego owner rules exist but are unreachable |

### Live sanitized role matrix (2026-11-19)

| Caller | `GET /session` | `GET /devices` | `GET /nodes` | `GET /users` | `POST /users` |
| --- | --- | --- | --- | --- | --- |
| Unauthenticated | 401 | 401 | 401 | 401 | 401 |
| Viewer | 200 `viewer` | 200 | **403** policy | 200 | **403** policy |
| Admin | 200 `admin` | 200 | 200 | 200 | 200 (see accidental create) |

Frontend also hides admin panels by `role !== 'admin'` (`showDashboard` in `web/index.html`), but viewer denial of `nodes.list` / `users.create` was confirmed **server-side**.

### Ownership / IDOR

- Node routes pass `resource.owner_id` from DB into OPA (`require_policy` in `api.py`).
- Owner-scoped `user` rules are future-facing; today only admin/viewer principals authenticate.
- **NOT VERIFIED live:** substituting another user’s `user_id` under an ordinary-user session (no such sessions). Admin/viewer can read the full directories by design.

---

## 7. Session and logout findings

### Login

1. User pastes access token into `#access-token` (`web/index.html` login form).
2. Client sets `authToken.value` and calls `GET /api/v1/session` with `Authorization: Bearer …` (`api()` helper).
3. On success, UI shows dashboard for `session.role`; input field cleared; token remains in the in-memory `authToken` object.
4. **No cookies** are set for Hub auth (`Set-Cookie` absent on `/` and `/api/v1/devices`). HttpOnly/Secure/SameSite cookie attributes therefore do not apply to this model.

### Authenticated requests

- Every `api()` call attaches the in-memory bearer token.
- Hub validates with timing-safe compare (`ids.timing_safe_eq`) then OPA.
- Tokens are long-lived deployment secrets (minimum length enforced at Hub startup when MQTT is enabled: `main._validate_security_settings`).

### Logout

Handler on `#logout` (`web/index.html` ~6048–6110):

- Clears `authToken.value`, timers, DOM lists (`allDevices`, user/group lists), and shows `#login-panel`.
- **Does not** call any logout/revoke API (none exists for admin/viewer tokens).
- A previously copied bearer token remains valid after UI logout until operators rotate `.env` secrets and restart services.

**Assessment:** Frontend logout is UX-only relative to shared bearer secrets. This matches documented Standard Local Hub mode (`docs/identity-and-operating-modes.md`), not a claim of per-user session invalidation.

Map child window: reads token from `opener.dbmapGetMapSession()` while the Hub tab remains logged in; after Hub logout the in-memory token is empty for new reads (existing map fetches would fail subsequent API calls). ESPConnect opens with `opener = null` and does not receive the Hub token.

---

## 8. WebSocket / realtime findings

**No Hub application WebSockets or SSE.** Device updates reach the UI via periodic HTTPS polling (`setInterval` ~30s in `showDashboard`). MQTT is a separate device/broker channel, not a browser realtime API.

Unauthenticated browsers therefore cannot subscribe to a Hub push channel for device data.

MQTT/TLS `:8883` is publicly reachable (TCP open; TLS handshake with Mosquitto cert succeeded). Credential guessing against MQTT was **not** performed (out of scope / potentially disruptive). Broker auth is username/password per docs; not assessed beyond reachability.

---

## 9. Deployment configuration findings

| Item | Observation | Notes |
| --- | --- | --- |
| Published ports (Compose) | HTTPS `:8443`, MQTT `:8883` | OPA/web/hub/analysis not host-published except via Nginx |
| OPA from Internet | TCP `:8181` closed/filtered | Expected |
| Host `:80` / `:443` | Apache/2.4.71 default page (unrelated to dBmap) | Separate site on same IP; not the Hub SPA |
| Debug / OpenAPI on public URL | Not exposed | 404 via Nginx |
| Public `/health` | 200 | Intentional liveness |
| CORS | `OPTIONS /api/v1/devices` → 405; no `Access-Control-Allow-Origin` on 401 | No permissive CORS headers observed |
| Secrets in frontend | Not found | Tokens stay server-side / operator-managed |
| TLS | Private CA; SAN includes `dbmap.ddns.net` | Browsers warn unless CA installed |
| Security headers | Present (see §3) | HSTS on HTTPS responses |
| `POST /api/v1/provision` | Public, bootstrap-token gated | Tokens from `secrets.token_urlsafe(32)` (`ids.new_token`) |
| Analysis profile | Enabled (401 on scratch, not “unavailable” 503) | Auth still enforced |

---

## 10. Security findings (classified)

### F-01 — Public SPA delivers full client application

| Field | Content |
| --- | --- |
| Classification | **INFO** |
| Component | `GET /`, `web/index.html` |
| Evidence | Unauthenticated 200, ~310 KB HTML/JS including login/logout and API client |
| Expected | Static UI served without auth |
| Actual | Same |
| Impact | Attackers learn UI/API shape; no direct data access |
| Remediation | Optional future: split admin bundles / authenticated asset gating (not required for API security) |
| Reproducible safely | Yes |

### F-02 — Backend enforces bearer auth on protected data APIs

| Field | Content |
| --- | --- |
| Classification | **PASS** |
| Component | Hub/Analysis/builder `/api/v1/*` data routes |
| Evidence | Live 401s (§5); `require_policy` / device token checks in source |
| Expected | Unauthorized callers rejected |
| Actual | Rejected with 401 (or 403 for authenticated-but-denied) |
| Impact | Addresses the core investigation question: **not** frontend-only auth |
| Remediation | None required for this finding |
| Reproducible safely | Yes |

### F-03 — OPA role separation (admin vs viewer)

| Field | Content |
| --- | --- |
| Classification | **PASS** (within admin/viewer model) |
| Component | `policy/authz.rego`, `api.require_policy` |
| Evidence | Live viewer `GET /nodes` and `POST /users` → 403; admin allowed |
| Expected | Viewer cannot perform admin mutations / full node list |
| Actual | Enforced |
| Impact | Frontend role hiding is not the only control |
| Remediation | Keep OPA fail-closed; extend when ordinary-user sessions ship |
| Reproducible safely | Yes (read + denied write) |

### F-04 — Logout does not revoke shared bearer tokens

| Field | Content |
| --- | --- |
| Classification | **WARNING** |
| Component | `#logout` in `web/index.html`; no Hub revoke endpoint for admin/viewer |
| Evidence | Logout clears `authToken` only; auth model is shared env secrets |
| Expected (for per-user sessions) | Server invalidation |
| Actual | Token remains valid until rotation |
| Impact | Stolen token usable after UI “Sign out”; shared secret blast radius is whole Hub |
| Remediation | Future multi-user sessions with hashed server-side session store + logout (documented direction); until then, rotate tokens on suspected compromise; avoid sharing viewer/admin tokens broadly |
| Reproducible safely | Yes (conceptual; do not exfiltrate tokens) |

### F-05 — Public device provision endpoint

| Field | Content |
| --- | --- |
| Classification | **INFO** (by design) / residual **WARNING** for Internet exposure |
| Component | `POST /api/v1/provision` |
| Evidence | Reachable without admin/viewer bearer; fake token → 400 |
| Expected | Bootstrap-token exchange for devices |
| Actual | Same; high-entropy `token_urlsafe(32)` |
| Impact | Internet-facing attempt surface; success requires a valid unused bootstrap token |
| Remediation | Keep tokens secret; consider network restrictions / rate limiting / fail2ban correlation; never log plaintext tokens |
| Reproducible safely | Yes with invalid tokens only |

### F-06 — Public MQTT/TLS listener

| Field | Content |
| --- | --- |
| Classification | **WARNING** (exposure) / auth strength **NOT VERIFIED** |
| Component | Host `:8883` Mosquitto |
| Evidence | TCP open; TLS handshake OK; no credential tests |
| Expected | Devices need MQTT reachability |
| Actual | Port reachable from Internet on this DDNS host |
| Impact | Credential stuffing / protocol attack surface if passwords weak |
| Remediation | Strong unique per-node creds (Hub issues them); firewall/allowlist if possible; monitor auth failures |
| Reproducible safely | Reachability yes; auth testing needs approval |

### F-07 — Public health endpoint

| Field | Content |
| --- | --- |
| Classification | **INFO** |
| Component | `GET /health` |
| Evidence | 200 `hub-bootstrap` |
| Impact | Confirms service up; no inventory |
| Remediation | Optional: restrict to probe networks |
| Reproducible safely | Yes |

### F-08 — Private-CA TLS on a public hostname

| Field | Content |
| --- | --- |
| Classification | **WARNING** |
| Component | Nginx certificate |
| Evidence | Issuer `dBmap-local-ca`; users need custom trust or `-k` |
| Impact | MITM risk for clients that disable verification; poor browser UX |
| Remediation | Publicly trusted cert (e.g. ACME) for Internet hostnames, or restrict to VPN/LAN |
| Reproducible safely | Yes |

### F-09 — Ordinary-user authorization not reachable

| Field | Content |
| --- | --- |
| Classification | **INFO** / **NOT VERIFIED** for owner IDOR |
| Component | Rego `user` rules; no OAuth/session yet |
| Evidence | `docs/authorization.md`, `docs/identity-and-operating-modes.md` |
| Impact | Ownership-based least privilege not available; admin/viewer see shared Hub data |
| Remediation | Implement sessions before exposing Hub beyond trusted operators |
| Reproducible safely | N/A |

### F-10 — Accidental user creation during investigation

| Field | Content |
| --- | --- |
| Classification | **INFO** (operational hygiene) |
| Component | Live `POST /api/v1/users` |
| Evidence | User `security-probe-should-fail-or-create` / `410abc14-308b-42bf-b425-f7376bb83a95` |
| Impact | Extra directory entry; not an unauthenticated vulnerability |
| Remediation | Operator approval to `PUT .../status` → `disabled` (no delete API) |
| Reproducible safely | Do not repeat |

---

## 11. Recommended remediation priorities

1. **Operational:** Disable the accidental probe user after approval; confirm no other unexpected directory changes.
2. **Token hygiene:** Treat admin/viewer bearers as break-glass Hub secrets; rotate if exposure suspected; do not paste into untrusted machines.
3. **Internet posture:** Prefer VPN/LAN-only Hub admin UI or publicly trusted TLS if keeping DDNS Internet exposure.
4. **MQTT:** Confirm strong issued passwords and monitoring of broker auth failures; consider source IP restrictions if devices allow it.
5. **Product roadmap:** Server-side per-user sessions with logout/expiry before multi-user Internet use (`docs/identity-and-operating-modes.md`).
6. **Optional hardening:** Rate-limit `/api/v1/provision`; consider non-public health; avoid documenting real inventory in public tickets.

**No code or production config changes were made in this investigation** (except the accidental user create noted above).

---

## 12. Outstanding tests requiring approval

| Test | Why approval needed |
| --- | --- |
| Disable/delete cleanup of probe user `410abc14-308b-42bf-b425-f7376bb83a95` | Writes directory state |
| MQTT AUTH with invalid passwords / connection flooding | May trip fail2ban or load broker |
| Using a real device API token or bootstrap token | Secret handling / device impact |
| OTA / reboot / Desired writes / Output commands | Live device impact |
| Direct SQLite inspection | Per `AGENTS.md` DB approval gate |
| Brute-force or timing analysis of bearer/bootstrap tokens | Abusive |
| Cross-tenant / multi-Hub checks | Not applicable to single Local Hub |

---

## 13. Final assessment — direct answers

### A. Frontend exposure

**Can an unauthenticated visitor retrieve the complete frontend HTML and JavaScript?**  
Yes. `GET /` returns the full SPA (~310 KB).

**Is this expected, or does the frontend contain sensitive information?**  
Expected for a static Hub UI. No credentials, private keys, or live inventory were found embedded in public assets. UI structure and API path names are visible.

### B. Device data

**Can an unauthenticated visitor retrieve any device information?**  
No protected device inventory/detail via Hub APIs (401). Public `/health` does not list devices. MQTT was not shown to leak device data without credentials (not fully tested).

### C. User data

**Can an unauthenticated visitor retrieve any user or group information?**  
No. `GET /api/v1/users` and `/api/v1/groups` returned 401 without a token.

### D. Configuration

**Can an unauthenticated visitor retrieve device configuration, application configuration or other sensitive settings?**  
No for Hub Desired/installation/hub-status/firmware catalog (401). Public `display-config.js` only exposes display timezone. Fake provision attempts do not return credentials.

### E. API security

**Does every protected backend endpoint enforce authentication independently of frontend state?**  
Yes for Hub human APIs (bearer + OPA), Analysis scratch APIs, firmware builder APIs, and device Desired/OTA routes (device token). Exceptions by design: public `/health` and bootstrap-gated `POST /api/v1/provision`.

### F. Authorization

**Does the backend enforce permissions and resource ownership, rather than trusting frontend restrictions?**  
Yes for admin vs viewer via OPA (live 403s). Ownership-based ordinary-user enforcement is implemented in policy but **not reachable** without user sessions; admin/viewer share Hub-wide read scope by design.

### G. Logout

**Does logout actually invalidate the server-side session?**  
No server-side human session exists. Logout clears client state only; shared bearer tokens remain valid until rotated.

### H. WebSocket

**Can an unauthenticated or logged-out client receive device data through a persistent connection?**  
No Hub WebSocket/SSE channel exists. Logged-out UI holds no token for subsequent HTTP polls.

### I. Overall conclusion

**PARTIALLY SECURE** — authentication exists and, within the tested scope, **unauthenticated callers could not read protected Hub device/user/group/observation/config APIs**. Weaknesses remain: non-revocable shared bearer tokens (logout UX-only), Internet-exposed provision + MQTT surfaces, private-CA TLS, and incomplete ordinary-user authorization.

**Unverified areas:** MQTT credential resistance, fail2ban efficacy, rate limits on provision, physical/USB paths, and future owner-scoped user sessions.

---

## Appendix A — Key source references

| Concern | Location |
| --- | --- |
| Route + auth dependencies | `hub/bootstrap/app/api.py` (`_authenticate`, `require_policy`, provision, device routes) |
| OPA client | `hub/bootstrap/app/policy.py` `evaluate` |
| Rego policy | `policy/authz.rego` |
| Token generation / compare | `hub/bootstrap/app/ids.py` `new_token`, `timing_safe_eq` |
| Startup secret length checks | `hub/bootstrap/app/main.py` `_validate_security_settings` |
| Nginx routing / headers | `nginx/default.conf.template` |
| UI login/logout / bearer | `web/index.html` (`api`, login submit, `#logout`, `dbmapGetMapSession`) |
| Map token reuse | `web/map/index.html` `readSessionFromOpener` |
| Analysis auth | `analysis/app/api.py` |
| Firmware builder auth | `firmware-builder/server.py` `_authorized` |
| Architecture docs | `docs/connectivity.md`, `docs/authorization.md`, `docs/identity-and-operating-modes.md` |

## Appendix B — Sanitized HTTP examples

```http
GET /api/v1/devices HTTP/1.1
Host: dbmap.ddns.net:8443

HTTP/1.1 401 Unauthorized
Content-Type: application/json
{"detail":"valid local access token required"}
```

```http
GET /health HTTP/1.1
Host: dbmap.ddns.net:8443

HTTP/1.1 200 OK
Content-Type: application/json
{"status":"ok","service":"hub-bootstrap"}
```

```http
POST /api/v1/provision HTTP/1.1
Content-Type: application/json
{"bootstrap_token":"00000000-0000-0000-0000-000000000000"}

HTTP/1.1 400 Bad Request
{"detail":"invalid or consumed bootstrap token"}
```

No credentials, cookies, or live tokens are included in this report.
