#include "common_udp.h"
#include "Rawsock_lib/rawsock_lamp.h"
#include "packet_structs.h"
#include "common_socket_man.h"
#include "errno.h"
#include "timer_man.h"
#include "options.h"
#include <unistd.h>
#include <stdio.h>   
#include <stdlib.h> 

/* Send control message.
Return values:
0: ok
-1: invalid argument
-2: sendto() error: cannot send packet
*/
int controlSenderUDP(arg_struct_udp *args, uint16_t session_id, int max_attempts, lamptype_t type, uint16_t followup_type, time_t interval_ms, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex) {
	struct lamphdr lampHeader;
	int counter=0;

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Timer management variables
	struct pollfd timerMon;
	int clockFd;

	// Return value of poll()
	int poll_retval=1;

	if(max_attempts<=0 || (type!=INIT && type!=ACK && type!=FOLLOWUP_CTRL) || (type==FOLLOWUP_CTRL && !IS_FOLLOWUP_CTRL_TYPE_VALID(followup_type))) {
		return -1;
	}

	if(type==INIT) {
		if(!termination_flag || !termination_flag_mutex) {
			return -1;
		} else {
			// Set the termination flag to 0
			pthread_mutex_lock(termination_flag_mutex);
			*termination_flag=0;
			pthread_mutex_unlock(termination_flag_mutex);
		}
	}

	// In case the type is ACK, only one message should be sent: force max_attempts to be 1
	if(type==ACK) {
		max_attempts=1;
	}

	lampHeadPopulate(&lampHeader, TYPE_TO_CTRL(type), session_id, 0); // Sequence number, starting from 0, is used only if multiple packets are sent
	if(type==INIT) {
		lampHeadSetConnType(&lampHeader,args->opts->mode_ub);
	} else if(type==FOLLOWUP_CTRL) {
		lampHeadSetFollowupCtrlType(&lampHeader,followup_type);
	}

	if(max_attempts==1) {
		if(sendto(args->sData.descriptor,&lampHeader,LAMP_HDR_SIZE(),NO_FLAGS,(struct sockaddr *)&(args->sData.addru.addrin[1]),sizeof(struct sockaddr_in))!=LAMP_HDR_SIZE()) {
			return -2;
		}
	} else {
		// Create and start timer
		if(timerCreateAndSet(&timerMon, &clockFd, interval_ms)<0) {
			return -2;
		}

		for(counter=0;counter<max_attempts;counter++) {
			if(poll_retval>0) {
				pthread_mutex_lock(termination_flag_mutex);
				if(*termination_flag) {
					break;
				}
				pthread_mutex_unlock(termination_flag_mutex);

				if(sendto(args->sData.descriptor,&lampHeader,LAMP_HDR_SIZE(),NO_FLAGS,(struct sockaddr *)&(args->sData.addru.addrin[1]),sizeof(struct sockaddr_in))!=LAMP_HDR_SIZE()) {
						return -2;
				}

				// Successive attempts will have an increased sequence number
				lampHeadIncreaseSeq(&lampHeader);
			}

			poll_retval=poll(&timerMon,1,INDEFINITE_BLOCK);
			if(poll_retval>0) {
				// "Clear the event" by performing a read() on a junk variable
				read(clockFd,&junk,sizeof(junk));
			}
		}
	}

	return 0;
}

