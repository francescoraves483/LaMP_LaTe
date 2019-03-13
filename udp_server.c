#include "udp_server_raw.h"
#include "report_manager.h"
#include "packet_structs.h"
#include "timeval_subtract.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include "common_thread.h"
#include "timer_man.h"
#include "common_udp.h"

#define CLEAR_ALL() pthread_mutex_destroy(&ack_report_received_mut);

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
static uint8_t ack_report_received; // Global flag set by the ackListener thread: = 1 when an ACK has been received, otherwise it is = 0

// Thread ID for ackListener
static pthread_t ackListener_tid;

// Mutex to protect the ack_report_received variable (as it written by a thread and read by another one, i.e. the main thread)
static pthread_mutex_t ack_report_received_mut;

// Function prototypes
static int transmitReportUDP(struct lampsock_data sData, struct options *opts);
extern inline int timevalSub(struct timeval *in, struct timeval *out);
static uint8_t ackSenderInit(arg_struct_udp *args);
static uint8_t initReceiver(struct lampsock_data *sData, uint64_t interval);

// Thread entry point functions
static void *ackListenerUDP(void *arg);

// As the report is periodically sent, a thread waits for an ACK message from the client. If it received, a proper variable is updated.
static void *ackListenerUDP (void *arg) {
	int *sFd=(int *) arg;
	controlRCVdata rcvData;
	int controlRcvRetValue;

	rcvData.session_id=lamp_id_session;

	controlRcvRetValue=controlReceiverUDP(*sFd,&rcvData,ACK,&ack_report_received,&ack_report_received_mut);

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

static uint8_t ackSenderInit(arg_struct_udp *args) {
	int controlSendRetValue;

	controlSendRetValue=controlSenderUDP(args,lamp_id_session,1,ACK,0,NULL,NULL);

	if(controlSendRetValue<0) {
		// Set error
		if(controlSendRetValue==-1) {
			t_tx_error=ERR_INVALID_ARG_CMONUDP;
		} else if(controlSendRetValue==-2) {
			t_tx_error=ERR_SEND_ACK;
		} else {
			t_rx_error=ERR_UNKNOWN;
		}
		return 1;
	}

	return 0;
}

static uint8_t initReceiver(struct lampsock_data *sData, uint64_t interval) {
	controlRCVdata rcvData;
	uint8_t return_val=0;
	int controlRcvRetValue;

	// struct timeval to set the more reasonable timeout after the first LaMP packet (i.e. the INIT packet) is received
	struct timeval rx_timeout_reasonable;

	controlRcvRetValue=controlReceiverUDP(sData->descriptor,&rcvData,INIT,NULL,NULL);

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
		// Set session data
		fprintf(stdout,"Server will accept all packets coming from client %s, id: %u\n",
			inet_ntoa(rcvData.controlRCV.ip),rcvData.controlRCV.session_id);

		// Set destination address inside the sendto sockaddr_in structure
		sData->addru.addrin[1].sin_addr.s_addr=rcvData.controlRCV.ip.s_addr;
		// Set the destination port inside the sendto sockaddr_in structure
		sData->addru.addrin[1].sin_port=rcvData.controlRCV.port;

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
			if(setsockopt(sData->descriptor, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout_reasonable, sizeof(rx_timeout_reasonable))!=0) {
				fprintf(stderr,"Warning: could not set RCVTIMEO: in case certain packets are lost,\n"
				"the program may run for an indefinite time and may need to be terminated with Ctrl+C.\n");
			}
		} else {
			return_val=1;
		}
	}

	return return_val;
}

