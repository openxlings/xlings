#!/usr/bin/env bash
# tests/e2e/run_all.sh — run the release-artifact E2E block (E2E-02..E2E-42)
# with per-test timing + a slowest-first summary, mirroring mcpp's runner.
#
# Usage:  bash tests/e2e/run_all.sh <release-tarball>
#         (release tarball = build/release.tar.gz, produced by linux_release.sh)
#
# The pre-release tests E2E-00 (mcpp builds xlings) and E2E-01 (bootstrap home)
# stay as discrete workflow steps because they're interleaved with the release
# build; everything from E2E-02 onward runs here against the built artifact.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"

TARBALL="${1:-build/release.tar.gz}"
if [[ ! -f "$TARBALL" ]]; then
    echo "FATAL: release tarball not found at $TARBALL"
    echo "Run tools/linux_release.sh first (or pass the path)."
    exit 1
fi
TARBALL="$(cd "$(dirname "$TARBALL")" && pwd)/$(basename "$TARBALL")"  # absolute

# CI is non-interactive; several install tests need this and it's harmless
# for the rest. (E2E-03 used to set it inline.)
export XLINGS_NON_INTERACTIVE=1

# ── portable millisecond timer (bash 5 EPOCHREALTIME, else whole-second date) ──
_t_ms() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        local er=${EPOCHREALTIME} s us
        s=${er%.*}; us=${er#*.}
        echo $(( 10#$s * 1000 + 10#$us / 1000 ))
    else
        echo $(( $(date +%s) * 1000 ))
    fi
}
_fmt_ms() {
    local ms=$1
    if (( ms < 1000 )); then echo "${ms}ms"; else
        printf '%d.%02ds' $(( ms / 1000 )) $(( (ms % 1000) / 10 ))
    fi
}

# ── test manifest: "LABEL|SCRIPT|ARG|FLAGS" ──
#   ARG   = "T" → pass the release tarball, "" → no arg
#   FLAGS = "allowfail" → a non-zero exit is tolerated (e.g. GitHub rate limits)
TESTS=(
    "E2E-02 |release_self_install_test.sh|T|"
    "E2E-02a|release_packaged_index_test.sh|T|"
    "E2E-02b|self_uninstall_test.sh|T|"
    "E2E-03 |release_quick_install_test.sh||allowfail"
    "E2E-04 |release_subos_smoke_test.sh|T|"
    "E2E-05 |project_e2e_test.sh||"
    "E2E-06 |sub_index_search_test.sh||"
    "E2E-07 |sub_index_install_test.sh||"
    "E2E-07a|install_subindex_first_run_test.sh||"
    "E2E-07b|install_refresh_on_missing_test.sh||"
    "E2E-08 |index_cache_test.sh||"
    "E2E-09 |legacy_config_test.sh||"
    "E2E-10 |pkginfo_install_dir_test.sh||"
    "E2E-11 |script_type_install_test.sh||"
    "E2E-12 |elfpatch_install_verify_test.sh|T|"
    "E2E-13 |remove_multi_version_test.sh||"
    "E2E-14 |tui_utf8_test.sh||"
    "E2E-15 |xlings_self_replace_test.sh||"
    "E2E-16 |self_doctor_test.sh||"
    "E2E-17 |cli_target_compat_test.sh||"
    "E2E-18 |cli_short_alias_removal_test.sh||"
    "E2E-19 |mirror_fallback_test.sh||"
    "E2E-20 |remove_self_guard_test.sh||"
    "E2E-21 |build_deps_split_test.sh||"
    "E2E-22 |subos_workspace_c2_schema_test.sh||"
    "E2E-23 |subos_install_remove_isolation_test.sh||"
    "E2E-24 |nested_xlings_home_test.sh||"
    "E2E-25 |subos_sandbox_test.sh||"
    "E2E-26 |subos_sandbox_gpu_test.sh||"
    "E2E-27 |install_silent_failure_test.sh||"
    "E2E-28 |shim_owner_anchoring_test.sh||"
    "E2E-29 |interface_multi_repo_error_visibility_test.sh||"
    "E2E-30 |custom_index_artifact_test.sh||"
    "E2E-31 |index_same_name_namespace_test.sh||"
    "E2E-32 |xvm_group_switch_test.sh||"
    "E2E-33 |home_config_lock_test.sh||"
    "E2E-34 |xvm_library_switch_test.sh||"
    "E2E-35 |xpkg_spec_gate_test.sh||"
    "E2E-36 |xvm_metadata_reset_test.sh||"
    "E2E-37 |xvm_files_probe_compat_test.sh||"
    "E2E-38 |self_update_failure_test.sh||"
    "E2E-39 |subos_profile_upgrade_test.sh||"
    "E2E-40 |subos_shell_level_test.sh||"
    "E2E-41 |index_artifact_update_test.sh||"
    "E2E-42 |self_doctor_multi_subos_test.sh||"
    "E2E-43 |declared_programs_verified_test.sh||"
    "E2E-44 |tui_output_contract_test.sh||"
    "E2E-45 |self_doctor_anchor_shim_test.sh||"
    "E2E-46 |subos_alias_sysroot_test.sh||"
    "E2E-47 |foreign_payload_reinstall_test.sh||"
    "E2E-48 |non_interactive_contract_test.sh||"
    "E2E-49 |group_switch_report_test.sh||"
    "E2E-50 |subos_scope_authority_test.sh||"
    "E2E-51 |install_use_semantics_test.sh||"
    "E2E-52 |inactive_installed_test.sh||"
    "E2E-53 |doctor_fix_convergence_test.sh||"
    "E2E-54 |fresh_xlings_home_install_test.sh||"
    "E2E-55 |list_exact_inventory_test.sh||"
    "E2E-56 |info_output_contract_test.sh||"
    "E2E-57 |subos_use_process_model_test.sh||"
    "E2E-58 |arch_evidence_contract_test.sh||"
    "E2E-59 |index_version_contract_test.sh||"
    "E2E-60 |subos_env_declaration_test.sh||"
    "E2E-61 |subos_env_probe_compat_test.sh||"
    "E2E-62 |loader_libc_same_source_test.sh||"
    "E2E-63 |subos_env_libc_guard_test.sh||"
    "E2E-64 |subos_single_version_test.sh||"
    # E2E-65/66 existed since 2026.8.8.2 but were never registered here --
    # they passed locally once and then never ran again anywhere.
    "E2E-65 |remove_orphan_payload_test.sh||"
    "E2E-66 |subos_env_use_switch_test.sh||"
    "E2E-67 |subos_runtime_binding_test.sh||"
    "E2E-68 |closure_guard_differential_test.sh||"
    "E2E-69 |local_query_no_index_sync_test.sh||"
    "E2E-70 |query_heavy_home_test.sh||"
    "E2E-71 |self_doctor_depth_test.sh||"
    "E2E-72 |elf_host_loader_payload_libc_guard_test.sh||"
    "E2E-73 |subos_runtime_authority_test.sh||"
    "E2E-74 |shared_registry_dep_install_dir_test.sh||"
    "E2E-75 |remove_foreign_provider_delegator_test.sh||"
    "E2E-76 |config_install_no_implicit_dir_test.sh||"
    "E2E-77 |subos_use_candidates_test.sh||"
)

PASS=0; FAIL=0; SOFTFAIL=0
FAILED_TESTS=()
TIMINGS=()

for entry in "${TESTS[@]}"; do
    IFS='|' read -r label script arg flags <<< "$entry"
    label="${label// /}"
    echo
    echo "=== $label: $script ==="
    args=()
    [[ "$arg" == "T" ]] && args+=("$TARBALL")

    _start=$(_t_ms)
    bash "$HERE/$script" "${args[@]}"
    rc=$?
    _dur=$(( $(_t_ms) - _start ))
    TIMINGS+=("$_dur $label $script")
    d="$(_fmt_ms "$_dur")"

    if [[ $rc -eq 0 ]]; then
        echo "PASS: $label ($script, $d)"
        ((PASS++))
    elif [[ "$flags" == *allowfail* ]]; then
        echo "SOFT-FAIL: $label ($script, exit $rc, $d) — tolerated (e.g. network/rate-limit)"
        ((SOFTFAIL++))
    else
        echo "FAIL: $label ($script, exit $rc, $d)"
        ((FAIL++))
        FAILED_TESTS+=("$label ($script, exit $rc)")
    fi
done

echo
echo "==============================================="
if [[ ${#TIMINGS[@]} -gt 0 ]]; then
    total=0
    for t in "${TIMINGS[@]}"; do total=$(( total + ${t%% *} )); done
    echo "E2E timing (slowest first; executed total $(_fmt_ms "$total")):"
    printf '%s\n' "${TIMINGS[@]}" | sort -rn | head -15 | while read -r ms label script; do
        printf '  %8s  %-8s %s\n' "$(_fmt_ms "$ms")" "$label" "$script"
    done
    echo "==============================================="
fi
echo "E2E Summary: $PASS passed, $FAIL failed, $SOFTFAIL soft-failed (tolerated)"
if [[ $FAIL -gt 0 ]]; then
    echo "Failed: ${FAILED_TESTS[*]}"
    exit 1
fi
exit 0
