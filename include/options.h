#ifndef LATENCYTEST_OPTIONS_H_INCLUDED
#define LATENCYTEST_OPTIONS_H_INCLUDED

#include <stdint.h>
#include <netinet/in.h>
#include "rawsock_lamp.h" // In order to import the definition of protocol_t
#include "math_utils.h"

// Valid options
// Any new option should be handled in the switch-case inside parse_options() and the corresponding char should be added to VALID_OPTS
// If an option accepts an additional argument, it is followed by ':'
#if !AMQP_1_0_ENABLED
#define VALID_OPTS "hust:n:c:df:svlmoyp:reX:A:BC:FM:NP:R:S:UVL:I:W:T:01"
#else 
#define VALID_OPTS "huat:n:c:df:svlmoyp:req:X:A:BC:FM:NP:R:S:UVL:I:W:T:H:01"
#endif

#if !AMQP_1_0_ENABLED
#define SUPPORTED_PROTOCOLS "[-u]"
#else
#define SUPPORTED_PROTOCOLS "[-u, -a]"
#endif

#define INIT_CODE 0xAB

#define REPORT_RETRY_INTERVAL_MS 200
#define REPORT_RETRY_MAX_ATTEMPTS 15
#define INIT_RETRY_INTERVAL_MS 100
#define INIT_RETRY_MAX_ATTEMPTS 30
#define FOLLOWUP_CTRL_RETRY_INTERVAL_MS 100
#define FOLLOWUP_CTRL_RETRY_MAX_ATTEMPTS 30
#define CLIENT_SRCPORT 46772

#define DEFAULT_LATE_PORT 46000
#define MAX_PAYLOAD_SIZE_UDP_LAMP 1448 // Set to 1448 B since: 20 B (IP hdr) + 8 B (UDP hdr) + 24 B (LaMP hdr) + 1448 B (payload) = 1500 B (MTU)
#define RAW_RX_PACKET_BUF_SIZE (ETHERMTU+14) // Ethernet MTU (1500 B) + 14 B of struct ether_header
#define MIN_TIMEOUT_VAL_S 1000 // Minimum timeout value for the server (in ms)
#define MIN_TIMEOUT_VAL_C 3000 // Minimum timeout value for the client (in ms)
#define POLL_ERRQUEUE_WAIT_TIMEOUT 100 // Timeout for pollErrqueueWait() in common_socket_man.h/.c (in ms)

// Default client interval/server timeout values
#define CLIENT_DEF_INTERVAL 100 // [ms]
#define SERVER_DEF_TIMEOUT 4000 // [ms]
#define CLIENT_DEF_TIMEOUT 2000 // [ms]

// Default number of packets
#define CLIENT_DEF_NUMBER 600 // [#]

// Number of decimal digits to be reported in the CSV file when in "-W" mode
#define W_DECIMAL_DIGITS 3 // [#]

// Default confidence interval mask
#define DEF_CONFIDENCE_INTERVAL_MASK 2

// Max port string size (define only when AMQP 1.0 is active)
#define MAX_PORT_STR_SIZE 6 // 5 characters + final '\0'

// nonwlan_mode values (defined here for readability reasons)
#define NONWLAN_MODE_WIRELESS 0
#define NONWLAN_MODE_WIRED 1
#define NONWLAN_MODE_ANY 2
#define NONWLAN_MODE_FORCED_NAME 3

// Default batch size when using a random interval (each batch will have the same interval between packets)
#define BATCH_SIZE_DEF 10

// Char <-> shift mapping for SET_REPORT_EXTRA_DATA_BIT
// To use SET_REPORT_EXTRA_DATA_BIT() you should specify the report_extra_data variable and one of these macros
#define CHAR_P 1
#define CHAR_R 2
#define CHAR_M 3
#define CHAR_N 4

// Utility macros to set and check the report_extra_data field's bit, enabling or disabling the printing of extra information to -W CSV files
#define SET_REPORT_EXTRA_DATA_BIT(report_extra_data,char_macro) (report_extra_data |= 1UL << char_macro)
#define SET_REPORT_DATA_ALL_BITS(report_extra_data) (report_extra_data=0xFF)
#define CHECK_REPORT_EXTRA_DATA_BIT_SET(report_extra_data,char_macro) ((report_extra_data >> char_macro) & 1U)

// Macro to check if report extra_data has a correct value
#define REPORT_IS_REPORT_EXTRA_DATA_OK(enabled_extra_data) (enabled_extra_data=='a' || enabled_extra_data=='p' || enabled_extra_data=='r')

// This value should be set to the number of bits in the type of "report_extra_data"
#define REPORT_EXTRA_DATA_BIT_SIZE 16

