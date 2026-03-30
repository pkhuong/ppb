FRAMA_C_IMAGE_TAG := ppb-frama-c-32
FRAMA_C_TARGET := src/ppb.c
FRAMA_C_MAIN := ppb_entry_point

FRAMA_C_WP_MEMORY_MODEL := typed+var+int+float+ref
FRAMA_C_WP_PAR := $(shell n=$$(nproc 2>/dev/null || echo 8); echo $$(( n < 16 ? n : 16 )))
FRAMA_C_WP_TIMEOUT := 5
FRAMA_C_WP_PROVER := alt-ergo,z3,cvc4

WP_FCT :=
FRAMA_C_WP_FCT := $(if $(WP_FCT),-wp-fct=$(WP_FCT))

FRAMA_C_EVA := \
	-eva-domains=cvalue,equality,symbolic-locations,gauges,octagon,inout \
	-eva-undefined-pointer-comparison-propagate-all
FRAMA_C_WP := \
	-wp-session /workspace/frama-c/wp/session \
	-wp-rte -wp-smoke-tests \
	-wp-model="$(FRAMA_C_WP_MEMORY_MODEL)" \
	-wp-status $(FRAMA_C_WP_FCT) \
	-wp-split -wp-cache=update \
	-wp-par=$(FRAMA_C_WP_PAR) -wp-timeout=$(FRAMA_C_WP_TIMEOUT) -wp-prover="$(FRAMA_C_WP_PROVER)"

FRAMA_C_DOCKER := docker

# Docker invocation for Frama-C analysis.
# --user root:root and OPAMROOTISOK=1 are also the Dockerfile defaults,
# but let's be clear that it's expected, for rootless runners.
FRAMA_C_DOCKER_RUN := $(FRAMA_C_DOCKER) run \
	-v "$(CURDIR):/workspace:rw,Z" \
	--user root:root --env=OPAMROOTISOK=1 \
	-w /workspace --rm $(FRAMA_C_IMAGE_TAG)

FRAMA_C_COMMON := frama-c \
	-session /workspace/frama-c/frama-c \
	-lib-entry \
	-constfold \
	-cpp-extra-args="-I include -isystem stubs-for-frama-c -std=c2x" -machdep=gcc_x86_64 -std=c23 \
	-cache-size 8 -memory-footprint 8 \
	-instantiate \
	$(FRAMA_C_TARGET)

# Recursive (=) so $@ expands per-target.
FRAMA_C_REPORT = -then -report \
	-report-classify \
	-report-csv=$@.csv -report-print-properties -report-no-proven

.PHONY: clean-frama-c FRAMA_C_IMAGE wp eva

clean-frama-c:
	rm -rf frama-c/

FRAMA_C_IMAGE: FORCE
	$(FRAMA_C_DOCKER) image inspect $(FRAMA_C_IMAGE_TAG) >/dev/null 2>&1 || $(FRAMA_C_DOCKER) build --platform linux/amd64 .docker -t $(FRAMA_C_IMAGE_TAG)
	mkdir -p frama-c/wp

wp: FRAMA_C_IMAGE
	$(FRAMA_C_DOCKER_RUN) $(FRAMA_C_COMMON) -then \
	$(FRAMA_C_WP) -wp \
	$(FRAMA_C_REPORT)

eva: FRAMA_C_IMAGE
	$(FRAMA_C_DOCKER_RUN) $(FRAMA_C_COMMON) -main $(FRAMA_C_MAIN) -then \
	$(FRAMA_C_EVA) -eva \
	$(FRAMA_C_WP) -wp \
	$(FRAMA_C_REPORT)
