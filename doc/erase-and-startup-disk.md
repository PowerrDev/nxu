# Erase All Content and Settings, and Startup Disk

System Settings > General > Transfer or Reset offers "Erase All Content and
Settings", and System Settings > General > Startup Disk chooses between the
system volume (`mainOS`) and the recovery volume (`Recovery`). System Settings
only writes request files; patchd does the work, and the erase itself runs at
boot from the recovery volume. Both features need the named GPT recovery
partition (`NXU Recovery`, mounted at `/Volumes/Recovery`) described in
[Software updates](software-update.md).

## Files

| Path | Written by | Meaning |
| --- | --- | --- |
| `/Volumes/mainOS/var/db/patchd/erase-request` | System Settings | exactly `erase-all-content-and-settings\n` |
| `/Volumes/mainOS/var/db/patchd/erase-status` | patchd | `scheduled\n` or `failed: REASON\n` |
| `/Volumes/mainOS/var/db/patchd/startup-disk-request` | System Settings | `mainOS now`, `mainOS later`, `Recovery now` or `Recovery later`, plus `\n` |
| `/Volumes/mainOS/var/db/patchd/startup-disk` | patchd, kernel | mirror of the current choice: `mainOS\n` or `Recovery\n` (missing means mainOS) |
| `/Volumes/Recovery/System/StartupDisk` | patchd, kernel | the stored startup disk choice |
| `/Volumes/Recovery/Erase/Queued` | patchd, triageOS | an erase is queued: `erase-all-content-and-settings\n` |
| `/Volumes/Recovery/Erase/Failed` | patchd (erase mode) | `failed: REASON\n`: an erase stopped after the system volume was changed |
| `/Volumes/Recovery/Erase/Log.txt` | patchd | every line logged for the last erase, from the request to the restart |
| `/Volumes/Recovery/System/Recovery/patchd` | recovery volume build | the patchd that runs the erase |
| `/Volumes/Recovery/System/Recovery/Factory/` | recovery volume build | the factory payload (below) |

The desktop's processes run as root (bootd is PID 1 with root credentials and
every service and app inherits them), so System Settings can create and
replace files in `/Volumes/mainOS/var/db/patchd`, the same way it writes
`/Volumes/mainOS/etc/hostname`. The directory is root-owned, mode 0755, as
built; no permission was widened. A request is consumed (unlinked) when
patchd reads it. A request without its final newline is treated as still
being written and left for the next poll (patchd polls twice a second).

## Factory payload

`tools/make_recovery_volume.py --factory-image SYSTEM.img` stores a complete
copy of that system image on the recovery volume:

```text
System/Recovery/patchd                       the image's own patchd
System/Recovery/Factory/Root/...             every file and directory of the image
System/Recovery/Factory/FactoryManifest      one line per entry
System/Recovery/Factory/FactoryManifest.sha256
```

`make run-i386-desktop` passes the fresh `system-template.img` when it first
creates `recovery-factory.img` (512 MiB). The template's files come to about
34 MB. The tool reads ownership and modes from the image with `debugfs`, so
setuid-root tools (`su`, `sudo`, `passwd`, `login`) keep mode 4755 and
`/Users/user` keeps its owner. `lost+found` and the request/status files of
`/var/db/patchd` are never part of the payload. Symlinks and device nodes are
rejected. The recovery volume is created once: delete `recovery-factory.img`
and `ssd.img` to rebuild it with a payload, because an older recovery
partition has none and patchd then refuses to schedule an erase.

`FactoryManifest` is plain text, parents before children:

```text
NXUFactoryManifest 1
d 0755 0 0 0 - System
f 4755 0 0 34340 <sha256> System/Library/CoreServices/su
...
end 172
```

Fields are kind (`d` or `f`), octal mode, uid, gid, size, SHA-256 (`-` for a
directory) and the path relative to the volume root (the rest of the line,
so spaces are allowed). `FactoryManifest.sha256` holds the manifest's own
SHA-256 and a newline. Hashes detect corruption; they do not authenticate a
publisher.

## Erase flow

1. **Request.** patchd sees `erase-request`, removes any old `erase-status`,
   consumes the request and checks it: exact content, no software update
   queued or active, the recovery volume mounted, `System/Recovery/patchd`
   present, and the factory manifest matching its SHA-256. A failed check
   writes `failed: REASON` and changes nothing.
