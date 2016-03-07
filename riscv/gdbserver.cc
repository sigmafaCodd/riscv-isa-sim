#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

#include "gdbserver.h"

template <typename T>
unsigned int circular_buffer_t<T>::size() const
{
  if (end >= start)
    return end - start;
  else
    return end + capacity - start;
}

template <typename T>
void circular_buffer_t<T>::consume(unsigned int bytes)
{
  start = (start + bytes) % capacity;
}

template <typename T>
unsigned int circular_buffer_t<T>::contiguous_empty_size() const
{
  if (end >= start)
    if (start == 0)
      return capacity - end - 1;
    else
      return capacity - end;
  else
    return start - end - 1;
}

template <typename T>
unsigned int circular_buffer_t<T>::contiguous_data_size() const
{
  if (end >= start)
    return end - start;
  else
    return capacity - start;
}

template <typename T>
void circular_buffer_t<T>::data_added(unsigned int bytes)
{
  end += bytes;
  assert(end <= capacity);
  if (end == capacity)
    end = 0;
}

template <typename T>
void circular_buffer_t<T>::reset()
{
  start = 0;
  end = 0;
}

template <typename T>
void circular_buffer_t<T>::append(const T *src, unsigned int count)
{
  unsigned int copy = std::min(count, contiguous_empty_size());
  memcpy(contiguous_empty(), src, copy * sizeof(T));
  data_added(copy);
  count -= copy;
  if (count > 0) {
    assert(count < contiguous_empty_size());
    memcpy(contiguous_empty(), src, count * sizeof(T));
    data_added(count);
  }
}

// Code inspired by/copied from OpenOCD server/server.c.

gdbserver_t::gdbserver_t(uint16_t port) :
  client_fd(0),
  recv_buf(64 * 1024), send_buf(64 * 1024)
{
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == -1) {
    fprintf(stderr, "error creating socket: %s\n", strerror(errno));
    abort();
  }

  int so_reuseaddr_option = 1;
  setsockopt(socket_fd,
      SOL_SOCKET,
      SO_REUSEADDR,
      (void *)&so_reuseaddr_option,
      sizeof(int));

  int oldopts = fcntl(socket_fd, F_GETFL, 0);
  fcntl(socket_fd, F_SETFL, oldopts | O_NONBLOCK);

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  sin.sin_port = htons(port);

  if (bind(socket_fd, (struct sockaddr *)&sin, sizeof(sin)) == -1) {
    fprintf(stderr, "couldn't bind to socket: %s\n", strerror(errno));
    abort();
  }

  /* These setsockopt()s must happen before the listen() */
  int window_size = 128 * 1024;
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF,
      (char *)&window_size, sizeof(window_size));
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF,
      (char *)&window_size, sizeof(window_size));

  if (listen(socket_fd, 1) == -1) {
    fprintf(stderr, "couldn't listen on socket: %s\n", strerror(errno));
    abort();
  }
}

void gdbserver_t::accept()
{
  struct sockaddr client_addr;
  socklen_t address_size = sizeof(client_addr);
  client_fd = ::accept(socket_fd, &client_addr, &address_size);
  if (client_fd == -1) {
    if (errno == EAGAIN) {
      // We'll try again in the next call.
    } else {
      fprintf(stderr, "failed to accept on socket: %s (%d)\n", strerror(errno), errno);
      abort();
    }
  } else {
    int oldopts = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, oldopts | O_NONBLOCK);
    expect_ack = false;
  }
}

void gdbserver_t::read()
{
  // Reading from a non-blocking socket still blocks if there is no data
  // available.

  size_t count = recv_buf.contiguous_empty_size();
  assert(count > 0);
  ssize_t bytes = ::read(client_fd, recv_buf.contiguous_empty(), count);
  if (bytes == -1) {
    if (errno == EAGAIN) {
      // We'll try again the next call.
    } else {
      fprintf(stderr, "failed to read on socket: %s (%d)\n", strerror(errno), errno);
      abort();
    }
  } else if (bytes == 0) {
    // The remote disconnected.
    client_fd = 0;
    recv_buf.reset();
    send_buf.reset();
  } else {
    printf("read %ld bytes\n", bytes);
    recv_buf.data_added(bytes);
  }
}

