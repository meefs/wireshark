/* packet-zbee-nwk.c
 * Dissector routines for the ZigBee Network Layer (NWK)
 * By Owen Kirby <osk@exegin.com>
 * Copyright 2009 Exegin Technologies Limited
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*  Include Files */
#include "config.h"


#include <epan/packet.h>
#include <epan/exceptions.h>
#include <epan/prefs.h>
#include <epan/addr_resolv.h>
#include <epan/address_types.h>
#include <epan/expert.h>
#include <epan/proto_data.h>
#include <epan/conversation_table.h>
#include <epan/conversation_filter.h>
#include <epan/tap.h>
#include <wsutil/bits_ctz.h>    /* for ws_ctz */
#include <wsutil/pint.h>
#include "packet-ieee802154.h"
#include "packet-zbee.h"
#include "packet-zbee-nwk.h"
#include "packet-zbee-aps.h"    /* for ZBEE_APS_CMD_KEY_LENGTH */
#include "packet-zbee-zdp.h"
#include "packet-zbee-security.h"
#include "packet-zbee-tlv.h"

/*************************/
/* Function Declarations */
/*************************/
/* Dissector Routines */
static int         dissect_zbee_nwk        (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data);
static void        dissect_zbee_nwk_cmd    (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet* packet);
static int         dissect_zbee_beacon     (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data);
static int         dissect_zbip_beacon     (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data);
static int         dissect_zbee_ie         (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data);
static void        dissect_ieee802154_zigbee_rejoin(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, unsigned *offset);
static void        dissect_ieee802154_zigbee_txpower(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, unsigned *offset);

/* Command Dissector Helpers */
static unsigned    dissect_zbee_nwk_route_req  (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                                                zbee_nwk_packet * packet, unsigned offset);
static unsigned    dissect_zbee_nwk_route_rep  (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset, uint8_t version);
static unsigned    dissect_zbee_nwk_status     (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_leave      (tvbuff_t *tvb, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_route_rec  (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                                                zbee_nwk_packet * packet, unsigned offset);
static unsigned    dissect_zbee_nwk_rejoin_req (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                                                zbee_nwk_packet * packet, unsigned offset);
static unsigned    dissect_zbee_nwk_rejoin_resp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                                                zbee_nwk_packet * packet, unsigned offset);
static unsigned    dissect_zbee_nwk_link_status(tvbuff_t *tvb, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_report     (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_update     (tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_ed_timeout_request(tvbuff_t *tvb, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_ed_timeout_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_link_pwr_delta(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset);
static unsigned    dissect_zbee_nwk_commissioning_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                                                        zbee_nwk_packet * packet, unsigned offset);
static unsigned    dissect_zbee_nwk_commissioning_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                                                         zbee_nwk_packet * packet, unsigned offset);
static void        proto_init_zbee_nwk         (void);
static void        proto_cleanup_zbee_nwk(void);
void               proto_register_zbee_nwk(void);
void               proto_reg_handoff_zbee_nwk(void);

/********************/
/* Global Variables */
/********************/
static int proto_zbee_nwk;
static int proto_zbee_beacon;
static int proto_zbip_beacon;
static int proto_zbee_ie;
static int hf_zbee_nwk_fcf;
static int hf_zbee_nwk_frame_type;
static int hf_zbee_nwk_proto_version;
static int hf_zbee_nwk_discover_route;
static int hf_zbee_nwk_multicast;
static int hf_zbee_nwk_security;
static int hf_zbee_nwk_source_route;
static int hf_zbee_nwk_ext_dst;
static int hf_zbee_nwk_ext_src;
static int hf_zbee_nwk_end_device_initiator;
static int hf_zbee_nwk_dst;
static int hf_zbee_nwk_src;
static int hf_zbee_nwk_addr;
static int hf_zbee_nwk_radius;
static int hf_zbee_nwk_seqno;
static int hf_zbee_nwk_mcast;
static int hf_zbee_nwk_mcast_mode;
static int hf_zbee_nwk_mcast_radius;
static int hf_zbee_nwk_mcast_max_radius;
static int hf_zbee_nwk_dst64;
static int hf_zbee_nwk_src64;
static int hf_zbee_nwk_addr64;
static int hf_zbee_nwk_src64_origin;
static int hf_zbee_nwk_relay_count;
static int hf_zbee_nwk_relay_index;
static int hf_zbee_nwk_relay;

static int hf_zbee_nwk_cmd_id;
static int hf_zbee_nwk_cmd_addr;
static int hf_zbee_nwk_cmd_route_id;
static int hf_zbee_nwk_cmd_route_dest;
static int hf_zbee_nwk_cmd_route_orig;
static int hf_zbee_nwk_cmd_route_resp;
static int hf_zbee_nwk_cmd_route_dest_ext;
static int hf_zbee_nwk_cmd_route_orig_ext;
static int hf_zbee_nwk_cmd_route_resp_ext;
static int hf_zbee_nwk_cmd_route_cost;
static int hf_zbee_nwk_cmd_route_options;
static int hf_zbee_nwk_cmd_route_opt_repair;
static int hf_zbee_nwk_cmd_route_opt_multicast;
static int hf_zbee_nwk_cmd_route_opt_dest_ext;
static int hf_zbee_nwk_cmd_route_opt_resp_ext;
static int hf_zbee_nwk_cmd_route_opt_orig_ext;
static int hf_zbee_nwk_cmd_route_opt_many_to_one;
static int hf_zbee_nwk_cmd_nwk_status;
static int hf_zbee_nwk_cmd_nwk_status_command_id;
static int hf_zbee_nwk_cmd_leave_rejoin;
static int hf_zbee_nwk_cmd_leave_request;
static int hf_zbee_nwk_cmd_leave_children;
static int hf_zbee_nwk_cmd_relay_count;
static int hf_zbee_nwk_cmd_relay_device;
static int hf_zbee_nwk_cmd_cinfo;
static int hf_zbee_nwk_cmd_cinfo_alt_coord;
static int hf_zbee_nwk_cmd_cinfo_type;
static int hf_zbee_nwk_cmd_cinfo_power;
static int hf_zbee_nwk_cmd_cinfo_idle_rx;
static int hf_zbee_nwk_cmd_cinfo_security;
static int hf_zbee_nwk_cmd_cinfo_alloc;
static int hf_zbee_nwk_cmd_rejoin_status;
static int hf_zbee_nwk_cmd_link_last;
static int hf_zbee_nwk_cmd_link_first;
static int hf_zbee_nwk_cmd_link_count;
static int hf_zbee_nwk_cmd_link_address;
static int hf_zbee_nwk_cmd_link_incoming_cost;
static int hf_zbee_nwk_cmd_link_outgoing_cost;
static int hf_zbee_nwk_cmd_report_type;
static int hf_zbee_nwk_cmd_report_count;
static int hf_zbee_nwk_cmd_update_type;
static int hf_zbee_nwk_cmd_update_count;
static int hf_zbee_nwk_cmd_update_id;
static int hf_zbee_nwk_panid;
static int hf_zbee_zboss_nwk_cmd_key;
static int hf_zbee_nwk_cmd_epid;
static int hf_zbee_nwk_cmd_end_device_timeout_request_enum;
static int hf_zbee_nwk_cmd_end_device_configuration;
static int hf_zbee_nwk_cmd_end_device_timeout_resp_status;
static int hf_zbee_nwk_cmd_end_device_timeout_resp_parent_info;
static int hf_zbee_nwk_cmd_prnt_info_mac_data_poll_keepalive_supported;
static int hf_zbee_nwk_cmd_prnt_info_ed_to_req_keepalive_supported;
static int hf_zbee_nwk_cmd_prnt_info_power_negotiation_supported;
static int hf_zbee_nwk_cmd_link_pwr_list_count;
static int hf_zbee_nwk_cmd_link_pwr_type;
static int hf_zbee_nwk_cmd_link_pwr_device_address;
static int hf_zbee_nwk_cmd_link_pwr_power_delta;
static int hf_zbee_nwk_cmd_association_type;

/*  ZigBee Beacons */
static int hf_zbee_beacon_protocol;
static int hf_zbee_beacon_stack_profile;
static int hf_zbee_beacon_version;
static int hf_zbee_beacon_router_capacity;
static int hf_zbee_beacon_depth;
static int hf_zbee_beacon_end_device_capacity;
static int hf_zbee_beacon_epid;
static int hf_zbee_beacon_tx_offset;
static int hf_zbee_beacon_update_id;

static int hf_zbip_beacon_allow_join;
static int hf_zbip_beacon_router_capacity;
static int hf_zbip_beacon_host_capacity;
static int hf_zbip_beacon_unsecure;
static int hf_zbip_beacon_network_id;

/* IEEE 802.15.4 IEs (Information Elements) */
static int hf_ieee802154_zigbee_ie;
static int hf_ieee802154_zigbee_ie_id;
static int hf_ieee802154_zigbee_ie_length;
static int hf_ieee802154_zigbee_ie_tx_power;
static int hf_ieee802154_zigbee_ie_source_addr;

static int hf_ieee802154_zigbee_rejoin_epid;
static int hf_ieee802154_zigbee_rejoin_source_addr;

static int ett_zbee_nwk;
static int ett_zbee_nwk_beacon;
static int ett_zbee_nwk_fcf;
static int ett_zbee_nwk_fcf_ext;
static int ett_zbee_nwk_mcast;
static int ett_zbee_nwk_route;
static int ett_zbee_nwk_cmd;
static int ett_zbee_nwk_cmd_options;
static int ett_zbee_nwk_cmd_cinfo;
static int ett_zbee_nwk_cmd_link;
static int ett_zbee_nwk_cmd_ed_to_rsp_prnt_info;
static int ett_zbee_nwk_cmd_link_pwr_struct;
static int ett_zbee_nwk_zigbee_ie_fields;
static int ett_zbee_nwk_ie_rejoin;
static int ett_zbee_nwk_header;
static int ett_zbee_nwk_header_ie;
static int ett_zbee_nwk_beacon_bitfield;

static expert_field ei_zbee_nwk_missing_payload;

static dissector_handle_t   aps_handle;
static dissector_handle_t   zbee_gp_handle;

static int zbee_nwk_address_type = -1;

static int zbee_nwk_tap;

/* Cached protocol identifier */
static int proto_ieee802154;

/********************/
/* Field Names      */
/********************/
/* Frame Types */
static const value_string zbee_nwk_frame_types[] = {
    { ZBEE_NWK_FCF_DATA,    "Data" },
    { ZBEE_NWK_FCF_CMD,     "Command" },
    { ZBEE_NWK_FCF_INTERPAN,"Interpan" },
    { 0, NULL }
};

/* Route Discovery Modes */
static const value_string zbee_nwk_discovery_modes[] = {
    { ZBEE_NWK_FCF_DISCOVERY_SUPPRESS,  "Suppress" },
    { ZBEE_NWK_FCF_DISCOVERY_ENABLE,    "Enable" },
    { ZBEE_NWK_FCF_DISCOVERY_FORCE,     "Force" },
    { 0, NULL }
};

/* Command Names*/
static const value_string zbee_nwk_cmd_names[] = {
    { ZBEE_NWK_CMD_ROUTE_REQ,           "Route Request" },
    { ZBEE_NWK_CMD_ROUTE_REPLY,         "Route Reply" },
    { ZBEE_NWK_CMD_NWK_STATUS,          "Network Status" },
    { ZBEE_NWK_CMD_LEAVE,               "Leave" },
    { ZBEE_NWK_CMD_ROUTE_RECORD,        "Route Record" },
    { ZBEE_NWK_CMD_REJOIN_REQ,          "Rejoin Request" },
    { ZBEE_NWK_CMD_REJOIN_RESP,         "Rejoin Response" },
    { ZBEE_NWK_CMD_LINK_STATUS,         "Link Status" },
    { ZBEE_NWK_CMD_NWK_REPORT,          "Network Report" },
    { ZBEE_NWK_CMD_NWK_UPDATE,          "Network Update" },
    { ZBEE_NWK_CMD_ED_TIMEOUT_REQUEST,  "End Device Timeout Request" },
    { ZBEE_NWK_CMD_ED_TIMEOUT_RESPONSE, "End Device Timeout Response" },
    { ZBEE_NWK_CMD_LINK_PWR_DELTA,      "Link Power Delta" },
    { ZBEE_NWK_CMD_COMMISSIONING_REQUEST,  "Network Commissioning Request" },
    { ZBEE_NWK_CMD_COMMISSIONING_RESPONSE,  "Network Commissioning Response" },
    { 0, NULL }
};

/* Many-To-One Route Discovery Modes. */
static const value_string zbee_nwk_cmd_route_many_modes[] = {
    { ZBEE_NWK_CMD_ROUTE_OPTION_MANY_NONE,  "Not Many-to-One" },
    { ZBEE_NWK_CMD_ROUTE_OPTION_MANY_REC,   "With Source Routing" },
    { ZBEE_NWK_CMD_ROUTE_OPTION_MANY_NOREC, "Without Source Routing" },
    { 0, NULL }
};

/* Rejoin Status Codes */
static const value_string zbee_nwk_rejoin_codes[] = {
    { IEEE802154_CMD_ASRSP_AS_SUCCESS,      "Success" },
    { IEEE802154_CMD_ASRSP_PAN_FULL,        "PAN Full" },
    { IEEE802154_CMD_ASRSP_PAN_DENIED,      "PAN Access Denied" },
    { 0, NULL }
};

/* Network Report Types */
static const value_string zbee_nwk_report_types[] = {
    { ZBEE_NWK_CMD_NWK_REPORT_ID_PAN_CONFLICT,  "PAN Identifier Conflict" },
    { ZBEE_NWK_CMD_NWK_REPORT_ID_ZBOSS_KEY_TRACE,  "ZBOSS key trace" },
    { 0, NULL }
};

/* Network Update Types */
static const value_string zbee_nwk_update_types[] = {
    { ZBEE_NWK_CMD_NWK_UPDATE_ID_PAN_UPDATE,  "PAN Identifier Update" },
    { 0, NULL }
};

/* Network Status Codes */
static const value_string zbee_nwk_status_codes[] = {
    { ZBEE_NWK_STATUS_NO_ROUTE_AVAIL,       "No Route Available" },
    { ZBEE_NWK_STATUS_TREE_LINK_FAIL,       "Tree Link Failure" },
    { ZBEE_NWK_STATUS_NON_TREE_LINK_FAIL,   "Non-tree Link Failure" },
    { ZBEE_NWK_STATUS_LOW_BATTERY,          "Low Battery" },
    { ZBEE_NWK_STATUS_NO_ROUTING,           "No Routing Capacity" },
    { ZBEE_NWK_STATUS_NO_INDIRECT,          "No Indirect Capacity" },
    { ZBEE_NWK_STATUS_INDIRECT_EXPIRE,      "Indirect Transaction Expiry" },
    { ZBEE_NWK_STATUS_DEVICE_UNAVAIL,       "Target Device Unavailable" },
    { ZBEE_NWK_STATUS_ADDR_UNAVAIL,         "Target Address Unallocated" },
    { ZBEE_NWK_STATUS_PARENT_LINK_FAIL,     "Parent Link Failure" },
    { ZBEE_NWK_STATUS_VALIDATE_ROUTE,       "Validate Route" },
    { ZBEE_NWK_STATUS_SOURCE_ROUTE_FAIL,    "Source Route Failure" },
    { ZBEE_NWK_STATUS_MANY_TO_ONE_FAIL,     "Many-to-One Route Failure" },
    { ZBEE_NWK_STATUS_ADDRESS_CONFLICT,     "Address Conflict" },
    { ZBEE_NWK_STATUS_VERIFY_ADDRESS,       "Verify Address" },
    { ZBEE_NWK_STATUS_PANID_UPDATE,         "PAN ID Update" },
    { ZBEE_NWK_STATUS_ADDRESS_UPDATE,       "Network Address Update" },
    { ZBEE_NWK_STATUS_BAD_FRAME_COUNTER,    "Bad Frame Counter" },
    { ZBEE_NWK_STATUS_BAD_KEY_SEQNO,        "Bad Key Sequence Number" },
    { 0, NULL }
};

/* Stack Profile Values. */
static const value_string zbee_nwk_stack_profiles[] = {
    { 0x00, "Network Specific" },
    { 0x01, "ZigBee Home" },
    { 0x02, "ZigBee PRO" },
    { 0, NULL }
};

/* ED Requested Timeout Enumerated Values */
static const value_string zbee_nwk_end_device_timeout_request[] = {
    { 0, "10 sec" },
    { 1, "2 min" },
    { 2, "4 min" },
    { 3, "8 min" },
    { 4, "16 min" },
    { 5, "32 min" },
    { 6, "64 min" },
    { 7, "128 min" },
    { 8, "256 min" },
    { 9, "512 min" },
    { 10, "1024 min" },
    { 11, "2048 min" },
    { 12, "4096 min" },
    { 13, "8192 min" },
    { 14, "16384 min" },
    { 0, NULL }
};


/* End Device Timeout Response Status Codes */
static const value_string zbee_nwk_end_device_timeout_resp_status[] = {
    { 0,      "Success" },
    { 1,      "Incorrect value" },
    { 0, NULL }
};

/* Names of IEEE 802.15.4 IEs (Information Elements) for ZigBee */
static const value_string ieee802154_zigbee_ie_names[] = {
    { ZBEE_ZIGBEE_IE_REJOIN,                    "Rejoin"   },
    { ZBEE_ZIGBEE_IE_TX_POWER,                  "Tx Power" },
    { ZBEE_ZIGBEE_IE_BEACON_PAYLOAD,            "Extended Beacon Payload" },
    { 0, NULL }
};

/* Stack Profile Values. */
static const value_string zbee_nwk_link_power_delta_types[] = {
    { 0x00, "Notification" },
    { 0x01, "Request" },
    { 0x02, "Response" },
    { 0x03, "Reserved" },
    { 0, NULL }
};

static const value_string zbee_nwk_commissioning_types[] = {
    { 0x00, "Initial Join with Key Negotiation" },
    { 0x01, "Rejoin with Key Negotiation" },
    { 0, NULL }
};

/* TODO: much of the following copied from ieee80154 dissector */
/*-------------------------------------
 * Hash Tables and Lists
 *-------------------------------------
 */
ieee802154_map_tab_t zbee_nwk_map;
GHashTable *zbee_table_nwk_keyring;
GHashTable *zbee_table_link_keyring;

static int zbee_nwk_address_to_str(const address* addr, char *buf, int buf_len)
{
    uint16_t zbee_nwk_addr = pletoh16(addr->data);

    if ((zbee_nwk_addr == ZBEE_BCAST_ALL) || (zbee_nwk_addr == ZBEE_BCAST_ACTIVE) || (zbee_nwk_addr == ZBEE_BCAST_ROUTERS)) {
        return (int)g_strlcpy(buf, "Broadcast", buf_len) + 1;
    }
    else {
        return snprintf(buf, buf_len, "0x%04x", zbee_nwk_addr) + 1;
    }
}

static int zbee_nwk_address_str_len(const address* addr _U_)
{
    return sizeof("Broadcast");
}

static int zbee_nwk_address_len(void)
{
    return sizeof(uint16_t);
}

/**
 *Extracts an integer sub-field from an int with a given mask
 *
*/
unsigned
zbee_get_bit_field(unsigned input, unsigned mask)
{
    /* Sanity Check, don't want infinite loops. */
    if (mask == 0) return 0;
    /* Shift input and mask together. */
    while (!(mask & 0x1)) {
        input >>= 1;
        mask >>=1;
    } /* while */
    return (input & mask);
} /* zbee_get_bit_field */

/**
 *Heuristic interpreter for the ZigBee network dissectors.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to data tree Wireshark uses to display packet.
 *@return Boolean value, whether it handles the packet or not.
*/
static bool
dissect_zbee_nwk_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    ieee802154_packet   *packet = (ieee802154_packet *)data;
    uint16_t            fcf;
    unsigned            ver;
    unsigned            type;

    /* All ZigBee frames must always have a 16-bit source and destination address. */
    if (packet == NULL) return false;

    /* If the frame type and version are not sane, then it's probably not ZigBee. */
    fcf = tvb_get_letohs(tvb, 0);
    ver = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_VERSION);
    type = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_FRAME_TYPE);
    if ((ver < ZBEE_VERSION_2004) || (ver > ZBEE_VERSION_2007)) return false;
    if (!try_val_to_str(type, zbee_nwk_frame_types)) return false;

    /* All interpan frames should originate from an extended address. */
    if (type == ZBEE_NWK_FCF_INTERPAN) {
        if (packet->src_addr_mode != IEEE802154_FCF_ADDR_EXT) return false;
    }
    /* All other ZigBee frames must have 16-bit source and destination addresses. */
    else {
        if (packet->src_addr_mode != IEEE802154_FCF_ADDR_SHORT) return false;
        if (packet->dst_addr_mode != IEEE802154_FCF_ADDR_SHORT) return false;
    }

    /* Assume it's ZigBee */
    dissect_zbee_nwk(tvb, pinfo, tree, packet);
    return true;
} /* dissect_zbee_heur */

