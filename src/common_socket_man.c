#include "common_socket_man.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <linux/ethtool.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>

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

int socketDataSetup(protocol_t protocol,struct lampsock_data *sData,struct options *opts,struct src_addrs *addressesptr) {
	// wlanLookup index and return value
	int wlanLookupIdx=0;

	int sds_retval=1;

	// Only UDP is supported as of now
	if(protocol!=UDP) {
		return 0;
	}

	// Set wlanLookupIdx, depending on the mode (loopback vs normal mode)
	if(opts->mode_cs==LOOPBACK_CLIENT || opts->mode_cs==LOOPBACK_SERVER) {
		wlanLookupIdx=WLANLOOKUP_LOOPBACK;
	} else {
		wlanLookupIdx=opts->if_index;
	}

	// Allocate memory for the source MAC address (only if 'RAW' is specified)
	if(opts->mode_raw==RAW) {
		addressesptr->srcmacaddr=prepareMacAddrT();
		if(macAddrTypeGet(addressesptr->srcmacaddr)==MAC_NULL) {
			fprintf(stderr,"Error: could not allocate memory to store the source MAC address.\n");
			return 0;
		}
	}

	// Look for available wireless/non-wireless interfaces, only if the user did not want to bind to all local interfaces (using option -N)
	switch(opts->nonwlan_mode) {
		case NONWLAN_MODE_WIRELESS:
		case NONWLAN_MODE_WIRED:
			{
			rawsockerr_t ret_wlanl_val;

			// Null the devname field in sData
			memset(sData->devname,0,IFNAMSIZ*sizeof(char));

			ret_wlanl_val=wlanLookup(sData->devname,&(sData->ifindex),addressesptr->srcmacaddr,&(addressesptr->srcIPaddr),wlanLookupIdx,opts->nonwlan_mode==NONWLAN_MODE_WIRELESS ? WLANLOOKUP_WLAN : WLANLOOKUP_NONWLAN);
			if(ret_wlanl_val<=0) {
				rs_printerror(stderr,ret_wlanl_val);
				return 0;
			}

			if(opts->mode_raw==RAW) {
				if(opts->dest_addr_u.destIPaddr.s_addr==addressesptr->srcIPaddr.s_addr) {
					fprintf(stderr,"Error: you cannot test yourself in raw mode.\n"
						"Use non raw sockets instead.\n");
					return 0;
				}

				// Check if wlanLookup() was able to properly write the source MAC address
				if(macAddrTypeGet(addressesptr->srcmacaddr)==MAC_BROADCAST || macAddrTypeGet(addressesptr->srcmacaddr)==MAC_NULL) {
					fprintf(stderr,"Could not retrieve source MAC address.\n");
					return 0;
				} else if(macAddrTypeGet(addressesptr->srcmacaddr)==MAC_ZERO) {
					// The returned MAC is 00:00:00:00:00:00, i.e. MAC_ZERO, as in the case of a tun interface with no MAC
					fprintf(stderr,"No valid MAC could be retrieved.\nProbably the selected interface is not an AF_PACKET one and has no MAC address.\n"
						"Please switch to non-raw sockets.\n");
					return 0;
				}
			}

			// In loopback mode, set the destination IP address as the source one, inside the 'options' structure
			if(opts->mode_cs==LOOPBACK_CLIENT || opts->mode_cs==LOOPBACK_SERVER) {
				options_set_destIPaddr(opts,addressesptr->srcIPaddr);
			}

			}
			break;

		case NONWLAN_MODE_ANY:
			if(opts->mode_raw==RAW) {
				fprintf(stderr,"Error: you requested to bind to all local interfaces, but raw sockets require\n"
					"a specific interface to be defined.\n");
				return 0;
			}

			break;

		case NONWLAN_MODE_FORCED_NAME:
			{
			// If a better solution, which does not require opening any dummy socket to get the IP address, is found, these
			// variables should probably be removed
			int dummysFd;
			struct ifreq ifreq;

			// Check if the specified interface is available and, if yes, get its interface index and IP address
			// if_nametoindex() is used (defined in <net/if.h>)
			sData->ifindex=if_nametoindex(opts->opt_devname);

			if(!sData->ifindex) {
				fprintf(stderr,"Error: cannot find the specified interface: '%s'.\n",opts->opt_devname);
				return 0;
			}

			// If everything is ok, copy opts->opt_devname into sData->devname
			strncpy(sData->devname,opts->opt_devname,IFNAMSIZ);

			// If a better solution, which does not require opening any dummy socket to get the IP address, is found, this part
			// should be updated accordingly
			dummysFd=socket(AF_INET,SOCK_DGRAM,0);

			strncpy(ifreq.ifr_name,sData->devname,IFNAMSIZ);
			ifreq.ifr_addr.sa_family=AF_INET;
			if(ioctl(dummysFd,SIOCGIFADDR,&ifreq)!=-1) {
				addressesptr->srcIPaddr=((struct sockaddr_in*)&ifreq.ifr_addr)->sin_addr;

				if(opts->verboseFlag) {
					fprintf(stdout,"[INFO] Interface '%s' has IP address %s.\n",opts->opt_devname,inet_ntoa(addressesptr->srcIPaddr));
				}
			} else {
				fprintf(stderr,"Error: Interface '%s' exists, but no IP address could be found for it.\n",opts->opt_devname);
				sds_retval=0;
			}

			// When using RAW sockets, retrieve also the source MAC address
			if(opts->mode_raw==RAW) {
				strncpy(ifreq.ifr_name,sData->devname,IFNAMSIZ);
				if(ioctl(dummysFd,SIOCGIFHWADDR,&ifreq)!=-1) {
					memcpy(addressesptr->srcmacaddr,ifreq.ifr_hwaddr.sa_data,MAC_ADDR_SIZE);
				} else {
					fprintf(stderr,"Could not retrieve source MAC address. ioctl() error.\n");
					return 0;
				}

				// Check if it was possible to retrieve the right source MAC address
				if(macAddrTypeGet(addressesptr->srcmacaddr)==MAC_BROADCAST || macAddrTypeGet(addressesptr->srcmacaddr)==MAC_NULL) {
					fprintf(stderr,"Could not retrieve source MAC address.\n");
					return 0;
				} else if(macAddrTypeGet(addressesptr->srcmacaddr)==MAC_ZERO) {
					// The returned MAC is 00:00:00:00:00:00, i.e. MAC_ZERO, as in the case of a tun interface with no MAC
					fprintf(stderr,"No valid MAC could be retrieved.\nProbably the selected interface is not an AF_PACKET one and has no MAC address.\n"
						"Please switch to non-raw sockets.\n");
					return 0;
				}
			}

			close(dummysFd);

			}

			break;

		default:
			fprintf(stderr,"Error: a programming error has occurred: unknown opts->nonwlan_mode in socketDataSetup().\n"
				"Please report this bug to the authors of the program.\n");
			sds_retval=0;
			break;
	}

	return sds_retval;
}

