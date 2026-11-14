#!/bin/sh
#
# Print the debugfs commands that give each account its home directory on a
# disk image whose files were all made root's: for every /etc/passwd entry
# that is not root's and whose home is on the system volume
# (/Volumes/mainOS/...) and exists in the disk root, `sif` its uid and gid.
# Used by the disk rules after their chown-to-root pass.
#
# usage: disk_home_owners.sh <disk root>

ROOT=${1:?usage: disk_home_owners.sh <disk root>}

awk -F: -v root="$ROOT" '
	$3 != 0 && index($6, "/Volumes/mainOS/") == 1 {
		home = substr($6, length("/Volumes/mainOS") + 1)
		if (system("test -d \"" root home "\"") == 0) {
			printf "sif \"%s\" uid %s\nsif \"%s\" gid %s\n", home, $3, home, $4
		}
	}' "$ROOT/etc/passwd"
