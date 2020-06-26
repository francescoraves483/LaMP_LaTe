#include "carbon_report_manager.h"
#include "common_socket_man.h"
#include <errno.h>
#include <inttypes.h>
#include <linux/if.h>
#include <math.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void carbonReportStructureReset(carbonReportStructure *report,uint8_t reset_dup_list) {
	report->averageLatency=0;
	report->maxLatency=0;
	report->minLatency=UINT64_MAX;
	report->variance=0;
	report->packetCount=0;
	report->errorsCount=0;

	report->_welfordM2=0;

	report->outOfOrderCount=0;

	// When a cyclical sequence number reset occurred during the last reporting interval,
	// _maxSeqNumber is a 'reconstructed' value (>65535, as if numbers were not cyclical)
	// Thus, for the next reporting interval, it should be set again to its 'non reconstructed'
	// value (<=65535)
	if(report->_maxSeqNumber!=-1 && report->_detectedSeqNoReset==1) {
		report->_maxSeqNumber-=UINT16_TOP;
	}

	// It will be set to 0 if no cyclical number resets occurred during the current reporting interval,
	// to 1 otherwise
	report->_detectedSeqNoReset=0;

	report->lossCount=0;

	if(reset_dup_list) {
		report->dupCount=0;
		carbonDupSL_reset(report->dupCountList);
	}

	report->_precMaxSeqNumber=report->_maxSeqNumber;
}

void carbonReportStructureInit(carbonReportStructure *report,struct options *opts) {
	report->_maxSeqNumber=INITIAL_SEQ_NO-1;
	report->_precMaxSeqNumber=-1;

	if(opts->dup_detect_enabled) {
		report->dupCountList=carbonDupSL_init(opts->mode_cs == SERVER || opts->mode_cs == LOOPBACK_SERVER ? 
			CARBON_REPORT_DEFAULT_FLUSH_STRUCT_SIZE :
			(int) (opts->carbon_interval*1000/opts->interval));
	}

	carbonReportStructureReset(report,0);
}

