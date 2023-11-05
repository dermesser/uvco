#!/bin/bash
fd '.+\.(cc|h|hpp|cpp|c)$' src -x clang-format -i
