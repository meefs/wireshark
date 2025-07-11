 /*
 * packet-iax2.c
 *
 * Routines for IAX2 packet disassembly
 * By Alastair Maw <asterisk@almaw.com>
 * Copyright 2003 Alastair Maw
 *
 * IAX2 is a VoIP protocol for the open source PBX Asterisk. Please see
 * http://www.asterisk.org for more information; see RFC 5456 for the
 * protocol.
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"


#include <epan/packet.h>
#include <epan/conversation.h>
#include <epan/reassemble.h>
#include <epan/expert.h>
#include <epan/aftypes.h>
#include <epan/tap.h>
#include <epan/proto_data.h>
#include <epan/tfs.h>
#include <wsutil/array.h>
#include <wsutil/str_util.h>

#include "packet-iax2.h"
#include <epan/iax2_codec_type.h>

void proto_register_iax2(void);
void proto_reg_handoff_iax2(void);

#define IAX2_PORT               4569
#define PROTO_TAG_IAX2          "IAX2"

/* enough to hold any address in an address_t */
#define MAX_ADDRESS 16

/* the maximum number of transfers (of each end) we can deal with per call,
 * plus one */
#define IAX_MAX_TRANSFERS 2

/* #define DEBUG_HASHING */
/* #define DEBUG_DESEGMENT */

/* Wireshark ID of the IAX2 protocol */
static int proto_iax2;

/* tap register id */
static int iax2_tap;

/* protocol tap info */
static iax2_info_t ii_arr[1];
static iax2_info_t *iax2_info = ii_arr;

/* The following hf_* variables are used to hold the wireshark IDs of
 * our header fields; they are filled out when we call
 * proto_register_field_array() in proto_register_iax2()
 */
static int hf_iax2_packet_type;
static int hf_iax2_retransmission;
static int hf_iax2_callno;
static int hf_iax2_scallno;
static int hf_iax2_dcallno;
static int hf_iax2_ts;
static int hf_iax2_minits;
static int hf_iax2_minividts;
static int hf_iax2_absts;
static int hf_iax2_lateness;
static int hf_iax2_minividmarker;
static int hf_iax2_oseqno;
static int hf_iax2_iseqno;
static int hf_iax2_type;
static int hf_iax2_csub;
static int hf_iax2_dtmf_csub;
static int hf_iax2_cmd_csub;
static int hf_iax2_iax_csub;
static int hf_iax2_voice_csub;
static int hf_iax2_voice_codec;
static int hf_iax2_video_csub;
static int hf_iax2_video_codec;
static int hf_iax2_marker;
static int hf_iax2_modem_csub;
static int hf_iax2_text_csub;
static int hf_iax2_text_text;
static int hf_iax2_html_csub;
static int hf_iax2_html_url;
static int hf_iax2_trunk_metacmd;
static int hf_iax2_trunk_cmddata;
static int hf_iax2_trunk_cmddata_ts;
static int hf_iax2_trunk_ts;
static int hf_iax2_trunk_ncalls;
static int hf_iax2_trunk_call_len;
static int hf_iax2_trunk_call_scallno;
static int hf_iax2_trunk_call_ts;
static int hf_iax2_trunk_call_data;

static int hf_iax2_ie_id;
static int hf_iax2_length;
static int hf_iax2_version;
static int hf_iax2_cap_g723_1;
static int hf_iax2_cap_gsm;
static int hf_iax2_cap_ulaw;
static int hf_iax2_cap_alaw;
static int hf_iax2_cap_g726_aal2;
static int hf_iax2_cap_adpcm;
static int hf_iax2_cap_slinear;
static int hf_iax2_cap_lpc10;
static int hf_iax2_cap_g729a;
static int hf_iax2_cap_speex;
static int hf_iax2_cap_ilbc;
static int hf_iax2_cap_g726;
static int hf_iax2_cap_g722;
static int hf_iax2_cap_siren7;
static int hf_iax2_cap_siren14;
static int hf_iax2_cap_slinear16;
static int hf_iax2_cap_jpeg;
static int hf_iax2_cap_png;
static int hf_iax2_cap_h261;
static int hf_iax2_cap_h263;
static int hf_iax2_cap_h263_plus;
static int hf_iax2_cap_h264;
static int hf_iax2_cap_mpeg4;
static int hf_iax2_cap_vp8;
static int hf_iax2_cap_t140_red;
static int hf_iax2_cap_t140;
static int hf_iax2_cap_g719;
static int hf_iax2_cap_speex16;
static int hf_iax2_cap_opus;
static int hf_iax2_cap_testlaw;

static int * const hf_iax2_caps[] = {
  &hf_iax2_cap_g723_1,
  &hf_iax2_cap_gsm,
  &hf_iax2_cap_ulaw,
  &hf_iax2_cap_alaw,
  &hf_iax2_cap_g726_aal2,
  &hf_iax2_cap_adpcm,
  &hf_iax2_cap_slinear,
  &hf_iax2_cap_lpc10,
  &hf_iax2_cap_g729a,
  &hf_iax2_cap_speex,
  &hf_iax2_cap_ilbc,
  &hf_iax2_cap_g726,
  &hf_iax2_cap_g722,
  &hf_iax2_cap_siren7,
  &hf_iax2_cap_siren14,
  &hf_iax2_cap_slinear16,
  &hf_iax2_cap_jpeg,
  &hf_iax2_cap_png,
  &hf_iax2_cap_h261,
  &hf_iax2_cap_h263,
  &hf_iax2_cap_h263_plus,
  &hf_iax2_cap_h264,
  &hf_iax2_cap_mpeg4,
  &hf_iax2_cap_vp8,
  &hf_iax2_cap_t140_red,
  &hf_iax2_cap_t140,
  &hf_iax2_cap_g719,
  &hf_iax2_cap_speex16,
  &hf_iax2_cap_opus,
  &hf_iax2_cap_testlaw,
  NULL
};

static int hf_iax2_fragment_unfinished;
static int hf_iax2_payload_data;
static int hf_iax2_fragments;
static int hf_iax2_fragment;
static int hf_iax2_fragment_overlap;
static int hf_iax2_fragment_overlap_conflict;
static int hf_iax2_fragment_multiple_tails;
static int hf_iax2_fragment_too_long_fragment;
static int hf_iax2_fragment_error;
static int hf_iax2_fragment_count;
static int hf_iax2_reassembled_in;
static int hf_iax2_reassembled_length;


/* hf_iax2_ies is an array of header fields, one per potential Information
 * Element. It's done this way (rather than having separate variables for each
 * IE) to make the dissection of information elements clearer and more
 * orthogonal.
 *
 * To add the ability to dissect a new information element, just add an
 * appropriate entry to hf[] in proto_register_iax2(); dissect_ies() will then
 * pick it up automatically.
 */
#define NUM_HF_IAX2_IES 256
static int hf_iax2_ies[NUM_HF_IAX2_IES];
static int hf_iax2_ie_datetime;
static int hf_IAX_IE_APPARENTADDR_SINFAMILY;
static int hf_IAX_IE_APPARENTADDR_SINPORT;
static int hf_IAX_IE_APPARENTADDR_SINADDR;
static int hf_IAX_IE_UNKNOWN_BYTE;
static int hf_IAX_IE_UNKNOWN_I16;
static int hf_IAX_IE_UNKNOWN_I32;
static int hf_IAX_IE_UNKNOWN_BYTES;

/* These are the ids of the subtrees that we may be creating */
static int ett_iax2;
static int ett_iax2_full_mini_subtree;
static int ett_iax2_type;              /* Frame-type specific subtree */
static int ett_iax2_ie;                /* single IE */
static int ett_iax2_codecs;            /* capabilities IE */
static int ett_iax2_ies_apparent_addr; /* apparent address IE */
static int ett_iax2_fragment;
static int ett_iax2_fragments;
static int ett_iax2_trunk_cmddata;
static int ett_iax2_trunk_call;

static expert_field ei_iax_too_many_transfers;
static expert_field ei_iax_circuit_id_conflict;
static expert_field ei_iax_peer_address_unsupported;
static expert_field ei_iax_invalid_len;

static dissector_handle_t iax2_handle;

static const fragment_items iax2_fragment_items = {
  &ett_iax2_fragment,
  &ett_iax2_fragments,
  &hf_iax2_fragments,
  &hf_iax2_fragment,
  &hf_iax2_fragment_overlap,
  &hf_iax2_fragment_overlap_conflict,
  &hf_iax2_fragment_multiple_tails,
  &hf_iax2_fragment_too_long_fragment,
  &hf_iax2_fragment_error,
  &hf_iax2_fragment_count,
  &hf_iax2_reassembled_in,
  &hf_iax2_reassembled_length,
  /* Reassembled data field */
  NULL,
  "iax2 fragments"
};

/* data-call subdissectors, AST_DATAFORMAT_* */
static dissector_table_t iax2_dataformat_dissector_table;
/* voice/video call subdissectors, AST_FORMAT_* */
static dissector_table_t iax2_codec_dissector_table;


/* IAX2 Meta trunk packet Command data flags */
#define IAX2_TRUNK_TS 1

/* IAX2 Full-frame types */
static const value_string iax_frame_types[] = {
  {0,                    "(0?)"},
  {AST_FRAME_DTMF_END,   "DTMF End"},
  {AST_FRAME_VOICE,      "Voice"},
  {AST_FRAME_VIDEO,      "Video"},
  {AST_FRAME_CONTROL,    "Control"},
  {AST_FRAME_NULL,       "NULL"},
  {AST_FRAME_IAX,        "IAX"},
  {AST_FRAME_TEXT,       "Text"},
  {AST_FRAME_IMAGE,      "Image"},
  {AST_FRAME_HTML,       "HTML"},
  {AST_FRAME_CNG,        "Comfort Noise"},
  {AST_FRAME_MODEM,      "Modem"},
  {AST_FRAME_DTMF_BEGIN, "DTMF Begin"},
  {0, NULL}
};
static value_string_ext iax_frame_types_ext = VALUE_STRING_EXT_INIT(iax_frame_types);

/* Subclasses for IAX packets */
static const value_string iax_iax_subclasses[] = {
  { 0, "(0?)"},
  { 1, "NEW"},
  { 2, "PING"},
  { 3, "PONG"},
  { 4, "ACK"},
  { 5, "HANGUP"},
  { 6, "REJECT"},
  { 7, "ACCEPT"},
  { 8, "AUTHREQ"},
  { 9, "AUTHREP"},
  {10, "INVAL"},
  {11, "LAGRQ"},
  {12, "LAGRP"},
  {13, "REGREQ"},
  {14, "REGAUTH"},
  {15, "REGACK"},
  {16, "REGREJ"},
  {17, "REGREL"},
  {18, "VNAK"},
  {19, "DPREQ"},
  {20, "DPREP"},
  {21, "DIAL"},
  {22, "TXREQ"},
  {23, "TXCNT"},
  {24, "TXACC"},
  {25, "TXREADY"},
  {26, "TXREL"},
  {27, "TXREJ"},
  {28, "QUELCH"},
  {29, "UNQULCH"},
  {30, "POKE"},
  {31, "PAGE"},
  {32, "MWI"},
  {33, "UNSUPPORTED"},
  {34, "TRANSFER"},
  {35, "PROVISION"},
  {36, "FWDOWNL"},
  {37, "FWDATA"},
  {38, "TXMEDIA"},
  {39, "RTKEY"},
  {40, "CALLTOKEN"},
  {0, NULL}
};
static value_string_ext iax_iax_subclasses_ext = VALUE_STRING_EXT_INIT(iax_iax_subclasses);

/* Subclasses for Control packets */
static const value_string iax_cmd_subclasses[] = {
  {0, "(0?)"},
  {1, "HANGUP"},
  {2, "RING"},
  {3, "RINGING"},
  {4, "ANSWER"},
  {5, "BUSY"},
  {6, "TKOFFHK"},
  {7, "OFFHOOK"},
  {0xFF, "stop sounds"}, /* sent by app_dial, and not much else */
  {0, NULL}
};
static value_string_ext iax_cmd_subclasses_ext = VALUE_STRING_EXT_INIT(iax_cmd_subclasses);

/* IAX2 to tap-voip call state mapping for command frames */
static const voip_call_state tap_cmd_voip_state[] = {
  VOIP_NO_STATE,
  VOIP_COMPLETED, /*HANGUP*/
  VOIP_RINGING,   /*RING*/
  VOIP_RINGING,   /*RINGING*/
  VOIP_IN_CALL,   /*ANSWER*/
  VOIP_REJECTED,  /*BUSY*/
  VOIP_UNKNOWN,   /*TKOFFHK*/
  VOIP_UNKNOWN    /*OFFHOOK*/
};
#define NUM_TAP_CMD_VOIP_STATES array_length(tap_cmd_voip_state)

/* IAX2 to tap-voip call state mapping for IAX frames */
static const voip_call_state tap_iax_voip_state[] = {
  VOIP_NO_STATE,
  VOIP_CALL_SETUP, /*NEW*/
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_COMPLETED,  /*HANGUP*/
  VOIP_REJECTED,   /*REJECT*/
  VOIP_RINGING,    /*ACCEPT*/
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_CALL_SETUP, /*DIAL*/
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE,
  VOIP_NO_STATE
};

#define NUM_TAP_IAX_VOIP_STATES array_length(tap_iax_voip_state)

/* Subclasses for Modem packets */
static const value_string iax_modem_subclasses[] = {
  {0, "(0?)"},
  {1, "T.38"},
  {2, "V.150"},
  {0, NULL}
};

/* Subclasses for Text packets */
static const value_string iax_text_subclasses[] = {
  {0, "Text"},
  {0, NULL}
};

/* Subclasses for HTML packets */
static const value_string iax_html_subclasses[] = {
  {0x01, "Sending a URL"},
  {0x02, "Data frame"},
  {0x04, "Beginning frame"},
  {0x08, "End frame"},
  {0x10, "Load is complete"},
  {0x11, "Peer does not support HTML"},
  {0x12, "Link URL"},
  {0x13, "Unlink URL"},
  {0x14, "Reject Link URL"},
  {0, NULL}
};


