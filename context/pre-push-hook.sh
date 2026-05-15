#!/usr/bin/env bash
# context/pre-push-hook.sh -- enforce R14 / R39 anti-leak policy
#
# Install : ln -sf $(pwd)/context/pre-push-hook.sh .git/hooks/pre-push
#           chmod +x context/pre-push-hook.sh
#
# Behaviour :
# - Refuses any push to remote origin for branches other than main.
# - Allows push to any other remote (private, devel, etc.) for any branch.
# - Allows push to origin for branch main without restriction.
#
# Rationale : on beamfs, the convention is INVERTED. origin points to the
# PUBLIC roastercode/beamfs repository ; the PRIVATE roastercode/beamfs-devel
# repository is reached via the remote named private. The R14 incident of
# 2026-04-30 and its recurrence on 2026-05-06 (see R14-INCIDENT-20260515.md)
# both consisted of pushing devel to origin (PUBLIC). This hook blocks that.
#
# Standard git pre-push hook arguments :
#   $1 : remote name
#   $2 : remote URL
#   stdin lines : <local_ref> <local_sha> <remote_ref> <remote_sha>

set -u

REMOTE_NAME="$1"
REMOTE_URL="$2"

# Allow everything except pushes to origin for non-main branches
if [ "$REMOTE_NAME" != "origin" ]; then
    exit 0
fi

# Read the ref lines from stdin
while IFS=' ' read -r local_ref local_sha remote_ref remote_sha; do
    # Branch being pushed
    branch_name="${remote_ref#refs/heads/}"
    if [ "$branch_name" = "main" ]; then
        continue
    fi
    # Refuse
    echo "" >&2
    echo "R14 / R39 ENFORCEMENT : refusing push to origin for non-main branch" >&2
    echo "" >&2
    echo "  remote : $REMOTE_NAME ($REMOTE_URL)" >&2
    echo "  branch : $branch_name" >&2
    echo "  local  : $local_sha" >&2
    echo "" >&2
    echo "Convention on this repository :" >&2
    echo "  origin  = roastercode/beamfs        PUBLIC  (only main allowed)" >&2
    echo "  private = roastercode/beamfs-devel  PRIVATE (devel goes here)" >&2
    echo "" >&2
    echo "To push branch $branch_name, use :" >&2
    echo "  git push private $branch_name" >&2
    echo "" >&2
    echo "If you really need to push $branch_name to origin (PUBLIC), bypass via :" >&2
    echo "  git push --no-verify origin $branch_name" >&2
    echo "This is intentionally hostile to force conscious override." >&2
    echo "" >&2
    exit 1
done

exit 0
