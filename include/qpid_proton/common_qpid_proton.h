#ifndef COMMONQPIDPROTON_H_INCLUDED
#define COMMONQPIDPROTON_H_INCLUDED

#include <proton/condition.h>
#include <proton/event.h>
#include <proton/message.h>
#include "common_socket_man.h"
#include "options.h"

// Offset to manually exclude the AMQP header from received messages. 
// This is only a workaround as we are not yet able to remove this header using the Qpid proton library
#define LAMP_PACKET_OFFSET 28

int qpid_condition_check_and_close_connection(pn_event_t *event,pn_condition_t *condition);
ssize_t qpid_lamp_message_send(pn_message_t *msg,pn_link_t *tx_link,pn_rwbytes_t *buf,struct amqp_data *aData,struct options *opts);

#endif