static int transmitReportUDP(struct lampsock_data sData, struct options *opts) {
	// LaMP header and LaMP packet buffer
	struct lamphdr lampHeader;
	byte_t *lampPacket;

	// LaMP packet size container
	uint32_t lampPacketSize=0;

	// Report payload length and report buffer
	size_t report_payloadlen;
	char report_buff[REPORT_BUFF_SIZE]; // REPORT_BUFF_SIZE defined inside report_manager.h

	// for loop counter
	int counter=0;

	// Timer management variables
	struct pollfd timerMon;
	int clockFd;

	int poll_retval=1;

	int return_val=0; // = 0 if ok, = 1 if an error occurred

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Copying the report string inside the report buffer
	repprintf(report_buff,reportData);

	// Compute report payload length
	report_payloadlen=strlen(report_buff);

	// Allocating buffers
	lampPacket=malloc(sizeof(struct lamphdr)+report_payloadlen);
	if(!lampPacket) {
		return 2;
	}

	lampPacketSize=LAMP_HDR_PAYLOAD_SIZE(report_payloadlen);

	lampHeadPopulate(&lampHeader, CTRL_UNIDIR_REPORT, lamp_id_session, 0); // Starting back from sequence number equal to 0

	// Create thread for receiving the ACK from the client
	pthread_create(&ackListener_tid,NULL,&ackListenerUDP,(void *) &(sData.descriptor));

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

			// Prepare the LaMP packet
			lampEncapsulate(lampPacket, &lampHeader, (byte_t *) report_buff, report_payloadlen);

			// Set timestamp
			lampHeadSetTimestamp((struct lamphdr *)lampPacket);

			if(sendto(sData.descriptor,lampPacket,lampPacketSize,NO_FLAGS,(struct sockaddr *)&sData.addru.addrin[1],sizeof(sData.addru.addrin[1]))!=lampPacketSize) {
				perror("sendto() for sending LaMP packet failed");
				fprintf(stderr,"Failed sending report. Retrying in %d second(s).\n",REPORT_RETRY_INTERVAL_MS);
			}
		}

		poll_retval=poll(&timerMon,1,INDEFINITE_BLOCK);
		if(poll_retval>0) {
			// "Clear the event" by performing a read() on a junk variable
			read(clockFd,&junk,sizeof(junk));
		}
	
		// Successive attempts will have an increased sequence number
		lampHeadIncreaseSeq(&lampHeader);
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
	free(lampPacket);

	return return_val;
}

