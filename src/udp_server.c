#include "udp_server_raw.h"
#include "report_manager.h"
#include "packet_structs.h"
#include "timeval_utils.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <linux/errqueue.h>
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
static modefollowup_t followup_mode_session;
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

	controlSendRetValue=controlSenderUDP(args,lamp_id_session,1,ACK,0,0,NULL,NULL);

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
			lampHeadSetTimestamp((struct lamphdr *)lampPacket,NULL);

			if(sendto(sData.descriptor,lampPacket,lampPacketSize,NO_FLAGS,(struct sockaddr *)&sData.addru.addrin[1],sizeof(sData.addru.addrin[1]))!=lampPacketSize) {
				perror("sendto() for sending LaMP packet failed");
				fprintf(stderr,"Failed sending report. Retrying in %d second(s).\n",REPORT_RETRY_INTERVAL_MS);
			}
		}

		poll_retval=poll(&timerMon,1,INDEFINITE_BLOCK);
		if(poll_retval>0) {
			// "Clear the event" by performing a read() on a junk variable
			if(read(clockFd,&junk,sizeof(junk))==-1) {
				fprintf(stderr,"Failed to read a timer event when sending the report. The next attempts may fail.\n");
			}
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
	byte_t lampPacket[MAX_LAMP_LEN+LAMP_HDR_SIZE()];
	// Pointer to the header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;
	// Pointer to the LaMP packet inside a UDP raw packet (used only in HARDWARE mode when retrieving tx timestamp through socket error queue)
	byte_t *lampPacketPtr=NULL;

	// arg_struct_udp to be passed to ackSenderInit()
	arg_struct_udp args;

	// recvfrom variables
	ssize_t rcv_bytes;
	// struct sockaddr_in to store the source IP address of the received LaMP packets
	struct sockaddr_in srcAddr;
	socklen_t srcAddrLen=sizeof(srcAddr);

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

	// while loop continue flag
	int continueFlag=1;

	// HARDWARE mode scm_timestamping structure
	struct scm_timestamping hw_ts;

	// SO_TIMESTAMP variables and structs (cmsg)
	struct msghdr mhdr;
	struct iovec iov;
	struct cmsghdr *cmsg = NULL;

	// Ancillary data buffers
	char ctrlBufHw[CMSG_SPACE(sizeof(struct scm_timestamping))];
	char ctrlBufSw[CMSG_SPACE(sizeof(struct timeval))];

	// Follow-up flag: it is used to discard any possibile follow-up request after the first one,
	//  when a client attempts to establish an hardware timers session
	uint8_t isnotfirst_FU=0;
	// Follow-up reply type: to be used only when a follow-up request is received from the client
	uint16_t followup_reply_type;

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

	ack_report_received=0;
	followup_mode_session=FOLLOWUP_OFF;
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
		"\t[timeout] = %" PRIu64 " ms\n"
		"\t[follow-up] = %s\n",
		opts->port,
		opts->interval<=MIN_TIMEOUT_VAL_S ? MIN_TIMEOUT_VAL_S : opts->interval,
		opts->refuseFollowup==1 ? "refused" : "accepted");

	// Print current UP
	if(opts->macUP==UINT8_MAX) {
		fprintf(stdout,"\t[user priority] = unset or unpatched kernel.\n\n");
	} else {
		fprintf(stdout,"\t[user priority] = %d\n\n",opts->macUP);
	}

	// Report structure inizialization
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType, opts->followup_mode);

	// Prepare sendto sockaddr_in structure (index 1) for the server ('sin_addr' and 'sin_port' will be set later on, as the server receives its first packet from a client)
	memset(&sData.addru.addrin[1],0,sizeof(sData.addru.addrin[1]));
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
		if (socketSetTimestamping(sData,SET_TIMESTAMPING_SW_RX)<0) {
		 	perror("socketSetTimestamping() error");
			fprintf(stderr,"Warning: SO_TIMESTAMP is probably not supported. Switching back to user-to-user latency.\n");
			opts->latencyType=USERTOUSER;
		}

		// Prepare ancillary data structures
		memset(&mhdr,0,sizeof(mhdr));

		// iovec buffers (scatter/gather arrays)
		iov.iov_base=lampPacket;
		iov.iov_len=sizeof(lampPacket);

		// Socket address structure
		mhdr.msg_name=&(srcAddr);
		mhdr.msg_namelen=srcAddrLen;

		// Ancillary data (control message)
		mhdr.msg_control=ctrlBufSw;
		mhdr.msg_controllen=sizeof(ctrlBufSw);

		// iovec arrays
		mhdr.msg_iov=&iov;
		mhdr.msg_iovlen=1; // 1 element for each recvmsg()

		// As reported into socket.h, these are the "flags on received message"
		mhdr.msg_flags=NO_FLAGS;
	}

	// Fill the 'args' structure
	args.sData=sData;
	args.opts=opts;

	// Open CSV file when '-W' is specified (as this only applies to the unidirectional mode, no file is create when the mode is not unidirectional)
	if(opts->Wfilename!=NULL && mode_session==UNIDIR) {
		Wfiledescriptor=openTfile(opts->Wfilename,opts->overwrite_W,opts->followup_mode!=FOLLOWUP_OFF,opts->report_extra_data);
		if(Wfiledescriptor<0) {
			fprintf(stderr,"Warning! Cannot open file for writing single packet latency data.\nThe '-W' option will be disabled.\n");
		}
	}
	
	if(ackSenderInit(&args)) {
		thread_error_print("UDP server ACK sender loop", t_tx_error);
		CLEAR_ALL()
		return 1;
	}

	// Start receiving packets
	while(continueFlag) {
		// If in KRT unidirectional/follow-up mode or in HARDWARE/SOFTWARE mode (requested by the client through a follow-up control message, use recvmsg(), otherwise, use recvfrom()
		if((mode_session==UNIDIR && opts->latencyType==KRT) || followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN || followup_mode_session==FOLLOWUP_ON_KRN_RX) {
			saferecvmsg(rcv_bytes,sData.descriptor,&mhdr,NO_FLAGS);

			// Extract ancillary data
			for(cmsg=CMSG_FIRSTHDR(&mhdr);cmsg!=NULL;cmsg=CMSG_NXTHDR(&mhdr, cmsg)) {
				// KRT (unidirectional) mode
                if((opts->latencyType==KRT || followup_mode_session==FOLLOWUP_ON_KRN_RX) && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMP) {
                    rx_timestamp=*((struct timeval *)CMSG_DATA(cmsg));
                }

                // HARDWARE/SOFTWARE (kernel tx+rx) mode
               	if((followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN) && cmsg->cmsg_level==SOL_SOCKET && cmsg->cmsg_type==SO_TIMESTAMPING) {
					hw_ts=*((struct scm_timestamping *)CMSG_DATA(cmsg));
	    			rx_timestamp.tv_sec=hw_ts.ts[followup_mode_session==FOLLOWUP_ON_HW ? 2 : 0].tv_sec;
	 				rx_timestamp.tv_usec=hw_ts.ts[followup_mode_session==FOLLOWUP_ON_HW ? 2 : 0].tv_nsec/MICROSEC_TO_NANOSEC;
	           	}
			}
		} else {
			saferecvfrom(rcv_bytes,sData.descriptor,lampPacket,MAX_LAMP_LEN,NO_FLAGS,(struct sockaddr *)&srcAddr,&srcAddrLen);
		}

		// Try to estimate the receive timestamp, in FOLLOWUP_ON_APP mode, in the best way possible and as soon as possible
		// Advantage: putting this code here allows getting a userspace timestamp as soon as the packet is received
		// Drawback: a rx_timestamp will be written for every received packet, even non-LaMP packets (provided that they can be
		// received through the UDP socket); in this case the gathered value will be ignored by the program
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
				fprintf(stderr,"Generic recvfrom() error. errno = %d.\n",errno);
				break;
			}
		}

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		// The packet is also discarded is the program receives less bytes, in the UDP payload, than
		//  the number of bytes in a LaMP header
		if(rcv_bytes<LAMP_HDR_SIZE() || !IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			continue;
		}

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

							iov.iov_base=lampPacket;
							iov.iov_len=sizeof(lampPacket);

							mhdr.msg_name=&(srcAddr);
							mhdr.msg_namelen=srcAddrLen;

							mhdr.msg_control=ctrlBufSw;
							mhdr.msg_controllen=sizeof(ctrlBufSw);

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
							followup_mode_session=(lamp_payloadlen_rx==FOLLOWUP_REQUEST_T_HW ? FOLLOWUP_ON_HW : FOLLOWUP_ON_KRN);

							memset(&mhdr,0,sizeof(mhdr));

							iov.iov_base=lampPacket;
							iov.iov_len=sizeof(lampPacket);

							mhdr.msg_name=&(srcAddr);
							mhdr.msg_namelen=srcAddrLen;

							mhdr.msg_control=ctrlBufHw;
							mhdr.msg_controllen=sizeof(ctrlBufHw);

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

			// Send follow-up reply
			controlSenderUDP(&args,lamp_id_session,1,FOLLOWUP_CTRL,followup_reply_type,0,NULL,NULL);

			isnotfirst_FU=1;

			continue;
		}

		if(isnotfirst_FU==0) {
			isnotfirst_FU=1;
		}

		if(lamp_type_rx==FOLLOWUP_CTRL) {
			// In a normal networking situation (i.e. with no packets out of order as the session is started) we should never reach this point
			fprintf(stdout,"Ignoring a follow-up request from %s (id=%u)..\n",
					inet_ntoa(srcAddr.sin_addr),lamp_id_rx);

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
					fprintf(stderr,"Error: negative latency (-%.3f ms - %s) for packet from %s (id=%u, seq=%u, rx_bytes=%d)!\nThe clock synchronization is not sufficienty precise to allow unidirectional measurements.\n",
						(double) (rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec)/1000,latencyTypePrinter(opts->latencyType),
						inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
					tripTime=0;
				} else {
					tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
				}

				if(tripTime!=0) {
					fprintf(stdout,"Received a unidirectional message from %s (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
						inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes,(double)tripTime/1000,latencyTypePrinter(opts->latencyType));
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
			break;

			case PINGLIKE:
				// Transmit the reply as the copy of the request: first, receive the request (see above), then, encapsulate it inside a new LaMP packet 
				// and send the reply, after printing that a ping-like message with a certain id and sequence number has been received by the client

				// Print here that a packet was received as default behaviour
				if(!opts->printAfter) {
					fprintf(stdout,"Received a ping-like message from %s (id=%u, seq=%u, rx_bytes=%d). Replying to client...\n",
						inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
				}

				// Change reply type inside the lampPacket buffer (or ENDREPLY, if this is the last packet), just received
				// This can be done, without the need of preparing a new packet, by using the lampHeaderPtr pointer (see the definitions at the beginning of this function)
				if(lamp_type_rx==PINGLIKE_REQ || lamp_type_rx==PINGLIKE_ENDREQ) {
					lampHeaderPtr->ctrl = continueFlag==1 ? CTRL_PINGLIKE_REPLY : CTRL_PINGLIKE_ENDREPLY;
				} else if(lamp_type_rx==PINGLIKE_REQ_TLESS || lamp_type_rx==PINGLIKE_ENDREQ_TLESS) {
					lampHeaderPtr->ctrl = continueFlag==1 ? CTRL_PINGLIKE_REPLY_TLESS : CTRL_PINGLIKE_ENDREPLY_TLESS;
				}

				lamp_type_tx=CTRL_TO_TYPE(lampHeaderPtr->ctrl);

				// If using application level or kernel level RX follow-up mode, gather the tx timestamp just before sending the packet
				if(followup_mode_session==FOLLOWUP_ON_APP || followup_mode_session==FOLLOWUP_ON_KRN_RX) {
					gettimeofday(&tx_timestamp,NULL);
				}

				// Send packet (as the reply does require to carry the client timestamp, the control field should now correspond to CTRL_PINGLIKE_REPLY)
				// 'rcv_bytes' still stores the packet size, thus it can be used as packet size to be passed to sendto()
				if(sendto(sData.descriptor,lampPacket,rcv_bytes,NO_FLAGS,(struct sockaddr *)&sData.addru.addrin[1],sizeof(sData.addru.addrin[1]))!=rcv_bytes) {
					perror("sendto() for sending LaMP packet failed");
					fprintf(stderr,"UDP server reported that it can't reply to the client with id=%u and seq=%u\n",lamp_id_rx,lamp_seq_rx);
				}

				// Print here that a packet was received if -1 was specified
				if(opts->printAfter) {
					fprintf(stdout,"Received a ping-like message from %s (id=%u, seq=%u, rx_bytes=%d). Reply sent to client...\n",
						inet_ntoa(srcAddr.sin_addr),lamp_id_rx,lamp_seq_rx,(int)rcv_bytes);
				}

				// If in hardware/software follow-up mode, gather the tx_timestamp from ancillary data
				if(followup_mode_session==FOLLOWUP_ON_HW || followup_mode_session==FOLLOWUP_ON_KRN) {
					// Loop until the right transmitted packet is received (as other packets are sent too, for which we are not
					//  interested in obtaining any tx timestamp - e.g. all the follow-up data tx timestamps are useless in our case)
					do {
						if(pollErrqueueWait(sData.descriptor,POLL_ERRQUEUE_WAIT_TIMEOUT)<=0) {
							rcv_bytes=-1;
							break;
						}
						saferecvmsg(rcv_bytes,sData.descriptor,&mhdr,MSG_ERRQUEUE);
						lampPacketPtr=UDPgetpacketpointers(lampPacket,NULL,NULL,NULL); // From Rawsock library
						lampHeadGetData(lampPacketPtr,&lamp_type_rx_errqueue,NULL,&lamp_seq_rx_errqueue,NULL,NULL,NULL);
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
						fprintf(stdout,"Sending follow-up data (id=%u, seq=%u). Processing delta: %.3f ms.\n",lamp_id_rx,lamp_seq_rx,((double) tx_timestamp.tv_sec)*SEC_TO_MILLISEC+((double) tx_timestamp.tv_usec)/MICROSEC_TO_MILLISEC);
					}

					// Send follow-up with the time difference timestamp
					if(sendFollowUpData(sData,lamp_id_rx,lamp_seq_rx,tx_timestamp)) {
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
		if(Wfiledescriptor>0) {
			closeTfile(Wfiledescriptor);
		}

		if(opts->udp_params.enabled) {
			// If '-w' was specified and the mode is undirectional, send a LateEND empty packet to make the 
			// receiving application stop reading the data; this empty packet is triggered by setting the 
			// second argument (report) to NULL
			printStatsSocket(opts,NULL,&(sData.sock_w_data),lamp_id_session);
		}

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