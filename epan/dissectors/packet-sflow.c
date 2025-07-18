/* packet-sflow.c
 * Routines for sFlow v5 dissection implemented according to the specifications
 * at http://www.sflow.org/sflow_version_5.txt
 *
 * Additional 802.11 structures support implemented according to the
 * specifications at http://www.sflow.org/sflow_80211.txt
 *
 * By Yi Yu <yiyu.inbox@gmail.com>
 *
 * TODO:
 *   802.11 aggregation data dissection                         (sFlow v5)
 *
 *
 * Based on Jeff Rizzo's <riz@boogers.sf.ca.us> dissector for sFlow v2/4
 * in Wireshark 1.0.8 public release.
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 *
 * This file (mostly) implements a dissector for sFlow (RFC3176),
 * from the version 4 spec at http://www.sflow.org/SFLOW-DATAGRAM.txt .
 *
 * TODO:
 *   Fix the highlighting of the datastream when bits are selected
 *   split things out into packet-sflow.h ?
 *   make routines more consistent as to whether they return
 *     'offset' or bytes consumed ('len')                       (sFlow v2/4)
 *   implement sampled_ipv4 and sampled_ipv6 packet data types  (sFlow v2/4)
 *   implement extended_user                                    (sFlow v2/4)
 *   implement extended_url                                     (sFlow v2/4)
 *   implement non-generic counters sampling                    (sFlow v2/4)
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/exceptions.h>
#include <epan/prefs.h>
#include <epan/expert.h>
#include <epan/to_str.h>
#include <epan/etypes.h>
#include <epan/ipproto.h>
#include <epan/tfs.h>
#include <epan/unit_strings.h>

#include <wsutil/array.h>
#include <wsutil/ws_roundup.h>
#include <wsutil/ws_padding_to.h>

#include "packet-sflow.h"

#define SFLOW_UDP_PORTS "6343"

void proto_register_sflow(void);

static dissector_handle_t sflow_handle;

/*
 *  sflow_245_ports : holds the currently used range of ports for sflow
 */
static bool global_dissect_samp_headers = true;
static bool global_analyze_samp_ip_headers;

#define ENTERPRISE_DEFAULT 0

#define ADDR_TYPE_UNKNOWN 0
#define ADDR_TYPE_IPV4    1
#define ADDR_TYPE_IPV6    2

#define FLOWSAMPLE 1
#define COUNTERSSAMPLE 2
#define EXPANDED_FLOWSAMPLE 3
#define EXPANDED_COUNTERSSAMPLE 4
#define LAG_PORT_STATS 7

static const value_string sflow_agent_address_types[] = {
    { ADDR_TYPE_IPV4, "IPv4" },
    { ADDR_TYPE_IPV6, "IPv6" },
    { 0, NULL }
};

static const value_string sflow_245_sampletype[] = {
    { FLOWSAMPLE,              "Flow sample"},
    { COUNTERSSAMPLE,          "Counters sample"},
    { EXPANDED_FLOWSAMPLE,     "Expanded flow sample"},
    { EXPANDED_COUNTERSSAMPLE, "Expanded counters sample"},
    { LAG_PORT_STATS,          "Lag Port stats"},
    { 0, NULL}
};

#define SFLOW_5_IEEE80211_VERSION_A 1
#define SFLOW_5_IEEE80211_VERSION_B 2
#define SFLOW_5_IEEE80211_VERSION_G 3
#define SFLOW_5_IEEE80211_VERSION_N 4

static const value_string sflow_5_ieee80211_versions [] = {
    { SFLOW_5_IEEE80211_VERSION_A, "802.11a"},
    { SFLOW_5_IEEE80211_VERSION_B, "802.11b"},
    { SFLOW_5_IEEE80211_VERSION_G, "802.11g"},
    { SFLOW_5_IEEE80211_VERSION_N, "802.11n"},
    { 0, NULL}
};

/* interface counter types */
#define SFLOW_245_COUNTERS_GENERIC 1
#define SFLOW_245_COUNTERS_ETHERNET 2
#define SFLOW_245_COUNTERS_TOKENRING 3
#define SFLOW_245_COUNTERS_FDDI 4
#define SFLOW_245_COUNTERS_VG 5
#define SFLOW_245_COUNTERS_WAN 6
#define SFLOW_245_COUNTERS_VLAN 7

static const value_string sflow_245_counterstype[] = {
    { SFLOW_245_COUNTERS_GENERIC,  "Generic counters"},
    { SFLOW_245_COUNTERS_ETHERNET, "Ethernet counters"},
    { SFLOW_245_COUNTERS_TOKENRING,"Token Ring counters"},
    { SFLOW_245_COUNTERS_FDDI,     "FDDI counters"},
    { SFLOW_245_COUNTERS_VG,       "100baseVG counters"},
    { SFLOW_245_COUNTERS_WAN,      "WAN counters"},
    { SFLOW_245_COUNTERS_VLAN,     "VLAN counters"},
    { 0, NULL}
};

#define MAX_HEADER_SIZE 256

#define SFLOW_245_PACKET_DATA_TYPE_HEADER 1
#define SFLOW_245_PACKET_DATA_TYPE_IPV4 2
#define SFLOW_245_PACKET_DATA_TYPE_IPV6 3

static const value_string sflow_245_packet_information_type[] = {
    { SFLOW_245_PACKET_DATA_TYPE_HEADER, "Packet headers are sampled"},
    { SFLOW_245_PACKET_DATA_TYPE_IPV4,   "IP Version 4 data"},
    { SFLOW_245_PACKET_DATA_TYPE_IPV6,   "IP Version 6 data"},
    { 0, NULL}
};

static const value_string extended_80211_suite_type_vals[] = {
    { 0, "Use group cipher suite"},
    { 1, "WEP-40"},
    { 2, "TKIP"},
    { 4, "CCMP"},
    { 5, "WEP-104"},
    { 0, NULL}
};

static const value_string sflow_ifdirection_vals[] = {
    { 1, "Full-Duplex"},
    { 2, "Half-Duplex"},
    { 3, "In"},
    { 4, "Out"},
    { 0, NULL}
};

static const true_false_string tfs_minimize_monetary_normal = { "Minimize Monetary", "Normal" };

static const value_string sflow_245_header_protocol[] = {
    { SFLOW_245_HEADER_ETHERNET,           "Ethernet"},
    { SFLOW_245_HEADER_TOKENBUS,           "Token Bus"},
    { SFLOW_245_HEADER_TOKENRING,          "Token Ring"},
    { SFLOW_245_HEADER_FDDI,               "FDDI"},
    { SFLOW_245_HEADER_FRAME_RELAY,        "Frame Relay"},
    { SFLOW_245_HEADER_X25,                "X.25"},
    { SFLOW_245_HEADER_PPP,                "PPP"},
    { SFLOW_245_HEADER_SMDS,               "SMDS"},
    { SFLOW_245_HEADER_AAL5,               "ATM AAL5"},
    { SFLOW_245_HEADER_AAL5_IP,            "ATM AAL5-IP (e.g., Cisco AAL5 mux)"},
    { SFLOW_245_HEADER_IPv4,               "IPv4"},
    { SFLOW_245_HEADER_IPv6,               "IPv6"},
    { SFLOW_245_HEADER_MPLS,               "MPLS"},
    { SFLOW_5_HEADER_POS,                  "PPP over SONET/SDH (RFC 1662, 2615)"},
    { SFLOW_5_HEADER_80211_MAC,            "802.11 MAC"},
    { SFLOW_5_HEADER_80211_AMPDU,          "802.11n Aggregated MPDU"},
    { SFLOW_5_HEADER_80211_AMSDU_SUBFRAME, "A-MSDU Subframe"},
    { 0, NULL}
};
static value_string_ext sflow_245_header_protocol_ext = VALUE_STRING_EXT_INIT(sflow_245_header_protocol);

/* extended packet data types */
#define SFLOW_245_EXTENDED_SWITCH 1
#define SFLOW_245_EXTENDED_ROUTER 2
#define SFLOW_245_EXTENDED_GATEWAY 3
#define SFLOW_245_EXTENDED_USER 4
#define SFLOW_245_EXTENDED_URL 5

static const value_string sflow_245_extended_data_types[] = {
    { SFLOW_245_EXTENDED_SWITCH, "Extended switch information"},
    { SFLOW_245_EXTENDED_ROUTER, "Extended router information"},
    { SFLOW_245_EXTENDED_GATEWAY, "Extended gateway information"},
    { SFLOW_245_EXTENDED_USER, "Extended user information"},
    { SFLOW_245_EXTENDED_URL, "Extended URL information"},
    { 0, NULL}
};


#define SFLOW_245_AS_SET 1
#define SFLOW_245_AS_SEQUENCE 2

static const value_string sflow_245_as_types[] = {
    { SFLOW_245_AS_SET, "AS Set"},
    { SFLOW_245_AS_SEQUENCE, "AS Sequence"},
    { 0, NULL}
};

#define SFLOW_245_IPV4_PRECEDENCE_ROUTINE 0
#define SFLOW_245_IPV4_PRECEDENCE_PRIORITY 1
#define SFLOW_245_IPV4_PRECEDENCE_IMMEDIATE 2
#define SFLOW_245_IPV4_PRECEDENCE_FLASH 3
#define SFLOW_245_IPV4_PRECEDENCE_FLASH_OVERRIDE 4
#define SFLOW_245_IPV4_PRECEDENCE_CRITIC_ECP 5
#define SFLOW_245_IPV4_PRECEDENCE_INTERNETWORK_CONTROL 6
#define SFLOW_245_IPV4_PRECEDENCE_NETWORK_CONTROL 7

static const value_string sflow_245_ipv4_precedence_types[] = {
    { SFLOW_245_IPV4_PRECEDENCE_ROUTINE, "Routine"},
    { SFLOW_245_IPV4_PRECEDENCE_PRIORITY, "Priority"},
    { SFLOW_245_IPV4_PRECEDENCE_IMMEDIATE, "Immediate"},
    { SFLOW_245_IPV4_PRECEDENCE_FLASH, "Flash"},
    { SFLOW_245_IPV4_PRECEDENCE_FLASH_OVERRIDE, "Flash Override"},
    { SFLOW_245_IPV4_PRECEDENCE_CRITIC_ECP, "CRITIC/ECP"},
    { SFLOW_245_IPV4_PRECEDENCE_INTERNETWORK_CONTROL, "Internetwork Control"},
    { SFLOW_245_IPV4_PRECEDENCE_NETWORK_CONTROL, "Network Control"},
    { 0, NULL}
};

/* sFlow v5 flow record formats */
#define SFLOW_5_RAW_PACKET_HEADER    1
#define SFLOW_5_ETHERNET_FRAME       2
#define SFLOW_5_IPV4                 3
#define SFLOW_5_IPV6                 4
#define SFLOW_5_SWITCH            1001
#define SFLOW_5_ROUTER            1002
#define SFLOW_5_GATEWAY           1003
#define SFLOW_5_USER              1004
#define SFLOW_5_URL               1005
#define SFLOW_5_MPLS_DATA         1006
#define SFLOW_5_NAT               1007
#define SFLOW_5_MPLS_TUNNEL       1008
#define SFLOW_5_MPLS_VC           1009
#define SFLOW_5_MPLS_FEC          1010
#define SFLOW_5_MPLS_LVP_FEC      1011
#define SFLOW_5_VLAN_TUNNEL       1012
#define SFLOW_5_80211_PAYLOAD     1013
#define SFLOW_5_80211_RX          1014
#define SFLOW_5_80211_TX          1015
#define SFLOW_5_80211_AGGREGATION 1016


static const value_string sflow_5_flow_record_type[] = {
    { SFLOW_5_RAW_PACKET_HEADER, "Raw packet header"},
    { SFLOW_5_ETHERNET_FRAME,    "Ethernet frame data"},
    { SFLOW_5_IPV4,              "IPv4 data"},
    { SFLOW_5_IPV6,              "IPv6 data"},
    { SFLOW_5_SWITCH,            "Extended switch data"},
    { SFLOW_5_ROUTER,            "Extended router data"},
    { SFLOW_5_GATEWAY,           "Extended gateway data"},
    { SFLOW_5_USER,              "Extended user data"},
    { SFLOW_5_URL,               "Extended URL data"},
    { SFLOW_5_MPLS_DATA,         "Extended MPLS data"},
    { SFLOW_5_NAT,               "Extended NAT data"},
    { SFLOW_5_MPLS_TUNNEL,       "Extended MPLS tunnel data"},
    { SFLOW_5_MPLS_VC,           "Extended MPLS VC data"},
    { SFLOW_5_MPLS_FEC,          "Extended MPLS FEC data"},
    { SFLOW_5_MPLS_LVP_FEC,      "Extended MPLS LVP FEC data"},
    { SFLOW_5_VLAN_TUNNEL,       "Extended VLAN tunnel"},
    { SFLOW_5_80211_PAYLOAD,     "Extended 802.11 payload"},
    { SFLOW_5_80211_RX,          "Extended 802.11 RX"},
    { SFLOW_5_80211_TX,          "Extended 802.11 TX"},
    { SFLOW_5_80211_AGGREGATION, "Extended 802.11 aggregation"},
    { 0, NULL}
};
static value_string_ext sflow_5_flow_record_type_ext = VALUE_STRING_EXT_INIT(sflow_5_flow_record_type);

/* sFlow v5 counters record formats */
#define SFLOW_5_GENERIC_INTERFACE 1
#define SFLOW_5_ETHERNET_INTERFACE 2
#define SFLOW_5_TOKEN_RING 3
#define SFLOW_5_100BASE_VG_INTERFACE 4
#define SFLOW_5_VLAN 5
#define SFLOW_5_80211_COUNTERS 6
#define SFLOW_5_LAG 7
#define SFLOW_5_PROCESSOR 1001
#define SFLOW_5_RADIO_UTILIZATION 1002

static const value_string sflow_5_counters_record_type[] = {
    { SFLOW_5_GENERIC_INTERFACE,    "Generic interface counters"},
    { SFLOW_5_ETHERNET_INTERFACE,   "Ethernet interface counters"},
    { SFLOW_5_TOKEN_RING,           "Token ring counters"},
    { SFLOW_5_100BASE_VG_INTERFACE, "100 Base VG interface counters"},
    { SFLOW_5_VLAN,                 "VLAN counters"},
    { SFLOW_5_LAG,                  "LAG counters"},
    { SFLOW_5_80211_COUNTERS,       "IEEE 802.11 counters"},
    { SFLOW_5_PROCESSOR,            "Processor information"},
    { SFLOW_5_RADIO_UTILIZATION,    "Radio utilization"},
    { 0, NULL}
};

/* sFlow v5 interface formats */
#define SFLOW_5_INT_FORMAT 0xC0000000
#define SFLOW_5_INT_VALUE  0x3FFFFFFF

#define SFLOW_5_INT_FORMAT_IFINDEX  0
#define SFLOW_5_INT_FORMAT_DISCARD  1
#define SFLOW_5_INT_FORMAT_MULTIPLE 2

static const value_string interface_format[] = {
    { SFLOW_5_INT_FORMAT_IFINDEX,       "ifindex"},
    { SFLOW_5_INT_FORMAT_DISCARD,       "packet discarded"},
    { SFLOW_5_INT_FORMAT_MULTIPLE,      "multiple interfaces"},
    { 0, NULL}
};