/* Information elements */
static const value_string iax_ies_type[] = {
  {IAX_IE_CALLED_NUMBER,   "Number/extension being called"},
  {IAX_IE_CALLING_NUMBER,  "Calling number"},
  {IAX_IE_CALLING_ANI,     "Calling number ANI for billing"},
  {IAX_IE_CALLING_NAME,    "Name of caller"},
  {IAX_IE_CALLED_CONTEXT,  "Context for number"},
  {IAX_IE_USERNAME,        "Username (peer or user) for authentication"},
  {IAX_IE_PASSWORD,        "Password for authentication"},
  {IAX_IE_CAPABILITY,      "Actual codec capability"},
  {IAX_IE_FORMAT,          "Desired codec format"},
  {IAX_IE_LANGUAGE,        "Desired language"},
  {IAX_IE_VERSION,         "Protocol version"},
  {IAX_IE_ADSICPE,         "CPE ADSI capability"},
  {IAX_IE_DNID,            "Originally dialed DNID"},
  {IAX_IE_AUTHMETHODS,     "Authentication method(s)"},
  {IAX_IE_CHALLENGE,       "Challenge data for MD5/RSA"},
  {IAX_IE_MD5_RESULT,      "MD5 challenge result"},
  {IAX_IE_RSA_RESULT,      "RSA challenge result"},
  {IAX_IE_APPARENT_ADDR,   "Apparent address of peer"},
  {IAX_IE_REFRESH,         "When to refresh registration"},
  {IAX_IE_DPSTATUS,        "Dialplan status"},
  {IAX_IE_CALLNO,          "Call number of peer"},
  {IAX_IE_CAUSE,           "Cause"},
  {IAX_IE_IAX_UNKNOWN,     "Unknown IAX command"},
  {IAX_IE_MSGCOUNT,        "How many messages waiting"},
  {IAX_IE_AUTOANSWER,      "Request auto-answering"},
  {IAX_IE_MUSICONHOLD,     "Request musiconhold with QUELCH"},
  {IAX_IE_TRANSFERID,      "Transfer Request Identifier"},
  {IAX_IE_RDNIS,           "Referring DNIS"},
  {IAX_IE_PROVISIONING,    "Provisioning info"},
  {IAX_IE_AESPROVISIONING, "AES Provisioning info"},
  {IAX_IE_DATETIME,        "Date/Time"},
  {IAX_IE_DEVICETYPE,      "Device type"},
  {IAX_IE_SERVICEIDENT,    "Service Identifier"},
  {IAX_IE_FIRMWAREVER,     "Firmware revision"},
  {IAX_IE_FWBLOCKDESC,     "Firmware block description"},
  {IAX_IE_FWBLOCKDATA,     "Firmware block of data"},
  {IAX_IE_PROVVER,         "Provisioning version"},
  {IAX_IE_CALLINGPRES,     "Calling presentation"},
  {IAX_IE_CALLINGTON,      "Calling type of number"},
  {IAX_IE_CALLINGTNS,      "Calling transit network select"},
  {IAX_IE_SAMPLINGRATE,    "Supported sampling rates"},
  {IAX_IE_CAUSECODE,       "Hangup cause"},
  {IAX_IE_ENCRYPTION,      "Encryption format"},
  {IAX_IE_ENCKEY,          "Raw encryption key"},
  {IAX_IE_CODEC_PREFS,     "Codec preferences"},
  {IAX_IE_RR_JITTER,       "Received jitter"},
  {IAX_IE_RR_LOSS,         "Received loss"},
  {IAX_IE_RR_PKTS,         "Received frames"},
  {IAX_IE_RR_DELAY,        "Max playout delay in ms for received frames"},
  {IAX_IE_RR_DROPPED,      "Dropped frames"},
  {IAX_IE_RR_OOO,          "Frames received out of order"},
  {IAX_IE_VARIABLE,        "IAX2 variable"},
  {IAX_IE_OSPTOKEN,        "OSP Token"},
  {IAX_IE_CALLTOKEN,       "Call Token"},
  {IAX_IE_CAPABILITY2,     "64-bit codec capability"},
  {IAX_IE_FORMAT2,         "64-bit codec format"},
  {IAX_IE_DATAFORMAT,      "Data call format"},
  {0, NULL}
};
static value_string_ext iax_ies_type_ext = VALUE_STRING_EXT_INIT(iax_ies_type);

#define CODEC_MASK(codec) ((codec) == (uint32_t)-1 ? 0 : (UINT64_C(1) << (codec)))

static const val64_string codec_types[] = {
  {CODEC_MASK(AST_FORMAT_G723_1),    "G.723.1 compression"},
  {CODEC_MASK(AST_FORMAT_GSM),       "GSM compression"},
  {CODEC_MASK(AST_FORMAT_ULAW),      "Raw mu-law data (G.711)"},
  {CODEC_MASK(AST_FORMAT_ALAW),      "Raw A-law data (G.711)"},
  {CODEC_MASK(AST_FORMAT_G726_AAL2), "ADPCM (G.726), 32kbps, AAL2 codeword packing)"},
  {CODEC_MASK(AST_FORMAT_ADPCM),     "ADPCM (IMA)"},
  {CODEC_MASK(AST_FORMAT_SLINEAR),   "Raw 16-bit Signed Linear (8000 Hz) PCM"},
  {CODEC_MASK(AST_FORMAT_LPC10),     "LPC10, 180 samples/frame"},
  {CODEC_MASK(AST_FORMAT_G729A),     "G.729a Audio"},
  {CODEC_MASK(AST_FORMAT_SPEEX),     "SpeeX Free Compression"},
  {CODEC_MASK(AST_FORMAT_ILBC),      "iLBC Free Compression"},
  {CODEC_MASK(AST_FORMAT_G726),      "ADPCM (G.726, 32kbps, RFC3551 codeword packing)"},
  {CODEC_MASK(AST_FORMAT_G722),      "G.722"},
  {CODEC_MASK(AST_FORMAT_SIREN7),    "G.722.1 (also known as Siren7, 32kbps assumed)"},
  {CODEC_MASK(AST_FORMAT_SIREN14),   "G.722.1 Annex C (also known as Siren14, 48kbps assumed)"},
  {CODEC_MASK(AST_FORMAT_SLINEAR16), "Raw 16-bit Signed Linear (16000 Hz) PCM"},
  {CODEC_MASK(AST_FORMAT_JPEG),      "JPEG Images"},
  {CODEC_MASK(AST_FORMAT_PNG),       "PNG Images"},
  {CODEC_MASK(AST_FORMAT_H261),      "H.261 Video"},
  {CODEC_MASK(AST_FORMAT_H263),      "H.263 Video"},
  {CODEC_MASK(AST_FORMAT_H263_PLUS), "H.263+ Video"},
  {CODEC_MASK(AST_FORMAT_H264),      "H.264 Video"},
  {CODEC_MASK(AST_FORMAT_MP4_VIDEO), "MPEG4 Video"},
  {CODEC_MASK(AST_FORMAT_VP8),       "VP8 Video"},
  {CODEC_MASK(AST_FORMAT_T140_RED),  "T.140 RED Text format RFC 4103"},
  {CODEC_MASK(AST_FORMAT_T140),      "T.140 Text format - ITU T.140, RFC 4103"},
  {CODEC_MASK(AST_FORMAT_G719),      "G.719 (64 kbps assumed)"},
  {CODEC_MASK(AST_FORMAT_SPEEX16),   "SpeeX Wideband (16kHz) Free Compression"},
  {CODEC_MASK(AST_FORMAT_OPUS),      "Opus audio (8kHz, 16kHz, 24kHz, 48Khz)"},
  {CODEC_MASK(AST_FORMAT_TESTLAW),   "Raw testing-law data (G.711)"},
  {0, NULL}
};
static val64_string_ext codec_types_ext = VAL64_STRING_EXT_INIT(codec_types);

static const value_string iax_dataformats[] = {
  {AST_DATAFORMAT_NULL,      "N/A (analogue call?)"},
  {AST_DATAFORMAT_V110,      "ITU-T V.110 rate adaption"},
  {AST_DATAFORMAT_H223_H245, "ITU-T H.223/H.245"},
  {0, NULL}
};


static const value_string iax_packet_types[] = {
  {IAX2_FULL_PACKET,       "Full packet"},
  {IAX2_MINI_VOICE_PACKET, "Mini voice packet"},
  {IAX2_MINI_VIDEO_PACKET, "Mini video packet"},
  {IAX2_TRUNK_PACKET,      "Trunk packet"},
  {0, NULL}
};

static const value_string iax_causecodes[] = {
  {AST_CAUSE_UNALLOCATED,                   "Unallocated"},
  {AST_CAUSE_NO_ROUTE_TRANSIT_NET,          "No route transit net"},
  {AST_CAUSE_NO_ROUTE_DESTINATION,          "No route to destination"},
  {AST_CAUSE_MISDIALLED_TRUNK_PREFIX,       "Misdialled trunk prefix"},
  {AST_CAUSE_CHANNEL_UNACCEPTABLE,          "Channel unacceptable"},
  {AST_CAUSE_CALL_AWARDED_DELIVERED,        "Call awarded delivered"},
  {AST_CAUSE_PRE_EMPTED,                    "Preempted"},
  {AST_CAUSE_NUMBER_PORTED_NOT_HERE,        "Number ported not here"},
  {AST_CAUSE_NORMAL_CLEARING,               "Normal clearing"},
  {AST_CAUSE_USER_BUSY,                     "User busy"},
  {AST_CAUSE_NO_USER_RESPONSE,              "No user response"},
  {AST_CAUSE_NO_ANSWER,                     "No answer"},
  {AST_CAUSE_SUBSCRIBER_ABSENT,             "Subscriber absent"},
  {AST_CAUSE_CALL_REJECTED,                 "Call rejected"},
  {AST_CAUSE_NUMBER_CHANGED,                "Number changed"},
  {AST_CAUSE_REDIRECTED_TO_NEW_DESTINATION, "Redirected to new destination"},
  {AST_CAUSE_ANSWERED_ELSEWHERE,            "Answered elsewhere"},
  {AST_CAUSE_DESTINATION_OUT_OF_ORDER,      "Destination out of order"},
  {AST_CAUSE_INVALID_NUMBER_FORMAT,         "Invalid number format"},
  {AST_CAUSE_FACILITY_REJECTED,             "Facility rejected"},
  {AST_CAUSE_RESPONSE_TO_STATUS_ENQUIRY,    "Response to status inquiry"},
  {AST_CAUSE_NORMAL_UNSPECIFIED,            "Normal unspecified"},
  {AST_CAUSE_NORMAL_CIRCUIT_CONGESTION,     "Normal circuit congestion"},
  {AST_CAUSE_NETWORK_OUT_OF_ORDER,          "Network out of order"},
  {AST_CAUSE_NORMAL_TEMPORARY_FAILURE,      "Normal temporary failure"},
  {AST_CAUSE_SWITCH_CONGESTION,             "Switch congestion"},
  {AST_CAUSE_ACCESS_INFO_DISCARDED,         "Access info discarded"},
  {AST_CAUSE_REQUESTED_CHAN_UNAVAIL,        "Requested channel unavailable"},
  {AST_CAUSE_FACILITY_NOT_SUBSCRIBED,       "Facility not subscribed"},
  {AST_CAUSE_OUTGOING_CALL_BARRED,          "Outgoing call barred"},
  {AST_CAUSE_INCOMING_CALL_BARRED,          "Incoming call barred"},
  {AST_CAUSE_BEARERCAPABILITY_NOTAUTH,      "Bearer capability not authorized"},
  {AST_CAUSE_BEARERCAPABILITY_NOTAVAIL,     "Bearer capability not available"},
  {AST_CAUSE_BEARERCAPABILITY_NOTIMPL,      "Bearer capability not implemented"},
  {AST_CAUSE_CHAN_NOT_IMPLEMENTED,          "Channel not implemented"},
  {AST_CAUSE_FACILITY_NOT_IMPLEMENTED,      "Facility not implemented"},
  {AST_CAUSE_INVALID_CALL_REFERENCE,        "Invalid call reference"},
  {AST_CAUSE_INCOMPATIBLE_DESTINATION,      "Incompatible destination"},
  {AST_CAUSE_INVALID_MSG_UNSPECIFIED,       "Invalid message unspecified"},
  {AST_CAUSE_MANDATORY_IE_MISSING,          "Mandatory IE missing"},
  {AST_CAUSE_MESSAGE_TYPE_NONEXIST,         "Message type nonexistent"},
  {AST_CAUSE_WRONG_MESSAGE,                 "Wrong message"},
  {AST_CAUSE_IE_NONEXIST,                   "IE nonexistent"},
  {AST_CAUSE_INVALID_IE_CONTENTS,           "Invalid IE contents"},
  {AST_CAUSE_WRONG_CALL_STATE,              "Wrong call state"},
  {AST_CAUSE_RECOVERY_ON_TIMER_EXPIRE,      "Recovery on timer expire"},
  {AST_CAUSE_MANDATORY_IE_LENGTH_ERROR,     "Mandatory IE length error"},
  {AST_CAUSE_PROTOCOL_ERROR,                "Protocol error"},
  {AST_CAUSE_INTERWORKING,                  "Interworking"},
  {0, NULL}
};
static value_string_ext iax_causecodes_ext = VALUE_STRING_EXT_INIT(iax_causecodes);

/* ************************************************************************* */

/* In order to track IAX calls, we have a hash table which maps
 * {addr,port type,port,call} to a unique circuit id.
 *
 * Each call has two such circuits associated with it (a forward and a
 * reverse circuit, where 'forward' is defined as the direction the NEW
 * packet went in), and we maintain an iax_call_data structure for each
 * call, attached to both circuits with circuit_add_proto_data.
 *
 * Because {addr,port type,port,call} quadruplets can be reused
 * (Asterisk reuses call numbers), circuit ids aren't unique to
 * individual calls and we treat NEW packets somewhat specially. When we
 * get such a packet, we see if there are any calls with a matching
 * circuit id, and make sure that its circuits are marked as ended
 * before that packet.
 *
 * A second complication is that we only know one quadruplet at the time
 * the NEW packet is processed: there is therefore cunningness in
 * iax_lookup_circuit_details() to look for replies to NEW packets and
 * create the reverse circuit.
 */


/* start with a hash of {addr,port type,port,call}->{id} */

typedef struct {
  address   addr;
  port_type ptype;
  uint32_t  port;
  uint32_t  callno;

  /* this is where addr->data points to. it's put in here for easy freeing */
  uint8_t address_data[MAX_ADDRESS];
} iax_circuit_key;

/* tables */
static GHashTable *iax_fid_table;
static reassembly_table iax_reassembly_table;

static GHashTable *iax_circuit_hashtab;
static unsigned circuitcount;

/* the number of keys and values to reserve space for in each memory chunk.
   We assume we won't be tracking many calls at once so this is quite low.
*/
#define IAX_INIT_PACKET_COUNT 10

#ifdef DEBUG_HASHING
static char *key_to_str( const iax_circuit_key *key )
{
  static int    i = 0;
  static char   str[3][80];
  char         *strp;
  char         *addrstr;

  i++;
  if (i >= 3) {
    i = 0;
  }
  strp = str[i];

  addrstr = address_to_str(NULL, &key->addr);
  snprintf(strp, 80, "{%s:%i,%i}",
             addrstr,
             key->port,
             key->callno);
  wmem_free(NULL, addrstr);
  return strp;
}
#endif

/* Hash Functions */
static int iax_circuit_equal(const void *v, const void *w)
{
  const iax_circuit_key *v1 = (const iax_circuit_key *)v;
  const iax_circuit_key *v2 = (const iax_circuit_key *)w;
  int result;

  result = (addresses_equal(&(v1->addr), &(v2->addr)) &&
            v1->ptype == v2->ptype &&
            v1->port  == v2->port  &&
            v1->callno== v2->callno);
#ifdef DEBUG_HASHING
  ws_debug("+++ Comparing for equality: %s, %s: %u", key_to_str(v1), key_to_str(v2), result);
#endif

  return result;
}

static unsigned iax_circuit_hash(const void *v)
{
  const iax_circuit_key *key = (const iax_circuit_key *)v;
  unsigned               hash_val;

  hash_val = 0;
  hash_val = add_address_to_hash(hash_val, &key->addr);
  hash_val += (unsigned)(key->ptype);
  hash_val += (unsigned)(key->port);
  hash_val += (unsigned)(key->callno);

#ifdef DEBUG_HASHING
  ws_debug("+++ Hashing key: %s, result %#x", key_to_str(key), hash_val);
#endif

  return (unsigned)hash_val;
}

/* Find, or create, a circuit for the given
   {address,porttype,port,call} quadruplet
*/
static unsigned iax_circuit_lookup(const address *address_p,
                                port_type ptype,
                                uint32_t port,
                                uint32_t callno)
{
  iax_circuit_key  key;
  uint32_t        *circuit_id_p;

  key.addr   = *address_p;
  key.ptype  = ptype;
  key.port   = port;
  key.callno = callno;

  circuit_id_p = (uint32_t *)g_hash_table_lookup(iax_circuit_hashtab, &key);
  if (! circuit_id_p) {
    iax_circuit_key *new_key;

    new_key = wmem_new(wmem_file_scope(), iax_circuit_key);
    new_key->addr.type = address_p->type;
    new_key->addr.len  = MIN(address_p->len, MAX_ADDRESS);
    new_key->addr.data = new_key->address_data;
    if (new_key->addr.len > 0)
      memcpy(new_key->address_data, address_p->data, new_key->addr.len);
    new_key->ptype     = ptype;
    new_key->port      = port;
    new_key->callno    = callno;

    circuit_id_p  = (uint32_t *)wmem_new(wmem_file_scope(), iax_circuit_key);
    *circuit_id_p = ++circuitcount;

    g_hash_table_insert(iax_circuit_hashtab, new_key, circuit_id_p);

#ifdef DEBUG_HASHING
    ws_debug("Created new circuit id %u for node %s", *circuit_id_p, key_to_str(new_key));
#endif
  }

  return *circuit_id_p;
}


/* ************************************************************************* */

