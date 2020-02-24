#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "rawsock.h"
#include "rawsock_lamp.h"
#include "udp_client_raw.h"
#include "udp_server_raw.h"
#include "udp_client.h"
#include "udp_server.h"
#include "options.h"
#include <linux/wireless.h>
#include <signal.h>
#include "common_socket_man.h"
#include <errno.h>

#if AMQP_1_0_ENABLED
#include <proton/proactor.h>
#include <proton/transport.h>

#include "qpid_proton_producer.h"
#include "qpid_proton_consumer.h"
#endif

static volatile sig_atomic_t end_prog_flag=0;

// Signal handler when the 'continuos daemon mode' is running - it will be called when SIGUSR1 is triggered
// USR1 is used to signal a server continuous mode proper termination
void end_prog_hdlr(int sig) {
	if(!end_prog_flag) {
		end_prog_flag=sig;
	}
	
	return;
}

int main (int argc, char **argv) {
	// Options management structure
	struct options opts;

	// wlanLookup() variables, for interface name, source MAC address and return value
	struct src_addrs addresses={.srcmacaddr=NULL}; // Initial value = NULL - if mode is not RAW, it will be left = NULL, and no source MAC will be obtained

	/* Socket management structure (containing a socket descriptor and a struct sockaddr_ll, see rawsock_lamp.h) */
	struct lampsock_data sData;

	// Interface index of the selected Linux interface (possibly using the -e/-I options)
	int ifindex;

	#if AMQP_1_0_ENABLED
	// AMQP 1.0 management structure
	struct amqp_data aData;
	#endif

	// Rx timeout structure
	struct timeval rx_timeout;

	// AMQP structures (defined only if AMQP 1.0 is enabled)
	#if AMQP_1_0_ENABLED
	char proactor_address_buff[PN_MAX_ADDR];
	#endif

	// Read options from command line
	options_initialize(&opts);
	if(parse_options(argc, argv, &opts)) {
		fprintf(stderr,"Error while parsing the options.\n");
		exit(EXIT_FAILURE);
	}

	// Print an info message when in continuous daemon mode
	if(opts.dmode) {
		fprintf(stdout,"The server will run in continuous mode. You can terminate it by calling 'kill -s USR1 <pid>'\n"
					"After giving the termination command, the current session will run until it will finish, then\n"
					"the program will be terminated. To get <pid>, you can use 'ps'.\n");

		// Set signal handlers
		(void) signal(SIGUSR1,end_prog_hdlr);
	}

	if(opts.protocol!=AMQP_1_0) {
		if(!socketDataSetup(opts.protocol,&sData,&opts,&addresses)) {
			exit(EXIT_FAILURE);
		}

		// Print current interface name
		fprintf(stdout,"\nThe program will work on the interface: %s (index: %x).\n\n",sData.devname,ifindex);
	}

	do {
		if(opts.protocol!=AMQP_1_0) {
			if(!socketOpen(opts.protocol,&sData,&opts,&addresses)) {
				exit(EXIT_FAILURE);
			}
		}	
		#if AMQP_1_0_ENABLED
		else if(opts.protocol==AMQP_1_0) {
		// Creating a new QPID Proton proactor with a new connection/transport (pn_proactor_connect2())
		aData.proactor=pn_proactor();
		pn_proactor_addr(proactor_address_buff,sizeof(proactor_address_buff),opts.dest_addr_u.destAddrStr,opts.portStr);

		pn_transport_t *t=pn_transport();

		pn_proactor_connect2(aData.proactor,NULL,t,proactor_address_buff);

		aData.message=pn_message();
		}
		#endif

		// Set srand() for all the random elements inside the client and server
		srand(time(NULL));

		switch(opts.mode_cs) {
			case CLIENT:
			case LOOPBACK_CLIENT:
				// Compute Rx timeout as: (MIN_TIMEOUT_VAL_C + opts.client_timeout) ms if -t <= MIN_TIMEOUT_VAL_C ms or (-t + opts.client_timeout) ms if 
				//  -t > MIN_TIMEOUT_VAL_C ms
				// Take into account that 'interval' is in 'ms' and 'tv_sec' is in 's'
				rx_timeout.tv_sec=opts.interval<=MIN_TIMEOUT_VAL_C ? (MIN_TIMEOUT_VAL_C+opts.client_timeout)/1000 : (opts.interval+opts.client_timeout)/1000;
				rx_timeout.tv_usec=0;
			break;
			case SERVER:
			case LOOPBACK_SERVER:
				// Server should start with a very big timeout, and then set it equal to -t (but the latter is performed inside udp_server.c)
				rx_timeout.tv_sec=86400; //... for instance, 86400 s = 24 h
				rx_timeout.tv_usec=0;
			break;
			default:
				// Should never enter here
				rx_timeout.tv_sec=0;
				rx_timeout.tv_usec=0;
			break;
		}

		/* Set Rx timeout, if the timeout cannot be set, issue a warning telling that, in case of packet loss,
		the program may run indefintely */
		// The timeout is set with setsockopt() when using a normal socket, while it set with pn_proactor_set_timeout()
		// (a Qpid Proton C library function) when using AMQP 1.0 (if enabled only)
		if(opts.protocol==UDP) {
			if(setsockopt(sData.descriptor, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout, sizeof(rx_timeout))!=0) {
				fprintf(stderr,"Warning: could not set RCVTIMEO: in case certain packets are lost,\n"
					"the program may run for an indefinite time and may need to be terminated with Ctrl+C.\n");
			}
		}
		#if AMQP_1_0_ENABLED
		else if(opts.protocol==AMQP_1_0) {
			aData.proactor_timeout=(pn_millis_t) (rx_timeout.tv_sec*1000+rx_timeout.tv_usec/1000);
			pn_proactor_set_timeout(aData.proactor,aData.proactor_timeout);
		}
		#endif

		switch(opts.mode_cs) {
			// Client is who sends packets
			case CLIENT:
			case LOOPBACK_CLIENT:
				if(opts.protocol==UDP) {
					if(opts.mode_raw == RAW ? runUDPclient_raw(sData, addresses.srcmacaddr, addresses.srcIPaddr, &opts) : runUDPclient(sData, &opts)) {
						if(!opts.dmode) {
							close(sData.descriptor);
							exit(EXIT_FAILURE);
						}
					}
				}

				#if AMQP_1_0_ENABLED
				if(opts.protocol==AMQP_1_0) {
					if(runAMQPproducer(aData,&opts)) {
						if(!opts.dmode) {
							close(sData.descriptor);
							exit(EXIT_FAILURE);
						}
					}
				}
				#endif
				break;
			// Server is who replies to packets
			case SERVER:
			case LOOPBACK_SERVER:
				if(opts.protocol==UDP) {
					if(opts.mode_raw == RAW ? runUDPserver_raw(sData, addresses.srcmacaddr, addresses.srcIPaddr, &opts) : runUDPserver(sData, &opts)) {
						if(!opts.dmode) {
							close(sData.descriptor);
							exit(EXIT_FAILURE);
						}
					}
				}

				#if AMQP_1_0_ENABLED
				if(opts.protocol==AMQP_1_0) {
					if(runAMQPconsumer(aData,&opts)) {
						if(!opts.dmode) {
							close(sData.descriptor);
							exit(EXIT_FAILURE);
						}
					}
				}
				#endif
				break;
			default:
				// Should never reach this point; it can be reached only if the 'options' module allows, due to a bug,
				// the execution with an unset "mode_cs" ("mode_clientserver")
				fprintf(stderr,"This string should never be printed.\nIf is is, there's a bug in the 'options' module.\n");
				break;
		}

		close(sData.descriptor);
	} while(opts.dmode && !end_prog_flag && (opts.mode_cs==SERVER || opts.mode_cs==LOOPBACK_SERVER));  // Continuosly run the server if the 'continuous daemon mode' is selected (a new socket will be created for each new session)

	fprintf(stdout,"\nProgram terminated.\n");

	if(addresses.srcmacaddr) freeMacAddrT(addresses.srcmacaddr);
	options_free(&opts);

	return 0;
}