static const value_string interface_discard[] = {
    {   0, "Net Unreachable"},
    {   1, "Host Unreachable"},
    {   2, "Protocol Unreachable"},
    {   3, "Port Unreachable"},
    {   4, "Fragmentation Needed and Don't Fragment was Set"},
    {   5, "Source Route Failed"},
    {   6, "Destination Network Unknown"},
    {   7, "Destination Host Unknown"},
    {   8, "Source Host Isolated"},
    {   9, "Communication with Destination Network is Administratively Prohibited"},
    {  10, "Communication with Destination Host is Administratively Prohibited"},
    {  11, "Destination Network Unreachable for Type of Service"},
    {  12, "Destination Host Unreachable for Type of Service"},
    {  13, "Communication Administratively Prohibited"},
    {  14, "Host Precedence Violation"},
    {  15, "Precedence cutoff in effect"},
    { 256, "unknown"},
    { 257, "ttl exceeded"},
    { 258, "ACL"},
    { 259, "no buffer space"},
    { 260, "RED"},
    { 261, "traffic shaping/rate limiting"},
    { 262, "packet too big (for protocols that don't support fragmentation)"},
    { 263, "Source MAC is multicast"},
    { 264, "VLAN tag mismatch"},
    { 265, "Ingress VLAN filter"},
    { 266, "Ingress spanning tree filter"},
    { 267, "Port list is empty"},
    { 268, "Port loopback filter"},
    { 269, "Blackhole route"},
    { 270, "Non IP"},
    { 271, "Unicast destination IP over multicast destination MAC"},
    { 272, "Destination IP is loopback address"},
    { 273, "Source IP is multicast"},
    { 274, "Source IP is looback address"},
    { 275, "IP header corrupted"},
    { 276, "IPv4 source address is limited broadcast"},
    { 277, "IPv6 multicast destination IP reserved scope"},
    { 278, "IPv6 multicast destination IP interface local scope"},
    { 279, "Unresolved neighbor"},
    { 280, "Multicast reverse path forwarding"},
    { 281, "Non routable packet"},
    { 282, "Decap error"},
    { 283, "Overlay source MAC is multicast"},
    { 284, "Unknown L2"},
    { 285, "Unknown L3"},
    { 286, "Unknown L3 exception"},
    { 287, "Unknown buffer"},
    { 288, "Unknown tunnel"},
    { 289, "Unknown L4"},
    { 290, "Source IP in unspecified"},
    { 291, "Mlag port isolation"},
    { 292, "Blackhole ARP neighbor"},
    { 293, "Source MAC is destination MAC"},
    { 294, "Destination MAC is reserved"},
    { 295, "Source IP class E"},
    { 296, "Multicast destination MAC mismatch"},
    { 297, "Source IP is destination IP"},
    { 298, "Destination IP is local network"},
    { 299, "Destination IP is link local"},
    { 300, "Overlay source MAC is destination MAC"},
    { 301, "Egress VLAN filter"},
    { 302, "Unicast reverse path forwarding"},
    { 303, "Split horizon"},
    { 304, "locked_port"},
    { 305, "dmac_filter"},
    { 306, "blackhole_nexthop"},
    { 307, "vxlan_parsing"},
    { 308, "llc_snap_parsing"},
    { 309, "vlan_parsing"},
    { 310, "pppoe_ppp_parsing"},
    { 311, "mpls_parsing"},
    { 312, "arp_parsing"},
    { 313, "ip_1_parsing"},
    { 314, "ip_n_parsing"},
    { 315, "gre_parsing"},
    { 316, "udp_parsing"},
    { 317, "tcp_parsing"},
    { 318, "ipsec_parsing"},
    { 319, "sctp_parsing"},
    { 320, "dccp_parsing"},
    { 321, "gtp_parsing"},
    { 322, "esp_parsing"},
    { 323, "unknown_parsing"},
    { 324, "pkt_too_small"},
    { 325, "unhandled_proto"},
    { 326, "ipv6disabled"},
    { 327, "invalid_proto"},
    { 328, "ip_noproto"},
    { 329, "skb_csum"},
    { 330, "skb_ucopy_fault"},
    { 331, "dev_ready"},
    { 332, "dev_hdr"},
    { 333, "dup_frag"},
    { 334, "skb_gso_seg"},
    { 335, "reverse_path_forwarding"},
    { 336, "icmp_parsing"},
    { 337, "tcp_md5notfound"},
    { 338, "tcp_md5unexpected"},
    { 339, "tcp_md5failure"},
    { 340, "tcp_flags"},
    { 341, "tcp_zerowindow"},
    { 342, "tcp_old_data"},
    { 343, "tcp_overwindow"},
    { 344, "tcp_ofomerge"},
    { 345, "tcp_rfc7323_paws"},
    { 346, "tcp_invalid_sequence"},
    { 347, "tcp_reset"},
    { 348, "tcp_invalid_syn"},
    { 349, "tcp_close"},
    { 350, "tcp_fastopen"},
    { 351, "tcp_old_ack"},
    { 352, "tcp_too_old_ack"},
    { 353, "tcp_ack_unsent_data"},
    { 354, "tcp_ofo_queue_prune"},
    { 355, "tcp_ofo_drop"},
    { 356, "tcp_minttl"},
    { 357, "ipv6_bad_exthdr"},
    { 358, "ipv6_ndisc_frag"},
    { 359, "ipv6_ndisc_hop_limit"},
    { 360, "ipv6_ndisc_bad_code"},
    { 361, "ipv6_ndisc_bad_options"},
    { 362, "ipv6_ndisc_ns_otherhost"},
    { 363, "tap_filter"},
    { 364, "tap_txfilter"},
    { 365, "tc_ingress"},
    { 366, "tc_egress"},
    { 367, "xdp"},
    { 368, "cpu_backlog"},
    { 369, "bpf_cgroup_egress"},
    { 370, "xfrm_policy"},
    { 371, "socket_filter"},
    { 372, "bgp_flowspec"},
    { 0, NULL}
};

/* ethernet counters.  These will be preceded by generic counters. */
struct ethernet_counters {
    uint32_t dot3StatsAlignmentErrors;
    uint32_t dot3StatsFCSErrors;
    uint32_t dot3StatsSingleCollisionFrames;
    uint32_t dot3StatsMultipleCollisionFrames;
    uint32_t dot3StatsSQETestErrors;
    uint32_t dot3StatsDeferredTransmissions;
    uint32_t dot3StatsLateCollisions;
    uint32_t dot3StatsExcessiveCollisions;
    uint32_t dot3StatsInternalMacTransmitErrors;
    uint32_t dot3StatsCarrierSenseErrors;
    uint32_t dot3StatsFrameTooLongs;
    uint32_t dot3StatsInternalMacReceiveErrors;
    uint32_t dot3StatsSymbolErrors;
};

struct sflow_address_type {
    int hf_addr_v4;
    int hf_addr_v6;
};


/* Initialize the protocol and registered fields */
static int proto_sflow;
static int hf_sflow_version;
static int hf_sflow_agent_address_type;
static int hf_sflow_agent_address_v4;
static int hf_sflow_agent_address_v6;
static int hf_sflow_5_sub_agent_id;
static int hf_sflow_5_sample_length;
static int hf_sflow_5_flow_data_length;
/* static int hf_sflow_5_counters_data_length; */
static int hf_sflow_245_seqnum;
static int hf_sflow_245_sysuptime;
static int hf_sflow_245_numsamples;
static int hf_sflow_245_header_protocol;
static int hf_sflow_245_sampletype;
static int hf_sflow_245_sampletype12;
static int hf_sflow_245_ipv4_precedence_type;
static int hf_sflow_5_flow_record_format;
static int hf_sflow_5_counters_record_format;
static int hf_sflow_245_header;
static int hf_sflow_245_packet_information_type;
static int hf_sflow_245_extended_information_type;
static int hf_sflow_245_vlan_in; /* incoming 802.1Q VLAN ID */
static int hf_sflow_245_vlan_out; /* outgoing 802.1Q VLAN ID */
static int hf_sflow_245_pri_in; /* incoming 802.1p priority */
static int hf_sflow_245_pri_out; /* outgoing 802.1p priority */
static int hf_sflow_245_nexthop_v4; /* nexthop address */
static int hf_sflow_245_nexthop_v6; /* nexthop address */
static int hf_sflow_245_ipv4_src;
static int hf_sflow_245_ipv4_dst;
static int hf_sflow_245_ipv6_src;
static int hf_sflow_245_ipv6_dst;
static int hf_sflow_245_nexthop_src_mask;
static int hf_sflow_245_nexthop_dst_mask;


/* extended gateway (all versions) */
static int hf_sflow_245_as;
static int hf_sflow_245_src_as;
static int hf_sflow_245_src_peer_as;
static int hf_sflow_245_dst_as_entries; /* aka length */
static int hf_sflow_245_dst_as;
/* extended gateway (>= version 4) */
static int hf_sflow_245_community_entries;
/* static int hf_sflow_245_community; */
static int hf_sflow_245_localpref;

/* generic interface counter */
static int hf_sflow_245_ifindex;
static int hf_sflow_245_iftype;
static int hf_sflow_245_ifspeed;
static int hf_sflow_245_ifdirection;
static int hf_sflow_245_ifadmin_status;
static int hf_sflow_245_ifoper_status;
static int hf_sflow_245_ifinoct;
static int hf_sflow_245_ifinpkt;
static int hf_sflow_245_ifinmcast;
static int hf_sflow_245_ifinbcast;
static int hf_sflow_245_ifinerr;
static int hf_sflow_245_ifindisc;
static int hf_sflow_245_ifinunk;
static int hf_sflow_245_ifoutoct;
static int hf_sflow_245_ifoutpkt;
static int hf_sflow_245_ifoutmcast;
static int hf_sflow_245_ifoutbcast;
static int hf_sflow_245_ifoutdisc;
static int hf_sflow_245_ifouterr;
static int hf_sflow_245_ifpromisc;

/* ethernet interface counter */
static int hf_sflow_245_dot3StatsAlignmentErrors;
static int hf_sflow_245_dot3StatsFCSErrors;
static int hf_sflow_245_dot3StatsSingleCollisionFrames;
static int hf_sflow_245_dot3StatsMultipleCollisionFrames;
static int hf_sflow_245_dot3StatsSQETestErrors;
static int hf_sflow_245_dot3StatsDeferredTransmissions;
static int hf_sflow_245_dot3StatsLateCollisions;
static int hf_sflow_245_dot3StatsExcessiveCollisions;
static int hf_sflow_245_dot3StatsInternalMacTransmitErrors;
static int hf_sflow_245_dot3StatsCarrierSenseErrors;
static int hf_sflow_245_dot3StatsFrameTooLongs;
static int hf_sflow_245_dot3StatsInternalMacReceiveErrors;
static int hf_sflow_245_dot3StatsSymbolErrors;

/* token ring counter */
static int hf_sflow_245_dot5StatsLineErrors;
static int hf_sflow_245_dot5StatsBurstErrors;
static int hf_sflow_245_dot5StatsACErrors;
static int hf_sflow_245_dot5StatsAbortTransErrors;
static int hf_sflow_245_dot5StatsInternalErrors;
static int hf_sflow_245_dot5StatsLostFrameErrors;
static int hf_sflow_245_dot5StatsReceiveCongestions;
static int hf_sflow_245_dot5StatsFrameCopiedErrors;
static int hf_sflow_245_dot5StatsTokenErrors;
static int hf_sflow_245_dot5StatsSoftErrors;
static int hf_sflow_245_dot5StatsHardErrors;
static int hf_sflow_245_dot5StatsSignalLoss;
static int hf_sflow_245_dot5StatsTransmitBeacons;
static int hf_sflow_245_dot5StatsRecoveries;
static int hf_sflow_245_dot5StatsLobeWires;
static int hf_sflow_245_dot5StatsRemoves;
static int hf_sflow_245_dot5StatsSingles;
static int hf_sflow_245_dot5StatsFreqErrors;

/* 100 BaseVG interface counters */
static int hf_sflow_245_dot12InHighPriorityFrames;
static int hf_sflow_245_dot12InHighPriorityOctets;
static int hf_sflow_245_dot12InNormPriorityFrames;
static int hf_sflow_245_dot12InNormPriorityOctets;
static int hf_sflow_245_dot12InIPMErrors;
static int hf_sflow_245_dot12InOversizeFrameErrors;
static int hf_sflow_245_dot12InDataErrors;
static int hf_sflow_245_dot12InNullAddressedFrames;
static int hf_sflow_245_dot12OutHighPriorityFrames;
static int hf_sflow_245_dot12OutHighPriorityOctets;
static int hf_sflow_245_dot12TransitionIntoTrainings;
static int hf_sflow_245_dot12HCInHighPriorityOctets;
static int hf_sflow_245_dot12HCInNormPriorityOctets;
static int hf_sflow_245_dot12HCOutHighPriorityOctets;

/* VLAN counters */
static int hf_sflow_245_vlan_id;
static int hf_sflow_245_octets;
static int hf_sflow_245_ucastPkts;
static int hf_sflow_245_multicastPkts;
static int hf_sflow_245_broadcastPkts;
static int hf_sflow_245_discards;

/* 802.11 interface counters */
static int hf_sflow_5_dot11TransmittedFragmentCount;
static int hf_sflow_5_dot11MulticastTransmittedFrameCount;
static int hf_sflow_5_dot11FailedCount;
static int hf_sflow_5_dot11RetryCount;
static int hf_sflow_5_dot11MultipleRetryCount;
static int hf_sflow_5_dot11FrameDuplicateCount;
static int hf_sflow_5_dot11RTSSuccessCount;
static int hf_sflow_5_dot11RTSFailureCount;
static int hf_sflow_5_dot11ACKFailureCount;
static int hf_sflow_5_dot11ReceivedFragmentCount;
static int hf_sflow_5_dot11MulticastReceivedFrameCount;
static int hf_sflow_5_dot11FCSErrorCount;
static int hf_sflow_5_dot11TransmittedFrameCount;
static int hf_sflow_5_dot11WEPUndecryptableCount;
static int hf_sflow_5_dot11QoSDiscardedFragmentCount;
static int hf_sflow_5_dot11AssociatedStationCount;
static int hf_sflow_5_dot11QoSCFPollsReceivedCount;
static int hf_sflow_5_dot11QoSCFPollsUnusedCount;
static int hf_sflow_5_dot11QoSCFPollsUnusableCount;
static int hf_sflow_5_dot11QoSCFPollsLostCount;
/* static int hf_sflow_5_ieee80211_version; */


/* processor information */
static int hf_sflow_5_cpu_5s;
static int hf_sflow_5_cpu_1m;
static int hf_sflow_5_cpu_5m;
static int hf_sflow_5_total_memory;
static int hf_sflow_5_free_memory;

/* radio utilisation */
static int hf_sflow_5_elapsed_time;
static int hf_sflow_5_on_channel_time;
static int hf_sflow_5_on_channel_busy_time;

/* Generated from convert_proto_tree_add_text.pl */
static int hf_sflow_5_extended_80211_suite_type;
static int hf_sflow_5_extended_80211_rx_channel;
static int hf_sflow_flow_sample_input_interface;
static int hf_sflow_counters_sample_sampling_interval;
static int hf_sflow_5_extended_url_host_length;
static int hf_sflow_245_ip_tcp_flag_syn;
static int hf_sflow_24_flow_sample_output_interface;
static int hf_sflow_5_flow_sample_output_interface;
static int hf_sflow_5_flow_sample_output_interface_form;
static int hf_sflow_5_flow_sample_output_interface_val;
static int hf_sflow_5_flow_sample_output_interface_val_discard;
static int hf_sflow_245_length_of_ip_packet;
static int hf_sflow_counters_sample_counters_type;
static int hf_sflow_5_extended_mpls_tunnel_id;
static int hf_sflow_flow_sample_sample_pool;
static int hf_sflow_5_extended_80211_tx_speed;
static int hf_sflow_5_extended_vlan_tunnel_tpid_tci_pair;
static int hf_sflow_245_extended_mpls_out_label_stack_entries;
static int hf_sflow_flow_sample_input_interface_value;
static int hf_sflow_flow_sample_sampling_rate;
static int hf_sflow_5_extended_80211_rx_rcpi;
static int hf_sflow_enterprise;
static int hf_sflow_enterprise_length;
static int hf_sflow_enterprise_data;
static int hf_sflow_245_header_frame_length;
static int hf_sflow_5_extended_user_destination_character_set;
static int hf_sflow_5_extended_80211_rx_bssid;
static int hf_sflow_5_extended_80211_tx_retransmission_duration;
static int hf_sflow_245_ethernet_length_of_mac_packet;
static int hf_sflow_245_ip_tcp_flag_psh;
static int hf_sflow_flow_sample_flow_record;
static int hf_sflow_245_extended_mpls_in_label;
static int hf_sflow_5_extended_user_source_character_set;
static int hf_sflow_5_extended_user_destination_user_string_length;
static int hf_sflow_counters_sample_sequence_number;
static int hf_sflow_5_extended_80211_rx_speed;
static int hf_sflow_5_extended_80211_rx_rsni;
static int hf_sflow_flow_sample_source_id_index;
static int hf_sflow_245_ip_tcp_flag_ece;
static int hf_sflow_245_ipv4_throughput;
static int hf_sflow_5_extended_80211_oui;
static int hf_sflow_counters_sample_source_id_type;
static int hf_sflow_flow_sample_input_interface_format;
static int hf_sflow_5_extended_80211_tx_channel;
static int hf_sflow_245_ip_tcp_flag_urg;
static int hf_sflow_5_extended_mpls_tunnel_name_length;
static int hf_sflow_5_extended_80211_tx_version;
static int hf_sflow_245_ipv4_delay;
static int hf_sflow_flow_sample_source_id_class;
static int hf_sflow_245_ethernet_source_mac_address;
static int hf_sflow_5_extended_mpls_ftn_mask;
static int hf_sflow_245_extended_mpls_out_label;
static int hf_sflow_245_ipv6_priority;
static int hf_sflow_245_ip_tcp_flag_fin;
static int hf_sflow_245_ip_destination_port;
static int hf_sflow_5_extended_mpls_vc_label_cos_value;
static int hf_sflow_5_extended_80211_rx_packet_duration;
static int hf_sflow_5_extended_80211_tx_packet_duration;
static int hf_sflow_245_ipv4_reliability;
static int hf_sflow_5_extended_80211_tx_power;
static int hf_sflow_24_flow_sample_multiple_outputs;
static int hf_sflow_5_extended_user_source_user_string_length;
static int hf_sflow_5_extended_80211_payload_length;
static int hf_sflow_5_flow_sample_output_interface_expanded_format;
static int hf_sflow_245_ethernet_packet_type;
static int hf_sflow_counters_sample_expanded_source_id_type;
static int hf_sflow_245_ip_source_port;
static int hf_sflow_245_extended_mpls_in_label_stack_entries;
static int hf_sflow_5_extended_mpls_vc_instance_name_length;
static int hf_sflow_245_ipv4_cost;
static int hf_sflow_5_extended_mpls_ftn_description_length;
static int hf_sflow_5_extended_vlan_tunnel_number_of_layers;
static int hf_sflow_5_extended_80211_tx_bssid;
static int hf_sflow_245_ip_tcp_flag_rst;
static int hf_sflow_245_ip_tcp_flag_ack;
static int hf_sflow_245_ip_tcp_flag_cwr;
static int hf_sflow_5_extended_80211_tx_retransmissions;
static int hf_sflow_5_extended_80211_rx_version;
static int hf_sflow_flow_sample_dropped_packets;
static int hf_sflow_counters_sample_expanded_source_id_index;
static int hf_sflow_245_header_payload_stripped;
static int hf_sflow_245_sampled_header_length;
static int hf_sflow_245_ethernet_destination_mac_address;
static int hf_sflow_counters_sample_source_id_class;
static int hf_sflow_5_extended_url_url_length;
static int hf_sflow_flow_sample_source_id_type;
static int hf_sflow_5_extended_mpls_fec_address_prefix_length;
static int hf_sflow_flow_sample_sequence_number;
static int hf_sflow_counters_sample_source_id_index;
static int hf_sflow_counters_sample_counters_records;
static int hf_sflow_5_extended_mpls_tunnel_cos_value;
static int hf_sflow_5_extended_mpls_vc_id;
static int hf_sflow_24_flow_sample_output_interface_value;
static int hf_sflow_5_flow_sample_output_interface_expanded_value;
static int hf_sflow_5_flow_sample_output_interface_expanded_value_discarded;
static int hf_sflow_5_flow_sample_output_interface_expanded_value_number;
static int hf_sflow_5_flow_sample_output_interface_expanded_value_ifindex;
static int hf_sflow_5_extended_user_destination_user;
static int hf_sflow_245_as_type;
static int hf_sflow_counters_sample_index;
static int hf_sflow_5_extended_url_url;
static int hf_sflow_flow_sample_index;
static int hf_sflow_5_extended_80211_rx_ssid;
static int hf_sflow_5_extended_mpls_vc_instance_name;
static int hf_sflow_5_extended_mpls_tunnel_name;
static int hf_sflow_5_extended_80211_payload;
static int hf_sflow_5_extended_user_source_user;
static int hf_sflow_5_extended_url_host;
static int hf_sflow_5_extended_80211_tx_ssid;
static int hf_sflow_5_extended_url_direction;
static int hf_sflow_5_extended_mpls_ftn_description;
static int hf_sflow_245_ip_protocol;

