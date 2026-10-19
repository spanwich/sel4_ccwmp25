# Load generator for the CCMP25 resilience measurement.
loadgen: loadgen.c
	cc -O2 -Wall -Wextra -pthread -o $@ loadgen.c
clean:
	rm -f loadgen
.PHONY: clean
