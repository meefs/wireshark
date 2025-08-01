/* packet-dplay.c
 * This is a dissector for the DirectPlay protocol.
 * Copyright 2006 - 2008 by Kai Blin
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "config.h"


#include <epan/packet.h>
#include <epan/aftypes.h>
#include <epan/tfs.h>
#include "packet-smb-common.h"

/* function declarations */
void proto_register_dplay(void);
void proto_reg_handoff_dplay(void);

static int dissect_type1a_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset);

static int proto_dplay;

/* Common data fields */
static int hf_dplay_size;              /* Size of the whole data */
static int hf_dplay_token;
static int hf_dplay_saddr_af;          /* WINSOCK_AF_INET, as this dissector does not handle IPX yet */
static int hf_dplay_saddr_port;        /* port to use for the reply to this packet */
static int hf_dplay_saddr_ip;          /* IP to use for the reply to this packet, or 0.0.0.0,
                                               then use the same IP as this packet used. */
static int hf_dplay_saddr_padding;     /* null padding used in s_addr_in structures */
static int hf_dplay_play_str;          /* always "play" without a null terminator */
static int hf_dplay_command;           /* the dplay command this message contains*/
static int hf_dplay_proto_dialect;     /* 0x0b00 for dplay7, 0x0e00 for dplay9 */
static int hf_dplay_play_str_2;        /* packet type 0x0015 encapsulates another packet */
static int hf_dplay_command_2;         /* that also has a "play" string, a command and a */
static int hf_dplay_proto_dialect_2;   /* protocol dialect, same as above */
static const int DPLAY_HEADER_OFFSET = 28;  /* The dplay header is 28 bytes in size */
static int hf_dplay_player_msg;

/* The following fields are not part of the header, but hopefully have the same
 * meaning for all packets they show up in. */

static int hf_dplay_sess_desc_flags; /* This is a 32bit field with some sort of a flag */
static int hf_dplay_flags_no_create_players;
static int hf_dplay_flags_0002;
static int hf_dplay_flags_migrate_host;
static int hf_dplay_flags_short_player_msg;
static int hf_dplay_flags_ignored;
static int hf_dplay_flags_can_join;
static int hf_dplay_flags_use_ping;
static int hf_dplay_flags_no_player_updates;
static int hf_dplay_flags_use_auth;
static int hf_dplay_flags_private_session;
static int hf_dplay_flags_password_req;
static int hf_dplay_flags_route;
static int hf_dplay_flags_server_player_only;
static int hf_dplay_flags_reliable;
static int hf_dplay_flags_preserve_order;
static int hf_dplay_flags_optimize_latency;
static int hf_dplay_flags_acqire_voice;
static int hf_dplay_flags_no_sess_desc_changes;

#define DPLAY_FLAG_NO_CREATE_PLAYERS 0x00000001
#define DPLAY_FLAG_0002 0x00000002
#define DPLAY_FLAG_MIGRATE_HOST 0x00000004
#define DPLAY_FLAG_SHORT_PLAYER_MSG 0x00000008
#define DPLAY_FLAG_IGNORED 0x00000010
#define DPLAY_FLAG_CAN_JOIN 0x00000020
#define DPLAY_FLAG_USE_PING 0x00000040
#define DPLAY_FLAG_NO_P_UPD 0x00000080
#define DPLAY_FLAG_USE_AUTH 0x00000100
#define DPLAY_FLAG_PRIV_SESS 0x00000200
#define DPLAY_FLAG_PASS_REQ 0x00000400
#define DPLAY_FLAG_ROUTE 0x00000800
#define DPLAY_FLAG_SRV_ONLY 0x00001000
#define DPLAY_FLAG_RELIABLE 0x00002000
#define DPLAY_FLAG_ORDER 0x00004000
#define DPLAY_FLAG_OPT_LAT 0x00008000
#define DPLAY_FLAG_ACQ_VOICE 0x00010000
#define DPLAY_FLAG_NO_SESS_DESC_CHANGES 0x00020000

/* Session description structure fields */
static int hf_dplay_sess_desc_length;
static int hf_dplay_game_guid;
static int hf_dplay_instance_guid;
static int hf_dplay_max_players;
static int hf_dplay_curr_players;
static int hf_dplay_sess_name_ptr;
static int hf_dplay_passwd_ptr;
static int hf_dplay_sess_desc_reserved_1;
static int hf_dplay_sess_desc_reserved_2;
static int hf_dplay_sess_desc_user_1;
static int hf_dplay_sess_desc_user_2;
static int hf_dplay_sess_desc_user_3;
static int hf_dplay_sess_desc_user_4;

/* PackedPlayer structure fields */
static int hf_dplay_pp_size;
static int hf_dplay_pp_flags;
static int hf_dplay_pp_flag_sysplayer;
static int hf_dplay_pp_flag_nameserver;
static int hf_dplay_pp_flag_in_group;
static int hf_dplay_pp_flag_sending;
static int hf_dplay_pp_id;
static int hf_dplay_pp_short_name_len;
static int hf_dplay_pp_long_name_len;
static int hf_dplay_pp_sp_data_size;
static int hf_dplay_pp_player_data_size;
static int hf_dplay_pp_num_players;
static int hf_dplay_pp_system_player;
static int hf_dplay_pp_fixed_size;
static int hf_dplay_pp_dialect;
static int hf_dplay_pp_unknown_1;
static int hf_dplay_pp_short_name;
static int hf_dplay_pp_long_name;
static int hf_dplay_pp_sp_data;
static int hf_dplay_pp_player_data;
static int hf_dplay_pp_player_id;
static int hf_dplay_pp_parent_id;
#define DPLAY_PP_FLAG_SYSPLAYER 0x00000001
#define DPLAY_PP_FLAG_NAMESERVER 0x00000002
#define DPLAY_PP_FLAG_IN_GROUP 0x00000004
#define DPLAY_PP_FLAG_SENDING 0x00000008

/* SuperPackedPlayer structure fields */
static int hf_dplay_spp_size;
static int hf_dplay_spp_flags;
static int hf_dplay_spp_flags_sysplayer;
static int hf_dplay_spp_flags_nameserver;
static int hf_dplay_spp_flags_in_group;
static int hf_dplay_spp_flags_sending;
static int hf_dplay_spp_id;
static int hf_dplay_spp_player_info_mask;
static int hf_dplay_spp_have_short_name;
static int hf_dplay_spp_have_long_name;
static int hf_dplay_spp_sp_length_type;
static int hf_dplay_spp_pd_length_type;
static int hf_dplay_spp_player_count_type;
static int hf_dplay_spp_have_parent_id;
static int hf_dplay_spp_shortcut_count_type;
#define DPLAY_SPP_INF_FLAG_HAVE_SHORT_NAME     0x00000001
#define DPLAY_SPP_INF_FLAG_HAVE_LONG_NAME      0x00000002
#define DPLAY_SPP_INF_FLAG_SP_LENGTH_TYPE      0x0000000C
#define DPLAY_SPP_INF_FLAG_PD_LENGTH_TYPE      0x00000030
#define DPLAY_SPP_INF_FLAG_PLAYER_COUNT_TYPE   0x000000C0
#define DPLAY_SPP_INF_FLAG_HAVE_PARENT_ID      0x00000100
#define DPLAY_SPP_INF_FLAG_SHORTCUT_COUNT_TYPE 0x00000600

static int hf_dplay_spp_dialect;
static int hf_dplay_spp_sys_player_id;
static int hf_dplay_spp_short_name;
static int hf_dplay_spp_long_name;
static int hf_dplay_spp_player_data_length;
static int hf_dplay_spp_player_data;
static int hf_dplay_spp_sp_data_length;
static int hf_dplay_spp_sp_data;
static int hf_dplay_spp_player_count;
static int hf_dplay_spp_player_id;
static int hf_dplay_spp_parent_id;
static int hf_dplay_spp_shortcut_count;
static int hf_dplay_spp_shortcut_id;
#define DPLAY_SPP_FLAG_SYSPLAYER 0x00000001
#define DPLAY_SPP_FLAG_NAMESERVER 0x00000002
#define DPLAY_SPP_FLAG_IN_GROUP 0x00000004
#define DPLAY_SPP_FLAG_SENDING 0x00000008

