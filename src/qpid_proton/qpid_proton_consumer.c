#include "common_qpid_proton.h"
#include "qpid_proton_consumer.h"
#include "report_manager.h"
#include "rawsock_lamp.h"
#include "timer_man.h"
#include "timeval_utils.h"

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

#include <proton/connection.h>
#include <proton/delivery.h>
#include <proton/link.h>
#include <proton/session.h>
#include <proton/transport.h>

#define BATCH_CREDIT_SIZE 1200

// Local global variables
static consumer_status_t consumerStatus;
static uint16_t lamp_id_session;
static modeub_t mode_session;

static int amqpACKReportSender(lamptype_t type,pn_link_t *lnk,struct amqp_data *aData,struct options *opts,reportStructure *reportPtr) {
	pn_data_t* message_body;

	// Report payload length and report buffer
	size_t report_payloadlen;
	char report_buff[REPORT_BUFF_SIZE]; // REPORT_BUFF_SIZE defined inside report_manager.h

	// LaMP header and LaMP packet buffer
	struct lamphdr lampHeader;
	byte_t *lampPacket;

	// LaMP packet size container
	uint32_t lampPacketSize=0;

	// This check is done for safety reasons: this function shall not send any message different than REPORT or ACK
	//  as other types are not yet supported for AMQP (i.e. no FOLLOWUP_CTRL)
	if((type!=ACK && type!=REPORT) || (type==REPORT && reportPtr==NULL)) {
		return -1;
	}

	if(type==REPORT) {
		// Allocating buffers
		lampPacket=malloc(sizeof(struct lamphdr)+REPORT_BUFF_SIZE);
		if(!lampPacket) {
			return -1;
		}

		// Copying the report string inside the report buffer
		repprintf(report_buff,(*reportPtr));

		// Compute report payload length
		report_payloadlen=strlen(report_buff);
	}

	lampHeadPopulate(&lampHeader,TYPE_TO_CTRL(type),lamp_id_session,0);

	if(type==REPORT) {
		lampPacketSize=LAMP_HDR_PAYLOAD_SIZE(report_payloadlen);

		// Prepare the LaMP packet
		lampEncapsulate(lampPacket,&lampHeader,(byte_t *) report_buff,report_payloadlen);
	} else {
		lampPacketSize=LAMP_HDR_SIZE();
		lampPacket=(byte_t *)&lampHeader;
	}

	pn_message_clear(aData->message);
	message_body=pn_message_body(aData->message);

	// Insert the LaMP packet inside AMQP
	pn_data_put_binary(message_body,pn_bytes(lampPacketSize,(const char *) lampPacket));

	if(qpid_lamp_message_send(aData->message,lnk,NULL,aData,opts)<0) {
		if(type==REPORT) free(lampPacket);
		return -1;
	}

	if(type==REPORT) free(lampPacket);

	return 1;
}

