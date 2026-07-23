# Text replacement for kimi's terminal output via LD interposition.
# Lives in ~/.kimi-code/textswap/ next to textswap.c.
#
#   make            # build textswap.so and inject it into the kimi binary
#   make install    # same, idempotent — re-run after every kimi update
#   make uninstall  # restore the original binary
#   make clean      # remove the built library

KIMI_BIN  := $(HOME)/.kimi-code/bin/kimi
KIMI_ORIG := $(KIMI_BIN).orig
LIB       := textswap.so
SRC       := textswap.c

.PHONY: all install uninstall clean

all: install

$(LIB): $(SRC)
	gcc -shared -fPIC -O2 -o $@.tmp $< -ldl
	mv $@.tmp $@

install: $(LIB)
	@if [ ! -f $(KIMI_ORIG) ]; then cp $(KIMI_BIN) $(KIMI_ORIG); fi
	@if readelf -d $(KIMI_BIN) | grep -q 'textswap\.so'; then \
		echo "already injected"; \
	else \
		cp $(KIMI_BIN) $(KIMI_BIN).textswap-tmp; \
		patchelf --add-needed $(CURDIR)/$(LIB) $(KIMI_BIN).textswap-tmp; \
		mv $(KIMI_BIN).textswap-tmp $(KIMI_BIN); \
		echo "injected $(CURDIR)/$(LIB) into $(KIMI_BIN)"; \
	fi

uninstall:
	cp $(KIMI_ORIG) $(KIMI_BIN).textswap-tmp
	mv $(KIMI_BIN).textswap-tmp $(KIMI_BIN)
	@echo "restored $(KIMI_BIN) from $(KIMI_ORIG)"

clean:
	rm -f $(LIB) $(LIB).tmp