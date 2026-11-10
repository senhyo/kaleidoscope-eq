// Forced-include compat for building engine/src/dsp.cpp on macOS (clang, arm64).
#pragma once
#ifdef __cplusplus
#include <clocale>
#include <cstdlib>
#endif
#include <xlocale.h>
typedef locale_t _locale_t;
#define _strtod_l strtod_l
#define _create_locale(cat, name) newlocale(LC_NUMERIC_MASK, name, (locale_t)0)
#define _free_locale freelocale
