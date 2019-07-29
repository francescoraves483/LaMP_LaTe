#include "udp_client.h"
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
#include <linux/errqueue.h>
#include <semaphore.h>
#include "timeval_utils.h"
#include "common_thread.h"
#include "timer_man.h"
#include "common_udp.h"

// Local global variables
static pthread_t txLoop_tid, rxLoop_tid, ackListenerInit_tid, initSender_tid, followupReplyListener_tid, followupRequestSender_tid;
static uint16_t lamp_id_session;
static reportStructure reportData;

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

static uint8_t ack_init_received=0; // Global flag set by the ackListenerInit thread: = 1 when an ACK has been received, otherwise it is = 0
static pthread_mutex_t ack_init_received_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the ack_init_received variable (as it written by a thread and read by another one)
static uint8_t followup_reply_received=0; // Global flag set by the followupReplyListener thread: = 1 when a reply has been received, otherwise it is = 0
static pthread_mutex_t followup_reply_received_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the followup_reply_received variable


extern inline int timevalSub(struct timeval *in, struct timeval *out);

// Function prototypes
static void txLoop (arg_struct_udp *args);
static void unidirRxTxLoop (arg_struct_udp *args);

// Thread entry point function prototypes
static void *txLoop_t (void *arg);
static void *rxLoop_t (void *arg);
static void *ackListenerInit (void *arg);
static void *followupReplyListener (void *arg);
static void *initSender (void *arg);
static void *followupRequestSender (void *arg);