/* SecurityDesc structure fields */
static int hf_dplay_sd_size;
static int hf_dplay_sd_flags;
static int hf_dplay_sd_sspi;
static int hf_dplay_sd_capi;
static int hf_dplay_sd_capi_type;
static int hf_dplay_sd_enc_alg;

/* Message Type 0x0001 data fields */
static int hf_dplay_type_01_name_offset;
static int hf_dplay_type_01_game_name;

/* Message Type 0x0002 data fields */
static int hf_dplay_type_02_game_guid;
static int hf_dplay_type_02_password_offset;
static int hf_dplay_type_02_flags;
static int hf_dplay_type_02_password;
static int hf_enum_sess_flag_join;
static int hf_enum_sess_flag_all;
static int hf_enum_sess_flag_passwd;
#define DPLAY_ENUM_SESS_FLAG_JOIN 0x00000001
#define DPLAY_ENUM_SESS_FLAG_ALL 0x00000002
#define DPLAY_ENUM_SESS_FLAG_PASSWD 0x00000040

/* Message Type 0x0005 data fields */
static int hf_dplay_type_05_flags;
static int hf_dplay_type_05_system_player;
static int hf_dplay_type_05_name_server;
static int hf_dplay_type_05_local;
static int hf_dplay_type_05_unknown; /* unknown, but always set */
static int hf_dplay_type_05_secure;
#define DPLAY_TYPE05_FLAG_SYSPLAYER 0x00000001
#define DPLAY_TYPE05_FLAG_NAMESERVER 0x00000002
#define DPLAY_TYPE05_FLAG_LOCAL 0x00000004
#define DPLAY_TYPE05_FLAG_UNKNOWN 0x00000008
#define DPLAY_TYPE05_FLAG_SECURE 0x00000200

/* Message Type 0x0007 data fields */
static int hf_dplay_type_07_dpid;
static int hf_dplay_type_07_sspi_offset;
static int hf_dplay_type_07_capi_offset;
static int hf_dplay_type_07_hresult;
static int hf_dplay_type_07_sspi;
static int hf_dplay_type_07_capi;

/* Data fields for message types 0x08, 0x09, 0x0b, 0x0c, 0x0d, 0x0e */
static int hf_dplay_multi_id_to;
static int hf_dplay_multi_player_id;
static int hf_dplay_multi_group_id;
static int hf_dplay_multi_create_offset;
static int hf_dplay_multi_password_offset;
static int hf_dplay_multi_password;

/* Message Type 0x000f data fields */
static int hf_dplay_type_0f_id_to;
static int hf_dplay_type_0f_id;
static int hf_dplay_type_0f_data_size;
static int hf_dplay_type_0f_data_offset;
static int hf_dplay_type_0f_data;

/* Message Type 0x0013 data fields */
static int hf_dplay_type_13_id_to;
static int hf_dplay_type_13_player_id;
static int hf_dplay_type_13_group_id;
static int hf_dplay_type_13_create_offset;
static int hf_dplay_type_13_password_offset;
static int hf_dplay_type_13_password;
static int hf_dplay_type_13_tick_count;

/* Message Type 0x0015 data fields */
static int hf_dplay_message_guid;
static int hf_dplay_type_15_packet_idx;
static int hf_dplay_type_15_data_size;
static int hf_dplay_type_15_offset;
static int hf_dplay_type_15_total_packets;
static int hf_dplay_type_15_msg_size;
static int hf_dplay_type_15_packet_offset;

/* Message Type 0x0016 and 0x0017 data fields */
static int hf_dplay_ping_id_from;
static int hf_dplay_ping_tick_count;

/* Message Type 0x001a data fields */
static int hf_dplay_type_1a_id_to;
static int hf_dplay_type_1a_sess_name_ofs;
static int hf_dplay_type_1a_password_ofs;
static int hf_dplay_type_1a_session_name;
static int hf_dplay_type_1a_password;

/* Message Type 0x0029 data fields */
static int hf_dplay_type_29_player_count;
static int hf_dplay_type_29_group_count;
static int hf_dplay_type_29_packed_offset;
static int hf_dplay_type_29_shortcut_count;
static int hf_dplay_type_29_description_offset;
static int hf_dplay_type_29_name_offset;
static int hf_dplay_type_29_password_offset;
static int hf_dplay_type_29_game_name;
static int hf_dplay_type_29_password;

/* Message Type 0x002f data fields */
static int hf_dplay_type_2f_dpid;

/* various */
static int ett_dplay;
static int ett_dplay_header;
static int ett_dplay_sockaddr;
static int ett_dplay_data;
static int ett_dplay_enc_packet;
static int ett_dplay_flags;
static int ett_dplay_sess_desc_flags;
static int ett_dplay_pp_flags;
static int ett_dplay_spp_flags;
static int ett_dplay_spp_info_mask;
static int ett_dplay_type02_flags;
static int ett_dplay_type05_flags;
static int ett_dplay_type29_spp;

static const value_string dplay_command_val[] = {
    { 0x0001, "Enum Sessions Reply" },
    { 0x0002, "Enum Sessions" },
    { 0x0003, "Enum Players Reply" },
    { 0x0004, "Enum Players" },
    { 0x0005, "Request Player ID" },
    { 0x0006, "Request Group ID" },
    { 0x0007, "Request Player Reply" },
    { 0x0008, "Create Player" },
    { 0x0009, "Create Group" },
    { 0x000a, "Player Message" },
    { 0x000b, "Delete Player" },
    { 0x000c, "Delete Group" },
    { 0x000d, "Add Player To Group" },
    { 0x000e, "Delete Player From Group" },
    { 0x000f, "Player Data Changed" },
    { 0x0010, "Player Name Changed" },
    { 0x0011, "Group Data Changed" },
    { 0x0012, "Group Name Changed" },
    { 0x0013, "Add Forward Request" },
    /* There is no command 0x0014 */
    { 0x0015, "Packet" },
    { 0x0016, "Ping" },
    { 0x0017, "Pong" },
    { 0x0018, "You Are Dead" },
    { 0x0019, "Player Wrapper" },
    { 0x001a, "Session Desc Changed" },
    { 0x001c, "Challenge" },
    { 0x001d, "Access Granted" },
    { 0x001e, "Logon Denied" },
    { 0x001f, "Auth Error" },
    { 0x0020, "Negotiate" },
    { 0x0021, "Challenge Response" },
    { 0x0022, "Signed"},
    /* There is no command 0x0023 */
    { 0x0024, "Add Forward Reply" },
    { 0x0025, "Ask For Multicast" },
    { 0x0026, "Ask For Multicast Guaranteed" },
    { 0x0027, "Add Shortcut To Group" },
    { 0x0028, "Delete Group From Group" },
    { 0x0029, "Super Enum Players Reply" },
    /* There is no command 0x002a */
    { 0x002b, "Key Exchange" },
    { 0x002c, "Key Exchange Reply" },
    { 0x002d, "Chat" },
    { 0x002e, "Add Forward" },
    { 0x002f, "Add Forward ACK" },
    { 0x0030, "Packet2 Data" },
    { 0x0031, "Packet2 ACK" },
    /* No commands 0x0032, 0x0033, 0x0034 */
    { 0x0035, "I Am Nameserver" },
    { 0x0036, "Voice" },
    { 0x0037, "Multicast Delivery" },
    { 0x0038, "Create Players Verify"},
    { 0     , NULL },
};

static const value_string dplay_af_val[] = {
    { WINSOCK_AF_INET, "AF_INET" },
    { WINSOCK_AF_IPX, "AF_IPX" },
    { 0     , NULL},
};

static const value_string dplay_proto_dialect_val[] = {
    { 0x0009, "dplay 6" },
    { 0x000a, "dplay 6.1" },
    { 0x000b, "dplay 6.1a" },
    { 0x000c, "dplay 7.1" },
    { 0x000d, "dplay 8" },
    { 0x000e, "dplay 9"},
    { 0     , NULL},
};

static const value_string dplay_token_val[] = {
    { 0xfab, "Remote Message" },
    { 0xcab, "Forwarded Message" },
    { 0xbab, "Server Message" },
    { 0    , NULL },
};

