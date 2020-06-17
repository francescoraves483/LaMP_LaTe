#include "carbon_report_manager.h"
#include "common_socket_man.h"
#include <errno.h>
#include <inttypes.h>
#include <linux/if.h>
#include <math.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void carbonReportStructureInit(carbonReportStructure *report) {
	report->averageLatency=0;
	report->maxLatency=0;
	report->minLatency=UINT64_MAX;
	report->variance=0;
	report->packetCount=0;
	report->errorsCount=0;

	report->_welfordM2=0;
}

void carbonReportStructureUpdate(carbonReportStructure *report, uint64_t tripTime) {
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

	// Reset the report
	carbonReportStructureInit(report);

	return 0;
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