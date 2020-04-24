#include "report_manager.h"
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h> 
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>

// Condidence interval array sizes
#define TSTUDSIZE90 125
#define TSTUDSIZE95 156
#define TSTUDSIZE99 229

// Condidence interval thresholds array sizes
#define TSTUDSIZE90THRS 13
#define TSTUDSIZE95THRS 16
#define TSTUDSIZE99THRS 22

#define TSTUDTHRS_INCR 0.001

// First number after UINT16_MAX (i.e. 65536 = 2^16, used for sequence number reconstruction after cyclical resets)
#define UINT16_TOP (UINT16_MAX+1)

// Static function to compute ts, to be used to compute the confidence intervals
/* 	It tries to emulate functions such as MATLAB's tinv(), but without inverting the Student's T cumulative distribution,
	guaranteeing a precision of 3 decimal digits over the returned values.
	In order to keep everything as fast and compact as possible, the first n DOF values,
	for each of the supported confidence intervals (p=.90/.95/.99, which should be the most common ones) are hard-coded.
	n (i.e. TSTUDSIZE_xx_) is computed as the first DOF value in which the maximum difference between the current inverse of 
	Student's T cdf and the previous one (i.e. the difference between two consecutive array values) is less than 1e-4.
	Then, all the consecutive DOF values, in which the Student's T distribution is converging to the Normal distribution,
	are approximated. This is done considering an ideal plot, for each p value, in which on the x axis the DOF are shown 
	(from n to the value in which the inverse of Student's T cdf corresponds to the inverne of Normal cdf - considering 3 
	decimal digits) and on the y axis the Student's T cdf inverse value (ts), for that probability and DOF value, is displayed. 
	Then, the x axis of each plot is partioned into segments, in which the ts value is the same, when approximated to 3 decimal 
	digits. 
	The boundaries of these segments are then stored inside the tstudvals_xx_thrs arrays.
	When DOF < n (i.e. arraysize), the hard-coded value is returned, when DOF >= n, the value of ts inside the corresponding interval is computed,
	as the last hardcoded value (rounded to 3 decimal digits) - 0.001 * the position of the interval, taking into account that
	moving to the nearest segment on the right causes the ts value to be decreased by 0.001 (i.e. by TSTUDTHRS_INCR).
*/
static double tsCalculator(int dof, int intervalIndex) {
	// Confidence intervals arrays
	float tstudvals90[TSTUDSIZE90]={6.3138,2.92,2.3534,2.1318,2.015,1.9432,1.8946,1.8595,1.8331,1.8125,1.7959,1.7823,1.7709,1.7613,1.7531,1.7459,1.7396,1.7341,1.7291,1.7247,1.7207,1.7171,1.7139,1.7109,1.7081,1.7056,1.7033,1.7011,1.6991,1.6973,1.6955,1.6939,1.6924,1.6909,1.6896,1.6883,1.6871,1.686,1.6849,1.6839,1.6829,1.682,1.6811,1.6802,1.6794,1.6787,1.6779,1.6772,1.6766,1.6759,1.6753,1.6747,1.6741,1.6736,1.673,1.6725,1.672,1.6716,1.6711,1.6706,1.6702,1.6698,1.6694,1.669,1.6686,1.6683,1.6679,1.6676,1.6672,1.6669,1.6666,1.6663,1.666,1.6657,1.6654,1.6652,1.6649,1.6646,1.6644,1.6641,1.6639,1.6636,1.6634,1.6632,1.663,1.6628,1.6626,1.6624,1.6622,1.662,1.6618,1.6616,1.6614,1.6612,1.6611,1.6609,1.6607,1.6606,1.6604,1.6602,1.6601,1.6599,1.6598,1.6596,1.6595,1.6594,1.6592,1.6591,1.659,1.6588,1.6587,1.6586,1.6585,1.6583,1.6582,1.6581,1.658,1.6579,1.6578,1.6577,1.6575,1.6574,1.6573,1.6572,1.6571};
	float tstudvals95[TSTUDSIZE95]={12.7062,4.3027,3.1824,2.7764,2.5706,2.4469,2.3646,2.306,2.2622,2.2281,2.201,2.1788,2.1604,2.1448,2.1314,2.1199,2.1098,2.1009,2.093,2.086,2.0796,2.0739,2.0687,2.0639,2.0595,2.0555,2.0518,2.0484,2.0452,2.0423,2.0395,2.0369,2.0345,2.0322,2.0301,2.0281,2.0262,2.0244,2.0227,2.0211,2.0195,2.0181,2.0167,2.0154,2.0141,2.0129,2.0117,2.0106,2.0096,2.0086,2.0076,2.0066,2.0057,2.0049,2.004,2.0032,2.0025,2.0017,2.001,2.0003,1.9996,1.999,1.9983,1.9977,1.9971,1.9966,1.996,1.9955,1.9949,1.9944,1.9939,1.9935,1.993,1.9925,1.9921,1.9917,1.9913,1.9908,1.9905,1.9901,1.9897,1.9893,1.989,1.9886,1.9883,1.9879,1.9876,1.9873,1.987,1.9867,1.9864,1.9861,1.9858,1.9855,1.9853,1.985,1.9847,1.9845,1.9842,1.984,1.9837,1.9835,1.9833,1.983,1.9828,1.9826,1.9824,1.9822,1.982,1.9818,1.9816,1.9814,1.9812,1.981,1.9808,1.9806,1.9804,1.9803,1.9801,1.9799,1.9798,1.9796,1.9794,1.9793,1.9791,1.979,1.9788,1.9787,1.9785,1.9784,1.9782,1.9781,1.978,1.9778,1.9777,1.9776,1.9774,1.9773,1.9772,1.9771,1.9769,1.9768,1.9767,1.9766,1.9765,1.9763,1.9762,1.9761,1.976,1.9759,1.9758,1.9757,1.9756,1.9755,1.9754,1.9753};
	float tstudvals99[TSTUDSIZE99]={63.6567,9.9248,5.8409,4.6041,4.0321,3.7074,3.4995,3.3554,3.2498,3.1693,3.1058,3.0545,3.0123,2.9768,2.9467,2.9208,2.8982,2.8784,2.8609,2.8453,2.8314,2.8188,2.8073,2.7969,2.7874,2.7787,2.7707,2.7633,2.7564,2.75,2.744,2.7385,2.7333,2.7284,2.7238,2.7195,2.7154,2.7116,2.7079,2.7045,2.7012,2.6981,2.6951,2.6923,2.6896,2.687,2.6846,2.6822,2.68,2.6778,2.6757,2.6737,2.6718,2.67,2.6682,2.6665,2.6649,2.6633,2.6618,2.6603,2.6589,2.6575,2.6561,2.6549,2.6536,2.6524,2.6512,2.6501,2.649,2.6479,2.6469,2.6459,2.6449,2.6439,2.643,2.6421,2.6412,2.6403,2.6395,2.6387,2.6379,2.6371,2.6364,2.6356,2.6349,2.6342,2.6335,2.6329,2.6322,2.6316,2.6309,2.6303,2.6297,2.6291,2.6286,2.628,2.6275,2.6269,2.6264,2.6259,2.6254,2.6249,2.6244,2.6239,2.6235,2.623,2.6226,2.6221,2.6217,2.6213,2.6208,2.6204,2.62,2.6196,2.6193,2.6189,2.6185,2.6181,2.6178,2.6174,2.6171,2.6167,2.6164,2.6161,2.6157,2.6154,2.6151,2.6148,2.6145,2.6142,2.6139,2.6136,2.6133,2.613,2.6127,2.6125,2.6122,2.6119,2.6117,2.6114,2.6111,2.6109,2.6106,2.6104,2.6102,2.6099,2.6097,2.6095,2.6092,2.609,2.6088,2.6086,2.6083,2.6081,2.6079,2.6077,2.6075,2.6073,2.6071,2.6069,2.6067,2.6065,2.6063,2.6061,2.606,2.6058,2.6056,2.6054,2.6052,2.6051,2.6049,2.6047,2.6045,2.6044,2.6042,2.6041,2.6039,2.6037,2.6036,2.6034,2.6033,2.6031,2.603,2.6028,2.6027,2.6025,2.6024,2.6022,2.6021,2.602,2.6018,2.6017,2.6015,2.6014,2.6013,2.6011,2.601,2.6009,2.6008,2.6006,2.6005,2.6004,2.6003,2.6001,2.6,2.5999,2.5998,2.5997,2.5996,2.5994,2.5993,2.5992,2.5991,2.599,2.5989,2.5988,2.5987,2.5986,2.5985,2.5984,2.5983,2.5982,2.5981,2.598,2.5979,2.5978,2.5977,2.5976,2.5975};
	
	// Confidence intervals threshold arrays
	int tstudvals90thrs[TSTUDSIZE90THRS]={125,131,144,158,177,200,230,270,328,418,576,926,2358};
	int tstudvals95thrs[TSTUDSIZE95THRS]={156,164,176,190,206,226,249,279,315,364,429,524,672,936,1545,4426};
	int tstudvals99thrs[TSTUDSIZE99THRS]={229,239,251,265,280,296,315,336,361,389,423,462,510,568,642,738,868,1054,1341,1842,2944,7332};

	float ts=-1.0;
	float baseval;

	float *array;
	int *arraythrs;
	int arraysize, arraythrssize;

	if(intervalIndex==0) {
		array=tstudvals90;
		arraythrs=tstudvals90thrs;
		arraysize=TSTUDSIZE90;
		arraythrssize=TSTUDSIZE90THRS;
	} else if(intervalIndex==1) {
		array=tstudvals95;
		arraythrs=tstudvals95thrs;
		arraysize=TSTUDSIZE95;
		arraythrssize=TSTUDSIZE95THRS;
	} else if(intervalIndex==2) {
		array=tstudvals99;
		arraythrs=tstudvals99thrs;
		arraysize=TSTUDSIZE99;
		arraythrssize=TSTUDSIZE99THRS;
	} else {
		return -1.0;
	}

	if(dof<=0) { // Safety check
		ts=0.0;
	} else if(dof>=arraysize) {
		baseval=truncf(array[arraysize-1]*1000)/1000;
		for(int i=0;i<arraythrssize && ts==-1;i++) {
			if(i==arraythrssize-1) {
				ts=baseval-TSTUDTHRS_INCR*i;
			} else if(dof>arraythrs[i] && dof<=arraythrs[i+1]) {
				ts=baseval-TSTUDTHRS_INCR*i;
			}
		}
	} else {
		ts=array[dof-1];
	}

	return ts;
}

