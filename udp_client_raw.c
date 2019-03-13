#include "udp_client_raw.h"
#include "packet_structs.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "Rawsock_lib/rawsock_lamp.h"
#include "report_manager.h"
#include <inttypes.h>
#include <errno.h>
#include "timeval_subtract.h"
#include "common_thread.h"
#include "timer_man.h"
#include "common_udp.h"

// Local global variables
static pthread_t txLoop_tid, rxLoop_tid, ackListenerInit_tid, initSender_tid;
static uint16_t lamp_id_session;
static reportStructure reportData;

// Transmit error container
static t_error_types t_tx_error=NO_ERR;
// Receive error container
static t_error_types t_rx_error=NO_ERR;

static uint8_t ack_init_received=0; // Global flag set by the ackListener thread: = 1 when an ACK has been received, otherwise it is = 0
static pthread_mutex_t ack_init_received_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the ack_received variable (as it written by a thread and read by another one)

extern inline int timevalSub(struct timeval *in, struct timeval *out);

// Function prototypes
static void txLoop (arg_struct *args);
static void unidirRxTxLoop (arg_struct *args);

// Thread entry point function prototypes
static void *txLoop_t (void *arg);
static void *rxLoop_t (void *arg);
static void *ackListenerInit (void *arg);
static void *initSender (void *arg);