/**
 *ZigBee NWK packet dissection routine for 2006, 2007 and Pro stack versions.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields.
 *@param tree pointer to data tree Wireshark uses to display packet.
 *@param data raw packet private data.
*/

static int
dissect_zbee_nwk_full(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    tvbuff_t            *payload_tvb = NULL;

    proto_item          *proto_root;
    proto_item          *ti = NULL;
    proto_tree          *nwk_tree;

    zbee_nwk_packet     packet;
    ieee802154_packet   *ieee_packet;

    unsigned            offset = 0;
    char                *src_addr, *dst_addr;

    uint16_t            fcf;

    ieee802154_short_addr   addr16;
    ieee802154_map_rec     *map_rec;
    ieee802154_hints_t     *ieee_hints;

    zbee_nwk_hints_t       *nwk_hints;
    bool                    unicast_src;

    static int * const fcf_flags_2007[] = {
        &hf_zbee_nwk_frame_type,
        &hf_zbee_nwk_proto_version,
        &hf_zbee_nwk_discover_route,
        &hf_zbee_nwk_multicast,
        &hf_zbee_nwk_security,
        &hf_zbee_nwk_source_route,
        &hf_zbee_nwk_ext_dst,
        &hf_zbee_nwk_ext_src,
        &hf_zbee_nwk_end_device_initiator,
        NULL
    };

    static int * const fcf_flags[] = {
        &hf_zbee_nwk_frame_type,
        &hf_zbee_nwk_proto_version,
        &hf_zbee_nwk_discover_route,
        &hf_zbee_nwk_security,
        NULL
    };

    /* Reject the packet if data is NULL */
    if (data == NULL)
        return 0;
    ieee_packet = (ieee802154_packet *)data;

    memset(&packet, 0, sizeof(packet));

    /* Set up hint structures */
    if (!pinfo->fd->visited) {
        /* Allocate frame data with hints for upper layers */
        nwk_hints = wmem_new0(wmem_file_scope(), zbee_nwk_hints_t);
        p_add_proto_data(wmem_file_scope(), pinfo, proto_zbee_nwk, 0, nwk_hints);
    } else {
        /* Retrieve existing structure */
        nwk_hints = (zbee_nwk_hints_t *)p_get_proto_data(wmem_file_scope(), pinfo, proto_zbee_nwk, 0);
    }

    ieee_hints = (ieee802154_hints_t *)p_get_proto_data(wmem_file_scope(), pinfo, proto_ieee802154, 0);

    /* Add ourself to the protocol column, clear the info column, and create the protocol tree. */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "ZigBee");
    col_clear(pinfo->cinfo, COL_INFO);
    proto_root = proto_tree_add_item(tree, proto_zbee_nwk, tvb, offset, -1, ENC_NA);
    nwk_tree = proto_item_add_subtree(proto_root, ett_zbee_nwk);

    /* Get and parse the FCF */
    fcf = tvb_get_letohs(tvb, offset);
    packet.type         = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_FRAME_TYPE);
    packet.version      = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_VERSION);
    packet.discovery    = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_DISCOVER_ROUTE);
    packet.security     = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_SECURITY);
    packet.multicast    = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_MULTICAST);
    packet.route        = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_SOURCE_ROUTE);
    packet.ext_dst      = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_EXT_DEST);
    packet.ext_src      = zbee_get_bit_field(fcf, ZBEE_NWK_FCF_EXT_SOURCE);

    /* Display the FCF. */
    if (packet.version >= ZBEE_VERSION_2007) {
        ti = proto_tree_add_bitmask(nwk_tree, tvb, offset, hf_zbee_nwk_fcf, ett_zbee_nwk_fcf, fcf_flags_2007, ENC_LITTLE_ENDIAN);
    } else {
        ti = proto_tree_add_bitmask(nwk_tree, tvb, offset, hf_zbee_nwk_fcf, ett_zbee_nwk_fcf, fcf_flags, ENC_LITTLE_ENDIAN);
    }
    proto_item_append_text(ti, " %s", val_to_str_const(packet.type, zbee_nwk_frame_types, "Unknown"));
    offset += 2;

    /* Add the frame type to the info column and protocol root. */
    proto_item_append_text(proto_root, " %s", val_to_str_const(packet.type, zbee_nwk_frame_types, "Unknown Type"));
    col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(packet.type, zbee_nwk_frame_types, "Reserved Frame Type"));

    if (packet.type != ZBEE_NWK_FCF_INTERPAN) {
        /* Get the destination address. */
        packet.dst = tvb_get_letohs(tvb, offset);

        set_address_tvb(&pinfo->net_dst, zbee_nwk_address_type, 2, tvb, offset);
        copy_address_shallow(&pinfo->dst, &pinfo->net_dst);
        dst_addr = address_to_str(pinfo->pool, &pinfo->dst);

        proto_tree_add_uint(nwk_tree, hf_zbee_nwk_dst, tvb, offset, 2, packet.dst);
        ti = proto_tree_add_uint(nwk_tree, hf_zbee_nwk_addr, tvb, offset, 2, packet.dst);
        proto_item_set_generated(ti);
        proto_item_set_hidden(ti);
        offset += 2;

        proto_item_append_text(proto_root, ", Dst: %s", dst_addr);
        col_append_fstr(pinfo->cinfo, COL_INFO, ", Dst: %s", dst_addr);

        /* Get the short nwk source address and pass it to upper layers */
        packet.src = tvb_get_letohs(tvb, offset);

        set_address_tvb(&pinfo->net_src, zbee_nwk_address_type, 2, tvb, offset);
        copy_address_shallow(&pinfo->src, &pinfo->net_src);
        src_addr = address_to_str(pinfo->pool, &pinfo->src);

        if (nwk_hints)
            nwk_hints->src = packet.src;
        proto_tree_add_uint(nwk_tree, hf_zbee_nwk_src, tvb, offset, 2, packet.src);
        ti = proto_tree_add_uint(nwk_tree, hf_zbee_nwk_addr, tvb, offset, 2, packet.src);
        proto_item_set_generated(ti);
        proto_item_set_hidden(ti);
        offset += 2;

        if (   (packet.src == ZBEE_BCAST_ALL)
               || (packet.src == ZBEE_BCAST_ACTIVE)
               || (packet.src == ZBEE_BCAST_ROUTERS)){
            /* Source Broadcast doesn't make much sense. */
            unicast_src = false;
        }
        else {
            unicast_src = true;
        }

        proto_item_append_text(proto_root, ", Src: %s", src_addr);
        col_append_fstr(pinfo->cinfo, COL_INFO, ", Src: %s", src_addr);

        /* Get and display the radius. */
        packet.radius = tvb_get_uint8(tvb, offset);
        proto_tree_add_uint(nwk_tree, hf_zbee_nwk_radius, tvb, offset, 1, packet.radius);
        offset += 1;

        /* Get and display the sequence number. */
        packet.seqno = tvb_get_uint8(tvb, offset);
        proto_tree_add_uint(nwk_tree, hf_zbee_nwk_seqno, tvb, offset, 1, packet.seqno);
        offset += 1;

        /* Add the extended destination address (ZigBee 2006 and later). */
        if ((packet.version >= ZBEE_VERSION_2007) && packet.ext_dst) {
            packet.dst64 = tvb_get_letoh64(tvb, offset);
            proto_tree_add_item(nwk_tree, hf_zbee_nwk_dst64, tvb, offset, 8, ENC_LITTLE_ENDIAN);
            ti = proto_tree_add_eui64(nwk_tree, hf_zbee_nwk_addr64, tvb, offset, 8, packet.dst64);
            proto_item_set_generated(ti);
            proto_item_set_hidden(ti);
            offset += 8;
        }

        /* Display the extended source address. (ZigBee 2006 and later). */
        if (packet.version >= ZBEE_VERSION_2007) {
            addr16.pan = ieee_packet->src_pan;

            if (packet.ext_src) {
                packet.src64 = tvb_get_letoh64(tvb, offset);
                proto_tree_add_item(nwk_tree, hf_zbee_nwk_src64, tvb, offset, 8, ENC_LITTLE_ENDIAN);
                ti = proto_tree_add_eui64(nwk_tree, hf_zbee_nwk_addr64, tvb, offset, 8, packet.src64);
                proto_item_set_generated(ti);
                proto_item_set_hidden(ti);
                offset += 8;

                if (!pinfo->fd->visited && nwk_hints) {
                    /* Provide hints to upper layers */
                    nwk_hints->src_pan = ieee_packet->src_pan;

                    /* Update nwk extended address hash table */
                    if ( unicast_src ) {
                        nwk_hints->map_rec = ieee802154_addr_update(&zbee_nwk_map,
                                                                    packet.src, addr16.pan, packet.src64, pinfo->current_proto, pinfo->num);
                    }
                }
            }
            else {
                /* See if extended source info was previously sniffed */
                if (!pinfo->fd->visited && nwk_hints) {
                    nwk_hints->src_pan = ieee_packet->src_pan;
                    addr16.addr = packet.src;

                    map_rec = (ieee802154_map_rec *) g_hash_table_lookup(zbee_nwk_map.short_table, &addr16);
                    if (map_rec) {
                        /* found a nwk mapping record */
                        nwk_hints->map_rec = map_rec;
                    }
                    else {
                        /* does ieee layer know? */
                        map_rec = (ieee802154_map_rec *) g_hash_table_lookup(ieee_packet->short_table, &addr16);
                        if (map_rec) nwk_hints->map_rec = map_rec;
                    }
                } /* (!pinfo->fd->visited) */
                else {
                    if (nwk_hints && nwk_hints->map_rec ) {
                        /* Display inferred source address info */
                        ti = proto_tree_add_eui64(nwk_tree, hf_zbee_nwk_src64, tvb, offset, 0,
                                                  nwk_hints->map_rec->addr64);
                        proto_item_set_generated(ti);
                        ti = proto_tree_add_eui64(nwk_tree, hf_zbee_nwk_addr64, tvb, offset, 0, nwk_hints->map_rec->addr64);
                        proto_item_set_generated(ti);
                        proto_item_set_hidden(ti);

                        if ( nwk_hints->map_rec->start_fnum ) {
                            ti = proto_tree_add_uint(nwk_tree, hf_zbee_nwk_src64_origin, tvb, 0, 0,
                                                     nwk_hints->map_rec->start_fnum);
                        }
                        else {
                            ti = proto_tree_add_uint_format_value(nwk_tree, hf_zbee_nwk_src64_origin, tvb, 0, 0, 0, "Pre-configured");
                        }
                        proto_item_set_generated(ti);
                    }
                }
            }

            /* If ieee layer didn't know its extended source address, and nwk layer does, fill it in */
            if (!pinfo->fd->visited) {
                if ( (ieee_packet->src_addr_mode == IEEE802154_FCF_ADDR_SHORT) &&
                     ieee_hints && !ieee_hints->map_rec ) {
                    addr16.pan = ieee_packet->src_pan;
                    addr16.addr = ieee_packet->src16;
                    map_rec = (ieee802154_map_rec *) g_hash_table_lookup(zbee_nwk_map.short_table, &addr16);

                    if (map_rec) {
                        /* found a ieee mapping record */
                        ieee_hints->map_rec = map_rec;
                    }
                }
            } /* (!pinfo->fd->visited */
        } /* (pinfo->zbee_stack_vers >= ZBEE_VERSION_2007) */

        /* Add multicast control field (ZigBee 2006 and later). */
        if ((packet.version >= ZBEE_VERSION_2007) && packet.multicast) {
            static int * const multicast_flags[] = {
                &hf_zbee_nwk_mcast_mode,
                &hf_zbee_nwk_mcast_radius,
                &hf_zbee_nwk_mcast_max_radius,
                NULL
            };

            uint8_t mcast_control = tvb_get_uint8(tvb, offset);
            packet.mcast_mode = zbee_get_bit_field(mcast_control, ZBEE_NWK_MCAST_MODE);
            packet.mcast_radius = zbee_get_bit_field(mcast_control, ZBEE_NWK_MCAST_RADIUS);
            packet.mcast_max_radius = zbee_get_bit_field(mcast_control, ZBEE_NWK_MCAST_MAX_RADIUS);

            proto_tree_add_bitmask(nwk_tree, tvb, offset, hf_zbee_nwk_mcast, ett_zbee_nwk_mcast, multicast_flags, ENC_NA);
            offset += 1;
        }

        /* Add the Source Route field. (ZigBee 2006 and later). */
        if ((packet.version >= ZBEE_VERSION_2007) && packet.route) {
            proto_tree *field_tree;
            uint8_t     relay_count;
            uint16_t    relay_addr;
            unsigned    i;

            /* Create a subtree for the source route field. */
            field_tree = proto_tree_add_subtree(nwk_tree, tvb, offset, 1, ett_zbee_nwk_route, &ti, "Source Route");

            /* Get and display the relay count. */
            relay_count = tvb_get_uint8(tvb, offset);
            proto_tree_add_uint(field_tree, hf_zbee_nwk_relay_count, tvb, offset, 1, relay_count);
            proto_item_append_text(ti, ", Length: %d", relay_count);
            offset += 1;

            /* Correct the length of the source route fields. */
            proto_item_set_len(ti, 1 + relay_count*2);

            /* Get and display the relay index. */
            proto_tree_add_item(field_tree, hf_zbee_nwk_relay_index, tvb, offset, 1, ENC_NA);
            offset += 1;

            /* Get and display the relay list. */
            for (i=0; i<relay_count; i++) {
                relay_addr = tvb_get_letohs(tvb, offset);
                proto_tree_add_uint_format(field_tree, hf_zbee_nwk_relay, tvb, offset, 2, relay_addr, "Relay %d: 0x%04x", i+1, relay_addr);
                offset += 2;
            } /* for */
        }
    } /* if not interpan */

    /*
     * Ensure that the payload exists. There are no valid ZigBee network
     * packets that have no payload.
     */
    if (offset >= tvb_captured_length(tvb)) {
        /* Non-existent or truncated payload. */
        expert_add_info(pinfo, proto_root, &ei_zbee_nwk_missing_payload);
        return tvb_captured_length(tvb);
    }
    /* Payload is encrypted, attempt security operations. */
    else if (packet.security) {
        payload_tvb = dissect_zbee_secure(tvb, pinfo, nwk_tree, offset);
        if (payload_tvb == NULL) {
            /* If Payload_tvb is NULL, then the security dissector cleaned up. */
            return tvb_captured_length(tvb);
        }
    }
    /* Plaintext payload. */
    else {
        payload_tvb = tvb_new_subset_remaining(tvb, offset);
    }

    if (packet.type == ZBEE_NWK_FCF_CMD) {
        /* Dissect the Network Command. */
        dissect_zbee_nwk_cmd(payload_tvb, pinfo, nwk_tree, &packet);
    }
    else if (packet.type == ZBEE_NWK_FCF_DATA || packet.type == ZBEE_NWK_FCF_INTERPAN) {
        /* Dissect the Network Payload (APS layer). */
        call_dissector_with_data(aps_handle, payload_tvb, pinfo, tree, &packet);
    }
    else {
        /* Invalid type. */
        call_data_dissector(payload_tvb, pinfo, tree);
    }

    tap_queue_packet(zbee_nwk_tap, pinfo, NULL);

    return tvb_captured_length(tvb);
} /* dissect_zbee_nwk */

