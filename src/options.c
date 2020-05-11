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
#include "rawsock.h"
#include "timer_man.h"

#define CSV_EXTENSION_LEN 4 // '.csv' length
#define CSV_EXTENSION_STR ".csv"
#define TXRX_STR_LEN 3 // '_tx/_rx' length
#define TX_STR ".tx"
#define RX_STR ".rx"

// Macros to perform stringizing: STRINGIFY(MACRO) will convert the macro expansion of MACRO into a proper string
#define STRINGIFY(value) STR(value)
#define STR(value) #value

// Option strings: defined here the description for each option to be then included inside print_long_info()
#define OPT_M_client \
	"  -M <destination MAC address>: specifies the destination MAC address.\n" \
	"\t  Mandatory only if socket is RAW ('-r' is selected) and protocol is UDP.\n"
#define OPT_q_client \
	"  -q <queue name>: name of an AMQP queue or topic (prepend with topic://) to be used. Client and server\n" \
	"\t  shall use the same queue name. This options applies only when AMQP 1.0 is used as a protocol.\n" \
	"\t  Two queues will be created: one for producer-to-consumer communication (.tx will be appended) and\n" \
	"\t  one from consumer-to-producer communication (.rx will be appended.\n"
#define OPT_n_client \
	"  -n <total number of packets to be sent>: specifies how many packets to send (default: "STRINGIFY(CLIENT_DEF_NUMBER)").\n"
#define OPT_t_client \
	"  -t <time interval in ms>: specifies the periodicity, in milliseconds, to send at (default: "STRINGIFY(CLIENT_DEF_INTERVAL)" ms).\n"
#define OPT_R_client \
	"  -R <random interval distrbution string>: allows the user to select a random periodicity, between\n" \
	"\t  the packets which will be sent, instead of using a fixed one (i.e. instead of sending exactly\n" \
	"\t  one packet every '-t' milliseconds). As argument, a string is expected, which should have the\n" \
	"\t  following format: <random distribution type character><distrbution parameter>,<optional batch size>\n" \
	"\t    <random distrbution type character>: can be 'u' for uniform, 'U' for improved uniform (no modulo\n" \
	"\t      bias but may cause an increase processing time when selecting a new random interval), 'e' for\n" \
	"\t      exponential, 'n' for a truncated normal between 1 ms and 2*<-t value>-1 ms\n" \
	"\t    <distribution parameter>: it represents the lower limit to select random numbers from for 'u' and 'U',\n" \
	"\t      the distribution mean for 'e' and the standard deviation for 'n'\n" \
	"\t    <optional batch size>: the optional batch size, separated from the rest of the string, is optional and\n" \
	"\t      it can be used to select how many packets should be sent before selecting a new random interval; if\n" \
	"\t      not specified, "STRINGIFY(BATCH_SIZE_DEF)" packets will be sent with a given random periodicity, before moving to a new random\n" \
	"\t      value of the periodicity itself. Using a value of '1' means that each packet will be sent after a\n" \
	"\t      randomly selected amount of time.\n" \
	"\t  When using -R, the value after -t will also assume a different meaning, in particular:\n" \
	"\t    -the upper limit to select random numbers from, for 'u' and 'U'\n" \
	"\t    -the distribution location parameter (i.e. its \"starting point\"), for 'e'\n" \
	"\t    -the distribution mean, for 'n'\n" \
	"\t  Examples:\n" \
	"\t  uniform with a lower limit of 10 ms and an upper limit of 100 ms (batch size: default): -t 100 -R u10\n" \
	"\t  uniform, as before, but with a batch size of 20 packets: -t 100 -R u10,20\n" \
	"\t  truncated normal between 1 ms and 399 ms, mean: 200 ms, std dev: 4 ms, batch: 15: -t 200 -R n4,15\n"
#define OPT_f_client \
	"  -f <filename, without extension>: print the report to a CSV file other than printing\n" \
	"\t  it on the screen.\n" \
	"\t  The default behaviour will append to an existing file; if the file does not exist,\n" \
	"\t  it is created.\n"
#define OPT_o_client \
	"  -o: valid only with '-f'; instead of appending to an existing file, overwrite it.\n"
#define OPT_y_both \
	"  -y: valid only with '-W': instead of creating new CSV files when the specified one already exists, just\n" \
	"\t  overwrite it.\n"
#define OPT_P_client \
	"  -P <payload size in B>: specify a LaMP payload length, in bytes, to be included inside\n" \
	"\t  the packet (default: 0 B).\n"
#define OPT_r_both \
	"  -r: use raw sockets, if supported for the current protocol.\n" \
	"\t  When '-r' is set, the program tries to insert the LaMP timestamp in the last \n" \
	"\t  possible instant before sending. 'sudo' (or proper permissions) is required in this case.\n"
#define OPT_A_both \
	"  -A <access category: BK | BE | VI | VO>: forces a certain EDCA MAC access category to\n" \
	"\t  be used (patched kernel required!).\n"
#define OPT_L_client \
	"  -L <latency type: u | r | s | h>: select latency type: user-to-user, KRT (Kernel Receive Timestamp),\n" \
	"\t  software kernel transmit and receive timestamps (only when supported by the NIC) or hardware\n" \
	"\t  timestamps (only when supported by the NIC)\n" \
	"\t  Default: u. Please note that the client supports this parameter only when in bidirectional mode.\n"
#define OPT_I_both \
	"  -I <interface index>: instead of using the first wireless/non-wireless interface, use the one with\n" \
	"\t  the specified index. The index must be >= 0. Use -h to print the valid indeces. Default value: 0.\n"
#define OPT_e_both \
	"  -e: use non-wireless interfaces instead of wireless ones. The default behaviour, without -e, is to\n" \
	"\t  look for available wireless interfaces and return an error if none are found.\n"
#define OPT_N_both \
	"  -N: instead of binding to a specific interface, bind to any local interface. Non raw sockets only.\n"
#define OPT_S_both \
	"  -S <interface name>: instead of automatically looking for available interfaces, use a specific interface\n" \
	"\t  which name is specified after this option. The suggestion is to rely on this option only when an interface\n" \
	"\t  is not listed when using -h (it may happen for AF_INET interfaces related to 4G modules, for instance).\n"
