package dbmap.authz

import rego.v1

default allow := false

# The local administrator is the break-glass role and may perform all API actions.
allow if {
	input.subject.authenticated == true
	input.subject.role == "admin"
}

# Local viewers can read shared status, device details, and the internal user directory.
allow if {
	input.subject.authenticated == true
	input.subject.role == "viewer"
	input.action in {
		"session.read",
		"devices.list_summary",
		"nodes.read_detail",
		"hub.status.read",
		"hub.logs.read",
		"observations.read",
		"users.read",
		"groups.read",
		"firmware.catalog.read",
	}
}

# Authenticated external users may see the shared device summary, but not details.
allow if {
	input.subject.authenticated == true
	input.subject.role == "user"
	input.action == "devices.list_summary"
}

# An authenticated owner may read the device's detailed status. Configuration,
# lifecycle, ownership, and installation changes remain local-admin operations.
allow if {
	input.subject.authenticated == true
	input.subject.role == "user"
	input.action == "nodes.read_detail"
	input.resource.owner_id == input.subject.id
}

# Provisioning slots are consumed by pending devices and supplied as policy context.
allow if {
	input.subject.authenticated == true
	input.subject.role == "user"
	input.action == "provisioning.create"
	input.context.pending_device_count < input.context.provisioning_limit
}
