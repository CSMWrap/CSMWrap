# Nuke built-in rules.
.SUFFIXES:

# This is the name that our final executable will have.
# Change as needed.
override OUTPUT := csmwrap

# Target architecture to build for. Default to x86_64.
ARCH := x86_64

# Install prefix; /usr/local is a good, standard default pick.
PREFIX := /usr/local

# Check if the architecture is supported.
ifeq ($(filter $(ARCH),ia32 x86_64),)
    $(error Architecture $(ARCH) not supported)
endif

# Default user QEMU flags. These are appended to the QEMU command calls.
QEMUFLAGS := -m 2G -smp 2

# User controllable host C compiler.
HOST_CC := cc

# User controllable toolchain and toolchain prefix.
TOOLCHAIN :=
TOOLCHAIN_PREFIX :=
ifneq ($(TOOLCHAIN),)
    ifeq ($(TOOLCHAIN_PREFIX),)
        TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
    endif
endif

# User controllable C compiler command.
ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
else
    CC := cc
endif

# User controllable linker command.
LD := $(TOOLCHAIN_PREFIX)ld

# User controllable objcopy command.
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy

# User controllable objdump command.
OBJDUMP := $(TOOLCHAIN_PREFIX)objdump

# User controllable strip command.
STRIP := $(TOOLCHAIN_PREFIX)strip

# Defaults overrides for variables if using "llvm" as toolchain.
ifeq ($(TOOLCHAIN),llvm)
    CC := clang
    LD := ld.lld
endif

# User controllable C flags.
CFLAGS := -g -O2 -pipe

# User controllable C preprocessor flags. We set none by default.
CPPFLAGS :=

# User controllable nasm flags.
NASMFLAGS := -g

# User controllable linker flags. We set none by default.
LDFLAGS :=

# User controllable version string.
BUILD_VERSION := $(shell git describe --tags --always 2>/dev/null || echo "Unknown")

# Check if CC is Clang.
override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep -q '^Target: '; echo $$?)

# Save user CFLAGS, CPPFLAGS, and LDFLAGS before we append internal flags.
override USER_CFLAGS := $(CFLAGS)
override USER_CPPFLAGS := $(CPPFLAGS)
override USER_LDFLAGS := $(LDFLAGS)

override define SEABIOS_CALL
	$(MAKE) -C seabios $(1) \
		HOSTCC="$(HOST_CC)" \
		CC="$(CC)" \
		LD="$(LD)" \
		OBJCOPY="$(OBJCOPY)" \
		OBJDUMP="$(OBJDUMP)" \
		STRIP="$(STRIP)" \
		CFLAGS="$(USER_CFLAGS)" \
		CPPFLAGS="$(USER_CPPFLAGS)" \
		LDFLAGS="$(USER_LDFLAGS)" \
		EXTRAVERSION=\"$(SEABIOS_EXTRAVERSION)\"
endef

# Internal C flags that should not be changed by the user.
override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fshort-wchar \
    -fno-lto \
    -fPIE \
    -ffunction-sections \
    -fdata-sections

# Internal C preprocessor flags that should not be changed by the user.
override CPPFLAGS := \
    -I src \
    -I picoefi/inc \
    -I flanterm/src \
    -I uACPI/include \
    -DUACPI_OVERRIDE_CONFIG \
    -DBUILD_VERSION=\"$(BUILD_VERSION)\" \
    -isystem freestnd-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

obj-$(ARCH)/flanterm/src/flanterm_backends/fb.c.o: override CPPFLAGS += \
	-DFLANTERM_FB_DISABLE_BUMP_ALLOC

# Internal nasm flags that should not be changed by the user.
override NASMFLAGS := \
    $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
    -Wall

# Architecture specific internal flags.
ifeq ($(ARCH),ia32)
    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target i686-unknown-none-elf
    endif
    override CFLAGS += \
        -m32 \
        -march=i686 \
        -mabi=sysv \
        -mno-80387 \
        -mno-mmx
    override LDFLAGS += \
        -m elf_i386
    override NASMFLAGS := \
        -f elf32 \
        $(NASMFLAGS)
endif
ifeq ($(ARCH),x86_64)
    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target x86_64-unknown-none-elf
    endif
    override CFLAGS += \
        -m64 \
        -march=x86-64 \
        -mabi=sysv \
        -mno-80387 \
        -mno-mmx \
        -mno-sse \
        -mno-sse2 \
        -mno-red-zone
    override LDFLAGS += \
        -m elf_x86_64
    override NASMFLAGS := \
        -f elf64 \
        $(NASMFLAGS)
endif

# Internal linker flags that should not be changed by the user.
override LDFLAGS += \
    -nostdlib \
    -pie \
    -z text \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T picoefi/$(ARCH)/link_script.lds