/* Send raw control message.
Return values:
0: ok
-1: invalid argument
-2: sendto() error: cannot send packet
-3: malloc() error: cannot allocate memory
*/
int controlSenderUDP_RAW(arg_struct *args, controlRCVdata *rcvData, uint16_t session_id, int max_attempts, lamptype_t type, uint16_t followup_type, time_t interval_ms, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex) {
	// Packet buffers and headers
	struct pktheaders_udp headers;
	struct pktbuffers_udp buffers = {NULL, NULL, NULL, NULL};
	// IP address (src+dest) structure
	struct ipaddrs ipaddrs;
	// "in packet" LaMP header pointer
	struct lamphdr *inpacket_lamphdr;

	// Final packet size
	size_t finalpktsize;

	int counter=0;

	// Junk variable (needed to clear the timer event with read())
	unsigned long long junk;

	// Timer management variables
	struct pollfd timerMon;
	int clockFd;

	// Return value of poll()
	int poll_retval=1;

	if(max_attempts<=0 || (type!=INIT && type!=ACK && type!=FOLLOWUP_CTRL) || (type==FOLLOWUP_CTRL && !IS_FOLLOWUP_CTRL_TYPE_VALID(followup_type))) {
		return -1;
	}

	if(type==INIT) {
		if(!termination_flag || !termination_flag_mutex) {
			return -1;
		} else {
			// Set the termination flag to 0
			pthread_mutex_lock(termination_flag_mutex);
			*termination_flag=0;
			pthread_mutex_unlock(termination_flag_mutex);
		}
	}

	// In case the type is ACK, only one message should be sent: force max_attempts to be 1
	if(type==ACK) {
		max_attempts=1;
	}

	// Populating headers
	// [IMPROVEMENT] Future improvement: get destination MAC through ARP or broadcasted information and not specified by the user
	etherheadPopulate(&(headers.etherHeader), args->srcMAC, rcvData->controlRCV.mac, ETHERTYPE_IP);
	IP4headPopulateS(&(headers.ipHeader), args->sData.devname, rcvData->controlRCV.ip, 0, 0, BASIC_UDP_TTL, IPPROTO_UDP, FLAG_NOFRAG_MASK, &ipaddrs);
	UDPheadPopulate(&(headers.udpHeader), args->opts->mode_cs==CLIENT ? rcvData->controlRCV.port : args->opts->port, args->opts->mode_cs==CLIENT ? args->opts->port : rcvData->controlRCV.port);
	lampHeadPopulate(&(headers.lampHeader), TYPE_TO_CTRL(type), session_id, 0); // Starting from sequence number = 0
	if(type==INIT) {
		lampHeadSetConnType(&(headers.lampHeader), args->opts->mode_ub);
	} else if(type==FOLLOWUP_CTRL) {
		lampHeadSetFollowupCtrlType(&(headers.lampHeader),followup_type);
	}

	// Allocating packet buffers (without payload - as an ACK is sent and it does not require any payload)
	// There is no need to allocate the lamppacket buffer, as the LaMP header will be directly encapsulated inside UDP
	buffers.udppacket=malloc(UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));
	if(!buffers.udppacket) {
		return -3;
	}

	buffers.ippacket=malloc(IP_UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));
	if(!buffers.ippacket) {
		free(buffers.udppacket);
		return -3;
	}

	buffers.ethernetpacket=malloc(ETH_IP_UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));
	if(!buffers.ethernetpacket) {
		free(buffers.ippacket);
		free(buffers.udppacket);
		return -3;
	}

	// Get "in packet" LaMP header pointer
	inpacket_lamphdr=(struct lamphdr *) (buffers.ethernetpacket+sizeof(struct ether_header)+sizeof(struct iphdr)+sizeof(struct udphdr));

	// Prepare datagram (ctrl = ACK is already set in lampHeadPopulate(), few lines before this one)
	IP4headAddID(&(headers.ipHeader),(unsigned short) (rand()%UINT16_MAX)); // random ID could be okay?

	UDPencapsulate(buffers.udppacket,&(headers.udpHeader),(byte_t *)&(headers.lampHeader),LAMP_HDR_SIZE(),ipaddrs);

	// 'IP4headAddTotLen' may also be skipped since IP4Encapsulate already takes care of filling the length field
	IP4Encapsulate(buffers.ippacket, &(headers.ipHeader), buffers.udppacket, UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));
	finalpktsize=etherEncapsulate(buffers.ethernetpacket, &(headers.etherHeader), buffers.ippacket, IP_UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));

	if(max_attempts==1) {
		if(rawLampSend(args->sData.descriptor, args->sData.addru.addrll, inpacket_lamphdr, buffers.ethernetpacket, finalpktsize, FLG_NONE, UDP)) {
			return -2;
		}
	} else {
		// Create and start timer
		if(timerCreateAndSet(&timerMon, &clockFd, interval_ms)<0) {
			return 1;
		}

		for(counter=0;counter<max_attempts;counter++) {
			if(poll_retval>0) {
				pthread_mutex_lock(termination_flag_mutex);
				if(*termination_flag) {
					break;
				}
				pthread_mutex_unlock(termination_flag_mutex);

				if(rawLampSend(args->sData.descriptor, args->sData.addru.addrll, inpacket_lamphdr, buffers.ethernetpacket, finalpktsize, FLG_NONE, UDP)) {
					return -2;
				}

				lampHeadIncreaseSeq(&(headers.lampHeader));
			}

			poll_retval=poll(&timerMon,1,INDEFINITE_BLOCK);
			if(poll_retval>0) {
				// "Clear the event" by performing a read() on a junk variable
				read(clockFd,&junk,sizeof(junk));
			}
		}
	}

	// Free all buffers before exiting
	free(buffers.udppacket);
	free(buffers.ippacket);
	free(buffers.ethernetpacket);

	return 0;
}

