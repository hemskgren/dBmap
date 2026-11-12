#pragma once

/**
 * Machine-readable USB serial provisioning protocol tokens (Sensor Platform).
 * Human diagnostics use ESP_LOG tag ``sp_serial``; these lines are the wire contract.
 *
 * Historical ``DBMAP_PROVISION_*`` tokens are no longer emitted. Hub web UI accepts
 * both SP_* and legacy DBMAP_* while older flashed images remain in the field.
 */
#define SP_PROVISION_READY_TOKEN "SP_PROVISION_READY"
#define SP_PROVISION_RESULT_PREFIX "SP_PROVISION_RESULT:"
