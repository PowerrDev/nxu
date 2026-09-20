# bootd service property lists

NXU root services are declared as XML property lists rather than compiled into
PID 1. `bootd` opens `/disk/System/Library/BootDaemons`, enumerates regular
`*.plist` entries through the VFS `readdir` interface, validates each document,
and creates one fixed-capacity job record per unique service label.

The parser is allocation-free. `CoreFoundation.framework/lib/plist` exposes a
generic event stream for the plist subset NXU currently needs, while
`lib/service` translates those events into a service configuration. Process
lifecycle and restart policy remain private to `bootd`.

## Current schema

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `Label` | string | yes | Unique service identity, for example `com.nxu.logd`. |
| `Program` | string | yes | Absolute executable path on the mounted system volume. |
| `RecoveryProgram` | string | no | Recovery executable used when the primary differs or cannot launch. |
| `RunAtLoad` | boolean | no | Requests activation during root-service bootstrap. |
| `KeepAlive` | boolean | no | Restarts the job after it exits. |
| `Disabled` | boolean | no | Prevents activation without removing the definition. |
| `DisabledInSafeMode` | boolean | no | Prevents activation when `-x` is present. |
| `DisabledBootArgument` | string | no | Prevents activation when the named boot argument is present. |
| `StartOnLogin` | boolean | no | Parsed and retained for the future login-session activation path. |
| `AllowFilesystemWrite` | boolean | no | Capability `NXU_CAP_FS_WRITE` for the job's process: open for write/create/truncate/append, `unlink`, `mkdir`. Default false. |
| `AllowDisplay` | boolean | no | Capability `NXU_CAP_DISPLAY` for the job's process: `display_claim`. Default false. |

A job's process holds only the capabilities its plist asks for; bootd passes them
to `nxu_spawn_caps`. `logd` and `patchd` set `AllowFilesystemWrite`, and
`windowserver` sets `AllowDisplay`. See [process control](kern/process-control.md#capabilities).

`StartOnLogin` does **not** launch anything yet. NXU has no login-session event
or bootd IPC control path today, so claiming that behavior would be false. A
future session manager can activate jobs from `/System/Library/LoginAgents`
without changing the plist parser or job representation.

## Example

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//NXU//DTD PLIST 1.0//EN" "http://www.nxu.local/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.nxu.logd</string>

    <key>Program</key>
    <string>/disk/System/Library/CoreServices/logd</string>

    <key>RecoveryProgram</key>
    <string>/disk/System/Library/CoreServices/logd.recovery</string>

    <key>RunAtLoad</key>
    <true/>

    <key>KeepAlive</key>
    <true/>

    <key>DisabledInSafeMode</key>
    <true/>

    <key>DisabledBootArgument</key>
    <string>-no-logd</string>
</dict>
</plist>
```

## Plist subset

The current parser accepts a top-level plist document containing dictionaries,
arrays, keys, strings, decimal integers and booleans. It understands XML
prologs, comments, a simple external DOCTYPE, UTF-8 BOMs, and the five XML
named entities `amp`, `lt`, `gt`, `quot`, and `apos`.

Binary plists, dates, real numbers, data blobs, numeric XML entities and general
XML DTD processing are not implemented. Unknown service keys are skipped,
including nested dictionary/array values, so the schema can grow without
breaking older bootd builds.

## Restart policy

A `KeepAlive` job is reaped with nonblocking `waitpid`. Five exits inside a
10-second window trigger a 10-second restart throttle. This prevents a broken
root service from turning PID 1 into a tight spawn/crash loop.
