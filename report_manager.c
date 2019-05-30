#include "report_manager.h"
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h> 
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

// Condidence interval array sizes
#define TSTUDSIZE90 95
#define TSTUDSIZE95 125
#define TSTUDSIZE99 196

// Condidence interval thresholds array sizes
#define TSTUDSIZE90THRS 9
#define TSTUDSIZE95THRS 13
#define TSTUDSIZE99THRS 20

#define TSTUDTHRS_INCR 0.001

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
	float tstudvals90[TSTUDSIZE90]={3.0777,1.8856,1.6377,1.5332,1.4759,1.4398,1.4149,1.3968,1.383,1.3722,1.3634,1.3562,1.3502,1.345,1.3406,1.3368,1.3334,1.3304,1.3277,1.3253,1.3232,1.3212,1.3195,1.3178,1.3163,1.315,1.3137,1.3125,1.3114,1.3104,1.3095,1.3086,1.3077,1.307,1.3062,1.3055,1.3049,1.3042,1.3036,1.3031,1.3025,1.302,1.3016,1.3011,1.3006,1.3002,1.2998,1.2994,1.2991,1.2987,1.2984,1.298,1.2977,1.2974,1.2971,1.2969,1.2966,1.2963,1.2961,1.2958,1.2956,1.2954,1.2951,1.2949,1.2947,1.2945,1.2943,1.2941,1.2939,1.2938,1.2936,1.2934,1.2933,1.2931,1.2929,1.2928,1.2926,1.2925,1.2924,1.2922,1.2921,1.292,1.2918,1.2917,1.2916,1.2915,1.2914,1.2912,1.2911,1.291,1.2909,1.2908,1.2907,1.2906,1.2905};
	float tstudvals95[TSTUDSIZE95]={6.3138,2.92,2.3534,2.1318,2.015,1.9432,1.8946,1.8595,1.8331,1.8125,1.7959,1.7823,1.7709,1.7613,1.7531,1.7459,1.7396,1.7341,1.7291,1.7247,1.7207,1.7171,1.7139,1.7109,1.7081,1.7056,1.7033,1.7011,1.6991,1.6973,1.6955,1.6939,1.6924,1.6909,1.6896,1.6883,1.6871,1.686,1.6849,1.6839,1.6829,1.682,1.6811,1.6802,1.6794,1.6787,1.6779,1.6772,1.6766,1.6759,1.6753,1.6747,1.6741,1.6736,1.673,1.6725,1.672,1.6716,1.6711,1.6706,1.6702,1.6698,1.6694,1.669,1.6686,1.6683,1.6679,1.6676,1.6672,1.6669,1.6666,1.6663,1.666,1.6657,1.6654,1.6652,1.6649,1.6646,1.6644,1.6641,1.6639,1.6636,1.6634,1.6632,1.663,1.6628,1.6626,1.6624,1.6622,1.662,1.6618,1.6616,1.6614,1.6612,1.6611,1.6609,1.6607,1.6606,1.6604,1.6602,1.6601,1.6599,1.6598,1.6596,1.6595,1.6594,1.6592,1.6591,1.659,1.6588,1.6587,1.6586,1.6585,1.6583,1.6582,1.6581,1.658,1.6579,1.6578,1.6577,1.6575,1.6574,1.6573,1.6572,1.6571};
	float tstudvals99[TSTUDSIZE99]={31.8205,6.9646,4.5407,3.7469,3.3649,3.1427,2.998,2.8965,2.8214,2.7638,2.7181,2.681,2.6503,2.6245,2.6025,2.5835,2.5669,2.5524,2.5395,2.528,2.5176,2.5083,2.4999,2.4922,2.4851,2.4786,2.4727,2.4671,2.462,2.4573,2.4528,2.4487,2.4448,2.4411,2.4377,2.4345,2.4314,2.4286,2.4258,2.4233,2.4208,2.4185,2.4163,2.4141,2.4121,2.4102,2.4083,2.4066,2.4049,2.4033,2.4017,2.4002,2.3988,2.3974,2.3961,2.3948,2.3936,2.3924,2.3912,2.3901,2.389,2.388,2.387,2.386,2.3851,2.3842,2.3833,2.3824,2.3816,2.3808,2.38,2.3793,2.3785,2.3778,2.3771,2.3764,2.3758,2.3751,2.3745,2.3739,2.3733,2.3727,2.3721,2.3716,2.371,2.3705,2.37,2.3695,2.369,2.3685,2.368,2.3676,2.3671,2.3667,2.3662,2.3658,2.3654,2.365,2.3646,2.3642,2.3638,2.3635,2.3631,2.3627,2.3624,2.362,2.3617,2.3614,2.361,2.3607,2.3604,2.3601,2.3598,2.3595,2.3592,2.3589,2.3586,2.3584,2.3581,2.3578,2.3576,2.3573,2.357,2.3568,2.3565,2.3563,2.3561,2.3558,2.3556,2.3554,2.3552,2.3549,2.3547,2.3545,2.3543,2.3541,2.3539,2.3537,2.3535,2.3533,2.3531,2.3529,2.3527,2.3525,2.3523,2.3522,2.352,2.3518,2.3516,2.3515,2.3513,2.3511,2.351,2.3508,2.3506,2.3505,2.3503,2.3502,2.35,2.3499,2.3497,2.3496,2.3494,2.3493,2.3492,2.349,2.3489,2.3487,2.3486,2.3485,2.3484,2.3482,2.3481,2.348,2.3478,2.3477,2.3476,2.3475,2.3474,2.3472,2.3471,2.347,2.3469,2.3468,2.3467,2.3466,2.3465,2.3463,2.3462,2.3461,2.346,2.3459,2.3458,2.3457,2.3456,2.3455};
	// Confidence intervals threshold arrays
	int tstudvals90thrs[TSTUDSIZE90THRS]={95,107,122,142,171,215,287,435,893};
	int tstudvals95thrs[TSTUDSIZE95THRS]={125,131,144,158,177,200,230,270,328,418,576,926,2358};
	int tstudvals99thrs[TSTUDSIZE99]={196,206,218,232,247,265,285,308,335,368,408,458,522,607,725,899,1184,1734,3238,24514};

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

