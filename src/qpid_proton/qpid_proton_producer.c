#include "common_qpid_proton.h"
#include "qpid_proton_producer.h"
#include "report_manager.h"
#include "rawsock_lamp.h"
#include "timer_man.h"

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include <proton/connection.h>
#include <proton/delivery.h>
#include <proton/link.h>
#include <proton/session.h>
#include <proton/transport.h>

// A credit of '2' on the receving link should be enough,
//  as we expect only one ACK and one REPORT to be received
#define RX_LINK_CREDIT 2
#define DELIVERYTAG_UNIDIR_SEQ_SIZE 21

// Defines for pn_error_condition
#define ERROR_COND_NO_ERROR 0
#define ERROR_COND_AMQP_ERROR 1
#define ERROR_COND_TIMEOUT 2

static producer_status_t producerStatus=P_JUSTSTARTED;
static uint16_t lamp_id_session;

static int allocatePacketBuffers(struct amqp_data *aData,struct options *opts,byte_t **payload_buff) {
	// Allocating packet buffers (with and without payload)
	if(opts->payloadlen!=0) {
		aData->lampPacket=malloc(sizeof(struct lamphdr)+opts->payloadlen);
		if(!aData->lampPacket) {
			return -1;
		}

		aData->lampPacketSize=LAMP_HDR_PAYLOAD_SIZE(opts->payloadlen);
	} else {
		// There is no need to allocate the lamppacket buffer, as the LaMP header will be directly sent as message body
		aData->lampPacketSize=LAMP_HDR_SIZE();
	}

	// Allocate and fill the LaMP payload buffer (if a certain payload size was requested)
	if(opts->payloadlen!=0) {
		*payload_buff=malloc((opts->payloadlen)*sizeof(byte_t));

		if(!(*payload_buff)) {
			free(aData->lampPacket);

			return -1;
		}

		for(int i=0;i<opts->payloadlen;i++) {
			(*payload_buff)[i]=(byte_t) (i & 15);
		}
	}

	return 1;
}

static int amqpControlSender(lamptype_t type,pn_link_t *lnk,struct amqp_data *aData,struct options *opts) {
	struct lamphdr lampHeader;
	pn_data_t* message_body;

	// This check is done for safety reasons: this function shall not send any message different than INIT or ACK
	//  as other types are not yet supported for AMQP (i.e. no FOLLOWUP_CTRL)
	if(type!=ACK && type!=INIT) {
		return -1;
	}

	lampHeadPopulate(&lampHeader,TYPE_TO_CTRL(type),lamp_id_session,INITIAL_SEQ_NO);

	if(type==INIT) {
		lampHeadSetConnType(&lampHeader,opts->mode_ub);
	}

	pn_message_clear(aData->message);
	message_body=pn_message_body(aData->message);

	// Insert the LaMP packet inside AMQP
	pn_data_put_binary(message_body,pn_bytes(LAMP_HDR_SIZE(),(const char *) &lampHeader));

	if(qpid_lamp_message_send(aData->message,lnk,NULL,aData,opts)<0) {
		fprintf(stderr,"Error sending a message. Details:\n%s\n",
			pn_error_text(pn_message_error(aData->message)));
		return -1;
	}

	return 1;
}

