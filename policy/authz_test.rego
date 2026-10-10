package dbmap.authz_test

import rego.v1
import data.dbmap.authz

test_admin_can_perform_any_action if {
	authz.allow with input as {
		"subject": {"id": "local-admin", "role": "admin", "authenticated": true},
		"action": "nodes.update_lifecycle",
		"resource": {"type": "nodes", "id": "EAR-001"},
		"context": {},
	}
}

test_viewer_can_read_summary_and_session if {
	authz.allow with input as {
		"subject": {"id": "local-viewer", "role": "viewer", "authenticated": true},
		"action": "session.read",
		"resource": {"type": "device_summaries"},
		"context": {},
	}
	authz.allow with input as {
		"subject": {"id": "local-viewer", "role": "viewer", "authenticated": true},
		"action": "devices.list_summary",
		"resource": {"type": "device_summaries"},
		"context": {},
	}
}

test_viewer_cannot_read_details_or_write if {
	not authz.allow with input as {
		"subject": {"id": "local-viewer", "role": "viewer", "authenticated": true},
		"action": "nodes.read_detail",
		"resource": {"type": "nodes", "id": "EAR-001"},
		"context": {},
	}
}

test_viewer_cannot_manage_users_or_identities if {
	not authz.allow with input as {
		"subject": {"id": "local-viewer", "role": "viewer", "authenticated": true},
		"action": "users.create",
		"resource": {"type": "users"},
		"context": {},
	}
	not authz.allow with input as {
		"subject": {"id": "local-viewer", "role": "viewer", "authenticated": true},
		"action": "users.identities.link",
		"resource": {"type": "users", "id": "user-123"},
		"context": {},
	}
}

test_owner_can_read_and_update_owned_device if {
	authz.allow with input as {
		"subject": {"id": "user-123", "role": "user", "authenticated": true},
		"action": "nodes.read_detail",
		"resource": {"type": "nodes", "id": "EAR-001", "owner_id": "user-123"},
		"context": {},
	}
	authz.allow with input as {
		"subject": {"id": "user-123", "role": "user", "authenticated": true},
		"action": "nodes.update_installation",
		"resource": {"type": "nodes", "id": "EAR-001", "owner_id": "user-123"},
		"context": {},
	}
}

test_user_can_read_shared_device_summary if {
	authz.allow with input as {
		"subject": {"id": "user-123", "role": "user", "authenticated": true},
		"action": "devices.list_summary",
		"resource": {"type": "device_summaries"},
		"context": {},
	}
}

test_user_cannot_access_another_users_device if {
	not authz.allow with input as {
		"subject": {"id": "user-123", "role": "user", "authenticated": true},
		"action": "nodes.read_detail",
		"resource": {"type": "nodes", "id": "EAR-001", "owner_id": "user-456"},
		"context": {},
	}
}

test_provisioning_allowed_below_limit if {
	authz.allow with input as {
		"subject": {"id": "user-123", "role": "user", "authenticated": true},
		"action": "provisioning.create",
		"resource": {"type": "provisioning"},
		"context": {"pending_device_count": 2, "provisioning_limit": 3},
	}
}

test_provisioning_denied_at_limit if {
	not authz.allow with input as {
		"subject": {"id": "user-123", "role": "user", "authenticated": true},
		"action": "provisioning.create",
		"resource": {"type": "provisioning"},
		"context": {"pending_device_count": 3, "provisioning_limit": 3},
	}
}

test_unauthenticated_subject_is_denied if {
	not authz.allow with input as {
		"subject": {"id": "anonymous", "role": "admin", "authenticated": false},
		"action": "nodes.list",
		"resource": {"type": "nodes"},
		"context": {},
	}
}
