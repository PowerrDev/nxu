# =============================================================================
# i386 userland: the app bundles and the Dock (desktop disks only)
# =============================================================================
#
#   make i386-disk I386_DESKTOP_APPS=1   also stage /Applications/*.app, the
#                                        Dock, the system fonts and the Dock's
#                                        configuration (run-i386-desktop does)
#
# The same bundles as the arm64 disk (see USER_APP_BUNDLES in the top-level
# Makefile, which names each bundle's path, executable and icon): each app is
# a static archive from UIService.framework (make nxu-apps-i386, nightly
# build-std like the desktop's own archive) linked here with the i386 C
# library and AppKit.framework's main. Off by default, so the i386 test disks
# need neither UIService.framework nor a nightly toolchain.

I386_DESKTOP_APPS ?= 0

ifeq ($(I386_DESKTOP_APPS),1)

UISERVICE_I386_APPS_DIR := $(UISERVICE_DIR)/build-i386/apps

I386_USER_APPS := $(addprefix $(I386_USER_BUILD)/apps/,$(USER_APP_BUNDLES))

I386_USER_APPKIT_OBJECTS := \
    $(I386_USER_BUILD)/frameworks/AppKit.framework/app_main.o \
    $(I386_USER_BUILD)/frameworks/AppKit.framework/app_entry_i386.o

.SECONDARY: $(I386_USER_APPKIT_OBJECTS)

.PHONY: uiservice-apps-i386-build

uiservice-apps-i386-build:

	$(MAKE) -C $(UISERVICE_DIR) BUILD_JOBS=$(BUILD_JOBS) nxu-apps-i386

$(UISERVICE_I386_APPS_DIR)/lib%.a: | uiservice-apps-i386-build ;

$(I386_USER_BUILD)/apps/%: $(I386_USER_COMMON_OBJECTS) $(I386_USER_APPKIT_OBJECTS) $(I386_USER_RUNTIME) $(UISERVICE_I386_APPS_DIR)/lib%.a makedefs/user-i386.ld | uiservice-apps-i386-build

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) $(I386_USER_LDFLAGS) --gc-sections --strip-debug $(I386_USER_COMMON_OBJECTS) $(I386_USER_APPKIT_OBJECTS) $(UISERVICE_I386_APPS_DIR)/lib$*.a $(I386_USER_RUNTIME) -o $@

# Extra prerequisites for the staging rule in userland.mk, and what it runs.
$(I386_USER_STAMP): $(I386_USER_APPS) $(USER_SYSTEM_FONTS) frameworks/BootDaemons.framework/Services/com.nxu.dock.plist

define I386_USER_EXTRA_STAGE
	$(Q)mkdir -p $(I386_DISKROOT)/System/Library/Fonts $(I386_DISKROOT)/System/Library/Preferences $(I386_DISKROOT)/System/Library/Resources/Images
	$(Q)cp $(USER_SYSTEM_FONTS) $(I386_DISKROOT)/System/Library/Fonts/
	$(Q)cp frameworks/BootDaemons.framework/Services/com.nxu.dock.plist $(I386_DISKROOT)/System/Library/BootDaemons/
	$(Q)cp tools/DiskRoot/System/Library/Preferences/com.nxu.dock.plist $(I386_DISKROOT)/System/Library/Preferences/
	$(Q)cp tools/DiskRoot/System/Library/Resources/Images/*.icns $(I386_DISKROOT)/System/Library/Resources/Images/
	$(foreach app,$(USER_APP_BUNDLES),$(Q)mkdir -p "$(I386_DISKROOT)/$(USER_APP_$(app)_BUNDLE)/Contents" && cp "tools/DiskRoot/$(USER_APP_$(app)_BUNDLE)/Contents/Info.plist" "$(I386_DISKROOT)/$(USER_APP_$(app)_BUNDLE)/Contents/"
	$(call stage_app,$(app),$(I386_USER_BUILD)/apps/$(app),$(I386_DISKROOT)))
endef

endif