void reportStructureInit(reportStructure *report, uint16_t initialSeqNumber, uint64_t totalPackets, latencytypes_t latencyType) {
	report->averageLatency=0.0;
	report->minLatency=UINT64_MAX;
	report->maxLatency=0;

	report->latencyType=latencyType;

	report->outOfOrderCount=0;
	report->packetCount=0;
	report->totalPackets=totalPackets;

	report->variance=0;

	// Internal members
	report->_isFirstUpdate=1;

	// Initialize also the lastSeqNumber, just to be safe against bugs in the following code, which should set, in any case, _lastSeqNumber
	report->_lastSeqNumber=0;

	report->_welfordM2=0;

	for(int i=0;i<CONFINT_NUMBER;i++) {
		report->confidenceIntervalDev[i]=-1.0;
	}
}

void reportStructureUpdate(reportStructure *report, uint64_t tripTime, uint16_t seqNumber) {
	// Set _lastSeqNumber on first update
	if(report->_isFirstUpdate==1) {
		report->_lastSeqNumber = seqNumber==0 ? UINT16_MAX : seqNumber-1;
		report->_isFirstUpdate=0;
	}

	report->packetCount++;

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
	if(report->_lastSeqNumber==UINT16_MAX ? seqNumber!=0 : seqNumber<=report->_lastSeqNumber) {
		report->outOfOrderCount++;
	}

	// Set last sequence number
	report->_lastSeqNumber=seqNumber;

	// Compute the current variance (std dev squared) value using Welford's online algorithm
	report->_welfordM2=report->_welfordM2+(tripTime-report->_welfordAverageLatencyOld)*(tripTime-report->averageLatency);
	if(report->packetCount>1) {
		report->variance=report->_welfordM2/(report->packetCount-1);
	}
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
		// No packets have been received
		fprintf(stream,"No packets received: \n"
			"(-) Minimum: - ms - Maximum: - ms - Average: - ms\n"
			"Standard Dev.: - ms\n"
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
			"(%s) Minimum: %.3f ms - Maximum: %.3f ms - Average: %.3f ms\n"
			"Standard Dev.: %.4f ms\n",
			report->totalPackets,
			latencyTypePrinter(report->latencyType),
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
			dprintf(csvfp,"Date,Time,ClientMode,SocketType,Protocol,UP,PayloadLen-B,TotReqPackets,Interval-s,LatencyType,MinLatency-ms,MaxLatency-ms,AvgLatency-ms,LostPackets-Perc,OutOfOrderCountDecr,StDev-ms,ConfInt90-,ConfInt90+,ConfInt95-,ConfInt95+,ConfInt99-,ConfInt99+\n");
		}

		// Set lostPktPerc depending on the sign of report->totalPackets-report->packetCount (the negative sign should never occur in normal program operations)
		// If no reportStructureUpdate() was ever called (i.e. minLatency is still UINT64_MAX), set the lost packets percentage to 100%
		if(report->minLatency!=UINT64_MAX) {
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
			"%" PRIu64 ","			// out-of-order count
			"%.4f,",				// standard deviation
			opts->macUP==UINT8_MAX ? 0 : opts->macUP,																				// macUP (UNSET is interpreted as '0', as AC_BE seems to be used when it is not explicitly defined)
			opts->payloadlen,																										// out-of-order count (# of decreasing sequence breaks)
			opts->number,																											// total number of packets requested
			((double) opts->interval)/1000,																							// interval between packets (in s)
			latencyTypePrinter(report->latencyType),																				// latency type (-L)
			report->minLatency==UINT64_MAX ? 0 : ((double) report->minLatency)/1000,												// minLatency
			((double) report->maxLatency)/1000,																						// maxLatency
			report->minLatency==UINT64_MAX ? 0 : report->averageLatency/1000,														// avgLatency
			lostPktPerc,																											// lost packets (perc)
			report->outOfOrderCount,																								// Out-of-order count
			sqrt(report->variance)/1000);																							// standard deviation (sqrt of variance)

		// Save confidence intervals data
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