#ifndef LATENCYTEST_REPORTMAN_H_INCLUDED
#define LATENCYTEST_REPORTMAN_H_INCLUDED

/* As dprintf(), which is used inside this module, has been standardized in POSIX.1-2008, 
   these POSIX functionalities have to be made available, thus the need of defining this feature test macro */
#define _POSIX_C_SOURCE 200809L 

#include <stdint.h>
#include <stdio.h>
#include "options.h"
#include "report_data_structs.h"

// Taking into account 20 characters to represent each 64 bit number + 10 characters to represent a 32 bit number + 3 characters to represent a 8 bit nummber + 20 characters and 5 decimal digits for each double (forced inside sprintf) + 1 character for layency type + 12 '-' chacaters=20*7+10+3*2+25*2+1+12=219 + some margin = 225
#define REPORT_BUFF_SIZE 225

#define CONFINT_NUMBER 3

// Maximum file number to be appended after a filename specified with '-W'
// After this maximum value is reached, the program will append on the file initially specified with '-W'
#define W_MAX_FILE_NUMBER 9999
#define W_MAX_FILE_NUMBER_DIGITS 4

// Header line when for CSV files containing per-packet data, both when follow-up is enabled and when it is disabled
#define PERPACKET_COMMON_FILE_HEADER_NO_FOLLOWUP "Sequence Number,RTT/Latency,Tx_Timestamp_s_us,Error"
#define PERPACKET_COMMON_FILE_HEADER_FOLLOWUP "Sequence Number,RTT/Latency,Est server processing time,Tx_Timestamp_s_us,Error"

// Same as before, but to be sent over the -w TCP socket to tell the receiving application which fields are to be expected for the current test
// According to the chosen socket data format, they are separated by ';'
#define PERPACKET_COMMON_SOCK_HEADER_NO_FOLLOWUP "seq;latency;tx_timestamp;error"
#define PERPACKET_COMMON_SOCK_HEADER_FOLLOWUP "seq;latency;est_proctime;tx_timestamp;error"

// Macro to write the report into a string
#define repprintf(str1,rep1)	sprintf(str1,"%" PRIu64 "-%.5lf-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%" PRIu64 "-%d-%.5lf-%" PRIi32 "-%" PRIu64 "-%" PRIu8 "-%" PRIu8 "-%" PRIu64, \
									rep1.minLatency,rep1.averageLatency,rep1.maxLatency,rep1.packetCount, \
									rep1.outOfOrderCount,rep1.errorsCount,(int) (rep1.latencyType),rep1.variance,rep1.lastMaxSeqNumber, \
									rep1.seqNumberResets,rep1._timeoutOccurred,\
									rep1.dupCountEnabled,rep1.dupCount);

// Macro to read from a report stored in a string
#define repscanf(str1,rep1ptr)		sscanf(str1,"%" SCNu64 "-%lf-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%" SCNu64 "-%d-%lf-%" SCNi32 "-%" SCNu64 "-%" SCNu8 "-%" SCNu8 "-%" SCNu64, \
									rep1ptr.minLatency,rep1ptr.averageLatency,rep1ptr.maxLatency,rep1ptr.packetCount, \
									rep1ptr.outOfOrderCount,rep1ptr.errorsCount,(int *) (rep1ptr.latencyType),rep1ptr.variance,rep1ptr.lastMaxSeqNumber, \
									rep1ptr.seqNumberResets,rep1ptr._timeoutOccurred,\
									rep1ptr.dupCountEnabled,rep1ptr.dupCount);

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType, modefollowup_t followupMode, uint8_t dup_detect_enabled);
void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber);
void reportSetTimeoutOccurred(reportStructure *report);
void reportStructureFinalize(reportStructure *report);
void reportStructureFree(reportStructure *report);
void reportStructureChangeTotalPackets(reportStructure *report, uint64_t totalPackets);
void printStats(reportStructure *report, FILE *stream, uint8_t confidenceIntervalsMask);
int printStatsCSV(struct options *opts, reportStructure *report, const char *filename);
int printStatsSocket(struct options *opts, reportStructure *report, report_sock_data_t *sock_data,uint16_t test_id);
int openTfile(const char *Tfilename, uint8_t overwrite, int followup_on_flag, char enabled_extra_data);
int openReportSocket(report_sock_data_t *sock_data,struct options *opts);
int writeToTFile(int Tfiledescriptor,int decimal_digits,perPackerDataStructure *perPktData);
int writeToReportSocket(report_sock_data_t *sock_data,int decimal_digits,perPackerDataStructure *perPktData,uint16_t test_id,uint8_t *first_call);
void closeTfile(int Tfilepointer);
void closeReportSocket(report_sock_data_t *sock_data);

#endif