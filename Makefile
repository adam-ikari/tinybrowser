BUILD ?= build
CMAKE  ?= cmake
.PHONY: all init test clean

all:
	$(CMAKE) -S . -B $(BUILD) -DTB_BUILD_CLI=ON
	$(CMAKE) --build $(BUILD) -j

test:
	$(CMAKE) -S . -B $(BUILD)
	$(CMAKE) --build $(BUILD) -j
	ctest --test-dir $(BUILD) --output-on-failure

init:
	git submodule update --init --recursive

clean:
	rm -rf $(BUILD)
