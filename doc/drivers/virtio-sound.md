# VirtIO Sound

## Overview

The VirtIO Sound driver plays PCM audio on the virtual sound card (VirtIO
device ID 25, OASIS VirtIO 1.2 section 5.14), on both transports: VirtIO-MMIO
on arm64 (`-device virtio-sound-device`) and VirtIO-PCI on i386
(`-device virtio-sound-pci`). It is output only. The device's capture stream
is reported but never used, and the rx queue is left unconfigured.

```text
program ── write / ioctl ──> /dev/audio0 (devfs) ──> virtio_sound_dev.c
kernel  ── boot chime ─────────────────────────────────┘        │
                                                                 v
                               virtio_sound_pcm.c: stream state machine, ring of periods
                                                                 │
                     virtio_sound.c: attach, control queue       │
                                                                 v
                     virtqueues: controlq, eventq, txq ──> QEMU virtio-snd ──> audio backend
```

| File | Role |
| --- | --- |
| `drivers/virtio/virtio_snd.h` | the wire format, byte for byte from the specification |
| `drivers/virtio/virtio_sound_core.[ch]` | pure decisions: feature negotiation, the stream state machine, format/rate names and negotiation, error codes (host tested) |
| `drivers/virtio/virtio_sound.c` | attach, queues, the synchronous control queue, item information |
| `drivers/virtio/virtio_sound_pcm.c` | the output stream, the ring of periods, completions, events, the interrupt |
| `drivers/virtio/virtio_sound_dev.c` | `/dev/audio0`: the ABI of `kern/audio/audio_defs.h` on top of the playback interface |
| `vfs/devfs.c` | the `/dev` namespace |
| `libk/wav.c`, `libk/mp3.c` | the WAV reader and converter, the MP3 recogniser |
| `kern/boot/boot_chime.c` | the boot chime |
| `frameworks/BootDaemons.framework/playsound.c` | the user program |

## Attach

`virtio_snd_attach()` runs from the bus scan, on either transport, and reports
every fact on its own line under the function that prints it (the default log
shows all of these, detail is behind `-v`):

1. where the device is: MMIO frame and GIC INTID, or PCI address and PIC line;
   vendor and device ID.
2. **Feature negotiation.** The transport resets the device, reads what it
   offers and confirms with `FEATURES_OK`; the driver logs each offered bit by
   name (`F_VERSION_1`, `F_RING_INDIRECT_DESC`, ...) as accepted or declined.
   Only `VIRTIO_F_VERSION_1` is accepted: the sound device defines no features of
   its own that the driver uses, and packed rings or in-order use would change the
   queue layout. A device without `VERSION_1` (a legacy device) is logged with the
   reason, left in the `FAILED` state and skipped: the boot goes on.
3. The configuration space: jacks, PCM streams, channel maps.
4. Queues: `controlq` (8 descriptors), `eventq` (16), `txq` (64); `rxq` is not
   configured. The control queue never interrupts, it is polled. 16 empty event
   buffers are posted.
5. The interrupt handler is bound (`interrupt handler attached: completions
   arrive by interrupt`) or, on a transport that is polling, the log says so.
6. `DRIVER_OK`, then the queries: `PCM_INFO` for every stream (direction, channel
   range, formats and rates by name, features), `JACK_INFO` and `CHMAP_INFO` when
   the configuration has any. QEMU reports two streams (0 output, 1 input; S8 U8
   S16 U16 S32 U32 FLOAT; 14 rates from 5512 to 384000 Hz; 1 to 2 channels), no
   jacks and no channel maps; it answers `NOT_SUPP` if asked, which the driver
   tolerates.
7. The playback stream is the first output stream that takes 16-bit stereo at
   44.1 kHz; the device is registered in the I/O registry under
   `DriverKitAudioFamily` and as `/dev/audio0`.

With no sound device the scan logs `virtio_snd_probe: no VirtIO Sound device` and
carries on. `-no-sound` on the boot line skips the driver.

## The control queue

`virtio_snd_control()` builds one request in a DMA page, sends it as a
device-readable descriptor followed by a device-writable one for the response, and
polls the used ring for at most two seconds. A request nobody answers marks the
device dead: it may still write into the page, so nothing more is ever sent.
Statuses map to `virtio_snd_error_t`; a status the specification does not define
is an error, so a zeroed response is never mistaken for success.

