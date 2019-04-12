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

static uint8_t ack_init_received=0; // Global flag set by the ackListenerInit thread: = 1 when an ACK has been received, otherwise it is = 0
static pthread_mutex_t ack_init_received_mut=PTHREAD_MUTEX_INITIALIZER; // Mutex to protect the ack_init_received variable (as it written by a thread and read by another one)

extern inline int timevalSub(struct timeval *in, struct timeval *out);

// Function prototypes
static void txLoop (arg_struct_udp *args);
static void unidirRxTxLoop (arg_struct_udp *args);

// Thread entry point function prototypes
static void *txLoop_t (void *arg);
static void *rxLoop_t (void *arg);
static void *ackListenerInit (void *arg);
static void *initSender (void *arg);

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

static void *initSender (void *arg) {
	arg_struct_udp *args=(arg_struct_udp *) arg;
	int return_value;

	return_value=controlSenderUDP(args,lamp_id_session,INIT_RETRY_MAX_ATTEMPTS,INIT,INIT_RETRY_INTERVAL_MS,&ack_init_received,&ack_init_received_mut);

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

	// Populating the LaMP header
	if(args->opts->mode_ub==PINGLIKE) {
		ctrl=CTRL_PINGLIKE_REQ;
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

	// Populate payload buffer only if 'payloadlen' is different than 0
	if(args->opts->payloadlen!=0) {
		payload_buff=malloc((args->opts->payloadlen)*sizeof(byte_t));

		if(!payload_buff) {
			free(lampPacket);
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

			// Set UNIDIR_STOP or PINGLIKE_ENDREQ when the last packet has to be transmitted, depending on the current mode_ub ("mode unidirectional/bidirectional")
			if(counter==args->opts->number-1) {
				if(args->opts->mode_ub==UNIDIR) {
					lampSetUnidirStop(&lampHeader);
				} else if(args->opts->mode_ub==PINGLIKE) {
					lampSetPinglikeEndreq(&lampHeader);
				}
			}
			
			// Encapsulate LaMP payload only if it is available
			if(args->opts->payloadlen!=0) {
				lampEncapsulate(lampPacket, &lampHeader, payload_buff, args->opts->payloadlen);
				// Set timestamp
				lampHeadSetTimestamp((struct lamphdr *)lampPacket);
			} else {
				lampPacket=(byte_t *)&lampHeader;
				lampHeadSetTimestamp(&lampHeader);
			}

			if(sendto(args->sData.descriptor,lampPacket,lampPacketSize,NO_FLAGS,(struct sockaddr *)&(args->sData.addru.addrin[1]),sizeof(struct sockaddr_in))!=lampPacketSize) {
				perror("sendto() for sending LaMP packet failed");
				fprintf(stderr,"Failed sending latency measurement packet with seq: %u.\nThe execution will terminate now.\n",lampHeader.seq);
				break;
			}

			if(args->opts->mode_ub==UNIDIR) {
				fprintf(stdout,"Sent unidirectional message with destination IP %s (id=%u, seq=%u).\n",
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

	// Packet buffer with size = maximum LaMP packet length
	byte_t lampPacket[MAX_LAMP_LEN];
	// Pointer to the header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;

	// recvfrom variables
	ssize_t rcv_bytes;

	// RX and TX timestamp containers
	struct timeval rx_timestamp, tx_timestamp;

	// Variable to store the latency (trip time)
	uint64_t tripTime;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx=0; 
	uint16_t lamp_payloadlen_rx;

	// SO_TIMESTAMP variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBuf[CMSG_SPACE(sizeof(struct timeval))];

	// struct sockaddr_in to store the source IP address of the received LaMP packets
	struct sockaddr_in srcAddr;
	socklen_t srcAddrLen=sizeof(srcAddr);

	// Prepare ancillary data structures, if KRT mode is selected
	if(args->opts->latencyType==KRT) {
		// iovec buffers (scatter/gather arrays)
		iov.iov_base=lampPacket;
		iov.iov_len=sizeof(lampPacket);

		// Socket address structure
		mhdr.msg_name=&(srcAddr);
		mhdr.msg_namelen=srcAddrLen;

		// Ancillary data (control message)
		mhdr.msg_control=ctrlBuf;
        mhdr.msg_controllen=sizeof(ctrlBuf);

        // iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
	}

	// Start receiving packets (this is the ping-like loop), specifying a "struct sockaddr_in" to recvfrom() in order to obtain the source MAC address
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
			saferecvfrom(rcv_bytes,args->sData.descriptor,lampPacket,MAX_LAMP_LEN,NO_FLAGS,(struct sockaddr *)&srcAddr,&srcAddrLen);
		}

		// Timeout or generic recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				t_rx_error=ERR_TIMEOUT;
				fprintf(stderr,"Timeout when waiting for the report. No report will be printed by the client.\n");
			} else {
				t_rx_error=ERR_RECVFROM_GENERIC;
			}
			break;
		}

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			continue;
		}

		// If the packet is really a LaMP packet, get the header data
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, &tx_timestamp, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_id_rx!=lamp_id_session || (lamp_type_rx!=PINGLIKE_REPLY && lamp_type_rx!=PINGLIKE_ENDREPLY)) {
			continue;
		}

		if(args->opts->latencyType==USERTOUSER) {
			gettimeofday(&rx_timestamp,NULL);
		}

		if(timevalSub(&tx_timestamp,&rx_timestamp)) {
			fprintf(stderr,"Warning: negative latency!\nThis could potentually indicate that SIOCGSTAMP is not working properly on your system.\n");
			tripTime=0;
		} else {
			tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
		}

		fprintf(stdout,"Received a reply from %s (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
			inet_ntoa(srcAddr.sin_addr), lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(args->opts->latencyType));

		// Update the current report structure
		reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);

	} while(lamp_type_rx!=PINGLIKE_ENDREPLY);

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

		if(controlSenderUDP(args,lamp_id_session,1,ACK,0,NULL,NULL)<0) {
			fprintf(stderr,"Failed sending ACK.\n");
			t_rx_error=ERR_SEND;
		}
	}
}

