# Optional POSIX front door. Visual Studio and Windows batch builds need no Make.
PYTHON ?= python3
CONFIGURATION ?= Debug
TARGET ?= reference

.PHONY: help prepare-deps check-deps export-gc assemble-gc verify-gc build debug devbuild release docs docs-locale clean setup-vscode
help:
	@echo "Dependency tasks: prepare-deps check-deps export-gc assemble-gc verify-gc"
	@echo "Other tasks: build debug devbuild release docs docs-locale clean setup-vscode"
	@echo "Build TARGET=reference (default), upstream or legacy; override PYTHON/CONFIGURATION."
	@echo "DLL builds require Windows/MSVC."

build:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration "$(CONFIGURATION)"

debug:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration Debug

devbuild:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration DevBuild

release:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration Release

prepare-deps check-deps export-gc assemble-gc verify-gc docs docs-locale clean setup-vscode:
	"$(PYTHON)" scripts/dev.py $@
