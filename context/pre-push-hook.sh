#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# context/pre-push-hook.sh -- what may reach the public repository
#
# Install : ln -sf "$(pwd)/context/pre-push-hook.sh" .git/hooks/pre-push
#
# beamfs is developed in public: origin is roastercode/beamfs, main is
# its only branch, and releases are signed tags beamfs-vX.Y (README,
# Versions). For origin, this hook refuses:
#
# - any branch other than main, and the deletion of main;
# - a push of main that does not fast-forward the remote: main is never
#   forced;
# - a commit without a good signature (%G? other than G), or whose
#   message carries a Claude-Session: or Co-Authored-By: line;
# - a tag that is not an annotated tag named beamfs-vX, beamfs-vX.Y or
#   beamfs-vX.Y.Z, whose signature verifies and whose commit is on
#   main; and the deletion of a tag.
#
# Other remotes are not checked. A stable branch for beamfs-vX.Y.Z will
# need this hook changed before its first push.
#
# It replaces the R14 / R39 hook of the period of private development,
# which allowed only main on origin and refused every tag.
#
# Arguments, as git passes them: $1 the remote name, $2 its URL; on
# stdin, one line per ref: <local_ref> <local_sha> <remote_ref> <remote_sha>

set -u

remote_name="$1"
remote_url="$2"
zero=0000000000000000000000000000000000000000

[ "$remote_name" = origin ] || exit 0

refuse() {
	echo "" >&2
	echo "pre-push: refused for $remote_name ($remote_url)" >&2
	echo "  $1" >&2
	echo "" >&2
	exit 1
}

# $1: the local commit, $2: the remote one, zero when the remote has none.
check_commits() {
	local range bad
	if [ "$2" = "$zero" ]; then
		range="$1 --not --remotes=$remote_name"
	else
		range="$2..$1"
	fi
	# shellcheck disable=SC2086
	bad=$(git log --format='%H %G?' $range | awk '$2 != "G" { print $1 }')
	[ -z "$bad" ] || refuse "commits without a good signature: $bad"
	# shellcheck disable=SC2086
	if git log --format=%B $range |
		grep -q -i -E '^(Claude-Session|Co-Authored-By):'; then
		refuse "a commit message carries a Claude-Session: or Co-Authored-By: line"
	fi
}

while read -r local_ref local_sha remote_ref remote_sha; do
	case "$remote_ref" in
	refs/heads/main)
		[ "$local_sha" != "$zero" ] || refuse "main cannot be deleted"
		if [ "$remote_sha" != "$zero" ]; then
			git merge-base --is-ancestor "$remote_sha" "$local_sha" 2>/dev/null ||
				refuse "main ($local_ref) does not fast-forward $remote_sha; main is never forced"
		fi
		check_commits "$local_sha" "$remote_sha"
		;;
	refs/tags/*)
		tag=${remote_ref#refs/tags/}
		[ "$local_sha" != "$zero" ] || refuse "tag $tag cannot be deleted"
		printf '%s\n' "$tag" | grep -q -E '^beamfs-v[0-9]+(\.[0-9]+){0,2}$' ||
			refuse "tag $tag is not named beamfs-vX, beamfs-vX.Y or beamfs-vX.Y.Z"
		[ "$(git cat-file -t "$local_sha")" = tag ] ||
			refuse "tag $tag is not an annotated tag"
		git verify-tag "$local_sha" >/dev/null 2>&1 ||
			refuse "tag $tag does not carry a good signature"
		target=$(git rev-parse "$local_sha^{commit}")
		git merge-base --is-ancestor "$target" refs/heads/main ||
			refuse "tag $tag points to $target, which is not on main"
		;;
	*)
		refuse "$remote_ref: only main and signed beamfs-v tags go to origin"
		;;
	esac
done

exit 0