// Run UDP server: with respect to the client, only one loop is present, thus with no need to create other threads
// This also makes the code simpler.
// It basically works as the client's UDP Tx Loop, but inserted within the main function, with some if-else statements
// to discriminate the pinglike and unidirectional communications, making the common portion of the code to be written only once
unsigned int runUDPserver(struct lampsock_data sData, struct options *opts) {
	// Packet buffer with size = maximum LaMP packet length
	byte_t lampPacket[MAX_LAMP_LEN];
	// Pointer to the header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;

	// arg_struct_udp to be passed to ackSenderInit()
	arg_struct_udp args;

	// recvfrom variables
	ssize_t rcv_bytes;
	// struct sockaddr_in to store the source IP address of the received LaMP packets
	struct sockaddr_in srcAddr;
	socklen_t srcAddrLen=sizeof(srcAddr);

	// RX and TX timestamp containers
	struct timeval rx_timestamp, tx_timestamp;
	// Variable to store the latency (trip time)
	uint64_t tripTime;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx=0; // Initilized to 0 in order to be sure to enter the while loop
	uint16_t lamp_payloadlen_rx;

	// while loop continue flag
	int continueFlag=1;

	// SO_TIMESTAMP variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffer
	char ctrlBuf[CMSG_SPACE(sizeof(struct timeval))];

	// Very important: initialize to 0 any flag that is used inside threads
	ack_report_received=0;
	t_rx_error=NO_ERR;
	t_tx_error=NO_ERR;
	if(pthread_mutex_init(&ack_report_received_mut,NULL)!=0) {
		fprintf(stderr,"Could not allocate a mutex to synchronize the internal threads.\nThe execution will terminate now.\n");
		return 1;
	}

	// It should not be needed to reset also errno, as it should always be checked only when a real error occurs

	// Inform the user about the current options
	fprintf(stdout,"UDP server started, with options:\n\t[socket type] = UDP\n"
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

	// Prepare sendto sockaddr_in structure (index 1) for the server ('sin_addr' and 'sin_port' will be set later on, as the server receives its first packet from a client)
	bzero(&sData.addru.addrin[1],sizeof(sData.addru.addrin[1]));
	sData.addru.addrin[1].sin_family=AF_INET;

	// Perform INIT procedure
	if(initReceiver(&sData, opts->interval)) {
		thread_error_print("UDP server INIT receiver loop", t_rx_error);
		CLEAR_ALL()
		return 1;
	}

	// -L is ignored if a bidirectional INIT packet was received (it's the client that should compute the latency, not the server)
	if(mode_session!=UNIDIR && opts->latencyType!=USERTOUSER) {
		fprintf(stderr,"Warning: a latency type was specified (-L), but it will be ignored.\n");
	} else if(mode_session==UNIDIR && opts->latencyType==KRT) {
		// Set SO_TIMESTAMP
		// Check if the KRT mode is supported by the current NIC and set the proper socket options
		if (socketSetTimestamping(sData.descriptor)<0) {
		 	perror("socketSetTimestamping() error");
			fprintf(stderr,"Warning: SO_TIMESTAMP is probably not suppoerted. Switching back to user-to-user latency.\n");
			opts->latencyType=USERTOUSER;
		}

		// Prepare ancillary data structures, if KRT mode is selected
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

	// Fill the 'args' structure
	args.sData=sData;
	args.opts=opts;
	
	if(ackSenderInit(&args)) {
		thread_error_print("UDP server ACK sender loop", t_tx_error);
		CLEAR_ALL()
		return 1;
	}

	// Start receiving packets
	while(continueFlag) {
		// If in KRT unidirectional mode, use recvmsg(), otherwise, use recvfrom
		if(mode_session==UNIDIR && opts->latencyType==KRT) {
			saferecvmsg(rcv_bytes,sData.descriptor,&mhdr,NO_FLAGS);

			// Extract ancillary data
			for (cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
                if (cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMP) {
                    rx_timestamp=*((struct timeval *)CMSG_DATA(cmsg));
                }
			}
		} else {
			saferecvfrom(rcv_bytes,sData.descriptor,lampPacket,MAX_LAMP_LEN,NO_FLAGS,(struct sockaddr *)&srcAddr,&srcAddrLen);
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

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			continue;
		}

		// If the packet is really a LaMP packet, get the header data
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, &lamp_seq_rx, &lamp_payloadlen_rx, &tx_timestamp, NULL);

		// Discard any (end)reply, ack, init or report, at the moment
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

				fprintf(stdout,"Received a unidirectional message from %s (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
					inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(opts->latencyType));

				// Update the current report structure
				reportStructureUpdate(&reportData,tripTime,lamp_seq_rx);
			break;

			case PINGLIKE:
				// Transmit the reply as the copy of the request: first, receive the request (see above), then, encapsulate it inside a new LaMP packet 
				// and send the reply, after printing that a ping-like message with a certain id and sequence number has been received by the client
				fprintf(stdout,"Received a ping-like message from %s (id=%u, seq=%u, rx_bytes=%d). Replying to client...\n",
					inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);

				// Change reply type inside the lampPacket buffer (or ENDREPLY, if this is the last packet), just received
				// This can be done, without the need of preparing a new packet, by using the lampHeaderPtr pointer (see the definitions at the beginning of this function)
				lampHeaderPtr->ctrl = continueFlag==1 ? CTRL_PINGLIKE_REPLY : CTRL_PINGLIKE_ENDREPLY;

				// Send packet (as the reply does require to carry the client timestamp, the control field should now correspond to CTRL_PINGLIKE_REPLY)
				// 'rcv_bytes' still stores the packet size, thus it can be used as packet size to be passed to sendto()
				if(sendto(sData.descriptor,lampPacket,rcv_bytes,NO_FLAGS,(struct sockaddr *)&sData.addru.addrin[1],sizeof(sData.addru.addrin[1]))!=rcv_bytes) {
					perror("sendto() for sending LaMP packet failed");
					fprintf(stderr,"UDP server reported that it can't reply to the client with id=%u and seq=%u\n",lamp_id_rx,lamp_seq_rx);
				}
			break;

			default:
			break;
		}
	}

	if(mode_session==UNIDIR) {
		// If the mode is the unidirectional one, get the destination IP/MAC from the last packet
		// Use as destination IP (destIP), the source IP of the last received packet (headerptrs.ipHeader->saddr)
		if(transmitReportUDP(sData, opts)) {
			fprintf(stderr,"UDP server reported an error while transmitting the report.\n"
				"No report will be transmitted.\n");
			CLEAR_ALL();
			return 1;
		}
	}

	// Destroy mutex (as it is no longer needed) and clear all the other data that should be clared (see the CLEAR_ALL() macro)
	CLEAR_ALL();

	return 0;
}