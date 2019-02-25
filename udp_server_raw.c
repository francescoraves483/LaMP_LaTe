#include "udp_server_raw.h"
#include "report_manager.h"
#include "packet_structs.h"
#include "timeval_subtract.h"
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/if.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include "common_thread.h"
#include "Rawsock_lib/ipcsum_alth.h"
#include "timer_man.h"
#include "common_udp.h"

#define CLEAR_ALL() pthread_mutex_destroy(&ack_report_received_mut); \
					freeMacAddrT(srcmacaddr_pkt);
	
typedef enum {
	FLAG_UNSET,
	FLAG_SET
} setunset_t;

// Receive error container
static t_error_types t_rx_error;
static t_error_types t_tx_error;

// Local global variables
static reportStructure reportData;
static uint16_t lamp_id_session;
static modeub_t mode_session;
static uint16_t client_port_session; // Stored in host byte order
static uint8_t ack_report_received; // Global flag set by the ackListener thread: = 1 when an ACK has been received, otherwise it is = 0

// Thread ID for ackListener
static pthread_t ackListener_tid;

// Mutex to protect the ack_report_received variable (as it written by a thread and read by another one, i.e. the main thread)
static pthread_mutex_t ack_report_received_mut;

// Function prototypes
static int transmitReport(struct lampsock_data sData, char *devname, struct options *opts, struct in_addr destIP, struct in_addr srcIP, macaddr_t srcMAC, macaddr_t destMAC);
extern inline int timevalSub(struct timeval *in, struct timeval *out);
static uint8_t initReceiverACKsender(struct arg_struct *args, uint64_t interval, in_port_t port);

// Thread entry point functions
static void *ackListener(void *arg);

