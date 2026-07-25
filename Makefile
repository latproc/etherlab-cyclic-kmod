KERNEL_RELEASE ?= $(shell uname -r)
KERNEL_BUILD ?= /lib/modules/$(KERNEL_RELEASE)/build
ETHERLAB_DKMS_NAME ?= ethercat-dkms
ETHERLAB_DKMS_SOURCE_DIRS := $(wildcard /usr/src/$(ETHERLAB_DKMS_NAME)-*)
ETHERLAB_VERSION ?= $(patsubst /usr/src/$(ETHERLAB_DKMS_NAME)-%,%,$(firstword \
	$(ETHERLAB_DKMS_SOURCE_DIRS)))
ETHERLAB_INCLUDE ?= /usr/src/$(ETHERLAB_DKMS_NAME)-$(ETHERLAB_VERSION)/include
ETHERLAB_SYMVERS ?= /var/lib/dkms/$(ETHERLAB_DKMS_NAME)/$(ETHERLAB_VERSION)/$(KERNEL_RELEASE)/$(shell uname -m)/module/Module.symvers

CPPFLAGS ?=
CFLAGS ?= -O2 -g
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
LIB_VERSION_MAJOR := 0
LIB_VERSION_MINOR := 16
LIB_VERSION := $(LIB_VERSION_MAJOR).$(LIB_VERSION_MINOR).0
SONAME := libcwethercat.so.$(LIB_VERSION_MAJOR)

.PHONY: all modules lib tools check-build-env test-build-contract \
	install install-lib uninstall uninstall-lib clean

MODULE_INSTALL_DIR := $(DESTDIR)/lib/modules/$(KERNEL_RELEASE)/extra/cw_ethercat
MODULE_FILES := kernel/cw_ethercat.ko kernel/cw_ethercat_probe.ko

LIB_OBJS := lib/cw_ethercat.o
LIB_STATIC := lib/libcwethercat.a
LIB_SHARED := lib/libcwethercat.so.$(LIB_VERSION)
LIB_SONAME_LINK := lib/libcwethercat.so.$(LIB_VERSION_MAJOR)
LIB_LINK := lib/libcwethercat.so
PKGCONFIG := lib/cwethercat.pc

all: modules lib tools

check-build-env:
	@if [ "$(origin ETHERLAB_VERSION)" = "file" ] && \
	    [ "$(words $(ETHERLAB_DKMS_SOURCE_DIRS))" -gt 1 ] && \
	    { [ "$(origin ETHERLAB_INCLUDE)" = "file" ] || \
	      [ "$(origin ETHERLAB_SYMVERS)" = "file" ]; }; then \
		echo "error: multiple EtherLab DKMS source trees found:" >&2; \
		printf '  %s\n' $(ETHERLAB_DKMS_SOURCE_DIRS) >&2; \
		echo "set ETHERLAB_VERSION or explicit ETHERLAB_INCLUDE and ETHERLAB_SYMVERS" >&2; \
		exit 1; \
	fi
	@test -d "$(KERNEL_BUILD)" || { \
		echo "error: kernel build directory not found: $(KERNEL_BUILD)" >&2; \
		exit 1; \
	}
	@test -f "$(ETHERLAB_INCLUDE)/ecrt.h" || { \
		echo "error: EtherLab ecrt.h not found: $(ETHERLAB_INCLUDE)/ecrt.h" >&2; \
		echo "set ETHERLAB_INCLUDE explicitly for a manual EtherLab build" >&2; \
		exit 1; \
	}
	@test -f "$(ETHERLAB_SYMVERS)" || { \
		echo "error: matching EtherLab Module.symvers not found: $(ETHERLAB_SYMVERS)" >&2; \
		echo "set ETHERLAB_SYMVERS explicitly; never use one from another kernel build" >&2; \
		exit 1; \
	}
	@grep -q '[[:space:]]ecrt_request_master[[:space:]]' "$(ETHERLAB_SYMVERS)" || { \
		echo "error: ecrt_request_master is absent from $(ETHERLAB_SYMVERS)" >&2; \
		exit 1; \
	}
	@printf '%s\n' \
		"KERNEL_RELEASE=$(KERNEL_RELEASE)" \
		"KERNEL_BUILD=$(KERNEL_BUILD)" \
		"ETHERLAB_INCLUDE=$(ETHERLAB_INCLUDE)" \
		"ETHERLAB_SYMVERS=$(ETHERLAB_SYMVERS)"

modules: check-build-env
	$(MAKE) -C "$(KERNEL_BUILD)" M="$(CURDIR)/kernel" \
		ETHERLAB_INCLUDE="$(ETHERLAB_INCLUDE)" \
		KBUILD_EXTRA_SYMBOLS="$(ETHERLAB_SYMVERS)" modules

lib: $(LIB_STATIC) $(LIB_SHARED) $(LIB_LINK) $(LIB_SONAME_LINK) $(PKGCONFIG)

lib/cw_ethercat.o: lib/cw_ethercat.c include/cw_ethercat.h \
		include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 -fPIC \
		-I"$(CURDIR)/include" -c -o "$@" "$<"

