#include "udp_client_raw.h"
#include "packet_structs.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "rawsock_lamp.h"
#include "report_manager.h"
#include <inttypes.h>
#include <errno.h>
#include <linux/errqueue.h>
#include "timeval_utils.h"
#include "carbon_thread_manager.h"
#include "common_thread.h"
#include "timer_man.h"
#include "common_udp.h"

// Local global variables
static pthread_t txLoop_tid, rxLoop_tid, ackListenerInit_tid, initSender_tid, followupReplyListener_tid, followupRequestSender_tid;
static uint16_t lamp_id_session;
static reportStructure reportData;
static carbonReportStructure carbonReportData;
static int carbon_metrics_flush_first;
static carbon_pthread_data_t ctd;

// Transmit error container
static t_error_types t_tx_error=NO_ERR;
// Receive error container
static t_error_types t_rx_error=NO_ERR;

// Data structure to store tx timestamps for HARDWARE/SOFTWARE mode
// When in HARDWARE/SOFTWARE mode, a structure to store the tx timestamps is needed
// Using timevalStoreList, as defined in timeval_utils.h
// This structure will be allocated only if HARDWARE/SOFTWARE mode is properly supported 
timevalStoreList tslist;

// The same applies for the following list, used to store temporary trip times (as timestamp differences)
// when waiting for the server follow-ups, containing an estimate on the time needed
// to process each client request and send the reply down to the hardware
// This list is allocated only when the follow-up mode is active
timevalStoreList triptimelist;

// Mutex to protect tslist, as defined before
static pthread_mutex_t tslist_mut=PTHREAD_MUTEX_INITIALIZER;

static uint8_t ack_init_received=0; // Global flag set by the ackListener thread: = 1 when an ACK has been received, otherwise it is = 0
static pthread_mutex_t ack_init_received_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the ack_received variable (as it written by a thread and read by another one)
static uint8_t followup_reply_received=0; // Global flag set by the followupReplyListener thread: = 1 when a reply has been received, otherwise it is = 0
static pthread_mutex_t followup_reply_received_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the followup_reply_received variable

#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
	static _Atomic int rx_loop_timeout_error=0; // If C11 atomic variables are supported, just define an atomic integer variable
#else
	static uint8_t rx_loop_timeout_error=0; // Global flag which is set to 1 by the rx loop, in bidirectional mode, when a timeout occurs, to stop also the tx loop
	static pthread_mutex_t rx_loop_timeout_error_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the rx_loop_timeout_error_mut variable
#endif

extern inline int timevalSub(struct timeval *in, struct timeval *out);

// Function prototypes
static void txLoop(arg_struct *args);
static void unidirRxTxLoop(arg_struct *args);
static void rawBuffersCleanup(struct pktbuffers_udp buffs);

// Thread entry point function prototypes
static void *txLoop_t (void *arg);
static void *rxLoop_t (void *arg);
static void *ackListenerInit (void *arg);
static void *initSender (void *arg);

static void *followupReplyListener (void *arg);
static void *followupRequestSender (void *arg);

