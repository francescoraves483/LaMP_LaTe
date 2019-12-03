#ifndef NONRAW_SOCKETMAN_H_INCLUDED
#define NONRAW_SOCKETMAN_H_INCLUDED
#include "Rawsock_lib/rawsock.h"
#include <linux/if_packet.h>
#include <net/if.h>

#define NO_FLAGS 0

#define SET_TIMESTAMPING_SW_RX 0x00
#define SET_TIMESTAMPING_SW_RXTX 0x01
#define SET_TIMESTAMPING_HW 0x02

// socketSetTimestamping() errors
#define SOCKETSETTS_ESETSOCKOPT -1 // setsockopt() error
#define SOCKETSETTS_EINVAL -2 // Invalid 'mode' argument
#define SOCKETSETTS_ENOHWSTAMPS -3 // No support for hardware timestamps (when SET_TIMESTAMPING_HW is requested)
#define SOCKETSETTS_ENOSUPP -4 // No device support for the requested timestamps
#define SOCKETSETTS_EETHTOOL -5 // Cannot check device timestamping capabilities

// Socket data structure, with a union in order to manage different protocols with the same struct
struct lampsock_data {
	int descriptor;
	union {
		struct sockaddr_ll addrll;
		struct sockaddr_in addrin[2]; // One structure for bind() and one structure for sendto()
	} addru;
	char devname[IFNAMSIZ];
};

int socketCreator(protocol_t protocol);
int socketSetTimestamping(struct lampsock_data sData, int mode);
int pollErrqueueWait(int sFd,uint64_t timeout_ms);

#endif