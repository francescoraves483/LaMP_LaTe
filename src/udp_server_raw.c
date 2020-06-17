#include "udp_server_raw.h"
#include "report_manager.h"
#include "packet_structs.h"
#include "timeval_utils.h"
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <linux/errqueue.h>
#include "carbon_thread_manager.h"
#include "common_thread.h"
#include "ipcsum_alth.h"
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
static modefollowup_t followup_mode_session;
static uint16_t client_port_session; // Stored in host byte order
static uint8_t ack_report_received; // Global flag set by the ackListener thread: = 1 when an ACK has been received, otherwise it is = 0

static carbonReportStructure carbonReportData;
static int carbon_metrics_flush_first;
static carbon_pthread_data_t ctd;

// Thread ID for ackListener
static pthread_t ackListener_tid;

// Mutex to protect the ack_report_received variable (as it written by a thread and read by another one, i.e. the main thread)
static pthread_mutex_t ack_report_received_mut;

// Function prototypes
static int transmitReport(struct lampsock_data sData, struct options *opts, struct in_addr destIP, struct in_addr srcIP, macaddr_t srcMAC, macaddr_t destMAC);
extern inline int timevalSub(struct timeval *in, struct timeval *out);
static uint8_t initReceiverACKsender(arg_struct *args, uint64_t interval, in_port_t port);

// Thread entry point functions
static void *ackListener(void *arg);

