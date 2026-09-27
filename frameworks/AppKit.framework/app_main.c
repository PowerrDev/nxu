/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        frameworks/AppKit.framework/app_main.c
 *
 * main() of every app bundle's executable (/Applications/<Name>.app/Contents/
 * SevOS/<Name>, and the Dock). The app itself is a static library from
 * UIService.framework (bundles/<app>) exporting UIApplicationMain; this runs
 * it on a stack of its own, because the process's initial stack is 16 KiB on
 * i386 (no demand paging there) and an app's first frame parses fonts.
 */

#include <stdint.h>

#define APPKIT_STACK_SIZE (512U * 1024U)

int UIApplicationMain(void);

/* app_entry.S: call entry with the stack pointer at stack_top, then switch back. */
int appkit_call_on_stack(int (*entry)(void), void *stack_top);

static uint8_t g_appkit_stack[APPKIT_STACK_SIZE] __attribute__((aligned(16)));

int
main(void)
{
	return appkit_call_on_stack(UIApplicationMain, g_appkit_stack + sizeof(g_appkit_stack));
}
