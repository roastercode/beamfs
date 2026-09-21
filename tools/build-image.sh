#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# build-image.sh 0.3.3 -- build the image, and show what it is doing.
#
# bitbake redirected to a file says nothing for eight minutes, which
# cannot be told from a build that has hung. bitbake on the terminal
# says everything, 9110 tasks of it.
#
# What is wanted is neither: a block that rewrites itself in place and
# holds the state of the moment. One line while tasks go by, more when
# there is more to say -- a warning, an error, a task that is taking
# its time. The size of the block is the amount of news, so a still
# block with one line means a build that is simply working.
#
# The full log stays in /tmp for when the answer is in the detail.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# set -u only after oe-init-build-env, which reads unset BBSERVER.
set -e
set +u

IMAGE="${1:-beamfs-research-image}"
BUILD="${BEAMFS_BUILD_DIR:-$HOME/yocto/poky/build-qemux86}"
LOG=/tmp/build-progress.log

cd "$HOME/yocto/poky" || { echo "no poky at $HOME/yocto/poky"; exit 1; }
# shellcheck disable=SC1091
source oe-init-build-env "$(basename "$BUILD")" > /dev/null
set -u
set +e

# The display model lives in lib/progress.sh, shared by every long
# job in this tree.
# shellcheck disable=SC1091
source "$HERE/lib/progress.sh"

: > "$LOG"
echo "  $IMAGE"
echo "  log: $LOG"
echo ""

bitbake "$IMAGE" > "$LOG" 2>&1 &
pid=$!
start=$(date +%s)
spin=0
frames='|/-\'

while kill -0 "$pid" 2>/dev/null; do
    progress_spin
    mark=$PROGRESS_MARK

    line=$(grep -a -oE "Running (noexec )?task [0-9]+ of [0-9]+ \(.*\)" "$LOG" | sed -n '$p')
    if [ -n "$line" ]; then
        n=${line#*task }; n=${n%% of *}
        m=${line#*of }; m=${m%% *}
        what=$(printf '%s' "$line" | sed -n 's/.*(\(.*\))/\1/p' | sed 's|.*/||')
        pct=$(( n * 100 / (m > 0 ? m : 1) ))
        body=$(printf '  %s %3d%%  %s of %s  %s  %s' "$mark" "$pct" "$n" "$m" "$what" "$(progress_elapsed "$start")")
    else
        head=$(sed -n '$p' "$LOG")
        body=$(printf '  %s  %s  %s' "$mark" "${head:0:70}" "$(progress_elapsed "$start")")
    fi

    # News grows the block. Warnings and errors are what a build has to
    # say for itself; everything else is it working.
    news=$(grep -a -E "^(ERROR|WARNING):" "$LOG" \
           | grep -a -vE "Host distribution|uninative|tainted from a forced run|WARNING messages" \
           | sed -n '1,6p' | sed 's/^/    /')
    [ -n "$news" ] && body="$body"$'\n'"$news"

    progress_draw "$body"
    sleep 1
done

wait "$pid"
rc=$?
progress_done "  done in $(progress_elapsed "$start")"
echo ""

grep -a -E "^ERROR|Tasks Summary|beamfs: MACHINE=" "$LOG"
err=$(grep -a -c "^ERROR" "$LOG")
echo "  errors : $err"
[ "$err" -gt 0 ] && grep -a -A15 "^ERROR" "$LOG"
echo "  exit   : $rc"
exit "$rc"
