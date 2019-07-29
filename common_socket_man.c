#include "common_socket_man.h"
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <sys/ioctl.h>
#include <poll.h>

int socketCreator(protocol_t protocol) {
	int sFd;

	switch(protocol) {
		case UDP:
			sFd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
		break;
		default:
			sFd=-1;
		break;
	}

	return sFd;
}

int socketSetTimestamping(struct lampsock_data sData, int mode) {
	int flags;
	int setsockopt_optname;

	struct ifreq ifr;
	struct hwtstamp_config hwconfig;

	struct ethtool_ts_info tsinfo;

	// Check if the request can be satisfied (i.e. check device timestamp capabilities)
	// We are using ethtool.h and the SIOCETHTOOL specific ioctl
	if(mode!=SET_TIMESTAMPING_HW) {
		// Get timestamp info
		tsinfo.cmd=ETHTOOL_GET_TS_INFO;

		strncpy(ifr.ifr_name,sData.devname,IFNAMSIZ);
		ifr.ifr_data=(void *)&tsinfo;

		// Issue request to the driver
		if(ioctl(sData.descriptor,SIOCETHTOOL,&ifr)<0) {
			return SOCKETSETTS_EETHTOOL;
		}
	}

	if(mode==SET_TIMESTAMPING_SW_RX) {
		if((tsinfo.so_timestamping & (SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE))!=(SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE)) {
			return SOCKETSETTS_ENOSUPP;
		}

		setsockopt_optname=SO_TIMESTAMP;
		flags=1;
	} else if(mode==SET_TIMESTAMPING_HW) {
		// Clear hardware timestamping configuration structures (see: kernel.org/doc/Documentation/networking/timestamping.txt)
		// Clear ifr
		memset(&ifr,0,sizeof(ifr)); 
		memset(&hwconfig,0,sizeof(hwconfig));

		// Set ifr_name and ifr_data (see: man7.org/linux/man-pages/man7/netdevice.7.html)
		strncpy(ifr.ifr_name,sData.devname,IFNAMSIZ);
		ifr.ifr_data=(void *)&hwconfig;

		hwconfig.tx_type=HWTSTAMP_TX_ON;
		hwconfig.rx_filter=HWTSTAMP_FILTER_ALL;

		// Issue request to the driver
		if(ioctl(sData.descriptor,SIOCSHWTSTAMP,&ifr)<0) {
			return SOCKETSETTS_ENOHWSTAMPS;
		}

		flags=SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
		setsockopt_optname=SO_TIMESTAMPING;
	} else if(mode==SET_TIMESTAMPING_SW_RXTX) {
		flags=SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE;

		if((tsinfo.so_timestamping & flags)!=flags) {
			return SOCKETSETTS_ENOSUPP;
		}

		setsockopt_optname=SO_TIMESTAMPING;
	} else {
		return SOCKETSETTS_EINVAL;
	}

	return setsockopt(sData.descriptor,SOL_SOCKET,setsockopt_optname,&flags,sizeof(flags));
}

int pollErrqueueWait(int sFd,uint64_t timeout_ms) {
	struct pollfd errqueueMon;
	int poll_retval;

	errqueueMon.fd=sFd;
	errqueueMon.revents=0;
	errqueueMon.events=0;

	while((poll_retval=poll(&errqueueMon,1,timeout_ms))>0 && errqueueMon.revents!=POLLERR);

	return poll_retval;
}