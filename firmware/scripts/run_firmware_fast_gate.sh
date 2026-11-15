#!/usr/bin/env bash
# Fast firmware test gate (shared local + firmware-builder entry point).
#
# Usage (from repo root or anywhere):
#   ./firmware/scripts/run_firmware_fast_gate.sh
#   ./firmware/scripts/run_firmware_fast_gate.sh --host-only
#   ./firmware/scripts/run_firmware_fast_gate.sh --modularity-only
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

usage() {
  cat <<'EOF'
run_firmware_fast_gate.sh — Ear firmware fast test gate

  (default)           Host DSP/AE/mic tests + Platform acoustic modularity (idf.py)
  --host-only         Host tests only (no ESP-IDF)
  --modularity-only   Isolated idf configure/archive checks only
  --keep-builds       Keep isolated gate build dirs under esp-ear/.gate-builds
  -h, --help          Show this help

Environment:
  FIRMWARE_GATE_ROOT  Override isolated build parent (default: esp-ear/.gate-builds)
  CC                  Host C compiler (default: cc)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-only) MODE="host-only"; shift ;;
    --modularity-only) MODE="modularity-only"; shift ;;
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

log() { printf '%s\n' "$*"; }
section() { printf '\n== %s ==\n' "$*"; }

record_pass() { PASS=$((PASS + 1)); log "PASS  $1"; }
record_fail() { FAIL=$((FAIL + 1)); log "FAIL  $1"; }
record_skip() { SKIP=$((SKIP + 1)); log "SKIP  $1"; }

have_idf() {
  command -v idf.py >/dev/null 2>&1
}

run_host_one() {
  local name="$1"
  shift
  local out="/tmp/dbmap-fw-gate-${name}-$$"
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
  run_host_one mic_count \
    ${nc_inc} \
    "${FIRMWARE_ROOT}/platform/node_common/mic_count.c" \
    "${FIRMWARE_ROOT}/platform/node_common/test_host/test_mic_count.c" || host_fail=1

  section "Host AE regression tests"
  local EAR_CE="${FIRMWARE_ROOT}/products/dbmap/ear/candidate_engine"
  local ae_inc="-I${EAR_AE}/include -I${SP_AUDIO}/include -I${EAR_AC}/include -I${SP_MIC}/include ${nc_inc} -I${EAR_CE}/include"
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
  run_host_one ae_m4_lifecycle_gap \
    ${ae_inc} \
    "${EAR_AE}/ae_gap.c" "${EAR_AE}/ae_signature.c" "${EAR_AE}/ae_history.c" \
    "${EAR_AE}/ae_engine.c" "${EAR_AE}/ae_time.c" \
    "${EAR_CE}/ce_decision.c" "${EAR_CE}/ce_module.c" "${EAR_CE}/ce_workflow.c" \
    "${EAR_CE}/ce_manager.c" "${EAR_CE}/ce_engine.c" \
    "${EAR_AE}/test_host/test_ae_m4_lifecycle_gap.c" || host_fail=1

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
      "${SP_AUDIO}/sp_acoustic_l2.c" "${SP_AUDIO}/include" 2>/dev/null; then
    log "  Platform DSP references EAR/DBMAP product code"
    ok=0
  fi
  if [[ "${ok}" -eq 1 ]]; then
    record_pass "cmake/conditional_srcs"
    record_pass "cmake/platform_no_ear_deps"
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
    sp_acoustic_l0.c.obj sp_acoustic_l1.c.obj sp_acoustic_l2.c.obj \
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
    sp_acoustic_l0.c.obj \
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
    sp_acoustic_l0.c.obj sp_acoustic_l1.c.obj \
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

overall_fail=0

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
