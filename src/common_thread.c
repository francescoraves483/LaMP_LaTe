#include "common_thread.h"

void thread_error_print(const char *name, t_error_types err) {
	switch(err) {
		case NO_ERR:
			fprintf(stderr,"%s execution was successful.\n",name);
			break;
		case ERR_UNKNOWN:
			fprintf(stderr,"%s reported an unknown error.\n",name);
			break;
		case ERR_TIMERCREATE:
			fprintf(stderr,"%s reported an error in timerfd_create().\n",name);
			break;
		case ERR_SETTIMER:
			fprintf(stderr,"%s reported an error in timerfd_settime().\n",name);
			break;
		case ERR_STOPTIMER:
			fprintf(stderr,"%s reported an error when trying to stop the periodic interval timer.\n",name);
			break;
		case ERR_RANDSETTIMER:
			fprintf(stderr,"%s reported an error when trying to set again the timer to a random value.\n",name);
			break;
		case ERR_SEND_INIT:
			fprintf(stderr,"%s reported an error in sending the LaMP INIT packet.\n",name);
			break;
		case ERR_IOCTL:
			fprintf(stderr,"%s reported an error in ioctl(SIOCGSTAMP).\n",name);
			break;
		case ERR_SEND:
			fprintf(stderr,"%s reported an error in sending a LaMP packet.\n",name);
			break;
		case ERR_TIMEOUT:
			fprintf(stderr,"%s reported an error: reception timed out.\n",name);
			break;
		case ERR_REPORT_TIMEOUT:
			fprintf(stderr,"%s reported an error: reception timed out when waiting for a REPORT.\n",name);
			break;
		case ERR_TIMEOUT_ACK:
			fprintf(stderr,"%s reported an error: reception timed out when waiting for an ACK.\n",name);
			break;
		case ERR_TIMEOUT_INIT:
			fprintf(stderr,"%s reported an error: reception timed out when waiting for a INIT packet.\n",name);
			break;
		case ERR_TIMEOUT_FOLLOWUP:
			fprintf(stderr,"%s reported an error: reception timed out when waiting for a Follow-up control packet.\n",name);
			break;
		case ERR_SEND_ACK:
			fprintf(stderr,"%s reported an error in sending a LaMP ACK packet.\n",name);
			break;
		case ERR_SEND_FOLLOWUP:
			fprintf(stderr,"%s reported an error in sending a LaMP Follow-up request packet.\n",name);
			break;
		case ERR_INVALID_ARG_CMONUDP:
			fprintf(stderr,"%s reported an error: invalid arguments in a common_udp function.\n",name);
			break;
		case ERR_MALLOC:
			fprintf(stderr,"%s reported an error: cannot allocate memory to store packet buffers.\n",name);
			break;
		case ERR_RECVFROM_GENERIC:
			fprintf(stderr,"%s reported a generic error when receiving packets, not related to any timeout.\n",name);
			break;
		case ERR_TXSTAMP:
			fprintf(stderr,"%s reported an error when retrieving the TX timestamp in hardware mode. Try using another mode.\n",name);
			break;
		case ERR_CLEAR_TIMER_EVENT:
			fprintf(stderr,"%s reported a timer error: a timer event could not be read and the execution was terminated.\n",name);
			break;
		default:
			fprintf(stderr,"%s reported a generic error.\n",name);
			break;
	}
}