static int hf_sflow_lag_port_padding;
static int hf_sflow_lag_port_actorsystemid;
static int hf_sflow_lag_port_partneropersystemid;
static int hf_sflow_lag_port_attachedaggid;
static int hf_sflow_lag_port_state;
static int hf_sflow_lag_port_actoradminstate;
static int hf_sflow_lag_port_actoroperstate;
static int hf_sflow_lag_port_partneradminstate;
static int hf_sflow_lag_port_partneroperstate;
static int hf_sflow_lag_port_reserved;
static int hf_sflow_5_lag_port_actoradminstate;
static int hf_sflow_5_lag_port_actoroperstate;
static int hf_sflow_5_lag_port_partneradminstate;
static int hf_sflow_5_lag_port_partneroperstate;
static int hf_sflow_lag_port_stats_lacpdusrx;
static int hf_sflow_lag_port_stats_markerpdusrx;
static int hf_sflow_lag_port_stats_markerresponsepdusrx;
static int hf_sflow_lag_port_stats_unknownrx;
static int hf_sflow_lag_port_stats_illegalrx;
static int hf_sflow_lag_port_stats_lacpdustx;
static int hf_sflow_lag_port_stats_markerpdustx;
static int hf_sflow_lag_port_stats_markerresponsepdustx;

/* Initialize the subtree pointers */
static int ett_sflow_245;
static int ett_sflow_245_sample;
static int ett_sflow_5_flow_record;
static int ett_sflow_5_counters_record;
static int ett_sflow_5_mpls_in_label_stack;
static int ett_sflow_5_mpls_out_label_stack;
static int ett_sflow_245_extended_data;
static int ett_sflow_245_gw_as_dst;
static int ett_sflow_245_gw_as_dst_seg;
static int ett_sflow_245_gw_community;
static int ett_sflow_245_sampled_header;
static int ett_sflow_lag_port_state_flags;
static int ett_sflow_5_output_interface;

static expert_field ei_sflow_invalid_address_type;

static dissector_table_t   header_subdissector_table;

static const unit_name_string units_total_packets = { " total packet", " total packets" };

void proto_reg_handoff_sflow_245(void);

