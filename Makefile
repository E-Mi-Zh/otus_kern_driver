PWD := $(shell pwd)
KDIR ?= /lib/modules/$(shell uname -r)/build

BLK_MODULE := ex_blk
TEST_DIR := checker
TEST_SCRIPT := $(TEST_DIR)/main.py

SRC_DIR := $(PWD)/src
BUILD_DIR := $(PWD)/build
BUILD_FILES += $(SRC_DIR)/*.ko

CLANG_FORMAT_VERS ?= 19
CLANG_FORMAT := clang-format-$(CLANG_FORMAT_VERS)
CLANG_FORMAT_FLAGS += -i
FORMAT_FILES := $(SRC_DIR)/*.[ch]
PYTHON_FORMAT_TOOL = black
PYTHON_FORMAT_FILES = \
 	$(TEST_DIR)/__init__.py \
 	$(TEST_DIR)/base_tester.py \
 	$(TEST_DIR)/main.py \
 	$(TEST_DIR)/blk_tester.py \

$(shell mkdir -p $(BUILD_DIR))

.PHONY: all clean format check install uninstall help

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	cp $(BUILD_FILES) $(BUILD_DIR)/.

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f dmesg_failure.log

format:
	$(CLANG_FORMAT) $(CLANG_FORMAT_FLAGS) $(FORMAT_FILES)
	@echo "Code formatted with clang-format"

format-python:
	$(PYTHON_FORMAT_TOOL) $(PYTHON_FORMAT_FILES)

check:
	$(TEST_SCRIPT) blk $(BLK_MODULE)

install: all
	@echo "Installing module to /lib/modules/$(shell uname -r)/extra/src"
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a
	@echo "Modules installed."
	@echo "Run 'sudo modprobe $(BLK_MODULE)' to load"

uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/src/$(BLK_MODULE).ko
	depmod -a
	@echo "Modules uninstalled"

load:
	insmod $(BUILD_DIR)/$(BLK_MODULE).ko
	@echo "Module $(BLK_MODULE) loaded"

unload:
	rmmod $(BLK_MODULE) || true
	@echo "Module $(BLK_MODULE) unloaded"

help:
	@echo "Available targets:"
	@echo "  all            - Build the kernel module (default)"
	@echo "  clean          - Clean build artifacts"
	@echo "  format         - Format source code with clang-format"
	@echo "  format-python  - Format Python source code with black"
	@echo "  check          - Test module"
	@echo "  install        - Install module to system"
	@echo "  uninstall      - Remove module from system"
	@echo "  load           - Load module"
	@echo "  unload         - Unload module"
