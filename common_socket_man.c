#include "common_socket_man.h"
#include <linux/net_tstamp.h> // Required for the SO_TIMESTAMPING flags

int socketCreator(protocol_t protocol) {
	int sFd;

	switch(protocol) {
		case UDP:
			sFd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
		break;
		default:
			sFd=1;
		break;
	}

	return sFd;
}

int socketSetTimestamping(int descriptor) {
	int flags=1;

	return setsockopt(descriptor,SOL_SOCKET,SO_TIMESTAMP,&flags,sizeof(flags));
}