/* Receive control message.
Return values:
0: message received and termination flag set
-1: invalid argument
-2: timeout occurred
-3: generic rcvfrom error occurred

This function uses session_id for receiving an ACK, or it sets session_id when receiving an INIT
*/
int controlReceiverUDP(int sFd, controlRCVdata *rcvData, lamptype_t type, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex) {
	// struct sockaddr_in to store the source IP address of the received LaMP packets
	struct sockaddr_in srcAddr;
	socklen_t srcAddrLen=sizeof(srcAddr);

	// Packet buffer with size = maximum LaMP packet length
	byte_t lampPacket[MAX_LAMP_LEN];
	// Pointer to the header, inside the packet buffer
	struct lamphdr *lampHeaderPtr=(struct lamphdr *) lampPacket;

	// LaMP relevant fields
	uint16_t lamp_type_idx;
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;

	ssize_t rcv_bytes;

	if((type!=INIT && type!=ACK && type!=FOLLOWUP_CTRL) || rcvData==NULL) {
		return -1;
	}

	while(1) {
		saferecvfrom(rcv_bytes,sFd,lampPacket,MAX_LAMP_LEN,NO_FLAGS,(struct sockaddr *)&srcAddr,&srcAddrLen);

		// Timeout or recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				return -2;
			} else {
				return -3;
			}
		}

		// Check whether the packet is really encapsulating LaMP; if it is not, discard packet
		if(!IS_LAMP(lampHeaderPtr->reserved,lampHeaderPtr->ctrl)) {
			continue;
		}

		// If the packet is really a LaMP packet, get the header data
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, NULL, &lamp_type_idx, NULL, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_type_rx!=type) {
			continue;
		}

		if(type==ACK) {
			if(lamp_id_rx!=rcvData->session_id) {
				continue;
			}
		} else if((type==INIT && IS_INIT_INDEX_VALID(lamp_type_idx)) || (type==FOLLOWUP_CTRL && IS_FOLLOWUP_CTRL_TYPE_VALID(lamp_type_idx))) {
			// If the type is INIT, populate the rcvData structure
			rcvData->controlRCV.ip=srcAddr.sin_addr;
			rcvData->controlRCV.port=srcAddr.sin_port;
			rcvData->controlRCV.session_id=lamp_id_rx;
			rcvData->controlRCV.type_idx=lamp_type_idx;
		} else {
			continue;
		}

		// If, finally, the packet is a control packet with the proper id and type, exit the while loop
		break;
	}

	if(termination_flag_mutex && termination_flag) {
		pthread_mutex_lock(termination_flag_mutex);
		*termination_flag=1;
		pthread_mutex_unlock(termination_flag_mutex);
	}

	return 0;
}

