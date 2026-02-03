#include "nl_utils.h"
#include <stdio.h>
#include <string.h>
#include <net/if.h>

// Structure to store the Netlink callback arguments
struct nl_cb_args {
    int signal_lv; // Retrieved RSSI
    uint8_t macaddr[6]; // Target MAC address (only the RSSI of the device with this MAC will be retrieved)
};

// Static callback function for netlink message processing
static int nl_callback_handler(struct nl_msg *msg, void *arg) {
    // Get the arguments casting again to struct nl_cb_args * from void *
    struct nl_cb_args *nl_cb_args_ptr = (struct nl_cb_args *)arg;

    // Extract the header and attributes from the netlink message
    struct nlattr *attrs[NL80211_ATTR_MAX + 1];
    // nlmsg_hdr() returns the actual netlink message casted to the type of the netlink message header
    // nlmsg_data() returns a pointer to the payload of the netlink message given the message header
    struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));

    // NL80211_ATTR_MAX: highest attribute number currently defined
    // This function iterates over the stream of attributes received via Netlink and stores a pointer to each
    // attribute in the index array specified as first argument (attrs) using the attribute type as index to the array
    // genlmsg_attrdata() returns a pointer to the message attributes, while genlmsg_attrlen() returns the length of message attributes
    // The second argument of both functions is the length of user headers, that are not present here
    // With genlmsg_attrdata(gnlh, 0) we get therefore the head of the attribute stream, and with
    // genlmsg_attrlen(gnlh, 0) the length of the attribute stream
    // The last argument is a possible policy for validating the attributes, that is not set here
    // Tip about nla_parse: attributes with a type greater than the maximum type specified will be silently
    // ignored in order to maintain backward compatibility
    nla_parse(attrs, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

    // Check if the MAC attribute is available; if not, no RSSI will be retrieved
    if (attrs[NL80211_ATTR_MAC]) {
        // Skip the RSSI retrieval if the RSSI is not the one of interest, passed as "macaddr"
        uint8_t *rssi_mac = (uint8_t *)nla_data(attrs[NL80211_ATTR_MAC]);

        char mac_str[18];
        char rssi_mac_str[18];

        // Convert the target MAC and the received MAC to strings, both with uppercase hex digits
        // (using uppercase hex in both MAC address is needed for the comparison)
        snprintf(mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                 nl_cb_args_ptr->macaddr[0],
                 nl_cb_args_ptr->macaddr[1],
                 nl_cb_args_ptr->macaddr[2],
                 nl_cb_args_ptr->macaddr[3],
                 nl_cb_args_ptr->macaddr[4],
                 nl_cb_args_ptr->macaddr[5]);

        snprintf(rssi_mac_str, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
                 rssi_mac[0],
                 rssi_mac[1],
                 rssi_mac[2],
                 rssi_mac[3],
                 rssi_mac[4],
                 rssi_mac[5]);

        if (strcmp(rssi_mac_str, mac_str) != 0) {
            // Skip the RSSI retrieval if the current MAC address is not the one of interest
            return NL_SKIP;
        }
    } else {
        // Skip the RSSI retrieval if the MAC address of the remote device cannot be retrieved with nl80211
        return NL_SKIP;
    }

    // Retrieve the signal level (RSSI), checking if the station information attribute is present
    if (attrs[NL80211_ATTR_STA_INFO]) {
        // Get the nested attributes using again nla_parse()
        // NL80211_STA_INFO_MAX: highest possible station information attribute
        struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];

        // Here we use nla_data to get a pointer to the actual data of the (nested) attribute stream
        // Since the data inside struct nlattr is stored after the attribute header, we use nla_data() to get a pointer to the actual data
        nla_parse(sinfo, NL80211_STA_INFO_MAX, (struct nlattr *)nla_data(attrs[NL80211_ATTR_STA_INFO]), nla_len(attrs[NL80211_ATTR_STA_INFO]), NULL);

        if (sinfo[NL80211_STA_INFO_SIGNAL]) {
            // We get here some bytes that represent an 8-bit signed integer: we therefore need to convert the result of nla_get_u8() to int8_t
            nl_cb_args_ptr->signal_lv = (int)((int8_t)nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL]));
        }
    }
    return NL_SKIP;
}

