#!/usr/bin/env bash
# Fast firmware test gate (shared local + firmware-builder entry point).
#
# Usage (from repo root or anywhere):
#   ./firmware/scripts/run_firmware_fast_gate.sh
#   ./firmware/scripts/run_firmware_fast_gate.sh --host-only
#   ./firmware/scripts/run_firmware_fast_gate.sh --modularity-only
#   ./firmware/scripts/run_firmware_fast_gate.sh --host-only --platform-dsp-only
#   ./firmware/scripts/run_firmware_fast_gate.sh --with-hub-observation-contract
#
# Exit 0 on success; non-zero on any mandatory failure.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SP_AUDIO="${FIRMWARE_ROOT}/platform/audio"
SP_MIC="${FIRMWARE_ROOT}/platform/mic"
EAR_AE="${FIRMWARE_ROOT}/products/dbmap/ear/audio_engine"
EAR_AC="${FIRMWARE_ROOT}/products/dbmap/ear/ear_acoustic"
ESP_EAR="${FIRMWARE_ROOT}/esp-ear"
GATE_ROOT="${FIRMWARE_GATE_ROOT:-${ESP_EAR}/.gate-builds}"

MODE="fast" # fast | host-only | modularity-only
CC_BIN="${CC:-cc}"
KEEP_GATE_BUILDS=0
# Explicit opt-out of Package 1 mandatory EAR host suites (default: required).
PLATFORM_DSP_ONLY=0
# Cross-boundary Observation publish contract. Alone, this is the only work.
WITH_HUB_OBSERVATION_CONTRACT=0
EXPLICIT_MODE=0
CONTRACT_TMP=""

usage() {
  cat <<'EOF'
run_firmware_fast_gate.sh — Ear firmware fast test gate

  (default)             Host DSP/AE/mic tests + Platform acoustic modularity (idf.py)
  --host-only           Host tests only (no ESP-IDF)
  --modularity-only     Isolated idf configure/archive checks only
  --platform-dsp-only   Skip Package 1 mandatory EAR suites
                        (ce_architecture, ear_critical_path, ear_critical_integration).
                        Default is false: those suites are required.
  --with-hub-observation-contract
                        Export seven production Observation JSON reports and run
                        the Hub MQTT ingest contract test against them.
                        Requires host cJSON and the Hub dev environment.
                        When this is the only mode flag, the firmware host gate
                        is not run.
  --keep-builds         Keep isolated gate build dirs under esp-ear/.gate-builds
  -h, --help            Show this help

Environment:
  FIRMWARE_GATE_ROOT  Override isolated build parent (default: esp-ear/.gate-builds)
  CC                  Host C compiler (default: cc)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-only) MODE="host-only"; EXPLICIT_MODE=1; shift ;;
    --modularity-only) MODE="modularity-only"; EXPLICIT_MODE=1; shift ;;
    --platform-dsp-only) PLATFORM_DSP_ONLY=1; shift ;;
    --with-hub-observation-contract) WITH_HUB_OBSERVATION_CONTRACT=1; shift ;;
    --keep-builds) KEEP_GATE_BUILDS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

GATE_START="$(date +%s)"
PASS=0
FAIL=0
SKIP=0
# Unique per-invocation host-binary dir (avoids PID collisions under concurrent gates).
HOST_GATE_TMP=""

log() { printf '%s\n' "$*"; }
section() { printf '\n== %s ==\n' "$*"; }

