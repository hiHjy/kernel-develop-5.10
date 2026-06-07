KDIR ?= $(abspath ../../kernel)
BUILD_DIR ?= $(CURDIR)/build
MODULE ?= dma_chardev_template

unexport AR AS CC CPP CXX LD NM OBJCOPY OBJDUMP READELF STRIP
unexport CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
unexport PKG_CONFIG PKG_CONFIG_LIBDIR PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR
unexport CONFIGURE_FLAGS SYSROOT
unexport HOSTCFLAGS HOSTCXXFLAGS HOSTLDFLAGS HOSTLDLIBS
unexport CFLAGS_FOR_BUILD CPPFLAGS_FOR_BUILD CXXFLAGS_FOR_BUILD LDFLAGS_FOR_BUILD

ARCH ?= arm64
DEFAULT_CROSS_COMPILE := $(abspath ../../toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-)
ifneq ($(USE_ENV_CROSS_COMPILE),1)
override CROSS_COMPILE := $(DEFAULT_CROSS_COMPILE)
endif
HOSTCC := /usr/bin/gcc
HOSTCXX := /usr/bin/g++
CLEAN_ENV := env -i HOME=$(HOME) USER=$(USER) LOGNAME=$(LOGNAME) SHELL=/bin/bash LC_ALL=C PATH=$(abspath ../../toolchain/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin):/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

.PHONY: all modules clean distclean prepare

all: modules

modules: prepare
	$(CLEAN_ENV) $(MAKE) -C $(KDIR) M=$(BUILD_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) HOSTCC=$(HOSTCC) HOSTCXX=$(HOSTCXX) modules
	cp -f $(BUILD_DIR)/$(MODULE).ko $(CURDIR)/$(MODULE).ko

prepare:
	mkdir -p $(BUILD_DIR)
	cp -f $(MODULE).c $(BUILD_DIR)/
	printf 'obj-m += $(MODULE).o\n' > $(BUILD_DIR)/Makefile

clean:
	$(CLEAN_ENV) $(MAKE) -C $(KDIR) M=$(BUILD_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) HOSTCC=$(HOSTCC) HOSTCXX=$(HOSTCXX) clean || true
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -f $(CURDIR)/$(MODULE).ko