#define OPT_p_both \
	"  -p <port>: specifies the port to be used. Can be specified only if protocol is UDP (default: "STRINGIFY(DEFAULT_LATE_PORT)") or AMQP.\n"
#define OPT_C_client \
	"  -C <confidence interval mask>: specifies an integer (mask) telling the program which confidence\n" \
	"\t  intervals to display (0 = none, 1 = .90, 2 = .95, 3 = .90/.95, 4= .99, 5=.90/.99, 6=.95/.99\n" \
	"\t  7=.90/.95/.99 (default: "STRINGIFY(DEF_CONFIDENCE_INTERVAL_MASK)").\n"
#define OPT_F_client \
	"  -F: enable the LaMP follow-up mechanism. At the moment only the ping-like mode is supporting this.\n" \
	"\t  This mechanism will send an additional follow-up message after each server reply, containing an\n" \
	"\t  estimate of the server processing time, which is computed depending on the chosen latency type.\n"
#define OPT_T_client \
	"  -T <time in ms>: Manually set a client timeout. The client timeout is always computed as:\n" \
	"\t  ("STRINGIFY(MIN_TIMEOUT_VAL_C)" + x) ms if |-t value| <= "STRINGIFY(MIN_TIMEOUT_VAL_C)" ms or (|-t value| + x) ms if |-t value| > "STRINGIFY(MIN_TIMEOUT_VAL_C)" ms; with -T you can\n" \
	"\t  set the 'x' value, in ms (default: " STRINGIFY(CLIENT_DEF_TIMEOUT)")\n"
#define OPT_W_both \
	"  -W <filename, without extension>: write, for the current test only, the single packet latency\n" \
	"\t  measurement data to the specified CSV file.\n" \
	"\t  When this option is specified LaTe wull check if the specified .csv file already exists. If yes,\n" \
	"\t  it will try to create a new file with name <filename>_0001.csv. If also this file is already existing\n" \
    "\t  it will try to create <filename>_0002.csv, and so on, until 9999 attemps are performed. When all the\n" \
    "\t  available attempts have been performed, it will simply append to <filename>.csv.\n" \
	"\t  Warning! This option may negatively impact performance.\n"
#define OPT_V_both \
	"  -V: turn on verbose mode; this is currently work in progress but specifying this option will print\n" \
	"\t  additional information when each test is performed. Not all modes/protocol will print more information\n" \
	"\t  when this mode is activated.\n"
#define OPT_w_both \
	"  -w <IPv4:port>: output per-packet and report data, just like -W and -f for a CSV file, to a socket, which can then be\n" \
	"\t  read by any other application for further processing. To improve usability, the data is sent towards the selected IPv4\n" \
	"\t  and port in a textual, CSV-like human-readable format.\n" \
	"\t  When no port is specified, "STRINGIFY(DEFAULT_W_SOCKET_PORT)" will be used.\n" \
	"\t  After <IPv4:port>, it is possibile to specify an interface, through its name, to bind the socket to, for instance:\n" \
	"\t  '-w 192.168.1.101:46001,enp2s0'; if no interface is specified, the socket will be bound to all interfaces.\n" \
	"\t  This options involves the usage of two parallel sockets, bounded and connected to the same port (and, optionally,\n" \
	"\t  interface): one UDP socket, for sending the per-packer data, and one TCP socket, for sending more sentitive data,\n" \
	"\t  namely an initial packet telling how to interpret the comma separated fields sent via the UDP socket, for the\n" \
	"\t  per-packet data, and a final packet containing the final report data (including min, max, average, and so on).\n" \
	"\t  The initial packet is formatted in this way: 'LaTeINIT,<LaMP ID>,fields=f1;f2;...;fn', where <LaMP ID> is the\n" \
	"\t  current test ID, and 'fields=' is telling the meaning of the comma-separated fields which will be sent via UDP,\n" \
	"\t  e.g. 'LaTeINIT,55321,fields=seq;latency;tx_timestamp;error'.\n" \
	"\t  Each UDP packet will be instead formatted in this way: 'LaTe,<LaMP ID>,<f1>,<f2>,...,<fn>', where <fi> is the value\n" \
	"\t  of the i-th field, as described in the first TCP packet, e.g. 'LaTe,55321,2,0.388,1588953642.084513,0'.\n" \
	"\t  Then, at the end of a test, a final packet will be sent via TCP, formatted as: 'LaTeEND,<LaMP ID>,f1=<f1>,...,fn=<fn>'\n" \
	"\t  where fi=<fi> are fields containing the final report data, e.g. timestamp=1588953727,clientmode=pinglike,..., reported\n" \
	"\t  in a very similar way to what it is normally saved inside an -f CSV file.\n" \
	"\t  An application reading this data should:\n" \
	"\t    1) Create a TCP socket, as server, waiting for a LaTe instance (TCP client) to connect. There should be then two cases: \n" \
	"\t    a.1) If 'LateINIT' is received, save which per-packet fields will be sent via UDP and the current test ID and start\n" \
	"\t         waiting for and receiving UDP packets, after opening a UDP socket.\n" \
	"\t    a.2) Discard all the UDP packets which are not starting with 'LaTe' and the correct ID, saved in (a.1).\n" \
	"\t    a.3) When a UDP packet is received, parse/process the data contained inside using the fields received in (a.1).\n" \
	"\t    a.4) When 'LateEND' is received and if it has the right ID, stop waiting for new UDP packets and save all the report data\n" \
	"\t         contained inside, processing it depending on the user needs. If 'LaTeEND' has the third field set to 'srvtermination',\n" \
	"\t         it should be considered just a way to gracefully terminate the current connection after a unidirectional test, without\n" \
	"\t         carrying any particular final report data.\n" \
	"\t    b.1) If 'LaTeEND' is received without any 'LaTeINIT' preceding it, a unidirectional client is involved and no per-packet\n" \
	"\t         data is expected to be received. In this case, just save all the report data contained inside, processing it\n" \
	"\t         depending on the user's needs.\n" \
	"\t    2) Close any TCP socket which was left open before accepting a new connection on the same port.\n"
