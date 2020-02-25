#ifndef LATENCYTEST_REPORTMAN_H_INCLUDED
#define LATENCYTEST_REPORTMAN_H_INCLUDED

/* As dprintf(), which is used inside this module, has been standardized in POSIX.1-2008, 
   these POSIX functionalities have to be made available, thus the need of defining this feature test macro */
#define _POSIX_C_SOURCE 200809L 

#include <stdint.h>
#include <stdio.h>
#include "options.h"

// Taking into account 20 characters to represent each 64 bit number + 20 characters and 5 decimal digits for each double (forced inside sprintf) + 1 character for layency type + 7 '-' chacaters=20*5+25*2+1+7=158 + some margin = 170
#define REPORT_BUFF_SIZE 170

#define CONFINT_NUMBER 3

// Maximum file number to be appended after a filename specified with '-W'
// After this maximum value is reached, the program will append on the file initially specified with '-W'
#define W_MAX_FILE_NUMBER 9999
#define W_MAX_FILE_NUMBER_DIGITS 4

// Macro to write the report into a string
#define repprintf(str1,rep1)	sprintf(str1,"%" PRIu64 "-%.5lf-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%d-%.5lf", \
									rep1.minLatency,rep1.averageLatency,rep1.maxLatency,rep1.packetCount, \
									rep1.outOfOrderCount,rep1.errorsCount,(int) (rep1.latencyType),rep1.variance);

// Macro to read from a report stored in a string
#define repscanf(str1,rep1ptr)		sscanf(str1,"%" SCNu64 "-%lf-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%d-%lf", \
									rep1ptr.minLatency,rep1ptr.averageLatency,rep1ptr.maxLatency,rep1ptr.packetCount, \
									rep1ptr.outOfOrderCount,rep1ptr.errorsCount,(int *) (rep1ptr.latencyType),rep1ptr.variance);

typedef struct reportStructure {
	uint64_t minLatency;		// us
	double averageLatency;		// us
	uint64_t maxLatency;		// us

	uint64_t packetCount;		// #
	uint64_t outOfOrderCount;	// #
	uint64_t totalPackets;		// #
	uint64_t errorsCount;		// #

	double variance;			// us

	latencytypes_t latencyType; // enum (defined in options.h)
	modefollowup_t followupMode; // enum (defined in options.h)

	// Internal members
	// Don't touch these variables, as they are managed internally by reportStructureUpdate()
	uint16_t _lastSeqNumber;			// # - not transmitted/not printed
	uint8_t _isFirstUpdate; 			// [0,1] - not transmitted/not printed
	double _welfordM2;					// us - not transmitted/not printed
	double _welfordAverageLatencyOld;	// us - not transmitted/not printed

	// Finalize-only member: they are used to print statistics, but they are not transmitted
	double confidenceIntervalDev[3];  // us - not transmitted (confidence interval deviation from mean value)
} reportStructure;

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType, modefollowup_t followupMode);
void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber);
void reportStructureFinalize(reportStructure *report);
void printStats(reportStructure *report, FILE *stream, uint8_t confidenceIntervalsMask);
int printStatsCSV(struct options *opts, reportStructure *report, const char *filename);
int openTfile(const char *Tfilename, int followup_on_flag);
int writeToTFile(int Tfiledescriptor,int followup_on_flag,int decimal_digits,uint64_t seqNo,int64_t signedTripTime,uint64_t tripTimeProc);
void closeTfile(int Tfilepointer);

#endif