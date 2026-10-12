# Userland ELF32 frameworks. Outputs never share the ARM64 staging tree.
I386_GUI_FRAMEWORKS ?= 0
I386_FRAMEWORK_PIC := $(I386_USER_BUILD)/framework-pic
I386_FRAMEWORK_CFLAGS = $(filter-out -fno-pic,$(I386_USER_CFLAGS)) -fPIC
I386_FRAMEWORK_LDFLAGS := -m elf_i386 -shared --hash-style=sysv -z max-page-size=4096 -z relro -z now
I386_FRAMEWORK_RUNTIME_OBJECTS = $(addprefix $(I386_FRAMEWORK_PIC)/,frameworks/lib/syscall_i386.o \
	frameworks/lib/string.o frameworks/lib/thread_i386.o frameworks/lib/allocator.o \
	frameworks/lib/abort.o frameworks/lib/clock.o libk/udivmoddi4.o)
I386_FRAMEWORK_FOUNDATION_OBJECTS = $(addprefix $(I386_FRAMEWORK_PIC)/,frameworks/CoreFoundation.framework/lib/plist/plist.o \
	frameworks/CoreFoundation.framework/lib/service/service_config.o \
	frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o)

$(I386_FRAMEWORK_PIC)/%.o: %.c
	@mkdir -p $(dir $@)
	$(Q)$(I386_USER_CC) $(I386_FRAMEWORK_CFLAGS) -c $< -o $@

$(I386_FRAMEWORK_PIC)/%.o: %.S
	@mkdir -p $(dir $@)
	$(Q)$(I386_USER_CC) $(I386_FRAMEWORK_CFLAGS) -c $< -o $@

$(I386_USER_BUILD)/Runtime.so: $(I386_FRAMEWORK_RUNTIME_OBJECTS)
	$(Q)$(LD) $(I386_FRAMEWORK_LDFLAGS) --no-undefined -Bsymbolic-functions --soname Runtime.framework/Runtime.so $^ -o $@

$(I386_USER_BUILD)/CoreFoundation.so: $(I386_FRAMEWORK_FOUNDATION_OBJECTS) $(I386_USER_BUILD)/Runtime.so
	$(Q)$(LD) $(I386_FRAMEWORK_LDFLAGS) --no-undefined --soname CoreFoundation.framework/CoreFoundation.so $^ -o $@

I386_WINDOWSERVER_PIC = $(I386_USER_BUILD)/windowserver-pic
I386_COREGRAPHICS_PIC = $(I386_USER_BUILD)/coregraphics-pic
I386_GUI_BUILD_TOOLS := tools/build_gui_pic.py tools/audit_gui_frameworks.py tools/gui_elf.py \
	frameworks/Runtime.framework/rust/allocator.rs frameworks/Runtime.framework/rust/panic.rs

$(I386_USER_BUILD)/WindowServer.so: $(WINDOWSERVER_PIC_SOURCES) $(I386_GUI_BUILD_TOOLS) \
	frameworks/WindowServer.framework/exports.map $(I386_USER_BUILD)/Runtime.so
	$(Q)python3 tools/build_gui_pic.py --arch i386 --offline --freestanding-sysroot --framework WindowServer \
		--runtime-dir "$(abspath $(I386_USER_BUILD))" --output "$(abspath $(I386_WINDOWSERVER_PIC))"
	$(Q)cp "$(I386_WINDOWSERVER_PIC)/audit/WindowServer.so" $@

$(I386_USER_BUILD)/CoreGraphics.so: $(COREGRAPHICS_PIC_SOURCES) $(COREGRAPHICS_PIC_ASSETS) \
	$(I386_GUI_BUILD_TOOLS) $(UISERVICE_DIR)/Cargo.lock frameworks/CoreGraphics.framework/lib/windowserver.c \
	frameworks/DesktopService.framework/lib/session.c frameworks/DesktopService.framework/lib/host.c \
	frameworks/CoreGraphics.framework/exports.map frameworks/DesktopService.framework/exports.map \
	$(I386_USER_BUILD)/Runtime.so $(I386_USER_BUILD)/CoreFoundation.so
	$(Q)python3 tools/build_gui_pic.py --arch i386 --offline --freestanding-sysroot --framework CoreGraphics \
		--runtime-dir "$(abspath $(I386_USER_BUILD))" --output "$(abspath $(I386_COREGRAPHICS_PIC))"
	$(Q)cp "$(I386_COREGRAPHICS_PIC)/audit/CoreGraphics.so" $@

