obj-m += cmp_rebar_shim.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

install: all
	bash ./scripts/install.sh

uninstall:
	bash ./scripts/uninstall.sh

check:
	bash -n scripts/*.sh
	python3 -m py_compile tools/p2p_test.py

help:
	@echo "targets:"
	@echo "  make            build cmp_rebar_shim.ko for the running kernel"
	@echo "  make install    install module + modprobe config + initramfs (root)"
	@echo "  make uninstall  remove the above (root)"
	@echo "  make check      shell syntax + python byte-compile"
	@echo "  make clean      remove build artifacts"

.PHONY: all clean install uninstall check help
