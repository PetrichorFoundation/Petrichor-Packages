# Builds every package in packages/ into this directory (name-version.ppk + index).
# Needs a checkout of the OS next to this one (or OS=/path/to/PetrichorOS): it supplies the compiler flags,
# the user-space library and the mkppk tool.
#
#   make          rebuild index and all .ppk files
#   make serve    serve this directory on http://127.0.0.1:8080 (QEMU guests reach it as 10.0.2.2:8080)

OS   ?= ../PetrichorOS
PKGS := $(notdir $(wildcard packages/*))

.PHONY: all serve
all:
	@test -f "$(OS)/Makefile" || { echo "Petrichor OS checkout not found at $(OS); pass OS=/path/to/PetrichorOS"; exit 1; }
	rm -f index
	@for p in $(PKGS); do \
	  $(MAKE) --no-print-directory -C "$(OS)" package PKGDIR="$(CURDIR)/packages/$$p" OUT="$(CURDIR)" || exit 1; \
	done
	@echo "index: $$(wc -l < index | tr -d ' ') packages"

serve:
	python3 -m http.server 8080 --bind 127.0.0.1