static const value_string dplay_spp_length_val[] = {
    { 0x0, "Not present" },
    { 0x1, "One byte" },
    { 0x2, "Two bytes" },
    { 0x3, "Four bytes" },
    { 0  , NULL},
};

static const value_string dplay_enc_alg_val[] = {
    { 0x0000, "Default" },
    { 0x6611, "AES" },
    { 0x6603, "3DES" },
    { 0x6601, "DES" },
    { 0x6602, "RC2" },
    { 0x6801, "RC4" },
    { 0     , NULL },
};

static int* const size_token_flags[] = {
    &hf_dplay_size,
    &hf_dplay_token,
    NULL
};

static int dissect_sockaddr_in(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    proto_tree *sa_tree;

    sa_tree = proto_tree_add_subtree(tree, tvb, offset, 16, ett_dplay_sockaddr, NULL,
            "DirectPlay sockaddr_in structure");
    proto_tree_add_item(sa_tree, hf_dplay_saddr_af, tvb, offset, 2, ENC_LITTLE_ENDIAN); offset += 2;
    proto_tree_add_item(sa_tree, hf_dplay_saddr_port, tvb, offset, 2, ENC_BIG_ENDIAN); offset += 2;
    proto_tree_add_item(sa_tree, hf_dplay_saddr_ip, tvb, offset, 4, ENC_BIG_ENDIAN); offset += 4;
    proto_tree_add_item(sa_tree, hf_dplay_saddr_padding, tvb, offset, 8, ENC_NA); offset += 8;
    return offset;
}

static int dissect_session_desc(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    static int * const flags[] = {
        &hf_dplay_flags_no_sess_desc_changes,
        &hf_dplay_flags_acqire_voice,
        &hf_dplay_flags_optimize_latency,
        &hf_dplay_flags_preserve_order,
        &hf_dplay_flags_reliable,
        &hf_dplay_flags_server_player_only,
        &hf_dplay_flags_route,
        &hf_dplay_flags_password_req,
        &hf_dplay_flags_private_session,
        &hf_dplay_flags_use_auth,
        &hf_dplay_flags_no_player_updates,
        &hf_dplay_flags_use_ping,
        &hf_dplay_flags_can_join,
        &hf_dplay_flags_ignored,
        &hf_dplay_flags_short_player_msg,
        &hf_dplay_flags_migrate_host,
        &hf_dplay_flags_0002,
        &hf_dplay_flags_no_create_players,
        NULL
    };

    proto_tree_add_item(tree, hf_dplay_sess_desc_length, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_bitmask(tree, tvb, offset, hf_dplay_sess_desc_flags, ett_dplay_sess_desc_flags, flags, ENC_LITTLE_ENDIAN); offset += 4;

    proto_tree_add_item(tree, hf_dplay_instance_guid, tvb, offset, 16, ENC_BIG_ENDIAN); offset += 16;
    proto_tree_add_item(tree, hf_dplay_game_guid, tvb, offset, 16, ENC_BIG_ENDIAN); offset += 16;
    proto_tree_add_item(tree, hf_dplay_max_players, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_curr_players, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_name_ptr, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_passwd_ptr, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_desc_reserved_1, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_desc_reserved_2, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_desc_user_1, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_desc_user_2, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_desc_user_3, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sess_desc_user_4, tvb, offset, 4, ENC_NA); offset += 4;

    return offset;
}

static int dissect_packed_player(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t sn_len, ln_len, sd_len, pd_len, num_players, i;
    int size;
    static int * const flags[] = {
        &hf_dplay_pp_flag_sending,
        &hf_dplay_pp_flag_in_group,
        &hf_dplay_pp_flag_nameserver,
        &hf_dplay_pp_flag_sysplayer,
        NULL
    };

    proto_tree_add_item_ret_uint(tree, hf_dplay_pp_size, tvb, offset, 4, ENC_LITTLE_ENDIAN, &size); offset += 4;

    proto_tree_add_bitmask(tree, tvb, offset, hf_dplay_pp_flags, ett_dplay_pp_flags, flags, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_pp_id, tvb, offset, 4, ENC_NA); offset += 4;

    proto_tree_add_item_ret_uint(tree, hf_dplay_pp_short_name_len, tvb, offset, 4, ENC_LITTLE_ENDIAN, &sn_len); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_pp_long_name_len, tvb, offset, 4, ENC_LITTLE_ENDIAN, &ln_len); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_pp_sp_data_size, tvb, offset, 4, ENC_LITTLE_ENDIAN, &sd_len); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_pp_player_data_size, tvb, offset, 4, ENC_LITTLE_ENDIAN, &pd_len); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_pp_num_players, tvb, offset, 4, ENC_LITTLE_ENDIAN, &num_players); offset += 4;

    proto_tree_add_item(tree, hf_dplay_pp_system_player, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_pp_fixed_size, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_pp_dialect, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_pp_unknown_1, tvb, offset, 4, ENC_NA); offset += 4;

    if (sn_len)
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_pp_short_name, NULL);

    if (ln_len)
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_pp_long_name, NULL);

    proto_tree_add_item(tree, hf_dplay_pp_sp_data, tvb, offset, sd_len, ENC_NA);
    offset += sd_len;

    if (pd_len) {
        proto_tree_add_item(tree, hf_dplay_pp_player_data, tvb, offset, pd_len, ENC_NA);
        offset += pd_len;
    }

    for (i=0; i < num_players; ++i) {
        proto_tree_add_item(tree, hf_dplay_pp_player_id, tvb, offset, 4, ENC_NA); offset += 4;
    }

    /* Size seems to miss the unknown empty dword */
    if (size + 4 > offset) {
        proto_tree_add_item(tree, hf_dplay_pp_parent_id, tvb, offset, 4, ENC_NA); offset += 4;
    }

    return offset;
}

static int spp_get_value(uint32_t length_type, tvbuff_t *tvb, int offset, uint32_t *value)
{
    int len = 0;

    *value = 0;

    switch (length_type) {
        case 1:
            len = 1;
            *value = tvb_get_uint8(tvb, offset);
            break;
        case 2:
            len = 2;
            *value = tvb_get_letohs(tvb, offset);
            break;
        case 3:
            len = 4;
            *value = tvb_get_letohl(tvb, offset);
            break;
    }

    return len;
}