# Use "find" to glob all *.c, *.S, and *.asm files in the tree
# (except the src/arch/* directories, as those are gonna be added
# in the next step).
override SRCFILES := $(shell find -L src cc-runtime/src picoefi/$(ARCH) flanterm/src uACPI/source -type f -not -path 'src/arch/*' 2>/dev/null | LC_ALL=C sort)
# Add architecture specific files, if they exist.
override SRCFILES += $(shell find -L src/arch/$(ARCH) -type f 2>/dev/null | LC_ALL=C sort)
# Obtain the object and header dependencies file names.
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))
ifneq ($(filter $(ARCH),ia32 x86_64),)
override NASMFILES := $(filter %.asm,$(SRCFILES))
endif
override OBJ := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.o) $(ASFILES:.S=.S.o))
ifneq ($(filter $(ARCH),ia32 x86_64),)
override OBJ += $(addprefix obj-$(ARCH)/,$(NASMFILES:.asm=.asm.o))
endif
override HEADER_DEPS := $(addprefix obj-$(ARCH)/,$(CFILES:.c=.c.d) $(ASFILES:.S=.S.d))

# Default target. This must come first, before header dependencies.
.PHONY: all
all:
	$(MAKE) seabios
	$(MAKE) bin-$(ARCH)/$(OUTPUT).efi

# Include header dependencies.
-include $(HEADER_DEPS)

obj-$(ARCH)/src/csmwrap.c.o: src/bins/Csm16.h

obj-$(ARCH)/src/video.c.o: src/bins/vgabios.h

obj-$(ARCH)/src/printf.c.o: override CPPFLAGS += \
    -I nanoprintf

# Rule to convert the final ELF executable to a .EFI PE executable.
bin-$(ARCH)/$(OUTPUT).efi: bin-$(ARCH)/$(OUTPUT) GNUmakefile
	mkdir -p "$(dir $@)"
	$(OBJCOPY) -O binary $< $@
	dd if=/dev/zero of=$@ bs=4096 count=0 seek=$$(( ($$(wc -c < $@) + 4095) / 4096 )) 2>/dev/null

# Link rules for the final executable.
bin-$(ARCH)/$(OUTPUT): GNUmakefile picoefi/$(ARCH)/link_script.lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

# Compilation rules for *.c files.
obj-$(ARCH)/%.c.o: %.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compilation rules for *.S files.
obj-$(ARCH)/%.S.o: %.S GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ifneq ($(filter $(ARCH),ia32 x86_64),)
# Compilation rules for *.asm (nasm) files.
obj-$(ARCH)/%.asm.o: %.asm GNUmakefile
	mkdir -p "$(dir $@)"
	nasm $(NASMFLAGS) $< -o $@
endif

# Rules to download the UEFI firmware per architecture for testing.
edk2-ovmf:
	curl -L https://github.com/osdev0/edk2-ovmf-nightly/releases/latest/download/edk2-ovmf.tar.gz | gunzip | tar -xf -

# Rules for running our executable in QEMU.
.PHONY: run
run: all edk2-ovmf
	mkdir -p boot/EFI/BOOT
ifeq ($(ARCH),ia32)
	cp bin-$(ARCH)/$(OUTPUT).efi boot/EFI/BOOT/BOOTIA32.EFI
	qemu-system-i386 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-drive file=fat:rw:boot \
		$(QEMUFLAGS)
endif
ifeq ($(ARCH),x86_64)
	cp bin-$(ARCH)/$(OUTPUT).efi boot/EFI/BOOT/BOOTX64.EFI
	qemu-system-x86_64 \
		-M q35 \
		-drive if=pflash,unit=0,format=raw,file=edk2-ovmf/ovmf-code-$(ARCH).fd,readonly=on \
		-drive file=fat:rw:boot \
		$(QEMUFLAGS)
endif
	rm -rf boot

# Remove object files and the final executable.
.PHONY: clean
clean: seabios/.config
	$(call SEABIOS_CALL,clean)
	rm -rf bin-$(ARCH) obj-$(ARCH)

# Remove everything built and generated including downloaded dependencies.
.PHONY: distclean
distclean: seabios/.config
	$(call SEABIOS_CALL,distclean)
	rm -rf src/bins
	rm -rf bin-* obj-* .cache compile_commands.json edk2-ovmf

# Install the final built executable to its final on-root location.
.PHONY: install
install: all
	install -d "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	install -m 644 bin-$(ARCH)/$(OUTPUT).efi "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH).efi"

# Try to undo whatever the "install" target did.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH).efi"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"

# SeaBIOS build targets.
SEABIOS_EXTRAVERSION := -CSMWrap-$(BUILD_VERSION)
.PHONY: seabios
seabios: seabios/.config
	$(call SEABIOS_CALL,)

src/bins/Csm16.h: GNUmakefile seabios/out/Csm16.bin
	mkdir -p src/bins
	cd seabios/out && xxd -i Csm16.bin >../../src/bins/Csm16.h

src/bins/vgabios.h: GNUmakefile seabios/out/vgabios.bin
	mkdir -p src/bins
	cd seabios/out && xxd -i vgabios.bin >../../src/bins/vgabios.h

seabios/.config: GNUmakefile seabios-config
	cp seabios-config seabios/.config
	$(call SEABIOS_CALL,olddefconfig)
