#!/bin/bash
fd '.+\.(cc|h|hpp|cpp|c)$' uvco test benchmark -x clang-format -i