static int dissect_dplay_super_packed_player(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t flags, is_sysplayer, info_mask;
    uint32_t sp_length_type, pd_length_type;
    uint32_t player_count_type, shortcut_count_type;
    uint32_t player_data_length, sp_data_length, player_count, shortcut_count;
    int len;
    static int * const ssp_flags[] = {
        &hf_dplay_spp_flags_sending,
        &hf_dplay_spp_flags_in_group,
        &hf_dplay_spp_flags_nameserver,
        &hf_dplay_spp_flags_sysplayer,
        NULL
    };
    static int* const info_mask_flags[] = {
        &hf_dplay_spp_have_short_name,
        &hf_dplay_spp_have_long_name,
        &hf_dplay_spp_sp_length_type,
        &hf_dplay_spp_pd_length_type,
        &hf_dplay_spp_player_count_type,
        &hf_dplay_spp_have_parent_id,
        &hf_dplay_spp_shortcut_count_type,
        NULL
    };

    proto_tree_add_item(tree, hf_dplay_spp_size, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;

    flags = tvb_get_letohl(tvb, offset);
    is_sysplayer = flags & 0x00000001;
    proto_tree_add_bitmask(tree, tvb, offset, hf_dplay_spp_flags, ett_dplay_spp_flags, ssp_flags, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_spp_id, tvb, offset, 4, ENC_NA); offset += 4;

    proto_tree_add_bitmask(tree, tvb, offset, hf_dplay_spp_player_info_mask, ett_dplay_spp_info_mask, info_mask_flags, ENC_LITTLE_ENDIAN);
    info_mask = tvb_get_letohl(tvb, offset);
    offset+=4;

    sp_length_type = (info_mask & DPLAY_SPP_INF_FLAG_SP_LENGTH_TYPE) >> 2;
    pd_length_type = (info_mask & DPLAY_SPP_INF_FLAG_PD_LENGTH_TYPE) >> 4;
    player_count_type = (info_mask & DPLAY_SPP_INF_FLAG_PLAYER_COUNT_TYPE) >> 6;
    shortcut_count_type = (info_mask & DPLAY_SPP_INF_FLAG_SHORTCUT_COUNT_TYPE) >> 9;

    if (is_sysplayer) {
        proto_tree_add_item(tree, hf_dplay_spp_dialect, tvb, offset, 4, ENC_LITTLE_ENDIAN);
    } else {
        proto_tree_add_item(tree, hf_dplay_spp_sys_player_id, tvb, offset, 4, ENC_NA);
    }
    offset += 4;

    if (info_mask & DPLAY_SPP_INF_FLAG_HAVE_SHORT_NAME) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_spp_short_name, NULL);
    }

    if (info_mask & DPLAY_SPP_INF_FLAG_HAVE_LONG_NAME) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_spp_long_name, NULL);
    }

    if (pd_length_type) {
        len = spp_get_value(pd_length_type, tvb, offset, &player_data_length);
        proto_tree_add_item(tree, hf_dplay_spp_player_data_length, tvb, offset, len, ENC_LITTLE_ENDIAN);
        offset += len;
        proto_tree_add_item(tree, hf_dplay_spp_player_data, tvb, offset, player_data_length, ENC_NA);
        offset += player_data_length;
    }

    if (sp_length_type) {
        len = spp_get_value(sp_length_type, tvb, offset, &sp_data_length);
        proto_tree_add_item(tree, hf_dplay_spp_sp_data_length, tvb, offset, len, ENC_LITTLE_ENDIAN);
        offset += len;
        proto_tree_add_item(tree, hf_dplay_spp_sp_data, tvb, offset, sp_data_length, ENC_NA);
        offset += sp_data_length;
    }

    if (player_count_type) {
        uint32_t i;

        len = spp_get_value(player_count_type, tvb, offset, &player_count);
        proto_tree_add_item(tree, hf_dplay_spp_player_count, tvb, offset, len, ENC_LITTLE_ENDIAN);
        offset += len;
        for (i=0; i < player_count; ++i) {
            proto_tree_add_item(tree, hf_dplay_spp_player_id, tvb, offset, 4, ENC_NA); offset += 4;
        }
    }

    if (info_mask & DPLAY_SPP_INF_FLAG_HAVE_PARENT_ID) {
        proto_tree_add_item(tree, hf_dplay_spp_parent_id, tvb, offset, 4, ENC_NA); offset += 4;
    }

    if (shortcut_count_type) {
        uint32_t i;

        len = spp_get_value(shortcut_count_type, tvb, offset, &shortcut_count);
        proto_tree_add_item(tree, hf_dplay_spp_shortcut_count, tvb, offset, len, ENC_LITTLE_ENDIAN);
        offset += len;
        for (i=0; i < shortcut_count; ++i) {
            proto_tree_add_item(tree, hf_dplay_spp_shortcut_id, tvb, offset, 4, ENC_NA); offset += 4;
        }
    }

    return offset;
}

static int dissect_security_desc(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    proto_tree_add_item(tree, hf_dplay_sd_size, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sd_flags, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sd_sspi, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sd_capi, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sd_capi_type, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_sd_enc_alg, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    return offset;
}

static int dissect_dplay_header(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    proto_tree_add_bitmask_list(tree, tvb, offset, 4, size_token_flags, ENC_LITTLE_ENDIAN);
    offset += 4;
    offset = dissect_sockaddr_in(tree, tvb, offset);
    proto_tree_add_item(tree, hf_dplay_play_str, tvb, offset, 4, ENC_ASCII); offset += 4;
    proto_tree_add_item(tree, hf_dplay_command, tvb, offset, 2, ENC_LITTLE_ENDIAN); offset += 2;
    proto_tree_add_item(tree, hf_dplay_proto_dialect, tvb, offset, 2, ENC_LITTLE_ENDIAN); offset += 2;
    return offset;
}

static int dissect_type01_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t name_offset;

    offset = dissect_session_desc(tree, tvb, offset);
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_01_name_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN, &name_offset); offset += 4;

    if (name_offset != 0) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_01_game_name, NULL);
    }
    return offset;
}

static int dissect_type02_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t passwd_offset;
    static int * const flags[] = {
        &hf_enum_sess_flag_passwd,
        &hf_enum_sess_flag_all,
        &hf_enum_sess_flag_join,
        NULL
    };

    passwd_offset = tvb_get_letohl(tvb, offset + 16);

    proto_tree_add_item(tree, hf_dplay_type_02_game_guid, tvb, offset, 16, ENC_BIG_ENDIAN); offset += 16;
    proto_tree_add_item(tree, hf_dplay_type_02_password_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_bitmask(tree, tvb, offset, hf_dplay_type_02_flags, ett_dplay_type02_flags, flags, ENC_LITTLE_ENDIAN); offset += 4;

    if (passwd_offset != 0) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_02_password, NULL);
    }
    return offset;
}

static int dissect_type05_message(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    static int * const flags[] = {
        &hf_dplay_type_05_secure,
        &hf_dplay_type_05_unknown,
        &hf_dplay_type_05_local,
        &hf_dplay_type_05_name_server,
        &hf_dplay_type_05_system_player,
        NULL
    };

    proto_tree_add_bitmask(tree, tvb, offset, hf_dplay_type_05_flags, ett_dplay_type05_flags, flags, ENC_LITTLE_ENDIAN);
    offset += 4;
    return offset;
}

static int dissect_type07_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t sspi_offset, capi_offset;

    proto_tree_add_item(tree, hf_dplay_type_07_dpid, tvb, offset, 4, ENC_NA); offset += 4;
    offset = dissect_security_desc(tree, tvb, offset);

    proto_tree_add_item_ret_uint(tree, hf_dplay_type_07_sspi_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN, &sspi_offset); offset += 4;

    proto_tree_add_item_ret_uint(tree, hf_dplay_type_07_capi_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN, &capi_offset); offset += 4;

    proto_tree_add_item(tree, hf_dplay_type_07_hresult, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;

    if (sspi_offset) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_07_sspi, NULL);
    }

    if (capi_offset) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_07_capi, NULL);
    }
    return offset;
}

static int dissect_player_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t pp_ofs;

    proto_tree_add_item(tree, hf_dplay_multi_id_to, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_multi_player_id, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_multi_group_id, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_multi_create_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN, &pp_ofs); offset += 4;
    proto_tree_add_item(tree, hf_dplay_multi_password_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    if (pp_ofs)
        offset = dissect_packed_player(tree, pinfo, tvb, offset);
    if (tvb_bytes_exist(tvb, offset, 2))
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_multi_password, NULL);
    return offset;
}

static int dissect_type0f_message(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    uint32_t data_size;

    proto_tree_add_item(tree, hf_dplay_type_0f_id_to, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_0f_id, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_0f_data_size, tvb, offset, 4, ENC_LITTLE_ENDIAN, &data_size); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_0f_data_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_0f_data, tvb, offset, data_size, ENC_NA);
    offset += data_size;

    return offset;
}

static int dissect_type13_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t pp_ofs, pw_ofs;

    proto_tree_add_item(tree, hf_dplay_type_13_id_to, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_13_player_id, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_13_group_id, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_13_create_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN, &pp_ofs); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_13_password_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN, &pw_ofs); offset += 4;
    if (pp_ofs)
        offset = dissect_packed_player(tree, pinfo, tvb, offset);
    if (pw_ofs)
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_13_password, NULL);
    proto_tree_add_item(tree, hf_dplay_type_13_tick_count, tvb, offset, 4, ENC_NA); offset += 4;

    return offset;
}

