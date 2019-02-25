#include "report_manager.h"
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h> 
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

static inline struct tm *getLocalTime(void) {
	time_t currtime = time(NULL);

	return localtime(&currtime);
}

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType) {
	report->averageLatency=-1.0;
	report->minLatency=UINT64_MAX;
	report->maxLatency=0;

	report->latencyType=latencyType;

	report->outOfOrderCount=0;
	report->packetCount=0;
	report->totalPackets=totalPackets;

	// Internal members
	report->_avgAccumulator=0;
	report->_isFirstUpdate=1;

	// Initialize also the lastSeqNumber, just to be safe against bugs in the following code, which should set, in any case, _lastSeqNumber
	report->_lastSeqNumber=0;
}

void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber) {
	// Set _lastSeqNumber on first update
	if(report->_isFirstUpdate==1) {
		report->_lastSeqNumber = seqNumber==0 ? UINT16_MAX : seqNumber-1;
		report->_isFirstUpdate=0;
	}

	report->packetCount++;
	report->_avgAccumulator+=tripTime;

	report->averageLatency=report->_avgAccumulator/report->packetCount;

	if(tripTime<report->minLatency) {
		report->minLatency=tripTime;
	}

	if(tripTime>report->maxLatency) {
		report->maxLatency=tripTime;
	}

	// An out of order packet is detected if any decreasing sequence number trend is detected in the sequence of packets.
	// The out of order count is related here to the number of times a decreasing sequence number is detected.
	// When the last sequence number is 65535 (UINT16_MAX), due to cyclic numbers, the expected current one is 0
	// This should not be detected as an error, as it is the only situation in which a decreasing sequence number is expected

	// [TODO] Discuss about this: in this case, only the "decreasing" trend from 65535 to 0 is detected as correct, otherwise it would
	//        be impossible to distinguish between wrong decreasing trend and "correct (cyclic)" decreasing trend
	if(report->_lastSeqNumber==UINT16_MAX ? seqNumber!=0 : seqNumber<=report->_lastSeqNumber) {
		report->outOfOrderCount++;
	}

	// Set last sequence number
	report->_lastSeqNumber=seqNumber;
}

void printStats(reportStructure *report, FILE *stream) {
	int latencyTypeIdx;

	if(report->averageLatency==-1) {
		// No packets have been received
		fprintf(stream,"No packets received: \n"
			"(-) Minimum: - ms - Maximum: - ms - Average: - ms\n"
			"Lost packets: 100%% [%" PRIu64 "/%" PRIu64 "]\n"
			"Out of order count (approx. as the number of times a decreasing seq. number is detected): -\n\n"
			"Please make sure that the server has been correctly launched!\n",
			report->totalPackets, 
			report->totalPackets);
	} else {
		if(report->packetCount>report->totalPackets) {
			fprintf(stream,"There's something wrong: the client received more packets than the total number set with -n.\n");
			fprintf(stream,"A negative percentage packet loss may occur.\n");
		}

		fprintf(stream,"Latency over %" PRIu64 " packets:\n"
			"(%s) Minimum: %.3f ms - Maximum: %.3f ms - Average: %.3f ms\n",
			report->totalPackets,
			latencyTypePrinter(report->latencyType),
			report->minLatency==UINT64_MAX ? 0 : ((double) report->minLatency)/1000, 
			((double) report->maxLatency)/1000,
			report->averageLatency/1000);

		// Negative percentages (should never enter here)
		if(report->packetCount>report->totalPackets) {
			fprintf(stream,"Lost packets: -%.2f%% [-%" PRIi64 "/%" PRIi64 "]\n",
				((double)(report->packetCount-report->totalPackets))*100/(report->totalPackets),
				report->packetCount-report->totalPackets,
				report->totalPackets);
		} else {
			// Positive percentages (should always enter here)
			fprintf(stream,"Lost packets: %.2f%% [%" PRIi64 "/%" PRIi64 "]\n",
				((double)(report->totalPackets-report->packetCount))*100/(report->totalPackets),
				report->totalPackets-report->packetCount,
				report->totalPackets);
		}

		fprintf(stream, "Out of order count (approx. as the number of times a decreasing seq. number is detected): %" PRIu64 "\n",
			report->outOfOrderCount);
	}

	if(report->totalPackets>UINT16_MAX) {
		fprintf(stream,"Note: the number of packets is very large. Since it causes the sequence numbers to cyclically reset,\n"
			"\t the out of order count may be inaccurate, in the order of +- <number of resets that occurred>.\n");
	}
}

