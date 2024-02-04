
#pragma once

#include <uv.h>

#include "promise/promise.h"
#include "run.h"

#include <functional>

void run_loop(const std::function<uvco::Promise<void>(uv_loop_t *)> &setup);
void run_loop(
    const std::function<uvco::Promise<void>(const uvco::Loop &)> &setup);