static int dissect_type15_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint16_t second_message_type;
    proto_tree *enc_tree;
    second_message_type = tvb_get_letohs(tvb, 72);

    proto_tree_add_item(tree, hf_dplay_message_guid, tvb, offset, 16, ENC_BIG_ENDIAN); offset += 16;
    proto_tree_add_item(tree, hf_dplay_type_15_packet_idx, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_15_data_size, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_15_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_15_total_packets, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_15_msg_size, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_15_packet_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;

    enc_tree = proto_tree_add_subtree(tree, tvb, offset, -1, ett_dplay_enc_packet, NULL, "DirectPlay encapsulated packet");

    proto_tree_add_item(enc_tree, hf_dplay_play_str_2, tvb, offset, 4, ENC_ASCII); offset += 4;
    proto_tree_add_item(enc_tree, hf_dplay_command_2, tvb, offset, 2, ENC_LITTLE_ENDIAN); offset += 2;
    proto_tree_add_item(enc_tree, hf_dplay_proto_dialect_2, tvb, offset, 2, ENC_LITTLE_ENDIAN); offset += 2;

    switch(second_message_type)
    {
        case 0x0005:
            offset = dissect_type05_message(enc_tree, tvb, offset);
            break;
        case 0x0007:
            offset = dissect_type05_message(enc_tree, tvb, offset);
            break;
        case 0x0008:
        case 0x0009:
        case 0x000b:
        case 0x000c:
        case 0x000d:
        case 0x000e:
        case 0x002e:
            offset = dissect_player_message(enc_tree, pinfo, tvb, offset);
            break;
        case 0x0013:
            offset = dissect_type13_message(enc_tree, pinfo, tvb, offset);
            break;
        case 0x001a:
            offset = dissect_type1a_message(enc_tree, pinfo, tvb, offset);
            break;
    }

    return offset;
}

static int dissect_ping_message(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    proto_tree_add_item(tree, hf_dplay_ping_id_from, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item(tree, hf_dplay_ping_tick_count, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;

    return offset;
}

static int dissect_type1a_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t sn_ofs, pw_ofs;

    proto_tree_add_item(tree, hf_dplay_type_1a_id_to, tvb, offset, 4, ENC_NA); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_1a_sess_name_ofs, tvb, offset, 4, ENC_LITTLE_ENDIAN, &sn_ofs); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_1a_password_ofs, tvb, offset, 4, ENC_LITTLE_ENDIAN, &pw_ofs); offset += 4;
    offset = dissect_session_desc(tree, tvb, offset);

    if (sn_ofs) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_1a_session_name, NULL);
    }

    if (pw_ofs) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_1a_password, NULL);
    }

    return offset;
}

static int dissect_type29_message(proto_tree *tree, packet_info* pinfo, tvbuff_t *tvb, int offset)
{
    uint32_t password_offset = tvb_get_letohl(tvb, offset + 24);
    int player_count, group_count, shortcut_count;
    int i;

    proto_tree_add_item_ret_uint(tree, hf_dplay_type_29_player_count, tvb, offset, 4, ENC_LITTLE_ENDIAN, &player_count); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_29_group_count, tvb, offset, 4, ENC_LITTLE_ENDIAN, &group_count); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_29_packed_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item_ret_uint(tree, hf_dplay_type_29_shortcut_count, tvb, offset, 4, ENC_LITTLE_ENDIAN, &shortcut_count); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_29_description_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_29_name_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    proto_tree_add_item(tree, hf_dplay_type_29_password_offset, tvb, offset, 4, ENC_LITTLE_ENDIAN); offset += 4;
    offset = dissect_session_desc(tree, tvb, offset);
    offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_29_game_name, NULL);

    if (password_offset != 0) {
        offset = display_unicode_string(tvb, pinfo, tree, offset, hf_dplay_type_29_password, NULL);
    }

    for (i=0; i < player_count; ++i) {
        proto_tree *spp_tree;

        spp_tree = proto_tree_add_subtree_format(tree, tvb, offset, 0, ett_dplay_type29_spp, NULL, "Player %d", i);
        offset = dissect_dplay_super_packed_player(spp_tree, pinfo, tvb, offset);
    }

    for (i=0; i < group_count; ++i) {
        proto_tree *spp_tree;

        spp_tree = proto_tree_add_subtree_format(tree, tvb, offset, 0, ett_dplay_type29_spp, NULL, "Group %d", i);
        offset = dissect_dplay_super_packed_player(spp_tree, pinfo, tvb, offset);
    }

    for (i=0; i < shortcut_count; ++i) {
        proto_tree *spp_tree;

        spp_tree = proto_tree_add_subtree_format(tree, tvb, offset, 0, ett_dplay_type29_spp, NULL, "Shortcut %d", i);
        offset = dissect_dplay_super_packed_player(spp_tree, pinfo, tvb, offset);
    }

    return offset;
}

static int dissect_type2f_message(proto_tree *tree, tvbuff_t *tvb, int offset)
{
    proto_tree_add_item(tree, hf_dplay_type_2f_dpid, tvb, offset, 4, ENC_NA); offset += 4;
    return offset;
}

static void dissect_dplay(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    uint16_t message_type;
    uint16_t second_message_type = UINT16_MAX;
    uint16_t proto_version;
    uint32_t dplay_id;
    uint8_t play_id[] = {'p','l','a','y'};

    dplay_id = tvb_get_letohl(tvb, 20);
    message_type = tvb_get_letohs(tvb, 24);
    proto_version = tvb_get_letohs(tvb, 26);

    if(memcmp(play_id, (uint8_t *)&dplay_id, 4) != 0)
    {
        col_set_str(pinfo->cinfo, COL_PROTOCOL, "DPLAY");
        col_set_str(pinfo->cinfo,COL_INFO, "DPlay data packet");
        return;
    }

    if(message_type == 0x0015)
    {
        second_message_type = tvb_get_letohs(tvb, 72);
    }

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "DPLAY");

    if(message_type == 0x0015)
        col_add_fstr(pinfo->cinfo,COL_INFO, "%s: %s, holding a %s",
            val_to_str(proto_version, dplay_proto_dialect_val, "Unknown (0x%04x)"),
            val_to_str(message_type, dplay_command_val, "Unknown (0x%04x)"),
            val_to_str(second_message_type, dplay_command_val, "Unknown (0x%04x)"));
    else
        col_add_fstr(pinfo->cinfo,COL_INFO, "%s: %s",
            val_to_str(proto_version, dplay_proto_dialect_val, "Unknown (0x%04x)"),
            val_to_str(message_type, dplay_command_val, "Unknown (0x%04x)"));

    if(tree)
    {
        proto_item *dplay_item;
        proto_tree *dplay_tree;
        proto_tree *dplay_header;
        proto_tree *dplay_data;
        int offset = 0;

        dplay_item = proto_tree_add_item(tree, proto_dplay, tvb, 0, -1, ENC_NA);
        dplay_tree = proto_item_add_subtree(dplay_item, ett_dplay);
        dplay_header = proto_tree_add_subtree(dplay_tree, tvb, offset, DPLAY_HEADER_OFFSET, ett_dplay_header, NULL, "DirectPlay header");

        offset = dissect_dplay_header(dplay_header, tvb, offset);

        /* Special handling for empty type 0x0004 packets */
        if(message_type == 0x0004)
            return;

        dplay_data = proto_tree_add_subtree(dplay_tree, tvb, offset, -1, ett_dplay_data, NULL, "DirectPlay data");

        switch(message_type)
        {
            case 0x0001:
                dissect_type01_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x0002:
                dissect_type02_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x0005:
                dissect_type05_message(dplay_data, tvb, offset);
                break;
            case 0x0007:
                dissect_type07_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x0008:
            case 0x0009:
            /* type 0a doesn't have a dplay header and is not handled here */
            case 0x000b:
            case 0x000c:
            case 0x000d:
            case 0x000e:
            case 0x002e:
            case 0x0038:
                dissect_player_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x000f:
                dissect_type0f_message(dplay_data, tvb, offset);
                break;
            case 0x0013:
                dissect_type13_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x0015:
                dissect_type15_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x0016:
            case 0x0017:
                dissect_ping_message(dplay_data, tvb, offset);
                break;
            case 0x001a:
                dissect_type1a_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x0029:
                dissect_type29_message(dplay_data, pinfo, tvb, offset);
                break;
            case 0x002f:
                dissect_type2f_message(dplay_data, tvb, offset);
                break;
        }
    }

}