nl_sock_info_t open_nl_socket(const char *interface_name) {
    nl_sock_info_t nl_sock_info;

    // Allocate a new netlink socket (that should be freed up later)
    nl_sock_info.nl_sock = nl_socket_alloc();

    // If the socket allocation failed, do not perform any further step,
    // and return a structure with a flag indicating the socket is invalid and shall not be used
    if(!nl_sock_info.nl_sock) {
        fprintf(stderr, "[nl_utils warning] Failed to allocate netlink socket for RSSI retrieval!\n");
        nl_sock_info.sock_valid = false;
        return nl_sock_info;
    }

    // Connect to the generic netlink socket
    // Optionally, the socket buffer size can be set with:
    // nl_socket_set_buffer_size(nl_sock_info.nl_sock,<rx buf size>,<tx buf size>);
    // Here, however, it is not necessary
    if(genl_connect(nl_sock_info.nl_sock)) {
        fprintf(stderr, "[nl_utils warning] Failed to connect to generic netlink for RSSI retrieval!\n");
        nl_socket_free(nl_sock_info.nl_sock);
        nl_sock_info.sock_valid = false;
        return nl_sock_info;
    }

    // Resolve the family name "nl80211" to the corresponding kernel ID
    nl_sock_info.nl80211_id = genl_ctrl_resolve(nl_sock_info.nl_sock, "nl80211");
    if(nl_sock_info.nl80211_id < 0) {
        fprintf(stderr, "[nl_utils warning] No nl80211 interface found!\n");
        nl_socket_free(nl_sock_info.nl_sock);
        nl_sock_info.sock_valid = false;
        return nl_sock_info;
    }

    // Retrieve the interface index for the given network interface name
    nl_sock_info.ifindex = if_nametoindex(interface_name);
    if(nl_sock_info.ifindex == 0) {
        fprintf(stderr, "[nl_utils warning] Cannot get ifindex for interface %s. No RSSI retrieval will occur.\n", interface_name);
        nl_socket_free(nl_sock_info.nl_sock);
        nl_sock_info.sock_valid = false;
        return nl_sock_info;
    }

    // If we reach this point, everything went well with the creation of the Netlink socket
    nl_sock_info.sock_valid = true;
    return nl_sock_info;
}

void free_nl_socket(nl_sock_info_t *nl_sock_info) {
    // Free the Netlink socket, if a nl_sock_info_t structure pointing to a valid socket is passed
    if (nl_sock_info->sock_valid) {
        nl_socket_free(nl_sock_info->nl_sock);
        // Now the socket is no more valid and shall no more be used
        nl_sock_info->sock_valid = false;
    }
}

double get_rssi_from_netlink(uint8_t macaddr[6], nl_sock_info_t nl_sock_info) {
    struct nl_cb_args nl_cb_args;

    // Set the default RSSI value to "RSSI_UNAVAILABLE" and the macaddr field to the one of interest, passed as first argument
    nl_cb_args.signal_lv = RSSI_UNAVAILABLE;
    memcpy(nl_cb_args.macaddr, macaddr, 6);

    // Avoid doing any operation and leave the RSSI as "RSSI_UNAVAILABLE" if the Netlink socket passed as second argument
    // is not valid or could not be opened
    if(!nl_sock_info.sock_valid || nl_sock_info.ifindex == 0) {
        return (double)nl_cb_args.signal_lv;
    }

    // Allocate a new netlink message
    struct nl_msg *nl_msg = nlmsg_alloc();

    // If allocation is not successful, leave the RSSI as "RSSI_UNAVAILABLE" and do not perform any further operation
    // If it is successful, prepare a netlink message to request station information with genlmsg_put() and nla_put_u32()
    if(nl_msg) {
        // We set here:
        // 1. Pointer to the Netlink message buffer
        // 2.3. A port ID and sequence number that are managed automatically
        // 4. The Netlink family ID (nl80211), as we want to retrieve information about a wireless 802.11 device
        // 5. Length of user headers (there are no additional user headers here)
        // 6. Additional Netlink message flags (NLM_F_DUMP is used to request a list of all devices)
        // 7. The Netlink command: NL80211_CMD_GET_STATION gets station attributes for stations identified by
        //    NL80211_ATTR_MAC on the interface identified by NL80211_ATTR_IFINDEX; as we use here NLM_F_DUMP,
        //    in theory we do not need to specify a MAC and add the attribute NL80211_ATTR_MAC
        // 8. The interface version, set here to the default value of 0
        genlmsg_put(nl_msg, NL_AUTO_PORT, NL_AUTO_SEQ, nl_sock_info.nl80211_id, 0, NLM_F_DUMP, NL80211_CMD_GET_STATION, 0);
        // Add a 32-bit integer attribute to netlink message, i.e., the ifindex of the target interface, as required by NL80211_CMD_GET_STATION
        nla_put_u32(nl_msg, NL80211_ATTR_IFINDEX, nl_sock_info.ifindex);

        // Set a custom callback handler (NL_CB_CUSTOM) to process the response from the kernel, when a valid message is received (NL_CB_VALID)
        // We pass as arguments a pointer to the nl_cb_args structure (casted to void * as required by nl_socket_modify_cb())
        nl_socket_modify_cb(nl_sock_info.nl_sock, NL_CB_VALID, NL_CB_CUSTOM, nl_callback_handler, (void *)&nl_cb_args);

        // Finalize and send the netlink message to the kernel
        nl_send_auto_complete(nl_sock_info.nl_sock, nl_msg);

        // Receive the response from the ntlink socket using the callback handler in nl_sock, as previously set
        // As per official documentation, nl_recvmsgs_default() will call nl_recvmsgs()
        // nl_recvmsgs() repeatedly calls nl_recv() and parses the received data as netlink messages;
        // it stops reading if one of the callbacks returns NL_STOP or nl_recv() returns either 0 or a negative error code.
        nl_recvmsgs_default(nl_sock_info.nl_sock);

        // Free the previously allocated message
        nlmsg_free(nl_msg);
    }

    // If everything went well, nl_cb_args.signal_lv will contain the retrieve RSSI value for the device with MAC address "macaddr"
    return (double) nl_cb_args.signal_lv;
}