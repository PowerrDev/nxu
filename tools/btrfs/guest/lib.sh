# Shared helpers for the guest-side scripts (POSIX sh, sourced).
# NXU_HTTP is the host's throw-away HTTP server, set by the apkovl start script.

say() { echo "guest: $*"; }

# The initramfs did not mount the modloop (the Btrfs module lives in it), so
# fetch the same file the boot media would have used and mount it.
setup_modules() {
	[ -d "/lib/modules/$(uname -r)" ] && return 0
	wget -q -O /tmp/modloop "$NXU_HTTP/modloop-virt" || return 1
	mkdir -p /.modloop /lib/modules
	mount -t squashfs -o loop,ro /tmp/modloop /.modloop || return 1
	mount --bind /.modloop/modules /lib/modules || return 1
	return 0
}

install_tools() {
	apk add --no-progress btrfs-progs python3 zstd bash util-linux e2fsprogs > /tmp/apk.log 2>&1 || { tail -5 /tmp/apk.log; return 1; }
}