int printStatsCSV(struct options *opts, reportStructure *report, const char *filename) {
	int csvfp;
	int printOpErrStatus=0;
	int fileAlreadyExists=0;
	struct tm *currdate;
	double lostPktPerc=0;

	if(opts->overwrite) {
		csvfp=open(filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
	} else {
		errno=0;

		csvfp=open(filename, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);

		if(csvfp<0) {
			if(errno==EEXIST) {
				fileAlreadyExists=1;
				csvfp=open(filename, O_WRONLY | O_APPEND);
			} else {
				printOpErrStatus=1;
			}
		}
	}

	if(csvfp && printOpErrStatus==0) {
		// Get current time and day
		currdate=getLocalTime();

		if(opts->overwrite || !fileAlreadyExists) {
			// Recreate CSV first line
			dprintf(csvfp,"Date,Time,ClientMode,SocketType,Protocol,UP,PayloadLen-B,TotReqPackets,Interval-s,LatencyType,MinLatency-ms,MaxLatency-ms,AvgLatency-ms,LostPackets-Perc,OutOfOrderCountDecr\n");
		}

		// Set lostPktPerc depending on the sign of report->totalPackets-report->packetCount (the negative sign should never occur in normal program operations)
		// If no reportStructureUpdate() was ever called (i.e. averageLatency is still -1), set the lost packets percentage to 100%
		if(report->averageLatency!=-1) {
			lostPktPerc=report->totalPackets>=report->packetCount ? ((double) ((report->totalPackets-report->packetCount)))*100/(report->totalPackets) : ((double) ((report->packetCount-report->totalPackets)))*-100/(report->packetCount);
		} else {
			lostPktPerc=100;
		}

		/* Save report data to file, in CSV style */
		// Save date and time
		dprintf(csvfp,"%d-%02d-%02d,%02d:%02d:%02d,",currdate->tm_year+1900,currdate->tm_mon+1,currdate->tm_mday,currdate->tm_hour,currdate->tm_min,currdate->tm_sec);

		// Save current mode (unidirectional or pinglike) and socket type
		dprintf(csvfp,"%s,%s,",opts->mode_ub==UNIDIR ? "Unidirectional" : "Pinglike",opts->mode_raw==RAW ? "Raw" : "Non raw");

		// Save current protocol (even if UDP only is supported as of now, it can be convenient to write a switch-case statement, to allow an easier future extensibility of the code with other protocols)
		switch(opts->protocol) {
			case UDP:		
				dprintf(csvfp,"UDP,");
			break;
			default:
				dprintf(csvfp,"Unknown,");
			break;
		}
		
		// Save report data
		dprintf(csvfp,"%d," 		// macUP
			"%" PRIu16 ","			// payloadLen
			"%" PRIu64 ","			// total number of packets requested
			"%.3f,"					// interval between packets (in s)
			"%s,"					// latency type (-L)
			"%.3f,"					// minLatency
			"%.3f,"					// maxLatency
			"%.3f,"					// avgLatency
			"%.2f,"					// lost packets (perc)
			"%" PRIu64 "\n",		// out-of-order count
			opts->macUP==UINT8_MAX ? 0 : opts->macUP,																				// macUP (UNSET is interpreted as '0', as AC_BE seems to be used when it is not explicitly defined)
			opts->payloadlen,																										// out-of-order count (# of decreasing sequence breaks)
			opts->number,																											// total number of packets requested
			((double) opts->interval)/1000,																							// interval between packets (in s)
			latencyTypePrinter(report->latencyType),																				// latency type (-L)
			report->minLatency==UINT64_MAX ? 0 : ((double) report->minLatency)/1000,												// minLatency
			((double) report->maxLatency)/1000,																						// maxLatency
			report->averageLatency!=-1 ? report->averageLatency/1000 : 0,															// avgLatency
			lostPktPerc,																											// lost packets (perc)
			report->outOfOrderCount);																								// Out-of-order count

		close(csvfp);

		fprintf(stdout,"Report data was saved inside %s\n",opts->filename);
	} else {
		printOpErrStatus=1;
	}

	return printOpErrStatus;
}