static void *ackListenerInit (void *arg) {
	arg_struct *args=(arg_struct *) arg;
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
	arg_struct *args=(arg_struct *) arg;
	int return_value;
	controlRCVdata initData;

	initData.controlRCV.ip=args->opts->dest_addr_u.destIPaddr;
	initData.controlRCV.port=CLIENT_SRCPORT;
	initData.controlRCV.session_id=lamp_id_session;
	memcpy(initData.controlRCV.mac,args->opts->destmacaddr,ETHER_ADDR_LEN);

	return_value=controlSenderUDP_RAW(args,&initData,lamp_id_session,INIT_RETRY_MAX_ATTEMPTS, INIT, 0, INIT_RETRY_INTERVAL_MS, &ack_init_received, &ack_init_received_mut);
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

static void *followupReplyListener (void *arg) {
	arg_struct_followup_listener_raw_ip *ful_arg=(arg_struct_followup_listener_raw_ip *) arg;
	controlRCVdata rcvData;
	int return_value;

	rcvData.session_id=lamp_id_session;

	return_value=controlReceiverUDP_RAW(ful_arg->sFd,CLIENT_SRCPORT,ful_arg->srcIP.s_addr,&rcvData,FOLLOWUP_CTRL,&followup_reply_received,&followup_reply_received_mut);

	if(return_value<0) {
		if(return_value==-1) {
			t_rx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(return_value==-2) {
			t_rx_error=ERR_TIMEOUT_FOLLOWUP;
		} else if(return_value==-3) {
			t_rx_error=ERR_RECVFROM_GENERIC;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}
	} else {
		// If everything went fine, save the type of reply which was received (ACCEPT or DENY)
		ful_arg->responseType=rcvData.controlRCV.type_idx;
	}

	pthread_exit(NULL);
}

static void *followupRequestSender (void *arg) {
	arg_struct *args=(arg_struct *) arg;
	int return_value;
	uint16_t followup_req_type;
	controlRCVdata fuData;

	// Set follow-up request type
	switch(args->opts->followup_mode) {
		case FOLLOWUP_ON_APP:
			followup_req_type=FOLLOWUP_REQUEST_T_APP;
			break;

		case FOLLOWUP_ON_KRN_RX:
			followup_req_type=FOLLOWUP_REQUEST_T_KRN_RX;
			break;

		case FOLLOWUP_ON_KRN:
			followup_req_type=FOLLOWUP_REQUEST_T_KRN;
			break;

		case FOLLOWUP_ON_HW:
			followup_req_type=FOLLOWUP_REQUEST_T_HW;
			break;

		default:
			t_tx_error=ERR_SEND_FOLLOWUP;
		break;
	}

	if(t_tx_error!=ERR_SEND_FOLLOWUP) {
		fuData.controlRCV.ip=args->opts->dest_addr_u.destIPaddr;
		fuData.controlRCV.port=CLIENT_SRCPORT;
		fuData.controlRCV.session_id=lamp_id_session;
		memcpy(fuData.controlRCV.mac,args->opts->destmacaddr,ETHER_ADDR_LEN);

		return_value=controlSenderUDP_RAW(args,&fuData,lamp_id_session,FOLLOWUP_CTRL_RETRY_MAX_ATTEMPTS,FOLLOWUP_CTRL,followup_req_type,FOLLOWUP_CTRL_RETRY_INTERVAL_MS,&followup_reply_received,&followup_reply_received_mut);

		if(return_value<0) {
			if(return_value==-1) {
				t_tx_error=ERR_INVALID_ARG_CMONUDP;
			} else if(return_value==-2) {
				t_tx_error=ERR_SEND_FOLLOWUP;
			} else {
				t_rx_error=ERR_UNKNOWN;
			}
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
	struct pollfd timerMon[2];
	int pollfd_size;
	int clockFd;
	int timerCaS_res=0;

	// Test duration specific timer variables
	int durationClockFd;
	int sendLast=0;

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Payload buffer
	byte_t *payload_buff=NULL;

	// while loop counters
	unsigned int counter=0;
	unsigned int batch_counter=0;

	// Final packet size
	size_t finalpktsize;

	// LaMP packet type and end_flag
	uint8_t ctrl=CTRL_PINGLIKE_REQ;
	endflag_t end_flag;
	uint32_t lampPacketSize=0;

	// SO_TIMESTAMPING variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBuf[CMSG_SPACE(sizeof(struct scm_timestamping))];

	// iov_base buffer pointer
	byte_t *data_iov=NULL;

	// LaMP packet pointer (from data_iov, to be used in HARDWARE/SOFTWARE mode only)
	byte_t *lampPacketRxPtr=NULL;

	// LaMP fields for packet retrieved from socket error queue (hardware tx timestamping only)
	uint16_t lamp_seq_rx_errqueue=0;
	lamptype_t lamp_type_rx_errqueue;

	// recvfrom variable (for HARDWARE/SOFTWARE mode only)
	ssize_t rcv_bytes;

	// struct scm_timestamping and struct timeval for the tx timestamp
	struct scm_timestamping hw_ts;
	struct timeval tx_timestamp;

	// Populating headers
	// [IMPROVEMENT] Future improvement: get destination MAC through ARP or broadcasted information and not specified by the user
	etherheadPopulate(&(headers.etherHeader), args->srcMAC, args->opts->destmacaddr, ETHERTYPE_IP);
	IP4headPopulateS(&(headers.ipHeader), args->sData.devname, args->opts->dest_addr_u.destIPaddr, 0, 0, BASIC_UDP_TTL, IPPROTO_UDP, FLAG_NOFRAG_MASK, &ipaddrs);
	UDPheadPopulate(&(headers.udpHeader), CLIENT_SRCPORT, args->opts->port);
	if(args->opts->mode_ub==PINGLIKE) {
		if(args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
			ctrl=CTRL_PINGLIKE_REQ_TLESS;
		} else {
			ctrl=CTRL_PINGLIKE_REQ;
		}
	} else if(args->opts->mode_ub==UNIDIR) {
		if(args->opts->number==1) {
			ctrl=CTRL_UNIDIR_STOP;
		} else {
			ctrl=CTRL_UNIDIR_CONTINUE;
		}
	}
	lampHeadPopulate(&(headers.lampHeader), ctrl, lamp_id_session, INITIAL_SEQ_NO); // Starting from sequence number = 0

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
		rawBuffersCleanup(buffers);
		t_tx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	buffers.ippacket=malloc(IP_UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.ippacket) {
		rawBuffersCleanup(buffers);
		t_tx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	buffers.ethernetpacket=malloc(ETH_IP_UDP_PACKET_SIZE_S(lampPacketSize));
	if(!buffers.ethernetpacket) {
		rawBuffersCleanup(buffers);
		t_tx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	memset(&mhdr,0,sizeof(mhdr));
	// Prepare ancillary data structures, if HARDWARE/SOFTWARE mode is selected (to get send timestamps)
	if(args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
		data_iov=malloc((ETH_IP_UDP_PACKET_SIZE_S(sizeof(struct lamphdr)+args->opts->payloadlen))*sizeof(byte_t));

		if(!data_iov) {
			rawBuffersCleanup(buffers);
			t_tx_error=ERR_MALLOC;
			pthread_exit(NULL);
		}

		// iovec buffers (scatter/gather arrays) 
		iov.iov_base=(void *)data_iov;
		iov.iov_len=(ETH_IP_UDP_PACKET_SIZE_S(sizeof(struct lamphdr)+args->opts->payloadlen))*sizeof(byte_t); // Macro from Rawsock library

		// Socket address structure (not needed here)
		mhdr.msg_name=NULL;
		mhdr.msg_namelen=0;

		// Ancillary data (control message)
		mhdr.msg_control=ctrlBuf;
        mhdr.msg_controllen=sizeof(ctrlBuf);

        // iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
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
			payload_buff[i]=(byte_t) (i & 15);
		}
	}

	// Get "in packet" LaMP header pointer
	inpacket_lamphdr=(struct lamphdr *) (buffers.ethernetpacket+sizeof(struct ether_header)+sizeof(struct iphdr)+sizeof(struct udphdr));

	// Set end_flag to FLG_CONTINUE
	end_flag=FLG_CONTINUE;

	// Create and start timer
	timerCaS_res=timerCreateAndSet(&timerMon[0], &clockFd, args->opts->interval);

	if(timerCaS_res==-1) {
		t_tx_error=ERR_TIMERCREATE;
		pthread_exit(NULL);
	} else if(timerCaS_res==-2) {
		t_tx_error=ERR_SETTIMER;
		pthread_exit(NULL);
	}

	// Create and start test duration timer (if required only - i.e. if the user specified -i)
	if(args->opts->duration_interval>0 || args->opts->seconds_to_end!=-1) {
		pollfd_size=2; // poll() will monitor two descriptors

		if(args->opts->seconds_to_end!=-1) {
			setTestDurationEndTime(args->opts);
		}
		timerCaS_res=timerCreateAndSet(&timerMon[1],&durationClockFd,args->opts->duration_interval*1000);

		if(timerCaS_res==-1) {
			t_tx_error=ERR_TIMERCREATE;
			close(clockFd);
			pthread_exit(NULL);
		} else if(timerCaS_res==-2) {
			t_tx_error=ERR_SETTIMER;
			close(clockFd);
			pthread_exit(NULL);
		}
	} else {
		pollfd_size=1; // poll() will monitor one descriptor only (clockFd)
	}

	if(args->opts->duration_interval>0) {
		args->opts->number=UINT64_MAX;
	}

	// Run until 'number' is reached or until the time specified with -i elapses
	while(counter<args->opts->number && sendLast==0) {
		// Stop the loop if the rx loop has reported a timeout
		#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
			if(rx_loop_timeout_error==1) {
				break;
			}
		#else
			pthread_mutex_lock(&rx_loop_timeout_error_mut);
			if(rx_loop_timeout_error==1) {
				break;
			}
			pthread_mutex_unlock(&rx_loop_timeout_error_mut);
		#endif

		// poll waiting for events happening on the timer descriptor (i.e. wait for timer expiration)
		if(poll(timerMon,pollfd_size,INDEFINITE_BLOCK)>0) {
			// If the duration timer expired, send now the last packet and set sendLast to 1 to terminate the current test
			if(args->opts->duration_interval!=0 && timerMon[1].revents>0) {
				if(read(durationClockFd,&junk,sizeof(junk))==-1) {
					t_tx_error=ERR_CLEAR_TIMER_EVENT;
					break;
				}

				timerStop(&durationClockFd);
				sendLast=1;
			}

			// "Clear the event" by performing a read() on a junk variable
			if(timerMon[0].revents>0 && read(clockFd,&junk,sizeof(junk))==-1) {
				t_tx_error=ERR_CLEAR_TIMER_EVENT;
				break;
			}

			// Rearm timer with a random timeout if '-R' was specified
			if(sendLast!=1 && args->opts->rand_type!=NON_RAND && batch_counter==args->opts->rand_batch_size) {
				if(timerRearmRandom(clockFd,args->opts)<0) {
					t_tx_error=ERR_RANDSETTIMER;
					pthread_exit(NULL);
				}
				batch_counter=0;
			}
			
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

			// Set end flag to FLG_STOP when it is time to send the last packet
			if(counter==(args->opts->number-1) || sendLast==1) {
					end_flag=FLG_STOP;
			}

			if(args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
				pthread_mutex_lock(&tslist_mut);
			}

			if(rawLampSend(args->sData.descriptor, args->sData.addru.addrll, inpacket_lamphdr, buffers.ethernetpacket, finalpktsize, end_flag, UDP)) {
				if(errno==EMSGSIZE) {
					fprintf(stderr,"Error: EMSGSIZE 90 Message too long.\n");
				}
				fprintf(stderr,"Failed sending latency measurement packet with seq: %u.\nThe execution will terminate now.\n",headers.lampHeader.seq);
				break;
			}

			// Retrieve tx timestamp if mode is HARDWARE/SOFTWARE
			// Extract ancillary data with the tx timestamp (if mode is HARDWARE/SOFTWARE)
			if(args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
				do {
					if(pollErrqueueWait(args->sData.descriptor,POLL_ERRQUEUE_WAIT_TIMEOUT)<=0) {
						rcv_bytes=-1;
						pthread_mutex_unlock(&tslist_mut);
						break;
					}
					saferecvmsg(rcv_bytes,args->sData.descriptor,&mhdr,MSG_ERRQUEUE);
					lampPacketRxPtr=UDPgetpacketpointers(data_iov,NULL,NULL,NULL); // From Rawsock library
					lampHeadGetData(lampPacketRxPtr,&lamp_type_rx_errqueue,NULL,&lamp_seq_rx_errqueue,NULL,NULL,NULL);
				} while(lamp_seq_rx_errqueue!=counter || (lamp_type_rx_errqueue!=PINGLIKE_REQ_TLESS && lamp_type_rx_errqueue!=PINGLIKE_ENDREQ_TLESS));

				if(rcv_bytes==-1) {
					t_rx_error=ERR_TXSTAMP;
					pthread_mutex_unlock(&tslist_mut);
					break;
				}

				for(cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
		           	if(cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMPING) {
		            	hw_ts=*((struct scm_timestamping *)CMSG_DATA(cmsg));
		             	tx_timestamp.tv_sec=hw_ts.ts[args->opts->latencyType==HARDWARE ? 2 : 0].tv_sec;
		       			tx_timestamp.tv_usec=hw_ts.ts[args->opts->latencyType==HARDWARE ? 2 : 0].tv_nsec/MICROSEC_TO_NANOSEC;
		           	}
				}

				// Save tx timestamp
				timevalSL_insert(tslist,counter,tx_timestamp);
				pthread_mutex_unlock(&tslist_mut);
			}

			// Increase sequence number for the next iteration
			lampHeadIncreaseSeq(&(headers.lampHeader));

			// Increase counter
			counter++;
			if(args->opts->rand_type!=NON_RAND) batch_counter++;
		}
	}

	// Free payload buffer
	if(args->opts->payloadlen!=0) {
		free(payload_buff);
	}

	// Set the report's total packets value to the amount of packets sent during this test, if -i was used
	if(args->opts->duration_interval!=0) {
		reportStructureChangeTotalPackets(&reportData,counter);
	}

	// Close timer file descriptor
	close(clockFd);

	// Free all buffers before exiting
	// Free the LaMP packet buffer only if it was allocated (otherwise a SIGSEGV may occur if payloadLen is 0)
	if(buffers.lamppacket) free(buffers.lamppacket);
	if(data_iov) free(data_iov);
	free(buffers.udppacket);
	free(buffers.ippacket);
	free(buffers.ethernetpacket);
}

static void *rxLoop_t (void *arg) {
	arg_struct *args=(arg_struct *) arg;

	int Wfiledescriptor=-1;

	// Packet buffer with size = Ethernet MTU
	byte_t packet[RAW_RX_PACKET_BUF_SIZE];

	// recvfrom variables
	ssize_t rcv_bytes;
	size_t UDPpayloadsize; // UDP payload size

	struct pktheadersptr_udp headerptrs;
	byte_t *lampPacket=NULL;

	// RX and TX timestamp containers (plus follow-up and trip time timestamps for the HARDWARE/SOFTWARE mode)
	struct timeval rx_timestamp, tx_timestamp, triptime_timestamp, packet_timestamp;
	struct scm_timestamping hw_ts;

	// Variable to store the latency (trip time)
	uint64_t tripTime=0;
	// Variable to store the processing time (server time delta) when using follow-up mode
	uint64_t tripTimeProc=0;

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

	// Ancillary data buffers
	char ctrlBufKrt[CMSG_SPACE(sizeof(struct timeval))];
	char ctrlBufHwSw[CMSG_SPACE(sizeof(struct scm_timestamping))];

	// Per-packet data structure (to be used when -W is selected)
	// The followup_on_flag can be already set here, just like the pointer to the report structure
	perPackerDataStructure perPktData;
	perPktData.followup_on_flag=args->opts->followup_mode!=FOLLOWUP_OFF;
	perPktData.enabled_extra_data=args->opts->report_extra_data;
	perPktData.reportDataPointer=&reportData;

	// timevalStoreList to store the tx_timestamp values when -W is selected and follow-up mode is active
	// A better explanation of this can be found in the comments inside udp_client.c
	timevalStoreList txstampslist=NULL_SL;

	int fu_flag=1; // Flag set to 0 when a follow-up is received after an ENDREPLY or ENDREPLY_TLESS (SOFTWARE or HARDWARE latencyType only, fixed to 0 for other types)
	int continueFlag=1; // Flag set to 0 when an ENDREPLY or ENDREPLY_TLESS is received
	int errorTsFlag=0; // Flag set to 1 when an error occurred in retrieving a timestamp (i.e. if no latency data can be reported for the current packet)

	// Flag managed internally by writeToReportSocket()
	uint8_t first_call=1;

	// Container for the source MAC address (read from packet)
	macaddr_t srcmacaddr_pkt=prepareMacAddrT();

	// Check if the MAC address was properly allocated
	if(macAddrTypeGet(srcmacaddr_pkt)==MAC_NULL) {
		t_rx_error=ERR_MALLOC;
		pthread_exit(NULL);
	}

	// Set fu_flag to 0 if follow-up mode is disabled
	if(args->opts->followup_mode==FOLLOWUP_OFF) {
		fu_flag=0;
	}

	// Prepare ancillary data structures, if KRT or HARDWARE or SOFTWARE mode is selected
	memset(&mhdr,0,sizeof(mhdr));
	if(args->opts->latencyType==KRT || args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
		// iovec buffers (scatter/gather arrays)
		iov.iov_base=packet;
		iov.iov_len=sizeof(packet);

		// Socket address structure
		mhdr.msg_name=NULL;
		mhdr.msg_namelen=0;

		// Ancillary data (control message)
		mhdr.msg_control=args->opts->latencyType!=KRT ? ctrlBufHwSw : ctrlBufKrt;
        mhdr.msg_controllen=args->opts->latencyType!=KRT ? sizeof(ctrlBufHwSw) : sizeof(ctrlBufKrt);

        // iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
	}

	// Initialize txstampslist (if follow-up mode is enabled)
	if(args->opts->Wfilename!=NULL && args->opts->followup_mode!=FOLLOWUP_OFF) {
		txstampslist=timevalSL_init();

		if(CHECK_SL_NULL(txstampslist)) {
			fprintf(stderr,"Warning: unable to allocate/initialize memory for saving per-packer tx timestamps to a CSV file.\nThe -W option will be disabled.\n");
			args->opts->Wfilename=NULL;
		}
	}

	// Open CSV file when in "-W" mode (i.e. "write every packet measurement data to CSV file")
	if(args->opts->Wfilename!=NULL) {
		Wfiledescriptor=openTfile(args->opts->Wfilename,args->opts->overwrite_W,args->opts->followup_mode!=FOLLOWUP_OFF,args->opts->report_extra_data);
		if(Wfiledescriptor<0) {
			fprintf(stderr,"Warning! Cannot open file for writing single packet latency data.\nThe '-W' option will be disabled.\n");
		}
	}

	// Already get all the packet pointers
	lampPacket=UDPgetpacketpointers(packet,&(headerptrs.etherHeader),&(headerptrs.ipHeader),&(headerptrs.udpHeader));
	lampGetPacketPointers(lampPacket,&(headerptrs.lampHeader));

	// From now on, 'payload' should -never- be used if (headerptrs.lampHeader)->payloadLen is 0

	// Start receiving packets until an 'ENDREPLY' one is received (this is the ping-like loop)
	do {
		// If in KRT mode or HARDWARE/SOFTWARE mode, use (the safe version of) recvmsg(), otherwise, use recvfrom()
		if(args->opts->latencyType==KRT || args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
			saferecvmsg(rcv_bytes,args->sData.descriptor,&mhdr,NO_FLAGS);
		} else {
			saferecvfrom(rcv_bytes,args->sData.descriptor,packet,RAW_RX_PACKET_BUF_SIZE,NO_FLAGS,(struct sockaddr *)&addrll,&addrllLen);
		}

		// Timeout or other recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				t_rx_error=ERR_TIMEOUT;
				fprintf(stderr,"Timeout when waiting for new packets.\n");

				// Signal to the tx loop that a timeout error occurred
				#if (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__))
					rx_loop_timeout_error=1;
				#else
					pthread_mutex_lock(&rx_loop_timeout_error_mut);
					rx_loop_timeout_error=1;
					pthread_mutex_unlock(&rx_loop_timeout_error_mut);
				#endif

				reportSetTimeoutOccurred(&reportData);
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
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, &packet_timestamp, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_id_rx!=lamp_id_session) {
			continue;
		}

		if(lamp_type_rx!=PINGLIKE_REPLY && lamp_type_rx!=PINGLIKE_ENDREPLY && lamp_type_rx!=PINGLIKE_REPLY_TLESS && lamp_type_rx!=PINGLIKE_ENDREPLY_TLESS) {
			if(args->opts->followup_mode!=FOLLOWUP_OFF && lamp_type_rx!=FOLLOWUP_DATA) {
				continue;
			}
		}

		if(lamp_type_rx==PINGLIKE_REPLY || lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==PINGLIKE_REPLY_TLESS || lamp_type_rx==PINGLIKE_ENDREPLY_TLESS) {
			// Extract ancillary data (if mode is KRT or if it is HARDWARE or SOFTWARE)
			if(args->opts->latencyType==KRT || args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
				for(cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
	                if(args->opts->latencyType==KRT && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMP) {
	                    rx_timestamp=*((struct timeval *)CMSG_DATA(cmsg));
	                }

	               	if((args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMPING) {
	                    hw_ts=*((struct scm_timestamping *)CMSG_DATA(cmsg));
	                    rx_timestamp.tv_sec=hw_ts.ts[args->opts->latencyType==HARDWARE ? 2 : 0].tv_sec;
	                    rx_timestamp.tv_usec=hw_ts.ts[args->opts->latencyType==HARDWARE ? 2 : 0].tv_nsec/MICROSEC_TO_NANOSEC;
	                }
				}
			} else if(args->opts->latencyType==USERTOUSER) {
				gettimeofday(&rx_timestamp,NULL);
			}

			if(args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
				pthread_mutex_lock(&tslist_mut);
				if(timevalSL_gather(tslist,lamp_seq_rx,&tx_timestamp)) {
					fprintf(stderr,"Error: could not retrieve transmit timestamp for packet number: %d.\n",lamp_seq_rx);
					errorTsFlag=1;
				}
				pthread_mutex_unlock(&tslist_mut);
			} else {
				tx_timestamp=packet_timestamp;
			}

			// Store tx_timestamp inside txstampslist, when -W is used together with -F
			if(Wfiledescriptor>0 && args->opts->followup_mode!=FOLLOWUP_OFF) {
				if(errorTsFlag==1) {
					tx_timestamp.tv_sec=0;
					tx_timestamp.tv_usec=0;
				}
				timevalSL_insert(txstampslist,lamp_seq_rx,tx_timestamp);
			}

			if(errorTsFlag==0) {
				if(timevalSub(&tx_timestamp,&rx_timestamp)) {
					fprintf(stderr,"Warning: negative latency!\nThis could potentually indicate that SO_TIMESTAMP is not working properly on your system.\n");
					tripTime=0;
				}
			}

			if(errorTsFlag==1) {
				// If a timestamping error occurred, set stored timestamp to 0
				rx_timestamp.tv_sec=0;
				rx_timestamp.tv_usec=0;

				// Set the flag back to 0 for the next iteration
				errorTsFlag=0;
			}

			// Compute triptime if follow-up mode is not active, otherwise just store the time difference timestamp, while waiting for the
			// follow-up message, containing the processing time delta to be used later on to compute the final triptime
			if(args->opts->followup_mode==FOLLOWUP_OFF) {
				tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
			} else {
				timevalSL_insert(triptimelist,lamp_seq_rx,rx_timestamp); // rx_timestamp now contains a timestamp difference (triptime as struct timeval)
			}
		}

		if(args->opts->followup_mode!=FOLLOWUP_OFF && lamp_type_rx==FOLLOWUP_DATA) {
			if(timevalSL_gather(triptimelist,lamp_seq_rx,&triptime_timestamp)) {
				fprintf(stderr,"Error: unable to compute delay for packet number: %d.\nIt is possible that a follow-up was received before the corresponding reply.\nReported time will be null.\n",lamp_seq_rx);
				errorTsFlag=1;
			} else {
				if((triptime_timestamp.tv_sec==0 && triptime_timestamp.tv_usec==0) || (packet_timestamp.tv_sec==0 && packet_timestamp.tv_usec==0)) {
					errorTsFlag=1;
				}

				if(errorTsFlag==0 && timevalSub(&packet_timestamp,&triptime_timestamp)) {
					fprintf(stderr,"Warning: negative time!\nThis could potentually indicate that SO_TIMESTAMP is not working properly on your system.\n");
					errorTsFlag=1;
				} else {
					tripTime=triptime_timestamp.tv_sec*SEC_TO_MICROSEC+triptime_timestamp.tv_usec;
				}
			}
		}

		if(errorTsFlag==1) {
			tripTime=0;

			// Set the flag back to 0 for the next iteration
			errorTsFlag=0;
		}

		// When using the follow-up mode, data is printed only when both the reply and the follow-up have been received
		if((args->opts->followup_mode==FOLLOWUP_OFF && (lamp_type_rx==PINGLIKE_REPLY || lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==PINGLIKE_REPLY_TLESS || lamp_type_rx==PINGLIKE_ENDREPLY_TLESS)) || 
			(args->opts->followup_mode!=FOLLOWUP_OFF && lamp_type_rx==FOLLOWUP_DATA)) {
			if(tripTime!=0) {
				// Get source MAC address from packet
				getSrcMAC(headerptrs.etherHeader,srcmacaddr_pkt);

				fprintf(stdout,"Received a reply from " PRI_MAC " (id=%u, seq=%u). Time: %.3f ms (%s)%s\n",
					MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(double)tripTime/1000,latencyTypePrinter(args->opts->latencyType),
					args->opts->followup_mode!=FOLLOWUP_OFF ? " (follow-up)" : "");
			}

			if(args->opts->followup_mode!=FOLLOWUP_OFF) {
				if(tripTime!=0) {
					tripTimeProc=packet_timestamp.tv_sec*SEC_TO_MICROSEC+packet_timestamp.tv_usec;
					fprintf(stdout,"Est. server processing time (follow-up): %.3f\n",(double)tripTimeProc/1000);
				} else {
					tripTimeProc=0;
					getSrcMAC(headerptrs.etherHeader,srcmacaddr_pkt);
					fprintf(stdout,"Error in packet from " PRI_MAC " (id=%u, seq=%u, rx_bytes=%d).\nThe server could not report any follow-up information about the processing time.\nNo RTT will be computed.\n",
						MAC_PRINTER(srcmacaddr_pkt),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
				}
			}

			// Update the current report structure
			reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);

			// In "-W" mode, write the current measured value to the specified CSV file too (if a file was successfully opened)
			if(Wfiledescriptor>0 || args->opts->udp_params.enabled) {
				perPktData.seqNo=lamp_seq_rx;
				perPktData.signedTripTime=tripTime;
				perPktData.tripTimeProc=tripTimeProc;

				if(args->opts->followup_mode!=FOLLOWUP_OFF) {
					timevalSL_gather(txstampslist,lamp_seq_rx,&tx_timestamp);
				}

				perPktData.tx_timestamp=tx_timestamp;

				if(Wfiledescriptor>0) {
					writeToTFile(Wfiledescriptor,W_DECIMAL_DIGITS,&perPktData);
				}

				if(args->opts->udp_params.enabled) {
					writeToReportSocket(&(args->sData.sock_w_data),W_DECIMAL_DIGITS,&perPktData,lamp_id_session,&first_call);
				}
			}

			// When -g is specified, update the Carbon/Graphite report structure
			// If this is the first time this point is reached, also start the metrics flush thread
			if(args->opts->carbon_sock_params.enabled) {
				if(carbon_metrics_flush_first==1) {
					carbon_metrics_flush_first=0;
					if(startCarbonTimedThread(&ctd,&carbonReportData,args->opts)<0) {
						t_rx_error=ERR_CARBON_THREAD;
						break;
					}
				}

				carbon_pthread_mutex_lock(ctd);
				carbonReportStructureUpdate(&carbonReportData,tripTime,lamp_seq_rx,args->opts->dup_detect_enabled);
				carbon_pthread_mutex_unlock(ctd);
			}

			if(continueFlag==0) {
				fu_flag=0;
			}
		}

		if(lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==PINGLIKE_ENDREPLY_TLESS) {
			continueFlag=0;
		}
	} while(continueFlag || fu_flag);

	// Free source MAC address memory area
	freeMacAddrT(srcmacaddr_pkt);

	if(!CHECK_SL_NULL(txstampslist)) {
		timevalSL_free(txstampslist);
	}

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
				t_rx_error=ERR_REPORT_TIMEOUT;
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
		ACKdata.controlRCV.ip=args->opts->dest_addr_u.destIPaddr;
		ACKdata.controlRCV.port=CLIENT_SRCPORT;
		memcpy(ACKdata.controlRCV.mac,args->opts->destmacaddr,ETHER_ADDR_LEN);

		if(controlSenderUDP_RAW(args,&ACKdata,lamp_id_session,1,ACK,0,0,NULL,NULL)<0) {
			fprintf(stderr,"Failed sending ACK.\n");
			t_rx_error=ERR_SEND;
		}
	}
}

static void rawBuffersCleanup(struct pktbuffers_udp buffs) {
	if(buffs.ethernetpacket) free(buffs.ethernetpacket);
	if(buffs.ippacket) free(buffs.ippacket);
	if(buffs.udppacket) free(buffs.udppacket);
	if(buffs.lamppacket) free(buffs.lamppacket);
}

unsigned int runUDPclient_raw(struct lampsock_data sData, macaddr_t srcMAC, struct in_addr srcIP, struct options *opts) {
	// Thread argument structures
	arg_struct args;
	arg_struct_followup_listener_raw_ip ful_raw_args;

	// Inform the user about the current options
	fprintf(stdout,"UDP client started, with options:\n\t[socket type] = RAW\n"
		"\t[interval] = %" PRIu64 " ms\n"
		"\t[reception timeout] = %" PRIu64 " ms\n",
		opts->interval, opts->interval<=MIN_TIMEOUT_VAL_C ? MIN_TIMEOUT_VAL_C+opts->client_timeout : opts->interval+opts->client_timeout);

	if(opts->duration_interval!=0) {
		fprintf(stdout,"\t[test duration] = %" PRIu32 " s\n",
			opts->duration_interval);
	} else {
		fprintf(stdout,"\t[total number of packets] = %" PRIu64 "\n",
			opts->number);
	}

	fprintf(stdout,"\t[mode] = %s\n"
		"\t[payload length] = %" PRIu16 " B \n"
		"\t[destination IP address] = %s\n"
		"\t[latency type] = %s\n"
		"\t[follow-up] = %s\n"
		"\t[random interval] = %s\n",
		opts->mode_ub==UNIDIR ? "unidirectional" : "ping-like", 
		opts->payloadlen, inet_ntoa(opts->dest_addr_u.destIPaddr),
		latencyTypePrinter(opts->latencyType),
		opts->followup_mode==FOLLOWUP_OFF ? "Off" : "On",
		opts->rand_type==NON_RAND ? "fixed periodic" : enum_to_str_rand_distribution_t(opts->rand_type)
		);

	if(opts->rand_type==NON_RAND) {
		fprintf(stdout,"\t[random interval batch] = -\n");
	} else {
		fprintf(stdout,"\t[random interval batch] = %" PRIu64 "\n",
			opts->rand_batch_size);
	}

	// Print current UP
	if(opts->macUP==UINT8_MAX) {
		fprintf(stdout,"\t[user priority] = unset or unpatched kernel\n");
	} else {
		fprintf(stdout,"\t[user priority] = %d\n",opts->macUP);
	}

	if(opts->latencyType==KRT) {
		// Check if the KRT mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_SW_RX)<0) {
		 	perror("socketSetTimestamping() error");
		    fprintf(stderr,"Warning: SO_TIMESTAMP is probably not suppoerted. Switching back to user-to-user latency.\n");
		    opts->latencyType=USERTOUSER;
		}
	}

	// Initialize the report structure
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType, opts->followup_mode, opts->dup_detect_enabled);

	// Initialize the Carbon report structure, if the -g option is used
	if(opts->carbon_sock_params.enabled) {
		if(openCarbonReportSocket(&carbonReportData,opts)<0) {
			fprintf(stderr,"Error: cannot open the socket for sending the data to Carbon/Graphite.\n");
			return 2;
		}

		carbonReportStructureInit(&carbonReportData,opts);

		// This flag is used to understand when the first data is available, in order to start the metrics flush thread (see carbon_thread_manager.c)
		carbon_metrics_flush_first=1;
	}

	// Populate/initialize the 'args' structs
	args.sData=sData;
	args.opts=opts;
	args.srcMAC=srcMAC;
	args.srcIP=srcIP;

	ful_raw_args.sFd=sData.descriptor; // (populate)
	ful_raw_args.srcIP=srcIP; // (populate)
	ful_raw_args.responseType=-1; // (initialize)

	// LaMP ID is randomly generated between 0 and 65535 (the maximum over 16 bits)
	lamp_id_session=(rand()+getpid())%UINT16_MAX;

	// This fprintf() terminates the series of call to inform the user about current settings -> using \n\n instead of \n
	fprintf(stdout,"\t[session LaMP ID] = %" PRIu16 "\n\n",lamp_id_session);

	if(opts->latencyType==KRT) {
		// Check if the KRT mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_SW_RX)<0) {
		 	perror("socketSetTimestamping() error");
		    fprintf(stderr,"Warning: SO_TIMESTAMP is probably not supported. Switching back to user-to-user latency.\n");
		    opts->latencyType=USERTOUSER;
		}
	} else if(opts->latencyType==SOFTWARE) {
		// Check if the SOFTWARE (kernel rx+tx timestamps) mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_SW_RXTX)<0) {
		 	perror("socketSetTimestamping() error");
		    fprintf(stderr,"Warning: software transmit/receive timestamping is not supported. Switching back to user-to-user latency.\n");
		    opts->latencyType=USERTOUSER;
		}
	} else if(opts->latencyType==HARDWARE) {
		// Check if the HARDWARE (kernel rx+tx timestamps) mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_HW)<0) {
		 	perror("socketSetTimestamping() error");
		    fprintf(stderr,"Warning: hardware timestamping is not supported. Switching back to user-to-user latency.\n");
		    opts->latencyType=USERTOUSER;
		}
	}

	// Start init procedure
	// Create INIT send and ACK listener threads
	pthread_create(&initSender_tid,NULL,&initSender,(void *) &args);
	pthread_create(&ackListenerInit_tid,NULL,&ackListenerInit,(void *) &args);

	// Wait for the threads to finish
	pthread_join(initSender_tid,NULL);
	pthread_join(ackListenerInit_tid,NULL);

	if(t_tx_error==NO_ERR && t_rx_error==NO_ERR) {
		if(opts->followup_mode!=FOLLOWUP_OFF) {
			pthread_create(&followupRequestSender_tid,NULL,&followupRequestSender,(void *) &args);
			pthread_create(&followupReplyListener_tid,NULL,&followupReplyListener,(void *) &ful_raw_args);

			// Wait for the threads to finish
			pthread_join(followupRequestSender_tid,NULL);
			pthread_join(followupReplyListener_tid,NULL);

			if(t_tx_error!=NO_ERR || t_rx_error!=NO_ERR) {
				fprintf(stderr,"Warning: cannot determine if the server supports the requested timestamps.\n\tDisabling follow-up messages.\n");
			} else {
				if(ful_raw_args.responseType!=FOLLOWUP_ACCEPT) {
					fprintf(stderr,"Warning: the server reported that it does not support the requested follow-up mechanism.\n\tDisabling follow-up messages.\n");
				    opts->followup_mode=FOLLOWUP_OFF;
				}
			}
		}

		// If mode is HARDWARE or SOFTWARE, initialize the data structure to store the tx timestamps
		if(opts->latencyType==HARDWARE || opts->latencyType==SOFTWARE) {
			tslist=timevalSL_init();

			if(CHECK_SL_NULL(tslist)) {
				fprintf(stderr,"Warning: unable to allocate memory for the hardware timestamping mode.\n\tSwitching back to user-to-user latency.\n");
		    	opts->latencyType=USERTOUSER;
			}
		}

		// If the follow-up mechanism is active, initialize the data structure to store triptimes when waiting for the follow-up messages
		if(opts->followup_mode!=FOLLOWUP_OFF) {
			triptimelist=timevalSL_init();

			if(CHECK_SL_NULL(triptimelist)) {
				fprintf(stderr,"Warning: unable to allocate memory for the follow-up mode.\n\tIt has been disabled.\n");
		    	opts->followup_mode=FOLLOWUP_OFF;
			}
		}

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

		// Terminate the carbon flush thread
		if(opts->carbon_sock_params.enabled && t_rx_error!=ERR_CARBON_THREAD) {
			stopCarbonTimedThread(&ctd);
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

		// Directly return only if a timeout did not occur, as, in case of timeout, we should still print the report.
		// If we exit now, losing the last packet (ENDREPLY or ENDREPLY_TLESS), which causes a timeout in the rx loop,
		// may mean losing the whole test, which is not desiderable
		if(t_rx_error!=ERR_TIMEOUT && t_rx_error!=ERR_REPORT_TIMEOUT) {
			return 1;
		}
		return 1;
	}

	if(t_rx_error!=ERR_REPORT_TIMEOUT) {
		/* Ok, the mode_ub==UNSET_UB case is not managed, but it should never happen to reach this point
		with an unset mode... at least not without getting errors or a chaotic thread behaviour! But it should not happen anyways. */
		fprintf(stdout,opts->mode_ub==PINGLIKE?"Ping-like ":"Unidirectional " "statistics:\n");
		// Print the statistics, if no error, before returning
		reportStructureFinalize(&reportData);
		printStats(&reportData,stdout,opts->confidenceIntervalMask);
	}

	if(opts->filename!=NULL) {
		// If '-f' was specified, print the report data to a file too
		printStatsCSV(opts,&reportData,opts->filename);
	}

	if(opts->udp_params.enabled) {
		// If '-w' was specified, send the report data inside a TCP packet, through a socket
		printStatsSocket(opts,&reportData,&(sData.sock_w_data),lamp_id_session);
	}

	if(opts->carbon_sock_params.enabled) {
		closeCarbonReportSocket(&carbonReportData);
		carbonReportStructureFree(&carbonReportData,opts);
	}

	reportStructureFree(&reportData);

	if(!CHECK_SL_NULL(tslist)) {
		timevalSL_free(tslist);
	}

	if(!CHECK_SL_NULL(triptimelist)) {
		timevalSL_free(triptimelist);
	}

	// Returning 0 if everything worked fine
	return 0;
}