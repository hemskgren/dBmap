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
| `users.read` | `users` | Allow | Deny |
| `users.create` | `users` | Allow | Deny |
| `users.update_status` | `users/{user_id}` | Allow | Deny |
| `users.identities.link` | `users/{user_id}/identities` | Allow | Deny |
| `users.identities.update_status` | `users/{user_id}/identities/{identity_id}` | Allow | Deny |

The admin rule grants every action to an authenticated admin. A viewer can read the session, Hub status, device summaries, and observations, but cannot read device details or change Hub state. Authentication still happens in the API; OPA never receives the bearer token.

## Operating modes

The current deployment is Standard Local Hub mode. Admin and viewer bearer tokens are configured locally; device provisioning, MQTT, the API, and local administration do not require GitHub or internet access. There is no ordinary-user sign-in flow today. See [User identity and operating modes](identity-and-operating-modes.md) for the chosen multi-user design and the current identity/device separation.

Multi-user sign-in is not enabled yet. The user and external-identity tables are additive groundwork, with local-admin-only API routes for provisioning users and explicitly linking or disabling GitHub identities. There is no OAuth callback, ordinary-user session, or user-specific resource authorization. Do not configure or rely on GitHub OAuth until that flow is implemented. When it is added, OAuth must establish a local session; ordinary API requests must use that session and must not call GitHub each time. The local admin bearer-token path must remain independent.

## Internal users and external identities

`users.user_id` is a dBmap-generated UUID and remains stable if a display name changes. `external_identities` links a user to a provider using the provider's stable subject, with a uniqueness constraint on `(provider, provider_subject)`. One internal user can have multiple identities. The schema stores no email address or provider access token. Disabling an identity changes its status and retains the identity row; it does not cascade through the internal user's data. First-time user creation, explicit linking, session handling, and account lifecycle are not implemented yet.

At startup, `init_db()` calls SQLAlchemy `create_all`, so these additive tables are created in an existing database without changing existing node IDs, installations, or observations. No table migration is required for this step.

The current admin API is `GET/POST /api/v1/users`, `GET /api/v1/users/{user_id}`, `PUT /api/v1/users/{user_id}/status`, `POST /api/v1/users/{user_id}/identities`, and `PUT /api/v1/users/{user_id}/identities/{identity_id}/status`. It is authorized by the existing local admin token and OPA; viewers are denied. There is no deletion operation. Linking requires an explicit GitHub numeric subject and never merges accounts by display name or email. This API does not authenticate GitHub users.

## Future policy inputs

The policy lets future authenticated users read shared device summaries, and defines owner access using `subject.id` and `resource.owner_id` for device details and edits. It also defines a provisioning quota using `context.pending_device_count` and `context.provisioning_limit`. The database has no device-ownership or provisioning-record model, so owner details and quota rules are not used by API routes. The existing `POST /api/v1/provision` endpoint exchanges a one-time device bootstrap token and keeps its current stateful validation. Add ownership and quota context only with the corresponding domain model and authorization behavior.

Run the Rego tests from the repository root:

```bash
sh scripts/test-policy.sh
```

Run the API and Python tests from `hub/bootstrap` as documented in the README. Those tests stub the OPA decision to focus on API enforcement; `scripts/test-policy.sh` runs the actual Rego rules in OPA.
