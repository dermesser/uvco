#!/bin/bash
fd '.+\.(cc|h|hpp|cpp|c)$' src test -x clang-format -i