static int amqpUNIDIRSenderSingleIter(struct lamphdr *commonLampHeader,unsigned int *counter,uint8_t *sendLast,byte_t *payload_buff,pn_link_t *lnk,struct amqp_data *aData,struct options *opts) {
	pn_data_t* message_body;
	char deliverytag_unidir_seq[DELIVERYTAG_UNIDIR_SEQ_SIZE];

	if(pn_link_credit(lnk)<=0) {
		fprintf(stderr,"Error sending packet with sequence number %d, not enough credit.\n",*counter);
		return -1;
	}

	// Set UNIDIR_STOP type if this is the last packet
	if(*counter==opts->number-1 || *sendLast==1) {
		lampSetUnidirStop(commonLampHeader);
		producerStatus=P_REPORTWAIT;
	}

	snprintf(deliverytag_unidir_seq,DELIVERYTAG_UNIDIR_SEQ_SIZE,"unidirdelivery_%05" PRIu16,*counter);
	pn_delivery(lnk,pn_dtag(deliverytag_unidir_seq,sizeof(deliverytag_unidir_seq)));

	pn_message_clear(aData->message);
	message_body=pn_message_body(aData->message);

	// Encapsulate LaMP payload only if it is requested
	if(opts->payloadlen!=0 && payload_buff!=NULL) {
		lampEncapsulate(aData->lampPacket,commonLampHeader,payload_buff,opts->payloadlen);
		// Set timestamp
		lampHeadSetTimestamp((struct lamphdr *)aData->lampPacket,NULL);
	} else {
		lampHeadSetTimestamp(commonLampHeader,NULL);
		 // The LaMP packet will be only composed by the header
		aData->lampPacket=(byte_t *)commonLampHeader;
	}

	// Insert the LaMP packet inside AMQP
	pn_data_put_binary(message_body,pn_bytes(aData->lampPacketSize,(const char *)aData->lampPacket));

	if(qpid_lamp_message_send(aData->message,lnk,NULL,aData,opts)<0) {
		fprintf(stderr,"Error sending a message. Details:\n%s\n",
			pn_error_text(pn_message_error(aData->message)));
		return -1;
	}

	fprintf(stdout,"Sent unidirectional AMQP transfer on link %s (id=%u, seq=%u)\n",
		pn_terminus_get_address(pn_link_target(lnk)),lamp_id_session,*counter);

	// Increase sequence number and counter for the next iteration
	lampHeadIncreaseSeq(commonLampHeader);
	(*counter)++;

	return 1;
}

static int amqpReportACKreceiver(lamptype_t type,reportStructure *reportDataPtr,pn_link_t *lnk,pn_delivery_t *d) {
	size_t rx_size;
	int isRightMsgReceived=0;

	// AMQP message buffer (size: maximum LaMP packet length + ADDITIONAL_AMQP_HEADER_MAX_SIZE)
	char amqp_message_buf[MAX_LAMP_LEN+ADDITIONAL_AMQP_HEADER_MAX_SIZE];

	// Structure containing the LaMP packet extracted from the AMQP message
	lampPacket_bytes_t lampPacketBytes;

	// Pointer to the LaMP header, inside the packet buffer
	struct lamphdr *lampHeaderPtr;
	// Pointer to the payload, inside the packet buffer
	//byte_t *lampPayloadPtr=lampPacket+LAMP_HDR_SIZE();
	byte_t *lampPayloadPtr;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;

	// Received AMQP data type
	pn_type_t amqp_type;

	// Get size of pending (received) message data
	rx_size=pn_delivery_pending(d);

	pn_link_recv(lnk,amqp_message_buf,rx_size);
	// If the delivery is not partial, nor aborted, proceeding with reading the message
	if(!pn_delivery_partial(d) && !pn_delivery_aborted(d)) {
		// Data has been processed
		pn_delivery_update(d,PN_ACCEPTED);
	    pn_delivery_settle(d);

	   	// Extract the LaMP packet from the AMQP message
	    amqp_type=lampPacketDecoder(amqp_message_buf,rx_size,&lampPacketBytes);

	    if(amqp_type!=PN_BINARY) {
	    	return 0;
	    }

	    lampHeaderPtr=(struct lamphdr *)lampPacketBytes.lampPacket.start;
	    lampPayloadPtr=((byte_t *)lampPacketBytes.lampPacket.start)+LAMP_HDR_SIZE();

		if(IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			lampHeadGetData((byte_t *)lampPacketBytes.lampPacket.start,&lamp_type_rx,&lamp_id_rx,NULL,NULL,NULL,NULL);

			if(lamp_type_rx==type && lamp_id_rx==lamp_id_session) {
				isRightMsgReceived=1;
				if(lamp_type_rx==REPORT && reportDataPtr!=NULL) {
					// We must now parse the report
					// Using & and * is a very dirty trick to allow the repscanf macro work even when reportDataPtr is a pointer
					//  to a reportStructure instead of a reportStructure (as, internally, the macro is using '.' to access the
					//  structure members instead of '->'). Shall be improved in the future.
					repscanf((const char *)lampPayloadPtr,&(*reportDataPtr));
				}
			}
		}
	}

    return isRightMsgReceived;
}

