#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
clang -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations \
  -fsanitize=address,undefined -g -DCJSON_NESTING_LIMIT=8 \
  -Imain -Imanaged_components/espressif__cjson/cJSON \
  main/sequence.c main/config_json.c managed_components/espressif__cjson/cJSON/cJSON.c \
  tests/test_sequence.c -o tests/test_sequence
./tests/test_sequence
