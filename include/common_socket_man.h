#ifndef NONRAW_SOCKETMAN_H_INCLUDED
#define NONRAW_SOCKETMAN_H_INCLUDED
#include "rawsock.h"
#include "report_manager.h"
#include "options.h"
#include <linux/if_packet.h>
#include <net/if.h>

#if AMQP_1_0_ENABLED
#include <proton/proactor.h>
#include <proton/message.h>
#endif

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

#define CONTAINERID_LEN 16 // 16 characters for 'L','a','T','e','_',<4 char: prod or cons>,'_','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','\0'
#define SENDERNAME_LEN 19 // 19 characters 'L','a','T','e','_',<4 char: prod or cons>,'_','t','x',_'<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','\0'
#define RECEIVERNAME_LEN 19 // 19 characters 'L','a','T','e','_',<4 char: prod or cons>,'_','r','x',_'<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','\0'

// Socket data structure, with a union in order to manage different protocols with the same struct
struct lampsock_data {
	int descriptor;
	union {
		struct sockaddr_ll addrll;
		struct sockaddr_in addrin[2]; // One structure for bind() and one structure for sendto()
	} addru;
	char devname[IFNAMSIZ];
	int ifindex;
	struct timeval rx_timeout;
};

#if AMQP_1_0_ENABLED
struct amqp_data {
	pn_proactor_t *proactor;
	pn_message_t *message;
	pn_millis_t proactor_timeout;
	byte_t *lampPacket;
	uint32_t lampPacketSize;
	char containerID[CONTAINERID_LEN];
	char senderName[RECEIVERNAME_LEN];
	char receiverName[RECEIVERNAME_LEN];

	int Wfiledescriptor; // File descriptor to write per-packet data to a CSV file when -W is specified
	perPackerDataStructure perPktData;	// Per-packet data structure (to be used when -W is selected)
};
#endif

struct src_addrs {
	struct in_addr srcIPaddr;
	macaddr_t srcmacaddr;
};

int socketCreator(protocol_t protocol);
int socketOpen(protocol_t protocol,struct lampsock_data *sData,struct options *opts,struct src_addrs *addressesptr);
int socketDataSetup(protocol_t protocol,struct lampsock_data *sData,struct options *opts,struct src_addrs *addressesptr);
int socketSetTimestamping(struct lampsock_data sData, int mode);
int pollErrqueueWait(int sFd,uint64_t timeout_ms);

#endif