unsigned int runUDPclient(struct lampsock_data sData, struct options *opts) {
	// Thread argument structure
	arg_struct_udp args;

	// Inform the user about the current options
	fprintf(stdout,"UDP client started, with options:\n\t[socket type] = UDP\n"
		"\t[interval] = %" PRIu64 " ms\n"
		"\t[reception timeout] = %" PRIu64 " ms\n"
		"\t[total number of packets] = %" PRIu64 "\n"
		"\t[mode] = %s\n"
		"\t[payload length] = %" PRIu16 " B \n"
		"\t[destination IP address] = %s\n"
		"\t[latency type] = %s\n",
		opts->interval, opts->interval<=MIN_TIMEOUT_VAL_C ? MIN_TIMEOUT_VAL_C+2000 : opts->interval+2000,
		opts->number, opts->mode_ub==UNIDIR?"unidirectional":"ping-like", 
		opts->payloadlen, inet_ntoa(opts->destIPaddr),
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

	// Prepare sendto sockaddr_in structure (index 1) for the client
	bzero(&(sData.addru.addrin[1]),sizeof(sData.addru.addrin[1]));
	sData.addru.addrin[1].sin_family=AF_INET;
	sData.addru.addrin[1].sin_port=htons(opts->port);
	sData.addru.addrin[1].sin_addr.s_addr=opts->destIPaddr.s_addr;

	// Populate the 'args' struct
	args.sData=sData;
	args.opts=opts;

	// LaMP ID is randomly generated between 0 and 65535 (the maximum over 16 bits)
	lamp_id_session=(rand()+getpid())%UINT16_MAX;

	// This fprintf() terminates the series of call to inform the user about current settings -> using \n\n instead of \n
	fprintf(stdout,"\t[session LaMP ID] = %" PRIu16 "\n\n",lamp_id_session);

	// Start init procedure
	// Create INIT send and ACK listener threads
	pthread_create(&initSender_tid,NULL,&initSender,(void *) &args);
	pthread_create(&ackListenerInit_tid,NULL,&ackListenerInit,(void *) &(sData.descriptor));

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
	reportStructureFinalize(&reportData);
	printStats(&reportData,stdout,opts->confidenceIntervalMask);

	if(opts->filename!=NULL) {
		// If '-f' was specified, print the report data to a file too
		printStatsCSV(opts,&reportData,opts->filename);
	}

	// Returning 0 if everything worked fine
	return 0;
}