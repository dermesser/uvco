// uvco (c) 2024 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <fmt/core.h>
#include <uv.h>

#include "uvco/internal/internal_utils.h"
#include "uvco/promise/multipromise.h"
#include "uvco/promise/promise.h"

#include <coroutine>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

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

  /// Close server and stop accepting client connections; must be awaited.
  Promise<void> close();

protected:
  explicit StreamServerBase(std::unique_ptr<UvStreamType> socket)
      : socket_{std::move(socket)} {}
  std::unique_ptr<UvStreamType> socket_;

private:
  static void onNewConnection(uv_stream_t *stream, uv_status status);

  struct ConnectionAwaiter_ {
    explicit ConnectionAwaiter_(UvStreamType &socket) : socket_{socket} {
      accepted_.reserve(4);
    }
    [[nodiscard]] bool await_ready() const;
    bool await_suspend(std::coroutine_handle<> handle);
    // Returns true if one or more connections were accepted.
    // Returns false if the listener should stop.
    [[nodiscard]] bool await_resume() const { return !stopped_; }

    /// Stop a listener coroutine.
    void stop();

    UvStreamType &socket_;
    std::optional<std::coroutine_handle<>> handle_;

    // Set of accepted connections or errors.
    using Accepted = std::variant<uv_status, StreamType>;
    std::vector<Accepted> accepted_;

    bool stopped_ = false;
  };
};

// Implementation contained in stream_server_base_impl.cc, where it's
// instantiated for the currently two socket types using it (UnixServer and
// TcpServer). Include stream_server_base_impl.h to obtain declarations for
// those specializations.

/// @}

} // namespace uvco