typedef struct {
  uint32_t    current_frag_id; /* invalid unless current_frag_bytes > 0 */
  uint32_t    current_frag_bytes;
  uint32_t    current_frag_minlen;
} iax_call_dirdata;

/* This is our per-call data structure, which is attached to both the
 * forward and reverse circuits.
 */
typedef struct iax_call_data {
  /* For this data, src and dst are relative to the original direction under
     which this call is stored. Obviously if the reversed flag is set true by
     iax_find_call, src and dst are reversed relative to the direction the
     actual source and destination of the data.

     if the codec changes mid-call, we update it here; because we store a codec
     number with each packet too, we handle going back to earlier packets
     without problem.
  */

  iax_dataformat_t dataformat;
  uint32_t         src_codec, dst_codec;
  uint32_t         src_vformat, dst_vformat;

  /* when a transfer takes place, we'll get a new circuit id; we assume that we
     don't try to transfer more than IAX_MAX_TRANSFERS times in a call */
  unsigned forward_circuit_ids[IAX_MAX_TRANSFERS];
  unsigned reverse_circuit_ids[IAX_MAX_TRANSFERS];
  unsigned n_forward_circuit_ids;
  unsigned n_reverse_circuit_ids;

  /* this is the subdissector for the call */
  dissector_handle_t subdissector;

  /* the absolute start time of the call */
  nstime_t start_time;

  /* time stamp from last full frame, in the first pass */
  uint32_t last_full_frame_ts;

  iax_call_dirdata dirdata[2];
} iax_call_data;



/* creates a new CONVERSATION_IAX2 circuit with a specified circuit id for a call
 *
 * typically a call has up to three associated circuits: an original source, an
 * original destination, and the result of a transfer.
 *
 * For each endpoint, a CONVERSATION_IAX2 circuit is created and added to the call_data
 * by this function
 *
 * 'reversed' should be true if this end is the one which would have _received_
 * the NEW packet, or it is an endpoint to which the 'destination' is being
 * transferred.
 *
 */
static conversation_t *iax2_new_circuit_for_call(packet_info *pinfo, proto_item * item,
                                            unsigned circuit_id, unsigned framenum,
                                            iax_call_data *iax_call, bool reversed)
{
  conversation_t *conv;

  if(!iax_call){
    return NULL;
  }
  if ((reversed && iax_call->n_reverse_circuit_ids >= IAX_MAX_TRANSFERS) ||
      (! reversed && iax_call->n_forward_circuit_ids >= IAX_MAX_TRANSFERS)) {
    expert_add_info(pinfo, item, &ei_iax_too_many_transfers);
    return NULL;
  }

  conv = conversation_new_by_id(framenum, CONVERSATION_IAX2,
                    circuit_id);

  conversation_add_proto_data(conv, proto_iax2, iax_call);

  if (reversed)
    iax_call -> reverse_circuit_ids[iax_call->n_reverse_circuit_ids++] = circuit_id;
  else
    iax_call -> forward_circuit_ids[iax_call->n_forward_circuit_ids++] = circuit_id;

  return conv;
}


/* returns true if this circuit id is a "forward" circuit for this call: ie, it
 * is the point which _sent_ the original 'NEW' packet, or a point to which that
 * end was subsequently transferred */
static bool is_forward_circuit(unsigned circuit_id,
                                   const iax_call_data *iax_call)
{
  unsigned i;
  for(i=0; i<iax_call->n_forward_circuit_ids; i++) {
    if (circuit_id == iax_call->forward_circuit_ids[i])
      return true;
  }
  return false;
}

/* returns true if this circuit id is a "reverse" circuit for this call: ie, it
 * is the point which _received_ the original 'NEW' packet, or a point to which that
 * end was subsequently transferred */
static bool is_reverse_circuit(unsigned circuit_id,
                                   const iax_call_data *iax_call)
{
  unsigned i;
  for(i=0; i<iax_call->n_reverse_circuit_ids; i++){
    if (circuit_id == iax_call->reverse_circuit_ids[i])
      return true;
  }
  return false;
}


static iax_call_data *iax_lookup_call_from_dest(packet_info *pinfo, proto_item * item,
                                                 unsigned src_circuit_id,
                                                 unsigned dst_circuit_id,
                                                 unsigned framenum,
                                                 bool *reversed_p)
{
  conversation_t  *dst_conv;
  iax_call_data *iax_call;
  bool           reversed = false;

  dst_conv = find_conversation_by_id(framenum, CONVERSATION_IAX2, dst_circuit_id);

  if (!dst_conv) {
#ifdef DEBUG_HASHING
    ws_debug("++ destination circuit not found, must have missed NEW packet");
#endif
    if (reversed_p)
      *reversed_p = false;
    return NULL;
  }

#ifdef DEBUG_HASHING
  ws_debug("++ found destination circuit");
#endif

  iax_call = (iax_call_data *)conversation_get_proto_data(dst_conv, proto_iax2);

  /* there's no way we can create a CONVERSATION_IAX2 circuit without adding
     iax call data to it; assert this */
  DISSECTOR_ASSERT(iax_call);

  if (is_forward_circuit(dst_circuit_id, iax_call)) {
#ifdef DEBUG_HASHING
    ws_debug("++ destination circuit matches forward_circuit_id of call, "
             "therefore packet is reversed");
#endif

    reversed = true;

    if (iax_call -> n_reverse_circuit_ids == 0) {
      /* we are going in the reverse direction, and this call
         doesn't have a reverse circuit associated with it.
         create one now. */
#ifdef DEBUG_HASHING
      ws_debug("++ reverse_circuit_id of call is zero, need to create a "
              "new reverse circuit for this call");
#endif

      iax2_new_circuit_for_call(pinfo, item, src_circuit_id, framenum, iax_call, true);
#ifdef DEBUG_HASHING
      ws_debug("++ done");
#endif
    } else if (!is_reverse_circuit(src_circuit_id, iax_call)) {
      expert_add_info_format(pinfo, item, &ei_iax_circuit_id_conflict,
                "IAX Packet %u from circuit ids %u->%u conflicts with earlier call with circuit ids %u->%u",
                framenum,
                src_circuit_id, dst_circuit_id,
                iax_call->forward_circuit_ids[0],
                iax_call->reverse_circuit_ids[0]);
      return NULL;
    }
  } else if (is_reverse_circuit(dst_circuit_id, iax_call)) {
#ifdef DEBUG_HASHING
    ws_debug("++ destination circuit matches reverse_circuit_id of call, "
            "therefore packet is forward");
#endif

    reversed = false;
    if (!is_forward_circuit(src_circuit_id, iax_call)) {
      expert_add_info_format(pinfo, item, &ei_iax_circuit_id_conflict,
                "IAX Packet %u from circuit ids %u->%u conflicts with earlier call with circuit ids %u->%u",
                framenum,
                src_circuit_id, dst_circuit_id,
                iax_call->forward_circuit_ids[0],
                iax_call->reverse_circuit_ids[0]);
      if (reversed_p)
        *reversed_p = false;
      return NULL;
    }
  } else {
    DISSECTOR_ASSERT_NOT_REACHED();
  }

  if (reversed_p)
    *reversed_p = reversed;

  return iax_call;
}


/* looks up an iax_call for this packet */
static iax_call_data *iax_lookup_call( packet_info *pinfo,
                                       uint32_t scallno,
                                       uint32_t dcallno,
                                       bool *reversed_p)
{
  bool           reversed = false;
  iax_call_data *iax_call = NULL;
  unsigned       src_circuit_id;
#ifdef DEBUG_HASHING
  char          *srcstr, *dststr;
#endif

#ifdef DEBUG_HASHING
  srcstr = address_to_str(NULL, &pinfo->src);
  dststr = address_to_str(NULL, &pinfo->dst);
  ws_debug("++ iax_lookup_circuit_details: Looking up circuit for frame %u, "
          "from {%s:%u:%u} to {%s:%u:%u}", pinfo->num,
          srcstr, pinfo->srcport, scallno,
          dststr, pinfo->destport, dcallno);
  wmem_free(NULL, srcstr);
  wmem_free(NULL, dststr);
#endif


  src_circuit_id = iax_circuit_lookup(&pinfo->src, pinfo->ptype,
                                      pinfo->srcport, scallno);


  /* the most reliable indicator of call is the destination callno, if
     we have one */
  if (dcallno != 0) {
    unsigned dst_circuit_id;
#ifdef DEBUG_HASHING
    ws_debug("++ dcallno non-zero, looking up destination circuit");
#endif

    dst_circuit_id = iax_circuit_lookup(&pinfo->dst, pinfo->ptype,
                                        pinfo->destport, dcallno);

    iax_call = iax_lookup_call_from_dest(pinfo, NULL, src_circuit_id, dst_circuit_id,
                                         pinfo->num, &reversed);
  } else {
    conversation_t *src_conv;

    /* in all other circumstances, the source circuit should already
     * exist: its absence indicates that we missed the all-important NEW
     * packet.
     */

    src_conv = find_conversation_by_id(pinfo->num, CONVERSATION_IAX2, src_circuit_id);

    if (src_conv) {
      iax_call = (iax_call_data *)conversation_get_proto_data(src_conv, proto_iax2);

      /* there's no way we can create a CONVERSATION_IAX2 circuit without adding
         iax call data to it; assert this */
      DISSECTOR_ASSERT(iax_call);

      if (is_forward_circuit(src_circuit_id, iax_call))
        reversed = false;
      else if (is_reverse_circuit(src_circuit_id, iax_call))
        reversed = true;
      else {
        /* there's also no way we can attach an iax_call_data to a circuit
           without the circuit being either the forward or reverse circuit
           for that call; assert this too.
        */
        DISSECTOR_ASSERT_NOT_REACHED();
      }
    }
  }

  if (reversed_p)
    *reversed_p = reversed;

#ifdef DEBUG_HASHING
  if (iax_call) {
    ws_debug("++ Found call for packet: id %u, reversed=%c", iax_call->forward_circuit_ids[0], reversed?'1':'0');
  } else {
    ws_debug("++ Call not found. Must have missed the NEW packet?");
  }
#endif

  return iax_call;
}

/* initialize the per-direction parts of an iax_call_data structure */
static void init_dir_data(iax_call_dirdata *dirdata)
{
  dirdata -> current_frag_bytes=0;
  dirdata -> current_frag_minlen=0;
}


/* handles a NEW packet by creating a new iax call and forward circuit.
   the reverse circuit is not created until the ACK is received and
   is created by iax_lookup_circuit_details. */
static iax_call_data *iax_new_call( packet_info *pinfo,
                                    uint32_t scallno)
{
  iax_call_data         *call;
  unsigned               circuit_id;
  static const nstime_t  millisecond = NSTIME_INIT_SECS_MSECS(0, 1);

#ifdef DEBUG_HASHING
  ws_debug("+ new_circuit: Handling NEW packet, frame %u", pinfo->num);
#endif

  circuit_id = iax_circuit_lookup(&pinfo->src, pinfo->ptype,
                                  pinfo->srcport, scallno);

  call = wmem_new(wmem_file_scope(), iax_call_data);
  call -> dataformat = AST_DATAFORMAT_NULL;
  call -> src_codec = 0;
  call -> dst_codec = 0;
  call -> src_vformat = 0;
  call -> dst_vformat = 0;
  call -> n_forward_circuit_ids = 0;
  call -> n_reverse_circuit_ids = 0;
  call -> subdissector = NULL;
  call -> start_time = pinfo->abs_ts;
  call -> last_full_frame_ts = 0;
  nstime_delta(&call -> start_time, &call -> start_time, &millisecond);
  init_dir_data(&call->dirdata[0]);
  init_dir_data(&call->dirdata[1]);

  iax2_new_circuit_for_call(pinfo, NULL, circuit_id, pinfo->num, call, false);

  return call;
}


/* ************************************************************************* */

/* per-packet data */
typedef struct iax_packet_data {
  bool           first_time; /* we're dissecting this packet for the first time; so
                              * things like codec and transfer requests should be
                              * propagated into the call data */
  iax_call_data *call_data;
  uint32_t       codec;
  bool           reversed;
  nstime_t       abstime;    /* the absolute time of this packet, based on its
                              * timestamp and the NEW packet's time (-1 if unknown) */
} iax_packet_data;

static iax_packet_data *iax_new_packet_data(iax_call_data *call, bool reversed)
{
  iax_packet_data *p = wmem_new(wmem_file_scope(), iax_packet_data);
  p->first_time    = true;
  p->call_data     = call;
  p->codec         = 0;
  p->reversed      = reversed;
  p->abstime.secs  = -1;
  p->abstime.nsecs = -1;
  return p;
}

static void  iax2_populate_pinfo_from_packet_data(packet_info *pinfo, const iax_packet_data *p)
{
  if (p->call_data != NULL) {
     /* if we missed the NEW packet for this call, call_data will be null. it's
      * tbd what the best thing to do here is. */
    pinfo->p2p_dir = p->reversed?P2P_DIR_RECV:P2P_DIR_SENT;

    col_set_str(pinfo->cinfo, COL_IF_DIR, p->reversed ? "rev" : "fwd");
  }
}


/* ************************************************************************* */

/* this is passed up from the IE dissector to the main dissector */
typedef struct
{
  address   peer_address;
  port_type peer_ptype;
  uint32_t  peer_port;
  uint32_t  peer_callno;
  uint32_t  dataformat;
} iax2_ie_data;


static uint32_t dissect_fullpacket(tvbuff_t *tvb, uint32_t offset,
                                  uint16_t scallno,
                                  packet_info *pinfo,
                                  proto_tree *iax2_tree,
                                  proto_tree *main_tree);


static uint32_t dissect_minipacket(tvbuff_t *tvb, uint32_t offset,
                                  uint16_t scallno,
                                  packet_info *pinfo,
                                  proto_tree *iax2_tree,
                                  proto_tree *main_tree);

static uint32_t dissect_minivideopacket(tvbuff_t *tvb, uint32_t offset,
                                       uint16_t scallno,
                                       packet_info *pinfo,
                                       proto_tree *iax2_tree,
                                       proto_tree *main_tree);

static uint32_t dissect_trunkpacket(tvbuff_t *tvb, uint32_t offset,
                                   uint16_t scallno,
                                   packet_info *pinfo,
                                   proto_tree *iax2_tree,
                                   proto_tree *main_tree);

static void dissect_payload(tvbuff_t *tvb, uint32_t offset,
                            packet_info *pinfo, proto_tree *iax2_tree,
                            proto_tree *tree, uint32_t ts, bool video,
                            iax_packet_data *iax_packet);



