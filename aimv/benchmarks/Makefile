# AIMV benchmarks Makefile
CC = clang
CFLAGS = -O2 -fsave-optimization-record=opt.yaml \
         -Rpass-missed=loop-vectorize -g

BENCHMARKS = dep_fail_alias dep_fail_stride cost_reject align_unknown multi_fail

.PHONY: all clean

all: $(BENCHMARKS:%=%.o)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o opt.yaml
