#ifndef LATENCYTEST_REPORTDATASTRUCTS_H_INCLUDED
#define LATENCYTEST_REPORTDATASTRUCTS_H_INCLUDED

// options.h already includes <netinet/in.h>, needed for "struct sockaddr_in"
#include "carbon_dup_list.h"
#include "dup_list.h"
#include "options.h"

// Expected negative gap to detect a reset in the cyclical sequence numbers
// Expected positive gap to detect out of order packets after a cyclical sequence number reset
// This parameter is a key parameter for the sequence number reconstruction and for the out of order 
// and packet loss computation
// It is not recommended to change this as reducing it too much or increasing it to much (making it too close
// to 65535), may imply wrong results when computing the metrics under certain network conditions
#define SEQUENCE_NUMBERS_RESET_THRESHOLD 10000

// First number after UINT16_MAX (i.e. 65536 = 2^16, used for sequence number reconstruction after cyclical resets)
#define UINT16_TOP (UINT16_MAX+1)

typedef struct report_sock_data {
	int descriptor_udp;
	int descriptor_tcp;
	struct sockaddr_in addrto;
} report_sock_data_t;

typedef struct reportStructure {
	uint64_t minLatency;		// us
	double averageLatency;		// us
	uint64_t maxLatency;		// us

	uint64_t packetCount;		// #
	uint64_t outOfOrderCount;	// #
	uint64_t totalPackets;		// #
	uint64_t errorsCount;		// #
	uint64_t lossCount;			// #

	double variance;			// us

	int32_t lastMaxSeqNumber;	// # - last (maximum) received sequence number
	uint64_t seqNumberResets;	// # - est. of the number of times the sequence numbers were reset due to being cyclical

	latencytypes_t latencyType; // enum (defined in options.h)
	modefollowup_t followupMode; // enum (defined in options.h)

	// Internal members
	// Don't touch these variables, as they are managed internally by reportStructureUpdate()
	uint8_t _timeoutOccurred;			// [0,1] - transmitted/not printed
	uint8_t _isFirstUpdate; 			// [0,1] - not transmitted/not printed
	double _welfordM2;					// us - not transmitted/not printed
	double _welfordAverageLatencyOld;	// us - not transmitted/not printed
	uint64_t _lastReconstructedSeqNo;	// # - not transmitted/not printed

	// Finalize-only member: they are used to print statistics, but they are not transmitted
	double confidenceIntervalDev[3];  // us - not transmitted (confidence interval deviation from mean value)

	uint64_t dupCount; 			// # - updated only if -D is not specified - transmitted/printed
	uint8_t dupCountEnabled;	// [0,1] - = 0 if the dupCount value shall not be taken into account, = 1 otherwise - transmitted/not printed
	dupStoreList dupCountList;	// Data struct - allocated and updated only if -D is not specified - not transmitted/not printed
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

typedef struct carbonReportStructure {
	uint64_t minLatency;		// us
	double averageLatency;		// us
	uint64_t maxLatency;		// us
	double variance;			// us
	uint64_t packetCount;		// #
	uint64_t errorsCount;		// #
	double _welfordM2;					// us - not transmitted (used for the variance/stdev computation)
	double _welfordAverageLatencyOld;	// us - not transmitted (used for the variance/stdev computation)

	int _maxSeqNumber;				// # - not sent to Graphite (used for the current flush interval packet loss estimation)
	int _precMaxSeqNumber;			// # - not sent to Graphite (used for the current flush interval packet loss estimation)
	uint8_t _detectedSeqNoReset; 	// [0,1] - = 0 if no sequence number cyclical reset has been detected in the current flush interval, = 1 otherwise

	uint64_t outOfOrderCount;	// #
	uint64_t lossCount;			// # - not reset after each data flush (updated over time)

	uint64_t dupCount; 					// # - updated only if -D is not specified
	carbonDupStoreList dupCountList;	// Data struct - allocated and updated only if -D is not specified

	int socketDescriptor;		// written only once when opening the socket to Carbon/Graphite with openCarbonReportSocket() in carbon_report_manager.h/.c
} carbonReportStructure;

#endif // LATENCYTEST_REPORTDATASTRUCTS_H_INCLUDED