/**
 *ZigBee packet dissection with proto version determination.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields.
 *@param tree pointer to data tree Wireshark uses to display packet.
 *@param data raw packet private data.
*/
static int
dissect_zbee_nwk(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data)
{
    uint8_t fcf0;
    uint8_t proto_version;

    fcf0 = tvb_get_uint8(tvb, 0);
    proto_version = (fcf0 & ZBEE_NWK_FCF_VERSION) >> 2;
    if (proto_version == ZBEE_VERSION_GREEN_POWER) {
        call_dissector_with_data(zbee_gp_handle, tvb, pinfo, tree, data);
    } else {
        /* TODO: add check for FCF proto versions. */
        dissect_zbee_nwk_full(tvb, pinfo, tree, data);
    }
    return tvb_captured_length(tvb);
}

/**
 *ZigBee Network command packet dissection routine for Wireshark.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to data tree Wireshark uses to display packet.
*/
static void dissect_zbee_nwk_cmd(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet* packet)
{
    proto_tree  *cmd_tree;
    proto_item  *cmd_root;

    unsigned    offset=0;
    uint8_t     cmd_id = tvb_get_uint8(tvb, offset);

    /* Create a subtree for this command. */
    cmd_tree = proto_tree_add_subtree_format(tree, tvb, offset, -1, ett_zbee_nwk_cmd, &cmd_root, "Command Frame: %s",
                                    val_to_str_const(cmd_id, zbee_nwk_cmd_names, "Unknown"));

    /* Add the command ID. */
    proto_tree_add_uint(cmd_tree, hf_zbee_nwk_cmd_id, tvb, offset, 1, cmd_id);
    offset += 1;

    /* Add the command name to the info column. */
    col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(cmd_id, zbee_nwk_cmd_names, "Unknown Command"));


    /* Handle the command. */
    switch(cmd_id){
        case ZBEE_NWK_CMD_ROUTE_REQ:
            /* Route Request Command. */
            offset = dissect_zbee_nwk_route_req(tvb, pinfo, cmd_tree, packet, offset);
            break;

        case ZBEE_NWK_CMD_ROUTE_REPLY:
            /* Route Reply Command. */
            offset = dissect_zbee_nwk_route_rep(tvb, pinfo, cmd_tree, offset, packet->version);
            break;

        case ZBEE_NWK_CMD_NWK_STATUS:
            /* Network Status Command. */
            offset = dissect_zbee_nwk_status(tvb, pinfo, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_LEAVE:
            /* Leave Command. */
            offset = dissect_zbee_nwk_leave(tvb, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_ROUTE_RECORD:
            /* Route Record Command. */
            offset = dissect_zbee_nwk_route_rec(tvb, pinfo, cmd_tree, packet, offset);
            break;

        case ZBEE_NWK_CMD_REJOIN_REQ:
            /* Rejoin Request Command. */
            offset = dissect_zbee_nwk_rejoin_req(tvb, pinfo, cmd_tree, packet, offset);
            break;

        case ZBEE_NWK_CMD_REJOIN_RESP:
            /* Rejoin Response Command. */
            offset = dissect_zbee_nwk_rejoin_resp(tvb, pinfo, cmd_tree, packet, offset);
            break;

        case ZBEE_NWK_CMD_LINK_STATUS:
            /* Link Status Command. */
            offset = dissect_zbee_nwk_link_status(tvb, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_NWK_REPORT:
            /* Network Report Command. */
            offset = dissect_zbee_nwk_report(tvb, pinfo, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_NWK_UPDATE:
            /* Network Update Command. */
            offset = dissect_zbee_nwk_update(tvb, pinfo, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_ED_TIMEOUT_REQUEST:
            /* Network End Device Timeout Request Command. */
            offset = dissect_zbee_nwk_ed_timeout_request(tvb, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_ED_TIMEOUT_RESPONSE:
            /* Network End Device Timeout Response Command. */
            offset = dissect_zbee_nwk_ed_timeout_response(tvb, pinfo, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_LINK_PWR_DELTA:
            offset = dissect_zbee_nwk_link_pwr_delta(tvb, pinfo, cmd_tree, offset);
            break;

        case ZBEE_NWK_CMD_COMMISSIONING_REQUEST:
            /* Network Commissioning Request Command. */
            offset = dissect_zbee_nwk_commissioning_request(tvb, pinfo, cmd_tree, packet, offset);
            break;

        case ZBEE_NWK_CMD_COMMISSIONING_RESPONSE:
            /* Network Commissioning Response Command. */
            offset = dissect_zbee_nwk_commissioning_response(tvb, pinfo, cmd_tree, packet, offset);
            break;

        default:
            /* Just break out and let the overflow handler deal with the payload. */
            break;
    } /* switch */

    /* Dissect any TLVs */
    offset = dissect_zbee_tlvs(tvb, pinfo, tree, offset, NULL, ZBEE_TLV_SRC_TYPE_ZBEE_NWK, cmd_id);

    /* There is excess data in the packet. */
    if (offset < tvb_captured_length(tvb)) {
        /* There are leftover bytes! */
        tvbuff_t    *leftover_tvb   = tvb_new_subset_remaining(tvb, offset);
        proto_tree  *root;

        /* Correct the length of the command tree. */
        root = proto_tree_get_root(tree);
        proto_item_set_len(cmd_root, offset);

        /* Dump the leftover to the data dissector. */
        call_data_dissector(leftover_tvb, pinfo, root);
    }
} /* dissect_zbee_nwk_cmd */

/**
 *Helper dissector for the Route Request command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param packet pointer to the network packet struct.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_route_req(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet * packet, unsigned offset)
{
    uint8_t route_options;
    uint16_t dest_addr;

    static int * const nwk_route_command_options_2007[] = {
        &hf_zbee_nwk_cmd_route_opt_multicast,
        &hf_zbee_nwk_cmd_route_opt_dest_ext,
        &hf_zbee_nwk_cmd_route_opt_many_to_one,
        NULL
    };

    static int * const nwk_route_command_options[] = {
        &hf_zbee_nwk_cmd_route_opt_repair,
        NULL
    };

    /* Get and display the route options field. */
    route_options = tvb_get_uint8(tvb, offset);
    if (packet->version >= ZBEE_VERSION_2007) {
        proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_route_options, ett_zbee_nwk_cmd_options, nwk_route_command_options_2007, ENC_NA);
    } else {
        proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_route_options, ett_zbee_nwk_cmd_options, nwk_route_command_options, ENC_NA);
    }
    offset += 1;

    /* Get and display the route request ID. */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_id, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* Get and display the destination address. */
    dest_addr = tvb_get_letohs(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_route_dest, tvb, offset, 2, dest_addr);
    offset += 2;

    /* Get and display the path cost. */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_cost, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* Get and display the extended destination address. */
    if (route_options & ZBEE_NWK_CMD_ROUTE_OPTION_DEST_EXT) {
        proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_dest_ext, tvb, offset, 8, ENC_LITTLE_ENDIAN);
        offset += 8;
    }

    /* Update the info column. */
    if (route_options & ZBEE_NWK_CMD_ROUTE_OPTION_MANY_MASK) {
        col_clear(pinfo->cinfo, COL_INFO);
        col_append_str(pinfo->cinfo, COL_INFO, "Many-to-One Route Request");
    }
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Dst: 0x%04x, Src: 0x%04x", dest_addr, packet->src);

    /* Done */
    return offset;
} /* dissect_zbee_nwk_route_req */

/**
 *Helper dissector for the Route Reply command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_route_rep(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset, uint8_t version)
{
    uint8_t route_options;
    uint16_t orig_addr;
    uint16_t resp_addr;

    static int * const nwk_route_command_options_2007[] = {
        &hf_zbee_nwk_cmd_route_opt_multicast,
        &hf_zbee_nwk_cmd_route_opt_resp_ext,
        &hf_zbee_nwk_cmd_route_opt_orig_ext,
        NULL
    };

    static int * const nwk_route_command_options[] = {
        &hf_zbee_nwk_cmd_route_opt_repair,
        NULL
    };

    /* Get and display the route options field. */
    route_options = tvb_get_uint8(tvb, offset);
    if (version >= ZBEE_VERSION_2007) {
        proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_route_options, ett_zbee_nwk_cmd_options, nwk_route_command_options_2007, ENC_NA);
    } else {
        proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_route_options, ett_zbee_nwk_cmd_options, nwk_route_command_options, ENC_NA);
    }
    offset += 1;

    /* Get and display the route request ID. */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_id, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* Get and display the originator address. */
    orig_addr = tvb_get_letohs(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_route_orig, tvb, offset, 2, orig_addr);
    offset += 2;

    /* Get and display the responder address. */
    resp_addr = tvb_get_letohs(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_route_resp, tvb, offset, 2, resp_addr);
    offset += 2;

    /* Get and display the path cost. */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_cost, tvb, offset, 1, ENC_NA);
    offset += 1;

    /* Get and display the originator extended address. */
    if (route_options & ZBEE_NWK_CMD_ROUTE_OPTION_ORIG_EXT) {
        proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_orig_ext, tvb, offset, 8, ENC_LITTLE_ENDIAN);
        offset += 8;
    }

    /* Get and display the responder extended address. */
    if (route_options & ZBEE_NWK_CMD_ROUTE_OPTION_RESP_EXT) {
        proto_tree_add_item(tree, hf_zbee_nwk_cmd_route_resp_ext, tvb, offset, 8, ENC_LITTLE_ENDIAN);
        offset += 8;
    }

    /* Update the info column. */
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Responder: 0x%04x, Originator: 0x%04x", resp_addr, orig_addr);

    /* Done */
    return offset;
} /* dissect_zbee_nwk_route_rep */

