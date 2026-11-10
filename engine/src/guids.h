#pragma once
#include <guiddef.h>

// COM class of the APO. register.ps1 writes the same value ($Clsid) into each endpoint's FxProperties.
// {3CDCC045-A1A2-4D8A-9369-FEDF2BD6C4C6}
static const CLSID CLSID_KaleidoscopeEqApo = {0x3cdcc045, 0xa1a2, 0x4d8a, {0x93, 0x69, 0xfe, 0xdf, 0x2b, 0xd6, 0xc4, 0xc6}};
#define KEQ_CLSID_STRING L"{3CDCC045-A1A2-4D8A-9369-FEDF2BD6C4C6}"
