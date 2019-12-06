#ifndef COMMON_UDP_H_INCLUDED
#define COMMON_UDP_H_INCLUDED

#include <pthread.h>
#include "common_thread.h"
#include "rawsock_lamp.h"

#define LO_ADDR_HEX 0x0100007f
#define CHECK_IP_ADDR_DST(ip) (headerptrs.ipHeader->daddr!=ip)
#define CHECK_IP_ADDR_SRC(ip) (headerptrs.ipHeader->saddr!=ip)

struct controlRCVstruct {
	uint16_t session_id;
	struct in_addr ip;
	in_port_t port;
	uint16_t type_idx;
	uint8_t mac[ETHER_ADDR_LEN];
};

typedef union _controlRCVdata {
	uint16_t session_id;
	struct controlRCVstruct controlRCV;
} controlRCVdata;

int controlSenderUDP(arg_struct_udp *args, uint16_t session_id, int max_attempts, lamptype_t type, uint16_t followup_type, time_t interval_ms, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex);
int controlSenderUDP_RAW(arg_struct *args, controlRCVdata *rcvData, uint16_t session_id, int max_attempts, lamptype_t type, uint16_t followup_type, time_t interval_ms, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex);
int controlReceiverUDP(int sFd, controlRCVdata *rcvData, lamptype_t type, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex);
int controlReceiverUDP_RAW(int sFd, in_port_t port, in_addr_t ip, controlRCVdata *rcvData, lamptype_t type, uint8_t *termination_flag, pthread_mutex_t *termination_flag_mutex);
int sendFollowUpData(struct lampsock_data sData,uint16_t id,uint16_t seq,struct timeval tDiff);
int sendFollowUpData_RAW(arg_struct *args,controlRCVdata *rcvData,uint16_t id,uint16_t ip_id,uint16_t seq,struct timeval tDiff);

#endif