// socketOpen will automalically close the socket in case of error
int socketOpen(protocol_t protocol,struct lampsock_data *sData,struct options *opts,struct src_addrs *addressesptr) {
	// Only UDP is supported as of now
	if(protocol!=UDP) {
		return 0;
	}

	// Open socket, discriminating the raw and non raw cases
	switch(opts->mode_raw) {
		case RAW:
			// Workaraound for hardware receive timestamps: ETH_P_ALL does not seem to generate
			// receive timestamps, as of now. So, as UDP only is currently supported, use ETH_P_IP
			// instead of ETH_P_ALL. This will hopefully change in the future.
			sData->descriptor=socket(AF_PACKET,SOCK_RAW,htons(ETH_P_IP));

			if(sData->descriptor==-1) {
				perror("socket() error");
				return 0;
			}

			// Prepare sockaddr_ll structure
			memset(&(sData->addru.addrll),0,sizeof(sData->addru.addrll));
			sData->addru.addrll.sll_ifindex=sData->ifindex;
			sData->addru.addrll.sll_family=AF_PACKET;
			sData->addru.addrll.sll_protocol=htons(ETH_P_IP);

			// Bind to the specified interface
			if(bind(sData->descriptor,(struct sockaddr *) &(sData->addru.addrll),sizeof(sData->addru.addrll))<0) {
				perror("Cannot bind to interface: bind() error");
				close(sData->descriptor);
				return 0;
			}
			break;

		case NON_RAW:
			// Add a nested switch case here when more than one protocol will be implemented (*)
			sData->descriptor=socketCreator(UDP);

			if(sData->descriptor==-1) {
				perror("socket() error");
				return 0;
			}

			// case UDP: (*) - when more than one protocol will be implemented...
			// Prepare bind sockaddr_in structure (index 0)
			memset(&(sData->addru.addrin[0]),0,sizeof(sData->addru.addrin[0]));
			sData->addru.addrin[0].sin_family=AF_INET;

			if(opts->mode_cs==CLIENT || opts->mode_cs==LOOPBACK_CLIENT) {
				sData->addru.addrin[0].sin_port=0;
			} else if(opts->mode_cs==SERVER || opts->mode_cs==LOOPBACK_SERVER) {
				sData->addru.addrin[0].sin_port=htons(opts->port);
			}

			if(opts->nonwlan_mode!=NONWLAN_MODE_ANY) {
				sData->addru.addrin[0].sin_addr.s_addr=addressesptr->srcIPaddr.s_addr;
			} else {
				sData->addru.addrin[0].sin_addr.s_addr=INADDR_ANY;
			}

			// Bind to the specified interface
			if(bind(sData->descriptor,(struct sockaddr *) &(sData->addru.addrin[0]),sizeof(sData->addru.addrin[0]))<0) {
				perror("Cannot bind to interface: bind() error");
				close(sData->descriptor);
				return 0;
			}
			break;

		default:
			// Should never enter here
			sData->descriptor=-1;
			break;
	}


	// Only if -A was specified, set a certain EDCA AC (works only in patched kernels, as of now)
	if(opts->macUP!=UINT8_MAX && setsockopt(sData->descriptor,SOL_SOCKET,SO_PRIORITY,&(opts->macUP),sizeof(opts->macUP))!=0) {
		perror("setsockopt() for SO_PRIORITY error");
		close(sData->descriptor);
		return 0;
	}

	return 1;
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

int connectWithTimeout(int sockfd, const struct sockaddr *addr,socklen_t addrlen,int timeout_ms) {
	int sock_opt;
	int connect_rval=0;
	int poll_rval;
	struct pollfd socketMon;

	// Retrieve the socket descriptor flags
	sock_opt=fcntl(sockfd,F_GETFL,NULL);
	if(sock_opt<0) {
		return CONNECT_ERROR_FCNTL_GETFL;
	}

	// Set the non-blocking flag to the socket
	if (fcntl(sockfd,F_SETFL,sock_opt | O_NONBLOCK)<0) {
		return CONNECT_ERROR_FCNTL_SETNOBLK;
	}

	socketMon.fd=sockfd;
	socketMon.events=POLLOUT;
	socketMon.revents=0;

	connect_rval=connect(sockfd,addr,addrlen);

	if(connect_rval<0) {
		// Check if the connect operation is still in progress
		if(errno==EINPROGRESS) {
			// Run poll() multiple times if it was interrupted by some signal (i.e. if errno was set to EINTR)
			do {
				poll_rval=poll(&socketMon,1,timeout_ms);
			} while(errno==EINTR);

			// Check the poll() return value (=0: timeout occurred, <0: poll error, >0: success)
			if(poll_rval==0) {
				return CONNECT_ERROR_TIMEOUT;
			} else if(poll_rval<0) {
				return CONNECT_ERROR_POLL;
			} else {
				// If everything was ok, check if the connect() completed successfully, by looking at SO_ERROR, as reported in connect(2)
				socklen_t int_len=sizeof(int);
				int so_error_val;

              	if (getsockopt(sockfd,SOL_SOCKET,SO_ERROR,(void*)(&so_error_val),&int_len) < 0) { 
                	return CONNECT_ERROR_STATUS_UNKNOWN;
              	}

              	// The connect() operation did not complete successfully, return so_error_val, containing why this operation failed
              	if(so_error_val) {
              		return so_error_val;
              	} else {
              		connect_rval=0; // Everything is ok, as of now, as if a call to a blocking connect returned 0
              	}
			}
		} else if(errno==EISCONN) {
			// EISCONN means that the TCP socket is already connected
			return CONNECT_ERROR_ALREADY_CONNECTED;
		} else {
			// connect() immediately failed
			return CONNECT_ERROR_IMMEDIATEFAILURE;
		}
	}

	// If no error occurred, we can proceed in restoring the blocking flag of the socket
	if (fcntl(sockfd,F_SETFL,sock_opt) < 0) {
		return CONNECT_ERROR_FCNTL_RESTOREBLK;
	}

	return connect_rval;
}

char *connectWithTimeoutStrError(int retval) {
	switch(retval) {
		case CONNECT_ERROR_FCNTL_GETFL:
			return "fcntl() GETFL error";
			break;

		case CONNECT_ERROR_FCNTL_SETNOBLK:
			return "fcntl() set non-blocking socket failed";
			break;

		case CONNECT_ERROR_FCNTL_RESTOREBLK:
			return "fcntl() could not restore blocking state to socket";
			break;

		case CONNECT_ERROR_TIMEOUT:
			return "TCP connection timed out";
			break;

		case CONNECT_ERROR_IMMEDIATEFAILURE:
			return "TCP connection could not be established on first attempt";
			break;

		case CONNECT_ERROR_POLL:
			return strerror(retval);
			break;

		case CONNECT_ERROR_STATUS_UNKNOWN:
			return "Unknown connect() status after trying to connect; cannot run getsockopt() to retrieve socket status";
			break;

		case CONNECT_ERROR_ALREADY_CONNECTED:
			return "TCP socket is already connected";
			break;

		default:
			return strerror(retval);
			break;
	}

	return "This point should have never been reached; please report this bug to the developers";
}