/**
 *Helper dissector for the Network Status command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_status(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset)
{
    uint8_t status_code;
    uint8_t command_id;
    uint16_t addr;

    /* Get and display the status code. */
    status_code = tvb_get_uint8(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_nwk_status, tvb, offset, 1, status_code);
    offset += 1;

    /* Get and display the destination address. */
    addr = tvb_get_letohs(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_route_dest, tvb, offset, 2, addr);
    offset += 2;

    /* Update the info column. */
    col_append_fstr(pinfo->cinfo, COL_INFO, ", 0x%04x: %s", addr, val_to_str_const(status_code, zbee_nwk_status_codes, "Unknown Status Code"));

    if (status_code == ZBEE_NWK_STATUS_UNKNOWN_COMMAND) {
        command_id = tvb_get_uint8(tvb, offset);
        proto_tree_add_uint(tree, hf_zbee_nwk_cmd_nwk_status_command_id, tvb, offset, 1, command_id);
        col_append_fstr(pinfo->cinfo, COL_INFO, ", Unknown Command ID 0x%02x (%s)", command_id,
                val_to_str_const(command_id, zbee_nwk_cmd_names, "Unknown ID"));
        offset++;
    }

    /* Done */
    return offset;
} /* dissect_zbee_nwk_status */

/**
 *Helper dissector for the Leave command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_leave(tvbuff_t *tvb, proto_tree *tree, unsigned offset)
{
    static int * const leave_options[] = {
        &hf_zbee_nwk_cmd_leave_rejoin,
        &hf_zbee_nwk_cmd_leave_request,
        &hf_zbee_nwk_cmd_leave_children,
        NULL
    };

    /* Get and display the leave options. */
    proto_tree_add_bitmask_list(tree, tvb, offset, 1, leave_options, ENC_NA);
    offset += 1;

    /* Done */
    return offset;
} /* dissect_zbee_nwk_leave */

/**
 *Helper dissector for the Route Record command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param packet pointer to the network packet struct.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_route_rec(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet * packet, unsigned offset)
{
    uint8_t relay_count;
    uint16_t relay_addr;
    unsigned   i;

    /* Get and display the relay count. */
    relay_count = tvb_get_uint8(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_relay_count, tvb, offset, 1, relay_count);
    offset += 1;

    /* Get and display the relay addresses. */
    for (i=0; i<relay_count; i++) {
        relay_addr = tvb_get_letohs(tvb, offset);
        proto_tree_add_uint_format(tree, hf_zbee_nwk_cmd_relay_device, tvb, offset, 2, relay_addr,
                                   "Relay Device %d: 0x%04x", i+1, relay_addr);
        offset += 2;
    } /* for */

    /* Update the info column. */
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Dst: 0x%04x", packet->dst);


    /* Done */
    return offset;
} /* dissect_zbee_nwk_route_rec */

/**
 *Helper dissector for the Rejoin Request command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param packet pointer to the network packet struct.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_rejoin_req(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet * packet, unsigned offset)
{
    static int * const capabilities[] = {
        &hf_zbee_nwk_cmd_cinfo_alt_coord,
        &hf_zbee_nwk_cmd_cinfo_type,
        &hf_zbee_nwk_cmd_cinfo_power,
        &hf_zbee_nwk_cmd_cinfo_idle_rx,
        &hf_zbee_nwk_cmd_cinfo_security,
        &hf_zbee_nwk_cmd_cinfo_alloc,
        NULL
    };

    proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_cinfo, ett_zbee_nwk_cmd_cinfo, capabilities, ENC_NA);
    offset += 1;

    /* Update the info column.*/
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Device: 0x%04x", packet->src);

    /* Done */
    return offset;
} /* dissect_zbee_nwk_rejoin_req */

/**
 *Helper dissector for the Rejoin Response command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param packet pointer to the network packet struct.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_rejoin_resp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet * packet _U_, unsigned offset)
{
    uint8_t status;
    uint16_t new_address;

    /* Get and display the short address. */
    new_address = tvb_get_uint16(tvb, offset, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_addr, tvb, offset, 2, ENC_LITTLE_ENDIAN);
    offset += 2;

    /* Get and display the rejoin status. */
    status = tvb_get_uint8(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_rejoin_status, tvb, offset, 1, status);
    offset += 1;

    /* Update the info column. */
    if (status == IEEE802154_CMD_ASRSP_AS_SUCCESS) {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", New Address: 0x%04x", new_address);
    }
    else {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", %s", val_to_str_const(status, zbee_nwk_rejoin_codes, "Unknown Rejoin Response"));
    }

    /* Done */
    return offset;
} /* dissect_zbee_nwk_rejoin_resp */

/**
 *Helper dissector for the Link Status command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_link_status(tvbuff_t *tvb, proto_tree *tree, unsigned offset)
{
    uint8_t options;
    int     i, link_count;
    proto_tree *subtree;
    static int * const link_options[] = {
        &hf_zbee_nwk_cmd_link_last,
        &hf_zbee_nwk_cmd_link_first,
        &hf_zbee_nwk_cmd_link_count,
        NULL
    };

    /* Get and Display the link status options. */
    options = tvb_get_uint8(tvb, offset);
    link_count = options & ZBEE_NWK_CMD_LINK_OPTION_COUNT_MASK;
    proto_tree_add_bitmask_list(tree, tvb, offset, 1, link_options, ENC_NA);
    offset += 1;

    /* Get and Display the link status list. */
    for (i=0; i<link_count; i++) {
        /* Get the address and link status. */
        subtree = proto_tree_add_subtree_format(tree, tvb, offset, 3, ett_zbee_nwk_cmd_link, NULL, "Link %d", i+1);
        proto_tree_add_item(subtree, hf_zbee_nwk_cmd_link_address, tvb, offset, 2, ENC_LITTLE_ENDIAN);
        proto_tree_add_item(subtree, hf_zbee_nwk_cmd_link_incoming_cost, tvb, offset+2, 1, ENC_NA);
        proto_tree_add_item(subtree, hf_zbee_nwk_cmd_link_outgoing_cost, tvb, offset+2, 1, ENC_NA);
        offset += (2+1);
    } /* for */

    /* TODO: Update the info column. */
    return offset;
} /* dissect_zbee_nwk_link_status */

/**
 *Helper dissector for the End Device Timeout Request command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_ed_timeout_request(tvbuff_t *tvb, proto_tree *tree, unsigned offset)
{
    /* See 3.4.11 End Device Timeout Request Command */

    /* 3.4.11.3.1 Requested Timeout Field */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_end_device_timeout_request_enum, tvb, offset, 1, ENC_NA);
    offset++;

    /* 3.4.11.3.2 End Device Configuration Field */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_end_device_configuration, tvb, offset, 1, ENC_NA);
    offset++;

    return offset;
} /* dissect_zbee_nwk_ed_timeout_request */

/**
 *Helper dissector for the End Device Timeout Response command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_ed_timeout_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset)
{
    static int * const end_device_parent_info[] = {
        &hf_zbee_nwk_cmd_prnt_info_mac_data_poll_keepalive_supported,
        &hf_zbee_nwk_cmd_prnt_info_ed_to_req_keepalive_supported,
        &hf_zbee_nwk_cmd_prnt_info_power_negotiation_supported,
        NULL
    };
    unsigned status = tvb_get_uint8(tvb, offset);
    /* 3.4.12 End Device Timeout Response Command */

    /* status */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_end_device_timeout_resp_status, tvb, offset, 1, ENC_NA);
    offset++;

    /* Parent Information bitmask */
    proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_end_device_timeout_resp_parent_info, ett_zbee_nwk_cmd_ed_to_rsp_prnt_info, end_device_parent_info, ENC_NA);
    offset++;

    proto_item_append_text(tree, ", %s", val_to_str_const(status, zbee_nwk_end_device_timeout_resp_status, "Unknown Status"));
    col_append_fstr(pinfo->cinfo, COL_INFO, ", %s", val_to_str_const(status, zbee_nwk_end_device_timeout_resp_status, "Unknown Status"));

    return offset;
} /* dissect_zbee_nwk_ed_timeout_response */

