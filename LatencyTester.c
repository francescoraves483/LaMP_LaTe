#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include "Rawsock_lib/rawsock.h"
#include "Rawsock_lib/rawsock_lamp.h"
#include "udp_client_raw.h"
#include "udp_server_raw.h"
#include "udp_client.h"
#include "udp_server.h"
#include "options.h"
#include <linux/wireless.h>
#include <signal.h>
#include "common_socket_man.h"
#include <errno.h>

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
	char devname[IFNAMSIZ]={0}; // It will contain the used interface name
	macaddr_t srcmacaddr=NULL; // Initial value = NULL - if mode is not RAW, it will be left = NULL, and no source MAC will be obtained
	int ret_wlanl_val;
	int ifindex;
	struct in_addr srcIPaddr;

	/* Socket management structure (containing a socket descriptor and a struct sockaddr_ll, see rawsock_lamp.h) */
	struct lampsock_data sData;

	// Rx timeout structure
	struct timeval rx_timeout;

	// wlanLookup index
	int wlanLookupIdx=0;

	// Read options from command line
	options_initialize(&opts);
	if(parse_options(argc, argv, &opts)) {
		fprintf(stderr,"Error while parsing the options.\n");
		exit(EXIT_FAILURE);
	}

	// Set wlanLookupIdx, depending on the mode (loopback vs normal mode)
	if(opts.mode_cs==LOOPBACK_CLIENT || opts.mode_cs==LOOPBACK_SERVER) {
		wlanLookupIdx=WLANLOOKUP_LOOPBACK;
	} else {
		wlanLookupIdx=opts.if_index;
	}

	// Allocate memory for the source MAC address (only if 'RAW' is specified)
	if(opts.mode_raw==RAW) {
		srcmacaddr=prepareMacAddrT();
		if(macAddrTypeGet(srcmacaddr)==MAC_NULL) {
			fprintf(stderr,"Error: could not allocate memory to store the source MAC address.\n");
			exit(EXIT_FAILURE);
		}
	}

	// Look for available wireless/non-wireless interfaces
	ret_wlanl_val=wlanLookup(devname,&ifindex,srcmacaddr,&srcIPaddr,wlanLookupIdx,opts.nonwlan_mode==0 ? WLANLOOKUP_WLAN : WLANLOOKUP_NONWLAN);
	if(ret_wlanl_val<=0) {
		rs_printerror(stderr,ret_wlanl_val);
		exit(EXIT_FAILURE);
	}

	if(opts.mode_raw==RAW) {
		if(opts.destIPaddr.s_addr==srcIPaddr.s_addr) {
		fprintf(stderr,"Error: you cannot test yourself in raw mode.\n"
			"Use non raw sockets instead.\n");
		exit(EXIT_FAILURE);
		}

		// Just for the sake of safety, check if wlanLookup() was able to properly write the source MAC address
		if(macAddrTypeGet(srcmacaddr)==MAC_BROADCAST || macAddrTypeGet(srcmacaddr)==MAC_NULL) {
			fprintf(stderr,"Could not retrieve source MAC address.\n");
			exit(EXIT_FAILURE);
		}
	}

	// Print current interface name
	fprintf(stdout,"\nThe program will work on the interface: %s (index: %x).\n\n",devname,ifindex);

	// In loopback mode, set the destination IP address as the source one, inside the 'options' structure
	if(opts.mode_cs==LOOPBACK_CLIENT || opts.mode_cs==LOOPBACK_SERVER) {
		options_set_destIPaddr(&opts,srcIPaddr);
	}

	// Open socket, discriminating the raw and non raw cases
	switch(opts.mode_raw) {
		case RAW:
			sData.descriptor=socket(AF_PACKET,SOCK_RAW,htons(ETH_P_ALL));

			if(sData.descriptor==-1) {
				perror("socket() error");
				exit(EXIT_FAILURE);
			}

			// Prepare sockaddr_ll structure
			bzero(&(sData.addru.addrll),sizeof(sData.addru.addrll));
			sData.addru.addrll.sll_ifindex=ifindex;
			sData.addru.addrll.sll_family=AF_PACKET;
			sData.addru.addrll.sll_protocol=htons(ETH_P_ALL);

			// Bind to the wireless interface
			if(bind(sData.descriptor,(struct sockaddr *) &(sData.addru.addrll),sizeof(sData.addru.addrll))<0) {
				perror("Cannot bind to interface: bind() error");
		  		close(sData.descriptor);
		  		exit(EXIT_FAILURE);
			}
		break;
		case NON_RAW:
			// Add a nested switch case here when more than one protocol will be implemented (*)
			sData.descriptor=socketCreator(UDP);

			if(sData.descriptor==-1) {
				perror("socket() error");
				exit(EXIT_FAILURE);
			}

			// case UDP: (*) - when more than one protocol will be implemented...
			// Prepare bind sockaddr_in structure (index 0)
			bzero(&(sData.addru.addrin[0]),sizeof(sData.addru.addrin[0]));
			sData.addru.addrin[0].sin_family=AF_INET;

			if(opts.mode_cs==CLIENT || opts.mode_cs==LOOPBACK_CLIENT) {
				sData.addru.addrin[0].sin_port=0;
			} else if(opts.mode_cs==SERVER || opts.mode_cs==LOOPBACK_SERVER) {
				sData.addru.addrin[0].sin_port=htons(opts.port);
			}

			sData.addru.addrin[0].sin_addr.s_addr=srcIPaddr.s_addr;

			// Bind to the wireless interface
			if(bind(sData.descriptor,(struct sockaddr *) &(sData.addru.addrin[0]),sizeof(sData.addru.addrin[0]))<0) {
				perror("Cannot bind to interface: bind() error");
		  		close(sData.descriptor);
		  		exit(EXIT_FAILURE);
			}

			// Bind to the wireless interface
			//if(setsockopt(sData.descriptor,SOL_SOCKET,SO_BINDTODEVICE,devname,strlen(devname))==-1) {
			//	perror("setsockopt() for SO_BINDTODEVICE error");
			//	close(sData.descriptor);
			//	exit(EXIT_FAILURE);
			//}
		break;
		default:
			// Should never enter here
			sData.descriptor=-1;
		break;
	}

	// Only if -A was specified, set a certain EDCA AC (works only in patched kernels, as of now)
	if(opts.macUP!=UINT8_MAX && setsockopt(sData.descriptor,SOL_SOCKET,SO_PRIORITY,&(opts.macUP),sizeof(opts.macUP))!=0) {
		perror("setsockopt() for SO_PRIORITY error");
		close(sData.descriptor);
		exit(EXIT_FAILURE);
	}

	// Set srand() for all the random elements inside the client and server
	srand(time(NULL));

	switch(opts.mode_cs) {
		case CLIENT:
		case LOOPBACK_CLIENT:
			// Compute Rx timeout as: (MIN_TIMEOUT_VAL_C + 2000) ms if -t <= MIN_TIMEOUT_VAL_C ms or t + 2 s if -t > MIN_TIMEOUT_VAL_C ms
			// Take into account that 'interval' is in 'ms' and 'tv_sec' is in 's'
			rx_timeout.tv_sec=opts.interval<=MIN_TIMEOUT_VAL_C ? (MIN_TIMEOUT_VAL_C+2000)/1000 : (opts.interval+2000)/1000;
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
	if(setsockopt(sData.descriptor, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout, sizeof(rx_timeout))!=0) {
		fprintf(stderr,"Warning: could not set RCVTIMEO: in case certain packets are lost,\n"
			"the program may run for an indefinite time and may need to be terminated with Ctrl+C.\n");
	}

	switch(opts.mode_cs) {
		// Client is who sends packets
		case CLIENT:
		case LOOPBACK_CLIENT:
			if(opts.mode_raw == RAW ? runUDPclient_raw(sData, devname, srcmacaddr, srcIPaddr, &opts) : runUDPclient(sData, &opts)) {
				close(sData.descriptor);
				exit(EXIT_FAILURE);
			}
			break;
		// Server is who replies to packets
		case SERVER:
		case LOOPBACK_SERVER:
			// Print an info message when in continuous daemon mode
			if(opts.dmode) {
				fprintf(stdout,"The server will run in continuous mode. You can terminate it by calling 'kill -s USR1 <pid>'\n"
					"After giving the termination command, the current session will run until it will finish, then\n"
					"the program will be terminated. To get <pid>, you can use 'ps'.\n");

				// Set signal handlers
				(void) signal(SIGUSR1,end_prog_hdlr);
			}

			do {
				if(opts.mode_raw == RAW ? runUDPserver_raw(sData, devname, srcmacaddr, srcIPaddr, &opts) : runUDPserver(sData, &opts)) {
					close(sData.descriptor);
					exit(EXIT_FAILURE);
				}

				// Reset the rx timeout and errno value (as last operation) when in continuous daemon mode, before launching the next server session
				if(opts.dmode) {
					rx_timeout.tv_sec=86400;
					rx_timeout.tv_usec=0;
					// Set back a big timeout for the next session, if continuous mode is selected
					if(setsockopt(sData.descriptor, SOL_SOCKET, SO_RCVTIMEO, &rx_timeout, sizeof(rx_timeout))!=0) {
						fprintf(stderr,"Warning: could not set RCVTIMEO: in case certain packets are lost,\n"
							"the program may run for an indefinite time and may need to be terminated with Ctrl+C.\n");
					}
				}
			} while(opts.dmode && !end_prog_flag); // Continuosly run the server if the 'continuous daemon mode' is selected
			break;
		default:
			// Should never reach this point; it can be reached only if the 'options' module allows, due to a bug,
			// the execution with an unset "mode_cs" ("mode_clientserver")
			fprintf(stderr,"This string should never be printed.\nIf is is, there's a bug in the 'options' module.\n");
			break;
	}

	fprintf(stdout,"\nProgram terminated.\n");

	if(srcmacaddr) freeMacAddrT(srcmacaddr);
	options_free(&opts);
	close(sData.descriptor);

	return 0;
}