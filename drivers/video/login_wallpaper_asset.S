/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/video/login_wallpaper_asset.S
 *
 * Embed the generated QOI login wallpaper in the kernel image.
 */
.section .rodata.login_wallpaper, "a", %progbits
.balign 16
.global nxu_login_wallpaper_start
.global nxu_login_wallpaper_end
nxu_login_wallpaper_start:
.incbin NXU_LOGIN_WALLPAPER_QOI
nxu_login_wallpaper_end:
