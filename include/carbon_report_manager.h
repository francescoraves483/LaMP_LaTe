#ifndef LATENCYTEST_CARBONREPORTMAN_H_INCLUDED
#define LATENCYTEST_CARBONREPORTMAN_H_INCLUDED

#include <stdint.h>
#include <stdio.h>
#include "report_data_structs.h"

void carbonReportStructureInit(carbonReportStructure *report,struct options *opts);
// The 'add_one' argument can be used to add '1' to the timestamp in seconds which is sent
// It is used to avoid losing data when the last metrics are flushed
int carbonReportStructureFlush(carbonReportStructure *report,struct options *opts,int decimal_digits,uint8_t add_one);
void carbonReportStructureUpdate(carbonReportStructure *report,uint64_t tripTime,int32_t seqNo,uint8_t dup_detect_enabled);
void carbonReportStructureFree(carbonReportStructure *report,struct options *opts);

int openCarbonReportSocket(carbonReportStructure *report,struct options *opts);
void closeCarbonReportSocket(carbonReportStructure *report);


#endif