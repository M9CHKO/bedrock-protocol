#include "RuntimeVars.h"
#include "RakString.h"

int RakNetProtocolVersion = RAKNET_PROTOCOL_VERSION;

void SetRakNetProtocolVersion(int version) {
	RakNetProtocolVersion = version;
}