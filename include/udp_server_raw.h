#ifndef LATENCYTEST_UDPSERVERRAW_H_INCLUDED
#define LATENCYTEST_UDPSERVERRAW_H_INCLUDED

#include "options.h"
#include "rawsock.h"
#include "rawsock_lamp.h"
#include "common_socket_man.h"

unsigned int runUDPserver_raw(struct lampsock_data sData, macaddr_t srcMAC, struct in_addr srcIP, struct options *opts);

#endif