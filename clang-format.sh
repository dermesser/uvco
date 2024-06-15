#!/bin/bash
fd '.+\.(cc|h|hpp|cpp|c)$' uvco test -x clang-format -i
