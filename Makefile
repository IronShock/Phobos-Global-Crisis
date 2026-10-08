# Optional POSIX front door. Visual Studio and Windows batch builds need no Make.
PYTHON ?= python3
CONFIGURATION ?= Debug

.PHONY: help build debug devbuild release docs docs-locale clean setup-vscode
help:
	@echo "Targets: build debug devbuild release docs docs-locale clean setup-vscode"
	@echo "Override PYTHON or CONFIGURATION as needed; DLL builds require Windows/MSVC."

build:
	"$(PYTHON)" scripts/dev.py build --configuration "$(CONFIGURATION)"

debug:
	"$(PYTHON)" scripts/dev.py build --configuration Debug

devbuild:
	"$(PYTHON)" scripts/dev.py build --configuration DevBuild

release:
	"$(PYTHON)" scripts/dev.py build --configuration Release

docs docs-locale clean setup-vscode:
	"$(PYTHON)" scripts/dev.py $@
