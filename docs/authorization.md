# Authorization policy

The Hub API authenticates the current local admin/viewer bearer tokens, then asks OPA for a decision before serving a protected API operation. Authorization rules live in `policy/authz.rego`; the API supplies a subject, named action, resource, and context. OPA is reachable only over the private Compose network. The API denies requests when OPA is unavailable or does not return an explicit boolean decision.

## Current local actions

| Action | Resource | Admin | Viewer |
| --- | --- | --- | --- |
| `session.read` | `session` | Allow | Allow |
| `devices.list_summary` | `device_summaries` | Allow | Allow |
| `hub.status.read` | `hub` | Allow | Allow |
| `observations.read` | `observations` | Allow | Allow |
| `nodes.create` | `nodes` | Allow | Deny |
| `nodes.list` | `nodes` | Allow | Deny |
| `nodes.read_detail` | `nodes/{node_id}` | Allow | Deny |
| `nodes.update_desired` | `nodes/{node_id}` | Allow | Deny |
| `nodes.update_lifecycle` | `nodes/{node_id}` | Allow | Deny |
| `nodes.update_installation` | `nodes/{node_id}` | Allow | Deny |
| `nodes.read_ownership` | `nodes/{node_id}/ownership` | Allow | Deny |
| `nodes.update_ownership` | `nodes/{node_id}/ownership` | Allow | Deny |
| `users.read` | `users` | Allow | Allow |
| `users.create` | `users` | Allow | Deny |
| `users.update_status` | `users/{user_id}` | Allow | Deny |
| `users.identities.link` | `users/{user_id}/identities` | Allow | Deny |
| `users.identities.update_status` | `users/{user_id}/identities/{identity_id}` | Allow | Deny |
| `groups.read` | `groups` | Allow | Allow |
| `groups.create` | `groups` | Allow | Deny |
| `groups.update` | `groups/{group_id}` | Allow | Deny |
| `firmware.catalog.read` | `firmware_catalog` | Allow | Allow |
| `firmware.catalog.upload` | `firmware_catalog` | Allow | Deny |
| `firmware.catalog.promote` | `firmware_catalog` | Allow | Deny |
| `firmware.catalog.delete` | `firmware_catalog` | Allow | Deny |

The admin rule grants every action to an authenticated admin. A viewer can read the session, Hub status, device summaries, observations, internal user directory, group directory, and all firmware catalog entries, including unregistered candidates, but cannot read device details or change Hub state, user records, groups, or firmware catalog records. Firmware uploads, promotion, and deletion are admin-only. Authentication still happens in the API; OPA never receives the bearer token.

The firmware catalog is served by the Hub API. Admins upload external firmware bundles as candidates and explicitly promote them after review; viewers can list and download all bundles, including candidates, without changing catalog state. The optional local ESP-IDF builder is not part of normal Compose startup or the current catalog workflow. Individual user sessions are not implemented, so there is no owner-scoped firmware compatibility selection or ordinary-user deployment flow.

## Operating modes

The current deployment is Standard Local Hub mode. Admin and viewer bearer tokens are configured locally; device provisioning, MQTT, the API, and local administration do not require GitHub or internet access. There is no ordinary-user sign-in flow today. See [User identity and operating modes](identity-and-operating-modes.md) for the chosen multi-user design and the current identity/device separation.

Multi-user sign-in is not enabled yet. The user and external-identity tables are additive groundwork, with local-admin-only API routes for provisioning users and explicitly linking or disabling GitHub identities. There is no OAuth callback, ordinary-user session, or user-specific resource authorization. Do not configure or rely on GitHub OAuth until that flow is implemented. When it is added, OAuth must establish a local session; ordinary API requests must use that session and must not call GitHub each time. The local admin bearer-token path must remain independent.

## Internal users and external identities

`users.user_id` is a dBmap-generated UUID and remains stable if a display name changes. `external_identities` links a user to a provider using the provider's stable subject, with a uniqueness constraint on `(provider, provider_subject)`. One internal user can have multiple identities. The schema stores no email address or provider access token. Disabling an identity changes its status and retains the identity row; it does not cascade through the internal user's data. First-time user creation, explicit linking, session handling, and account lifecycle are not implemented yet.

At startup, `init_db()` calls SQLAlchemy `create_all`, so these additive tables are created in an existing database without changing existing node IDs, installations, or observations. No table migration is required for this step.

The current user API is `GET/POST /api/v1/users`, `GET /api/v1/users/{user_id}`, `PUT /api/v1/users/{user_id}/status`, `POST /api/v1/users/{user_id}/identities`, and `PUT /api/v1/users/{user_id}/identities/{identity_id}/status`. Group management uses `GET/POST /api/v1/groups`, `PUT /api/v1/groups/{group_id}/members/{user_id}`, `DELETE /api/v1/groups/{group_id}/members/{user_id}`, and `PUT /api/v1/groups/{group_id}/status`. These management operations require the local admin token and OPA; viewers can read group and user directories. There is no deletion operation for users or groups. Linking requires an explicit GitHub numeric subject and never merges accounts by display name or email. This API does not authenticate GitHub users.

## Future policy inputs

Users may read shared device summaries. An authenticated user may read detailed status only when `subject.id` matches `resource.owner_id`; owners cannot change desired state, installation, lifecycle, or ownership. The owner detail rule is covered by OPA tests but is not reachable through an ordinary-user session yet: the API currently authenticates only the local admin and viewer bearer tokens. Device ownership is stored separately in `device_ownership`, with a retained assignment history and at most one current user or group owner per device. Existing nodes begin unassigned. Admin-only `GET/PUT /api/v1/nodes/{node_id}/ownership` routes inspect or change assignments; disabled users and groups cannot be newly assigned. Reassignment and unassignment close the active record rather than deleting it. No node IDs, installations, observations, or ownership history are regenerated or erased.

For node-specific actions, the API resolves the current owner from the database and includes `resource.owner_id` in OPA input. This is already wired for every node resource route. Ordinary-user sessions are still absent, so only the admin and viewer bearer-token principals can currently reach those routes. When user sessions are introduced, they must identify the active internal user ID and OPA will enforce the owner match without trusting a client-supplied owner ID. Keep ownership changes admin-only. The existing `POST /api/v1/provision` endpoint exchanges a one-time device bootstrap token; device provisioning remains independent of users and ownership. The policy also retains a future provisioning quota rule, but there is no provisioning quota model or ordinary-user provisioning route yet.

Installation remains separate data keyed by the stable node ID in `installation_metadata`. The current one-row-per-device representation suits the single Local Hub installation context; relocation updates installation metadata without changing node identity or observations. A future multi-site installation history is outside this change.

Run the Rego tests from the repository root:

```bash
sh scripts/test-policy.sh
```

Run the API and Python tests from `hub/bootstrap` as documented in the README. Those tests stub the OPA decision to focus on API enforcement; `scripts/test-policy.sh` runs the actual Rego rules in OPA.
