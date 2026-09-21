/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        kern/tests/sound_test.h
 *
 * Plays audio through the kernel path (the audio driver, the /dev/audio0
 * character device, the boot chime) so the host can check what really
 * reached QEMU's audio backend.
 */

#ifndef NXU_KERN_TESTS_SOUND_TEST_H
#define NXU_KERN_TESTS_SOUND_TEST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The test tone: stereo 16-bit at 44100 Hz. Each channel is a triangle wave
 * of exactly 100 frames (441 Hz), the right channel the left one inverted,
 * so the host can tell both channels and the frame order apart. The signal is
 * defined by this formula alone (tools/audio/verify_capture.py has the same),
 * frame n being the n-th frame after the start of the tone.
 */
#define SOUND_TEST_TONE_PERIOD_FRAMES 100U
#define SOUND_TEST_TONE_AMPLITUDE 6000
#define SOUND_TEST_TONE_FRAMES 66150U /* 1.5 s */
#define SOUND_TEST_TAIL_FRAMES 13230U /* 0.3 s of silence after it, so the host's buffer is flushed */

void sound_test_fill_tone(int16_t *frames, uint32_t first_frame, uint32_t count);

/*
 * sound_test_run:
 *
 * Play the tone, then the boot chime, in a kernel thread that sleeps on the
 * driver's wait queue where interrupts are delivered (the calling thread
 * waits for it), and print one line for each thing that was checked. True
 * when the tone played in full without an underrun or an I/O error.
 */
bool sound_test_run(void);

/*
 * sound_test_run_shared:
 *
 * The same test for a boot that has other work going on and has played the
 * boot chime itself: the tone waits for the chime's thread to release the
 * device and the chime is not played a second time (playsound still plays it
 * once from user space). Callable from any kernel thread once the scheduler
 * and interrupts run.
 */
bool sound_test_run_shared(void);

#endif
