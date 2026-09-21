# SPDX-License-Identifier: GPL-2.0-only
#
# progress.sh 1.1.0 -- the beamfs way of showing a long background job.
#
# This is the project's display model. Every long-running tool here
# uses it, so that watching a build, a sweep or a campaign feels the
# same and means the same thing.
#
# The model, in three rules:
#
#   1. One block, rewritten in place. Never a stack of lines. What is
#      on screen is the state of now, not a history of then; the
#      history is in the log file, which is where history belongs.
#
#   2. The block is as tall as there is news. A quiet job is one line:
#      a spinner turning, what it is on, and how long it has been.
#      A job with something to say grows by exactly the lines it
#      needs -- a warning, an error, a step that is dragging -- and
#      shrinks back when the news is over. So the height of the block
#      is itself information, readable without reading.
#
#   3. A line is only left behind when it has become permanent: a
#      stage that finished, an error that will not un-happen. Those
#      are printed above the block and stay. Nothing else persists.
#
# The spinner is not decoration. A block that has not changed in a
# minute and a spinner that is still turning say "slow task"; a
# spinner that has stopped says "hung", and that distinction is the
# whole reason the display exists.
#
# Usage:
#
#   source "$(dirname "$0")/lib/progress.sh"
#   progress_draw "$body"     # body may be one line or many
#   progress_spin             # sets PROGRESS_MARK to the next frame
#   progress_done "text"      # clears the block, prints text for good
#
# progress_draw is not thread safe and expects to own the bottom of
# the terminal. Do not print anything else while a block is up; use
# progress_note for a line that must persist.

_progress_drawn=0
_progress_spin=0

# The current frame, in PROGRESS_MARK.
#
# It does not print: a caller writing mark=$(progress_spin) would run
# this in a subshell, where the counter is incremented and then thrown
# away with the subshell, so the spinner sat on its first frame for a
# whole eight-minute build while the line above it changed happily. A
# variable crosses back, a subshell does not.
progress_spin() {
    local frames='|/-\'
    PROGRESS_MARK="${frames:_progress_spin%4:1}"
    _progress_spin=$((_progress_spin + 1))
}

# Redraw the block. Walk back up over what was drawn last time, write
# the new body, and clear any line the new body no longer needs -- a
# leftover line from a taller block would read as current news.
progress_draw() {
    local body="$1" n i line
    n=$(printf '%s\n' "$body" | wc -l)
    [ "$_progress_drawn" -gt 0 ] && printf '\033[%dA' "$_progress_drawn"
    while IFS= read -r line; do
        printf '\033[2K%s\n' "$line"
    done <<< "$body"
    if [ "$n" -lt "$_progress_drawn" ]; then
        for ((i = 0; i < _progress_drawn - n; i++)); do printf '\033[2K\n'; done
        printf '\033[%dA' "$((_progress_drawn - n))"
    fi
    _progress_drawn=$n
}

# A line that has earned permanence: print it above the block, then
# redraw the block under it.
progress_note() {
    local keep="$2"
    [ "$_progress_drawn" -gt 0 ] && printf '\033[%dA' "$_progress_drawn"
    printf '\033[2K%s\n' "$1"
    _progress_drawn=0
    [ -n "${keep:-}" ] && progress_draw "$keep"
}

progress_done() {
    progress_draw "$1"
    _progress_drawn=0
    echo ""
}

# Elapsed seconds since a start stamp, as the display wants it.
progress_elapsed() {
    local s=$(( $(date +%s) - $1 ))
    if [ "$s" -lt 120 ]; then printf '%ds' "$s"
    else printf '%dm%02ds' "$((s / 60))" "$((s % 60))"; fi
}