static void *ackListenerInit (void *arg) {
	struct arg_struct *args=(struct arg_struct *) arg;
	controlRCVdata rcvData;
	int return_value;

	rcvData.session_id=lamp_id_session;

	return_value=controlReceiverUDP_RAW(args->sData.descriptor,CLIENT_SRCPORT,args->srcIP.s_addr,&rcvData,ACK,&ack_init_received,&ack_init_received_mut);
	if(return_value<0) {
		if(return_value==-1) {
			t_rx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(return_value==-2) {
			t_rx_error=ERR_TIMEOUT_ACK;
		} else if(return_value==-3) {
			t_rx_error=ERR_RECVFROM_GENERIC;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}
	}

	pthread_exit(NULL);
}

static void *initSender (void *arg) {
	struct arg_struct *args=(struct arg_struct *) arg;
	int return_value;
	controlRCVdata initData;

	initData.controlRCV.ip=args->opts->destIPaddr;
	initData.controlRCV.port=CLIENT_SRCPORT;
	initData.controlRCV.session_id=lamp_id_session;
	memcpy(initData.controlRCV.mac,args->opts->destmacaddr,ETHER_ADDR_LEN);

	return_value=controlSenderUDP_RAW(args,&initData,lamp_id_session,INIT_RETRY_MAX_ATTEMPTS, INIT, INIT_RETRY_INTERVAL_MS, &ack_init_received, &ack_init_received_mut);
	if(return_value<0) {
		if(return_value==-1) {
			t_tx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(return_value==-2) {
			t_tx_error=ERR_SEND_INIT;
		} else if(return_value==-3) {
			t_rx_error=ERR_MALLOC;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}
	}

	pthread_exit(NULL);
}

static void *txLoop_t (void *arg) {
	arg_struct *args=(arg_struct *) arg;

	// Call the Tx loop
	txLoop(args);

	pthread_exit(NULL);
}

static void txLoop (arg_struct *args) {
	// Packet buffers and headers
	struct pktheaders_udp headers;
	struct pktbuffers_udp buffers = {NULL, NULL, NULL, NULL};
	// IP address (src+dest) structure
	struct ipaddrs ipaddrs;
	// id to be inserted in the id field on the IP header
	unsigned int id=START_ID;
	// "in packet" LaMP header pointer
	struct lamphdr *inpacket_lamphdr;

	// Timer management variables
	struct pollfd timerMon;
	int clockFd;
	int timerCaS_res=0;

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Payload buffer
	byte_t *payload_buff=NULL;

	// while loop counter
	unsigned int counter=0;

	// Final packet size
	size_t finalpktsize;

	// LaMP packet type and end_flag
	uint8_t ctrl=CTRL_PINGLIKE_REQ;
	endflag_t end_flag;
	uint32_t lampPacketSize=0;

	// Populating headers
	// [IMPROVEMENT] Future improvement: get destination MAC through ARP or broadcasted information and not specified by the user
	etherheadPopulate(&(headers.etherHeader), args->srcMAC, args->opts->destmacaddr, ETHERTYPE_IP);
	IP4headPopulateS(&(headers.ipHeader), args->devname, args->opts->destIPaddr, 0, 0, BASIC_UDP_TTL, IPPROTO_UDP, FLAG_NOFRAG_MASK, &ipaddrs);
	UDPheadPopulate(&(headers.udpHeader), CLIENT_SRCPORT, args->opts->port);
	if(args->opts->mode_ub==PINGLIKE) {
		ctrl=CTRL_PINGLIKE_REQ;
	} else if(args->opts->mode_ub==UNIDIR) {
		if(args->opts->number==1) {
			ctrl=CTRL_UNIDIR_STOP;
		} else {
			ctrl=CTRL_UNIDIR_CONTINUE;
		}
	}
	lampHeadPopulate(&(headers.lampHeader), ctrl, lamp_id_session, 0); // Starting from sequence number = 0

	// Allocating packet buffers (with and without payload)
	if(args->opts->payloadlen!=0) {
		buffers.lamppacket=malloc(sizeof(struct lamphdr)+args->opts->payloadlen);
		if(!buffers.lamppacket) {
			t_tx_error=ERR_MALLOC;
			pthread_exit(NULL);
		}

		lampPacketSize=LAMP_HDR_PAYLOAD_SIZE(args->opts->payloadlen);
	} else {
		// There is no need to allocate the lamppacket buffer, as the LaMP header will be directly encapsulated inside UDP
		lampPacketSize=LAMP_HDR_SIZE();
	}

	buffers.udppacket=malloc(UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.udppacket) {
		free(buffers.lamppacket);
		t_tx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	buffers.ippacket=malloc(IP_UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.ippacket) {
		free(buffers.lamppacket);
		free(buffers.udppacket);
		t_tx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	buffers.ethernetpacket=malloc(ETH_IP_UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.ethernetpacket) {
		free(buffers.lamppacket);
		free(buffers.ippacket);
		free(buffers.udppacket);
		t_tx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	// Populate payload buffer only if 'payloadlen' is different than 0
	if(args->opts->payloadlen!=0) {
		payload_buff=malloc((args->opts->payloadlen)*sizeof(byte_t));
		if(!payload_buff) {
			free(buffers.lamppacket);
			free(buffers.ethernetpacket);
			free(buffers.ippacket);
			free(buffers.udppacket);

			t_tx_error=ERR_MALLOC;
			pthread_exit(NULL);
		}

		for(int i=0;i<args->opts->payloadlen;i++) {
			payload_buff[i]=(byte_t) (i%16);
		}
	}

	// Get "in packet" LaMP header pointer
	inpacket_lamphdr=(struct lamphdr *) (buffers.ethernetpacket+sizeof(struct ether_header)+sizeof(struct iphdr)+sizeof(struct udphdr));

	// Set end_flag to FLG_CONTINUE
	end_flag=FLG_CONTINUE;

	// Create and start timer
	timerCaS_res=timerCreateAndSet(&timerMon, &clockFd, args->opts->interval);

	if(timerCaS_res==-1) {
		t_tx_error=ERR_TIMERCREATE;
		pthread_exit(NULL);
	} else if(timerCaS_res==-2) {
		t_tx_error=ERR_SETTIMER;
		pthread_exit(NULL);
	}

	// Run until 'number' is reached
	while(counter<args->opts->number) {
		// poll waiting for events happening on the timer descriptor (i.e. wait for timer expiration)
		if(poll(&timerMon,1,INDEFINITE_BLOCK)>0) {
			// "Clear the event" by performing a read() on a junk variable
			read(clockFd,&junk,sizeof(junk));
			
			// Prepare datagram
			IP4headAddID(&(headers.ipHeader),(unsigned short) id);
			id+=INCR_ID;

			// Encapsulate LaMP payload only it is available
			if(args->opts->payloadlen!=0) {
				lampEncapsulate(buffers.lamppacket, &(headers.lampHeader), payload_buff, args->opts->payloadlen);
				UDPencapsulate(buffers.udppacket,&(headers.udpHeader),buffers.lamppacket,(size_t) lampPacketSize,ipaddrs);
			} else {
				UDPencapsulate(buffers.udppacket,&(headers.udpHeader),(byte_t *)&(headers.lampHeader),(size_t) lampPacketSize,ipaddrs);
			}

			// 'IP4headAddTotLen' may also be skipped since IP4Encapsulate already takes care of filling the length field
			IP4Encapsulate(buffers.ippacket, &(headers.ipHeader), buffers.udppacket, UDP_PACKET_SIZE_S(lampPacketSize));
			finalpktsize=etherEncapsulate(buffers.ethernetpacket, &(headers.etherHeader), buffers.ippacket, IP_UDP_PACKET_SIZE_S(lampPacketSize));

			if(args->opts->mode_ub==UNIDIR) {
				fprintf(stdout,"Sent unidirectional message with destination MAC: " PRI_MAC " (id=%u, seq=%u).\n",
					MAC_PRINTER(args->opts->destmacaddr), lamp_id_session, counter);
			}

			// Set end flag to FLG_STOP when it's time to send the last packet
			if(counter==(args->opts->number-1)) {
					end_flag=FLG_STOP;
			}

			if(rawLampSend(args->sData.descriptor, args->sData.addru.addrll, inpacket_lamphdr, buffers.ethernetpacket, finalpktsize, end_flag, UDP)) {
				if(errno==EMSGSIZE) {
					fprintf(stderr,"Error: EMSGSIZE 90 Message too long.\n");
				}
				fprintf(stderr,"Failed sending latency measurement packet with seq: %u.\nThe execution will terminate now.\n",headers.lampHeader.seq);
				break;
			}

			// Increase sequence number for the next iteration
			lampHeadIncreaseSeq(&(headers.lampHeader));

			// Increase counter
			counter++;
		}
	}

	// Free payload buffer
	if(args->opts->payloadlen!=0) {
		free(payload_buff);
	}

	// Close timer file descriptor
	close(clockFd);

	// Free all buffers before exiting
	// Free the LaMP packet buffer only if it was allocated (otherwise a SIGSEGV may occur if payloadLen is 0)
	if(buffers.lamppacket) free(buffers.lamppacket);
	free(buffers.udppacket);
	free(buffers.ippacket);
	free(buffers.ethernetpacket);
}

static void *rxLoop_t (void *arg) {
	arg_struct *args=(arg_struct *) arg;

	// Packet buffer with size = Ethernet MTU
	byte_t packet[RAW_RX_PACKET_BUF_SIZE];

	// recvfrom variables
	ssize_t rcv_bytes;
	size_t UDPpayloadsize; // UDP payload size

	struct pktheadersptr_udp headerptrs;
	byte_t *payload=NULL;
	byte_t *lampPacket=NULL;

	// RX and TX timestamp containers
	struct timeval rx_timestamp, tx_timestamp;

	// Variable to store the latency (trip time)
	uint64_t tripTime;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx;
	uint16_t lamp_payloadlen_rx;

	// struct sockaddr_ll filled by recvfrom() and used to filter out outgoing traffic
	struct sockaddr_ll addrll;
	socklen_t addrllLen=sizeof(addrll);

	// SO_TIMESTAMP variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBuf[CMSG_SPACE(sizeof(struct timeval))];

	// Container for the source MAC address (read from packet)
	macaddr_t srcmacaddr_pkt=prepareMacAddrT();

	// Check if the MAC address was properly allocated
	if(macAddrTypeGet(srcmacaddr_pkt)==MAC_NULL) {
		t_rx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	// Prepare ancillary data structures, if KRT mode is selected
	if(args->opts->latencyType==KRT) {
		// iovec buffers (scatter/gather arrays)
		iov.iov_base=packet;
		iov.iov_len=sizeof(packet);

		// Socket address structure
		mhdr.msg_name=&(addrll);
		mhdr.msg_namelen=addrllLen;

		// Ancillary data (control message)
		mhdr.msg_control=ctrlBuf;
        mhdr.msg_controllen=sizeof(ctrlBuf);

        // iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
	}

	// Already get all the packet pointers
	lampPacket=UDPgetpacketpointers(packet,&(headerptrs.etherHeader),&(headerptrs.ipHeader),&(headerptrs.udpHeader));
	payload=lampGetPacketPointers(lampPacket,&(headerptrs.lampHeader));

	// From now on, 'payload' should -never- be used if (headerptrs.lampHeader)->payloadLen is 0

	// Start receiving packets until an 'ENDREPLY' one is received (this is the ping-like loop)
	do {
		// If in KRT mode, use recvmsg(), otherwise, use recvfrom
		if(args->opts->latencyType==KRT) {
			saferecvmsg(rcv_bytes,args->sData.descriptor,&mhdr,NO_FLAGS);

			// Extract ancillary data
			for (cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
                if (cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMP) {
                    rx_timestamp=*((struct timeval *)CMSG_DATA(cmsg));
                }
			}
		} else {
			saferecvfrom(rcv_bytes,args->sData.descriptor,packet,RAW_RX_PACKET_BUF_SIZE,NO_FLAGS,(struct sockaddr *)&addrll,&addrllLen);
		}
		

		// Timeout or other recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				t_rx_error=ERR_TIMEOUT;
				fprintf(stderr,"Timeout when waiting for the report. No report will be printed by the client.\n");
			} else {
				t_rx_error=ERR_RECVFROM_GENERIC;
			}
			break;
		}

		// Filter out all outgoing packets - [IMPROVEMENT] use BPF instead
		if(addrll.sll_pkttype==PACKET_OUTGOING) {
			continue;
		}

		// Go on only if it is a datagram of interest (in our case if it is IPv4/UDP and if has the right destination IP address (i.e. own IP address)
		if (ntohs((headerptrs.etherHeader)->ether_type)!=ETHERTYPE_IP) { 
			continue;
		}

		if ((headerptrs.ipHeader)->protocol!=IPPROTO_UDP || CHECK_IP_ADDR_DST(args->srcIP.s_addr) ||ntohs((headerptrs.udpHeader)->dest)!=CLIENT_SRCPORT) {
			continue;
		}

		// Verify checksums
		// Validate checksum (combined mode: IP+UDP): if it is wrong, discard packet
		UDPpayloadsize=UDPgetpayloadsize((headerptrs.udpHeader));
		if(!validateEthCsum(packet, (headerptrs.udpHeader)->check, &((headerptrs.ipHeader)->check), CSUM_UDPIP, (void *) &UDPpayloadsize)) {
			continue;
		}

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP((headerptrs.lampHeader)->reserved,(headerptrs.lampHeader)->ctrl)) {
			continue;
		}

		// If the packet is really a LaMP packet, get the header data
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, &tx_timestamp, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_id_rx!=lamp_id_session || (lamp_type_rx!=PINGLIKE_REPLY && lamp_type_rx!=PINGLIKE_ENDREPLY)) {
			continue;
		}

		if(args->opts->latencyType==USERTOUSER) {
			gettimeofday(&rx_timestamp,NULL); // gettimeofday() considered as defaut behaviour
		}


		if(timevalSub(&tx_timestamp,&rx_timestamp)) {
			fprintf(stderr,"Warning: negative latency!\nThis could potentually indicate that SIOCGSTAMP is not working properly on your system.\n");
			tripTime=0;
		} else {
			tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
		}

		// Get source MAC address from packet
		getSrcMAC(headerptrs.etherHeader,srcmacaddr_pkt);

		fprintf(stdout,"Received a reply from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
			MAC_PRINTER(srcmacaddr_pkt), lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(args->opts->latencyType));

		// Update the current report structure
		reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);

	} while(lamp_type_rx!=PINGLIKE_ENDREPLY);

	// Free source MAC address memory area
	freeMacAddrT(srcmacaddr_pkt);

	pthread_exit(NULL);
}

static void unidirRxTxLoop (arg_struct *args) {
	// Packet buffers for Rx
	byte_t packet[RAW_RX_PACKET_BUF_SIZE];
	byte_t *payload=NULL;
	byte_t *lampPacket=NULL;

	// Header pointers
	struct pktheadersptr_udp headerptrs;

	// UDP payload size container
	size_t UDPpayloadsize;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx;
	uint16_t lamp_payloadlen_rx;

	uint8_t timeoutFlag=0; // = 1 if a timeout occurred, otherwise it should remain = 0

	ssize_t rcv_bytes;

	controlRCVdata ACKdata;

	// struct sockaddr_ll filled by recvfrom() and used to filter out outgoing traffic
	struct sockaddr_ll addrll;
	socklen_t addrllLen=sizeof(addrll);

	/* --------------------------- Rx part --------------------------- */

	// Already get all the packet pointers for Rx
	lampPacket=UDPgetpacketpointers(packet,&(headerptrs.etherHeader),&(headerptrs.ipHeader),&(headerptrs.udpHeader));
	payload=lampGetPacketPointers(lampPacket,&(headerptrs.lampHeader));

	// There's no real loop now, just wait for a correct report and send ACK
	while(1) {
		saferecvfrom(rcv_bytes,args->sData.descriptor,packet,RAW_RX_PACKET_BUF_SIZE,NO_FLAGS,(struct sockaddr *)&addrll,&addrllLen);

		// Timeout or other recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				t_rx_error=ERR_TIMEOUT;
				fprintf(stderr,"Timeout when waiting for the report. No report will be printed by the client.\n");
			} else {
				t_rx_error=ERR_RECVFROM_GENERIC;
			}
			timeoutFlag=1;
			break;
		}

		// Filter out all outgoing packets - [IMPROVEMENT] use BPF instead
		if(addrll.sll_pkttype==PACKET_OUTGOING) {
			continue;
		}

		// Go on only if it is a datagram of interest (in our case if it is UDP)
		if (ntohs((headerptrs.etherHeader)->ether_type)!=ETHERTYPE_IP) { 
			continue;
		}
		if ((headerptrs.ipHeader)->protocol!=IPPROTO_UDP || CHECK_IP_ADDR_DST(args->srcIP.s_addr) || ntohs((headerptrs.udpHeader)->dest)!=CLIENT_SRCPORT) {
			continue;
		}

		// Verify checksums
		// Validate checksum (combined mode: IP+UDP): if it is wrong, discard packet
		UDPpayloadsize=UDPgetpayloadsize((headerptrs.udpHeader));
		if(!validateEthCsum(packet, (headerptrs.udpHeader)->check, &((headerptrs.ipHeader)->check), CSUM_UDPIP, (void *) &UDPpayloadsize)) {
			continue;
		}

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP((headerptrs.lampHeader)->reserved,(headerptrs.lampHeader)->ctrl)) {
			continue;
		}

		// If the packet is really a LaMP packet, get the header data
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, NULL, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_id_rx!=lamp_id_session || lamp_type_rx!=REPORT) {
			continue;
		}

		// If, finally, the packet is a report one, parse the report structure and send ACK
		break;
	}

	if(timeoutFlag==0) {
		/* --------------------------- Tx and report parsing part --------------------------- */

		// Parse report structure (for now, it is encoded as a string for conveniency)
		// Total packets is known to the client only, in this implementation, and it is already set thanks to reportStructureInit(), which
		// is setting it to 'opts->number'
		repscanf((const char *)payload,&reportData);

		// Fill the ACKdata structure
		ACKdata.controlRCV.ip=args->opts->destIPaddr;
		ACKdata.controlRCV.port=CLIENT_SRCPORT;
		memcpy(ACKdata.controlRCV.mac,args->opts->destmacaddr,ETHER_ADDR_LEN);

		if(controlSenderUDP_RAW(args, &ACKdata, lamp_id_session, 1, ACK, 0, NULL, NULL)<0) {
			fprintf(stderr,"Failed sending ACK.\n");
			t_rx_error=ERR_SEND;
		}
	}
}