/**
 *Helper dissector for the Link Power Delta command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_link_pwr_delta(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, unsigned offset)
{
    int     i;
    int     count;
    int     delta;
    uint8_t type;
    uint16_t addr;
    proto_tree *subtree;

    type = tvb_get_uint8(tvb, offset) & ZBEE_NWK_CMD_NWK_LINK_PWR_DELTA_TYPE_MASK;
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_link_pwr_type, tvb, offset, 1, ENC_NA);
    offset++;

    count = tvb_get_uint8(tvb, offset);
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_link_pwr_list_count, tvb, offset, 1, ENC_NA);
    offset++;

    proto_item_append_text(tree, ": %s, Count %d", val_to_str_const(type, zbee_nwk_link_power_delta_types, "Unknown"), count);

    for (i=0; i<count; i++) {
        subtree = proto_tree_add_subtree(tree, tvb, count, 3, ett_zbee_nwk_cmd_link_pwr_struct, NULL, "Power Delta Structure");
        addr = tvb_get_uint16(tvb, offset, ENC_LITTLE_ENDIAN);
        proto_tree_add_item(subtree, hf_zbee_nwk_cmd_link_pwr_device_address, tvb, offset, 2, ENC_LITTLE_ENDIAN);
        offset += 2;
        delta = (char)tvb_get_uint8(tvb, offset);
        proto_tree_add_item(subtree, hf_zbee_nwk_cmd_link_pwr_power_delta, tvb, offset, 1, ENC_NA);
        offset++;
        proto_item_append_text(subtree, ": Device Address 0x%04X, Power Delta %d dBm", addr, delta);
    }
    return offset;
}

/**
 *Helper dissector for the Network Commissioning Request command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
 */
static unsigned
dissect_zbee_nwk_commissioning_request(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet * packet, unsigned offset)
{
    /* See 3.4.14 Network Commissioning Request Command */

    static int * const capabilities[] = {
        &hf_zbee_nwk_cmd_cinfo_alt_coord,
        &hf_zbee_nwk_cmd_cinfo_type,
        &hf_zbee_nwk_cmd_cinfo_power,
        &hf_zbee_nwk_cmd_cinfo_idle_rx,
        &hf_zbee_nwk_cmd_cinfo_security,
        &hf_zbee_nwk_cmd_cinfo_alloc,
        NULL
    };

    /* 3.4.14.3 Association Type */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_association_type, tvb, offset, 1, ENC_NA);
    offset += 1;

    proto_tree_add_bitmask(tree, tvb, offset, hf_zbee_nwk_cmd_cinfo, ett_zbee_nwk_cmd_cinfo, capabilities, ENC_NA);
    offset += 1;

    /* Update the info column.*/
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Device: 0x%04x", packet->src);

    return offset;
} /* dissect_zbee_nwk_commissioning_request */

/**
 *Helper dissector for the Commissioning Response command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_commissioning_response(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, zbee_nwk_packet * packet _U_, unsigned offset)
{
    uint8_t status;
    uint16_t new_address;

    /* Get and display the short address. */
    new_address = tvb_get_uint16(tvb, offset, ENC_LITTLE_ENDIAN);
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_addr, tvb, offset, 2, ENC_LITTLE_ENDIAN);
    offset += 2;

    /* Get and display the rejoin status. */
    status = tvb_get_uint8(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_rejoin_status, tvb, offset, 1, status);
    offset += 1;

    /* Update the info column. */
    if (status == IEEE802154_CMD_ASRSP_AS_SUCCESS) {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", New Address: 0x%04x", new_address);
    }
    else {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", %s", val_to_str_const(status, zbee_nwk_rejoin_codes, "Unknown Commissioning Response"));
    }

    return offset;
} /* dissect_zbee_nwk_commissioning_response */

/**
 *Helper dissector for the Network Report command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_report(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset)
{
    uint8_t options;
    uint8_t report_type;
    int     report_count;
    int     i;

    /* Get and display the command options field. */
    options = tvb_get_uint8(tvb, offset);
    report_count = options & ZBEE_NWK_CMD_NWK_REPORT_COUNT_MASK;
    report_type = options & ZBEE_NWK_CMD_NWK_REPORT_ID_MASK;
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_report_type, tvb, offset, 1, report_type);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_report_count, tvb, offset, 1, report_count);
    offset += 1;
    report_type >>= ws_ctz(ZBEE_NWK_CMD_NWK_REPORT_ID_MASK);

    /* Get and display the epid. */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_epid, tvb, offset, 8, ENC_LITTLE_ENDIAN);
    offset += 8;

    if (report_type == ZBEE_NWK_CMD_NWK_REPORT_ID_PAN_CONFLICT) {

        /* Report information contains a list of PANS with range of the sender. */
        for (i=0; i<report_count; i++) {
            proto_tree_add_item(tree, hf_zbee_nwk_panid, tvb, offset, 2, ENC_LITTLE_ENDIAN);
            offset += 2;
        } /* for */
    }
    if (report_type == ZBEE_NWK_CMD_NWK_REPORT_ID_ZBOSS_KEY_TRACE) {
        uint8_t             key[ZBEE_APS_CMD_KEY_LENGTH];

        for (i=0; i<ZBEE_APS_CMD_KEY_LENGTH ; i++) {
            key[i] = tvb_get_uint8(tvb, offset+i);
        } /* for */
        proto_tree_add_item(tree, hf_zbee_zboss_nwk_cmd_key, tvb, offset, ZBEE_APS_CMD_KEY_LENGTH, ENC_NA);
        offset += ZBEE_APS_CMD_KEY_LENGTH;
        zbee_sec_add_key_to_keyring(pinfo, key);
    }

    /* Update the info column. */
    col_append_fstr(pinfo->cinfo, COL_INFO, ", %s", val_to_str_const(report_type, zbee_nwk_report_types, "Unknown Report Type"));

    /* Done */
    return offset;
} /* dissect_zbee_nwk_report */

/**
 *Helper dissector for the Network Update command.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to the command subtree.
 *@param  offset into the tvb to begin dissection.
 *@return offset after command dissection.
*/
static unsigned
dissect_zbee_nwk_update(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, unsigned offset)
{
    uint8_t options;
    uint8_t update_type;
    uint8_t update_id;
    int     update_count;
    int     i;

    /* Get and display the command options field. */
    options = tvb_get_uint8(tvb, offset);
    update_count = options & ZBEE_NWK_CMD_NWK_UPDATE_COUNT_MASK;
    update_type = options & ZBEE_NWK_CMD_NWK_UPDATE_ID_MASK;
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_update_type, tvb, offset, 1, update_type);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_update_count, tvb, offset, 1, update_count);
    offset += 1;

    /* Get and display the epid. */
    proto_tree_add_item(tree, hf_zbee_nwk_cmd_epid, tvb, offset, 8, ENC_LITTLE_ENDIAN);
    offset += 8;

    /* Get and display the updateID. */
    update_id = tvb_get_uint8(tvb, offset);
    proto_tree_add_uint(tree, hf_zbee_nwk_cmd_update_id, tvb, offset, 1, update_id);
    offset += 1;

    if (update_type == ZBEE_NWK_CMD_NWK_UPDATE_ID_PAN_UPDATE) {

        /* Report information contains a list of PANS with range of the sender. */
        for (i=0; i<update_count; i++) {
            proto_tree_add_item(tree, hf_zbee_nwk_panid, tvb, offset, 2, ENC_LITTLE_ENDIAN);
            offset += 2;
        } /* for */
    }

    /* Update the info column. */
    col_append_fstr(pinfo->cinfo, COL_INFO, ", %s", val_to_str_const(update_type, zbee_nwk_update_types, "Unknown Update Type"));

    /* Done */
    return offset;
} /* dissect_zbee_nwk_update */


/**
 *Heuristic interpreter for the ZigBee PRO beacon dissectors.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to data tree Wireshark uses to display packet.
 *@return Boolean value, whether it handles the packet or not.
*/
static bool
dissect_zbee_beacon_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    ieee802154_packet   *packet = (ieee802154_packet *)data;

    /* All ZigBee frames must always have a 16-bit source address. */
    if (!packet) return false;
    if (packet->src_addr_mode != IEEE802154_FCF_ADDR_SHORT) return false;
    if (tvb_captured_length(tvb) == 0) return false;

    /* ZigBee beacons begin with a protocol identifier. */
    if (tvb_get_uint8(tvb, 0) != ZBEE_NWK_BEACON_PROTOCOL_ID) return false;
    dissect_zbee_beacon(tvb, pinfo, tree, packet);
    return true;
} /* dissect_zbee_beacon_heur */

/**
 *Dissector for Legacy ZigBee Beacon Payloads (prior to the Enhanced Beacon)
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to data tree Wireshark uses to display packet.
*/
static int dissect_zbee_beacon(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{

    proto_item  *beacon_root;
    proto_tree  *beacon_tree;
    unsigned     offset = 0;
    uint8_t      version;
    uint32_t     profile;

    static int * const beacon_fields[] = {
        &hf_zbee_beacon_stack_profile,
        &hf_zbee_beacon_version,
        &hf_zbee_beacon_router_capacity,
        &hf_zbee_beacon_depth,
        &hf_zbee_beacon_end_device_capacity,
        NULL
    };

    /* Add ourself to the protocol column. */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "ZigBee");
    /* Create the tree for this beacon. */
    beacon_root = proto_tree_add_item(tree, proto_zbee_beacon, tvb, 0, -1, ENC_NA);
    beacon_tree = proto_item_add_subtree(beacon_root, ett_zbee_nwk_beacon);

    /* Get and display the protocol id, must be 0 on all ZigBee beacons. */
    proto_tree_add_item(beacon_tree, hf_zbee_beacon_protocol, tvb, offset, 1, ENC_NA);
    offset += 1;

    proto_tree_add_bitmask_text(beacon_tree, tvb, offset, 2, "Beacon: ", NULL , ett_zbee_nwk_beacon_bitfield, beacon_fields,
            ENC_LITTLE_ENDIAN, BMT_NO_INT|BMT_NO_TFS);

    /* Get and display the stack profile and protocol version. */
    version = (uint8_t)((tvb_get_uint16(tvb, offset, ENC_LITTLE_ENDIAN) & ZBEE_NWK_BEACON_PROTOCOL_VERSION) >> 4);
    profile = (uint32_t)(tvb_get_uint16(tvb, offset, ENC_LITTLE_ENDIAN) & ZBEE_NWK_BEACON_STACK_PROFILE);
    proto_item_append_text(beacon_root, ", %s", val_to_str_const(profile, zbee_nwk_stack_profiles, "Unknown Profile"));
    offset += 2;

    if (version >= ZBEE_VERSION_2007) {
        /* In ZigBee 2006 and later, the beacon contains an extended PAN ID. */
        proto_tree_add_item(beacon_tree, hf_zbee_beacon_epid, tvb, offset, 8, ENC_LITTLE_ENDIAN);
        col_append_fstr(pinfo->cinfo, COL_INFO, ", EPID: %s", eui64_to_display(pinfo->pool,
            tvb_get_uint64(tvb, offset, ENC_LITTLE_ENDIAN)));
        proto_item_append_text(beacon_root, ", EPID: %s", eui64_to_display(pinfo->pool,
                tvb_get_uint64(tvb, offset, ENC_LITTLE_ENDIAN)));
        offset += 8;

        /*
         * In ZigBee 2006 the Tx-Offset is optional, while in the 2007 and
         * later versions, the Tx-Offset is a required value. Since both 2006 and
         * and 2007 versions have the same protocol version (2), we should treat
         * the Tx-Offset as well as the update ID as optional elements
         */
        if (tvb_bytes_exist(tvb, offset, 3)) {
            proto_tree_add_item(beacon_tree, hf_zbee_beacon_tx_offset, tvb, offset, 3, ENC_LITTLE_ENDIAN);
            offset += 3;

            /* Get and display the update ID. */
            if(tvb_captured_length_remaining(tvb, offset)) {
                proto_tree_add_item(beacon_tree, hf_zbee_beacon_update_id, tvb, offset, 1, ENC_NA);
                offset += 1;
            }
        }
    }
    else if (tvb_bytes_exist(tvb, offset, 3)) {
        /* In ZigBee 2004, the Tx-Offset is an optional value. */
        proto_tree_add_item(beacon_tree, hf_zbee_beacon_tx_offset, tvb, offset, 3, ENC_LITTLE_ENDIAN);
        offset += 3;

    }

    offset = dissect_zbee_tlvs(tvb, pinfo, beacon_tree, offset, data, ZBEE_TLV_SRC_TYPE_DEFAULT, 0);

    return offset;
} /* dissect_zbee_beacon */