static int
dissect_iax2(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
  proto_item  *iax2_item;
  proto_tree  *iax2_tree;
  proto_tree  *full_mini_subtree = NULL;
  uint32_t     offset            = 0, len;
  uint16_t     scallno           = 0;
  uint16_t     stmp;
  packet_type  type;
  proto_item *full_mini_base;

  /* set up the protocol and info fields in the summary pane */
  col_set_str(pinfo->cinfo, COL_PROTOCOL, PROTO_TAG_IAX2);
  col_clear(pinfo->cinfo, COL_INFO);

  /* add the 'iax2' tree to the main tree */
  iax2_item = proto_tree_add_item(tree, proto_iax2, tvb, offset, -1, ENC_NA);
  iax2_tree = proto_item_add_subtree(iax2_item, ett_iax2);

  stmp = tvb_get_ntohs(tvb, offset);
  if (stmp == 0) {
    /* starting with 0x0000 indicates meta packet which can be either a mini
     * video packet or a trunk packet */
    offset+=2;
    stmp = tvb_get_ntohs(tvb, offset);
    if (stmp & 0x8000) {
      /* mini video packet */
      type = IAX2_MINI_VIDEO_PACKET;
      scallno = stmp & 0x7FFF;
      offset += 2;
    }
    else {
      type = IAX2_TRUNK_PACKET;
    }
  } else {
    /* The source call/fullpacket flag is common to both mini and full packets */
    scallno = tvb_get_ntohs(tvb, offset);
    offset += 2;
    if (scallno & 0x8000)
      type = IAX2_FULL_PACKET;
    else {
      type = IAX2_MINI_VOICE_PACKET;
    }
    scallno &= 0x7FFF;
  }

  full_mini_base = proto_tree_add_uint(iax2_tree, hf_iax2_packet_type, tvb, 0, offset, type);
  full_mini_subtree = proto_item_add_subtree(full_mini_base, ett_iax2_full_mini_subtree);

  if (scallno != 0)
    proto_tree_add_item(full_mini_subtree, hf_iax2_scallno, tvb, offset-2, 2, ENC_BIG_ENDIAN);

  iax2_info->ptype = type;
  iax2_info->scallno = 0;
  iax2_info->dcallno = 0;
  iax2_info->ftype = 0;
  iax2_info->csub = 0;
  iax2_info->payload_len = 0;
  iax2_info->timestamp = 0;
  iax2_info->callState = VOIP_NO_STATE;
  iax2_info->messageName = NULL;
  iax2_info->callingParty = NULL;
  iax2_info->calledParty = NULL;
  iax2_info->payload_data = NULL;

  switch (type) {
    case IAX2_FULL_PACKET:
      len = dissect_fullpacket(tvb, offset, scallno, pinfo, full_mini_subtree, tree);
      break;
    case IAX2_MINI_VOICE_PACKET:
      iax2_info->messageName = "MINI_VOICE_PACKET";
      len = dissect_minipacket(tvb, offset, scallno, pinfo, full_mini_subtree, tree);
      break;
    case IAX2_MINI_VIDEO_PACKET:
      iax2_info->messageName = "MINI_VIDEO_PACKET";
      len = dissect_minivideopacket(tvb, offset, scallno, pinfo, full_mini_subtree, tree);
      break;
    case IAX2_TRUNK_PACKET:
      iax2_info->messageName = "TRUNK_PACKET";
      len = dissect_trunkpacket(tvb, offset, scallno, pinfo, full_mini_subtree, tree);
      break;
    default:
      len = 0;
  }

  /* update the 'length' of the main IAX2 header field so that it covers just the headers,
     not the audio data. */
  proto_item_set_len(iax2_item, len);
  tap_queue_packet(iax2_tap, pinfo, iax2_info);
  return tvb_captured_length(tvb);
}

static proto_item *dissect_datetime_ie(tvbuff_t *tvb, uint32_t offset, proto_tree *ies_tree)
{
  struct tm tm;
  uint32_t  ie_val;
  nstime_t  datetime;

  proto_tree_add_item(ies_tree, hf_iax2_ies[IAX_IE_DATETIME], tvb, offset + 2, 4, ENC_BIG_ENDIAN);
  ie_val = tvb_get_ntohl(tvb, offset+2);

  /* who's crazy idea for a time encoding was this? */
  tm.tm_sec  = (ie_val       & 0x1f) << 1;
  tm.tm_min  = (ie_val>>5)   & 0x3f;
  tm.tm_hour = (ie_val>>11)  & 0x1f;
  tm.tm_mday = (ie_val>>16)  & 0x1f;
  tm.tm_mon  = ((ie_val>>21) & 0x0f) - 1;
  tm.tm_year = ((ie_val>>25) & 0x7f) + 100;
  tm.tm_isdst= -1; /* there's no info on whether DST was in force; assume it's
                    * the same as currently */

  datetime.secs = mktime(&tm);
  datetime.nsecs = 0;
  return proto_tree_add_time(ies_tree, hf_iax2_ie_datetime, tvb, offset+2, 4, &datetime);
}


/* dissect the information elements in an IAX frame. Returns the updated offset */
static uint32_t dissect_ies(tvbuff_t *tvb, packet_info *pinfo, uint32_t offset,
                           proto_tree *iax_tree, proto_item * iax_item,
                           iax2_ie_data *ie_data)
{
  DISSECTOR_ASSERT(ie_data);

  while (offset < tvb_reported_length(tvb)) {

    int     ies_type = tvb_get_uint8(tvb, offset);
    int     ies_len  = tvb_get_uint8(tvb, offset + 1);
    uint16_t apparent_addr_family;

    /* do non-tree-dependent stuff first */
    switch (ies_type) {
      case IAX_IE_DATAFORMAT:
        if (ies_len != 4) {
          proto_tree_add_expert(iax_tree, pinfo, &ei_iax_invalid_len, tvb, offset+1, 1);
          break;
        }
        ie_data -> dataformat = tvb_get_ntohl(tvb, offset+2);
        break;

      case IAX_IE_CALLED_NUMBER:
        iax2_info->calledParty = tvb_format_text(pinfo->pool, tvb, offset+2, ies_len);
        break;
      case IAX_IE_CALLING_NUMBER:
        iax2_info->callingParty = tvb_format_text(pinfo->pool, tvb, offset+2, ies_len);
        break;

      case IAX_IE_APPARENT_ADDR:
        /* The IAX2 I-D says that the "apparent address" structure
           "is the same as the linux struct sockaddr_in", without
           bothering to note that the address family field is in
           *host* byte order in that structure (the I-D seems to be
           assuming that "everything is a Vax^Wx86 or x86-64" with
           the address family field being little-endian).

           This means the address family values are the Linux
           address family values. */
        apparent_addr_family = tvb_get_letohs(tvb, offset+2);
        switch (apparent_addr_family) {
          case LINUX_AF_INET:
            /* IAX is always over UDP */
            ie_data->peer_ptype = PT_UDP;
            ie_data->peer_port = tvb_get_ntohs(tvb, offset+4);

            /* the ip address is big-endian, but then so is peer_address.data */
            set_address_tvb(&ie_data->peer_address, AT_IPv4, 4, tvb, offset+6);
            break;

          default:
            expert_add_info_format(pinfo, iax_item, &ei_iax_peer_address_unsupported,
                "Not supported in IAX dissector: peer address family of %u", apparent_addr_family);
            break;
        }
        break;
    }


    /* the rest of this stuff only needs doing if we have an iax_tree */

    if (iax_tree && ies_type < NUM_HF_IAX2_IES) {
      proto_item *ti, *ie_item = NULL;
      proto_tree *ies_tree;
      int ie_hf = hf_iax2_ies[ies_type];

      ies_tree = proto_tree_add_subtree(iax_tree, tvb, offset, ies_len+2, ett_iax2_ie, &ti, " ");

      proto_tree_add_uint(ies_tree, hf_iax2_ie_id, tvb, offset, 1, ies_type);
      proto_tree_add_uint(ies_tree, hf_iax2_length, tvb, offset + 1, 1, ies_len);


      /* hf_iax2_ies[] is an array, indexed by IE number, of header-fields, one
         per IE. Apart from a couple of special cases which require more
         complex decoding, we can just look up an entry from the array, and add
         the relevant item, although the encoding value used depends on the
         type of the item.
      */

      switch (ies_type) {
        case IAX_IE_DATETIME:
          ie_item = dissect_datetime_ie(tvb, offset, ies_tree);
          break;


        case IAX_IE_CAPABILITY:
        {
          if (ies_len != 4) {
            proto_tree_add_expert(ies_tree, pinfo, &ei_iax_invalid_len, tvb, offset+1, 1);
            break;
          }

          ie_item =
            proto_tree_add_bitmask(ies_tree, tvb, offset + 2, ie_hf,
              ett_iax2_codecs, hf_iax2_caps, ENC_BIG_ENDIAN);
          break;
        }


        case IAX_IE_CAPABILITY2:
        {
          int version = tvb_get_uint8(tvb, offset + 2);

          proto_tree_add_uint(ies_tree, hf_iax2_version, tvb, offset + 2, 1, version);

          if (version == 0) {
            if (ies_len != 9) {
              proto_tree_add_expert(ies_tree, pinfo, &ei_iax_invalid_len, tvb, offset+1, 1);
              break;
            }

            ie_item =
              proto_tree_add_bitmask(ies_tree, tvb, offset + 3, ie_hf,
                ett_iax2_codecs, hf_iax2_caps, ENC_BIG_ENDIAN);
          }
          break;
        }


        case IAX_IE_FORMAT:
        {
          if (ies_len != 4) {
            proto_tree_add_expert(ies_tree, pinfo, &ei_iax_invalid_len, tvb, offset+1, 1);
            break;
          }

          ie_item =
            proto_tree_add_item(ies_tree, ie_hf,
                                tvb, offset + 2, 4, ENC_BIG_ENDIAN);
          break;
        }


        case IAX_IE_FORMAT2:
        {
          int version = tvb_get_uint8(tvb, offset + 2);

          proto_tree_add_uint(ies_tree, hf_iax2_version, tvb, offset + 2, 1, version);

          if (version == 0) {
            if (ies_len != 9) {
              proto_tree_add_expert(ies_tree, pinfo, &ei_iax_invalid_len, tvb, offset+1, 1);
              break;
            }

            ie_item =
              proto_tree_add_item(ies_tree, ie_hf,
                                  tvb, offset + 3, 8, ENC_BIG_ENDIAN);
          }
          break;
        }


        case IAX_IE_APPARENT_ADDR:
        {
          proto_tree *sockaddr_tree;

          sockaddr_tree = proto_tree_add_subtree(ies_tree, tvb, offset + 2, 16,
                            ett_iax2_ies_apparent_addr, &ie_item, "Apparent Address");

          /* The IAX2 I-D says that the "apparent address" structure
             "is the same as the linux struct sockaddr_in", without
             bothering to note that the address family field is in
             *host* byte order in that structure (the I-D seems to be
             assuming that "everything is a Vax^Wx86 or x86-64" with
             the address family field being little-endian).

             This means the address family values are the Linux
             address family values. */
          apparent_addr_family = tvb_get_letohs(tvb, offset+2);
          proto_tree_add_uint(sockaddr_tree, hf_IAX_IE_APPARENTADDR_SINFAMILY, tvb, offset + 2, 2, apparent_addr_family);

          if (apparent_addr_family == LINUX_AF_INET) {
            uint32_t addr;
            proto_tree_add_uint(sockaddr_tree, hf_IAX_IE_APPARENTADDR_SINPORT, tvb, offset + 4, 2, ie_data->peer_port);
            memcpy(&addr, ie_data->peer_address.data, 4);
            proto_tree_add_ipv4(sockaddr_tree, hf_IAX_IE_APPARENTADDR_SINADDR, tvb, offset + 6, 4, addr);
          }
          break;
        }

        default:
          if (ie_hf != 0) {
            int explen = proto_registrar_get_length(ie_hf);
            if (explen != 0 && ies_len != explen) {
              proto_tree_add_expert(ies_tree, pinfo, &ei_iax_invalid_len, tvb, offset+1, 1);
              break;
            }

            switch (proto_registrar_get_ftype(ie_hf)) {
            case FT_UINT8:
            case FT_UINT16:
            case FT_UINT24:
            case FT_UINT32:
            case FT_UINT64:
            case FT_INT8:
            case FT_INT16:
            case FT_INT24:
            case FT_INT32:
            case FT_INT64:
            case FT_BOOLEAN:
            case FT_IPv4:
                ie_item = proto_tree_add_item(ies_tree, ie_hf, tvb, offset + 2, ies_len, ENC_BIG_ENDIAN);
                break;

            case FT_BYTES:
            case FT_NONE:
                ie_item = proto_tree_add_item(ies_tree, ie_hf, tvb, offset + 2, ies_len, ENC_NA);
                break;

            case FT_STRING:
            case FT_STRINGZ:
                ie_item = proto_tree_add_item(ies_tree, ie_hf, tvb, offset + 2, ies_len, ENC_UTF_8|ENC_NA);
                break;

            default:
                DISSECTOR_ASSERT_NOT_REACHED();
                break;
            }
          } else {
            /* we don't understand this ie: add a generic one */
            uint32_t      value;
            const uint8_t *ptr;
            const char   *ie_name = val_to_str_ext_const(ies_type, &iax_ies_type_ext, "Unknown");

            switch (ies_len) {
              case 1:
                value = tvb_get_uint8(tvb, offset + 2);
                ie_item =
                  proto_tree_add_uint_format(ies_tree, hf_IAX_IE_UNKNOWN_BYTE,
                                             tvb, offset+2, 1, value,
                                             "%s: %#02x", ie_name, value);
                break;

              case 2:
                value = tvb_get_ntohs(tvb, offset + 2);
                ie_item =
                  proto_tree_add_uint_format(ies_tree, hf_IAX_IE_UNKNOWN_I16,
                                             tvb, offset+2, 2, value,
                                             "%s: %#04x", ie_name, value);
                break;

              case 4:
                value = tvb_get_ntohl(tvb, offset + 2);
                ie_item =
                  proto_tree_add_uint_format(ies_tree, hf_IAX_IE_UNKNOWN_I32,
                                             tvb, offset+2, 4, value,
                                             "%s: %#08x", ie_name, value);
                break;

              default:
                ptr = tvb_get_string_enc(pinfo->pool, tvb, offset + 2, ies_len, ENC_ASCII);
                ie_item =
                  proto_tree_add_string_format(ies_tree, hf_IAX_IE_UNKNOWN_BYTES,
                                               tvb, offset+2, ies_len, ptr,
                                               "%s: %s", ie_name, ptr);
                break;
            }
          }
          break;
      }

      /* Retrieve the text from the item we added, and append it to the main IE
       * item */
      if (ie_item && !proto_item_is_hidden(ti)) {
        field_info *ie_finfo = PITEM_FINFO(ie_item);

        /* if the representation of the item has already been set, use that;
           else we have to allocate a block to put the text into */
        if (ie_finfo && ie_finfo->rep != NULL)
          proto_item_set_text(ti, "Information Element: %s",
                              ie_finfo->rep->representation);
        else {
          uint8_t *ie_val = (uint8_t *)wmem_alloc(pinfo->pool, ITEM_LABEL_LENGTH);
          proto_item_fill_label(ie_finfo, ie_val, NULL);
          proto_item_set_text(ti, "Information Element: %s",
                              ie_val);
        }
      }
    }

    offset += ies_len + 2;
  }
  return offset;
}

static uint32_t uncompress_subclass(uint8_t csub)
{
  /* If the SC_LOG flag is set, return 2^csub otherwise csub */
  if (csub & 0x80) {
    /* special case for 'compressed' -1 */
    if (csub == 0xff)
      return (uint32_t)-1;
    else
      return csub & 0x3F;
  }
  else {
    switch (csub) {
      case 0x01: return 0;
      case 0x02: return 1;
      case 0x04: return 2;
      case 0x08: return 3;
      case 0x10: return 4;
      case 0x20: return 5;
      case 0x40: return 6;
      default: return (uint32_t)-1;
    }
  }
}

/* returns the new offset */
static uint32_t dissect_iax2_command(tvbuff_t *tvb, uint32_t offset,
                                    packet_info *pinfo, proto_tree *tree,
                                    iax_packet_data *iax_packet)
{
  uint8_t        csub = tvb_get_uint8(tvb, offset);
  proto_item*    ti;
  iax2_ie_data   ie_data;
  iax_call_data *iax_call;

  ie_data.peer_address.type = AT_NONE;
  ie_data.peer_address.len  = 0;
  ie_data.peer_address.data = NULL;
  ie_data.peer_ptype        = PT_NONE;
  ie_data.peer_port         = 0;
  ie_data.peer_callno       = 0;
  ie_data.dataformat        = (uint32_t)-1;
  iax_call                  = iax_packet -> call_data;

  /* add the subclass */
  ti = proto_tree_add_uint(tree, hf_iax2_iax_csub, tvb, offset, 1, csub);
  offset++;

  col_append_fstr(pinfo->cinfo, COL_INFO, " %s",
                     val_to_str_ext(csub, &iax_iax_subclasses_ext, "unknown (0x%02x)"));

  if (offset >= tvb_reported_length(tvb))
    return offset;

  offset = dissect_ies(tvb, pinfo, offset, tree, ti, &ie_data);

  /* if this is a data call, set up a subdissector for the circuit */
  if (iax_call && ie_data.dataformat != (uint32_t)-1 && iax_call -> subdissector == NULL) {
    iax_call -> subdissector = dissector_get_uint_handle(iax2_dataformat_dissector_table, ie_data.dataformat);
    iax_call -> dataformat = (iax_dataformat_t)ie_data.dataformat;
  }

  /* if this is a transfer request, record it in the call data */
  if (csub == IAX_COMMAND_TXREQ && iax_packet -> first_time) {
    if (ie_data.peer_address.type != AT_NONE && ie_data.peer_callno != 0) {
      unsigned tx_circuit = iax_circuit_lookup(&ie_data.peer_address,
                                            ie_data.peer_ptype,
                                            ie_data.peer_port,
                                            ie_data.peer_callno);

      iax2_new_circuit_for_call(pinfo, NULL, tx_circuit, pinfo->num, iax_call, iax_packet->reversed);
    }
  }

  return offset;
}