unsigned int runUDPclient_raw(struct lampsock_data sData, char *devname, macaddr_t srcMAC, struct in_addr srcIP, struct options *opts) {
	// Thread argument structure
	arg_struct args;

	// Inform the user about the current options
	fprintf(stdout,"UDP client started, with options:\n\t[socket type] = RAW\n"
		"\t[interval] = %" PRIu64 " ms\n"
		"\t[reception timeout] = %" PRIu64 " ms\n"
		"\t[total number of packets] = %" PRIu64 "\n"
		"\t[mode] = %s\n"
		"\t[payload length] = %" PRIu16 " B \n"
		"\t[destination IP address] = %s\n"
		"\t[destination MAC address] = " PRI_MAC "\n"
		"\t[latency type] = %s\n",
		opts->interval, opts->interval<=MIN_TIMEOUT_VAL_C ? MIN_TIMEOUT_VAL_C+2000 : opts->interval+2000,
		opts->number, opts->mode_ub==UNIDIR?"unidirectional":"ping-like", 
		opts->payloadlen, inet_ntoa(opts->destIPaddr), MAC_PRINTER(opts->destmacaddr),
		latencyTypePrinter(opts->latencyType));

	// Print current UP
	if(opts->macUP==UINT8_MAX) {
		fprintf(stdout,"\t[user priority] = unset or unpatched kernel\n");
	} else {
		fprintf(stdout,"\t[user priority] = %d\n",opts->macUP);
	}

	if(opts->latencyType==KRT) {
		// Check if the KRT mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData.descriptor)<0) {
		 	perror("socketSetTimestamping() error");
		    fprintf(stderr,"Warning: SO_TIMESTAMP is probably not suppoerted. Switching back to user-to-user latency.\n");
		    opts->latencyType=USERTOUSER;
		}
	}

	// Initialize the report structure
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType);

	// Populate the 'args' struct
	args.sData=sData;
	args.opts=opts;
	args.devname=devname;
	args.srcMAC=srcMAC;
	args.srcIP=srcIP;

	// LaMP ID is randomly generated between 0 and 65535 (the maximum over 16 bits)
	lamp_id_session=(rand()+getpid())%UINT16_MAX;

	// This fprintf() terminates the series of call to inform the user about current settings -> using \n\n instead of \n
	fprintf(stdout,"\t[session LaMP ID] = %" PRIu16 "\n\n",lamp_id_session);

	// Start init procedure
	// Create INIT send and ACK listener threads
	pthread_create(&initSender_tid,NULL,&initSender,(void *) &args);
	pthread_create(&ackListenerInit_tid,NULL,&ackListenerInit,(void *) &args);

	// Wait for the threads to finish
	pthread_join(initSender_tid,NULL);
	pthread_join(ackListenerInit_tid,NULL);

	if(t_tx_error==NO_ERR && t_rx_error==NO_ERR) {
		if(opts->mode_ub==PINGLIKE) {
			// Create a sending thread and a receiving thread, then wait for their termination
			pthread_create(&txLoop_tid,NULL,&txLoop_t,(void *) &args);
			pthread_create(&rxLoop_tid,NULL,&rxLoop_t,(void *) &args);

			// Wait for the threads to finish
			pthread_join(txLoop_tid,NULL);
			pthread_join(rxLoop_tid,NULL);
		} else if(opts->mode_ub==UNIDIR) {
			txLoop(&args);
			unidirRxTxLoop(&args);
		} else {
			fprintf(stderr,"Error: some unknown error caused the mode not be set when starting the UDP client.\n");
			return 1;
		}
	} else {
		fprintf(stderr,"Error: the init procedure could not be completed. No test will be performed.\n");
	}

	// Print error messages, if errors have occurred (and, in case of error, return 1)
	if(t_tx_error!=NO_ERR) {
		thread_error_print("UDP Tx loop", t_tx_error);
		return 1;
	}

	if(t_rx_error!=NO_ERR) {
		thread_error_print("UDP Rx loop", t_rx_error);
		return 1;
	}

	/* Ok, the mode_ub==UNSET_UB case is not managed, but it should never happen to reach this point
	with an unset mode... at least not without getting errors or a chaotic thread behaviour! But it should not happen anyways. */
	fprintf(stdout,opts->mode_ub==PINGLIKE?"Ping-like ":"Unidirectional " "statistics:\n");
	// Print the statistics, if no error, before returning
	printStats(&reportData,stdout);

	if(opts->filename!=NULL) {
		// If '-f' was specified, print the report data to a file too
		printStatsCSV(opts,&reportData,opts->filename);
	}

	// Returning 0 if everything worked fine
	return 0;
}