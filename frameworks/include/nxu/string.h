#ifndef NXU_USER_STRING_H
#define NXU_USER_STRING_H

#include <stdbool.h>
#include <stdint.h>

uint64_t nxu_strlen(const char *string);
bool nxu_streq(const char *left, const char *right);
bool nxu_arg_present(const char *arguments, const char *argument);

#endif
