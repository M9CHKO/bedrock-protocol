#pragma once

#include <string>
#include "RakNetVersion.h"

// Patch for MCPE - Updates to this number are somtimes made when MCPE protocol has breaking changes
extern int RakNetProtocolVersion;

void SetRakNetProtocolVersion(int version);