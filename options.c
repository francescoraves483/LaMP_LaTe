#include "options.h"
#include "version.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include "Rawsock_lib/rawsock.h"

#define CSV_EXTENSION_LEN 4 // '.csv' length
#define CSV_EXTENSION_STR ".csv"

static const char *latencyTypes[]={"Unknown","User-to-user","KRT"};

static void print_long_info(void) {
	fprintf(stdout,"\nUsage: %s [-c <destination address> [mode] | -l [mode] | -s | -m] [protocol] [options]\n"
		"%s [-h]: print help and information about available interfaces (including indeces)\n"
		"%s [-v]: print version information\n\n"
		"-c: client mode\n"
		"-s: server mode\n"
		"-l: loopback client mode (send packets to first loopback interface - does not support raw sockets)\n"
		"-m: loopback server mode (binds to the loopback interface - does not support raw sockets)\n\n"
		"<destination address>:\n"
		"  This is the destination address of the server. It depends on the protocol.\n"
		"  UDP: <destination address> = <destination IP address>\n"
		"\n"

		"[mode]:\n"
		"  Client operating mode (the server will adapt its mode depending on the incoming packets).\n"
		"  -B: ping-like bidirectional mode\n"
		"  -U: unidirectional mode (requires clocks to be perfectly synchronized - highly experimental\n"
		"\t  - use at your own risk!)\n"
		"\n"

		"[protocol]:\n"
		"  Protocol to be used, in which the LaMP packets will be encapsulated.\n"
		"  -u: UDP\n"
		"\n"

		"[options] - Mandatory client options:\n"
		"  -M <destination MAC address>: specifies the destination MAC address.\n"
		"\t  Mandatory only if socket is RAW ('-r' is selected) and protocol is UDP.\n"
		"\n"

		"[options] - Optional client options:\n"
		"  -n <total number of packets to be sent>: specifies how many packets to send (default: %d).\n"
		"  -t <time interval in ms>: specifies the periodicity, in milliseconds, to send at (default: %d ms).\n"
		"  -f <filename, without extension>: print the report to a CSV file other than printing\n"
		"\t  it on the screen.\n"
		"\t  The default behaviour will append to an existing file; if the file does not exist,\n" 
		"\t  it is created.\n"
		"  -o: valid only with '-f'; instead of appending to an existing file, overwrite it.\n"
		"  -P <payload size in B>: specify a LaMP payload length, in bytes, to be included inside\n"
		"\t  the packet (default: 0 B).\n"
		"  -r: use raw sockets, if supported for the current protocol.\n"
		"\t  When '-r' is set, the program tries to insert the LaMP timestamp in the last \n"
		"\t  possible instant before sending. 'sudo' (or proper permissions) is required in this case.\n"
		"  -A <access category: BK | BE | VI | VO>: forces a certain EDCA MAC access category to\n"
		"\t  be used (patched kernel required!).\n"
		"  -L <latency type: u | r>: select latency type: user-to-user or KRT (Kernel Receive Timestamp).\n"
		"\t  Default: u. Please note that the client supports this parameter only when in bidirectional mode.\n"
		"  -I <interface index>: instead of using the first wireless/non-wireless interface, use the one with\n"
		"\t  the specified index. The index must be >= 0. Use -h to print the valid indeces. Default value: 0.\n"
		"  -e: use non-wireless interfaces instead of wireless ones. The default behaviour, without -e, is to\n"
		"\t  look for available wireless interfaces and return an error if none are found.\n"
		"  -p <port>: specifies the port to be used. Can be specified only if protocol is UDP (default: %d).\n"
		"\n"

		"[options] - Mandatory server options:\n"
		"   <none>"
		"\n"

		"[options] - Optional server options:\n"
		"  -t <timeout in ms>: specifies the timeout after which the connection should be\n"
		"\t  considered lost (minimum value: %d ms, otherwise %d ms will be automatically set - default: %d ms).\n"
		"  -r: use raw sockets, if supported for the current protocol.\n"
		"\t  When '-r' is set, the program tries to insert the LaMP timestamp in the last \n"
		"\t  possible instant before sending. 'sudo' (or proper permissions) is required in this case.\n"
		"  -A <access category: BK | BE | VI | VO>: forces a certain EDCA MAC access category to\n"
		"\t  be used (patched kernel required!).\n"
		"  -d: set the server in 'continuous daemon mode': as a session is terminated, the server\n"
		"\t  will be restarted and will be able to accept new packets from other clients.\n"
		"  -L <latency type: u | r>: select latency type: user-to-user or KRT (Kernel Receive Timestamp).\n"
		"\t  Default: u. Please note that the server supports this parameter only when in unidirectional mode.\n"
		"\t  If a bidirectional INIT packet is received, the mode is completely ignored.\n"
		"  -I <interface index>: instead of using the first wireless/non-wireless interface, use the one with\n"
		"\t  the specified index. The index must be >= 0. Use -h to print the valid indeces. Default value: 0.\n"
		"  -e: use non-wireless interfaces instead of wireless ones. The default behaviour, without -e, is to\n"
		"\t  look for available wireless interfaces and return an error if none are found.\n"
		"  -p <port>: specifies the port to be used. Can be specified only if protocol is UDP (default: %d).\n"
		"\n"

		"Example of usage:\n"
		"Non-RAW sockets and UDP:\n"
		"  Client (port 7000, ping-like, 200 packets, one packet every 50 ms, LaMP payload size: 700 B, user-to-user):\n"
		"\t./%s -c 192.168.1.180 -p 7000 -B -u -t 50 -n 200 -P 700\n"
		"  Server (port 7000, timeout: 5000 ms):\n"
		"\t./%s -s -p 7000 -t 5000 -u\n\n"
		"RAW sockets and UDP:\n"
		"  Client (port 7000, ping-like, 200 packets, one packet every 50 ms, LaMP payload size: 700 B, user-to-user):\n"
		"\t./%s -c 192.168.1.180 -p 7000 -B -u -t 50 -n 200 -M D8:61:62:04:9C:A2 -P 700 -r\n"
		"  Server (port 7000, timeout: 5000 ms):\n"
		"\t./%s -s -p 7000 -t 5000 -u -r\n\n"
		"Non-RAW sockets and UDP, over loopback, with default options:\n"
		"  Client (port %d, ping-like, %d packets, one packet every %d ms, LaMP payload size: 0 B, user-to-user):\n"
		"\t./%s -m -u\n"
		"  Server (port %d, timeout: %d ms):\n"
		"\t./%s -l -B -u\n\n"

		"The source code is available at:\n"
		"%s\n",
		PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT,
		CLIENT_DEF_NUMBER,
		CLIENT_DEF_INTERVAL,
		DEFAULT_UDP_PORT,
		MIN_TIMEOUT_VAL_S,MIN_TIMEOUT_VAL_S,SERVER_DEF_TIMEOUT,
		DEFAULT_UDP_PORT,
		PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT,
		DEFAULT_UDP_PORT,CLIENT_DEF_NUMBER,CLIENT_DEF_INTERVAL,PROG_NAME_SHORT,
		DEFAULT_UDP_PORT,SERVER_DEF_TIMEOUT,PROG_NAME_SHORT,
		GITHUB_LINK);

		fprintf(stdout,"\nAvailable interfaces (use -I <index> to bind to a specific WLAN interface,\n"
			"or -I <index> -e to bind to a specific non-WLAN interface):\n");
		vifPrinter(stdout); // vifPrinter() from Rawsock library 0.2.1

	exit(EXIT_SUCCESS);
}

