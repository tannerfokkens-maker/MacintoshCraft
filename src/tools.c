#include <stdio.h>
#include <string.h>

#ifdef MAC68K_PLATFORM
  #include "mac68k_net.h"
  extern int errno;
#else
  #include <errno.h>
#endif

#ifdef ESP_PLATFORM
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "esp_timer.h"
#elif defined(MAC68K_PLATFORM)
  /* Using mac68k_net.h stubs */
#else
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
  #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
  #endif
#endif

#include "globals.h"
#include "varnum.h"
#include "procedures.h"
#include "tools.h"
#include "profiler.h"

#ifndef htonll
  static uint64_t htonll (uint64_t value) {
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF)) << 32);
  #else
    return value;
  #endif
  }
#endif

// Keep track of the total amount of bytes received with recv_all
// Helps notice misread packets and clean up after errors
uint64_t total_bytes_received = 0;

// Packet buffering system - reduces network calls by batching writes
uint8_t packet_buffer[PACKET_BUFFER_SIZE];
int packet_buffer_len = 0;
int packet_buffer_fd = -1;  // -1 means not buffering

void packet_start (int client_fd) {
  packet_buffer_fd = client_fd;
  packet_buffer_len = 0;
}

void packet_write (const void *buf, size_t len) {
  if (packet_buffer_len + len > PACKET_BUFFER_SIZE) {
    // Buffer overflow - flush and continue
    packet_flush_continue();
  }
  memcpy(packet_buffer + packet_buffer_len, buf, len);
  packet_buffer_len += len;
}

ssize_t packet_flush_continue (void) {
  if (packet_buffer_fd == -1 || packet_buffer_len == 0) {
    return 0;
  }
  int fd = packet_buffer_fd;
  ssize_t result = send_all(fd, packet_buffer, packet_buffer_len);
  packet_buffer_len = 0;
  // Keep fd set for more packets
  packet_buffer_fd = fd;
  return result;
}

ssize_t packet_flush (void) {
  ssize_t result = packet_flush_continue();
  packet_buffer_fd = -1;
  return result;
}

void packet_end (void) {
  packet_buffer_len = 0;
  packet_buffer_fd = -1;
}

ssize_t recv_all (int client_fd, void *buf, size_t n, uint8_t require_first) {
  char *p = buf;
  size_t total = 0;

  // Track time of last meaningful network update
  // Used to handle timeout when client is stalling
  int64_t last_update_time = get_program_time();

  // If requested, exit early when first byte not immediately available
  if (require_first) {
    ssize_t r = recv(client_fd, p, 1, MSG_PEEK);
    if (r <= 0) {
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 0; // no first byte available yet
      }
      return -1; // error or connection closed
    }
  }

  // Busy-wait (with task yielding) until we get exactly n bytes
  while (total < n) {
    ssize_t r = recv(client_fd, p + total, n - total, 0);
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // handle network timeout
        if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
          disconnectClient(&client_fd, -1);
          return -1;
        }
        task_yield();
        continue;
      } else {
        total_bytes_received += total;
        return -1; // real error
      }
    } else if (r == 0) {
      // connection closed before full read
      total_bytes_received += total;
      return total;
    }
    total += r;
    last_update_time = get_program_time();
  }

  total_bytes_received += total;
  return total; // got exactly n bytes
}

