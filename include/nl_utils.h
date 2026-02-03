// nl80211 interface utils
#ifndef NL_RSSI_UTILS_H
#define NL_RSSI_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <linux/nl80211.h>

#define RSSI_UNAVAILABLE -80000

typedef struct {
    struct nl_sock *nl_sock;
    unsigned int ifindex;
    int nl80211_id;
    bool sock_valid;
} nl_sock_info_t;

nl_sock_info_t open_nl_socket(const char *interface_name);
void free_nl_socket(nl_sock_info_t *nl_sock_info);
double get_rssi_from_netlink(uint8_t macaddr[6], nl_sock_info_t nl_sock_info);

#endif