static void print_short_info_err(struct options *options) {
	options_free(options);

	fprintf(stdout,"\nUsage: %s [-c <destination address> [mode] | -l [mode] | -s | -m] [protocol] [options]\n"
		"%s [-h]: print help\n"
		"%s [-v]: print version information\n\n"
		"-c: client mode\n"
		"-s: server mode\n"
		"-l: loopback client mode (send packets to first loopback interface - does not support raw sockets)\n"
		"-m: loopback server mode (binds to the loopback interface - does not support raw sockets)\n\n",
		PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT);

	exit(EXIT_FAILURE);
}

void options_initialize(struct options *options) {
	int i; // for loop index

	options->protocol=UNSET_P;
	options->mode_cs=UNSET_MCS;
	options->mode_ub=UNSET_MUB;
	options->interval=0;
	options->number=CLIENT_DEF_NUMBER;
	options->payloadlen=0;

	// Initial UP is set to 'UINT8_MAX', as it should not be a valid value
	// When this value is detected by the application, no setsockopt is called
	options->macUP=UINT8_MAX;

	options->init=INIT_CODE;

	// IP-UDP specific (should be inserted inside a union when other protocols will be implemented)
	options->destIPaddr.s_addr=0;
	options->port=DEFAULT_UDP_PORT;

	for(i=0;i<6;i++) {
		options->destmacaddr[i]=0x00;
	}

	options->mode_raw=NON_RAW; // NON_RAW mode is selected by default

	options->filename=NULL;
	options->overwrite=0;

	options->dmode=0;
	options->terminator=0;

	options->latencyType=USERTOUSER; // Default: user-to-user latency

	options->nonwlan_mode=0;
	options->if_index=0;
}

