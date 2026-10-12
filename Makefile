# Optional POSIX front door. Visual Studio and Windows batch builds need no Make.
PYTHON ?= python3
CONFIGURATION ?= Debug
TARGET ?= gc

.PHONY: help prepare check prepare-reference build debug devbuild release docs docs-locale clean setup-vscode
help:
	@echo "Git tasks: prepare check prepare-reference"
	@echo "Other tasks: build debug devbuild release docs docs-locale clean setup-vscode"
	@echo "Build TARGET=gc (default), upstream or reference; override PYTHON/CONFIGURATION."
	@echo "DLL builds require Windows/MSVC."

build:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration "$(CONFIGURATION)"

debug:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration Debug

devbuild:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration DevBuild

release:
	"$(PYTHON)" scripts/dev.py build --target "$(TARGET)" --configuration Release

prepare check prepare-reference docs docs-locale clean setup-vscode:
	"$(PYTHON)" scripts/dev.py $@