#define OPT_X_both \
	"  -X: when -W/-w is specified, it is possible to print extra single packet information by specifying some characters\n" \
	"\t  after -X. In particular, 'p' will print a Packet Error Rate considering all the packets before the current one,\n" \
	"\t  'r' will print reconstructed non cyclical sequence numbers (i.e. monotonic increasing sequence numbers even\n" \
	"\t  when LaMP sequence numbers are cyclically reset between 65535, 'm' will print the maximum measured value .\n" \
	"\t  up to the current packet and 'n' will print the minimum measured value up to the current packet.\n" \
	"\t  'a' can be used as a shortcut to print all the available information.\n" \
	"\t  This option is valid only when -W or -w (or both) is selected.\n"
#define OPT_H_server \
	"  -H: specify the address of the AMQP 1.0 broker.\n"
#define OPT_t_server \
	"  -t <timeout in ms>: specifies the timeout after which the connection should be\n" \
	"\t  considered lost (minimum value: "STRINGIFY(MIN_TIMEOUT_VAL_S)" ms, otherwise "STRINGIFY(MIN_TIMEOUT_VAL_S)" ms will be automatically set - default: "STRINGIFY(SERVER_DEF_TIMEOUT)" ms).\n"
#define OPT_d_server \
	"  -d: set the server in 'continuous daemon mode': as a session is terminated, the server\n" \
	"\t  will be restarted and will be able to accept new packets from other clients.\n"
#define OPT_L_server \
	"  -L <latency type: u | r>: select latency type: user-to-user or KRT (Kernel Receive Timestamp).\n" \
	"\t  Default: u. Please note that the server supports this parameter only when in unidirectional mode.\n" \
	"\t  If a bidirectional INIT packet is received, the mode is completely ignored.\n"
#define OPT_0_server \
	"  -0: force refusing follow-up mode, even when a client is requesting to use it.\n"
#define OPT_1_server \
	"  -1: force printing that a packet was received after sending the corresponding reply, instead of as soon as\n" \
	"\t  a packet is received from the client; this can help reducing the server processing time a bit as no\n" \
	"\t  'printf' is called before sending a reply.\n"

static const char *latencyTypes[]={"Unknown","User-to-user","KRT","Software (kernel) timestamps","Hardware timestamps"};

// Safer implementation of strchr for the -w option, reading up to the maximum expected number of characters in -w (i.e. MAX_w_STRING_SIZE)
static int strchr_w_opt(char *str, size_t str_len, char character) {
	int found=0;

	if(str==NULL || str_len<=0) {
		return -1;
	}

	// Look for the character inside the string, looking at a maximum of MAX_w_STRING_SIZE characters, or up to str_len, or up to a '\0'
	for(int i=0;i<MAX_w_STRING_SIZE && str[i]!='\0' && i<str_len;i++) {
		if(str[i]==character) {
			found=1;
			break;
		}
	}

	return found;
}

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
		#if AMQP_1_0_ENABLED
		"  AMQP 1.0: <destination address> = <broker address>\n"
		#endif
		"\n"

		"[mode]:\n"
		"  Client operating mode (the server will adapt its mode depending on the incoming packets).\n"
		"  -B: ping-like bidirectional mode\n"
		"  -U: unidirectional mode (requires clocks to be perfectly synchronized - highly experimental\n"
		"\t  - use at your own risk!)\n"
		#if AMQP_1_0_ENABLED
		"\n"
		"  When using AMQP 1.0, only -U is supported. The LaMP client will act as producer and the LaMP\n"
		"\t  server as consumer. The only supported latency type is User-to-user, at the moment.\n"
		#endif
		"\n"

		"[protocol]:\n"
		"  Protocol to be used, in which the LaMP packets will be encapsulated.\n"
		"  -u: UDP\n"
		#if AMQP_1_0_ENABLED
		"  -a: AMQP 1.0 (using the Qpid Proton C library) - non raw sockets only - no loopback\n"
		#endif
		"\n"

		"[options] - Mandatory client options:\n"
			OPT_M_client
			#if AMQP_1_0_ENABLED
			OPT_q_client
			#endif
			"\n"

		"[options] - Optional client options:\n"
			// General options
			OPT_n_client
			OPT_p_both
			OPT_r_both
			OPT_t_client
			OPT_A_both
			OPT_C_client
			OPT_F_client
			OPT_L_client
			OPT_P_client
			OPT_R_client	
			OPT_T_client
			OPT_V_both

			// File options
			OPT_f_client
			OPT_o_client
			OPT_w_both
			"\t  When in unidirectional mode, no per-packet data or 'LaTeINIT' is sent with -w, as they are managed by\n"
			"\t  the server. A 'LaTeEND' packet, with the final statistics, will be sent via TCP at the end of the test\n"
			"\t  only.\n"
			OPT_y_both
			OPT_W_both
			"\t  This options applies to a client only in ping-like mode.\n"
			OPT_X_both

			// Interface options
			OPT_e_both
			OPT_I_both
			#if AMQP_1_0_ENABLED
			"\t  This option cannot be used for AMQP 1.0 as we have no control over the binding mechanism of Qpid Proton.\n"
			#endif
			OPT_N_both
			OPT_S_both
			#if AMQP_1_0_ENABLED
			"\t  This option cannot be used for AMQP 1.0 as we have no control over the binding mechanism of Qpid Proton.\n"
			#endif
			"\n"

		"[options] - Mandatory server options:\n"
			#if AMQP_1_0_ENABLED
			OPT_H_server
			#else
			"   <none>"
			#endif
			"\n"

		"[options] - Optional server options:\n"
			// General options
			OPT_d_server
			OPT_p_both
			OPT_r_both
			OPT_t_server
			OPT_A_both
			OPT_L_server
			OPT_V_both
			OPT_0_server
			OPT_1_server

			// File options
			OPT_w_both
			"\t  This options applies to a server only in unidirectional mode; in this case, 'LaTeEND' won't contain\n"
			"\t  any final report, but it will just be formatted as 'LaTe,<LaMP ID>,srvtermination' and it can be used\n"
			"\t  to gracefully terminate the connection from the application reading the -w data. A server, during a\n"
			"\t  unidirectional test, can only send per-packet data to an external application via TCP/UDP.\n"
			OPT_y_both
			OPT_W_both
			"\t  This options applies to a server only in unidirectional mode.\n"
			OPT_X_both

			// Interface options
			OPT_e_both
			OPT_I_both
			#if AMQP_1_0_ENABLED
			"\t  This option cannot be used for AMQP 1.0 as we have no control over the binding mechanism of Qpid Proton.\n"
			#endif
			OPT_N_both
			OPT_S_both
			#if AMQP_1_0_ENABLED
			"\t  This option cannot be used for AMQP 1.0 as we have no control over the binding mechanism of Qpid Proton.\n"
			#endif
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
		"\t./%s -l -B -u\n"
		"  Server (port %d, timeout: %d ms):\n"
		"\t./%s -m -u\n\n"

		"The source code is available at:\n"
		"%s\n",
		PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT, // Basic help
		PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT,PROG_NAME_SHORT, // Example of usage
		DEFAULT_LATE_PORT,CLIENT_DEF_NUMBER,CLIENT_DEF_INTERVAL,PROG_NAME_SHORT, // Example of usage
		DEFAULT_LATE_PORT,SERVER_DEF_TIMEOUT,PROG_NAME_SHORT, // Example of usage
		GITHUB_LINK); // Source code link

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
	options->client_timeout=CLIENT_DEF_TIMEOUT;
	options->number=CLIENT_DEF_NUMBER;
	options->payloadlen=0;

	// Initial UP is set to 'UINT8_MAX', as it should not be a valid value
	// When this value is detected by the application, no setsockopt is called
	options->macUP=UINT8_MAX;

	options->init=INIT_CODE;

	// IP-UDP specific (should be inserted inside a union when other protocols will be implemented)
	options->dest_addr_u.destIPaddr.s_addr=0;
	options->port=DEFAULT_LATE_PORT;

	for(i=0;i<6;i++) {
		options->destmacaddr[i]=0x00;
	}

	options->mode_raw=NON_RAW; // NON_RAW mode is selected by default

	options->filename=NULL;
	options->overwrite=0;
	options->overwrite_W=0;

	options->opt_devname=NULL;

	options->dmode=0;

	options->latencyType=USERTOUSER; // Default: user-to-user latency

	options->nonwlan_mode=NONWLAN_MODE_WIRELESS;
	options->if_index=0;

	options->confidenceIntervalMask=0x02; // Binary 010 as default value: i.e. print only .95 confidence intervals

	options->followup_mode=FOLLOWUP_OFF;
	options->refuseFollowup=0;

	options->verboseFlag=0;

	options->Wfilename=NULL;

	options->printAfter=0;

	options->dest_addr_u.destAddrStr=NULL;

	#if AMQP_1_0_ENABLED
	options->queueNameTx=NULL;
	options->queueNameRx=NULL;
	#endif

	options->rand_type=NON_RAND;
	options->rand_param=-1;

	options->rand_batch_size=BATCH_SIZE_DEF;

	options->report_extra_data=0; // No valid char specified when initializing the options structure

	// Initializing udp_params
	options->udp_params.port=DEFAULT_W_SOCKET_PORT;
	options->udp_params.devname=NULL;
	options->udp_params.enabled=0;
}

