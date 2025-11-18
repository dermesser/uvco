// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#pragma once

#include <boost/assert.hpp>
#include <uv.h>

#include "uvco/internal/internal_utils.h"

namespace uvco {

/// @addtogroup Close
/// Internally used by various classes to safely close and deallocate libuv
/// handles.
/// @{

void onCloseCallback(uv_handle_t *handle);

template <typename Handle> bool isClosed(const Handle *h) {
  return 0 != uv_is_closing((uv_handle_t *)h);
}

/// closeHandle() takes care of safely closing a handle. Canonically you should
/// await the returned promise to be sure that the handle is closed. However, if
/// the promise is dropped and thus the coroutine cancelled, the libuv close
/// operation will still be carried out safely in the background.
///
/// The template types and arguments are expressed as they are to support, e.g.,
/// `uv_tcp_close_reset`.
///
/// closeHandle() has an intricate implementation which allows a safe
/// synchronous call, e.g. from destructors: In that case, the handle will still
/// be closed correctly, but the `handle` itself will also be freed. This is
/// always done when it detects that the closeHandle coroutine has been
/// cancelled (due to the returned promise having been dropped).
template <typename Handle, typename CloserArg>
void closeHandle(Handle *handle,
                 void (*closer)(CloserArg *, void (*)(uv_handle_t *))) {
  BOOST_ASSERT(handle != nullptr);
  resetData(handle);
  closer((CloserArg *)handle, onCloseCallback);
}

/// Specialization for uv_handle_t handles.
template <typename Handle> void closeHandle(Handle *handle) {
  closeHandle<Handle, uv_handle_t>(handle, uv_close);
}

/// @}

} // namespace uvco