2. **Queue.** patchd starts a new `Erase/Log.txt`, writes and syncs
   `Erase/Queued`, reads it back, writes `scheduled` to `erase-status` and
   restarts the machine (its service plist grants `AllowReset`). If reset is
   unavailable the erase stays queued for the next restart.
3. **Boot.** After the volumes are mounted and any software update is
   resolved, `boot_erase_init()` (kern/update/erase.c) finds the marker. It
   checks that the recovery patchd is a valid ELF and the manifest exists;
   otherwise it removes the marker and boots normally. Shift+R or
   `recovery=1` still enters recovery and leaves the erase queued. Otherwise
   the kernel starts `/Volumes/Recovery/System/Recovery/patchd
   --erase-all-content-and-settings` as PID 1 instead of bootd. No desktop,
   services or boot chime run. The binary is not on the volume being wiped.
   Only that PID 1, on that boot, may unlink and rmdir protected system paths
   (`/Volumes/mainOS/System`, `/Volumes/mainOS/Library`).
4. **Verify the payload.** patchd loads and checks the manifest, then hashes
   every factory file against its size and SHA-256. If anything is missing or
   corrupt, nothing is removed. patchd writes `failed: REASON` to
   `erase-status`, removes the marker and restarts into the unchanged system.
5. **Wipe.** Every file and directory below `/Volumes/mainOS` is removed,
   including `Users`, `var` and `etc`. Only `lost+found` and any filesystem
   mounted below the volume are kept. Each removal is logged with its path
   and size. Each removal is tried three times before the erase fails.
6. **Reinstall.** For each manifest entry, in order: directories are created.
   Files are copied while hashing the bytes read, then read back from the
   system volume and hashed again. Owner and mode are set, then read back. A
   mismatch is logged with the read-back size and hash and retried, up to
   three attempts per file. After a sync, every installed file is hashed once
   more.
7. **Finish.** patchd removes `Erase/Queued` (and any `Failed`), sets the
   startup disk to mainOS, logs the summary and restarts into the fresh
   system.

If the erase fails after the wipe began, patchd writes `Erase/Failed`,
removes `Erase/Queued` and restarts. While `Failed` exists, every boot logs
the reason and starts triageOS. In the recovery shell, `erase confirm` queues
the erase again (it removes `Failed`, writes `Queued` and restarts).
`rm /Volumes/Recovery/Erase/Failed` boots mainOS as it is. An erase that is
interrupted (power loss) keeps its `Queued` marker and runs again from the
start on the next boot.

## Logs

Every line has the form `function_name: text`. Lines go to the serial
console (kernel console) and to `/Volumes/Recovery/Erase/Log.txt`. Example:

```text
boot_erase_init: erase all content and settings is queued (/Volumes/Recovery/Erase/Queued)
boot_erase_spawn: loading /Volumes/Recovery/System/Recovery/patchd --erase-all-content-and-settings as PID 1
patchd_erase_run: factory manifest matches its SHA-256: 172 entries
erase_verify_payload: verified factory file etc/passwd (315 bytes, sha256 ...)
erase_unlink: removed file /Volumes/mainOS/Users/user/notes.txt (42 bytes)
erase_remove_tree: removed directory /Volumes/mainOS/Users/user/
erase_install: created directory /Volumes/mainOS/System/ (mode 0755)
erase_install_file: installed /Volumes/mainOS/System/Library/CoreServices/su (34340 bytes, sha256 ..., mode 4755, owner 0:0): verified
erase_final_check: final verification passed for 172 entries
patchd_erase_run: erase all content and settings completed successfully
```

## Startup Disk

patchd consumes `startup-disk-request`, validates it, stores the choice in
`/Volumes/Recovery/System/StartupDisk` (read back after a sync), updates the
`startup-disk` mirror and restarts when the request says `now`. Recovery
needs the recovery volume to be mounted. NVRAM is not used, because it is
read-only under QEMU. At every service start patchd brings the mirror back in
line with the stored choice.

At boot, `boot_startup_disk_init()` (kern/boot/startup_disk.c) reads the
stored choice. Recovery is a one-shot choice. The kernel first resets the
stored choice and the mirror to mainOS and syncs them. Only then does it
start triageOS. If the reset cannot be written, it starts mainOS instead. A
restart from recovery therefore always returns to mainOS. A missing or
malformed choice means mainOS.
