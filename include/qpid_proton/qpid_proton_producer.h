#ifndef QPIDPROTONPRODUCER_H_INCLUDED
#define QPIDPROTONPRODUCER_H_INCLUDED

#include "options.h"
#include "common_socket_man.h"

typedef enum {
	P_JUSTSTARTED,
	P_INITSENT,
	P_ACKRECEIVED,
	P_SENDING,
	P_REPORTWAIT,
	P_REPORTRECEIVED,
	P_ACKSENT
} producer_status_t;
	
unsigned int runAMQPproducer(struct amqp_data aData, struct options *opts);

#endif