#ifndef LATENCYTEST_PACKETSTRUCTS_H_INCLUDED
#define LATENCYTEST_PACKETSTRUCTS_H_INCLUDED

#include "Rawsock_lib/rawsock.h"
#include "Rawsock_lib/rawsock_lamp.h"

#define START_ID 11349
#define INCR_ID 0

struct pktheaders_udp {
	struct ether_header etherHeader;
	struct iphdr ipHeader;
	struct udphdr udpHeader;
	struct lamphdr lampHeader;
};

struct pktheadersptr_udp {
	struct ether_header *etherHeader;
	struct iphdr *ipHeader;
	struct udphdr *udpHeader;
	struct lamphdr *lampHeader;
};

struct pktbuffers_udp {
	byte_t *ethernetpacket;
	byte_t *ippacket;
	byte_t *udppacket;
	byte_t *lamppacket;
};

#endif