## The stream

The state machine of section 5.14.6.6 (UNINIT, PARAMS_SET, PREPARED, STARTED,
STOPPED, RELEASED) is enforced in the driver: every request is checked with
`virtio_snd_pcm_transition()` before it is sent, and an illegal one is refused
(`VIRTIO_SND_E_ILLEGAL`, logged) because the device treats it as a fatal driver
error. The state changes only when the device answers OK.

Parameters are negotiated against what `PCM_INFO` reported: the rate and channel
count are honoured when the stream has them, otherwise the nearest above (then
below) and the nearest count; the sample format falls back to S16, S32, float, U8,
S8. The format actually used is handed back to the caller. Streams are set up lazily:
the first write does `SET_PARAMS`, `PREPARE` and allocates the ring.

### The ring of periods

16 periods, one DMA page each. A period is one message: the 4-byte header (the
stream ID) and up to 4080 bytes of samples in the first descriptor, the 8-byte
status the device writes back in a second descriptor at the end of the page.
4080 bytes is a whole number of frames for mono or stereo at 8, 16 or 32 bits or
float, so a period never ends inside a frame. At 44.1 kHz stereo 16-bit a period
is about 23 ms and the ring 370 ms. The descriptors are taken once per open.

A writer copies into the period at the head of the ring and hands a full one to the
device; the stream starts once 8 periods are queued (or when the writer drains). The
last, partial period is padded with silence to a whole frame.

### Completions, blocking, underruns

The device completes periods in the order it consumes them (when the host's audio
backend has taken the samples, not when they have been heard). The interrupt
handler acknowledges the device (MMIO: write the status back; PCI: reading the ISR
byte clears it), collects the finished periods, frees their slots and wakes the
writer. A writer that finds the ring full:

- **sleeps** on a wait queue when the device's interrupt is bound and it is a
  real thread (a system call or a kernel thread), with interrupts masked so that
  joining the queue and sleeping cannot be split by the handler that would wake
  it;
- **polls** otherwise (the boot context, or a transport with no interrupt): it
  collects the completions itself and yields.

A non-blocking descriptor returns a short count, or `-NXU_SYS_E_AGAIN` if nothing
was taken. A signal interrupts the sleep (`-NXU_SYS_E_INTERRUPTED`, or a short
count if some data was already taken).

An **underrun** is counted when the last queued period completes while the stream
runs and the writer is not draining: the device ran dry and the host plays
silence until more arrives. Nothing is logged from the interrupt handler; the
counts are in the totals and the close summary.

**Drain** hands over the partial period, starts the stream if needed, waits until
every period has completed, waits 120 ms for the host to play out what it buffered,
and stops. **Stop** (and close) release the stream, which completes what the
device still holds, so queued audio is cut.

All ring state is only touched with interrupts masked: this kernel has one CPU and
that is the lock. SMP would need a real one.

### Events

Sixteen event buffers stay posted on `eventq`; each event (`JACK_CONNECTED`,
`JACK_DISCONNECTED`, `PCM_PERIOD_ELAPSED`, `PCM_XRUN`, `CTL_NOTIFY`) is counted, the
last one remembered and the buffer reposted. QEMU 11 does not send any (its event
queue is unimplemented), so this path is exercised only up to the posting.

## `/dev/audio0`

The character device of the playback stream, in `devfs`, mounted at `/dev`.

- **open** for writing, one program at a time (`-NXU_SYS_E_BUSY` for a second);
  needs `NXU_CAP_AUDIO` (a process started without it gets `-NXU_SYS_E_DENIED`; the
  kernel and PID 1 always have it). Opening a device node for writing does not need
  `NXU_CAP_FS_WRITE`.
- **write** PCM frames in the negotiated format: 16-bit stereo at 44.1 kHz until
  told otherwise.
- **ioctl** (`nxu_ioctl`, `NXU_SYS_IOCTL`): `NXU_AUDIO_GET_INFO` (what the device
  supports, the current format, state and counters), `NXU_AUDIO_SET_PARAMS`,
  `NXU_AUDIO_DRAIN`, `NXU_AUDIO_STOP`, `NXU_AUDIO_SET_NONBLOCK`. The structures are
  in `kern/audio/audio_defs.h`.