static void dissect_dplay_player_msg(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "DPLAY");
    col_set_str(pinfo->cinfo,COL_INFO, "DPlay player to player message");

    if(tree)
    {
        proto_item *dplay_item;
        proto_tree *dplay_tree;
        proto_tree *data_tree;
        int offset = 0;

        dplay_item = proto_tree_add_item(tree, proto_dplay, tvb, offset, -1, ENC_NA);
        dplay_tree = proto_item_add_subtree(dplay_item, ett_dplay);
        data_tree  = proto_tree_add_subtree(dplay_tree, tvb, offset, -1, ett_dplay_data, NULL, "Message content");

        proto_tree_add_bitmask_list(data_tree, tvb, offset, 4, size_token_flags, ENC_LITTLE_ENDIAN);
        offset += 4;
        offset = dissect_sockaddr_in(data_tree, tvb, offset);
        /* Now there's two dplay IDs iff the session desc does not have the
         * "short player message" flag set */
        proto_tree_add_item(data_tree, hf_dplay_player_msg, tvb, offset, -1, ENC_NA);

    }
}
static bool heur_dissect_dplay(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
    uint32_t dplay_id, token;

    if(tvb_captured_length(tvb) < 25)
        return false;

    /* The string play = 0x706c6179 */
    dplay_id = tvb_get_ntohl(tvb, 20);
    if( dplay_id == 0x706c6179) {
        dissect_dplay(tvb, pinfo, tree);
        return true;
    }


    /* There is a player to player message that does not contain "play" */
    token = tvb_get_letohl(tvb, 0);
    token = (token & 0xfff00000) >> 20;
    if (token == 0xfab || token == 0xbab || token == 0xcab) {
      /* Check the s_addr_in structure */
      if (tvb_get_letohs(tvb, 4) == WINSOCK_AF_INET) {
        int offset;
        for (offset = 12; offset <= 20; offset++)
          if (tvb_get_uint8(tvb, offset) != 0)
            return false;

        dissect_dplay_player_msg(tvb, pinfo, tree);
        return true;
      }
    }

    return false;
}