// As the report is periodically sent, a thread waits for an ACK message from the client. If it received, a proper variable is updated.
static void *ackListener(void *arg) {
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

static uint8_t initReceiverACKsender(arg_struct *args, uint64_t interval, in_port_t port) {
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

		if(rcvData.controlRCV.type_idx==INIT_UNIDIR_INDEX) {
			mode_session=UNIDIR;
		} else if(rcvData.controlRCV.type_idx==INIT_PINGLIKE_INDEX) {
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
			controlSendRetValue=controlSenderUDP_RAW(args,&rcvData,lamp_id_session,1,ACK,0,0,NULL,NULL);
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

static int transmitReport(struct lampsock_data sData, struct options *opts, struct in_addr destIP, struct in_addr srcIP, macaddr_t srcMAC, macaddr_t destMAC) {
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
	IP4headPopulateS(&(headers.ipHeader), sData.devname, destIP, 0, 0, BASIC_UDP_TTL, IPPROTO_UDP, FLAG_NOFRAG_MASK, &ipaddrs);
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
			if(read(clockFd,&junk,sizeof(junk))==-1) {
				fprintf(stderr,"Failed to read a timer event when sending the report. The next attempts may fail.\n");
			}
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
unsigned int runUDPserver_raw(struct lampsock_data sData, macaddr_t srcMAC, struct in_addr srcIP, struct options *opts) {
	arg_struct args;

	// Packet buffer with size = Ethernet MTU
	byte_t packet[RAW_RX_PACKET_BUF_SIZE];

	// recvfrom variables
	ssize_t rcv_bytes;
	size_t UDPpayloadsize; // UDP payload size

	struct pktheadersptr_udp headerptrs;
	byte_t *lampPacket=NULL;

	// RX and TX timestamp containers
	struct timeval rx_timestamp={.tv_sec=0,.tv_usec=0}, tx_timestamp={.tv_sec=0,.tv_usec=0};

	// Variable to store the latency (trip time)
	uint64_t tripTime;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx=0; // Initilized to 0 in order to be sure to enter the while loop
	uint16_t lamp_payloadlen_rx;
	lamptype_t lamp_type_tx; // Hardware tx timestamping only

	// LaMP fields for packet retrieved from socket error queue (hardware tx timestamping only)
	uint16_t lamp_seq_rx_errqueue=0;
	lamptype_t lamp_type_rx_errqueue;

	// Container for the source MAC address (read from packet)
	macaddr_t srcmacaddr_pkt=prepareMacAddrT();

	// Check if the MAC address was properly allocated
	if(macAddrTypeGet(srcmacaddr_pkt)==MAC_NULL) {
		return 1;
	}

	// while loop continue flag
	int continueFlag=1;

	// HARDWARE/SOFTWARE mode scm_timestamping structure
	struct scm_timestamping hw_ts;

	// SO_TIMESTAMP/SO_TIMESTAMPING variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBufKrt[CMSG_SPACE(sizeof(struct timeval))];
	char ctrlBufHwSw[CMSG_SPACE(sizeof(struct scm_timestamping))];

	// struct in_addr containing the destination IP address (read as source IP address from the packets coming from the client)
	// To be passed as argument to transmitReport()
	struct in_addr destIP_inaddr;

	// struct sockaddr_ll filled by recvfrom() and used to filter out outgoing traffic
	struct sockaddr_ll addrll;
	socklen_t addrllLen=sizeof(addrll);

	uint8_t isnotfirst_FU=0;
	uint16_t followup_reply_type;

	controlRCVdata fuData;

	// File descriptor for -W option (write per-packet data to CSV file)
	int Wfiledescriptor=-1;

	// Per-packet data structure (to be used when -W is selected)
	// The followup_on_flag can be already set here, together with tripTimeProc,
	// as there is no processing time estimation when working in unidirectional mode
	// (i.e. when working on the only mode in which the server can actually use -W)
	// Also the pointer to the report structure can be set here
	perPackerDataStructure perPktData;
	perPktData.followup_on_flag=0;
	perPktData.tripTimeProc=0;
	perPktData.enabled_extra_data=opts->report_extra_data;
	perPktData.reportDataPointer=&reportData;

	// timevalSub() return value (to check whether the result of a timeval subtraction is negative)
	int timevalSub_retval=0;

	// Flag managed internally by writeToReportSocket()
	uint8_t first_call=1;

	// Very important: initialize to 0 any flag that is used inside threads
	ack_report_received=0;
	followup_mode_session=FOLLOWUP_OFF;
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
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType, opts->followup_mode);

	// Populate the 'args' struct
	args.sData=sData;
	args.opts=opts;
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

	// Initialize the carbon report structure (only if the mode is unidirectional, as this is the case
	// in which the server manages the per-packet statistics)
	if(mode_session==UNIDIR) {
		// Initialize the Carbon report structure only if the -g option is used
		if(opts->carbon_sock_params.enabled) {
			if(openCarbonReportSocket(&carbonReportData,opts)<0) {
				fprintf(stderr,"Error: cannot open the socket for sending the data to Carbon/Graphite.\n");
				CLEAR_ALL()
				return 2;
			}

			carbonReportStructureInit(&carbonReportData);

			// This flag is used to understand when the first data is available, in order to start the metrics flush thread (see carbon_thread_manager.c)
			carbon_metrics_flush_first=1;
		}
	}
	
	// -L is ignored if a bidirectional INIT packet was received (it's the client that should compute the latency, not the server)
	if(mode_session!=UNIDIR && opts->latencyType!=USERTOUSER) {
		fprintf(stderr,"Warning: a latency type was specified (-L), but it will be ignored.\n");
	} else if(mode_session==UNIDIR && opts->latencyType==KRT) {
		// Set SO_TIMESTAMP
		// Check if the KRT mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_SW_RX)<0) {
		 	perror("socketSetTimestamping() error");
			fprintf(stderr,"Warning: SO_TIMESTAMP is probably not supported. Switching back to user-to-user latency.\n");
			opts->latencyType=USERTOUSER;
		}

		// Prepare ancillary data structures, if KRT mode is selected
		memset(&mhdr,0,sizeof(mhdr));

		// iovec buffers (scatter/gather arrays)
		iov.iov_base=packet;
		iov.iov_len=sizeof(packet);

		// Socket address structure
		mhdr.msg_name=&(addrll);
		mhdr.msg_namelen=addrllLen;

		// Ancillary data (control message)
		mhdr.msg_control=ctrlBufKrt;
	    mhdr.msg_controllen=sizeof(ctrlBufKrt);

	       // iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
	}

	// Open CSV file when '-W' is specified (as this only applies to the unidirectional mode, no file is create when the mode is not unidirectional)
	if(opts->Wfilename!=NULL && mode_session==UNIDIR) {
		Wfiledescriptor=openTfile(opts->Wfilename,opts->overwrite_W,opts->followup_mode!=FOLLOWUP_OFF,opts->report_extra_data);
		if(Wfiledescriptor<0) {
			fprintf(stderr,"Warning! Cannot open file for writing single packet latency data.\nThe '-W' option will be disabled.\n");
		}
	}

	// Already get all the packet pointers
	lampPacket=UDPgetpacketpointers(packet,&(headerptrs.etherHeader),&(headerptrs.ipHeader),&(headerptrs.udpHeader));
	lampGetPacketPointers(lampPacket,&(headerptrs.lampHeader));

	// From now on, 'payload' should -never- be used if (headerptrs.lampHeader)->payloadLen is 0

	// Start receiving packets
	while(continueFlag) {
		// If in KRT unidirectional mode or in HARDWARE/SOFTWARE mode (requested by the client through a follow-up control message, use recvmsg(), otherwise, use recvfrom())
		if((mode_session==UNIDIR && opts->latencyType==KRT) || followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN || followup_mode_session==FOLLOWUP_ON_KRN_RX) {
			saferecvmsg(rcv_bytes,sData.descriptor,&mhdr,NO_FLAGS);
		} else {
			saferecvfrom(rcv_bytes,sData.descriptor,packet,RAW_RX_PACKET_BUF_SIZE,NO_FLAGS,(struct sockaddr *)&addrll,&addrllLen);
		}

		if(followup_mode_session==FOLLOWUP_ON_APP) {
			gettimeofday(&rx_timestamp,NULL);
		}

		// Timeout or other recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				fprintf(stderr,"Timeout reached when receiving packets. Connection terminated.\n");

				if(mode_session==UNIDIR) {
					reportSetTimeoutOccurred(&reportData);
				}
				
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

		// Discard any (end)reply, ack, init, report or follow-up data, at the moment
		if(lamp_type_rx==PINGLIKE_REPLY || lamp_type_rx==PINGLIKE_REPLY_TLESS || lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==ACK || lamp_type_rx==REPORT || lamp_type_rx==INIT || lamp_type_rx==FOLLOWUP_DATA) {
			continue;
		}

		// Check whether the id is of interest or not
		if(lamp_id_rx!=lamp_id_session) {
			continue;
		}
		
		if((mode_session==UNIDIR && opts->latencyType==KRT) || followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN || followup_mode_session==FOLLOWUP_ON_KRN_RX) {
			for(cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
				// KRT (unidirectional) mode
                if((opts->latencyType==KRT || followup_mode_session==FOLLOWUP_ON_KRN_RX) && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMP) {
                    rx_timestamp=*((struct timeval *)CMSG_DATA(cmsg));
                }

                // HARDWARE or SOFTWARE (kernel tx+rx) mode (bidirectional/ping-like only)
               	if((followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN) && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMPING) {
					hw_ts=*((struct scm_timestamping *)CMSG_DATA(cmsg));
	    			rx_timestamp.tv_sec=hw_ts.ts[followup_mode_session==FOLLOWUP_ON_HW ? 2 : 0].tv_sec;
	 				rx_timestamp.tv_usec=hw_ts.ts[followup_mode_session==FOLLOWUP_ON_HW ? 2 : 0].tv_nsec/MICROSEC_TO_NANOSEC;
	           	}
			}
		}

		// Check if this is a follow-up request packet (after the first one, all the subseuent ones will be ignored)
		// Moreover, if a normal request packet is received, the session will go on without activating follow-ups
		// and future follow-up requests will be ignored (in order not to provide inconsistent and/or mixed data to the client)
		if(lamp_type_rx==FOLLOWUP_CTRL && IS_FOLLOWUP_REQUEST(lamp_payloadlen_rx) && isnotfirst_FU==0) {
			// Received a follow-up request
			if(opts->refuseFollowup) {
				// Just deny the request
				followup_reply_type=FOLLOWUP_DENY;
			} else {
				switch(lamp_payloadlen_rx) {
					case FOLLOWUP_REQUEST_T_APP:
						followup_reply_type=FOLLOWUP_ACCEPT; // There is now no reason to deny a request like this one
						followup_mode_session=FOLLOWUP_ON_APP;
						break;

					case FOLLOWUP_REQUEST_T_KRN_RX:
						if(socketSetTimestamping(sData,SET_TIMESTAMPING_SW_RX)<0) {
							followup_reply_type=FOLLOWUP_DENY;
						} else {
							// Prepare ancillary data structure
							followup_reply_type=FOLLOWUP_ACCEPT;
							followup_mode_session=FOLLOWUP_ON_KRN_RX;

							memset(&mhdr,0,sizeof(mhdr));

							iov.iov_base=packet;
							iov.iov_len=sizeof(packet);

							mhdr.msg_name=NULL;
							mhdr.msg_namelen=0;

							mhdr.msg_control=ctrlBufKrt;
							mhdr.msg_controllen=sizeof(ctrlBufKrt);

							mhdr.msg_iov=&iov;
							mhdr.msg_iovlen=1;

							mhdr.msg_flags=NO_FLAGS;
						}
						break;

					case FOLLOWUP_REQUEST_T_HW:
					case FOLLOWUP_REQUEST_T_KRN:
						if(socketSetTimestamping(sData,lamp_payloadlen_rx==FOLLOWUP_REQUEST_T_HW ? SET_TIMESTAMPING_HW : SET_TIMESTAMPING_SW_RXTX)<0) {
							followup_reply_type=FOLLOWUP_DENY;
						} else {
							// Prepare ancillary data structure
							followup_reply_type=FOLLOWUP_ACCEPT;
							followup_mode_session=lamp_payloadlen_rx==FOLLOWUP_REQUEST_T_HW ? FOLLOWUP_ON_HW : FOLLOWUP_ON_KRN;

							memset(&mhdr,0,sizeof(mhdr));

							iov.iov_base=packet;
							iov.iov_len=sizeof(packet);

							mhdr.msg_name=NULL;
							mhdr.msg_namelen=0;

							mhdr.msg_control=ctrlBufHwSw;
							mhdr.msg_controllen=sizeof(ctrlBufHwSw);

							mhdr.msg_iov=&iov;
							mhdr.msg_iovlen=1;

							mhdr.msg_flags=NO_FLAGS;
						}
						break;

					default:
						followup_reply_type=FOLLOWUP_UNKNOWN;
						break;
				}
			}

			fuData.controlRCV.ip.s_addr=headerptrs.ipHeader->saddr;
			fuData.controlRCV.port=client_port_session;
			fuData.controlRCV.session_id=lamp_id_session;
			memcpy(fuData.controlRCV.mac,srcmacaddr_pkt,ETHER_ADDR_LEN);

			// Send follow-up reply
			controlSenderUDP_RAW(&args,&fuData,lamp_id_session,1,FOLLOWUP_CTRL,followup_reply_type,0,NULL,NULL);

			isnotfirst_FU=1;

			continue;
		}

		if(isnotfirst_FU==0) {
			isnotfirst_FU=1;
		}

		if(lamp_type_rx==FOLLOWUP_CTRL) {
			// In a normal networking situation (i.e. with no packets out of order as the session is started) we should never reach this point
			fprintf(stdout,"Ignoring a follow-up request from " PRI_MAC " (id=%u)..\n",
					MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx);

			continue;
		}

		// If the packet is marked as last packet, set the continue flag to 0 for exiting the loop
		if(lamp_type_rx==UNIDIR_STOP || lamp_type_rx==PINGLIKE_ENDREQ || lamp_type_rx==PINGLIKE_ENDREQ_TLESS) {
			continueFlag=0;
		}

		switch(mode_session) {
			case UNIDIR:
				if(opts->latencyType==USERTOUSER) {
					gettimeofday(&rx_timestamp,NULL);
				}

				timevalSub_retval=timevalSub(&tx_timestamp,&rx_timestamp);
				if(timevalSub_retval) {
					fprintf(stderr,"Error: negative latency (-%.3f ms - %s) for packet from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d)!\nThe clock synchronization is not sufficienty precise to allow unidirectional measurements.\n",
						(double) (rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec)/1000,latencyTypePrinter(opts->latencyType),
						MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
					tripTime=0;
				} else {
					tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
				}

				if(tripTime!=0) {
					fprintf(stdout,"Received a unidirectional message from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
						MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(opts->latencyType));
				}

				// Update the current report structure
				reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);

				// When '-W' is specified, write the current measured value to the specified CSV file too (if a file was successfully opened)
				if(Wfiledescriptor>0 || opts->udp_params.enabled) {
					perPktData.seqNo=lamp_seq_rx;
					perPktData.signedTripTime=timevalSub_retval==0 ? rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec : -(rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec);
					perPktData.tx_timestamp=tx_timestamp;

					if(Wfiledescriptor>0) {
						writeToTFile(Wfiledescriptor,W_DECIMAL_DIGITS,&perPktData);
					}

					if(opts->udp_params.enabled) {
						writeToReportSocket(&(sData.sock_w_data),W_DECIMAL_DIGITS,&perPktData,lamp_id_session,&first_call);
					}
				}

				// When -g is specified, update the Carbon/Graphite report structure
				// If this is the first time this point is reached, also start the metrics flush thread
				if(opts->carbon_sock_params.enabled) {
					if(carbon_metrics_flush_first==1) {
						carbon_metrics_flush_first=0;
						if(startCarbonTimedThread(&ctd,&carbonReportData,opts)<0) {
							t_rx_error=ERR_CARBON_THREAD;
							break;
						}
					}

					carbon_pthread_mutex_lock(ctd);
					carbonReportStructureUpdate(&carbonReportData,tripTime);
					carbon_pthread_mutex_unlock(ctd);
				}
			break;

			case PINGLIKE:
				// Transmit the reply as the copy of the request, with the some modified fields, which can be modified from packet
				// thanks to the pointers to the various headers, without having to un-encapsulate everything or to re-encapsulate the
				// various headers to build a separate packet. This -should- maintain a better efficiency...
				// ...but is bypassing a bit the "rawsock" library, directly resorting to 'memcpy's... hope it is not so unreadable!
				// At the next received packet, the memory area pointed by 'packet' should be substituted by the newly received packet,
				// thus allowing in some way this mechanism of directly replacing bytes inside the received data.

				// Print here that a packet was received as default behaviour
				if(!opts->printAfter) {
					fprintf(stdout,"Received a ping-like message from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d). Replying to client...\n",
						MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
				}

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
				
				// Set REPLY/REPLY_TLESS as LaMP type (or ENDREPLY/ENDREPLY_TLESS if this is the last packet)
				if(lamp_type_rx==PINGLIKE_REQ || lamp_type_rx==PINGLIKE_ENDREQ) {
					headerptrs.lampHeader->ctrl = continueFlag==1 ? CTRL_PINGLIKE_REPLY : CTRL_PINGLIKE_ENDREPLY;
				} else if(lamp_type_rx==PINGLIKE_REQ_TLESS || lamp_type_rx==PINGLIKE_ENDREQ_TLESS) {
					headerptrs.lampHeader->ctrl = continueFlag==1 ? CTRL_PINGLIKE_REPLY_TLESS : CTRL_PINGLIKE_ENDREPLY_TLESS;
				}

				lamp_type_tx=CTRL_TO_TYPE(headerptrs.lampHeader->ctrl);

				// Recompute IP checksum
				headerptrs.ipHeader->check=0;
				headerptrs.ipHeader->check=ip_fast_csum((__u8 *)headerptrs.ipHeader, headerptrs.ipHeader->ihl);

				// If using application level or kernel level RX follow-up mode, gather the tx timestamp just before sending the packet
				if(followup_mode_session==FOLLOWUP_ON_APP || followup_mode_session==FOLLOWUP_ON_KRN_RX) {
					gettimeofday(&tx_timestamp,NULL);
				}

				// Send packet (as the reply does require to carry the client timestamp, the control field should now correspond to CTRL_PINGLIKE_REPLY)
				// 'rcv_bytes' still stores the packet size, thus it can be used as packet size to be passed to rawLampSend(), wich will in turn call sendto() with that size
				// rawLampSend should also take care of re-computing the checksum, which is changed due to the different fields in the reply packet.
				if(rawLampSend(sData.descriptor, sData.addru.addrll, headerptrs.lampHeader, packet, rcv_bytes, FLG_NONE, UDP)) {
					fprintf(stderr,"UDP server reported that it can't reply to the client with id=%u and seq=%u\n",lamp_id_rx,lamp_seq_rx);
				}

				// Print here that a packet was received if -1 was specified
				if(opts->printAfter) {
					fprintf(stdout,"Received a ping-like message from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d). Reply sent to client...\n",
						MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
				}

				// If in hardware timestamping or software (kernel) follow-up mode, gather the tx_timestamp from ancillary data
				if(followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN) {
					// Loop until the right transmitted packet is received (as other packets are sent too, for which we are not
					//  interested in obtaining any tx timestamp - e.g. all the follow-up data tx timestamps are useless in our case)
					do {
						if(pollErrqueueWait(sData.descriptor,POLL_ERRQUEUE_WAIT_TIMEOUT)<=0) {
							rcv_bytes=-1;
							break;
						}
						saferecvmsg(rcv_bytes,sData.descriptor,&mhdr,MSG_ERRQUEUE);
						lampHeadGetData(lampPacket,&lamp_type_rx_errqueue,NULL,&lamp_seq_rx_errqueue,NULL,NULL,NULL);
					} while(lamp_seq_rx_errqueue!=lamp_seq_rx || lamp_type_rx_errqueue!=lamp_type_tx);

					if(rcv_bytes==-1) {
						t_rx_error=ERR_TXSTAMP;
						break;
					}

					for(cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
			           	if(cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMPING) {
			            	hw_ts=*((struct scm_timestamping *)CMSG_DATA(cmsg));
			             	tx_timestamp.tv_sec=hw_ts.ts[followup_mode_session==FOLLOWUP_ON_HW ? 2 : 0].tv_sec;
			       			tx_timestamp.tv_usec=hw_ts.ts[followup_mode_session==FOLLOWUP_ON_HW ? 2 : 0].tv_nsec/MICROSEC_TO_NANOSEC;
			           	}
					}
				}

				// If follow-up mode is active, send the follow-up data packet
				if(followup_mode_session!=FOLLOWUP_OFF) {
					// Compute the difference between the rx and tx timestamps (difference stored in tx_timestamp, i.e. the "out" argument of timevalSub())
					// This is done since normally tx_timestamp > rx_timestamp
					if(timevalSub(&rx_timestamp,&tx_timestamp)) {
						fprintf(stderr,"Error: negative time!\nCannot compute follow-up processing time for the current packet (id=%u, seq=%u).\n",lamp_id_rx,lamp_seq_rx);
						tx_timestamp.tv_sec=0;
						tx_timestamp.tv_usec=0;
					} else {
						fprintf(stdout,"Sending follow-up data. Processing delta: %.3f ms.\n",((double) tx_timestamp.tv_sec)*SEC_TO_MILLISEC+((double) tx_timestamp.tv_usec)/MICROSEC_TO_MILLISEC);
					}
					
					// Send follow-up with the time difference timestamp (fuData should be already filled with all the proper data)
					if(sendFollowUpData_RAW(&args,&fuData,lamp_id_rx,headerptrs.ipHeader->id,lamp_seq_rx,tx_timestamp)) {
						perror("sendto() for sending LaMP follow-up data failed");
						fprintf(stderr,"UDP server reported that it can't reply to the client with id=%u and seq=%u (follow-up)\n",lamp_id_rx,lamp_seq_rx);
					}
				}
			break;

			default:
			break;
		}
	}

	if(mode_session==UNIDIR) {
		// Terminate the carbon flush thread
		// carbon_metrics_flush_first is checked in order to verify if the thread has been created or not
		// If the thread has been created, the carbon_metrics_flush_first flag should have been set to 0
		if(opts->carbon_sock_params.enabled && carbon_metrics_flush_first==0) {
			stopCarbonTimedThread(&ctd);
		}

		if(Wfiledescriptor>0) {
			closeTfile(Wfiledescriptor);
		}

		destIP_inaddr.s_addr=headerptrs.ipHeader->saddr;
		// If the mode is the unidirectional one, get the destination IP/MAC from the last packet
		// Use as destination IP (destIP), the source IP of the last received packet (headerptrs.ipHeader->saddr)
		if(transmitReport(sData, opts, destIP_inaddr, srcIP, srcMAC, srcmacaddr_pkt)) {
			fprintf(stderr,"UDP server reported an error while transmitting the report.\n"
				"No report will be transmitted.\n");
			CLEAR_ALL();
			return 4;
		}

		if(opts->udp_params.enabled) {
			// If '-w' was specified and the mode is undirectional, send a LateEND empty packet to make the 
			// receiving application stop reading the data; this empty packet is triggered by setting the 
			// second argument (report) to NULL
			printStatsSocket(opts,NULL,&(sData.sock_w_data),lamp_id_session);
		}

		if(opts->carbon_sock_params.enabled) {
			// If '-g' was specified and the mode is unidirectional, close the previously opened socket
			// for flushing metrics to Carbon
			closeCarbonReportSocket(&carbonReportData);
		}
	}

	// Destroy mutex (as it is no longer needed) and clear all the other data that should be clared (see the CLEAR_ALL() macro)
	CLEAR_ALL();

	return 0;
}