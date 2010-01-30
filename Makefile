-include local-options.mk

ifndef APXS
  APXS := $(shell which apxs2)
endif

ifndef APXS
  APXS := $(shell which apxs)
endif

ifndef APXS
  $(error No apxs found in path, set APXS = /path/to/apxs in local-options.mk)
endif

all: local-shared-build

local-shared-build: module

module: .libs/mod_upload_progress.o

.libs/mod_upload_progress.o: mod_upload_progress.c
	$(APXS) -c $(CFLAGS) mod_upload_progress.c

#   install the shared object file into Apache 
install: local-shared-build
	$(APXS) -i $(CFLAGS) mod_upload_progress.la

#   cleanup
.PHONY: clean
clean:
	-rm -f mod_upload_progress.o mod_upload_progress.lo mod_upload_progress.slo mod_upload_progress.la
	-rm -rf .libs/
