# =============================================================================
# i386 port: integration glue
# =============================================================================
#
# Sources that belong to no single area: the seams between them, and (as the
# port is brought up end to end) the shared kernel sources every area assumed
# someone else would link. Included by makedefs/i386.mk.

I386_C_SOURCES += \
    mach/i386/mmio_map.c