/* dissect a sampled header - layer 2 protocols */
static int
dissect_sflow_245_sampled_header(tvbuff_t *tvb, packet_info *pinfo,
                                 proto_tree *tree, volatile int offset) {
    uint32_t          version, header_proto, frame_length;
    uint32_t header_length;
    tvbuff_t         *next_tvb;
    proto_tree       *sflow_245_header_tree;
    proto_item       *ti;
    /* stuff for saving column state before calling other dissectors.
     * Thanks to Guy Harris for the tip. */
    bool              save_writable;
    bool              save_in_error_pkt;
    address           save_dl_src, save_dl_dst, save_net_src, save_net_dst, save_src, save_dst;

    version = tvb_get_ntohl(tvb, 0);
    header_proto = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_245_header_protocol, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    frame_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_245_header_frame_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    if (version == 5) {
        proto_tree_add_item(tree, hf_sflow_245_header_payload_stripped, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }

    proto_tree_add_item_ret_uint(tree, hf_sflow_245_sampled_header_length, tvb, offset, 4, ENC_BIG_ENDIAN, &header_length);
    offset += 4;

    ti = proto_tree_add_item(tree, hf_sflow_245_header, tvb, offset, header_length, ENC_NA);
    sflow_245_header_tree = proto_item_add_subtree(ti, ett_sflow_245_sampled_header);

    /* hand the header off to the appropriate dissector.  It's probably
     * a short frame, so ignore any exceptions. */
    next_tvb = tvb_new_subset_length_caplen(tvb, offset, header_length, frame_length);

    /* save some state */
    save_writable = col_get_writable(pinfo->cinfo, -1);

    /*
       If sFlow samples a TCP packet it is very likely that the
       TCP analysis will flag the packet as having some error with
       the sequence numbers.  sFlow only report on a "sample" of
       traffic so many packets will not be reported on.  This is
       most obvious if the colorizing rules are on, but will also
       cause confusion if you attempt to filter on
       "tcp.analysis.flags".

       The following only works to suppress IP/TCP errors, but
       it is a start anyway.  Other protocols carried as payloads
       may exhibit similar issues.

       I think what is really needed is a more general
       "protocol_as_payload" flag.  Of course then someone has to
       play whack-a-mole and add code to implement it to any
       protocols that could be carried as a payload.  In the case
       of sFlow that pretty much means anything on your network.
     */
    save_in_error_pkt = pinfo->flags.in_error_pkt;
    if (!global_analyze_samp_ip_headers) {
        pinfo->flags.in_error_pkt = true;
    }

    col_set_writable(pinfo->cinfo, -1, false);
    copy_address_shallow(&save_dl_src, &pinfo->dl_src);
    copy_address_shallow(&save_dl_dst, &pinfo->dl_dst);
    copy_address_shallow(&save_net_src, &pinfo->net_src);
    copy_address_shallow(&save_net_dst, &pinfo->net_dst);
    copy_address_shallow(&save_src, &pinfo->src);
    copy_address_shallow(&save_dst, &pinfo->dst);

    TRY
    {
        if ((global_dissect_samp_headers == false) ||
            !dissector_try_uint(header_subdissector_table, header_proto, next_tvb, pinfo, sflow_245_header_tree))
        {
            call_data_dissector(next_tvb, pinfo, sflow_245_header_tree);
        }
    }

    CATCH_BOUNDS_ERRORS {
    }
    ENDTRY;

    /* restore saved state */
    col_set_writable(pinfo->cinfo, -1, save_writable);
    pinfo->flags.in_error_pkt = save_in_error_pkt;
    copy_address_shallow(&pinfo->dl_src, &save_dl_src);
    copy_address_shallow(&pinfo->dl_dst, &save_dl_dst);
    copy_address_shallow(&pinfo->net_src, &save_net_src);
    copy_address_shallow(&pinfo->net_dst, &save_net_dst);
    copy_address_shallow(&pinfo->src, &save_src);
    copy_address_shallow(&pinfo->dst, &save_dst);

    /* XDR requires 4-byte alignment */
    offset += WS_ROUNDUP_4(header_length);
    return offset;
}

static int
dissect_sflow_245_address_type(tvbuff_t *tvb, packet_info *pinfo,
                               proto_tree *tree, int offset,
                               struct sflow_address_type *hf_type,
                               address *addr) {
    uint32_t addr_type;
    int len;

    addr_type = tvb_get_ntohl(tvb, offset);
    offset += 4;

    switch (addr_type) {
    case ADDR_TYPE_UNKNOWN:
        len = 0;
        break;
    case ADDR_TYPE_IPV4:
        len = 4;
        proto_tree_add_item(tree, hf_type->hf_addr_v4, tvb, offset, 4, ENC_BIG_ENDIAN);
        break;
    case ADDR_TYPE_IPV6:
        len = 16;
        proto_tree_add_item(tree, hf_type->hf_addr_v6, tvb, offset, 16, ENC_NA);
        break;
    default:
        /* Invalid address type, or a type we don't understand; we don't
           know the length. We treat it as having no contents; that
           doesn't trap us in an endless loop, as we at least include
           the address type and thus at least advance the offset by 4.
           Note that we have a problem, though. */
        len = 0;
        proto_tree_add_expert_format(tree, pinfo, &ei_sflow_invalid_address_type, tvb,
                                     offset - 4, 4, "Unknown address type (%u)", addr_type);
    }

    if (addr) {
        switch (len) {
        default:
            clear_address(addr);
            break;
        case 4:
            set_address_tvb(addr, AT_IPv4, len, tvb, offset);
            break;
        case 16:
            set_address_tvb(addr, AT_IPv6, len, tvb, offset);
            break;
        }
    }

    return offset + len;
}

/* extended switch data, after the packet data */
static int
dissect_sflow_245_extended_switch(tvbuff_t *tvb, proto_tree *tree, int offset) {
    proto_tree_add_item(tree, hf_sflow_245_vlan_in, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_245_pri_in, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_245_vlan_out, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_245_pri_out, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* extended router data, after the packet data */
static int
dissect_sflow_245_extended_router(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset) {
    struct sflow_address_type addr_type;

    addr_type.hf_addr_v4 = hf_sflow_245_nexthop_v4;
    addr_type.hf_addr_v6 = hf_sflow_245_nexthop_v6;

    offset = dissect_sflow_245_address_type(tvb, pinfo, tree, offset, &addr_type, NULL);
    proto_tree_add_item(tree, hf_sflow_245_nexthop_src_mask, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_245_nexthop_dst_mask, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    return offset;
}

/* extended MPLS data */
static int
dissect_sflow_5_extended_mpls_data(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset) {
    uint32_t    in_label_count, out_label_count, label, i, j;
    proto_tree *in_stack;
    proto_tree *out_stack;
    struct sflow_address_type addr_type;

    addr_type.hf_addr_v4 = hf_sflow_245_nexthop_v4;
    addr_type.hf_addr_v6 = hf_sflow_245_nexthop_v6;

    offset = dissect_sflow_245_address_type(tvb, pinfo, tree, offset, &addr_type, NULL);

    in_label_count = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_245_extended_mpls_in_label_stack_entries, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    in_stack = proto_tree_add_subtree(tree, tvb, offset, -1, ett_sflow_5_mpls_in_label_stack, NULL, "In Label Stack");

    /* by applying the mask, we avoid possible corrupted data that causes huge number of loops
     * 255 is a sensible limit of label count */
    for (i = 0, j = 0; i < (in_label_count & 0x000000ff); i++, j += 4) {
        label = tvb_get_ntohl(tvb, offset + j);
        proto_tree_add_uint_format(in_stack, hf_sflow_245_extended_mpls_in_label, tvb, offset, 4,
            label, "Label %u: %u", i + 1, label);
    }
    offset += (in_label_count * 4);

    out_label_count = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_245_extended_mpls_out_label_stack_entries, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    out_stack = proto_tree_add_subtree(tree, tvb, offset, -1, ett_sflow_5_mpls_in_label_stack, NULL, "Out Label Stack");

    /* by applying the mask, we avoid possible corrupted data that causes huge number of loops
     * 255 is a sensible limit of label count */
    for (i = 0, j = 0; i < (out_label_count & 0x000000ff); i++, j += 4) {
        label = tvb_get_ntohl(tvb, offset + j);
        proto_tree_add_uint_format(out_stack, hf_sflow_245_extended_mpls_out_label, tvb, offset, 4,
            label, "Label %u: %u", i + 1, label);
    }
    offset = offset + out_label_count * 4;

    return offset;
}

/* extended NAT data */
static int
dissect_sflow_5_extended_nat(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset) {
    struct sflow_address_type addr_type;

    addr_type.hf_addr_v4 = hf_sflow_245_ipv4_src;
    addr_type.hf_addr_v6 = hf_sflow_245_ipv6_src;

    offset = dissect_sflow_245_address_type(tvb, pinfo, tree, offset, &addr_type, NULL);

    addr_type.hf_addr_v4 = hf_sflow_245_ipv4_dst;
    addr_type.hf_addr_v6 = hf_sflow_245_ipv6_dst;

    offset = dissect_sflow_245_address_type(tvb, pinfo, tree, offset, &addr_type, NULL);

    return offset;
}

/* extended gateway data, after the packet data */
static int
dissect_sflow_245_extended_gateway(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset) {
    int32_t len = 0;
    int32_t i, j, comm_len, dst_len, dst_seg_len;
    uint32_t path_type;
    int32_t kludge;

    uint32_t version = tvb_get_ntohl(tvb, 0); /* get sFlow version */
    proto_item *ti;
    proto_tree *sflow_245_dst_as_tree;
    proto_tree *sflow_245_comm_tree;
    proto_tree *sflow_245_dst_as_seg_tree;

    /* sFlow v5 contains next hop router IP address */
    if (version == 5) {
        struct sflow_address_type addr_type;

        addr_type.hf_addr_v4 = hf_sflow_245_nexthop_v4;
        addr_type.hf_addr_v6 = hf_sflow_245_nexthop_v6;

        offset = dissect_sflow_245_address_type(tvb, pinfo, tree, offset, &addr_type, NULL);
    }

    proto_tree_add_item(tree, hf_sflow_245_as, tvb, offset + len, 4, ENC_BIG_ENDIAN);
    len += 4;

    proto_tree_add_item(tree, hf_sflow_245_src_as, tvb, offset + len, 4, ENC_BIG_ENDIAN);
    len += 4;

    proto_tree_add_item(tree, hf_sflow_245_src_peer_as, tvb, offset + len, 4, ENC_BIG_ENDIAN);
    len += 4;

    dst_len = tvb_get_ntohl(tvb, offset + len);
    ti = proto_tree_add_uint(tree, hf_sflow_245_dst_as_entries, tvb, offset + len, 4, dst_len);
    sflow_245_dst_as_tree = proto_item_add_subtree(ti, ett_sflow_245_gw_as_dst);
    len += 4;

    for (i = 0; i < dst_len; i++) {
        if (version < 4) {
            /* Version 2 AS paths are different than versions >= 4 as
               follows:

               There is no type encoded in the packet.

               The destination ASs are encoded as an array of integers
               rather as an array of arrays of integers.  I just
               pretended they were encoded as an array of arrays with
               an implicit length of 1 to not have to do two
               completely separate blocks for the different versions.

               Having a subtree for "arrays" guaranteed to have only a
               single element proved cumbersome to navigate so I moved
               the creation of the subtree to only happen for versions
               >= 4.
             */
            dst_seg_len = 1;
            sflow_245_dst_as_seg_tree = sflow_245_dst_as_tree;
        } else {
            path_type = tvb_get_ntohl(tvb, offset + len);
            len += 4;
            dst_seg_len = tvb_get_ntohl(tvb, offset + len);
            len += 4;
            kludge = 8;
            ti = proto_tree_add_uint_format(tree, hf_sflow_245_as_type, tvb, offset + len - kludge, kludge, path_type,
                    "%s, (%u entries)", val_to_str_const(path_type, sflow_245_as_types, "Unknown AS type"), dst_seg_len);
            sflow_245_dst_as_seg_tree = proto_item_add_subtree(ti, ett_sflow_245_gw_as_dst_seg);
        }

        for (j = 0; j < dst_seg_len; j++) {
            proto_tree_add_item(sflow_245_dst_as_seg_tree, hf_sflow_245_dst_as, tvb, offset + len, 4, ENC_BIG_ENDIAN);
            len += 4;
        }
    }


    if (version >= 4) {
        comm_len = tvb_get_ntohl(tvb, offset + len);

        ti = proto_tree_add_uint(tree, hf_sflow_245_community_entries, tvb, offset + len, 4, comm_len);
        sflow_245_comm_tree = proto_item_add_subtree(ti, ett_sflow_245_gw_community);
        len += 4;
        for (i = 0; i < comm_len; i++) {
            proto_tree_add_item(sflow_245_comm_tree,
                    hf_sflow_245_dst_as, tvb, offset + len,
                    4, ENC_BIG_ENDIAN);
            len += 4;
        }

        proto_tree_add_item(tree, hf_sflow_245_localpref, tvb, offset + len, 4, ENC_BIG_ENDIAN);
        len += 4;

    }

    return offset + len;
}

/* sflow v5 ethernet frame data */
static int
dissect_sflow_5_ethernet_frame(tvbuff_t *tvb, proto_tree *tree, int offset) {

    proto_tree_add_item(tree, hf_sflow_245_ethernet_length_of_mac_packet, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ethernet_source_mac_address, tvb, offset, 6, ENC_NA);
    /* Padded to 4 byte offset */
    offset += 8;

    proto_tree_add_item(tree, hf_sflow_245_ethernet_destination_mac_address, tvb, offset, 6, ENC_NA);
    /* Padded to 4 byte offset */
    offset += 8;

    proto_tree_add_item(tree, hf_sflow_245_ethernet_packet_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 IPv4 data */
static int
dissect_sflow_5_ipv4(tvbuff_t *tvb, proto_tree *tree, int offset) {

    proto_tree_add_item(tree, hf_sflow_245_length_of_ip_packet, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_protocol, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ipv4_src, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ipv4_dst, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_source_port, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_destination_port, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_cwr, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_ece, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_urg, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_ack, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_psh, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_rst, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_syn, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_fin, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* 7 bits for type of service, plus 1 reserved bit */
    proto_tree_add_item(tree, hf_sflow_245_ipv4_precedence_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ipv4_delay, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ipv4_throughput, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ipv4_reliability, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ipv4_cost, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 IPv6 data */
static int
dissect_sflow_5_ipv6(tvbuff_t *tvb, proto_tree *tree, int offset) {

    proto_tree_add_item(tree, hf_sflow_245_length_of_ip_packet, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_protocol, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ipv6_src, tvb, offset, 16, ENC_NA);
    offset += 16;

    proto_tree_add_item(tree, hf_sflow_245_ipv6_dst, tvb, offset, 16, ENC_NA);
    offset += 16;

    proto_tree_add_item(tree, hf_sflow_245_ip_source_port, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_destination_port, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_cwr, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_ece, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_urg, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_ack, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_psh, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_rst, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_syn, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_245_ip_tcp_flag_fin, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* Priority -- Traffic class field enables a source to identify the desired
       delivery priority of the packets. Priority values are divided into
       ranges: traffic where the source provides congestion control and
       non-congestion control traffic.

       It is displayed as unsigned integer here according to sFlow specification */

    proto_tree_add_item(tree, hf_sflow_245_ipv6_priority, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 user data */
static int
dissect_sflow_5_extended_user(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t src_length, dest_length;

    /* charset is not processed here, all chars are assumed to be ASCII */
    proto_tree_add_item(tree, hf_sflow_5_extended_user_source_character_set, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    src_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_user_source_user_string_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract source user info char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_user_source_user, tvb, offset, src_length, ENC_ASCII);
    offset += src_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(src_length);

    /* charset is not processed here, all chars are assumed to be ASCII */
    proto_tree_add_item(tree, hf_sflow_5_extended_user_destination_character_set, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    dest_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_user_destination_user_string_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract destination user info char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_user_destination_user, tvb, offset, dest_length, ENC_ASCII);
    offset += dest_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(dest_length);

    return offset;
}

/* sflow v5 URL data */
static int
dissect_sflow_5_extended_url(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t direction, url_length, host_length;

    direction = tvb_get_ntohl(tvb, offset);
    switch (direction) {
        case 1:
            proto_tree_add_uint_format(tree, hf_sflow_5_extended_url_direction, tvb, offset, 4, direction,
                                        "Source Address is Server(%u)", direction);
            break;
        case 2:
            proto_tree_add_uint_format(tree, hf_sflow_5_extended_url_direction, tvb, offset, 4, direction,
                                        "Destination Address is Server (%u)", direction);
            break;
        default:
            proto_tree_add_uint_format(tree, hf_sflow_5_extended_url_direction, tvb, offset, 4, direction,
                                        "Server Unspecified (%u)", direction);
            break;
    }
    offset += 4;

    url_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_url_url_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract URL char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_url_url, tvb, offset, url_length, ENC_ASCII);
    offset += url_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(url_length);

    host_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_url_host_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract host info char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_url_host, tvb, offset, host_length, ENC_ASCII);
    offset += host_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(host_length);

    return offset;
}

/* sflow v5 MPLS tunnel */
static int
dissect_sflow_5_extended_mpls_tunnel(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t name_length;

    name_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_tunnel_name_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract tunnel name char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_tunnel_name, tvb, offset, name_length, ENC_ASCII);
    offset += name_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(name_length);

    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_tunnel_id, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_tunnel_cos_value, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 MPLS VC */
static int
dissect_sflow_5_extended_mpls_vc(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t name_length;

    name_length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_vc_instance_name_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract source user info char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_vc_instance_name, tvb, offset, name_length, ENC_ASCII);
    offset += name_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(name_length);

    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_vc_id, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_vc_label_cos_value, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 MPLS FEC */
static int
dissect_sflow_5_extended_mpls_fec(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t length;

    length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_ftn_description_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract MPLS FTN description char by char */
    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_ftn_description, tvb, offset, length, ENC_ASCII);
    offset += length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(length);

    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_ftn_mask, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 MPLS LVP FEC */
static int
dissect_sflow_5_extended_mpls_lvp_fec(tvbuff_t *tvb, proto_tree *tree, int offset) {

    proto_tree_add_item(tree, hf_sflow_5_extended_mpls_fec_address_prefix_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    return offset;
}

/* sflow v5 extended VLAN tunnel */
static int
dissect_sflow_5_extended_vlan_tunnel(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t num, i;

    num = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_vlan_tunnel_number_of_layers, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* loop strip 802.1Q TPID/TCI layers. each TPID/TCI pair is represented as a
       single 32 bit integer layers listed from outermost to innermost */
    for (i = 0; i < num; i++) {
        proto_tree_add_item(tree, hf_sflow_5_extended_vlan_tunnel_tpid_tci_pair, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }

    return offset;
}

/* sflow v5 extended 802.11 payload */
static int
dissect_sflow_5_extended_80211_payload(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t cipher_suite, OUI, suite_type, length;

    cipher_suite = tvb_get_ntohl(tvb, offset);
    OUI = cipher_suite >> 8;
    suite_type = cipher_suite & 0x000000ff;

    if (OUI == 0x000FAC) {
        proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_oui, tvb, offset, 3, OUI, "Default (0x%X)", OUI);
        offset += 3;
        proto_tree_add_item(tree, hf_sflow_5_extended_80211_suite_type, tvb, offset, 1, ENC_BIG_ENDIAN);
    } else {
        proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_oui, tvb, offset, 3, OUI, "Other vendor (0x%X)", OUI);
        offset += 3;
        proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_suite_type, tvb, offset, 1,
            suite_type, "vendor specific (%u)", suite_type);
    }
    offset++;

    length = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_5_extended_80211_payload_length, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* extract data byte by byte */
    proto_tree_add_item(tree, hf_sflow_5_extended_80211_payload, tvb, offset, length, ENC_NA);
    offset += length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(length);

    return offset;
}

/* sflow v5 extended 802.11 rx */
static int
dissect_sflow_5_extended_80211_rx(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t ssid_length, duration;

    /* extract SSID char by char. max char count = 32 */
    ssid_length = tvb_get_ntohl(tvb, offset);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_ssid, tvb, offset, ssid_length, ENC_ASCII);
    offset += ssid_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(ssid_length);

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_bssid, tvb, offset, 6, ENC_NA);
    /* Padded to 4 byte offset */
    offset += 8;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_version, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_channel, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_speed, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_rsni, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_rcpi, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    duration = tvb_get_ntohl(tvb, offset);
    if (duration == 0) {
        proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_rx_packet_duration, tvb, offset, 4, duration, "Unknown");
    } else {
        proto_tree_add_item(tree, hf_sflow_5_extended_80211_rx_packet_duration, tvb, offset, 4, ENC_BIG_ENDIAN);
    }
    offset += 4;

    return offset;
}

/* sflow v5 extended 802.11 tx */
static int
dissect_sflow_5_extended_80211_tx(tvbuff_t *tvb, proto_tree *tree, int offset) {
    uint32_t ssid_length, transmissions, packet_duration, retrans_duration;

    /* extract SSID char by char. max char count = 32 */
    ssid_length = tvb_get_ntohl(tvb, offset);
    if (ssid_length > 32)
        ssid_length = 32;
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_ssid, tvb, offset, ssid_length, ENC_ASCII);
    offset += ssid_length;
    /* get the correct offset by adding padding byte count */
    offset += WS_PADDING_TO_4(ssid_length);

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_bssid, tvb, offset, 6, ENC_NA);
    /* Padded to 4 byte offset */
    offset += 8;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_version, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    transmissions = tvb_get_ntohl(tvb, offset);
    switch (transmissions) {
        case 0:
            proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_tx_retransmissions, tvb, offset, 4,
                    0, "Unknown");
            break;
        case 1:
            proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_tx_retransmissions, tvb, offset, 4,
                    1, "Packet transmitted successfully on first attempt");
            break;
        default:
            proto_tree_add_uint(tree, hf_sflow_5_extended_80211_tx_retransmissions, tvb, offset, 4, transmissions - 1);
            break;
    }
    offset += 4;

    packet_duration = tvb_get_ntohl(tvb, offset);
    if (packet_duration == 0) {
        proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_tx_packet_duration, tvb, offset, 4, packet_duration, "Unknown");
    } else {
        proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_packet_duration, tvb, offset, 4, ENC_BIG_ENDIAN);
    }
    offset += 4;

    retrans_duration = tvb_get_ntohl(tvb, offset);
    if (retrans_duration == 0) {
        proto_tree_add_uint_format_value(tree, hf_sflow_5_extended_80211_tx_retransmission_duration, tvb, offset, 4, retrans_duration, "Unknown");
    } else {
        proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_retransmission_duration, tvb, offset, 4, ENC_BIG_ENDIAN);
    }
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_channel, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_speed, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    proto_tree_add_item(tree, hf_sflow_5_extended_80211_tx_power, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* sflow v5 extended 802.11 aggregation */
static int
dissect_sflow_5_extended_80211_aggregation(tvbuff_t *tvb _U_, proto_tree *tree _U_, int offset) {

    return offset;
}

/* dissect an sflow v2/4 flow sample */
static int
dissect_sflow_24_flow_sample(tvbuff_t *tvb, packet_info *pinfo,
        proto_tree *tree, int offset, proto_item *parent) {
    uint32_t    sequence_number, sampling_rate, output;

    proto_tree *extended_data_tree;
    proto_item *ti;
    uint32_t    packet_type, extended_data, ext_type, i;

    sequence_number = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_flow_sample_sequence_number, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_item_append_text(parent, ", seq %u", sequence_number);
    proto_tree_add_item(tree, hf_sflow_flow_sample_source_id_class, tvb, offset + 4, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_flow_sample_index, tvb, offset + 4, 4, ENC_BIG_ENDIAN);
    sampling_rate = tvb_get_ntohl(tvb, offset + 8);
    proto_tree_add_uint_format_value(tree, hf_sflow_flow_sample_sampling_rate, tvb, offset + 8, 4,
            sampling_rate, "1 out of %u packets",
            sampling_rate);

    proto_tree_add_item(tree, hf_sflow_flow_sample_sample_pool, tvb, offset + 12, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_flow_sample_dropped_packets, tvb, offset + 16, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_flow_sample_input_interface, tvb, offset + 20, 4, ENC_BIG_ENDIAN);
    output = tvb_get_ntohl(tvb, offset + 24);
    if (output & 0x80000000) {
        output & 0x7fffffff ?
            proto_tree_add_uint_format_value(tree, hf_sflow_24_flow_sample_multiple_outputs, tvb, offset + 24, 4,
                output & 0x7fffffff, "%u interfaces", output & 0x7fffffff) :
            proto_tree_add_uint_format_value(tree, hf_sflow_24_flow_sample_multiple_outputs, tvb, offset + 24, 4,
                0x80000000, "unknown number");
    } else {
        proto_tree_add_item(tree, hf_sflow_24_flow_sample_output_interface, tvb, offset + 24, 4, ENC_BIG_ENDIAN);
    }
    offset += 28;

    /* what kind of flow sample is it? */
    packet_type = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_245_packet_information_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    switch (packet_type) {
        case SFLOW_245_PACKET_DATA_TYPE_HEADER:
            offset = dissect_sflow_245_sampled_header(tvb, pinfo, tree, offset);
            break;
        case SFLOW_245_PACKET_DATA_TYPE_IPV4:
        case SFLOW_245_PACKET_DATA_TYPE_IPV6:
        default:
            break;
    }
    /* still need to dissect extended data */
    extended_data = tvb_get_ntohl(tvb, offset);
    offset += 4;

    for (i = 0; i < extended_data; i++) {
        /* figure out what kind of extended data it is */
        ext_type = tvb_get_ntohl(tvb, offset);

        /* create a subtree.  Might want to move this to
         * the end, so more info can be correct.
         */
        ti = proto_tree_add_uint(tree, hf_sflow_245_extended_information_type, tvb, offset, 4, ext_type);
        extended_data_tree = proto_item_add_subtree(ti, ett_sflow_245_extended_data);
        offset += 4;

        switch (ext_type) {
            case SFLOW_245_EXTENDED_SWITCH:
                offset = dissect_sflow_245_extended_switch(tvb, extended_data_tree, offset);
                break;
            case SFLOW_245_EXTENDED_ROUTER:
                offset = dissect_sflow_245_extended_router(tvb, pinfo, extended_data_tree, offset);
                break;
            case SFLOW_245_EXTENDED_GATEWAY:
                offset = dissect_sflow_245_extended_gateway(tvb, pinfo, extended_data_tree, offset);
                break;
            case SFLOW_245_EXTENDED_USER:
                break;
            case SFLOW_245_EXTENDED_URL:
                break;
            default:
                break;
        }
        proto_item_set_end(ti, tvb, offset);
    }
    return offset;

}

/* dissect an sflow v5 flow record */
static int
dissect_sflow_5_flow_record(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset) {
    proto_tree *flow_data_tree;
    proto_item *ti;
    uint32_t    enterprise_format, enterprise, format;

    /* what kind of flow sample is it? */
    enterprise_format = tvb_get_ntohl(tvb, offset);
    enterprise = enterprise_format >> 12;
    format = enterprise_format & 0x00000fff;

    /* only accept default enterprise 0 (InMon sFlow) */
    if (enterprise == ENTERPRISE_DEFAULT) {
        flow_data_tree = proto_tree_add_subtree(tree, tvb, offset, -1, ett_sflow_5_flow_record, &ti,
                val_to_str_ext_const(format, &sflow_5_flow_record_type_ext, "Unknown sample format"));

        proto_tree_add_uint_format_value(flow_data_tree, hf_sflow_enterprise, tvb, offset, 4,
                            enterprise, "standard sFlow (%u)", enterprise);
        proto_tree_add_item(flow_data_tree, hf_sflow_5_flow_record_format, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;

        proto_tree_add_item(flow_data_tree, hf_sflow_5_flow_data_length, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;

        switch (format) {
            case SFLOW_5_RAW_PACKET_HEADER:
                offset = dissect_sflow_245_sampled_header(tvb, pinfo, flow_data_tree, offset);
                break;
            case SFLOW_5_ETHERNET_FRAME:
                offset = dissect_sflow_5_ethernet_frame(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_IPV4:
                offset = dissect_sflow_5_ipv4(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_IPV6:
                offset = dissect_sflow_5_ipv6(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_SWITCH:
                offset = dissect_sflow_245_extended_switch(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_ROUTER:
                offset = dissect_sflow_245_extended_router(tvb, pinfo, flow_data_tree, offset);
                break;
            case SFLOW_5_GATEWAY:
                offset = dissect_sflow_245_extended_gateway(tvb, pinfo, flow_data_tree, offset);
                break;
            case SFLOW_5_USER:
                offset = dissect_sflow_5_extended_user(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_URL:
                offset = dissect_sflow_5_extended_url(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_MPLS_DATA:
                offset = dissect_sflow_5_extended_mpls_data(tvb, pinfo, flow_data_tree, offset);
                break;
            case SFLOW_5_NAT:
                offset = dissect_sflow_5_extended_nat(tvb, pinfo, flow_data_tree, offset);
                break;
            case SFLOW_5_MPLS_TUNNEL:
                offset = dissect_sflow_5_extended_mpls_tunnel(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_MPLS_VC:
                offset = dissect_sflow_5_extended_mpls_vc(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_MPLS_FEC:
                offset = dissect_sflow_5_extended_mpls_fec(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_MPLS_LVP_FEC:
                offset = dissect_sflow_5_extended_mpls_lvp_fec(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_VLAN_TUNNEL:
                offset = dissect_sflow_5_extended_vlan_tunnel(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_80211_PAYLOAD:
                offset = dissect_sflow_5_extended_80211_payload(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_80211_RX:
                offset = dissect_sflow_5_extended_80211_rx(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_80211_TX:
                offset = dissect_sflow_5_extended_80211_tx(tvb, flow_data_tree, offset);
                break;
            case SFLOW_5_80211_AGGREGATION:
                offset = dissect_sflow_5_extended_80211_aggregation(tvb, flow_data_tree, offset);
                break;
            default:
                break;
        }
    } else {
        /* unknown enterprise format, what to do?? */
        uint32_t length;

        flow_data_tree = proto_tree_add_subtree(tree, tvb, offset, -1,
            ett_sflow_5_flow_record, &ti, "Unknown enterprise format");
        proto_tree_add_uint_format_value(flow_data_tree, hf_sflow_enterprise, tvb, offset, 4,
                                    enterprise, "Non-standard sFlow (%u)", enterprise);
        offset += 4;
        /* get length */
        proto_tree_add_item_ret_uint(flow_data_tree, hf_sflow_enterprise_length, tvb, offset, 4, ENC_BIG_ENDIAN, &length);
        offset += 4;
        /* show data as bytes */
        proto_tree_add_item(flow_data_tree, hf_sflow_enterprise_data, tvb, offset, length, ENC_NA);
        offset += length;
        /* get the correct offset by adding padding byte count */
        offset += WS_PADDING_TO_4(length);
    }
    proto_item_set_end(ti, tvb, offset);

    return offset;
}

/* dissect generic interface counters */
static int
dissect_sflow_5_generic_interface(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifindex, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_iftype, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifspeed, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifdirection, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifadmin_status, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifoper_status, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifinoct, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifinpkt, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifinmcast, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifinbcast, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifindisc, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifinerr, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifinunk, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifoutoct, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifoutpkt, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifoutmcast, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifoutbcast, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifoutdisc, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifouterr, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ifpromisc, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* dissect ethernet interface counters */
static int
dissect_sflow_5_ethernet_interface(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsAlignmentErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsFCSErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsSingleCollisionFrames, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsMultipleCollisionFrames, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsSQETestErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsDeferredTransmissions, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsLateCollisions, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsExcessiveCollisions, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsInternalMacTransmitErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsCarrierSenseErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsFrameTooLongs, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsInternalMacReceiveErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot3StatsSymbolErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* dissect token ring counters */
static int
dissect_sflow_5_token_ring(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsLineErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsBurstErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsACErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsAbortTransErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsInternalErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsLostFrameErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsReceiveCongestions, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsFrameCopiedErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsTokenErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsSoftErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsHardErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsSignalLoss, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsTransmitBeacons, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsRecoveries, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsLobeWires, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsRemoves, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsSingles, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot5StatsFreqErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* dissect 100 BaseVG interface counters */
static int
dissect_sflow_5_vg_interface(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InHighPriorityFrames, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InHighPriorityOctets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InNormPriorityFrames, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InNormPriorityOctets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InIPMErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InOversizeFrameErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InDataErrors, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12InNullAddressedFrames, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12OutHighPriorityFrames, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12OutHighPriorityOctets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12TransitionIntoTrainings, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12HCInHighPriorityOctets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12HCInNormPriorityOctets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_dot12HCOutHighPriorityOctets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    return offset;
}

/* dissect VLAN counters */
static int
dissect_sflow_5_vlan(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_245_vlan_id, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_octets, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_ucastPkts, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_multicastPkts, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_broadcastPkts, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_245_discards, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

static int * const sflow_5_lag_port_state_flags[] = {
    &hf_sflow_5_lag_port_actoradminstate,
    &hf_sflow_5_lag_port_actoroperstate,
    &hf_sflow_5_lag_port_partneradminstate,
    &hf_sflow_5_lag_port_partneroperstate,
    NULL
};

static int
dissect_sflow_5_lag(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_actorsystemid, tvb, offset, 6, ENC_NA);
    offset += 6;
    /* XDR requires 4-byte alignment */
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_padding, tvb, offset, 2, ENC_NA);
    offset += 2;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_partneropersystemid, tvb, offset, 6, ENC_NA);
    offset += 6;
    /* XDR requires 4-byte alignment */
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_padding, tvb, offset, 2, ENC_NA);
    offset += 2;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_attachedaggid, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_bitmask(counter_data_tree, tvb, offset, hf_sflow_lag_port_state, ett_sflow_lag_port_state_flags, sflow_5_lag_port_state_flags, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_lacpdusrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_markerpdusrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_markerresponsepdusrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_unknownrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_illegalrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_lacpdustx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_markerpdustx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_lag_port_stats_markerresponsepdustx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* dissect 802.11 counters */
static int
dissect_sflow_5_80211_counters(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11TransmittedFragmentCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11MulticastTransmittedFrameCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11FailedCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11RetryCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11MultipleRetryCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11FrameDuplicateCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11RTSSuccessCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11RTSFailureCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11ACKFailureCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11ReceivedFragmentCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11MulticastReceivedFrameCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11FCSErrorCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11TransmittedFrameCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11WEPUndecryptableCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11QoSDiscardedFragmentCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11AssociatedStationCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11QoSCFPollsReceivedCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11QoSCFPollsUnusedCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11QoSCFPollsUnusableCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_dot11QoSCFPollsLostCount, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* dissect processor information */
static int
dissect_sflow_5_processor_information(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_5_cpu_5s, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_cpu_1m, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_cpu_5m, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_total_memory, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_free_memory, tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    return offset;
}

/* dissect radio utilization */
static int
dissect_sflow_5_radio_utilization(proto_tree *counter_data_tree, tvbuff_t *tvb, int offset) {

    proto_tree_add_item(counter_data_tree, hf_sflow_5_elapsed_time, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_on_channel_time, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(counter_data_tree, hf_sflow_5_on_channel_busy_time, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/* dissect an sflow v5 counters record */
static int
dissect_sflow_5_counters_record(tvbuff_t *tvb, proto_tree *tree, int offset) {
    proto_tree *counter_data_tree;
    proto_item *ti;
    uint32_t    enterprise_format, enterprise, format;

    /* what kind of flow sample is it? */
    enterprise_format = tvb_get_ntohl(tvb, offset);
    enterprise = enterprise_format >> 12;
    format = enterprise_format & 0x00000fff;

    if (enterprise == ENTERPRISE_DEFAULT) { /* only accept default enterprise 0 (InMon sFlow) */
        counter_data_tree = proto_tree_add_subtree(tree, tvb, offset, -1, ett_sflow_5_counters_record, &ti,
                val_to_str_const(format, sflow_5_counters_record_type, "Unknown sample format"));

        proto_tree_add_uint_format_value(counter_data_tree, hf_sflow_enterprise, tvb, offset, 4,
                                enterprise, "standard sFlow (%u)", enterprise);

        proto_tree_add_item(counter_data_tree, hf_sflow_5_counters_record_format, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;

        proto_tree_add_item(counter_data_tree, hf_sflow_5_flow_data_length, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;

        switch (format) {
            case SFLOW_5_GENERIC_INTERFACE:
                offset = dissect_sflow_5_generic_interface(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_ETHERNET_INTERFACE:
                offset = dissect_sflow_5_ethernet_interface(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_TOKEN_RING:
                offset = dissect_sflow_5_token_ring(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_100BASE_VG_INTERFACE:
                offset = dissect_sflow_5_vg_interface(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_VLAN:
                offset = dissect_sflow_5_vlan(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_LAG:
                offset = dissect_sflow_5_lag(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_80211_COUNTERS:
                offset = dissect_sflow_5_80211_counters(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_PROCESSOR:
                offset = dissect_sflow_5_processor_information(counter_data_tree, tvb, offset);
                break;
            case SFLOW_5_RADIO_UTILIZATION:
                offset = dissect_sflow_5_radio_utilization(counter_data_tree, tvb, offset);
                break;
            default:
                break;
        }
    } else { /* unknown enterprise format, what to do?? */
        uint32_t length;

        counter_data_tree = proto_tree_add_subtree(tree, tvb, offset, -1,
            ett_sflow_5_counters_record, &ti, "Unknown enterprise format");
        proto_tree_add_uint_format_value(counter_data_tree, hf_sflow_enterprise, tvb, offset, 4,
                        enterprise, "Non-standard sFlow (%u)", enterprise);
        offset += 4;
        /* get length */
        proto_tree_add_item_ret_uint(counter_data_tree, hf_sflow_enterprise_length, tvb, offset, 4, ENC_BIG_ENDIAN, &length);
        offset += 4;
        /* show data as bytes */
        proto_tree_add_item(counter_data_tree, hf_sflow_enterprise_data, tvb, offset, length, ENC_NA);
        offset += length;
        /* get the correct offset by adding padding byte count */
        offset += WS_PADDING_TO_4(length);
    }
    proto_item_set_end(ti, tvb, offset);

    return offset;
}

/* dissect an sflow v5 flow sample */
static void
dissect_sflow_5_flow_sample(tvbuff_t *tvb, packet_info *pinfo,
        proto_tree *tree, int offset, proto_item *parent) {

    uint32_t sequence_number, sampling_rate,
            output, records, i, output_format;
    proto_item *ti;
    proto_tree *output_interface_tree;

    sequence_number = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_flow_sample_sequence_number, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_item_append_text(parent, ", seq %u", sequence_number);

    proto_tree_add_item(tree, hf_sflow_flow_sample_source_id_class, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_flow_sample_index, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    sampling_rate = tvb_get_ntohl(tvb, offset);
    proto_tree_add_uint_format_value(tree, hf_sflow_flow_sample_sampling_rate, tvb, offset, 4,
            sampling_rate, "1 out of %u packets", sampling_rate);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_sample_pool, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_dropped_packets, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_input_interface, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    ti = proto_tree_add_item_ret_uint(tree, hf_sflow_5_flow_sample_output_interface, tvb, offset, 4, ENC_BIG_ENDIAN, &output);
    output_interface_tree = proto_item_add_subtree(ti, ett_sflow_5_output_interface);
    output_format = output >> 30;
    proto_tree_add_item(output_interface_tree, hf_sflow_5_flow_sample_output_interface_form, tvb, offset, 4, ENC_BIG_ENDIAN);
    switch(output_format) {
        case SFLOW_5_INT_FORMAT_DISCARD:
            proto_tree_add_item(output_interface_tree, hf_sflow_5_flow_sample_output_interface_val_discard, tvb, offset, 4, ENC_BIG_ENDIAN);
            break;
        case SFLOW_5_INT_FORMAT_MULTIPLE:
            ti =proto_tree_add_item(output_interface_tree, hf_sflow_5_flow_sample_output_interface_val, tvb, offset, 4, ENC_BIG_ENDIAN);
            if (output == 0x80000000) {
                proto_item_append_text(ti, " unknown number of interfaces greater than 1");
            }
            break;
        case SFLOW_5_INT_FORMAT_IFINDEX:
        default:
            proto_tree_add_item(output_interface_tree, hf_sflow_5_flow_sample_output_interface_val, tvb, offset, 4, ENC_BIG_ENDIAN);
            break;
    }
    offset += 4;
    records = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_flow_sample_flow_record, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* start loop processing flow records */
    /* we set an upper records limit to 255 in case corrupted data causes
     * huge number of loops! */
    for (i = 0; i < (records&0x000000ff); i++) {
        offset = dissect_sflow_5_flow_record(tvb, pinfo, tree, offset);
    }

}

/* dissect an expanded flow sample */
static void
dissect_sflow_5_expanded_flow_sample(tvbuff_t *tvb, packet_info *pinfo,
        proto_tree *tree, int offset, proto_item *parent) {

    proto_item *ti;
    uint32_t sequence_number, sampling_rate, records, i, output_format, output_value;

    sequence_number = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_flow_sample_sequence_number, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_item_append_text(parent, ", seq %u", sequence_number);
    proto_tree_add_item(tree, hf_sflow_flow_sample_source_id_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_source_id_index, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    sampling_rate = tvb_get_ntohl(tvb, offset);
    proto_tree_add_uint_format_value(tree, hf_sflow_flow_sample_sampling_rate, tvb, offset, 4,
            sampling_rate, "1 out of %u packets", sampling_rate);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_sample_pool, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_dropped_packets, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_input_interface_format, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_flow_sample_input_interface_value, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_sflow_5_flow_sample_output_interface_expanded_format, tvb, offset, 4, ENC_BIG_ENDIAN, &output_format);
    offset += 4;
    switch(output_format) {
        case SFLOW_5_INT_FORMAT_DISCARD:
            proto_tree_add_item(tree, hf_sflow_5_flow_sample_output_interface_expanded_value_discarded, tvb, offset, 4, ENC_BIG_ENDIAN);
            break;
        case SFLOW_5_INT_FORMAT_MULTIPLE:
            ti =proto_tree_add_item_ret_uint(tree, hf_sflow_5_flow_sample_output_interface_expanded_value_number, tvb, offset, 4, ENC_BIG_ENDIAN, &output_value);
            if (output_value == 0x0) {
                proto_item_append_text(ti, " unknown number of interfaces greater than 1");
            }
            break;
        case SFLOW_5_INT_FORMAT_IFINDEX:
            proto_tree_add_item(tree, hf_sflow_5_flow_sample_output_interface_expanded_value_ifindex, tvb, offset, 4, ENC_BIG_ENDIAN);
            break;
        default:
            proto_tree_add_item(tree, hf_sflow_5_flow_sample_output_interface_expanded_value, tvb, offset, 4, ENC_BIG_ENDIAN);
            break;
    }
    offset += 4;
    records = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_flow_sample_flow_record, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* start loop processing flow records
     * we limit record count to 255 in case corrupted data may cause huge number of loops */
    for (i = 0; i < (records&0x000000ff); i++) {
        offset = dissect_sflow_5_flow_record(tvb, pinfo, tree, offset);
    }
}

/* dissect an sflow v2/4 counters sample */
static int
dissect_sflow_24_counters_sample(tvbuff_t *tvb, proto_tree *tree, int offset, proto_item *parent) {

    uint32_t sequence_number, counters_type;

    sequence_number = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_counters_sample_sequence_number, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_item_append_text(parent, ", seq %u", sequence_number);

    proto_tree_add_item(tree, hf_sflow_counters_sample_source_id_class, tvb, offset + 4, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_counters_sample_index, tvb, offset + 4, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_counters_sample_sampling_interval, tvb, offset + 8, 4, ENC_BIG_ENDIAN);
    counters_type = tvb_get_ntohl(tvb, offset + 12);
    proto_tree_add_item(tree, hf_sflow_counters_sample_counters_type, tvb, offset + 12, 4, ENC_BIG_ENDIAN);

    offset += 16;

    /* most counters types have the "generic" counters first */
    switch (counters_type) {
        case SFLOW_245_COUNTERS_GENERIC:
        case SFLOW_245_COUNTERS_ETHERNET:
        case SFLOW_245_COUNTERS_TOKENRING:
        case SFLOW_245_COUNTERS_FDDI:
        case SFLOW_245_COUNTERS_VG:
        case SFLOW_245_COUNTERS_WAN:
            proto_tree_add_item(tree, hf_sflow_245_ifindex, tvb, offset, 4, ENC_BIG_ENDIAN);
            proto_item_append_text(parent, ", ifIndex %u", tvb_get_ntohl(tvb, offset));
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_iftype, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifspeed, tvb, offset, 8, ENC_BIG_ENDIAN);
            offset += 8;
            proto_tree_add_item(tree, hf_sflow_245_ifdirection, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifadmin_status, tvb, offset, 4, ENC_BIG_ENDIAN);
            proto_tree_add_item(tree, hf_sflow_245_ifoper_status, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifinoct, tvb, offset, 8, ENC_BIG_ENDIAN);
            offset += 8;
            proto_tree_add_item(tree, hf_sflow_245_ifinpkt, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifinmcast, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifinbcast, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifindisc, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifinerr, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifinunk, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifoutoct, tvb, offset, 8, ENC_BIG_ENDIAN);
            offset += 8;
            proto_tree_add_item(tree, hf_sflow_245_ifoutpkt, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifoutmcast, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifoutbcast, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifoutdisc, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifouterr, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            proto_tree_add_item(tree, hf_sflow_245_ifpromisc, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
            break;
    }

    /* Some counter types have other info to gather */
    switch (counters_type) {
        case SFLOW_245_COUNTERS_ETHERNET:
            offset += (int)sizeof (struct ethernet_counters);
            break;
        case SFLOW_245_COUNTERS_TOKENRING:
            offset = dissect_sflow_5_token_ring(tree, tvb, offset);
            break;
        case SFLOW_245_COUNTERS_VG:
            offset = dissect_sflow_5_vg_interface(tree, tvb, offset);
            break;
        case SFLOW_245_COUNTERS_VLAN:
            offset = dissect_sflow_5_vlan(tree, tvb, offset);
            break;
        default:
            break;
    }
    return offset;
}

/* dissect an sflow v5 counters sample */
static void
dissect_sflow_5_counters_sample(tvbuff_t *tvb, proto_tree *tree, int offset, proto_item *parent) {
    uint32_t sequence_number, records, i;

    /* grab the flow header.  This will remain in network byte
       order, so must convert each item before use */
    sequence_number = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_counters_sample_sequence_number, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_item_append_text(parent, ", seq %u", sequence_number);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_counters_sample_source_id_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_tree_add_item(tree, hf_sflow_counters_sample_source_id_index, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    records = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_counters_sample_counters_records, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* start loop processing counters records
     * limit record count to 255 in case corrupted data may cause huge number of loops */
    for (i = 0; i < (records&0x000000ff); i++) {
        offset = dissect_sflow_5_counters_record(tvb, tree, offset);
    }
}

/* dissect an expanded counters sample */
static void
dissect_sflow_5_expanded_counters_sample(tvbuff_t *tvb, proto_tree *tree, int offset, proto_item *parent) {
    uint32_t sequence_number, records, i;

    sequence_number = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_counters_sample_sequence_number, tvb, offset, 4, ENC_BIG_ENDIAN);
    proto_item_append_text(parent, ", seq %u", sequence_number);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_counters_sample_expanded_source_id_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    proto_tree_add_item(tree, hf_sflow_counters_sample_expanded_source_id_index, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;
    records = tvb_get_ntohl(tvb, offset);
    proto_tree_add_item(tree, hf_sflow_counters_sample_counters_records, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /* start loop processing counters records
     * limit record count to 255 in case corrupted data may cause huge number of loops */
    for (i = 0; i < (records&0x000000ff); i++) {
        offset = dissect_sflow_5_counters_record(tvb, tree, offset);
    }
}

static int * const sflow_lag_port_state_flags[] = {
    &hf_sflow_lag_port_actoradminstate,
    &hf_sflow_lag_port_actoroperstate,
    &hf_sflow_lag_port_partneradminstate,
    &hf_sflow_lag_port_partneroperstate,
    &hf_sflow_lag_port_reserved,
    NULL
};

/* dissect an LAG Port Stats ( http://www.sflow.org/sflow_lag.txt ) */
static void
dissect_sflow_5_lag_port_stats(tvbuff_t *tvb, proto_tree *tree, int offset, proto_item *parent _U_) {

    proto_tree_add_item(tree, hf_sflow_lag_port_actorsystemid, tvb, offset, 6, ENC_NA);
    offset += 6;

    proto_tree_add_item(tree, hf_sflow_lag_port_partneropersystemid, tvb, offset, 6, ENC_NA);
    offset += 6;

    proto_tree_add_item(tree, hf_sflow_lag_port_attachedaggid, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_bitmask(tree, tvb, offset, hf_sflow_lag_port_state, ett_sflow_lag_port_state_flags, sflow_lag_port_state_flags, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_lacpdusrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_markerpdusrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_markerresponsepdusrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_unknownrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_illegalrx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_lacpdustx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_markerpdustx, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(tree, hf_sflow_lag_port_stats_markerresponsepdustx, tvb, offset, 4, ENC_BIG_ENDIAN);
    /*offset += 4;*/
}

/* Code to dissect the sflow v2/4/5 samples */
static int
dissect_sflow_245_samples(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, int offset, uint32_t version) {
    proto_tree *sflow_245_sample_tree;
    proto_item *ti;             /* tree item */
    uint32_t    sample_type, enterprise, format, length;

    /* decide what kind of sample it is. */
    sample_type = tvb_get_ntohl(tvb, offset);
    if (version == 5) {
        enterprise = sample_type >> 12;
        format = sample_type & 0x00000fff;

        if (enterprise == ENTERPRISE_DEFAULT) { /* only accept default enterprise 0 (InMon sFlow) */
            sflow_245_sample_tree = proto_tree_add_subtree(tree, tvb, offset, -1, ett_sflow_245_sample, &ti,
                    val_to_str_const(format, sflow_245_sampletype, "Unknown sample format"));

            proto_tree_add_uint_format_value(sflow_245_sample_tree, hf_sflow_enterprise, tvb, offset, 4, enterprise, "standard sFlow (%u)", enterprise);
            proto_tree_add_item(sflow_245_sample_tree, hf_sflow_245_sampletype12, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;

            length = tvb_get_ntohl(tvb, offset);
            proto_tree_add_item(sflow_245_sample_tree, hf_sflow_5_sample_length, tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;

            switch (format) {
                case FLOWSAMPLE:
                    dissect_sflow_5_flow_sample(tvb, pinfo, sflow_245_sample_tree, offset, ti);
                    break;
                case COUNTERSSAMPLE:
                    dissect_sflow_5_counters_sample(tvb, sflow_245_sample_tree, offset, ti);
                    break;
                case EXPANDED_FLOWSAMPLE:
                    dissect_sflow_5_expanded_flow_sample(tvb, pinfo, sflow_245_sample_tree, offset, ti);
                    break;
                case EXPANDED_COUNTERSSAMPLE:
                    dissect_sflow_5_expanded_counters_sample(tvb, sflow_245_sample_tree, offset, ti);
                    break;
                case LAG_PORT_STATS:
                    dissect_sflow_5_lag_port_stats(tvb, sflow_245_sample_tree, offset, ti);
                    break;
                default:
                    break;
            }
            /* Make sure the length doesn't run past the end of the packet */
            tvb_ensure_bytes_exist(tvb, offset, length);
            /* current offset points to sample length field, which is 4 bytes from the beginning of the packet*/
            offset += length;
        } else { /* unknown enterprise format, what to do?? */
            sflow_245_sample_tree = proto_tree_add_subtree(tree, tvb, offset, -1,
                        ett_sflow_245_sample, &ti, "Unknown enterprise format");
            proto_tree_add_uint_format_value(sflow_245_sample_tree, hf_sflow_enterprise, tvb, offset, 4,
                            enterprise, "Non-standard sFlow (%u)", enterprise);
            offset = tvb_captured_length(tvb);
        }

    } else { /* version 2 or 4 */
        sflow_245_sample_tree = proto_tree_add_subtree(tree, tvb, offset, -1, ett_sflow_245_sample, &ti,
                val_to_str_const(sample_type, sflow_245_sampletype, "Unknown sample type"));

        proto_tree_add_item(sflow_245_sample_tree, hf_sflow_245_sampletype, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;

        switch (sample_type) {
            case FLOWSAMPLE:
                offset = dissect_sflow_24_flow_sample(tvb, pinfo, sflow_245_sample_tree, offset, ti);
                break;
            case COUNTERSSAMPLE:
                offset = dissect_sflow_24_counters_sample(tvb, sflow_245_sample_tree, offset, ti);
                break;
            default:
                break;
        }
    }
    proto_item_set_end(ti, tvb, offset);

    return offset;
}

/* Code to actually dissect the packets */
static int
dissect_sflow_245(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
    /* Set up structures needed to add the protocol subtree and manage it */
    proto_item                   *ti;
    proto_tree                   *sflow_245_tree;
    uint32_t                      version, sub_agent_id, seqnum;
    address                       addr_details;
    int                           sflow_addr_type;
    struct sflow_address_type     addr_type;
    uint32_t                      uptime;

    uint32_t       numsamples;
    unsigned       offset = 0;
    unsigned       i      = 0;

    addr_type.hf_addr_v4 = hf_sflow_agent_address_v4;
    addr_type.hf_addr_v6 = hf_sflow_agent_address_v6;

    /*
     * We fetch the version and address type so that we can determine,
     * ahead of time, whether this is an sFlow packet or not, before
     * we do *anything* to the columns or the protocol tree.
     *
     * XXX - we might want to deem this "not sFlow" if we don't have at
     * least 8 bytes worth of data.
     */
    version = tvb_get_ntohl(tvb, offset);
    if (version != 2 && version != 4 && version != 5) {
       /* Unknown version; assume it's not an sFlow packet. */
       return 0;
    }

    sflow_addr_type = tvb_get_ntohl(tvb, offset + 4);
    switch (sflow_addr_type) {
        case ADDR_TYPE_UNKNOWN:
        case ADDR_TYPE_IPV4:
        case ADDR_TYPE_IPV6:
            break;

        default:
            /*
             * Address type we don't know about; assume it's not an sFlow
             * packet.
             */
            return 0;
    }
    /* Make entries in Protocol column and Info column on summary display */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "sFlow");

    /* create display subtree for the protocol */
    ti = proto_tree_add_item(tree, proto_sflow, tvb, 0, -1, ENC_NA);

    sflow_245_tree = proto_item_add_subtree(ti, ett_sflow_245);

    col_add_fstr(pinfo->cinfo, COL_INFO, "V%u", version);
    proto_tree_add_item(sflow_245_tree, hf_sflow_version, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    proto_tree_add_item(sflow_245_tree, hf_sflow_agent_address_type, tvb, offset, 4, ENC_BIG_ENDIAN);
    offset = dissect_sflow_245_address_type(tvb, pinfo, sflow_245_tree, offset,
                                            &addr_type, &addr_details);
    switch (sflow_addr_type) {
        case ADDR_TYPE_UNKNOWN:
            break;
        case ADDR_TYPE_IPV4:
        case ADDR_TYPE_IPV6:
            col_append_fstr(pinfo->cinfo, COL_INFO, ", agent %s", address_to_str(pinfo->pool, &addr_details));
            break;
    }

    if (version == 5) {
        sub_agent_id = tvb_get_ntohl(tvb, offset);
        col_append_fstr(pinfo->cinfo, COL_INFO, ", sub-agent ID %u", sub_agent_id);
        proto_tree_add_uint(sflow_245_tree, hf_sflow_5_sub_agent_id, tvb, offset, 4, sub_agent_id);
        offset += 4;
    }
    seqnum = tvb_get_ntohl(tvb, offset);
    col_append_fstr(pinfo->cinfo, COL_INFO, ", seq %u", seqnum);
    proto_tree_add_uint(sflow_245_tree, hf_sflow_245_seqnum, tvb, offset, 4, seqnum);
    offset += 4;
    uptime = tvb_get_ntohl(tvb, offset);
    proto_tree_add_uint_format_value(sflow_245_tree, hf_sflow_245_sysuptime, tvb, offset, 4, uptime, "%s (%ums)",
        unsigned_time_secs_to_str(pinfo->pool, uptime / 1000), uptime);
    offset += 4;
    numsamples = tvb_get_ntohl(tvb, offset);
    col_append_fstr(pinfo->cinfo, COL_INFO, ", %u samples", numsamples);
    proto_tree_add_uint(sflow_245_tree, hf_sflow_245_numsamples, tvb, offset, 4, numsamples);
    offset += 4;

    /* Ok, we're now at the end of the sflow_245 datagram header;
     * everything from here out should be samples. Loop over
     * the expected number of samples, and pass them to the appropriate
     * dissectors.
     */

    /* limit number of samples to 255 to avoid huge number of loops
     * caused by corrupted data */
    for (i = 0; i < (numsamples & 0x000000ff); i++) {
        offset = dissect_sflow_245_samples(tvb, pinfo, sflow_245_tree, offset, version);
    }

    return tvb_captured_length(tvb);
}

/* Register the protocol with Wireshark */

void
proto_register_sflow(void) {

    module_t *sflow_245_module;

    /* Setup list of header fields  See Section 1.6.1 for details*/
    static hf_register_info hf[] = {
        { &hf_sflow_version,
            { "Datagram version", "sflow_245.version",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "sFlow datagram version", HFILL}},
        { &hf_sflow_agent_address_type,
            { "Agent address type", "sflow_245.agenttype",
                FT_UINT32, BASE_DEC, VALS(sflow_agent_address_types), 0x0,
                "sFlow agent address type", HFILL}},
        { &hf_sflow_agent_address_v4,
            { "Agent address", "sflow_245.agent",
                FT_IPv4, BASE_NONE, NULL, 0x0,
                "sFlow Agent IP address", HFILL}},
        { &hf_sflow_agent_address_v6,
            { "Agent address", "sflow_245.agent.v6",
                FT_IPv6, BASE_NONE, NULL, 0x0,
                "sFlow Agent IPv6 address", HFILL}},
        { &hf_sflow_5_sub_agent_id,
            { "Sub-agent ID", "sflow_245.sub_agent_id",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "sFlow sub-agent ID", HFILL}},
        { &hf_sflow_5_sample_length,
            { "Sample length (byte)", "sflow_5.sample_length",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "sFlow sample length", HFILL}},
        { &hf_sflow_5_flow_data_length,
            { "Flow data length (byte)", "sflow_5.flow_data_length",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "sFlow flow data length", HFILL}},
#if 0
        { &hf_sflow_5_counters_data_length,
            { "Counters data length (byte)", "sflow_5.counter_data_length",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "sFlow counters data length", HFILL}},
#endif
        { &hf_sflow_245_seqnum,
            { "Sequence number", "sflow_245.sequence_number",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "sFlow datagram sequence number", HFILL}},
        { &hf_sflow_245_sysuptime,
            { "SysUptime", "sflow_245.sysuptime",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "System Uptime", HFILL}},
        { &hf_sflow_245_numsamples,
            { "NumSamples", "sflow_245.numsamples",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Number of samples in sFlow datagram", HFILL}},
        { &hf_sflow_245_sampletype,
            { "sFlow sample type", "sflow_245.sampletype",
                FT_UINT32, BASE_DEC, VALS(sflow_245_sampletype), 0x0,
                "Type of sFlow sample", HFILL}},
        { &hf_sflow_245_sampletype12,
            { "sFlow sample type", "sflow_245.sampletype",
                FT_UINT32, BASE_DEC, VALS(sflow_245_sampletype), 0x00000FFF,
                "Type of sFlow sample", HFILL}},
#if 0
        { &hf_sflow_5_ieee80211_version,
            { "Version", "sflow_245.ieee80211_version",
                FT_UINT32, BASE_DEC, VALS(sflow_5_ieee80211_versions), 0x0,
                "IEEE 802.11 Version", HFILL}},
#endif
        { &hf_sflow_245_ipv4_precedence_type,
            { "Precedence", "sflow_245.ipv4_precedence_type",
                FT_UINT32, BASE_DEC, VALS(sflow_245_ipv4_precedence_types), 0xE0,
                "IPv4 Precedence Type", HFILL}},
        { &hf_sflow_5_flow_record_format,
            { "Format", "sflow_245.flow_record_format",
                FT_UINT32, BASE_DEC | BASE_EXT_STRING, &sflow_5_flow_record_type_ext, 0x00000FFF,
                "Format of sFlow flow record", HFILL}},
        { &hf_sflow_5_counters_record_format,
            { "Format", "sflow_245.counters_record_format",
                FT_UINT32, BASE_DEC, VALS(sflow_5_counters_record_type), 0x00000FFF,
                "Format of sFlow counters record", HFILL}},
        { &hf_sflow_245_header_protocol,
            { "Header protocol", "sflow_245.header_protocol",
                FT_UINT32, BASE_DEC | BASE_EXT_STRING, &sflow_245_header_protocol_ext, 0x0,
                "Protocol of sampled header", HFILL}},
        { &hf_sflow_245_header,
            { "Header of sampled packet", "sflow_245.header",
                FT_BYTES, BASE_NONE, NULL, 0x0,
                "Data from sampled header", HFILL}},
        { &hf_sflow_245_packet_information_type,
            { "Sample type", "sflow_245.packet_information_type",
                FT_UINT32, BASE_DEC, VALS(sflow_245_packet_information_type), 0x0,
                "Type of sampled information", HFILL}},
        { &hf_sflow_245_extended_information_type,
            { "Extended information type", "sflow_245.extended_information_type",
                FT_UINT32, BASE_DEC, VALS(sflow_245_extended_data_types), 0x0,
                "Type of extended information", HFILL}},
        { &hf_sflow_245_vlan_in,
            { "Incoming 802.1Q VLAN", "sflow_245.vlan.in",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Incoming VLAN ID", HFILL}},
        { &hf_sflow_245_vlan_out,
            { "Outgoing 802.1Q VLAN", "sflow_245.vlan.out",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Outgoing VLAN ID", HFILL}},
        { &hf_sflow_245_pri_in,
            { "Incoming 802.1p priority", "sflow_245.pri.in",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_pri_out,
            { "Outgoing 802.1p priority", "sflow_245.pri.out",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_nexthop_v4,
            { "Next hop", "sflow_245.nexthop",
                FT_IPv4, BASE_NONE, NULL, 0x0,
                "Next hop address", HFILL}},
        { &hf_sflow_245_ipv4_src,
            { "Source IP address", "sflow_245.ipv4_src",
                FT_IPv4, BASE_NONE, NULL, 0x0,
                "Source IPv4 address", HFILL}},
        { &hf_sflow_245_ipv4_dst,
            { "Destination IP address", "sflow_245.ipv4_dst",
                FT_IPv4, BASE_NONE, NULL, 0x0,
                "Destination IPv4 address", HFILL}},
        { &hf_sflow_245_nexthop_v6,
            { "Next hop", "sflow_245.nexthop.v6",
                FT_IPv6, BASE_NONE, NULL, 0x0,
                "Next hop address", HFILL}},
        { &hf_sflow_245_ipv6_src,
            { "Source IP address", "sflow_245.ipv6_src",
                FT_IPv6, BASE_NONE, NULL, 0x0,
                "Source IPv6 address", HFILL}},
        { &hf_sflow_245_ipv6_dst,
            { "Destination IP address", "sflow_245.ipv6_dst",
                FT_IPv6, BASE_NONE, NULL, 0x0,
                "Destination IPv6 address", HFILL}},
        { &hf_sflow_245_nexthop_src_mask,
            { "Next hop source mask", "sflow_245.nexthop.src_mask",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Next hop source mask bits", HFILL}},
        { &hf_sflow_245_nexthop_dst_mask,
            { "Next hop destination mask", "sflow_245.nexthop.dst_mask",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Next hop destination mask bits", HFILL}},
        { &hf_sflow_245_ifindex,
            { "Interface index", "sflow_245.ifindex",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_as,
            { "AS Router", "sflow_245.as",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Autonomous System of Router", HFILL}},
        { &hf_sflow_245_src_as,
            { "AS Source", "sflow_245.srcAS",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Autonomous System of Source", HFILL}},
        { &hf_sflow_245_src_peer_as,
            { "AS Peer", "sflow_245.peerAS",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Autonomous System of Peer", HFILL}},
        { &hf_sflow_245_dst_as_entries,
            { "AS Destinations", "sflow_245.dstASentries",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Autonomous System destinations", HFILL}},
        { &hf_sflow_245_dst_as,
            { "AS Destination", "sflow_245.dstAS",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Autonomous System destination", HFILL}},
        /* Needed for sFlow >= 4.  If I had a capture to test... */
        { &hf_sflow_245_community_entries,
            { "Gateway Communities", "sflow_245.communityEntries",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
#if 0
        { &hf_sflow_245_community,
            { "Gateway Community", "sflow_245.community",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Gateway Communities", HFILL}},
#endif
        { &hf_sflow_245_localpref,
            { "localpref", "sflow_245.localpref",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Local preferences of AS route", HFILL}},
        /**/
        { &hf_sflow_245_iftype,
            { "Interface Type", "sflow_245.iftype",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifspeed,
            { "Interface Speed", "sflow_245.ifspeed",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifdirection,
            { "Interface Direction", "sflow_245.ifdirection",
                FT_UINT32, BASE_DEC, VALS(sflow_ifdirection_vals), 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifadmin_status,
            { "IfAdminStatus", "sflow_245.ifadmin_status",
                FT_BOOLEAN, 32, TFS(&tfs_up_down), 0x00000001,
                NULL, HFILL}},
        { &hf_sflow_245_ifoper_status,
            { "IfOperStatus", "sflow_245.ifoper_status",
                FT_BOOLEAN, 32, TFS(&tfs_up_down), 0x00000002,
                NULL, HFILL}},
        { &hf_sflow_245_ifinoct,
            { "Input Octets", "sflow_245.ifinoct",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifinpkt,
            { "Input Packets", "sflow_245.ifinpkt",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifinmcast,
            { "Input Multicast Packets", "sflow_245.ifinmcast",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifinbcast,
            { "Input Broadcast Packets", "sflow_245.ifinbcast",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifindisc,
            { "Input Discarded Packets", "sflow_245.ifindisc",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifinerr,
            { "Input Errors", "sflow_245.ifinerr",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifinunk,
            { "Input Unknown Protocol Packets", "sflow_245.ifinunk",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifoutoct,
            { "Output Octets", "sflow_245.ifoutoct",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifoutpkt,
            { "Output Packets", "sflow_245.ifoutpkt",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifoutmcast,
            { "Output Multicast Packets", "sflow_245.ifoutmcast",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifoutbcast,
            { "Output Broadcast Packets", "sflow_245.ifoutbcast",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifoutdisc,
            { "Output Discarded Packets", "sflow_245.ifoutdisc",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifouterr,
            { "Output Errors", "sflow_245.ifouterr",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ifpromisc,
            { "Promiscuous Mode", "sflow_245.ifpromisc",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_dot3StatsAlignmentErrors,
            { "Alignment Errors", "sflow_245.dot3StatsAlignmentErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Alignment Errors", HFILL}},
        { &hf_sflow_245_dot3StatsFCSErrors,
            { "FCS Errors", "sflow_245.dot3StatsFCSErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats FCS Errors", HFILL}},
        { &hf_sflow_245_dot3StatsSingleCollisionFrames,
            { "Single Collision Frames", "sflow_245.dot3StatsSingleCollisionFrames",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Single Collision Frames", HFILL}},
        { &hf_sflow_245_dot3StatsMultipleCollisionFrames,
            { "Multiple Collision Frames", "sflow_245.dot3StatsMultipleCollisionFrames",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Multiple Collision Frames", HFILL}},
        { &hf_sflow_245_dot3StatsSQETestErrors,
            { "SQE Test Errors", "sflow_245.dot3StatsSQETestErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats SQE Test Errors", HFILL}},
        { &hf_sflow_245_dot3StatsDeferredTransmissions,
            { "Deferred Transmissions", "sflow_245.dot3StatsDeferredTransmissions",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Deferred Transmissions", HFILL}},
        { &hf_sflow_245_dot3StatsLateCollisions,
            { "Late Collisions", "sflow_245.dot3StatsLateCollisions",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Late Collisions", HFILL}},
        { &hf_sflow_245_dot3StatsExcessiveCollisions,
            { "Excessive Collisions", "sflow_245.dot3StatsExcessiveCollisions",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Excessive Collisions", HFILL}},
        { &hf_sflow_245_dot3StatsInternalMacTransmitErrors,
            { "Internal Mac Transmit Errors", "sflow_245.dot3StatsInternalMacTransmitErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Internal Mac Transmit Errors", HFILL}},
        { &hf_sflow_245_dot3StatsCarrierSenseErrors,
            { "Carrier Sense Errors", "sflow_245.dot3StatsCarrierSenseErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Carrier Sense Errors", HFILL}},
        { &hf_sflow_245_dot3StatsFrameTooLongs,
            { "Frame Too Longs", "sflow_245.dot3StatsFrameTooLongs",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Frame Too Longs", HFILL}},
        { &hf_sflow_245_dot3StatsInternalMacReceiveErrors,
            { "Internal Mac Receive Errors", "sflow_245.dot3StatsInternalMacReceiveErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Internal Mac Receive Errors", HFILL}},
        { &hf_sflow_245_dot3StatsSymbolErrors,
            { "Symbol Errors", "sflow_245.dot3StatsSymbolErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot3 Stats Symbol Errors", HFILL}},
        { &hf_sflow_245_dot5StatsLineErrors,
            { "Line Errors", "sflow_245.dot5StatsLineErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Line Errors", HFILL}},
        { &hf_sflow_245_dot5StatsBurstErrors,
            { "Burst Errors", "sflow_245.dot5StatsBurstErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Burst Errors", HFILL}},
        { &hf_sflow_245_dot5StatsACErrors,
            { "AC Errors", "sflow_245.dot5StatsACErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats AC Errors", HFILL}},
        { &hf_sflow_245_dot5StatsAbortTransErrors,
            { "Abort Trans Errors", "sflow_245.dot5StatsAbortTransErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Abort Trans Errors", HFILL}},
        { &hf_sflow_245_dot5StatsInternalErrors,
            { "Internal Errors", "sflow_245.dot5StatsInternalErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Internal Errors", HFILL}},
        { &hf_sflow_245_dot5StatsLostFrameErrors,
            { "Lost Frame Errors", "sflow_245.dot5StatsLostFrameErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Lost Frame Errors", HFILL}},
        { &hf_sflow_245_dot5StatsReceiveCongestions,
            { "Receive Congestions", "sflow_245.dot5StatsReceiveCongestions",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Receive Congestions", HFILL}},
        { &hf_sflow_245_dot5StatsFrameCopiedErrors,
            { "Frame Copied Errors", "sflow_245.dot5StatsFrameCopiedErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Frame Copied Errors", HFILL}},
        { &hf_sflow_245_dot5StatsTokenErrors,
            { "Token Errors", "sflow_245.dot5StatsTokenErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Token Errors", HFILL}},
        { &hf_sflow_245_dot5StatsSoftErrors,
            { "Soft Errors", "sflow_245.dot5StatsSoftErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Soft Errors", HFILL}},
        { &hf_sflow_245_dot5StatsHardErrors,
            { "Hard Errors", "sflow_245.dot5StatsHardErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Hard Errors", HFILL}},
        { &hf_sflow_245_dot5StatsSignalLoss,
            { "Signal Loss", "sflow_245.dot5StatsSignalLoss",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Signal Loss", HFILL}},
        { &hf_sflow_245_dot5StatsTransmitBeacons,
            { "Transmit Beacons", "sflow_245.dot5StatsTransmitBeacons",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Transmit Beacons", HFILL}},
        { &hf_sflow_245_dot5StatsRecoveries,
            { "Recoveries", "sflow_245.dot5StatsRecoveries",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Recoveries", HFILL}},
        { &hf_sflow_245_dot5StatsLobeWires,
            { "Lobe Wires", "sflow_245.dot5StatsLobeWires",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Lobe Wires", HFILL}},
        { &hf_sflow_245_dot5StatsRemoves,
            { "Removes", "sflow_245.dot5StatsRemoves",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Removes", HFILL}},
        { &hf_sflow_245_dot5StatsSingles,
            { "Singles", "sflow_245.dot5StatsSingles",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Singles", HFILL}},
        { &hf_sflow_245_dot5StatsFreqErrors,
            { "Freq Errors", "sflow_245.dot5StatsFreqErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot5 Stats Freq Errors", HFILL}},
        { &hf_sflow_245_dot12InHighPriorityFrames,
            { "In High Priority Frames", "sflow_245.dot12InHighPriorityFrames",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Input High Priority Frames", HFILL}},
        { &hf_sflow_245_dot12InHighPriorityOctets,
            { "In High Priority Octets", "sflow_245.dot12InHighPriorityOctets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                "dot12 Input High Priority Octets", HFILL}},
        { &hf_sflow_245_dot12InNormPriorityFrames,
            { "In Normal Priority Frames", "sflow_245.dot12InNormPriorityFrames",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Input Normal Priority Frames", HFILL}},
        { &hf_sflow_245_dot12InNormPriorityOctets,
            { "In Normal Priority Octets", "sflow_245.dot12InNormPriorityOctets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                "dot12 Input Normal Priority Octets", HFILL}},
        { &hf_sflow_245_dot12InIPMErrors,
            { "In IPM Errors", "sflow_245.dot12InIPMErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Input IPM Errors", HFILL}},
        { &hf_sflow_245_dot12InOversizeFrameErrors,
            { "In Oversize Frame Errors", "sflow_245.dot12InOversizeFrameErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Input Oversize Frame Errors", HFILL}},
        { &hf_sflow_245_dot12InDataErrors,
            { "In Data Errors", "sflow_245.dot12InDataErrors",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Input Data Errors", HFILL}},
        { &hf_sflow_245_dot12InNullAddressedFrames,
            { "In Null Addressed Frames", "sflow_245.dot12InNullAddressedFrames",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Input Null Addressed Frames", HFILL}},
        { &hf_sflow_245_dot12OutHighPriorityFrames,
            { "Out High Priority Frames", "sflow_245.dot12OutHighPriorityFrames",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Output High Priority Frames", HFILL}},
        { &hf_sflow_245_dot12OutHighPriorityOctets,
            { "Out High Priority Octets", "sflow_245.dot12OutHighPriorityOctets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                "dot12 Out High Priority Octets", HFILL}},
        { &hf_sflow_245_dot12TransitionIntoTrainings,
            { "Transition Into Trainings", "sflow_245.dot12TransitionIntoTrainings",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "dot12 Transition Into Trainings", HFILL}},
        { &hf_sflow_245_dot12HCInHighPriorityOctets,
            { "HC In High Priority Octets", "sflow_245.dot12HCInHighPriorityOctets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                "dot12 HC Input High Priority Octets", HFILL}},
        { &hf_sflow_245_dot12HCInNormPriorityOctets,
            { "HC In Normal Priority Octets", "sflow_245.dot12HCInNormPriorityOctets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                "dot12 HC Input Normal Priority Octets", HFILL}},
        { &hf_sflow_245_dot12HCOutHighPriorityOctets,
            { "HC Out High Priority Octets", "sflow_245.dot12HCOutHighPriorityOctets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                "dot12 HC Output High Priority Octets", HFILL}},
        { &hf_sflow_245_vlan_id,
            { "VLAN ID", "sflow_245.vlan_id",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_octets,
            { "Octets", "sflow_245.octets",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_ucastPkts,
            { "Unicast Packets", "sflow_245.ucastPkts",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_multicastPkts,
            { "Multicast Packets", "sflow_245.multicastPkts",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_broadcastPkts,
            { "Broadcast Packets", "sflow_245.broadcastPkts",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_245_discards,
            { "Discards", "sflow_245.discards",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11TransmittedFragmentCount,
            { "Transmitted Fragment Count", "sflow_5.dot11TransmittedFragmentCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11MulticastTransmittedFrameCount,
            { "Multicast Transmitted Frame Count", "sflow_5.dot11MulticastTransmittedFrameCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11FailedCount,
            { "Failed Count", "sflow_5.dot11FailedCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11RetryCount,
            { "Retry Count", "sflow_5.dot11RetryCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11MultipleRetryCount,
            { "Multiple Retry Count", "sflow_5.dot11MultipleRetryCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11FrameDuplicateCount,
            { "Frame Duplicate Count", "sflow_5.dot11FrameDuplicateCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11RTSSuccessCount,
            { "RTS Success Count", "sflow_5.dot11RTSSuccessCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11RTSFailureCount,
            { "Failure Count", "sflow_5.dot11RTSFailureCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11ACKFailureCount,
            { "ACK Failure Count", "sflow_5.dot11ACKFailureCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11ReceivedFragmentCount,
            { "Received Fragment Count", "sflow_5.dot11ReceivedFragmentCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11MulticastReceivedFrameCount,
            { "Multicast Received Frame Count", "sflow_5.dot11MulticastReceivedFrameCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11FCSErrorCount,
            { "FCS Error Count", "sflow_5.dot11FCSErrorCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11TransmittedFrameCount,
            { "Transmitted Frame Count", "sflow_5.dot11TransmittedFrameCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11WEPUndecryptableCount,
            { "WEP Undecryptable Count", "sflow_5.dot11WEPUndecryptableCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11QoSDiscardedFragmentCount,
            { "QoS Discarded Fragment Count", "sflow_5.dot11QoSDiscardedFragmentCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11AssociatedStationCount,
            { "Associated Station Count", "sflow_5.dot11AssociatedStationCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11QoSCFPollsReceivedCount,
            { "QoS CF Polls Received Count", "sflow_5.dot11QoSCFPollsReceivedCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11QoSCFPollsUnusedCount,
            { "QoS CF Polls Unused Count", "sflow_5.dot11QoSCFPollsUnusedCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11QoSCFPollsUnusableCount,
            { "QoS CF Polls Unusable Count", "sflow_5.dot11QoSCFPollsUnusableCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_dot11QoSCFPollsLostCount,
            { "QoS CF Polls Lost Count", "sflow_5.dot11QoSCFPollsLostCount",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_cpu_5s,
            { "5s CPU Load (100 = 1%)", "sflow_5.cpu_5s",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Average CPU Load Over 5 Seconds (100 = 1%)", HFILL}},
        { &hf_sflow_5_cpu_1m,
            { "1m CPU Load (100 = 1%)", "sflow_5.cpu_1m",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Average CPU Load Over 1 Minute (100 = 1%)", HFILL}},
        { &hf_sflow_5_cpu_5m,
            { "5m CPU Load (100 = 1%)", "sflow_5.cpu_5m",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Average CPU Load Over 5 Minutes (100 = 1%)", HFILL}},
        { &hf_sflow_5_total_memory,
            { "Total Memory", "sflow_5.total_memory",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_free_memory,
            { "Free Memory", "sflow_5.free_memory",
                FT_UINT64, BASE_DEC, NULL, 0x0,
                NULL, HFILL}},
        { &hf_sflow_5_elapsed_time,
            { "Elapsed Time (ms)", "sflow_5.elapsed_time",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Elapsed Time in ms", HFILL}},
        { &hf_sflow_5_on_channel_time,
            { "On Channel (ms)", "sflow_5.on_channel_time",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Time in ms Spent on Channel", HFILL}},
        { &hf_sflow_5_on_channel_busy_time,
            { "On Channel Busy (ms)", "sflow_5.channel_busy_time",
                FT_UINT32, BASE_DEC, NULL, 0x0,
                "Time in ms Spent on Channel and Busy", HFILL}},

      /* Generated from convert_proto_tree_add_text.pl */
      { &hf_sflow_245_header_frame_length,
        { "Frame Length", "sflow_245.header.frame_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_header_payload_stripped,
        { "Payload stripped", "sflow_245.header.payload_stripped",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_sampled_header_length,
        { "Sampled header length", "sflow_245.header.sampled_header_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_extended_mpls_in_label_stack_entries,
        { "In Label Stack Entries", "sflow_245.extended_mpls.in_label_stack_entries",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_extended_mpls_in_label,
        { "Label", "sflow_245.extended_mpls.in_label",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_extended_mpls_out_label_stack_entries,
        { "Out Label Stack Entries", "sflow_245.extended_mpls.out_label_stack_entries",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_extended_mpls_out_label,
        { "Label", "sflow_245.extended_mpls.out_label",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ethernet_length_of_mac_packet,
        { "Length of MAC Packet", "sflow_245.ethernet.length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ethernet_source_mac_address,
        { "Source MAC Address", "sflow_245.ethernet.source_mac_address",
          FT_ETHER, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ethernet_destination_mac_address,
        { "Destination MAC Address", "sflow_245.ethernet.destination_mac_address",
          FT_ETHER, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ethernet_packet_type,
        { "Ethernet Packet Type", "sflow_245.ethernet.packet_type",
          FT_UINT32, BASE_HEX, VALS(etype_vals), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_length_of_ip_packet,
        { "Length of IP Packet", "sflow_245.ip.length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_source_port,
        { "Source Port", "sflow_245.ip.source_port",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_destination_port,
        { "Destination Port", "sflow.ip.destination_port",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_cwr,
        { "TCP Flag (CWR)", "sflow_245.ip.tcp_flag.cwr",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000080,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_ece,
        { "TCP Flag (ECE)", "sflow_245.ip.tcp_flag.ece",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000040,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_urg,
        { "TCP Flag (URG)", "sflow_245.ip.tcp_flag.urg",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000020,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_ack,
        { "TCP Flag (ACK)", "sflow_245.ip.tcp_flag.ack",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000010,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_psh,
        { "TCP Flag (PSH)", "sflow_245.ip.tcp_flag.psh",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000008,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_rst,
        { "TCP Flag (RST)", "sflow_245.ip.tcp_flag.rst",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000004,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_syn,
        { "TCP Flag (SYN)", "sflow_245.ip.tcp_flag.syn",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000002,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_tcp_flag_fin,
        { "TCP Flag (FIN)", "sflow_245.ip.tcp_flag.fin",
          FT_BOOLEAN, 32, TFS(&tfs_set_notset), 0x00000001,
          NULL, HFILL }
      },
      { &hf_sflow_245_ipv4_delay,
        { "Delay", "sflow_245.ipv4_delay",
          FT_BOOLEAN, 32, TFS(&tfs_low_normal), 0x00000010,
          NULL, HFILL }
      },
      { &hf_sflow_245_ipv4_throughput,
        { "Throughput", "sflow_245.ipv4_throughput",
          FT_BOOLEAN, 32, TFS(&tfs_high_normal), 0x00000008,
          NULL, HFILL }
      },
      { &hf_sflow_245_ipv4_reliability,
        { "Reliability", "sflow_245.ipv4_reliability",
          FT_BOOLEAN, 32, TFS(&tfs_high_normal), 0x00000004,
          NULL, HFILL }
      },
      { &hf_sflow_245_ipv4_cost,
        { "Cost (RFC1349)", "sflow_245.ipv4_cost",
          FT_BOOLEAN, 32, TFS(&tfs_minimize_monetary_normal), 0x00000002,
          NULL, HFILL }
      },
      { &hf_sflow_245_ipv6_priority,
        { "Priority", "sflow_245.ipv6_priority",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_user_source_character_set,
        { "Source Character Set", "sflow_5.extended_user.source_character_set",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_user_source_user_string_length,
        { "Source User String Length (bytes)", "sflow_5.extended_user.source_user_string_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_user_destination_character_set,
        { "Destination Character Set", "sflow_5.extended_user.destination_character_set",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_user_destination_user_string_length,
        { "Destination User String Length (bytes)", "sflow_5.extended_user.destination_user_string_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_url_url_length,
        { "URL Length (bytes)", "sflow_5.extended_url.url_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_url_host_length,
        { "Host Length (bytes)", "sflow_5.extended_url.host_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_tunnel_name_length,
        { "Tunnel Name Length (bytes)", "sflow_5.extended_mpls_tunnel.name_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_tunnel_id,
        { "Tunnel ID", "sflow_5.extended_mpls_tunnel.id",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_tunnel_cos_value,
        { "Tunnel COS Value", "sflow_5.extended_mpls_tunnel.cos_value",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_vc_instance_name_length,
        { "VC Instance Name Length (bytes)", "sflow_5.extended_mpls_vc.instance_name_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_vc_id,
        { "VLL/VC ID", "sflow_5.extended_mpls_vc.id",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_vc_label_cos_value,
        { "VC Label COS Value", "sflow_5.extended_mpls_vc.label_cos_value",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_ftn_description_length,
        { "MPLS FTN Description Length (bytes)", "sflow_5.extended_mpls.ftn_description_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_ftn_mask,
        { "MPLS FTN Mask", "sflow_5.extended_mpls.ftn_mask",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_fec_address_prefix_length,
        { "MPLS FEC Address Prefix Length (bytes)", "sflow_5.extended_mpls.fec_address_prefix_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_vlan_tunnel_number_of_layers,
        { "Number of Layers", "sflow_5.extended_vlan_tunnel.number_of_layers",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_vlan_tunnel_tpid_tci_pair,
        { "TPID/TCI Pair as Integer", "sflow_5.extended_vlan_tunnel.tpid_tci_pair",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_oui,
        { "OUI", "sflow_5.extended_80211.oui",
          FT_UINT24, BASE_HEX, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_suite_type,
        { "Suite Type", "sflow_5.extended_80211.suite_type",
          FT_UINT8, BASE_DEC, VALS(extended_80211_suite_type_vals), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_payload_length,
        { "Payload Length", "sflow_5.extended_80211.payload_length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_bssid,
        { "BSSID", "sflow_5.extended_80211.rx.bssid",
          FT_ETHER, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_version,
        { "Version", "sflow_5.extended_80211.rx.version",
          FT_UINT32, BASE_DEC, VALS(sflow_5_ieee80211_versions), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_channel,
        { "Channel", "sflow_5.extended_80211.rx.channel",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_speed,
        { "Speed", "sflow_5.extended_80211.rx.speed",
          FT_UINT64, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_rsni,
        { "RSNI", "sflow_5.extended_80211.rx.rsni",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_rcpi,
        { "RCPI", "sflow_5.extended_80211.rx.rcpi",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_packet_duration,
        { "Packet Duration (ms)", "sflow_5.extended_80211.rx.packet_duration",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_bssid,
        { "BSSID", "sflow_5.extended_80211.tx.bssid",
          FT_ETHER, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_version,
        { "Version", "sflow_5.extended_80211.tx.version",
          FT_UINT32, BASE_DEC, VALS(sflow_5_ieee80211_versions), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_retransmissions,
        { "Retransmissions", "sflow_5.extended_80211.tx.retransmissions",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_packet_duration,
        { "Packet Duration (ms)", "sflow_5.extended_80211.tx.packet_duration",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_retransmission_duration,
        { "Retransmission Duration (ms)", "sflow_5.extended_80211.tx.retransmission_duration",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_channel,
        { "Channel", "sflow_5.extended_80211.tx.channel",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_speed,
        { "Speed", "sflow_5.extended_80211.tx.speed",
          FT_UINT64, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_power,
        { "Power", "sflow_5.extended_80211.tx.power",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_sequence_number,
        { "Sequence number", "sflow.flow_sample.sequence_number",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_source_id_class,
        { "Source ID class", "sflow.flow_sample.source_id_class",
          FT_UINT32, BASE_DEC, NULL, 0xFF000000,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_sampling_rate,
        { "Sampling rate", "sflow.flow_sample.sampling_rate",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_sample_pool,
        { "Sample pool", "sflow.flow_sample.sample_pool",
          FT_UINT32, BASE_DEC|BASE_UNIT_STRING, UNS(&units_total_packets), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_dropped_packets,
        { "Dropped packets", "sflow.flow_sample.dropped_packets",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_input_interface,
        { "Input interface (ifIndex)", "sflow.flow_sample.input_interface",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_24_flow_sample_multiple_outputs,
        { "Multiple outputs", "sflow.flow_sample.multiple_outputs",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_expanded_format,
        { "Output interface expanded format", "sflow.flow_sample.output_interface.expanded.format",
          FT_UINT32, BASE_DEC, VALS(interface_format), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_24_flow_sample_output_interface,
        { "Output interface (ifIndex)", "sflow.flow_sample.output_interface",
          FT_UINT32, BASE_DEC, NULL, 0x7fffffff,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface,
        { "Output interface", "sflow.flow_sample.output_interface",
          FT_UINT32, BASE_HEX, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_form,
        { "Output interface format", "sflow.flow_sample.output_interface_format",
          FT_UINT32, BASE_DEC, VALS(interface_format), SFLOW_5_INT_FORMAT,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_val,
        { "Output interface value", "sflow.flow_sample.output_interface_value",
          FT_UINT32, BASE_DEC, NULL, SFLOW_5_INT_VALUE,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_val_discard,
        { "Output interface value", "sflow.flow_sample.output_interface_value",
          FT_UINT32, BASE_DEC, VALS(interface_discard), SFLOW_5_INT_VALUE,
          NULL, HFILL }
      },
      { &hf_sflow_enterprise,
        { "Enterprise", "sflow.enterprise",
          FT_UINT32, BASE_DEC, NULL, 0xFFFFF000,
          NULL, HFILL }
      },
      { &hf_sflow_enterprise_length,
        { "Length", "sflow.enterprise.length",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_enterprise_data,
        { "Data", "sflow.enterprise.data",
          FT_BYTES, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_flow_record,
        { "Flow record", "sflow.flow_sample.flow_record",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_source_id_type,
        { "Source ID type", "sflow.flow_sample.source_id_type",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_source_id_index,
        { "Source ID index", "sflow.flow_sample.source_id_index",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_input_interface_format,
        { "Input interface format", "sflow.flow_sample.input_interface_format",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_input_interface_value,
        { "Input interface value", "sflow.flow_sample.input_interface_value",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_24_flow_sample_output_interface_value,
        { "Output interface value", "sflow.flow_sample.output_interface_value",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_expanded_value,
        { "Output interface expanded value", "sflow.flow_sample.output_interface_expanded.value",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_expanded_value_discarded,
        { "Output interface packet discarded", "sflow.flow_sample.output_interface_expanded.value_discarded",
          FT_UINT32, BASE_DEC, VALS(interface_discard), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_expanded_value_number,
        { "Output inferface number of interfaces", "sflow.flow_sample.output_interface_expanded.number",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_flow_sample_output_interface_expanded_value_ifindex,
        { "Output interface ifIndex", "sflow.flow_sample.output_interface_expanded.ifindex",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_sequence_number,
        { "Sequence number", "sflow.counters_sample.sequence_number",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_source_id_class,
        { "Source ID class", "sflow.counters_sample.source_id_class",
          FT_UINT32, BASE_DEC, NULL, 0xFF000000,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_sampling_interval,
        { "Sampling Interval", "sflow.counters_sample.sampling_interval",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_counters_type,
        { "Counters type", "sflow.counters_sample.counters_type",
          FT_UINT32, BASE_DEC, VALS(sflow_245_counterstype), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_source_id_type,
        { "Source ID type", "sflow.counters_sample.source_id_type",
          FT_UINT32, BASE_DEC, NULL, 0xFF000000,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_source_id_index,
        { "Source ID index", "sflow.counters_sample.source_id_index",
          FT_UINT32, BASE_DEC, NULL, 0x00FFFFFF,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_counters_records,
        { "Counters records", "sflow.counters_sample.counters_records",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_expanded_source_id_type,
        { "Source ID type", "sflow.counters_sample.source_id_type",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_expanded_source_id_index,
        { "Source ID index", "sflow.counters_sample.source_id_index",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_padding,
        { "Padding", "sflow.lag_port.padding",
          FT_BYTES, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_actorsystemid,
        { "Actor System ID", "sflow.lag_port.actor_system_id",
          FT_ETHER, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_partneropersystemid,
        { "Partner Oper System ID", "sflow.lag_port.partner_oper_system_id",
          FT_ETHER, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_attachedaggid,
        { "Port Attached Agg ID", "sflow.lag_port.attached_agg_id",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_state,
        { "State", "sflow.lag_port.state",
          FT_UINT32, BASE_HEX, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_actoradminstate,
        { "Actor Admin State", "sflow.lag_port.actor_admin_state",
          FT_BOOLEAN, 32, NULL, 0x00000001,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_actoroperstate,
        { "Actor Oper State", "sflow.lag_port.actor_oper_state",
          FT_BOOLEAN, 32, NULL, 0x00000002,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_partneradminstate,
        { "Partner Admin State", "sflow.lag_port.partner_admin_state",
          FT_BOOLEAN, 32, NULL, 0x00000004,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_partneroperstate,
        { "Partner Oper State", "sflow.lag_port.partner_oper_state",
          FT_BOOLEAN, 32, NULL, 0x00000008,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_reserved,
        { "Reserved", "sflow.lag_port.reserved",
          FT_UINT32, BASE_HEX, NULL, 0xFFFFFFF0,
          NULL, HFILL }
      },
      { &hf_sflow_5_lag_port_actoradminstate,
        { "Actor Admin State", "sflow.lag_port.actor_admin_state",
          FT_BOOLEAN, 32, NULL, 0x000000FF,
          NULL, HFILL }
      },
      { &hf_sflow_5_lag_port_actoroperstate,
        { "Actor Oper State", "sflow.lag_port.actor_oper_state",
          FT_BOOLEAN, 32, NULL, 0x0000FF00,
          NULL, HFILL }
      },
      { &hf_sflow_5_lag_port_partneradminstate,
        { "Partner Admin State", "sflow.lag_port.partner_admin_state",
          FT_BOOLEAN, 32, NULL, 0x00FF0000,
          NULL, HFILL }
      },
      { &hf_sflow_5_lag_port_partneroperstate,
        { "Partner Oper State", "sflow.lag_port.partner_oper_state",
          FT_BOOLEAN, 32, NULL, 0xFF000000,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_lacpdusrx,
        { "LACPDUs Rx", "sflow.lag_port.lacpdus.rx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_markerpdusrx,
        { "Marker PDUs Rx", "sflow.lag_port.marker_pdus.rx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_markerresponsepdusrx,
        { "Marker Response PDUs Rx", "sflow.lag_port.marker_response_pdus.rx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_unknownrx,
        { "Unknown Rx", "sflow.lag_port.unknown.rx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_illegalrx,
        { "Illegal Rx", "sflow.lag_port.illegal.rx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_lacpdustx,
        { "LACPDUs Tx", "sflow.lag_port.lacpdus.tx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_markerpdustx,
        { "Marker PDUs Tx", "sflow.lag_port.marker_pdus.tx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_lag_port_stats_markerresponsepdustx,
        { "Marker Response PDUs Tx", "sflow.lag_port.marker_response_pdus.tx",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },

      { &hf_sflow_245_as_type,
        { "AS Type", "sflow.as_type",
          FT_UINT32, BASE_DEC, VALS(sflow_245_as_types), 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_245_ip_protocol,
        { "IP Protocol", "sflow.ip_protocol",
          FT_UINT32, BASE_DEC|BASE_EXT_STRING, &ipproto_val_ext, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_user_source_user,
        { "Source User", "sflow_5.extended_user.source_user",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_user_destination_user,
        { "Destination User", "sflow_5.extended_user.destination_user",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_url_direction,
        { "Direction", "sflow_5.extended_url.direction",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_url_url,
        { "URL", "sflow_5.extended_url.url",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_url_host,
        { "Host", "sflow_5.extended_url.host",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_tunnel_name,
        { "Tunnel Name", "sflow_5.extended_mpls_tunnel.tunnel_name",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_vc_instance_name,
        { "VC Instance Name", "sflow_5.extended_mpls_vc.vc_instance_name",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_mpls_ftn_description,
        { "MPLS FTN Description", "sflow_5.extended_mpls.ftn_description",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_payload,
        { "Payload", "sflow_5.extended_80211.payload",
          FT_BYTES, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_rx_ssid,
        { "SSID", "sflow_5.extended_80211.rx.ssid",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_5_extended_80211_tx_ssid,
        { "SSID", "sflow_5.extended_80211.tx.ssid",
          FT_STRING, BASE_NONE, NULL, 0x0,
          NULL, HFILL }
      },
      { &hf_sflow_flow_sample_index,
        { "Index", "sflow.flow_sample.index",
          FT_UINT32, BASE_DEC, NULL, 0x00FFFFFF,
          NULL, HFILL }
      },
      { &hf_sflow_counters_sample_index,
        { "Index", "sflow.counters_sample.index",
          FT_UINT32, BASE_DEC, NULL, 0x0,
          NULL, HFILL }
      },
    };

    /* Setup protocol subtree array */
    static int * ett[] = {
        &ett_sflow_245,
        &ett_sflow_245_sample,
        &ett_sflow_5_flow_record,
        &ett_sflow_5_counters_record,
        &ett_sflow_5_mpls_in_label_stack,
        &ett_sflow_5_mpls_out_label_stack,
        &ett_sflow_245_extended_data,
        &ett_sflow_245_gw_as_dst,
        &ett_sflow_245_gw_as_dst_seg,
        &ett_sflow_245_gw_community,
        &ett_sflow_245_sampled_header,
        &ett_sflow_lag_port_state_flags,
        &ett_sflow_5_output_interface,
    };

    static ei_register_info ei[] = {
        { &ei_sflow_invalid_address_type, { "sflow.invalid_address_type", PI_MALFORMED, PI_ERROR, "Unknown/invalid address type", EXPFILL }},
    };

    expert_module_t* expert_sflow;

    /* Register the protocol name and description */
    proto_sflow = proto_register_protocol("InMon sFlow", "sFlow", "sflow");

    /* Required function calls to register the header fields and subtrees used */
    proto_register_field_array(proto_sflow, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    expert_sflow = expert_register_protocol(proto_sflow);
    expert_register_field_array(expert_sflow, ei, array_length(ei));

    header_subdissector_table  = register_dissector_table("sflow_245.header_protocol", "SFLOW header protocol", proto_sflow, FT_UINT32, BASE_DEC);

    /* Register our dissector handle */
    sflow_handle = register_dissector("sflow", dissect_sflow_245, proto_sflow);

    /* Register our configuration options for sFlow */
    sflow_245_module = prefs_register_protocol(proto_sflow, NULL);

    /*
       If I use a filter like "ip.src == 10.1.1.1" this will, in
       addition to the usual suspects, find every sFlow packet
       where *any* of the payload headers contain 10.1.1.1 as a
       src addr.  I think this may not be the desired behavior.
       It can certainly be confusing since the ip.src being found
       is buried about 3 subtrees deep and the subtrees might be
       under any one of the sampled (payload) header trees. It is
       certainly not quickly obvious why the filter matched.
     */
    prefs_register_bool_preference(sflow_245_module, "enable_dissection",
            "Dissect data in sampled headers",
            "Enabling dissection makes it easy to view protocol details in each of the sampled headers."
            "  Disabling dissection may reduce noise caused when display filters match the contents of"
            " any sampled header(s).",
            &global_dissect_samp_headers);
    /*
       It is not clear to me that it *ever* makes sense to enable
       this option.  However, it was previously the default
       behavior so I'll leave it as an option if someone thinks
       they have a use for it.
     */
    prefs_register_bool_preference(sflow_245_module, "enable_analysis",
            "Analyze data in sampled IP headers",
            "This option only makes sense if dissection of sampled headers is enabled and probably not even then.",
            &global_analyze_samp_ip_headers);
}

void
proto_reg_handoff_sflow_245(void) {

    dissector_add_uint_range_with_preference("udp.port", SFLOW_UDP_PORTS, sflow_handle);
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
