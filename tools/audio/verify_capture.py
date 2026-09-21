#!/usr/bin/env python3
"""
Check what QEMU's wav audiodev recorded while the kernel played the sound test.

usage: verify_capture.py <capture.wav> <Boot_Audio.wav> [--chime-first] --tone [--chimes N]

The capture is what really reached the host's audio backend, so this is the
proof that the driver's samples arrived: not silent, the right length, and the
right samples.

  --tone       the kernel's test tone (kern/tests/sound_test.h): 66150 frames of
               a 100-frame triangle wave, the right channel the left inverted,
               then 13230 frames of silence. It is found at the first sound in
               the capture and compared frame by frame against the formula.
  --chimes N   the boot chime, N times, one after another after that: each is
               located by a 64-frame signature from the reference file, then
               compared frame by frame with it.
  --chime-first  one chime before the tone instead: the recording of a boot that
               plays the chime as it starts and runs the tone test after it (the
               unified boot). The tone is then the first sound after that chime.

A recording stops where QEMU stopped, and QEMU does not finalize the header when
it is killed or exits from a debug-exit device: the sizes in it are 0 then, so
the data is taken to run to the end of the file.

Exit status 0 when every check held; each check prints one line.
"""

import array
import struct
import sys

TONE_PERIOD = 100
TONE_AMPLITUDE = 6000
TONE_FRAMES = 66150
TAIL_FRAMES = 13230
RATE = 44100

failures = 0


def check(condition, message):
    global failures
    print(("ok    " if condition else "FAIL  ") + "verify_capture: " + message)
    if not condition:
        failures += 1
    return condition


def read_wav(path):
    """The (frames as array('h') of interleaved samples, channels, rate) of a 16-bit PCM WAV file."""
    with open(path, "rb") as handle:
        data = handle.read()

    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("verify_capture: %s is not a WAV file" % path)

    offset = 12
    channels = rate = bits = None
    samples = None

    while offset + 8 <= len(data):
        tag = data[offset:offset + 4]
        size = struct.unpack("<I", data[offset + 4:offset + 8])[0]
        body = offset + 8

        if tag == b"fmt ":
            _, channels, rate, _, _, bits = struct.unpack("<HHIIHH", data[body:body + 16])
        elif tag == b"data":
            # A recording that was never finalized has 0 (or a huge value) here: use what is in the file.
            end = len(data) if size == 0 or body + size > len(data) else body + size
            samples = array.array("h")
            samples.frombytes(data[body:end - ((end - body) % 4)])
            break

        offset = body + size + (size & 1)

    if channels is None or samples is None:
        raise SystemExit("verify_capture: %s has no fmt or data chunk" % path)

    if (channels, rate, bits) != (2, RATE, 16):
        raise SystemExit("verify_capture: %s is %d-bit, %d channel(s), %d Hz; expected 16-bit stereo at %d Hz" % (path, bits, channels, rate, RATE))

    return samples


