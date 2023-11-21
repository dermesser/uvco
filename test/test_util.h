
#pragma once

#include <uv.h>

#include "promise.h"

#include <functional>

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)>& setup);
