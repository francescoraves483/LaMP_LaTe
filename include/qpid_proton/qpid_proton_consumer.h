#ifndef QPIDPROTONCONSUMER_H_INCLUDED
#define QPIDPROTONCONSUMER_H_INCLUDED

#include "options.h"
#include "common_socket_man.h"

typedef enum {
	C_JUSTSTARTED,
	C_INITRECEIVED,
	C_ACKSENT,
	C_RECEIVING,
	C_FINISHED_RECEIVING,
	C_REPORTSENT,
	C_ACKRECEIVED
} consumer_status_t;
	
unsigned int runAMQPconsumer(struct amqp_data aData, struct options *opts);

#endif