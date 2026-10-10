package dbmap.authz

import rego.v1

default allow := false

# The local administrator is the break-glass role and may perform all API actions.
allow if {
	input.subject.authenticated == true
	input.subject.role == "admin"
}

# Local viewers can use the session endpoint and see the public device summary.
allow if {
	input.subject.authenticated == true
	input.subject.role == "viewer"
	input.action in {"session.read", "devices.list_summary", "hub.status.read", "observations.read"}
}

# Authenticated external users may see the shared device summary, but not details.
allow if {
	input.subject.authenticated == true
	input.subject.role == "user"
	input.action == "devices.list_summary"
}

# A future authenticated user may inspect or update a device they own.
allow if {
	input.subject.authenticated == true
	input.subject.role == "user"
	input.action in {"nodes.read_detail", "nodes.update_desired", "nodes.update_installation"}
	input.resource.owner_id == input.subject.id
}

# Provisioning slots are consumed by pending devices and supplied as policy context.
allow if {
	input.subject.authenticated == true
	input.subject.role == "user"
	input.action == "provisioning.create"
	input.context.pending_device_count < input.context.provisioning_limit
}
