#!/bin/bash
#
# Checks the i386 user programs against what the kernel's ELF loader accepts
# (kern/loader/elf_format.h) and the user address map (kern/i386/vm_param.h).
# Behind `make test-i386-userland`.
#
# For every program in the build directory it verifies, with llvm-readelf:
#
#   * ELF32, little-endian, EM_386, ET_EXEC
#   * the entry point lies inside a PT_LOAD segment that is executable
#   * no segment is both writable and executable
#   * every PT_LOAD starts on a page, keeps file offset and virtual address
#     congruent modulo the page size, and its file bytes lie inside the file
#   * every segment lies inside [0x1000, VM_MAP_BASE), the user image window
#   * no two segments share a page
#
# and prints each program's file size and segment sizes. Plain bash 3.2.

set -u

USER_BUILD=${1:?usage: check_i386_user.sh <user build dir> [program...]}
shift

PROGRAMS="bootd logd patchd ipctest_a ipctest_b threadtest sockettest_server sockettest_client"
if [ $# -gt 0 ]; then PROGRAMS="$*"; fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PAGE=4096
IMAGE_LOW=$((0x1000))

READELF=${READELF:-llvm-readelf}
if ! command -v "$READELF" >/dev/null 2>&1; then
	for candidate in /usr/local/opt/llvm/bin/llvm-readelf /opt/homebrew/opt/llvm/bin/llvm-readelf; do
		if [ -x "$candidate" ]; then READELF=$candidate; break; fi
	done
fi
if ! command -v "$READELF" >/dev/null 2>&1; then
	echo "check_i386_user: llvm-readelf not found" >&2
	exit 2
fi

# VM_MAP_BASE is where the image window ends; take it from the kernel header.
MAP_BASE=$(sed -n 's/^#define VM_MAP_BASE[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1/p' "$ROOT/kern/i386/vm_param.h" | head -1)
if [ -z "$MAP_BASE" ]; then
	echo "check_i386_user: cannot read VM_MAP_BASE from kern/i386/vm_param.h" >&2
	exit 2
fi
MAP_BASE=$((MAP_BASE))

failures=0
checked=0

fail() {
	printf 'FAIL  %-18s %s\n' "$1" "$2"
	failures=$((failures + 1))
}

check_program() {
	local name=$1 path="$USER_BUILD/$1" header size entry bad=0
	local class data type machine
	local lines count=0 index
	local -a off va fsz msz flg al

	if [ ! -f "$path" ]; then
		fail "$name" "missing: $path"
		return
	fi

	checked=$((checked + 1))
	size=$(wc -c < "$path" | tr -d ' ')
	header=$("$READELF" -h "$path" 2>&1)

	class=$(printf '%s\n' "$header" | sed -n 's/^ *Class: *//p')
	data=$(printf '%s\n' "$header" | sed -n 's/^ *Data: *//p')
	type=$(printf '%s\n' "$header" | sed -n 's/^ *Type: *\([A-Z_]*\).*/\1/p')
	machine=$(printf '%s\n' "$header" | sed -n 's/^ *Machine: *//p')
	entry=$(printf '%s\n' "$header" | sed -n 's/^ *Entry point address: *//p')

	[ "$class" = "ELF32" ] || { fail "$name" "class is '$class', want ELF32"; bad=1; }
	case "$data" in *little*) ;; *) fail "$name" "data is '$data', want little endian"; bad=1 ;; esac
	[ "$type" = "EXEC" ] || { fail "$name" "type is '$type', want EXEC"; bad=1; }
	[ "$machine" = "Intel 80386" ] || { fail "$name" "machine is '$machine', want Intel 80386"; bad=1; }

	# LOAD <offset> <vaddr> <paddr> <filesz> <memsz> <flags...> <align>
	lines=$("$READELF" -l -W "$path" 2>&1 | awk '$1 == "LOAD" {
		flags = ""
		for (i = 7; i < NF; i++) flags = flags $i
		print $2, $3, $5, $6, flags, $NF
	}')

	while read -r a b c d e f; do
		[ -n "$a" ] || continue
		off[$count]=$((a)); va[$count]=$((b)); fsz[$count]=$((c)); msz[$count]=$((d)); flg[$count]=$e; al[$count]=$((f))
		count=$((count + 1))
	done <<EOF
$lines
EOF

	if [ "$count" -eq 0 ]; then
		fail "$name" "no PT_LOAD segment"
		return
	fi

	local entry_ok=0 detail=""

	for ((index = 0; index < count; index++)); do
		local o=${off[$index]} v=${va[$index]} f=${fsz[$index]} m=${msz[$index]} fl=${flg[$index]} a=${al[$index]}
		local end=$((v + m)) other

		case "$fl" in *W*E*|*E*W*) fail "$name" "segment $index is writable and executable ($fl)"; bad=1 ;; esac
		[ $((v % PAGE)) -eq 0 ] || { fail "$name" "segment $index vaddr $(printf '0x%x' "$v") is not page aligned"; bad=1; }
		[ $(((v - o) % PAGE)) -eq 0 ] || { fail "$name" "segment $index file offset and vaddr are not congruent mod $PAGE"; bad=1; }
		[ "$a" -ge "$PAGE" ] || { fail "$name" "segment $index alignment $a is below the page size"; bad=1; }
		[ "$m" -ge "$f" ] || { fail "$name" "segment $index memsz < filesz"; bad=1; }
		[ $((o + f)) -le "$size" ] || { fail "$name" "segment $index file bytes end at $((o + f)), past end of file ($size)"; bad=1; }
		[ "$v" -ge "$IMAGE_LOW" ] || { fail "$name" "segment $index starts below 0x1000"; bad=1; }
		[ "$end" -le "$MAP_BASE" ] || { fail "$name" "segment $index ends at $(printf '0x%x' "$end"), past VM_MAP_BASE $(printf '0x%x' "$MAP_BASE")"; bad=1; }

		for ((other = 0; other < index; other++)); do
			local ov=${va[$other]} oend=$((${va[$other]} + ${msz[$other]}))
			local pstart=$((v / PAGE * PAGE)) pend=$(((end + PAGE - 1) / PAGE * PAGE))
			local ostart=$((ov / PAGE * PAGE)) opend=$(((oend + PAGE - 1) / PAGE * PAGE))

			if [ "$pstart" -lt "$opend" ] && [ "$ostart" -lt "$pend" ]; then
				fail "$name" "segments $other and $index share a page"
				bad=1
			fi
		done

		case "$fl" in *E*) if [ $((entry)) -ge "$v" ] && [ $((entry)) -lt "$end" ]; then entry_ok=1; fi ;; esac

		detail="$detail [$(printf '0x%x' "$v") $fl file $f mem $m]"
	done

	[ "$entry_ok" -eq 1 ] || { fail "$name" "entry $entry is not inside an executable PT_LOAD"; bad=1; }

	if [ "$bad" -eq 0 ]; then
		printf 'ok    %-18s %6s bytes, entry %s,%s\n' "$name" "$size" "$entry" "$detail"
	fi
}

for program in $PROGRAMS; do
	check_program "$program"
done

if [ "$checked" -eq 0 ]; then
	echo "check_i386_user: no programs checked" >&2
	exit 1
fi

if [ "$failures" -ne 0 ]; then
	echo "check_i386_user: $failures failure(s)"
	exit 1
fi

echo "check_i386_user: $checked programs ok"
