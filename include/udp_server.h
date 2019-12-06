#ifndef LATENCYTEST_UDPSERVER_H_INCLUDED
#define LATENCYTEST_UDPSERVER_H_INCLUDED

#include "options.h"
#include "rawsock_lamp.h"
#include "common_socket_man.h"

unsigned int runUDPserver(struct lampsock_data sData, struct options *opts);

#endif