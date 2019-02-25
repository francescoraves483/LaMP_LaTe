#ifndef LATENCYTEST_REPORTMAN_H_INCLUDED
#define LATENCYTEST_REPORTMAN_H_INCLUDED

/* As dprintf(), which is used inside this module, has been standardized in POSIX.1-2008, 
   these POSIX functionalities have to be made available, thus the need of defining this feature test macro */
#define _POSIX_C_SOURCE 200809L 

#include <stdint.h>
#include <stdio.h>
#include "options.h"

// Taking into account 20 characters to represent each 64 bit number + 20 characters and 5 decimal digits for each double (forced inside sprintf) + 1 character for layency type=20*4+25*1+1=106 + some margin = 110
#define REPORT_BUFF_SIZE 110

// Macro to write the report into a string
#define repprintf(str1,rep1)	sprintf(str1,"%" PRIu64 "-%.5lf-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%d", \
									rep1.minLatency,rep1.averageLatency,rep1.maxLatency,rep1.packetCount, \
									rep1.outOfOrderCount,(int) (rep1.latencyType));

// Macro to read from a report stored in a string
#define repscanf(str1,rep1ptr)		sscanf(str1,"%" SCNu64 "-%lf-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%d", \
									rep1ptr.minLatency,rep1ptr.averageLatency,rep1ptr.maxLatency,rep1ptr.packetCount, \
									rep1ptr.outOfOrderCount,(int *) (rep1ptr.latencyType));

typedef struct reportStructure {
	uint64_t minLatency;		// us
	double averageLatency;		// us
	uint64_t maxLatency;		// us

	uint64_t packetCount;		// #
	uint64_t outOfOrderCount;	// #
	uint64_t totalPackets;		// #

	latencytypes_t latencyType; // enum (defined in options.h)

	// Internal members
	// Don't touch these variables, as they are managed internally by reportStructureUpdate()
	uint64_t _avgAccumulator;		// # - not transmitted
	uint16_t _lastSeqNumber;		// # - not transmitted
	uint8_t _isFirstUpdate; 		// [0,1] - not transmitted
} reportStructure;

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType);
void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber);
void printStats(reportStructure *report, FILE *stream);
int printStatsCSV(struct options *opts, reportStructure *report, const char *filename);

#endif