// As the report is periodically sent, a thread waits for an ACK message from the client. If it received, a proper variable is updated.
static void *ackListener(void *arg) { // [TODO] Return rcvData - document rcvData
	arg_struct_rx *args=(arg_struct_rx *) arg;
	controlRCVdata rcvData;

	int controlRcvRetValue;

	rcvData.session_id=lamp_id_session;

	controlRcvRetValue=controlReceiverUDP_RAW(args->sData.descriptor,args->opts->port,args->srcIP.s_addr,&rcvData,ACK,&ack_report_received,&ack_report_received_mut);

	if(controlRcvRetValue<0) {
		if(controlRcvRetValue==-1) {
			t_rx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(controlRcvRetValue==-2) {
			t_rx_error=ERR_TIMEOUT_ACK;
		} else if(controlRcvRetValue==-3) {
			t_rx_error=ERR_RECVFROM_GENERIC;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}
	}

	pthread_exit(NULL);
}

static uint8_t initReceiverACKsender(struct arg_struct *args, uint64_t interval, in_port_t port) {
	controlRCVdata rcvData;
	uint8_t return_val=0; // = 0 is everything is ok, = 1 if an error occurred
	int controlRcvRetValue, controlSendRetValue;

	// struct timeval to set the more reasonable timeout after the first LaMP packet (i.e. the INIT packet) is received
	struct timeval rx_timeout_reasonable;

	controlRcvRetValue=controlReceiverUDP_RAW(args->sData.descriptor,port,args->srcIP.s_addr,&rcvData,INIT,NULL,NULL);

	if(controlRcvRetValue<0) {
		// Set error
		if(controlRcvRetValue==-1) {
			t_rx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(controlRcvRetValue==-2) {
			t_rx_error=ERR_TIMEOUT_INIT;
		} else if(controlRcvRetValue==-3) {
			t_rx_error=ERR_RECVFROM_GENERIC;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}

		return_val=1;
	} else {
		// Set the destination port inside the sendto sockaddr_in structure
		client_port_session=rcvData.controlRCV.port;

		// Set session data
		fprintf(stdout,"Server will accept all packets coming from client %s, port: %d, id: %u\n",
			inet_ntoa(rcvData.controlRCV.ip),client_port_session,rcvData.controlRCV.session_id);

		lamp_id_session=rcvData.controlRCV.session_id;

		if(rcvData.controlRCV.conn_idx==INIT_UNIDIR_INDEX) {
			mode_session=UNIDIR;
		} else if(rcvData.controlRCV.conn_idx==INIT_PINGLIKE_INDEX) {
			mode_session=PINGLIKE;
		} else {
			mode_session=UNSET_MUB;
		}

		if(mode_session!=UNSET_MUB) {
			fprintf(stdout,"Server will work in %s mode.\n",mode_session==UNIDIR ? "unidirectional" : "ping-like");

			// Set also a more reasonable timeout, as defined by the user with -t or equal to MIN_TIMEOUT_VAL_S ms if the user specified less than MIN_TIMEOUT_VAL_S ms
			if(interval<=MIN_TIMEOUT_VAL_S) {
				rx_timeout_reasonable.tv_sec=MIN_TIMEOUT_VAL_S/1000;
				rx_timeout_reasonable.tv_usec=0;
			} else {
				rx_timeout_reasonable.tv_sec=(time_t) ((interval)/MILLISEC_TO_SEC);
				rx_timeout_reasonable.tv_usec=MILLISEC_TO_MICROSEC*interval-rx_timeout_reasonable.tv_sec*SEC_TO_MICROSEC;
			}
			if(setsockopt(args->sData.descriptor, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout_reasonable, sizeof(rx_timeout_reasonable))!=0) {
				fprintf(stderr,"Warning: could not set RCVTIMEO: in case certain packets are lost,\n"
				"the program may run for an indefinite time and may need to be terminated with Ctrl+C.\n");
			}

			// Send ACK
			controlSendRetValue=controlSenderUDP_RAW(args, &rcvData, lamp_id_session, 1, ACK, 0, NULL, NULL);
			if(controlSendRetValue<0) {
				// Set error
				if(controlSendRetValue==-1) {
					t_tx_error=ERR_INVALID_ARG_CMONUDP;
				} else if(controlSendRetValue==-2) {
					t_tx_error=ERR_SEND_ACK;
				} else {
					t_rx_error=ERR_UNKNOWN;
				}
				return_val=1;
			}
		} else {
			return_val=1;
		}
	}

	return return_val;
}

static int transmitReport(struct lampsock_data sData, char *devname, struct options *opts, struct in_addr destIP, struct in_addr srcIP, macaddr_t srcMAC, macaddr_t destMAC) {
	// Packet buffers and headers
	struct pktheaders_udp headers;
	struct pktbuffers_udp buffers;
	// IP address (src+dest) structure
	struct ipaddrs ipaddrs;
	// id to be inserted in the id field on the IP header (random ID could be ok?)
	unsigned int id=rand()%UINT16_MAX;
	// "in packet" LaMP header pointer
	struct lamphdr *inpacket_lamphdr;

	// LaMP packet size container
	uint32_t lampPacketSize=0;

	// Report payload length and report buffer
	size_t report_payloadlen;
	char report_buff[REPORT_BUFF_SIZE]; // REPORT_BUFF_SIZE defined inside report_manager.h

	// Final packet size
	size_t finalpktsize;

	// While loop counter
	int counter=0;

	// Timer management variables
	struct pollfd timerMon;
	int clockFd;

	int poll_retval=1;

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Return value (= 0 if everything is ok, = 1 if an error occurred)
	int return_val=0;

	// Thread argument structure
	arg_struct_rx args;

	// Populate the 'args' struct
	args.sData=sData;
	args.opts=opts;
	args.srcIP=srcIP;

	// Populating headers
	// [IMPROVEMENT] Future improvement: get destination MAC through ARP or broadcasted information and not specified by the user
	etherheadPopulate(&(headers.etherHeader), srcMAC, destMAC, ETHERTYPE_IP);
	IP4headPopulateS(&(headers.ipHeader), devname, destIP, 0, 0, BASIC_UDP_TTL, IPPROTO_UDP, FLAG_NOFRAG_MASK, &ipaddrs);
	UDPheadPopulate(&(headers.udpHeader), opts->port, client_port_session);

	// Copying the report string inside the report buffer
	repprintf(report_buff,reportData);

	// Compute report payload length
	report_payloadlen=strlen(report_buff);

	// Allocating buffers
	buffers.lamppacket=malloc(sizeof(struct lamphdr)+report_payloadlen);
	if(!buffers.lamppacket) {
		return 2;
	}

	lampPacketSize=LAMP_HDR_PAYLOAD_SIZE(report_payloadlen);

	buffers.udppacket=malloc(UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.udppacket) {
		free(buffers.lamppacket);
		return 2;
	}

	buffers.ippacket=malloc(IP_UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.ippacket) {
		free(buffers.lamppacket);
		free(buffers.udppacket);
		return 2;
	}

	buffers.ethernetpacket=malloc(ETH_IP_UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.ethernetpacket) {
		free(buffers.lamppacket);
		free(buffers.ippacket);
		free(buffers.udppacket);
		return 2;
	}

	// Get "in packet" LaMP header pointer
	inpacket_lamphdr=(struct lamphdr *) (buffers.ethernetpacket+sizeof(struct ether_header)+sizeof(struct iphdr)+sizeof(struct udphdr));

	lampHeadPopulate(&(headers.lampHeader), CTRL_UNIDIR_REPORT, lamp_id_session, 0); // Starting back from sequence number = 0

	// Create thread for receiving the ACK from the client
	pthread_create(&ackListener_tid,NULL,&ackListener,(void *) &args);

	// Create and start timer
	if(timerCreateAndSet(&timerMon, &clockFd, REPORT_RETRY_INTERVAL_MS)<0) {
		return 1;
	}

	// Send report over and over until an ACK is received or until REPORT_RETRY_MAX_ATTEMPTS attempts have been tried (REPORT_RETRY_MAX_ATTEMPTS is defined in options.h)
	for(counter=0;counter<REPORT_RETRY_MAX_ATTEMPTS;counter++) {
		if(poll_retval>0) {
			pthread_mutex_lock(&ack_report_received_mut);
			if(ack_report_received) {
				break;
			}
			pthread_mutex_unlock(&ack_report_received_mut);

			// Prepare datagram
			IP4headAddID(&(headers.ipHeader),(unsigned short) id); // random ID could be okay?

			lampEncapsulate(buffers.lamppacket, &(headers.lampHeader), (byte_t *) report_buff, report_payloadlen);
			UDPencapsulate(buffers.udppacket,&(headers.udpHeader),buffers.lamppacket,lampPacketSize,ipaddrs);

			// 'IP4headAddTotLen' may also be skipped since IP4Encapsulate already takes care of filling the length field
			IP4Encapsulate(buffers.ippacket, &(headers.ipHeader), buffers.udppacket, UDP_PACKET_SIZE_S(lampPacketSize));
			finalpktsize=etherEncapsulate(buffers.ethernetpacket, &(headers.etherHeader), buffers.ippacket, IP_UDP_PACKET_SIZE_S(lampPacketSize));

			if(rawLampSend(sData.descriptor, sData.addru.addrll, inpacket_lamphdr, buffers.ethernetpacket, finalpktsize, FLG_NONE, UDP)) {
				fprintf(stderr,"Failed sending report. Retrying in %d millisecond(s).\n",REPORT_RETRY_INTERVAL_MS);
			}
		}
	
		// Successive attempts will have an increased sequence number
		lampHeadIncreaseSeq(&(headers.lampHeader));

		poll_retval=poll(&timerMon,1,INDEFINITE_BLOCK);
		if(poll_retval>0) {
			// "Clear the event" by performing a read() on a junk variable
			read(clockFd,&junk,sizeof(junk));
		}
	}

	// Make sure to go on only if the ackLister thread has terminated its execution
	// See also: https://stackoverflow.com/questions/39104425/threads-exiting-before-joining 
	pthread_join(ackListener_tid,NULL);

	if(t_rx_error!=NO_ERR) {
		thread_error_print("UDP server ACK listerner loop (report transmission)", t_rx_error);
		return_val=1;
	}

	close(clockFd);

	// Free all buffers before returning
	free(buffers.lamppacket);
	free(buffers.udppacket);
	free(buffers.ippacket);
	free(buffers.ethernetpacket);

	return return_val;
}

// Run UDP server: with respect to the client, only one loop is present, thus with no need to create other threads
// This also makes the code simpler.
// It basically works as the client's UDP Tx Loop, but inserted within the main function, with some if-else statements
// to discriminate the pinglike and unidirectional communications, making the common portion of the code to be written only once
unsigned int runUDPserver_raw(struct lampsock_data sData, char *devname, macaddr_t srcMAC, struct in_addr srcIP, struct options *opts) {
	struct arg_struct args;

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
	uint16_t lamp_seq_rx=0; // Initilized to 0 in order to be sure to enter the while loop
	uint16_t lamp_payloadlen_rx;

	// Container for the source MAC address (read from packet)
	macaddr_t srcmacaddr_pkt=prepareMacAddrT();

	// Check if the MAC address was properly allocated
	if(macAddrTypeGet(srcmacaddr_pkt)==MAC_NULL) {
		return 1;
	}

	// while loop continue flag
	int continueFlag=1;

	// SO_TIMESTAMP variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBuf[CMSG_SPACE(sizeof(struct timeval))];

	// struct in_addr containing the destination IP address (read as source IP address from the packets coming from the client)
	// To be passed as argument to transmitReport()ù
	struct in_addr destIP_inaddr;

	// struct sockaddr_ll filled by recvfrom() and used to filter out outgoing traffic
	struct sockaddr_ll addrll;
	socklen_t addrllLen=sizeof(addrll);

	// Very important: initialize to 0 any flag that is used inside threads
	ack_report_received=0;
	t_rx_error=NO_ERR;
	t_tx_error=NO_ERR;
	if(pthread_mutex_init(&ack_report_received_mut,NULL)!=0) {
		fprintf(stderr,"Could not allocate a mutex to synchronize the internal threads.\nThe execution will terminate now.\n");
		return 2;
	}

	// Inform the user about the current options
	fprintf(stdout,"UDP server started, with options:\n\t[socket type] = RAW\n"
		"\t[listening on port] = %ld\n"
		"\t[timeout] = %" PRIu64 " ms\n",
		opts->port,
		opts->interval<=MIN_TIMEOUT_VAL_S ? MIN_TIMEOUT_VAL_S : opts->interval);

	// Print current UP
	if(opts->macUP==UINT8_MAX) {
		fprintf(stdout,"\t[user priority] = unset or unpatched kernel.\n\n");
	} else {
		fprintf(stdout,"\t[user priority] = %d\n\n",opts->macUP);
	}

	// Report structure inizialization
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType);

	// Populate the 'args' struct
	args.sData=sData;
	args.opts=opts;
	args.devname=devname;
	args.srcMAC=srcMAC;
	args.srcIP=srcIP;

	if(initReceiverACKsender(&args, opts->interval, opts->port)<0) {
		if(t_rx_error!=NO_ERR) {
			thread_error_print("UDP server INIT procedure (INIT reception)", t_rx_error);
		}

		if(t_tx_error!=NO_ERR) {
			thread_error_print("UDP server INIT procedure (ACK transmission)", t_tx_error);
		}

		CLEAR_ALL();
		return 3;
	}
	
	// -L is ignored if a bidirectional INIT packet was received (it's the client that should compute the latency, not the server)
	if(mode_session!=UNIDIR && opts->latencyType!=USERTOUSER) {
		fprintf(stderr,"Warning: a latency type was specified (-L), but it will be ignored.\n");
	} else if(mode_session==UNIDIR && opts->latencyType==RTT) {
		// Set SO_TIMESTAMP
		// Check if the RTT mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData.descriptor)<0) {
		 	perror("socketSetTimestamping() error");
			fprintf(stderr,"Warning: SO_TIMESTAMP is probably not suppoerted. Switching back to user-to-user latency.\n");
			opts->latencyType=USERTOUSER;
		}

		// Prepare ancillary data structures, if RTT mode is selected
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

	// Start receiving packets (this is the ping-like loop)
	while(continueFlag) {
		// If in RTT unidirectional mode, use recvmsg(), otherwise, use recvfrom
		if(mode_session==UNIDIR && opts->latencyType==RTT) {
			saferecvmsg(rcv_bytes,sData.descriptor,&mhdr,NO_FLAGS);

			// Extract ancillary data
			for (cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
                if (cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMP) {
                    rx_timestamp=*((struct timeval *)CMSG_DATA(cmsg));
                }
			}
		} else {
			saferecvfrom(rcv_bytes,sData.descriptor,packet,RAW_RX_PACKET_BUF_SIZE,NO_FLAGS,(struct sockaddr *)&addrll,&addrllLen);
		}

		// Timeout or other recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				fprintf(stderr,"Timeout reached when receiving packets. Connection terminated.\n");
				break;
			} else {
				fprintf(stderr,"Genetic recvfrom() error. errno = %d.\n",errno);
				break;
			}
		}

		// Filter out all outgoing packets - [IMPROVEMENT] use BPF instead
		if(addrll.sll_pkttype==PACKET_OUTGOING) {
			continue;
		}

		// Go on only if it is a datagram of interest (in our case if it is UDP and if it has the correct port number)
		if (ntohs((headerptrs.etherHeader)->ether_type)!=ETHERTYPE_IP) { 
			continue;
		}
		if ((headerptrs.ipHeader)->protocol!=IPPROTO_UDP || CHECK_IP_ADDR_DST(srcIP.s_addr) || ntohs((headerptrs.udpHeader)->dest)!=opts->port) {
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

		// Get source MAC address from packet (useful to print the information about from which client the server is accepting packets)
		// ...but also used in ping-like mode to prepare the reply to be sent!
		getSrcMAC(headerptrs.etherHeader,srcmacaddr_pkt);

		// If the packet is really a LaMP packet, get the header data
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, &tx_timestamp, NULL);

		// Discard any reply, ack or report, at the moment
		if(lamp_type_rx==PINGLIKE_REPLY || lamp_type_rx==PINGLIKE_REPLY_TLESS || lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==ACK || lamp_type_rx==REPORT || lamp_type_rx==INIT) {
			continue;
		}

		// Check whether the id is of interest or not
		if(lamp_id_rx!=lamp_id_session) {
			continue;
		}

		// If the packet is marked as last packet, set the continue flag to 0 for exiting the loop
		if(lamp_type_rx==UNIDIR_STOP || lamp_type_rx==PINGLIKE_ENDREQ) {
			continueFlag=0;
		}

		switch(mode_session) {
			case UNIDIR:
				if(opts->latencyType==USERTOUSER) {
					gettimeofday(&rx_timestamp,NULL);
				}

				if(timevalSub(&tx_timestamp,&rx_timestamp)) {
					fprintf(stderr,"Warning: negative latency!\nThe clock synchronization is not sufficienty precise to allow unidirectional measurements.\n");
					tripTime=0;
				} else {
					tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
				}

				fprintf(stdout,"Received a unidirectional message from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
					MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(opts->latencyType));

				// Update the current report structure
				reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);
			break;

			case PINGLIKE:
				// Transmit the reply as the copy of the request, with the some modified fields, which can be modified from packet
				// thanks to the pointers to the various headers, without having to un-encapsulate everything or to re-encapsulate the
				// various headers to build a separate packet. This -should- maintain a better efficiency...
				// ...but is bypassing a bit the "rawsock" library, directly resorting to 'memcpy's... hope it is not so unreadable!
				// At the next received packet, the memory area pointed by 'packet' should be substituted by the newly received packet,
				// thus allowing in some way this mechanism of directly replacing bytes inside the received data.

				fprintf(stdout,"Received a ping-like message from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d). Replying to client...\n",
					MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);

				// Edit some 'packet' fields
				// memcpy the MAC addresses
				memcpy((headerptrs.etherHeader)->ether_shost,srcMAC,ETHER_ADDR_LEN); // As source, my MAC address
				memcpy((headerptrs.etherHeader)->ether_dhost,srcmacaddr_pkt,ETHER_ADDR_LEN); // As destination, the previously source MAC address
				// Set destination IP and source IP
				headerptrs.ipHeader->daddr=headerptrs.ipHeader->saddr; // Swap destination and source IP addresses
				headerptrs.ipHeader->saddr=srcIP.s_addr; // Set source IP address as the own address
				// Swap UDP ports
				headerptrs.udpHeader->source=headerptrs.udpHeader->dest;
				headerptrs.udpHeader->dest=htons(client_port_session);
				
				// Set REPLY as LaMP type (or ENDREPLY if this is the last packet)
				headerptrs.lampHeader->ctrl = continueFlag==1 ? CTRL_PINGLIKE_REPLY : CTRL_PINGLIKE_ENDREPLY;

				// Recompute IP checksum
				headerptrs.ipHeader->check=0;
				headerptrs.ipHeader->check=ip_fast_csum((__u8 *)headerptrs.ipHeader, headerptrs.ipHeader->ihl);

				// Send packet (as the reply does require to carry the client timestamp, the control field should now correspond to CTRL_PINGLIKE_REPLY)
				// 'rcv_bytes' still stores the packet size, thus it can be used as packet size to be passed to rawLampSend(), wich will in turn call sendto() with that size
				// rawLampSend should also take care of re-computing the checksum, which is changed due to the different fields in the reply packet.
				if(rawLampSend(sData.descriptor, sData.addru.addrll, headerptrs.lampHeader, packet, rcv_bytes, FLG_NONE, UDP)) {
					fprintf(stderr,"UDP server reported that it can't reply to the client with id=%u and seq=%u\n",lamp_id_rx,lamp_seq_rx);
				}
			break;

			default:
			break;
		}
	}

	if(mode_session==UNIDIR) {
		destIP_inaddr.s_addr=headerptrs.ipHeader->saddr;
		// If the mode is the unidirectional one, get the destination IP/MAC from the last packet
		// Use as destination IP (destIP), the source IP of the last received packet (headerptrs.ipHeader->saddr)
		if(transmitReport(sData, devname, opts, destIP_inaddr, srcIP, srcMAC, srcmacaddr_pkt)) {
			fprintf(stderr,"UDP server reported an error while transmitting the report.\n"
				"No report will be transmitted.\n");
			CLEAR_ALL();
			return 4;
		}
	}

	// Destroy mutex (as it is no longer needed) and clear all the other data that should be clared (see the CLEAR_ALL() macro)
	CLEAR_ALL();

	return 0;
}