void proto_register_dplay(void)
{
    static hf_register_info hf [] = {
    /* Common data fields */
    { &hf_dplay_size,
        { "DirectPlay package size", "dplay.size", FT_UINT32, BASE_DEC,
        NULL, 0x000FFFFF, NULL, HFILL}},
    { &hf_dplay_token,
        { "DirectPlay token", "dplay.token", FT_UINT32, BASE_HEX,
        VALS(dplay_token_val), 0xFFF00000, NULL, HFILL}},
    { &hf_dplay_saddr_af,
        { "DirectPlay s_addr_in address family", "dplay.saddr.af", FT_UINT16, BASE_HEX,
        VALS(dplay_af_val), 0x0, NULL, HFILL}},
    { &hf_dplay_saddr_port,
        { "DirectPlay s_addr_in port", "dplay.saddr.port", FT_UINT16, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_saddr_ip,
        { "DirectPlay s_addr_in ip address", "dplay.saddr.ip", FT_IPv4, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_saddr_padding,
        { "DirectPlay s_addr_in null padding", "dplay.saddr.padding", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_play_str,
        { "DirectPlay action string", "dplay.dplay_str", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_command,
        { "DirectPlay command", "dplay.command", FT_UINT16, BASE_HEX,
        VALS(dplay_command_val), 0x0, NULL, HFILL}},
    { &hf_dplay_proto_dialect,
        { "DirectPlay dialect version", "dplay.dialect.version", FT_UINT16, BASE_HEX,
        VALS(dplay_proto_dialect_val), 0x0, NULL, HFILL}},
    { &hf_dplay_play_str_2,
        { "DirectPlay second action string", "dplay.dplay_str_2", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_command_2,
        { "DirectPlay second command", "dplay.command_2", FT_UINT16, BASE_HEX,
        VALS(dplay_command_val), 0x0, NULL, HFILL}},
    { &hf_dplay_proto_dialect_2,
        { "DirectPlay second dialect version", "dplay.dialect.version_2", FT_UINT16, BASE_HEX,
        VALS(dplay_proto_dialect_val), 0x0, NULL, HFILL}},
    { &hf_dplay_player_msg,
        { "DirectPlay Player to Player message", "dplay.player_msg", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Session Desc structure fields */
    { &hf_dplay_sess_desc_flags,
        { "DirectPlay session desc flags", "dplay.flags", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_flags_no_create_players,
        { "no create players flag", "dplay.flags.no_create_players", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_NO_CREATE_PLAYERS, NULL, HFILL}},
    { &hf_dplay_flags_0002,
        { "unused", "dplay.flags.unused", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_0002, NULL, HFILL}},
    { &hf_dplay_flags_migrate_host,
        { "migrate host flag", "dplay.flags.migrate_host", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_MIGRATE_HOST, NULL, HFILL}},
    { &hf_dplay_flags_short_player_msg,
        { "short player message", "dplay.flags.short_player_msg", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_SHORT_PLAYER_MSG, NULL, HFILL}},
    { &hf_dplay_flags_ignored,
        { "ignored", "dplay.ignored", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_IGNORED, NULL, HFILL}},
    { &hf_dplay_flags_can_join,
        { "can join", "dplay.flags.can_join", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_CAN_JOIN, NULL, HFILL}},
    { &hf_dplay_flags_use_ping,
        { "use ping", "dplay.flags.use_ping", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_USE_PING, NULL, HFILL}},
    { &hf_dplay_flags_no_player_updates,
        { "no player updates", "dplay.flags.no_player_updates", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_NO_P_UPD, NULL, HFILL}},
    { &hf_dplay_flags_use_auth,
        { "use authentication", "dplay.flags.use_auth", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_USE_AUTH, NULL, HFILL}},
    { &hf_dplay_flags_private_session,
        { "private session", "dplay.flags.priv_sess", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_PRIV_SESS, NULL, HFILL}},
    { &hf_dplay_flags_password_req,
        { "password required", "dplay.flags.pass_req", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_PASS_REQ, NULL, HFILL}},
    { &hf_dplay_flags_route,
        { "route via game host", "dplay.flags.route", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_ROUTE, NULL, HFILL}},
    { &hf_dplay_flags_server_player_only,
        { "get server player only", "dplay.flags.srv_p_only", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_SRV_ONLY, NULL, HFILL}},
    { &hf_dplay_flags_reliable,
        { "use reliable protocol", "dplay.flags.reliable", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_RELIABLE, NULL, HFILL}},
    { &hf_dplay_flags_preserve_order,
        { "preserve order", "dplay.flags.order", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_ORDER, NULL, HFILL}},
    { &hf_dplay_flags_optimize_latency,
        { "optimize for latency", "dplay.flags.opt_latency", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_OPT_LAT, "Opt Latency", HFILL}},
    { &hf_dplay_flags_acqire_voice,
        { "acquire voice", "dplay.flags.acq_voice", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_ACQ_VOICE, "Acq Voice", HFILL}},
    { &hf_dplay_flags_no_sess_desc_changes,
        { "no session desc changes", "dplay.flags.no_sess_desc", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_FLAG_NO_SESS_DESC_CHANGES, "No Sess Desc Changes", HFILL}},
    { &hf_dplay_instance_guid,
        { "DirectPlay instance guid", "dplay.instance.guid", FT_GUID, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_game_guid,
        { "DirectPlay game GUID", "dplay.game.guid", FT_GUID, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_length,
        { "DirectPlay session desc length", "dplay.sess_desc.length", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_max_players,
        { "DirectPlay max players", "dplay.sess_desc.max_players", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_curr_players,
        { "DirectPlay current players", "dplay.sess_desc.curr_players", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_name_ptr,
        { "Session description name pointer placeholder", "dplay.sess_desc.name_ptr", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_passwd_ptr,
        { "Session description password pointer placeholder", "dplay.sess_desc.pw_ptr", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_reserved_1,
        { "Session description reserved 1", "dplay.sess_desc.res_1", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_reserved_2,
        { "Session description reserved 2", "dplay.sess_desc.res_2", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_user_1,
        { "Session description user defined 1", "dplay.sess_desc.user_1", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_user_2,
        { "Session description user defined 2", "dplay.sess_desc.user_2", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_user_3,
        { "Session description user defined 3", "dplay.sess_desc.user_3", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sess_desc_user_4,
        { "Session description user defined 4", "dplay.sess_desc.user_4", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* PackedPlayer structure fields */
    { &hf_dplay_pp_size,
        { "PackedPlayer size", "dplay.pp.size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_flags,
        { "PackedPlayer flags", "dplay.pp.flags", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_flag_sysplayer,
        { "is system player", "dplay.pp.flags.sysplayer", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_PP_FLAG_SYSPLAYER, NULL, HFILL}},
    { &hf_dplay_pp_flag_nameserver,
        { "is name server", "dplay.pp.flags.nameserver", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_PP_FLAG_NAMESERVER, NULL, HFILL}},
    { &hf_dplay_pp_flag_in_group,
        { "in group", "dplay.pp.flags.in_group", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_PP_FLAG_IN_GROUP, NULL, HFILL}},
    { &hf_dplay_pp_flag_sending,
        { "sending player on local machine", "dplay.pp.flags.sending", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_SPP_FLAG_SENDING, NULL, HFILL}},
    { &hf_dplay_pp_id,
        { "PackedPlayer ID", "dplay.pp.id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_short_name_len,
        { "PackedPlayer short name length", "dplay.pp.short_name_len", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_long_name_len,
        { "PackedPlayer long name length", "dplay.pp.long_name_len", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_sp_data_size,
        { "PackedPlayer service provider data size", "dplay.pp.sp_data_size", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_player_data_size,
        { "PackedPlayer player data size", "dplay.pp.player_data_size", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_num_players,
        { "PackedPlayer player count", "dplay.pp.player_count", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_system_player,
        { "PackedPlayer system player ID", "dplay.pp.sysplayer_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_fixed_size,
        { "PackedPlayer fixed size", "dplay.pp.fixed_size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_dialect,
        { "PackedPlayer dialect version", "dplay.pp.dialect", FT_UINT32, BASE_HEX,
        VALS(dplay_proto_dialect_val), 0x0, NULL, HFILL}},
    { &hf_dplay_pp_unknown_1,
        { "PackedPlayer unknown 1", "dplay.pp.unknown_1", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_short_name,
        { "PackedPlayer short name", "dplay.pp.short_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_long_name,
        { "PackedPlayer long name", "dplay.pp.long_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_player_data,
        { "PackedPlayer player data", "dplay.pp.player_data", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_sp_data,
        { "PackedPlayer service provider data", "dplay.pp.sp_data", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_player_id,
        { "PackedPlayer player ID", "dplay.pp.player_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_pp_parent_id,
        { "PackedPlayer parent ID", "dplay.pp.parent_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* SuperPackedPlayer structure fields */
    { &hf_dplay_spp_size,
        { "SuperPackedPlayer size", "dplay.spp.size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_flags,
        { "SuperPackedPlayer flags", "dplay.spp.flags", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_flags_sysplayer,
        { "is system player", "dplay.spp.flags.sysplayer", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_SPP_FLAG_SYSPLAYER, NULL, HFILL}},
    { &hf_dplay_spp_flags_nameserver,
        { "is name server", "dplay.spp.flags.nameserver", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_SPP_FLAG_NAMESERVER, NULL, HFILL}},
    { &hf_dplay_spp_flags_in_group,
        { "in group", "dplay.spp.flags.in_group", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_SPP_FLAG_IN_GROUP, NULL, HFILL}},
    { &hf_dplay_spp_flags_sending,
        { "sending player on local machine", "dplay.spp.flags.sending", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_SPP_FLAG_SENDING, NULL, HFILL}},
    { &hf_dplay_spp_id,
        { "SuperPackedPlayer ID", "dplay.spp.id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_player_info_mask,
        { "SuperPackedPlayer player info mask", "dplay.spp.pim", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_have_short_name,
        { "SuperPackedPlayer have short name", "dplay.spp.pim.short_name", FT_BOOLEAN, 32,
        TFS(&tfs_yes_no), DPLAY_SPP_INF_FLAG_HAVE_SHORT_NAME, NULL, HFILL}},
    { &hf_dplay_spp_have_long_name,
        { "SuperPackedPlayer have long name", "dplay.spp.pim.long_name", FT_BOOLEAN, 32,
        TFS(&tfs_yes_no), DPLAY_SPP_INF_FLAG_HAVE_LONG_NAME, NULL, HFILL}},
    { &hf_dplay_spp_sp_length_type,
        { "SuperPackedPlayer service provider length info", "dplay.spp.pim.sp_length", FT_UINT32, BASE_DEC,
        VALS(dplay_spp_length_val), DPLAY_SPP_INF_FLAG_SP_LENGTH_TYPE, NULL, HFILL}},
    { &hf_dplay_spp_pd_length_type,
        { "SuperPackedPlayer player data length info", "dplay.spp.pim.pd_length", FT_UINT32, BASE_DEC,
        VALS(dplay_spp_length_val), DPLAY_SPP_INF_FLAG_PD_LENGTH_TYPE, NULL, HFILL}},
    { &hf_dplay_spp_player_count_type,
        { "SuperPackedPlayer player count info", "dplay.spp.pim.player_count", FT_UINT32, BASE_DEC,
        VALS(dplay_spp_length_val), DPLAY_SPP_INF_FLAG_PLAYER_COUNT_TYPE, NULL, HFILL}},
    { &hf_dplay_spp_have_parent_id,
        { "SuperPackedPlayer have parent ID", "dplay.spp.pim.parent_id", FT_BOOLEAN, 32,
        TFS(&tfs_yes_no), DPLAY_SPP_INF_FLAG_HAVE_PARENT_ID, NULL, HFILL}},
    { &hf_dplay_spp_shortcut_count_type,
        { "SuperPackedPlayer shortcut count info", "dplay.spp.pim.shortcut_count", FT_UINT32, BASE_DEC,
        VALS(dplay_spp_length_val), DPLAY_SPP_INF_FLAG_SHORTCUT_COUNT_TYPE, NULL, HFILL}},
    { &hf_dplay_spp_dialect,
        { "SuperPackedPlayer dialect version", "dplay.spp.dialect", FT_UINT32, BASE_HEX,
        VALS(dplay_proto_dialect_val), 0x0, NULL, HFILL}},
    { &hf_dplay_spp_sys_player_id,
        { "SuperPackedPlayer system player ID", "dplay.spp.sysplayer_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_short_name,
        { "SuperPackedPlayer short name", "dplay.spp.short_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_long_name,
        { "SuperPackedPlayer long name", "dplay.spp.long_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_player_data_length,
        { "SuperPackedPlayer player data length", "dplay.spp.pd_length", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_player_data,
        { "SuperPackedPlayer player data", "dplay.spp.player_data", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_sp_data_length,
        { "SuperPackedPlayer service provider data length", "dplay.spp.sp_data_length", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_sp_data,
        { "SuperPackedPlayer service provider data", "dplay.spp.sp_data", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_player_count,
        { "SuperPackedPlayer player count", "dplay.spp.player_count", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_player_id,
        { "SuperPackedPlayer player ID", "dplay.spp.player_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_parent_id,
        { "SuperPackedPlayer parent ID", "dplay.spp.parent_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_shortcut_count,
        { "SuperPackedPlayer shortcut count", "dplay.spp.shortcut_count", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_spp_shortcut_id,
        { "SuperPackedPlayer shortcut ID", "dplay.spp.shortcut_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for SecDesc struct */
    { &hf_dplay_sd_size,
        { "SecDesc struct size", "dplay.sd.size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sd_flags,
        { "SecDesc flags", "dplay.sd.flags", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sd_sspi,
        { "SecDesc SSPI provider ptr", "dplay.sd.sspi", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sd_capi,
        { "SecDesc CAPI provider ptr", "dplay.sd.capi", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sd_capi_type,
        { "SecDesc CAPI provider type", "dplay.sd.capi_type", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_sd_enc_alg,
        { "SecDesc encryption algorithm", "dplay.sd.enc_alg", FT_UINT32, BASE_HEX,
        VALS(dplay_enc_alg_val), 0x0, NULL, HFILL}},

    /* Data fields for message type 0x0001 */
    { &hf_dplay_type_01_name_offset,
        { "Enum Session Reply name offset", "dplay.type_01.name_offs", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_01_game_name,
        { "Enum Session Reply game name", "dplay.type_01.game_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x0002 */
    { &hf_dplay_type_02_game_guid,
        { "DirectPlay game GUID", "dplay.type02.game.guid", FT_GUID, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_02_password_offset,
        { "Enum Sessions password offset", "dplay.type02.password_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_02_flags,
        { "Enum Session flags", "dplay.type02.flags", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_02_password,
        { "Session password", "dplay.type02.password", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_enum_sess_flag_join,
        { "Enumerate joinable sessions", "dplay.type02.joinable", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_ENUM_SESS_FLAG_JOIN, NULL, HFILL}},
    { &hf_enum_sess_flag_all,
        { "Enumerate all sessions", "dplay.type02.all", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_ENUM_SESS_FLAG_ALL, NULL, HFILL}},
    { &hf_enum_sess_flag_passwd,
        { "Enumerate sessions requiring a password", "dplay.type02.pw_req", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_ENUM_SESS_FLAG_PASSWD, NULL, HFILL}},

    /* Data fields for message type 0x0005 */
    { &hf_dplay_type_05_flags,
        { "Player ID request flags", "dplay.type_05.flags", FT_UINT32, BASE_HEX,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_05_system_player,
        { "is system player", "dplay.type_05.flags.sys_player", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_TYPE05_FLAG_SYSPLAYER, NULL, HFILL}},
    { &hf_dplay_type_05_name_server,
        { "is name server", "dplay.type_05.flags.name_server", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_TYPE05_FLAG_NAMESERVER, NULL, HFILL}},
    { &hf_dplay_type_05_local,
        { "is local player", "dplay.type_05.flags.local", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_TYPE05_FLAG_LOCAL, NULL, HFILL}},
    { &hf_dplay_type_05_unknown,
        { "unknown", "dplay.type_05.flags.unknown", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_TYPE05_FLAG_UNKNOWN, NULL, HFILL}},
    { &hf_dplay_type_05_secure,
        { "is secure session", "dplay.type_05.flags.secure", FT_BOOLEAN, 32,
        TFS(&tfs_present_absent), DPLAY_TYPE05_FLAG_SECURE, NULL, HFILL}},

    /* Data fields for message type 0x0007 */
    { &hf_dplay_type_07_dpid,
        { "DirectPlay ID", "dplay.type_07.dpid", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_07_sspi_offset,
        { "SSPI provider offset", "dplay.type_07.sspi_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_07_capi_offset,
        { "CAPI provider offset", "dplay.type_07.capi_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_07_hresult,
        { "Request player HRESULT", "dplay.type_07.hresult", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_07_sspi,
        { "SSPI provider", "dplay.type_07.sspi", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_07_capi,
        { "CAPI provider", "dplay.type_07.capi", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x0008, 0x0009, 0x000b, 0x000c, 0x000d,
     * 0x000e, 0x002e and 0x0038*/
    { &hf_dplay_multi_id_to,
        { "ID to", "dplay.multi.id_to", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_multi_player_id,
        { "Player ID", "dplay.multi.player_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_multi_group_id,
        { "Group ID", "dplay.multi.group_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_multi_create_offset,
        { "Offset to PackedPlayer struct", "dplay.multi.create_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_multi_password_offset,
        { "Offset to password", "dplay.multi.password_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_multi_password,
        { "Password", "dplay.multi.password", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x000f */
    { &hf_dplay_type_0f_id_to,
        { "ID to", "dplay.type_0f.id_to", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_0f_id,
        { "Player ID", "dplay.type_0f.player_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_0f_data_size,
        { "Data Size", "dplay.type_0f.data_size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_0f_data_offset,
        { "Data Offset", "dplay.type_0f.data_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_0f_data,
        { "Player Data", "dplay.type_0f.player_data", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x0013 */
    { &hf_dplay_type_13_id_to,
        { "ID to", "dplay.type_13.id_to", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_13_player_id,
        { "Player ID", "dplay.type_13.player_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_13_group_id,
        { "Group ID", "dplay.type_13.group_id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_13_create_offset,
        { "Create Offset", "dplay.type_13.create_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_13_password_offset,
        { "Password Offset", "dplay.type_13.password_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_13_password,
        { "Password", "dplay.type_13.password", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_13_tick_count,
        { "Tick count? Looks like an ID", "dplay.type_13.tick_count", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x0015 */
    { &hf_dplay_message_guid,
        { "Message GUID", "dplay.message.guid", FT_GUID, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_15_packet_idx,
        { "Packet Index", "dplay.type_15.packet_idx", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_15_data_size,
        { "Data Size", "dplay.type_15.data_size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_15_offset,
        { "Offset", "dplay.type_15.offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_15_total_packets,
        { "Total Packets", "dplay.type_15.total_packets", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_15_msg_size,
        { "Message size", "dplay.type_15.message.size", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_15_packet_offset,
        { "Packet offset", "dplay.type_15.packet_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},

    /* Data field for message type 0x0016 and 0x0017 */
    { &hf_dplay_ping_id_from,
        { "ID From", "dplay.ping.id_from", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_ping_tick_count,
        { "Tick Count", "dplay.ping.tick_count", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x001a */
    { &hf_dplay_type_1a_id_to,
        { "ID From", "dplay.type_1a.id_to", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_1a_sess_name_ofs,
        { "Session Name Offset", "dplay.type_1a.sess_name_ofs", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_1a_password_ofs,
        { "Password Offset", "dplay.type_1a.password_offset", FT_UINT32, BASE_DEC,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_1a_session_name,
        { "Session Name", "dplay.type_1a.session_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_1a_password,
        { "Password", "dplay.type_1a.password", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x0029 */
    { &hf_dplay_type_29_player_count,
        { "SuperEnumPlayers Reply player count", "dplay.type_29.player_count", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_group_count,
        { "SuperEnumPlayers Reply group count", "dplay.type_29.group_count", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_packed_offset,
        { "SuperEnumPlayers Reply packed offset", "dplay.type_29.packed_offset", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_shortcut_count,
        { "SuperEnumPlayers Reply shortcut count", "dplay.type_29.shortcut_count", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_description_offset,
        { "SuperEnumPlayers Reply description offset", "dplay.type_29.desc_offset", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_name_offset,
        { "SuperEnumPlayers Reply name offset", "dplay.type_29.name_offset", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_password_offset,
        { "SuperEnumPlayers Reply password offset", "dplay.type_29.pass_offset", FT_UINT32,
        BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_game_name,
        { "SuperEnumPlayers Reply game name", "dplay.type_29.game_name", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    { &hf_dplay_type_29_password,
        { "SuperEnumPlayers Reply Password", "dplay.type_29.password", FT_STRING, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},

    /* Data fields for message type 0x002f */
    { &hf_dplay_type_2f_dpid,
        { "ID of the forwarded player", "dplay.type_29.id", FT_BYTES, BASE_NONE,
        NULL, 0x0, NULL, HFILL}},
    };

    static int *ett[] = {
        &ett_dplay,
        &ett_dplay_header,
        &ett_dplay_sockaddr,
        &ett_dplay_data,
        &ett_dplay_flags,
        &ett_dplay_enc_packet,
        &ett_dplay_sess_desc_flags,
        &ett_dplay_pp_flags,
        &ett_dplay_spp_flags,
        &ett_dplay_spp_info_mask,
        &ett_dplay_type02_flags,
        &ett_dplay_type05_flags,
        &ett_dplay_type29_spp,
    };

    proto_dplay = proto_register_protocol (
        "DirectPlay Protocol",
        "DPLAY",
        "dplay"
        );
    proto_register_field_array(proto_dplay, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
}

void proto_reg_handoff_dplay(void)
{
    heur_dissector_add("udp", heur_dissect_dplay, "DirectPlay over UDP", "dplay_udp", proto_dplay, HEURISTIC_ENABLE);
    heur_dissector_add("tcp", heur_dissect_dplay, "DirectPlay over TCP", "dplay_tcp", proto_dplay, HEURISTIC_ENABLE);
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
