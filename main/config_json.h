#pragma once
#include "sequence.h"
bool config_parse(const char *text, sequence_t *config, char *error, unsigned size);
char *config_encode(const sequence_t *config);