// Drain stale movement packets from receive buffer during send waits.
// Prevents receive queue buildup while blocked on slow sends.
static void drain_stale_movement_packets(int client_fd) {
  uint8_t peek_buf[32];

  // Keep draining while there are movement packets to skip
  while (1) {
    #ifdef _WIN32
      ssize_t peeked = recv(client_fd, (char *)peek_buf, sizeof(peek_buf), MSG_PEEK);
    #else
      ssize_t peeked = recv(client_fd, peek_buf, sizeof(peek_buf), MSG_PEEK);
    #endif

    if (peeked <= 2) return;

    // Parse varint length
    int pos = 0;
    int length = 0;
    int shift = 0;

    while (pos < peeked && pos < 4) {
      uint8_t byte = peek_buf[pos++];
      length |= (byte & 0x7F) << shift;
      if (!(byte & 0x80)) break;
      shift += 7;
    }

    if (pos >= peeked) return;

    // Parse packet ID
    int packet_id = 0;
    shift = 0;
    int id_start = pos;

    while (pos < peeked) {
      uint8_t byte = peek_buf[pos++];
      packet_id |= (byte & 0x7F) << shift;
      if (!(byte & 0x80)) break;
      shift += 7;
    }

    // Only drain if this is a movement packet (0x1D-0x20) AND more packets follow
    if (packet_id < 0x1D || packet_id > 0x20) return;

    // Check if there's another packet after this one
    int total_packet_len = id_start + length;

    // Peek further to see if there's more data after this packet
    #ifdef _WIN32
      ssize_t more = recv(client_fd, (char *)peek_buf, total_packet_len + 1, MSG_PEEK);
    #else
      ssize_t more = recv(client_fd, peek_buf,
                          total_packet_len + 1 > (int)sizeof(peek_buf) ? sizeof(peek_buf) : total_packet_len + 1,
                          MSG_PEEK);
    #endif

    // Only discard if more data is waiting (don't drop the last position update)
    if (more <= total_packet_len) return;

    // Consume and discard this stale movement packet
    #ifdef _WIN32
      recv(client_fd, (char *)peek_buf, total_packet_len > (int)sizeof(peek_buf) ? sizeof(peek_buf) : total_packet_len, 0);
      if (total_packet_len > (int)sizeof(peek_buf)) {
        // Drain rest in chunks
        int remaining = total_packet_len - sizeof(peek_buf);
        while (remaining > 0) {
          int chunk = remaining > (int)sizeof(peek_buf) ? sizeof(peek_buf) : remaining;
          recv(client_fd, (char *)peek_buf, chunk, 0);
          remaining -= chunk;
        }
      }
    #else
      // Drain the full packet
      int remaining = total_packet_len;
      while (remaining > 0) {
        int chunk = remaining > (int)sizeof(peek_buf) ? sizeof(peek_buf) : remaining;
        ssize_t got = recv(client_fd, peek_buf, chunk, 0);
        if (got <= 0) return;
        remaining -= got;
      }
    #endif
  }
}

ssize_t send_all (int client_fd, const void *buf, ssize_t len) {
  PROF_START(NET_SEND);
  // Treat any input buffer as *uint8_t for simplicity
  const uint8_t *p = (const uint8_t *)buf;
  ssize_t sent = 0;

  // Track time of last meaningful network update
  // Used to handle timeout when client is stalling
  int64_t last_update_time = get_program_time();

  // Busy-wait (with task yielding) until all data has been sent
  while (sent < len) {
    #ifdef _WIN32
      ssize_t n = send(client_fd, p + sent, len - sent, 0);
    #else
      ssize_t n = send(client_fd, p + sent, len - sent, MSG_NOSIGNAL);
    #endif
    if (n > 0) { // some data was sent, log it
      sent += n;
      last_update_time = get_program_time();
      continue;
    }
    if (n == 0) { // connection was closed, treat this as an error
      errno = ECONNRESET;
      PROF_END(NET_SEND);
      return -1;
    }
    // not yet ready to transmit, try again
    #ifdef _WIN32 //handles windows socket timeout
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
    #else
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
    #endif
      PROF_BLOCKED(NET_SEND);  // Track blocking waits
      // handle network timeout
      if (get_program_time() - last_update_time > NETWORK_TIMEOUT_TIME) {
        disconnectClient(&client_fd, -2);
        PROF_END(NET_SEND);
        return -1;
      }
      // While waiting for send buffer to drain, prevent receive queue buildup
      drain_stale_movement_packets(client_fd);
      task_yield();
      continue;
    }
    PROF_END(NET_SEND);
    return -1; // real error
  }

  PROF_END(NET_SEND);
  return sent;
}

void discard_all (int client_fd, size_t remaining, uint8_t require_first) {
  while (remaining > 0) {
    size_t recv_n = remaining > MAX_RECV_BUF_LEN ? MAX_RECV_BUF_LEN : remaining;
    ssize_t received = recv_all(client_fd, recv_buffer, recv_n, require_first);
    if (received < 0) return;
    if (received > remaining) return;
    remaining -= received;
    require_first = false;
  }
}

