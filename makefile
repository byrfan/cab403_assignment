CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu11 -O2 -pthread

SRC   = src
BUILD = build
BINS  = car controller call internal safety

all: $(BINS:%=$(BUILD)/%)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/car: $(SRC)/car.c $(SRC)/conn.c $(SRC)/car.h $(SRC)/conn.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC)/car.c $(SRC)/conn.c

$(BUILD)/controller: $(SRC)/controller.c $(SRC)/conn.c $(SRC)/conn.h $(SRC)/util.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC)/controller.c $(SRC)/conn.c

$(BUILD)/call: $(SRC)/call.c $(SRC)/conn.c $(SRC)/conn.h $(SRC)/util.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC)/call.c $(SRC)/conn.c

$(BUILD)/internal: $(SRC)/internal.c $(SRC)/car.h $(SRC)/util.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC)/internal.c

$(BUILD)/safety: $(SRC)/safety.c $(SRC)/car.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRC)/safety.c

clean:
	rm -rf $(BUILD)

.PHONY: all clean
