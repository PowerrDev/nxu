# Secure Anchor Protocol

SAP authorizes one selected file deletion through a trusted desktop modal.
The first screen asks: "Deleting this file could break your entire system and
make it unusable. Are you sure?" Continue opens a masked password field.
Delete succeeds only after tepOS verifies the existing login passcode. Cancel,
Escape, timeout, unavailable authentication and a cancelled caller preserve the
file. Incorrect credentials keep the modal open for retry or cancellation.

`nxu_sap_delete(path)` explicitly requests authorization. Ordinary `unlink`
under `/disk/System` or `/disk/Library` also routes through SAP. Other unlink
operations retain the existing filesystem capability checks. Authorization
does not grant the application a reusable filesystem capability or expose the
password to it. Password buffers and keyboard events are wiped after use.

The ARM64 desktop host pauses presentation and input delivery while the kernel
worker owns the prompt. The modal displays the selected path, captures input
exclusively, and restores the desktop when finished. It requires an unlocked
desktop and fails closed on headless boots and i386. Only one request is active
at a time; other callers receive BUSY. Requests expire after two minutes.

The operation is limited to regular files on the disk mount. Directories and
parent traversal are rejected; no recursive deletion occurs. The worker holds
the selected vnode, then ext4 verifies the same directory entry and vnode under
its transaction lock before deletion. Changing the target or an unsupported
filesystem causes failure. Accepted deletion is followed by filesystem sync.
Existing ext4 restrictions on deleting files held open by other clients apply.

SAP currently protects deletion, not every write/truncate/update operation.
It is not a general sudo command runner, multi-user account system, framework
signature verifier or transactional system updater.

Run `make test-sap` to exercise the real GUI and mailbox client with an isolated
test mailbox provider and private disk. The test checks cancellation, rejected
credentials, masked password entry, selected-file persistence and input
isolation. The provider does not validate boot signatures or model tepOS's
persistent rate limiting; production verification uses the existing tepOS
mailbox driver and its authentication policy.

The dynamic ARM64 desktop has a separate integration test:
`python3 tools/check_gui_trusted.py`. SAP's presentation/input exclusion works
without the kernel UIService renderer. The test confirms app surfaces and
sessions survive the modal and authentication characters do not reach app
input. Login availability is also checked with `--unavailable`, and first
password setup with `--setup`.

Use `python3 tools/check_gui_trusted.py --restart` to kill the compositor while
the SAP modal owns the screen. The kernel keeps the trusted panel exclusive;
after cancellation, bootd restarts the services and the same app process regains
its surface and input. Window positions reset during desktop recovery.