static inline struct tm *getLocalTime(void) {
	time_t currtime = time(NULL);

	return localtime(&currtime);
}

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType, modefollowup_t followupMode) {
	report->averageLatency=0.0;
	report->minLatency=UINT64_MAX;
	report->maxLatency=0;

	report->latencyType=latencyType;
	report->followupMode=followupMode;

	report->outOfOrderCount=0;
	report->packetCount=0;
	report->totalPackets=totalPackets;
	report->errorsCount=0;

	report->variance=0;

	// Internal members
	report->_isFirstUpdate=1;

	// Initialize also the lastSeqNumber, just to be safe against bugs in the following code, which should set, in any case, lastSeqNumber
	report->lastSeqNumber=0;

	report->seqNumberResets=0;

	report->_timeoutOccurred=0;

	report->_welfordM2=0;

	for(int i=0;i<CONFINT_NUMBER;i++) {
		report->confidenceIntervalDev[i]=-1.0;
	}
}

void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber) {
	report->packetCount++;

	if(tripTime!=0) {
		// Set lastSeqNumber on first update
		if(report->_isFirstUpdate==1) {
			report->lastSeqNumber = seqNumber==0 ? UINT16_MAX : seqNumber-1;
			report->_isFirstUpdate=0;
		} else {
			// Try to infer if a sequence number reset occurred
			// This operation is placed here as it is applicable only when this is not the first report update
			if((int32_t)seqNumber-(int32_t)report->lastSeqNumber<-SEQUENCE_NUMBERS_RESET_THRESHOLD) {
				report->seqNumberResets++;
			}
		}

		report->_welfordAverageLatencyOld=report->averageLatency;
		report->averageLatency+=(tripTime-report->averageLatency)/report->packetCount;

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
		if(report->lastSeqNumber==UINT16_MAX ? seqNumber!=0 : seqNumber<=report->lastSeqNumber) {
			report->outOfOrderCount++;
		}

		// Set last sequence number
		report->lastSeqNumber=seqNumber;

		// Compute the current variance (std dev squared) value using Welford's online algorithm
		report->_welfordM2=report->_welfordM2+(tripTime-report->_welfordAverageLatencyOld)*(tripTime-report->averageLatency);
		if(report->packetCount>1) {
			report->variance=report->_welfordM2/(report->packetCount-1);
		}
	} else {
		// If tripTime is zero, a timestamping error occurred: count the current packet as a packet containing an error
		// This packet will be counter as received, but it will not be used to compute the final statistics
		report->errorsCount++;
	}
}

