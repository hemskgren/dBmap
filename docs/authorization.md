# Authorization policy

The Hub API authenticates the current local admin/viewer bearer tokens, then asks OPA for a decision before serving a protected API operation. Authorization rules live in `policy/authz.rego`; the API supplies a subject, named action, resource, and context. OPA is reachable only over the private Compose network. The API denies requests when OPA is unavailable or does not return an explicit boolean decision.

## Current local actions

| Action | Resource | Admin | Viewer |
| --- | --- | --- | --- |
| `session.read` | `session` | Allow | Allow |
| `devices.list_summary` | `device_summaries` | Allow | Allow |
| `nodes.create` | `nodes` | Allow | Deny |
| `nodes.list` | `nodes` | Allow | Deny |
| `nodes.read_detail` | `nodes/{node_id}` | Allow | Deny |
| `nodes.update_desired` | `nodes/{node_id}` | Allow | Deny |
| `nodes.update_lifecycle` | `nodes/{node_id}` | Allow | Deny |
| `nodes.update_installation` | `nodes/{node_id}` | Allow | Deny |
| `observations.read` | `observations` | Allow | Deny |

The admin rule grants every action to an authenticated admin. A viewer is limited to session and summary reads. Authentication still happens in the API; OPA never receives the bearer token.

## Future policy inputs

The policy lets future authenticated users read shared device summaries, and defines owner access using `subject.id` and `resource.owner_id` for device details and edits. It also defines a provisioning quota using `context.pending_device_count` and `context.provisioning_limit`. The current database has no user ownership or provisioning-record model, so owner details and quota rules are not yet used by API routes. The existing `POST /api/v1/provision` endpoint exchanges a one-time device bootstrap token and keeps its current stateful validation. Add the ownership and quota context when the Phase E domain model is implemented.

Run the Rego tests from the repository root:

```bash
sh scripts/test-policy.sh
```

Run the API and Python tests from `hub/bootstrap` as documented in the README. Those tests stub the OPA decision to focus on API enforcement; `scripts/test-policy.sh` runs the actual Rego rules in OPA.