void gdbserver_t::write()
{
  if (send_buf.empty())
    return;

  while (!send_buf.empty()) {
    unsigned int count = send_buf.contiguous_data_size();
    assert(count > 0);
    ssize_t bytes = ::write(client_fd, send_buf.contiguous_data(), count);
    if (bytes == -1) {
      fprintf(stderr, "failed to write to socket: %s (%d)\n", strerror(errno), errno);
      abort();
    } else if (bytes == 0) {
      // Client can't take any more data right now.
      break;
    } else {
      printf("wrote %ld bytes:\n", bytes);
      for (unsigned int i = 0; i < bytes; i++) {
        printf("%c", send_buf[i]);
      }
      printf("\n");
      send_buf.consume(bytes);
    }
  }
}

void print_packet(const std::vector<uint8_t> &packet)
{
  for (uint8_t c : packet) {
    fprintf(stderr, "%c", c);
  }
  fprintf(stderr, "\n");
}

uint8_t compute_checksum(const std::vector<uint8_t> &packet)
{
  uint8_t checksum = 0;
  for (auto i = packet.begin() + 1; i != packet.end() - 3; i++ ) {
    checksum += *i;
  }
  return checksum;
}

uint8_t character_hex_value(uint8_t character)
{
  if (character >= '0' && character <= '9')
    return character - '0';
  if (character >= 'a' && character <= 'f')
    return 10 + character - 'a';
  if (character >= 'A' && character <= 'F')
    return 10 + character - 'A';
  return 0;
}

uint8_t extract_checksum(const std::vector<uint8_t> &packet)
{
  return character_hex_value(*(packet.end() - 1)) +
    16 * character_hex_value(*(packet.end() - 2));
}

void gdbserver_t::process_requests()
{
  // See https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html

  while (!recv_buf.empty()) {
    std::vector<uint8_t> packet;
    for (unsigned int i = 0; i < recv_buf.size(); i++) {
      uint8_t b = recv_buf[i];

      if (packet.empty() && expect_ack && b == '+') {
        fprintf(stderr, "Received ack\n");
        recv_buf.consume(1);
        break;
      }

      if (b == '$') {
        // Start of new packet.
        if (!packet.empty()) {
          fprintf(stderr, "Received malformed %ld-byte packet from debug client\n", packet.size());
          print_packet(packet);
          recv_buf.consume(i);
          break;
        }
      }

      packet.push_back(b);

      // Packets consist of $<packet-data>#<checksum>
      // where <checksum> is 
      if (packet.size() >= 4 &&
          packet[packet.size()-3] == '#') {
        handle_packet(packet);
        recv_buf.consume(i+1);
        break;
      }
    }
    // There's a partial packet in the buffer. Wait until we get more data to
    // process it.
    if (packet.size())
      break;
  }
}

void gdbserver_t::handle_set_threadid(const std::vector<uint8_t> &packet)
{
  if (packet[2] == 'g' && packet[3] == '0') {
    // Use thread 0 for all operations.
    send("OK");
  } else {
    send("$#00");
  }
}

void gdbserver_t::handle_halt_reason(const std::vector<uint8_t> &packet)
{
  send_packet("S00");
}

void gdbserver_t::handle_packet(const std::vector<uint8_t> &packet)
{
  if (compute_checksum(packet) != extract_checksum(packet)) {
    fprintf(stderr, "Received %ld-byte packet with invalid checksum\n", packet.size());
    fprintf(stderr, "Computed checksum: %x\n", compute_checksum(packet));
    print_packet(packet);
    send("-");
    return;
  }

  fprintf(stderr, "Received %ld-byte packet from debug client\n", packet.size());
  print_packet(packet);
  send("+");

  switch (packet[1]) {
    case 'H':
      return handle_set_threadid(packet);
    case '?':
      return handle_halt_reason(packet);
  }

  // Not supported.
  send_packet("");
}

void gdbserver_t::handle()
{
  if (client_fd > 0) {
    this->read();
    this->write();

  } else {
    this->accept();
  }

  this->process_requests();
}

void gdbserver_t::send(const char* msg)
{
  unsigned int length = strlen(msg);
  send_buf.append((const uint8_t *) msg, length);
}

void gdbserver_t::send_packet(const char* data)
{
  send("$");
  send(data);
  send("#");

  uint8_t checksum = 0;
  for ( ; *data; data++) {
    checksum += *data;
  }
  char checksum_string[3];
  sprintf(checksum_string, "%02x", checksum);
  send(checksum_string);
  expect_ack = true;
}