record_pass() { PASS=$((PASS + 1)); log "PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); log "FAIL  $1"; }
record_skip() { SKIP=$((SKIP + 1)); log "SKIP  $1"; }

cleanup_host_gate_tmp() {
  if [[ -n "${HOST_GATE_TMP:-}" && -d "${HOST_GATE_TMP}" ]]; then
    rm -rf "${HOST_GATE_TMP}"
  fi
  HOST_GATE_TMP=""
}

cleanup_contract_tmp() {
  if [[ -n "${CONTRACT_TMP:-}" && -d "${CONTRACT_TMP}" ]]; then
    rm -rf "${CONTRACT_TMP}"
  fi
  CONTRACT_TMP=""
}

cleanup_gate_tmp() {
  cleanup_host_gate_tmp
  cleanup_contract_tmp
}

trap cleanup_gate_tmp EXIT

have_idf() {
  command -v idf.py >/dev/null 2>&1
}

run_host_one() {
  local name="$1"
  shift
  if [[ -z "${HOST_GATE_TMP:-}" || ! -d "${HOST_GATE_TMP}" ]]; then
    log "FAIL  host/${name} (host gate temp dir missing)"
    record_fail "host/${name}"
    return 1
  fi
  local out="${HOST_GATE_TMP}/${name}"
  # shellcheck disable=SC2068
  if "${CC_BIN}" -std=c11 -Wall -Wextra -O2 $@ -lm -o "${out}"; then
    if "${out}"; then
      record_pass "host/${name}"
      rm -f "${out}"
      return 0
    fi
  fi
  record_fail "host/${name}"
  rm -f "${out}"
  return 1
}

run_host_tests() {
  cleanup_host_gate_tmp
  HOST_GATE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/dbmap-fw-gate.XXXXXX")"

  section "Host Platform acoustic DSP tests"
  local host_fail=0
  local sp_inc="-I${SP_AUDIO}/include"
  run_host_one acoustic_l0 \
    ${sp_inc} \
    "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/test_host/test_acoustic_l0.c" || host_fail=1
  run_host_one acoustic_l0_parity \
    ${sp_inc} \
    "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/test_host/test_acoustic_l0_parity.c" || host_fail=1
  run_host_one acoustic_l1 \
    ${sp_inc} \
    "${SP_AUDIO}/sp_acoustic_l1.c" "${SP_AUDIO}/test_host/test_acoustic_l1.c" || host_fail=1
  run_host_one acoustic_l2 \
    ${sp_inc} \
    "${SP_AUDIO}/sp_acoustic_l1.c" "${SP_AUDIO}/sp_acoustic_l2.c" \
    "${SP_AUDIO}/test_host/test_acoustic_l2.c" || host_fail=1
  run_host_one acoustic_golden \
    ${sp_inc} -I"${SP_AUDIO}/test_host/fixtures" \
    "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/sp_acoustic_l1.c" \
    "${SP_AUDIO}/sp_acoustic_l2.c" \
    "${SP_AUDIO}/test_host/test_acoustic_golden.c" || host_fail=1
  run_host_one audio_analyze_multimic \
    ${sp_inc} \
    -DCONFIG_SP_AUDIO_L0=1 -DCONFIG_SP_AUDIO_L1=1 -DCONFIG_SP_AUDIO_L2=1 \
    "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/sp_acoustic_l1.c" \
    "${SP_AUDIO}/sp_acoustic_l2.c" "${SP_AUDIO}/sp_audio_analyze.c" \
    "${SP_AUDIO}/test_host/test_audio_analyze_multimic.c" || host_fail=1

  section "Host Platform mic tests (migration-relevant)"
  local nc_inc="-I${FIRMWARE_ROOT}/platform/node_common/include"
  local mic_inc="-I${SP_MIC}/include ${nc_inc}"
  run_host_one stereo_unpack \
    ${mic_inc} \
    "${SP_MIC}/i2s_stereo.c" "${SP_MIC}/audio_metrics.c" \
    "${SP_MIC}/test_host/test_stereo_unpack.c" || host_fail=1
  run_host_one sp_mic_window_job \
    ${mic_inc} \
    "${SP_MIC}/sp_mic_window_job.c" "${SP_MIC}/ear_mic_channel_api.c" \
    "${SP_MIC}/audio_metrics.c" \
    "${SP_MIC}/test_host/test_sp_mic_window_job.c" || host_fail=1
  run_host_one ear_mic_channel_api \
    ${mic_inc} \
    "${SP_MIC}/ear_mic_channel_api.c" "${SP_MIC}/audio_metrics.c" \
    "${SP_MIC}/test_host/test_ear_mic_channel_api.c" || host_fail=1
  run_host_one background_rms \
    ${mic_inc} \
    "${SP_MIC}/background_rms.c" \
    "${SP_MIC}/test_host/test_background_rms.c" || host_fail=1
  run_host_one capture_diag_format \
    ${mic_inc} \
    "${SP_MIC}/test_host/test_capture_diag_format.c" || host_fail=1
  run_host_one mic_count \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/mic_count.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_mic_count.c" || host_fail=1

  section "Host Platform connectivity recovery"
  local nc_host_inc="${nc_inc} -I${FIRMWARE_ROOT}/platform/node_common/test_host"
  run_host_one wifi_reconnect_sm \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/wifi_reconnect_sm.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_wifi_reconnect_sm.c" || host_fail=1
  run_host_one backoff \
    ${nc_host_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/backoff.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_backoff.c" || host_fail=1
  run_host_one mqtt_progress \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/mqtt_progress.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_mqtt_progress.c" || host_fail=1
  run_host_one net_dispatch_sm \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/net_dispatch_sm.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_net_dispatch_sm.c" || host_fail=1
  run_host_one connectivity_supervisor_sm \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/connectivity_supervisor_sm.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_connectivity_supervisor_sm.c" || host_fail=1
  run_host_one connectivity_stage3_sm \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/connectivity_stage3_sm.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_connectivity_stage3_sm.c" || host_fail=1
  run_host_one connectivity_stage4_sm \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/connectivity_stage4_sm.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_connectivity_stage4_sm.c" || host_fail=1
  run_host_one diag_memory \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/diag_memory.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_diag_memory.c" || host_fail=1

  section "Host AE regression tests"
  local EAR_CE="${FIRMWARE_ROOT}/products/dbmap/ear/candidate_engine"
  local EAR_AP="${FIRMWARE_ROOT}/products/dbmap/ear/acoustic_profile"
  local ae_inc="-I${EAR_AE}/include -I${SP_AUDIO}/include -I${EAR_AC}/include -I${SP_MIC}/include ${nc_inc} -I${EAR_CE}/include -I${EAR_AP}/include"
  run_host_one ae_align \
    ${ae_inc} \
    "${EAR_AE}/ae_align.c" "${EAR_AE}/ae_signature.c" \
    "${EAR_AE}/test_host/test_ae_align.c" || host_fail=1
  run_host_one ae_stereo \
    ${ae_inc} \
    "${EAR_AE}/ae_align.c" "${EAR_AE}/ae_stereo.c" \
    "${EAR_AE}/test_host/test_ae_stereo.c" || host_fail=1
  run_host_one ae_signature_history \
    ${ae_inc} \
    "${EAR_AE}/ae_signature.c" "${EAR_AE}/ae_history.c" "${EAR_AE}/ae_engine.c" \
    "${EAR_AE}/ae_time.c" \
    "${EAR_AE}/test_host/test_ae_signature_history.c" || host_fail=1
  run_host_one ae_time_contract \
    ${ae_inc} \
    "${EAR_AE}/ae_time.c" "${EAR_AE}/ae_signature.c" \
    "${EAR_AE}/ae_history.c" "${EAR_AE}/ae_engine.c" \
    "${EAR_AE}/test_host/test_ae_time_contract.c" || host_fail=1
  run_host_one ae_window_job_align \
    ${ae_inc} \
    "${EAR_AE}/ae_align.c" "${EAR_AE}/ae_signature.c" \
    "${SP_MIC}/sp_mic_window_job.c" "${SP_MIC}/ear_mic_channel_api.c" \
    "${SP_MIC}/audio_metrics.c" \
    "${EAR_AE}/test_host/test_ae_window_job_align.c" || host_fail=1
  run_host_one ae_stereo_integrity \
    ${ae_inc} \
    "${EAR_AE}/ae_align.c" "${EAR_AE}/ae_stereo.c" "${EAR_AE}/ae_signature.c" \
    "${SP_MIC}/i2s_stereo.c" "${SP_MIC}/audio_metrics.c" \
    "${EAR_AE}/test_host/test_ae_stereo_integrity.c" || host_fail=1
  run_host_one ae_features_from_pcm \
    ${ae_inc} \
    "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/sp_acoustic_l1.c" \
    "${EAR_AE}/ae_signature.c" \
    "${EAR_AE}/test_host/test_ae_features_from_pcm.c" || host_fail=1
  run_host_one ae_window_validity_contract \
    ${ae_inc} \
    "${EAR_AE}/ae_align.c" "${EAR_AE}/ae_gap.c" "${EAR_AE}/ae_time.c" \
    "${EAR_AE}/ae_signature.c" "${EAR_AE}/ae_history.c" "${EAR_AE}/ae_engine.c" \
    "${EAR_AE}/test_host/test_ae_window_validity_contract.c" || host_fail=1
  # Package 5: real ae_device_poll + analyze over the production window slot.
  local EAR_AE_RT="${FIRMWARE_ROOT}/products/dbmap/ear/ae_runtime"
  if [[ -f "${EAR_AE_RT}/test_host/test_ae_device_intake.c" \
    && -f "${EAR_AE_RT}/ae_device.c" \
    && -f "${SP_MIC}/sp_mic_window_job.c" ]]; then
    run_host_one ae_device_intake \
      -DCONFIG_DBMAP_EAR_AUDIO_ENGINE=1 -DCONFIG_SP_AUDIO_L0=1 \
      "-I${EAR_AE_RT}/test_host/host_shim" \
      "-I${EAR_AE_RT}/test_host" \
      "-I${EAR_AE_RT}/include" \
      ${ae_inc} "-I${SP_MIC}" \
      "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/sp_audio_analyze.c" \
      "${SP_MIC}/sp_mic_window_job.c" "${SP_MIC}/ear_mic_channel_api.c" \
      "${SP_MIC}/audio_metrics.c" \
      "${EAR_AC}/ear_acoustic_runtime.c" \
      "${EAR_AE}/ae_align.c" "${EAR_AE}/ae_stereo.c" "${EAR_AE}/ae_gap.c" \
      "${EAR_AE}/ae_signature.c" "${EAR_AE}/ae_history.c" \
      "${EAR_AE}/ae_time.c" "${EAR_AE}/ae_engine.c" \
      "${EAR_AE_RT}/ae_sig_capture.c" \
      "${EAR_AE_RT}/ae_device.c" \
      "${EAR_AE_RT}/test_host/host_mic_facade.c" \
      "${EAR_AE_RT}/test_host/test_ae_device_intake.c" || host_fail=1
  else
    record_fail "host/ae_device_intake (sources/tests not in tree)"
    host_fail=1
  fi
  # Mic diagnostic emit policy (show_zero_error_counters); header-only helper.
  if [[ -f "${SP_MIC}/test_host/test_sp_mic_error_report.c" ]]; then
    run_host_one sp_mic_error_report \
      "-I${SP_MIC}/include" \
      "${SP_MIC}/test_host/test_sp_mic_error_report.c" || host_fail=1
  else
    record_skip "host/sp_mic_error_report (test not in tree)"
  fi
  # M4 links CE module split (decision/module/workflow/manager/profile) plus
  # acoustic_profile. Optional CE/SE/OM suites still SKIP when absent.
  # Package 1 mandatory suites below are required unless --platform-dsp-only.
  local EAR_SE="${FIRMWARE_ROOT}/products/dbmap/ear/signature_engine"
  local EAR_OBS="${FIRMWARE_ROOT}/products/dbmap/ear/ear_observation"
  local EAR_OBS_INC="${EAR_OBS}/include"
  local PLAT_OBS="${FIRMWARE_ROOT}/platform/observation"
  # Fail-closed helper for Package 1 mandatory suites (default mode).
  fail_required_host() {
    local name="$1"
    local why="$2"
    record_fail "host/${name} (${why})"
    host_fail=1
  }

  local ce_srcs_ready=0
  local ce_srcs=()
  if [[ -f "${EAR_CE}/ce_decision.c" && -f "${EAR_CE}/ce_module.c" \
    && -f "${EAR_CE}/ce_workflow.c" && -f "${EAR_CE}/ce_manager.c" \
    && -f "${EAR_CE}/ce_module_profile.c" && -f "${EAR_CE}/ce_engine.c" \
    && -f "${EAR_AP}/as_profile_match.c" \
    && -f "${EAR_AP}/as_profile_registry.c" \
    && -f "${EAR_AP}/as_profile_as001.c" \
    && -f "${EAR_SE}/sig_engine.c" \
    && -f "${EAR_SE}/ear_siren_se_public.c" ]]; then
    ce_srcs_ready=1
    ce_srcs=(
      "${EAR_AE}/ae_signature.c" "${EAR_AE}/ae_history.c"
      "${EAR_AE}/ae_engine.c" "${EAR_AE}/ae_time.c"
      "${EAR_AP}/as_profile_as001.c" "${EAR_AP}/as_profile_registry.c"
      "${EAR_AP}/as_profile_match.c"
      "${EAR_SE}/sig_engine.c" "${EAR_SE}/ear_siren_se_public.c"
      "${EAR_CE}/ce_decision.c" "${EAR_CE}/ce_module.c"
      "${EAR_CE}/ce_module_profile.c" "${EAR_CE}/ce_workflow.c"
      "${EAR_CE}/ce_manager.c" "${EAR_CE}/ce_engine.c"
    )
    ae_inc="${ae_inc} -I${EAR_SE}/include -I${EAR_OBS_INC}"
    run_host_one ae_m4_lifecycle_gap \
      ${ae_inc} \
      "${EAR_AE}/ae_gap.c" "${ce_srcs[@]}" \
      "${EAR_AE}/test_host/test_ae_m4_lifecycle_gap.c" || host_fail=1
    run_host_one ce_lifecycle \
      ${ae_inc} \
      "${ce_srcs[@]}" \
      "${EAR_CE}/test_host/test_ce_lifecycle.c" || host_fail=1
    run_host_one ce_identity \
      ${ae_inc} \
      "${ce_srcs[@]}" \
      "${EAR_CE}/test_host/test_ce_identity.c" || host_fail=1
    if [[ -f "${EAR_AP}/test_host/test_as_profile_as001.c" ]]; then
      run_host_one as_profile_as001 \
        ${ae_inc} \
        "${ce_srcs[@]}" \
        "${EAR_AP}/test_host/test_as_profile_as001.c" || host_fail=1
    else
      record_skip "host/as_profile_as001 (test not in tree)"
    fi
    if [[ -f "${EAR_AP}/test_host/test_as001_as002_compare.c" \
      && -f "${EAR_SE}/ear_siren_se_public.c" ]]; then
      run_host_one as001_as002_compare \
        ${ae_inc} \
        "${EAR_AP}/as_profile_as001.c" "${EAR_AP}/as_profile_registry.c" \
        "${EAR_AP}/as_profile_match.c" \
        "${EAR_SE}/ear_siren_se_public.c" \
        "${EAR_AP}/test_host/test_as001_as002_compare.c" || host_fail=1
    else
      record_skip "host/as001_as002_compare (sources/tests not in tree)"
    fi
    if [[ -f "${EAR_SE}/test_host/test_sig_engine.c" ]]; then
      run_host_one sig_engine \
        ${ae_inc} -DSIG_ENGINE_ALLOW_HOST_HOOKS=1 \
        "${EAR_AP}/as_profile_as001.c" "${EAR_AP}/as_profile_registry.c" \
        "${EAR_AP}/as_profile_match.c" \
        "${EAR_SE}/sig_engine.c" "${EAR_SE}/ear_siren_se_public.c" \
        "${EAR_SE}/test_host/test_sig_engine.c" || host_fail=1
    else
      record_skip "host/sig_engine (test not in tree)"
    fi
    if [[ -f "${EAR_SE}/test_host/test_sig_engine_ce_primary.c" ]]; then
      run_host_one sig_engine_ce_primary \
        ${ae_inc} \
        "${ce_srcs[@]}" \
        "${EAR_SE}/test_host/test_sig_engine_ce_primary.c" || host_fail=1
    else
      record_skip "host/sig_engine_ce_primary (test not in tree)"
    fi
    local EAR_AE_RT="${FIRMWARE_ROOT}/products/dbmap/ear/ae_runtime"
    if [[ -f "${EAR_AE_RT}/test_host/test_ae_sig_capture.c" \
      && -f "${EAR_AE_RT}/ae_sig_capture.c" ]]; then
      run_host_one ae_sig_capture \
        ${ae_inc} "-I${EAR_AE_RT}/include" \
        -DCONFIG_DBMAP_EAR_AE_SIG_CAPTURE=1 \
        "${EAR_AP}/as_profile_as001.c" "${EAR_AP}/as_profile_registry.c" \
        "${EAR_AP}/as_profile_match.c" \
        "${EAR_AE_RT}/ae_sig_capture.c" \
        "${EAR_AE_RT}/test_host/test_ae_sig_capture.c" || host_fail=1
    else
      record_skip "host/ae_sig_capture (sources/tests not in tree)"
    fi
    if [[ -f "${EAR_OBS}/test_host/test_ear_obs_manager.c" \
      && -f "${EAR_OBS}/ear_obs_manager.c" \
      && -f "${EAR_OBS}/ear_promotion_policy.c" \
      && -f "${EAR_OBS}/ear_cls_progressive.c" \
      && -f "${PLAT_OBS}/episode_registry.c" ]]; then
      run_host_one ear_obs_manager \
        ${ae_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${ce_srcs[@]}" \
        "${PLAT_OBS}/episode_registry.c" \
        "${EAR_OBS}/ear_cls_progressive.c" \
        "${EAR_OBS}/ear_cls_as001.c" \
        "${EAR_OBS}/ear_obs_manager.c" \
        "${EAR_OBS}/ear_obs_start_retain.c" \
        "${EAR_OBS}/ear_promotion_policy.c" \
        "${EAR_OBS}/ear_sig_product.c" \
        "${EAR_OBS}/ear_obs_lifecycle_section.c" \
        "${PLAT_OBS}/observation_lifecycle.c" \
        "${EAR_OBS}/test_host/test_ear_obs_manager.c" || host_fail=1
      run_host_one ear_obs_start_publish \
        ${ae_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${PLAT_OBS}/observation_lifecycle.c" \
        "${EAR_OBS}/ear_obs_start_retain.c" \
        "${EAR_OBS}/ear_obs_lifecycle_section.c" \
        "${EAR_OBS}/test_host/test_ear_obs_start_publish.c" || host_fail=1
      run_host_one ear_cls_progressive \
        ${ae_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${ce_srcs[@]}" \
        "${PLAT_OBS}/episode_registry.c" \
        "${EAR_OBS}/ear_cls_progressive.c" \
        "${EAR_OBS}/ear_cls_as001.c" \
        "${EAR_OBS}/ear_obs_manager.c" \
        "${EAR_OBS}/ear_promotion_policy.c" \
        "${EAR_OBS}/test_host/test_ear_cls_progressive.c" || host_fail=1
      run_host_one ear_siren_se_public \
        ${ae_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${ce_srcs[@]}" \
        "${PLAT_OBS}/episode_registry.c" \
        "${EAR_OBS}/ear_cls_progressive.c" \
        "${EAR_OBS}/ear_cls_as001.c" \
        "${EAR_OBS}/ear_obs_manager.c" \
        "${EAR_OBS}/ear_promotion_policy.c" \
        "${EAR_OBS}/test_host/test_ear_siren_se_public.c" || host_fail=1
      # AS-001 assessment fill + Observation JSON (requires IDF cJSON).
      if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/components/json/cJSON/cJSON.c" ]]; then
        local EAR_ACOU_INC="${FIRMWARE_ROOT}/products/dbmap/ear/ear_acoustic/include"
        run_host_one ear_cls_as001 \
          ${ae_inc} "-I${EAR_OBS}/include" "-I${EAR_ACOU_INC}" \
          "-I${IDF_PATH}/components/json/cJSON" \
          "${EAR_OBS}/ear_cls_as001.c" \
          "${EAR_OBS}/observation_payload.c" \
          "${IDF_PATH}/components/json/cJSON/cJSON.c" \
          "${EAR_OBS}/test_host/test_ear_cls_as001.c" || host_fail=1
      else
        record_skip "host/ear_cls_as001 (IDF_PATH/cJSON unavailable)"
      fi
      run_host_one ae_sig_module_independence \
        ${ae_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${ce_srcs[@]}" \
        "${EAR_OBS}/test_host/test_ae_sig_module_independence.c" || host_fail=1
    else
      record_skip "host/ear_obs_manager (OM sources/tests not in tree)"
      record_skip "host/ear_cls_progressive (OM sources/tests not in tree)"
      record_skip "host/ear_siren_se_public (OM sources/tests not in tree)"
      record_skip "host/ear_cls_as001 (OM sources/tests not in tree)"
      record_skip "host/ae_sig_module_independence (OM sources/tests not in tree)"
    fi
  else
    record_skip "host/ae_m4_lifecycle_gap (CE/profile sources not in tree)"
    record_skip "host/ce_lifecycle (CE/profile sources not in tree)"
    record_skip "host/ce_identity (CE/profile sources not in tree)"
    record_skip "host/as_profile_as001 (CE/profile sources not in tree)"
    record_skip "host/ear_obs_manager (CE/profile sources not in tree)"
    record_skip "host/ear_cls_progressive (CE/profile sources not in tree)"
    record_skip "host/ear_siren_se_public (CE/profile sources not in tree)"
    record_skip "host/ear_cls_as001 (CE/profile sources not in tree)"
    record_skip "host/ae_sig_module_independence (CE/profile sources not in tree)"
  fi

  # ------------------------------------------------------------------
  # Package 1 mandatory suites — required by default.
  # Sole applicability skip: explicit --platform-dsp-only.
  # Independent of test_ear_obs_manager.c. Missing sources / compiler
  # inputs / link deps / compile / assert => FAIL (never successful SKIP
  # unless --platform-dsp-only). Keep promotion_bridge / observation_payload
  # ungated.
  # ------------------------------------------------------------------
  local req_inc="${ae_inc}"
  if [[ "${ce_srcs_ready}" -eq 1 ]]; then
    :
  else
    # Still need SE/OBS include dirs for Critical Path compiles.
    req_inc="${ae_inc} -I${EAR_SE}/include -I${EAR_OBS_INC}"
  fi

  if [[ "${PLATFORM_DSP_ONLY}" -eq 1 ]]; then
    log "Platform-DSP-only scope selected"
    record_skip "host/ce_architecture (Platform-DSP-only scope selected)"
    record_skip "host/ear_critical_path (Platform-DSP-only scope selected)"
    record_skip "host/ear_critical_integration (Platform-DSP-only scope selected)"
  else
    # --- ce_architecture (requires complete CE/profile/SE link set) ---
    if [[ "${ce_srcs_ready}" -ne 1 ]]; then
      fail_required_host ce_architecture "required CE/profile/SE sources missing"
    elif [[ ! -f "${EAR_CE}/test_host/test_ce_architecture.c" ]]; then
      fail_required_host ce_architecture "required test source missing"
    elif [[ ! -f "${EAR_OBS}/ce_promotion_bridge.c" ]]; then
      fail_required_host ce_architecture "required production source missing: ce_promotion_bridge.c"
    elif [[ ! -f "${PLAT_OBS}/episode_registry.c" ]]; then
      fail_required_host ce_architecture "required production source missing: episode_registry.c"
    else
      run_host_one ce_architecture \
        ${req_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${ce_srcs[@]}" \
        "${PLAT_OBS}/episode_registry.c" \
        "${EAR_OBS}/ce_promotion_bridge.c" \
        "${EAR_CE}/test_host/test_ce_architecture.c" || host_fail=1
    fi

    # --- ear_critical_path (independent of CE link set / OM test file) ---
    if [[ ! -f "${EAR_OBS}/test_host/test_ear_critical_path.c" ]]; then
      fail_required_host ear_critical_path "required test source missing"
    elif [[ ! -f "${EAR_OBS}/ear_critical_path.c" ]]; then
      fail_required_host ear_critical_path "required production source missing: ear_critical_path.c"
    elif [[ ! -f "${EAR_AE}/ae_signature.c" ]]; then
      fail_required_host ear_critical_path "required production source missing: ae_signature.c"
    elif [[ ! -f "${PLAT_OBS}/episode_registry.c" ]]; then
      fail_required_host ear_critical_path "required production source missing: episode_registry.c"
    else
      run_host_one ear_critical_path \
        ${req_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${EAR_AE}/ae_signature.c" \
        "${EAR_OBS}/ear_critical_path.c" \
        "${PLAT_OBS}/episode_registry.c" \
        "${EAR_OBS}/test_host/test_ear_critical_path.c" || host_fail=1
    fi

    # --- ear_critical_integration (needs CE link set + OM production sources) ---
    if [[ "${ce_srcs_ready}" -ne 1 ]]; then
      fail_required_host ear_critical_integration "required CE/profile/SE sources missing"
    elif [[ ! -f "${EAR_OBS}/test_host/test_ear_critical_integration.c" ]]; then
      fail_required_host ear_critical_integration "required test source missing"
    elif [[ ! -f "${EAR_OBS}/ear_critical_path.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_critical_path.c"
    elif [[ ! -f "${EAR_OBS}/ear_obs_ota_cleanup.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_obs_ota_cleanup.c"
    elif [[ ! -f "${EAR_OBS}/ear_obs_manager.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_obs_manager.c"
    elif [[ ! -f "${EAR_OBS}/ear_promotion_policy.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_promotion_policy.c"
    elif [[ ! -f "${EAR_OBS}/ear_sig_product.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_sig_product.c"
    elif [[ ! -f "${EAR_OBS}/ear_cls_progressive.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_cls_progressive.c"
    elif [[ ! -f "${EAR_OBS}/ear_cls_as001.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: ear_cls_as001.c"
    elif [[ ! -f "${PLAT_OBS}/episode_registry.c" ]]; then
      fail_required_host ear_critical_integration "required production source missing: episode_registry.c"
    else
      run_host_one ear_critical_integration \
        ${req_inc} "-I${EAR_OBS}/include" "-I${PLAT_OBS}/include" \
        "${ce_srcs[@]}" \
        "${PLAT_OBS}/episode_registry.c" \
        "${EAR_OBS}/ear_cls_progressive.c" \
        "${EAR_OBS}/ear_cls_as001.c" \
        "${EAR_OBS}/ear_obs_manager.c" \
        "${EAR_OBS}/ear_promotion_policy.c" \
        "${EAR_OBS}/ear_sig_product.c" \
        "${EAR_OBS}/ear_obs_lifecycle_section.c" \
        "${PLAT_OBS}/observation_lifecycle.c" \
        "${EAR_OBS}/ear_critical_path.c" \
        "${EAR_OBS}/ear_obs_ota_cleanup.c" \
        "${EAR_OBS}/test_host/test_ear_critical_integration.c" || host_fail=1
    fi
  fi

  cleanup_host_gate_tmp
  return "${host_fail}"
}

assert_cmake_modularity_structure() {
  section "CMake modularity structure (Platform audio)"
  local cmake="${SP_AUDIO}/CMakeLists.txt"
  local ok=1
  if ! grep -q 'if(CONFIG_SP_AUDIO_L0)' "${cmake}" \
    || ! grep -q 'if(CONFIG_SP_AUDIO_L1)' "${cmake}" \
    || ! grep -q 'if(CONFIG_SP_AUDIO_L2)' "${cmake}" \
    || ! grep -q 'list(APPEND srcs "sp_acoustic_l0.c")' "${cmake}" \
    || ! grep -q 'list(APPEND srcs "sp_acoustic_l1.c")' "${cmake}" \
    || ! grep -q 'list(APPEND srcs "sp_acoustic_l2.c")' "${cmake}"; then
    ok=0
  fi
  # EAR must not own algorithm .c files anymore
  if [[ -f "${EAR_AC}/acoustic_l0.c" || -f "${EAR_AC}/acoustic_l1.c" || -f "${EAR_AC}/acoustic_l2.c" ]]; then
    log "  stale EAR acoustic_l*.c still present"
    ok=0
  fi
  # Platform DSP sources must not include product paths/headers
  if grep -R -n 'products/dbmap\|dbmap_ear_\|ear_observation\|ae_engine' \
      "${SP_AUDIO}/sp_acoustic_l0.c" "${SP_AUDIO}/sp_acoustic_l1.c" \
      "${SP_AUDIO}/sp_acoustic_l2.c" "${SP_AUDIO}/sp_audio_analyze.c" \
      "${SP_AUDIO}/include" 2>/dev/null; then
    log "  Platform DSP references EAR/DBMAP product code"
    ok=0
  fi
  # Platform mic Kconfig must be generic (no EAR node-type / EAR_MIC ownership)
  local mic_kconfig="${SP_MIC}/Kconfig"
  if grep -E 'DBMAP_NODE_TYPE_EAR|DBMAP_EAR_MIC_' "${mic_kconfig}" >/dev/null 2>&1; then
    log "  Platform mic Kconfig still references EAR-specific symbols"
    ok=0
  fi
  if ! grep -q 'SP_MIC_SAMPLE_RATE_HZ' "${mic_kconfig}" \
    || ! grep -q 'SP_MIC_WINDOW_MS' "${mic_kconfig}"; then
    log "  Platform mic Kconfig missing generic SP_MIC_* symbols"
    ok=0
  fi
  if [[ "${ok}" -eq 1 ]]; then
    record_pass "cmake/conditional_srcs"
    record_pass "cmake/platform_no_ear_deps"
    record_pass "cmake/platform_mic_kconfig_generic"
    return 0
  fi
  record_fail "cmake/conditional_srcs"
  return 1
}

archive_members() {
  local archive="$1"
  ar t "${archive}" | sort
}

expect_members() {
  local label="$1"
  local archive="$2"
  shift 2
  local expected=("$@")
  local got
  got="$(archive_members "${archive}")"
  local missing=0
  local exp
  for exp in "${expected[@]}"; do
    if ! printf '%s\n' "${got}" | grep -qx "${exp}"; then
      log "  missing member: ${exp}"
      missing=1
    fi
  done
  local member
  while IFS= read -r member; do
    case "${member}" in
      sp_acoustic_l*.c.obj|acoustic_l*.c.obj)
        local ok=0
        for exp in "${expected[@]}"; do
          if [[ "${member}" == "${exp}" ]]; then ok=1; break; fi
        done
        if [[ "${ok}" -eq 0 ]]; then
          log "  unexpected member: ${member}"
          missing=1
        fi
        ;;
    esac
  done <<< "${got}"
  if [[ "${missing}" -eq 0 ]]; then
    record_pass "${label}"
    return 0
  fi
  log "  archive members were:"
  while IFS= read -r member; do
    log "    ${member}"
  done <<< "${got}"
  record_fail "${label}"
  return 1
}

write_overlay() {
  local path="$1"
  shift
  printf '%s\n' "$@" > "${path}"
}

configure_isolated() {
  local name="$1"
  local overlay="$2"
  local build_dir="${GATE_ROOT}/${name}"
  local sdkconfig="${build_dir}/sdkconfig"
  rm -rf "${build_dir}"
  mkdir -p "${build_dir}"
  (
    cd "${ESP_EAR}"
    idf.py -B "${build_dir}" \
      -D "SDKCONFIG=${sdkconfig}" \
      -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;${overlay}" \
      set-target esp32s3
  ) >/dev/null
}

build_audio_archive() {
  local build_dir="$1"
  ninja -C "${build_dir}" esp-idf/audio/libaudio.a >/dev/null
}

build_ear_acoustic_archive() {
  local build_dir="$1"
  ninja -C "${build_dir}" esp-idf/ear_acoustic/libear_acoustic.a >/dev/null
}

run_modularity_checks() {
  section "Acoustic modularity (isolated IDF builds)"
  if ! have_idf; then
    record_fail "modularity/idf.py_required_but_missing"
    return 1
  fi

  mkdir -p "${GATE_ROOT}"
  local overlays="${GATE_ROOT}/overlays"
  mkdir -p "${overlays}"
  local mod_fail=0
  local project_sdk_hash=""
  if [[ -f "${ESP_EAR}/sdkconfig" ]]; then
    project_sdk_hash="$(cksum "${ESP_EAR}/sdkconfig" | awk '{print $1" "$2}')"
  fi

  # Product overlays still toggle EAR symbols; those select SP_AUDIO_*.
  write_overlay "${overlays}/l0_only.defaults" \
    '# CONFIG_DBMAP_EAR_OBS_L1 is not set' \
    '# CONFIG_DBMAP_EAR_OBS_L2 is not set' \
    '# CONFIG_DBMAP_EAR_OBS_L2_PUBLISH is not set' \
    '# CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2 is not set' \
    '# CONFIG_SP_AUDIO_L1 is not set' \
    '# CONFIG_SP_AUDIO_L2 is not set'

  write_overlay "${overlays}/l0_l1.defaults" \
    '# CONFIG_DBMAP_EAR_OBS_L2 is not set' \
    '# CONFIG_DBMAP_EAR_OBS_L2_PUBLISH is not set' \
    '# CONFIG_DBMAP_EAR_OBS_LIFECYCLE_V2 is not set' \
    '# CONFIG_SP_AUDIO_L2 is not set'

  write_overlay "${overlays}/invalid_l2.defaults" \
    '# CONFIG_DBMAP_EAR_OBS_L1 is not set' \
    'CONFIG_DBMAP_EAR_OBS_L2=y' \
    '# CONFIG_SP_AUDIO_L1 is not set' \
    'CONFIG_SP_AUDIO_L2=y'

  # A) Normal EAR defaults → L0+L1+L2 on Platform audio component
  log "Configuring EAR defaults (L0+L1+L2)…"
  local ear_build="${GATE_ROOT}/ear-full"
  local ear_sdk="${ear_build}/sdkconfig"
  rm -rf "${ear_build}"
  mkdir -p "${ear_build}"
  (
    cd "${ESP_EAR}"
    idf.py -B "${ear_build}" \
      -D "SDKCONFIG=${ear_sdk}" \
      -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults" \
      set-target esp32s3
  ) >/dev/null
  build_audio_archive "${ear_build}"
  expect_members "modularity/ear_full_audio_archive" \
    "${ear_build}/esp-idf/audio/libaudio.a" \
    sp_acoustic_l0.c.obj sp_audio_analyze.c.obj \
    sp_acoustic_l1.c.obj sp_acoustic_l2.c.obj \
    || mod_fail=1
  build_ear_acoustic_archive "${ear_build}"
  expect_members "modularity/ear_runtime_archive" \
    "${ear_build}/esp-idf/ear_acoustic/libear_acoustic.a" \
    ear_acoustic_runtime.c.obj \
    || mod_fail=1
  if ! grep -q '^CONFIG_SP_AUDIO_L0=y$' "${ear_sdk}" \
    || ! grep -q '^CONFIG_SP_AUDIO_L1=y$' "${ear_sdk}" \
    || ! grep -q '^CONFIG_SP_AUDIO_L2=y$' "${ear_sdk}" \
    || ! grep -q '^CONFIG_SP_MIC_PCM_RETAIN=y$' "${ear_sdk}"; then
    record_fail "modularity/ear_full_sdkconfig_flags"
    mod_fail=1
  else
    record_pass "modularity/ear_full_sdkconfig_flags"
  fi

  # B) L0-only
  log "Configuring L0-only…"
  configure_isolated "l0-only" "${overlays}/l0_only.defaults"
  build_audio_archive "${GATE_ROOT}/l0-only"
  expect_members "modularity/l0_only_archive" \
    "${GATE_ROOT}/l0-only/esp-idf/audio/libaudio.a" \
    sp_acoustic_l0.c.obj sp_audio_analyze.c.obj \
    || mod_fail=1
  if grep -q 'CONFIG_SP_AUDIO_L1 1' "${GATE_ROOT}/l0-only/config/sdkconfig.h" 2>/dev/null \
    || grep -q 'CONFIG_SP_AUDIO_L2 1' "${GATE_ROOT}/l0-only/config/sdkconfig.h" 2>/dev/null; then
    record_fail "modularity/l0_only_sdkconfig_flags"
    mod_fail=1
  else
    record_pass "modularity/l0_only_sdkconfig_flags"
  fi

  # C) L0+L1
  log "Configuring L0+L1…"
  configure_isolated "l0-l1" "${overlays}/l0_l1.defaults"
  build_audio_archive "${GATE_ROOT}/l0-l1"
  expect_members "modularity/l0_l1_archive" \
    "${GATE_ROOT}/l0-l1/esp-idf/audio/libaudio.a" \
    sp_acoustic_l0.c.obj sp_audio_analyze.c.obj sp_acoustic_l1.c.obj \
    || mod_fail=1

  # D) Invalid L2 without L1 — Kconfig must drop L2
  log "Configuring invalid L2-without-L1…"
  configure_isolated "invalid-l2" "${overlays}/invalid_l2.defaults"
  local inv_sdk="${GATE_ROOT}/invalid-l2/sdkconfig"
  if grep -q '^CONFIG_SP_AUDIO_L1=y$' "${inv_sdk}" 2>/dev/null \
    || grep -q '^CONFIG_DBMAP_EAR_OBS_L1=y$' "${inv_sdk}" 2>/dev/null; then
    record_fail "modularity/invalid_l2_l1_must_be_off"
    mod_fail=1
  else
    record_pass "modularity/invalid_l2_l1_must_be_off"
  fi
  if grep -q '^CONFIG_SP_AUDIO_L2=y$' "${inv_sdk}" 2>/dev/null \
    || grep -q '^CONFIG_DBMAP_EAR_OBS_L2=y$' "${inv_sdk}" 2>/dev/null; then
    record_fail "modularity/invalid_l2_rejected"
    mod_fail=1
  else
    record_pass "modularity/invalid_l2_rejected"
  fi
  if grep -q 'sp_acoustic_l1\.c' "${GATE_ROOT}/invalid-l2/build.ninja" \
    || grep -q 'sp_acoustic_l2\.c' "${GATE_ROOT}/invalid-l2/build.ninja"; then
    record_fail "modularity/invalid_l2_ninja_excludes_l1_l2"
    mod_fail=1
  else
    record_pass "modularity/invalid_l2_ninja_excludes_l1_l2"
  fi

  if [[ -n "${project_sdk_hash}" ]]; then
    local after_hash
    after_hash="$(cksum "${ESP_EAR}/sdkconfig" | awk '{print $1" "$2}')"
    if [[ "${after_hash}" == "${project_sdk_hash}" ]]; then
      record_pass "modularity/project_sdkconfig_untouched"
    else
      record_fail "modularity/project_sdkconfig_untouched"
      mod_fail=1
    fi
  else
    record_pass "modularity/project_sdkconfig_absent_ok"
  fi

  if [[ "${KEEP_GATE_BUILDS}" -eq 0 ]]; then
    rm -rf "${GATE_ROOT}"
  fi
  return "${mod_fail}"
}

run_observation_publish_contract() {
  section "Firmware-to-Hub observation publish contract"
  local repo_root hub ear_obs ear_ae ear_ac ear_ce ear_ap ear_se plat_obs plat_audio nc
  repo_root="$(cd "${FIRMWARE_ROOT}/.." && pwd)"
  hub="${repo_root}/hub/bootstrap"
  ear_obs="${FIRMWARE_ROOT}/products/dbmap/ear/ear_observation"
  ear_ae="${FIRMWARE_ROOT}/products/dbmap/ear/audio_engine"
  ear_ac="${FIRMWARE_ROOT}/products/dbmap/ear/ear_acoustic"
  ear_ce="${FIRMWARE_ROOT}/products/dbmap/ear/candidate_engine"
  ear_ap="${FIRMWARE_ROOT}/products/dbmap/ear/acoustic_profile"
  ear_se="${FIRMWARE_ROOT}/products/dbmap/ear/signature_engine"
  plat_obs="${FIRMWARE_ROOT}/platform/observation"
  plat_audio="${FIRMWARE_ROOT}/platform/audio"
  nc="${FIRMWARE_ROOT}/platform/node_common"

  local missing=0
  local required=(
    "${ear_obs}/test_host/test_ear_critical_integration.c"
    "${ear_obs}/observation_payload.c"
    "${ear_obs}/ear_critical_path.c"
    "${ear_obs}/ear_obs_manager.c"
    "${ear_obs}/ear_cls_as001.c"
    "${ear_obs}/ear_cls_progressive.c"
    "${nc}/identity.c"
    "${hub}/pyproject.toml"
    "${hub}/tests/test_observation_publish_contract.py"
  )
  local path
  for path in "${required[@]}"; do
    if [[ ! -f "${path}" ]]; then
      log "FAIL  contract missing ${path}"
      missing=1
    fi
  done
  if ! command -v uv >/dev/null 2>&1; then
    log "FAIL  contract missing uv"
    missing=1
  fi
  local cjson_inc=""
  local cjson_src=""
  local cjson_lib=""
  if [[ -n "${IDF_PATH:-}" && -f "${IDF_PATH}/components/json/cJSON/cJSON.c" ]]; then
    cjson_inc="-I${IDF_PATH}/components/json/cJSON"
    cjson_src="${IDF_PATH}/components/json/cJSON/cJSON.c"
  elif [[ -f /usr/include/cjson/cJSON.h ]]; then
    cjson_inc="-I/usr/include/cjson"
    cjson_lib="-lcjson"
  elif [[ -f /usr/include/cJSON.h ]]; then
    cjson_lib="-lcjson"
  else
    log "FAIL  contract missing cJSON (IDF_PATH or libcjson-dev)"
    missing=1
  fi
  if [[ "${missing}" -ne 0 ]]; then
    record_fail "contract/dependencies"
    return 1
  fi

  cleanup_contract_tmp
  CONTRACT_TMP="$(mktemp -d "${TMPDIR:-/tmp}/dbmap-obs-contract.XXXXXX")"
  local reports="${CONTRACT_TMP}/reports"
  mkdir -p "${reports}"
  local bin="${CONTRACT_TMP}/export"
  local inc=(
    -I"${ear_ae}/include" -I"${plat_audio}/include" -I"${ear_ac}/include"
    -I"${FIRMWARE_ROOT}/platform/mic/include" -I"${nc}/include" -I"${nc}/test_host"
    -I"${ear_ce}/include" -I"${ear_ap}/include" -I"${ear_se}/include"
    -I"${ear_obs}/include" -I"${plat_obs}/include"
  )
  # shellcheck disable=SC2086
  if ! "${CC_BIN}" -std=c11 -Wall -Wextra -O2 \
      -DDBMAP_OBS_PUBLISH_CONTRACT_EXPORT \
      -DCONFIG_DBMAP_NODE_TYPE_EAR=1 \
      "${inc[@]}" ${cjson_inc} \
      "${ear_ae}/ae_signature.c" "${ear_ae}/ae_history.c" \
      "${ear_ae}/ae_engine.c" "${ear_ae}/ae_time.c" \
      "${ear_ap}/as_profile_as001.c" "${ear_ap}/as_profile_registry.c" \
      "${ear_ap}/as_profile_match.c" \
      "${ear_se}/sig_engine.c" "${ear_se}/ear_siren_se_public.c" \
      "${ear_ce}/ce_decision.c" "${ear_ce}/ce_module.c" \
      "${ear_ce}/ce_module_profile.c" "${ear_ce}/ce_workflow.c" \
      "${ear_ce}/ce_manager.c" "${ear_ce}/ce_engine.c" \
      "${plat_obs}/episode_registry.c" \
      "${ear_obs}/ear_cls_progressive.c" "${ear_obs}/ear_cls_as001.c" \
      "${ear_obs}/ear_obs_manager.c" "${ear_obs}/ear_promotion_policy.c" \
      "${ear_obs}/ear_sig_product.c" "${ear_obs}/ear_obs_lifecycle_section.c" \
      "${plat_obs}/observation_lifecycle.c" \
      "${ear_obs}/ear_critical_path.c" \
      "${ear_obs}/ear_obs_ota_cleanup.c" "${ear_obs}/observation_payload.c" \
      ${cjson_src} \
      "${nc}/identity.c" \
      "${ear_obs}/test_host/test_ear_critical_integration.c" \
      ${cjson_lib} -lm -o "${bin}"; then
    record_fail "contract/c_export_compile"
    return 1
  fi
  if ! "${bin}" --export-publish-contract "${reports}"; then
    record_fail "contract/c_export"
    return 1
  fi
  local report_count
  report_count="$(find "${reports}" -maxdepth 1 -name 'report-*.json' | wc -l | tr -d ' ')"
  if [[ "${report_count}" != "7" || ! -f "${reports}/topic.txt" ]]; then
    log "FAIL  contract report count=${report_count}"
    record_fail "contract/report_count"
    return 1
  fi
  # Each Hub command is checked on its own. A subshell under "if !" disables
  # errexit, so a later successful Ruff run must not hide a failed sync or pytest.
  local sync_rc=0
  ( cd "${hub}" && uv sync --extra dev --frozen ) || sync_rc=$?
  if [[ "${sync_rc}" -ne 0 ]]; then
    log "FAIL  contract hub sync rc=${sync_rc}"
    record_fail "contract/hub_sync"
    return 1
  fi
  local pytest_rc=0
  local pytest_tmp="${CONTRACT_TMP}/pytest"
  mkdir -p "${pytest_tmp}"
  (
    cd "${hub}" && DBMAP_OBS_CONTRACT_DIR="${reports}" \
      uv run pytest tests/test_observation_publish_contract.py -q \
      --basetemp "${pytest_tmp}"
  ) || pytest_rc=$?
  if [[ "${pytest_rc}" -ne 0 ]]; then
    log "FAIL  contract hub pytest rc=${pytest_rc}"
    record_fail "contract/hub_pytest"
    return 1
  fi
  local ruff_rc=0
  ( cd "${hub}" && uv run ruff check tests/test_observation_publish_contract.py ) || ruff_rc=$?
  if [[ "${ruff_rc}" -ne 0 ]]; then
    log "FAIL  contract hub ruff rc=${ruff_rc}"
    record_fail "contract/hub_ruff"
    return 1
  fi
  record_pass "contract/observation_publish"
  cleanup_contract_tmp
  return 0
}

overall_fail=0

if [[ "${WITH_HUB_OBSERVATION_CONTRACT}" -eq 1 && "${EXPLICIT_MODE}" -eq 0 ]]; then
  run_observation_publish_contract || overall_fail=1
else
case "${MODE}" in
  host-only)
    assert_cmake_modularity_structure || overall_fail=1
    run_host_tests || overall_fail=1
    ;;
  modularity-only)
    assert_cmake_modularity_structure || overall_fail=1
    run_modularity_checks || overall_fail=1
    ;;
  fast)
    assert_cmake_modularity_structure || overall_fail=1
    run_host_tests || overall_fail=1
    if have_idf; then
      run_modularity_checks || overall_fail=1
    else
      record_fail "modularity/idf.py_required_for_default_fast_gate"
      overall_fail=1
      log "Hint: source ESP-IDF export.sh, or use --host-only when IDF is unavailable."
    fi
    ;;
esac
  if [[ "${WITH_HUB_OBSERVATION_CONTRACT}" -eq 1 ]]; then
    run_observation_publish_contract || overall_fail=1
  fi
fi

GATE_END="$(date +%s)"
DURATION=$((GATE_END - GATE_START))
section "Summary"
log "pass=${PASS} fail=${FAIL} skip=${SKIP} duration_s=${DURATION} mode=${MODE}"
if [[ "${overall_fail}" -ne 0 || "${FAIL}" -ne 0 ]]; then
  log "RESULT: FAIL"
  exit 1
fi
log "RESULT: PASS"
exit 0
