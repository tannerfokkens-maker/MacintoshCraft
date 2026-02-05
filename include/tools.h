#ifndef H_TOOLS
#define H_TOOLS

#include <unistd.h>

#include "globals.h"

static inline int mod_abs (int a, int b) {
  return ((a % b) + b) % b;
}
static inline int div_floor (int a, int b) {
  return a % b < 0 ? (a - b) / b : a / b;
}

extern uint64_t total_bytes_received;
ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first);
ssize_t send_all (int client_fd, const void *buf, ssize_t len);
void discard_all (int client_fd, size_t remaining, uint8_t require_first);

// Packet buffering system - reduces network calls by batching writes
#define PACKET_BUFFER_SIZE 2048
extern uint8_t packet_buffer[PACKET_BUFFER_SIZE];
extern int packet_buffer_len;
extern int packet_buffer_fd;

void packet_start (int client_fd);      // Begin buffering for a client
ssize_t packet_flush (void);            // Send buffered data and end buffering
ssize_t packet_flush_continue (void);   // Send buffered data but keep buffering
void packet_end (void);                 // End buffering without sending (discard)
void packet_write (const void *buf, size_t len);  // Add data to buffer

ssize_t writeByte (int client_fd, uint8_t byte);
ssize_t writeUint16 (int client_fd, uint16_t num);
ssize_t writeUint32 (int client_fd, uint32_t num);
ssize_t writeUint64 (int client_fd, uint64_t num);
ssize_t writeFloat (int client_fd, float num);
ssize_t writeDouble (int client_fd, double num);

uint8_t readByte (int client_fd);
uint16_t readUint16 (int client_fd);
int16_t readInt16 (int client_fd);
uint32_t readUint32 (int client_fd);
uint64_t readUint64 (int client_fd);
int64_t readInt64 (int client_fd);
float readFloat (int client_fd);
double readDouble (int client_fd);

ssize_t readLengthPrefixedData (int client_fd);
void readString (int client_fd);
void readStringN (int client_fd, uint32_t max_length);

uint32_t fast_rand ();
uint64_t splitmix64 (uint64_t state);

#ifdef ESP_PLATFORM
  #include "esp_timer.h"
  #define get_program_time esp_timer_get_time
#else
  int64_t get_program_time ();
#endif

// Check if high-priority action packets are waiting in the socket buffer
// Returns 1 if action packets found, 0 otherwise
int hasActionPacketWaiting (int client_fd);

// Check if more movement packets are queued after the current one
// Returns 1 if more movement packets are waiting, 0 otherwise
int hasMoreMovementPackets (int client_fd);

#endif