/**
 *Heuristic interpreter for the ZigBee IP beacon dissectors.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to data tree Wireshark uses to display packet.
 *@return Boolean value, whether it handles the packet or not.
*/
static bool
dissect_zbip_beacon_heur(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    ieee802154_packet   *packet = (ieee802154_packet *)data;

    /* All ZigBee frames must always have a 16-bit source address. */
    if (!packet) return false;
    if (packet->src_addr_mode != IEEE802154_FCF_ADDR_SHORT) return false;
    if (tvb_captured_length(tvb) == 0) return false;

    /* ZigBee beacons begin with a protocol identifier. */
    if (tvb_get_uint8(tvb, 0) != ZBEE_IP_BEACON_PROTOCOL_ID) return false;
    dissect_zbip_beacon(tvb, pinfo, tree, packet);
    return true;
} /* dissect_zbip_beacon_heur */

/**
 *Dissector for ZigBee IP beacons.
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields
 *@param tree pointer to data tree Wireshark uses to display packet.
*/
static int dissect_zbip_beacon(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    ieee802154_packet   *packet = (ieee802154_packet *)data;

    proto_item  *beacon_root;
    proto_tree  *beacon_tree;
    unsigned    offset = 0;
    uint8_t     proto_id;
    char        *ssid;

    /* Reject the packet if data is NULL */
    if (!packet) return 0;

    /* Add ourself to the protocol column. */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "ZigBee IP");
    /* Create the tree for this beacon. */
    beacon_root = proto_tree_add_item(tree, proto_zbip_beacon, tvb, 0, -1, ENC_NA);
    beacon_tree = proto_item_add_subtree(beacon_root, ett_zbee_nwk_beacon);

    /* Update the info column. */
    col_clear(pinfo->cinfo, COL_INFO);
    col_append_fstr(pinfo->cinfo, COL_INFO, "Beacon, Src: 0x%04x", packet->src16);

    /* Get and display the protocol id, must be 0x02 on all ZigBee beacons. */
    proto_id = tvb_get_uint8(tvb, offset);
    proto_tree_add_uint(beacon_tree, hf_zbee_beacon_protocol, tvb, offset, 1, proto_id);
    offset += 1;

    /* Get and display the beacon flags */
    proto_tree_add_item(beacon_tree, hf_zbip_beacon_allow_join, tvb, offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(beacon_tree, hf_zbip_beacon_router_capacity, tvb, offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(beacon_tree, hf_zbip_beacon_host_capacity, tvb, offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(beacon_tree, hf_zbip_beacon_unsecure, tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    /* Get and display the network ID. */
    proto_tree_add_item(beacon_tree, hf_zbip_beacon_network_id, tvb, offset, 16, ENC_ASCII);

    ssid = tvb_get_string_enc(pinfo->pool, tvb, offset, 16, ENC_ASCII|ENC_NA);
    col_append_fstr(pinfo->cinfo, COL_INFO, ", SSID: %s", ssid);
    offset += 16;

    offset = dissect_zbee_tlvs(tvb, pinfo, beacon_tree, offset, data, ZBEE_TLV_SRC_TYPE_DEFAULT, 0);

    /* Check for leftover bytes. */
    if (offset < tvb_captured_length(tvb)) {
        /* Bytes leftover! */
        tvbuff_t    *leftover_tvb   = tvb_new_subset_remaining(tvb, offset);
        proto_tree  *root;

        /* Correct the length of the beacon tree. */
        root = proto_tree_get_root(tree);
        proto_item_set_len(beacon_root, offset);

        /* Dump the leftover to the data dissector. */
        call_data_dissector(leftover_tvb, pinfo, root);
    }
    return tvb_captured_length(tvb);
} /* dissect_zbip_beacon */

/**
 *Subdissector command for ZigBee Specific IEs (Information Elements)
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields (unused).
 *@param tree pointer to command subtree.
 *@param data pointer to the length of the payload IE.
*/
static int
dissect_zbee_ie(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    proto_tree *subtree;
    tvbuff_t   *ie_tvb;
    uint16_t    zigbee_ie;
    uint16_t    id;
    uint16_t    length;
    unsigned    pie_length;
    unsigned    offset = 0;

    static int * const fields[] = {
        &hf_ieee802154_zigbee_ie_id,
        &hf_ieee802154_zigbee_ie_length,
        NULL
    };

    pie_length = *(int *)data;

    do {
        zigbee_ie =  tvb_get_letohs(tvb, offset);
        id        = (zigbee_ie & ZBEE_ZIGBEE_IE_ID_MASK) >> 6;
        length    =  zigbee_ie & ZBEE_ZIGBEE_IE_LENGTH_MASK;

        /* Create a subtree for this command frame. */
        subtree = proto_tree_add_subtree(tree, tvb, offset, 2+length, ett_zbee_nwk_header, NULL, "ZigBee IE");
        proto_item_append_text(subtree, ", %s, Length: %d", val_to_str_const(id, ieee802154_zigbee_ie_names, "Unknown"), length);

        proto_tree_add_bitmask(subtree, tvb, offset, hf_ieee802154_zigbee_ie,
                               ett_zbee_nwk_zigbee_ie_fields, fields, ENC_LITTLE_ENDIAN);
        offset += 2;

        switch (id) {
            case ZBEE_ZIGBEE_IE_REJOIN:
                dissect_ieee802154_zigbee_rejoin(tvb, pinfo, subtree, &offset);
                break;

            case ZBEE_ZIGBEE_IE_TX_POWER:
                dissect_ieee802154_zigbee_txpower(tvb, pinfo, subtree, &offset);
                break;

            case ZBEE_ZIGBEE_IE_BEACON_PAYLOAD:
                ie_tvb = tvb_new_subset_length(tvb, offset, ZBEE_NWK_BEACON_LENGTH);
                offset += dissect_zbee_beacon(ie_tvb, pinfo, subtree, NULL); /* Legacy ZigBee Beacon */
                dissect_ieee802154_superframe(tvb, pinfo, subtree, &offset);
                proto_tree_add_item(subtree, hf_ieee802154_zigbee_ie_source_addr, tvb, offset, 2, ENC_NA);
                offset += 2;
                break;

            default:
                if (length > 0) { /* just use the data dissector */
                    call_data_dissector(tvb, pinfo, tree);
                    offset += length;
                }
                break;
        }
    } while (offset < pie_length);

    return tvb_captured_length(tvb);
}

/**
 *Subdissector for the ZigBee specific TX Power IE (information element)
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields (unused).
 *@param tree pointer to command subtree.
 *@param offset offset into the tvbuff to begin dissection.
*/
static void
dissect_ieee802154_zigbee_txpower(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, unsigned *offset)
{
    int32_t txpower;

    txpower = (char)tvb_get_uint8(tvb, *offset); /* tx power is a signed byte */

    proto_tree_add_item_ret_int(tree, hf_ieee802154_zigbee_ie_tx_power, tvb, *offset, 1, ENC_NA, &txpower);
    proto_item_append_text(tree, ", TX Power %d dBm", txpower);

    *offset += 1;
}

/**
 *Subdissector for the ZigBee specific Rejoin IE (information element)
 *
 *@param tvb pointer to buffer containing raw packet.
 *@param pinfo pointer to packet information fields (unused).
 *@param tree pointer to command subtree.
 *@param offset offset into the tvbuff to begin dissection.
*/
static void
dissect_ieee802154_zigbee_rejoin(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, unsigned *offset)
{
    proto_tree *subtree;

    subtree = proto_tree_add_subtree(tree, tvb, *offset, 10, ett_zbee_nwk_ie_rejoin, NULL, "ZigBee Rejoin");

    proto_tree_add_item(subtree, hf_ieee802154_zigbee_rejoin_epid, tvb, *offset, 8, ENC_LITTLE_ENDIAN);
    proto_item_append_text(tree, ", EPID %s", eui64_to_display(pinfo->pool,
            tvb_get_uint64(tvb, *offset, ENC_LITTLE_ENDIAN)));
    *offset += 8;

    proto_tree_add_item(subtree, hf_ieee802154_zigbee_rejoin_source_addr, tvb, *offset, 2, ENC_LITTLE_ENDIAN);
    proto_item_append_text(tree, ", Src: 0x%04x",
            tvb_get_uint16(tvb, *offset, ENC_LITTLE_ENDIAN));
    *offset += 2;

} /* dissect_ieee802154_zigbee_rejoin */

static const char* zbee_nwk_conv_get_filter_type(conv_item_t* conv, conv_filter_type_e filter)
{
    if ((filter == CONV_FT_SRC_ADDRESS) && (conv->src_address.type == zbee_nwk_address_type))
        return "zbee_nwk.src";

    if ((filter == CONV_FT_DST_ADDRESS) && (conv->dst_address.type == zbee_nwk_address_type))
        return "zbee_nwk.dst";

    if ((filter == CONV_FT_ANY_ADDRESS) && (conv->src_address.type == zbee_nwk_address_type))
        return "zbee_nwk.addr";

    return CONV_FILTER_INVALID;
}

static ct_dissector_info_t zbee_nwk_ct_dissector_info = {&zbee_nwk_conv_get_filter_type };

static tap_packet_status zbee_nwk_conversation_packet(void *pct, packet_info *pinfo, epan_dissect_t *edt _U_, const void *vip _U_, tap_flags_t flags)
{
    conv_hash_t *hash = (conv_hash_t*)pct;
    hash->flags = flags;

    add_conversation_table_data(hash, &pinfo->net_src, &pinfo->net_dst, 0, 0, 1,
            pinfo->fd->pkt_len, &pinfo->rel_ts, &pinfo->abs_ts,
            &zbee_nwk_ct_dissector_info, CONVERSATION_NONE);

    return TAP_PACKET_REDRAW;
}

static const char* zbee_nwk_endpoint_get_filter_type(endpoint_item_t* endpoint, conv_filter_type_e filter)
{
  if ((filter == CONV_FT_ANY_ADDRESS) && (endpoint->myaddress.type == zbee_nwk_address_type))
  {
      return "zbee_nwk.addr";
  }
  else
  {
      return CONV_FILTER_INVALID;
  }
}

static et_dissector_info_t zbee_nwk_endpoint_dissector_info = {&zbee_nwk_endpoint_get_filter_type };

static tap_packet_status zbee_nwk_endpoint_packet(void *pit, packet_info *pinfo, epan_dissect_t *edt _U_, const void *vip _U_, tap_flags_t flags)
{
    conv_hash_t *hash = (conv_hash_t*)pit;
    hash->flags = flags;

    /* Take two "add" passes per packet, adding for each direction, ensures that all
     packets are counted properly (even if address is sending to itself)
     XXX - this could probably be done more efficiently inside endpoint_table */
    add_endpoint_table_data(hash, &pinfo->net_src, 0, true, 1,
            pinfo->fd->pkt_len, &zbee_nwk_endpoint_dissector_info, ENDPOINT_NONE);
    add_endpoint_table_data(hash, &pinfo->net_dst, 0, false, 1,
            pinfo->fd->pkt_len, &zbee_nwk_endpoint_dissector_info, ENDPOINT_NONE);

    return TAP_PACKET_REDRAW;
}

static bool zbee_nwk_filter_valid(packet_info *pinfo, void *user_data _U_)
{
    return proto_is_frame_protocol(pinfo->layers, "zbee_nwk");
}

static char* zbee_nwk_build_filter(packet_info *pinfo, void *user_data _U_)
{
    return ws_strdup_printf("zbee_nwk.addr eq %s and zbee_nwk.addr eq %s",
            address_to_str(pinfo->pool, &pinfo->net_src),
            address_to_str(pinfo->pool, &pinfo->net_dst));
}

/**
 *ZigBee protocol registration routine.
 *
*/
void proto_register_zbee_nwk(void)
{
    static hf_register_info hf[] = {

            { &hf_zbee_nwk_fcf,
            { "Frame Control Field",             "zbee_nwk.fcf", FT_UINT16, BASE_HEX, NULL,
                0x0, NULL, HFILL }},

            { &hf_zbee_nwk_frame_type,
            { "Frame Type",             "zbee_nwk.frame_type", FT_UINT16, BASE_HEX, VALS(zbee_nwk_frame_types),
                ZBEE_NWK_FCF_FRAME_TYPE, NULL, HFILL }},

            { &hf_zbee_nwk_proto_version,
            { "Protocol Version",       "zbee_nwk.proto_version", FT_UINT16, BASE_DEC, NULL, ZBEE_NWK_FCF_VERSION,
                NULL, HFILL }},

            { &hf_zbee_nwk_discover_route,
            { "Discover Route",         "zbee_nwk.discovery", FT_UINT16, BASE_HEX, VALS(zbee_nwk_discovery_modes),
                ZBEE_NWK_FCF_DISCOVER_ROUTE,
                "Determines how route discovery may be handled, if at all.", HFILL }},

            { &hf_zbee_nwk_multicast,
            { "Multicast",              "zbee_nwk.multicast", FT_BOOLEAN, 16, NULL, ZBEE_NWK_FCF_MULTICAST,
                NULL, HFILL }},

            { &hf_zbee_nwk_security,
            { "Security",               "zbee_nwk.security", FT_BOOLEAN, 16, NULL, ZBEE_NWK_FCF_SECURITY,
                "Whether or not security operations are performed on the network payload.", HFILL }},

            { &hf_zbee_nwk_source_route,
            { "Source Route",           "zbee_nwk.src_route", FT_BOOLEAN, 16, NULL, ZBEE_NWK_FCF_SOURCE_ROUTE,
                NULL, HFILL }},

            { &hf_zbee_nwk_ext_dst,
            { "Destination",            "zbee_nwk.ext_dst", FT_BOOLEAN, 16, NULL, ZBEE_NWK_FCF_EXT_DEST,
                NULL, HFILL }},

            { &hf_zbee_nwk_ext_src,
            { "Extended Source",        "zbee_nwk.ext_src", FT_BOOLEAN, 16, NULL, ZBEE_NWK_FCF_EXT_SOURCE,
                NULL, HFILL }},

            { &hf_zbee_nwk_end_device_initiator,
            { "End Device Initiator",   "zbee_nwk.end_device_initiator", FT_BOOLEAN, 16, NULL, ZBEE_NWK_FCF_END_DEVICE_INITIATOR,
                NULL, HFILL }},

            { &hf_zbee_nwk_dst,
            { "Destination",            "zbee_nwk.dst", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_src,
            { "Source",                 "zbee_nwk.src", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_addr,
            { "Address",                 "zbee_nwk.addr", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_radius,
            { "Radius",                 "zbee_nwk.radius", FT_UINT8, BASE_DEC, NULL, 0x0,
                "Number of hops remaining for a range-limited broadcast packet.", HFILL }},

            { &hf_zbee_nwk_seqno,
            { "Sequence Number",        "zbee_nwk.seqno", FT_UINT8, BASE_DEC, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_mcast,
            { "Multicast Control Field",         "zbee_nwk.multicast.cf", FT_UINT8, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_mcast_mode,
            { "Multicast Mode",         "zbee_nwk.multicast.mode", FT_UINT8, BASE_DEC, NULL, ZBEE_NWK_MCAST_MODE,
                "Controls whether this packet is permitted to be routed through non-members of the multicast group.",
                HFILL }},

            { &hf_zbee_nwk_mcast_radius,
            { "Non-Member Radius",      "zbee_nwk.multicast.radius", FT_UINT8, BASE_DEC, NULL, ZBEE_NWK_MCAST_RADIUS,
                "Limits the range of multicast packets when being routed through non-members.", HFILL }},

            { &hf_zbee_nwk_mcast_max_radius,
            { "Max Non-Member Radius",  "zbee_nwk.multicast.max_radius", FT_UINT8, BASE_DEC, NULL,
                ZBEE_NWK_MCAST_MAX_RADIUS, NULL, HFILL }},

            { &hf_zbee_nwk_dst64,
            { "Destination",   "zbee_nwk.dst64", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_src64,
            { "Extended Source",        "zbee_nwk.src64", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_addr64,
            { "Extended Address",        "zbee_nwk.addr64", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_src64_origin,
            { "Origin",        "zbee_nwk.src64.origin", FT_FRAMENUM, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_relay_count,
            { "Relay Count",            "zbee_nwk.relay.count", FT_UINT8, BASE_DEC, NULL, 0x0,
                "Number of entries in the relay list.", HFILL }},

            { &hf_zbee_nwk_relay_index,
            { "Relay Index",            "zbee_nwk.relay.index", FT_UINT8, BASE_DEC, NULL, 0x0,
                "Number of relays required to route to the source device.", HFILL }},

            { &hf_zbee_nwk_relay,
            { "Relay",            "zbee_nwk.relay", FT_UINT16, BASE_DEC, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_id,
            { "Command Identifier",     "zbee_nwk.cmd.id", FT_UINT8, BASE_HEX, VALS(zbee_nwk_cmd_names), 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_addr,
            { "Address",                "zbee_nwk.cmd.addr", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_id,
            { "Route ID",               "zbee_nwk.cmd.route.id", FT_UINT8, BASE_DEC, NULL, 0x0,
                "A sequence number for routing commands.", HFILL }},

            { &hf_zbee_nwk_cmd_route_dest,
            { "Destination",            "zbee_nwk.cmd.route.dest", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_orig,
            { "Originator",             "zbee_nwk.cmd.route.orig", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_resp,
            { "Responder",              "zbee_nwk.cmd.route.resp", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_dest_ext,
            { "Extended Destination",   "zbee_nwk.cmd.route.dest_ext", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_orig_ext,
            { "Extended Originator",    "zbee_nwk.cmd.route.orig_ext", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_resp_ext,
            { "Extended Responder",     "zbee_nwk.cmd.route.resp_ext", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_cost,
            { "Path Cost",              "zbee_nwk.cmd.route.cost", FT_UINT8, BASE_DEC, NULL, 0x0,
                "A value specifying the efficiency of this route.", HFILL }},

            { &hf_zbee_nwk_cmd_route_options,
            { "Command Options",           "zbee_nwk.cmd.route.opts", FT_UINT8, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_opt_repair,
            { "Route Repair",           "zbee_nwk.cmd.route.opts.repair", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_ROUTE_OPTION_REPAIR,
                "Flag identifying whether the route request command was to repair a failed route.", HFILL }},

            { &hf_zbee_nwk_cmd_route_opt_multicast,
            { "Multicast",              "zbee_nwk.cmd.route.opts.mcast", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_ROUTE_OPTION_MCAST,
                "Flag identifying this as a multicast route request.", HFILL }},

            { &hf_zbee_nwk_cmd_route_opt_dest_ext,
            { "Extended Destination",   "zbee_nwk.cmd.route.opts.dest_ext", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_ROUTE_OPTION_DEST_EXT, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_opt_resp_ext,
            { "Extended Responder",   "zbee_nwk.cmd.route.opts.resp_ext", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_ROUTE_OPTION_RESP_EXT, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_opt_orig_ext,
            { "Extended Originator",    "zbee_nwk.cmd.route.opts.orig_ext", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_ROUTE_OPTION_ORIG_EXT, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_route_opt_many_to_one,
            { "Many-to-One Discovery",  "zbee_nwk.cmd.route.opts.many2one", FT_UINT8, BASE_HEX,
                VALS(zbee_nwk_cmd_route_many_modes), ZBEE_NWK_CMD_ROUTE_OPTION_MANY_MASK,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_nwk_status,
            { "Status Code",            "zbee_nwk.cmd.status", FT_UINT8, BASE_HEX, VALS(zbee_nwk_status_codes), 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_nwk_status_command_id,
            { "Unknown Command ID",     "zbee_nwk.cmd.status.unknown_command_id", FT_UINT8, BASE_HEX,
                    VALS(zbee_nwk_cmd_names), 0x0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_leave_rejoin,
            { "Rejoin",                 "zbee_nwk.cmd.leave.rejoin", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_LEAVE_OPTION_REJOIN, "Flag instructing the device to rejoin the network.", HFILL }},

            { &hf_zbee_nwk_cmd_leave_request,
            { "Request",                "zbee_nwk.cmd.leave.request", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_LEAVE_OPTION_REQUEST,
                "Flag identifying the direction of this command. 1=Request, 0=Indication", HFILL }},

            { &hf_zbee_nwk_cmd_leave_children,
            { "Remove Children",        "zbee_nwk.cmd.leave.children", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_LEAVE_OPTION_CHILDREN,
                "Flag instructing the device to remove its children in addition to itself.", HFILL }},

            { &hf_zbee_nwk_cmd_relay_count,
            { "Relay Count",            "zbee_nwk.cmd.relay_count", FT_UINT8, BASE_DEC, NULL, 0x0,
                "Number of relays required to route to the destination.", HFILL }},

            { &hf_zbee_nwk_cmd_relay_device,
            { "Relay Device",            "zbee_nwk.cmd.relay_device", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_cinfo,
            { "Capability Information",  "zbee_nwk.cmd.cinfo", FT_UINT8, BASE_HEX, NULL,
                0x0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_cinfo_alt_coord,
            { "Alternate Coordinator",  "zbee_nwk.cmd.cinfo.alt_coord", FT_BOOLEAN, 8, NULL,
                IEEE802154_CMD_CINFO_ALT_PAN_COORD,
                "Indicates that the device is able to operate as a PAN coordinator.", HFILL }},

            { &hf_zbee_nwk_cmd_cinfo_type,
            { "Full-Function Device",   "zbee_nwk.cmd.cinfo.ffd", FT_BOOLEAN, 8, NULL,
                IEEE802154_CMD_CINFO_DEVICE_TYPE, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_cinfo_power,
            { "AC Power",               "zbee_nwk.cmd.cinfo.power", FT_BOOLEAN, 8, NULL,
                IEEE802154_CMD_CINFO_POWER_SRC, "Indicates this device is using AC/Mains power.", HFILL }},

            { &hf_zbee_nwk_cmd_cinfo_idle_rx,
            { "Rx On When Idle",        "zbee_nwk.cmd.cinfo.on_idle", FT_BOOLEAN, 8, NULL,
                IEEE802154_CMD_CINFO_IDLE_RX,
                "Indicates the receiver is active when the device is idle.", HFILL }},

            { &hf_zbee_nwk_cmd_cinfo_security,
            { "Security Capability",    "zbee_nwk.cmd.cinfo.security", FT_BOOLEAN, 8, NULL,
                IEEE802154_CMD_CINFO_SEC_CAPABLE,
                "Indicates this device is capable of performing encryption/decryption.", HFILL }},

            { &hf_zbee_nwk_cmd_cinfo_alloc,
            { "Allocate Short Address", "zbee_nwk.cmd.cinfo.alloc", FT_BOOLEAN, 8, NULL,
                IEEE802154_CMD_CINFO_ALLOC_ADDR,
                "Flag requesting the parent to allocate a short address for this device.", HFILL }},

            { &hf_zbee_nwk_cmd_rejoin_status,
            { "Status",                 "zbee_nwk.cmd.rejoin_status", FT_UINT8, BASE_HEX,
                VALS(zbee_nwk_rejoin_codes), 0x0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_last,
            { "Last Frame",             "zbee_nwk.cmd.link.last", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_LINK_OPTION_LAST_FRAME,
                "Flag indicating the last in a series of link status commands.", HFILL }},

            { &hf_zbee_nwk_cmd_link_first,
            { "First Frame",            "zbee_nwk.cmd.link.first", FT_BOOLEAN, 8, NULL,
                ZBEE_NWK_CMD_LINK_OPTION_FIRST_FRAME,
                "Flag indicating the first in a series of link status commands.", HFILL }},

            { &hf_zbee_nwk_cmd_link_count,
            { "Link Status Count",      "zbee_nwk.cmd.link.count", FT_UINT8, BASE_DEC, NULL,
                ZBEE_NWK_CMD_LINK_OPTION_COUNT_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_address,
            { "Address",      "zbee_nwk.cmd.link.address", FT_UINT16, BASE_HEX, NULL,
                0x0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_incoming_cost,
            { "Incoming Cost",      "zbee_nwk.cmd.link.incoming_cost", FT_UINT8, BASE_DEC, NULL,
                ZBEE_NWK_CMD_LINK_INCOMMING_COST_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_outgoing_cost,
            { "Outgoing Cost",      "zbee_nwk.cmd.link.outgoing_cost", FT_UINT8, BASE_DEC, NULL,
                ZBEE_NWK_CMD_LINK_OUTGOING_COST_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_report_type,
            { "Report Type",            "zbee_nwk.cmd.report.type", FT_UINT8, BASE_HEX,
                VALS(zbee_nwk_report_types), ZBEE_NWK_CMD_NWK_REPORT_ID_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_report_count,
            { "Report Information Count",   "zbee_nwk.cmd.report.count", FT_UINT8, BASE_DEC, NULL,
                ZBEE_NWK_CMD_NWK_REPORT_COUNT_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_update_type,
            { "Update Type",            "zbee_nwk.cmd.update.type", FT_UINT8, BASE_HEX,
                VALS(zbee_nwk_update_types), ZBEE_NWK_CMD_NWK_UPDATE_ID_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_update_count,
            { "Update Information Count",   "zbee_nwk.cmd.update.count", FT_UINT8, BASE_DEC, NULL,
                ZBEE_NWK_CMD_NWK_UPDATE_COUNT_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_update_id,
            { "Update ID",              "zbee_nwk.cmd.update.id", FT_UINT8, BASE_DEC, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_panid,
            { "PAN ID",        "zbee_nwk.panid", FT_UINT16, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_zboss_nwk_cmd_key,
            { "ZBOSS Key",        "zbee_nwk.zboss_key", FT_BYTES, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_epid,
            { "Extended PAN ID",        "zbee_nwk.cmd.epid", FT_EUI64, BASE_NONE, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_end_device_timeout_request_enum,
            { "Requested Timeout Enumeration",        "zbee_nwk.cmd.ed_tmo_req", FT_UINT8, BASE_DEC,
              VALS(zbee_nwk_end_device_timeout_request), 0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_end_device_configuration,
            { "End Device Configuration",        "zbee_nwk.cmd.ed_config", FT_UINT8, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_end_device_timeout_resp_status,
            { "Status",        "zbee_nwk.cmd.ed_tmo_rsp_status", FT_UINT8, BASE_DEC,
              VALS(zbee_nwk_end_device_timeout_resp_status), 0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_end_device_timeout_resp_parent_info,
            { "Parent Information",        "zbee_nwk.cmd.ed_prnt_info", FT_UINT8, BASE_HEX, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_nwk_cmd_prnt_info_mac_data_poll_keepalive_supported,
            { "MAC Data Poll Keepalive",           "zbee_nwk.cmd.ed_prnt_info.mac_data_poll_keepalive", FT_BOOLEAN, 8, NULL,
              ZBEE_NWK_CMD_ED_TIMEO_RSP_PRNT_INFO_MAC_DATA_POLL_KEEPAL_SUPP,
              NULL, HFILL }},

            { &hf_zbee_nwk_cmd_prnt_info_ed_to_req_keepalive_supported,
            { "End Device Timeout Request Keepalive",           "zbee_nwk.cmd.ed_prnt_info.ed_tmo_req_keepalive", FT_BOOLEAN, 8, NULL,
              ZBEE_NWK_CMD_ED_TIMEO_RSP_PRNT_INFO_ED_TIMOU_REQ_KEEPAL_SUPP,
              NULL, HFILL }},

            { &hf_zbee_nwk_cmd_prnt_info_power_negotiation_supported,
            { "Power Negotiation Supported",           "zbee_nwk.cmd.power_negotiation_supported", FT_BOOLEAN, 8, NULL,
              ZBEE_NWK_CMD_ED_TIMEO_RSP_PRNT_INFO_PWR_NEG_SUPP,
              NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_pwr_type,
            { "Type",        "zbee_nwk.cmd.link_pwr_delta.type", FT_UINT8, BASE_HEX,
                VALS(zbee_nwk_link_power_delta_types), ZBEE_NWK_CMD_NWK_LINK_PWR_DELTA_TYPE_MASK, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_pwr_list_count,
            { "Structure Count",        "zbee_nwk.cmd.link_pwr_delta.list_count", FT_UINT8, BASE_DEC, NULL, 0x0,
              NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_pwr_device_address,
            { "Device Address",      "zbee_nwk.cmd.link_pwr_delta.address", FT_UINT16, BASE_HEX, NULL,
                0x0, NULL, HFILL }},

            { &hf_zbee_nwk_cmd_link_pwr_power_delta,
            { "Power Delta",         "zbee_nwk.cmd.link_pwr_delta.power_delta", FT_INT8, BASE_DEC, NULL, 0x0,
                    NULL, HFILL }},

            { &hf_zbee_nwk_cmd_association_type,
              { "Association Type",        "zbee_nwk.cmd.association_type", FT_UINT8, BASE_HEX,
                VALS(zbee_nwk_commissioning_types), 0x0, NULL, HFILL }},

            { &hf_zbee_beacon_protocol,
            { "Protocol ID",            "zbee_beacon.protocol", FT_UINT8, BASE_DEC, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbee_beacon_stack_profile,
            { "Stack Profile",          "zbee_beacon.profile", FT_UINT16, BASE_HEX,
                VALS(zbee_nwk_stack_profiles), ZBEE_NWK_BEACON_STACK_PROFILE, NULL, HFILL }},

            { &hf_zbee_beacon_version,
            { "Protocol Version",       "zbee_beacon.version", FT_UINT16, BASE_DEC, NULL, ZBEE_NWK_BEACON_PROTOCOL_VERSION,
                NULL, HFILL }},

            { &hf_zbee_beacon_router_capacity,
            { "Router Capacity", "zbee_beacon.router", FT_BOOLEAN, 16, NULL, ZBEE_NWK_BEACON_ROUTER_CAPACITY,
                "Whether the device can accept join requests from routing capable devices.", HFILL }},

            { &hf_zbee_beacon_depth,
            { "Device Depth",           "zbee_beacon.depth", FT_UINT16, BASE_DEC, NULL, ZBEE_NWK_BEACON_NETWORK_DEPTH,
                "The tree depth of the device, 0 indicates the network coordinator.", HFILL }},

            { &hf_zbee_beacon_end_device_capacity,
            { "End Device Capacity",        "zbee_beacon.end_dev", FT_BOOLEAN, 16, NULL, ZBEE_NWK_BEACON_END_DEVICE_CAPACITY,
                "Whether the device can accept join requests from ZigBee end devices.", HFILL }},

            { &hf_zbee_beacon_epid,
            { "Extended PAN ID",        "zbee_beacon.ext_panid", FT_EUI64, BASE_NONE, NULL, 0x0,
                "Extended PAN identifier.", HFILL }},

            { &hf_zbee_beacon_tx_offset,
            { "Tx Offset",              "zbee_beacon.tx_offset", FT_UINT24, BASE_DEC, NULL, 0x0,
                "The time difference between a device and its parent's beacon.", HFILL }},

            { &hf_zbee_beacon_update_id,
            { "Update ID",              "zbee_beacon.update_id", FT_UINT8, BASE_DEC, NULL, 0x0,
                NULL, HFILL }},

            { &hf_zbip_beacon_allow_join,
            { "Allow Join",             "zbip_beacon.allow_join", FT_BOOLEAN, 8, NULL, ZBEE_IP_BEACON_ALLOW_JOIN,
                NULL, HFILL }},

            { &hf_zbip_beacon_router_capacity,
            { "Router Capacity",        "zbip_beacon.router", FT_BOOLEAN, 8, NULL, ZBEE_IP_BEACON_ROUTER_CAPACITY,
                "Whether this device can accept new routers on the network.", HFILL }},

            { &hf_zbip_beacon_host_capacity,
            { "Host Capacity",        "zbip_beacon.host", FT_BOOLEAN, 8, NULL, ZBEE_IP_BEACON_HOST_CAPACITY,
                "Whether this device can accept new host on the network.", HFILL }},

            { &hf_zbip_beacon_unsecure,
            { "Unsecure Network",     "zbip_beacon.unsecure", FT_BOOLEAN, 8, NULL, ZBEE_IP_BEACON_UNSECURE,
                "Indicates that this network is not using link layer security.", HFILL }},

            { &hf_zbip_beacon_network_id,
            { "Network ID",           "zbip_beacon.network_id", FT_STRING, BASE_NONE, NULL, 0x0,
                "A string that uniquely identifies this network.", HFILL }},

            { &hf_ieee802154_zigbee_ie,
            { "IE header",                       "zbee_nwk.zigbee_ie", FT_UINT16, BASE_HEX, NULL, 0x0, NULL, HFILL }},

            { &hf_ieee802154_zigbee_ie_id,
            { "Id",                              "zbee_nwk.zigbee_ie.id", FT_UINT16, BASE_HEX, VALS(ieee802154_zigbee_ie_names),
                    ZBEE_ZIGBEE_IE_ID_MASK, NULL, HFILL }},

            { &hf_ieee802154_zigbee_ie_length,
            { "Length",                           "zbee_nwk.zigbee_ie.length", FT_UINT16, BASE_DEC, NULL,
                    ZBEE_ZIGBEE_IE_LENGTH_MASK, NULL, HFILL }},

            { &hf_ieee802154_zigbee_ie_tx_power,
            { "Tx Power (dBm)",                  "zbee_nwk.zigbee_ie.tx_power", FT_INT8, BASE_DEC, NULL, 0x0, NULL, HFILL }},

            { &hf_ieee802154_zigbee_ie_source_addr,
            { "Source Address",                  "zbee_nwk.zigbee_ie.source_address", FT_UINT16, BASE_HEX, NULL, 0x0, NULL, HFILL }},


            { &hf_ieee802154_zigbee_rejoin_epid,
            { "Extended PAN ID",                "zbee_nwk.zigbee_rejoin.ext_panid", FT_EUI64, BASE_NONE, NULL, 0x0,
                "Extended PAN identifier", HFILL }},

            { &hf_ieee802154_zigbee_rejoin_source_addr,
            { "Source Address",                  "zbee_nwk.zigbee_rejoin.source_address", FT_UINT16, BASE_HEX, NULL, 0x0, NULL, HFILL }},


    };

    /*  NWK Layer subtrees */
    static int *ett[] = {
        &ett_zbee_nwk,
        &ett_zbee_nwk_beacon,
        &ett_zbee_nwk_fcf,
        &ett_zbee_nwk_fcf_ext,
        &ett_zbee_nwk_mcast,
        &ett_zbee_nwk_route,
        &ett_zbee_nwk_cmd,
        &ett_zbee_nwk_cmd_options,
        &ett_zbee_nwk_cmd_cinfo,
        &ett_zbee_nwk_cmd_link,
        &ett_zbee_nwk_cmd_ed_to_rsp_prnt_info,
        &ett_zbee_nwk_cmd_link_pwr_struct,
        &ett_zbee_nwk_zigbee_ie_fields,
        &ett_zbee_nwk_ie_rejoin,
        &ett_zbee_nwk_header,
        &ett_zbee_nwk_header_ie,
        &ett_zbee_nwk_beacon_bitfield,
    };

    static ei_register_info ei[] = {
        { &ei_zbee_nwk_missing_payload, { "zbee_nwk.missing_payload", PI_MALFORMED, PI_ERROR, "Missing Payload", EXPFILL }},
    };

    expert_module_t* expert_zbee_nwk;

    register_init_routine(proto_init_zbee_nwk);
    register_cleanup_routine(proto_cleanup_zbee_nwk);

    /* Register the protocol with Wireshark. */
    proto_zbee_nwk = proto_register_protocol("ZigBee Network Layer", "ZigBee", ZBEE_PROTOABBREV_NWK);
    proto_zbee_beacon = proto_register_protocol("ZigBee Beacon", "ZigBee Beacon", "zbee_beacon");
    proto_zbip_beacon = proto_register_protocol("ZigBee IP Beacon", "ZigBee IP Beacon", "zbip_beacon");
    proto_zbee_ie = proto_register_protocol("ZigBee IE", "ZigBee IE", "zbee_ie");
    proto_register_field_array(proto_zbee_nwk, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    expert_zbee_nwk = expert_register_protocol(proto_zbee_nwk);
    expert_register_field_array(expert_zbee_nwk, ei, array_length(ei));

    /* Register the dissectors with Wireshark. */
    register_dissector(ZBEE_PROTOABBREV_NWK, dissect_zbee_nwk, proto_zbee_nwk);
    register_dissector("zbee_beacon", dissect_zbee_beacon, proto_zbee_beacon);
    register_dissector("zbip_beacon", dissect_zbip_beacon, proto_zbip_beacon);
    register_dissector("zbee_ie", dissect_zbee_ie, proto_zbee_ie);

    zbee_nwk_address_type = address_type_dissector_register("AT_ZIGBEE", "ZigBee 16-bit address",
            zbee_nwk_address_to_str, zbee_nwk_address_str_len, NULL, NULL, zbee_nwk_address_len, NULL, NULL);

    /* Register the Security dissector. */
    zbee_security_register(NULL, proto_zbee_nwk);

    zbee_nwk_tap = register_tap(ZBEE_PROTOABBREV_NWK);

    register_conversation_table(proto_zbee_nwk, true, zbee_nwk_conversation_packet, zbee_nwk_endpoint_packet);
    register_conversation_filter(ZBEE_PROTOABBREV_NWK, "ZigBee Network Layer", zbee_nwk_filter_valid, zbee_nwk_build_filter, NULL);
} /* proto_register_zbee_nwk */

/**
 *Registers the zigbee dissector with Wireshark.
 *
*/
void proto_reg_handoff_zbee_nwk(void)
{
    /* Find the other dissectors we need. */
    aps_handle      = find_dissector_add_dependency(ZBEE_PROTOABBREV_APS, proto_zbee_nwk);
    zbee_gp_handle  = find_dissector_add_dependency(ZBEE_PROTOABBREV_NWK_GP, proto_zbee_nwk);

    /* Register our dissector with IEEE 802.15.4 */
    dissector_add_for_decode_as(IEEE802154_PROTOABBREV_WPAN_PANID, find_dissector(ZBEE_PROTOABBREV_NWK));
    heur_dissector_add(IEEE802154_PROTOABBREV_WPAN_BEACON, dissect_zbee_beacon_heur, "ZigBee Beacon", "zbee_wpan_beacon", proto_zbee_beacon, HEURISTIC_ENABLE);
    heur_dissector_add(IEEE802154_PROTOABBREV_WPAN_BEACON, dissect_zbip_beacon_heur, "ZigBee IP Beacon", "zbip_wpan_beacon", proto_zbip_beacon, HEURISTIC_ENABLE);
    heur_dissector_add(IEEE802154_PROTOABBREV_WPAN, dissect_zbee_nwk_heur, "ZigBee Network Layer over IEEE 802.15.4", "zbee_nwk_wpan", proto_zbee_nwk, HEURISTIC_ENABLE);

    proto_ieee802154 = proto_get_id_by_filter_name(IEEE802154_PROTOABBREV_WPAN);

} /* proto_reg_handoff_zbee */

static void free_keyring_key(void *key)
{
    g_free(key);
}

static void free_keyring_val(void *a)
{
    GSList **slist = (GSList **)a;
    g_slist_free_full(*slist, g_free);
    g_free(slist);
}

/**
 *Init routine for the nwk dissector. Creates a
 *
*/
static void
proto_init_zbee_nwk(void)
{
    zbee_nwk_map.short_table = g_hash_table_new(ieee802154_short_addr_hash, ieee802154_short_addr_equal);
    zbee_nwk_map.long_table = g_hash_table_new(ieee802154_long_addr_hash, ieee802154_long_addr_equal);
    zbee_table_nwk_keyring  = g_hash_table_new_full(g_int_hash, g_int_equal, free_keyring_key, free_keyring_val);
} /* proto_init_zbee_nwk */

static void
proto_cleanup_zbee_nwk(void)
{
    g_hash_table_destroy(zbee_nwk_map.short_table);
    g_hash_table_destroy(zbee_nwk_map.long_table);
    g_hash_table_destroy(zbee_table_nwk_keyring);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
