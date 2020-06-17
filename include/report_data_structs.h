#ifndef LATENCYTEST_REPORTDATASTRUCTS_H_INCLUDED
#define LATENCYTEST_REPORTDATASTRUCTS_H_INCLUDED

// options.h already includes <netinet/in.h>, needed for "struct sockaddr_in"
#include "options.h"

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

typedef struct carbonReportStructure {
	uint64_t minLatency;		// us
	double averageLatency;		// us
	uint64_t maxLatency;		// us
	double variance;			// us
	uint64_t packetCount;		// #
	uint64_t errorsCount;		// #
	double _welfordM2;					// us - not transmitted/not printed (used for the variance computation)
	double _welfordAverageLatencyOld;	// us - not transmitted/not printed (used for the variance computation)

	int socketDescriptor;		// written only once when opening the socket to Carbon/Graphite with openCarbonReportSocket() in carbon_report_manager.h/.c
} carbonReportStructure;

#endif // LATENCYTEST_REPORTDATASTRUCTS_H_INCLUDED