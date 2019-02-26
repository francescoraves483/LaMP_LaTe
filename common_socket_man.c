#include "common_socket_man.h"

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