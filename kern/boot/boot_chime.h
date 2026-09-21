/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/boot/boot_chime.h
 *
 * The boot chime: a sound the kernel plays when display output starts.
 *
 * The sound is /System/Library/Resources/Audio/Boot_Audio.mp3 in the system
 * volume. The kernel has no MP3 decoder, so it recognises the MP3 (and says
 * what it is in the boot log) and plays Boot_Audio.wav beside it, the same
 * sound decoded once by `make boot-audio`. Nothing is embedded in the kernel
 * image.
 *
 * The display comes up mid-scan of the device bus, long before the system
 * volume is mounted and before the scheduler exists, so the chime is a
 * hand-off in two steps and never delays the boot:
 *
 *   boot_chime_arm     display output has started: note the time, say the
 *                      chime is armed (called from the driver bring-up)
 *   boot_chime_start   the system volume is mounted: start a kernel thread
 *                      that waits (with a time limit, saying why if it
 *                      gives up) for the sound device and the file, then
 *                      plays through /dev/audio0, feeding the device as its
 *                      interrupts free periods
 *
 * `-no-chime` on the boot command line turns it off.
 */

#ifndef NXU_KERN_BOOT_BOOT_CHIME_H
#define NXU_KERN_BOOT_BOOT_CHIME_H

#include <stdbool.h>

#define BOOT_CHIME_MP3_PATH "/disk/System/Library/Resources/Audio/Boot_Audio.mp3"
#define BOOT_CHIME_WAV_PATH "/disk/System/Library/Resources/Audio/Boot_Audio.wav"

void boot_chime_arm(void);
void boot_chime_start(void);

typedef enum {
	BOOT_CHIME_IDLE,	/* not started: disabled, not armed, or the volume is not mounted yet */
	BOOT_CHIME_PLAYING,	/* the thread is waiting for the device and the file, or streaming */
	BOOT_CHIME_PLAYED,	/* played in full without an underrun */
	BOOT_CHIME_FAILED	/* the thread ended and the chime did not play */
} boot_chime_state_t;

/* Where the boot chime thread is; for a test that must wait for the sound device to be free. */
boot_chime_state_t boot_chime_state(void);

/*
 * boot_chime_play:
 *
 * Play the chime now, in the calling thread (a thread the scheduler can run
 * something else in place of): wait for the device and the file, describe the
 * MP3, read the WAV, open /dev/audio0, stream the samples and drain. What the
 * boot thread does, and what the sound test calls. True when all of it played
 * without an underrun.
 */
bool boot_chime_play(void);

#endif