- **close** stops what is queued, frees the ring and logs the totals.

## WAV files and `playsound`

`libk/wav.c` reads RIFF/WAVE without trusting a size in the file (LIST and other
chunks are skipped, the pad byte of odd chunks honoured, a wrong RIFF size ignored,
a data chunk that claims more than the file holds accepted and flagged) and
refuses what is malformed or unsupported with a reason. It handles 8, 16, 24 and
32-bit PCM, 32 and 64-bit float and `WAVE_FORMAT_EXTENSIBLE`, and converts any of
them to 16-bit stereo (mono on both channels, extra channels dropped) without
floating-point instructions. `make test-audio-host` runs it under ASan and UBSan
against every prefix of four files, every byte set to six values, tens of thousands
of random mutations and noise (355759 checks).

`playsound [file.wav]` (arm64 and i386, needs `NXU_CAP_AUDIO`) reads the file, parses
it, opens `/dev/audio0`, negotiates, streams and reports the file, format, duration,
what the device took, bytes written, periods, interrupts, underruns and time. With no
file it plays the boot chime.

## The boot chime

`/System/Library/Resources/Audio/Boot_Audio.mp3` is the boot sound (MPEG-1 Layer III,
44.1 kHz stereo 128 kbps, ID3v2.3 tag). The kernel has no MP3 decoder, so the
system volume also holds `Boot_Audio.wav`, the same sound decoded once with macOS
`afconvert` by `make boot-audio` (`tools/audio/make_boot_audio.sh`, checked with the
kernel's own WAV reader) and committed. Both files are in the arm64 and the i386
disk images. Nothing is embedded in the kernel.

The display comes up in the middle of the bus scan, before the scheduler and long
before the system volume is mounted, so the chime is a hand-off:

1. `boot_chime_arm()` (called the moment the first splash frame is shown) logs
   `boot chime armed` and the time.
2. `boot_chime_start()` (after the system volume mounts) starts a kernel thread that
   waits at most 10 s for the sound device and the file, logging which was missing
   if it gives up, then logs what the MP3 is (from its own headers), that the
   pre-decoded WAV is used, reads it, plays it through `/dev/audio0` and logs the
   start and end times and the delay after display output started.

The thread only runs once the scheduler starts, which is the first moment the
device's interrupts are enabled, so this is the earliest instant the chime can be
played by interrupt; what delays it after the first frame is the boot splash's
3-second hold, then the rest of the initialization before the scheduler. Measured
on QEMU with TCG it starts about 4 s after display output started. `-no-chime`
turns it off.

## Logging

Every line starts with the name of the function that prints it. One fact per line at
the default verbosity; raw feature masks, per-request traces, per-transition
detail and counters that only mean something once audio has played are behind `-v`
(`kverbosef`). `virtio_snd_dump()` prints the end-of-boot summary (device, streams,
`/dev/audio0`, interrupts by interrupt or polled, opens, periods, underruns).

## Tests

| Test | What it proves |
| --- | --- |
| `make test-audio-host` | the sound core (356 checks), the WAV reader and converter (355759), the MP3 recogniser (6027), natively under ASan+UBSan |
| `make test TEST=sound` | audible: tone, chime and `playsound` through the host's speakers |
| `sound` in `make check` | the same with QEMU's wav backend; `tools/audio/verify_capture.py` compares the recording with the tone formula and with `Boot_Audio.wav`, sample by sample |
| `make test-i386-sound` | the same on i386 over VirtIO-PCI with the interrupt bound (checks the interrupt count) and polled, plus the boot with no sound device |

## Limits

- Output only; one stream, one open at a time; the 16-period ring is fixed.
- No resampling: a rate the device lacks is adjusted by the device's negotiation,
  not converted.
- Jack and channel-map queries are logged but not used; no controls (`VIRTIO_SND_F_CTLS`).
- The recording of the wav backend has a zeroed header when QEMU is stopped, not
  finalized; the checker reads to the end of the file.
- A boot where `bootd` stalls for seconds (see the note on the flake in the test
  report) starves the chime thread, whose ring holds 370 ms.