unsigned int parse_options(int argc, char **argv, struct options *options) {
	int char_option;
	int values[6];
	int i; // for loop index
	uint8_t v_flag=0; // = 1 if -v was selected, in order to exit immediately after reporting the requested information
	uint8_t M_flag=0; // = 1 if a destination MAC address was specified. If it is not, and we are running in raw server mode, report an error
	uint8_t L_flag=0; // = 1 if a latency type was explicitely defined (with -L), otherwise = 0
	uint8_t eI_flag=0; // = 1 if either -e or -I (or both) was specified, otheriwse = 0
	uint8_t C_flag=0; // = 1 if -C was specified, otheriwise = 0
	uint8_t F_flag=0; // = 1 if -F was specified, otherwise = 0
	uint8_t T_flag=0; // = 1 if -T was specified, otherwise = 0
	uint8_t N_flag=0; // = 1 if -N was specified, otherwise = 0

	char *sPtr; // String pointer for strtoul() and strtol() calls.
	size_t filenameLen=0; // Filename length for the '-f' mode

	#if AMQP_1_0_ENABLED
	size_t queueNameLen=0; // Queue name length for the '-q' mode
	uint8_t H_flag=0; // =1 if -H was specified, otherwise = 0
	#endif

	size_t destAddrLen=0; // Destination address string length

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
				options->protocol=UDP;
				break;

			#if AMQP_1_0_ENABLED
			case 'a':
				options->protocol=AMQP_1_0;
				// Artificially change the default port to the AMQP default port (i.e. 5672)
				options->port=5672;
				break;
			#endif

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

				// Parse string after -c (it will be kept as it is for AMQP 1.0, if enabled, or converted to an IP address for UDP/TCP/etc...)
				destAddrLen=strlen(optarg)+1;
				if(destAddrLen>1) {
					options->dest_addr_u.destAddrStr=malloc((destAddrLen)*sizeof(char));
					if(!options->dest_addr_u.destAddrStr) {
						fprintf(stderr,"Error in parsing the specified destination address: cannot allocate memory.\n");
						print_short_info_err(options);
					}
					strncpy(options->dest_addr_u.destAddrStr,optarg,destAddrLen);
				} else {
					fprintf(stderr,"Error in parsing the filename: null string length.\n");
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

				// Only if AMQP 1.0 is active, save a string with the port too, as it can be useful later on, without the need of converting
				// this value from unsigned long to char* again
				#if AMQP_1_0_ENABLED
				strncpy(options->portStr,optarg,MAX_PORT_STR_SIZE);
				#endif

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

			case 'y':
				options->overwrite_W=1;
				break;

			case 'e':
				options->nonwlan_mode=NONWLAN_MODE_WIRED;
				eI_flag=1;
				break;

			case 'v':
				fprintf(stdout,"%s, version %s, date %s\n",PROG_NAME_LONG,VERSION,DATE);
				v_flag=1;
				break;

			case 'w':
				{
				char *str_ptr;
				int opt_devnameLen=0;
				char *saveptr_strtok=NULL;
				uint8_t specified_fields=0x00;
				size_t optargLen=0;
				long port_long;

				// Check if the specified string has a valid size (it should contain at least an IP address, i.e. a minimum of 7 characters + '\0' in the worst case, and
				// it should be shorter than MAX_w_STRING_SIZE, as defined in options.h)
				optargLen=strlen(optarg)+1;

				if(optargLen<8 || optargLen>MAX_w_STRING_SIZE) {
					fprintf(stderr,"Error: the argument specified after -w is either too short or too long. Size: %zu, admitted sizes: [7,%d].\n",optargLen-1,MAX_w_STRING_SIZE-1);
					fprintf(stderr,"If you specified an interface name, please check if it is correct.\n");
					print_short_info_err(options);
				}

				// Check if an interface is specified directly after the IP address (i.e. a ',' is encountered instead of ':',
				// which is used to separate the port from the IP address)
				// In order to do so, we discriminate different cases setting different bits of specified_fields:
				// 1) If at least one ':' is found, we can infer that the port has been specified (set bit 1 in specified_fields)
				// 2) If one ',' is found, we can infer that an interface name has been specified (set bit 2 in specified_fields)
				// If both bits are set, we can conclude that both port and interface name were specified (both bit 1 and bit 2 are set)
				if(strchr_w_opt(optarg,optargLen,':')) {
					specified_fields |= 0x01;
				}

				if(strchr_w_opt(optarg,optargLen,',')) {
					specified_fields |= 0x02;
				}

				str_ptr=strtok_r(optarg,":,",&saveptr_strtok);

				// Parse IP addresss
				if(str_ptr==NULL || inet_pton(AF_INET,str_ptr,&(options->udp_params.ip_addr))!=1) {
					fprintf(stderr,"Error in parsing the destination IP address for the UDP socket (-w option).\n");
					print_short_info_err(options);
				}
				
				// If bit 1 in specified_fields is set, parse port number
				if(specified_fields & 0x01) {
					str_ptr=strtok_r(NULL,":,",&saveptr_strtok);

					if(str_ptr!=NULL) {
						if(sscanf(str_ptr,"%ld",&port_long)!=1 || port_long<1 || port_long>65535) {
							fprintf(stderr,"Error in parsing the port for the UDP socket (-w option). Bad value?\n");
							print_short_info_err(options);
						}
						options->udp_params.port=(uint16_t) port_long;
					}
				}

				// If bit 2 in specified_fields is set, parse the interface name
				if(specified_fields & 0x02) {
					str_ptr=strtok_r(NULL,":,",&saveptr_strtok);

					if(str_ptr!=NULL) {
						opt_devnameLen=strlen(str_ptr)+1;
						if(opt_devnameLen>1) {
							options->udp_params.devname=malloc(opt_devnameLen*sizeof(char));
							if(!options->udp_params.devname) {
								fprintf(stderr,"Error in parsing the interface name for the UDP socket (-w option): cannot allocate memory.\n");
								fprintf(stderr,"If this error persists, try not specifying any interface and bind to all the available ones.\n");
								print_short_info_err(options);
							}
							strncpy(options->udp_params.devname,str_ptr,opt_devnameLen);
						} else {
							fprintf(stderr,"Error in parsing the interface name for the UDP socket (-w option): null string length.\n");
							print_short_info_err(options);
						}
					}
				}

				options->udp_params.enabled=1;

				}
				break;

			#if AMQP_1_0_ENABLED
			case 'q':
				queueNameLen=strlen(optarg)+1;
				if(queueNameLen>1) {
					options->queueNameTx=malloc((queueNameLen+TXRX_STR_LEN)*sizeof(char));
					options->queueNameRx=malloc((queueNameLen+TXRX_STR_LEN)*sizeof(char));
					if(!options->queueNameTx || !options->queueNameRx) {
						fprintf(stderr,"Error in parsing the specified queue name: cannot allocate memory.\n");
						print_short_info_err(options);
					}
					strncpy(options->queueNameTx,optarg,queueNameLen);
					strncat(options->queueNameTx,TX_STR,TXRX_STR_LEN);

					strncpy(options->queueNameRx,optarg,queueNameLen);
					strncat(options->queueNameRx,RX_STR,TXRX_STR_LEN);
				} else {
					fprintf(stderr,"Error in parsing the queue name: null string length.\n");
					print_short_info_err(options);
				}
				break;
			#endif

			case 'X':
				{

				size_t optargLen=strlen(optarg);

				if(optargLen>REPORT_EXTRA_DATA_BIT_SIZE) {
					fprintf(stderr,"Error: a maximum of %d characters can be specified after -X.\n",REPORT_EXTRA_DATA_BIT_SIZE);
					print_short_info_err(options);
				}

				if(optargLen<=0) {
					fprintf(stderr,"Error: cannot find any character after -X.\n");
					print_short_info_err(options);
				}

				if(optargLen==1 && optarg[0]=='a') {
					SET_REPORT_DATA_ALL_BITS(options->report_extra_data);
				} else {
					for(int i=0;i<optargLen;i++) {
						if(optarg[i]=='p') {
							if(CHECK_REPORT_EXTRA_DATA_BIT_SET(options->report_extra_data,CHAR_P)) {
								fprintf(stderr,"Warning: speficied character '%c' after -X, but it was already selected.\n",'p');
							}

							SET_REPORT_EXTRA_DATA_BIT(options->report_extra_data,CHAR_P);
						} else if(optarg[i]=='r') {
							if(CHECK_REPORT_EXTRA_DATA_BIT_SET(options->report_extra_data,CHAR_R)) {
								fprintf(stderr,"Warning: speficied character '%c' after -X, but it was already selected.\n",'r');
							}

							SET_REPORT_EXTRA_DATA_BIT(options->report_extra_data,CHAR_R);
						} else if(optarg[i]=='m') {
							if(CHECK_REPORT_EXTRA_DATA_BIT_SET(options->report_extra_data,CHAR_M)) {
								fprintf(stderr,"Warning: speficied character '%c' after -X, but it was already selected.\n",'m');
							}

							SET_REPORT_EXTRA_DATA_BIT(options->report_extra_data,CHAR_M);
						} else if(optarg[i]=='n') {
							if(CHECK_REPORT_EXTRA_DATA_BIT_SET(options->report_extra_data,CHAR_N)) {
								fprintf(stderr,"Warning: speficied character '%c' after -X, but it was already selected.\n",'n');
							}

							SET_REPORT_EXTRA_DATA_BIT(options->report_extra_data,CHAR_N);
						} else if(optarg[i]=='a') {
							fprintf(stderr,"Error: 'a' was specified, together with other -X characters, but it should be used alone.\n");
							print_short_info_err(options);
						} else {
							fprintf(stderr,"Error: invalid character ('%c') specified after -X. Valid options:\n"
								"  'p': print 'PER till now' for each packet\n"
								"  'r': print 'Reconstructed (non cyclical) LaMP sequence numbers' for each packet\n"
								"  'a': print all the available information.\n",optarg[i]);
							print_short_info_err(options);
						}
					}
				}

				}

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

			case 'C':
				errno=0; // Setting errno to 0 as suggested in the strtol() man page
				options->confidenceIntervalMask=strtoul(optarg,&sPtr,0);
				if(sPtr==optarg) {
					fprintf(stderr,"Cannot find any digit in the specified mask.\n");
					print_short_info_err(options);
				} else if(errno || options->confidenceIntervalMask>0x07) {
					fprintf(stderr,"Error in parsing the mask specified after -C.\n");
					print_short_info_err(options);
				}
				C_flag=1;
				break;

			case 'F':
				F_flag=1;
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

			case 'N':
				options->nonwlan_mode=NONWLAN_MODE_ANY;
				N_flag=1;
				break;

			case 'P':
				if(sscanf(optarg,"%" SCNu16, &(options->payloadlen))==EOF) {
					fprintf(stderr,"Error when reading the payload length after -P.\n");
					print_short_info_err(options);
				}
				break;

			case 'R':
				// If the first character corresponds to a correct random distribution type, parse 'param',
				//  which will define an additional parameter for each distribution.
				// If available, parse also the batch size, in which the interval will be kept the same.
				switch(optarg[0]) {
					case 'u':
						if(sscanf(optarg,"%*c%lf,%" SCNu64,&(options->rand_param),&(options->rand_batch_size))<1) {
							fprintf(stderr,"Error parsing the lower interval limit for the uniform random interval.\n");
							print_short_info_err(options);
						}
						options->rand_type=RAND_PSEUDOUNIFORM;
						break;
					case 'U':
						if(sscanf(optarg,"%*c%lf,%" SCNu64,&(options->rand_param),&(options->rand_batch_size))<1) {
							fprintf(stderr,"Error parsing the lower interval limit for the uniform (improved) random interval.\n");
							print_short_info_err(options);
						}
						options->rand_type=RAND_UNIFORM;
						break;
					case 'e':
						if(sscanf(optarg,"%*c%lf,%" SCNu64,&(options->rand_param),&(options->rand_batch_size))<1) {
							fprintf(stderr,"Error parsing the mean for the exponential random interval.\n");
							print_short_info_err(options);
						}
						options->rand_type=RAND_EXPONENTIAL;
						break;
					case 'n':
						if(sscanf(optarg,"%*c%lf,%" SCNu64,&(options->rand_param),&(options->rand_batch_size))<1) {
							fprintf(stderr,"Error parsing the standard deviation for the normal random interval.\n");
							print_short_info_err(options);
						}
						options->rand_type=RAND_NORMAL;
						break;
					default:
						fprintf(stderr,"Error: the character '%c' you specified after '-R' does not correspond to any random distribution.\n"
							"Valid options are: 'u'='uniform','U'='uniform (improved, no modulo bias)','e'='exponential','n'='normal'\n",
							optarg[0]);
						print_short_info_err(options);
				}
				break;

			case 'S':
				{
				int opt_devnameLen=0;

				if(options->nonwlan_mode!=NONWLAN_MODE_WIRELESS) {
					fprintf(stderr,"Error when selecting -S: -e/-I or -N was already specified.\nPlease specify only one option between -e/-I, -N or -S.\n");
					print_short_info_err(options);
				}

				opt_devnameLen=strlen(optarg)+1;
				if(opt_devnameLen>1) {
					options->opt_devname=malloc(opt_devnameLen*sizeof(char));
					if(!options->opt_devname) {
						fprintf(stderr,"Error in parsing the interface name specified with -S: cannot allocate memory.\n");
						fprintf(stderr,"If this error persists, try using -e or -N instead.\n");
						print_short_info_err(options);
					}
					strncpy(options->opt_devname,optarg,opt_devnameLen);
				} else {
					fprintf(stderr,"Error in parsing the interface name specified with -S: null string length.\n");
					print_short_info_err(options);
				}

				options->nonwlan_mode=NONWLAN_MODE_FORCED_NAME;

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

			case 'V':
				options->verboseFlag=1;
				break;

			case 'L':
				if(strlen(optarg)!=1) {
					fprintf(stderr,"Error: only one character shall be specified after -L.\n");
					print_short_info_err(options);
				}

				if(optarg[0]!='r' && optarg[0]!='u' && optarg[0]!='s' && optarg[0]!='h') {
					fprintf(stderr,"Error: valid -L options: 'u', 'r', 'h' (not supported on every NIC).\n");
					print_short_info_err(options);
				}

				switch(optarg[0]) {
					case 'u':
						options->latencyType=USERTOUSER;
						break;

					case 'r':
						options->latencyType=KRT;
						break;

					case 's':
						options->latencyType=SOFTWARE;
						break;

					case 'h':
						options->latencyType=HARDWARE;
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

			case 'W':
				filenameLen=strlen(optarg)+1;
				if(filenameLen>1) {
					options->Wfilename=malloc((filenameLen+CSV_EXTENSION_LEN)*sizeof(char));
					if(!options->Wfilename) {
						fprintf(stderr,"Error in parsing the filename for the -W mode: cannot allocate memory.\n");
						print_short_info_err(options);
					}
					strncpy(options->Wfilename,optarg,filenameLen);
					strncat(options->Wfilename,CSV_EXTENSION_STR,CSV_EXTENSION_LEN);
				} else {
					fprintf(stderr,"Error in parsing the filename for the -W mode: null string length.\n");
					print_short_info_err(options);
				}
				break;

			case 'T':
				errno=0;
				options->client_timeout=strtoul(optarg,&sPtr,0);
				if(sPtr==optarg) {
					fprintf(stderr,"Cannot find any digit in the specified time interval.\n");
					print_short_info_err(options);
				} else if(errno) {
					fprintf(stderr,"Error in parsing the client timeout value.\n");
					print_short_info_err(options);
				}
				T_flag=1;
				break;

			#if AMQP_1_0_ENABLED
			case 'H':
				// Parse string after -H (AMQP 1.0 only)
				destAddrLen=strlen(optarg)+1;
				if(destAddrLen>1) {
					options->dest_addr_u.destAddrStr=malloc((destAddrLen)*sizeof(char));
					if(!options->dest_addr_u.destAddrStr) {
						fprintf(stderr,"Error in parsing the specified destination address: cannot allocate memory.\n");
						print_short_info_err(options);
					}
					strncpy(options->dest_addr_u.destAddrStr,optarg,destAddrLen);
				} else {
					fprintf(stderr,"Error in parsing the destination address: null string length.\n");
					print_short_info_err(options);
				}

				H_flag=1;
			#endif

			case '0':
				options->refuseFollowup=1;
				break;

			case '1':
				options->printAfter=1;
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
		// Parse destination IP address if protocol is not AMQP
		if(options->protocol==UDP) {
			struct in_addr destIPaddr_tmp;
			if(inet_pton(AF_INET,options->dest_addr_u.destAddrStr,&(destIPaddr_tmp))!=1) {
				fprintf(stderr,"Error in parsing the destination IP address (required with -s).\n");
				print_short_info_err(options);
			}
			free(options->dest_addr_u.destAddrStr);

			options->dest_addr_u.destIPaddr=destIPaddr_tmp;
		}

		if(options->mode_raw==RAW && M_flag==0) {
			fprintf(stderr,"Error: in this initial version, the raw client requires the destionation MAC address too (with -M).\n");
			print_short_info_err(options);
		}
		if(options->protocol==UDP && options->dest_addr_u.destIPaddr.s_addr==0) {
			fprintf(stderr,"Error: when in UDP client mode, an IP address should be correctly specified.\n");
			print_short_info_err(options);
		}
		if(options->mode_ub==UNSET_MUB) {
			fprintf(stderr,"Error: in client mode either ping-like (-B) or unidirectional (-U) communication should be specified.\n");
			print_short_info_err(options);
		}
		if(options->printAfter==1) {
			fprintf(stderr,"Warning: -1 was specified but it will be ignored, as it is a server only option.\n");
		}
	} else if(options->mode_cs==SERVER) {
		if(options->mode_ub!=UNSET_MUB) {
			fprintf(stderr,"Warning: -B or -U was specified, but in server (-s) mode these parameters are ignored.\n");
		}
		if(C_flag==1) {
			fprintf(stderr,"Warning: -C is a client-only option. It will be ignored.\n");
		}
		if(T_flag==1) {
			fprintf(stderr,"Warning: -T is a client-only option. It will be ignored.\n");
		}
		if(options->rand_type!=NON_RAND) {
			fprintf(stderr,"Error: -R is a client-only option.\n");
			print_short_info_err(options);
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
		if(options->printAfter==1) {
			fprintf(stderr,"Warning: -1 was specified but it will be ignored, as it is a server only option.\n");
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
		if(C_flag==1) {
			fprintf(stderr,"Warning: -C is a client-only option. It will be ignored.\n");
		}
		if(T_flag==1) {
			fprintf(stderr,"Warning: -T is a client-only option. It will be ignored.\n");
		}
		if(options->rand_type!=NON_RAND) {
			fprintf(stderr,"Error: -R is a client-only option.\n");
			print_short_info_err(options);
		}
	}

	#if AMQP_1_0_ENABLED
	// No raw sockets are supported for AMQP 1.0
	if(options->protocol==AMQP_1_0) {
		if(options->mode_raw==RAW) {
			fprintf(stderr,"Error: AMQP 1.0 does not support raw sockets yet.\n");
			print_short_info_err(options);
		}

		if(options->mode_cs==CLIENT && H_flag==1) {
			fprintf(stderr,"Error: -H is a server only AMQP 1.0 option.\n");
			print_short_info_err(options);
		}

		if(options->mode_cs==SERVER && H_flag==0) {
			fprintf(stderr,"Error: running in LaMP server mode but no broker address specified.\n");
			fprintf(stderr,"Please specify one with -H.\n");
			print_short_info_err(options);
		}

		if(options->queueNameRx==NULL || options->queueNameTx==NULL) {
			fprintf(stderr,"Error: you should specify a queue/topic name to be used with -q.\n"
				"LaTe will then use two queues for AMQP communication, i.e.\n"
				"<specified queue name>_tx and <specified queue name>_rx.\n");
			print_short_info_err(options);
		}

		// No -e or -I can be specified (print a warning telling that they will be ignored)
		if(eI_flag==1) {
			fprintf(stderr,"Warning: -e or -I was specified but it will be ignored.\n");
		}

		// Only undirectional mode is supported as of now (with consumer/producer which can be run
		//  on the same PC, or on different devices if they have a synchronized clock)
		if(options->mode_ub==PINGLIKE) {
			fprintf(stderr,"Error: AMQP 1.0 does not support bidirectional mode yet.\n");
			print_short_info_err(options);
		}

		// No loopback client is supported for AMQP 1.0
		if(options->mode_cs==LOOPBACK_CLIENT || options->mode_cs==LOOPBACK_SERVER) {
			fprintf(stderr,"Error: AMQP 1.0 does not support loopback clients/servers.\n");
			print_short_info_err(options);
		}

		if(options->macUP!=UINT8_MAX) {
			fprintf(stderr,"Warning: no priority can be set for AMQP 1.0 packets for the time being.\n");
			fprintf(stderr,"\t The specified Access Category will be ignored.\n");
		}

		// Only user-to-user latency is supported with AMQP 1.0, for the moment
		if(options->latencyType!=USERTOUSER) {
			fprintf(stderr,"Error: you specified latency type '%c' but only user-to-user is supported with AMQP 1.0.\n",options->latencyType);
			print_short_info_err(options);
		}

		if(N_flag==1) {
			fprintf(stderr,"Warning: -N was specified, but for AMQP 1.0 it is already the default option\n"
				"(i.e. there is no biding to specific interfaces yet).\n");
		}
	}
	#endif

	// If rand type is NON_RAND, set the batch size equal to the number specified with -n
	// This is done only for saving this value in the CSV file/print it when a client starts when no random distribution is selected
	// It should have no other practical implications as NON_RAND ignores any random periodicity batch size
	if(options->rand_type==NON_RAND) {
		options->rand_batch_size=options->number;
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

	// Check the consistency of the parameters specified after -R, if -R was specified (i.e. if options->rand_type!=NON_RAND)
	if(options->rand_type!=NON_RAND) {
		char *consistency_check_str=NULL;

		consistency_check_str=timerRandDistribCheckConsistency(options->interval,options->rand_param,options->rand_type);

		if(consistency_check_str!=NULL) {
			fprintf(stderr,"Error when specifying the '-R' value: %s.\n",consistency_check_str);
			print_short_info_err(options);
		}
	}

	// Important note: when adding futher protocols that cannot support, somehow, raw sockets, always check for -r not being set

	// Check for -L and -B/-U consistency (-L supported only with -B in clients, -L supported only with -U in servers, otherwise, it is ignored)
	if(L_flag==1 && (options->mode_cs==CLIENT || options->mode_cs==LOOPBACK_CLIENT) && options->mode_ub!=PINGLIKE) {
		fprintf(stderr,"Error: latency type can be specified only when the client is working in ping-like mode (-B).\n");
		print_short_info_err(options);
	}

	// -L h cannot be specified in unidirectional mode (i.e. a server can never specify 'h' as latency type)
	if((options->mode_cs==SERVER || options->mode_cs==LOOPBACK_SERVER) && options->latencyType==HARDWARE) {
		fprintf(stderr,"Error: hardware timestamps are only supported by clients and in ping-like mode (-B).\n");
		print_short_info_err(options);
	}

	// -L s cannot be specified in unidirectional mode (i.e. a server can never specify 's' as latency type)
	if((options->mode_cs==SERVER || options->mode_cs==LOOPBACK_SERVER) && options->latencyType==SOFTWARE) {
		fprintf(stderr,"Error: kernel receive/transmit timestamps are only supported by clients and in ping-like mode (-B).\n");
		print_short_info_err(options);
	}

	// Check consistency between parameters
	if((options->mode_cs==CLIENT || options->mode_cs==LOOPBACK_CLIENT) && options->refuseFollowup==1) {
		fprintf(stderr,"Error: -0 (refuse follow-up requests) is a server only option.");
		print_short_info_err(options);
	}

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

	if(options->overwrite_W==1 && options->Wfilename==NULL) {
		fprintf(stderr,"Error: '-y' (overwrite mode for -W files) can be specified only when the output to a file (with -W) is requested.\n");
		print_short_info_err(options);
	}

	if(options->filename!=NULL && options->mode_cs==SERVER) {
		fprintf(stderr,"Error: '-f' is client-only, since only the client can print reports in the current version.\n");
		print_short_info_err(options);
	}

	if((options->mode_cs==SERVER || options->mode_cs==LOOPBACK_SERVER) && F_flag==1) {
		fprintf(stderr,"Error: '-F' is client-only.\n");
		print_short_info_err(options);
	}

	if(options->mode_ub==UNIDIR && F_flag==1) {
		fprintf(stderr,"Error: '-F' is supported in ping-like mode only, at the moment.\n");
		print_short_info_err(options);
	}

	if(eI_flag==1 && N_flag==1) {
		fprintf(stdout,"Error: -e/-I are incompatible with -N. Please specify only one option.\n");
		print_short_info_err(options);
	}

	if(N_flag==1 && options->mode_raw==RAW) {
		fprintf(stdout,"Error: raw sockets need a specific interface to be specified. You cannot use -N with -r.\n");
		print_short_info_err(options);
	}

	// Print a warning in case a MAC address was specified with -M in UDP non raw mode, as the MAC is obtained through ARP and this argument will be ignored
	if(options->protocol==UDP && options->mode_raw==NON_RAW && M_flag==1) {
		fprintf(stderr,"Warning: a destination MAC address has been specified, but it will be ignored and obtained through ARP.\n");
	}

	if(options->rand_type!=NON_RAND && options->rand_batch_size>options->number) {
		fprintf(stderr,"Error: the random interval batch size cannot be greater than the number of packets (i.e. %" PRIu64 ").\n",options->number);
		print_short_info_err(options);
	}

	if(options->Wfilename==NULL && !options->udp_params.enabled && options->report_extra_data!=0) {
		fprintf(stderr,"Error: the -X option requires -W or -w to be selected too.\n");
		print_short_info_err(options);
	}

	if(options->udp_params.enabled && options->udp_params.port == options->port) {
		fprintf(stderr,"Error: the main socket used by LaMP and the socket for the -w option cannot have the same port.\n");
		fprintf(stderr,"Port for the main LaMP socket (it can be changed with -p): %lu\n",options->port);
		print_short_info_err(options);
	}

	// Get the correct follow-up mode, depending on the current latency type, if F_flag=1 (i.e. if -F was specified)
	if(F_flag==1) {
		switch(options->latencyType) {
			case USERTOUSER:
				options->followup_mode=FOLLOWUP_ON_APP;
			break;

			case KRT:
				options->followup_mode=FOLLOWUP_ON_KRN_RX;
			break;

			case SOFTWARE:
				options->followup_mode=FOLLOWUP_ON_KRN;
			break;

			case HARDWARE:
				options->followup_mode=FOLLOWUP_ON_HW;
			break;

			default:
				fprintf(stderr,"Warning: unknown error. No follow-up mechanism will be activated.\n");
				options->followup_mode=FOLLOWUP_OFF;
			break;
		}
	}

	return 0;
}

void options_free(struct options *options) {
	if(options->filename) {
		free(options->filename);
	}

	if(options->opt_devname) {
		free(options->opt_devname);
	}

	if(options->Wfilename) {
		free(options->Wfilename);
	}

	if(options->udp_params.enabled==1 && options->udp_params.devname) {
		free(options->udp_params.devname);
	}

	#if AMQP_1_0_ENABLED
	if(options->queueNameTx) {
		free(options->queueNameTx);
	}

	if(options->queueNameRx) {
		free(options->queueNameRx);
	}

	if(options->protocol==AMQP_1_0 && options->dest_addr_u.destAddrStr) {
		free(options->dest_addr_u.destAddrStr);
	}
	#endif
}

void options_set_destIPaddr(struct options *options, struct in_addr destIPaddr) {
	options->dest_addr_u.destIPaddr=destIPaddr;
}

const char * latencyTypePrinter(latencytypes_t latencyType) {
	// enum can be used as index array, provided that the order inside the definition of latencytypes_t is the same as the one inside latencyTypes[]
	return latencyTypes[latencyType];
}