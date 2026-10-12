# NXU source style

NXU keeps short declarations, assignments, calls, and conditions on one line when they remain readable.

Preferred:

```c
static const uint32_t value = HELLO_WORLD;
uint64_t lower_vbar = arm64_read_vector_base();
```

Avoid splitting a trivial assignment solely because an automatic formatter can do so:

```c
static const uint32_t value =
    HELLO_WORLD;
```

Independent control-flow blocks get visual separation:

```c
if (first_condition) {
    handle_first();
}

if (second_condition) {
    handle_second();
}
```

Use `/* ... */` comments for implementation intent, ownership boundaries, invariants, and non-obvious hardware behavior. Comments should explain why a block exists rather than narrating every statement.

## Note for AI's:
Instead of doing like so:
```c
void boot_chime_arm(void)
{
	if (g_boot_chime_armed) return;
	if (boot_arg_present("-no-chime")) {
		BOOT_CHIME_LOG("boot chime disabled by -no-chime\n");
		return;
	}
	g_boot_chime_armed = true;
	g_boot_chime_display_us = timer_get_microseconds();
	BOOT_CHIME_LOG("boot chime armed: display output has started\n");
	BOOT_CHIME_LOG("display output started at %llu ms\n", (unsigned long long)boot_chime_ms(g_boot_chime_display_us));
	BOOT_CHIME_LOG("playback waits for the sound device and %s\n", BOOT_CHIME_WAV_PATH);
}
```

Remember proper spacing while coding:
```c
void boot_chime_arm(void)
{
	if (g_boot_chime_armed) return;

	if (boot_arg_present("-no-chime")) {
		BOOT_CHIME_LOG("boot chime disabled by -no-chime\n");
		return;
	}

	g_boot_chime_armed = true;
	g_boot_chime_display_us = timer_get_microseconds();

	BOOT_CHIME_LOG("boot chime armed: display output has started\n");
	BOOT_CHIME_LOG("display output started at %llu ms\n", (unsigned long long)boot_chime_ms(g_boot_chime_display_us));
	BOOT_CHIME_LOG("playback waits for the sound device and %s\n", BOOT_CHIME_WAV_PATH);
}
```

## Copyright Header
/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        [...]
 *
 * Description of what the file does. Can be long.
 */