$(LIB_STATIC): $(LIB_OBJS)
	$(AR) rcs "$@" $^

$(LIB_SHARED): $(LIB_OBJS)
	$(CC) -shared -Wl,-soname,$(SONAME) -o "$@" $^

$(LIB_SONAME_LINK): $(LIB_SHARED)
	ln -sfn "$(notdir $(LIB_SHARED))" "$@"

$(LIB_LINK): $(LIB_SONAME_LINK)
	ln -sfn "$(notdir $(LIB_SONAME_LINK))" "$@"

$(PKGCONFIG): lib/cwethercat.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
		-e 's|@VERSION@|$(LIB_VERSION)|g' \
		"$<" > "$@"

tools: lib tools/cw_ec_bus tools/cw_ec_abi_test tools/cw_ec_sdo \
	tools/cw_ec_config tools/cw_ec_config_stress

test-build-contract:
	./tools/cw_ec_test_build_contract.sh

# Feature tools link libcwethercat. abi_test keeps raw ioctls so hostile
# struct_size/reserved checks exercise the kernel UAPI directly.
tools/cw_ec_bus: tools/cw_ec_bus.c include/cw_ethercat.h include/cw_ethercat_uapi.h \
		$(LIB_STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<" $(LIB_STATIC)

tools/cw_ec_abi_test: tools/cw_ec_abi_test.c include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<"

tools/cw_ec_sdo: tools/cw_ec_sdo.c include/cw_ethercat.h include/cw_ethercat_uapi.h \
		$(LIB_STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<" $(LIB_STATIC)

tools/cw_ec_config: tools/cw_ec_config.c include/cw_ethercat.h include/cw_ethercat_uapi.h \
		$(LIB_STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<" $(LIB_STATIC)

tools/cw_ec_config_stress: tools/cw_ec_config_stress.c include/cw_ethercat.h \
		include/cw_ethercat_uapi.h $(LIB_STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<" $(LIB_STATIC)

install: modules install-lib
	install -d "$(MODULE_INSTALL_DIR)"
	install -m 0644 $(MODULE_FILES) "$(MODULE_INSTALL_DIR)"
	@if [ -z "$(DESTDIR)" ]; then depmod -a "$(KERNEL_RELEASE)"; fi

install-lib: lib
	install -d "$(DESTDIR)$(INCLUDEDIR)"
	install -d "$(DESTDIR)$(LIBDIR)"
	install -d "$(DESTDIR)$(PKGCONFIGDIR)"
	install -m 0644 include/cw_ethercat_uapi.h include/cw_ethercat.h \
		"$(DESTDIR)$(INCLUDEDIR)/"
	install -m 0644 $(LIB_STATIC) "$(DESTDIR)$(LIBDIR)/"
	install -m 0755 $(LIB_SHARED) "$(DESTDIR)$(LIBDIR)/"
	ln -sfn "$(notdir $(LIB_SHARED))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_SONAME_LINK))"
	ln -sfn "$(notdir $(LIB_SONAME_LINK))" \
		"$(DESTDIR)$(LIBDIR)/$(notdir $(LIB_LINK))"
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
		-e 's|@VERSION@|$(LIB_VERSION)|g' \
		lib/cwethercat.pc.in > "$(DESTDIR)$(PKGCONFIGDIR)/cwethercat.pc"

uninstall: uninstall-lib
	$(RM) $(addprefix $(MODULE_INSTALL_DIR)/,$(notdir $(MODULE_FILES)))
	@rmdir --ignore-fail-on-non-empty "$(MODULE_INSTALL_DIR)" 2>/dev/null || true
	@if [ -z "$(DESTDIR)" ]; then depmod -a "$(KERNEL_RELEASE)"; fi

uninstall-lib:
	$(RM) "$(DESTDIR)$(INCLUDEDIR)/cw_ethercat_uapi.h" \
		"$(DESTDIR)$(INCLUDEDIR)/cw_ethercat.h"
	$(RM) "$(DESTDIR)$(LIBDIR)/libcwethercat.a" \
		"$(DESTDIR)$(LIBDIR)/libcwethercat.so" \
		"$(DESTDIR)$(LIBDIR)/libcwethercat.so.$(LIB_VERSION_MAJOR)" \
		"$(DESTDIR)$(LIBDIR)/libcwethercat.so.$(LIB_VERSION)"
	$(RM) "$(DESTDIR)$(PKGCONFIGDIR)/cwethercat.pc"

clean:
	-$(MAKE) -C "$(KERNEL_BUILD)" M="$(CURDIR)/kernel" clean
	$(RM) tools/cw_ec_bus tools/cw_ec_abi_test tools/cw_ec_sdo \
		tools/cw_ec_config tools/cw_ec_config_stress
	$(RM) $(LIB_OBJS) $(LIB_STATIC) $(LIB_SHARED) $(LIB_SONAME_LINK) \
		$(LIB_LINK) $(PKGCONFIG)
