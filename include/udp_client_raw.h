#ifndef LATENCYTEST_UDPCLIENTRAW_H_INCLUDED
#define LATENCYTEST_UDPCLIENTRAW_H_INCLUDED

#include "options.h"
#include "rawsock.h"
#include "rawsock_lamp.h"
#include "common_socket_man.h"

#define START_ID 11349
#define INCR_ID 0
	
unsigned int runUDPclient_raw(struct lampsock_data sData, macaddr_t srcMAC, struct in_addr srcIP, struct options *opts); 

#endif