/* Receive raw control message.
Return values:
0: message received and termination flag set
-1: invalid argument
-2: timeout occurred
-3: generic rcvfrom error occurred
*/
int controlReceiverUDP_RAW(int sFd, in_port_t port, in_addr_t ip, controlRCVdata *rcvData, lamptype_t type, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex) {
	// Packet buffer with size = RAW_RX_PACKET_BUF_SIZE
	byte_t packet[RAW_RX_PACKET_BUF_SIZE];

	// Header pointers and packet buffers
	struct pktheadersptr_udp headerptrs;
	byte_t *lampPacket=NULL;

	// UDP payload size (i.e. LaMP packet size)
	size_t UDPpayloadsize;

	// LaMP relevant fields
	uint16_t lamp_type_idx;
	lamptype_t lamp_type_rx;
	uint16_t lamp_id_rx;

	// struct sockaddr_ll filled by recvfrom() and used to filter out outgoing traffic
	struct sockaddr_ll addrll;
	socklen_t addrllLen=sizeof(addrll);

	ssize_t rcv_bytes;

	if((type!=INIT && type!=ACK && type!=FOLLOWUP_CTRL) || rcvData==NULL) {
		return -1;
	}

	// Already get all the packet pointers
	lampPacket=UDPgetpacketpointers(packet,&(headerptrs.etherHeader),&(headerptrs.ipHeader),&(headerptrs.udpHeader));
	lampGetPacketPointers(lampPacket,&(headerptrs.lampHeader));

	while(1) {
		saferecvfrom(rcv_bytes,sFd,packet,RAW_RX_PACKET_BUF_SIZE,NO_FLAGS,(struct sockaddr *)&addrll,&addrllLen);

		// Timeout or recvfrom() error occurred
		if(rcv_bytes==-1) {
			if(errno==EAGAIN) {
				return -2;
			} else {
				return -3;
			}
		}

		// Filter out all outgoing packets - [IMPROVEMENT] use BPF instead
		if(addrll.sll_pkttype==PACKET_OUTGOING) {
			continue;
		}

		// Go on only if it is a datagram of interest (in our case if it is UDP)
		if (ntohs((headerptrs.etherHeader)->ether_type)!=ETHERTYPE_IP) { 
			continue;
		}

		if ((headerptrs.ipHeader)->protocol!=IPPROTO_UDP || CHECK_IP_ADDR_DST(ip) || ntohs((headerptrs.udpHeader)->dest)!=port) {
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
		lampHeadGetData(lampPacket, &lamp_type_rx, &lamp_id_rx, NULL, &lamp_type_idx, NULL, NULL);

		// Discard any LaMP packet which is not of interest
		if(lamp_type_rx!=type) {
			continue;
		}

		if(type==ACK) {
			if(lamp_id_rx!=rcvData->session_id) {
				continue;
			}
		} else if((type==INIT && IS_INIT_INDEX_VALID(lamp_type_idx)) || (type==FOLLOWUP_CTRL && IS_FOLLOWUP_CTRL_TYPE_VALID(lamp_type_idx))) {
			// If the type is init, populate the rcvData structure
			rcvData->controlRCV.ip.s_addr=headerptrs.ipHeader->saddr;
			rcvData->controlRCV.port=ntohs(headerptrs.udpHeader->source);
			rcvData->controlRCV.session_id=lamp_id_rx;
			rcvData->controlRCV.type_idx=lamp_type_idx;
			memcpy(rcvData->controlRCV.mac,(headerptrs.etherHeader)->ether_shost,ETHER_ADDR_LEN);
		} else {
			continue;
		}

		// If, finally, the packet is a control packet with the proper id and type, exit the while loop
		break;
	}

	if(termination_flag_mutex && termination_flag) {
		pthread_mutex_lock(termination_flag_mutex);
		*termination_flag=1;
		pthread_mutex_unlock(termination_flag_mutex);
	}

	return 0;
}

/* Send FOLLOWUP_DATA message with no payload
Return values:
0: message sent correctly
1; error when sending the message
*/
int sendFollowUpData(struct lampsock_data sData,uint16_t id,uint16_t seq,struct timeval tDiff) {
	struct lamphdr lampHeader;

	lampHeadPopulate(&lampHeader, CTRL_FOLLOWUP_DATA, id, seq);

	lampHeadSetTimestamp(&lampHeader,&tDiff);

	return sendto(sData.descriptor,(byte_t *)&lampHeader,LAMP_HDR_SIZE(),NO_FLAGS,(struct sockaddr *)&(sData.addru.addrin[1]),sizeof(struct sockaddr_in))!=LAMP_HDR_SIZE();
}

/* Send FOLLOWUP_DATA message with no payload (raw socket)
Return values:
0: message sent correctly
1; error when sending the message
*/
int sendFollowUpData_RAW(arg_struct *args,controlRCVdata *rcvData,uint16_t id,uint16_t ip_id,uint16_t seq,struct timeval tDiff) {
	struct pktheaders_udp headers;
	struct pktbuffers_udp_nopayload_fixed buffers;
	size_t finalpktsize;
	struct ipaddrs ipaddrs;
	struct lamphdr *inpacket_lamphdr;

	lampHeadPopulate(&(headers.lampHeader), CTRL_FOLLOWUP_DATA, id, seq);
	lampHeadSetTimestamp(&(headers.lampHeader),&tDiff);

	etherheadPopulate(&(headers.etherHeader), args->srcMAC, rcvData->controlRCV.mac, ETHERTYPE_IP);
	IP4headPopulateS(&(headers.ipHeader), args->sData.devname, rcvData->controlRCV.ip, 0, 0, BASIC_UDP_TTL, IPPROTO_UDP, FLAG_NOFRAG_MASK, &ipaddrs);
	IP4headAddID(&(headers.ipHeader),(unsigned short) ip_id);
	UDPheadPopulate(&(headers.udpHeader), args->opts->mode_cs==CLIENT ? rcvData->controlRCV.port : args->opts->port, args->opts->mode_cs==CLIENT ? args->opts->port : rcvData->controlRCV.port);

	UDPencapsulate(buffers.udppacket,&(headers.udpHeader),(byte_t *)&(headers.lampHeader),LAMP_HDR_SIZE(),ipaddrs);
	IP4Encapsulate(buffers.ippacket, &(headers.ipHeader), buffers.udppacket, UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));
	finalpktsize=etherEncapsulate(buffers.ethernetpacket, &(headers.etherHeader), buffers.ippacket, IP_UDP_PACKET_SIZE_S(LAMP_HDR_SIZE()));

	// Get "in packet" LaMP header pointer, as required by rawLampSend
	inpacket_lamphdr=(struct lamphdr *) (buffers.ethernetpacket+sizeof(struct ether_header)+sizeof(struct iphdr)+sizeof(struct udphdr));

	return rawLampSend(args->sData.descriptor, args->sData.addru.addrll, inpacket_lamphdr, buffers.ethernetpacket, finalpktsize, FLG_NONE, UDP);
}