static void iax2_add_ts_fields(packet_info *pinfo, proto_tree *iax2_tree, tvbuff_t *tvb, iax_packet_data *iax_packet, packet_type type, uint32_t relts)
{
  uint32_t    full_relts = relts;
  nstime_t    lateness;
  proto_item *item;

  if (iax_packet->call_data == NULL) {
    /* no call info for this frame; perhaps we missed the NEW packet */
    return;
  }

  if (iax_packet->abstime.secs == -1) {
    nstime_t rel;

    switch (type) {
    case IAX2_MINI_VOICE_PACKET:
      /* RFC 5456 says
       *
       *   Abbreviated 'Mini Frames' are normally used for audio and
       *   video; however, each time the time-stamp is a multiple of
       *   32,768 (0x8000 hex), a standard or 'Full Frame' MUST be sent.
       *
       * and, for what it later calls "Mini Frames", by which it means
       * what we're calling "mini voice packets", it says:
       *
       *   Mini frames carry a 16-bit time-stamp, which is the lower 16 bits
       *   of the transmitting peer's full 32-bit time-stamp for the call.
       *   The time-stamp allows synchronization of incoming frames so that
       *   they MAY be processed in chronological order instead of the
       *   (possibly different) order in which they are received.  The 16-bit
       *   time-stamp wraps after 65.536 seconds, at which point a full frame
       *   SHOULD be sent to notify the remote peer that its time-stamp has
       *   been reset.  A call MUST continue to send mini frames starting
       *   with time-stamp 0 even if acknowledgment of the resynchronization
       *   is not received.
       *
       * *If* we see all the full frames, that means we *should* be able
       * to convert the 16-bit time stamp to a full 32-bit time stamp by
       * ORing the upper 16 bits of the last full frame time stamp we saw
       * in above the 16-bit time stamp.
       *
       * XXX - what, if anything, should we do about full frames we've
       * missed? */
      full_relts = (iax_packet->call_data->last_full_frame_ts & 0xFFFF0000) | relts;
      break;

    case IAX2_FULL_PACKET:
    case IAX2_TRUNK_PACKET:
      /* Timestamps have the full 32 bits of the timestamp.
       * Save it, to add to the mini-packet time stamps.
       *
       * XXX - that's a maximum of 4294967296 milliseconds
       * or about 4294967 seconds or about 49 days.
       * Do we need to worry about that overflowing? */
      full_relts = relts;
      iax_packet->call_data->last_full_frame_ts = full_relts;
      break;

    case IAX2_MINI_VIDEO_PACKET:
      /* See the comment above in the IAX2_MINI_VOICE_PACKET case.
       * Note also that RFC 5456 says, in section 8.1.3.1 "Meta Video
       * Frames", which covers what we're calling "mini video packets":
       *
       *   Meta video frames carry a 16-bit time-stamp, which is the lower 16
       *   bits of the transmitting peer's full 32-bit time-stamp for the
       *   call.  When this time-stamp wraps, a Full Frame SHOULD be sent to
       *   notify the remote peer that the time-stamp has been reset to 0.
       *
       * *but* it also shows the uppermost bit of that time stamp as "?",
       * with a 15-bit time stamp, in the ASCII-art packet diagram after
       * it.  dissect_minivideopacket() says "bit 15 of the ts is used to
       * represent the rtp 'marker' bit"; presumably that's what's going
       * on, but the RFC doesn't say that.
       *
       * So we assume that the time stamp is only 15 bits, and that the
       * upper *17* bits of the last full frame's time stamp need to be
       * ORed in above the 15 bits of time stamp.
       *
       * XXX - do we need to worry about overflows or missed packets
       * with full timestamps? */
      full_relts = (iax_packet->call_data->last_full_frame_ts & 0xFFFF8000) | relts;
      break;
    }

    /* Convert the full relative time stamp to an nstime_t */
    rel.secs = full_relts / 1000;
    rel.nsecs = (full_relts % 1000) * 1000000;

    /* Add it to the start time to get the absolute time. */
    nstime_sum(&iax_packet->abstime, &iax_packet->call_data->start_time, &rel);
  }
  iax2_info->timestamp = relts; /* raw time stamp; nobody uses it */

  if (iax2_tree) {
    item = proto_tree_add_time(iax2_tree, hf_iax2_absts, tvb, 0, 0, &iax_packet->abstime);
    proto_item_set_generated(item);

    nstime_delta(&lateness, &pinfo->abs_ts, &iax_packet->abstime);

    item = proto_tree_add_time(iax2_tree, hf_iax2_lateness, tvb, 0, 0, &lateness);
    proto_item_set_generated(item);
  }
}

/* returns the new offset */
static uint32_t
dissect_fullpacket(tvbuff_t *tvb, uint32_t offset,
                   uint16_t scallno,
                   packet_info *pinfo, proto_tree *iax2_tree,
                   proto_tree *main_tree)
{
  uint16_t dcallno;
  uint32_t ts;
  uint8_t type;
  uint8_t csub;
  uint32_t codec;

  proto_tree      *packet_type_tree = NULL;
  iax_call_data   *iax_call;
  iax_packet_data *iax_packet;
  bool             reversed;
  bool             rtp_marker;

  /*
   * remove the top bit for retransmission detection
   */
  dcallno = tvb_get_ntohs(tvb, offset) & 0x7FFF;
  ts = tvb_get_ntohl(tvb, offset + 2);
  type = tvb_get_uint8(tvb, offset + 8);
  csub = tvb_get_uint8(tvb, offset + 9);
  iax2_info->ftype   = type;
  iax2_info->csub    = csub;
  iax2_info->scallno = scallno;
  iax2_info->dcallno = dcallno;

  /* see if we've seen this packet before */
  iax_packet = (iax_packet_data *)p_get_proto_data(wmem_file_scope(), pinfo, proto_iax2, 0);
  if (!iax_packet) {
    /* if not, find or create an iax_call info structure for this IAX session. */

    if (type == AST_FRAME_IAX && csub == IAX_COMMAND_NEW) {
      /* NEW packets start a new call */
      iax_call = iax_new_call(pinfo, scallno);
      reversed = false;
    } else {
      iax_call = iax_lookup_call(pinfo, scallno, dcallno,
                                 &reversed);
    }

    iax_packet = iax_new_packet_data(iax_call, reversed);
    p_add_proto_data(wmem_file_scope(), pinfo, proto_iax2, 0, iax_packet);
  } else {
    iax_call = iax_packet->call_data;
    reversed = iax_packet->reversed;
  }

  iax2_populate_pinfo_from_packet_data(pinfo, iax_packet);

  if (iax2_tree) {
    proto_item *packet_type_base;

    proto_tree_add_item(iax2_tree, hf_iax2_dcallno, tvb, offset, 2, ENC_BIG_ENDIAN);

    proto_tree_add_item(iax2_tree, hf_iax2_retransmission, tvb, offset, 2, ENC_BIG_ENDIAN);

    if (iax_call) {
      proto_item *item =
        proto_tree_add_uint(iax2_tree, hf_iax2_callno, tvb, 0, 4,
                            iax_call->forward_circuit_ids[0]);
      proto_item_set_generated(item);
    }

    proto_tree_add_uint(iax2_tree, hf_iax2_ts, tvb, offset+2, 4, ts);
    iax2_add_ts_fields(pinfo, iax2_tree, tvb, iax_packet, IAX2_FULL_PACKET, ts);

    proto_tree_add_item(iax2_tree, hf_iax2_oseqno, tvb, offset+6, 1,
                        ENC_BIG_ENDIAN);

    proto_tree_add_item(iax2_tree, hf_iax2_iseqno, tvb, offset+7, 1,
                        ENC_BIG_ENDIAN);
    packet_type_base = proto_tree_add_uint(iax2_tree, hf_iax2_type, tvb,
                                           offset+8, 1, type);

    /* add the type-specific subtree */
    packet_type_tree = proto_item_add_subtree(packet_type_base, ett_iax2_type);
  } else {
    iax2_add_ts_fields(pinfo, iax2_tree, tvb, iax_packet, IAX2_FULL_PACKET, ts);
  }


  /* add frame type to info line */
  col_add_fstr(pinfo->cinfo, COL_INFO, "%s, source call# %d, timestamp %ums",
                 val_to_str_ext(type, &iax_frame_types_ext, "Unknown (0x%02x)"),
                 scallno, ts);

  iax2_info->messageName = val_to_str_ext(type, &iax_frame_types_ext, "Unknown (0x%02x)");

  switch (type) {
  case AST_FRAME_IAX:
    offset=dissect_iax2_command(tvb, offset+9, pinfo, packet_type_tree, iax_packet);
    iax2_info->messageName = val_to_str_ext(csub, &iax_iax_subclasses_ext, "unknown (0x%02x)");
    if (csub < NUM_TAP_IAX_VOIP_STATES) iax2_info->callState = tap_iax_voip_state[csub];
    break;

  case AST_FRAME_DTMF_BEGIN:
  case AST_FRAME_DTMF_END:
    proto_tree_add_item(packet_type_tree, hf_iax2_dtmf_csub, tvb, offset+9, 1, ENC_ASCII);
    offset += 10;

    col_append_fstr(pinfo->cinfo, COL_INFO, " digit %s", format_char(pinfo->pool, csub));
    break;

  case AST_FRAME_CONTROL:
    /* add the subclass */
    proto_tree_add_uint(packet_type_tree, hf_iax2_cmd_csub, tvb,
                         offset+9, 1, csub);
    offset += 10;

    col_append_fstr(pinfo->cinfo, COL_INFO, " %s",
                      val_to_str_ext(csub, &iax_cmd_subclasses_ext, "unknown (0x%02x)"));
    iax2_info->messageName = val_to_str_ext (csub, &iax_cmd_subclasses_ext, "unknown (0x%02x)");
    if (csub < NUM_TAP_CMD_VOIP_STATES) iax2_info->callState = tap_cmd_voip_state[csub];
    break;

  case AST_FRAME_VOICE:
    /* add the codec */
    iax_packet -> codec = codec = uncompress_subclass(csub);

    if (packet_type_tree) {
      proto_item *item;
      proto_tree_add_item(packet_type_tree, hf_iax2_voice_csub, tvb, offset+9, 1, ENC_BIG_ENDIAN);
      item = proto_tree_add_uint64(packet_type_tree, hf_iax2_voice_codec, tvb, offset+9, 1, CODEC_MASK(codec));
      proto_item_set_generated(item);
    }

    offset += 10;

    if (iax_call) {
      if (reversed) {
        iax_call->dst_codec = codec;
      } else {
        iax_call->src_codec = codec;
      }
    }

    dissect_payload(tvb, offset, pinfo, iax2_tree, main_tree, ts, false, iax_packet);
    break;

  case AST_FRAME_VIDEO:
    /* bit 6 of the csub is used to represent the rtp 'marker' bit */
    rtp_marker = csub & 0x40 ? true:false;
    iax_packet -> codec = codec = uncompress_subclass((uint8_t)(csub & ~0x40));

    if (packet_type_tree) {
      proto_item *item;
      proto_tree_add_item(packet_type_tree, hf_iax2_video_csub, tvb, offset+9, 1, ENC_BIG_ENDIAN);
      proto_tree_add_item(packet_type_tree, hf_iax2_marker, tvb, offset+9, 1, ENC_BIG_ENDIAN);
      item = proto_tree_add_uint64(packet_type_tree, hf_iax2_video_codec, tvb, offset+9, 1, CODEC_MASK(codec));
      proto_item_set_generated(item);
    }

    offset += 10;

    if (iax_call && iax_packet -> first_time) {
      if (reversed) {
        iax_call->dst_vformat = codec;
      } else {
        iax_call->src_vformat = codec;
      }
    }

    if (rtp_marker)
      col_append_str(pinfo->cinfo, COL_INFO, ", Mark");


    dissect_payload(tvb, offset, pinfo, iax2_tree, main_tree, ts, true, iax_packet);
    break;

  case AST_FRAME_MODEM:
    proto_tree_add_item(packet_type_tree, hf_iax2_modem_csub, tvb, offset+9, 1, ENC_BIG_ENDIAN);
    offset += 10;

    col_append_fstr(pinfo->cinfo, COL_INFO, " %s",
                      val_to_str(csub, iax_modem_subclasses, "unknown (0x%02x)"));
    break;

  case AST_FRAME_TEXT:
    proto_tree_add_item(packet_type_tree, hf_iax2_text_csub, tvb, offset+9, 1, ENC_BIG_ENDIAN);
    offset += 10;

    {
      int textlen = tvb_captured_length_remaining(tvb, offset);
      if (textlen > 0)
      {
        proto_tree_add_item(packet_type_tree, hf_iax2_text_text, tvb, offset, textlen, ENC_UTF_8);
        offset += textlen;
      }
    }
    break;

  case AST_FRAME_HTML:
    proto_tree_add_item(packet_type_tree, hf_iax2_html_csub, tvb, offset+9, 1, ENC_BIG_ENDIAN);
    offset += 10;

    if (csub == 0x01)
    {
      int urllen = tvb_captured_length_remaining(tvb, offset);
      if (urllen > 0)
      {
        proto_item *pi = proto_tree_add_item(packet_type_tree, hf_iax2_html_url, tvb, offset, urllen, ENC_UTF_8);
        proto_item_set_url(pi);
        offset += urllen;
      }
    }
    break;

  case AST_FRAME_CNG:
  default:
    proto_tree_add_uint(packet_type_tree, hf_iax2_csub, tvb, offset+9,
                        1, csub);
    offset += 10;

    col_append_fstr(pinfo->cinfo, COL_INFO, " subclass %d", csub);
    break;
  }

  /* next time we come to parse this packet, don't propagate the codec into the
   * call_data */
  iax_packet->first_time = false;

  return offset;
}

static iax_packet_data *iax2_get_packet_data_for_minipacket(packet_info *pinfo,
                                                            uint16_t scallno,
                                                            bool video)
{
  /* see if we've seen this packet before */
  iax_packet_data *p = (iax_packet_data *)p_get_proto_data(wmem_file_scope(), pinfo, proto_iax2, 0);

  if (!p) {
    /* if not, find or create an iax_call info structure for this IAX session. */
    bool reversed;
    iax_call_data *iax_call;

    iax_call = iax_lookup_call(pinfo, scallno, 0, &reversed);

    p = iax_new_packet_data(iax_call, reversed);
    p_add_proto_data(wmem_file_scope(), pinfo, proto_iax2, 0, p);

    /* set the codec for this frame to be whatever the last full frame used */
    if (iax_call) {
     if (video)
        p->codec = reversed ? iax_call -> dst_vformat : iax_call -> src_vformat;
      else
        p->codec = reversed ? iax_call -> dst_codec : iax_call -> src_codec;
    }
  }

  iax2_populate_pinfo_from_packet_data(pinfo, p);
  return p;
}


