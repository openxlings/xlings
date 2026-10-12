#!/usr/bin/env bash
# Privacy lint (AGENTS.md "Privacy"): nothing in the repository identifies a
# person -- no personal home paths, no session temp paths, no personal email
# addresses, no rule, decision, home or machine attributed to someone.
#
# Functional public infrastructure is not personal data in this sense: a
# repository URL the product fetches from, CODEOWNERS, a package format's own
# maintainer field (AUR's `# Maintainer:`). A line that must say something
# this flags carries `privacy-ok: <why>`.
set -euo pipefail
cd "$(dirname "$0")/.."

# Placeholders and the product's own paths; a one-letter name is a placeholder.
allowed_home='([a-z]|user|runner|xlings|you|alice|bob|carol|hm|me|demo|dev|example|root|someone|USER|xtest|builder|ubuntu|vagrant|ci|test|admin|<[^>]*>|\$[A-Za-z_{(]+)'
patterns=(
  "(?<![A-Za-z0-9_./-])/home/(?!${allowed_home}/)[a-z][a-z0-9_-]*/"
  "(?<![A-Za-z0-9_.-])/Users/(?!${allowed_home}/|Shared/)[A-Za-z][A-Za-z0-9_-]*/"
  "/tmp/claude-[0-9]+/"
  "[A-Za-z0-9._%+-]+@(gmail|qq|163|126|outlook|hotmail|yahoo|foxmail|icloud)\\.com"
  "maintainer'?s (rule|home|machine|real home|own machine|ask|requirement|model|call|decision)"
  "the maintainer (said|decided|confirmed|asked|wants|wanted)"
  "维护者(确认|评审|要求|反馈|决定|的原话|的模型|在真实|提的|拍板|已表态|已确认|：)"
  "（维护者[，,)）]"
)
joined="$(IFS='|'; echo "${patterns[*]}")"

hits="$(git ls-files -z -- . \
          ':!tests/fixtures' ':!tests/e2e/runtime' ':!.agents/skills/ui-ux-pro-max' \
          ':!*.png' ':!*.jpg' ':!*.svg' ':!*.ico' ':!*.gz' ':!*.zst' \
        | xargs -0 grep -nIP -- "$joined" 2>/dev/null \
        | grep -v 'privacy-ok:' \
        | grep -vE '^[^:]+:[0-9]+:# (Maintainer|Contributor):' \
        | grep -v '^tools/lint_privacy.sh:' || true)"
if [[ -n "$hits" ]]; then
  echo "privacy lint: these lines identify a person (AGENTS.md \"Privacy\"):"
  echo "$hits"
  exit 1
fi
echo "privacy lint: PASS (no personal paths, emails or attributions)"