ssize_t writeByte (int client_fd, uint8_t byte) {
  if (packet_buffer_fd == client_fd) {
    packet_write(&byte, 1);
    return 1;
  }
  return send_all(client_fd, &byte, 1);
}
ssize_t writeUint16 (int client_fd, uint16_t num) {
  uint16_t be = htons(num);
  if (packet_buffer_fd == client_fd) {
    packet_write(&be, sizeof(be));
    return sizeof(be);
  }
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint32 (int client_fd, uint32_t num) {
  uint32_t be = htonl(num);
  if (packet_buffer_fd == client_fd) {
    packet_write(&be, sizeof(be));
    return sizeof(be);
  }
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeUint64 (int client_fd, uint64_t num) {
  uint64_t be = htonll(num);
  if (packet_buffer_fd == client_fd) {
    packet_write(&be, sizeof(be));
    return sizeof(be);
  }
  return send_all(client_fd, &be, sizeof(be));
}
ssize_t writeFloat (int client_fd, float num) {
  uint32_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonl(bits);
  if (packet_buffer_fd == client_fd) {
    packet_write(&bits, sizeof(bits));
    return sizeof(bits);
  }
  return send_all(client_fd, &bits, sizeof(bits));
}
ssize_t writeDouble (int client_fd, double num) {
  uint64_t bits;
  memcpy(&bits, &num, sizeof(bits));
  bits = htonll(bits);
  if (packet_buffer_fd == client_fd) {
    packet_write(&bits, sizeof(bits));
    return sizeof(bits);
  }
  return send_all(client_fd, &bits, sizeof(bits));
}

uint8_t readByte (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 1, false);
  return recv_buffer[0];
}
uint16_t readUint16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((uint16_t)recv_buffer[0] << 8) | recv_buffer[1];
}
int16_t readInt16 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 2, false);
  return ((int16_t)recv_buffer[0] << 8) | (int16_t)recv_buffer[1];
}
uint32_t readUint32 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 4, false);
  return ((uint32_t)recv_buffer[0] << 24) |
         ((uint32_t)recv_buffer[1] << 16) |
         ((uint32_t)recv_buffer[2] << 8) |
         ((uint32_t)recv_buffer[3]);
}
uint64_t readUint64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((uint64_t)recv_buffer[0] << 56) |
         ((uint64_t)recv_buffer[1] << 48) |
         ((uint64_t)recv_buffer[2] << 40) |
         ((uint64_t)recv_buffer[3] << 32) |
         ((uint64_t)recv_buffer[4] << 24) |
         ((uint64_t)recv_buffer[5] << 16) |
         ((uint64_t)recv_buffer[6] << 8) |
         ((uint64_t)recv_buffer[7]);
}
int64_t readInt64 (int client_fd) {
  recv_count = recv_all(client_fd, recv_buffer, 8, false);
  return ((int64_t)recv_buffer[0] << 56) |
         ((int64_t)recv_buffer[1] << 48) |
         ((int64_t)recv_buffer[2] << 40) |
         ((int64_t)recv_buffer[3] << 32) |
         ((int64_t)recv_buffer[4] << 24) |
         ((int64_t)recv_buffer[5] << 16) |
         ((int64_t)recv_buffer[6] << 8) |
         ((int64_t)recv_buffer[7]);
}
float readFloat (int client_fd) {
  uint32_t bytes = readUint32(client_fd);
  float output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}
double readDouble (int client_fd) {
  uint64_t bytes = readUint64(client_fd);
  double output;
  memcpy(&output, &bytes, sizeof(output));
  return output;
}

// Receive length prefixed data with bounds checking
ssize_t readLengthPrefixedData (int client_fd) {
  uint32_t length = readVarInt(client_fd);
  if (length >= MAX_RECV_BUF_LEN) {
    printf("ERROR: Received length (%lu) exceeds maximum (%u)\n", length, MAX_RECV_BUF_LEN);
    disconnectClient(&client_fd, -1);
    recv_count = 0;
    return 0;
  }
  return recv_all(client_fd, recv_buffer, length, false);
}

// Reads a networked string into recv_buffer
void readString (int client_fd) {
  recv_count = readLengthPrefixedData(client_fd);
  recv_buffer[recv_count] = '\0';
}
// Reads a networked string of up to N bytes into recv_buffer
void readStringN (int client_fd, uint32_t max_length) {
  // Forward to readString if max length is invalid
  if (max_length >= MAX_RECV_BUF_LEN) {
    readString(client_fd);
    return;
  }
  // Attempt to read full string within maximum
  uint32_t length = readVarInt(client_fd);
  if (max_length > length) {
    recv_count = recv_all(client_fd, recv_buffer, length, false);
    recv_buffer[recv_count] = '\0';
    return;
  }
  // Read string up to maximum, dump the rest
  recv_count = recv_all(client_fd, recv_buffer, max_length, false);
  recv_buffer[recv_count] = '\0';
  uint8_t dummy;
  for (uint32_t i = max_length; i < length; i ++) {
    recv_all(client_fd, &dummy, 1, false);
  }
}