static uint32_t dissect_minivideopacket(tvbuff_t *tvb, uint32_t offset,
                                       uint16_t scallno, packet_info *pinfo,
                                       proto_tree *iax2_tree, proto_tree *main_tree)
{
  uint32_t         ts;
  iax_packet_data *iax_packet;
  bool             rtp_marker;
  proto_item      *item;

  ts = tvb_get_ntohs(tvb, offset);

  /* bit 15 of the ts is used to represent the rtp 'marker' bit */
  rtp_marker = ts & 0x8000 ? true:false;
  ts &= ~0x8000;

  iax_packet = iax2_get_packet_data_for_minipacket(pinfo, scallno, true);

  if (iax2_tree) {
    if (iax_packet->call_data) {
      item =
        proto_tree_add_uint(iax2_tree, hf_iax2_callno, tvb, 0, 4,
                            iax_packet->call_data->forward_circuit_ids[0]);
      proto_item_set_generated(item);
    }

    proto_tree_add_item(iax2_tree, hf_iax2_minividts, tvb, offset, 2, ENC_BIG_ENDIAN);
    iax2_add_ts_fields(pinfo, iax2_tree, tvb, iax_packet, IAX2_MINI_VIDEO_PACKET, ts);
    proto_tree_add_item(iax2_tree, hf_iax2_minividmarker, tvb, offset, 2, ENC_BIG_ENDIAN);
  } else {
    iax2_add_ts_fields(pinfo, iax2_tree, tvb, iax_packet, IAX2_MINI_VIDEO_PACKET, ts);
  }

  offset += 2;

  col_add_fstr(pinfo->cinfo, COL_INFO,
                   "Mini video packet, source call# %d, timestamp %ums%s",
                   scallno, ts, rtp_marker?", Mark":"");


  dissect_payload(tvb, offset, pinfo, iax2_tree, main_tree, ts, true, iax_packet);

  /* next time we come to parse this packet, don't propagate the codec into the
   * call_data */
  iax_packet->first_time = false;

  return offset;
}

static uint32_t dissect_minipacket(tvbuff_t *tvb, uint32_t offset, uint16_t scallno,
                                  packet_info *pinfo, proto_tree *iax2_tree,
                                  proto_tree *main_tree)
{
  uint32_t         ts;
  iax_packet_data *iax_packet;
  proto_item      *item;

  ts = tvb_get_ntohs(tvb, offset);

  iax_packet = iax2_get_packet_data_for_minipacket(pinfo, scallno, false);

  if (iax2_tree) {
    if (iax_packet->call_data) {
      item = proto_tree_add_uint(iax2_tree, hf_iax2_callno, tvb, 0, 4,
                                 iax_packet->call_data->forward_circuit_ids[0]);
      proto_item_set_generated(item);
    }

    proto_tree_add_uint(iax2_tree, hf_iax2_minits, tvb, offset, 2, ts);
    iax2_add_ts_fields(pinfo, iax2_tree, tvb, iax_packet, IAX2_MINI_VOICE_PACKET, ts);
  } else {
    iax2_add_ts_fields(pinfo, iax2_tree, tvb, iax_packet, IAX2_MINI_VOICE_PACKET, ts);
  }


  offset += 2;

  col_add_fstr(pinfo->cinfo, COL_INFO,
                    "Mini packet, source call# %d, timestamp %ums",
                    scallno, ts);


  /* XXX fix the timestamp logic */
  dissect_payload(tvb, offset, pinfo, iax2_tree, main_tree, ts, false, iax_packet);


  /* next time we come to parse this packet, don't propagate the codec into the
   * call_data */
  iax_packet->first_time = false;

  return offset;
}


