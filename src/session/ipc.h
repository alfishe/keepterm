// Local transports of the session protocol.
//
// POSIX: unix stream sockets in the 0700 runtime folder, socket 0600, peer uid checked on both
// ends (SO_PEERCRED / getpeereid). Windows: overlapped named pipes with a protected DACL for the
// user SID, PIPE_REJECT_REMOTE_CLIENTS, FILE_FLAG_FIRST_PIPE_INSTANCE,
// and the token SID of the peer compared on both ends.
//
// The streams are blocking with optional timeouts: one thread may read while another writes.
// shutdown() wakes both. Each client gets a reader and a writer thread
// (no single event loop per host).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "base/error.h"

namespace kt::session::ipc {

class Stream {
public:
    virtual ~Stream() = default;
    // Reads at least one byte. 0 = end of stream. timeout_ms < 0 waits forever; on timeout the
    // error is Errc::timeout.
    virtual Result<std::size_t> read(std::span<std::byte> buf, int timeout_ms = -1) = 0;
    virtual Result<void> write_all(std::span<const std::byte> bytes) = 0;
    // Ends pending and future reads and writes (any thread). Idempotent.
    virtual void shutdown() = 0;
    virtual uint32_t peer_pid() const = 0;
};

class Listener {
public:
    virtual ~Listener() = default;
    // Blocks until a client of the same user connects; Errc::lost after close().
    virtual Result<std::unique_ptr<Stream>> accept() = 0;
    virtual void close() = 0;   // any thread; also removes the socket file
};

// `endpoint` is a socket path or pipe name from os::endpoint_name(). A stale socket file (no
// listener) is replaced; a live one is Errc::busy. `max_instances` limits pipe instances on
// Windows (0 = unlimited).
Result<std::unique_ptr<Listener>> listen(const std::string& endpoint, unsigned max_instances = 0);

// Connects and checks that the server runs as the same user.
Result<std::unique_ptr<Stream>> connect(const std::string& endpoint, int timeout_ms);

}  // namespace kt::session::ipc