def tone_frame(index):
    phase = index % TONE_PERIOD
    step = 2 * TONE_AMPLITUDE // (TONE_PERIOD // 2)

    if phase < TONE_PERIOD // 2:
        left = -TONE_AMPLITUDE + phase * step
    else:
        left = TONE_AMPLITUDE - (phase - TONE_PERIOD // 2) * step

    return left, -left


def first_sound(samples, start=0, threshold=0):
    """The frame index of the first frame at or after start with a sample above the threshold, or None."""
    for index in range(start * 2, len(samples)):
        if abs(samples[index]) > threshold:
            return index // 2

    return None


def compare(capture, at, reference, first, count, tolerance):
    """How many of the count frames of reference (from frame first) equal the capture's from frame at, within tolerance."""
    good = 0
    limit = min(count, len(reference) // 2 - first, len(capture) // 2 - at)

    for index in range(limit):
        a = 2 * (at + index)
        b = 2 * (first + index)

        if abs(capture[a] - reference[b]) <= tolerance and abs(capture[a + 1] - reference[b + 1]) <= tolerance:
            good += 1

    return good, limit


def verify_tone(capture, search_from=0):
    start = first_sound(capture, search_from, 0)

    if not check(start is not None, "the capture is not silent"):
        return None

    print("      the tone starts at frame %d (%.0f ms into the recording)" % (start, start * 1000.0 / RATE))

    reference = array.array("h")

    for index in range(TONE_FRAMES):
        left, right = tone_frame(index)
        reference.append(left)
        reference.append(right)

    good, limit = compare(capture, start, reference, 0, TONE_FRAMES, 2)

    check(limit == TONE_FRAMES, "the recording holds all %d frames of the tone (%d)" % (TONE_FRAMES, limit))
    check(good == TONE_FRAMES, "every frame of the tone equals the formula, both channels (%d of %d)" % (good, TONE_FRAMES))

    # Each channel is the inverse of the other, so a swapped or shifted channel cannot pass.
    inverted = sum(1 for index in range(start, start + TONE_FRAMES) if capture[2 * index] == -capture[2 * index + 1])
    check(inverted == TONE_FRAMES, "the right channel is the left one inverted in every frame (%d of %d)" % (inverted, TONE_FRAMES))

    tail_start = start + TONE_FRAMES
    tail = capture[2 * tail_start:2 * (tail_start + TAIL_FRAMES)]
    check(len(tail) == 2 * TAIL_FRAMES and max((abs(v) for v in tail), default=0) <= 2, "the %d frames after the tone are silence" % TAIL_FRAMES)

    peak = max(abs(v) for v in capture[2 * start:2 * tail_start])
    check(peak >= TONE_AMPLITUDE - 2 and peak <= TONE_AMPLITUDE + 2, "the peak is the tone's amplitude (%d)" % peak)

    return tail_start + TAIL_FRAMES


def verify_chime(capture, reference, search_from, number, label=None):
    name = label or "chime %d" % number
    frames = len(reference) // 2
    window = 64

    # A 64-frame signature from the loud part of the chime, found byte for byte.
    signature_at = 2000
    signature = reference[2 * signature_at:2 * (signature_at + window)].tobytes()
    position = capture.tobytes().find(signature, 4 * search_from)

    if not check(position >= 0 and position % 4 == 0, "%s: its signature is in the recording" % name):
        return None

    start = position // 4 - signature_at

    good, limit = compare(capture, start, reference, 0, frames, 2)

    print("      %s starts at frame %d (%.0f ms)" % (name, start, start * 1000.0 / RATE))
    check(limit == frames, "%s: the recording holds all %d frames (%d)" % (name, frames, limit))
    check(good == frames, "%s: every frame equals Boot_Audio.wav (%d of %d)" % (name, good, frames))
    check(abs(frames * 1000 // RATE - 2356) <= 1, "%s: %d ms long" % (name, frames * 1000 // RATE))
    return start + frames


def main(argv):
    if len(argv) < 4 or "--tone" not in argv and "--chimes" not in argv:
        print(__doc__)
        return 2

    capture = read_wav(argv[1])
    reference = read_wav(argv[2])
    chimes = int(argv[argv.index("--chimes") + 1]) if "--chimes" in argv else 0

    print("verify_capture: %s: %d frames, %.3f s" % (argv[1], len(capture) // 2, len(capture) / 2 / RATE))

    position = 0

    if "--chime-first" in argv:
        position = verify_chime(capture, reference, 0, 0, "boot chime")

        if position is None:
            return 1

    if "--tone" in argv:
        position = verify_tone(capture, position)

        if position is None:
            return 1

    for number in range(1, chimes + 1):
        position = verify_chime(capture, reference, position, number)

        if position is None:
            break

    print("verify_capture: %s" % ("passed" if failures == 0 else "FAILED (%d)" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
