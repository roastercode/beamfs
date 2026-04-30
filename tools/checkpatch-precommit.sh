#!/bin/bash
#
# checkpatch-precommit.sh -- run upstream Linux checkpatch.pl on staged
# beamfs sources before allowing a commit.
#
# This is a defensive layer against style drift and well-known bug patterns
# (uninitialized stack variables, missing endianness conversions, locking
# imbalance, etc.) catalogued by the upstream kernel scripts/checkpatch.pl.
#
# Installation:
#   cd ~/git/beamfs
#   ln -sf ../../tools/checkpatch-precommit.sh .git/hooks/pre-commit
#
# Bypass (use sparingly, never on push):
#   git commit --no-verify
#
# Audit rationale: pre-commit static checks are a recommended practice for
# safety-critical code (DO-178C 6.3.4, ECSS-Q-ST-80C). The hook is non-
# blocking by design (warnings only) but exits non-zero on errors so that
# committing requires explicit acknowledgement.
#

set -e

# Locate checkpatch.pl. Order of preference:
#   1. Gentoo /usr/src/linux symlink (tracks current kernel)
#   2. Local frozen copy in tools/checkpatch.pl
CHECKPATCH=""
for candidate in /usr/src/linux/scripts/checkpatch.pl \
                 "$(dirname "$0")/checkpatch.pl"; do
    if [ -x "$candidate" ] || [ -r "$candidate" ]; then
        CHECKPATCH="$candidate"
        break
    fi
done

if [ -z "$CHECKPATCH" ]; then
    echo "checkpatch-precommit: no checkpatch.pl found." >&2
    echo "  Tried: /usr/src/linux/scripts/checkpatch.pl" >&2
    echo "         tools/checkpatch.pl (local frozen copy)" >&2
    echo "  Skipping pre-commit checks (commit allowed)." >&2
    exit 0
fi

# Repo root is the parent of .git, where the hook is invoked from.
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

STAGED=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(c|h)$' || true)

if [ -z "$STAGED" ]; then
    exit 0
fi

echo "checkpatch-precommit: running $CHECKPATCH on staged C files..." >&2

failed=0
for f in $STAGED; do
    if ! git diff --cached "$f" | \
            "$CHECKPATCH" --no-tree --strict --no-summary --terse \
                          --ignore FILE_PATH_CHANGES,SPDX_LICENSE_TAG \
                          - 1>&2; then
        failed=1
        echo "  -> issues in $f" >&2
    fi
done

if [ "$failed" -ne 0 ]; then
    echo "" >&2
    echo "checkpatch-precommit: failures detected." >&2
    echo "  Fix the issues above, or bypass with 'git commit --no-verify'" >&2
    echo "  (the latter is acceptable for refactor-only commits but should" >&2
    echo "   be justified in the commit message)." >&2
    exit 1
fi

exit 0
