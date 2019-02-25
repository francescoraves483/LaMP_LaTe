#ifndef NONRAW_SOCKETMAN_H_INCLUDED
#define NONRAW_SOCKETMAN_H_INCLUDED
#include "Rawsock_lib/rawsock.h"
#include <linux/if_packet.h>

#define NO_FLAGS 0

// Socket data structure, with a union in order to manage different protocols with the same struct
struct lampsock_data {
	int descriptor;
	union {
		struct sockaddr_ll addrll;
		struct sockaddr_in addrin[2]; // One structure for bind() and one structure for sendto()
	} addru;
};

int socketCreator(protocol_t protocol);
int socketSetTimestamping(int descriptor);

#endif