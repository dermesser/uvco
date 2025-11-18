// uvco (c) 2023 Lewin Bormann. See LICENSE for specific terms.

#include <uv.h>

#include "uvco/close.h"
#include "uvco/internal/internal_utils.h"

namespace uvco {

void onCloseCallback(uv_handle_t *handle) {
  BOOST_ASSERT(dataIsNull(handle));
  UvHandleDeleter::del(handle);
}

} // namespace uvco