void carbonReportStructureUpdate(carbonReportStructure *report,uint64_t tripTime,int32_t seqNo,uint8_t dup_detect_enabled) {
	uint8_t detectedSeqNoResetNow=0;
	int32_t gap;

	// Check for duplicates, saving the current sequence number into a "carbonDupStoreList" (a sort of optimized list for storing
	// already received sequence numbers when reporting intervals are involved). If the sequence number has been never received
	// before, CDSL_NOTFOUND is returned by carbonDupSL_insertandcheck() and the number is stored. If instead it has been received 
	// before, dupSL_insertandcheck() will return CDSL_FOUND to signal a duplicated packet
	// The duplicate check is performed considering the numbers received in the current reporting interval and in the previous one
	if(dup_detect_enabled && carbonDupSL_insertandcheck(report->dupCountList,seqNo)==CDSL_FOUND) {
		report->dupCount++;
	} else {
		report->packetCount++;
		// Compute the gap between the currently received sequence number and the last maximum sequence number received so far
		gap=(int32_t)seqNo-(int32_t)report->_maxSeqNumber;

		// If a reset occurred during the current reporting interval, the gap should be 'corrected' as the stored "_maxSeqNumber"
		// is now a 'reconstructed' number, as if numbers were not cyclical
		if(report->_detectedSeqNoReset==1) {
			gap+=UINT16_TOP;
		}

		// Try to detect a cyclical sequence number reset, by detecting a big negative gap between the currently received 
		// sequence number and the last maximum sequence number received so far
		if(report->_maxSeqNumber!=-1 && gap<-SEQUENCE_NUMBERS_RESET_THRESHOLD) {
			report->_detectedSeqNoReset=1;
			detectedSeqNoResetNow=1;
		}

		// If a reset has been detected during the current reporting interval, reconstruct the sequence numbers of any
		// packet after the reset, as if sequence numbers were not cyclical, to compute the current metrics
		// Example: 65535->0->1, '0' is considered as '65536' and '1' as 65537' until the next reporting interval
		// 'gap<SEQUENCE_NUMBERS_RESET_THRESHOLD' is checked to avoid summing UINT16_TOP to out of order packets coming
		// after a reset, i.e. in: 65535->0->65532->1, 0 and 1 should be 'reconstructed' while 65532 shall not
		if(detectedSeqNoResetNow || (report->_detectedSeqNoReset==1 && gap<SEQUENCE_NUMBERS_RESET_THRESHOLD)) {
			seqNo+=UINT16_TOP;
		}

		// A packet containing a timestamping error should not be taken into account in the current latency computation
		if(tripTime!=0) {
			report->_welfordAverageLatencyOld=report->averageLatency;
			report->averageLatency+=(tripTime-report->averageLatency)/report->packetCount;

			if(tripTime<report->minLatency) {
				report->minLatency=tripTime;
			}

			if(tripTime>report->maxLatency) {
				report->maxLatency=tripTime;
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

		// Try to see if a packet has been received out of order - when no duplicate packet detection is enabled
		// duplicated packets will be reported as "out of order" (that's why the comparison is using "<=" instead of "<")
		// 'gap>=SEQUENCE_NUMBERS_RESET_THRESHOLD' is checked to detect out of order packets after a cyclical
		// sequence number reset occurs, when they are recived in the next reporting intervals after the one in which the
		// reset occurred.
		// Example (= is used to write the reconstructed sequence numbers, when applicable): 
		// |65535 0=65536 1=65537 2=65538 3=65539|4 65534 5 6 7| ...
		// In this case, '65534' is out of order (causing a big positive gap with respect to the (cyclical) maximum received
		// so far, i.e. 4). However, it is not strictly true that 'seqNo<=report->_maxSeqNumber'
		if(detectedSeqNoResetNow==0 && (seqNo<=report->_maxSeqNumber || gap>=SEQUENCE_NUMBERS_RESET_THRESHOLD)) {
			report->outOfOrderCount++;

			// When an out of order packet is detected, we can remove one loss,
			// as an older missing packet, which was detected as lost, has been
			// received now
			// This is done only if the out of order packet is related to the current reporting interval and it is actually
			// 'restoring' a packet loss which occurred during the current interval (checking 'seqNo>report->_precMaxSeqNumber')
			if(report->lossCount>0 && seqNo>report->_precMaxSeqNumber) {
				report->lossCount--;
			}
		}

		// Compute the maximum received sequence number and try to see if there is a gap in the sequence numbers, which could
		// potentially indicate a packet loss
		// Packets implying a big positive gap with respect to the maximum sequence number received so far should not be
		// considered, as they are likely out of order after a cyclical sequence number reset occurs
		if(seqNo>report->_maxSeqNumber && gap<SEQUENCE_NUMBERS_RESET_THRESHOLD) {
			if(seqNo>report->_maxSeqNumber+1) {
				report->lossCount+=seqNo-1-report->_maxSeqNumber;
			}

			report->_maxSeqNumber=seqNo;
		}
	}
}

int carbonReportStructureFlush(carbonReportStructure *report,struct options *opts,int decimal_digits,uint8_t add_one) {
	char sockbuff[MAX_g_SOCK_BUF_SIZE];
	struct timespec now;

	if(report==NULL) {
		return -1;
	}

	// Don't send anything if there is no data in the report structure (maybe the report flush interval is too short?)
	if(report->minLatency==UINT64_MAX) {
		if(report->errorsCount) {
			fprintf(stderr,"Warning: all the packets received during the current flush interval were invalid.\n");
		}
		return 1;
	}

	if(opts->carbon_metric_path==NULL || !opts->carbon_sock_params.enabled) {
		return -1;
	}

	// Get current timestamp
	clock_gettime(CLOCK_REALTIME,&now);

	now.tv_sec+=add_one;

	// Prepare buffer for the average latency
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.avg %.*f %" PRIu64 "\n",opts->carbon_metric_path,decimal_digits,report->averageLatency/1000.0,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the average latency metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the maximum latency
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.max %.*f %" PRIu64 "\n",opts->carbon_metric_path,decimal_digits,(double)report->maxLatency/1000.0,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the maximum latency metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the minimum latency
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.min %.*f %" PRIu64 "\n",opts->carbon_metric_path,decimal_digits,(double)report->minLatency/1000.0,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the minimum latency metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the standard deviation
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.stdev %.*f %" PRIu64 "\n",opts->carbon_metric_path,decimal_digits,sqrt(report->variance)/1000.0,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the average latency metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the packet count
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.count %" PRIu64 " %" PRIu64 "\n",opts->carbon_metric_path,report->packetCount,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the packet count metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the error count
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.errors %" PRIu64 " %" PRIu64 "\n",opts->carbon_metric_path,report->errorsCount,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the error count metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the out of order count
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.outoforder %" PRIu64 " %" PRIu64 "\n",opts->carbon_metric_path,report->outOfOrderCount,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the out of order count metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	// Prepare buffer for the estimated packet loss count
	snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.packetloss %" PRIu64 " %" PRIu64 "\n",opts->carbon_metric_path,report->lossCount,now.tv_sec);

	// Send to Graphite
	if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
		fprintf(stderr,"%s() error: cannot send the out of order count metric to Carbon. Details: %s\n",__func__,strerror(errno));
		return -3;
	}

	if(opts->dup_detect_enabled) {
		// Prepare buffer for the detected number of duplicated packets
		snprintf(sockbuff,MAX_g_SOCK_BUF_SIZE,"%s.dupcount %" PRIu64 " %" PRIu64 "\n",opts->carbon_metric_path,report->dupCount,now.tv_sec);

		// Send to Graphite
		if(send(report->socketDescriptor,sockbuff,strlen(sockbuff),0)!=strlen(sockbuff)) {
			fprintf(stderr,"%s() error: cannot send the duplicate packet count metric to Carbon. Details: %s\n",__func__,strerror(errno));
			return -3;
		}
	}

	if(opts->verboseFlag) {		
		fprintf(stdout,"[INFO] Reporting the following metrics related to network reliability:\n"
			"Current interval packet loss = %" PRIu64 "\n"
			"Current interval out of order count = %" PRIu64 "\n"
			"Current interval duplicated packet count = %" PRIu64 "\n",
			report->lossCount,
			report->outOfOrderCount,
			report->dupCount
			);
	}

	// Reset the report
	carbonReportStructureReset(report,opts->dup_detect_enabled);

	return 0;
}

void carbonReportStructureFree(carbonReportStructure *report,struct options *opts) {
	if(opts->dup_detect_enabled) {
		carbonDupSL_free(report->dupCountList);
	}
}

int openCarbonReportSocket(carbonReportStructure *report,struct options *opts) {
	struct sockaddr_in bind_addrin;
	struct sockaddr_in connect_addrin;
	struct ifreq ifreq;
	int connect_rval=0;
	int sfd;

	if(!opts->carbon_sock_params.enabled) {
		return -3;
	}

	// Check if the user specified the usage of a UDP or TCP socket
	if(opts->carbon_sock_type==G_UDP) {
		sfd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
	} else {
		sfd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
	}

	// Bind to the given interface/IP address, if an interface name was specified
   	if(opts->carbon_sock_params.devname!=NULL) {
    	// First of all, retrieve the IP address from the specified devname
    	strncpy(ifreq.ifr_name,opts->carbon_sock_params.devname,IFNAMSIZ);
    	ifreq.ifr_addr.sa_family=AF_INET;

    	if(ioctl(sfd,SIOCGIFADDR,&ifreq)!=-1) {
			bind_addrin.sin_addr.s_addr=((struct sockaddr_in*)&ifreq.ifr_addr)->sin_addr.s_addr;
		} else {
			fprintf(stderr,"Error: cannot bind the socket for metrics transmission to Carbon to\nthe specified interface (-g option).\n"
				"Details: cannot retrieve IP address for interface %s.\n",opts->carbon_sock_params.devname);
			return -3;
		}

    	bind_addrin.sin_port=0;
    	bind_addrin.sin_family=AF_INET;

    	// Call bind() to bind the UDP socket to specified interface
		if(bind(sfd,(struct sockaddr *) &(bind_addrin),sizeof(bind_addrin))<0) {
			fprintf(stderr,"Error: cannot bind the socket for metrics transmission to Carbon to\nthe specified interface (-g option).\n"
				"Details: %s\n",strerror(errno));
			return -3;
		}
    }

    // Prepare a data struct and call connect() 
    // For TCP it will perform the actual connection establishment
    // For UDP it will just set the default address at which packets will be sent, for each send() call
    connect_addrin.sin_addr=opts->carbon_sock_params.ip_addr;
    connect_addrin.sin_port=htons(opts->carbon_sock_params.port);
    connect_addrin.sin_family=AF_INET;

    if(opts->carbon_sock_type==G_UDP) {
    	connect_rval=connect(sfd,(struct sockaddr *) &(connect_addrin),sizeof(connect_addrin));
    } else {
    	connect_rval=connectWithTimeout(sfd,(struct sockaddr *) &(connect_addrin),sizeof(connect_addrin),TCP_g_SOCKET_CONNECT_TIMEOUT);
    	if(connect_rval!=0) {
    		fprintf(stderr,"Error: connectWithTimeout() could not be completed. Details: %s.\n",connectWithTimeoutStrError(connect_rval));
    	}
    }

    if(connect_rval!=0) {
    	fprintf(stderr,"Error: cannot connect the socket to any Carbon server. Please check if any server is running at %s:%" PRIu16 ".\n",
    		inet_ntoa(opts->carbon_sock_params.ip_addr),opts->carbon_sock_params.port);
    	close(sfd);
    	return -2;
    }

    report->socketDescriptor=sfd;

	return 0;
}

void closeCarbonReportSocket(carbonReportStructure *report) {
	if(report->socketDescriptor>0) {
		close(report->socketDescriptor);
	}
}