// Latency types
typedef enum {
	UNKNOWN,	// Unset latency type
	USERTOUSER,	// Userspace timestamps
	KRT,	  	// Kernel receive timestamps
	SOFTWARE, 	// Kernel receive and transmit timestamps
	HARDWARE 	// Hardware timestamps
} latencytypes_t;

typedef enum {
	UNSET_MCS,
	CLIENT,
	LOOPBACK_CLIENT,
	SERVER,
	LOOPBACK_SERVER
} modecs_t;

typedef enum {
	UNSET_MUB,
	PINGLIKE,
	UNIDIR
} modeub_t;

typedef enum {
	FOLLOWUP_OFF,
	FOLLOWUP_ON_APP,	 // Application level timestamps
	FOLLOWUP_ON_KRN_RX,	 // KRT timestamps
	FOLLOWUP_ON_KRN,	 // Kernel rx and tx timestamps -> reserved for future use (not yet implemented)
	FOLLOWUP_ON_HW		 // Hardware timestamps
} modefollowup_t;

typedef enum {
	NON_RAW,
	RAW
} moderaw_t;

struct options {
	uint8_t init;
	protocol_t protocol;
	modecs_t mode_cs;
	modeub_t mode_ub;
	moderaw_t mode_raw;
	uint64_t interval;
	uint64_t client_timeout;
	uint64_t number;
	uint16_t payloadlen; // uint16_t because the LaMP len field is 16 bits long
	int macUP;
	char *filename; // Filename for the -f mode
	uint8_t overwrite; // In '-f' mode, overwrite will be = 1 if '-o' is specified (overwrite and do not append to existing file), otherwise it will be = 0 (default = 0)
	uint8_t overwrite_W; // In '-W' mode, overwrite will be = 1 if '-o' is specified (overwrite and do not create new files when a CSV file already exists), otherwise it will be = 0 (default = 0)
	char *opt_devname; // Interface name for the -S mode
	uint8_t dmode; // Set with '-d': = 1 if continuous server mode is selected, = 0 otherwise (default = 0)
	char latencyType; // Set with the option '-L': can be 'u' (user-to-user, gettimeofday() - default), 'r' (KRT, gettimeofday()+ancillary data), 'h' (hardware timestamps when supported)
	uint8_t nonwlan_mode; // = 0 if the program should bind to wireless interfaces, = 1 otherwise, = 2 if binding to any local interface (default: 0)
	long if_index; // Interface index to be used (default: 0)
	uint8_t confidenceIntervalMask; // Confidence interval mask: a user shall specify xx1 to print the .90 intervals, x1x for the .95 ones and 1xx for the .99 ones (default .95 only)
	modefollowup_t followup_mode; // = FOLLOWUP_OFF if no follow-up mechanism should be used, = FOLLOWUP_ON_* otherwise (default: 0)
	uint8_t refuseFollowup; // Server only. =1 if the server should deny any follow-up request coming the client, =0 otherwise (default: 0)
	uint8_t verboseFlag; // =1 if verbose mode is on, =0 otherwise (default: 0, i.e. no -V specified)
	char *Wfilename; // Filename for the -W mode
	uint8_t printAfter; // Server only. =0 if the server should print that a packet was received before sending the reply, =1 to print after sending the reply (default: 0)

	union {
		struct in_addr destIPaddr;
		char *destAddrStr;
	} dest_addr_u;

	unsigned long port;
	uint8_t destmacaddr[6];

	#if AMQP_1_0_ENABLED
	char portStr[MAX_PORT_STR_SIZE]; // This is defined and used only when AMQP 1.0 is enabled
	char *queueNameTx;
	char *queueNameRx;
	#endif

	rand_distribution_t rand_type; // Random -t distribution type, set with '-R'. Default: NON_RAND (i.e. no random interval between packets)
	double rand_param; // Its meaning depends on 'rand_type'
	uint64_t rand_batch_size; // Defaults to BATCH_SIZE_DEF when rand_type!=NON_RAND or it is set to 'number' and practically not used when rand_type==NON_RAND
	
	// Report extra data to be printed to -W CSV files only when explicitely requested
	// It is saved as a set of bits (up to 16), which enable a certain extra information to be printed
	// The bits meaning is (where "res"="reserved" and the character is the one toggling that bit/information with -X):
	// (res) (res) (res) (res) (res) (res) (res) (res) (res) (res) (res) ('n') ('m') ('r') ('p')
	// -X a will set all the bits to 1
	// All the reserved bits are ignored, no matter the value they assume
	uint16_t report_extra_data;
};

void options_initialize(struct options *options);
unsigned int parse_options(int argc, char **argv, struct options *options);
void options_free(struct options *options);
void options_set_destIPaddr(struct options *options, struct in_addr destIPaddr);
const char * latencyTypePrinter(latencytypes_t latencyType);

#endif