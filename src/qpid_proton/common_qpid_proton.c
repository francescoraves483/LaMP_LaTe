#include "common_qpid_proton.h"
#include <stdio.h>
#include <proton/connection.h>
#include <unistd.h>

ssize_t qpid_lamp_message_send(pn_message_t *msg,pn_link_t *tx_link,pn_rwbytes_t *buf,struct amqp_data *aData,struct options *opts) {
	// Set the message TTL as the currently set proactor timeout
	if(pn_message_set_ttl(msg,aData->proactor_timeout)!=0) {
		return 0;
	}

	return pn_message_send(msg,tx_link,buf);
}

int qpid_condition_check_and_close_connection(pn_event_t *event,pn_condition_t *condition) {
	int return_value=1;

	if(pn_condition_is_set(condition)) {
		fprintf(stderr,"AMQP Condition raised due to closing of an endpoint.\n"
			"More info: event: %s\n"
			"Condition: %s\n"
			"Description: %s\n",
			pn_event_type_name(pn_event_type(event)),
			pn_condition_get_name(condition),
			pn_condition_get_description(condition));
		return_value=-1;
	} else {
		fprintf(stderr,"Event %s caused an exception but no information is available.\n",pn_event_type_name(pn_event_type(event)));
	}

	pn_connection_close(pn_event_connection(event));
	fprintf(stdout,"AMQP connection closed.\n");

	return return_value;
}