uint32_t fast_rand () {
  rng_seed ^= rng_seed << 13;
  rng_seed ^= rng_seed >> 17;
  rng_seed ^= rng_seed << 5;
  return rng_seed;
}

uint64_t splitmix64 (uint64_t state) {
  uint64_t z = state + 0x9e3779b97f4a7c15;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
  z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
  return z ^ (z >> 31);
}

#ifndef ESP_PLATFORM
// Returns system time in microseconds.
// On ESP-IDF, this is available in "esp_timer.h", and returns time *since
// the start of the program*, and NOT wall clock time. To ensure
// compatibility, this should only be used to measure time intervals.
int64_t get_program_time () {
  #ifdef MAC68K_PLATFORM
  // Classic Mac OS: TickCount() returns 1/60th second ticks since boot
  // Convert to microseconds: ticks * (1000000 / 60) = ticks * 16667
  extern unsigned long TickCount(void);
  return (int64_t)TickCount() * 16667LL;
  #else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
  #endif
}
#endif

// Check if more movement packets are queued after the current one.
// Used to skip stale position packets and prevent queue buildup.
// Returns 1 if more movement packets are waiting, 0 otherwise.
int hasMoreMovementPackets (int client_fd) {
  // Peek a small amount - we just need to see the next packet
  uint8_t peek_buf[16];

  #ifdef _WIN32
    ssize_t peeked = recv(client_fd, (char *)peek_buf, sizeof(peek_buf), MSG_PEEK);
  #else
    ssize_t peeked = recv(client_fd, peek_buf, sizeof(peek_buf), MSG_PEEK);
  #endif

  if (peeked <= 2) return 0;

  // Parse varint length
  int pos = 0;
  int length = 0;
  int shift = 0;

  while (pos < peeked) {
    uint8_t byte = peek_buf[pos++];
    length |= (byte & 0x7F) << shift;
    if (!(byte & 0x80)) break;
    shift += 7;
    if (pos > 3) return 0; // Invalid varint
  }

  if (pos >= peeked) return 0;

  // Parse packet ID (next varint)
  int packet_id = 0;
  shift = 0;

  while (pos < peeked) {
    uint8_t byte = peek_buf[pos++];
    packet_id |= (byte & 0x7F) << shift;
    if (!(byte & 0x80)) break;
    shift += 7;
  }

  // Movement packets are 0x1D, 0x1E, 0x1F, 0x20
  return (packet_id >= 0x1D && packet_id <= 0x20);
}

// Check if high-priority action packets are waiting in the socket buffer.
// Peeks ahead in the receive buffer to find packet IDs for mining/placing.
// Returns 1 if action packets found, 0 otherwise.
int hasActionPacketWaiting (int client_fd) {
  // Peek up to 64 bytes to scan for action packet IDs
  // Format: [varint length][varint packet_id][data...]
  uint8_t peek_buf[64];

  #ifdef _WIN32
    ssize_t peeked = recv(client_fd, (char *)peek_buf, sizeof(peek_buf), MSG_PEEK);
  #else
    ssize_t peeked = recv(client_fd, peek_buf, sizeof(peek_buf), MSG_PEEK);
  #endif

  if (peeked <= 0) return 0;

  // Scan through peeked data looking for packet boundaries
  int pos = 0;
  while (pos < peeked - 1) {
    // Try to read varint length
    int length = 0;
    int shift = 0;
    int len_bytes = 0;

    while (pos + len_bytes < peeked) {
      uint8_t byte = peek_buf[pos + len_bytes];
      length |= (byte & 0x7F) << shift;
      len_bytes++;
      if (!(byte & 0x80)) break;
      shift += 7;
      if (len_bytes > 3) break; // Invalid varint
    }

    if (len_bytes > 3 || pos + len_bytes >= peeked) break;

    // Read packet ID (next varint after length)
    int id_pos = pos + len_bytes;
    int packet_id = 0;
    shift = 0;

    while (id_pos < peeked) {
      uint8_t byte = peek_buf[id_pos];
      packet_id |= (byte & 0x7F) << shift;
      id_pos++;
      if (!(byte & 0x80)) break;
      shift += 7;
    }

    // Check for action packet IDs: 0x28 (mining), 0x3F (place), 0x40 (use item)
    if (packet_id == 0x28 || packet_id == 0x3F || packet_id == 0x40) {
      return 1;
    }

    // Move to next packet (skip length bytes + packet data)
    pos += len_bytes + length;
  }

  return 0;
}
