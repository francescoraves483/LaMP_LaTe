#include "common_socket_man.h"
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
#include <sys/ioctl.h>
#include <linux/if.h>

// Condidence interval array sizes
#define TSTUDSIZE90 125
#define TSTUDSIZE95 156
#define TSTUDSIZE99 229

// Condidence interval thresholds array sizes
#define TSTUDSIZE90THRS 13
#define TSTUDSIZE95THRS 16
#define TSTUDSIZE99THRS 22

#define TSTUDTHRS_INCR 0.001

// Macros for extra fields printing in writeToTFile() and writeToUDPSocket()
#define compute_reconstructedSeqNo(perPktData) perPktData->reportDataPointer!=NULL ? \
			perPktData->reportDataPointer->_lastReconstructedSeqNo : \
			-1

#define compute_perTillNow(perPktData) perPktData->reportDataPointer!=NULL ? \
	((double)(perPktData->reportDataPointer->lossCount))/((double)perPktData->reportDataPointer->seqNumberResets*UINT16_TOP+perPktData->reportDataPointer->lastMaxSeqNumber+1-INITIAL_SEQ_NO) : \
	-1

#define compute_minLatency(perPktData) perPktData->reportDataPointer!=NULL ? (double)(perPktData->reportDataPointer->minLatency)/1000 : -1

#define compute_maxLatency(perPktData) perPktData->reportDataPointer!=NULL ? (double)perPktData->reportDataPointer->maxLatency/1000 : -1


static inline double computeLostPktPerc(reportStructure *report) {
	double lostPktPerc;

	// Set lostPktPerc depending on the sign of report->totalPackets-report->packetCount (the negative sign should never occur in normal program operations)
	// If no reportStructureUpdate() was ever called (i.e. minLatency is still UINT64_MAX), set the lost packets percentage to 100%
	if(report->minLatency!=UINT64_MAX) {
		lostPktPerc=report->totalPackets>=report->packetCount ? ((double) ((report->totalPackets-report->packetCount)))*100/(report->totalPackets) : ((double) ((report->packetCount-report->totalPackets)))*-100/(report->packetCount);
	} else {
		lostPktPerc=100;
	}

	return lostPktPerc;
}

static inline double computeLostPktPercLastSeqNo(reportStructure *report) {
	double lostPktPercLastSeqNo;

	// This function works almost as computeLostPktPerc(), but it computes the lost packet percentage up to the last received sequence number
	if(report->minLatency!=UINT64_MAX) {
		lostPktPercLastSeqNo=((double)(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber+1-report->packetCount)*100/((report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber+1);
	} else {
		lostPktPercLastSeqNo=100;
	}

	return lostPktPercLastSeqNo;
}

static inline char *getProtocolName(protocol_t protocol) {
	switch(protocol) {
		case UDP:		
			return "UDP";
		break;
		#if AMQP_1_0_ENABLED
		case AMQP_1_0:
			return "AMQP 1.0";
		break;
		#endif
		default:
			return "Unknown";
		break;
	}
}

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
	time_t currtime=time(NULL);

	return localtime(&currtime);
}

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType, modefollowup_t followupMode, uint8_t dup_detect_enabled) {
	report->averageLatency=0.0;
	report->minLatency=UINT64_MAX;
	report->maxLatency=0;

	report->latencyType=latencyType;
	report->followupMode=followupMode;

	report->outOfOrderCount=0;
	report->packetCount=0;
	report->totalPackets=totalPackets;
	report->errorsCount=0;
	report->lossCount=0;

	report->variance=0;

	// Internal members
	report->_isFirstUpdate=1;

	report->lastMaxSeqNumber=INITIAL_SEQ_NO-1;

	report->seqNumberResets=0;

	report->_timeoutOccurred=0;

	report->_welfordM2=0;

	report->_lastReconstructedSeqNo=-1;

	for(int i=0;i<CONFINT_NUMBER;i++) {
		report->confidenceIntervalDev[i]=-1.0;
	}

	report->dupCount=0;
	if(dup_detect_enabled) {
		// Initialize a dupStoreList data structure for detecting sequence numbers
		// Its size is equal to the minimum number of previously received sequence numbers which should be taken
		// into account when detecting duplicated packets
		report->dupCountList=dupSL_init(SEQUENCE_NUMBERS_RESET_THRESHOLD);
		report->dupCountEnabled=1;
	} else {
		report->dupCountEnabled=0;
	}
}