static int amqpUNIDIRReceiver(pn_link_t *lnk,pn_delivery_t *d,struct amqp_data *aData,struct options *opts, reportStructure *reportPtr) {
	size_t rx_size;
	// AMQP message buffer (size: maximum LaMP packet length + LAMP_PACKET_OFFSET bytes of AMQP header)
	char amqp_message_buf[MAX_LAMP_LEN+LAMP_PACKET_OFFSET];

	// Pointer to LaMP packet buffer with size = maximum LaMP packet length
	byte_t *lampPacket=(byte_t *) &(amqp_message_buf[0])+LAMP_PACKET_OFFSET;
	// Pointer to the LaMP header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;

	// RX and TX timestamp containers
	struct timeval rx_timestamp={.tv_sec=0,.tv_usec=0}, tx_timestamp={.tv_sec=0,.tv_usec=0};
	// Variable to store the latency (trip time)
	uint64_t tripTime;

	// LaMP relevant fields (timestamp structures are defined above)
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_seq_rx=0; // Initilized to 0 in order to be sure to enter the while loop

	// Return value (i.e. a flag telling whether to continue receiving UNIDIR messages or not)
	int continueFlag=1;

	// timevalSub() return value (to check whether the result of a timeval subtraction is negative)
	int timevalSub_retval=0;

	// Get size of pending (received) message data
	rx_size=pn_delivery_pending(d);

	if(opts->verboseFlag) {
		fprintf(stdout,"[INFO] Received LaMP message size=%zu\n",rx_size-LAMP_PACKET_OFFSET);
	}

	pn_link_recv(lnk,amqp_message_buf,rx_size);
	// If the delivery is not partial, nor aborted, proceeding with reading the message
	if(!pn_delivery_partial(d) && !pn_delivery_aborted(d)) {
		// Data has been processed
		pn_delivery_update(d,PN_ACCEPTED);
	    pn_delivery_settle(d);

		if(IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			// If the packet is really a LaMP packet, get the header data, including the embedded timestamp
			lampHeadGetData(lampPacket,&lamp_type_rx,&lamp_id_rx,&lamp_seq_rx,NULL,&tx_timestamp,NULL);

			if(opts->verboseFlag) {
				display_packet("[INFO] LaMP packet content",lampPacket,rx_size-LAMP_PACKET_OFFSET);
			}

			// Only UNIDIR packets are supported as of now, as only the unidirectional mode is available for AMQP 1.0
			if(lamp_type_rx!=UNIDIR_CONTINUE && lamp_type_rx!=UNIDIR_STOP) {
				return 2;
			}

			// Check whether the id is of interest or not; if it is not, just return with a value of '0' (i.e. ignored packet)
			if(lamp_id_rx!=lamp_id_session) {
				return 2;
			}

			// When UNIDIR_STOP is received, set the continueFlag to 0, to exit the loop after the current iteration
			if(lamp_type_rx==UNIDIR_STOP) {
				continueFlag=0;
			}

			if(opts->latencyType==USERTOUSER) {
				gettimeofday(&rx_timestamp,NULL);
			}

			timevalSub_retval=timevalSub(&tx_timestamp,&rx_timestamp);
			if(timevalSub_retval) {
				fprintf(stderr,"Error: negative latency (-%.3f ms - %s) for packet from queue/topic %s (id=%u, seq=%u, rx_bytes=%d)!\nThe clock synchronization is not sufficienty precise to allow unidirectional measurements.\n",
						(double) (rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec)/1000,latencyTypePrinter(opts->latencyType),
						pn_terminus_get_address(pn_link_source(lnk)),lamp_id_rx,lamp_seq_rx,(int)rx_size-LAMP_PACKET_OFFSET);
				tripTime=0;
			} else {
				tripTime=rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec;
			}

			if(tripTime!=0) {
				fprintf(stdout,"Received a unidirectional message from queue/topic %s (id=%u, seq=%u, rx_bytes=%d). Time: %.3f ms (%s)\n",
					pn_terminus_get_address(pn_link_source(lnk)),lamp_id_rx,lamp_seq_rx,(int)rx_size-LAMP_PACKET_OFFSET,(double)tripTime/1000,latencyTypePrinter(opts->latencyType));
			}

			// Update the current report structure
			reportStructureUpdate(reportPtr,tripTime,lamp_seq_rx);

			// In "-W" mode, write the current measured value to the specified CSV file too (if a file was successfully opened)
			if(aData->Wfiledescriptor>0) {
				aData->perPktData.seqNo=lamp_seq_rx;
				aData->perPktData.signedTripTime=timevalSub_retval==0 ? rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec : -(rx_timestamp.tv_sec*SEC_TO_MICROSEC+rx_timestamp.tv_usec);
				aData->perPktData.tx_timestamp=tx_timestamp;
				writeToTFile(aData->Wfiledescriptor,W_DECIMAL_DIGITS,&(aData->perPktData));
			}
		}
	}

	return continueFlag;
}