static int producerEventHandler(struct amqp_data *aData,struct options *opts,reportStructure *reportPtr,pn_event_t* handled_event) {
	pn_connection_t* connection;
	pn_session_t *s;
	static pn_link_t *l_rx;
	static pn_link_t *l_tx;
	const char deliverytag_init[]="initdelivery";
	const char deliverytag_ack[]="ackdelivery_prod";

	// while loop counter for sending -n test packets
	static unsigned int counter=0;
	static unsigned int batch_counter=1;
	// Common LaMP header for sending test packets
	struct lamphdr commonLampHeader;
	// LaMP payload buffer (which is equal for each time the producerEventHandler() is called)
	static byte_t *payload_buff=NULL;

	// Timer and poll() variables
	int clockFd;
	int timerCaS_res=0;
	int poll_retval=1;
	struct pollfd timerMon[2];
	static int pollfd_size=1;

	int durationClockFd;
	static uint8_t sendLast=0;

	// Junk variable (needed to clear the timer event with read()) - maybe a better solution avoiding this exists...
	unsigned long long junk;

	if(opts->verboseFlag) {
		fprintf(stdout,"[INFO] Current Qpid Proton event: %s\n",pn_event_type_name(pn_event_type(handled_event)));
	}

	// Reset timeout
	pn_proactor_set_timeout(aData->proactor,aData->proactor_timeout);

	switch(pn_event_type(handled_event)) {
		case PN_CONNECTION_INIT:
			connection=pn_event_connection(handled_event);
			s=pn_session(connection);

			pn_connection_set_container(connection,aData->containerID);

			pn_connection_open(connection);

			pn_session_open(s);

			l_tx=pn_sender(s,aData->senderName);
			pn_terminus_set_address(pn_link_target(l_tx),opts->queueNameTx);

			l_rx=pn_receiver(s,aData->receiverName);
			pn_terminus_set_address(pn_link_source(l_rx),opts->queueNameRx);

			// Open one link on a queue for sending messages (<queue name>_tx)
			//  and one on a queue for receiving messages (<queue name>_rx)
			pn_link_open(l_tx);
			pn_link_open(l_rx);

			// Grant enough credit on the receiving link
			pn_link_flow(l_rx,RX_LINK_CREDIT);
			break;

		case PN_LINK_FLOW:
			{
				
			pn_link_t *lnk=pn_event_link(handled_event);
			if(strcmp(pn_link_name(lnk),aData->senderName)==0) {
				// We must send something
				if(producerStatus==P_JUSTSTARTED) {
					// Send INIT
					if(pn_link_credit(lnk)<=0) {
						fprintf(stderr,"Error senduing INIT, not enough credit.\n");
						return -1;
					}
					pn_delivery(lnk,pn_dtag(deliverytag_init,sizeof(deliverytag_init)));
					if(amqpControlSender(INIT,lnk,aData,opts)==-1) {
						fprintf(stderr,"Error sending INIT. Send error.\n");
						return -1;
					}
					// Update status, as the INIT was sent
					producerStatus=P_INITSENT;
				}
			} else {
				// Just print a warning message, as PN_LINK_FLOW is not expected on receiving link (l_rx)
				fprintf(stdout,"Warning: unexpected PN_LINK_FLOW event occurred.\n");
			}

			}
			break;

		case PN_DELIVERY:
			{

			pn_link_t *lnk=pn_event_link(handled_event);
			if(strcmp(pn_link_name(lnk),aData->senderName)==0) {
				if(opts->verboseFlag) {
					fprintf(stdout,"[INFO] Producer is handling a PN_DELIVERY on link=%s\n",pn_link_name(lnk));
				}

				// Tx link
				// If we enter here, also according to the send.c example in the Qpid Proton C documentation, it is
				//  because we received an ack from the remote peer than a message was delivered

				// Getting the delivery associated with the event
				pn_delivery_t* e_delivery=pn_event_delivery(handled_event);

				if(producerStatus==P_SENDING) {
					if(opts->verboseFlag) {
						fprintf(stdout,"[INFO] Producer is handling a readable PN_DELIVERY on link=%s, with producer status=P_SENDING\n",pn_link_name(lnk));
					}

					// Wait for timer expiration
					poll_retval=poll(timerMon,pollfd_size,INDEFINITE_BLOCK);
					if(poll_retval>0) {
						if(opts->duration_interval!=0 && timerMon[1].revents>0) {
							if(read(durationClockFd,&junk,sizeof(junk))==-1) {
								fprintf(stderr,"Error: unable to read an event from the test duration timer.\nIt is not safe to continue the current test from packet: %d.\n",counter);
								return -4;
							}
	
							timerStop(&durationClockFd);
							sendLast=1;
						}

						// "Clear the event" by performing a read() on a junk variable
						if(timerMon[0].revents>0 && read(clockFd,&junk,sizeof(junk))==-1) {
							fprintf(stderr,"Error: unable to read a timer event before sending a packet.\nIt is not safe to continue the current test from packet: %d.\n",counter);
							return -4;
						}
					}

					// Rearm timer with a random timeout if '-R' was specified
					if(sendLast!=1 && opts->rand_type!=NON_RAND && batch_counter==opts->rand_batch_size) {
						if(timerRearmRandom(clockFd,opts)<0) {
							fprintf(stderr,"Error: unable to set random interval with distribution %s\n",enum_to_str_rand_distribution_t(opts->rand_type));
							return -3;
						}
						batch_counter=0;
					}

					// Continue sending packets. The producerStatus is automatically updated to P_REPORTWAIT when
					// the last LaMP packet is sent.
					if(amqpUNIDIRSenderSingleIter(&commonLampHeader,&counter,&sendLast,payload_buff,l_tx,aData,opts)==-1) {
						fprintf(stderr,"Error: unable to send packet with sequence number %d\n",counter);
						return -1;
					}

					batch_counter++;
				}
					
				// Checking the delivery remote state; if PN_ACCEPTED (delivery successfully processed), do nothing,
				//  as everything is ok, otherwise print an error message and terminate the current session
				if(pn_delivery_remote_state(e_delivery)!=PN_ACCEPTED) {
					fprintf(stderr,"Error: unexpected delivery state: %s.\n",pn_disposition_type_name(pn_delivery_remote_state(e_delivery)));
					pn_connection_close(pn_event_connection(handled_event));
					return -1;
				}
			} else {
				// Rx link: we need to receive something
				pn_delivery_t* e_delivery=pn_event_delivery(handled_event);

				if(opts->verboseFlag) {
					fprintf(stdout,"[INFO] Producer is handling a PN_DELIVERY on link=%s\n",pn_link_name(lnk));
				}

				if(pn_delivery_readable(e_delivery)) {
					// Check if the link is the right one
					pn_link_t *lnk_d=pn_delivery_link(e_delivery);

					if(strcmp(pn_link_name(lnk),pn_link_name(lnk_d))!=0) {
						fprintf(stderr,"Error: mismatching link name associated to the current delivery.\n"
							"Expected: %s. Got: %s.\n",pn_link_name(lnk),pn_link_name(lnk_d));
						break;
					}

					if(producerStatus==P_INITSENT) {
						if(opts->verboseFlag) {
							fprintf(stdout,"[INFO] Producer is handling a readable PN_DELIVERY on link=%s, with producer status=P_INITSENT\n",pn_link_name(lnk));
						}

						if(amqpReportACKreceiver(ACK,NULL,lnk,e_delivery)) {
							// We received a correct ACK: we can update the status of the producer
							producerStatus=P_ACKRECEIVED;
						}

						// We can now start sending packets
						// Populate a LaMP Header which will be common for all LaMP packets,
						// except for sequence number and, for the last one, type
						lampHeadPopulate(&commonLampHeader,opts->number==1 ? CTRL_UNIDIR_STOP : CTRL_UNIDIR_CONTINUE,lamp_id_session,0);

						if(allocatePacketBuffers(aData,opts,&payload_buff)==-1) {
							fprintf(stderr,"Error: cannot allocate memory.\n");
							return -1;
						};

						// Create and start timer
						timerCaS_res=timerCreateAndSet(&timerMon[0], &clockFd, opts->interval);

						if(timerCaS_res==-1) {
							return -1;
						} else if(timerCaS_res==-2) {
							return -1;
						}

						// Create and start test duration timer (if required only - i.e. if the user specified -i)
						if(opts->duration_interval>0 || opts->seconds_to_end!=-1) {
							pollfd_size=2;
							
							if(opts->seconds_to_end!=-1) {
								setTestDurationEndTime(opts);
							}			

							timerCaS_res=timerCreateAndSet(&timerMon[1],&durationClockFd,opts->duration_interval*1000);
					
							if(timerCaS_res==-1) {
								return -1;
							} else if(timerCaS_res==-2) {
								return -1;
							}
						} else {
							pollfd_size=1;
						}

						// Ready to send packets: update status
						producerStatus=P_SENDING;

						// Start sending the first test packet
						if(amqpUNIDIRSenderSingleIter(&commonLampHeader,&counter,&sendLast,payload_buff,l_tx,aData,opts)==-1) {
							fprintf(stderr,"Error: unable to send packet with sequence number %d\n",counter);
							return -1;
						}
					} else if(producerStatus==P_REPORTWAIT) {
						// Before waiting for the report, as the last packet should have been sent when the status is P_REPORTWAIT,
						// set set the report's total packets value to the amount of packets sent during this test, if -i is used
						if(opts->duration_interval!=0) {
							reportStructureChangeTotalPackets(reportPtr,counter);
						}
						if(opts->verboseFlag) {
							fprintf(stdout,"[INFO] Producer is handling a readable PN_DELIVERY on link=%s, with producer status=P_REPORTWAIT\n",pn_link_name(lnk));
						}

						if(amqpReportACKreceiver(REPORT,reportPtr,lnk,e_delivery)) {
							// We received and parsed correct REPORT: we can update the status of the producer
							producerStatus=P_REPORTRECEIVED;
						}

						// Send ACK, if there is enought credit to do so (but it is expected to have it)
						if(pn_link_credit(l_tx)<=0) {
							fprintf(stderr,"Warning: error sending ACK, not enough credit.\n");
							return -1;
						}
						pn_delivery(l_tx,pn_dtag(deliverytag_ack,sizeof(deliverytag_ack)));
						if(amqpControlSender(ACK,l_tx,aData,opts)==-1) {
							fprintf(stderr,"Error: cannot send final ACK.\n");
							//return -1;
						}

						producerStatus=P_ACKSENT;
						}
				} else {
					fprintf(stderr,"Warning: current delivery on receiving link is not readable.\n");
				}
			}

			}
			break;

		case PN_TRANSPORT_CLOSED:
			return qpid_condition_check_and_close_connection(handled_event,pn_transport_condition(pn_event_transport(handled_event)));

		case PN_CONNECTION_REMOTE_CLOSE:
			return qpid_condition_check_and_close_connection(handled_event,pn_connection_remote_condition(pn_event_connection(handled_event)));

		case PN_SESSION_REMOTE_CLOSE:
			return qpid_condition_check_and_close_connection(handled_event,pn_session_remote_condition(pn_event_session(handled_event)));

		case PN_LINK_REMOTE_CLOSE:
   		case PN_LINK_REMOTE_DETACH:
   			return qpid_condition_check_and_close_connection(handled_event,pn_link_remote_condition(pn_event_link(handled_event)));

		case PN_PROACTOR_INACTIVE:
			return -1;

		case PN_PROACTOR_TIMEOUT:
			return -2;

		default:
			break;
	}

	return 1;
}