void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber) {
	uint8_t seqNumberResetOccurred=0;

	// Compute the gap between the currently received sequence number and the last maximum sequence number received so far
	int32_t gap=(int32_t)seqNumber-(int32_t)report->lastMaxSeqNumber;

	// Try to detect a cyclical sequence number reset if there is a big negative gap (i.e. if something like ...->65533->65534->0->2->... happens)
	if(report->lastMaxSeqNumber!=-1 && gap<-SEQUENCE_NUMBERS_RESET_THRESHOLD) {
		report->seqNumberResets++;
		seqNumberResetOccurred=1;
	}
	
	// Reconstruct the current sequence number to obtain its value as if numbers were not cyclical
	// Example: cyclical = 65535->0->1   -   reconstructed = 65535->65536->65537
	// If no cyclical sequence number resets occurred so far, just set _lastReconstructedSeqNo to the current sequence number
	// Take into account also the case in which out of order packets are received after a reset
	// Example: 65535->0->65534->1 ...  -  we should not sum '65536' to '65534'
	// The detection of out of order packets after a reset is performed by detecting a large positive gap between the current
	// packet and the last received (cyclical) maximum sequence number
	report->_lastReconstructedSeqNo = report->seqNumberResets == 0 ?
		seqNumber :
		(report->seqNumberResets-(gap>=SEQUENCE_NUMBERS_RESET_THRESHOLD))*UINT16_TOP+seqNumber;

	// Check for duplicates, saving the current sequence number into a "dupStoreList" (a sort of optimized list for storing
	// already received sequence numbers). If the sequence number has been never received before, DSL_NOTFOUND is returned by
	// dupSL_insertandcheck() and the number is stored. If instead it has been received before, dupSL_insertandcheck() will
	// return DSL_FOUND to signal a duplicated packet
	if(report->dupCountEnabled && dupSL_insertandcheck(report->dupCountList,report->_lastReconstructedSeqNo)==DSL_FOUND) {
		report->dupCount++;
	} else {
		report->packetCount++;

		if(tripTime!=0) {
			report->_welfordAverageLatencyOld=report->averageLatency;
			report->averageLatency+=(tripTime-report->averageLatency)/report->packetCount;

			if(tripTime<report->minLatency) {
				report->minLatency=tripTime;
			}

			if(tripTime>report->maxLatency) {
				report->maxLatency=tripTime;
			}

			// An out of order packet is detected if any decreasing sequence number trend is detected in the sequence of packets,
			// with respect to the maximum sequence number received so far
			// It should not be detected if a normal cyclical reset of sequence numbers has occurred
			// A packet is also considered out of order when a very big positive gap is detected, which should provide more
			// robustness when trying to detect out of order packets just after a cyclical reset occurred
			// For example in: 65535->0->1->65534->2->3 '65534' should be detected as out of order, even if there is not actually
			// a decreasing trend between '1' and '65534'
			if(seqNumberResetOccurred==0 && (seqNumber<=report->lastMaxSeqNumber || gap>=SEQUENCE_NUMBERS_RESET_THRESHOLD)) {
				report->outOfOrderCount++;

				if(seqNumber!=report->lastMaxSeqNumber && report->lossCount>0) {
					report->lossCount--;
				}
			}

			// Update the packet loss count if a loss occurred (used, for the time being, only for the PER-till-now computation 
			// when -W is selected)
			// Manage also the case in which a sequence number reset just occurred (in this case, we must compare the
			// 'reconstructed' version of seqNumber with the last maximum received number - taking into account that also
			// lastMaxSeqNumber is cyclically reset, in such a way that it is sufficient to add UINT16_TOP, without taking
			// into account how many resets occurred so far)
			// Example: 65535->1 - to detect the loss of '0', we should compare '1+65536 (=65537)' with '65535', not '1'
			if(((seqNumberResetOccurred==1 && seqNumber+UINT16_TOP>report->lastMaxSeqNumber+1) ||
				(seqNumberResetOccurred==0 && seqNumber>report->lastMaxSeqNumber+1)) && 
				gap<SEQUENCE_NUMBERS_RESET_THRESHOLD) {

				report->lossCount+=seqNumber-1-report->lastMaxSeqNumber;

				// Negative loss correction (i.e. if a sequence number reset just occurred, we must consider, as said before,
				// seqNumber+UINT16_TOP, thus summing UINT16_TOP to the previous report->lossCount computation)
				if(seqNumberResetOccurred==1) {
					report->lossCount+=UINT16_TOP;
				}
			}

			// Compute the maximum received sequence number so far (as "last sequence number")
			// When a cyclical number reset is detected, the new maximum so far should be the current sequence number, even if,
			// strictly speaking, it is smaller than the last maximum
			// In this way, it is possible to consider a cyclically resetting lastMaxSeqNumber
			// A new maximum is not detected if gap>=SEQUENCE_NUMBERS_RESET_THRESHOLD (i.e. with a very large positive gap), as the
			// current packet is considered, in this case, as out of order.
			if(report->lastMaxSeqNumber!=-1 && (seqNumberResetOccurred==1 || (seqNumber>report->lastMaxSeqNumber && gap<SEQUENCE_NUMBERS_RESET_THRESHOLD))) {
				report->lastMaxSeqNumber=seqNumber;
			}

			// Set, at the beginning, the maximum sequence number so far to seqNumber
			if(report->lastMaxSeqNumber==-1) {
				report->lastMaxSeqNumber=seqNumber;
			}

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

void reportStructureFree(reportStructure *report) {
	if(report->dupCountEnabled) {
		dupSL_free(report->dupCountList);
	}
}

void reportStructureChangeTotalPackets(reportStructure *report, uint64_t totalPackets) {
	report->totalPackets=totalPackets;
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

		// Negative percentages (should never enter here if we are detecting duplicates, i.e. if -D is not specified)
		if(report->packetCount>report->totalPackets) {
			fprintf(stream,"Lost packets: -%.2f%% [-%" PRIi64 "/%" PRIi64 "]\n",
				((double)(report->packetCount-report->totalPackets))*100/(report->totalPackets),
				report->packetCount-report->totalPackets,
				report->totalPackets);
		} else {
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

		if(report->dupCountEnabled) {
			fprintf(stream,"Number of duplicated packets: %" PRIu64 "\n",
				report->dupCount);
		}

		// If a timeout occurred, print that a timeout occurred and print also the packet loss up to that sequence number.
		// The real last (highest so far) sequence number (as if LaMP sequence numbers were not cyclical) is estimated using report->seqNumberResets, with:
		// (report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber-report->packetCount+1).
		if(report->_timeoutOccurred==1) {
			fprintf(stream,"Timeout occurred: highest sequence number: %" PRIu16 " (non-cyclical: %" PRIu64 ")\nEst. packet loss up to highest sequence number: %.2f%% [%" PRIi64 "/%" PRIi64 "]\n",
				report->lastMaxSeqNumber,
				(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber,
				((double)(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber+1-report->packetCount)*100/((report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber+1),
				(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber-report->packetCount+1,
				(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber+1);
		}

		if(!report->dupCountEnabled && report->packetCount>report->totalPackets) {
			fprintf(stream,"There's something wrong: the client received more packets than the total number set with -n.\n");
			fprintf(stream,"A negative percentage packet loss may occur.\n");
			fprintf(stream,"Use -D to enable duplicate packet detection.\n");
		}
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
				"TestDuration-s,"
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
				"HighestSeqNumber,"
				"HighestSeqNumber-reconstructed-noncyclical,"
				"LostPacketHighestSeq-Perc,"
				"reportingSuccessful,"
				"ConfInt90l,"
				"ConfInt90u,"
				"ConfInt95l,"
				"ConfInt95u,"
				"ConfInt99l,"
				"ConfInt99u\n");
		}

		lostPktPerc=computeLostPktPerc(report);

		lostPktPercLastSeqNo=computeLostPktPercLastSeqNo(report);

		/* Save report data to file, in CSV style */
		// Save date and time
		dprintf(csvfp,"%d-%02d-%02d,%02d:%02d:%02d,",currdate->tm_year+1900,currdate->tm_mon+1,currdate->tm_mday,currdate->tm_hour,currdate->tm_min,currdate->tm_sec);

		// Save current mode (unidirectional or pinglike) and socket type
		dprintf(csvfp,"%s,%s,",opts->mode_ub==UNIDIR ? "Unidirectional" : "Pinglike",opts->mode_raw==RAW ? "Raw" : "Non raw");

		// Save current protocol
		dprintf(csvfp,"%s,",getProtocolName(opts->protocol));
		
		// Save report data
		dprintf(csvfp,"%d," 		// macUP
			"%" PRIu16 ","			// payloadLen
			"%" PRIu64 ","			// total number of packets requested
			"%" PRIu32 ","			// total test duration (-i, in s)
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
			"%.2f,"					// lost packets up to last sequence number (perc)
			"%d,",					// reportingSuccessful (= 1 if everything is ok, = 0 if a reporting error occurred)
			opts->macUP==UINT8_MAX ? 0 : opts->macUP,																				// macUP (UNSET is interpreted as '0', as AC_BE seems to be used when it is not explicitly defined)
			opts->payloadlen,																										// out-of-order count (# of decreasing sequence breaks)
			opts->duration_interval == 0 ? opts->number : 0,																		// total number of packets requested
			opts->duration_interval,																								// total test duration (-i, in s)
			(double) opts->interval,																								// interval between packets (in ms)
			opts->rand_type==NON_RAND ? "fixed periodic" : enum_to_str_rand_distribution_t(opts->rand_type),						// (random) interval type
			opts->rand_param,																										// random interval param (if available, if not, it is equal to -1)
			opts->rand_type==NON_RAND ? report->totalPackets : opts->rand_batch_size,																							// random interval batch size (if available, if not using random intervals, it is forced to be always = total number of packets)
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
			report->lastMaxSeqNumber,																								// last sequence number
			(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber,															// last sequence number (non cyclical, reconstructed)
			lostPktPercLastSeqNo,																									// lost packets up to last sequence number (perc)																						
			report->minLatency!=UINT64_MAX);																						// reportingSuccessful (= 1 if everything is ok, = 0 if a reporting error occurred)
		
		// Save confidence intervals data (if a confidence interval is negative, limit it to 0, as real latency cannot be negative)
		// Do this only if there is actually data in the report (i.e. if at least one packet has been received - if no packets have
		// been received or if no report was received back in unidirectional mode, report->minLatency is expected to be still equal
		// to its initialized value (i.e. UINT64_MAX)
		if(report->minLatency!=UINT64_MAX) {
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
		} else {
			dprintf(csvfp,"-1,-1,-1,-1,-1,-1\n");
		}

		close(csvfp);

		if(report->minLatency!=UINT64_MAX) {
			fprintf(stdout,"Report data was saved inside %s\n",opts->filename);
		} else {
			fprintf(stdout,"Empty report data (test failed) was saved inside %s\n",opts->filename);
		}
	} else {
		printOpErrStatus=1;
	}

	return printOpErrStatus;
}

// If this function is called with report==NULL, an empty LaTeEND packet will be sent, formatting its content as:
// 'LaTeEND,<LaMP ID>,srvtermination'
int printStatsSocket(struct options *opts, reportStructure *report, report_sock_data_t *sock_data,uint16_t test_id) {
	char sockbuff_tcp[MAX_w_TCP_SOCK_BUF_SIZE];
	int str_char_count=0;

	struct timespec curr_ts;
	long curr_ts_ms;

	const int confidenceIntervals[CONFINT_NUMBER]={90,95,99};

	if(report==NULL) {
		snprintf(sockbuff_tcp,MAX_w_TCP_SOCK_BUF_SIZE,"LaTeEND,%" PRIu16 ",srvtermination",test_id);
	} else {
		// Get the current time (in s and ns from the epoch, i.e. since 00:00:00 UTC, January 1st 1970)
		if(clock_gettime(CLOCK_REALTIME,&curr_ts)==-1) {
			// In case of error, send a null timestamp
			curr_ts_ms=0;
		} else {
			curr_ts_ms=curr_ts.tv_sec+round(curr_ts.tv_nsec/1.0e6);
		}

		str_char_count=snprintf(sockbuff_tcp,MAX_w_TCP_SOCK_BUF_SIZE,"LaTeEND,%" PRIu16 ","
				"timestamp=%ld,"
				"protocol=%s,"
				"clientmode=%s,"
				"UP=%d,"
				"payloadlen=%" PRIu16 ","
				"totpackets=%" PRIu64 ","
				"testduration_s=%" PRIu32 ","
				"interval_ms=%.3f,"
				"interval_type=%s,"
				"int_distr_param=%lf,"
				"int_distr_batch=%" PRIu64 ","
				"latencytype=%s,"
				"followup=%d,"
				"min_ms=%.3f,"
				"max_ms=%.3f,"
				"avg_ms=%.3f,"
				"lostpkts_perc=%.2f,"
				"errors=%" PRIu64 ","
				"outoforder=%" PRIu64 ","
				"stdev=%.4f,"
				"timeout=%d,"
				"highestseqno=%" PRIu64 ","
				"losttolast=%.2f,"
				"reportok=%d",
				test_id,																								// (LaMP ID after "LaTeEND,")
				curr_ts_ms,																								// timestamp
				getProtocolName(opts->protocol),																		// protocol
				opts->mode_ub==UNIDIR ? "unidirectional" : "pinglike",													// clientmode
				opts->macUP==UINT8_MAX ? 0 : opts->macUP,																// UP
				opts->payloadlen,																						// payloadlen
				opts->duration_interval == 0 ? opts->number : 0,														// totpackets
				opts->duration_interval,																				// testduration_s
				(double) opts->interval,																				// interval_ms
				opts->rand_type==NON_RAND ? "fixed_periodic" : enum_to_str_rand_distribution_t(opts->rand_type),		// interval_type
				opts->rand_param,																						// int_distr_param
				opts->rand_type==NON_RAND ? report->totalPackets : opts->rand_batch_size,								// int_distr_batch
				latencyTypePrinter(report->latencyType),																// latencytype
				report->followupMode,																					// followup (full follow-up mode enum value, =0 if off, >0 if on)
				report->minLatency==UINT64_MAX ? 0 : ((double) report->minLatency)/1000,								// min_ms
				((double) report->maxLatency)/1000,																		// max_ms
				report->minLatency==UINT64_MAX ? 0 : report->averageLatency/1000,										// avg_ms
				computeLostPktPerc(report),																				// lostpkt_perc
				report->errorsCount,																					// errors
				report->outOfOrderCount,																				// outoforder
				sqrt(report->variance)/1000,																			// stdev
				report->_timeoutOccurred,																				// timeout
				(report->seqNumberResets*UINT16_TOP)+report->lastMaxSeqNumber,											// highestseqno (reconstructed, non cyclical)
				computeLostPktPercLastSeqNo(report),																	// losttolast
				report->minLatency!=UINT64_MAX																			// reportok
				);

			// Send only the confidence intervals which were requested trough -C
			for(int i=0;i<CONFINT_NUMBER;i++) {
				if(opts->confidenceIntervalMask & (1<<i)) {
					str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,",confint%dm=%.3f,confint%dp=%.3f",
						confidenceIntervals[i],
						report->averageLatency-report->confidenceIntervalDev[i]<0?0:(report->averageLatency-report->confidenceIntervalDev[i])/1000,
						confidenceIntervals[i],
						(report->averageLatency+report->confidenceIntervalDev[i])/1000);
				}
			}
		}

		// Send the final test data over the TCP socket
		if(send(sock_data->descriptor_tcp,sockbuff_tcp,strlen(sockbuff_tcp),0)!=strlen(sockbuff_tcp)) {
			fprintf(stderr,"%s() error: cannot send the final test data via the specified TCP socket (-w).\n",__func__);
			perror("TCP socket error:");
			return 1;
		}

	return 0;
}

int openTfile(const char *Tfilename, uint8_t overwrite, int followup_on_flag, char enabled_extra_data) {
	int csvfd;
	char *Tfilename_fileno;

	errno=0;

	if(Tfilename==NULL) {
		return -2;
	}

	if(overwrite) {
		csvfd=open(Tfilename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
	} else {
		csvfd=open(Tfilename, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
	}

	if(csvfd<0) {
		if(errno==EEXIST) {
			// File already exists
			// When overwrite==1 we never expect to reach this point
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
		dprintf(csvfd,PERPACKET_COMMON_FILE_HEADER_NO_FOLLOWUP);
	} else {
		dprintf(csvfd,PERPACKET_COMMON_FILE_HEADER_FOLLOWUP);
	}

	// Write additional data header section (the order in which these 'ifs' are written is important)
	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(enabled_extra_data,CHAR_P)) {
		dprintf(csvfd,",PER till now");
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(enabled_extra_data,CHAR_R)) {
		dprintf(csvfd,",Reconstructed Sequence Number");
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(enabled_extra_data,CHAR_M)) {
		dprintf(csvfd,",Current minimum");
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(enabled_extra_data,CHAR_N)) {
		dprintf(csvfd,",Current maximum");
	}

	dprintf(csvfd,"\n");

	return csvfd;
}

// This function tries to open a UDP socket for the per-packet data transmission and a TCP socket for the
// initial data and final aggregated test data transmission
// The outcome is considered successful only when both socket are open (and the TCP socket is successfully
// connected to a server)
int openReportSocket(report_sock_data_t *sock_data,struct options *opts) {
	struct sockaddr_in bind_addrin;
	struct sockaddr_in connect_addrin;
	struct ifreq ifreq;
	int connect_timeout_rval=0;

	if(!opts->udp_params.enabled) {
		return -3;
	}

	sock_data->descriptor_udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);

    if(sock_data->descriptor_udp==-1) {
    	fprintf(stderr,"Error: cannot open UDP socket for per-packet data transmission (-w option).\nDetails: %s\n",strerror(errno));
        return -1;
    }

    sock_data->descriptor_tcp=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);

    if(sock_data->descriptor_tcp==-1) {
    	close(sock_data->descriptor_udp);
    	fprintf(stderr,"Error: cannot open TCP socket for per-packet data transmission (-w option).\nDetails: %s\n",strerror(errno));
        return -1;
    }

    // Bind to the given interface/IP address, if an interface name was specified
   	if(opts->udp_params.devname!=NULL) {
    	// First of all, retrieve the IP address from the specified devname
    	strncpy(ifreq.ifr_name,opts->udp_params.devname,IFNAMSIZ);
    	ifreq.ifr_addr.sa_family=AF_INET;

    	if(ioctl(sock_data->descriptor_udp,SIOCGIFADDR,&ifreq)!=-1) {
			bind_addrin.sin_addr.s_addr=((struct sockaddr_in*)&ifreq.ifr_addr)->sin_addr.s_addr;
		} else {
			fprintf(stderr,"Error: cannot bind the UDP socket for per-packet data transmission to\nthe specified interface (-w option).\n"
				"Details: cannot retrieve IP address for interface %s.\n",opts->udp_params.devname);
			return -3;
		}

    	bind_addrin.sin_port=0;
    	bind_addrin.sin_family=AF_INET;

    	// Call bind() to bind the UDP socket to specified interface
		if(bind(sock_data->descriptor_udp,(struct sockaddr *) &(bind_addrin),sizeof(bind_addrin))<0) {
			fprintf(stderr,"Error: cannot bind the UDP socket for per-packet data transmission to\nthe specified interface (-w option).\nDetails: %s\n",strerror(errno));
			return -3;
		}

		// Call bind() to bind the TCP socket to a specified interface
		if(bind(sock_data->descriptor_udp,(struct sockaddr *) &(bind_addrin),sizeof(bind_addrin))<0) {
			fprintf(stderr,"Error: cannot bind the UDP socket for per-packet data transmission to\nthe specified interface (-w option).\nDetails: %s\n",strerror(errno));
			return -3;
		}
    }

    // Try to connect to a server (needed for TCP), with a timeout of TCP_w_SOCKET_CONNECT_TIMEOUT milliseconds
    connect_addrin.sin_addr=opts->udp_params.ip_addr;
    connect_addrin.sin_port=htons(opts->udp_params.port);
    connect_addrin.sin_family=AF_INET;

    connect_timeout_rval=connectWithTimeout(sock_data->descriptor_tcp,(struct sockaddr *) &(connect_addrin),sizeof(connect_addrin),TCP_w_SOCKET_CONNECT_TIMEOUT);
    if(connect_timeout_rval!=0) {
    	fprintf(stderr,"Error: cannot connect the TCP socket to a server. Please check if any server is running at %s:%" PRIu16 ".\n"
    		"Details: %s.\n",inet_ntoa(opts->udp_params.ip_addr),opts->udp_params.port,connectWithTimeoutStrError(connect_timeout_rval));
    	close(sock_data->descriptor_udp);
    	close(sock_data->descriptor_tcp);
    	return -2;
    }

    memset(&(sock_data->addrto),0,sizeof(sock_data->addrto));
    sock_data->addrto.sin_family=AF_INET;
    sock_data->addrto.sin_port=htons(opts->udp_params.port);
    sock_data->addrto.sin_addr.s_addr=opts->udp_params.ip_addr.s_addr;

	return 0;
}

// When printing additional data with -X, this function shall always be called after updating the report with "reportStructureUpdate()"
int writeToTFile(int Tfiledescriptor,int decimal_digits,perPackerDataStructure *perPktData) {
	int dprintf_ret_val;
	// "PER Till Now" (Packet Error Rate till now) is basically computed as the percentage packet loss over all the packets before the last one
	double perTillNow;
	uint64_t reconstructedSeqNo;

	if(perPktData->followup_on_flag==0) {
		dprintf_ret_val=dprintf(Tfiledescriptor,"%" PRIu64 ",%.*f,%ld.%06ld,%d",
			perPktData->seqNo,
			decimal_digits,(double)(perPktData->signedTripTime)/1000,
			(long int)(perPktData->tx_timestamp.tv_sec),(long int)(perPktData->tx_timestamp.tv_usec),
			perPktData->signedTripTime<=0 ? 1 : 0);
	} else {
		dprintf_ret_val=dprintf(Tfiledescriptor,"%" PRIu64 ",%.*f,%.*f,%ld.%06ld,%d",
			perPktData->seqNo,
			decimal_digits,(double)(perPktData->signedTripTime)/1000,
			decimal_digits,(double)(perPktData->tripTimeProc)/1000,
			(long int)(perPktData->tx_timestamp.tv_sec),(long int)(perPktData->tx_timestamp.tv_usec),
			perPktData->signedTripTime<=0 ? 1 : 0);
	}

	// Print extra data to CSV file, if requested with -X
	// -X 'a' will set all the bits in "enabled_extra_data", thus making the program enter in all the if statements below
	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_P)) {
		perTillNow=compute_perTillNow(perPktData);
		dprintf_ret_val+=dprintf(Tfiledescriptor,",%.2f",perTillNow);
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_R)) {
		reconstructedSeqNo=compute_reconstructedSeqNo(perPktData);
		dprintf_ret_val+=dprintf(Tfiledescriptor,",%" PRIu64,reconstructedSeqNo);
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_M)) {
		// perPktData->reportDataPointer->minLatency contains the current maximum measured latency (at the end of the test will contain the global test maximum)
		dprintf_ret_val+=dprintf(Tfiledescriptor,",%.*f",decimal_digits,compute_minLatency(perPktData));
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_N)) {
		// perPktData->reportDataPointer->minLatency contains the current minimum measured latency (at the end of the test will contain the global test minimum)
		dprintf_ret_val+=dprintf(Tfiledescriptor,",%.*f",decimal_digits,compute_maxLatency(perPktData));
	}

	dprintf_ret_val+=dprintf(Tfiledescriptor,"\n");

	return dprintf_ret_val;
}

int writeToReportSocket(report_sock_data_t *sock_data,int decimal_digits,perPackerDataStructure *perPktData,uint16_t test_id,uint8_t *first_call) {
	char sockbuff[MAX_w_UDP_SOCK_BUF_SIZE];
	char sockbuff_tcp[MAX_w_TCP_SOCK_BUF_SIZE];
	int str_char_count=0;
	int return_error_code=0;

	if(first_call==NULL) {
		return -3;
	}

	if(*first_call) {
		str_char_count=snprintf(sockbuff_tcp,MAX_w_TCP_SOCK_BUF_SIZE,"LaTeINIT,%" PRIu16 ",fields=",test_id);

		if(perPktData->followup_on_flag==0) {
			str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_TCP_SOCK_BUF_SIZE-str_char_count,"%s",PERPACKET_COMMON_SOCK_HEADER_NO_FOLLOWUP);
		} else {
			str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_TCP_SOCK_BUF_SIZE-str_char_count,"%s",PERPACKET_COMMON_SOCK_HEADER_FOLLOWUP);
		}

		// Tell the receiving application which optional fields are set too
		if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_P)) {
			str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,";pertillnow");
		}

		if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_R)) {
			str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,";fullseq");
		}

		if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_M)) {
			str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,";currmin");
		}

		if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_N)) {
			str_char_count+=snprintf(str_char_count+sockbuff_tcp,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,";currmax");
		}

		// Send the current data via the TCP socket
		if(sock_data!=NULL) {
			if(send(sock_data->descriptor_tcp,sockbuff_tcp,strlen(sockbuff_tcp),0)!=strlen(sockbuff_tcp)) {
				fprintf(stderr,"%s() error: cannot send the current information via the specified TCP socket (-w).\n",__func__);
				perror("TCP socket error:");
				return -3;
			}
		} else {
			fprintf(stderr,"%s() error: no valid TCP socket has been set up for the -w option.\n",__func__);
			return -4;
		}

		str_char_count=0;
		*first_call=0;
	}

	// Prepare the full string (i.e. the UDP packet content) to be sent via the UDP socket
	if(perPktData->followup_on_flag==0) {
		str_char_count=snprintf(sockbuff,MAX_w_UDP_SOCK_BUF_SIZE,"LaTe,%" PRIu16 ",%" PRIu64 ",%.*f,%ld.%06ld,%d",
			test_id,
			perPktData->seqNo,
			decimal_digits,(double)(perPktData->signedTripTime)/1000,
			(long int)(perPktData->tx_timestamp.tv_sec),(long int)(perPktData->tx_timestamp.tv_usec),
			perPktData->signedTripTime<=0 ? 1 : 0);
	} else {
		str_char_count=snprintf(sockbuff,MAX_w_UDP_SOCK_BUF_SIZE,"LaTe,%" PRIu16 "%" PRIu64 ",%.*f,%.*f,%ld.%06ld,%d",
			test_id,
			perPktData->seqNo,
			decimal_digits,(double)(perPktData->signedTripTime)/1000,
			decimal_digits,(double)(perPktData->tripTimeProc)/1000,
			(long int)(perPktData->tx_timestamp.tv_sec),(long int)(perPktData->tx_timestamp.tv_usec),
			perPktData->signedTripTime<=0 ? 1 : 0);
	}

	// Save the required extra (-X) data too
	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_P)) {
		str_char_count+=snprintf(str_char_count+sockbuff,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,",%.2f",compute_perTillNow(perPktData));
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_R)) {
		str_char_count+=snprintf(str_char_count+sockbuff,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,",%" PRIu64,compute_reconstructedSeqNo(perPktData));
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_M)) {
		str_char_count+=snprintf(str_char_count+sockbuff,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,",%.*f",decimal_digits,compute_minLatency(perPktData));
	}

	if(CHECK_REPORT_EXTRA_DATA_BIT_SET(perPktData->enabled_extra_data,CHAR_N)) {
		str_char_count+=snprintf(str_char_count+sockbuff,MAX_w_UDP_SOCK_BUF_SIZE-str_char_count,",%.*f",decimal_digits,compute_maxLatency(perPktData));
	}

	// Send the current data via a UDP socket
	if(sock_data!=NULL) {
		if(sendto(sock_data->descriptor_udp,sockbuff,strlen(sockbuff),0,(struct sockaddr *)&(sock_data->addrto),sizeof(struct sockaddr_in))!=strlen(sockbuff)) {
			fprintf(stderr,"%s() error: cannot send the current information via the specified UDP socket (-w).\n",__func__);
			perror("UDP socket error:");
			return_error_code=-1;
		}
	} else {
		fprintf(stderr,"%s() error: no valid UDP socket has been set up for the -w option.\n",__func__);
		return_error_code=-2;
	}

	return return_error_code;
}

void closeTfile(int Tfiledescriptor) {
	if(Tfiledescriptor>0) {
		close(Tfiledescriptor);
	}
}

void closeReportSocket(report_sock_data_t *sock_data) {
	if(sock_data==NULL) {
		return;
	}

	if(sock_data->descriptor_udp>0) {
		close(sock_data->descriptor_udp);
	}

	if(sock_data->descriptor_tcp>0) {
		close(sock_data->descriptor_tcp);
	}
}