$(I386_USER_BUILD)/DesktopService.so: $(I386_USER_BUILD)/CoreGraphics.so
	$(Q)cp "$(I386_COREGRAPHICS_PIC)/audit/DesktopService.so" $@

$(I386_FRAMEWORK_PIC)/frameworks/BootDaemons.framework/desktop_service.o: frameworks/BootDaemons.framework/desktop_service.c $(I386_USER_BUILD)/DesktopService.so
	@mkdir -p $(dir $@)
	$(Q)$(I386_USER_CC) $(I386_FRAMEWORK_CFLAGS) -I"$(I386_COREGRAPHICS_PIC)/include" \
		-c frameworks/BootDaemons.framework/desktop_service.c -o $@

I386_DYNAMIC_LDFLAGS := -m elf_i386 -pie --no-undefined --hash-style=sysv -z max-page-size=4096 -z relro -z now \
	--dynamic-linker /System/Library/CoreServices/dyrld-runtime

$(I386_USER_BUILD)/desktop_service: $(I386_FRAMEWORK_PIC)/frameworks/crt0_i386.o \
	$(I386_FRAMEWORK_PIC)/frameworks/BootDaemons.framework/desktop_service.o \
	$(I386_USER_BUILD)/DesktopService.so $(I386_USER_BUILD)/CoreGraphics.so $(I386_USER_BUILD)/Runtime.so
	$(Q)$(LD) $(I386_DYNAMIC_LDFLAGS) $^ -o $@

$(I386_USER_BUILD)/windowserver_service: $(I386_FRAMEWORK_PIC)/frameworks/crt0_i386.o \
	$(I386_FRAMEWORK_PIC)/frameworks/BootDaemons.framework/windowserver_service.o \
	$(I386_USER_BUILD)/WindowServer.so $(I386_USER_BUILD)/CoreFoundation.so $(I386_USER_BUILD)/Runtime.so
	$(Q)$(LD) $(I386_DYNAMIC_LDFLAGS) $^ -o $@

$(I386_USER_BUILD)/dyrld: $(I386_USER_COMMON_OBJECTS) $(I386_USER_BOOTSTRAP_CLIENT) \
	$(I386_USER_BUILD)/frameworks/BootDaemons.framework/dyrld.o $(I386_USER_BUILD)/libk/sha256.o $(I386_USER_RUNTIME)
	$(Q)$(LD) $(I386_USER_LDFLAGS) $(filter %.o,$^) $(I386_USER_RUNTIME) -o $@

$(I386_USER_BUILD)/dyrld-runtime: $(I386_USER_COMMON_OBJECTS) $(I386_USER_BOOTSTRAP_CLIENT) \
	$(I386_USER_BUILD)/frameworks/Dyrld.framework/runtime.o $(I386_USER_RUNTIME)
	$(Q)$(LD) $(I386_USER_LDFLAGS) $(filter %.o,$^) $(I386_USER_RUNTIME) -o $@

I386_GUI_LIBRARIES = $(addprefix $(I386_USER_BUILD)/,Runtime.so CoreFoundation.so WindowServer.so CoreGraphics.so DesktopService.so)
I386_GUI_SERVICES = $(addprefix $(I386_USER_BUILD)/,dyrld dyrld-runtime windowserver_service desktop_service)

.PHONY: i386-frameworks
i386-frameworks: $(I386_GUI_LIBRARIES) $(I386_GUI_SERVICES)

ifeq ($(I386_GUI_FRAMEWORKS),1)
i386-userland: i386-frameworks
$(I386_USER_STAMP): $(I386_GUI_LIBRARIES) $(I386_GUI_SERVICES) \
	$(addprefix frameworks/BootDaemons.framework/Services/com.nxu.,dyrld.plist windowserver.plist desktop.plist)
endif

$(I386_USER_BUILD)/libk/sha256.o: I386_USER_CFLAGS += -Ilibk
$(I386_FRAMEWORK_PIC)/frameworks/BootDaemons.framework/windowserver_service.o: I386_FRAMEWORK_CFLAGS += -I$(WINDOWSERVER_INCLUDE)

-include $(I386_FRAMEWORK_RUNTIME_OBJECTS:%.o=%.d) $(I386_FRAMEWORK_FOUNDATION_OBJECTS:%.o=%.d)
-include $(I386_FRAMEWORK_PIC)/frameworks/BootDaemons.framework/desktop_service.d
-include $(I386_FRAMEWORK_PIC)/frameworks/BootDaemons.framework/windowserver_service.d
-include $(I386_USER_BUILD)/frameworks/Dyrld.framework/runtime.d $(I386_USER_BUILD)/frameworks/BootDaemons.framework/dyrld.d
