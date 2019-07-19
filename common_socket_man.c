#include "common_socket_man.h"
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <poll.h>

int socketCreator(protocol_t protocol) {
	int sFd;

	switch(protocol) {
		case UDP:
			sFd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
		break;
		default:
			sFd=1;
		break;
	}

	return sFd;
}

int socketSetTimestamping(struct lampsock_data sData, int mode) {
	int flags;
	int setsockopt_optname;

	struct ifreq hwtstamp;
	struct hwtstamp_config hwconfig;

	if(mode==SET_TIMESTAMPING_SW) {
		setsockopt_optname=SO_TIMESTAMP;
		flags=1;
	} else if(mode==SET_TIMESTAMPING_HW) {
		// Clear hardware timestamping configuration structures (see: kernel.org/doc/Documentation/networking/timestamping.txt)
		memset(&hwtstamp,0,sizeof(hwtstamp));
		memset(&hwconfig,0,sizeof(hwconfig));

		// Set ifr_name and ifr_data (see: man7.org/linux/man-pages/man7/netdevice.7.html)
		strncpy(hwtstamp.ifr_name,sData.devname,sizeof(hwtstamp.ifr_name));
		hwtstamp.ifr_data=(void *)&hwconfig;

		hwconfig.tx_type=HWTSTAMP_TX_ON;
		hwconfig.rx_filter=HWTSTAMP_FILTER_ALL;

		// Issue request to the driver
		if (ioctl(sData.descriptor,SIOCSHWTSTAMP,&hwtstamp)<0) {
			return -3;
		}

		setsockopt_optname=SO_TIMESTAMPING;
		flags=SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE;
	} else {
		return -2;
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