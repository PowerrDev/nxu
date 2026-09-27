# App bundles and the Dock

sevOS apps are processes. Each lives in a bundle on the system volume, the Dock
starts them, and they put their windows on the desktop through the kernel's UI
session calls. The desktop itself (UIService's window manager, menu bar and
WindowServer) still runs in the kernel, but no app is linked into it any more.

## What is where on the volume

```text
/Applications/Voyager.app/Contents/
    Info.plist                     name, executable, icon
    SevOS/Voyager                  the app's executable
    Resources/Voyager.icns         its icon
/Applications/Activity Monitor.app/Contents/...
/System/Library/CoreServices/Dock.app/Contents/{Info.plist,SevOS/Dock}
/System/Library/CoreServices/About sevOS.app/Contents/...
/System/Library/Preferences/com.nxu.dock.plist   what the Dock keeps
/System/Library/BootDaemons/com.nxu.dock.plist   bootd starts the Dock
/System/Library/Fonts/Inter-{Regular,SemiBold}.ttf   read by every app at start
/System/Library/Resources/Images/*.icns          the icons' source copies
```

The `Info.plist` files and the Dock configuration are tracked in `tools/DiskRoot`;
the executables are built and staged by `make userland` (see
[Building](#building)), and each bundle gets a copy of its icon from
`/System/Library/Resources/Images` when it is staged.

### Info.plist

| Key | Meaning |
| --- | --- |
| `CFBundleName` | The name the Dock shows over the icon. Defaults to the bundle's name without `.app`. |
| `CFBundleExecutable` | The executable in `Contents/SevOS`. Defaults to the same. |
| `CFBundleIconFile` | The icon in `Contents/Resources`; `.icns` is added when it has no extension. |
| `LSUIElement` | `true`: an agent app (About sevOS). The Dock starts it when asked and never shows it. |
| `CFBundleIdentifier`, `CFBundleVersion` | Informational. |

### com.nxu.dock.plist

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `persistent-apps` | array | none | The apps the Dock keeps, left to right: a dict with a `path` (`/Applications/Voyager.app`) or a bare path string. At most 12. |
| `tilesize` | integer | 48 | Icon size in points (16 to 128), multiplied by the display's scale. |
| `show-recents` | boolean | true | Show apps that are running but not kept, and up to 3 that ran recently, after a divider. |
| `show-trash` | boolean | true | The Trash at the right end, after a divider. |

Paths are the user's (`/Applications/...`); the Dock adds the `/disk` mount
point itself.

## The Dock

`/System/Library/CoreServices/Dock.app` is started by bootd
(`com.nxu.dock.plist`: `RunAtLoad`, `KeepAlive`; `-no-dock` disables it). It
waits in `nxu_ui_control(NXU_UI_CONTROL_SESSION)` until the desktop is up (after
the login screen when there is one), reads its configuration and each bundle's
`Info.plist`, and decodes the icons: the image in the `.icns` best suited to the
tile (the smallest at least as big, all modern `.icns` images are PNGs), inflated
with `miniz_oxide`, scaled down by area in premultiplied alpha. On a kernel
without the desktop it sleeps for good rather than letting bootd restart it.

Clicking an app starts `Contents/SevOS/<CFBundleExecutable>` with `nxu_spawn`,
or, if it runs, asks the desktop to bring it forward
(`NXU_UI_CONTROL_ACTIVATE`, only the Dock may). The Dock starts every app,
including the About sevOS the system menu opens and the apps Voyager opens
(the desktop sends it `NXU_UI_MSG_LAUNCH`), so it knows which run: it reaps them with `waitpid` and
shows a dot under the running ones.

The Dock draws its icons, dots and dividers over transparency. The desktop
places that frame centred along the bottom, keeps it above every window, puts
the panel's material under it (the wallpaper behind it, blurred and veiled in
white, with a hairline edge: only the desktop has the wallpaper) and draws the
name bubble over the icon under the pointer as a window of its own, from the
name and position the Dock sends
(`ui-service-nxu/src/dockhost.rs` in UIService.framework).

## Apps in Voyager

A folder named `<Name>.app` is shown as the app: its own icon (the `.icns` its
`Info.plist` names, decoded by UIService's `crates/ui-bundle` and cached per
size), its name without `.app`, kind "Application"; opening it launches the
app (`ui_core::bundle::open`, which is `NXU_UI_CONTROL_LAUNCH`) rather than
entering the folder.

## The UI session calls

`kern/syscall/ui_session_defs.h` is the whole interface; UIService's
`crates/ui-session` mirrors it byte for byte (both check the same sizes).

| Call | What it does |
| --- | --- |
| `nxu_ui_control(NXU_UI_CONTROL_SESSION, &session)` | Waits for the desktop; returns its scale, screen size and menu bar height. |
| `nxu_ui_connect(&info)` | An app: its name, bundle, window size limits, titlebar, menus. The Dock: its kind. Returns the connection number. |
| `nxu_ui_receive(connection, &message, wait)` | The next message: an input event (content coordinates), a redraw request with the content size, a menu command, an animation frame, quit, launch (Dock). `wait` is milliseconds, 0 or `NXU_UI_WAIT_FOREVER`. |
| `nxu_ui_submit(connection, &submit)` | A frame (XRGB content pixels; the Dock's are ARGB), the pointer shape, the menu items' states, whether it animates, close. |
| `nxu_ui_control(NXU_UI_CONTROL_ACTIVATE, pid)` | The Dock: bring that app forward. |
| `nxu_ui_control(NXU_UI_CONTROL_ACTIVITY, &request)` | Activity Monitor's process table. |
| `nxu_ui_control(NXU_UI_CONTROL_LAUNCH, path)` | Open a bundle (Voyager opening an app): the desktop hands it to the Dock. |

The kernel side is `drivers/video/ui_service_bridge.c`. Each connection has a
message queue (pointer moves coalesce, animation frames are one at a time) and
two frame buffers: submit copies the app's frame out of its address space into
the spare one without the lock and swaps it in under it, and the desktop copies
the ready one into the window's backing store. The desktop never touches app
memory and a slow app never stalls it: its window keeps the last frame. Receive
and submit run on any CPU; connect and control stay on the boot CPU. A submit
(and a connect, a launch, an activation) kicks the desktop out of its idle
sleep (`sched_kick_sleepers`), as an input interrupt does, so a frame is on
screen at once rather than at the next 10 ms timer tick.

On the desktop's side (UIService's `ui-service-nxu`) each connection is a
`RemoteApp` (`remote.rs`): the window's chrome, backing store and place, drawn
around the app's content. The close button asks the app to quit and ends it
after two seconds if it does not; an app whose process is gone loses its window.

The desktop handles only the last of the pointer moves queued since its previous
pass (the cursor itself follows every packet), so a drag or a resize does one
step per frame, not one per mouse report. A live resize asks the app for its
content at the new size and shows the window at a size only together with the
app's frame for it, a step behind the pointer, the way macOS does: no empty
content between steps, and one composite per step. An app that has not answered
within half a second gets the pointer's size with blank content. A resizable
window with no maximum of its own can grow to one and a half times its opening
size (within the screen): the kernel keeps a few copies of a window at its
largest size, which a whole screen each at 2x would not fit on i386.

## An app's process

UIService's `crates/ui-app-nxu` runs any `ui_app::App` as a process: it waits
for the desktop, reads Inter from `/System/Library/Fonts`, connects with the
app's `INFO`, `WINDOW` and `MENUS`, and turns messages into `App::event`,
`draw`, `tick` and `menu_command`, applying everything already queued before one
redraw. Directory listings (Voyager) come from the file system calls, the process
table (Activity Monitor) from `NXU_UI_CONTROL_ACTIVITY`. `bundles/<app>` in
UIService.framework is each app's static archive, exporting `UIApplicationMain`.

`frameworks/AppKit.framework/app_main.c` is every bundle executable's `main`: it
runs `UIApplicationMain` on a 512 KiB stack of its own (`app_entry*.S`), since
the initial user stack is 16 KiB on i386.

## Building

`make userland` (and so `make run`, `make check`) runs
`make -C ../UIService.framework nxu-apps`, links each archive in
`build/apps/` with the userland C library and AppKit.framework
(`--gc-sections --strip-debug`), and the staging step puts the executables,
icons, fonts and plists in place. `USER_APP_BUNDLES` in the Makefile lists the
bundles, with each one's `_BUNDLE` path, `_EXEC` name and `_ICON`.

i386: `make run-i386-desktop` stages the same bundles (`I386_DESKTOP_APPS=1`,
`makedefs/i386/userland_apps.mk`, archives from `make nxu-apps-i386`) and the
i386 desktop boot now starts bootd first, as arm64 does, so the Dock runs there
too.

### Adding an app

1. The app is a `ui_app::App` crate in UIService.framework's `apps/`.
2. Add `bundles/<app>` there (copy `bundles/voyager`: a staticlib calling
   `ui_app_nxu::run("/Applications/<Name>.app", <App>::new)`) and list it in
   `NXU_BUNDLES` in UIService's Makefile.
3. Here: put `Contents/Info.plist` in `tools/DiskRoot/Applications/<Name>.app`,
   the icon in `tools/DiskRoot/System/Library/Resources/Images`, and add the
   app to `USER_APP_BUNDLES` with its `_BUNDLE`, `_EXEC` and `_ICON`.
4. Add it to `persistent-apps` to keep it in the Dock.

## Limits

- Eight connections, the Dock's included; an app has one window.
- The panel's blur is of the wallpaper only: windows moved behind the Dock are
  not blurred through it.
- The Trash has no action yet, and recent apps are forgotten at reboot.
- On i386, a mapping changed in a process that runs threads on several CPUs at
  once is not invalidated on the other CPUs (see [i386 SMP](i386/smp.md)). The
  apps and daemons are single-threaded.
