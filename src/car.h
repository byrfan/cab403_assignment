#ifndef CAR_H
#define CAR_H

#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#define CLOSING     "CLOSING"
#define CLOSED      "CLOSED"
#define BETWEEN     "BETWEEN"
#define OPENING     "OPENING"
#define OPEN        "OPEN"

typedef struct
{
  pthread_mutex_t mutex;          // Locked while the contents of the structure are being accessed/modified
  pthread_cond_t cond;            // Signalled when the contents of the structure change
  char status[8];                 // NUL-terminated C string indicating the elevator's status
  char current_floor[4];          // NUL-terminated C string in the range of "B99" to "B1" and "1" to "999"
  char target_floor[4];           // Controller's immediate movement target
  char floor_request[4];          // Passenger floor selection mailbox
  uint8_t open_button;            // 1 if open doors button is pressed, 0 otherwise
  uint8_t close_button;           // 1 if close doors button is pressed, 0 otherwise
  uint8_t floor_request_pending;  // 1 while floor_request awaits transmission
  uint8_t door_obstruction;       // 1 if obstruction detected, 0 otherwise
  uint8_t overload;               // 1 if overload detected
  uint8_t emergency_stop;         // 1 if emergency stop button has been pressed, 0 otherwise
  uint8_t service_mode;           // 1 while in individual service mode
  uint8_t emergency_mode;         // 0 if not in emergency mode, 1 if in emergency mode
  uint32_t safety_heartbeat;      // Sequence incremented by the safety process
} car_shared_mem;

typedef struct
{
    char* name;
    size_t delay;
    char* low;
    char* high;
} car_metadata;

#endif