unsigned int runAMQPproducer(struct amqp_data aData, struct options *opts, report_sock_data_t *sock_w_data) {
	// LaMP parameters
	reportStructure reportData;

	// Error flag for the main event loop
	uint8_t pn_error_condition=ERROR_COND_NO_ERROR;

	// Event handler return value
	int pn_eventhdlr_retval=0;

	// Inform the user about the current options
	fprintf(stdout,"Qpid Proton AMQP client started, with options:\n\t[node] = producer (LaMP client role)\n"
		"\t[interval] = %" PRIu64 " ms\n"
		"\t[reception timeout] = %" PRIu32 " ms\n",
		opts->interval, aData.proactor_timeout);

	if(opts->duration_interval!=0) {
		fprintf(stdout,"\t[test duration] = %" PRIu32 " s\n",
			opts->duration_interval);
	} else {
		fprintf(stdout,"\t[total number of packets] = %" PRIu64 "\n",
			opts->number);
	}

	fprintf(stdout,"\t[mode] = %s\n"
		"\t[payload length] = %" PRIu16 " B \n"
		"\t[broker address] = %s\n"
		"\t[latency type] = %s\n"
		"\t[follow-up] = not supported\n"
		"\t[random interval] = %s\n",
		opts->mode_ub==UNIDIR ? "unidirectional" : "ping-like", 
		opts->payloadlen, opts->dest_addr_u.destAddrStr,
		latencyTypePrinter(opts->latencyType),
		opts->rand_type==NON_RAND ? "fixed periodic" : enum_to_str_rand_distribution_t(opts->rand_type));

	if(opts->rand_type==NON_RAND) {
		fprintf(stdout,"\t[random interval batch] = -\n");
	} else {
		fprintf(stdout,"\t[random interval batch] = %" PRIu64 "\n",
			opts->rand_batch_size);
	}

	// LaMP ID is randomly generated between 0 and 65535 (the maximum over 16 bits)
	lamp_id_session=(rand()+getpid())%UINT16_MAX;

	// This fprintf() terminates the series of call to inform the user about current settings -> using \n\n instead of \n
	fprintf(stdout,"\t[session LaMP ID] = %" PRIu16 "\n\n",lamp_id_session);

	// Initialize the report structure
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType, opts->followup_mode, opts->dup_detect_enabled);

	// Set container ID, sender name and received name, depending on the chosen LaMP ID
	snprintf(aData.containerID,CONTAINERID_LEN,"LaTe_prod_%05" PRIu16,lamp_id_session);
	snprintf(aData.senderName,SENDERNAME_LEN,"LaTe_prod_tx_%05" PRIu16,lamp_id_session);
	snprintf(aData.receiverName,RECEIVERNAME_LEN,"LaTe_prod_rx_%05" PRIu16,lamp_id_session);

	if(opts->duration_interval>0) {
		opts->number=UINT64_MAX;
	}

	// Start waiting for events
	do {
		pn_event_batch_t *events_batch=pn_proactor_wait(aData.proactor);
		for(pn_event_t *e=pn_event_batch_next(events_batch);e!=NULL;e=pn_event_batch_next(events_batch)) {
			pn_eventhdlr_retval=producerEventHandler(&aData,opts,&reportData,e);
			if(pn_eventhdlr_retval==-1) {
				pn_error_condition=ERROR_COND_AMQP_ERROR;
			} else if(pn_eventhdlr_retval==-2) {
				fprintf(stdout,"Error: timeout occurred.\n");
				pn_error_condition=ERROR_COND_TIMEOUT;
			}
		}
		pn_proactor_done(aData.proactor,events_batch);
	} while(!pn_error_condition && producerStatus!=P_ACKSENT);

	if(pn_error_condition==ERROR_COND_NO_ERROR) {
		/* Ok, the mode_ub==UNSET_UB case is not managed, but it should never happen to reach this point
		with an unset mode... at least not without getting errors or a chaotic thread behaviour! But it should not happen anyways. */
		fprintf(stdout,opts->mode_ub==PINGLIKE?"Ping-like ":"Unidirectional " "statistics:\n");
		// Print the statistics, if no error, before returning
		reportStructureFinalize(&reportData);
		printStats(&reportData,stdout,opts->confidenceIntervalMask);
	} 


	if(pn_error_condition==ERROR_COND_NO_ERROR || pn_error_condition==ERROR_COND_TIMEOUT) {
		if(opts->filename!=NULL) {
			// If '-f' was specified, print the report data to a file too
			printStatsCSV(opts,&reportData,opts->filename);
		}

		if(opts->udp_params.enabled) {
			// If '-w' was specified, send the report data inside a TCP packet, through a socket
			printStatsSocket(opts,&reportData,sock_w_data,lamp_id_session);
		}
	}

	reportStructureFree(&reportData);

	// Free Qpid proton allocated memory
	pn_proactor_free(aData.proactor);
	pn_message_free(aData.message);

	// Returning 0 if everything worked fine
	return pn_error_condition;
}