#ifndef LATENCYTEST_REPORTMAN_H_INCLUDED
#define LATENCYTEST_REPORTMAN_H_INCLUDED

/* As dprintf(), which is used inside this module, has been standardized in POSIX.1-2008, 
   these POSIX functionalities have to be made available, thus the need of defining this feature test macro */
#define _POSIX_C_SOURCE 200809L 

#include <stdint.h>
#include <stdio.h>
#include "options.h"

typedef struct udp_sock_data {
	int descriptor;
	struct sockaddr_in addrto;
} udp_sock_data_t;

// Taking into account 20 characters to represent each 64 bit number + 5 characters to represent a 16 bit nummber + 3 characters to represent a 8 bit nummber + 20 characters and 5 decimal digits for each double (forced inside sprintf) + 1 character for layency type + 10 '-' chacaters=20*6+6+25*2+1+10=158 + some margin = 201
#define REPORT_BUFF_SIZE 201

#define CONFINT_NUMBER 3

// Maximum file number to be appended after a filename specified with '-W'
// After this maximum value is reached, the program will append on the file initially specified with '-W'
#define W_MAX_FILE_NUMBER 9999
#define W_MAX_FILE_NUMBER_DIGITS 4

// Header line when for CSV files containing per-packet data, both when follow-up is enabled and when it is disabled
#define PERPACKET_COMMON_FILE_HEADER_NO_FOLLOWUP "Sequence Number,RTT/Latency,Tx_Timestamp_s_us,Error"
#define PERPACKET_COMMON_FILE_HEADER_FOLLOWUP "Sequence Number,RTT/Latency,Est server processing time,Tx_Timestamp_s_us,Error"

// Expected negative gap to detect a reset in the cyclical sequence numbers
#define SEQUENCE_NUMBERS_RESET_THRESHOLD 10000

// Macro to write the report into a string
#define repprintf(str1,rep1)	sprintf(str1,"%" PRIu64 "-%.5lf-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%d-%.5lf-%" PRIu16 "-%" PRIu64 "-%" PRIu8, \
									rep1.minLatency,rep1.averageLatency,rep1.maxLatency,rep1.packetCount, \
									rep1.outOfOrderCount,rep1.errorsCount,(int) (rep1.latencyType),rep1.variance,rep1.lastSeqNumber, \
									rep1.seqNumberResets,rep1._timeoutOccurred);

// Macro to read from a report stored in a string
#define repscanf(str1,rep1ptr)		sscanf(str1,"%" SCNu64 "-%lf-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%d-%lf-%" SCNu16 "-%" SCNu64 "-%" SCNu8, \
									rep1ptr.minLatency,rep1ptr.averageLatency,rep1ptr.maxLatency,rep1ptr.packetCount, \
									rep1ptr.outOfOrderCount,rep1ptr.errorsCount,(int *) (rep1ptr.latencyType),rep1ptr.variance,rep1ptr.lastSeqNumber, \
									rep1ptr.seqNumberResets,rep1ptr._timeoutOccurred);

typedef struct reportStructure {
	uint64_t minLatency;		// us
	double averageLatency;		// us
	uint64_t maxLatency;		// us

	uint64_t packetCount;		// #
	uint64_t outOfOrderCount;	// #
	uint64_t totalPackets;		// #
	uint64_t errorsCount;		// #

	double variance;			// us

	uint16_t lastSeqNumber;		// # - last received sequence number
	uint64_t seqNumberResets;	// # - est. of the number of times the sequence numbers were reset due to being cyclical

	latencytypes_t latencyType; // enum (defined in options.h)
	modefollowup_t followupMode; // enum (defined in options.h)

	// Internal members
	// Don't touch these variables, as they are managed internally by reportStructureUpdate()
	uint8_t _timeoutOccurred;			// [0,1] - transmitted/not printed
	uint8_t _isFirstUpdate; 			// [0,1] - not transmitted/not printed
	double _welfordM2;					// us - not transmitted/not printed
	double _welfordAverageLatencyOld;	// us - not transmitted/not printed

	// Finalize-only member: they are used to print statistics, but they are not transmitted
	double confidenceIntervalDev[3];  // us - not transmitted (confidence interval deviation from mean value)
} reportStructure;

// Structure containing the per-packet data which can be written to a CSV file for each packet
typedef struct perPackerDataStructure {
	int followup_on_flag;
	uint64_t seqNo;
	int64_t signedTripTime;
	uint64_t tripTimeProc;
	struct timeval tx_timestamp;
	uint16_t enabled_extra_data; // See the "uint16_t report_extra_data" field in "struct options" (options.h) for a more detailed description of this field
	reportStructure *reportDataPointer;
} perPackerDataStructure;

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType, modefollowup_t followupMode);
void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber);
void reportSetTimeoutOccurred(reportStructure *report);
void reportStructureFinalize(reportStructure *report);
void printStats(reportStructure *report, FILE *stream, uint8_t confidenceIntervalsMask);
int printStatsCSV(struct options *opts, reportStructure *report, const char *filename);
int openTfile(const char *Tfilename, uint8_t overwrite, int followup_on_flag, char enabled_extra_data);
int openUDPSocket(udp_sock_data_t *sock_data,struct options *opts);
int writeToTFile(int Tfiledescriptor,int decimal_digits,perPackerDataStructure *perPktData);
int writeToUDPSocket(udp_sock_data_t *sock_data,int decimal_digits,perPackerDataStructure *perPktData);
void closeTfile(int Tfilepointer);
void closeUDPSocket(udp_sock_data_t *sock_data);

#endif