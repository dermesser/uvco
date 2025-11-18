// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <uv.h>

#include "uvco/internal/internal_utils.h"
#include "uvco/name_resolution.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"

#include <memory>

namespace uvco {

/// @addtogroup StreamServer
/// @{

/// Helper template class to statically dispatch to the different
/// `uv_xyz_init()` functions. Necessary for accepting connections, when new
/// sockets are created and need to be initialized.
template <typename UvStreamType> struct UvStreamInitHelper {
  static void init(uv_loop_t * /*loop*/, UvStreamType * /*stream*/) {
    BOOST_ASSERT_MSG(false, "UvStreamInit not specialized for this type");
  }
};

/// Not for use in user code; base class for e.g. UnixServer and TcpServer.
///
/// Because accepting connections looks the same for Unix and TCP servers, the
/// behavior is defined here and shared by both. However, the implementation
/// must be generic over the stream type, so the actual stream type is a
/// template parameter.
template <typename UvStreamType, typename StreamType> class StreamServerBase {
  struct ConnectionAwaiter_;

public:
  StreamServerBase(const StreamServerBase &) = delete;
  StreamServerBase(StreamServerBase &&) = default;
  StreamServerBase &operator=(const StreamServerBase &) = delete;
  StreamServerBase &operator=(StreamServerBase &&) = default;
  ~StreamServerBase();

  /// Return client connections as clients connect.
  ///
  /// Raises exceptions if errors occur during accepting or listening.
  ///
  /// This generator may not be `co_await`ed on after having called `close()`.
  MultiPromise<StreamType> listen(int backlog = 128);

  /// Get the address the server is bound to.
  [[nodiscard]] AddressHandle getSockname() const;

  /// Close server and stop accepting client connections. As opposed to many
  /// other close() methods, it's synchronous, and therefore needs not be
  /// called, as the destructor calls it.
  ///
  /// Internally it makes use of the quasi-sync behavior of closeHandle(),
  /// making it actually more robust than a coroutine (against dropping etc.)
  void close();

protected:
  explicit StreamServerBase(std::unique_ptr<UvStreamType> socket)
      : socket_{std::move(socket)} {}
  std::unique_ptr<UvStreamType> socket_;

private:
  /// Called by libuv when a new connection arrives.
  static void onNewConnection(uv_stream_t *stream, uv_status status);
};

// Implementation contained in stream_server_base_impl.cc, where it's
// instantiated for the currently two socket types using it (UnixServer and
// TcpServer). Include stream_server_base_impl.h to obtain declarations for
// those specializations.

/// @}

} // namespace uvco
