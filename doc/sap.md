# Secure Anchor Protocol

SAP authorizes one selected file deletion through a trusted modal on ARM64 and
i386. Recovery PID 1 can also request deletion of one selected directory tree.
The first screen asks: "Deleting this file could break your entire system and
make it unusable. Are you sure?" Continue opens a masked password field.
Delete succeeds only after tepOS verifies the existing login passcode. Cancel,
Escape, timeout, unavailable authentication and a cancelled caller preserve the
file. Incorrect credentials keep the modal open for retry or cancellation.

`nxu_sap_delete(path)` explicitly requests authorization. Ordinary `unlink`
under `/Volumes/mainOS/System` or `/Volumes/mainOS/Library` also routes through SAP. Other unlink
operations retain the existing filesystem capability checks. Authorization
does not grant the application a reusable filesystem capability or expose the
password to it. Password buffers and keyboard events are wiped after use.

The ARM64 desktop host pauses presentation and input delivery while the kernel
worker owns the prompt. The modal displays the selected path, captures input
exclusively, and restores the prior screen when finished. It requires an unlocked
desktop session or recovery boot and fails closed without a suitable graphical
display or working tepOS password verification. Only one request is active
at a time; other callers receive BUSY. Requests expire after two minutes.

`nxu_sap_delete` remains limited to regular files on the disk mount. Recovery's
`nxu_sap_remove_tree` is available only to PID 1 with filesystem write capability.
In the recovery terminal, use:

```sh
rm -r --force-system /Volumes/mainOS/System/Library/Frameworks/Some.framework
```

The modal names the selected folder and warns that it will delete its contents.
Continue opens the masked password field. Cancellation or failed authentication
occurs before any child is removed. Approval starts a kernel-owned traversal
rather than granting a reusable capability to the terminal. Filesystem failures
or interruption after deletion starts may leave a partially removed tree.

The worker holds the selected vnode and every directory on the current branch.
ext4 verifies each entry against the held vnode under its transaction lock before
removal. The traversal does not follow symlinks or cross mounted filesystems,
and rejects mount roots and parent traversal. Its bounded traversal stack is
heap allocated. A different directory requires a fresh approval. Accepted
deletion is followed by filesystem sync. Existing restrictions on deleting
files held open by other clients apply.

SAP currently protects deletion, not every write/truncate/update operation.
It is not a general sudo command runner, multi-user account system, framework
signature verifier or transactional system updater.

Recovery removes its software cursor before executing a terminal command that
may open SAP. The trusted panel and recovery draw mainOS's 32×32 arrow asset, including
its alpha edges, shadow and click hotspot; cancellation or completion restores
recovery cursor rendering.

To refresh the embedded mainOS cursor, copy the desktop arrow asset to
`tools/UI/Cursors/mainos-arrow.cur` and run
`python3 tools/generate_pointer_cursor.py` before rebuilding.
