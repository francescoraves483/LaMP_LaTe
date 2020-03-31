#ifndef COMMONQPIDPROTON_H_INCLUDED
#define COMMONQPIDPROTON_H_INCLUDED

#include <proton/condition.h>
#include <proton/event.h>
#include <proton/message.h>
#include "common_socket_man.h"
#include "options.h"

#define ADDITIONAL_AMQP_HEADER_MAX_SIZE 100
#define VERBOSE_PRINT_RX_PACKET_BYTES 30

typedef struct lampPacket_bytes_t_ {
	pn_bytes_t lampPacket;
	pn_message_t *msg_ptr;
} lampPacket_bytes_t;

int qpid_condition_check_and_close_connection(pn_event_t *event,pn_condition_t *condition);
ssize_t qpid_lamp_message_send(pn_message_t *msg,pn_link_t *tx_link,pn_rwbytes_t *buf,struct amqp_data *aData,struct options *opts);
pn_type_t lampPacketDecoder(char *amqp_message_buf,size_t rx_size,lampPacket_bytes_t *lampPacket);

#endif