static int amqpInitACKreceiver(lamptype_t type,pn_link_t *lnk,pn_delivery_t *d,struct options *opts) {
	size_t rx_size;
	int isRightMsgReceived=0;

	// AMQP message buffer (size: maximum LaMP packet length + LAMP_PACKET_OFFSET bytes of AMQP header)
	char amqp_message_buf[MAX_LAMP_LEN+LAMP_PACKET_OFFSET];

	// Pointer to LaMP packet buffer with size = maximum LaMP packet length
	byte_t *lampPacket=(byte_t *) &(amqp_message_buf[0])+LAMP_PACKET_OFFSET;
	// Pointer to the LaMP header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;

	// LaMP relevant fields
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;
	uint16_t lamp_type_idx;

	// Get size of pending (received) message data
	rx_size=pn_delivery_pending(d);

	pn_link_recv(lnk,amqp_message_buf,rx_size);
	// If the delivery is not partial, nor aborted, proceeding with reading the message
	if(!pn_delivery_partial(d) && !pn_delivery_aborted(d)) {
		if(IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			lampHeadGetData(lampPacket,&lamp_type_rx,&lamp_id_rx,NULL,&lamp_type_idx,NULL,NULL);

			if(opts!=NULL && opts->verboseFlag) {
				display_packet("[INFO] LaMP packet content",lampPacket,rx_size-LAMP_PACKET_OFFSET);
			}

			if(lamp_type_rx==INIT) {
				if(opts==NULL) {
					return 0;
				}

				isRightMsgReceived=1;

				// We must now parse the INIT message
				// No INIT packet requesting a PINGLIKE mode is expected, but this king of packet is in any
				//  case supported to easily extend the AMQP Qpid Proton module to pinglike tests in the future
				mode_session = lamp_type_idx==INIT_UNIDIR_INDEX ? UNIDIR : PINGLIKE;
				lamp_id_session=lamp_id_rx;

				fprintf(stdout,"Server will accept all packets coming from link %s, id: %u\n",
					pn_terminus_get_address(pn_link_source(lnk)),
					lamp_id_session);

				fprintf(stdout,"Server will work in %s mode.\n",mode_session==UNIDIR ? "unidirectional" : "ping-like");
			} else if(lamp_type_rx==ACK && lamp_id_rx==lamp_id_session) {
				isRightMsgReceived=1;
			}
		}
	} else {
		fprintf(stderr,"Delivery error: the current delivery is either partial or aborted. Aborted: %d, Partial: %d\n",pn_delivery_aborted(d),pn_delivery_partial(d));
	}

	// Data has been processed
	pn_delivery_update(d,PN_ACCEPTED);
    pn_delivery_settle(d);

    return isRightMsgReceived;
}

