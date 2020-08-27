#ifndef NONRAW_SOCKETMAN_H_INCLUDED
#define NONRAW_SOCKETMAN_H_INCLUDED
#include <linux/if_packet.h>
#include <net/if.h>
#include "rawsock.h"
#include "report_data_structs.h"
#include "options.h"

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

#if AMQP_1_0_ENABLED
#define CONTAINERID_LEN 16 // 16 characters for 'L','a','T','e','_',<4 char: prod or cons>,'_','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','\0'
#define SENDERNAME_LEN 19 // 19 characters 'L','a','T','e','_',<4 char: prod or cons>,'_','t','x',_'<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','\0'
#define RECEIVERNAME_LEN 19 // 19 characters 'L','a','T','e','_',<4 char: prod or cons>,'_','r','x',_'<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','<LaMP ID>','\0'
#endif

// connectWithTimeout() errors
#define CONNECT_ERROR_FCNTL_GETFL -2
#define CONNECT_ERROR_FCNTL_SETNOBLK -3
#define CONNECT_ERROR_FCNTL_RESTOREBLK -4
#define CONNECT_ERROR_TIMEOUT -5
#define CONNECT_ERROR_IMMEDIATEFAILURE -6
#define CONNECT_ERROR_POLL -7
#define CONNECT_ERROR_STATUS_UNKNOWN -8
#define CONNECT_ERROR_ALREADY_CONNECTED -9

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

	// UDP socket parameters for the -w reporting option
 	report_sock_data_t sock_w_data;
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
int connectWithTimeout(int sockfd, const struct sockaddr *addr,socklen_t addrlen,int timeout_ms);
char *connectWithTimeoutStrError(int retval);

#endif