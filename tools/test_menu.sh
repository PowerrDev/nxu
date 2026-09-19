#!/bin/bash
#
# Interactive test picker behind `make tests`.
#
# The registry itself lives in makedefs/tests.mk. This script only presents it
# (through `make test-list`) and hands the chosen id back to `make test`, so
# adding a test there is all it takes to make it show up here.
#
# Plain bash 3.2 (what macOS ships): no associative arrays, no fzf, just
# `read`. Type a number, a test name, or an unambiguous name prefix.

set -u

cd "$(dirname "$0")/.." || exit 1

MAKE_CMD=${MAKE:-make}
NAME_WIDTH=22

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
	BOLD=$'\033[1m'
	DIM=$'\033[2m'
	ACCENT=$'\033[36m'
	RESET=$'\033[0m'
	COLOR=1
else
	BOLD=
	DIM=
	ACCENT=
	RESET=
	COLOR=0
fi

ids=()
groups=()
descs=()

load_registry() {
	local rows id group desc

	rows=$("$MAKE_CMD" --no-print-directory -s test-list) || return 1
	while IFS=$'\t' read -r id group desc; do
		[ -n "$id" ] || continue
		ids+=("$id")
		groups+=("$group")
		descs+=("$desc")
	done <<EOF
$rows
EOF
	[ "${#ids[@]}" -gt 0 ]
}

# Inner width of the box: as wide as the terminal allows, capped so the
# descriptions stay readable on huge windows.
box_width() {
	local cols
	cols=$(tput cols 2>/dev/null || echo 80)
	[ "$cols" -gt 78 ] && cols=78
	[ "$cols" -lt 52 ] && cols=52
	echo $((cols - 2))
}

repeat() {
	local out= i
	for ((i = 0; i < $2; i++)); do out+=$1; done
	printf '%s' "$out"
}

# box_line <plain> <styled>: prints one boxed row, padding by the length of the
# unstyled text so colour escapes do not skew the right border.
box_line() {
	local plain=$1 styled=$2
	printf '│%s%*s│\n' "$styled" $((width - ${#plain})) ''
}

draw() {
	local i group last_group= number desc room

	width=$(box_width)

	[ "$COLOR" = 1 ] && printf '\033[2J\033[H'
	printf '\n'
	printf '╭%s╮\n' "$(repeat '─' "$width")"
	box_line "  NXU  Kernel tests" "  ${BOLD}NXU${RESET}  ${DIM}Kernel tests${RESET}"
	printf '├%s┤\n' "$(repeat '─' "$width")"

	for ((i = 0; i < ${#ids[@]}; i++)); do
		group=${groups[$i]}
		if [ "$group" != "$last_group" ]; then
			[ -n "$last_group" ] && box_line "" ""
			box_line "  $group" "  ${BOLD}$group${RESET}"
			last_group=$group
		fi

		number=$((i + 1))
		room=$((width - 2 - 3 - 2 - NAME_WIDTH - 1 - 1))
		desc=${descs[$i]:0:$room}
		box_line \
			"$(printf '  %3d  %-*s %s' "$number" "$NAME_WIDTH" "${ids[$i]}" "$desc")" \
			"$(printf '  %s%3d%s  %-*s %s%s%s' "$ACCENT" "$number" "$RESET" "$NAME_WIDTH" "${ids[$i]}" "$DIM" "$desc" "$RESET")"
	done

	printf '├%s┤\n' "$(repeat '─' "$width")"
	box_line "  number or name to run   q to quit" "  ${DIM}number or name to run   q to quit${RESET}"
	printf '╰%s╯\n\n' "$(repeat '─' "$width")"
}

# resolve <input>: sets `picked` to the index of the matching test, or fails.
resolve() {
	local input=$1 i matches=0

	picked=
	case $input in
	'' | *[!0-9]*) ;;
	*)
		if [ "$input" -ge 1 ] && [ "$input" -le "${#ids[@]}" ]; then
			picked=$((input - 1))
			return 0
		fi
		return 1
		;;
	esac

	for ((i = 0; i < ${#ids[@]}; i++)); do
		if [ "${ids[$i]}" = "$input" ]; then
			picked=$i
			return 0
		fi
		case ${ids[$i]} in
		"$input"*)
			picked=$i
			matches=$((matches + 1))
			;;
		esac
	done

	[ "$matches" -eq 1 ]
}

if ! load_registry; then
	echo "test_menu: could not read the test registry (make test-list failed)" >&2
	exit 1
fi

if [ ! -t 0 ]; then
	echo "Available tests (run one with: make test TEST=<name>):" >&2
	for ((i = 0; i < ${#ids[@]}; i++)); do
		printf '  %-*s %s\n' "$NAME_WIDTH" "${ids[$i]}" "${descs[$i]}" >&2
	done
	exit 1
fi

notice=
while :; do
	draw
	[ -n "$notice" ] && printf '  %s\n\n' "$notice"
	notice=

	printf '  %sselect%s %s>%s ' "$BOLD" "$RESET" "$ACCENT" "$RESET"
	if ! IFS= read -r choice; then
		printf '\n'
		exit 0
	fi

	case $choice in
	q | Q | quit | exit)
		exit 0
		;;
	'')
		continue
		;;
	esac

	if ! resolve "$choice"; then
		notice="${BOLD}'$choice'${RESET} is not a test; pick a number or a name from the list."
		continue
	fi

	printf '\n  %s>%s running %s%s%s\n\n' "$ACCENT" "$RESET" "$BOLD" "${ids[$picked]}" "$RESET"
	"$MAKE_CMD" test TEST="${ids[$picked]}"
	status=$?

	printf '\n  %s exited with status %d. Enter for the menu, q to quit: ' "${ids[$picked]}" "$status"
	IFS= read -r again || exit "$status"
	case $again in
	q | Q | quit | exit) exit "$status" ;;
	esac
done
