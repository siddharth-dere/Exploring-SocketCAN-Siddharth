CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDLIBS ?= -lm

TARGETS = vehicle_ecu dashboard_ecu logger_ecu canfd_test

.PHONY: all clean

all: $(TARGETS)

vehicle_ecu: vehicle_ecu.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

dashboard_ecu: dashboard_ecu.c
	$(CC) $(CFLAGS) -o $@ $<

logger_ecu: logger_ecu.c
	$(CC) $(CFLAGS) -o $@ $<

canfd_test: canfd_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS) *.o can_log.csv