void reportSetTimeoutOccurred(reportStructure *report) {
	report->_timeoutOccurred=1;
}

void reportStructureFinalize(reportStructure *report) {
	double stderr;

	// Standard error - in us
	stderr=sqrt(report->variance/report->packetCount);

	// Compute confidence intervals using Student's T distribution
	for(int i=0;i<CONFINT_NUMBER;i++) {
		report->confidenceIntervalDev[i]=tsCalculator(report->packetCount-1,i)*stderr;
	}
}

void printStats(reportStructure *report, FILE *stream, uint8_t confidenceIntervalsMask) {
	int i;
	const char *confidenceIntervalLabels[]={".90",".95",".99"};

	if(report->minLatency==UINT64_MAX) {
		// No packets have been received (or they all caused timestamping errors)
		fprintf(stream,"Latency over 0 packets: \n"
			"(-) Minimum: - ms - Maximum: - ms - Average: - ms\n"
			"Standard Dev.: - ms\n"
			"Lost packets: 100%% [%" PRIu64 "/%" PRIu64 "]\n"
			"Errors count: %" PRIu64 "\n"
			"Out of order count (approx. as the number of times a decreasing seq. number is detected): -\n\n"
			"No packets with useful data for computing statistics were received.\n"
			"Please make sure that the server has been correctly launched!\n",
			report->totalPackets, 
			report->totalPackets,
			report->errorsCount);
	} else {
		if(report->packetCount>report->totalPackets) {
			fprintf(stream,"There's something wrong: the client received more packets than the total number set with -n.\n");
			fprintf(stream,"A negative percentage packet loss may occur.\n");
		}

		// Latency/RTT is computed over all the correctly received packets, excluding all the packets which caused timestamping errors (counted by report->errorsCount)
		fprintf(stream,"Latency over %" PRIu64 " packets:\n"
			"(%s)%s Minimum: %.3f ms - Maximum: %.3f ms - Average: %.3f ms\n"
			"Standard Dev.: %.4f ms\n",
			report->totalPackets-report->errorsCount,
			latencyTypePrinter(report->latencyType),
			report->followupMode!=FOLLOWUP_OFF ? " (follow-up)" : "",
			report->minLatency==UINT64_MAX ? 0 : ((double) report->minLatency)/1000, 
			((double) report->maxLatency)/1000,
			report->averageLatency/1000,
			sqrt(report->variance)/1000);

		// Print only the confidence intervals which were requested
		for(i=0;i<CONFINT_NUMBER;i++) {
			if(confidenceIntervalsMask & (1<<i)) {
				fprintf(stream,"Confidence intervals (%s): [%.3f ; %.3f] ms\n",
					confidenceIntervalLabels[i],
					report->averageLatency-report->confidenceIntervalDev[i]<0?0:(report->averageLatency-report->confidenceIntervalDev[i])/1000,
					(report->averageLatency+report->confidenceIntervalDev[i])/1000);
			}
		}

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

		// Print the number of packets which caused timestamping errors
		fprintf(stream,"Error count: %" PRIu64 "\n",report->errorsCount);

		fprintf(stream, "Out of order count (approx. as the number of times a decreasing seq. number is detected): %" PRIu64 "\n",
			report->outOfOrderCount);

		fprintf(stream,"Est. number of cyclical sequence number resets: %" PRIu64 "\n",
			report->seqNumberResets);

		// If a timeout occurred, print that a timeout occurred and print also the packet loss up to that sequence number.
		// The real last sequence number (as if LaMP sequence numbers were not cyclical) is estimated using report->seqNumberResets, with:
		// (report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber-report->packetCount+1).
		if(report->_timeoutOccurred==1) {
			fprintf(stream,"Timeout occurred: last sequence number: %" PRIu16 " (non-cyclical: %" PRIu64 ")\nEst. packet loss up to last sequence number: %.2f%% [%" PRIi64 "/%" PRIi64 "]\n",
				report->lastSeqNumber,
				(report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber,
				((double)(report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber+1-report->packetCount)*100/((report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber+1),
				(report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber-report->packetCount+1,
				(report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber+1);
		}
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
	double lostPktPercLastSeqNo=0;

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
			dprintf(csvfp,"Date,"
				"Time,"
				"ClientMode,"
				"SocketType,"
				"Protocol,"
				"UP,"
				"PayloadLen-B,"
				"TotReqPackets,"
				"Interval-ms,"
				"Interval-type,"
				"Interval-Distrib-Param,"
				"Interval-Distrib-Batch-Size,"
				"LatencyType,"
				"FollowUp,"
				"MinLatency-ms,"
				"MaxLatency-ms,"
				"AvgLatency-ms,"
				"LostPackets-Perc,"
				"ErrorsCount,"
				"OutOfOrderCountDecr,"
				"StDev-ms,"
				"SeqNumberResets,"
				"TimeoutOccurred,"
				"lastSeqNumber,"
				"lastSeqNumber-reconstructed-noncyclical,"
				"LostPacketLastSeq-Perc,"
				"ConfInt90l,"
				"ConfInt90u,"
				"ConfInt95l,"
				"ConfInt95u,"
				"ConfInt99l,"
				"ConfInt99u\n");
		}

		// Set lostPktPerc depending on the sign of report->totalPackets-report->packetCount (the negative sign should never occur in normal program operations)
		// If no reportStructureUpdate() was ever called (i.e. minLatency is still UINT64_MAX), set the lost packets percentage to 100%
		if(report->minLatency!=UINT64_MAX) {
			lostPktPerc=report->totalPackets>=report->packetCount ? ((double) ((report->totalPackets-report->packetCount)))*100/(report->totalPackets) : ((double) ((report->packetCount-report->totalPackets)))*-100/(report->packetCount);
		} else {
			lostPktPerc=100;
		}

		// Do almost the same for the lost packet percentage up to the last received sequence number
		if(report->minLatency!=UINT64_MAX) {
			lostPktPercLastSeqNo=((double)(report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber+1-report->packetCount)*100/((report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber);
		} else {
			lostPktPercLastSeqNo=100;
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
			"%.3f,"					// interval between packets (in ms)
			"%s,"					// (random) interval type
			"%lf,"					// random interval param (if available, if not, it is equal to -1)
			"%" PRIu64 ","			// random interval batch size (if available, if not using random intervals, it is forced to be always = total number of packets)
			"%s,"					// latency type (-L)
			"%s,"					// follow-up (-F)
			"%.3f,"					// minLatency
			"%.3f,"					// maxLatency
			"%.3f,"					// avgLatency
			"%.2f,"					// lost packets (perc)
			"%" PRIu64 ","			// errors count
			"%" PRIu64 ","			// out-of-order count
			"%.4f,"					// standard deviation
			"%" PRIu64 ","			// est. number of sequence number resets
			"%d,"					// timeout occurred (0 = no, 1 = yes)
			"%" PRIu16 ","			// last sequence number
			"%" PRIu64 ","			// last sequence number (non cyclical, reconstructed)
			"%.2f,",				// lost packets up to last sequence number (perc)
			opts->macUP==UINT8_MAX ? 0 : opts->macUP,																				// macUP (UNSET is interpreted as '0', as AC_BE seems to be used when it is not explicitly defined)
			opts->payloadlen,																										// out-of-order count (# of decreasing sequence breaks)
			opts->number,																											// total number of packets requested
			(double) opts->interval,																								// interval between packets (in ms)
			opts->rand_type==NON_RAND ? "fixed periodic" : enum_to_str_rand_distribution_t(opts->rand_type),						// (random) interval type
			opts->rand_param,																										// random interval param (if available, if not, it is equal to -1)
			opts->rand_batch_size,																									// random interval batch size (if available, if not using random intervals, it is forced to be always = total number of packets)
			latencyTypePrinter(report->latencyType),																				// latency type (-L)
			report->followupMode!=FOLLOWUP_OFF ? "On" : "Off",																		// follow-up (-F)					
			report->minLatency==UINT64_MAX ? 0 : ((double) report->minLatency)/1000,												// minLatency
			((double) report->maxLatency)/1000,																						// maxLatency
			report->minLatency==UINT64_MAX ? 0 : report->averageLatency/1000,														// avgLatency
			lostPktPerc,																											// lost packets (perc)
			report->errorsCount,																									// errors count
			report->outOfOrderCount,																								// Out-of-order count
			sqrt(report->variance)/1000,																							// standard deviation (sqrt of variance)
			report->seqNumberResets,																								// est. number of sequence number resets
			report->_timeoutOccurred,																								// timeout occurred (0 = no, 1 = yes)
			report->lastSeqNumber,																									// last sequence number
			(report->seqNumberResets*UINT16_TOP)+report->lastSeqNumber,															// last sequence number (non cyclical, reconstructed)
			lostPktPercLastSeqNo);																									// lost packets up to last sequence number (perc)																						

		// Save confidence intervals data (if a confidence interval is negative, limit it to 0, as real latency cannot be negative)
		for(int i=0;i<CONFINT_NUMBER;i++) {
			dprintf(csvfp,
				"%.3f,"
				"%.3f",
				report->averageLatency-report->confidenceIntervalDev[i]<0?0:(report->averageLatency-report->confidenceIntervalDev[i])/1000,
				(report->averageLatency+report->confidenceIntervalDev[i])/1000);

			if(i<CONFINT_NUMBER-1) {
				dprintf(csvfp,",");
			} else {
				dprintf(csvfp,"\n");
			}
		}

		close(csvfp);

		fprintf(stdout,"Report data was saved inside %s\n",opts->filename);
	} else {
		printOpErrStatus=1;
	}

	return printOpErrStatus;
}

int openTfile(const char *Tfilename,int followup_on_flag) {
	int csvfd;
	char *Tfilename_fileno;

	errno=0;

	if(Tfilename==NULL) {
		return -2;
	}

	csvfd=open(Tfilename, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);

	if(csvfd<0) {
		if(errno==EEXIST) {
			// File already exists
			// Try to create a new file, appending an increasing number after <Tfilename>, until W_MAX_FILE_NUMBER attemps are reached
			int fileno=1;
			int fileopendone=0;

			Tfilename_fileno=malloc((strlen(Tfilename)+W_MAX_FILE_NUMBER_DIGITS+1)*sizeof(char));

			if(!Tfilename_fileno) {
				return -3;
			}

			while(fileno<=W_MAX_FILE_NUMBER && fileopendone==0) {
				snprintf(Tfilename_fileno,strlen(Tfilename)+W_MAX_FILE_NUMBER_DIGITS+2,"%.*s_%0*d.csv",(int) (strlen(Tfilename)-4),Tfilename,W_MAX_FILE_NUMBER_DIGITS,fileno);

				csvfd=open(Tfilename_fileno, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
				if(csvfd<0 && errno==EEXIST) {
					fileno++;
					errno=0;
				} else {
					// File open operation done (i.e. the file was opened or an error occurred)
					fileopendone=1; 
				}
			}

			free(Tfilename_fileno);

			// Attempted to create W_MAX_FILE_NUMBER different files but they all already exist
			// In this case, just append to the original file which was specified
			if(fileopendone==0) {
				csvfd=open(Tfilename, O_WRONLY | O_APPEND);
			}
		}
	}

	if(csvfd<0) {
		return csvfd;
	}

	// Write CSV file header, depending on the followup_on_flag flag value
	if(followup_on_flag==0) {
		dprintf(csvfd,PERPACKET_FILE_HEADER_NO_FOLLOWUP);
	} else {
		dprintf(csvfd,PERPACKET_FILE_HEADER_FOLLOWUP);
	}

	return csvfd;
}

int writeToTFile(int Tfiledescriptor,int decimal_digits,perPackerDataStructure *perPktData) {
	int dprintf_ret_val;

	if(perPktData->followup_on_flag==0) {
		dprintf_ret_val=dprintf(Tfiledescriptor,"%" PRIu64 ",%.*f,%ld.%06ld,%d,%" PRIu64 ",%" PRIu64 "\n",
			perPktData->seqNo,
			decimal_digits,(double)(perPktData->signedTripTime)/1000,
			(long int)(perPktData->tx_timestamp.tv_sec),(long int)(perPktData->tx_timestamp.tv_usec),
			perPktData->signedTripTime<=0 ? 1 : 0,
			perPktData->reportDataPointer!=NULL ? perPktData->reportDataPointer->seqNumberResets : -1,
			perPktData->reportDataPointer!=NULL ? perPktData->reportDataPointer->seqNumberResets*UINT16_TOP+(uint64_t)perPktData->seqNo:-1);
	} else {
		dprintf_ret_val=dprintf(Tfiledescriptor,"%" PRIu64 ",%.*f,%.*f,%ld.%06ld,%d,%" PRIu64 ",%" PRIu64 "\n",
			perPktData->seqNo,
			decimal_digits,(double)(perPktData->signedTripTime)/1000,
			decimal_digits,(double)(perPktData->tripTimeProc)/1000,
			(long int)(perPktData->tx_timestamp.tv_sec),(long int)(perPktData->tx_timestamp.tv_usec),
			perPktData->signedTripTime<=0 ? 1 : 0,
			perPktData->reportDataPointer!=NULL ? perPktData->reportDataPointer->seqNumberResets : -1,
			perPktData->reportDataPointer!=NULL ? perPktData->reportDataPointer->seqNumberResets*UINT16_TOP+(uint64_t)perPktData->seqNo:-1);
	}

	return dprintf_ret_val;
}

void closeTfile(int Tfiledescriptor) {
	if(Tfiledescriptor>0) {
		close(Tfiledescriptor);
	}
}