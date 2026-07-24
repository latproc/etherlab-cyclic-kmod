KERNEL_RELEASE ?= $(shell uname -r)
KERNEL_BUILD ?= /lib/modules/$(KERNEL_RELEASE)/build
ETHERLAB_DKMS_NAME ?= ethercat-dkms
ETHERLAB_VERSION ?= $(patsubst /usr/src/$(ETHERLAB_DKMS_NAME)-%,%,$(firstword \
	$(wildcard /usr/src/$(ETHERLAB_DKMS_NAME)-*)))
ETHERLAB_INCLUDE ?= /usr/src/$(ETHERLAB_DKMS_NAME)-$(ETHERLAB_VERSION)/include
ETHERLAB_SYMVERS ?= /var/lib/dkms/$(ETHERLAB_DKMS_NAME)/$(ETHERLAB_VERSION)/$(KERNEL_RELEASE)/$(shell uname -m)/module/Module.symvers

CPPFLAGS ?=
CFLAGS ?= -O2 -g

.PHONY: all modules tools check-build-env install uninstall clean

MODULE_INSTALL_DIR := $(DESTDIR)/lib/modules/$(KERNEL_RELEASE)/extra/cw_ethercat
MODULE_FILES := kernel/cw_ethercat.ko kernel/cw_ethercat_probe.ko

all: modules tools

check-build-env:
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

tools: tools/cw_ec_bus tools/cw_ec_abi_test tools/cw_ec_sdo \
	tools/cw_ec_config tools/cw_ec_config_stress

tools/cw_ec_bus: tools/cw_ec_bus.c include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<"

tools/cw_ec_abi_test: tools/cw_ec_abi_test.c include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<"

tools/cw_ec_sdo: tools/cw_ec_sdo.c include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<"

tools/cw_ec_config: tools/cw_ec_config.c include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<"

tools/cw_ec_config_stress: tools/cw_ec_config_stress.c include/cw_ethercat_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror -std=c11 \
		-I"$(CURDIR)/include" -o "$@" "$<"

install: modules
	install -d "$(MODULE_INSTALL_DIR)"
	install -m 0644 $(MODULE_FILES) "$(MODULE_INSTALL_DIR)"
	@if [ -z "$(DESTDIR)" ]; then depmod -a "$(KERNEL_RELEASE)"; fi

uninstall:
	$(RM) $(addprefix $(MODULE_INSTALL_DIR)/,$(notdir $(MODULE_FILES)))
	@rmdir --ignore-fail-on-non-empty "$(MODULE_INSTALL_DIR)" 2>/dev/null || true
	@if [ -z "$(DESTDIR)" ]; then depmod -a "$(KERNEL_RELEASE)"; fi

clean:
	$(MAKE) -C "$(KERNEL_BUILD)" M="$(CURDIR)/kernel" clean
	$(RM) tools/cw_ec_bus tools/cw_ec_abi_test tools/cw_ec_sdo \
		tools/cw_ec_config tools/cw_ec_config_stress