unsigned int parse_options(int argc, char **argv, struct options *options) {
	int char_option;
	int values[6];
	int i; // for loop index
	uint8_t v_flag=0; // =1 if -v was selected, in order to exit immediately after reporting the requested information
	uint8_t M_flag=0; // =1 if a destination MAC address was specified. If it is not, and we are running in raw server mode, report an error
	uint8_t L_flag=0; // =1 if a latency type was explicitely defined (with -L), otherwise = 0
	uint8_t eI_flag=0; // =1 if either -e or -I (or both) was specified, otheriwse = 0
	/* 
	   The p_flag has been inserted only for future use: it is set as a port is explicitely defined. This allows to check if a port was specified
	   for a protocol without the concept of 'port', as more protocols will be implemented in the future. In that case, it will be possible to
	   print a warning and ignore the specified value.
	*/
	uint8_t p_flag=0; // =1 if a port was explicitely specified, otherwise = 0
	char *sPtr; // String pointer for strtoul() and strtol() calls.
	size_t filenameLen=0; // Filename length for the '-f' mode

	if(options->init!=INIT_CODE) {
		fprintf(stderr,"parse_options: you are trying to parse the options without initialiting\n"
			"struct options, this is not allowed.\n");
		return 1;
	}

	while ((char_option=getopt(argc, argv, VALID_OPTS)) != EOF) {
		switch(char_option) {
			case 'h':
				print_long_info();
				break;

			case 'u':
				fprintf(stdout,"Note: using normal UDP sockets, it is not possible to send packets\n"
					"\t over the loopback interface, as the program binds to wireless interfaces only.\n"
					"\t You can, however, send packets to yourself (using your own IP address), to get\n"
					"\t a very similar effect with respect to using loopback.\n");
				options->protocol=UDP;
				break;

			case 'r':
				fprintf(stderr,"Warning: root privilieges are required to use raw sockets.\n");
				options->mode_raw=RAW;
				break;

			case 'd':
				options->dmode=1;
				break;

			case 't':
				errno=0; // Setting errno to 0 as suggested in the strtoul() man page
				options->interval=strtoul(optarg,&sPtr,0);
				if(sPtr==optarg) {
					fprintf(stderr,"Cannot find any digit in the specified time interval.\n");
					print_short_info_err(options);
				} else if(errno) {
					fprintf(stderr,"Error in parsing the time interval.\n");
					print_short_info_err(options);
				}
				break;

			case 'n':
				errno=0; // Setting errno to 0 as suggested in the strtoul() man page
				options->number=strtoul(optarg,&sPtr,0);
				if(sPtr==optarg) {
					fprintf(stderr,"Cannot find any digit in the specified number of packets.\n");
					print_short_info_err(options);
				} else if(errno || options->number==0) {
					fprintf(stderr,"Error in parsing the number of packets.\n\t Please note that '0' is not accepted as value for -n).\n");
					print_short_info_err(options);
				}
				break;

			case 'c':
				if(options->mode_cs==LOOPBACK_CLIENT) {
					fprintf(stderr,"Error: normal client (-c) and loopback client (-l) are not compatible.\n");
					print_short_info_err(options);
				}

				if(options->mode_cs==UNSET_MCS) {
					options->mode_cs=CLIENT;
				} else {
					fprintf(stderr,"Error: only one option between -s|-m and -c|-l is allowed.\n");
					print_short_info_err(options);
				}

				// If everything is ok, parse the destination IP address
				if(inet_pton(AF_INET,optarg,&(options->destIPaddr))!=1) {
					fprintf(stderr,"Error in parsing the destination IP address (required with -s).\n");
					print_short_info_err(options);
				}
				break;

			case 's':
				if(options->mode_cs==LOOPBACK_SERVER) {
					fprintf(stderr,"Error: normal server (-s) and loopback server (-m) are not compatible.\n");
					print_short_info_err(options);
				}

				if(options->mode_cs==UNSET_MCS) {
					options->mode_cs=SERVER;
				} else {
					fprintf(stderr,"Error: only one option between -s|-m and -c|-l is allowed.\n");
					print_short_info_err(options);
				}
				break;

			case 'l':
				if(options->mode_cs==CLIENT) {
					fprintf(stderr,"Error: normal client (-c) and loopback client (-l) are not compatible.\n");
					print_short_info_err(options);
				}

				if(options->mode_cs==UNSET_MCS) {
					options->mode_cs=LOOPBACK_CLIENT;
				} else {
					fprintf(stderr,"Error: only one option between -s|-m and -c|-l is allowed.\n");
					print_short_info_err(options);
				}
				break;

			case 'm':
				if(options->mode_cs==SERVER) {
					fprintf(stderr,"Error: normal server (-s) and loopback server (-m) are not compatible.\n");
					print_short_info_err(options);
				}

				if(options->mode_cs==UNSET_MCS) {
					options->mode_cs=LOOPBACK_SERVER;
				} else {
					fprintf(stderr,"Error: only one option between -s|-m and -c|-l is allowed.\n");
					print_short_info_err(options);
				}
				break;

			case 'p':
				errno=0; // Setting errno to 0 as suggested in the strtol() man page
				options->port=strtoul(optarg,&sPtr,0);
				if(sPtr==optarg) {
					fprintf(stderr,"Cannot find any digit in the specified port.\n");
					print_short_info_err(options);
				} else if(errno || options->port<1 || options->port>65535) {
					fprintf(stderr,"Error in parsing the port.\n");
					print_short_info_err(options);
				}
				// If the UDP source port is equal to the fixed one we chose for the client,
				// print a warning and use CLIENT_SRCPORT+1
				if(options->port==CLIENT_SRCPORT) {
					options->port=CLIENT_SRCPORT+1;
					fprintf(stderr,"Port cannot be equal to the raw client source port (%d). %ld will be used instead.\n",CLIENT_SRCPORT,options->port);
				}

				p_flag=1;
				break;

			case 'f':
				filenameLen=strlen(optarg)+1;
				if(filenameLen>1) {
					options->filename=malloc((filenameLen+CSV_EXTENSION_LEN)*sizeof(char));
					if(!options->filename) {
						fprintf(stderr,"Error in parsing the filename: cannot allocate memory.\n");
						print_short_info_err(options);
					}
					strncpy(options->filename,optarg,filenameLen);
					strncat(options->filename,CSV_EXTENSION_STR,CSV_EXTENSION_LEN);
				} else {
					fprintf(stderr,"Error in parsing the filename: null string length.\n");
					print_short_info_err(options);
				}
				break;

			case 'o':
				options->overwrite=1;
				break;

			case 'e':
				options->nonwlan_mode=1;
				eI_flag=1;
				break;

			case 'v':
				fprintf(stdout,"%s, version %s, date %s\n",PROG_NAME_LONG,VERSION,DATE);
				v_flag=1;
				break;

			case 'A':
				// This requires a patched kernel: print a warning!
				fprintf(stderr,"Warning: the use of the -A option requires a patched kernel.\n"
					"See: https://github.com/francescoraves483/OpenWrt-V2X\n"
					"Or: https://github.com/florianklingler/OpenC2X-embedded.\n");
				// Mapping between UP (0 to 7) and AC (BK to VO)
				if(strcmp(optarg,"BK") == 0) {
					options->macUP=1; // UP=1 (2) is AC_BK
				} else if(strcmp(optarg,"BE") == 0) {
					options->macUP=0; // UP=0 (3) is AC_BE
				} else if(strcmp(optarg,"VI") == 0) {
					options->macUP=4; // UP=4 (5) is AC_VI
				} else if(strcmp(optarg,"VO") == 0) {
					options->macUP=6; // UP=6 (7) is AC_VO
				} else {
				// Leave to default (UINT8_MAX), i.e. no AC is set to socket, and print error
					fprintf(stderr, "Invalid AC specified with -A.\nValid ones are: BK, BE, VI, VO.\nNo AC will be set.\n");
				}
				break;

			case 'B':
				if(options->mode_ub==UNSET_MUB) {
					options->mode_ub=PINGLIKE;
				} else {
					fprintf(stderr,"Only one option between -B and -U is allowed.\n");
					print_short_info_err(options);
				}
				break;

			case 'M':
				if(sscanf(optarg,SCN_MAC,MAC_SCANNER(values))!=6) {
					fprintf(stderr,"Error when reading the destination MAC address after -M.\n");
					print_short_info_err(options);
				}
				for(i=0;i<6;i++) {
					options->destmacaddr[i]=(uint8_t) values[i];
				}
				M_flag=1;
				break;

			case 'P':
				if(sscanf(optarg,"%" SCNu16, &(options->payloadlen))==EOF) {
					fprintf(stderr,"Error when reading the payload length after -P.\n");
					print_short_info_err(options);
				}
				break;

			case 'U':
				if(options->mode_ub==UNSET_MUB) {
					options->mode_ub=UNIDIR;
				} else {
					fprintf(stderr,"Only one option between -B and -U is allowed.\n");
					print_short_info_err(options);
				}
				fprintf(stderr,"Warning: the use of the -U option requires the system clock to be perfectly\n"
					"synchronized with a common reference.\n"
					"In case you are not sure if the clock is synchronized, please use -B instead.\n");
				break;

			case 'L':
				if(strlen(optarg)!=1) {
					fprintf(stderr,"Error: only one character shall be specified after -L.\n");
					print_short_info_err(options);
				}

				if(optarg[0]!='r' && optarg[0]!='u') {
					fprintf(stderr,"Error: valid -L options: 'u', 'r'.\n");
					print_short_info_err(options);
				}

				switch(optarg[0]) {
					case 'u':
						options->latencyType=USERTOUSER;
						break;

					case 'r':
						options->latencyType=KRT;
						break;

					default:
						options->latencyType=UNKNOWN;
					break;
				}

				L_flag=1;

				break;

			case 'I':
				errno=0; // Setting errno to 0 as suggested in the strtol() man page
				options->if_index=strtol(optarg,&sPtr,0);
				if(sPtr==optarg) {
					fprintf(stderr,"Cannot find any digit in the specified interface index.\n");
					print_short_info_err(options);
				} else if(errno || options->if_index<0) {
					fprintf(stderr,"Error: invalid interface index.\n");
					print_short_info_err(options);
				}
				eI_flag=1;
				break;

			default:
				print_short_info_err(options);

		}

	}

	if(v_flag) {
		exit(EXIT_SUCCESS); // Exit with SUCCESS code if -v was selected
	}

	if(options->mode_cs==UNSET_MCS) {
		fprintf(stderr,"Error: a mode must be specified, either client (-c) or server (-s).\n");
		print_short_info_err(options);
	} else if(options->mode_cs==CLIENT) {
		if(options->mode_raw==RAW && M_flag==0) {
			fprintf(stderr,"Error: in this initial version, the raw client requires the destionation MAC address too (with -M).\n");
			print_short_info_err(options);
		}
		if(options->protocol==UDP && options->destIPaddr.s_addr==0) {
			fprintf(stderr,"Error: when in UDP client mode, an IP address should be correctly specified.\n");
			print_short_info_err(options);
		}
		if(options->mode_ub==UNSET_MUB) {
			fprintf(stderr,"Error: in client mode either ping-like (-B) or unidirectional (-U) communication should be specified.\n");
			print_short_info_err(options);
		}
	} else if(options->mode_cs==SERVER) {
		if(options->mode_ub!=UNSET_MUB) {
			fprintf(stderr,"Warning: -B or -U was specified, but in server (-s) mode these parameters are ignored.\n");
		}
	} else if(options->mode_cs==LOOPBACK_CLIENT) {
		if(eI_flag==1) {
			fprintf(stderr,"Error: -I/-e are not supported when using loopback interfaces, as only one interface is used.\n");
			print_short_info_err(options);
		}
		if(options->mode_raw==RAW) {
			fprintf(stderr,"Error: raw sockets are not supported in loopback clients.\n");
			print_short_info_err(options);
		}
		if(options->mode_ub==UNSET_MUB) {
			fprintf(stderr,"Error: in loopback client mode either ping-like (-B) or unidirectional (-U) communication should be specified.\n");
			print_short_info_err(options);
		}
	} else if(options->mode_cs==LOOPBACK_SERVER) {
		if(eI_flag==1) {
			fprintf(stderr,"Error: -I/-e are not supported when using loopback interfaces, as only one interface is used.\n");
			print_short_info_err(options);
		}
		if(options->mode_raw==RAW) {
			fprintf(stderr,"Error: raw sockets are not supported in loopback servers.\n");
			print_short_info_err(options);
		}
		if(options->mode_ub!=UNSET_MUB) {
			fprintf(stderr,"Warning: -B or -U was specified, but in loopback server (-m) mode these parameters are ignored.\n");
		}
	}

	if(options->interval==0) {
		if(options->mode_cs==CLIENT || options->mode_cs==LOOPBACK_CLIENT) {
			// Set the default periodicity value if no explicit value was defined
			options->interval=CLIENT_DEF_INTERVAL;
		} else if(options->mode_cs==SERVER || options->mode_cs==LOOPBACK_SERVER) {
			// Set the default timeout value if no explicit value was defined
			options->interval=SERVER_DEF_TIMEOUT;
		}
	}

	// Important note: when adding futher protocols that cannot support, somehow, raw sockets, always check for -r not being set

	// Check for -L and -B/-U consistency (-L supported only with -B in clients, -L supported only with -U in servers, otherwise, it is ignored)
	if(L_flag==1 && (options->mode_cs==CLIENT || options->mode_cs==LOOPBACK_CLIENT) && options->mode_ub!=PINGLIKE) {
		fprintf(stderr,"Error: latency type can be specified only when the client is working in ping-like mode (-B).\n");
		print_short_info_err(options);
	}

	// Check consistency between parameters
	if(options->protocol==UNSET_P) {
		fprintf(stderr,"Error: a protocol must be specified. Supported protocols: %s\n",SUPPORTED_PROTOCOLS);
		print_short_info_err(options);
	}

	if(options->protocol==UDP && options->payloadlen>MAX_PAYLOAD_SIZE_UDP_LAMP) {
		fprintf(stderr,"Error: the specified LaMP payload length is too big.\nPayloads up to %d B are supported with UDP.\n",MAX_PAYLOAD_SIZE_UDP_LAMP);
		print_short_info_err(options);
	}

	if(options->overwrite==1 && options->filename==NULL) {
		fprintf(stderr,"Error: '-o' (overwrite mode) can be specified only when the output to a file (with -f) is requested.\n");
		print_short_info_err(options);
	}

	if(options->filename!=NULL && options->mode_cs==SERVER) {
		fprintf(stderr,"Error: '-f' is client-only, since only the client can print reports in the current version.\n");
		print_short_info_err(options);
	}

	// Print a warning in case a MAC address was specified with -M in UDP non raw mode, as the MAC is obtained through ARP and this argument will be ignored
	if(options->protocol==UDP && options->mode_raw==NON_RAW && M_flag==1) {
		fprintf(stderr,"Warning: a destination MAC address has been specified, but it will be ignored and obtained through ARP.\n");
	}

	return 0;
}

void options_free(struct options *options) {
	if(options->filename) {
		free(options->filename);
	}
}

void options_set_destIPaddr(struct options *options, struct in_addr destIPaddr) {
	options->destIPaddr=destIPaddr;
}

const char * latencyTypePrinter(latencytypes_t latencyType) {
	// enum can be used as index array, provided that the order inside the definition of latencytypes_t is the same as the one inside latencyTypes[]
	return latencyTypes[latencyType];
}