static int consumerEventHandler(struct amqp_data *aData,struct options *opts,reportStructure *reportPtr,pn_event_t* handled_event) {
	pn_connection_t* connection;
	pn_session_t *s;
	static pn_link_t *l_rx;
	static pn_link_t *l_tx;
	const char deliverytag_report[]="reportdelivery";
	const char deliverytag_ack[]="ackdelivery_cons";

	// Return values
	int amqpUNIDIRReceiver_retval=0;

	if(opts->verboseFlag) {
		fprintf(stdout,"[INFO] Current Qpid Proton event: %s\n",pn_event_type_name(pn_event_type(handled_event)));
	}

	// If the consumer has not just been started, reset the timeout
	if(consumerStatus!=C_JUSTSTARTED) {
		pn_proactor_set_timeout(aData->proactor,aData->proactor_timeout);
	}

	switch(pn_event_type(handled_event)) {
		case PN_CONNECTION_INIT:
			connection=pn_event_connection(handled_event);
			pn_connection_set_container(connection,aData->containerID);

			s=pn_session(connection);

			pn_connection_open(connection);

			pn_session_open(s);

			l_tx=pn_sender(s,aData->senderName);
			pn_terminus_set_address(pn_link_target(l_tx),opts->queueNameRx);

			l_rx=pn_receiver(s,aData->receiverName);
			pn_terminus_set_address(pn_link_source(l_rx),opts->queueNameTx);

			// Open one link on a queue for receiving messages sent by the producer (<queue name>_tx)
			//  and one on a queue for sending messages to be received by the producer (<queue name>_rx)
			pn_link_open(l_tx);
			pn_link_open(l_rx);

			// Grant enough credit on the receiving link: as we do not know how
			//  many messages we will receive, grant credit for a batch of messages,
			//  of size: BATCH_CREDIT_SIZE
			pn_link_flow(l_rx,BATCH_CREDIT_SIZE);
			break;

		case PN_LINK_FLOW:
			if(opts->verboseFlag) {
				fprintf(stdout,"[INFO] There is no PN_LINK_FLOW handling in the consumer module.\n");
			}
			break;

		case PN_DELIVERY:
			{

			pn_link_t *lnk=pn_event_link(handled_event);
			if(strcmp(pn_link_name(lnk),aData->receiverName)==0) {
				// Rx link
				// We must receive something, starting from the INIT packet
				pn_delivery_t* e_delivery=pn_event_delivery(handled_event);

				if(pn_delivery_readable(e_delivery)) {
					// Check if the link is the right one
					pn_link_t *lnk_d=pn_delivery_link(e_delivery);

					if(strcmp(pn_link_name(lnk),pn_link_name(lnk_d))!=0) {
						fprintf(stderr,"Error: mismatching link name associated to the current delivery.\n"
							"Expected: %s. Got: %s.\n",pn_link_name(lnk),pn_link_name(lnk_d));
						break;
					}

					if(consumerStatus==C_JUSTSTARTED) {
						if(amqpInitACKreceiver(INIT,lnk,e_delivery,opts)) {
							// Set also a more reasonable timeout, as defined by the user with -t or equal to MIN_TIMEOUT_VAL_S ms if the user specified less than MIN_TIMEOUT_VAL_S ms
							if(opts->interval<=MIN_TIMEOUT_VAL_S) {
								aData->proactor_timeout=(pn_millis_t) (MIN_TIMEOUT_VAL_S/1000);
							} else {
								aData->proactor_timeout=(pn_millis_t) opts->interval;
							}
							pn_proactor_set_timeout(aData->proactor,aData->proactor_timeout);

							// We received the INIT packet, we can update the consumer status and, if -W is specified, open the CSV file to write the per-packet test data to
							consumerStatus=C_INITRECEIVED;

							// Open CSV file when in "-W" mode (i.e. "write every packet measurement data to CSV file")
							if(opts->Wfilename!=NULL) {
								// No follow-up is supported for AMQP 1.0 testing, but, instead of passing just '0' to openTfile() it can be useful to keep
								//  the check for a possible follow-up mode, as it may be implemented in some way in the future
								aData->Wfiledescriptor=openTfile(opts->Wfilename,opts->followup_mode!=FOLLOWUP_OFF);

								// Already set some fields in the per-packet data structure, which won't change during the whole test
								aData->perPktData.followup_on_flag=opts->followup_mode!=FOLLOWUP_OFF;
								aData->perPktData.tripTimeProc=0;

								if(aData->Wfiledescriptor<0) {
									fprintf(stderr,"Warning! Cannot open file for writing single packet latency data.\nThe '-W' option will be disabled.\n");
								}
							}

							// We can now send the ACK to the producer
							if(pn_link_credit(l_tx)<=0) {
								fprintf(stderr,"Error senduing ACK to INIT packet, not enough credit.\n");
								return -1;
							}
							pn_delivery(l_tx,pn_dtag(deliverytag_ack,sizeof(deliverytag_ack)));
							if(amqpACKReportSender(ACK,l_tx,aData,opts,NULL)==-1) {
								return -1;
							}
							// Update status, as the ACK was sent
							consumerStatus=C_ACKSENT;
						}
					} else if(consumerStatus==C_ACKSENT) {
						// We can start the LaMP UNIDIR packet reception
						amqpUNIDIRReceiver_retval=amqpUNIDIRReceiver(lnk,e_delivery,aData,opts,reportPtr);

						if(amqpUNIDIRReceiver_retval==0) {
							// The test completed successfully, we can now update the consumer status
							consumerStatus=C_FINISHED_RECEIVING;
						} else if(amqpUNIDIRReceiver_retval==-1) {
							pn_connection_close(pn_event_connection(handled_event));
							return -1;
						}

						// If there is not enough credit to receive all the packets, grant more
						if(pn_link_credit(lnk)<=0) {
							pn_link_flow(lnk,BATCH_CREDIT_SIZE);
							fprintf(stdout,"[INFO] Granted more credit to the sender.\n");
						}

						if(amqpUNIDIRReceiver_retval==0) {
							// Send the report
							pn_delivery(l_tx,pn_dtag(deliverytag_report,sizeof(deliverytag_report)));
							if(amqpACKReportSender(REPORT,l_tx,aData,opts,reportPtr)==-1) {
								return -1;
							}
							consumerStatus=C_REPORTSENT;
						}
					} else if(consumerStatus==C_REPORTSENT) {
						if(amqpInitACKreceiver(ACK,lnk,e_delivery,NULL)) {
							// We received and parsed a correct ACK: we can update the status of the producer
							consumerStatus=C_ACKRECEIVED;
						}
					} 
				} else {
					fprintf(stderr,"Warning: current delivery on receiving link is not readable.\n");
				}
			} else {
				pn_delivery_t* e_delivery=pn_event_delivery(handled_event);

				// Checking the delivery remote state; if PN_ACCEPTED (delivery successfully processed), do nothing,
				//  as everything is ok, otherwise print an error message and terminate the current session
				if(pn_delivery_remote_state(e_delivery)!=PN_ACCEPTED) {
					fprintf(stderr,"Error: unexpected delivery state: %s.\n",pn_disposition_type_name(pn_delivery_remote_state(e_delivery)));
					pn_connection_close(pn_event_connection(handled_event));
					return -1;
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

unsigned int runAMQPconsumer(struct amqp_data aData, struct options *opts) {
	// LaMP parameters
	reportStructure reportData;

	// Error flag for the main event loop
	uint8_t pn_error_condition=0;

	// Event handler return value
	int pn_eventhdlr_retval=0;

	// Inform the user about the current options
	fprintf(stdout,"Qpid Proton AMQP client started, with options:\n\t[node] = consumer (LaMP server role)\n"
		"\t[port] = %ld\n"
		"\t[timeout] = %" PRIu64 " ms\n"
		"\t[follow-up] = not supported\n"
		"\t[user priority] = not supported\n",
		opts->port,
		opts->interval<=MIN_TIMEOUT_VAL_S ? MIN_TIMEOUT_VAL_S : opts->interval);

	// Consumer status initialization (must be performed here as in daemon mode it should be reset every time the consumer is re-launched)
	consumerStatus=C_JUSTSTARTED;

	// Report structure inizialization
	reportStructureInit(&reportData,0,opts->number,opts->latencyType,opts->followup_mode);

	// Generate an initial session ID, which will not be used by the LaMP session, but which can be used to generate the
	//  container ID, sender name and receiver name
	lamp_id_session=(rand()+getpid())%UINT16_MAX;

	// This fprintf() terminates the series of call to inform the user about current settings -> using \n\n instead of \n
	fprintf(stdout,"\t[initial session LaMP ID for AMQP] = %" PRIu16 "\n\n",lamp_id_session);

	// Initialize the report structure
	reportStructureInit(&reportData, 0, opts->number, opts->latencyType, opts->followup_mode);

	// Set container ID, sender name and received name, depending on the chosen LaMP ID
	snprintf(aData.containerID,CONTAINERID_LEN,"LaTe_cons_%05" PRIu16,lamp_id_session);
	snprintf(aData.senderName,SENDERNAME_LEN,"LaTe_cons_tx_%05" PRIu16,lamp_id_session);
	snprintf(aData.receiverName,RECEIVERNAME_LEN,"LaTe_cons_rx_%05" PRIu16,lamp_id_session);

	// Start waiting for events
	do {
		pn_event_batch_t *events_batch=pn_proactor_wait(aData.proactor);
		for(pn_event_t *e=pn_event_batch_next(events_batch);e!=NULL;e=pn_event_batch_next(events_batch)) {
			pn_eventhdlr_retval=consumerEventHandler(&aData,opts,&reportData,e);
			if(pn_eventhdlr_retval==-1) {
				pn_error_condition=1;
			} else if(pn_eventhdlr_retval==-2) {
				fprintf(stdout,"Error: timeout occurred.\n");
				pn_error_condition=1;
			}
		}
		pn_proactor_done(aData.proactor,events_batch);
	} while(!pn_error_condition && consumerStatus!=C_ACKRECEIVED);

	// Close the "per-packet data" CSV file if '-W' was specified and a file was correctly opened
	if(aData.Wfiledescriptor>0) {
		closeTfile(aData.Wfiledescriptor);
	}

	if(opts->filename!=NULL) {
		// If '-f' was specified, print the report data to a file too
		printStatsCSV(opts,&reportData,opts->filename);
	}

	// Free Qpid proton allocated memory
	pn_proactor_free(aData.proactor);
	pn_message_free(aData.message);

	// Returning 0 if everything worked fine
	return pn_error_condition;
}