static void *ackListenerInit (void *arg) {
	int *sFd=(int *) arg;
	controlRCVdata rcvData;
	int return_value;

	rcvData.session_id=lamp_id_session;

	return_value=controlReceiverUDP(*sFd,&rcvData,ACK,&ack_init_received,&ack_init_received_mut);
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

static void *followupReplyListener (void *arg) {
	arg_struct_followup_listener *ful_arg=(arg_struct_followup_listener *) arg;
	controlRCVdata rcvData;
	int return_value;

	rcvData.session_id=lamp_id_session;

	return_value=controlReceiverUDP(ful_arg->sFd,&rcvData,FOLLOWUP_CTRL,&followup_reply_received,&followup_reply_received_mut);
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

static void *initSender (void *arg) {
	arg_struct_udp *args=(arg_struct_udp *) arg;
	int return_value;

	return_value=controlSenderUDP(args,lamp_id_session,INIT_RETRY_MAX_ATTEMPTS,INIT,0,INIT_RETRY_INTERVAL_MS,&ack_init_received,&ack_init_received_mut);

	if(return_value<0) {
		if(return_value==-1) {
			t_tx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(return_value==-2) {
			t_tx_error=ERR_SEND_INIT;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}
	}

	pthread_exit(NULL);
}

static void *followupRequestSender (void *arg) {
	arg_struct_udp *args=(arg_struct_udp *) arg;
	int return_value;
	uint16_t followup_req_type;

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
		return_value=controlSenderUDP(args,lamp_id_session,FOLLOWUP_CTRL_RETRY_MAX_ATTEMPTS,FOLLOWUP_CTRL,followup_req_type,FOLLOWUP_CTRL_RETRY_INTERVAL_MS,&followup_reply_received,&followup_reply_received_mut);

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
	arg_struct_udp *args=(arg_struct_udp *) arg;

	// Call the Tx loop
	txLoop(args);

	pthread_exit(NULL);
}

static void txLoop (arg_struct_udp *args) {
	// LaMP header and LaMP packet buffer
	struct lamphdr lampHeader;
	byte_t *lampPacket=NULL;
	byte_t *lampPacketRxPtr=NULL;

	// Timer variables
	struct pollfd timerMon;
	int clockFd;
	int timerCaS_res=0;

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Payload buffer
	byte_t *payload_buff=NULL;

	// while loop counter
	unsigned int counter=0;

	// LaMP packet type
	uint8_t ctrl=CTRL_PINGLIKE_REQ;
	uint32_t lampPacketSize=0;

	// SO_TIMESTAMPING variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBuf[CMSG_SPACE(sizeof(struct scm_timestamping))];

	// struct scm_timestamping and struct timeval for the tx timestamp
	struct scm_timestamping hw_ts;
	struct timeval tx_timestamp;

	// recvfrom variable (for HARDWARE mode only)
	ssize_t rcv_bytes;

	// LaMP fields for packet retrieved from socket error queue (hardware tx timestamping only)
	uint16_t lamp_seq_rx_errqueue=0;
	lamptype_t lamp_type_rx_errqueue;

	// Buffer used in HARDWARE mode only, set as iov_base.
	// As "the recvmsg call returns the original
	//  outgoing data packet with two ancillary messages attached"
	//  the size of this buffer is set to be equal to the size of
	//  the lampPacket buffer, allocated later on, plus all the 
	//  UDP/IPv4/Ethernet headers
	byte_t *data_iov=NULL;

	// Populating the LaMP header
	if(args->opts->mode_ub==PINGLIKE) {
		// Timestampless request in HARDWARE/SOFTWARE mode, as timestamps are directly gathered and managed inside the client (both tx and rx)
		if(args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
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
	lampHeadPopulate(&lampHeader, ctrl, lamp_id_session, 0); // Starting from sequence number = 0

	// Allocating packet buffers (with and without payload)
	if(args->opts->payloadlen!=0) {
		lampPacket=malloc(sizeof(struct lamphdr)+args->opts->payloadlen);
		if(!lampPacket) {
			t_tx_error=ERR_MALLOC;
			pthread_exit(NULL);
		}

		lampPacketSize=LAMP_HDR_PAYLOAD_SIZE(args->opts->payloadlen);
	} else {
		// There is no need to allocate the lamppacket buffer, as the LaMP header will be directly sent as UDP payload
		lampPacketSize=LAMP_HDR_SIZE();
	}

	memset(&mhdr,0,sizeof(mhdr));
	// Prepare ancillary data structures, if HARDWARE or SOFTWARE mode is selected (to get send timestamps)
	if(args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
		data_iov=malloc(ETH_IP_UDP_PACKET_SIZE_S(sizeof(struct lamphdr)+args->opts->payloadlen));

		if(!data_iov) {
			free(lampPacket);
			t_tx_error=ERR_MALLOC;
			pthread_exit(NULL);
		}

		// iovec buffers (scatter/gather arrays) 
		iov.iov_base=(void *)data_iov;
		iov.iov_len=ETH_IP_UDP_PACKET_SIZE_S(sizeof(struct lamphdr)+args->opts->payloadlen); // Macro from Rawsock library

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
			free(lampPacket);

			if(data_iov) {
				free(data_iov);
			}

			t_tx_error=ERR_MALLOC;
			pthread_exit(NULL);
		}

		for(int i=0;i<args->opts->payloadlen;i++) {
			payload_buff[i]=(byte_t) (i & 15);
		}
	}

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

			// Set UNIDIR_STOP or PINGLIKE_ENDREQ (TLESS for HARDWARE mode) when the last packet has to be transmitted, depending on the current mode_ub ("mode unidirectional/bidirectional")
			if(counter==args->opts->number-1) {
				if(args->opts->mode_ub==UNIDIR) {
					lampSetUnidirStop(&lampHeader);
				} else if(args->opts->mode_ub==PINGLIKE) {
					lampSetPinglikeEndreqAll(&lampHeader);
				}
			}

			// Encapsulate LaMP payload only if it is available
			if(args->opts->payloadlen!=0) {
				lampEncapsulate(lampPacket, &lampHeader, payload_buff, args->opts->payloadlen);
				// Set timestamp
				lampHeadSetTimestamp((struct lamphdr *)lampPacket,NULL);
			} else {
				lampHeadSetTimestamp(&lampHeader,NULL);
				lampPacket=(byte_t *)&lampHeader; // The LaMP packet is only composed by the header
			}

			if(args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
				pthread_mutex_lock(&tslist_mut);
			}

			if(sendto(args->sData.descriptor,lampPacket,lampPacketSize,NO_FLAGS,(struct sockaddr *)&(args->sData.addru.addrin[1]),sizeof(struct sockaddr_in))!=lampPacketSize) {
				perror("sendto() for sending LaMP packet failed");
				fprintf(stderr,"Failed sending latency measurement packet with seq: %u.\nThe execution will terminate now.\n",lampHeader.seq);
				break;
			}

			// Retrieve tx timestamp if mode is HARDWARE/SOFTWARE (i.e. either HARDWARE or software kernel tx and rx timestamps)
			// Extract ancillary data with the tx timestamp (if mode is HARDWARE/SOFTWARE)
			if(args->opts->latencyType==HARDWARE || args->opts->latencyType==SOFTWARE) {
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

			if(args->opts->mode_ub==UNIDIR) {
				fprintf(stdout,"Sent unidirectional message with destination IP %s (id=%u, seq=%u)\n",
					inet_ntoa(args->opts->destIPaddr), lamp_id_session, counter);
			}

			// Increase sequence number for the next iteration
			lampHeadIncreaseSeq(&lampHeader);

			// Increase counter
			counter++;
		}
	}

	// Free payload buffer
	if(args->opts->payloadlen!=0) {
		free(payload_buff);
		// Free the LaMP packet buffer only if it was allocated (otherwise a SIGSEGV may occur if payloadLen is 0)
		free(lampPacket);
	}

	// Close timer file descriptor
	close(clockFd);
}

static void *rxLoop_t (void *arg) {
	arg_struct_udp *args=(arg_struct_udp *) arg;

	int Wfiledescriptor=-1;

	// Packet buffer with size = maximum LaMP packet length
	byte_t lampPacket[MAX_LAMP_LEN + LAMP_HDR_SIZE()];
	// Pointer to the header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;

	// recvfrom variables
	ssize_t rcv_bytes;

	int fu_flag=1; // Flag set to 0 when a follow-up is received after an ENDREPLY or ENDREPLY_TLESS (HARDWARE latencyType only, fixed to 0 for other types)
	int continueFlag=1; // Flag set to 0 when an ENDREPLY or ENDREPLY_TLESS is received
	int errorTsFlag=0; // Flag set to 1 when an error occurred in retrieving a timestamp (i.e. if no latency data can be reported for the current packet)

	// RX and TX timestamp containers (plus follow-up and trip time timestamps for the HARDWARE mode)
	struct timeval rx_timestamp, tx_timestamp, triptime_timestamp, packet_timestamp;
	struct scm_timestamping hw_ts;

	// Variable to store the latency (trip time)
	uint64_t tripTime=0;

	// Variable to store the processing time (server time delta) when using follow-up mode
	uint64_t tripTimeProc=0;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx=0; 
	uint16_t lamp_payloadlen_rx;

	// SO_TIMESTAMP variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg=NULL;

	// Ancillary data buffers
	char ctrlBufSw[CMSG_SPACE(sizeof(struct timeval))];
	char ctrlBufHw[CMSG_SPACE(sizeof(struct scm_timestamping))];

	// struct sockaddr_in to store the source IP address of the received LaMP packets
	struct sockaddr_in srcAddr;
	socklen_t srcAddrLen=sizeof(srcAddr);

	// Set fu_flag to 0 if follow-up mode is disabled
	if(args->opts->followup_mode==FOLLOWUP_OFF) {
		fu_flag=0;
	}

	// Prepare ancillary data structures, if KRT or HARDWARE/SOFTWARE mode is selected
	if(args->opts->latencyType==KRT || args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
		// iovec buffers (scatter/gather arrays)
		iov.iov_base=lampPacket;
		iov.iov_len=sizeof(lampPacket);

		// Socket address structure
		mhdr.msg_name=(void *)&srcAddr;
		mhdr.msg_namelen=srcAddrLen;

		// Ancillary data (control message)
		mhdr.msg_control=args->opts->latencyType!=KRT ? ctrlBufHw : ctrlBufSw;
        mhdr.msg_controllen=args->opts->latencyType!=KRT ? sizeof(ctrlBufHw) : sizeof(ctrlBufSw);

        // iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
	}

	// Open CSV file when in "-W" mode (i.e. "write every packet measurement data to CSV file")
	if(args->opts->Wfilename!=NULL) {
		Wfiledescriptor=openTfile(args->opts->Wfilename,args->opts->followup_mode!=FOLLOWUP_OFF);
		if(Wfiledescriptor<0) {
			fprintf(stderr,"Warning! Cannot open file for writing single packet latency data.\nThe '-W' option will be disabled.\n");
		}
	}

	// Start receiving packets (this is the ping-like loop), specifying a "struct sockaddr_in" to recvfrom() in order to obtain the source MAC address
	do {
		// If in KRT or HARDWARE/SOFTWARE mode, use recvmsg(), otherwise, use recvfrom()
		if(args->opts->latencyType==KRT || args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
			saferecvmsg(rcv_bytes,args->sData.descriptor,&mhdr,NO_FLAGS);
		} else {
			saferecvfrom(rcv_bytes,args->sData.descriptor,lampPacket,MAX_LAMP_LEN,NO_FLAGS,(struct sockaddr *)&srcAddr,&srcAddrLen);
		}

		// Timeout or generic recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				t_rx_error=ERR_TIMEOUT;
				fprintf(stderr,"Timeout when waiting for new packets.\n");
			} else {
				t_rx_error=ERR_RECVFROM_GENERIC;
			}
			break;
		}

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			continue;
		}

		// If the packet is really a LaMP packet, get the header data (followup_timestamp will be null when a timestampless reply is received in HARDWARE mode)
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, &packet_timestamp, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_id_rx!=lamp_id_session) {
			continue;
		}

		// The client is not expected to receive any FOLLOWUP_CTRL at this point!
		if(lamp_type_rx==FOLLOWUP_CTRL) {
			continue;
		}

		if(lamp_type_rx==PINGLIKE_REPLY || lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==PINGLIKE_REPLY_TLESS || lamp_type_rx==PINGLIKE_ENDREPLY_TLESS) {
			// Extract ancillary data (if mode is KRT or if it is HARDWARE)
			if(args->opts->latencyType==KRT || args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
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

			if(args->opts->latencyType==SOFTWARE || args->opts->latencyType==HARDWARE) {
				pthread_mutex_lock(&tslist_mut);
				if(timevalSL_gather(tslist,lamp_seq_rx,&tx_timestamp)) {
					fprintf(stderr,"Error: could not retrieve transmit timestamp for packet number: %d.\n",lamp_seq_rx);
					errorTsFlag=1;
				}
				pthread_mutex_unlock(&tslist_mut);
			} else {
				tx_timestamp=packet_timestamp;
			}

			if(errorTsFlag==0) {
				if(timevalSub(&tx_timestamp,&rx_timestamp)) {
					fprintf(stderr,"Error: negative latency!\nThis could potentually indicate that SO_TIMESTAMP is not working properly on your system.\n");
					errorTsFlag=1;
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
				fprintf(stderr,"Error: unable to compute delay for packet number: %d.\nIt is possible that a follow-up was received before the corresponding reply.\n",lamp_seq_rx);
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
		if(args->opts->followup_mode==FOLLOWUP_OFF || (args->opts->followup_mode!=FOLLOWUP_OFF && lamp_type_rx==FOLLOWUP_DATA)) {
			if(tripTime!=0) {
				fprintf(stdout,"Received a reply from %s (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)%s\n",
					inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(args->opts->latencyType),
					args->opts->followup_mode!=FOLLOWUP_OFF ? " (follow-up)" : "");
			}

			if(args->opts->followup_mode!=FOLLOWUP_OFF) {
				if(tripTime!=0) {
					tripTimeProc=packet_timestamp.tv_sec*SEC_TO_MICROSEC+packet_timestamp.tv_usec;
					fprintf(stdout,"Est. server processing time (follow-up): %.3f\n",(double)tripTimeProc/1000);
				} else {
					tripTimeProc=0;
					fprintf(stdout,"Error in packet from %s (id=%u, seq=%u, rx_bytes=%d).\nThe server could not report any follow-up information about the processing time.\nNo RTT will be computed.\n",
						inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
				}
			}

			// Update the current report structure
			reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);

			// In "-W" mode, write the current measured value to the specified CSV file too (if a file was successfully opened)
			if(Wfiledescriptor>0) {
				writeToTFile(Wfiledescriptor,args->opts->followup_mode!=FOLLOWUP_OFF,W_DECIMAL_DIGITS,tripTime,tripTimeProc);
			}

			if(continueFlag==0) {
				fu_flag=0;
			}
		}

		if(lamp_type_rx==PINGLIKE_ENDREPLY || lamp_type_rx==PINGLIKE_ENDREPLY_TLESS) {
			continueFlag=0;
		}
	} while(continueFlag || fu_flag);

	if(Wfiledescriptor>0) {
		closeTfile(Wfiledescriptor);
	}

	pthread_exit(NULL);
}

static void unidirRxTxLoop (arg_struct_udp *args) {
	// Packet buffer with size = maximum LaMP packet length
	byte_t lampPacket[MAX_LAMP_LEN];
	// Pointer to the header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;
	// Pointer to the payload, inside the packet buffer
	byte_t *lampPayloadPtr=lampPacket+LAMP_HDR_SIZE();

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx;
	uint16_t lamp_payloadlen_rx;

	uint8_t timeoutFlag=0; // = 1 if a timeout occurred, otherwise it should remain = 0

	ssize_t rcv_bytes;

	/* --------------------------- Rx part --------------------------- */

	// There's no real loop now, just wait for a correct report and send ACK
	while(1) {
		saferecvfrom(rcv_bytes,args->sData.descriptor,lampPacket,MAX_LAMP_LEN,NO_FLAGS,NULL, NULL);

		// Timeout or generic recvfrom() error occurred
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

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
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
		repscanf((const char *)lampPayloadPtr,&reportData);

		if(controlSenderUDP(args,lamp_id_session,1,ACK,0,0,NULL,NULL)<0) {
			fprintf(stderr,"Failed sending ACK.\n");
			t_rx_error=ERR_SEND;
		}
	}
}

unsigned int runUDPclient(struct lampsock_data sData, struct options *opts) {
	// Thread argument structures
	arg_struct_udp args;
	arg_struct_followup_listener ful_args;

	// Inform the user about the current options
	fprintf(stdout,"UDP client started, with options:\n\t[socket type] = UDP\n"
		"\t[interval] = %" PRIu64 " ms\n"
		"\t[reception timeout] = %" PRIu64 " ms\n"
		"\t[total number of packets] = %" PRIu64 "\n"
		"\t[mode] = %s\n"
		"\t[payload length] = %" PRIu16 " B \n"
		"\t[destination IP address] = %s\n"
		"\t[latency type] = %s\n"
		"\t[follow-up] = %s\n",
		opts->interval, opts->interval<=MIN_TIMEOUT_VAL_C ? MIN_TIMEOUT_VAL_C+2000 : opts->interval+2000,
		opts->number, opts->mode_ub==UNIDIR ? "unidirectional" : "ping-like", 
		opts->payloadlen, inet_ntoa(opts->destIPaddr),
		latencyTypePrinter(opts->latencyType),
		opts->followup_mode==FOLLOWUP_OFF ? "Off" : "On");

	// Print current UP
	if(opts->macUP==UINT8_MAX) {
		fprintf(stdout,"\t[user priority] = unset or unpatched kernel\n");
	} else {
		fprintf(stdout,"\t[user priority] = %d\n",opts->macUP);
	}

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
		// Check if the HARDWARE mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_HW)<0) {
		 	perror("socketSetTimestamping() error");
		    fprintf(stderr,"Warning: hardware timestamping is not supported. Switching back to user-to-user latency.\n");
		    opts->latencyType=USERTOUSER;
		}
	}

	// Initialize the report structure
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType, opts->followup_mode);

	// Prepare sendto sockaddr_in structure (index 1) for the client
	memset(&(sData.addru.addrin[1]),0,sizeof(sData.addru.addrin[1]));
	sData.addru.addrin[1].sin_family=AF_INET;
	sData.addru.addrin[1].sin_port=htons(opts->port);
	sData.addru.addrin[1].sin_addr.s_addr=opts->destIPaddr.s_addr;

	// Populate/initialize the 'args' structs
	args.sData=sData; // (populate)
	args.opts=opts; // (populate)

	ful_args.sFd=sData.descriptor; // (populate)
	ful_args.responseType=-1; // (initialize)

	// Start init procedure
	// Create INIT send and ACK listener threads
	pthread_create(&initSender_tid,NULL,&initSender,(void *) &args);
	pthread_create(&ackListenerInit_tid,NULL,&ackListenerInit,(void *) &(sData.descriptor));

	// Wait for the threads to finish
	pthread_join(initSender_tid,NULL);
	pthread_join(ackListenerInit_tid,NULL);

	if(t_tx_error==NO_ERR && t_rx_error==NO_ERR) {
		if(opts->followup_mode!=FOLLOWUP_OFF) {
			// If the user has requested the follow-up mode, start the FOLLOWUP request/reply procedure
			// The client will send a request to the server, which will should "ACCEPT" if it supports
			//  the requested type of timestamping (depending on which kind of latency type is specified,
			//  i.e. if HW timestamps are requested, the server will reply with "ACCEPT" if it supports them
			//  too, or "DENY" if they are not supported".
			pthread_create(&followupRequestSender_tid,NULL,&followupRequestSender,(void *) &args);
			pthread_create(&followupReplyListener_tid,NULL,&followupReplyListener,(void *) &ful_args);

			// Wait for the threads to finish
			pthread_join(followupRequestSender_tid,NULL);
			pthread_join(followupReplyListener_tid,NULL);

			if(t_tx_error!=NO_ERR || t_rx_error!=NO_ERR) {
				if(opts->latencyType==HARDWARE) {
					fprintf(stderr,"Warning: cannot determine if the server supports hardware timestamps.\n\tDisabling follow-up messages.\n");
				} else {
					fprintf(stderr,"Warning: cannot determine if the server supports the requested timestamps.\n\tDisabling follow-up messages.\n");
				}
			    opts->followup_mode=FOLLOWUP_OFF;
			} else {
				if(ful_args.responseType!=FOLLOWUP_ACCEPT) {
					if(opts->latencyType==HARDWARE) {
						fprintf(stderr,"Warning: the server reported that it does not support hardware timestamping.\n\tDisabling follow-up messages.\n");
					} else {
						fprintf(stderr,"Warning: the server reported that it does not support the requested follow-up mechanism.\n\tDisabling follow-up messages.\n");
					}
				    opts->followup_mode=FOLLOWUP_OFF;
				}
			}
		}

		// If mode is HARDWARE or SOFTWARE, initialize the data structure to store the tx timestamps and the semaphore 'tx_sem'
		if(opts->latencyType==HARDWARE || opts->latencyType==SOFTWARE) {
			tslist=timevalSL_init();

			if(CHECK_SL_NULL(tslist)) {
				fprintf(stderr,"Warning: unable to allocate/initialize memory for the hardware/software timestamping mode.\n\tSwitching back to user-to-user latency.\n");
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

		// Start rx and tx loops
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
	reportStructureFinalize(&reportData);
	printStats(&reportData,stdout,opts->confidenceIntervalMask);

	if(opts->filename!=NULL) {
		// If '-f' was specified, print the report data to a file too
		printStatsCSV(opts,&reportData,opts->filename);
	}

	if(!CHECK_SL_NULL(tslist)) {
		timevalSL_free(tslist);
	}

	if(!CHECK_SL_NULL(triptimelist)) {
		timevalSL_free(triptimelist);
	}

	// Returning 0 if everything worked fine
	return 0;
}