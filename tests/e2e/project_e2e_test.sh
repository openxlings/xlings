#!/usr/bin/env bash
set -euo pipefail

bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_home_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_workspace_platform_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_local_repo_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_global_fallback_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_xlings_res_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_xlings_res_override_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_xlings_res_region_map_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_xlings_res_multi_server_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_xlings_res_cross_region_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/subos_payload_refcount_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project_shim_mirror_test.sh"
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/shim_project_context_test.sh"