static uint32_t dissect_trunkcall_ts(tvbuff_t *tvb, uint32_t offset, proto_tree *iax2_tree, uint16_t *scallno)
{
  proto_tree *call_tree;
  uint16_t    datalen, rlen, ts;
  /*
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |     Data Length (in octets)   |R|     Source Call Number      |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |           time-stamp          |                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               |
   |                                       Data                    |
   :                                                               :
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   */
  datalen = tvb_get_ntohs(tvb, offset);
  *scallno = tvb_get_ntohs(tvb, offset + 2);
  ts = tvb_get_ntohs(tvb, offset + 4);

  rlen = MIN(tvb_captured_length(tvb) - offset - 6, datalen);

  if (iax2_tree) {
    call_tree = proto_tree_add_subtree_format(iax2_tree, tvb, offset, rlen + 6,
                        ett_iax2_trunk_call, NULL, "Trunk call from %u, ts: %u", *scallno, ts);

    proto_tree_add_item(call_tree, hf_iax2_trunk_call_len, tvb, offset, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(call_tree, hf_iax2_trunk_call_scallno, tvb, offset + 2, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(call_tree, hf_iax2_trunk_call_ts, tvb, offset + 4, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(call_tree, hf_iax2_trunk_call_data, tvb, offset + 6, rlen, ENC_NA);
  }
  offset += 6 + rlen;

  return offset;
}

static uint32_t dissect_trunkcall_nots(tvbuff_t *tvb, uint32_t offset, proto_tree *iax2_tree, uint16_t *scallno)
{
  proto_tree *call_tree;
  uint16_t    datalen, rlen;
  /*
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |R|      Source Call Number     |     Data Length (in octets)   |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                                                               |
   :                             Data                              :
   |                                                               |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   */
  *scallno = tvb_get_ntohs(tvb, offset);
  datalen = tvb_get_ntohs(tvb, offset + 2);

  rlen = MIN(tvb_captured_length(tvb) - offset - 4, datalen);

  if (iax2_tree) {
    call_tree = proto_tree_add_subtree_format(iax2_tree, tvb, offset, rlen + 6,
                        ett_iax2_trunk_call, NULL, "Trunk call from %u", *scallno);

    proto_tree_add_item(call_tree, hf_iax2_trunk_call_scallno, tvb, offset, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(call_tree, hf_iax2_trunk_call_len, tvb, offset + 2, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(call_tree, hf_iax2_trunk_call_data, tvb, offset + 4, rlen, ENC_NA);
  }
  offset += 4 + rlen;

  return offset;
}

typedef struct _call_list {
  uint16_t           scallno;
  struct _call_list *next;
} call_list;

static call_list *call_list_append(wmem_allocator_t *pool, call_list *list, uint16_t scallno)
{
  call_list *node = wmem_new0(pool, call_list);

  node->scallno = scallno;

  if (list) {
    call_list *cur = list;
    while (cur->next) {
      cur = cur->next;
    }
    cur->next = node;
    return list;
  } else {
    return node;
  }
}

static bool call_list_find(call_list *list, uint16_t scallno)
{
  for (; list; list = list->next) {
    if (list->scallno == scallno) {
      return true;
    }
  }
  return false;
}

static unsigned call_list_length(call_list *list)
{
  unsigned count = 0;
  for (; list; list = list->next) {
    count++;
  }
  return count;
}

static uint32_t dissect_trunkpacket(tvbuff_t *tvb, uint32_t offset,
                                   uint16_t scallno_param _U_, packet_info *pinfo,
                                   proto_tree *iax2_tree, proto_tree *main_tree _U_)
{
  uint8_t     cmddata, trunkts;
  unsigned    nframes    = 0, ncalls = 0;
  proto_item *cd, *nc    = NULL;
  proto_tree *field_tree = NULL;
  call_list  *calls      = NULL;
  /*iax_packet_data *iax_packet;*/

  cmddata = tvb_get_uint8(tvb, offset + 1);
  trunkts = cmddata & IAX2_TRUNK_TS;

  /* 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1   */
  /* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
  /* |F|         Meta Indicator      |V|Meta Command | Cmd Data (0)  | */
  /* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
  /* |                            time-stamp                         | */
  /* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */

  if (iax2_tree) {
    /* Meta Command */
    proto_tree_add_item(iax2_tree, hf_iax2_trunk_metacmd, tvb, offset, 1, ENC_BIG_ENDIAN);

    /* Command data */
    cd = proto_tree_add_uint(iax2_tree, hf_iax2_trunk_cmddata, tvb, offset + 1, 1, cmddata);
    field_tree = proto_item_add_subtree(cd, ett_iax2_trunk_cmddata);
    if (trunkts)
      proto_item_append_text(cd, " (trunk timestamps)");

    /* CD -> Trunk timestamp */
    proto_tree_add_boolean(field_tree, hf_iax2_trunk_cmddata_ts, tvb, offset + 1, 1, cmddata);

    /* Timestamp */
    proto_tree_add_item(iax2_tree, hf_iax2_trunk_ts, tvb, offset + 2, 4, ENC_BIG_ENDIAN);
  }

  offset += 6;

  if (trunkts) {
    /* Trunk calls with timestamp */
    while(tvb_captured_length_remaining(tvb, offset) >= 6) {
      uint16_t scallno;
      offset = dissect_trunkcall_ts(tvb, offset, iax2_tree, &scallno);
      if (!call_list_find(calls, scallno)) {
        calls = call_list_append(pinfo->pool, calls, scallno);
      }
      nframes++;
    }
  }
  else {
    /* Trunk calls without timestamp */
    while(tvb_captured_length_remaining(tvb, offset) >= 4) {
      uint16_t scallno;
      offset = dissect_trunkcall_nots(tvb, offset, iax2_tree, &scallno);
      if (!call_list_find(calls, scallno)) {
        calls = call_list_append(pinfo->pool, calls, scallno);
      }
      nframes++;
    }
  }

  ncalls = call_list_length(calls);

  if (iax2_tree) {
    /* number of items */
    nc = proto_tree_add_uint(iax2_tree, hf_iax2_trunk_ncalls, NULL, 0, 0, ncalls);
    proto_item_set_generated(nc);
  }

  col_add_fstr(pinfo->cinfo, COL_INFO, "Trunk packet with %d media frame%s for %d call%s",
               nframes, plurality(nframes, "", "s"),
               ncalls, plurality(ncalls, "", "s"));

  return offset;
}


static void process_iax_pdu(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree,
                              bool video, iax_packet_data *iax_packet)
{
  uint32_t       codec    = iax_packet -> codec;
  iax_call_data *iax_call = iax_packet -> call_data;

#ifdef DEBUG_DESEGMENT
  ws_debug("calling process_iax_pdu; len = %u", tvb_reported_length(tvb));
#endif

  if (!video && iax_call && iax_call->subdissector) {
    iax2_dissector_info_t dissector_info;

    /* info for subdissectors. We always pass on the original forward circuit,
     * and steal the p2p_dir flag to indicate the direction */
    if (iax_packet->call_data == NULL) {
     /* if we missed the NEW packet for this call, call_data will be null. it's
      * tbd what the best thing to do here is. */
      memset(&dissector_info, 0, sizeof(dissector_info));
    } else {
      dissector_info.ctype = CONVERSATION_IAX2;
      dissector_info.circuit_id = (uint32_t)iax_packet->call_data->forward_circuit_ids[0];
    }

    call_dissector_with_data(iax_call->subdissector, tvb, pinfo, tree, &dissector_info);
  } else if (codec != 0 && dissector_try_uint(iax2_codec_dissector_table, codec, tvb, pinfo, tree)) {
    /* codec dissector handled our data */
  } else {
    /* we don't know how to dissect our data: dissect it as data */
    call_data_dissector(tvb, pinfo, tree);
  }

#ifdef DEBUG_DESEGMENT
  ws_debug("called process_iax_pdu; pinfo->desegment_len=%u; pinfo->desegment_offset=%u",
            pinfo->desegment_len, pinfo->desegment_offset);
#endif
}

static void desegment_iax(tvbuff_t *tvb, packet_info *pinfo, proto_tree *iax2_tree,
                          proto_tree *tree, bool video, iax_packet_data *iax_packet)
{

  iax_call_data    *iax_call       = iax_packet -> call_data;
  iax_call_dirdata *dirdata;
  void *            value          = NULL;
  uint32_t          frag_offset    = 0;
  fragment_head    *fd_head;
  bool              must_desegment = false;

  DISSECTOR_ASSERT(iax_call);

  pinfo->can_desegment    = 2;
  pinfo->desegment_offset = 0;
  pinfo->desegment_len    = 0;

#ifdef DEBUG_DESEGMENT
  ws_debug("dissecting packet %u", pinfo->num);
#endif

  dirdata = &(iax_call->dirdata[!!(iax_packet->reversed)]);

  if ((!pinfo->fd->visited && (dirdata->current_frag_bytes > 0)) ||
     ((value = g_hash_table_lookup(iax_fid_table, GUINT_TO_POINTER(pinfo->num))) != NULL)) {

    /* then we are continuing an already-started pdu */
    uint32_t fid;
    uint32_t frag_len = tvb_reported_length(tvb);
    bool complete;

#ifdef DEBUG_DESEGMENT
    ws_debug("visited: %i; c_f_b: %u; hash: %u->%u", pinfo->fd->visited?1:0,
            dirdata->current_frag_bytes, pinfo->num, dirdata->current_frag_id);
#endif

    if (!pinfo->fd->visited) {
      uint32_t tot_len;
      fid = dirdata->current_frag_id;
      tot_len                      = dirdata->current_frag_minlen;
      DISSECTOR_ASSERT(g_hash_table_lookup(iax_fid_table, GUINT_TO_POINTER(pinfo->num)) == NULL);
      g_hash_table_insert(iax_fid_table, GUINT_TO_POINTER(pinfo->num), GUINT_TO_POINTER(fid));
      frag_offset                  = dirdata->current_frag_bytes;
      dirdata->current_frag_bytes += frag_len;
      complete                     = dirdata->current_frag_bytes > tot_len;
#ifdef DEBUG_DESEGMENT
      ws_debug("hash: %u->%u; frag_offset: %u; c_f_b: %u; totlen: %u",
              pinfo->num, fid, frag_offset, dirdata->current_frag_bytes, tot_len);
#endif
    } else {
      fid = GPOINTER_TO_UINT(value);
      /* these values are unused by fragment_add if pinfo->fd->visited */
      dirdata->current_frag_bytes = 0;
      complete = false;
    }

    /* fragment_add checks for already-added */
    fd_head = fragment_add(&iax_reassembly_table, tvb, 0, pinfo, fid, NULL,
                           frag_offset,
                           frag_len, !complete);

    if (fd_head && (pinfo->num == fd_head->reassembled_in)) {
      int32_t old_len;
      tvbuff_t *next_tvb = tvb_new_chain(tvb, fd_head->tvb_data);
      add_new_data_source(pinfo, next_tvb, "Reassembled IAX2");

      process_iax_pdu(next_tvb, pinfo, tree, video, iax_packet);

      /* calculate the amount of data which was available to the higher-level
         dissector before we added this segment; if the returned offset is
         within that section, the higher-level dissector was unable to find any
         pdus; if it's after that, it found one or more complete PDUs.
      */
      old_len = (int32_t)(tvb_reported_length(next_tvb) - frag_len);
      if (pinfo->desegment_len &&
          (pinfo->desegment_offset < old_len)) {
        /* oops, it wasn't actually complete */
        fragment_set_partial_reassembly(&iax_reassembly_table, pinfo, fid, NULL);
        if (pinfo->desegment_len == DESEGMENT_ONE_MORE_SEGMENT) {
          /* only one more byte should be enough for a retry */
          dirdata->current_frag_minlen = fd_head->datalen + 1;
        } else {
          dirdata->current_frag_minlen = fd_head->datalen + pinfo->desegment_len;
        }
      } else {
        /* we successfully dissected some data; create the proto tree items for
         * the fragments, and flag any remaining data for desegmentation */

        proto_item *iax_tree_item, *frag_tree_item;
        /* this nargery is to insert the fragment tree into the main tree
         * between the IAX protocol entry and the subdissector entry */
        show_fragment_tree(fd_head, &iax2_fragment_items, tree, pinfo, next_tvb, &frag_tree_item);
        iax_tree_item = proto_item_get_parent(proto_tree_get_parent(iax2_tree));
        if (frag_tree_item && iax_tree_item)
          proto_tree_move_item(tree, iax_tree_item, frag_tree_item);

        dirdata->current_frag_minlen = dirdata->current_frag_id = dirdata->current_frag_bytes = 0;

        if (pinfo->desegment_len) {
          /* there's a bit of data left to desegment */
          must_desegment = true;
          /* make desegment_offset relative to our tvb */
          pinfo->desegment_offset -= old_len;
        }

        /* don't add a 'reassembled in' item for this pdu */
        fd_head = NULL;
      }
    }
  } else {
    /* This segment was not found in our table, so it doesn't
       contain a continuation of a higher-level PDU.
       Call the normal subdissector.
    */

    process_iax_pdu(tvb, pinfo, tree, video, iax_packet);

    if (pinfo->desegment_len) {
      /* the higher-level dissector has asked for some more data - ie,
         the end of this segment does not coincide with the end of a
         higher-level PDU. */
      must_desegment = true;
    }

    fd_head = NULL;
  }

  /* must_desegment is set if the end of this segment (or the whole of it)
   * contained the start of a higher-level PDU; we must add whatever is left of
   * this segment (after pinfo->desegment_offset) to a fragment table for disassembly. */
  if (must_desegment) {
    uint32_t fid = pinfo->num; /* a new fragment id */
    uint32_t deseg_offset = pinfo->desegment_offset;
    uint32_t frag_len = tvb_reported_length_remaining(tvb, deseg_offset);
    dirdata->current_frag_id = fid;
    dirdata->current_frag_bytes = frag_len;

    if (pinfo->desegment_len == DESEGMENT_ONE_MORE_SEGMENT) {
      /* only one more byte should be enough for a retry */
      dirdata->current_frag_minlen = frag_len + 1;
    } else {
      dirdata->current_frag_minlen = frag_len + pinfo->desegment_len;
    }

    fd_head = fragment_add(&iax_reassembly_table,
                           tvb, deseg_offset, pinfo, fid, NULL,
                           0, frag_len, true);
#ifdef DEBUG_DESEGMENT
    ws_debug("Start offset of undissected bytes: %u; "
            "Bytes remaining in this segment: %u; min required bytes: %u\n",
            deseg_offset, frag_len, frag_len + pinfo->desegment_len);
#endif
  }

  /* add a 'reassembled in' item if necessary */
  if (fd_head != NULL) {
    uint32_t deseg_offset = pinfo->desegment_offset;
    if (fd_head->reassembled_in != 0 &&
        !(fd_head->flags & FD_PARTIAL_REASSEMBLY)) {
      proto_item *iax_tree_item;
      iax_tree_item = proto_tree_add_uint(tree, hf_iax2_reassembled_in,
                                          tvb, deseg_offset, tvb_reported_length_remaining(tvb, deseg_offset),
                                          fd_head->reassembled_in);
      proto_item_set_generated(iax_tree_item);
    } else {
      /* this fragment is never reassembled */
      proto_tree_add_item(tree, hf_iax2_fragment_unfinished, tvb, deseg_offset, -1, ENC_NA);
    }

    if (pinfo->desegment_offset == 0) {
      col_set_str(pinfo->cinfo, COL_PROTOCOL, "IAX2");
      col_set_str(pinfo->cinfo, COL_INFO, "[IAX2 segment of a reassembled PDU]");
    }
  }

  pinfo->can_desegment = 0;
  pinfo->desegment_offset = 0;
  pinfo->desegment_len = 0;
}

static void dissect_payload(tvbuff_t *tvb, uint32_t offset,
                            packet_info *pinfo, proto_tree *iax2_tree,
                            proto_tree *tree, uint32_t ts _U_, bool video,
                            iax_packet_data *iax_packet)
{
#if 0
  bool           out_of_order = false;
#endif
  tvbuff_t      *sub_tvb;
  uint32_t       codec        = iax_packet -> codec;
  uint32_t       nbytes;
  iax_call_data *iax_call     = iax_packet -> call_data;

  if (offset >= tvb_reported_length(tvb)) {
    col_append_str(pinfo->cinfo, COL_INFO, ", empty frame");
    return;
  }

  sub_tvb = tvb_new_subset_remaining(tvb, offset);

  /* XXX shouldn't pass through out-of-order packets. */

  if (!video && iax_call && iax_call -> dataformat != 0) {
      col_append_fstr(pinfo->cinfo, COL_INFO, ", data, format %s",
                      val_to_str(iax_call -> dataformat,
                                 iax_dataformats, "unknown (0x%02x)"));
#if 0
      if (out_of_order)
        col_append_str(pinfo->cinfo, COL_INFO, " (out-of-order packet)");
#endif
  } else {
      col_append_fstr(pinfo->cinfo, COL_INFO, ", %s",
                      val64_to_str_ext_wmem(pinfo->pool, CODEC_MASK(codec), &codec_types_ext, "unknown (0x%04x)"));
  }

  nbytes = tvb_reported_length(sub_tvb);
  proto_tree_add_item(iax2_tree, hf_iax2_payload_data, sub_tvb, 0, -1, ENC_NA);

  iax2_info->payload_len = nbytes;
  iax2_info->payload_data = tvb_get_ptr(sub_tvb, 0, -1);

  /* pass the rest of the block to a subdissector */
  if (iax_packet->call_data)
    desegment_iax(sub_tvb, pinfo, iax2_tree, tree, video, iax_packet);
  else
    process_iax_pdu(sub_tvb, pinfo, tree, video, iax_packet);
}

/*
 * Init routines
 */

/* called at the start of a capture. We should clear out our static, per-capture
 * data.
 */

static void
iax_init_protocol(void)
{
  iax_circuit_hashtab = g_hash_table_new(iax_circuit_hash, iax_circuit_equal);
  circuitcount = 0;

  iax_fid_table = g_hash_table_new(g_direct_hash, g_direct_equal);
}

static void
iax_cleanup_protocol(void)
{
  g_hash_table_destroy(iax_circuit_hashtab);
  g_hash_table_destroy(iax_fid_table);
}


void
proto_register_iax2(void)
{
  /* A header field is something you can search/filter on.
   *
   * We create a structure to register our fields. It consists of an
   * array of hf_register_info structures, each of which are of the format
   * {&(field id), {name, abbrev, type, display, strings, bitmask, blurb, HFILL}}.
   */

  static hf_register_info hf[] = {

    {&hf_iax2_packet_type,
     {"Packet type", "iax2.packet_type",
      FT_UINT8, BASE_DEC, VALS(iax_packet_types), 0,
      "Full/minivoice/minivideo/trunk packet",
      HFILL}},

    {&hf_iax2_callno,
     {"Call identifier", "iax2.call",
      FT_UINT32, BASE_DEC, NULL, 0,
      "This is the identifier Wireshark assigns to identify this call."
      " It does not correspond to any real field in the protocol",
      HFILL }},

    {&hf_iax2_scallno,
     {"Source call", "iax2.src_call",
      FT_UINT16, BASE_DEC, NULL, 0x7FFF,
      "src_call holds the number of this call at the packet source pbx",
      HFILL}},

    /* FIXME could this be turned into a FRAMENUM field? */
    {&hf_iax2_dcallno,
     {"Destination call", "iax2.dst_call",
      FT_UINT16, BASE_DEC, NULL, 0x7FFF,
      "dst_call holds the number of this call at the packet destination",
      HFILL}},

    {&hf_iax2_retransmission,
     {"Retransmission", "iax2.retransmission",
      FT_BOOLEAN, 16, NULL, 0x8000,
      "retransmission is set if this packet is a retransmission of an earlier failed packet",
      HFILL}},

    {&hf_iax2_ts,
     {"Timestamp", "iax2.timestamp",
      FT_UINT32, BASE_DEC, NULL, 0x0,
      "timestamp is the time, in ms after the start of this call, at which this packet was transmitted",
      HFILL}},

    {&hf_iax2_minits,
     {"Timestamp", "iax2.timestamp",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      "timestamp is the time, in ms after the start of this call, at which this packet was transmitted",
      HFILL}},

    {&hf_iax2_minividts,
     {"Timestamp", "iax2.timestamp",
      FT_UINT16, BASE_DEC, NULL, 0x7FFF,
      "timestamp is the time, in ms after the start of this call, at which this packet was transmitted",
      HFILL}},

    {&hf_iax2_absts,
     {"Absolute Time", "iax2.abstime",
      FT_ABSOLUTE_TIME, ABSOLUTE_TIME_LOCAL, NULL, 0x0,
      "The absolute time of this packet (calculated by adding the IAX timestamp to  the start time of this call)",
      HFILL}},

    {&hf_iax2_lateness,
     {"Lateness", "iax2.lateness",
      FT_RELATIVE_TIME, BASE_NONE, NULL, 0x0,
      "The lateness of this packet compared to its timestamp",
      HFILL}},

    {&hf_iax2_minividmarker,
     {"Marker", "iax2.video.mini_marker",
      FT_UINT16, BASE_DEC, NULL, 0x8000,
      "RTP end-of-frame marker",
      HFILL}},

    {&hf_iax2_oseqno,
     {"Outbound seq.no.", "iax2.oseqno",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      "oseqno is the sequence no of this packet. The first packet has oseqno==0,"
      " and subsequent packets increment the oseqno by 1",
      HFILL}},

    {&hf_iax2_iseqno,
     {"Inbound seq.no.", "iax2.iseqno",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      "iseqno is the sequence no of the last successfully received packet",
      HFILL}},

    {&hf_iax2_type,
     {"Type", "iax2.type",
      FT_UINT8, BASE_DEC | BASE_EXT_STRING, &iax_frame_types_ext, 0x0,
      "For full IAX2 frames, type is the type of frame",
      HFILL}},

    {&hf_iax2_csub,
     {"Unknown subclass", "iax2.subclass",
      FT_UINT8, BASE_DEC, NULL, 0x0,
      "Subclass of unknown type of full IAX2 frame",
      HFILL}},

    {&hf_iax2_dtmf_csub,
     {"DTMF subclass (digit)", "iax2.dtmf.subclass",
      FT_STRINGZ, BASE_NONE, NULL, 0x0,
      "DTMF subclass gives the DTMF digit",
      HFILL}},

    {&hf_iax2_cmd_csub,
     {"Control subclass", "iax2.control.subclass",
      FT_UINT8, BASE_DEC | BASE_EXT_STRING, &iax_cmd_subclasses_ext, 0x0,
      "This gives the command number for a Control packet.",
      HFILL}},

    {&hf_iax2_iax_csub,
     {"IAX subclass", "iax2.iax.subclass",
      FT_UINT8, BASE_DEC | BASE_EXT_STRING, &iax_iax_subclasses_ext, 0x0,
      "IAX subclass gives the command number for IAX signaling packets",
      HFILL}},

    {&hf_iax2_voice_csub,
     {"Voice Subclass (compressed codec no)", "iax2.voice.subclass",
      FT_UINT8, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_voice_codec,
     {"CODEC", "iax2.voice.codec",
      FT_UINT64, BASE_HEX | BASE_EXT_STRING | BASE_VAL64_STRING, &codec_types_ext, 0x0,
      "CODEC gives the codec used to encode audio data",
      HFILL}},

    {&hf_iax2_video_csub,
     {"Video Subclass (compressed codec no)", "iax2.video.subclass",
      FT_UINT8, BASE_DEC, NULL, 0xBF,
      NULL, HFILL}},

    {&hf_iax2_marker,
     {"Marker", "iax2.video.marker",
      FT_BOOLEAN, 8, NULL, 0x40,
      "RTP end-of-frame marker",
      HFILL}},

    {&hf_iax2_video_codec,
     {"CODEC", "iax2.video.codec",
      FT_UINT64, BASE_HEX | BASE_EXT_STRING | BASE_VAL64_STRING, &codec_types_ext, 0,
      "The codec used to encode video data",
      HFILL}},

    {&hf_iax2_modem_csub,
     {"Modem subclass", "iax2.modem.subclass",
      FT_UINT8, BASE_DEC, VALS(iax_modem_subclasses), 0x0,
      "Modem subclass gives the type of modem",
      HFILL}},

    {&hf_iax2_text_csub,
     {"Text subclass", "iax2.text.subclass",
      FT_UINT8, BASE_DEC, VALS(iax_text_subclasses), 0x0,
      NULL,
      HFILL}},

    {&hf_iax2_text_text,
     {"Text", "iax2.text.text",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL,
      HFILL}},

    {&hf_iax2_html_csub,
     {"HTML subclass", "iax2.html.subclass",
      FT_UINT8, BASE_DEC, VALS(iax_html_subclasses), 0x0,
      NULL,
      HFILL}},

    {&hf_iax2_html_url,
     {"HTML URL", "iax2.html.url",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL,
      HFILL}},

    {&hf_iax2_trunk_ts,
     {"Timestamp", "iax2.timestamp",
      FT_UINT32, BASE_DEC, NULL, 0x0,
      "timestamp is the time, in ms after the start of Command data this call,"
      " at which this trunk packet was transmitted",
      HFILL}},

    {&hf_iax2_trunk_metacmd,
     {"Meta command", "iax2.trunk.metacmd",
      FT_UINT8, BASE_DEC, NULL, 0x7F,
      "Meta command indicates whether or not the Meta Frame is a trunk.",
      HFILL}},

    {&hf_iax2_trunk_cmddata,
     {"Command data", "iax2.trunk.cmddata",
      FT_UINT8, BASE_HEX, NULL, 0x0,
      "Flags for options that apply to a trunked call",
      HFILL}},

    {&hf_iax2_trunk_cmddata_ts,
     {"Trunk timestamps", "iax2.trunk.cmddata.ts",
      FT_BOOLEAN, 8, NULL, IAX2_TRUNK_TS,
      "True: calls do each include their own timestamp",
      HFILL}},

    {&hf_iax2_trunk_call_len,
     {"Data length", "iax2.trunk.call.len",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      "Trunk call data length in octets",
      HFILL}},

    {&hf_iax2_trunk_call_scallno,
     {"Source call number", "iax2.trunk.call.scallno",
      FT_UINT16, BASE_DEC, NULL, 0x7FFF,
      "Trunk call source call number",
      HFILL}},

    {&hf_iax2_trunk_call_ts,
     {"Timestamp", "iax2.trunk.call.ts",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      "timestamp is the time, in ms after the start of this call, at which this packet was transmitted",
      HFILL}},

    {&hf_iax2_trunk_call_data,
     {"Data", "iax2.trunk.call.payload",
      FT_BYTES, BASE_NONE, NULL, 0x0,
      "Payload carried by this trunked packet.",
      HFILL}},

    {&hf_iax2_trunk_ncalls,
     {"Number of calls", "iax2.trunk.ncalls",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      "Number of calls in this trunk packet",
      HFILL}},

    /*
     * Decoding for the ies
     */

    {&hf_IAX_IE_APPARENTADDR_SINFAMILY,
     {"Family", "iax2.iax.app_addr.sinfamily",
      FT_UINT16, BASE_DEC, NULL, 0,
      NULL, HFILL }},

    {&hf_IAX_IE_APPARENTADDR_SINPORT,
     {"Port", "iax2.iax.app_addr.sinport",
      FT_UINT16, BASE_DEC, NULL, 0,
      NULL, HFILL }},

    {&hf_IAX_IE_APPARENTADDR_SINADDR,
     {"Address", "iax2.iax.app_addr.sinaddr",
      FT_IPv4, BASE_NONE, NULL, 0,
      NULL, HFILL }},

    {&hf_iax2_ies[IAX_IE_CALLED_NUMBER],
     {"Number/extension being called", "iax2.iax.called_number",
      FT_STRING,
      BASE_NONE, NULL, 0x0, NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLING_NUMBER],
     {"Calling number", "iax2.iax.calling_number",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},


    {&hf_iax2_ies[IAX_IE_CALLING_ANI],
     {"Calling number ANI for billing", "iax2.iax.calling_ani",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLING_NAME],
     {"Name of caller", "iax2.iax.calling_name",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLED_CONTEXT],
     {"Context for number", "iax2.iax.called_context",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_USERNAME],
     {"Username (peer or user) for authentication", "iax2.iax.username",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_PASSWORD],
     {"Password for authentication", "iax2.iax.password",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CAPABILITY],
     {"Actual codec capability", "iax2.iax.capability",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_FORMAT],
     {"Desired codec format", "iax2.iax.format",
      FT_UINT64, BASE_HEX | BASE_EXT_STRING | BASE_VAL64_STRING, &codec_types_ext, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_LANGUAGE],
     {"Desired language", "iax2.iax.language",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_VERSION],
     {"Protocol version", "iax2.iax.version",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_ADSICPE],
     {"CPE ADSI capability", "iax2.iax.cpe_adsi",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_DNID],
     {"Originally dialed DNID", "iax2.iax.dnid",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_AUTHMETHODS],
     {"Authentication method(s)", "iax2.iax.auth.methods",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CHALLENGE],
     {"Challenge data for MD5/RSA", "iax2.iax.auth.challenge",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_MD5_RESULT],
     {"MD5 challenge result", "iax2.iax.auth.md5",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RSA_RESULT],
     {"RSA challenge result", "iax2.iax.auth.rsa",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_REFRESH],
     {"When to refresh registration", "iax2.iax.refresh",
      FT_INT16, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_DPSTATUS],
     {"Dialplan status", "iax2.iax.dialplan_status",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLNO],
     {"Call number of peer", "iax2.iax.call_no",
      FT_UINT16, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CAUSE],
     {"Cause", "iax2.iax.cause",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_IAX_UNKNOWN],
     {"Unknown IAX command", "iax2.iax.iax_unknown",
      FT_BYTES, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_MSGCOUNT],
     {"How many messages waiting", "iax2.iax.msg_count",
      FT_INT16, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_AUTOANSWER],
     {"Request auto-answering", "iax2.iax.autoanswer",
      FT_NONE, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_MUSICONHOLD],
     {"Request musiconhold with QUELCH", "iax2.iax.moh",
      FT_NONE, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_TRANSFERID],
     {"Transfer Request Identifier", "iax2.iax.transferid",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RDNIS],
     {"Referring DNIS", "iax2.iax.rdnis",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_PROVISIONING],
     {"Provisioning info", "iax2.iax.provisioning",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_AESPROVISIONING],
     {"AES Provisioning info", "iax2.iax.aesprovisioning",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_DATETIME],
     {"Date/Time", "iax2.iax.datetime.raw",
      FT_UINT32, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ie_datetime,
     {"Date/Time", "iax2.iax.datetime",
      FT_ABSOLUTE_TIME, ABSOLUTE_TIME_LOCAL, NULL, 0x0,
      NULL, HFILL }},

    {&hf_iax2_ies[IAX_IE_DEVICETYPE],
     {"Device type", "iax2.iax.devicetype",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_SERVICEIDENT],
     {"Service identifier", "iax2.iax.serviceident",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_FIRMWAREVER],
     {"Firmware version", "iax2.iax.firmwarever",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_FWBLOCKDESC],
     {"Firmware block description", "iax2.iax.fwblockdesc",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_FWBLOCKDATA],
     {"Firmware block of data", "iax2.iax.fwblockdata",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_PROVVER],
     {"Provisioning version", "iax2.iax.provver",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLINGPRES],
     {"Calling presentation", "iax2.iax.callingpres",
      FT_UINT8, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLINGTON],
     {"Calling type of number", "iax2.iax.callington",
      FT_UINT8, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CALLINGTNS],
     {"Calling transit network select", "iax2.iax.callingtns",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_SAMPLINGRATE],
     {"Supported sampling rates", "iax2.iax.samplingrate",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CAUSECODE],
     {"Hangup cause", "iax2.iax.causecode",
      FT_UINT8, BASE_HEX | BASE_EXT_STRING, &iax_causecodes_ext, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_ENCRYPTION],
     {"Encryption format", "iax2.iax.encryption",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_ENCKEY],
     {"Encryption key", "iax2.iax.enckey",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CODEC_PREFS],
     {"Codec negotiation", "iax2.iax.codecprefs",
      FT_STRING, BASE_NONE, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RR_JITTER],
     {"Received jitter (as in RFC1889)", "iax2.iax.rrjitter",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RR_LOSS],
     {"Received loss (high byte loss pct, low 24 bits loss count, as in rfc1889)", "iax2.iax.rrloss",
       FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RR_PKTS],
     {"Total frames received", "iax2.iax.rrpkts",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RR_DELAY],
     {"Max playout delay in ms for received frames", "iax2.iax.rrdelay",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RR_DROPPED],
     {"Dropped frames (presumably by jitterbuffer)", "iax2.iax.rrdropped",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_RR_OOO],
     {"Frame received out of order", "iax2.iax.rrooo",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_CAPABILITY2],
     {"64-bit codec capability", "iax2.iax.capability2",
      FT_UINT64, BASE_HEX, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_FORMAT2],
     {"64-bit codec format", "iax2.iax.format2",
      FT_UINT64, BASE_HEX | BASE_EXT_STRING | BASE_VAL64_STRING, &codec_types_ext, 0x0,
      NULL, HFILL}},

    {&hf_iax2_ies[IAX_IE_DATAFORMAT],
     {"Data call format", "iax2.iax.dataformat",
      FT_UINT32, BASE_HEX, VALS(iax_dataformats), 0x0,
      NULL, HFILL}},

    {&hf_IAX_IE_UNKNOWN_BYTE,
     {"Unknown", "iax2.iax.unknownbyte",
      FT_UINT8, BASE_HEX, NULL, 0x0,
      "Raw data for unknown IEs", HFILL}},

    {&hf_IAX_IE_UNKNOWN_I16,
     {"Unknown", "iax2.iax.unknownshort",
      FT_UINT16, BASE_HEX, NULL, 0x0,
      "Raw data for unknown IEs", HFILL}},

    {&hf_IAX_IE_UNKNOWN_I32,
     {"Unknown", "iax2.iax.unknownlong",
      FT_UINT32, BASE_HEX, NULL, 0x0,
      "Raw data for unknown IEs", HFILL}},

    {&hf_IAX_IE_UNKNOWN_BYTES,
     {"Unknown", "iax2.iax.unknownstring",
      FT_STRING, BASE_NONE, NULL, 0x0,
      "Raw data for unknown IEs", HFILL}},

    {&hf_iax2_ie_id,
     {"IE id", "iax2.ie_id",
      FT_UINT8, BASE_DEC|BASE_EXT_STRING, &iax_ies_type_ext, 0x0,
      NULL, HFILL}},

    {&hf_iax2_length,
     {"Length", "iax2.length",
      FT_UINT8, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    {&hf_iax2_version,
     {"Version", "iax2.version",
      FT_UINT8, BASE_DEC, NULL, 0x0,
      NULL, HFILL}},

    /* capabilities */
    {&hf_iax2_cap_g723_1,
     {"G.723.1 compression", "iax2.cap.g723_1",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_G723_1),
      NULL, HFILL }},

    {&hf_iax2_cap_gsm,
     {"GSM compression", "iax2.cap.gsm",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_GSM),
      NULL, HFILL }},

    {&hf_iax2_cap_ulaw,
     {"Raw mu-law data (G.711)", "iax2.cap.ulaw",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_ULAW),
      NULL, HFILL }},

     {&hf_iax2_cap_alaw,
      {"Raw A-law data (G.711)", "iax2.cap.alaw",
       FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_ALAW),
       NULL, HFILL } },

    {&hf_iax2_cap_g726_aal2,
     {"ADPCM (G.726, 32kbps, AAL2 codeword packing)", "iax2.cap.g726_aal2",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_G726_AAL2),
      NULL, HFILL }},

    {&hf_iax2_cap_adpcm,
     {"ADPCM", "iax2.cap.adpcm",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_ADPCM),
      NULL, HFILL }},

    {&hf_iax2_cap_slinear,
     {"Raw 16-bit Signed Linear (8000 Hz) PCM", "iax2.cap.slinear",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_SLINEAR),
      NULL, HFILL }},

    {&hf_iax2_cap_lpc10,
     {"LPC10, 180 samples/frame", "iax2.cap.lpc10",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_LPC10),
      NULL, HFILL }},

    {&hf_iax2_cap_g729a,
     {"G.729a Audio", "iax2.cap.g729a",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_G729A),
      NULL, HFILL }},

    {&hf_iax2_cap_speex,
     {"SpeeX Free Compression", "iax2.cap.speex",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_SPEEX),
      NULL, HFILL }},

    {&hf_iax2_cap_ilbc,
     {"iLBC Free Compression", "iax2.cap.ilbc",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_ILBC),
      NULL, HFILL }},

    {&hf_iax2_cap_g726,
     {"ADPCM (G.726, 32kbps, RFC3551 codeword packing)", "iax2.cap.g726",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_G726),
      NULL, HFILL }},

    {&hf_iax2_cap_g722,
     {"G.722", "iax2.cap.g722",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_G722),
      NULL, HFILL }},

    {&hf_iax2_cap_siren7,
     {"G.722.1 (also known as Siren7, 32kbps assumed)", "iax2.cap.siren7",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_SIREN7),
      NULL, HFILL }},

    {&hf_iax2_cap_siren14,
     {"G.722.1 Annex C (also known as Siren14, 48kbps assumed)", "iax2.cap.siren14",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_SIREN14),
      NULL, HFILL }},

    {&hf_iax2_cap_slinear16,
     {"Raw 16-bit Signed Linear (16000 Hz) PCM", "iax2.cap.slinear16",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_SLINEAR16),
      NULL, HFILL }},

    {&hf_iax2_cap_jpeg,
     {"JPEG images", "iax2.cap.jpeg",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_JPEG),
      NULL, HFILL }},

    {&hf_iax2_cap_png,
     {"PNG images", "iax2.cap.png",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_PNG),
      NULL, HFILL }},

    {&hf_iax2_cap_h261,
     {"H.261 video", "iax2.cap.h261",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_H261),
      NULL, HFILL }},

    {&hf_iax2_cap_h263,
     {"H.263 video", "iax2.cap.h263",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_H263),
      NULL, HFILL }},

    {&hf_iax2_cap_h263_plus,
     {"H.263+ video", "iax2.cap.h263_plus",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_H263_PLUS),
      NULL, HFILL }},

    {&hf_iax2_cap_h264,
     {"H.264 video", "iax2.cap.h264",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_H264),
      NULL, HFILL }},

    {&hf_iax2_cap_mpeg4,
     {"MPEG4 video", "iax2.cap.mpeg4",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_MP4_VIDEO),
      NULL, HFILL }},

    {&hf_iax2_cap_vp8,
     {"VP8 video", "iax2.cap.vp8",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_VP8),
      NULL, HFILL }},

    {&hf_iax2_cap_t140_red,
     {"T.140 RED Text format RFC 4103", "iax2.cap.t140_red",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_T140_RED),
      NULL, HFILL }},

    {&hf_iax2_cap_t140,
     {"T.140 Text format - ITU T.140, RFC 4103", "iax2.cap.t140",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_T140),
      NULL, HFILL }},

    {&hf_iax2_cap_g719,
     {"G.719 (64 kbps assumed)", "iax2.cap.g719",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_G719),
      NULL, HFILL }},

    {&hf_iax2_cap_speex16,
     {"SpeeX Wideband (16kHz) Free Compression", "iax2.cap.speex16",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_SPEEX16),
      NULL, HFILL }},

    {&hf_iax2_cap_opus,
     {"Opus audio (8kHz, 16kHz, 24kHz, 48Khz)", "iax2.cap.opus",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_OPUS),
      NULL, HFILL }},

    {&hf_iax2_cap_testlaw,
     {"Raw testing-law data (G.711)", "iax2.cap.testlaw",
      FT_BOOLEAN, 64, TFS(&tfs_supported_not_supported), CODEC_MASK(AST_FORMAT_TESTLAW),
      NULL, HFILL }},

    {&hf_iax2_fragment_unfinished,
     {"IAX2 fragment, unfinished", "iax2.fragment_unfinished",
      FT_BYTES, BASE_NONE, NULL, 0x0,
      NULL, HFILL }},

    {&hf_iax2_payload_data,
     {"IAX2 payload", "iax2.payload_data",
      FT_BYTES, BASE_NONE, NULL, 0x0,
      NULL, HFILL }},

    /* reassembly stuff */
    {&hf_iax2_fragments,
     {"IAX2 Fragments", "iax2.fragments",
      FT_NONE, BASE_NONE, NULL, 0x0,
      NULL, HFILL }},

    {&hf_iax2_fragment,
     {"IAX2 Fragment data", "iax2.fragment",
      FT_FRAMENUM, BASE_NONE, NULL, 0x0,
      NULL, HFILL }},

    {&hf_iax2_fragment_overlap,
     {"Fragment overlap", "iax2.fragment.overlap",
      FT_BOOLEAN, BASE_NONE, NULL, 0x0,
      "Fragment overlaps with other fragments", HFILL }},

    {&hf_iax2_fragment_overlap_conflict,
     {"Conflicting data in fragment overlap", "iax2.fragment.overlap.conflict",
      FT_BOOLEAN, BASE_NONE, NULL, 0x0,
      "Overlapping fragments contained conflicting data", HFILL }},

    {&hf_iax2_fragment_multiple_tails,
     {"Multiple tail fragments found", "iax2.fragment.multipletails",
      FT_BOOLEAN, BASE_NONE, NULL, 0x0,
      "Several tails were found when defragmenting the packet", HFILL }},

    {&hf_iax2_fragment_too_long_fragment,
     {"Fragment too long", "iax2.fragment.toolongfragment",
      FT_BOOLEAN, BASE_NONE, NULL, 0x0,
      "Fragment contained data past end of packet", HFILL }},

    {&hf_iax2_fragment_error,
     {"Defragmentation error", "iax2.fragment.error",
      FT_FRAMENUM, BASE_NONE, NULL, 0x0,
      "Defragmentation error due to illegal fragments", HFILL }},

    {&hf_iax2_fragment_count,
     {"Fragment count", "iax2.fragment.count",
      FT_UINT32, BASE_DEC, NULL, 0x0,
      NULL, HFILL }},

    {&hf_iax2_reassembled_in,
     {"IAX2 fragment, reassembled in frame", "iax2.reassembled_in",
      FT_FRAMENUM, BASE_NONE, NULL, 0x0,
      "This IAX2 packet is reassembled in this frame", HFILL }},

    {&hf_iax2_reassembled_length,
     {"Reassembled IAX2 length", "iax2.reassembled.length",
      FT_UINT32, BASE_DEC, NULL, 0x0,
      "The total length of the reassembled payload", HFILL }}
  };

  static int *ett[] = {
    &ett_iax2,
    &ett_iax2_full_mini_subtree,
    &ett_iax2_type,
    &ett_iax2_ie,
    &ett_iax2_codecs,
    &ett_iax2_ies_apparent_addr,
    &ett_iax2_fragment,
    &ett_iax2_fragments,
    &ett_iax2_trunk_cmddata,
    &ett_iax2_trunk_call
  };

  static ei_register_info ei[] = {
    { &ei_iax_too_many_transfers, { "iax2.too_many_transfers", PI_PROTOCOL, PI_WARN, "Too many transfers for iax_call", EXPFILL }},
    { &ei_iax_circuit_id_conflict, { "iax2.circuit_id_conflict", PI_PROTOCOL, PI_WARN, "Circuit ID conflict", EXPFILL }},
    { &ei_iax_peer_address_unsupported, { "iax2.peer_address_unsupported", PI_PROTOCOL, PI_WARN, "Peer address unsupported", EXPFILL }},
    { &ei_iax_invalid_len, { "iax2.invalid_len", PI_PROTOCOL, PI_WARN, "Invalid length", EXPFILL }},
  };

  expert_module_t* expert_iax;

  proto_iax2 = proto_register_protocol("Inter-Asterisk eXchange v2", "IAX2", "iax2");
  proto_register_field_array(proto_iax2, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));
  expert_iax = expert_register_protocol(proto_iax2);
  expert_register_field_array(expert_iax, ei, array_length(ei));

  iax2_handle = register_dissector("iax2", dissect_iax2, proto_iax2);

  iax2_codec_dissector_table = register_dissector_table(
    "iax2.codec", "IAX codec number", proto_iax2, FT_UINT32, BASE_HEX);
  iax2_dataformat_dissector_table = register_dissector_table(
    "iax2.dataformat", "IAX dataformat number", proto_iax2, FT_UINT32, BASE_HEX);

  /* register our init routine to be called at the start of a capture,
     to clear out our hash tables etc */
  register_init_routine(&iax_init_protocol);
  register_cleanup_routine(&iax_cleanup_protocol);
  reassembly_table_register(&iax_reassembly_table,
                        &addresses_reassembly_table_functions);

  iax2_tap = register_tap("IAX2");
}

void
proto_reg_handoff_iax2(void)
{
  dissector_handle_t v110_handle;

  dissector_add_uint_with_preference("udp.port", IAX2_PORT, iax2_handle);
  v110_handle =  find_dissector("v110");
  if (v110_handle)
    dissector_add_uint("iax2.dataformat", AST_DATAFORMAT_V110, v110_handle);
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 2
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=2 tabstop=8 expandtab:
 * :indentSize=2:tabSize=8:noTabs=true:
 */
