/* packet-amqp.c
 *
 * AMQP 0-9, 0-9-1, 0-10 and AMQP 1.0 Wireshark dissector
 *
 * Author: Martin Sustrik <sustrik@imatix.com> (AMQP 0-9)
 * Author: Steve Huston <shuston@riverace.com> (extended for AMQP 0-10)
 * Author: Pavel Moravec <pmoravec@redhat.com> (extended for AMQP 1.0)
 *
 * Copyright (c) 1996-2007 iMatix Corporation
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * See
 *     http://www.amqp.org/resources/download
 *     http://www.rabbitmq.com/protocol.html
 *
 * for specifications for various versions of the AMQP protocol.
 */

#include "config.h"

#include <math.h>

#include <epan/packet.h>
#include <epan/expert.h>
#include <epan/prefs.h>
#include <epan/decode_as.h>
#include <epan/to_str.h>
#include <epan/proto_data.h>
#include <epan/tfs.h>
#include <wsutil/str_util.h>
#include <epan/uat.h>
#include "packet-tcp.h"
#include "packet-tls.h"


void proto_register_amqp(void);
void proto_reg_handoff_amqp(void);
/*  Generic data  */

#define AMQP_PORT   5672
static unsigned amqps_port = 5671; /* AMQP over SSL/TLS */

/*
 * This dissector handles AMQP 0-9, 0-10 and 1.0. The conversation structure
 * contains the version being run - it's only really reliably detected at
 * protocol init. If this dissector starts in the middle of a conversation
 * it will try to figure it out, but conversation start is the best.
 */

/* #define AMQP_V0_8           1 */
#define AMQP_V0_9           2
/* #define AMQP_V0_91          3 */
#define AMQP_V0_10          4
#define AMQP_V1_0           5
typedef struct {
    uint8_t version;
    wmem_map_t *channels; /* maps channel_num to amqp_channel_t */
} amqp_conv;

static dissector_table_t version_table;
static dissector_table_t media_type_subdissector_table;

struct amqp_delivery;
typedef struct amqp_delivery amqp_delivery;

struct amqp_delivery {
    uint64_t delivery_tag;          /* message number or delivery tag */
    uint32_t msg_framenum;          /* basic.publish or basic.deliver frame */
    uint32_t ack_framenum;          /* basic.ack or basic.nack frame */
    amqp_delivery *prev;
};

typedef struct {
    char *type;        /* content type */
    char *encoding;    /* content encoding. Not used in subdissector now */
} amqp_content_params;

typedef struct _amqp_channel_t {
    amqp_conv *conn;
    bool confirms;                   /* true if publisher confirms are enabled */
    uint16_t channel_num;                 /* channel number */
    uint64_t publish_count;               /* number of messages published so far */
    amqp_delivery *last_delivery1;       /* list of unacked messages on tcp flow1 */
    amqp_delivery *last_delivery2;       /* list of unacked messages on tcp flow2 */
    amqp_content_params *content_params; /* parameters of content */
} amqp_channel_t;

typedef struct _amqp_message_decode_t {
    uint32_t match_criteria;
    char *topic_pattern;
    GRegex *topic_regex;
    char *payload_proto_name;
    dissector_handle_t payload_proto;
    char *topic_more_info;
} amqp_message_decode_t;

#define MATCH_CRITERIA_EQUAL        0
#define MATCH_CRITERIA_CONTAINS     1
#define MATCH_CRITERIA_STARTS_WITH  2
#define MATCH_CRITERIA_ENDS_WITH    3
#define MATCH_CRITERIA_REGEX        4

static const value_string match_criteria[] = {
    { MATCH_CRITERIA_EQUAL,       "Equal to" },
    { MATCH_CRITERIA_CONTAINS,    "Contains" },
    { MATCH_CRITERIA_STARTS_WITH, "Starts with" },
    { MATCH_CRITERIA_ENDS_WITH,   "Ends with" },
    { MATCH_CRITERIA_REGEX,       "Regular Expression" },
    { 0, NULL }
};

#define MAX_BUFFER 256

/* 0-9 and 0-9-1 defines */

#define AMQP_0_9_FRAME_TYPE_METHOD                                      1
#define AMQP_0_9_FRAME_TYPE_CONTENT_HEADER                              2
#define AMQP_0_9_FRAME_TYPE_CONTENT_BODY                                3
#define AMQP_0_9_FRAME_TYPE_OOB_METHOD                                  4
#define AMQP_0_9_FRAME_TYPE_OOB_CONTENT_HEADER                          5
#define AMQP_0_9_FRAME_TYPE_OOB_CONTENT_BODY                            6
#define AMQP_0_9_FRAME_TYPE_TRACE                                       7
#define AMQP_0_9_FRAME_TYPE_HEARTBEAT                                   8

#define AMQP_0_9_CLASS_CONNECTION                                      10
#define AMQP_0_9_CLASS_CHANNEL                                         20
#define AMQP_0_9_CLASS_ACCESS                                          30
#define AMQP_0_9_CLASS_EXCHANGE                                        40
#define AMQP_0_9_CLASS_QUEUE                                           50
#define AMQP_0_9_CLASS_BASIC                                           60
#define AMQP_0_9_CLASS_FILE                                            70
#define AMQP_0_9_CLASS_STREAM                                          80
#define AMQP_0_9_CLASS_TX                                              90
#define AMQP_0_9_CLASS_DTX                                            100
#define AMQP_0_9_CLASS_TUNNEL                                         110
#define AMQP_0_9_CLASS_CONFIRM                                         85

#define AMQP_0_9_METHOD_CONNECTION_START                               10
#define AMQP_0_9_METHOD_CONNECTION_START_OK                            11
#define AMQP_0_9_METHOD_CONNECTION_SECURE                              20
#define AMQP_0_9_METHOD_CONNECTION_SECURE_OK                           21
#define AMQP_0_9_METHOD_CONNECTION_TUNE                                30
#define AMQP_0_9_METHOD_CONNECTION_TUNE_OK                             31
#define AMQP_0_9_METHOD_CONNECTION_OPEN                                40
#define AMQP_0_9_METHOD_CONNECTION_OPEN_OK                             41
#define AMQP_0_9_METHOD_CONNECTION_REDIRECT                            42
#define AMQP_0_9_METHOD_CONNECTION_CLOSE                               50
#define AMQP_0_9_METHOD_CONNECTION_CLOSE_OK                            51
#define AMQP_0_9_METHOD_CONNECTION_BLOCKED                             60
#define AMQP_0_9_METHOD_CONNECTION_UNBLOCKED                           61

#define AMQP_0_9_METHOD_CHANNEL_OPEN                                   10
#define AMQP_0_9_METHOD_CHANNEL_OPEN_OK                                11
#define AMQP_0_9_METHOD_CHANNEL_FLOW                                   20
#define AMQP_0_9_METHOD_CHANNEL_FLOW_OK                                21
#define AMQP_0_9_METHOD_CHANNEL_CLOSE                                  40
#define AMQP_0_9_METHOD_CHANNEL_CLOSE_OK                               41
#define AMQP_0_9_METHOD_CHANNEL_RESUME                                 50
#define AMQP_0_9_METHOD_CHANNEL_PING                                   60
#define AMQP_0_9_METHOD_CHANNEL_PONG                                   70
#define AMQP_0_9_METHOD_CHANNEL_OK                                     80

#define AMQP_0_9_METHOD_ACCESS_REQUEST                                 10
#define AMQP_0_9_METHOD_ACCESS_REQUEST_OK                              11

#define AMQP_0_9_METHOD_EXCHANGE_DECLARE                               10
#define AMQP_0_9_METHOD_EXCHANGE_DECLARE_OK                            11
#define AMQP_0_9_METHOD_EXCHANGE_DELETE                                20
#define AMQP_0_9_METHOD_EXCHANGE_DELETE_OK                             21
#define AMQP_0_9_METHOD_EXCHANGE_BIND                                  30
#define AMQP_0_9_METHOD_EXCHANGE_BIND_OK                               31
#define AMQP_0_9_METHOD_EXCHANGE_UNBIND                                40
#define AMQP_0_9_METHOD_EXCHANGE_UNBIND_OK                             51

#define AMQP_0_9_METHOD_QUEUE_DECLARE                                  10
#define AMQP_0_9_METHOD_QUEUE_DECLARE_OK                               11
#define AMQP_0_9_METHOD_QUEUE_BIND                                     20
#define AMQP_0_9_METHOD_QUEUE_BIND_OK                                  21
#define AMQP_0_9_METHOD_QUEUE_UNBIND                                   50
#define AMQP_0_9_METHOD_QUEUE_UNBIND_OK                                51
#define AMQP_0_9_METHOD_QUEUE_PURGE                                    30
#define AMQP_0_9_METHOD_QUEUE_PURGE_OK                                 31
#define AMQP_0_9_METHOD_QUEUE_DELETE                                   40
#define AMQP_0_9_METHOD_QUEUE_DELETE_OK                                41

#define AMQP_0_9_METHOD_BASIC_QOS                                      10
#define AMQP_0_9_METHOD_BASIC_QOS_OK                                   11
#define AMQP_0_9_METHOD_BASIC_CONSUME                                  20
#define AMQP_0_9_METHOD_BASIC_CONSUME_OK                               21
#define AMQP_0_9_METHOD_BASIC_CANCEL                                   30
#define AMQP_0_9_METHOD_BASIC_CANCEL_OK                                31
#define AMQP_0_9_METHOD_BASIC_PUBLISH                                  40
#define AMQP_0_9_METHOD_BASIC_RETURN                                   50
#define AMQP_0_9_METHOD_BASIC_DELIVER                                  60
#define AMQP_0_9_METHOD_BASIC_GET                                      70
#define AMQP_0_9_METHOD_BASIC_GET_OK                                   71
#define AMQP_0_9_METHOD_BASIC_GET_EMPTY                                72
#define AMQP_0_9_METHOD_BASIC_ACK                                      80
#define AMQP_0_9_METHOD_BASIC_REJECT                                   90
/* basic(100) is in 0-9 called Recover and in 0-9-1 Recover.Async,
 * we will use the more recent 0-9-1 terminology */
#define AMQP_0_9_METHOD_BASIC_RECOVER_ASYNC                           100
#define AMQP_0_9_METHOD_BASIC_RECOVER                                 110
#define AMQP_0_9_METHOD_BASIC_RECOVER_OK                              111
#define AMQP_0_9_METHOD_BASIC_NACK                                    120

#define AMQP_0_9_METHOD_FILE_QOS                                       10
#define AMQP_0_9_METHOD_FILE_QOS_OK                                    11
#define AMQP_0_9_METHOD_FILE_CONSUME                                   20
#define AMQP_0_9_METHOD_FILE_CONSUME_OK                                21
#define AMQP_0_9_METHOD_FILE_CANCEL                                    30
#define AMQP_0_9_METHOD_FILE_CANCEL_OK                                 31
#define AMQP_0_9_METHOD_FILE_OPEN                                      40
#define AMQP_0_9_METHOD_FILE_OPEN_OK                                   41
#define AMQP_0_9_METHOD_FILE_STAGE                                     50
#define AMQP_0_9_METHOD_FILE_PUBLISH                                   60
#define AMQP_0_9_METHOD_FILE_RETURN                                    70
#define AMQP_0_9_METHOD_FILE_DELIVER                                   80
#define AMQP_0_9_METHOD_FILE_ACK                                       90
#define AMQP_0_9_METHOD_FILE_REJECT                                   100

#define AMQP_0_9_METHOD_STREAM_QOS                                     10
#define AMQP_0_9_METHOD_STREAM_QOS_OK                                  11
#define AMQP_0_9_METHOD_STREAM_CONSUME                                 20
#define AMQP_0_9_METHOD_STREAM_CONSUME_OK                              21
#define AMQP_0_9_METHOD_STREAM_CANCEL                                  30
#define AMQP_0_9_METHOD_STREAM_CANCEL_OK                               31
#define AMQP_0_9_METHOD_STREAM_PUBLISH                                 40
#define AMQP_0_9_METHOD_STREAM_RETURN                                  50
#define AMQP_0_9_METHOD_STREAM_DELIVER                                 60

#define AMQP_0_9_METHOD_TX_SELECT                                      10
#define AMQP_0_9_METHOD_TX_SELECT_OK                                   11
#define AMQP_0_9_METHOD_TX_COMMIT                                      20
#define AMQP_0_9_METHOD_TX_COMMIT_OK                                   21
#define AMQP_0_9_METHOD_TX_ROLLBACK                                    30
#define AMQP_0_9_METHOD_TX_ROLLBACK_OK                                 31

#define AMQP_0_9_METHOD_DTX_SELECT                                     10
#define AMQP_0_9_METHOD_DTX_SELECT_OK                                  11
#define AMQP_0_9_METHOD_DTX_START                                      20
#define AMQP_0_9_METHOD_DTX_START_OK                                   21

#define AMQP_0_9_METHOD_TUNNEL_REQUEST                                 10

#define AMQP_0_9_METHOD_CONFIRM_SELECT                                 10
#define AMQP_0_9_METHOD_CONFIRM_SELECT_OK                              11

/* AMQP 1.0 values */

#define AMQP_1_0_AMQP_FRAME 0
#define AMQP_1_0_SASL_FRAME 1
#define AMQP_1_0_TLS_FRAME  2

#define AMQP_1_0_AMQP_OPEN        0x10
#define AMQP_1_0_AMQP_BEGIN       0x11
#define AMQP_1_0_AMQP_ATTACH      0x12
#define AMQP_1_0_AMQP_FLOW        0x13
#define AMQP_1_0_AMQP_TRANSFER    0x14
#define AMQP_1_0_AMQP_DISPOSITION 0x15
#define AMQP_1_0_AMQP_DETACH      0x16
#define AMQP_1_0_AMQP_END         0x17
#define AMQP_1_0_AMQP_CLOSE       0x18

#define AMQP_1_0_SASL_MECHANISMS 0x40
#define AMQP_1_0_SASL_INIT       0x41
#define AMQP_1_0_SASL_CHALLENGE  0x42
#define AMQP_1_0_SASL_RESPONSE   0x43
#define AMQP_1_0_SASL_OUTCOME    0x44

#define AMQP_1_0_AMQP_TYPE_ERROR 0x1d
#define AMQP_1_0_AMQP_TYPE_HEADER 0x70
#define AMQP_1_0_AMQP_TYPE_DELIVERY_ANNOTATIONS 0x71
#define AMQP_1_0_AMQP_TYPE_MESSAGE_ANNOTATIONS 0x72
#define AMQP_1_0_AMQP_TYPE_PROPERTIES 0x73
#define AMQP_1_0_AMQP_TYPE_APPLICATION_PROPERTIES 0x74
#define AMQP_1_0_AMQP_TYPE_DATA 0x75
#define AMQP_1_0_AMQP_TYPE_AMQP_SEQUENCE 0x76
#define AMQP_1_0_AMQP_TYPE_AMQP_VALUE 0x77
#define AMQP_1_0_AMQP_TYPE_FOOTER 0x78
#define AMQP_1_0_AMQP_TYPE_RECEIVED 0x23
#define AMQP_1_0_AMQP_TYPE_ACCEPTED 0x24
#define AMQP_1_0_AMQP_TYPE_REJECTED 0x25
#define AMQP_1_0_AMQP_TYPE_RELEASED 0x26
#define AMQP_1_0_AMQP_TYPE_MODIFIED 0x27
#define AMQP_1_0_AMQP_TYPE_SOURCE 0x28
#define AMQP_1_0_AMQP_TYPE_TARGET 0x29
#define AMQP_1_0_AMQP_TYPE_DELETE_ON_CLOSE 0x2b
#define AMQP_1_0_AMQP_TYPE_DELETE_ON_NO_LINKS 0x2c
#define AMQP_1_0_AMQP_TYPE_DELETE_ON_NO_MESSAGE 0x2d
#define AMQP_1_0_AMQP_TYPE_DELETE_ON_NO_LINKS_OR_MESSAGE 0x2e
#define AMQP_1_0_AMQP_TYPE_COORDINATOR 0x30
#define AMQP_1_0_AMQP_TYPE_DECLARE 0x31
#define AMQP_1_0_AMQP_TYPE_DISCHARGE 0x32
#define AMQP_1_0_AMQP_TYPE_DECLARED 0x33
#define AMQP_1_0_AMQP_TYPE_TRANSACTIONAL_STATE 0x34

#define AMQP_1_0_TYPE_DESCRIPTOR_CONSTRUCTOR 0x00

#define AMQP_1_0_TYPE_NULL   0x40
#define AMQP_1_0_TYPE_LIST0   0x45
#define AMQP_1_0_TYPE_LIST8   0xc0
#define AMQP_1_0_TYPE_LIST32   0xd0
#define AMQP_1_0_TYPE_MAP8   0xc1
#define AMQP_1_0_TYPE_MAP32   0xd1
#define AMQP_1_0_TYPE_ARRAY8   0xe0
#define AMQP_1_0_TYPE_ARRAY32   0xf0

/* AMQP 0-10 values */

#define AMQP_0_10_FRAME_CONTROL  0
#define AMQP_0_10_FRAME_COMMAND  1
#define AMQP_0_10_FRAME_HEADER   2
#define AMQP_0_10_FRAME_BODY     3

#define AMQP_0_10_TYPE_STR16     0x95
#define AMQP_0_10_TYPE_MAP       0xa8
#define AMQP_0_10_TYPE_LIST      0xa9
#define AMQP_0_10_TYPE_ARRAY     0xaa
#define AMQP_0_10_TYPE_STRUCT32  0xab

#define AMQP_0_10_CLASS_CONNECTION           0x01
#define AMQP_0_10_METHOD_CONNECTION_START          0x01
#define AMQP_0_10_METHOD_CONNECTION_START_OK       0x02
#define AMQP_0_10_METHOD_CONNECTION_SECURE         0x03
#define AMQP_0_10_METHOD_CONNECTION_SECURE_OK      0x04
#define AMQP_0_10_METHOD_CONNECTION_TUNE           0x05
#define AMQP_0_10_METHOD_CONNECTION_TUNE_OK        0x06
#define AMQP_0_10_METHOD_CONNECTION_OPEN           0x07
#define AMQP_0_10_METHOD_CONNECTION_OPEN_OK        0x08
#define AMQP_0_10_METHOD_CONNECTION_REDIRECT       0x09
#define AMQP_0_10_METHOD_CONNECTION_HEARTBEAT      0x0a
#define AMQP_0_10_METHOD_CONNECTION_CLOSE          0x0b
#define AMQP_0_10_METHOD_CONNECTION_CLOSE_OK       0x0c

#define AMQP_0_10_CLASS_SESSION              0x02
#define AMQP_0_10_METHOD_SESSION_ATTACH            0x01
#define AMQP_0_10_METHOD_SESSION_ATTACHED          0x02
#define AMQP_0_10_METHOD_SESSION_DETACH            0x03
#define AMQP_0_10_METHOD_SESSION_DETACHED          0x04
#define AMQP_0_10_METHOD_SESSION_REQUEST_TIMEOUT   0x05
#define AMQP_0_10_METHOD_SESSION_TIMEOUT           0x06
#define AMQP_0_10_METHOD_SESSION_COMMAND_POINT     0x07
#define AMQP_0_10_METHOD_SESSION_EXPECTED          0x08
#define AMQP_0_10_METHOD_SESSION_CONFIRMED         0x09
#define AMQP_0_10_METHOD_SESSION_COMPLETED         0x0a
#define AMQP_0_10_METHOD_SESSION_KNOWN_COMPLETED   0x0b
#define AMQP_0_10_METHOD_SESSION_FLUSH             0x0c
#define AMQP_0_10_METHOD_SESSION_GAP               0x0d

#define AMQP_0_10_CLASS_EXECUTION            0x03
#define AMQP_0_10_METHOD_EXECUTION_SYNC            0x01
#define AMQP_0_10_METHOD_EXECUTION_RESULT          0x02
#define AMQP_0_10_METHOD_EXECUTION_EXCEPTION       0x03

#define AMQP_0_10_CLASS_MESSAGE              0x04
#define AMQP_0_10_STRUCT_MESSAGE_DELIVERY_PROPERTIES   0x01
#define AMQP_0_10_STRUCT_MESSAGE_FRAGMENT_PROPERTIES   0x02
#define AMQP_0_10_STRUCT_MESSAGE_MESSAGE_PROPERTIES    0x03
#define AMQP_0_10_STRUCT_MESSAGE_ACQUIRED              0x04
#define AMQP_0_10_STRUCT_MESSAGE_RESUME_RESULT         0x05
#define AMQP_0_10_METHOD_MESSAGE_TRANSFER          0x01
#define AMQP_0_10_METHOD_MESSAGE_ACCEPT            0x02
#define AMQP_0_10_METHOD_MESSAGE_REJECT            0x03
#define AMQP_0_10_METHOD_MESSAGE_RELEASE           0x04
#define AMQP_0_10_METHOD_MESSAGE_ACQUIRE           0x05
#define AMQP_0_10_METHOD_MESSAGE_RESUME            0x06
#define AMQP_0_10_METHOD_MESSAGE_SUBSCRIBE         0x07
#define AMQP_0_10_METHOD_MESSAGE_CANCEL            0x08
#define AMQP_0_10_METHOD_MESSAGE_SET_FLOW_MODE     0x09
#define AMQP_0_10_METHOD_MESSAGE_FLOW              0x0a
#define AMQP_0_10_METHOD_MESSAGE_FLUSH             0x0b
#define AMQP_0_10_METHOD_MESSAGE_STOP              0x0c

#define AMQP_0_10_CLASS_TX                   0x05
#define AMQP_0_10_METHOD_TX_SELECT                 0x01
#define AMQP_0_10_METHOD_TX_COMMIT                 0x02
#define AMQP_0_10_METHOD_TX_ROLLBACK               0x03

#define AMQP_0_10_CLASS_DTX                  0x06
#define AMQP_0_10_STRUCT_DTX_XA_RESULT          0x01
#define AMQP_0_10_STRUCT_DTX_RECOVER_RESULT     0x03
#define AMQP_0_10_METHOD_DTX_SELECT                0x01
#define AMQP_0_10_METHOD_DTX_START                 0x02
#define AMQP_0_10_METHOD_DTX_END                   0x03
#define AMQP_0_10_METHOD_DTX_COMMIT                0x04
#define AMQP_0_10_METHOD_DTX_FORGET                0x05
#define AMQP_0_10_METHOD_DTX_GET_TIMEOUT           0x06
#define AMQP_0_10_METHOD_DTX_PREPARE               0x07
#define AMQP_0_10_METHOD_DTX_RECOVER               0x08
#define AMQP_0_10_METHOD_DTX_ROLLBACK              0x09
#define AMQP_0_10_METHOD_DTX_SET_TIMEOUT           0x0a

#define AMQP_0_10_CLASS_EXCHANGE             0x07
#define AMQP_0_10_STRUCT_EXCHANGE_QUERY_RESULT  0x01
#define AMQP_0_10_STRUCT_EXCHANGE_BOUND_RESULT  0x02
#define AMQP_0_10_METHOD_EXCHANGE_DECLARE          0x01
#define AMQP_0_10_METHOD_EXCHANGE_DELETE           0x02
#define AMQP_0_10_METHOD_EXCHANGE_QUERY            0x03
#define AMQP_0_10_METHOD_EXCHANGE_BIND             0x04
#define AMQP_0_10_METHOD_EXCHANGE_UNBIND           0x05
#define AMQP_0_10_METHOD_EXCHANGE_BOUND            0x06

#define AMQP_0_10_CLASS_QUEUE                0x08
#define AMQP_0_10_STRUCT_QUEUE_QUERY_RESULT     0x01
#define AMQP_0_10_METHOD_QUEUE_DECLARE             0x01
#define AMQP_0_10_METHOD_QUEUE_DELETE              0x02
#define AMQP_0_10_METHOD_QUEUE_PURGE               0x03
#define AMQP_0_10_METHOD_QUEUE_QUERY               0x04

#define AMQP_0_10_CLASS_FILE                 0x09
#define AMQP_0_10_STRUCT_FILE_PROPERTIES        0x01
#define AMQP_0_10_METHOD_FILE_QOS                  0x01
#define AMQP_0_10_METHOD_FILE_QOS_OK               0x02
#define AMQP_0_10_METHOD_FILE_CONSUME              0x03
#define AMQP_0_10_METHOD_FILE_CONSUME_OK           0x04
#define AMQP_0_10_METHOD_FILE_CANCEL               0x05
#define AMQP_0_10_METHOD_FILE_OPEN                 0x06
#define AMQP_0_10_METHOD_FILE_OPEN_OK              0x07
#define AMQP_0_10_METHOD_FILE_STAGE                0x08
#define AMQP_0_10_METHOD_FILE_PUBLISH              0x09
#define AMQP_0_10_METHOD_FILE_RETURN               0x0a
#define AMQP_0_10_METHOD_FILE_DELIVER              0x0b
#define AMQP_0_10_METHOD_FILE_ACK                  0x0c
#define AMQP_0_10_METHOD_FILE_REJECT               0x0d

#define AMQP_0_10_CLASS_STREAM               0x0a
#define AMQP_0_10_STRUCT_STREAM_PROPERTIES      0x01
#define AMQP_0_10_METHOD_STREAM_QOS                0x01
#define AMQP_0_10_METHOD_STREAM_QOS_OK             0x02
#define AMQP_0_10_METHOD_STREAM_CONSUME            0x03
#define AMQP_0_10_METHOD_STREAM_CONSUME_OK         0x04
#define AMQP_0_10_METHOD_STREAM_CANCEL             0x05
#define AMQP_0_10_METHOD_STREAM_PUBLISH            0x06
#define AMQP_0_10_METHOD_STREAM_RETURN             0x07
#define AMQP_0_10_METHOD_STREAM_DELIVER            0x08

/*  Private functions  */

static unsigned
dissect_amqp_0_9_field_value(tvbuff_t *tvb, packet_info *pinfo, int offset, unsigned length,
                             const char *name, proto_tree *field_table_tree);

static void
dissect_amqp_0_10_struct32(tvbuff_t *tvb, packet_info *pinfo, proto_item *ti);

static amqp_channel_t*
get_conversation_channel(conversation_t *conv, uint16_t channel_num);

static void
record_msg_delivery(tvbuff_t *tvb, packet_info *pinfo, uint16_t channel_num,
    uint64_t delivery_tag);

static void
record_msg_delivery_c(conversation_t *conv, amqp_channel_t *channel,
    tvbuff_t *tvb, packet_info *pinfo, uint64_t delivery_tag);

static void
record_delivery_ack(tvbuff_t *tvb, packet_info *pinfo, uint16_t channel_num,
    uint64_t delivery_tag, bool multiple);

static void
record_delivery_ack_c(conversation_t *conv, amqp_channel_t *channel,
    tvbuff_t *tvb, packet_info *pinfo, uint64_t delivery_tag, bool multiple);

static void
generate_msg_reference(tvbuff_t *tvb, packet_info *pinfo, proto_tree *prop_tree);

static void
generate_ack_reference(tvbuff_t *tvb, packet_info *pinfo, proto_tree *prop_tree);

/*  AMQP 0-10 type decoding information  */

typedef int (*type_formatter)(tvbuff_t *tvb,
                              packet_info* pinfo,
                              unsigned offset,        /* In tvb where data starts */
                              unsigned length,        /* Length of data, if known */
                              const char **value); /* Receive formatted val */
struct amqp_typeinfo {
    uint8_t         typecode;   /* From AMQP 0-10 spec */
    const char     *amqp_typename;
    type_formatter  formatter;
    unsigned        known_size;
};

/*  AMQP 1-0 type decoding information  */

typedef int (*type_dissector)(tvbuff_t *tvb,
                              packet_info *pinfo,
                              unsigned offset,        /* In tvb where data starts */
                              unsigned length,        /* Length of data, if known */
                              proto_item *item,
                              int hf_amqp_type);

struct amqp1_typeinfo {
    uint8_t         typecode;   /* From AMQP 0-10 spec */
    const char     *amqp_typename;
    const int       ftype;
    unsigned        known_size;
    type_dissector  dissector;
    type_formatter  formatter;
};

struct amqp_synonym_types_t {
    const int *hf_none; /* Must be of type FT_NONE */
    const int *hf_uint; /* FT_UINT */
    const int *hf_str;  /* FT_STRING */
    const int *hf_bin;  /* FT_BYTES */
    const int *hf_guid; /* FT_GUID */
};

/*  struct for field interpreting format code (i.e. 0x70 for msg.header) to relevant hf_* variable
 *  (here hf_amqp_1_0_messageHeader). If the type is list, next 2 struct items specify how to
 *  interpret list items (in terms of hf_* variable)
 */
struct amqp_defined_types_t {
    const int format_code;
    int       *hf_amqp_type;
    uint32_t  hf_amqp_subtype_count;
    int * const *hf_amqp_subtypes;
};

/* functions for decoding 1.0 type and/or value */


static const struct amqp1_typeinfo* decode_fixed_type(uint8_t code);

static void
get_amqp_1_0_value_formatter(tvbuff_t *tvb,
                             packet_info *pinfo,
                             uint8_t code,
                             int offset,
                             int hf_amqp_type,
                             const char *name,
                             uint32_t hf_amqp_subtype_count,
                             int * const *hf_amqp_subtypes,
                             unsigned *length_size,
                             proto_item *item);

static unsigned
get_amqp_1_0_type_formatter(tvbuff_t *tvb,
                            packet_info* pinfo,
                            int offset,
                            int *hf_amqp_type,
                            const char **name,
                            uint32_t *hf_amqp_subtype_count,
                            int * const **hf_amqp_subtypes,
                            unsigned *length_size);

static void
get_amqp_1_0_type_value_formatter(tvbuff_t *tvb,
                                  packet_info *pinfo,
                                  int offset,
                                  int hf_amqp_type,
                                  const char *name,
                                  unsigned *length_size,
                                  proto_item *item);

/* functions for decoding particular primitive types */

static int
dissect_amqp_1_0_fixed(tvbuff_t *tvb, packet_info *pinfo,
                       unsigned offset, unsigned length,
                       proto_item *item, int hf_amqp_type);

static int
dissect_amqp_1_0_variable(tvbuff_t *tvb, packet_info *pinfo,
                          unsigned offset, unsigned length,
                          proto_item *item, int hf_amqp_type);

static int
dissect_amqp_1_0_timestamp(tvbuff_t *tvb, packet_info *pinfo _U_,
                           unsigned offset, unsigned length,
                           proto_item *item, int hf_amqp_type);

static int
dissect_amqp_1_0_skip(tvbuff_t *tvb _U_, packet_info *pinfo _U_,
                      unsigned offset _U_, unsigned length _U_,
                      proto_item *item _U_, int hf_amqp_type _U_);

static int
dissect_amqp_1_0_zero(tvbuff_t *tvb, packet_info *pinfo,
                      unsigned offset, unsigned length _U_,
                      proto_item *item, int hf_amqp_type);

static int
dissect_amqp_1_0_true(tvbuff_t *tvb, packet_info *pinfo,
                      unsigned offset, unsigned length _U_,
                      proto_item *item, int hf_amqp_type);

static int
dissect_amqp_1_0_false(tvbuff_t *tvb, packet_info *pinfo,
                               unsigned offset, unsigned length _U_,
                               proto_item *item, int hf_amqp_type);

static int
format_amqp_1_0_null(tvbuff_t *tvb _U_, packet_info* pinfo _U_,
                     unsigned offset _U_, unsigned length _U_,
                     const char **value);

static int
format_amqp_1_0_boolean_true(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                             const char **value);

static int
format_amqp_1_0_boolean_false(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                              const char **value);

static int
format_amqp_1_0_boolean(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                        const char **value);

static int
format_amqp_1_0_uint(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length,
                     const char **value);

static int
format_amqp_1_0_int(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length,
                    const char **value);

static int
format_amqp_1_0_float(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                      const char **value);

static int
format_amqp_1_0_double(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                       const char **value);

static int
format_amqp_1_0_decimal(tvbuff_t *tvb _U_, packet_info* pinfo, unsigned offset _U_, unsigned length,
                        const char **value);

static int
format_amqp_1_0_char(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                     const char **value);

static int
format_amqp_1_0_timestamp(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                          const char **value);

static int
format_amqp_1_0_uuid(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                     const char **value);

static int
format_amqp_1_0_bin(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length,
                    const char **value);

static int
format_amqp_1_0_str(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length,
                    const char **value);

static int
format_amqp_1_0_symbol(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length,
                       const char **value);

static bool
get_amqp_0_10_type_formatter(uint8_t code, packet_info* pinfo,
                             const char **name,
                             type_formatter *decoder,
                             unsigned *length_size);

static int
format_amqp_0_10_bin(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value);

static int
format_amqp_0_10_int(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value);

static int
format_amqp_0_10_uint(tvbuff_t *tvb, packet_info* pinfo,
                      unsigned offset, unsigned length,
                      const char **value);

static int
format_amqp_0_10_char(tvbuff_t *tvb, packet_info* pinfo,
                      unsigned offset, unsigned length,
                      const char **value);

static int
format_amqp_0_10_boolean(tvbuff_t *tvb, packet_info* pinfo,
                         unsigned offset, unsigned length,
                         const char **value);

static int
format_amqp_0_10_vbin(tvbuff_t *tvb, packet_info* pinfo,
                      unsigned offset, unsigned length,
                      const char **value);

static int
format_amqp_0_10_str(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value);

static void
format_amqp_0_10_sequence_set(tvbuff_t *tvb, unsigned offset, unsigned length,
                              proto_item *item);

/*  Various handles  */

static int proto_amqp;
static int proto_amqpv0_9;
static int proto_amqpv0_10;
static int proto_amqpv1_0;

/* 1.0 handles */

static int hf_amqp_1_0_size;
static int hf_amqp_1_0_doff;
static int hf_amqp_1_0_type;
static int hf_amqp_1_0_containerId;
static int hf_amqp_1_0_hostname;
static int hf_amqp_1_0_maxFrameSize;
static int hf_amqp_1_0_channelMax;
static int hf_amqp_1_0_idleTimeOut;
static int hf_amqp_1_0_outgoingLocales;
static int hf_amqp_1_0_incomingLocales;
static int hf_amqp_1_0_offeredCapabilities;
static int hf_amqp_1_0_desiredCapabilities;
static int hf_amqp_1_0_properties;
static int hf_amqp_1_0_remoteChannel;
static int hf_amqp_1_0_nextOutgoingId;
static int hf_amqp_1_0_incomingWindow;
static int hf_amqp_1_0_outgoingWindow;
static int hf_amqp_1_0_handleMax;
static int hf_amqp_1_0_name;
static int hf_amqp_1_0_handle;
static int hf_amqp_1_0_role;
static int hf_amqp_1_0_sndSettleMode;
static int hf_amqp_1_0_rcvSettleMode;
static int hf_amqp_1_0_source;
static int hf_amqp_1_0_target;
static int hf_amqp_1_0_deleteOnClose;
static int hf_amqp_1_0_deleteOnNoLinks;
static int hf_amqp_1_0_deleteOnNoMessages;
static int hf_amqp_1_0_deleteOnNoLinksOrMessages;
static int hf_amqp_1_0_coordinator;
static int hf_amqp_1_0_declare;
static int hf_amqp_1_0_globalId;
static int hf_amqp_1_0_discharge;
static int hf_amqp_1_0_txnId;
static int hf_amqp_1_0_fail;
static int hf_amqp_1_0_declared;
static int hf_amqp_1_0_transactionalState;
static int hf_amqp_1_0_outcome;
static int hf_amqp_1_0_unsettled;
static int hf_amqp_1_0_incompleteUnsettled;
static int hf_amqp_1_0_initialDeliveryCount;
static int hf_amqp_1_0_maxMessageSize;
static int hf_amqp_1_0_nextIncomingId;
static int hf_amqp_1_0_deliveryCount;
static int hf_amqp_1_0_sectionNumber;
static int hf_amqp_1_0_sectionOffset;
static int hf_amqp_1_0_deliveryFailed;
static int hf_amqp_1_0_undeliverableHere;
static int hf_amqp_1_0_linkCredit;
static int hf_amqp_1_0_available;
static int hf_amqp_1_0_drain;
static int hf_amqp_1_0_echo;
static int hf_amqp_1_0_deliveryId;
static int hf_amqp_1_0_deliveryTag;
static int hf_amqp_1_0_messageFormat;
static int hf_amqp_1_0_settled;
static int hf_amqp_1_0_more;
static int hf_amqp_1_0_state;
static int hf_amqp_1_0_resume;
static int hf_amqp_1_0_aborted;
static int hf_amqp_1_0_batchable;
static int hf_amqp_1_0_first;
static int hf_amqp_1_0_last;
static int hf_amqp_1_0_closed;
static int hf_amqp_1_0_amqp_performative;
static int hf_amqp_1_0_error;
static int hf_amqp_1_0_messageHeader;
static int hf_amqp_1_0_deliveryAnnotations;
static int hf_amqp_1_0_messageAnnotations;
static int hf_amqp_1_0_messageProperties;
static int hf_amqp_1_0_applicationProperties;
static int hf_amqp_1_0_data;
static int hf_amqp_1_0_amqp_sequence;
static int hf_amqp_1_0_amqp_value;
static int hf_amqp_1_0_footer;
static int hf_amqp_1_0_received;
static int hf_amqp_1_0_accepted;
static int hf_amqp_1_0_rejected;
static int hf_amqp_1_0_released;
static int hf_amqp_1_0_modified;
static int hf_amqp_1_0_condition;
static int hf_amqp_1_0_description;
static int hf_amqp_1_0_info;
static int hf_amqp_1_0_address;
static int hf_amqp_1_0_durable;
static int hf_amqp_1_0_terminusDurable;
static int hf_amqp_1_0_priority;
static int hf_amqp_1_0_ttl;
static int hf_amqp_1_0_firstAcquirer;
static int hf_amqp_1_0_expiryPolicy;
static int hf_amqp_1_0_timeout;
static int hf_amqp_1_0_dynamic;
static int hf_amqp_1_0_dynamicNodeProperties;
static int hf_amqp_1_0_distributionMode;
static int hf_amqp_1_0_filter;
static int hf_amqp_1_0_defaultOutcome;
static int hf_amqp_1_0_outcomes;
static int hf_amqp_1_0_capabilities;
static int hf_amqp_1_0_messageId;
static int hf_amqp_1_0_userId;
static int hf_amqp_1_0_to;
static int hf_amqp_1_0_subject;
static int hf_amqp_1_0_replyTo;
static int hf_amqp_1_0_correlationId;
static int hf_amqp_1_0_contentType;
static int hf_amqp_1_0_contentEncoding;
static int hf_amqp_1_0_absoluteExpiryTime;
static int hf_amqp_1_0_creationTime;
static int hf_amqp_1_0_groupId;
static int hf_amqp_1_0_groupSequence;
static int hf_amqp_1_0_replyToGroupId;
static int hf_amqp_1_0_sasl_method;
static int hf_amqp_1_0_mechanisms;
static int hf_amqp_1_0_mechanism;
static int hf_amqp_1_0_initResponse;
static int hf_amqp_1_0_saslChallenge;
static int hf_amqp_1_0_saslResponse;
static int hf_amqp_1_0_saslCode;
static int hf_amqp_1_0_saslAdditionalData;
static int hf_amqp_1_0_list;
static int hf_amqp_1_0_map;
/* variables for variant sub-types (see amqp_synonym_types)
 * - fields of type="*" can be of any type
 * - fields with multiple="true" may contain the type or an array */
static int hf_amqp_1_0_outgoingLocales_sym;
static int hf_amqp_1_0_incomingLocales_sym;
static int hf_amqp_1_0_offeredCapabilities_sym;
static int hf_amqp_1_0_desiredCapabilities_sym;
static int hf_amqp_1_0_address_str;
static int hf_amqp_1_0_source_str;
static int hf_amqp_1_0_target_str;
static int hf_amqp_1_0_outcomes_sym;
static int hf_amqp_1_0_capabilities_sym;
static int hf_amqp_1_0_messageId_uint;
static int hf_amqp_1_0_messageId_str;
static int hf_amqp_1_0_messageId_bin;
static int hf_amqp_1_0_messageId_uuid;
static int hf_amqp_1_0_correlationId_uint;
static int hf_amqp_1_0_correlationId_str;
static int hf_amqp_1_0_correlationId_bin;
static int hf_amqp_1_0_correlationId_uuid;
static int hf_amqp_1_0_to_str;
static int hf_amqp_1_0_replyTo_str;
static int hf_amqp_1_0_mechanisms_sym;

/* Several field can be of multiple types. To distinguish it among hf_amqp_1_0_* variables,
 * table below "translates" original hf_amqp_1_0_* variable to the type-specific one.
 * Each row contains synonym fields for {FT_NONE, FT_UINT, FT_STRING, FT_BYTES, FT_GUID}
 * NULL indicates no synonym of a given type
 * FT_NONE field must be always present
 */
static const struct amqp_synonym_types_t amqp_synonym_types[] = {
    {&hf_amqp_1_0_outgoingLocales, NULL, &hf_amqp_1_0_outgoingLocales_sym, NULL, NULL},
    {&hf_amqp_1_0_incomingLocales, NULL, &hf_amqp_1_0_incomingLocales_sym, NULL, NULL},
    {&hf_amqp_1_0_offeredCapabilities, NULL, &hf_amqp_1_0_offeredCapabilities_sym, NULL, NULL},
    {&hf_amqp_1_0_desiredCapabilities, NULL, &hf_amqp_1_0_desiredCapabilities_sym, NULL, NULL},
    {&hf_amqp_1_0_address, NULL, &hf_amqp_1_0_address_str, NULL, NULL},
    {&hf_amqp_1_0_source, NULL, &hf_amqp_1_0_source_str, NULL, NULL},
    {&hf_amqp_1_0_target, NULL, &hf_amqp_1_0_target_str, NULL, NULL},
    {&hf_amqp_1_0_outcomes, NULL, &hf_amqp_1_0_outcomes_sym, NULL, NULL},
    {&hf_amqp_1_0_capabilities, NULL, &hf_amqp_1_0_capabilities_sym, NULL, NULL},
    {&hf_amqp_1_0_messageId, &hf_amqp_1_0_messageId_uint, &hf_amqp_1_0_messageId_str, &hf_amqp_1_0_messageId_bin, &hf_amqp_1_0_messageId_uuid},
    {&hf_amqp_1_0_messageId, &hf_amqp_1_0_messageId_uint, &hf_amqp_1_0_messageId_str, &hf_amqp_1_0_messageId_bin, &hf_amqp_1_0_messageId_uuid},
    {&hf_amqp_1_0_correlationId, &hf_amqp_1_0_correlationId_uint, &hf_amqp_1_0_correlationId_str, &hf_amqp_1_0_correlationId_bin, &hf_amqp_1_0_correlationId_uuid},
    {&hf_amqp_1_0_to, NULL, &hf_amqp_1_0_to_str, NULL, NULL},
    {&hf_amqp_1_0_replyTo, NULL, &hf_amqp_1_0_replyTo_str, NULL, NULL},
    {&hf_amqp_1_0_mechanisms, NULL, &hf_amqp_1_0_mechanisms_sym, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL}
};

/* fields with hf_* types for list items;
 * i.e. sasl.init method has 3 arguments in a list (mechanism, init.response, hostname)
 * so when dissecting sasl.init arguments list, identify the list items with
 * corresponding hf_* variable */
static int * const amqp_1_0_sasl_mechanisms_items[] = { &hf_amqp_1_0_mechanisms };
static int * const amqp_1_0_sasl_init_items[] = { &hf_amqp_1_0_mechanism,
                                             &hf_amqp_1_0_initResponse,
                                             &hf_amqp_1_0_hostname };
static int * const amqp_1_0_sasl_challenge_items[] = { &hf_amqp_1_0_saslChallenge };
static int * const amqp_1_0_sasl_response_items[] = { &hf_amqp_1_0_saslResponse };
static int * const amqp_1_0_sasl_outcome_items[] = { &hf_amqp_1_0_saslCode,
                                                &hf_amqp_1_0_saslAdditionalData };
static int * const amqp_1_0_amqp_open_items[] = { &hf_amqp_1_0_containerId,
                                             &hf_amqp_1_0_hostname,
                                             &hf_amqp_1_0_maxFrameSize,
                                             &hf_amqp_1_0_channelMax,
                                             &hf_amqp_1_0_idleTimeOut,
                                             &hf_amqp_1_0_outgoingLocales,
                                             &hf_amqp_1_0_incomingLocales,
                                             &hf_amqp_1_0_offeredCapabilities,
                                             &hf_amqp_1_0_desiredCapabilities,
                                             &hf_amqp_1_0_properties };
static int * const amqp_1_0_amqp_begin_items[] = { &hf_amqp_1_0_remoteChannel,
                                              &hf_amqp_1_0_nextOutgoingId,
                                              &hf_amqp_1_0_incomingWindow,
                                              &hf_amqp_1_0_outgoingWindow,
                                              &hf_amqp_1_0_handleMax,
                                              &hf_amqp_1_0_offeredCapabilities,
                                              &hf_amqp_1_0_desiredCapabilities,
                                              &hf_amqp_1_0_properties };
static int * const amqp_1_0_amqp_attach_items[] = { &hf_amqp_1_0_name,
                                               &hf_amqp_1_0_handle,
                                               &hf_amqp_1_0_role,
                                               &hf_amqp_1_0_sndSettleMode,
                                               &hf_amqp_1_0_rcvSettleMode,
                                               &hf_amqp_1_0_source,
                                               &hf_amqp_1_0_target,
                                               &hf_amqp_1_0_unsettled,
                                               &hf_amqp_1_0_incompleteUnsettled,
                                               &hf_amqp_1_0_initialDeliveryCount,
                                               &hf_amqp_1_0_maxMessageSize,
                                               &hf_amqp_1_0_offeredCapabilities,
                                               &hf_amqp_1_0_desiredCapabilities,
                                               &hf_amqp_1_0_properties };
static int * const amqp_1_0_amqp_flow_items[] = { &hf_amqp_1_0_nextIncomingId,
                                             &hf_amqp_1_0_incomingWindow,
                                             &hf_amqp_1_0_nextOutgoingId,
                                             &hf_amqp_1_0_outgoingWindow,
                                             &hf_amqp_1_0_handle,
                                             &hf_amqp_1_0_deliveryCount,
                                             &hf_amqp_1_0_linkCredit,
                                             &hf_amqp_1_0_available,
                                             &hf_amqp_1_0_drain,
                                             &hf_amqp_1_0_echo,
                                             &hf_amqp_1_0_properties };
static int * const amqp_1_0_amqp_transfer_items[] = { &hf_amqp_1_0_handle,
                                                 &hf_amqp_1_0_deliveryId,
                                                 &hf_amqp_1_0_deliveryTag,
                                                 &hf_amqp_1_0_messageFormat,
                                                 &hf_amqp_1_0_settled,
                                                 &hf_amqp_1_0_more,
                                                 &hf_amqp_1_0_rcvSettleMode,
                                                 &hf_amqp_1_0_state,
                                                 &hf_amqp_1_0_resume,
                                                 &hf_amqp_1_0_aborted,
                                                 &hf_amqp_1_0_batchable };
static int * const amqp_1_0_amqp_disposition_items[] = { &hf_amqp_1_0_role,
                                                    &hf_amqp_1_0_first,
                                                    &hf_amqp_1_0_last,
                                                    &hf_amqp_1_0_settled,
                                                    &hf_amqp_1_0_state,
                                                    &hf_amqp_1_0_batchable };
static int * const amqp_1_0_amqp_detach_items[] = { &hf_amqp_1_0_handle,
                                               &hf_amqp_1_0_closed,
                                               &hf_amqp_1_0_error };
static int * const amqp_1_0_amqp_end_items[] = { &hf_amqp_1_0_error };
static int * const amqp_1_0_amqp_close_items[] = { &hf_amqp_1_0_error };
static int * const amqp_1_0_error_items[] = { &hf_amqp_1_0_condition,
                                         &hf_amqp_1_0_description,
                                         &hf_amqp_1_0_info };
static int * const amqp_1_0_messageHeader_items[] = { &hf_amqp_1_0_durable,
                                                 &hf_amqp_1_0_priority,
                                                 &hf_amqp_1_0_ttl,
                                                 &hf_amqp_1_0_firstAcquirer,
                                                 &hf_amqp_1_0_deliveryCount };
static int * const amqp_1_0_received_items[] = { &hf_amqp_1_0_sectionNumber,
                                            &hf_amqp_1_0_sectionOffset };
static int * const amqp_1_0_rejected_items[] = { &hf_amqp_1_0_error };
static int * const amqp_1_0_modified_items[] = { &hf_amqp_1_0_deliveryFailed,
                                            &hf_amqp_1_0_undeliverableHere,
                                            &hf_amqp_1_0_messageAnnotations };
static int * const amqp_1_0_source_items[] = { &hf_amqp_1_0_address,
                                          &hf_amqp_1_0_terminusDurable,
                                          &hf_amqp_1_0_expiryPolicy,
                                          &hf_amqp_1_0_timeout,
                                          &hf_amqp_1_0_dynamic,
                                          &hf_amqp_1_0_dynamicNodeProperties,
                                          &hf_amqp_1_0_distributionMode,
                                          &hf_amqp_1_0_filter,
                                          &hf_amqp_1_0_defaultOutcome,
                                          &hf_amqp_1_0_outcomes,
                                          &hf_amqp_1_0_capabilities };
static int * const amqp_1_0_target_items[] = { &hf_amqp_1_0_address,
                                          &hf_amqp_1_0_terminusDurable,
                                          &hf_amqp_1_0_expiryPolicy,
                                          &hf_amqp_1_0_timeout,
                                          &hf_amqp_1_0_dynamic,
                                          &hf_amqp_1_0_dynamicNodeProperties,
                                          &hf_amqp_1_0_capabilities };
static int * const amqp_1_0_messageProperties_items[] = { &hf_amqp_1_0_messageId,
                                                     &hf_amqp_1_0_userId,
                                                     &hf_amqp_1_0_to,
                                                     &hf_amqp_1_0_subject,
                                                     &hf_amqp_1_0_replyTo,
                                                     &hf_amqp_1_0_correlationId,
                                                     &hf_amqp_1_0_contentType,
                                                     &hf_amqp_1_0_contentEncoding,
                                                     &hf_amqp_1_0_absoluteExpiryTime,
                                                     &hf_amqp_1_0_creationTime,
                                                     &hf_amqp_1_0_groupId,
                                                     &hf_amqp_1_0_groupSequence,
                                                     &hf_amqp_1_0_replyToGroupId };
static int * const amqp_1_0_coordinator_items[] = { &hf_amqp_1_0_capabilities };
static int * const amqp_1_0_declare_items[] = { &hf_amqp_1_0_globalId };
static int * const amqp_1_0_discharge_items[] = { &hf_amqp_1_0_txnId,
                                             &hf_amqp_1_0_fail };
static int * const amqp_1_0_declared_items[] = { &hf_amqp_1_0_txnId };
static int * const amqp_1_0_transactionalState_items[] = { &hf_amqp_1_0_txnId,
                                                      &hf_amqp_1_0_outcome };

/* 0-10 handles */

static int hf_amqp_0_10_format;
static int hf_amqp_0_10_position;
static int hf_amqp_0_10_type;
static int hf_amqp_0_10_size;
static int hf_amqp_0_10_track;
static int hf_amqp_0_10_class;
static int hf_amqp_0_10_connection_method;
static int hf_amqp_0_10_session_method;
static int hf_amqp_0_10_execution_method;
static int hf_amqp_0_10_message_method;
static int hf_amqp_0_10_tx_method;
static int hf_amqp_0_10_dtx_method;
static int hf_amqp_0_10_exchange_method;
static int hf_amqp_0_10_queue_method;
static int hf_amqp_0_10_file_method;
static int hf_amqp_0_10_stream_method;
static int hf_amqp_0_10_argument_packing_flags;
static int hf_amqp_0_10_session_header;
static int hf_amqp_0_10_session_header_sync;
static int hf_amqp_0_10_struct32_size;
static int hf_amqp_0_10_struct32;
static int hf_amqp_0_10_struct32_padding;
static int hf_amqp_0_10_struct32_class;
static int hf_amqp_0_10_struct32_struct;
static int hf_amqp_0_10_array_type;
static int hf_amqp_0_10_array_element_count;
static int hf_amqp_0_10_array_string;
static int hf_amqp_0_10_message_body;
static int hf_amqp_0_10_dtx_xid;
static int hf_amqp_0_10_dtx_xid_format;
static int hf_amqp_0_10_dtx_xid_global_id;
static int hf_amqp_0_10_dtx_xid_branch_id;
static int hf_amqp_0_10_struct_delivery_properties_discard_unroutable;
static int hf_amqp_0_10_struct_delivery_properties_immediate;
static int hf_amqp_0_10_struct_delivery_properties_redelivered;
static int hf_amqp_0_10_struct_delivery_properties_priority;
static int hf_amqp_0_10_struct_delivery_properties_mode;
static int hf_amqp_0_10_struct_delivery_properties_ttl;
static int hf_amqp_0_10_struct_delivery_properties_timestamp;
static int hf_amqp_0_10_struct_delivery_properties_expiration;
static int hf_amqp_0_10_struct_delivery_properties_exchange;
static int hf_amqp_0_10_struct_delivery_properties_routing_key;
static int hf_amqp_0_10_struct_delivery_properties_resume_ttl;
static int hf_amqp_0_10_struct_fragment_properties_first;
static int hf_amqp_0_10_struct_fragment_properties_last;
static int hf_amqp_0_10_struct_fragment_properties_size;
/* static int hf_amqp_0_10_struct_message_properties; */
static int hf_amqp_0_10_struct_message_properties_content_len;
static int hf_amqp_0_10_struct_message_properties_message_id;
static int hf_amqp_0_10_struct_message_properties_correlation;
static int hf_amqp_0_10_struct_message_properties_reply_to;
static int hf_amqp_0_10_struct_message_properties_content_type;
static int hf_amqp_0_10_struct_message_properties_content_encoding;
static int hf_amqp_0_10_struct_message_properties_user_id;
static int hf_amqp_0_10_struct_message_properties_app_id;
static int hf_amqp_0_10_struct_message_properties_application_headers;
static int hf_amqp_0_10_struct_reply_to_exchange;
static int hf_amqp_0_10_struct_reply_to_routing_key;
static int hf_amqp_0_10_struct_acquired_transfers;
static int hf_amqp_0_10_struct_resume_result_offset;
static int hf_amqp_0_10_struct_exchange_query_result_durable;
static int hf_amqp_0_10_struct_exchange_query_result_not_found;
static int hf_amqp_0_10_struct_exchange_bound_result_exchange_not_found;
static int hf_amqp_0_10_struct_exchange_bound_result_queue_not_found;
static int hf_amqp_0_10_struct_exchange_bound_result_queue_not_matched;
static int hf_amqp_0_10_struct_exchange_bound_result_key_not_matched;
static int hf_amqp_0_10_struct_exchange_bound_result_args_not_matched;
static int hf_amqp_0_10_struct_queue_query_result_durable;
static int hf_amqp_0_10_struct_queue_query_result_exclusive;
static int hf_amqp_0_10_struct_queue_query_result_auto_delete;
static int hf_amqp_0_10_struct_queue_query_result_message_count;
static int hf_amqp_0_10_struct_queue_query_result_subscriber_count;
static int hf_amqp_0_10_struct_file_properties_content_type;
static int hf_amqp_0_10_struct_file_properties_content_encoding;
static int hf_amqp_0_10_struct_file_properties_headers;
static int hf_amqp_0_10_struct_file_properties_priority;
static int hf_amqp_0_10_struct_file_properties_reply_to;
static int hf_amqp_0_10_struct_file_properties_message_id;
static int hf_amqp_0_10_struct_file_properties_filename;
static int hf_amqp_0_10_struct_file_properties_timestamp;
static int hf_amqp_0_10_struct_file_properties_cluster_id;
static int hf_amqp_0_10_struct_stream_properties_content_type;
static int hf_amqp_0_10_struct_stream_properties_content_encoding;
static int hf_amqp_0_10_struct_stream_properties_headers;
static int hf_amqp_0_10_struct_stream_properties_priority;
static int hf_amqp_0_10_struct_stream_properties_timestamp;
static int hf_amqp_0_10_method_session_attach_name;
static int hf_amqp_0_10_method_session_attach_name_size;
static int hf_amqp_0_10_method_session_attach_force;
static int hf_amqp_0_10_method_session_detached_code;
static int hf_amqp_0_10_method_session_timeout;
static int hf_amqp_0_10_method_session_completed_timely;
static int hf_amqp_0_10_method_session_flush_expected;
static int hf_amqp_0_10_method_session_flush_confirmed;
static int hf_amqp_0_10_method_session_flush_completed;
static int hf_amqp_0_10_method_session_command_point_id;
static int hf_amqp_0_10_method_session_command_point_offset;
static int hf_amqp_0_10_method_session_commands;
static int hf_amqp_0_10_method_session_fragments;
static int hf_amqp_0_10_method_execution_command_id;
static int hf_amqp_0_10_method_execution_exception_error;
static int hf_amqp_0_10_method_execution_field_index;
static int hf_amqp_0_10_method_execution_description;
static int hf_amqp_0_10_method_execution_error_info;
static int hf_amqp_0_10_method_message_transfer_destination;
static int hf_amqp_0_10_method_message_transfer_accept_mode;
static int hf_amqp_0_10_method_message_transfer_acquire_mode;
static int hf_amqp_0_10_method_message_accept_transfers;
static int hf_amqp_0_10_method_message_transfer_reject_code;
static int hf_amqp_0_10_method_message_reject_text;
static int hf_amqp_0_10_method_message_release_set_redelivered;
static int hf_amqp_0_10_method_message_dest;
static int hf_amqp_0_10_method_message_resume_id;
static int hf_amqp_0_10_method_message_subscribe_queue;
static int hf_amqp_0_10_method_message_subscribe_exclusive;
static int hf_amqp_0_10_method_message_subscribe_resume_ttl;
static int hf_amqp_0_10_method_message_subscribe_args;
static int hf_amqp_0_10_method_message_flow_mode;
static int hf_amqp_0_10_method_message_credit_unit;
static int hf_amqp_0_10_method_message_credit_value;
static int hf_amqp_0_10_method_dtx_start_join;
static int hf_amqp_0_10_method_dtx_start_resume;
static int hf_amqp_0_10_method_dtx_end_fail;
static int hf_amqp_0_10_method_dtx_end_suspend;
static int hf_amqp_0_10_method_dtx_commit_one_phase;
static int hf_amqp_0_10_method_dtx_set_timeout_timeout;
static int hf_amqp_0_10_method_exchange_declare_exchange;
static int hf_amqp_0_10_method_exchange_declare_type;
static int hf_amqp_0_10_method_exchange_declare_alt_exchange;
static int hf_amqp_0_10_method_exchange_declare_passive;
static int hf_amqp_0_10_method_exchange_declare_durable;
static int hf_amqp_0_10_method_exchange_declare_auto_delete;
static int hf_amqp_0_10_method_exchange_declare_arguments;
static int hf_amqp_0_10_method_exchange_delete_if_unused;
static int hf_amqp_0_10_method_exchange_bind_queue;
static int hf_amqp_0_10_method_exchange_binding_key;
static int hf_amqp_0_10_method_queue_name;
static int hf_amqp_0_10_method_queue_alt_exchange;
static int hf_amqp_0_10_method_queue_declare_passive;
static int hf_amqp_0_10_method_queue_declare_durable;
static int hf_amqp_0_10_method_queue_declare_exclusive;
static int hf_amqp_0_10_method_queue_declare_auto_delete;
static int hf_amqp_0_10_method_queue_declare_arguments;
static int hf_amqp_0_10_method_queue_delete_if_unused;
static int hf_amqp_0_10_method_queue_delete_if_empty;
static int hf_amqp_0_10_method_file_qos_prefetch_size;
static int hf_amqp_0_10_method_file_qos_prefetch_count;
static int hf_amqp_0_10_method_file_qos_global;
static int hf_amqp_0_10_method_file_consumer_tag;
static int hf_amqp_0_10_method_file_consume_no_local;
static int hf_amqp_0_10_method_file_consume_no_ack;
static int hf_amqp_0_10_method_file_consume_exclusive;
static int hf_amqp_0_10_method_file_consume_nowait;
static int hf_amqp_0_10_method_file_consume_arguments;
static int hf_amqp_0_10_method_file_identifier;
static int hf_amqp_0_10_method_file_open_content_size;
static int hf_amqp_0_10_method_file_open_ok_staged_size;
static int hf_amqp_0_10_method_file_publish_exchange;
static int hf_amqp_0_10_method_file_publish_routing_key;
static int hf_amqp_0_10_method_file_publish_mandatory;
static int hf_amqp_0_10_method_file_publish_immediate;
static int hf_amqp_0_10_method_file_return_reply_code;
static int hf_amqp_0_10_method_file_return_reply_text;
static int hf_amqp_0_10_method_file_return_exchange;
static int hf_amqp_0_10_method_file_return_routing_key;
static int hf_amqp_0_10_method_file_deliver_consumer_tag;
static int hf_amqp_0_10_method_file_deliver_delivery_tag;
static int hf_amqp_0_10_method_file_deliver_redelivered;
static int hf_amqp_0_10_method_file_deliver_exchange;
static int hf_amqp_0_10_method_file_deliver_routing_key;
static int hf_amqp_0_10_method_file_ack_delivery_tag;
static int hf_amqp_0_10_method_file_ack_multiple;
static int hf_amqp_0_10_method_file_reject_delivery_tag;
static int hf_amqp_0_10_method_file_reject_requeue;
static int hf_amqp_0_10_method_stream_qos_prefetch_size;
static int hf_amqp_0_10_method_stream_qos_prefetch_count;
/* static int hf_amqp_0_10_method_stream_qos_consume_rate; */
static int hf_amqp_0_10_method_stream_qos_global;
static int hf_amqp_0_10_method_stream_consumer_tag;
static int hf_amqp_0_10_method_stream_consume_no_local;
static int hf_amqp_0_10_method_stream_consume_exclusive;
static int hf_amqp_0_10_method_stream_consume_nowait;
static int hf_amqp_0_10_method_stream_consume_arguments;
static int hf_amqp_0_10_method_stream_publish_exchange;
static int hf_amqp_0_10_method_stream_publish_routing_key;
static int hf_amqp_0_10_method_stream_publish_mandatory;
static int hf_amqp_0_10_method_stream_publish_immediate;
static int hf_amqp_0_10_method_stream_return_reply_code;
static int hf_amqp_0_10_method_stream_return_reply_text;
static int hf_amqp_0_10_method_stream_return_exchange;
static int hf_amqp_0_10_method_stream_return_routing_key;
static int hf_amqp_0_10_method_stream_deliver_consumer_tag;
static int hf_amqp_0_10_method_stream_deliver_delivery_tag;
static int hf_amqp_0_10_method_stream_deliver_exchange;
static int hf_amqp_0_10_method_stream_deliver_queue;
static int hf_amqp_channel;
static int hf_amqp_reserved;
static int hf_amqp_0_9_type;
static int hf_amqp_0_9_length;
static int hf_amqp_0_9_method_class_id;
static int hf_amqp_method_connection_method_id;
static int hf_amqp_method_channel_method_id;
static int hf_amqp_method_access_method_id;
static int hf_amqp_method_exchange_method_id;
static int hf_amqp_method_queue_method_id;
static int hf_amqp_method_basic_method_id;
static int hf_amqp_method_file_method_id;
static int hf_amqp_method_stream_method_id;
static int hf_amqp_method_tx_method_id;
static int hf_amqp_method_dtx_method_id;
static int hf_amqp_method_tunnel_method_id;
static int hf_amqp_method_confirm_method_id;
static int hf_amqp_method_arguments;
static int hf_amqp_method_connection_start_version_major;
static int hf_amqp_method_connection_start_version_minor;
static int hf_amqp_method_connection_start_server_properties;
static int hf_amqp_0_9_method_connection_start_mechanisms;
static int hf_amqp_0_10_method_connection_start_mechanisms;
static int hf_amqp_0_9_method_connection_start_locales;
static int hf_amqp_0_10_method_connection_start_locales;
static int hf_amqp_method_connection_start_ok_client_properties;
static int hf_amqp_method_connection_start_ok_mechanism;
static int hf_amqp_method_connection_start_ok_response;
static int hf_amqp_method_connection_start_ok_locale;
static int hf_amqp_method_connection_secure_challenge;
static int hf_amqp_method_connection_secure_ok_response;
static int hf_amqp_method_connection_tune_channel_max;
static int hf_amqp_0_9_method_connection_tune_frame_max;
static int hf_amqp_0_10_method_connection_tune_frame_max;
static int hf_amqp_0_9_method_connection_tune_heartbeat;
static int hf_amqp_0_10_method_connection_tune_heartbeat_min;
static int hf_amqp_0_10_method_connection_tune_heartbeat_max;
static int hf_amqp_method_connection_tune_ok_channel_max;
static int hf_amqp_0_9_method_connection_tune_ok_frame_max;
static int hf_amqp_0_10_method_connection_tune_ok_frame_max;
static int hf_amqp_method_connection_tune_ok_heartbeat;
static int hf_amqp_method_connection_open_virtual_host;
static int hf_amqp_0_9_method_connection_open_capabilities;
static int hf_amqp_0_10_method_connection_open_capabilities;
static int hf_amqp_0_9_method_connection_open_insist;
static int hf_amqp_0_10_method_connection_open_insist;
static int hf_amqp_0_9_method_connection_open_ok_known_hosts;
static int hf_amqp_0_10_method_connection_open_ok_known_hosts;
static int hf_amqp_method_connection_redirect_host;
static int hf_amqp_0_9_method_connection_redirect_known_hosts;
static int hf_amqp_0_10_method_connection_redirect_known_hosts;
static int hf_amqp_0_9_method_connection_close_reply_code;
static int hf_amqp_0_10_method_connection_close_reply_code;
static int hf_amqp_method_connection_close_reply_text;
static int hf_amqp_method_connection_close_class_id;
static int hf_amqp_method_connection_close_method_id;
static int hf_amqp_method_connection_blocked_reason;
static int hf_amqp_method_channel_open_out_of_band;
static int hf_amqp_method_channel_open_ok_channel_id;
static int hf_amqp_method_channel_flow_active;
static int hf_amqp_method_channel_flow_ok_active;
static int hf_amqp_method_channel_close_reply_code;
static int hf_amqp_method_channel_close_reply_text;
static int hf_amqp_method_channel_close_class_id;
static int hf_amqp_method_channel_close_method_id;
static int hf_amqp_method_channel_resume_channel_id;
static int hf_amqp_method_access_request_realm;
static int hf_amqp_method_access_request_exclusive;
static int hf_amqp_method_access_request_passive;
static int hf_amqp_method_access_request_active;
static int hf_amqp_method_access_request_write;
static int hf_amqp_method_access_request_read;
static int hf_amqp_method_access_request_ok_ticket;
static int hf_amqp_method_exchange_declare_ticket;
static int hf_amqp_method_exchange_declare_exchange;
static int hf_amqp_method_exchange_declare_type;
static int hf_amqp_method_exchange_declare_passive;
static int hf_amqp_method_exchange_declare_durable;
static int hf_amqp_method_exchange_declare_auto_delete;
static int hf_amqp_method_exchange_declare_internal;
static int hf_amqp_method_exchange_declare_nowait;
static int hf_amqp_method_exchange_declare_arguments;
static int hf_amqp_method_exchange_bind_destination;
static int hf_amqp_method_exchange_bind_source;
static int hf_amqp_method_exchange_bind_routing_key;
static int hf_amqp_method_exchange_bind_nowait;
static int hf_amqp_method_exchange_bind_arguments;
static int hf_amqp_method_exchange_delete_ticket;
static int hf_amqp_method_exchange_delete_exchange;
static int hf_amqp_method_exchange_delete_if_unused;
static int hf_amqp_method_exchange_delete_nowait;
static int hf_amqp_method_queue_declare_ticket;
static int hf_amqp_method_queue_declare_queue;
static int hf_amqp_method_queue_declare_passive;
static int hf_amqp_method_queue_declare_durable;
static int hf_amqp_method_queue_declare_exclusive;
static int hf_amqp_method_queue_declare_auto_delete;
static int hf_amqp_method_queue_declare_nowait;
static int hf_amqp_method_queue_declare_arguments;
static int hf_amqp_method_queue_declare_ok_queue;
static int hf_amqp_method_queue_declare_ok_message_count;
static int hf_amqp_method_queue_declare_ok_consumer_count;
static int hf_amqp_method_queue_bind_ticket;
static int hf_amqp_method_queue_bind_queue;
static int hf_amqp_method_queue_bind_exchange;
static int hf_amqp_method_queue_bind_routing_key;
static int hf_amqp_method_queue_bind_nowait;
static int hf_amqp_method_queue_bind_arguments;
static int hf_amqp_method_queue_unbind_ticket;
static int hf_amqp_method_queue_unbind_queue;
static int hf_amqp_method_queue_unbind_exchange;
static int hf_amqp_method_queue_unbind_routing_key;
static int hf_amqp_method_queue_unbind_arguments;
static int hf_amqp_method_queue_purge_ticket;
static int hf_amqp_method_queue_purge_queue;
static int hf_amqp_method_queue_purge_nowait;
static int hf_amqp_method_queue_purge_ok_message_count;
static int hf_amqp_method_queue_delete_ticket;
static int hf_amqp_method_queue_delete_queue;
static int hf_amqp_method_queue_delete_if_unused;
static int hf_amqp_method_queue_delete_if_empty;
static int hf_amqp_method_queue_delete_nowait;
static int hf_amqp_method_queue_delete_ok_message_count;
static int hf_amqp_method_basic_qos_prefetch_size;
static int hf_amqp_method_basic_qos_prefetch_count;
static int hf_amqp_method_basic_qos_global;
static int hf_amqp_method_basic_consume_ticket;
static int hf_amqp_method_basic_consume_queue;
static int hf_amqp_method_basic_consume_consumer_tag;
static int hf_amqp_method_basic_consume_no_local;
static int hf_amqp_method_basic_consume_no_ack;
static int hf_amqp_method_basic_consume_exclusive;
static int hf_amqp_method_basic_consume_nowait;
static int hf_amqp_method_basic_consume_filter;
static int hf_amqp_method_basic_consume_ok_consumer_tag;
static int hf_amqp_method_basic_cancel_consumer_tag;
static int hf_amqp_method_basic_cancel_nowait;
static int hf_amqp_method_basic_cancel_ok_consumer_tag;
static int hf_amqp_method_basic_publish_number;
static int hf_amqp_method_basic_publish_ticket;
static int hf_amqp_method_basic_publish_exchange;
static int hf_amqp_method_basic_publish_routing_key;
static int hf_amqp_method_basic_publish_mandatory;
static int hf_amqp_method_basic_publish_immediate;
static int hf_amqp_method_basic_return_reply_code;
static int hf_amqp_method_basic_return_reply_text;
static int hf_amqp_method_basic_return_exchange;
static int hf_amqp_method_basic_return_routing_key;
static int hf_amqp_method_basic_deliver_consumer_tag;
static int hf_amqp_method_basic_deliver_delivery_tag;
static int hf_amqp_method_basic_deliver_redelivered;
static int hf_amqp_method_basic_deliver_exchange;
static int hf_amqp_method_basic_deliver_routing_key;
static int hf_amqp_method_basic_get_ticket;
static int hf_amqp_method_basic_get_queue;
static int hf_amqp_method_basic_get_no_ack;
static int hf_amqp_method_basic_get_ok_delivery_tag;
static int hf_amqp_method_basic_get_ok_redelivered;
static int hf_amqp_method_basic_get_ok_exchange;
static int hf_amqp_method_basic_get_ok_routing_key;
static int hf_amqp_method_basic_get_ok_message_count;
static int hf_amqp_method_basic_get_empty_cluster_id;
static int hf_amqp_method_basic_ack_delivery_tag;
static int hf_amqp_method_basic_ack_multiple;
static int hf_amqp_method_basic_reject_delivery_tag;
static int hf_amqp_method_basic_reject_requeue;
static int hf_amqp_method_basic_recover_requeue;
static int hf_amqp_method_basic_nack_delivery_tag;
static int hf_amqp_method_basic_nack_multiple;
static int hf_amqp_method_basic_nack_requeue;
static int hf_amqp_method_file_qos_prefetch_size;
static int hf_amqp_method_file_qos_prefetch_count;
static int hf_amqp_method_file_qos_global;
static int hf_amqp_method_file_consume_ticket;
static int hf_amqp_method_file_consume_queue;
static int hf_amqp_method_file_consume_consumer_tag;
static int hf_amqp_method_file_consume_no_local;
static int hf_amqp_method_file_consume_no_ack;
static int hf_amqp_method_file_consume_exclusive;
static int hf_amqp_method_file_consume_nowait;
static int hf_amqp_method_file_consume_filter;
static int hf_amqp_method_file_consume_ok_consumer_tag;
static int hf_amqp_method_file_cancel_consumer_tag;
static int hf_amqp_method_file_cancel_nowait;
static int hf_amqp_method_file_cancel_ok_consumer_tag;
static int hf_amqp_method_file_open_identifier;
static int hf_amqp_method_file_open_content_size;
static int hf_amqp_method_file_open_ok_staged_size;
static int hf_amqp_method_file_publish_ticket;
static int hf_amqp_method_file_publish_exchange;
static int hf_amqp_method_file_publish_routing_key;
static int hf_amqp_method_file_publish_mandatory;
static int hf_amqp_method_file_publish_immediate;
static int hf_amqp_method_file_publish_identifier;
static int hf_amqp_method_file_return_reply_code;
static int hf_amqp_method_file_return_reply_text;
static int hf_amqp_method_file_return_exchange;
static int hf_amqp_method_file_return_routing_key;
static int hf_amqp_method_file_deliver_consumer_tag;
static int hf_amqp_method_file_deliver_delivery_tag;
static int hf_amqp_method_file_deliver_redelivered;
static int hf_amqp_method_file_deliver_exchange;
static int hf_amqp_method_file_deliver_routing_key;
static int hf_amqp_method_file_deliver_identifier;
static int hf_amqp_method_file_ack_delivery_tag;
static int hf_amqp_method_file_ack_multiple;
static int hf_amqp_method_file_reject_delivery_tag;
static int hf_amqp_method_file_reject_requeue;
static int hf_amqp_method_stream_qos_prefetch_size;
static int hf_amqp_method_stream_qos_prefetch_count;
static int hf_amqp_method_stream_qos_consume_rate;
static int hf_amqp_method_stream_qos_global;
static int hf_amqp_method_stream_consume_ticket;
static int hf_amqp_method_stream_consume_queue;
static int hf_amqp_method_stream_consume_consumer_tag;
static int hf_amqp_method_stream_consume_no_local;
static int hf_amqp_method_stream_consume_exclusive;
static int hf_amqp_method_stream_consume_nowait;
static int hf_amqp_method_stream_consume_filter;
static int hf_amqp_method_stream_consume_ok_consumer_tag;
static int hf_amqp_method_stream_cancel_consumer_tag;
static int hf_amqp_method_stream_cancel_nowait;
static int hf_amqp_method_stream_cancel_ok_consumer_tag;
static int hf_amqp_method_stream_publish_ticket;
static int hf_amqp_method_stream_publish_exchange;
static int hf_amqp_method_stream_publish_routing_key;
static int hf_amqp_method_stream_publish_mandatory;
static int hf_amqp_method_stream_publish_immediate;
static int hf_amqp_method_stream_return_reply_code;
static int hf_amqp_method_stream_return_reply_text;
static int hf_amqp_method_stream_return_exchange;
static int hf_amqp_method_stream_return_routing_key;
static int hf_amqp_method_stream_deliver_consumer_tag;
static int hf_amqp_method_stream_deliver_delivery_tag;
static int hf_amqp_method_stream_deliver_exchange;
static int hf_amqp_method_stream_deliver_queue;
static int hf_amqp_method_dtx_start_dtx_identifier;
static int hf_amqp_method_tunnel_request_meta_data;
static int hf_amqp_method_confirm_select_nowait;
static int hf_amqp_field;
static int hf_amqp_field_name;
static int hf_amqp_field_type;
static int hf_amqp_field_integer;
static int hf_amqp_field_unsigned_integer;
static int hf_amqp_field_string;
static int hf_amqp_field_boolean;
static int hf_amqp_field_byte;
static int hf_amqp_field_unsigned_byte;
static int hf_amqp_field_short_int;
static int hf_amqp_field_short_uint;
static int hf_amqp_field_long_int;
static int hf_amqp_field_float;
static int hf_amqp_field_double;
static int hf_amqp_field_decimal;
static int hf_amqp_field_timestamp;
static int hf_amqp_field_byte_array;
static int hf_amqp_header_class_id;
static int hf_amqp_header_weight;
static int hf_amqp_header_body_size;
static int hf_amqp_header_property_flags;
static int hf_amqp_header_properties;
static int hf_amqp_header_basic_content_type;
static int hf_amqp_header_basic_content_encoding;
static int hf_amqp_header_basic_headers;
static int hf_amqp_header_basic_delivery_mode;
static int hf_amqp_header_basic_priority;
static int hf_amqp_header_basic_correlation_id;
static int hf_amqp_header_basic_reply_to;
static int hf_amqp_header_basic_expiration;
static int hf_amqp_header_basic_message_id;
static int hf_amqp_header_basic_timestamp;
static int hf_amqp_header_basic_type;
static int hf_amqp_header_basic_user_id;
static int hf_amqp_header_basic_app_id;
static int hf_amqp_header_basic_cluster_id;
static int hf_amqp_header_file_content_type;
static int hf_amqp_header_file_content_encoding;
static int hf_amqp_header_file_headers;
static int hf_amqp_header_file_priority;
static int hf_amqp_header_file_reply_to;
static int hf_amqp_header_file_message_id;
static int hf_amqp_header_file_filename;
static int hf_amqp_header_file_timestamp;
static int hf_amqp_header_file_cluster_id;
static int hf_amqp_header_stream_content_type;
static int hf_amqp_header_stream_content_encoding;
static int hf_amqp_header_stream_headers;
static int hf_amqp_header_stream_priority;
static int hf_amqp_header_stream_timestamp;
static int hf_amqp_header_tunnel_headers;
static int hf_amqp_header_tunnel_proxy_name;
static int hf_amqp_header_tunnel_data_name;
static int hf_amqp_header_tunnel_durable;
static int hf_amqp_header_tunnel_broadcast;
static int hf_amqp_0_10_dtx_xa_status;
static int hf_amqp_payload;
static int hf_amqp_init_protocol;
static int hf_amqp_init_id;
static int hf_amqp_init_id_major;
static int hf_amqp_init_id_minor;
static int hf_amqp_init_version_major;
static int hf_amqp_init_version_minor;
static int hf_amqp_init_version_revision;
static int hf_amqp_message_in;
static int hf_amqp_ack_in;
static int hf_amqp_method_connection_start_server_properties_size;
static int hf_amqp_0_10_method_connection_start_mechanisms_size;
static int hf_amqp_0_10_method_connection_start_locales_size;
static int hf_amqp_method_connection_start_ok_client_properties_size;
static int hf_amqp_0_10_method_connection_open_capabilities_size;
static int hf_amqp_0_10_method_connection_open_ok_known_hosts_size;
static int hf_amqp_0_10_method_connection_redirect_known_hosts_size;
static int hf_amqp_0_10_method_execution_error_info_size;
static int hf_amqp_0_10_method_exchange_declare_arguments_size;
static int hf_amqp_0_10_method_queue_declare_arguments_size;
static int hf_amqp_0_10_method_file_consume_arguments_size;
static int hf_amqp_0_10_method_stream_consume_arguments_size;
static int hf_amqp_0_10_struct_message_properties_application_headers_size;
static int hf_amqp_0_10_struct_file_properties_headers_size;
static int hf_amqp_0_10_struct_stream_properties_headers_size;
static int hf_amqp_0_10_struct_dtx_recover_result_size;

static int ett_amqp;
static int ett_header;
static int ett_args;
static int ett_props;
static int ett_field_table;
static int ett_amqp_init;
static int ett_amqp_0_9_field;
static int ett_amqp_0_10_map;
static int ett_amqp_0_10_array;
static int ett_amqp_0_10_struct;
static int ett_amqp_1_0_list;
static int ett_amqp_1_0_array;
static int ett_amqp_1_0_map;

static expert_field ei_amqp_connection_error;
static expert_field ei_amqp_channel_error;
static expert_field ei_amqp_message_undeliverable;
static expert_field ei_amqp_bad_flag_value;
static expert_field ei_amqp_unknown_stream_method;
static expert_field ei_amqp_unknown_basic_method;
static expert_field ei_amqp_unknown_frame_type;
static expert_field ei_amqp_field_short;
static expert_field ei_amqp_bad_length;
static expert_field ei_amqp_unknown_command_class;
static expert_field ei_amqp_unknown_tunnel_method;
static expert_field ei_amqp_unknown_confirm_method;
static expert_field ei_amqp_invalid_class_code;
static expert_field ei_amqp_unknown_access_method;
static expert_field ei_amqp_unknown_tx_method;
static expert_field ei_amqp_unknown_header_class;
static expert_field ei_amqp_unknown_connection_method;
static expert_field ei_amqp_unknown_queue_method;
static expert_field ei_amqp_unknown_channel_method;
static expert_field ei_amqp_unknown_dtx_method;
static expert_field ei_amqp_unknown_method_class;
static expert_field ei_amqp_unknown_file_method;
static expert_field ei_amqp_unknown_exchange_method;
static expert_field ei_amqp_unknown_sasl_command;
static expert_field ei_amqp_unknown_amqp_command;
static expert_field ei_amqp_unknown_amqp_type;
static expert_field ei_amqp_invalid_number_of_params;
static expert_field ei_amqp_size_exceeds_65K;
static expert_field ei_amqp_array_type_unknown;

static dissector_handle_t amqp_tcp_handle;

static amqp_message_decode_t *amqp_message_decodes;
static unsigned num_amqp_message_decodes;

static void *amqp_message_decode_copy_cb(void *dest, const void *orig, size_t len _U_)
{
    const amqp_message_decode_t *o = (const amqp_message_decode_t *)orig;
    amqp_message_decode_t *d = (amqp_message_decode_t *)dest;

    d->match_criteria = o->match_criteria;
    d->topic_pattern = g_strdup(o->topic_pattern);
    d->payload_proto_name = g_strdup(o->payload_proto_name);
    d->payload_proto = o->payload_proto;
    d->topic_more_info = g_strdup(o->topic_more_info);

    return d;
}

static bool amqp_message_decode_update_cb(void *record, char **error)
{
    amqp_message_decode_t *u = (amqp_message_decode_t *)record;

    if (u->topic_pattern == NULL || strlen(u->topic_pattern) == 0) {
        *error = g_strdup("Missing topic pattern");
        return false;
    }

    if (u->payload_proto_name == NULL || strlen(u->payload_proto_name) == 0) {
        *error = g_strdup("Missing payload protocol");
        return false;
    }

    if (u->match_criteria == MATCH_CRITERIA_REGEX) {
        u->topic_regex = g_regex_new(u->topic_pattern, (GRegexCompileFlags) G_REGEX_OPTIMIZE, (GRegexMatchFlags) 0, NULL);
        if (!u->topic_regex) {
            *error = g_strdup_printf("Invalid regex: %s", u->topic_pattern);
            return false;
        }
    }

    return true;
}

static void amqp_message_decode_free_cb(void *record)
{
    amqp_message_decode_t *u = (amqp_message_decode_t *)record;

    g_free(u->topic_pattern);
    if (u->topic_regex) {
        g_regex_unref(u->topic_regex);
    }
    g_free(u->payload_proto_name);
    g_free(u->topic_more_info);
}

UAT_VS_DEF(message_decode, match_criteria, amqp_message_decode_t, uint32_t, MATCH_CRITERIA_EQUAL, "Equal to")
UAT_CSTRING_CB_DEF(message_decode, topic_pattern, amqp_message_decode_t)
UAT_DISSECTOR_DEF(message_decode, payload_proto, payload_proto, payload_proto_name, amqp_message_decode_t)
UAT_CSTRING_CB_DEF(message_decode, topic_more_info, amqp_message_decode_t)


/*  Various enumerations  */

static const value_string amqp_1_0_SASL_code_value [] = {
    {0, "ok"},
    {1, "auth"},
    {2, "sys"},
    {3, "sys-perm"},
    {4, "sys-temp"},
    {0, NULL}
};

static const true_false_string amqp_1_0_role_value = {
    "receiver",
    "sender"
};

static const value_string amqp_1_0_sndSettleMode_value[] = {
    {0, "unsettled"},
    {1, "settled"},
    {2, "mixed"},
    {0, NULL}
};

static const value_string amqp_1_0_rcvSettleMode_value[] = {
    {0, "first"},
    {1, "second"},
    {0, NULL}
};

static const value_string amqp_1_0_terminus_durable_value[] = {
    {0, "none"},
    {1, "configuration"},
    {2, "unsettled-state"},
    {0, NULL}
};

static const value_string amqp_1_0_AMQP_performatives [] = {
    {AMQP_1_0_AMQP_OPEN,        "open"},
    {AMQP_1_0_AMQP_BEGIN,       "begin"},
    {AMQP_1_0_AMQP_ATTACH,      "attach"},
    {AMQP_1_0_AMQP_FLOW,        "flow"},
    {AMQP_1_0_AMQP_TRANSFER,    "transfer"},
    {AMQP_1_0_AMQP_DISPOSITION, "disposition"},
    {AMQP_1_0_AMQP_DETACH,      "detach"},
    {AMQP_1_0_AMQP_END,         "end"},
    {AMQP_1_0_AMQP_CLOSE,       "close"},
    {0, NULL}
};

static const value_string amqp_1_0_SASL_methods [] = {
    {AMQP_1_0_SASL_MECHANISMS, "sasl.mechanisms"},
    {AMQP_1_0_SASL_INIT,       "sasl.init"},
    {AMQP_1_0_SASL_CHALLENGE,  "sasl.challenge"},
    {AMQP_1_0_SASL_RESPONSE,   "sasl.response"},
    {AMQP_1_0_SASL_OUTCOME,    "sasl.outcome"},
    {0, NULL}
};

static const value_string amqp_1_0_type [] = {
    {AMQP_1_0_AMQP_FRAME, "AMQP"},
    {AMQP_1_0_SASL_FRAME, "SASL"},
    {AMQP_1_0_TLS_FRAME,  "TLS"},
    {0, NULL}
};

static const value_string amqp_0_10_frame_position [] = {
    {0x00,  "----"},
    {0x01,  "---e"},
    {0x02,  "--b-"},
    {0x03,  "--be"},
    {0x04,  "-E--"},
    {0x05,  "-E-e"},
    {0x06,  "-Eb-"},
    {0x07,  "-Ebe"},
    {0x08,  "B---"},
    {0x09,  "B--e"},
    {0x0a,  "B-b-"},
    {0x0b,  "B-be"},
    {0x0c,  "BE--"},
    {0x0d,  "BE-e"},
    {0x0e,  "BEb-"},
    {0x0f,  "BEbe"},
    {0, NULL}
};

static const value_string amqp_0_10_frame_types [] = {
    {0,     "Control"},
    {1,     "Command"},
    {2,     "Header"},
    {3,     "Body"},
    {0, NULL}
};

static const value_string amqp_0_10_frame_tracks [] = {
    {0,     "Control"},
    {1,     "Command"},
    {0, NULL}
};

static const value_string amqp_0_10_class [] = {
    {AMQP_0_10_CLASS_CONNECTION,  "Connection"},
    {AMQP_0_10_CLASS_SESSION,     "Session"},
    {AMQP_0_10_CLASS_EXECUTION,   "Execution"},
    {AMQP_0_10_CLASS_MESSAGE,     "Message"},
    {AMQP_0_10_CLASS_TX,          "Tx"},
    {AMQP_0_10_CLASS_DTX,         "Dtx"},
    {AMQP_0_10_CLASS_EXCHANGE,    "Exchange"},
    {AMQP_0_10_CLASS_QUEUE,       "Queue"},
    {AMQP_0_10_CLASS_FILE,        "File"},
    {AMQP_0_10_CLASS_STREAM,      "Stream"},
    {0, NULL}
};

static const value_string amqp_0_10_connection_methods [] = {
    {AMQP_0_10_METHOD_CONNECTION_START,     "connection.start"},
    {AMQP_0_10_METHOD_CONNECTION_START_OK,  "connection.start-ok"},
    {AMQP_0_10_METHOD_CONNECTION_SECURE,    "connection.secure"},
    {AMQP_0_10_METHOD_CONNECTION_SECURE_OK, "connection.secure-ok"},
    {AMQP_0_10_METHOD_CONNECTION_TUNE,      "connection.tune"},
    {AMQP_0_10_METHOD_CONNECTION_TUNE_OK,   "connection.tune-ok"},
    {AMQP_0_10_METHOD_CONNECTION_OPEN,      "connection.open"},
    {AMQP_0_10_METHOD_CONNECTION_OPEN_OK,   "connection.open-ok"},
    {AMQP_0_10_METHOD_CONNECTION_REDIRECT,  "connection.redirect"},
    {AMQP_0_10_METHOD_CONNECTION_HEARTBEAT, "connection.heartbeat"},
    {AMQP_0_10_METHOD_CONNECTION_CLOSE,     "connection.close"},
    {AMQP_0_10_METHOD_CONNECTION_CLOSE_OK,  "connection.close-ok"},
    {0, NULL}
};

static const value_string amqp_0_10_session_methods [] = {
    {AMQP_0_10_METHOD_SESSION_ATTACH,           "session.attach"},
    {AMQP_0_10_METHOD_SESSION_ATTACHED,         "session.attached"},
    {AMQP_0_10_METHOD_SESSION_DETACH,           "session.detach"},
    {AMQP_0_10_METHOD_SESSION_DETACHED,         "session.detached"},
    {AMQP_0_10_METHOD_SESSION_REQUEST_TIMEOUT,  "session.request-timeout"},
    {AMQP_0_10_METHOD_SESSION_TIMEOUT,          "session.timeout"},
    {AMQP_0_10_METHOD_SESSION_COMMAND_POINT,    "session.command-point"},
    {AMQP_0_10_METHOD_SESSION_EXPECTED,         "session.expected"},
    {AMQP_0_10_METHOD_SESSION_CONFIRMED,        "session.confirmed"},
    {AMQP_0_10_METHOD_SESSION_COMPLETED,        "session.completed"},
    {AMQP_0_10_METHOD_SESSION_KNOWN_COMPLETED,  "session.known-completed"},
    {AMQP_0_10_METHOD_SESSION_FLUSH,            "session.flush"},
    {AMQP_0_10_METHOD_SESSION_GAP,              "session.gap"},
    {0, NULL}
};

static const value_string amqp_0_10_execution_methods [] = {
    {AMQP_0_10_METHOD_EXECUTION_SYNC,       "execution.sync"},
    {AMQP_0_10_METHOD_EXECUTION_RESULT,     "execution.result"},
    {AMQP_0_10_METHOD_EXECUTION_EXCEPTION,  "execution.exception"},
    {0, NULL}
};

static const value_string amqp_0_10_message_methods [] = {
    {AMQP_0_10_METHOD_MESSAGE_TRANSFER,      "message.transfer"},
    {AMQP_0_10_METHOD_MESSAGE_ACCEPT,        "message.accept"},
    {AMQP_0_10_METHOD_MESSAGE_REJECT,        "message.reject"},
    {AMQP_0_10_METHOD_MESSAGE_RELEASE,       "message.release"},
    {AMQP_0_10_METHOD_MESSAGE_ACQUIRE,       "message.acquire"},
    {AMQP_0_10_METHOD_MESSAGE_RESUME,        "message.resume"},
    {AMQP_0_10_METHOD_MESSAGE_SUBSCRIBE,     "message.subscribe"},
    {AMQP_0_10_METHOD_MESSAGE_CANCEL,        "message.cancel"},
    {AMQP_0_10_METHOD_MESSAGE_SET_FLOW_MODE, "message.set-flow-mode"},
    {AMQP_0_10_METHOD_MESSAGE_FLOW,          "message.flow"},
    {AMQP_0_10_METHOD_MESSAGE_FLUSH,         "message.flush"},
    {AMQP_0_10_METHOD_MESSAGE_STOP,          "message.stop"},
    {0, NULL}
};

static const value_string amqp_0_10_tx_methods [] = {
    {AMQP_0_10_METHOD_TX_SELECT,    "tx.select"},
    {AMQP_0_10_METHOD_TX_COMMIT,    "tx.commit"},
    {AMQP_0_10_METHOD_TX_ROLLBACK,  "tx.rollback"},
    {0, NULL}
};

static const value_string amqp_0_10_dtx_methods [] = {
    {AMQP_0_10_METHOD_DTX_SELECT,       "dtx.select"},
    {AMQP_0_10_METHOD_DTX_START,        "dtx.start"},
    {AMQP_0_10_METHOD_DTX_END,          "dtx.end"},
    {AMQP_0_10_METHOD_DTX_COMMIT,       "dtx.commit"},
    {AMQP_0_10_METHOD_DTX_FORGET,       "dtx.forget"},
    {AMQP_0_10_METHOD_DTX_GET_TIMEOUT,  "dtx.get-timeout"},
    {AMQP_0_10_METHOD_DTX_PREPARE,      "dtx.prepare"},
    {AMQP_0_10_METHOD_DTX_RECOVER,      "dtx.recover"},
    {AMQP_0_10_METHOD_DTX_ROLLBACK,     "dtx.rollback"},
    {AMQP_0_10_METHOD_DTX_SET_TIMEOUT,  "dtx.set-timeout"},
    {0, NULL}
};

static const value_string amqp_0_10_exchange_methods [] = {
    {AMQP_0_10_METHOD_EXCHANGE_DECLARE,  "exchange.declare"},
    {AMQP_0_10_METHOD_EXCHANGE_DELETE,   "exchange.delete"},
    {AMQP_0_10_METHOD_EXCHANGE_QUERY,    "exchange.query"},
    {AMQP_0_10_METHOD_EXCHANGE_BIND,     "exchange.bind"},
    {AMQP_0_10_METHOD_EXCHANGE_UNBIND,   "exchange.unbind"},
    {AMQP_0_10_METHOD_EXCHANGE_BOUND,    "exchange.bound"},
    {0, NULL}
};

static const value_string amqp_0_10_queue_methods [] = {
    {AMQP_0_10_METHOD_QUEUE_DECLARE,  "queue.declare"},
    {AMQP_0_10_METHOD_QUEUE_DELETE,   "queue.delete"},
    {AMQP_0_10_METHOD_QUEUE_PURGE,    "queue.purge"},
    {AMQP_0_10_METHOD_QUEUE_QUERY,    "queue.query"},
    {0, NULL}
};

static const value_string amqp_0_10_file_methods [] = {
    {AMQP_0_10_METHOD_FILE_QOS,         "file.qos"},
    {AMQP_0_10_METHOD_FILE_QOS_OK,      "file.qos-ok"},
    {AMQP_0_10_METHOD_FILE_CONSUME,     "file.consume"},
    {AMQP_0_10_METHOD_FILE_CONSUME_OK,  "file.consume-ok"},
    {AMQP_0_10_METHOD_FILE_CANCEL,      "file.cancel"},
    {AMQP_0_10_METHOD_FILE_OPEN,        "file.open"},
    {AMQP_0_10_METHOD_FILE_OPEN_OK,     "file.open-ok"},
    {AMQP_0_10_METHOD_FILE_STAGE,       "file.stage"},
    {AMQP_0_10_METHOD_FILE_PUBLISH,     "file.publish"},
    {AMQP_0_10_METHOD_FILE_RETURN,      "file.return"},
    {AMQP_0_10_METHOD_FILE_DELIVER,     "file.deliver"},
    {AMQP_0_10_METHOD_FILE_ACK,         "file.ack"},
    {AMQP_0_10_METHOD_FILE_REJECT,      "file.reject"},
    {0, NULL}
};

static const value_string amqp_0_10_stream_methods [] = {
    {AMQP_0_10_METHOD_STREAM_QOS,         "stream.qos"},
    {AMQP_0_10_METHOD_STREAM_QOS_OK,      "stream.qos-ok"},
    {AMQP_0_10_METHOD_STREAM_CONSUME,     "stream.consume"},
    {AMQP_0_10_METHOD_STREAM_CONSUME_OK,  "stream.consume-ok"},
    {AMQP_0_10_METHOD_STREAM_CANCEL,      "stream.cancel"},
    {AMQP_0_10_METHOD_STREAM_PUBLISH,     "stream.publish"},
    {AMQP_0_10_METHOD_STREAM_RETURN,      "stream.return"},
    {AMQP_0_10_METHOD_STREAM_DELIVER,     "stream.deliver"},
    {0, NULL}
};

static const value_string amqp_0_10_method_connection_close_reply_codes [] = {
    {200,   "normal"},
    {320,   "connection-forced"},
    {402,   "invalid-path"},
    {501,   "framing-error"},
    {0, NULL}
};

static const true_false_string amqp_0_10_session_header_sync = {
    "notification requested", "notification NOT requested"
};

static const value_string amqp_0_10_method_session_detached_codes [] = {
    {0,    "normal"},
    {1,    "session-busy"},
    {2,    "transport-busy"},
    {3,    "not-attached"},
    {4,    "unknown-ids"},
    {0, NULL}
};

static const value_string amqp_0_10_method_execution_exception_errors [] = {
    {403,   "unauthorized-access"},
    {404,   "not-found"},
    {405,   "resource-locked"},
    {406,   "precondition-failed"},
    {408,   "resource-deleted"},
    {409,   "illegal-state"},
    {503,   "command-invalid"},
    {506,   "resource-limit-exceeded"},
    {530,   "not-allowed"},
    {531,   "illegal-argument"},
    {540,   "not-implemented"},
    {541,   "internal-error"},
    {542,   "invalid-argument"},
    {0, NULL}
};

static const value_string amqp_0_10_message_transfer_accept_modes [] = {
    {0,    "explicit"},
    {1,    "none"},
    {0, NULL}
};

static const value_string amqp_0_10_message_transfer_acquire_modes [] = {
    {0,    "pre-acquired"},
    {1,    "not-acquired"},
    {0, NULL}
};

static const value_string amqp_0_10_message_transfer_reject_codes [] = {
    {0,    "unspecified"},
    {1,    "unroutable"},
    {2,    "immediate"},
    {0, NULL}
};

static const value_string amqp_0_10_message_flow_modes [] = {
    {0,    "credit"},
    {1,    "window"},
    {0, NULL}
};

static const value_string amqp_0_10_message_credit_units [] = {
    {0,    "message"},
    {1,    "byte"},
    {0, NULL}
};

static const value_string amqp_0_10_xa_status [] = {
    {0,    "Normal execution completion. (xa-ok)"},
    {1,    "The rollback was caused for an unspecified reason. (xa-rbrollback)"},
    {2,    "A transaction branch took too long. (xa-rbtimeout)"},
    {3,    "The transaction branch may have been heuristically completed. (xa-heurhaz)"},
    {4,    "The transaction branch has been heuristically committed. (xa-heurcom)"},
    {5,    "The transaction branch has been heuristically rolled back. (xa-heurrb)"},
    {6,    "The transaction branch has been heuristically committed and rolled back. (xa-heurmix)"},
    {7,    "The transaction branch was read-only and has been committed. (xa-rdonly)"},
    {0, NULL}
};

static const value_string amqp_0_10_struct_delivery_properties_priorities [] = {
    {0,    "lowest"},
    {1,    "lower"},
    {2,    "low"},
    {3,    "below-average"},
    {4,    "medium"},
    {5,    "above-average"},
    {6,    "high"},
    {7,    "higher"},
    {8,    "very-high"},
    {9,    "highest"},
    {0, NULL}
};

static const value_string amqp_0_10_struct_delivery_properties_modes [] = {
    {1,    "non-persistent"},
    {2,    "persistent"},
    {0, NULL}
};

static const value_string amqp_0_10_file_return_codes [] = {
    {311,    "content-too-large"},
    {312,    "no-route"},
    {313,    "no-consumers"},
    {0, NULL}
};

static const value_string amqp_0_10_stream_return_codes [] = {
    {311,    "content-too-large"},
    {312,    "no-route"},
    {313,    "no-consumers"},
    {0, NULL}
};

static const value_string amqp_0_10_struct32_vals[] = {
    { ((AMQP_0_10_CLASS_MESSAGE << 8) | (AMQP_0_10_STRUCT_MESSAGE_DELIVERY_PROPERTIES)), "message.delivery-properties" },
    { ((AMQP_0_10_CLASS_MESSAGE << 8) | (AMQP_0_10_STRUCT_MESSAGE_FRAGMENT_PROPERTIES)), "message.fragment-properties" },
    { ((AMQP_0_10_CLASS_MESSAGE << 8) | (AMQP_0_10_STRUCT_MESSAGE_MESSAGE_PROPERTIES)), "message.message-properties" },
    { ((AMQP_0_10_CLASS_MESSAGE << 8) | (AMQP_0_10_STRUCT_MESSAGE_ACQUIRED)), "message.acquired" },
    { ((AMQP_0_10_CLASS_MESSAGE << 8) | (AMQP_0_10_STRUCT_MESSAGE_RESUME_RESULT)), "message.resume-result" },
    { ((AMQP_0_10_CLASS_DTX << 8) | (AMQP_0_10_STRUCT_DTX_XA_RESULT)), "dtx.xa-status" },
    { ((AMQP_0_10_CLASS_DTX << 8) | (AMQP_0_10_STRUCT_DTX_RECOVER_RESULT)), "dtx.recover-result" },
    { ((AMQP_0_10_CLASS_EXCHANGE << 8) | (AMQP_0_10_STRUCT_EXCHANGE_QUERY_RESULT)), "exchange.exchange-query-result" },
    { ((AMQP_0_10_CLASS_EXCHANGE << 8) | (AMQP_0_10_STRUCT_EXCHANGE_BOUND_RESULT)), "exchange.exchange-bound-result" },
    { ((AMQP_0_10_CLASS_QUEUE << 8) | (AMQP_0_10_STRUCT_QUEUE_QUERY_RESULT)), "queue.queue-query-result" },
    { ((AMQP_0_10_CLASS_FILE << 8) | (AMQP_0_10_STRUCT_FILE_PROPERTIES)), "file.file-properties" },
    { ((AMQP_0_10_CLASS_STREAM << 8) | (AMQP_0_10_STRUCT_STREAM_PROPERTIES)), "stream.stream-properties" },
    { 0, NULL }
};

static const value_string amqp_0_9_frame_types [] = {
    {AMQP_0_9_FRAME_TYPE_METHOD,             "Method"},
    {AMQP_0_9_FRAME_TYPE_CONTENT_HEADER,     "Content header"},
    {AMQP_0_9_FRAME_TYPE_CONTENT_BODY,       "Content body"},
    {AMQP_0_9_FRAME_TYPE_OOB_METHOD,         "OOB Method"},
    {AMQP_0_9_FRAME_TYPE_OOB_CONTENT_HEADER, "OOB Content header"},
    {AMQP_0_9_FRAME_TYPE_OOB_CONTENT_BODY,   "OOB Content body"},
    {AMQP_0_9_FRAME_TYPE_TRACE ,             "Trace"},
    {AMQP_0_9_FRAME_TYPE_HEARTBEAT,          "Heartbeat"},
    {0, NULL}
};

static const value_string amqp_0_9_method_classes [] = {
    {AMQP_0_9_CLASS_CONNECTION, "Connection"},
    {AMQP_0_9_CLASS_CHANNEL,    "Channel"},
    {AMQP_0_9_CLASS_ACCESS,     "Access"},
    {AMQP_0_9_CLASS_EXCHANGE,   "Exchange"},
    {AMQP_0_9_CLASS_QUEUE,      "Queue"},
    {AMQP_0_9_CLASS_BASIC,      "Basic"},
    {AMQP_0_9_CLASS_FILE,       "File"},
    {AMQP_0_9_CLASS_STREAM,     "Stream"},
    {AMQP_0_9_CLASS_TX,         "Tx"},
    {AMQP_0_9_CLASS_DTX,        "Dtx"},
    {AMQP_0_9_CLASS_TUNNEL,     "Tunnel"},
    {AMQP_0_9_CLASS_CONFIRM,    "Confirm"},
    {0, NULL}
};

static const value_string amqp_method_connection_methods [] = {
    {10, "Start"},
    {11, "Start-Ok"},
    {20, "Secure"},
    {21, "Secure-Ok"},
    {30, "Tune"},
    {31, "Tune-Ok"},
    {40, "Open"},
    {41, "Open-Ok"},
    {42, "Redirect"},
    {50, "Close"},
    {51, "Close-Ok"},
    {60, "Blocked"},
    {61, "Unblocked"},
    {0, NULL}
};

static const value_string amqp_method_channel_methods [] = {
    {10, "Open"},
    {11, "Open-Ok"},
    {20, "Flow"},
    {21, "Flow-Ok"},
    {40, "Close"},
    {41, "Close-Ok"},
    {50, "Resume"},
    {60, "Ping"},
    {70, "Pong"},
    {80, "Ok"},
    {0, NULL}
};

static const value_string amqp_method_access_methods [] = {
    {10, "Request"},
    {11, "Request-Ok"},
    {0, NULL}
};

static const value_string amqp_method_exchange_methods [] = {
    {10, "Declare"},
    {11, "Declare-Ok"},
    {20, "Delete"},
    {21, "Delete-Ok"},
    {30, "Bind"},
    {31, "Bind-Ok"},
    {40, "Unbind"},
    {41, "Unbind-Ok"},
    {0, NULL}
};

static const value_string amqp_method_queue_methods [] = {
    {10, "Declare"},
    {11, "Declare-Ok"},
    {20, "Bind"},
    {21, "Bind-Ok"},
    {50, "Unbind"},
    {51, "Unbind-Ok"},
    {30, "Purge"},
    {31, "Purge-Ok"},
    {40, "Delete"},
    {41, "Delete-Ok"},
    {0, NULL}
};

static const value_string amqp_method_basic_methods [] = {
    {10, "Qos"},
    {11, "Qos-Ok"},
    {20, "Consume"},
    {21, "Consume-Ok"},
    {30, "Cancel"},
    {31, "Cancel-Ok"},
    {40, "Publish"},
    {50, "Return"},
    {60, "Deliver"},
    {70, "Get"},
    {71, "Get-Ok"},
    {72, "Get-Empty"},
    {80, "Ack"},
    {90, "Reject"},
    /* basic(100) is in 0-9 called Recover and in 0-9-1 Recover.Async,
     * we will use the more recent 0-9-1 terminology */
    {100, "Recover-Async"},
    {110, "Recover"},
    {111, "Recover-Ok"},
    {120, "Nack"},
    {0, NULL}
};

static const value_string amqp_method_file_methods [] = {
    {10, "Qos"},
    {11, "Qos-Ok"},
    {20, "Consume"},
    {21, "Consume-Ok"},
    {30, "Cancel"},
    {31, "Cancel-Ok"},
    {40, "Open"},
    {41, "Open-Ok"},
    {50, "Stage"},
    {60, "Publish"},
    {70, "Return"},
    {80, "Deliver"},
    {90, "Ack"},
    {100, "Reject"},
    {0, NULL}
};

static const value_string amqp_method_stream_methods [] = {
    {10, "Qos"},
    {11, "Qos-Ok"},
    {20, "Consume"},
    {21, "Consume-Ok"},
    {30, "Cancel"},
    {31, "Cancel-Ok"},
    {40, "Publish"},
    {50, "Return"},
    {60, "Deliver"},
    {0, NULL}
};

static const value_string amqp_method_tx_methods [] = {
    {10, "Select"},
    {11, "Select-Ok"},
    {20, "Commit"},
    {21, "Commit-Ok"},
    {30, "Rollback"},
    {31, "Rollback-Ok"},
    {0, NULL}
};

static const value_string amqp_method_dtx_methods [] = {
    {10, "Select"},
    {11, "Select-Ok"},
    {20, "Start"},
    {21, "Start-Ok"},
    {0, NULL}
};

static const value_string amqp_method_tunnel_methods [] = {
    {10, "Request"},
    {0, NULL}
};

static const value_string amqp_0_10_array_type_vals [] = {
    {AMQP_0_10_TYPE_STR16, "str16"},
    {AMQP_0_10_TYPE_STRUCT32, "struct32"},
    {0, NULL}
};

static const value_string amqp_method_confirm_methods [] = {
    {10, "Select"},
    {11, "Select-Ok"},
    {0, NULL}
};

/*  AMQP 0-10 Type Info  */
static const struct amqp_typeinfo amqp_0_10_fixed_types[] = {
    { 0x00, "bin8",    format_amqp_0_10_bin,     1 },
    { 0x01, "int8",    format_amqp_0_10_int,     1 },
    { 0x02, "uint8",   format_amqp_0_10_uint,    1 },
    { 0x04, "char",    format_amqp_0_10_char,    1 },
    { 0x08, "boolean", format_amqp_0_10_boolean, 1 },
    { 0x10, "bin16",   format_amqp_0_10_bin,     2 },
    { 0x11, "int16",   format_amqp_0_10_int,     2 },
    { 0x12, "uint16",  format_amqp_0_10_uint,    2 },
    { 0x20, "bin32",   format_amqp_0_10_bin,     4 },
    { 0x21, "int32",   format_amqp_0_10_int,     4 },
    { 0x22, "uint32",  format_amqp_0_10_uint,    4 },
    { 0xff, "end", 0, 0 }
};

static const struct amqp_typeinfo amqp_0_10_var_types[] = {
    { 0x80, "vbin8",   format_amqp_0_10_vbin, 1 },
    { 0x95, "str16",   format_amqp_0_10_str, 2 },
    { 0xff, "end", 0, 0 }
};

/*  AMQP 1.0 Type Info  */
static const struct amqp1_typeinfo amqp_1_0_fixed_types[] = {
    { 0x40, "null",       FT_NONE,          0,  dissect_amqp_1_0_skip,      format_amqp_1_0_null },
    { 0x41, "bool",       FT_BOOLEAN,       0,  dissect_amqp_1_0_true,      format_amqp_1_0_boolean_true },
    { 0x42, "bool",       FT_BOOLEAN,       0,  dissect_amqp_1_0_false,     format_amqp_1_0_boolean_false },
    { 0x56, "bool",       FT_BOOLEAN,       1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_boolean },
    { 0x50, "ubyte",      FT_UINT8,         1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_uint },
    { 0x60, "ushort",     FT_UINT16,        2,  dissect_amqp_1_0_fixed,     format_amqp_1_0_uint },
    { 0x70, "uint",       FT_UINT32,        4,  dissect_amqp_1_0_fixed,     format_amqp_1_0_uint },
    { 0x52, "smalluint",  FT_UINT8,         1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_uint },
    { 0x43, "uint0",      FT_UINT8,         0,  dissect_amqp_1_0_zero,      format_amqp_1_0_uint },
    { 0x80, "ulong",      FT_UINT64,        8,  dissect_amqp_1_0_fixed,     format_amqp_1_0_uint },
    { 0x53, "smallulong", FT_UINT8,         1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_uint },
    { 0x44, "ulong0",     FT_UINT8,         0,  dissect_amqp_1_0_zero,      format_amqp_1_0_uint },
    { 0x51, "byte",       FT_INT8,          1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_int },
    { 0x61, "short",      FT_INT16,         2,  dissect_amqp_1_0_fixed,     format_amqp_1_0_int },
    { 0x71, "int",        FT_INT32,         4,  dissect_amqp_1_0_fixed,     format_amqp_1_0_int },
    { 0x54, "smallint",   FT_INT8,          1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_int },
    { 0x81, "long",       FT_INT64,         8,  dissect_amqp_1_0_fixed,     format_amqp_1_0_int },
    { 0x55, "smalllong",  FT_INT8,          1,  dissect_amqp_1_0_fixed,     format_amqp_1_0_int },
    { 0x72, "float",      FT_FLOAT,         4,  dissect_amqp_1_0_fixed,     format_amqp_1_0_float },
    { 0x82, "double",     FT_DOUBLE,        8,  dissect_amqp_1_0_fixed,     format_amqp_1_0_double },
    { 0x74, "decimal32",  FT_BYTES,         4,  dissect_amqp_1_0_fixed,     format_amqp_1_0_decimal },
    { 0x84, "decimal64",  FT_BYTES,         8,  dissect_amqp_1_0_fixed,     format_amqp_1_0_decimal },
    { 0x94, "decimal128", FT_BYTES,         16, dissect_amqp_1_0_fixed,     format_amqp_1_0_decimal },
    { 0x73, "char",       FT_STRING,        4,  dissect_amqp_1_0_fixed,     format_amqp_1_0_char },
    { 0x83, "timestamp",  FT_ABSOLUTE_TIME, 8,  dissect_amqp_1_0_timestamp, format_amqp_1_0_timestamp },
    { 0x98, "uuid",       FT_GUID,          16, dissect_amqp_1_0_fixed,     format_amqp_1_0_uuid },
    { 0xa0, "vbin8",      FT_BYTES,         1,  dissect_amqp_1_0_variable,  format_amqp_1_0_bin },
    { 0xb0, "vbin32",     FT_BYTES,         4,  dissect_amqp_1_0_variable,  format_amqp_1_0_bin },
    { 0xa1, "str8-utf8",  FT_STRING,        1,  dissect_amqp_1_0_variable,  format_amqp_1_0_str },
    { 0xb1, "str32-utf8", FT_STRING,        4,  dissect_amqp_1_0_variable,  format_amqp_1_0_str },
    { 0xa3, "sym8",       FT_STRING,        1,  dissect_amqp_1_0_variable,  format_amqp_1_0_symbol },
    { 0xb3, "sym32",      FT_STRING,        4,  dissect_amqp_1_0_variable,  format_amqp_1_0_symbol },
    { 0xff, "end", 0, 0, 0, 0 }
};

/* see explanation at declaration of amqp_defined_types_t */
static const struct amqp_defined_types_t amqp_1_0_defined_types[] = {
    {AMQP_1_0_AMQP_TYPE_ERROR,                  &hf_amqp_1_0_error,                 3, amqp_1_0_error_items },
    {AMQP_1_0_AMQP_TYPE_HEADER,                 &hf_amqp_1_0_messageHeader,         5, amqp_1_0_messageHeader_items },
    {AMQP_1_0_AMQP_TYPE_DELIVERY_ANNOTATIONS,   &hf_amqp_1_0_deliveryAnnotations,   0, NULL },
    {AMQP_1_0_AMQP_TYPE_MESSAGE_ANNOTATIONS,    &hf_amqp_1_0_messageAnnotations,    0, NULL },
    {AMQP_1_0_AMQP_TYPE_PROPERTIES,             &hf_amqp_1_0_messageProperties,     13, amqp_1_0_messageProperties_items },
    {AMQP_1_0_AMQP_TYPE_APPLICATION_PROPERTIES, &hf_amqp_1_0_applicationProperties, 0, NULL },
    {AMQP_1_0_AMQP_TYPE_DATA,                   &hf_amqp_1_0_data,                  0, NULL },
    {AMQP_1_0_AMQP_TYPE_AMQP_SEQUENCE,          &hf_amqp_1_0_amqp_sequence,         0, NULL },
    {AMQP_1_0_AMQP_TYPE_AMQP_VALUE,             &hf_amqp_1_0_amqp_value,            0, NULL },
    {AMQP_1_0_AMQP_TYPE_FOOTER,                 &hf_amqp_1_0_footer,                0, NULL },
    {AMQP_1_0_AMQP_TYPE_RECEIVED,               &hf_amqp_1_0_received,              2, amqp_1_0_received_items },
    {AMQP_1_0_AMQP_TYPE_ACCEPTED,               &hf_amqp_1_0_accepted,              0, NULL },
    {AMQP_1_0_AMQP_TYPE_REJECTED,               &hf_amqp_1_0_rejected,              1, amqp_1_0_rejected_items },
    {AMQP_1_0_AMQP_TYPE_RELEASED,               &hf_amqp_1_0_released,              0, NULL },
    {AMQP_1_0_AMQP_TYPE_MODIFIED,               &hf_amqp_1_0_modified,              3, amqp_1_0_modified_items },
    {AMQP_1_0_AMQP_TYPE_SOURCE,                 &hf_amqp_1_0_source,                11, amqp_1_0_source_items },
    {AMQP_1_0_AMQP_TYPE_TARGET,                 &hf_amqp_1_0_target,                7, amqp_1_0_target_items },
    {AMQP_1_0_AMQP_TYPE_DELETE_ON_CLOSE,        &hf_amqp_1_0_deleteOnClose,         0, NULL },
    {AMQP_1_0_AMQP_TYPE_DELETE_ON_NO_LINKS,     &hf_amqp_1_0_deleteOnNoLinks,       0, NULL },
    {AMQP_1_0_AMQP_TYPE_DELETE_ON_NO_MESSAGE,   &hf_amqp_1_0_deleteOnNoMessages,    0, NULL },
    {AMQP_1_0_AMQP_TYPE_DELETE_ON_NO_LINKS_OR_MESSAGE, &hf_amqp_1_0_deleteOnNoLinksOrMessages, 0, NULL },
    {AMQP_1_0_AMQP_TYPE_COORDINATOR,            &hf_amqp_1_0_coordinator,           1, amqp_1_0_coordinator_items },
    {AMQP_1_0_AMQP_TYPE_DECLARE,                &hf_amqp_1_0_declare,               1, amqp_1_0_declare_items },
    {AMQP_1_0_AMQP_TYPE_DISCHARGE,              &hf_amqp_1_0_discharge,             2, amqp_1_0_discharge_items },
    {AMQP_1_0_AMQP_TYPE_DECLARED,               &hf_amqp_1_0_declared,              1, amqp_1_0_declared_items },
    {AMQP_1_0_AMQP_TYPE_TRANSACTIONAL_STATE,    &hf_amqp_1_0_transactionalState,    2, amqp_1_0_transactionalState_items },
    { 0, NULL, 0, NULL }
};

static void
check_amqp_version(tvbuff_t *tvb, amqp_conv *conn)
{
    uint32_t f0_9_length;

    /*
     * If we already know and the version and this isn't a protocol header,
     * return ok. 0-10 and up can run protocol headers in each direction,
     * so if it looks like a protocol header, snag the version even if one
     * is already recorded. Multi-protocol brokers can negotiate down.
     */
    if ((conn->version != 0) && (tvb_get_uint8(tvb, 0) != 'A'))
        return;

    if (tvb_memeql(tvb, 0, (const uint8_t*)"AMQP", 4) == 0) {
        /* AMQP 0-* has protocol major/minor in 6th/7th byte, while AMQP 1.0
         * has it in 5th/6th byte (7th is revision)
         */
        uint8_t fivth_byte;
        uint8_t sixth_byte;
        uint8_t seventh_byte;

        fivth_byte = tvb_get_uint8(tvb, 5);
        sixth_byte = tvb_get_uint8(tvb, 6);
        seventh_byte = tvb_get_uint8(tvb, 7);
        if ((fivth_byte == 1) && (sixth_byte == 0) && (seventh_byte == 0))
            conn->version = AMQP_V1_0;
        else if (sixth_byte == 0) {
            if (seventh_byte == 9)
                conn->version = AMQP_V0_9;
            else if (seventh_byte == 10)
                conn->version = AMQP_V0_10;
        }
        return;
    }

    /*
     * It's not a protocol header and the AMQP version isn't known. Try to
     * deduce it from the content.
     *
     * First indicator is the frame length. 0-9 has a 32-bit length in
     * octets 3-7. In 0-10, those are the second octet of the segment type,
     * one reserved octet that should always be zero, a four-bit track number
     * (high bits zero), and the first octet of the 16-bit channel number.
     * In 1.0, those are the lowest-value octet of the 32-bit frame size,
     * an octet for data offset (at least 2), a type code octet (0x00 for
     * an AMQP frame, 0x01 for a SASL frame), and the first of two
     * type-specific octets in the frame header.
     *
     * If the frame fits within the PDU, and there's a frame end byte (0xCE)
     * where it should be, this is almost certainly 0-9.  (Compare with "less
     * than or equal to", as there may be more than one frame in a PDU.)
     *
     * Else, higher version. 0-10 has 5th octet 0x00 while 1.0
     * has there at least 2 (DOFF) - use this fact to determine.
     */
    f0_9_length = tvb_get_ntohl(tvb, 3) + 7 + 1; /* Add header and end */
    if ((f0_9_length <= tvb_reported_length(tvb)) &&
        (tvb_get_uint8(tvb, f0_9_length - 1) == 0xCE))
        conn->version = AMQP_V0_9;
    else if (tvb_get_uint8(tvb, 4) == 0x00)
        conn->version = AMQP_V0_10;
    else
        conn->version = AMQP_V1_0;
    return;
}


static unsigned
get_amqp_1_0_message_len(packet_info *pinfo _U_, tvbuff_t *tvb,
                         int offset, void *data _U_)
{
    /*  Heuristic - protocol initialisation frame starts with 'AMQP'  */
    if (tvb_memeql(tvb, offset, (const uint8_t*)"AMQP", 4) == 0)
        return 8;
    return (unsigned) tvb_get_ntohl(tvb, offset);
}

static unsigned
get_amqp_0_10_message_len(packet_info *pinfo _U_, tvbuff_t *tvb,
                          int offset, void *data _U_)
{
    /*  Heuristic - protocol initialisation frame starts with 'AMQP'  */
    if (tvb_memeql(tvb, offset, (const uint8_t*)"AMQP", 4) == 0)
        return 8;

    return (unsigned) tvb_get_ntohs(tvb, offset + 2); /*  Max *frame* length = 65K; */
}

static unsigned
get_amqp_0_9_message_len(packet_info *pinfo _U_, tvbuff_t *tvb,
                         int offset, void *data _U_)
{
    uint32_t length;

    /*  Heuristic - protocol initialisation frame starts with 'AMQP'  */
    if (tvb_memeql(tvb, offset, (const uint8_t*)"AMQP", 4) == 0)
        return 8;

    /*
     * XXX - the location of the length differs from protocol version to
     * protocol version; for now, we only handle version 0-9, and we
     * clamp the length at 1MB so we don't go nuts if we get a bogus
     * length due to dissecting the wrong version (or getting a malformed
     * packet).
     */
    length = tvb_get_ntohl(tvb, offset + 3);
    if (length > 1048576) /* [0x100000] */
        length = 1048576;
    return length + 8;
}


/*  Dissection routine for AMQP 0-9 field tables  */

static void
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_0_9_field_table(tvbuff_t *tvb, packet_info *pinfo, int offset, unsigned length, proto_item *item)
{
    proto_tree *field_table_tree, *field_item_tree;
    proto_item *field_item;
    unsigned    namelen, vallen;
    const uint8_t *name;
    int         field_start;

    field_table_tree = proto_item_add_subtree(item, ett_amqp);

    while (length != 0) {
        field_start = offset;
        field_item = proto_tree_add_item(field_table_tree, hf_amqp_field, tvb,
                                   offset, 1, ENC_NA);
        namelen = tvb_get_uint8(tvb, offset);
        offset += 1;
        length -= 1;
        if (length < namelen)
            goto too_short;
        field_item_tree = proto_item_add_subtree(field_item, ett_amqp_0_9_field);
        proto_tree_add_item_ret_string(field_item_tree, hf_amqp_field_name, tvb, offset, namelen, ENC_UTF_8, pinfo->pool, &name);
        proto_item_set_text(field_item, "%s", name);
        offset += namelen;
        length -= namelen;

        increment_dissection_depth(pinfo);
        vallen = dissect_amqp_0_9_field_value(tvb, pinfo, offset, length, name, field_item_tree);
        decrement_dissection_depth(pinfo);
        if(vallen == 0)
            goto too_short;
        offset += vallen;
        length -= vallen;
    }
    return;

too_short:
    proto_tree_add_expert(field_table_tree, pinfo, &ei_amqp_field_short, tvb, field_start, offset - field_start);
    return;
}

/*  Dissection routine for AMQP 0-9 field arrays  */

static void
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_0_9_field_array(tvbuff_t *tvb, packet_info *pinfo, int offset, unsigned length, proto_item *item)
{
    proto_tree *field_table_tree, *field_item_tree;
    proto_item *field_item;
    int         field_start, idx;
    unsigned    vallen;
    const char *name;

    field_table_tree = proto_item_add_subtree(item, ett_amqp);
    idx = 0;

    while (length != 0) {
        field_start = offset;
        field_item = proto_tree_add_none_format(field_table_tree, hf_amqp_field, tvb,
                                   offset, 0, "[%i]", idx);
        field_item_tree = proto_item_add_subtree(field_item, ett_amqp_0_9_field);
        name = wmem_strdup_printf(pinfo->pool, "[%i]", idx);

        increment_dissection_depth(pinfo);
        vallen = dissect_amqp_0_9_field_value(tvb, pinfo, offset, length, name, field_item_tree);
        decrement_dissection_depth(pinfo);
        if(vallen == 0)
            goto too_short;
        offset += vallen;
        length -= vallen;

        idx++;
    }
    return;

too_short:
    proto_tree_add_expert(field_table_tree, pinfo, &ei_amqp_field_short, tvb, field_start, offset - field_start);
    return;
}

/* The common practice of AMQP 0-9-1 brokers and clients differs to what has
 * been described in the AMQP 0-9-1 standard.
 *
 * Here's a tabular summary of the state of things:
 * See also https://www.rabbitmq.com/amqp-0-9-1-errata.html
 *
 *   0-9   0-9-1   Industry   Type
 * --------------------------------------------
 *         t       t          Boolean
 *         b       b          Signed 8-bit
 *         B                  Unsigned 8-bit
 *         U       s          Signed 16-bit
 *         u                  Unsigned 16-bit
 *   I     I       I          Signed 32-bit
 *         i                  Unsigned 32-bit
 *         L       l          Signed 64-bit
 *         l                  Unsigned 64-bit
 *         f       f          32-bit float
 *         d       d          64-bit float
 *   D     D       D          Decimal
 *         s                  Short string
 *   S     S       S          Long string
 *         A       A          Array
 *   T     T       T          Timestamp (u64)
 *   F     F       F          Nested Table
 *   V     V       V          Void
 *                 x          Byte array
 *
 * This dissector conforms to the common practice rather than to the standard
 * and uses the tags in the third column. We don't *think* there is a vendor
 * who follows the 0-9-1 spec for this bit.
 */

static const value_string amqp_0_9_field_type_vals[] = {
    { 'A', "array" },
    { 'B', "unsigned byte" },
    { 'D', "decimal" },
    { 'F', "field table" },
    { 'I', "integer" },
    { 'S', "string" },
    { 'T', "timestamp" },
    { 'V', "void" },
    { 'b', "byte" },
    { 'd', "double" },
    { 'f', "float" },
    { 'i', "unsigned integer" },
    { 'l', "long int" },
    { 's', "short int" },
    { 't', "boolean" },
    { 'u', "short uint" },
    { 'x', "byte array" },
    { 0, NULL },
};

static unsigned
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_0_9_field_value(tvbuff_t *tvb, packet_info *pinfo, int offset, unsigned length,
                             const char *name _U_, proto_tree *field_tree)
{
    proto_item *field_item, *type_item, *ti = NULL;
    unsigned    vallen;
    uint8_t     type;
    const char *amqp_typename;
    int         value_start;

    value_start = offset;
    if (length < 1)
        return 0; /* too short */
    type = tvb_get_uint8(tvb, offset);
    amqp_typename = val_to_str_const(type, amqp_0_9_field_type_vals, "unknown type");
    field_item = proto_tree_get_parent(field_tree);
    proto_item_append_text(field_item, " (%s)", amqp_typename);
    type_item = proto_tree_add_item(field_tree, hf_amqp_field_type, tvb, offset, 1, ENC_ASCII);
    offset += 1;
    length -= 1;
    switch (type) {
    case 'I': /* signed 32-bit */
        if (length < 4)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_integer, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
        break;
    case 'D': /* 40-bit decimal floating point, biased towards small numbers */
    {
        if (length < 5)
            return 0; /* too short */
        double decimal = tvb_get_ntohl(tvb, offset + 1) / pow(10, tvb_get_uint8(tvb, offset));
        ti = proto_tree_add_double(field_tree, hf_amqp_field_decimal, tvb, offset, 5, decimal);
        offset += 5;
        break;
    }
    case 'S': /* long string, UTF-8 encoded */
        if (length < 4)
            return 0; /* too short */
        ti = proto_tree_add_item_ret_length(field_tree, hf_amqp_field_string, tvb, offset, 4, ENC_BIG_ENDIAN|ENC_UTF_8, &vallen);
        offset += vallen;
        break;
    case 'T': /* timestamp (u64) */
        if (length < 8)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_timestamp, tvb,
                            offset, 8, ENC_TIME_SECS|ENC_BIG_ENDIAN);
        offset += 8;
        break;
    case 'F': /* nested table */
        if (length < 4)
            return 0; /* too short */
        vallen  = tvb_get_ntohl(tvb, offset);
        offset += 4;
        length -= 4;
        if (length < vallen)
            return 0; /* too short */
        dissect_amqp_0_9_field_table(tvb, pinfo, offset, vallen, field_tree);
        offset += vallen;
        break;
    case 'V':
        break;
    /* AMQP 0-9-1 types */
    case 't': /* boolean */
        if (length < 1)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_boolean, tvb, offset, 1, ENC_NA);
        offset += 1;
        break;
    case 'b': /* signed 8-bit */
        if (length < 1)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_byte, tvb, offset, 1, ENC_NA);
        offset += 1;
        break;
    case 'B': /* unsigned 8-bit */
        if (length < 1)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_unsigned_byte, tvb, offset, 1, ENC_NA);
        offset += 1;
        break;
    case 's': /* signed 16-bit */
        if (length < 2)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_short_int, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;
        break;
    case 'u': /* unsigned 16-bit */
        if (length < 2)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_short_uint, tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += 2;
        break;
    case 'i': /* unsigned 32-bit */
        if (length < 4)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_unsigned_integer, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
        break;
    case 'l': /* signed 64-bit */
        if (length < 8)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_long_int, tvb, offset, 8, ENC_BIG_ENDIAN);
        offset += 8;
        break;
    case 'f': /* 32-bit float */
        if (length < 4)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_float, tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
        break;
    case 'd': /* 64-bit float */
        if (length < 8)
            return 0; /* too short */
        ti = proto_tree_add_item(field_tree, hf_amqp_field_double, tvb, offset, 8, ENC_BIG_ENDIAN);
        offset += 8;
        break;
    case 'A': /* array */
        if (length < 4)
            return 0; /* too short */
        vallen  = tvb_get_ntohl(tvb, offset);
        offset += 4;
        length -= 4;
        if (length < vallen)
            return 0; /* too short */
        dissect_amqp_0_9_field_array(tvb, pinfo, offset, vallen, ti);
        offset += vallen;
        break;
    case 'x': /* byte array */
        if (length < 4)
            return 0; /* too short */
        ti = proto_tree_add_item_ret_length(field_tree, hf_amqp_field_byte_array, tvb,
                                 offset, 4, ENC_NA, &vallen);
        offset += vallen;
        break;
    default:
        expert_add_info(pinfo, type_item, &ei_amqp_array_type_unknown);
        /* Without knowing the type, we don't know how much to increment
         * the offset, so break out. */
        return 0;
    }

    proto_item_set_end(field_item, tvb, offset);
    if (ti != NULL) {
        proto_item_append_text(field_item, ": %s", proto_item_get_display_repr(pinfo->pool, ti));
    }
    return offset - value_start;
}

/* Get amqp_0_10 32bit size field from a PDU */

/*  XXX: This is a hack.
 *  The issue: there are numerous places in the amqp_0_10 code
 *   where a 32bit size field is fetched from the PDU and
 *   then used as the size of the following data field and
 *   to advance 'offset' & etc with the potential
 *   to cause an overflow (using 32bit arithmetic).
 *  The hack: limit the size to 65K.
 *  Strictly speaking this is not OK since field sizes
 *   presumably can be larger than 65K.
 *  However: the code, as written, assumes that a field
 *   fits within an AMQP_0_10 "frame" which has, by definition, a
 *   maximum size of 65K.
 */

#define AMQP_0_10_SIZE_MAX(s) (((unsigned)(s) < (1U << 16)) ? (unsigned)s : (1U << 16))
static unsigned
amqp_0_10_get_32bit_size(tvbuff_t *tvb, int offset) {
    unsigned size = tvb_get_ntohl(tvb, offset);
    return AMQP_0_10_SIZE_MAX(size);
}

static unsigned
amqp_0_10_get_32bit_size_new(proto_tree* tree, packet_info* pinfo, tvbuff_t *tvb, int hf, int offset) {
    unsigned size;
    proto_item* ti;

    ti = proto_tree_add_item_ret_uint(tree, hf, tvb, offset, 4, ENC_BIG_ENDIAN, &size);
    if (size > 0xFFFF)
    {
        expert_add_info(pinfo, ti, &ei_amqp_size_exceeds_65K);
        size = 0xFFFF;
    }

    return size;
}

/*  Dissection routine for AMQP 0-10 maps  */

static void
dissect_amqp_0_10_map(tvbuff_t *tvb, packet_info* pinfo, proto_item *item)
{
    proto_item     *map_tree;
    unsigned        namelen, size;
    uint8_t         type;
    const char     *name;
    const char     *amqp_typename;
    const char     *value;
    uint32_t        i, field_count;
    int             offset = 0;
    type_formatter  formatter;

    map_tree = proto_item_add_subtree(item, ett_amqp_0_10_map);
    field_count = tvb_get_ntohl(tvb, offset);
    offset += 4;
    proto_item_append_text(item, " (%u %s)", field_count, plurality(field_count, "entry", "entries"));
    for (i = 0; ((i < field_count) && (tvb_reported_length_remaining(tvb, offset) > 0)); i++) {
        unsigned field_length = 0;
        unsigned field_start = offset;
        namelen = tvb_get_uint8(tvb, offset);
        offset += 1;
        name = (char*) tvb_get_string_enc(pinfo->pool, tvb, offset, namelen, ENC_UTF_8|ENC_NA);
        offset += namelen;
        type = tvb_get_uint8(tvb, offset);
        offset += 1;
        if (get_amqp_0_10_type_formatter(type, pinfo, &amqp_typename, &formatter, &size)) {
            field_length = formatter(tvb, pinfo, offset, size, &value); /* includes var 'length' field if var field */
            field_length = AMQP_0_10_SIZE_MAX(field_length);
            proto_tree_add_none_format(map_tree,
                                       hf_amqp_field,
                                       tvb,
                                       field_start,
                                       1 + namelen + 1 + field_length,
                                       "%s (%s): %s",
                                       name, amqp_typename, value);
            offset += field_length;
        }
        else {  /* type not found in table: Do special processing */
            unsigned size_field_len = 0;

            switch (type) {
            case AMQP_0_10_TYPE_MAP:
            case AMQP_0_10_TYPE_LIST:
            case AMQP_0_10_TYPE_ARRAY:
                field_length = amqp_0_10_get_32bit_size(tvb, offset);
                size_field_len = 4;
                proto_tree_add_none_format(map_tree, hf_amqp_field,
                                           tvb, field_start, (1 + namelen + 1 + 4 + field_length),
                                           "%s (composite): %d bytes",
                                           name, field_length);
                break;

            default: {   /* Determine total field length from the type */
                unsigned temp = 1U << ((type & 0x70) >> 4);  /* Map type to a length value */
                amqp_typename = "unimplemented type";

                /* fixed length cases */
                if ((type & 0x80) == 0) {
                    field_length = temp;  /* Actual length of the field */
                }
                else if ((type & 0xc0) == 0xc0) {
                    field_length = 5;
                }
                else if ((type & 0xd0) == 0xd0) {
                    field_length = 9;
                }
                else if ((type & 0xf0) == 0xf0) {
                    field_length = 0;
                }

                /* variable length/reserved cases */
                else if ((type & 0x80) == 0x80) {
                    size_field_len = temp;
                    switch (size_field_len) {
                    case 1:
                        field_length = tvb_get_uint8(tvb, offset);
                        break;
                    case 2:
                        field_length = tvb_get_ntohs(tvb, offset);
                        break;
                    case 4:
                        field_length = amqp_0_10_get_32bit_size(tvb, offset);
                        break;
                    default:
                        field_length = 1;    /* Reserved... skip 1 */
                        amqp_typename = "reserved";
                        break;
                    }
                }
                else {
                    DISSECTOR_ASSERT_NOT_REACHED();
                }
                proto_tree_add_none_format(map_tree, hf_amqp_field,
                                           tvb, field_start, 1 + namelen + 1 + size_field_len + field_length,
                                           "%s (%s): (value field length: %d bytes)",
                                           name, amqp_typename, field_length);
            } /* default */
            } /* switch (type) */

            offset += (size_field_len + field_length);
        }
    }
}

/*  Dissection routine for AMQP 0-10 maps  */
static void
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_0_10_array(tvbuff_t *tvb,
                        packet_info *pinfo,
                        int offset,          /* Start of array in tvb */
                        proto_item *item)
{
    proto_item *type_item, *struct_item;
    proto_tree *array_tree;
    uint16_t    len16;
    uint32_t    type, i, element_count;
    uint32_t    struct_length;
    tvbuff_t    *next_tvb;

    element_count = tvb_get_ntohl(tvb, offset+1);
    array_tree = proto_item_add_subtree(item, ett_amqp_0_10_array);
    proto_item_append_text(item, " (array of %u element%s)", element_count, plurality(element_count, "", "s"));
    type_item = proto_tree_add_item_ret_uint(array_tree, hf_amqp_0_10_array_type, tvb, offset, 1, ENC_NA, &type);
    offset += 1;
    proto_tree_add_item_ret_uint(array_tree, hf_amqp_0_10_array_element_count, tvb, offset, 4, ENC_BIG_ENDIAN, &element_count);
    offset += 4;

    for (i = 0; ((i < element_count) && (tvb_reported_length_remaining(tvb, offset) > 0)); i++) {
        switch (type) {
        case AMQP_0_10_TYPE_STR16:
            len16 = tvb_get_ntohs(tvb, offset);
            proto_tree_add_item(array_tree, hf_amqp_0_10_array_string, tvb, offset, 2,  ENC_UTF_8|ENC_BIG_ENDIAN);
            offset += (2 + len16);
            break;

        case AMQP_0_10_TYPE_STRUCT32:
            struct_length = amqp_0_10_get_32bit_size_new(array_tree, pinfo, tvb, hf_amqp_0_10_struct32_size, offset);
            offset += 4;

            struct_item = proto_tree_add_item(array_tree,
                                     hf_amqp_0_10_struct32,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
            proto_item_set_len(struct_item, struct_length);

            if (struct_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, struct_length);
            }
            dissect_amqp_0_10_struct32(next_tvb, pinfo, struct_item);
            offset += struct_length;
            break;

        default:
            expert_add_info(pinfo, type_item, &ei_amqp_array_type_unknown);
            /*  Don't bother continuing through the loop: we don't know how
             *  much to increment the offset by and the type doesn't change
             *  so there's nothing interesting to do...
             */
            return;
        }
    }
}

static void
dissect_amqp_0_10_xid (tvbuff_t *tvb,
                       int offset,
                       proto_item *ti)
{
    proto_item *xid_tree;
    uint8_t     flag1/*, flag2*/;

    xid_tree = proto_item_add_subtree(ti, ett_args);
    flag1 = tvb_get_uint8(tvb, offset);
    /*flag2 = tvb_get_uint8(tvb, offset+1);*/
    proto_tree_add_item(xid_tree, hf_amqp_0_10_argument_packing_flags,
                        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    if (flag1 & 0x01) {
        /*  format (uint32) */
        proto_tree_add_item(xid_tree,
                            hf_amqp_0_10_dtx_xid_format,
                            tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }
    if (flag1 & 0x02) {
        /* global-id (vbin8) */
        proto_tree_add_item(xid_tree,
                            hf_amqp_0_10_dtx_xid_global_id,
                            tvb, offset, 1, ENC_NA);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x04) {
        /* branch-id (vbin8) */
        proto_tree_add_item(xid_tree,
                            hf_amqp_0_10_dtx_xid_branch_id,
                            tvb, offset, 1, ENC_NA);
        /* offset += (1 + tvb_get_uint8(tvb, offset)); */
    }
}

/*  Dissection routine for AMQP 0-10 frames  */

static void
dissect_amqp_0_10_connection(tvbuff_t *tvb,
                             packet_info *pinfo,
                             proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;  /* args struct packing flags */
    uint32_t     arg_length;
    int          flags_offset;
    const char *method_name;
    int offset = 0;
    tvbuff_t *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_connection_methods,
                                   "<invalid connection method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo,  COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_connection_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);
    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_CONNECTION_START:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  server-properties (map)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_method_connection_start_server_properties_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_method_connection_start_server_properties,
                                     tvb,
                                     offset,
                                     arg_length, ENC_NA);
            if (arg_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, arg_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            offset += arg_length;
        }
        if (flag1 & 0x02) {
            /*  mechanisms (str16-array)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_connection_start_mechanisms_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_connection_start_mechanisms,
                                     tvb,
                                     offset,
                                     arg_length, ENC_NA);
            dissect_amqp_0_10_array (tvb,
                                     pinfo,
                                     offset,
                                     ti);
            offset += arg_length;
        }
        if (flag1 & 0x04) {
            /*  locales (str16-array)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_connection_start_locales_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_connection_start_locales,
                                     tvb,
                                     offset,
                                     arg_length, ENC_NA);
            dissect_amqp_0_10_array (tvb,
                                     pinfo,
                                     offset,
                                     ti);
            /* offset += arg_length; */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_START_OK:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  client-properties (map)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_method_connection_start_ok_client_properties_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_method_connection_start_ok_client_properties,
                                     tvb,
                                     offset,
                                     arg_length, ENC_NA);
            if (arg_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, arg_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            offset += arg_length;
        }
        if (flag1 & 0x02) {
            /*  mechanism (str8)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_start_ok_mechanism,
                                tvb, offset, 1,
                                ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {
            /*  response (vbin32)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_start_ok_response,
                                tvb, offset, 4,
                                ENC_BIG_ENDIAN);
            offset += (4 + tvb_get_ntohl(tvb, offset));
        }
        if (flag1 & 0x08) {
            /*  locale (str8)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_start_ok_locale,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_SECURE:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  challenge (vbin32)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_secure_challenge,
                                tvb, offset, 4,
                                ENC_BIG_ENDIAN);
            /* offset += (4 + tvb_get_ntohl(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_SECURE_OK:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  response (vbin32)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_secure_ok_response,
                                tvb, offset, 4,
                                ENC_BIG_ENDIAN);
            /* offset += (4 + tvb_get_ntohl(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_TUNE:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  channel-max (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_tune_channel_max,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x02) {
            /*  max-frame-size (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_connection_tune_frame_max,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x04) {
            /*  heartbeat-min (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_connection_tune_heartbeat_min,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x08) {
            /*  heartbeat-max (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_connection_tune_heartbeat_max,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            /* offset += 2; */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_TUNE_OK:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  channel-max (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_tune_ok_channel_max,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x02) {
            /*  max-frame-size (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_connection_tune_ok_frame_max,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x04) {
            /*  heartbeat (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_tune_ok_heartbeat,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            /* offset += 2; */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_OPEN:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  virtual-host (str8)  */
            proto_tree_add_item(args_tree,
                                     hf_amqp_method_connection_open_virtual_host,
                                     tvb,
                                     offset,
                                     1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {
            /*  capabilities (str16-array)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_connection_open_capabilities_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_connection_open_capabilities,
                                     tvb,
                                     offset,
                                     arg_length, ENC_ASCII);
            dissect_amqp_0_10_array (tvb,
                                     pinfo,
                                     offset,
                                     ti);
            /* offset += arg_length; */
        }
        /*
         * 3rd argument is an optional bit, insist.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_connection_open_insist,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_CONNECTION_OPEN_OK:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  known-hosts (amqp-host-array)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_connection_open_ok_known_hosts_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_connection_open_ok_known_hosts,
                                     tvb,
                                     offset,
                                     arg_length, ENC_NA);
            dissect_amqp_0_10_array (tvb,
                                     pinfo,
                                     offset,
                                     ti);
            /* offset += arg_length; */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_REDIRECT:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  host (amqp-host-url [str16])  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_redirect_host,
                                tvb, offset, 2,
                                ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (2 + tvb_get_ntohs(tvb, offset));
        }
        if (flag1 & 0x02) {
            /*  known-hosts (amqp-host-array)  */
            arg_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_connection_redirect_known_hosts_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_connection_redirect_known_hosts,
                                     tvb,
                                     offset,
                                     arg_length, ENC_NA);
            dissect_amqp_0_10_array (tvb,
                                     pinfo,
                                     offset,
                                     ti);
            /* offset += arg_length; */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_HEARTBEAT:
        break;

    case AMQP_0_10_METHOD_CONNECTION_CLOSE:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  reply-code (uint16)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_connection_close_reply_code,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x02) {
            /*  reply-text (str8)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_method_connection_close_reply_text,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset + (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_CONNECTION_CLOSE_OK:
        break;
    }
}

static void
dissect_amqp_0_10_session(tvbuff_t *tvb,
                          packet_info *pinfo,
                          proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint32_t     size;
    uint32_t     array_size;
    int          flags_offset;
    const char *method_name;
    int offset = 0;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_session_methods,
                                   "<invalid session method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_session_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);
    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_SESSION_ATTACH:
        if ((flag1 & ~0x03) || ((flag2 != 0)))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  name (vbin16)  */
            proto_tree_add_item_ret_uint(args_tree, hf_amqp_0_10_method_session_attach_name_size,
                                tvb, offset, 2, ENC_BIG_ENDIAN, &size);
            offset += 2;
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_attach_name,
                                tvb, offset, size, ENC_NA);
            /* offset += size; */
        }
        /*
         * 2nd argument is an optional bit, force.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_session_attach_force,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_SESSION_ATTACHED:
    case AMQP_0_10_METHOD_SESSION_DETACH:
        if ((flag1 != 0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  name (vbin16)  */
            proto_tree_add_item_ret_uint(args_tree, hf_amqp_0_10_method_session_attach_name_size,
                                tvb, offset, 2, ENC_BIG_ENDIAN, &size);
            offset += 2;
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_attach_name,
                                tvb, offset, size, ENC_NA);
            /* offset += size; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_DETACHED:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  name (vbin16)  */
            proto_tree_add_item_ret_uint(args_tree, hf_amqp_0_10_method_session_attach_name_size,
                                tvb, offset, 2, ENC_BIG_ENDIAN, &size);
            offset += 2;
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_attach_name,
                                tvb, offset, size, ENC_NA);
            offset += size;
        }
        if (flag1 & 0x02) {
            /*  code (detach-code [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_detached_code,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            /* offset += 1; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_REQUEST_TIMEOUT:
    case AMQP_0_10_METHOD_SESSION_TIMEOUT:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  timeout (uint32)  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_timeout,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            /* offset += 4; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_COMMAND_POINT:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  command-id (sequence-no [uint32])  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_command_point_id,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
        }
        if (flag1 & 0x02) {
            /*  command-offset (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_session_command_point_offset,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            /* offset += 8; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_EXPECTED:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  commands (commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_commands,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            offset += size;
        }
        if (flag1 & 0x02) {
            /*  fragments (command-fragments [array of command-fragment]) */
            array_size = amqp_0_10_get_32bit_size(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_fragments,
                                     tvb, offset, array_size + 4, ENC_NA);
            offset += 4;
            dissect_amqp_0_10_array(tvb,
                                    pinfo,
                                    offset,
                                    ti);
            /* offset += array_size; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_CONFIRMED:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  commands (commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_commands,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            offset += size;
        }
        if (flag1 & 0x02) {
            /*  fragments (command-fragments [array of command-fragment]) */
            array_size = amqp_0_10_get_32bit_size(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_fragments,
                                     tvb, offset, array_size + 4, ENC_NA);
            offset += 4;
            dissect_amqp_0_10_array(tvb,
                                    pinfo,
                                    offset,
                                    ti);
            /* offset += array_size; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_COMPLETED:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  commands (commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_commands,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            /* offset += size; */
        }
        /*
         * 2nd argument is an optional bit, timely-reply.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_session_completed_timely,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_SESSION_KNOWN_COMPLETED:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  commands (commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_commands,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            /* offset += size; */
        }
        break;

    case AMQP_0_10_METHOD_SESSION_FLUSH:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_session_flush_expected,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_session_flush_confirmed,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_session_flush_completed,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_SESSION_GAP:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  commands (commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_session_commands,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            /* offset += size; */
        }
        break;

    }
}

static void
dissect_amqp_0_10_execution(tvbuff_t *tvb,
                            packet_info *pinfo,
                            proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      amqp_class = 0, method;
    uint8_t      flag1, flag2;
    uint32_t     struct_size;
    int          class_hf;
    const char *method_name;
    int offset = 0;
    tvbuff_t *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_execution_methods,
                                   "<invalid execution method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_execution_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);
    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_EXECUTION_SYNC:
        if ((flag1 != 0) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        break;

    case AMQP_0_10_METHOD_EXECUTION_RESULT:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  command-id (sequence-no [uint32])  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_execution_command_id,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
        }
        if (flag1 & 0x02) {
            /*  value (struct32) */
            struct_size = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_struct32_size, offset);
            offset += 4;

            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_struct32,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
            proto_item_set_len(ti, struct_size);
            if (struct_size > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, struct_size);
            }
            dissect_amqp_0_10_struct32(next_tvb, pinfo, ti);
            /* offset += struct_size; */
        }
        break;

    case AMQP_0_10_METHOD_EXECUTION_EXCEPTION:
        if ((flag1 & ~0x7f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /* error-code (error-code [uint16]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_execution_exception_error,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x02) {
            /*  command-id (sequence-no [uint32])  */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_execution_command_id,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
        }
        if (flag1 & 0x04) {
            /*  class-code (uint8) */
            amqp_class = tvb_get_uint8(tvb, offset);
            proto_tree_add_item(args_tree, hf_amqp_0_10_class,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;
        }
        if (flag1 & 0x08) {
            /*  command-code (uint8) */
            switch(amqp_class) {
            case AMQP_0_10_CLASS_CONNECTION:
                class_hf = hf_amqp_0_10_connection_method;
                break;
            case AMQP_0_10_CLASS_SESSION:
                class_hf = hf_amqp_0_10_session_method;
                break;
            case AMQP_0_10_CLASS_EXECUTION:
                class_hf = hf_amqp_0_10_execution_method;
                break;
            case AMQP_0_10_CLASS_MESSAGE:
                class_hf = hf_amqp_0_10_message_method;
                break;
            case AMQP_0_10_CLASS_TX:
                class_hf = hf_amqp_0_10_tx_method;
                break;
            case AMQP_0_10_CLASS_DTX:
                class_hf = hf_amqp_0_10_dtx_method;
                break;
            case AMQP_0_10_CLASS_EXCHANGE:
                class_hf = hf_amqp_0_10_exchange_method;
                break;
            case AMQP_0_10_CLASS_QUEUE:
                class_hf = hf_amqp_0_10_queue_method;
                break;
            case AMQP_0_10_CLASS_FILE:
                class_hf = hf_amqp_0_10_file_method;
                break;
            case AMQP_0_10_CLASS_STREAM:
                class_hf = hf_amqp_0_10_stream_method;
                break;
            default:
                class_hf = -1;
                break;
            }
            if (class_hf != -1)
                proto_tree_add_item(args_tree, class_hf,
                                    tvb, offset, 1, ENC_BIG_ENDIAN);
            else
                expert_add_info_format(pinfo, args_tree, &ei_amqp_invalid_class_code, "Invalid class code %x", amqp_class);
            offset += 1;
        }
        if (flag1 & 0x10) {
            /*  field-index (uint8) */
            proto_tree_add_item(args_tree, hf_amqp_0_10_method_execution_field_index,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;
        }
        if (flag1 & 0x20) {
            /*  description (str16) */
            proto_tree_add_item(args_tree, hf_amqp_0_10_method_execution_description,
                                tvb, offset, 2, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (2 + tvb_get_ntohs(tvb, offset));
        }
        if (flag1 & 0x40) {
            /*  error-info (map) */
            struct_size = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_execution_error_info_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_execution_error_info,
                                     tvb,
                                     offset,
                                     struct_size, ENC_NA);
            if (struct_size > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, struct_size);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += struct_size; */
        }
        break;
    }
}

static void
dissect_amqp_0_10_message(tvbuff_t *tvb,
                          packet_info *pinfo,
                          proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint16_t     size;
    uint32_t     map_size;
    int          flags_offset;
    const char *method_name;
    int offset = 0;
    tvbuff_t    *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_message_methods,
                                   "<invalid message method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_message_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);
    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_MESSAGE_TRANSFER:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* destination (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_transfer_destination,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* accept-mode (accept-mode [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_transfer_accept_mode,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;
        }
        if (flag1 & 0x04) {     /* acquire-mode (acquire-mode [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_transfer_acquire_mode,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            /* offset += 1; */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_ACCEPT:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  transfers (session.commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_message_accept_transfers,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            /* offset += size; */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_REJECT:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  transfers (session.commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_message_accept_transfers,
                                     tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            offset += size;
        }
        if (flag1 & 0x02) {     /* reject-code (reject-code [uint16]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_transfer_reject_code,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x04) {     /* text (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_reject_text,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_RELEASE:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  transfers (session.commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_accept_transfers,
                                tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            /* offset += size; */
        }
        /*
         * 2nd argument is an optional bit, set-redelivered.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_message_release_set_redelivered,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_MESSAGE_ACQUIRE:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  transfers (session.commands [sequence-set])  */
            size = tvb_get_ntohs(tvb, offset);
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_accept_transfers,
                                tvb, offset, size + 2, ENC_NA);
            offset += 2;
            format_amqp_0_10_sequence_set(tvb, offset, size, ti);
            /* offset += size; */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_RESUME:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {
            /*  resume-id (resume-id [str16]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_resume_id,
                                tvb, offset, 2, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (2 + tvb_get_ntohs(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_SUBSCRIBE:
        if (flag2 != 0)
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_subscribe_queue,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* accept-mode (accept-mode [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_transfer_accept_mode,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;
        }
        if (flag1 & 0x08) {     /* acquire-mode (acquire-mode [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_transfer_acquire_mode,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;
        }
        /*
         * 5th argument is an optional bit, exclusive.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_message_subscribe_exclusive,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x20) {
            /*  resume-id (resume-id [str16]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_resume_id,
                                tvb, offset, 2, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (2 + tvb_get_ntohs(tvb, offset));
        }
        if (flag1 & 0x40) {
            /*  resume-ttl (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_subscribe_resume_ttl,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            offset += 8;
        }
        if (flag1 & 0x80) {
            /*  arguments (map) */
            map_size = amqp_0_10_get_32bit_size(tvb, offset);
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_message_subscribe_args,
                                     tvb,
                                     offset,
                                     4 + map_size, ENC_NA);
            if (map_size > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_size);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += (4 + map_size); */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_CANCEL:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_SET_FLOW_MODE:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {
            /*  flow-mode (flow-mode [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_flow_mode,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            /* offset += 1; */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_FLOW:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {
            /*  unit (credit-unit [uint8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_credit_unit,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            offset += 1;
        }
        if (flag1 & 0x04) {
            /*  value (uint32) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_credit_value,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            /* offset += 4; */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_FLUSH:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_MESSAGE_STOP:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {
            /*  destination (destination [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_message_dest,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;
    }
}

static void
dissect_amqp_0_10_tx(tvbuff_t *tvb,
                     packet_info *pinfo,
                     proto_tree *tree)
{
    uint8_t      method;
    uint8_t      flag1, flag2;
    const char *method_name;
    proto_item *ti;
    int offset = 1;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_tx_methods,
                                   "<invalid tx method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_tx_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    /* offset += 2; */

    /* No args on any method in this class */
}

static void
dissect_amqp_0_10_dtx(tvbuff_t *tvb,
                      packet_info *pinfo,
                      proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint16_t     xid_length;
    int          flags_offset;
    const char *method_name;
    int offset = 0;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_dtx_methods,
                                   "<invalid dtx method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_dtx_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    /* No args for dtx.select or dtx.recover */
    if ((method == AMQP_0_10_METHOD_DTX_SELECT) ||
        (method == AMQP_0_10_METHOD_DTX_RECOVER))
        return;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);

    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_DTX_START:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* xid (xid) */
            xid_length = tvb_get_ntohs(tvb, offset);
            offset += 2;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_dtx_xid,
                                     tvb,
                                     offset - 2,
                                     xid_length + 2, ENC_NA);
            dissect_amqp_0_10_xid (tvb,
                                   offset,
                                   ti);
            /* offset += xid_length; */
        }
        /*
         * 2nd, 3rd arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_dtx_start_join,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_dtx_start_resume,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);

        break;

    case AMQP_0_10_METHOD_DTX_END:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* xid (xid) */
            xid_length = tvb_get_ntohs(tvb, offset);
            offset += 2;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_dtx_xid,
                                     tvb,
                                     offset - 2,
                                     xid_length + 2, ENC_NA);
            dissect_amqp_0_10_xid (tvb,
                                   offset,
                                   ti);
            /* offset += xid_length; */
        }
        /*
         * 2nd, 3rd arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_dtx_end_fail,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_dtx_end_suspend,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_DTX_COMMIT:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* xid (xid) */
            xid_length = tvb_get_ntohs(tvb, offset);
            offset += 2;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_dtx_xid,
                                     tvb,
                                     offset - 2,
                                     xid_length + 2, ENC_NA);
            dissect_amqp_0_10_xid (tvb,
                                   offset,
                                   ti);
            /* offset += xid_length; */
        }
        /*
         * 2nd argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_dtx_commit_one_phase,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_DTX_FORGET:
    case AMQP_0_10_METHOD_DTX_GET_TIMEOUT:
    case AMQP_0_10_METHOD_DTX_PREPARE:
    case AMQP_0_10_METHOD_DTX_ROLLBACK:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* xid (xid) */
            xid_length = tvb_get_ntohs(tvb, offset);
            offset += 2;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_dtx_xid,
                                     tvb,
                                     offset - 2,
                                     xid_length + 2, ENC_NA);
            dissect_amqp_0_10_xid (tvb,
                                   offset,
                                   ti);
            /* offset += xid_length; */
        }
        break;

    case AMQP_0_10_METHOD_DTX_SET_TIMEOUT:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* xid (xid) */
            xid_length = tvb_get_ntohs(tvb, offset);
            offset += 2;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_dtx_xid,
                                     tvb,
                                     offset - 2,
                                     xid_length + 2, ENC_NA);
            dissect_amqp_0_10_xid (tvb,
                                   offset,
                                   ti);
            offset += xid_length;
        }
        if (flag1 & 0x02) {    /* timeout (uint32) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_dtx_set_timeout_timeout,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            /* offset += 2; */
        }
        break;

    }
}

static void
dissect_amqp_0_10_exchange(tvbuff_t *tvb,
                           packet_info *pinfo,
                           proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint32_t     map_length;
    int          flags_offset;
    const char *method_name;
    int offset = 0;
    tvbuff_t *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_exchange_methods,
                                   "<invalid exchange method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_exchange_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);

    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_EXCHANGE_DECLARE:
        if ((flag1 & ~0x7f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* type (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_type,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* alternate-exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_alt_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        /*
         * 4th-6th arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_exchange_declare_passive,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_exchange_declare_durable,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_exchange_declare_auto_delete,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x40) {     /* arguments (map) */
            map_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_exchange_declare_arguments_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_exchange_declare_arguments,
                                     tvb,
                                     offset,
                                     map_length, ENC_NA);
            if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += map_length; */
        }
        break;

    case AMQP_0_10_METHOD_EXCHANGE_DELETE:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        /*
         * 2nd argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_exchange_delete_if_unused,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_EXCHANGE_QUERY:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_EXCHANGE_BIND:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_bind_queue,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* binding-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_binding_key,
                                tvb, offset, 1, ENC_ASCII);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x08) {     /* arguments (map) */
            map_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_exchange_declare_arguments_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_exchange_declare_arguments,
                                     tvb,
                                     offset,
                                     map_length, ENC_NA);
            if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += map_length; */
        }
        break;

    case AMQP_0_10_METHOD_EXCHANGE_UNBIND:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_bind_queue,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* binding-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_binding_key,
                                tvb, offset, 1, ENC_ASCII);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_EXCHANGE_BOUND:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* exchange (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_declare_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_bind_queue,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* binding-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_exchange_binding_key,
                                tvb, offset, 1, ENC_ASCII);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x08) {     /* arguments (map) */
            map_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_exchange_declare_arguments_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_exchange_declare_arguments,
                                     tvb,
                                     offset,
                                     map_length, ENC_NA);
            if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += map_length; */
        }
        break;
    }
}

static void
dissect_amqp_0_10_queue(tvbuff_t *tvb,
                        packet_info *pinfo,
                        proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint32_t     map_length;
    int          flags_offset;
    const char *method_name;
    int offset = 0;
    tvbuff_t *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_queue_methods,
                                   "<invalid queue method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_queue_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);

    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_QUEUE_DECLARE:
        if ((flag1 & ~0x7f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_name,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* alternate-exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_alt_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        /*
         * 3rd-6th arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_queue_declare_passive,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_queue_declare_durable,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_queue_declare_exclusive,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_queue_declare_auto_delete,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x40) {     /* arguments (map) */
            map_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_queue_declare_arguments_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_queue_declare_arguments,
                                     tvb,
                                     offset,
                                     map_length, ENC_NA);
            if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += map_length; */
        }
        break;

    case AMQP_0_10_METHOD_QUEUE_DELETE:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_name,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        /*
         * 2nd-3rd arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_queue_delete_if_unused,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_queue_delete_if_empty,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_QUEUE_PURGE:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_name,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_QUEUE_QUERY:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_name,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;
    }
}

static void
dissect_amqp_0_10_file(tvbuff_t *tvb,
                       packet_info *pinfo,
                       proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint32_t     map_length;
    int          flags_offset;
    const char *method_name;
    int offset = 0;
    tvbuff_t    *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_file_methods,
                                   "<invalid file method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_file_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);

    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_FILE_QOS:
        if ((flag1 & ~0x07) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* prefetch-size (uint32) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_qos_prefetch_size,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
        }
        if (flag1 & 0x02) {     /* prefetch-count (uint16) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_qos_prefetch_count,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            /* offset += 2; */
        }
        /*
         * 3rd argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_qos_global,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_FILE_QOS_OK:
    case AMQP_0_10_METHOD_FILE_STAGE:
        /* No args */
        break;

    case AMQP_0_10_METHOD_FILE_CONSUME:
        if ((flag1 & ~0x7f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_name,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* consumer-tag (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_consumer_tag,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        /*
         * 3rd-6th arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_consume_no_local,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_consume_no_ack,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_consume_exclusive,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_consume_nowait,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x40) {     /* arguments (map) */
            map_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_file_consume_arguments_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_file_consume_arguments,
                                     tvb,
                                     offset,
                                     map_length, ENC_NA);
            if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += map_length; */
        }
        break;

    case AMQP_0_10_METHOD_FILE_CONSUME_OK:
    case AMQP_0_10_METHOD_FILE_CANCEL:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* consumer-tag (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_consumer_tag,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_FILE_OPEN:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* identifier (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_identifier,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* content-size (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_open_content_size,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            /* offset += 8; */
        }
        break;

    case AMQP_0_10_METHOD_FILE_OPEN_OK:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* staged-size (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_open_ok_staged_size,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            /* offset += 8; */
        }
        break;

    case AMQP_0_10_METHOD_FILE_PUBLISH:
        if ((flag1 & ~0x1f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_publish_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* routing-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_publish_routing_key,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        /*
         * 3rd-4th arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_publish_mandatory,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_publish_immediate,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x10) {     /* identifier (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_identifier,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_FILE_RETURN:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* reply-code (return-code [uint16]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_return_reply_code,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x02) {     /* reply-text (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_return_reply_text,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_return_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x08) {     /* routing-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_return_routing_key,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_FILE_DELIVER:
        if ((flag1 & ~0x3f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* consumer-tag (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_deliver_consumer_tag,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* delivery-tag (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_deliver_delivery_tag,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            offset += 8;
        }
        /*
         * 3rd argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_deliver_redelivered,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x08) {     /* exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_deliver_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x10) {     /* routing-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_deliver_routing_key,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x20) {     /* identifier (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_identifier,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_FILE_ACK:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* delivery-tag (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_ack_delivery_tag,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            /* offset += 8; */
        }
        /*
         * 2nd argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_ack_multiple,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_FILE_REJECT:
        if ((flag1 & ~0x03) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* delivery-tag (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_file_reject_delivery_tag,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            /* offset += 8; */
        }
        /*
         * 2nd argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_file_reject_requeue,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;
    }
}

static void
dissect_amqp_0_10_stream(tvbuff_t *tvb,
                         packet_info *pinfo,
                         proto_tree *tree)
{
    proto_item  *args_tree;
    proto_item  *ti;
    proto_item  *flags_item;
    uint8_t      method;
    uint8_t      flag1, flag2;
    uint32_t     map_length;
    int          flags_offset;
    const char *method_name;
    int offset = 0;
    tvbuff_t *next_tvb;

    method = tvb_get_uint8(tvb, offset+1);
    method_name = val_to_str_const(method, amqp_0_10_stream_methods,
                                   "<invalid stream method>");
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", method_name);
    col_set_fence(pinfo->cinfo, COL_INFO);

    proto_tree_add_item(tree, hf_amqp_0_10_stream_method,
                        tvb, offset+1, 1, ENC_BIG_ENDIAN);
    offset += 2;
    /*
     * Session header is 2 bytes; one that tells that it's 1 byte long, then
     * the byte itself. Bit 0 is sync.
     */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    ti = proto_tree_add_item(tree, hf_amqp_0_10_session_header, tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 != 1) || ((flag2 & 0xfe) != 0))
        proto_item_append_text(ti, " (Invalid)");
    else
        proto_tree_add_item(tree, hf_amqp_0_10_session_header_sync,
                            tvb, offset + 1, 1, ENC_BIG_ENDIAN);
    offset += 2;

    ti = proto_tree_add_item(tree, hf_amqp_method_arguments,
                             tvb, offset, -1, ENC_NA);
    args_tree = proto_item_add_subtree(ti, ett_args);

    /*
     * The flag bits are a simple bit string, not a net-byte-order
     * field. tvb_get_bits16() doesn't know how to do little-endian
     * at this time, so just pick out two bytes.
     */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;
    switch (method) {
    case AMQP_0_10_METHOD_STREAM_QOS:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* prefetch-size (uint32) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_qos_prefetch_size,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            offset += 4;
        }
        if (flag1 & 0x02) {     /* prefetch-count (uint16) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_qos_prefetch_count,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x04) {     /* consume-rate (uint32) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_qos_prefetch_size,
                                tvb, offset, 4, ENC_BIG_ENDIAN);
            /* offset += 4; */
        }
        /*
         * 4th argument is an optional bit.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_stream_qos_global,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_STREAM_QOS_OK:
        /* No args */
        break;

    case AMQP_0_10_METHOD_STREAM_CONSUME:
        if ((flag1 & ~0x3f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_queue_name,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* consumer-tag (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_consumer_tag,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        /*
         * 3rd-5th arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_stream_consume_no_local,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_stream_consume_exclusive,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_stream_consume_nowait,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        if (flag1 & 0x20) {     /* arguments (map) */
            map_length = amqp_0_10_get_32bit_size_new(args_tree, pinfo, tvb, hf_amqp_0_10_method_stream_consume_arguments_size, offset);
            offset += 4;
            ti = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_method_stream_consume_arguments,
                                     tvb,
                                     offset,
                                     map_length, ENC_NA);
            if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, map_length);
            }
            dissect_amqp_0_10_map (next_tvb, pinfo, ti);
            /* offset += map_length; */
        }
        break;

    case AMQP_0_10_METHOD_STREAM_CONSUME_OK:
    case AMQP_0_10_METHOD_STREAM_CANCEL:
        if ((flag1 & ~0x01) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* consumer-tag (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_consumer_tag,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_STREAM_PUBLISH:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_publish_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* routing-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_publish_routing_key,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        /*
         * 3rd-4th arguments are optional bits.
         */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_stream_publish_mandatory,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_stream_publish_immediate,
                            tvb, flags_offset, 1, ENC_BIG_ENDIAN);
        break;

    case AMQP_0_10_METHOD_STREAM_RETURN:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* reply-code (return-code [uint16]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_return_reply_code,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
        }
        if (flag1 & 0x02) {     /* reply-text (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_return_reply_text,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x04) {     /* exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_return_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x08) {     /* routing-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_return_routing_key,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;

    case AMQP_0_10_METHOD_STREAM_DELIVER:
        if ((flag1 & ~0x0f) || (flag2 != 0))
            expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
        if (flag1 & 0x01) {     /* consumer-tag (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_deliver_consumer_tag,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x02) {     /* delivery-tag (uint64) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_deliver_delivery_tag,
                                tvb, offset, 8, ENC_BIG_ENDIAN);
            offset += 8;
        }
        if (flag1 & 0x04) {     /* exchange (exchange.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_deliver_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (flag1 & 0x08) {     /* queue (queue.name [str8]) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_method_stream_deliver_queue,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            /* offset += (1 + tvb_get_uint8(tvb, offset)); */
        }
        break;
    }
}

static void
dissect_amqp_0_10_struct_delivery_properties(tvbuff_t *tvb,
                                             packet_info *pinfo,
                                             proto_tree *tree)
{
    proto_item *args_tree;
    proto_item *flags_item;
    uint8_t     flag1, flag2;
    uint64_t    timestamp;
    int         flags_offset;
    nstime_t    tv;
    int         offset = 0;

    args_tree = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    if (flag2 & ~0x0f)
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;

    /* First 3 fields are bits */
    proto_tree_add_item(args_tree,
                        hf_amqp_0_10_struct_delivery_properties_discard_unroutable,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(args_tree,
                        hf_amqp_0_10_struct_delivery_properties_immediate,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(args_tree,
                        hf_amqp_0_10_struct_delivery_properties_redelivered,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    if (flag1 & 0x08) {
        /* delivery-priority (delivery-priority [uint8]) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_priority,
                            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    if (flag1 & 0x10) {
        /* delivery-mode (delivery-mode [uint8]) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_mode,
                            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    if (flag1 & 0x20) {
        /* ttl (uint64) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_ttl,
                            tvb, offset, 8, ENC_BIG_ENDIAN);
        offset += 8;
    }
    if (flag1 & 0x40) {
        /* timestamp (datetime [uint64]) */
        timestamp = tvb_get_ntoh64(tvb, offset);
        tv.secs = (time_t)timestamp;
        tv.nsecs = 0;
        proto_tree_add_time(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_timestamp,
                            tvb, offset, 8, &tv);
        offset += 8;
    }
    if (flag1 & 0x80) {
        /* expiration (datetime [uint64]) */
        timestamp = tvb_get_ntoh64(tvb, offset);
        tv.secs = (time_t)timestamp;
        tv.nsecs = 0;
        proto_tree_add_time(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_expiration,
                            tvb, offset, 8, &tv);
        offset += 8;
    }
    if (flag2 & 0x01) {
        /* exchange (exchange.name [str8]) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_exchange,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag2 & 0x02) {
        /* routing-key (str8) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_routing_key,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag2 & 0x04) {
        /*  resume-id (resume-id [str16]) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_method_message_resume_id,
                            tvb, offset, 2, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (2 + tvb_get_ntohs(tvb, offset));
    }
    if (flag2 & 0x08) {
        /*  resume-ttl (uint64) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_delivery_properties_resume_ttl,
                            tvb, offset, 8, ENC_BIG_ENDIAN);
        /* offset += 8; */
    }
}

static void
dissect_amqp_0_10_struct_fragment_properties(tvbuff_t *tvb,
                                             packet_info *pinfo,
                                             proto_tree *tree)
{
    proto_item *args_tree;
    proto_item *flags_item;
    uint8_t     flag1, flag2;
    int         flags_offset;
    int         offset = 0;

    args_tree = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(args_tree,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 & ~0x07) || (flag2 != 0))
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;

    /* First 2 fields are bits */
    proto_tree_add_item(args_tree,
                        hf_amqp_0_10_struct_fragment_properties_first,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(args_tree,
                        hf_amqp_0_10_struct_fragment_properties_last,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    if (flag1 & 0x04) {
        /* fragment-size (uint64) */
        proto_tree_add_item(args_tree,
                            hf_amqp_0_10_struct_fragment_properties_size,
                            tvb, offset, 8, ENC_BIG_ENDIAN);
        /* offset += 8; */
    }
}

static void
dissect_amqp_0_10_struct_message_properties(tvbuff_t *tvb,
                                            packet_info *pinfo,
                                            proto_tree *tree)
{
    proto_item *ti;
    proto_item *frag;
    proto_item *args_tree;
    proto_item *flags_item, *subflags_item;
    uint8_t     flag1, flag2;
    uint8_t     subflag1, subflag2;
    uint16_t    len16;
    uint32_t    map_length;
    int         offset = 0;
    tvbuff_t *next_tvb;

    frag = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(frag,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    if (flag2 & ~0x01)
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;
    if (flag1 & 0x01) {
        /*  content-length (uint64) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_content_len,
                            tvb, offset, 8, ENC_BIG_ENDIAN);
        offset += 8;
    }
    if (flag1 & 0x02) {
        /* message-id (uuid) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_message_id,
                            tvb, offset, 16, ENC_BIG_ENDIAN);
        offset += 16;
    }
    if (flag1 & 0x04) {
        /* correlation-id (vbin16) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_correlation,
                            tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += (2 + tvb_get_ntohs(tvb, offset));
    }
    if (flag1 & 0x08) {
        /* reply-to (reply-to) */
        /* This is another struct, length 2, packing 2 */
        len16 = tvb_get_ntohs(tvb, offset);
        offset += 2;
        ti = proto_tree_add_item(frag,
                                 hf_amqp_0_10_struct_message_properties_reply_to,
                                 tvb, offset, len16, ENC_NA);
        args_tree = proto_item_add_subtree(ti, ett_args);
        subflags_item = proto_tree_add_item(args_tree,
                                            hf_amqp_0_10_argument_packing_flags,
                                            tvb, offset, 2, ENC_BIG_ENDIAN);
        subflag1 = tvb_get_uint8(tvb, offset);
        subflag2 = tvb_get_uint8(tvb, offset + 1);
        if ((subflag1 & ~0x03) || (subflag2 != 0))
            expert_add_info(pinfo, subflags_item, &ei_amqp_bad_flag_value);
        offset += 2;
        if (subflag1 & 0x01) {
            /* exchange (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_struct_reply_to_exchange,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
        if (subflag1 & 0x02) {
            /* routing-key (str8) */
            proto_tree_add_item(args_tree,
                                hf_amqp_0_10_struct_reply_to_routing_key,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
            offset += (1 + tvb_get_uint8(tvb, offset));
        }
    }
    if (flag1 & 0x10) {
        /* content-type (str8) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_content_type,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x20) {
        /* content-encoding (str8) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_content_encoding,
                                tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x40) {
        /* user-id (vbin16 ) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_user_id,
                            tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += (2 + tvb_get_ntohs(tvb, offset));
    }
    if (flag1 & 0x80) {
        /* app-id (vbin16 ) */
        proto_tree_add_item(frag,
                            hf_amqp_0_10_struct_message_properties_app_id,
                            tvb, offset, 2, ENC_BIG_ENDIAN);
        offset += (2 + tvb_get_ntohs(tvb, offset));
    }
    if (flag2 & 0x01) {
        /* application-headers (map) */
        map_length = amqp_0_10_get_32bit_size_new(frag, pinfo, tvb, hf_amqp_0_10_struct_message_properties_application_headers_size, offset);
        offset += 4;
        ti = proto_tree_add_item(frag,
                                 hf_amqp_0_10_struct_message_properties_application_headers,
                                 tvb,
                                 offset,
                                 map_length, ENC_NA);
        if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
        {
            next_tvb = tvb_new_subset_remaining(tvb, offset);
        }
        else
        {
            next_tvb = tvb_new_subset_length(tvb, offset, map_length);
        }
        dissect_amqp_0_10_map (next_tvb, pinfo, ti);
        /* offset += map_length; */
    }
}

static void
dissect_amqp_0_10_struct_exchange_query_result(tvbuff_t *tvb,
                                               packet_info *pinfo,
                                               proto_item *tree)
{
    proto_item *ti;
    proto_item *result;
    proto_item *flags_item;
    uint8_t     flag1, flag2;
    uint32_t    map_length;
    int         flags_offset;
    int         offset = 0;
    tvbuff_t   *next_tvb;

    result = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(result,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    if (flag2 & ~0x0f)
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;
    if (flag1 & 0x01) {
        /*  type (str8) */
        proto_tree_add_item(result,
                            hf_amqp_0_10_method_exchange_declare_type,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    proto_tree_add_item(result,
                        hf_amqp_0_10_struct_exchange_query_result_durable,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(result,
                        hf_amqp_0_10_struct_exchange_query_result_not_found,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    if (flag1 & 0x08) {
        /*  arguments (map) */
        map_length = amqp_0_10_get_32bit_size_new(result, pinfo, tvb, hf_amqp_0_10_method_exchange_declare_arguments_size, offset);
        offset += 4;
        ti = proto_tree_add_item(result,
                                 hf_amqp_0_10_method_exchange_declare_arguments,
                                 tvb,
                                 offset,
                                 map_length, ENC_NA);
        if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
        {
            next_tvb = tvb_new_subset_remaining(tvb, offset);
        }
        else
        {
            next_tvb = tvb_new_subset_length(tvb, offset, map_length);
        }
        dissect_amqp_0_10_map (next_tvb, pinfo, ti);
        /* offset += map_length; */
    }
}

static void
dissect_amqp_0_10_struct_queue_query_result(tvbuff_t *tvb,
                                            packet_info *pinfo,
                                            proto_item *tree)
{
    proto_item *ti;
    proto_item *result;
    proto_item *flags_item;
    uint8_t     flag1, flag2;
    uint32_t    map_length;
    int         flags_offset;
    int         offset = 0;
    tvbuff_t   *next_tvb;

    result = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flags_offset = offset;
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(result,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);

    if (flag2 != 0)
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;
    if (flag1 & 0x01) {
        /*  queue (name [str8]) */
        proto_tree_add_item(result,
                            hf_amqp_0_10_method_queue_name,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x02) {     /* alternate-exchange (exchange.name [str8]) */
        proto_tree_add_item(result,
                            hf_amqp_0_10_method_queue_alt_exchange,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    /*
     * 3rd-5th arguments are optional bits.
     */
    proto_tree_add_item(result,
                        hf_amqp_0_10_struct_queue_query_result_durable,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(result,
                        hf_amqp_0_10_struct_queue_query_result_exclusive,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(result,
                        hf_amqp_0_10_struct_queue_query_result_auto_delete,
                        tvb, flags_offset, 1, ENC_BIG_ENDIAN);
    if (flag1 & 0x20) {     /* arguments (map) */
        map_length = amqp_0_10_get_32bit_size_new(result, pinfo, tvb, hf_amqp_0_10_method_queue_declare_arguments_size, offset);
        offset += 4;
        ti = proto_tree_add_item(result,
                                 hf_amqp_0_10_method_queue_declare_arguments,
                                 tvb,
                                 offset,
                                 map_length, ENC_NA);
        if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
        {
            next_tvb = tvb_new_subset_remaining(tvb, offset);
        }
        else
        {
            next_tvb = tvb_new_subset_length(tvb, offset, map_length);
        }
        dissect_amqp_0_10_map (next_tvb, pinfo, ti);
        offset += map_length;
    }
    if (flag1 & 0x40) {     /* message-count (uint32) */
        proto_tree_add_item(result,
                            hf_amqp_0_10_struct_queue_query_result_message_count,
                            tvb, offset, 4, ENC_BIG_ENDIAN);
        offset += 4;
    }
    if (flag1 & 0x80) {     /* subscriber-count (uint32) */
        proto_tree_add_item(result,
                            hf_amqp_0_10_struct_queue_query_result_subscriber_count,
                            tvb, offset, 4, ENC_BIG_ENDIAN);
        /* offset += 4; */
    }
}

static void
dissect_amqp_0_10_struct_file_properties(tvbuff_t *tvb,
                                         packet_info *pinfo,
                                         proto_tree *tree)
{
    proto_item *ti;
    proto_item *props;
    proto_item *flags_item;
    uint8_t     flag1, flag2;
    uint32_t    map_length;
    uint64_t    timestamp;
    int         offset = 0;
    nstime_t    tv;
    tvbuff_t   *next_tvb;

    props = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(props,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    if (flag2 & ~0x01)
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;
    if (flag1 & 0x01) {
        /*  content-type (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_content_type,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x02) {
        /*  content-encoding (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_content_encoding,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x04) {
        /* headers (map) */
        map_length = amqp_0_10_get_32bit_size_new(props, pinfo, tvb, hf_amqp_0_10_struct_file_properties_headers_size, offset);
        offset += 4;
        ti = proto_tree_add_item(props,
                                 hf_amqp_0_10_struct_file_properties_headers,
                                 tvb,
                                 offset,
                                 map_length, ENC_NA);
        if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
        {
            next_tvb = tvb_new_subset_remaining(tvb, offset);
        }
        else
        {
            next_tvb = tvb_new_subset_length(tvb, offset, map_length);
        }
        dissect_amqp_0_10_map (next_tvb, pinfo, ti);
        offset += map_length;
    }
    if (flag1 & 0x08) {
        /* priority (uint8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_priority,
                            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    if (flag1 & 0x10) {
        /* reply-to (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_reply_to,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x20) {
        /* message-id (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_message_id,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x40) {
        /* filename (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_filename,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x80) {
        /* timestamp (datetime [uint64]) */
        timestamp = tvb_get_ntoh64(tvb, offset);
        tv.secs = (time_t)timestamp;
        tv.nsecs = 0;
        proto_tree_add_time(props,
                            hf_amqp_0_10_struct_file_properties_timestamp,
                            tvb, offset, 8, &tv);
        offset += 8;
    }
    if (flag2 & 0x01) {
        /* cluster-id (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_file_properties_cluster_id,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        /* offset += (1 + tvb_get_uint8(tvb, offset)); */
    }
}

static void
dissect_amqp_0_10_struct_stream_properties(tvbuff_t *tvb,
                                           packet_info *pinfo,
                                           proto_tree *tree)
{
    proto_item *ti;
    proto_item *props;
    proto_item *flags_item;
    uint8_t     flag1, flag2;
    uint32_t    map_length;
    uint64_t    timestamp;
    int         offset = 0;
    nstime_t    tv;
    tvbuff_t   *next_tvb;

    props = proto_item_add_subtree(tree, ett_args);
    offset += 2; /* Skip class and struct codes */
    flag1 = tvb_get_uint8(tvb, offset);
    flag2 = tvb_get_uint8(tvb, offset+1);
    flags_item = proto_tree_add_item(props,
                                     hf_amqp_0_10_argument_packing_flags,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
    if ((flag1 & ~0x1f) || (flag2 != 0))
        expert_add_info(pinfo, flags_item, &ei_amqp_bad_flag_value);
    offset += 2;
    if (flag1 & 0x01) {
        /*  content-type (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_stream_properties_content_type,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x02) {
        /*  content-encoding (str8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_stream_properties_content_encoding,
                            tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
        offset += (1 + tvb_get_uint8(tvb, offset));
    }
    if (flag1 & 0x04) {
        /* headers (map) */
        map_length = amqp_0_10_get_32bit_size_new(props, pinfo, tvb, hf_amqp_0_10_struct_stream_properties_headers_size, offset);
        offset += 4;
        ti = proto_tree_add_item(props,
                                 hf_amqp_0_10_struct_stream_properties_headers,
                                 tvb,
                                 offset,
                                 map_length, ENC_NA);
        if (map_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
        {
            next_tvb = tvb_new_subset_remaining(tvb, offset);
        }
        else
        {
            next_tvb = tvb_new_subset_length(tvb, offset, map_length);
        }
        dissect_amqp_0_10_map (next_tvb, pinfo, ti);
        offset += map_length;
    }
    if (flag1 & 0x08) {
        /* priority (uint8) */
        proto_tree_add_item(props,
                            hf_amqp_0_10_struct_stream_properties_priority,
                            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    if (flag1 & 0x10) {
        /* timestamp (datetime [uint64]) */
        timestamp = tvb_get_ntoh64(tvb, offset);
        tv.secs = (time_t)timestamp;
        tv.nsecs = 0;
        proto_tree_add_time(props,
                            hf_amqp_0_10_struct_stream_properties_timestamp,
                            tvb, offset, 8, &tv);
        /* offset += 8; */
    }
}

static void
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_0_10_struct32(tvbuff_t *tvb,
                           packet_info *pinfo,
                           proto_item *ti)
{
    uint32_t    class_code, struct_code;
    uint8_t     flag1;
    uint16_t    size;
    proto_item *ti2, *result;
    proto_tree *tree;
    int offset = 0;

    tree = proto_item_add_subtree(ti, ett_args);

    proto_tree_add_item_ret_uint(tree, hf_amqp_0_10_struct32_class, tvb, offset, 1, ENC_NA, &class_code);
    proto_tree_add_item_ret_uint(tree, hf_amqp_0_10_struct32_struct, tvb, offset+1, 1, ENC_NA, &struct_code);

    increment_dissection_depth(pinfo);

    switch(class_code) {
    case AMQP_0_10_CLASS_MESSAGE:
        switch (struct_code) {
        case AMQP_0_10_STRUCT_MESSAGE_DELIVERY_PROPERTIES:
            dissect_amqp_0_10_struct_delivery_properties(tvb,
                                                         pinfo,
                                                         tree);
            break;
        case AMQP_0_10_STRUCT_MESSAGE_FRAGMENT_PROPERTIES:
            dissect_amqp_0_10_struct_fragment_properties(tvb,
                                                         pinfo,
                                                         tree);
            break;
        case AMQP_0_10_STRUCT_MESSAGE_MESSAGE_PROPERTIES:
            dissect_amqp_0_10_struct_message_properties(tvb,
                                                        pinfo,
                                                        tree);
            break;
        case AMQP_0_10_STRUCT_MESSAGE_ACQUIRED:
            result = proto_item_add_subtree(tree, ett_args);
            offset += 2; /* Class/type codes */
            flag1 = tvb_get_uint8(tvb, offset);
            proto_tree_add_item(result, hf_amqp_0_10_argument_packing_flags,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
            if (flag1 & 0x01) {
                /*  transfers (commands [sequence-set])  */
                size = tvb_get_ntohs(tvb, offset);
                ti2 = proto_tree_add_item(result,
                                         hf_amqp_0_10_struct_acquired_transfers,
                                         tvb, offset, size + 2, ENC_NA);
                format_amqp_0_10_sequence_set(tvb, offset + 2, size, ti2);
            }
            break;
        case AMQP_0_10_STRUCT_MESSAGE_RESUME_RESULT:
            result = proto_item_add_subtree(tree, ett_args);
            offset += 2; /* Class/type codes */
            flag1 = tvb_get_uint8(tvb, offset);
            proto_tree_add_item(result, hf_amqp_0_10_argument_packing_flags,
                                tvb, offset, 2, ENC_BIG_ENDIAN);
            offset += 2;
            if (flag1 & 0x01) {
                /*  offset (uint64)  */
                proto_tree_add_item(result,
                                    hf_amqp_0_10_struct_resume_result_offset,
                                    tvb, offset, 8, ENC_BIG_ENDIAN);
            }
            break;
        }
        break;

    case AMQP_0_10_CLASS_DTX:
        switch (struct_code) {
        case AMQP_0_10_STRUCT_DTX_XA_RESULT:
            offset += 2; /* Class/type codes */
            /*flag1 = tvb_get_uint8(tvb, offset);*/
            proto_tree_add_item(tree, hf_amqp_0_10_struct32_padding, tvb, offset, 2, ENC_NA);
            offset += 2;
            proto_tree_add_item(tree, hf_amqp_0_10_dtx_xa_status, tvb, offset, 2, ENC_BIG_ENDIAN);
            break;

        case AMQP_0_10_STRUCT_DTX_RECOVER_RESULT:
            offset += 2; /* Class/type codes */
            proto_tree_add_item(tree, hf_amqp_0_10_struct32_padding, tvb, offset, 2, ENC_NA);
            offset += 2;
            amqp_0_10_get_32bit_size_new(tree, pinfo, tvb, hf_amqp_0_10_struct_dtx_recover_result_size, offset);
            offset += 4;
            dissect_amqp_0_10_array(tvb,
                                    pinfo,
                                    offset,
                                    tree);
            break;
        }
        break;

    case AMQP_0_10_CLASS_EXCHANGE:
        switch (struct_code) {
        case AMQP_0_10_STRUCT_EXCHANGE_QUERY_RESULT:
            dissect_amqp_0_10_struct_exchange_query_result(tvb,
                                                           pinfo,
                                                           tree);
            break;

        case AMQP_0_10_STRUCT_EXCHANGE_BOUND_RESULT:
            result = proto_item_add_subtree(tree, ett_args);
            offset += 2; /* Class/type codes */
            proto_tree_add_item(result,
                                hf_amqp_0_10_struct_exchange_bound_result_exchange_not_found,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(result,
                                hf_amqp_0_10_struct_exchange_bound_result_queue_not_found,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(result,
                                hf_amqp_0_10_struct_exchange_bound_result_queue_not_matched,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(result,
                                hf_amqp_0_10_struct_exchange_bound_result_key_not_matched,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(result,
                                hf_amqp_0_10_struct_exchange_bound_result_args_not_matched,
                                tvb, offset, 1, ENC_BIG_ENDIAN);
            break;
        }
        break;

    case AMQP_0_10_CLASS_QUEUE:
        switch (struct_code) {
        case AMQP_0_10_STRUCT_QUEUE_QUERY_RESULT:
            dissect_amqp_0_10_struct_queue_query_result(tvb, pinfo, tree);
            break;
        }
        break;

    case AMQP_0_10_CLASS_FILE:
        switch (struct_code) {
        case AMQP_0_10_STRUCT_FILE_PROPERTIES:
            dissect_amqp_0_10_struct_file_properties(tvb, pinfo, tree);
            break;
        }
        break;

    case AMQP_0_10_CLASS_STREAM:
        switch (struct_code) {
        case AMQP_0_10_STRUCT_STREAM_PROPERTIES:
            dissect_amqp_0_10_struct_stream_properties(tvb, pinfo, tree);
            break;
        }
        break;
    }
    decrement_dissection_depth(pinfo);
}

/* decodes AMQP 1.0 list
 * arguments:
 *   tvb: obvious
 *   pinfo: obvious
 *   offset: obvious
 *   bound: boundary within that the list has to end
 *   item: obvious
 *   hf_amqp_type: what hf_* type is the list itself
 *   hf_amqp_subtype_count: length of hf_amqp_subtypes
 *   hf_amqp_subtypes: what hf_* types are the list items
 *   name: what to show for unformatted content
 */
static unsigned
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_1_0_list(tvbuff_t *tvb,
                      packet_info *pinfo,
                      int offset,
                      proto_item *item,
                      int hf_amqp_type,
                      uint32_t hf_amqp_subtype_count,
                      int * const *hf_amqp_subtypes,
                      const char *name)
{
    proto_item *list_tree;
    uint8_t     type;
    uint8_t     count_len;
    uint32_t    i, element_count;
    uint32_t    element_size;
    uint32_t    decoded_element_size;
    uint32_t    orig_offset;
    uint32_t    decoded_elements;
    int         hf_amqp_item;

    list_tree = 0;
    decoded_elements = 0;
    orig_offset = offset;

    type = tvb_get_uint8(tvb, offset);
    offset += 1;
    switch (type) {
    case AMQP_1_0_TYPE_LIST0:
        count_len = 0;
        element_size = 0;
        element_count = 0;
        break;
    case AMQP_1_0_TYPE_LIST8:
        count_len = 1;
        element_size = tvb_get_uint8(tvb, offset);
        element_count = tvb_get_uint8(tvb, offset+count_len);
        break;
    case AMQP_1_0_TYPE_LIST32:
        count_len = 4;
        element_size = tvb_get_ntohl(tvb, offset);
        element_count = tvb_get_ntohl(tvb, offset+count_len);
        break;
    default:
        proto_tree_add_none_format(list_tree, hf_amqp_1_0_list, tvb,
                                   offset-1,
                                   1,
                                   "(unknown type %d)",
                                   type);
        expert_add_info_format(pinfo,
                               list_tree,
                               &ei_amqp_unknown_amqp_type,
                               "Unknown AMQP list type %d",
                               type);
        return 0;
    }

    list_tree = proto_tree_add_item(item,
                                    hf_amqp_type,
                                    tvb,
                                    offset-1,
                                    element_size+1+count_len,
                                    ENC_BIG_ENDIAN);
    proto_item_set_text(list_tree, "%s", name ? name : proto_registrar_get_name(hf_amqp_type));
    offset += (count_len*2);

    if (element_count > 0)
        list_tree = proto_item_add_subtree(list_tree, ett_amqp_1_0_list);
    /* display the item count for custom lists only
     * standard structures contain NULL items, so the real element count is different */
    if (hf_amqp_subtype_count == 0)
        proto_item_append_text(list_tree, " (list of %d element%s)", element_count, plurality(element_count, "", "s"));

    if (element_count > element_size)
    {
        expert_add_info_format(pinfo,
                               list_tree,
                               &ei_amqp_invalid_number_of_params,
                               "Number of list elements (%d) bigger than list size (%d)",
                               element_count, element_size);
        return 0;
    }

    for (i = 0; ((i < element_count) && (tvb_reported_length_remaining(tvb, offset) > 0)); i++) {
        decoded_element_size = 0;
        if (decoded_elements<hf_amqp_subtype_count)
            hf_amqp_item = *(hf_amqp_subtypes[decoded_elements]);
        else
            hf_amqp_item = hf_amqp_1_0_list; /* dynamic item */
        get_amqp_1_0_type_value_formatter(tvb,
                                          pinfo,
                                          offset,
                                          hf_amqp_item,
                                          NULL,
                                          &decoded_element_size,
                                          list_tree);
        decoded_elements += 1;
        offset += decoded_element_size;
    }
    if (i != element_count)
        expert_add_info_format(pinfo,
                               list_tree,
                               &ei_amqp_invalid_number_of_params,
                               "Number of list elements (%d) not matching number of decoded elements (%d)",
                               element_count+decoded_elements, decoded_elements);
    return offset-orig_offset;
}

/* decodes AMQP 1.0 map
 *  arguments: see dissect_amqp_1_0_list
 */
static unsigned
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_1_0_map(tvbuff_t *tvb,
                     packet_info *pinfo,
                     int offset,
                     proto_item *item,
                     int hf_amqp_type,
                     const char *name)
{
    proto_item *map_tree;
    uint8_t     type;
    uint8_t     count_len;
    uint32_t    element_count;
    uint32_t    element_size;
    const struct amqp1_typeinfo* element_type;
    uint32_t    decoded_element_size;
    uint32_t    orig_offset;
    const char *value = NULL;

    map_tree = 0;
    orig_offset = offset;

    type = tvb_get_uint8(tvb, offset);
    offset += 1;
    switch (type) {
    case AMQP_1_0_TYPE_MAP8:
        count_len = 1;
        element_size = tvb_get_uint8(tvb, offset);
        element_count = tvb_get_uint8(tvb, offset+count_len);
        break;
    case AMQP_1_0_TYPE_MAP32:
        count_len = 4;
        element_size = tvb_get_ntohl(tvb, offset);
        element_count = tvb_get_ntohl(tvb, offset+count_len);
        break;
    default:
        proto_tree_add_none_format(map_tree, hf_amqp_1_0_map, tvb,
                                   offset-1,
                                   1,
                                   "(unknown type %d)",
                                   type);
        expert_add_info_format(pinfo,
                               map_tree,
                               &ei_amqp_unknown_amqp_type,
                               "Unknown AMQP map type %d",
                               type);
        return tvb_reported_length_remaining(tvb, orig_offset);
    }

    if (proto_registrar_get_ftype(hf_amqp_type) != FT_NONE) {
        map_tree = proto_tree_add_item(item,
                                          hf_amqp_type,
                                          tvb,
                                          offset-1,
                                          element_size+1+count_len,
                                          ENC_NA);
    } else {
        map_tree = proto_tree_add_none_format(item,
                                          hf_amqp_type,
                                          tvb,
                                          offset-1,
                                          element_size+1+count_len,
                                          "%s",
                                          name ? name : proto_registrar_get_name(hf_amqp_type));
    }
    offset += (count_len*2);

    if (element_count > 0)
        map_tree = proto_item_add_subtree(map_tree, ett_amqp_1_0_map);
    if (element_count%2==1) {
        expert_add_info_format(pinfo,
                               map_tree,
                               &ei_amqp_invalid_number_of_params,
                               "Odd number of map items: %d",
                               element_count);
        return tvb_reported_length_remaining(tvb, orig_offset);
    }

    if (element_count > element_size)
    {
        expert_add_info_format(pinfo,
                               map_tree,
                               &ei_amqp_invalid_number_of_params,
                               "Number of map elements (%d) bigger than map size (%d)",
                               element_count, element_size);
        return tvb_reported_length_remaining(tvb, orig_offset);
    }

    proto_item_append_text(map_tree,
                           " (map of %d element%s)",
                           (element_count/2),
                           plurality(element_count/2, "", "s"));

    while ((element_count > 0) && (tvb_reported_length_remaining(tvb, offset) > 0)) {
        if (element_count%2 == 0) { /* decode key */
            element_type = decode_fixed_type(tvb_get_uint8(tvb, offset));
            if (element_type)
            {
                decoded_element_size=element_type->formatter(tvb, pinfo, offset+1, element_type->known_size, &value);
                offset += (decoded_element_size+1);
            }
            else
            { /* can't decode key type */
                proto_tree_add_none_format(map_tree, hf_amqp_1_0_map, tvb,
                                           offset,
                                           1,
                                           "(unknown map key type %d)",
                                           tvb_get_uint8(tvb, offset));
                expert_add_info_format(pinfo,
                                       map_tree,
                                       &ei_amqp_unknown_amqp_type,
                                       "Unknown AMQP map key type %d",
                                       tvb_get_uint8(tvb, offset));
                offset += 1;
            }
        }
        else { /* decode value */
            get_amqp_1_0_type_value_formatter(tvb,
                                              pinfo,
                                              offset,
                                              hf_amqp_1_0_list, /* dynamic item */
                                              value,
                                              &decoded_element_size,
                                              map_tree);
            offset += decoded_element_size;
        }
        element_count--;
    }
    return offset-orig_offset;
}

/* decodes AMQP 1.0 array
 *  arguments: see dissect_amqp_1_0_list
 */
static unsigned
// NOLINTNEXTLINE(misc-no-recursion)
dissect_amqp_1_0_array(tvbuff_t *tvb,
                       packet_info *pinfo,
                       int offset,
                       proto_item *item,
                       int hf_amqp_type,
                       uint32_t hf_amqp_subtype_count,
                       int * const *hf_amqp_subtypes,
                       const char *name)
{
    proto_item *array_tree;
    uint8_t     type;
    uint8_t     count_len;
    uint32_t    i, element_count;
    uint32_t    element_size;
    uint32_t    element_type;
    uint32_t    decoded_element_size;
    uint32_t    orig_offset;
    uint32_t    decoded_elements;
    int         hf_amqp_item;
    uint32_t    hf_amqp_subtype_count_array = 0;
    int * const  *hf_amqp_subtypes_array = NULL;
    const char  *type_name_array = NULL;

    array_tree = 0;
    decoded_elements = 0;
    orig_offset = offset;

    type = tvb_get_uint8(tvb, offset);
    offset += 1;
    switch (type) {
    case AMQP_1_0_TYPE_ARRAY8:
        count_len = 1;
        element_size = tvb_get_uint8(tvb, offset);
        element_count = tvb_get_uint8(tvb, offset+count_len);
        break;
    case AMQP_1_0_TYPE_ARRAY32:
        count_len = 4;
        element_size = tvb_get_ntohl(tvb, offset);
        element_count = tvb_get_ntohl(tvb, offset+count_len);
        break;
    default:
        proto_tree_add_none_format(array_tree, hf_amqp_1_0_list, tvb,
                                   offset-1,
                                   1,
                                   "(unknown type %d)",
                                   type);
        expert_add_info_format(pinfo,
                               array_tree,
                               &ei_amqp_unknown_amqp_type,
                               "Unknown AMQP array type %d",
                               type);
        return tvb_reported_length_remaining(tvb, orig_offset);
    }

    element_type = get_amqp_1_0_type_formatter(tvb,
                                               pinfo,
                                               offset+count_len*2,
                                               &hf_amqp_type,
                                               &type_name_array,
                                               &hf_amqp_subtype_count_array,
                                               &hf_amqp_subtypes_array,
                                               &decoded_element_size);

    array_tree = proto_tree_add_item(item,
                                     hf_amqp_type,
                                     tvb,
                                     offset-1,
                                     element_size+1+count_len,
                                     ENC_BIG_ENDIAN);
    proto_item_set_text(array_tree, "%s", name ? name : proto_registrar_get_name(hf_amqp_type));
    offset += (count_len*2+decoded_element_size);

    if (element_count > 0)
        array_tree = proto_item_add_subtree(array_tree, ett_amqp_1_0_array);
    /* display the item count for custom arrays only
     * standard structures contain NULL items, so the real element count is different */
    if (hf_amqp_subtype_count == 0)
        proto_item_append_text(array_tree, " (array of %d element%s)", element_count, plurality(element_count, "", "s"));

    if (element_count > element_size)
    {
        expert_add_info_format(pinfo,
                               array_tree,
                               &ei_amqp_invalid_number_of_params,
                               "Number of array elements (%d) bigger than array size (%d)",
                               element_count, element_size);
        return tvb_reported_length_remaining(tvb, orig_offset);
    }

    for (i = 0; ((i < element_count) && (tvb_reported_length_remaining(tvb, offset) > 0)); i++) {
        decoded_element_size = 0;
        if (decoded_elements<hf_amqp_subtype_count)
            hf_amqp_item = *(hf_amqp_subtypes[decoded_elements]);
        else
            hf_amqp_item = hf_amqp_1_0_list; /* dynamic item */
        get_amqp_1_0_value_formatter(tvb,
                                     pinfo,
                                     element_type, /* code */
                                     offset,
                                     hf_amqp_item,
                                     (proto_registrar_get_nth(hf_amqp_type))->name, /* name */
                                     hf_amqp_subtype_count_array, /* subitem list count */
                                     hf_amqp_subtypes_array, /* subitem list hf_.. list */
                                     &decoded_element_size,
                                     array_tree);
        decoded_elements += 1;
        if (decoded_element_size==0)
            decoded_element_size=1; /* necessary for 0x40 or similar values where value_formatter returns size of _value_ 0 (type=1 not counted) */
        offset += decoded_element_size;
    }
    if (i != element_count)
        expert_add_info_format(pinfo,
                               array_tree,
                               &ei_amqp_invalid_number_of_params,
                               "Number of array elements (%d) not matching number of decoded elements (%d)",
                               element_count+decoded_elements, decoded_elements);
    return offset-orig_offset;
}

/* decodes AMQP 1.0 AMQP performative (open, attach, transfer or so)
 * arguments:
 *   tvb, offset, length, amqp_tree, pinfo: obvious
 *   method_name: what to print to col_append_str method in dissect_amqp_1_0_frame
 */
static void
dissect_amqp_1_0_AMQP_frame(tvbuff_t *tvb,
                            proto_item *amqp_item,
                            packet_info *pinfo)
{
    proto_item  *args_tree;
    uint32_t    arg_length = 0;
    uint32_t    method;
    int         offset = 0;
    proto_item* ti;

    args_tree = proto_item_add_subtree(amqp_item, ett_args);

    if (tvb_reported_length(tvb) == 0) { /* empty keepalive sent */
        col_append_str(pinfo->cinfo, COL_INFO, "(empty)");
        col_set_fence(pinfo->cinfo, COL_INFO);
        return;
    }

    ti = proto_tree_add_item_ret_uint(args_tree, hf_amqp_1_0_amqp_performative, tvb, offset+2, 1, ENC_BIG_ENDIAN, &method);
    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", val_to_str_const(method, amqp_1_0_AMQP_performatives, "<invalid AMQP performative>"));
    col_set_fence(pinfo->cinfo, COL_INFO);

    offset += 3;    /* descriptor-constructor & fixed_one length & AMQP performative code */
    switch(method) {
        case AMQP_1_0_AMQP_OPEN:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    10, amqp_1_0_amqp_open_items, NULL);
            break;
        case AMQP_1_0_AMQP_BEGIN:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    8, amqp_1_0_amqp_begin_items, NULL);
            break;
        case AMQP_1_0_AMQP_ATTACH:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    14, amqp_1_0_amqp_attach_items, NULL);
            break;
        case AMQP_1_0_AMQP_FLOW:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    11, amqp_1_0_amqp_flow_items, NULL);
            break;
        case AMQP_1_0_AMQP_TRANSFER:
            arg_length = dissect_amqp_1_0_list(tvb,
                                               pinfo,
                                               offset,
                                               args_tree,
                                               hf_amqp_method_arguments,
                                               11, amqp_1_0_amqp_transfer_items, NULL);

            /* now decode message header, annotations, properties and data */
            while ((arg_length > 0) && (tvb_reported_length_remaining(tvb, offset + arg_length) > 0)) {
                offset += arg_length;
                get_amqp_1_0_type_value_formatter(tvb,
                                                    pinfo,
                                                    offset,
                                                    hf_amqp_1_0_list, /* dynamic item */
                                                    NULL,
                                                    &arg_length,
                                                    args_tree);
            }
            break;
        case AMQP_1_0_AMQP_DISPOSITION:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    6, amqp_1_0_amqp_disposition_items, NULL);
            break;
        case AMQP_1_0_AMQP_DETACH:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    3, amqp_1_0_amqp_detach_items, NULL);
            break;
        case AMQP_1_0_AMQP_END:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    1, amqp_1_0_amqp_end_items, NULL);
            break;
        case AMQP_1_0_AMQP_CLOSE:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    1, amqp_1_0_amqp_close_items, NULL);
            break;
        default:
            expert_add_info_format(pinfo,
                                   ti,
                                   &ei_amqp_unknown_amqp_command,
                                   "Unknown AMQP performative %d",
                                   method);
    }
}

/* decodes AMQP 1.0 SASL methods (mechanisms offer, challenge, response,..)
 * arguments: see dissect_amqp_1_0_AMQP_frame
 */
static void
dissect_amqp_1_0_SASL_frame(tvbuff_t *tvb,
                            proto_item *amqp_item,
                            packet_info *pinfo)
{
    proto_item  *args_tree;
    uint32_t    method;
    int         offset = 0;
    proto_item *ti;

    args_tree = proto_item_add_subtree(amqp_item, ett_args);
    ti = proto_tree_add_item_ret_uint(args_tree, hf_amqp_1_0_sasl_method, tvb, offset+2, 1, ENC_BIG_ENDIAN, &method);

    col_append_fstr(pinfo->cinfo, COL_INFO, "%s ", val_to_str_const(method, amqp_1_0_SASL_methods, "<invalid SASL method>"));
    col_set_fence(pinfo->cinfo, COL_INFO);

    offset += 3; /* descriptor-constructor & fixed_one length & SASL method code */
    switch(method) {
        case AMQP_1_0_SASL_MECHANISMS:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    1, amqp_1_0_sasl_mechanisms_items, NULL);
            break;
        case AMQP_1_0_SASL_INIT:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    3, amqp_1_0_sasl_init_items, NULL);
            break;
         case AMQP_1_0_SASL_CHALLENGE:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    1, amqp_1_0_sasl_challenge_items, NULL);
            break;
        case AMQP_1_0_SASL_RESPONSE:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    1, amqp_1_0_sasl_response_items, NULL);
            break;
        case AMQP_1_0_SASL_OUTCOME:
            dissect_amqp_1_0_list(tvb,
                                    pinfo,
                                    offset,
                                    args_tree,
                                    hf_amqp_method_arguments,
                                    2, amqp_1_0_sasl_outcome_items, NULL);
            break;
        default:
            expert_add_info_format(pinfo,
                                   ti,
                                   &ei_amqp_unknown_sasl_command,
                                   "Unknown SASL command %d",
                                   method);
    }
}

static int
dissect_amqp_1_0_frame(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    proto_item  *ti, *size_item;
    proto_tree  *amqp_tree;
    uint8_t     frame_type;
    uint32_t    length;
    unsigned    offset;
    tvbuff_t    *next_tvb;

    col_clear(pinfo->cinfo, COL_INFO);

    /*  Heuristic - protocol initialisation frame starts with 'AMQP' followed by 0x0  */
    if (tvb_memeql(tvb, 0, "AMQP", 4) == 0) {
        uint8_t        proto_major;
        uint8_t        proto_minor;
        uint8_t        proto_revision;

        proto_major    = tvb_get_uint8(tvb, 5);
        proto_minor    = tvb_get_uint8(tvb, 6);
        proto_revision = tvb_get_uint8(tvb, 7);
        col_append_fstr(pinfo->cinfo, COL_INFO, "Protocol-Header%s %d-%d-%d ",
                                  (tvb_get_uint8(tvb, 4)==0x2) ? "(TLS)" : "", /* frame type = 2 => TLS */
                                  proto_major,
                                  proto_minor,
                                  proto_revision);
        col_set_fence(pinfo->cinfo, COL_INFO);

        if (tree) {
            ti = proto_tree_add_item(tree, proto_amqp, tvb, 0, -1, ENC_NA);
            amqp_tree = proto_item_add_subtree(ti, ett_amqp_init);
            proto_tree_add_item(amqp_tree, hf_amqp_init_protocol,         tvb, 0, 4, ENC_ASCII);
            proto_tree_add_item(amqp_tree, hf_amqp_init_id,               tvb, 4, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_major,    tvb, 5, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_minor,    tvb, 6, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_revision, tvb, 7, 1, ENC_BIG_ENDIAN);
        }
        return 8;
    }

    /* Protocol frame */

    /* frame header */
    ti = proto_tree_add_item(tree, proto_amqp, tvb, 0, -1, ENC_NA);
    amqp_tree = proto_item_add_subtree(ti, ett_amqp);
    size_item = proto_tree_add_item_ret_uint(amqp_tree, hf_amqp_1_0_size, tvb, 0, 4, ENC_BIG_ENDIAN, &length);
    proto_tree_add_item(amqp_tree, hf_amqp_1_0_doff, tvb, 4, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(amqp_tree, hf_amqp_1_0_type, tvb, 5, 1, ENC_BIG_ENDIAN);
    proto_tree_add_item(amqp_tree, hf_amqp_channel,  tvb, 6, 2, ENC_BIG_ENDIAN);

    offset     = 4*tvb_get_uint8(tvb,4); /* i.e. 4*DOFF */
    frame_type = tvb_get_uint8(tvb, 5);
    if (length < offset) {
        expert_add_info(pinfo, size_item, &ei_amqp_bad_length);
        return 8;
    }

    if (length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
    {
        next_tvb = tvb_new_subset_remaining(tvb, offset);
    }
    else
    {
        next_tvb = tvb_new_subset_length(tvb, offset, length);
    }

    switch(frame_type) {
    case AMQP_1_0_AMQP_FRAME:
        dissect_amqp_1_0_AMQP_frame(next_tvb, amqp_tree, pinfo);
        break;
    case AMQP_1_0_SASL_FRAME:
        dissect_amqp_1_0_SASL_frame(next_tvb, amqp_tree, pinfo);
        break;
    case AMQP_1_0_TLS_FRAME:
        /* should not occur, this is handled in '(tvb_memeql(tvb, 0, (const uint8_t*)"AMQP", 4) == 0)' test above */
        break;
    default:
        expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_frame_type, "Unknown frame type %d", frame_type);
    }

    return tvb_reported_length(tvb);
}

static int
dissect_amqp_0_10_frame(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    proto_item *ti;
    proto_item *amqp_tree = NULL;
    uint8_t     frame_type;
    uint16_t    length;
    uint32_t    struct_length;
    unsigned    offset;
    tvbuff_t   *next_tvb;

    /*  Heuristic - protocol initialisation frame starts with 'AMQP'  */
    if (tvb_memeql(tvb, 0, (const uint8_t*)"AMQP", 4) == 0) {
        uint8_t        proto_major;
        uint8_t        proto_minor;

        proto_major = tvb_get_uint8(tvb, 6);
        proto_minor = tvb_get_uint8(tvb, 7);
        col_append_fstr(pinfo->cinfo, COL_INFO, "Protocol-Header %d-%d ",
                                  proto_major,
                                  proto_minor);
        col_set_fence(pinfo->cinfo, COL_INFO);

        if (tree) {
            ti = proto_tree_add_item(tree, proto_amqp, tvb, 0, -1, ENC_NA);
            amqp_tree = proto_item_add_subtree(ti, ett_amqp_init);
            proto_tree_add_item(amqp_tree, hf_amqp_init_protocol,      tvb, 0, 4, ENC_ASCII);
            proto_tree_add_item(amqp_tree, hf_amqp_init_id_major,      tvb, 4, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_id_minor,      tvb, 5, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_major, tvb, 6, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_minor, tvb, 7, 1, ENC_BIG_ENDIAN);
        }
        return 8;
    }

    /* Protocol frame */
    if (tree) {
        ti = proto_tree_add_item(tree, proto_amqp, tvb, 0, -1, ENC_NA);
        amqp_tree = proto_item_add_subtree(ti, ett_amqp);
        proto_tree_add_item(amqp_tree, hf_amqp_0_10_format,   tvb, 0, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_0_10_position, tvb, 0, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_0_10_type,     tvb, 1, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_0_10_size,     tvb, 2, 2, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_0_10_track,    tvb, 5, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_channel,       tvb, 6, 2, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_reserved,      tvb, 8, 4, ENC_BIG_ENDIAN);
    }

    frame_type = tvb_get_uint8(tvb, 1);
    length     = tvb_get_ntohs(tvb, 2);
    offset     = 12;
    next_tvb = tvb_new_subset_remaining(tvb, offset);

    switch(frame_type) {
    case AMQP_0_10_FRAME_COMMAND:
        /* Fall through */
    case AMQP_0_10_FRAME_CONTROL:
        proto_tree_add_item(amqp_tree, hf_amqp_0_10_class, tvb, offset+0, 1, ENC_BIG_ENDIAN);
        switch(tvb_get_uint8(tvb, offset + 0)) {
        case AMQP_0_10_CLASS_CONNECTION:
            dissect_amqp_0_10_connection(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_SESSION:
            dissect_amqp_0_10_session(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_EXECUTION:
            dissect_amqp_0_10_execution(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_MESSAGE:
            dissect_amqp_0_10_message(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_TX:
            dissect_amqp_0_10_tx(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_DTX:
            dissect_amqp_0_10_dtx(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_EXCHANGE:
            dissect_amqp_0_10_exchange(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_QUEUE:
            dissect_amqp_0_10_queue(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_FILE:
            dissect_amqp_0_10_file(next_tvb, pinfo, amqp_tree);
            break;
        case AMQP_0_10_CLASS_STREAM:
            dissect_amqp_0_10_stream(next_tvb, pinfo, amqp_tree);
            break;
        default:
            expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_command_class, "Unknown command/control class %d", tvb_get_uint8(tvb, offset + 0));
        }
        break;

    case AMQP_0_10_FRAME_HEADER:
        col_append_str(pinfo->cinfo, COL_INFO, "header ");
        col_set_fence(pinfo->cinfo, COL_INFO);
        while (tvb_reported_length_remaining(tvb, offset) > 0)
        {
            struct_length = amqp_0_10_get_32bit_size_new(amqp_tree, pinfo, tvb, hf_amqp_0_10_struct32_size, offset);
            offset += 4;

            ti = proto_tree_add_item(amqp_tree,
                                     hf_amqp_0_10_struct32,
                                     tvb, offset, 2, ENC_BIG_ENDIAN);
            proto_item_set_len(ti, struct_length);
            if (struct_length > (uint32_t)tvb_reported_length_remaining(tvb, offset))
            {
                next_tvb = tvb_new_subset_remaining(tvb, offset);
            }
            else
            {
                next_tvb = tvb_new_subset_length(tvb, offset, struct_length);
            }
            dissect_amqp_0_10_struct32(next_tvb, pinfo, ti);
            offset += struct_length;
        }
        break;

    case AMQP_0_10_FRAME_BODY:
        col_append_str(pinfo->cinfo, COL_INFO, "message-body ");
        col_set_fence(pinfo->cinfo, COL_INFO);
        proto_tree_add_item(amqp_tree,
                            hf_amqp_0_10_message_body,
                            tvb, offset, length - 12, ENC_NA);
        break;

    default:
        expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_frame_type, "Unknown frame type %d", frame_type);
    }

    return tvb_reported_length(tvb);
}


/*  Dissection routine for method Connection.Start                        */

static int
dissect_amqp_0_9_method_connection_start(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;

    /*  version-major (octet)    */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_start_version_major,
        tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    /*  version-minor (octet)    */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_start_version_minor,
        tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    /*  server-properties (table)  */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_connection_start_server_properties,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    /*  mechanisms (longstr)     */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_start_mechanisms,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    /*  locales (longstr)        */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_start_locales,
                        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Connection.Start-Ok                     */

static int
dissect_amqp_0_9_method_connection_start_ok(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;

    /*  client-properties (table)  */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_connection_start_ok_client_properties,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    /*  mechanism (shortstr)     */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_start_ok_mechanism,
        tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
    offset += (1 + tvb_get_uint8(tvb, offset));

    /*  response (longstr)       */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_start_ok_response,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += (4 + tvb_get_ntohl(tvb, offset));

    /*  locale (shortstr)        */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_start_ok_locale,
        tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
    offset += (1 + tvb_get_uint8(tvb, offset));

    return offset;
}

/*  Dissection routine for method Connection.Secure                       */

static int
dissect_amqp_0_9_method_connection_secure(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  challenge (longstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_secure_challenge,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += (4 + tvb_get_ntohl(tvb, offset));

    return offset;
}

/*  Dissection routine for method Connection.Secure-Ok                    */

static int
dissect_amqp_0_9_method_connection_secure_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  response (longstr)       */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_secure_ok_response,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += (4 + tvb_get_ntohl(tvb, offset));

    return offset;
}

/*  Dissection routine for method Connection.Tune                         */

static int
dissect_amqp_0_9_method_connection_tune(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  channel-max (short)      */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_tune_channel_max,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  frame-max (long)         */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_tune_frame_max,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  heartbeat (short)        */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_tune_heartbeat,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    return offset;
}

/*  Dissection routine for method Connection.Tune-Ok                      */

static int
dissect_amqp_0_9_method_connection_tune_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  channel-max (short)      */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_tune_ok_channel_max,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  frame-max (long)         */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_tune_ok_frame_max,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  heartbeat (short)        */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_tune_ok_heartbeat,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    return offset;
}

/*  Dissection routine for method Connection.Open                         */

static int
dissect_amqp_0_9_method_connection_open(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    const uint8_t* vhost;
    /*  virtual-host (shortstr)  */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_connection_open_virtual_host,
        tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN, pinfo->pool, &vhost);
    col_append_fstr(pinfo->cinfo, COL_INFO, "vhost=%s ", vhost);
    offset += (1 + tvb_get_uint8(tvb, offset));

    /*  capabilities (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_open_capabilities,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  insist (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_open_insist,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Connection.Open-Ok                      */

static int
dissect_amqp_0_9_method_connection_open_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  known-hosts (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_open_ok_known_hosts,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Connection.Redirect                     */

static int
dissect_amqp_0_9_method_connection_redirect(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  host (shortstr)          */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_redirect_host,
        tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN);
    offset += (1 + tvb_get_uint8(tvb, offset));

    /*  known-hosts (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_redirect_known_hosts,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Connection.Close                        */

static int
dissect_amqp_0_9_method_connection_close(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *tf_code;
    const uint8_t* reply;

    /*  reply-code (short)       */
    tf_code = proto_tree_add_item(args_tree, hf_amqp_0_9_method_connection_close_reply_code,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    if (tvb_get_ntohs(tvb, offset) > 200)
        expert_add_info(pinfo, tf_code, &ei_amqp_connection_error);
    offset += 2;

    /*  reply-text (shortstr)    */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_connection_close_reply_text,
        tvb, offset, 1, ENC_ASCII|ENC_BIG_ENDIAN, pinfo->pool, &reply);
    col_append_fstr(pinfo->cinfo, COL_INFO, "reply=%s ", reply);
    offset += (1 + tvb_get_uint8(tvb, offset));

    /*  class-id (short)         */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_close_class_id,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  method-id (short)        */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_close_method_id,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    return offset;
}

/*  Dissection routine for method Connection.Close-Ok                     */

static int
dissect_amqp_0_9_method_connection_close_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Connection.Blocked                      */

static int
dissect_amqp_0_9_method_connection_blocked(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  reason (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_connection_blocked_reason,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Connection.Unblocked                    */

static int
dissect_amqp_0_9_method_connection_unblocked(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Channel.Open                            */

static int
dissect_amqp_0_9_method_channel_open(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  out-of-band (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_open_out_of_band,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Channel.Open-Ok                         */

static int
dissect_amqp_0_9_method_channel_open_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  channel-id (longstr)     */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_open_ok_channel_id,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Channel.Flow                            */

static int
dissect_amqp_0_9_method_channel_flow(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  active (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_flow_active,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Channel.Flow-Ok                         */

static int
dissect_amqp_0_9_method_channel_flow_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  active (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_flow_ok_active,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Channel.Close                           */

static int
dissect_amqp_0_9_method_channel_close(uint16_t channel_num, tvbuff_t *tvb,
    packet_info *pinfo, int offset, proto_tree *args_tree)
{
    proto_item *tf_code;
    const uint8_t* reply;

    /*  reply-code (short)       */
    tf_code = proto_tree_add_item(args_tree, hf_amqp_method_channel_close_reply_code,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    if (tvb_get_ntohs(tvb, offset) > 200)
        expert_add_info(pinfo, tf_code, &ei_amqp_channel_error);
    offset += 2;

    /*  reply-text (shortstr)    */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_channel_close_reply_text,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &reply);
    col_append_fstr(pinfo->cinfo, COL_INFO, "reply=%s ", reply);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  class-id (short)         */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_close_class_id,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  method-id (short)        */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_close_method_id,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /* delete channel */
    if(!PINFO_FD_VISITED(pinfo))
    {
        conversation_t *conv;
        amqp_conv *conn;

        conv = find_or_create_conversation(pinfo);
        conn = (amqp_conv *)conversation_get_proto_data(conv, proto_amqp);
        if (conn)
            wmem_map_remove(conn->channels, GUINT_TO_POINTER((uint32_t)channel_num));
    }

    return offset;
}

/*  Dissection routine for method Channel.Close-Ok                        */

static int
dissect_amqp_0_9_method_channel_close_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Channel.Resume                          */

static int
dissect_amqp_0_9_method_channel_resume(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  channel-id (longstr)     */
    proto_tree_add_item(args_tree, hf_amqp_method_channel_resume_channel_id,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Channel.Ping                            */

static int
dissect_amqp_0_9_method_channel_ping(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Channel.Pong                            */

static int
dissect_amqp_0_9_method_channel_pong(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Channel.Ok                              */

static int
dissect_amqp_0_9_method_channel_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Access.Request                          */

static int
dissect_amqp_0_9_method_access_request(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  realm (shortstr)         */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_realm,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  exclusive (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_exclusive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  passive (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_passive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  active (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_active,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  write (bit)              */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_write,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  read (bit)               */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_read,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Access.Request-Ok                       */

static int
dissect_amqp_0_9_method_access_request_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_access_request_ok_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    return offset;
}

/*  Dissection routine for method Exchange.Declare                        */

static int
dissect_amqp_0_9_method_exchange_declare(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;
    const uint8_t* exchange;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_exchange_declare_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &exchange);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", exchange);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  type (shortstr)          */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_type,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  passive (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_passive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  durable (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_durable,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  auto-delete (bit)        */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_auto_delete,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  internal (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_internal,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  arguments (table)        */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_exchange_declare_arguments,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Exchange.Declare-Ok                     */

static int
dissect_amqp_0_9_method_exchange_declare_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Exchange.Bind                           */

static int
dissect_amqp_0_9_method_exchange_bind(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;
    const uint8_t* str;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_declare_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  destination (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_exchange_bind_destination,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "dx=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  source (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_exchange_bind_source,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "sx=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_exchange_bind_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "bk=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_bind_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);
    offset += 1;

    /*  arguments (table)        */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_exchange_bind_arguments,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Exchange.Bind-Ok                        */

static int
dissect_amqp_0_9_method_exchange_bind_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Exchange.Delete                         */

static int
dissect_amqp_0_9_method_exchange_delete(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    const uint8_t* exchange;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_delete_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_exchange_delete_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &exchange);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", exchange);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  if-unused (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_delete_if_unused,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_exchange_delete_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Exchange.Delete-Ok                      */

static int
dissect_amqp_0_9_method_exchange_delete_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Queue.Declare                           */

static int
dissect_amqp_0_9_method_queue_declare(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;
    const uint8_t* queue;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_declare_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &queue);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", queue);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  passive (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_passive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  durable (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_durable,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  exclusive (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_exclusive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  auto-delete (bit)        */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_auto_delete,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  arguments (table)        */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_queue_declare_arguments,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Queue.Declare-Ok                        */

static int
dissect_amqp_0_9_method_queue_declare_ok(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    const uint8_t* queue;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_declare_ok_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &queue);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", queue);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  message-count (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_ok_message_count,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  consumer-count (long)    */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_declare_ok_consumer_count,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/*  Dissection routine for method Queue.Bind                              */

static int
dissect_amqp_0_9_method_queue_bind(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;
    const uint8_t* str;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_bind_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_bind_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_bind_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_bind_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "bk=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_bind_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  arguments (table)        */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_queue_bind_arguments,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Queue.Bind-Ok                           */

static int
dissect_amqp_0_9_method_queue_bind_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Queue.Unbind                            */

static int
dissect_amqp_0_9_method_queue_unbind(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;
    const uint8_t* str;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_unbind_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_unbind_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_unbind_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_unbind_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "rk=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  arguments (table)        */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_queue_unbind_arguments,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Queue.Unbind-Ok                         */

static int
dissect_amqp_0_9_method_queue_unbind_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Queue.Purge                             */

static int
dissect_amqp_0_9_method_queue_purge(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    const uint8_t* queue;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_purge_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_purge_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &queue);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", queue);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_purge_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Queue.Purge-Ok                          */

static int
dissect_amqp_0_9_method_queue_purge_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  message-count (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_purge_ok_message_count,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/*  Dissection routine for method Queue.Delete                            */

static int
dissect_amqp_0_9_method_queue_delete(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    const uint8_t* queue;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_delete_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_queue_delete_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &queue);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", queue);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  if-unused (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_delete_if_unused,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  if-empty (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_delete_if_empty,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_delete_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Queue.Delete-Ok                         */

static int
dissect_amqp_0_9_method_queue_delete_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  message-count (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_queue_delete_ok_message_count,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    return offset;
}

/*  Dissection routine for method Basic.Qos                               */

static int
dissect_amqp_0_9_method_basic_qos(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  prefetch-size (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_qos_prefetch_size,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  prefetch-count (short)   */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_qos_prefetch_count,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  global (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_qos_global,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Basic.Qos-Ok                            */

static int
dissect_amqp_0_9_method_basic_qos_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Basic.Consume                           */

static int
dissect_amqp_0_9_method_basic_consume(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;
    const uint8_t* queue;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_consume_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &queue);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", queue);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  no-local (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_no_local,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  no-ack (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_no_ack,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  exclusive (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_exclusive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  filter (table)           */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_basic_consume_filter,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Basic.Consume-Ok                        */

static int
dissect_amqp_0_9_method_basic_consume_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_consume_ok_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Basic.Cancel                            */

static int
dissect_amqp_0_9_method_basic_cancel(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_cancel_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_cancel_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Basic.Cancel-Ok                         */

static int
dissect_amqp_0_9_method_basic_cancel_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_cancel_ok_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Basic.Publish                           */

static int
dissect_amqp_0_9_method_basic_publish(uint16_t channel_num,
    tvbuff_t *tvb, packet_info *pinfo, int offset, proto_tree *args_tree)
{
    amqp_delivery *delivery;
    proto_item *pi;
    const uint8_t* str;

    /* message number (long long) */
    if(!PINFO_FD_VISITED(pinfo))
    {
        conversation_t *conv;
        amqp_channel_t *channel;

        conv = find_or_create_conversation(pinfo);
        channel = get_conversation_channel(conv, channel_num);

        record_msg_delivery_c(conv, channel, tvb, pinfo, ++channel->publish_count);
    }

    delivery = (amqp_delivery *)p_get_proto_data(pinfo->pool, pinfo, proto_amqp,
        (uint32_t)tvb_raw_offset(tvb));
    if(delivery)
    {
        pi = proto_tree_add_uint64(args_tree, hf_amqp_method_basic_publish_number,
            tvb, offset-2, 2, delivery->delivery_tag);
        proto_item_set_generated(pi);
    }

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_publish_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_publish_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_publish_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "rk=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  mandatory (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_publish_mandatory,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  immediate (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_publish_immediate,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Basic.Return                            */

static int
dissect_amqp_0_9_method_basic_return(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *tf_code;

    /*  reply-code (short)       */
    tf_code = proto_tree_add_item(args_tree, hf_amqp_method_basic_return_reply_code,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    if (tvb_get_ntohs(tvb, offset) > 200)
        expert_add_info(pinfo, tf_code, &ei_amqp_message_undeliverable);
    offset += 2;

    /*  reply-text (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_return_reply_text,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_return_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_return_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Basic.Deliver                           */

static int
dissect_amqp_0_9_method_basic_deliver(uint16_t channel_num,
    tvbuff_t *tvb, packet_info *pinfo, int offset, proto_tree *args_tree)
{
    uint64_t delivery_tag;
    const uint8_t* str;

    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_deliver_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_deliver_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    delivery_tag = tvb_get_ntoh64(tvb, offset);
    offset += 8;

    /*  redelivered (bit)        */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_deliver_redelivered,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_deliver_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_deliver_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "rk=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    if(!PINFO_FD_VISITED(pinfo))
        record_msg_delivery(tvb, pinfo, channel_num, delivery_tag);

    return offset;
}

/*  Dissection routine for method Basic.Get                               */

static int
dissect_amqp_0_9_method_basic_get(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    const uint8_t* queue;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_get_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_get_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &queue);
    col_append_fstr(pinfo->cinfo, COL_INFO, "q=%s ", queue);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  no-ack (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_get_no_ack,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Basic.Get-Ok                            */

static int
dissect_amqp_0_9_method_basic_get_ok(uint16_t channel_num,
    tvbuff_t *tvb, packet_info *pinfo, int offset, proto_tree *args_tree)
{
    uint64_t delivery_tag;
    const uint8_t* str;

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_get_ok_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    delivery_tag = tvb_get_ntoh64(tvb, offset);
    offset += 8;

    /*  redelivered (bit)        */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_get_ok_redelivered,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  exchange (shortstr)      */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_get_ok_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "x=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item_ret_string(args_tree, hf_amqp_method_basic_get_ok_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &str);
    col_append_fstr(pinfo->cinfo, COL_INFO, "rk=%s ", str);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  message-count (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_get_ok_message_count,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    if(!PINFO_FD_VISITED(pinfo))
        record_msg_delivery(tvb, pinfo, channel_num, delivery_tag);

    return offset;
}

/*  Dissection routine for method Basic.Get-Empty                         */

static int
dissect_amqp_0_9_method_basic_get_empty(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  cluster-id (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_get_empty_cluster_id,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Basic.Ack                               */

static int
dissect_amqp_0_9_method_basic_ack(uint16_t channel_num,
    tvbuff_t *tvb, packet_info *pinfo, int offset, proto_tree *args_tree)
{
    uint64_t delivery_tag;
    int multiple;

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_ack_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    delivery_tag = tvb_get_ntoh64(tvb, offset);
    offset += 8;

    /*  multiple (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_ack_multiple,
        tvb, offset, 1, ENC_BIG_ENDIAN);
    multiple = tvb_get_uint8(tvb, offset) & 0x01;

    if(!PINFO_FD_VISITED(pinfo))
        record_delivery_ack(tvb, pinfo, channel_num, delivery_tag, multiple);

    return offset;
}

/*  Dissection routine for method Basic.Reject                            */

static int
dissect_amqp_0_9_method_basic_reject(uint16_t channel_num,
    tvbuff_t *tvb, packet_info *pinfo, int offset, proto_tree *args_tree)
{
    uint64_t delivery_tag;

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_reject_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    delivery_tag = tvb_get_ntoh64(tvb, offset);
    offset += 8;

    /*  requeue (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_reject_requeue,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    if(!PINFO_FD_VISITED(pinfo))
        record_delivery_ack(tvb, pinfo, channel_num, delivery_tag, false);

    return offset;
}

/*  Dissection routine for method Basic.Recover-Async                     */

static int
dissect_amqp_0_9_method_basic_recover_async(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  requeue (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_recover_requeue,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Basic.Recover                           */

static int
dissect_amqp_0_9_method_basic_recover(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  requeue (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_recover_requeue,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Basic.Recover-Ok                        */

static int
dissect_amqp_0_9_method_basic_recover_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Basic.Nack                              */

static int
dissect_amqp_0_9_method_basic_nack(uint16_t channel_num,
    tvbuff_t *tvb, packet_info *pinfo, int offset, proto_tree *args_tree)
{
    uint64_t delivery_tag;
    int multiple;

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_nack_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    delivery_tag = tvb_get_ntoh64(tvb, offset);
    offset += 8;

    /*  multiple (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_nack_multiple,
        tvb, offset, 1, ENC_BIG_ENDIAN);
    multiple = tvb_get_uint8(tvb, offset) & 0x01;

    /*  requeue (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_basic_nack_requeue,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    if(!PINFO_FD_VISITED(pinfo))
        record_delivery_ack(tvb, pinfo, channel_num, delivery_tag, multiple);

    return offset;
}

/*  Dissection routine for method File.Qos                                */

static int
dissect_amqp_0_9_method_file_qos(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  prefetch-size (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_file_qos_prefetch_size,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  prefetch-count (short)   */
    proto_tree_add_item(args_tree, hf_amqp_method_file_qos_prefetch_count,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  global (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_file_qos_global,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method File.Qos-Ok                             */

static int
dissect_amqp_0_9_method_file_qos_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method File.Consume                            */

static int
dissect_amqp_0_9_method_file_consume(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  no-local (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_no_local,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  no-ack (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_no_ack,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  exclusive (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_exclusive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  filter (table)           */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_file_consume_filter,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method File.Consume-Ok                         */

static int
dissect_amqp_0_9_method_file_consume_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_consume_ok_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method File.Cancel                             */

static int
dissect_amqp_0_9_method_file_cancel(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_cancel_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_file_cancel_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method File.Cancel-Ok                          */

static int
dissect_amqp_0_9_method_file_cancel_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_cancel_ok_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method File.Open                               */

static int
dissect_amqp_0_9_method_file_open(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  identifier (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_file_open_identifier,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  content-size (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_open_content_size,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    return offset;
}

/*  Dissection routine for method File.Open-Ok                            */

static int
dissect_amqp_0_9_method_file_open_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  staged-size (longlong)   */
    proto_tree_add_item(args_tree, hf_amqp_method_file_open_ok_staged_size,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    return offset;
}

/*  Dissection routine for method File.Stage                              */

static int
dissect_amqp_0_9_method_file_stage(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method File.Publish                            */

static int
dissect_amqp_0_9_method_file_publish(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_file_publish_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_file_publish_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_file_publish_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  mandatory (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_file_publish_mandatory,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  immediate (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_file_publish_immediate,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  identifier (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_file_publish_identifier,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method File.Return                             */

static int
dissect_amqp_0_9_method_file_return(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  reply-code (short)       */
    proto_tree_add_item(args_tree, hf_amqp_method_file_return_reply_code,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  reply-text (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_file_return_reply_text,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_file_return_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_file_return_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method File.Deliver                            */

static int
dissect_amqp_0_9_method_file_deliver(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_deliver_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_deliver_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    /*  redelivered (bit)        */
    proto_tree_add_item(args_tree, hf_amqp_method_file_deliver_redelivered,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_file_deliver_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_file_deliver_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  identifier (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_file_deliver_identifier,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method File.Ack                                */

static int
dissect_amqp_0_9_method_file_ack(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_ack_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    /*  multiple (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_file_ack_multiple,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method File.Reject                             */

static int
dissect_amqp_0_9_method_file_reject(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_file_reject_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    /*  requeue (bit)            */
    proto_tree_add_item(args_tree, hf_amqp_method_file_reject_requeue,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Stream.Qos                              */

static int
dissect_amqp_0_9_method_stream_qos(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  prefetch-size (long)     */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_qos_prefetch_size,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  prefetch-count (short)   */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_qos_prefetch_count,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  consume-rate (long)      */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_qos_consume_rate,
        tvb, offset, 4, ENC_BIG_ENDIAN);
    offset += 4;

    /*  global (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_qos_global,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Stream.Qos-Ok                           */

static int
dissect_amqp_0_9_method_stream_qos_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Stream.Consume                          */

static int
dissect_amqp_0_9_method_stream_consume(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;

    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  queue (shortstr)         */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  no-local (bit)           */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_no_local,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  exclusive (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_exclusive,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    offset += 1;
    /*  filter (table)           */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_stream_consume_filter,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Stream.Consume-Ok                       */

static int
dissect_amqp_0_9_method_stream_consume_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_consume_ok_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Stream.Cancel                           */

static int
dissect_amqp_0_9_method_stream_cancel(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_cancel_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_cancel_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Stream.Cancel-Ok                        */

static int
dissect_amqp_0_9_method_stream_cancel_ok(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_cancel_ok_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Stream.Publish                          */

static int
dissect_amqp_0_9_method_stream_publish(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  ticket (short)           */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_publish_ticket,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_publish_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_publish_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  mandatory (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_publish_mandatory,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    /*  immediate (bit)          */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_publish_immediate,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Stream.Return                           */

static int
dissect_amqp_0_9_method_stream_return(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  reply-code (short)       */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_return_reply_code,
        tvb, offset, 2, ENC_BIG_ENDIAN);
    offset += 2;

    /*  reply-text (shortstr)    */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_return_reply_text,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_return_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  routing-key (shortstr)   */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_return_routing_key,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Stream.Deliver                          */

static int
dissect_amqp_0_9_method_stream_deliver(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  consumer-tag (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_deliver_consumer_tag,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  delivery-tag (longlong)  */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_deliver_delivery_tag,
        tvb, offset, 8, ENC_BIG_ENDIAN);
    offset += 8;

    /*  exchange (shortstr)      */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_deliver_exchange,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    /*  queue (shortstr)         */
    proto_tree_add_item(args_tree, hf_amqp_method_stream_deliver_queue,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Tx.Select                               */

static int
dissect_amqp_0_9_method_tx_select(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Tx.Select-Ok                            */

static int
dissect_amqp_0_9_method_tx_select_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Tx.Commit                               */

static int
dissect_amqp_0_9_method_tx_commit(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Tx.Commit-Ok                            */

static int
dissect_amqp_0_9_method_tx_commit_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Tx.Rollback                             */

static int
dissect_amqp_0_9_method_tx_rollback(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Tx.Rollback-Ok                          */

static int
dissect_amqp_0_9_method_tx_rollback_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Dtx.Select                              */

static int
dissect_amqp_0_9_method_dtx_select(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Dtx.Select-Ok                           */

static int
dissect_amqp_0_9_method_dtx_select_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Dtx.Start                               */

static int
dissect_amqp_0_9_method_dtx_start(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  dtx-identifier (shortstr)  */
    proto_tree_add_item(args_tree, hf_amqp_method_dtx_start_dtx_identifier,
        tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
    offset += 1 + tvb_get_uint8(tvb, offset);

    return offset;
}

/*  Dissection routine for method Dtx.Start-Ok                            */

static int
dissect_amqp_0_9_method_dtx_start_ok(tvbuff_t *tvb _U_,
    int offset, proto_tree *args_tree _U_)
{
    return offset;
}

/*  Dissection routine for method Tunnel.Request                          */

static int
dissect_amqp_0_9_method_tunnel_request(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *args_tree)
{
    proto_item *ti;

    /*  meta-data (table)        */
    ti = proto_tree_add_item(
        args_tree, hf_amqp_method_tunnel_request_meta_data,
        tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
    dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
    offset += 4 + tvb_get_ntohl(tvb, offset);

    return offset;
}

/*  Dissection routine for method Confirm.Select                          */

static int
dissect_amqp_0_9_method_confirm_select(tvbuff_t *tvb,
    int offset, proto_tree *args_tree)
{
    /*  nowait (bit)             */
    proto_tree_add_item(args_tree, hf_amqp_method_confirm_select_nowait,
        tvb, offset, 1, ENC_BIG_ENDIAN);

    return offset;
}

/*  Dissection routine for method Confirm.Select-Ok                       */

static int
dissect_amqp_0_9_method_confirm_select_ok(uint16_t channel_num,
    tvbuff_t *tvb _U_, packet_info *pinfo, int offset, proto_tree *args_tree _U_)
{
    if(!PINFO_FD_VISITED(pinfo))
    {
        amqp_channel_t *channel;
        channel = get_conversation_channel(find_or_create_conversation(pinfo), channel_num);
        channel->confirms = true;
    }

    return offset;
}


/*  Dissection routine for content headers of class basic          */

static int
dissect_amqp_0_9_content_header_basic(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *prop_tree, amqp_content_params *eh_ptr)
{
    proto_item   *ti;
    uint16_t      prop_flags;
    nstime_t      tv;
    const uint8_t *content;

    prop_flags = tvb_get_ntohs(tvb, 19);

    if (prop_flags & 0x8000) {
        /*  content-type (shortstr)  */
        proto_tree_add_item_ret_string(prop_tree, hf_amqp_header_basic_content_type,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &content);
        col_append_fstr(pinfo->cinfo, COL_INFO, "type=%s ", content);

        eh_ptr->type = ascii_strdown_inplace(
            (char*)tvb_get_string_enc(wmem_file_scope(), tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII));

        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  content-encoding (shortstr)  */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_content_encoding,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);

        eh_ptr->encoding = ascii_strdown_inplace(
            tvb_get_string_enc(wmem_file_scope(), tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII));

        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  headers (table)          */
        ti = proto_tree_add_item(
            prop_tree, hf_amqp_header_basic_headers,
            tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
        dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
        offset += 4 + tvb_get_ntohl(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  delivery-mode (octet)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_delivery_mode,
            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  priority (octet)         */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_priority,
            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  correlation-id (shortstr)  */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_correlation_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  reply-to (shortstr)      */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_reply_to,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  expiration (shortstr)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_expiration,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  message-id (shortstr)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_message_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  timestamp (timestamp)    */
        tv.secs = (time_t)tvb_get_ntoh64(tvb, offset);
        tv.nsecs = 0;
        proto_tree_add_time(prop_tree, hf_amqp_header_basic_timestamp,
                            tvb, offset, 8, &tv);
        offset += 8;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  type (shortstr)          */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_type,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  user-id (shortstr)       */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_user_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  app-id (shortstr)        */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_app_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  cluster-id (shortstr)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_basic_cluster_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    /*prop_flags <<= 1;*/

    return offset;
}
/*  Dissection routine for content headers of class file           */

static int
dissect_amqp_0_9_content_header_file(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *prop_tree)
{
    proto_item   *ti;
    uint16_t      prop_flags;
    nstime_t      tv;
    const uint8_t *content;

    prop_flags = tvb_get_ntohs(tvb, 19);

    if (prop_flags & 0x8000) {
        /*  content-type (shortstr)  */
        proto_tree_add_item_ret_string(prop_tree, hf_amqp_header_file_content_type,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &content);
        col_append_fstr(pinfo->cinfo, COL_INFO, "type=%s ", content);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  content-encoding (shortstr)  */
        proto_tree_add_item(prop_tree, hf_amqp_header_file_content_encoding,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  headers (table)          */
        ti = proto_tree_add_item(prop_tree, hf_amqp_header_file_headers,
            tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
        dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
        offset += 4 + tvb_get_ntohl(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  priority (octet)         */
        proto_tree_add_item(prop_tree, hf_amqp_header_file_priority,
            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  reply-to (shortstr)      */
        proto_tree_add_item(prop_tree, hf_amqp_header_file_reply_to,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  message-id (shortstr)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_file_message_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  filename (shortstr)      */
        proto_tree_add_item(prop_tree, hf_amqp_header_file_filename,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  timestamp (timestamp)    */
        tv.secs = (time_t)tvb_get_ntoh64(tvb, offset);
        tv.nsecs = 0;
        proto_tree_add_time(prop_tree, hf_amqp_header_file_timestamp,
                            tvb, offset, 8, &tv);
        offset += 8;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  cluster-id (shortstr)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_file_cluster_id,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    /*prop_flags <<= 1;*/

    return offset;
}
/*  Dissection routine for content headers of class stream         */

static int
dissect_amqp_0_9_content_header_stream(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *prop_tree)
{
    proto_item   *ti;
    uint16_t      prop_flags;
    nstime_t      tv;
    const uint8_t *content;

    prop_flags = tvb_get_ntohs(tvb, 19);

    if (prop_flags & 0x8000) {
        /*  content-type (shortstr)  */
        proto_tree_add_item_ret_string(prop_tree, hf_amqp_header_stream_content_type,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII|ENC_NA, pinfo->pool, &content);
        col_append_fstr(pinfo->cinfo, COL_INFO, "type=%s ", content);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  content-encoding (shortstr)  */
        proto_tree_add_item(prop_tree, hf_amqp_header_stream_content_encoding,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  headers (table)          */
        ti = proto_tree_add_item(prop_tree, hf_amqp_header_stream_headers,
            tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
        dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
        offset += 4 + tvb_get_ntohl(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  priority (octet)         */
        proto_tree_add_item(prop_tree, hf_amqp_header_stream_priority,
            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  timestamp (timestamp)    */
        tv.secs = (time_t)tvb_get_ntoh64(tvb, offset);
        tv.nsecs = 0;
        proto_tree_add_time(prop_tree, hf_amqp_header_stream_timestamp,
                            tvb, offset, 8, &tv);
        offset += 8;
    }
    /*prop_flags <<= 1;*/

    return offset;
}

/*  Dissection routine for content headers of class tunnel         */

static int
dissect_amqp_0_9_content_header_tunnel(tvbuff_t *tvb, packet_info *pinfo,
    int offset, proto_tree *prop_tree)
{
    proto_item *ti;
    uint16_t    prop_flags;

    prop_flags = tvb_get_ntohs(tvb, 19);

    if (prop_flags & 0x8000) {
        /*  headers (table)          */
        ti = proto_tree_add_item(prop_tree, hf_amqp_header_tunnel_headers,
            tvb, offset + 4, tvb_get_ntohl(tvb, offset), ENC_NA);
        dissect_amqp_0_9_field_table(tvb, pinfo, offset + 4, tvb_get_ntohl(tvb, offset), ti);
        offset += 4 + tvb_get_ntohl(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  proxy-name (shortstr)    */
        proto_tree_add_item(prop_tree, hf_amqp_header_tunnel_proxy_name,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  data-name (shortstr)     */
        proto_tree_add_item(prop_tree, hf_amqp_header_tunnel_data_name,
            tvb, offset + 1, tvb_get_uint8(tvb, offset), ENC_ASCII);
        offset += 1 + tvb_get_uint8(tvb, offset);
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  durable (octet)          */
        proto_tree_add_item(prop_tree, hf_amqp_header_tunnel_durable,
            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    prop_flags <<= 1;

    if (prop_flags & 0x8000) {
        /*  broadcast (octet)        */
        proto_tree_add_item(prop_tree, hf_amqp_header_tunnel_broadcast,
            tvb, offset, 1, ENC_BIG_ENDIAN);
        offset += 1;
    }
    /*prop_flags <<= 1;*/

    return offset;
}

/*  Dissection routine for AMQP 0-9 frames  */

static int
dissect_amqp_0_9_frame(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    proto_item    *ti;
    proto_item    *amqp_tree = NULL;
    proto_item    *args_tree;
    proto_item    *prop_tree;
    unsigned       length;
    uint8_t        frame_type;
    uint16_t       channel_num, class_id, method_id;

    /*  Heuristic - protocol initialisation frame starts with 'AMQP'  */
    if (tvb_memeql(tvb, 0, (const uint8_t*)"AMQP", 4) == 0) {
        uint8_t        proto_id, proto_major, proto_minor;

        proto_id = tvb_get_uint8(tvb, 5);
        proto_major = tvb_get_uint8(tvb, 6);
        proto_minor = tvb_get_uint8(tvb, 7);
        col_append_fstr(pinfo->cinfo, COL_INFO, "Protocol-Header %u-%u-%u",
                                  proto_id,
                                  proto_major,
                                  proto_minor);
        col_set_fence(pinfo->cinfo, COL_INFO);

        if (tree) {
            ti = proto_tree_add_item(tree, proto_amqp, tvb, 0, -1, ENC_NA);
            amqp_tree = proto_item_add_subtree(ti, ett_amqp_init);
            proto_tree_add_item(amqp_tree, hf_amqp_init_protocol, tvb, 0, 4, ENC_ASCII);
            proto_tree_add_item(amqp_tree, hf_amqp_init_id_major, tvb, 4, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_id_minor, tvb, 5, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_major, tvb, 6, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(amqp_tree, hf_amqp_init_version_minor, tvb, 7, 1, ENC_BIG_ENDIAN);
        }
        return 8;
    }

    if (tree) {
        ti = proto_tree_add_item(tree, proto_amqp, tvb, 0, -1, ENC_NA);
        amqp_tree = proto_item_add_subtree(ti, ett_amqp);
        proto_tree_add_item(amqp_tree, hf_amqp_0_9_type,   tvb, 0, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_channel,    tvb, 1, 2, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_0_9_length, tvb, 3, 4, ENC_BIG_ENDIAN);
    }

    frame_type = tvb_get_uint8(tvb, 0);
    channel_num = tvb_get_ntohs(tvb, 1);
    length     = tvb_get_ntohl(tvb, 3);

    switch (frame_type) {
    case AMQP_0_9_FRAME_TYPE_METHOD:
        class_id = tvb_get_ntohs(tvb, 7);
        proto_tree_add_item(amqp_tree, hf_amqp_0_9_method_class_id,
                            tvb, 7, 2, ENC_BIG_ENDIAN);
        switch (class_id) {
        case AMQP_0_9_CLASS_CONNECTION:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_connection_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);
            col_append_fstr(pinfo->cinfo, COL_INFO, "Connection.%s ",
                            val_to_str( method_id, amqp_method_connection_methods, "Unknown (%u)"));
            switch (method_id) {
            case AMQP_0_9_METHOD_CONNECTION_START:
                dissect_amqp_0_9_method_connection_start(tvb,
                                                         pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_START_OK:
                dissect_amqp_0_9_method_connection_start_ok(tvb,
                                                            pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_SECURE:
                dissect_amqp_0_9_method_connection_secure(tvb,
                                                          11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_SECURE_OK:
                dissect_amqp_0_9_method_connection_secure_ok(tvb,
                                                             11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_TUNE:
                dissect_amqp_0_9_method_connection_tune(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_TUNE_OK:
                dissect_amqp_0_9_method_connection_tune_ok(tvb,
                                                           11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_OPEN:
                dissect_amqp_0_9_method_connection_open(tvb,
                                                        pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_OPEN_OK:
                dissect_amqp_0_9_method_connection_open_ok(tvb,
                                                           11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_REDIRECT:
                dissect_amqp_0_9_method_connection_redirect(tvb,
                                                            11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_CLOSE:
                dissect_amqp_0_9_method_connection_close(tvb,
                                                         pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_CLOSE_OK:
                dissect_amqp_0_9_method_connection_close_ok(tvb,
                                                            11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_BLOCKED:
                dissect_amqp_0_9_method_connection_blocked(tvb,
                                                           11, args_tree);
                break;
            case AMQP_0_9_METHOD_CONNECTION_UNBLOCKED:
                dissect_amqp_0_9_method_connection_unblocked(tvb,
                                                             11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_connection_method, "Unknown connection method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_CHANNEL:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_channel_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);

            col_append_fstr(pinfo->cinfo, COL_INFO, "Channel.%s ",
                            val_to_str( method_id, amqp_method_channel_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_CHANNEL_OPEN:
                dissect_amqp_0_9_method_channel_open(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_OPEN_OK:
                dissect_amqp_0_9_method_channel_open_ok(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_FLOW:
                dissect_amqp_0_9_method_channel_flow(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_FLOW_OK:
                dissect_amqp_0_9_method_channel_flow_ok(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_CLOSE:
                dissect_amqp_0_9_method_channel_close(channel_num, tvb,
                                                      pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_CLOSE_OK:
                dissect_amqp_0_9_method_channel_close_ok(tvb,
                                                         11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_RESUME:
                dissect_amqp_0_9_method_channel_resume(tvb,
                                                       11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_PING:
                dissect_amqp_0_9_method_channel_ping(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_PONG:
                dissect_amqp_0_9_method_channel_pong(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_CHANNEL_OK:
                dissect_amqp_0_9_method_channel_ok(tvb,
                                                   11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_channel_method, "Unknown channel method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_ACCESS:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_access_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);
            switch (method_id) {
            case AMQP_0_9_METHOD_ACCESS_REQUEST:
                dissect_amqp_0_9_method_access_request(tvb,
                                                       11, args_tree);
                col_append_str(pinfo->cinfo, COL_INFO,
                               "Access.Request ");
                break;
            case AMQP_0_9_METHOD_ACCESS_REQUEST_OK:
                dissect_amqp_0_9_method_access_request_ok(tvb,
                                                          11, args_tree);
                col_append_str(pinfo->cinfo, COL_INFO,
                               "Access.Request-Ok ");
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_access_method, "Unknown access method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_EXCHANGE:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_exchange_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);
            col_append_fstr(pinfo->cinfo, COL_INFO, "Exchange.%s ",
                            val_to_str( method_id, amqp_method_exchange_methods, "Unknown (%u)"));
            switch (method_id) {
            case AMQP_0_9_METHOD_EXCHANGE_DECLARE:
                dissect_amqp_0_9_method_exchange_declare(tvb,
                                                         pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_DECLARE_OK:
                dissect_amqp_0_9_method_exchange_declare_ok(tvb,
                                                            11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_BIND:
                dissect_amqp_0_9_method_exchange_bind(tvb,
                                                      pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_BIND_OK:
                dissect_amqp_0_9_method_exchange_bind_ok(tvb,
                                                         11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_DELETE:
                dissect_amqp_0_9_method_exchange_delete(tvb,
                                                        pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_DELETE_OK:
                dissect_amqp_0_9_method_exchange_delete_ok(tvb,
                                                           11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_UNBIND:
                /* the same parameters as in bind */
                dissect_amqp_0_9_method_exchange_bind(tvb,
                                                      pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_EXCHANGE_UNBIND_OK:
                /* the same parameters as in bind-ok */
                dissect_amqp_0_9_method_exchange_bind_ok(tvb,
                                                         11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_exchange_method, "Unknown exchange method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_QUEUE:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_queue_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);
            col_append_fstr(pinfo->cinfo, COL_INFO, "Queue.%s ",
                            val_to_str( method_id, amqp_method_queue_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_QUEUE_DECLARE:
                dissect_amqp_0_9_method_queue_declare(tvb,
                                                      pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_DECLARE_OK:
                dissect_amqp_0_9_method_queue_declare_ok(tvb,
                                                         pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_BIND:
                dissect_amqp_0_9_method_queue_bind(tvb,
                                                   pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_BIND_OK:
                dissect_amqp_0_9_method_queue_bind_ok(tvb,
                                                      11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_UNBIND:
                dissect_amqp_0_9_method_queue_unbind(tvb,
                                                     pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_UNBIND_OK:
                dissect_amqp_0_9_method_queue_unbind_ok(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_PURGE:
                dissect_amqp_0_9_method_queue_purge(tvb,
                                                    pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_PURGE_OK:
                dissect_amqp_0_9_method_queue_purge_ok(tvb,
                                                       11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_DELETE:
                dissect_amqp_0_9_method_queue_delete(tvb,
                                                     pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_QUEUE_DELETE_OK:
                dissect_amqp_0_9_method_queue_delete_ok(tvb,
                                                        11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_queue_method, "Unknown queue method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_BASIC:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_basic_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);

            col_append_fstr(pinfo->cinfo, COL_INFO, "Basic.%s ",
                            val_to_str( method_id, amqp_method_basic_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_BASIC_QOS:
                dissect_amqp_0_9_method_basic_qos(tvb,
                                                  11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_QOS_OK:
                dissect_amqp_0_9_method_basic_qos_ok(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_CONSUME:
                dissect_amqp_0_9_method_basic_consume(tvb,
                                                      pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_CONSUME_OK:
                dissect_amqp_0_9_method_basic_consume_ok(tvb,
                                                         11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_CANCEL:
                dissect_amqp_0_9_method_basic_cancel(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_CANCEL_OK:
                dissect_amqp_0_9_method_basic_cancel_ok(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_PUBLISH:
                dissect_amqp_0_9_method_basic_publish(channel_num, tvb,
                                                      pinfo, 11, args_tree);
                generate_ack_reference(tvb, pinfo, amqp_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_RETURN:
                dissect_amqp_0_9_method_basic_return(tvb,
                                                     pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_DELIVER:
                dissect_amqp_0_9_method_basic_deliver(channel_num, tvb,
                                                      pinfo, 11, args_tree);
                generate_ack_reference(tvb, pinfo, amqp_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_GET:
                dissect_amqp_0_9_method_basic_get(tvb,
                                                  pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_GET_OK:
                dissect_amqp_0_9_method_basic_get_ok(channel_num, tvb,
                                                     pinfo, 11, args_tree);
                generate_ack_reference(tvb, pinfo, amqp_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_GET_EMPTY:
                dissect_amqp_0_9_method_basic_get_empty(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_ACK:
                dissect_amqp_0_9_method_basic_ack(channel_num, tvb,
                                                  pinfo, 11, args_tree);
                generate_msg_reference(tvb, pinfo, amqp_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_REJECT:
                dissect_amqp_0_9_method_basic_reject(channel_num, tvb,
                                                     pinfo, 11, args_tree);
                generate_msg_reference(tvb, pinfo, amqp_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_RECOVER_ASYNC:
                dissect_amqp_0_9_method_basic_recover_async(tvb,
                                                            11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_RECOVER:
                dissect_amqp_0_9_method_basic_recover(tvb,
                                                      11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_RECOVER_OK:
                dissect_amqp_0_9_method_basic_recover_ok(tvb,
                                                         11, args_tree);
                break;
            case AMQP_0_9_METHOD_BASIC_NACK:
                dissect_amqp_0_9_method_basic_nack(channel_num, tvb,
                                                   pinfo, 11, args_tree);
                generate_msg_reference(tvb, pinfo, amqp_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_basic_method, "Unknown basic method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_FILE:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_file_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);

            col_append_fstr(pinfo->cinfo, COL_INFO, "File.%s ",
                            val_to_str( method_id, amqp_method_file_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_FILE_QOS:
                dissect_amqp_0_9_method_file_qos(tvb,
                                                 11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_QOS_OK:
                dissect_amqp_0_9_method_file_qos_ok(tvb,
                                                    11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_CONSUME:
                dissect_amqp_0_9_method_file_consume(tvb,
                                                     pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_CONSUME_OK:
                dissect_amqp_0_9_method_file_consume_ok(tvb,
                                                        11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_CANCEL:
                dissect_amqp_0_9_method_file_cancel(tvb,
                                                    11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_CANCEL_OK:
                dissect_amqp_0_9_method_file_cancel_ok(tvb,
                                                       11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_OPEN:
                dissect_amqp_0_9_method_file_open(tvb,
                                                  11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_OPEN_OK:
                dissect_amqp_0_9_method_file_open_ok(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_STAGE:
                dissect_amqp_0_9_method_file_stage(tvb,
                                                   11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_PUBLISH:
                dissect_amqp_0_9_method_file_publish(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_RETURN:
                dissect_amqp_0_9_method_file_return(tvb,
                                                    11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_DELIVER:
                dissect_amqp_0_9_method_file_deliver(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_ACK:
                dissect_amqp_0_9_method_file_ack(tvb,
                                                 11, args_tree);
                break;
            case AMQP_0_9_METHOD_FILE_REJECT:
                dissect_amqp_0_9_method_file_reject(tvb,
                                                    11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_file_method, "Unknown file method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_STREAM:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_stream_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);

            col_append_fstr(pinfo->cinfo, COL_INFO, "Stream.%s ",
                            val_to_str( method_id, amqp_method_stream_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_STREAM_QOS:
                dissect_amqp_0_9_method_stream_qos(tvb,
                                                   11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_QOS_OK:
                dissect_amqp_0_9_method_stream_qos_ok(tvb,
                                                      11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_CONSUME:
                dissect_amqp_0_9_method_stream_consume(tvb,
                                                       pinfo, 11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_CONSUME_OK:
                dissect_amqp_0_9_method_stream_consume_ok(tvb,
                                                          11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_CANCEL:
                dissect_amqp_0_9_method_stream_cancel(tvb,
                                                      11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_CANCEL_OK:
                dissect_amqp_0_9_method_stream_cancel_ok(tvb,
                                                         11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_PUBLISH:
                dissect_amqp_0_9_method_stream_publish(tvb,
                                                       11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_RETURN:
                dissect_amqp_0_9_method_stream_return(tvb,
                                                      11, args_tree);
                break;
            case AMQP_0_9_METHOD_STREAM_DELIVER:
                dissect_amqp_0_9_method_stream_deliver(tvb,
                                                       11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_stream_method, "Unknown stream method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_TX:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_tx_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);

            col_append_fstr(pinfo->cinfo, COL_INFO, "Tx.%s ",
                            val_to_str( method_id, amqp_method_tx_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_TX_SELECT:
                dissect_amqp_0_9_method_tx_select(tvb,
                                                  11, args_tree);
                break;
            case AMQP_0_9_METHOD_TX_SELECT_OK:
                dissect_amqp_0_9_method_tx_select_ok(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_TX_COMMIT:
                dissect_amqp_0_9_method_tx_commit(tvb,
                                                  11, args_tree);
                break;
            case AMQP_0_9_METHOD_TX_COMMIT_OK:
                dissect_amqp_0_9_method_tx_commit_ok(tvb,
                                                     11, args_tree);
                break;
            case AMQP_0_9_METHOD_TX_ROLLBACK:
                dissect_amqp_0_9_method_tx_rollback(tvb,
                                                    11, args_tree);
                break;
            case AMQP_0_9_METHOD_TX_ROLLBACK_OK:
                dissect_amqp_0_9_method_tx_rollback_ok(tvb,
                                                       11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_tx_method, "Unknown tx method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_DTX:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_dtx_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);

            col_append_fstr(pinfo->cinfo, COL_INFO, "Dtx.%s ",
                            val_to_str( method_id, amqp_method_dtx_methods, "Unknown (%u)"));

            switch (method_id) {
            case AMQP_0_9_METHOD_DTX_SELECT:
                dissect_amqp_0_9_method_dtx_select(tvb,
                                                   11, args_tree);
                break;
            case AMQP_0_9_METHOD_DTX_SELECT_OK:
                dissect_amqp_0_9_method_dtx_select_ok(tvb,
                                                      11, args_tree);
                break;
            case AMQP_0_9_METHOD_DTX_START:
                dissect_amqp_0_9_method_dtx_start(tvb,
                                                  11, args_tree);
                break;
            case AMQP_0_9_METHOD_DTX_START_OK:
                dissect_amqp_0_9_method_dtx_start_ok(tvb,
                                                     11, args_tree);
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_dtx_method, "Unknown dtx method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_TUNNEL:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_tunnel_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);
            switch (method_id) {
            case AMQP_0_9_METHOD_TUNNEL_REQUEST:
                dissect_amqp_0_9_method_tunnel_request(tvb,
                                                       pinfo, 11, args_tree);
                col_append_str(pinfo->cinfo, COL_INFO,
                               "Tunnel.Request ");
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_tunnel_method, "Unknown tunnel method %u", method_id);
            }
            break;
        case AMQP_0_9_CLASS_CONFIRM:
            method_id = tvb_get_ntohs(tvb, 9);
            proto_tree_add_item(amqp_tree, hf_amqp_method_confirm_method_id,
                                tvb, 9, 2, ENC_BIG_ENDIAN);
            ti = proto_tree_add_item(amqp_tree, hf_amqp_method_arguments,
                                     tvb, 11, length - 4, ENC_NA);
            args_tree = proto_item_add_subtree(ti, ett_args);
            switch (method_id) {
            case AMQP_0_9_METHOD_CONFIRM_SELECT:
                dissect_amqp_0_9_method_confirm_select(tvb,
                                                       11, args_tree);
                col_append_str(pinfo->cinfo, COL_INFO,
                               "Confirm.Select ");
                break;
            case AMQP_0_9_METHOD_CONFIRM_SELECT_OK:
                dissect_amqp_0_9_method_confirm_select_ok(channel_num, tvb, pinfo,
                                                          11, args_tree);
                col_append_str(pinfo->cinfo, COL_INFO,
                               "Confirm.Select-Ok ");
                break;
            default:
                expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_confirm_method, "Unknown confirm method %u", method_id);
            }
            break;
        default:
            expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_method_class, "Unknown method class %u", class_id);
        }
        break;
    case AMQP_0_9_FRAME_TYPE_CONTENT_HEADER:
        class_id = tvb_get_ntohs(tvb, 7);
        proto_tree_add_item(amqp_tree, hf_amqp_header_class_id,
                            tvb, 7, 2, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_header_weight,
                            tvb, 9, 2, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_header_body_size,
                            tvb, 11, 8, ENC_BIG_ENDIAN);
        proto_tree_add_item(amqp_tree, hf_amqp_header_property_flags,
                            tvb, 19, 2, ENC_BIG_ENDIAN);
        ti = proto_tree_add_item(amqp_tree, hf_amqp_header_properties,
                                 tvb, 21, length - 14, ENC_NA);
        prop_tree = proto_item_add_subtree(ti, ett_props);
        col_append_str(pinfo->cinfo, COL_INFO, "Content-Header ");
        switch (class_id) {
        case AMQP_0_9_CLASS_BASIC: {
                amqp_channel_t *channel;
                channel = get_conversation_channel(find_or_create_conversation(pinfo), channel_num);
                channel->content_params = wmem_new0(wmem_file_scope(), amqp_content_params);

                dissect_amqp_0_9_content_header_basic(tvb,
                                                      pinfo, 21, prop_tree, channel->content_params);
            }
            break;
        case AMQP_0_9_CLASS_FILE:
            dissect_amqp_0_9_content_header_file(tvb,
                                                 pinfo, 21, prop_tree);
            break;
        case AMQP_0_9_CLASS_STREAM:
            dissect_amqp_0_9_content_header_stream(tvb,
                                                   pinfo, 21, prop_tree);
            break;
        case AMQP_0_9_CLASS_TUNNEL:
            dissect_amqp_0_9_content_header_tunnel(tvb,
                                                   pinfo, 21, prop_tree);
            break;
        default:
            expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_header_class, "Unknown header class %u", class_id);
        }
        break;
    case AMQP_0_9_FRAME_TYPE_CONTENT_BODY:
        proto_tree_add_item(amqp_tree, hf_amqp_payload,
                            tvb, 7, length, ENC_NA);
        col_append_str(pinfo->cinfo, COL_INFO, "Content-Body ");

        /* try to find dissector for content */
        amqp_channel_t *channel;
        tvbuff_t       *body_tvb;
        amqp_content_params  *content_params;

		channel = get_conversation_channel(find_or_create_conversation(pinfo), channel_num);
		content_params = channel->content_params;

        if (content_params != NULL && content_params->type != NULL) {
            body_tvb = tvb_new_subset_length(tvb, 7, length);
            dissector_try_string_with_data(media_type_subdissector_table, content_params->type, body_tvb, pinfo, amqp_tree, true, NULL);
        }
        break;
    case AMQP_0_9_FRAME_TYPE_HEARTBEAT:
        col_append_str(pinfo->cinfo, COL_INFO,
                       "Heartbeat ");
        break;
    default:
        expert_add_info_format(pinfo, amqp_tree, &ei_amqp_unknown_frame_type, "Unknown frame type %u", frame_type);
    }

    col_set_fence(pinfo->cinfo, COL_INFO);
    return tvb_reported_length(tvb);
}

static amqp_channel_t*
get_conversation_channel(conversation_t *conv, uint16_t channel_num)
{
    amqp_conv *conn;
    amqp_channel_t *channel;

    /* the amqp_conv structure was already created to record the AMQP version */
    conn = (amqp_conv *)conversation_get_proto_data(conv, proto_amqp);
    if (!conn)
        return NULL;

    channel = (amqp_channel_t *)wmem_map_lookup(conn->channels, GUINT_TO_POINTER((uint32_t)channel_num));
    if(channel == NULL)
    {
        channel = wmem_new0(wmem_file_scope(), amqp_channel_t);
        channel->conn = conn;
        channel->channel_num = channel_num;
        wmem_map_insert(conn->channels, GUINT_TO_POINTER((uint32_t)channel_num), channel);
    }

    return channel;
}

static void
record_msg_delivery(tvbuff_t *tvb, packet_info *pinfo, uint16_t channel_num,
    uint64_t delivery_tag)
{
    conversation_t *conv;
    amqp_channel_t *channel;

    conv = find_or_create_conversation(pinfo);
    channel = get_conversation_channel(conv, channel_num);
    record_msg_delivery_c(conv, channel, tvb, pinfo, delivery_tag);
}

static void
record_msg_delivery_c(conversation_t *conv, amqp_channel_t *channel,
    tvbuff_t *tvb, packet_info *pinfo, uint64_t delivery_tag)
{
    struct tcp_analysis *tcpd;
    amqp_delivery **dptr;
    amqp_delivery *delivery;

    tcpd = get_tcp_conversation_data(conv, pinfo);
    /* separate messages sent in each direction */
    dptr = tcpd->fwd == &(tcpd->flow1) ? &channel->last_delivery1 : &channel->last_delivery2;

    delivery = wmem_new0(wmem_file_scope(), amqp_delivery);
    delivery->delivery_tag = delivery_tag;
    delivery->msg_framenum = pinfo->num;
    /* append to the list of unacked deliveries */
    delivery->prev = (*dptr);
    (*dptr) = delivery;

    p_add_proto_data(pinfo->pool, pinfo, proto_amqp, (uint32_t)tvb_raw_offset(tvb), delivery);
}

static void
record_delivery_ack(tvbuff_t *tvb, packet_info *pinfo, uint16_t channel_num,
    uint64_t delivery_tag, bool multiple)
{
    conversation_t *conv;
    amqp_channel_t *channel;

    conv = find_or_create_conversation(pinfo);
    channel = get_conversation_channel(conv, channel_num);
    record_delivery_ack_c(conv, channel, tvb, pinfo, delivery_tag, multiple);
}

static void
record_delivery_ack_c(conversation_t *conv, amqp_channel_t *channel,
    tvbuff_t *tvb, packet_info *pinfo, uint64_t delivery_tag, bool multiple)
{
    struct tcp_analysis *tcpd;
    amqp_delivery **dptr;
    amqp_delivery *last_acked = NULL;

    tcpd = get_tcp_conversation_data(conv, pinfo);
    /* the basic.ack may be sent in both directions, but always opposite
     * to the basic.publish or basic.deliver */
    dptr = tcpd->rev == &(tcpd->flow1) ? &channel->last_delivery1 : &channel->last_delivery2;
    while(*dptr)
    {
        if((*dptr)->delivery_tag == delivery_tag)
        {
            do
            {
                amqp_delivery *delivery = (*dptr);
                *dptr = delivery->prev; /* remove from the list of unacked */

                delivery->ack_framenum = pinfo->num;
                /* append to the list of acked deliveries */
                delivery->prev = last_acked;
                last_acked = delivery;
            }
            while(multiple && *dptr);
        }
        else
            dptr = &(*dptr)->prev; /* goto next */
    }

    p_add_proto_data(pinfo->pool, pinfo, proto_amqp,
        (uint32_t)tvb_raw_offset(tvb), last_acked);
}

static void
generate_msg_reference(tvbuff_t *tvb, packet_info *pinfo, proto_tree *amqp_tree)
{
    amqp_delivery *delivery;
    proto_item *pi;

    delivery = (amqp_delivery *)p_get_proto_data(pinfo->pool, pinfo, proto_amqp,
        (uint32_t)tvb_raw_offset(tvb));
    while(delivery != NULL)
    {
        if(delivery->msg_framenum)
        {
            pi = proto_tree_add_uint(amqp_tree, hf_amqp_message_in,
                tvb, 0, 0, delivery->msg_framenum);
            proto_item_set_generated(pi);
        }

        delivery = delivery->prev;
    }
}

static void
generate_ack_reference(tvbuff_t *tvb, packet_info *pinfo, proto_tree *amqp_tree)
{
    amqp_delivery *delivery;

    delivery = (amqp_delivery *)p_get_proto_data(pinfo->pool, pinfo, proto_amqp,
        (uint32_t)tvb_raw_offset(tvb));
    if(delivery && delivery->ack_framenum)
    {
        proto_item *pi;

        pi = proto_tree_add_uint(amqp_tree, hf_amqp_ack_in, tvb, 0, 0, delivery->ack_framenum);
        proto_item_set_generated(pi);
    }

}

/*  AMQP 1.0 Type Decoders  */

static const struct amqp1_typeinfo* decode_fixed_type(uint8_t code)
{
    int i;

    for (i = 0; amqp_1_0_fixed_types[i].typecode != 0xff; ++i) {
        if (amqp_1_0_fixed_types[i].typecode == code)
            return &amqp_1_0_fixed_types[i];
    }
    return NULL;
}

/* For given code, the routine decodes its value, format & print output.
 * If the code is compound type (array,list,map), it calls relevant
 * dissect_* routines for decoding its items
 * arguments:
 *   tvb, pinfo, code, offset, bound: obvious
 *   hf_amqp_type: what hf_* variable corresponds to type of the code
 *   name: name of type of this code (applicable to map items and type descriptor
 *   hf_amqp_subtype_count: for format code to be list, expected number of list items
 *   hf_amqp_subtypes: for format code to be list, field of hf_* variables of list items
 *   length_size: decoded length
 */
static void
// NOLINTNEXTLINE(misc-no-recursion)
get_amqp_1_0_value_formatter(tvbuff_t *tvb,
                             packet_info *pinfo,
                             uint8_t code,
                             int offset,
                             int hf_amqp_type,
                             const char *name,
                             uint32_t hf_amqp_subtype_count,
                             int * const *hf_amqp_subtypes,
                             unsigned *length_size,
                             proto_item *item)
{
    const struct amqp1_typeinfo* element_type;
    const char *value = NULL;

    increment_dissection_depth(pinfo);
    element_type = decode_fixed_type(code);
    if (element_type)
    {
        const struct amqp_synonym_types_t *synonyms;
        int shift_view = 0;

        /* some AMQP fields can be of several types; by default we use FT_NONE,
         * but to enable filtering we try to find a field corresponding to
         * the actual type */
        if (proto_registrar_get_ftype(hf_amqp_type) == FT_NONE)
        {
            for (synonyms = amqp_synonym_types; synonyms->hf_none != NULL; synonyms++)
            {
                if (*(synonyms->hf_none) == hf_amqp_type)
                {
                    if (FT_IS_UINT(element_type->ftype) && synonyms->hf_uint != NULL)
                        hf_amqp_type = *(synonyms->hf_uint);
                    else if (FT_IS_STRING(element_type->ftype) && synonyms->hf_str != NULL)
                        hf_amqp_type = *(synonyms->hf_str);
                    else if (element_type->ftype == FT_BYTES && synonyms->hf_bin != NULL)
                        hf_amqp_type = *(synonyms->hf_bin);
                    else if (element_type->ftype == FT_GUID && synonyms->hf_guid != NULL)
                        hf_amqp_type = *(synonyms->hf_guid);
                    break;
                }
            }
        }

        if (proto_registrar_get_ftype(hf_amqp_type) != FT_NONE)
        {
            /* we know the field as well its type, use native dissectors */
            *length_size = element_type->dissector(tvb, pinfo,
                                                   offset,
                                                   element_type->known_size,
                                                   item, hf_amqp_type);
        }
        else if(code == AMQP_1_0_TYPE_NULL)
        {
            /* null value says that a particular field was optional and is omitted
             * the omitted fields of standard structures are not shown
             * however, we still display null values of custom lists, maps and arrays */
            *length_size = 0;
            if(hf_amqp_type == hf_amqp_1_0_list)
            {
                proto_tree_add_none_format(item, hf_amqp_type,
                                           tvb,
                                           offset-1,
                                           1,
                                           "%s: (null)",
                                           name ? name : proto_registrar_get_name(hf_amqp_type));
            }
        }
        else
        {
            /* multi-type and custom fields must be converted to a string */
            *length_size = element_type->formatter(tvb, pinfo, offset, element_type->known_size, &value);

            if (code/16 > 0x9) /* variable width code is 0xa[0-9] or 0xb[0-9] */
               /* shift to right to skip the variable length indicator */
               shift_view = element_type->known_size;
            else if(*length_size == 0)
                /* shift to left to show at least the type code */
                shift_view = -1;

            proto_tree_add_none_format(item, hf_amqp_type,
                                       tvb,
                                       offset+shift_view,
                                       (*length_size)-shift_view,
                                       "%s (%s): %s",
                                       name ? name : proto_registrar_get_name(hf_amqp_type),
                                       element_type->amqp_typename, value);
        }
    }
    else { /* no fixed code, i.e. compound (list, map, array) */
        switch (code) {
            case AMQP_1_0_TYPE_LIST0:
            case AMQP_1_0_TYPE_LIST8:
            case AMQP_1_0_TYPE_LIST32:
                *length_size = dissect_amqp_1_0_list(tvb,
                                                     pinfo,
                                                     offset-1, /* "-1" due to decode type again in the method */
                                                     item,
                                                     hf_amqp_type,
                                                     hf_amqp_subtype_count,
                                                     hf_amqp_subtypes, name);
                if (*length_size == 0) {
                    /* something went wrong during list dissection; let's stop here */
                    *length_size = tvb_reported_length_remaining(tvb, offset);
                } else {
                    *length_size -= 1; /* "-1" due to decode type again in the method */
                }
                break;
            case AMQP_1_0_TYPE_MAP8:
            case AMQP_1_0_TYPE_MAP32:
                 /* "-1" due to decode type again in the method */
                *length_size = dissect_amqp_1_0_map(tvb, pinfo, offset-1, item, hf_amqp_type, name)-1;
                break;
            case AMQP_1_0_TYPE_ARRAY8:
            case AMQP_1_0_TYPE_ARRAY32:
                *length_size = dissect_amqp_1_0_array(tvb,
                                                      pinfo,
                                                      offset-1, /* "-1" due to decode type again in the method */
                                                      item,
                                                      hf_amqp_type,
                                                      hf_amqp_subtype_count,
                                                      hf_amqp_subtypes, name)-1; /* "-1" due to decode type again in the method */
                break;
            default:
                expert_add_info_format(pinfo,
                                       item,
                                       &ei_amqp_unknown_amqp_type,
                                       "Unknown AMQP type %d (0x%x) of field \"%s\"",
                                       code, code,
                                       name ? name : proto_registrar_get_name(hf_amqp_type));
                *length_size = tvb_reported_length_remaining(tvb, offset); /* to stop dissecting */
                break;
        }
    }
    decrement_dissection_depth(pinfo);
}

/* It decodes 1.0 type, including type constructor
 * arguments: see get_amqp_1_0_value_formatter
 * return code: decoded format code of primitive type
 */
static unsigned
get_amqp_1_0_type_formatter(tvbuff_t *tvb,
                            packet_info* pinfo,
                            int offset,
                            int *hf_amqp_type,
                            const char **name,
                            uint32_t *hf_amqp_subtype_count,
                            int * const **hf_amqp_subtypes,
                            unsigned *length_size)
{
    int    i;
    int    code;
    int    format_code_type;
    unsigned  format_len = 0;
    unsigned  orig_offset = offset;

    code = tvb_get_uint8(tvb, offset);
    offset += 1;
    if (code == AMQP_1_0_TYPE_DESCRIPTOR_CONSTRUCTOR) {
        format_code_type = tvb_get_uint8(tvb, offset);
        offset += 1;
        if (format_code_type%16==0xf) { /* i.e. format codes like %x5F %x00-FF */
            offset += 1;
        }
        switch (format_code_type/16) {
        case 4: /* empty */
            format_len=0;
            break;
        case 5: /* fixed-one */
            format_len=1;
            code = (int)tvb_get_uint8(tvb, offset);
            break;
        case 6: /* fixed-two */
            format_len=2;
            code = (int)tvb_get_ntohs(tvb, offset);
            break;
        case 7: /* fixed-four */
            format_len=4;
            code = (int)tvb_get_ntohl(tvb, offset);
            break;
        case 8: /* fixed-eight */
            format_len=8;
            code = (int)tvb_get_ntoh64(tvb, offset);
            /* TODO: use a int64_t for 32-bit platforms? we never compare it to
             * anything bigger than an int anyways... */
            break;
        case 9: /* fixed-sixteen */
            format_len=16;
            /* TODO: somehow set code = next_128_bytes */
            break;
        case 0xa: /* variable-one */
            format_len = format_amqp_1_0_str(tvb, pinfo, offset, 1, name);
            break;
        case 0xb: /* variable-four */
            format_len = format_amqp_1_0_str(tvb, pinfo, offset, 4, name);
            break;
        /* TODO: could be type compound? or array? */
        }
        offset += format_len;
        for (i = 0; amqp_1_0_defined_types[i].format_code != 0x00; ++i) {
            if (amqp_1_0_defined_types[i].format_code == code) {
                *hf_amqp_type = *(amqp_1_0_defined_types[i].hf_amqp_type);
                *hf_amqp_subtype_count = amqp_1_0_defined_types[i].hf_amqp_subtype_count;
                *hf_amqp_subtypes = amqp_1_0_defined_types[i].hf_amqp_subtypes;
                break;
            }
        }
        /* now take the real primitive format code */
        code = tvb_get_uint8(tvb, offset);
        offset += 1;
    }
    *length_size = (offset-orig_offset);
    return code;
}

/* It decodes both 1.0 type and its value, in fact it just calls
 * get_amqp_1_0_type_formatter and get_amqp_1_0_value_formatter methods
 * arguments: see get_amqp_1_0_value_formatter
 */
static void
// NOLINTNEXTLINE(misc-no-recursion)
get_amqp_1_0_type_value_formatter(tvbuff_t *tvb,
                                  packet_info *pinfo,
                                  int offset,
                                  int hf_amqp_type,   /* what to print in GUI if name==NULL */
                                  const char *name,   /* what to print in GUI  */
                                  unsigned *length_size, /* decoded length */
                                  proto_item *item)
{
    int        code;
    uint32_t   hf_amqp_subtype_count = 0;
    int * const *hf_amqp_subtypes = NULL;
    const char *type_name = NULL;
    const char *format_name = NULL;
    unsigned   type_length_size;

    code = get_amqp_1_0_type_formatter(tvb,
                                       pinfo,
                                       offset,
                                       &hf_amqp_type,
                                       &type_name,
                                       &hf_amqp_subtype_count,
                                       &hf_amqp_subtypes,
                                       &type_length_size);
    if ((name != NULL) || (type_name != NULL))
    {
      if (type_name == NULL)
        format_name = name;
      else if (name == NULL)
        format_name = type_name;
      else
      {
        format_name = wmem_strdup_printf(pinfo->pool, "%s : %s", name, type_name);
      }
    }
    offset += type_length_size;
    get_amqp_1_0_value_formatter(tvb,
                                 pinfo,
                                 code,
                                 offset,
                                 hf_amqp_type,
                                 format_name,
                                 hf_amqp_subtype_count,
                                 hf_amqp_subtypes,
                                 length_size,
                                 item);
    *length_size += type_length_size;
}

static void
get_amqp_timestamp(nstime_t *nstime, tvbuff_t *tvb, unsigned offset)
{
    int64_t msec;

    msec = tvb_get_ntoh64(tvb, offset);
    nstime->secs = (time_t)(msec / 1000);
    nstime->nsecs = (int)(msec % 1000)*1000000;
}

static int
dissect_amqp_1_0_fixed(tvbuff_t *tvb, packet_info *pinfo _U_,
                       unsigned offset, unsigned length,
                       proto_item *item, int hf_amqp_type)
{
    proto_tree_add_item(item, hf_amqp_type, tvb, offset, length, ENC_BIG_ENDIAN);
    return length;
}


static bool find_data_dissector(tvbuff_t *msg_tvb, packet_info *pinfo, proto_tree *item)
{
    //get amqp to string field
    if (item == NULL) return false;

    GPtrArray *array = proto_find_finfo(item, hf_amqp_1_0_to_str);

    if (array == NULL) return false;
    if (array->len == 0) {
        g_ptr_array_free(array, true);
        return false;
    }

    field_info *fi = (field_info*)array->pdata[0];
    if (fi == NULL || !FT_IS_STRING(fvalue_type_ftenum(fi->value))) {
        g_ptr_array_free(array, true);
        return false;
    }

    const char* msg_to = fvalue_get_string(fi->value);

    amqp_message_decode_t *message_decode_entry = NULL;
    size_t topic_str_len;
    size_t topic_pattern_len;
    bool match_found = false;

    //compare amqp to string field with uat entries
    for (unsigned i = 0; i < num_amqp_message_decodes && !match_found; i++) {
        message_decode_entry = &amqp_message_decodes[i];
        switch (message_decode_entry->match_criteria) {

            case MATCH_CRITERIA_EQUAL:
                match_found = (strcmp(msg_to, message_decode_entry->topic_pattern) == 0);
                break;

            case MATCH_CRITERIA_CONTAINS:
                match_found = (strstr(msg_to, message_decode_entry->topic_pattern) != NULL);
                break;

            case MATCH_CRITERIA_STARTS_WITH:
                topic_str_len = strlen(msg_to);
                topic_pattern_len = strlen(message_decode_entry->topic_pattern);
                match_found = ((topic_str_len >= topic_pattern_len) &&
                               (strncmp(msg_to, message_decode_entry->topic_pattern, topic_pattern_len) == 0));
                break;

            case MATCH_CRITERIA_ENDS_WITH:
                topic_str_len = strlen(msg_to);
                topic_pattern_len = strlen(message_decode_entry->topic_pattern);
                match_found = ((topic_str_len >= topic_pattern_len) &&
                               (strcmp(msg_to + (topic_str_len - topic_pattern_len), message_decode_entry->topic_pattern) == 0));
                break;

            case MATCH_CRITERIA_REGEX:
                if (message_decode_entry->topic_regex) {
                  GMatchInfo *match_info = NULL;
                  g_regex_match(message_decode_entry->topic_regex, msg_to, (GRegexMatchFlags) 0, &match_info);
                  match_found = g_match_info_matches(match_info);
                  g_match_info_free(match_info);
                }
                break;

            default:
                /* Unknown match criteria */
                break;
        }


        if (match_found) {
            call_dissector_with_data(message_decode_entry->payload_proto, msg_tvb, pinfo, item , message_decode_entry->topic_more_info);
        }
    }


    g_ptr_array_free(array, true);

    return match_found;
}

static int
dissect_amqp_1_0_variable(tvbuff_t *tvb, packet_info *pinfo,
                          unsigned offset, unsigned length,
                          proto_item *item, int hf_amqp_type)
{
    unsigned bin_length;

    if (length == 1)
        bin_length = tvb_get_uint8(tvb, offset);
    else if (length == 4)
        bin_length = tvb_get_ntohl(tvb, offset);
    else {
        expert_add_info_format(pinfo, item, &ei_amqp_unknown_amqp_type,
                               "Invalid size of length indicator %d!", length);
        return length;
    }
    offset += length;

    bool is_dissected = false;
    if (hf_amqp_type == hf_amqp_1_0_data) {
        tvbuff_t *msg_tvb = tvb_new_subset_length(tvb, offset, bin_length);
        is_dissected = find_data_dissector(msg_tvb, pinfo, item);
    }

    if (!is_dissected) {
        proto_tree_add_item(item, hf_amqp_type, tvb, offset, bin_length, ENC_NA);
    }
    return length+bin_length;
}

static int
dissect_amqp_1_0_timestamp(tvbuff_t *tvb, packet_info *pinfo _U_,
                           unsigned offset, unsigned length,
                           proto_item *item, int hf_amqp_type)
{
    nstime_t nstime;
    get_amqp_timestamp(&nstime, tvb, offset);

    proto_tree_add_time(item, hf_amqp_type, tvb, offset, length, &nstime);
    return length;
}

static int
dissect_amqp_1_0_skip(tvbuff_t *tvb _U_, packet_info *pinfo _U_,
                      unsigned offset _U_, unsigned length _U_,
                      proto_item *item _U_, int hf_amqp_type _U_)
{
    /* null value means the respective field is omitted */
    return 0;
}

static int
dissect_amqp_1_0_zero(tvbuff_t *tvb, packet_info *pinfo,
                      unsigned offset, unsigned length _U_,
                      proto_item *item, int hf_amqp_type)
{
    switch(proto_registrar_get_ftype(hf_amqp_type))
    {
    case FT_UINT8:
    case FT_UINT16:
    case FT_UINT24:
    case FT_UINT32:
        proto_tree_add_uint(item, hf_amqp_type, tvb, offset-1, 1, 0);
        break;
    case FT_UINT40:
    case FT_UINT48:
    case FT_UINT56:
    case FT_UINT64:
        proto_tree_add_uint64(item, hf_amqp_type, tvb, offset-1, 1, 0L);
        break;
    case FT_INT8:
    case FT_INT16:
    case FT_INT24:
    case FT_INT32:
        proto_tree_add_int(item, hf_amqp_type, tvb, offset-1, 1, 0);
        break;
    case FT_INT40:
    case FT_INT48:
    case FT_INT56:
    case FT_INT64:
        proto_tree_add_int64(item, hf_amqp_type, tvb, offset-1, 1, 0L);
        break;
    default:
        expert_add_info_format(pinfo, item, &ei_amqp_unknown_amqp_type,
                               "Unexpected integer at frame position %d to list field \"%s\"",
                               offset,
                               proto_registrar_get_name(hf_amqp_type));
    }

    return 0;
}

static int
dissect_amqp_1_0_true(tvbuff_t *tvb, packet_info *pinfo _U_,
                      unsigned offset, unsigned length _U_,
                      proto_item *item, int hf_amqp_type)
{
    proto_tree_add_boolean(item, hf_amqp_type, tvb, offset-1, 1, true);
    return 0;
}

static int
dissect_amqp_1_0_false(tvbuff_t *tvb, packet_info *pinfo _U_,
                       unsigned offset, unsigned length _U_,
                       proto_item *item, int hf_amqp_type)
{
    proto_tree_add_boolean(item, hf_amqp_type, tvb, offset-1, 1, false);
    return 0;
}

static int
format_amqp_1_0_null(tvbuff_t *tvb _U_, packet_info* pinfo _U_,
                      unsigned offset _U_, unsigned length _U_,
                      const char **value)
{
    *value = "(null)";
    return 0;
}

static int
format_amqp_1_0_boolean_true(tvbuff_t *tvb _U_, packet_info* pinfo _U_,
                        unsigned offset _U_, unsigned length _U_,
                        const char **value)
{
    *value = "true";
    return 0;
}

static int
format_amqp_1_0_boolean_false(tvbuff_t *tvb _U_, packet_info* pinfo _U_,
                        unsigned offset _U_, unsigned length _U_,
                        const char **value)
{
    *value = "false";
    return 0;
}

static int
format_amqp_1_0_boolean(tvbuff_t *tvb, packet_info* pinfo _U_,
                        unsigned offset, unsigned length _U_,
                        const char **value)
{
    uint8_t val;

    val = tvb_get_uint8(tvb, offset);
    *value = val ? "true" : "false";
    return 1;
}

/* this covers ubyte, ushort, uint and ulong */
static int
format_amqp_1_0_uint(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value)
{
    uint64_t val;

    if (length == 0)
        val = 0;
    else if (length == 1)
        val = tvb_get_uint8(tvb, offset);
    else if (length == 2)
        val = tvb_get_ntohs(tvb, offset);
    else if (length == 4)
        val = tvb_get_ntohl(tvb, offset);
    else if (length == 8)
        val = tvb_get_ntoh64(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid uint length %d!", length);
        return length;
    }
    *value = wmem_strdup_printf(pinfo->pool, "%" PRIu64, val);
    return length;
}

/* this covers byte, short, int and long */
static int
format_amqp_1_0_int(tvbuff_t *tvb, packet_info* pinfo,
                    unsigned offset, unsigned length,
                    const char **value)
{
    int64_t val;

    if (length == 1)
        val = tvb_get_int8(tvb, offset);
    else if (length == 2)
        val = tvb_get_ntohis(tvb, offset);
    else if (length == 4)
        val = tvb_get_ntohil(tvb, offset);
    else if (length == 8)
        val = tvb_get_ntohi64(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid int length %d!", length);
        return length;
    }
    *value = wmem_strdup_printf(pinfo->pool, "%" PRIi64, val);
    return length;
}

static int
format_amqp_1_0_float(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                      const char **value)
{
    float floatval;
    floatval = tvb_get_ntohieee_float(tvb, offset);
    *value = wmem_strdup_printf(pinfo->pool, "%f", floatval);
    return 4;
}

static int
format_amqp_1_0_double(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                       const char **value)
{
    double doubleval;
    doubleval = tvb_get_ntohieee_double(tvb, offset);
    *value = wmem_strdup_printf(pinfo->pool, "%f", doubleval);
    return 8;
}

static int
format_amqp_1_0_decimal(tvbuff_t *tvb _U_, packet_info* pinfo _U_, unsigned offset _U_, unsigned length,
                        const char **value)
{
    /* TODO: this requires the _Decimal32 datatype from ISO/IEC TR 24732
     * and corresponding support in printf and glib
     */
    *value = "(not supported)";
    return length;
}

static int
format_amqp_1_0_char(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                     const char **value)
{
    /* one UTF-32BE encoded Unicode character */
    *value = tvb_get_string_enc(pinfo->pool, tvb, offset, 4, ENC_UCS_4 | ENC_BIG_ENDIAN);
    return 4;
}

static int
format_amqp_1_0_timestamp(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                          const char **value)
{
    nstime_t nstime;
    get_amqp_timestamp(&nstime, tvb, offset);

    *value = abs_time_to_str(pinfo->pool, &nstime, ABSOLUTE_TIME_UTC, false);
    return 8;
}

static int
format_amqp_1_0_uuid(tvbuff_t *tvb, packet_info* pinfo, unsigned offset, unsigned length _U_,
                     const char **value)
{
    e_guid_t uuid;
    tvb_get_guid(tvb, offset, &uuid, ENC_BIG_ENDIAN);
    *value = guid_to_str(pinfo->pool, &uuid);
    return 16;
}

static int
format_amqp_1_0_bin(tvbuff_t *tvb, packet_info* pinfo,
                    unsigned offset, unsigned length,
                    const char **value)
{
    unsigned bin_length;

    if (length == 1)
        bin_length = tvb_get_uint8(tvb, offset);
    else if (length == 4)
        bin_length = tvb_get_ntohl(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid binary length size %d!", length);
        return length;
    }
    offset += length;
    *value = tvb_bytes_to_str(pinfo->pool, tvb, offset, bin_length);
    return (length+bin_length);
}

static int
format_amqp_1_0_str(tvbuff_t *tvb, packet_info* pinfo,
                    unsigned offset, unsigned length,
                    const char **value)
{
    unsigned string_length;

    if (length == 1)
        string_length = tvb_get_uint8(tvb, offset);
    else if (length == 4)
        string_length = tvb_get_ntohl(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid string length size %d!", length);
        return length;
    }
    offset += length;
    *value = tvb_get_string_enc(pinfo->pool, tvb, offset, string_length, ENC_UTF_8|ENC_NA);
    /* offset += string_length; */
    return (string_length + length);
}

static int
format_amqp_1_0_symbol(tvbuff_t *tvb, packet_info* pinfo,
                       unsigned offset, unsigned length,
                       const char **value)
{
    unsigned symbol_length;
    if (length == 1)
        symbol_length = tvb_get_uint8(tvb, offset);
    else if (length == 4)
        symbol_length = tvb_get_ntohl(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid symbol length size %d!", length);
        return length;
    }
    offset += length;
    *value = tvb_get_string_enc(pinfo->pool, tvb, offset, symbol_length, ENC_ASCII|ENC_NA);
    /* offset += symbol_length; */
    return (symbol_length + length);
}


/*  AMQP 0-10 Type Decoders  */

static bool
get_amqp_0_10_type_formatter(uint8_t code, packet_info* pinfo,
                             const char **name,
                             type_formatter *formatter,
                             unsigned *length_size)
{
    int i;
    const struct amqp_typeinfo *table;

    if (code & 0x80)
        table = amqp_0_10_var_types;
    else
        table = amqp_0_10_fixed_types;
    for (i = 0; table[i].typecode != 0xff; ++i) {
        if (table[i].typecode == code) {
            *name        = wmem_strdup(pinfo->pool, table[i].amqp_typename);
            *formatter   = table[i].formatter;
            *length_size = table[i].known_size;
            return true;
        }
    }
    return false;
}

static int
format_amqp_0_10_bin(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value)
{
    *value = tvb_bytes_to_str(pinfo->pool, tvb, offset, length);
    return length;
}

static int
format_amqp_0_10_int(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value)
{
    int val;

    if (length == 1)
        val = tvb_get_int8(tvb, offset);
    else if (length == 2)
        val = tvb_get_ntohis(tvb, offset);
    else if (length == 4)
        val = tvb_get_ntohil(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid int length %d!", length);
        return length;
    }
    *value = wmem_strdup_printf(pinfo->pool, "%d", val);
    return length;
}

static int
format_amqp_0_10_uint(tvbuff_t *tvb, packet_info* pinfo,
                      unsigned offset, unsigned length,
                      const char **value)
{
    unsigned int val;

    if (length == 1)
        val = tvb_get_uint8(tvb, offset);
    else if (length == 2)
        val = tvb_get_ntohs(tvb, offset);
    else if (length == 4)
        val = tvb_get_ntohl(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid uint length %d!", length);
        return length;
    }
    *value = wmem_strdup_printf(pinfo->pool, "%u", val);
    return length;
}

static int
format_amqp_0_10_char(tvbuff_t *tvb, packet_info* pinfo,
                      unsigned offset, unsigned length _U_,
                      const char **value)
{
    *value = tvb_format_text(pinfo->pool, tvb, offset, 1);
    return 1;
}

static int
format_amqp_0_10_boolean(tvbuff_t *tvb, packet_info* pinfo,
                         unsigned offset, unsigned length _U_,
                         const char **value)
{
    uint8_t val;

    val = tvb_get_uint8(tvb, offset);
    *value = wmem_strdup(pinfo->pool, val ? "true" : "false");
    return 1;
}

static int
format_amqp_0_10_vbin(tvbuff_t *tvb, packet_info* pinfo,
                      unsigned offset, unsigned length,
                      const char **value)
{
    unsigned bin_length;

    if (length == 1)
        bin_length = tvb_get_uint8(tvb, offset);
    else if (length == 2)
        bin_length = tvb_get_ntohs(tvb, offset);
    else if (length == 4)
        bin_length = amqp_0_10_get_32bit_size(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid vbin length size %d!", length);
        return length;
    }
    offset += length;
    *value = tvb_bytes_to_str(pinfo->pool, tvb, offset, bin_length);
    /* offset += bin_length; */
    return (bin_length + length);
}

static int
format_amqp_0_10_str(tvbuff_t *tvb, packet_info* pinfo,
                     unsigned offset, unsigned length,
                     const char **value)
{
    unsigned string_length;

    if (length == 1)
        string_length = tvb_get_uint8(tvb, offset);
    else if (length == 2)
        string_length = tvb_get_ntohs(tvb, offset);
    else if (length == 4)
        string_length = amqp_0_10_get_32bit_size(tvb, offset);
    else {
        *value = wmem_strdup_printf(pinfo->pool, "Invalid string length size %d!", length);
        return length;
    }
    offset += length;
    *value = tvb_get_string_enc(pinfo->pool, tvb, offset, string_length, ENC_UTF_8|ENC_NA);
    /* offset += string_length; */
    return (string_length + length);
}

static void
format_amqp_0_10_sequence_set(tvbuff_t *tvb, unsigned offset, unsigned length,
                              proto_item *item)
{
    unsigned i, values;

    /* Must be 4-byte values */
    if ((length % 4) != 0) {
        proto_item_append_text(item, "Invalid sequence set length %u",
                               length);
    }

    values = length / 4;
    /* There must be pairs of values */
    if ((values % 2) != 0) {
        proto_item_append_text(item, "Invalid sequence set value count %u",
                               values);
    }
    proto_item_append_text(item, " [");
    for (i = 0; i < values; i += 2) {
        proto_item_append_text(item, "(%u, %u)%s",
                               tvb_get_ntohl(tvb, offset),
                               tvb_get_ntohl(tvb, offset + 4),
                               (i < (values - 2)) ? ", " : "");
        offset += 8;
        length -= 8;
    }
    proto_item_append_text(item, "]");
}

static void amqp_prompt(packet_info *pinfo _U_, char* result)
{
    snprintf(result, MAX_DECODE_AS_PROMPT_LEN, "AMQP version as");
}

static void *amqp_value(packet_info *pinfo)
{
    unsigned version = AMQP_V1_0;
    conversation_t *conv = find_conversation_pinfo(pinfo, 0);
    if (conv != NULL)
    {
        amqp_conv *conn = (amqp_conv *)conversation_get_proto_data(conv, proto_amqp);
        if (conn != NULL)
            version = conn->version;
    }

    return GUINT_TO_POINTER(version);
}

static int
dissect_amqpv0_9(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data)
{
    tcp_dissect_pdus(tvb, pinfo, tree, true, 7, get_amqp_0_9_message_len,
                         dissect_amqp_0_9_frame, data);
    return tvb_captured_length(tvb);
}

static int
dissect_amqpv0_10(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data)
{
    tcp_dissect_pdus(tvb, pinfo, tree, true, 8, get_amqp_0_10_message_len,
                         dissect_amqp_0_10_frame, data);
    return tvb_captured_length(tvb);
}

static int
dissect_amqpv1_0(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data)
{
    tcp_dissect_pdus(tvb, pinfo, tree, true, 8, get_amqp_1_0_message_len,
                         dissect_amqp_1_0_frame, data);
    return tvb_captured_length(tvb);
}

/*  Main dissection routine  */

static int
dissect_amqp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data)
{
    conversation_t *conv;
    amqp_conv *conn;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "AMQP");
    col_clear(pinfo->cinfo, COL_INFO);

    /* We need at least 8 bytes to check the protocol and get the frame size */
    if (tvb_reported_length (tvb) < 8) {
        /* But at this moment we don't know how much we will need */
        pinfo->desegment_len = DESEGMENT_ONE_MORE_SEGMENT;
        return -1; /* need more data */
    }

    /* Find (or build) conversation to remember the protocol version */
    conv = find_or_create_conversation(pinfo);
    conn = (amqp_conv *)conversation_get_proto_data(conv, proto_amqp);
    if (conn == NULL) {
        conn = wmem_new0(wmem_file_scope(), amqp_conv);
        conn->channels = wmem_map_new(wmem_file_scope(), g_direct_hash, g_direct_equal);
        conversation_add_proto_data(conv, proto_amqp, conn);
    }
    check_amqp_version(tvb, conn);
    /* Restore can_desegment to whatever TCP set it to before calling the
     * subdissector (which will decrement it a second time) in order for
     * tcp_dissect_pdus() to work as expected.
     */
    pinfo->can_desegment = pinfo->saved_can_desegment;
    if (!dissector_try_uint_with_data(version_table, conn->version, tvb, pinfo, tree, false, data))
    {
        col_append_str(pinfo->cinfo, COL_INFO, "AMQP (unknown version)");
        col_set_fence(pinfo->cinfo, COL_INFO);
    }

    return tvb_captured_length(tvb);
}

/*  Basic registration functions  */

void
proto_register_amqp(void)
{
    /*
     * Setup of field format array. A few of the 0-9 fields are reused
     * in 0-10, but there are many separate.
     */
    static hf_register_info hf[] = {
        {&hf_amqp_1_0_size, {
            "Length", "amqp.length",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Length of the frame", HFILL}},
        {&hf_amqp_1_0_doff, {
            "Doff", "amqp.doff",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            "Data offset", HFILL}},
        {&hf_amqp_1_0_type, {
            "Type", "amqp.type",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_type), 0x0,
            "Frame type", HFILL}},
        {&hf_amqp_1_0_amqp_performative, {
            "Performative", "amqp.performative",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_AMQP_performatives), 0x0,
            NULL, HFILL}},
        {&hf_amqp_1_0_sasl_method, {
            "SASL Method", "amqp.sasl.method",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_SASL_methods), 0x0,
            NULL, HFILL}},
        {&hf_amqp_1_0_list, {
            "list-item", "amqp.list",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_map, {
            "map-item", "amqp.map",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_containerId, {
            "Container-Id", "amqp.performative.arguments.containerId",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_hostname, {
            "Hostname", "amqp.performative.arguments.hostname",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_maxFrameSize, {
            "Max-Frame-Size", "amqp.performative.arguments.maxFrameSize",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_channelMax, {
            "Channel-Max", "amqp.performative.arguments.channelMax",
            FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_idleTimeOut, {
            "Idle-Timeout", "amqp.performative.arguments.idleTimeout",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_outgoingLocales, {
            "Outgoing-Locales", "amqp.performative.arguments.outgoingLocales",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_incomingLocales, {
            "Incoming-Locales", "amqp.performative.arguments.incomingLocales",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_offeredCapabilities, {
            "Offered-Capabilities", "amqp.arguments.offeredCapabilities",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_desiredCapabilities, {
            "Desired-Capabilities", "amqp.performative.arguments.desiredCapabilities",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_properties, {
            "Properties", "amqp.performative.arguments.properties",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_nextIncomingId, {
            "Next-Incoming-Id", "amqp.performative.arguments.nextIncomingId",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_deliveryCount, {
            "Delivery-Count", "amqp.performative.arguments.deliveryCount",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_sectionNumber, {
            "Section-Number", "amqp.received.sectionNumber",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Section number of received message", HFILL}},
        {&hf_amqp_1_0_sectionOffset, {
            "Section-Offset", "amqp.received.sectionOffset",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Section offset of received message", HFILL}},
        {&hf_amqp_1_0_deliveryFailed, {
            "Delivery-Failed", "amqp.modified.deliveryFailed",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_undeliverableHere, {
            "Undeliverable-Here", "amqp.modified.undeliverableHere",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_linkCredit, {
            "Link-Credit", "amqp.performative.arguments.linkCredit",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_available, {
            "Available", "amqp.performative.arguments.available",
            FT_UINT32, BASE_DEC, NULL, 0,
            "The number of available messages", HFILL}},
        {&hf_amqp_1_0_drain, {
            "Drain", "amqp.performative.arguments.drain",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Drain mode", HFILL}},
        {&hf_amqp_1_0_echo, {
            "Echo", "amqp.performative.arguments.echo",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Request state from partner", HFILL}},
        {&hf_amqp_1_0_deliveryId, {
            "Delivery-Id", "amqp.performative.arguments.deliveryId",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_deliveryTag, {
            "Delivery-Tag", "amqp.performative.arguments.deliveryTag",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_messageFormat, {
            "Message-Format", "amqp.performative.arguments.messageFormat",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_settled, {
            "Settled", "amqp.performative.arguments.settled",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_more, {
            "More", "amqp.performative.arguments.more",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "The message has more content", HFILL}},
        {&hf_amqp_1_0_state, {
            "State", "amqp.performative.arguments.state",
            FT_NONE, BASE_NONE, NULL, 0,
            "State of the delivery at sender", HFILL}},
        {&hf_amqp_1_0_resume, {
            "Resume", "amqp.performative.arguments.resume",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Resumed delivery", HFILL}},
        {&hf_amqp_1_0_aborted, {
            "Aborted", "amqp.performative.arguments.aborted",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Message is aborted", HFILL}},
        {&hf_amqp_1_0_batchable, {
            "Batchable", "amqp.performative.arguments.batchable",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Batchable hint", HFILL}},
        {&hf_amqp_1_0_first, {
            "First", "amqp.performative.arguments.first",
            FT_UINT32, BASE_DEC, NULL, 0,
            "Lower bound of deliveries", HFILL}},
        {&hf_amqp_1_0_last, {
            "Last", "amqp.performative.arguments.last",
            FT_UINT32, BASE_DEC, NULL, 0,
            "Upper bound of deliveries", HFILL}},
        {&hf_amqp_1_0_closed, {
            "Closed", "amqp.performative.arguments.closed",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Sender closed the link", HFILL}},
        {&hf_amqp_1_0_remoteChannel, {
            "Remote-Channel", "amqp.performative.arguments.remoteChannel",
            FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_nextOutgoingId, {
            "Next-Outgoing-Id", "amqp.performative.arguments.nextOutgoingId",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_incomingWindow, {
            "Incoming-Window", "amqp.performative.arguments.incomingWindow",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_outgoingWindow, {
            "Outgoing-Window", "amqp.performative.arguments.outgoingWindow",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_handleMax, {
            "Handle-Max", "amqp.performative.arguments.handleMax",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_name, {
            "Name", "amqp.performative.arguments.name",
            FT_STRING, BASE_NONE, NULL, 0,
            "Name of the link", HFILL}},
        {&hf_amqp_1_0_handle, {
            "Handle", "amqp.performative.arguments.handle",
            FT_UINT32, BASE_DEC, NULL, 0,
            "Handle for the link while attached", HFILL}},
        {&hf_amqp_1_0_role, {
            "Role", "amqp.performative.arguments.role",
            FT_BOOLEAN, BASE_NONE, TFS(&amqp_1_0_role_value), 0,
            "Role of the link endpoint", HFILL}},
        {&hf_amqp_1_0_sndSettleMode, {
            "Send-Settle-Mode", "amqp.performative.arguments.sndSettleMode",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_sndSettleMode_value), 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_rcvSettleMode, {
            "Receive-Settle-Mode", "amqp.performative.arguments.rcvSettleMode",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_rcvSettleMode_value), 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_source, {
            "Source", "amqp.performative.arguments.source",
            FT_NONE, BASE_NONE, NULL, 0,
            "Source for messages", HFILL}},
        {&hf_amqp_1_0_target, {
            "Target", "amqp.performative.arguments.target",
            FT_NONE, BASE_NONE, NULL, 0,
            "Target for messages", HFILL}},
        {&hf_amqp_1_0_deleteOnClose, {
            "Delete-On-Close", "amqp.lifetime-policy.deleteOnClose",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_deleteOnNoLinks, {
            "Delete-On-No-Links", "amqp.lifetime-policy.deleteOnNoLinks",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_deleteOnNoMessages, {
            "Delete-On-No-Messages", "amqp.lifetime-policy.deleteOnNoMessages",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_deleteOnNoLinksOrMessages, {
            "Delete-On-No-Links-Or-Messages", "amqp.lifetime-policy.deleteOnNoLinksOrMessages",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_coordinator, {
            "Coordinator", "amqp.tx.coordinator",
            FT_NONE, BASE_NONE, NULL, 0,
            "Transaction coordinator", HFILL}},
        {&hf_amqp_1_0_declare, {
            "Declare", "amqp.tx.declare",
            FT_NONE, BASE_NONE, NULL, 0,
            "Declare transaction", HFILL}},
        {&hf_amqp_1_0_globalId, {
            "Global-Id", "amqp.tx.arguments.globalId",
            FT_NONE, BASE_NONE, NULL, 0,
            "Global id of a transaction", HFILL}},
        {&hf_amqp_1_0_discharge, {
            "Discharge", "amqp.tx.discharge",
            FT_NONE, BASE_NONE, NULL, 0,
            "Discharge transaction", HFILL}},
        {&hf_amqp_1_0_txnId, {
            "Txn-Id", "amqp.tx.arguments.txnId",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Transaction id", HFILL}},
        {&hf_amqp_1_0_fail, {
            "Fail", "amqp.tx.arguments.fail",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Fail flag of transaction", HFILL}},
        {&hf_amqp_1_0_declared, {
            "Declared", "amqp.tx.declared",
            FT_NONE, BASE_NONE, NULL, 0,
            "Declared transaction", HFILL}},
        {&hf_amqp_1_0_transactionalState, {
            "Transactional-State", "amqp.tx.transactionalState",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_outcome, {
            "Outcome", "amqp.tx.arguments.outcome",
            FT_NONE, BASE_NONE, NULL, 0,
            "Outcome of transaction", HFILL}},
        {&hf_amqp_1_0_unsettled, {
            "Unsettled", "amqp.performative.arguments.unsettled",
            FT_NONE, BASE_NONE, NULL, 0,
            "Unsettled delivery state", HFILL}},
        {&hf_amqp_1_0_incompleteUnsettled, {
            "Incomplete-Unsettled", "amqp.performative.arguments.incompleteUnsettled",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_initialDeliveryCount, {
            "Initial-Delivery-Count", "amqp.performative.arguments.initDeliveryCount",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_maxMessageSize, {
            "Max-Message-Size", "amqp.performative.arguments.maxMessageSize",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_error, {
            "Error", "amqp.performative.arguments.error",
            FT_NONE, BASE_NONE, NULL, 0,
            "Error in a performative", HFILL}},
        {&hf_amqp_1_0_messageHeader, {
            "Message-Header", "amqp.header",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_messageProperties, {
            "Message-Properties", "amqp.properties",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_deliveryAnnotations, {
            "Delivery-Annotations", "amqp.deliveryAnnotations",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_messageAnnotations, {
            "Message-Annotations", "amqp.messageAnnotations",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_applicationProperties, {
            "Application-Properties", "amqp.applicationProperties",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_data, {
            "Data", "amqp.data",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Opaque binary data", HFILL}},
        {&hf_amqp_1_0_amqp_sequence, {
            "AMQP-Sequence", "amqp.sequence",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_amqp_value, {
            "AMQP-Value", "amqp.value",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_footer, {
            "Footer", "amqp.footer",
            FT_NONE, BASE_NONE, NULL, 0,
            "Message footer", HFILL}},
        {&hf_amqp_1_0_received, {
            "Received", "amqp.delivery-state.received",
            FT_NONE, BASE_NONE, NULL, 0,
            "Received messages", HFILL}},
        {&hf_amqp_1_0_accepted, {
            "Accepted", "amqp.delivery-state.accepted",
            FT_NONE, BASE_NONE, NULL, 0,
            "Accepted messages", HFILL}},
        {&hf_amqp_1_0_rejected, {
            "Rejected", "amqp.delivery-state.rejected",
            FT_NONE, BASE_NONE, NULL, 0,
            "Rejected messages", HFILL}},
        {&hf_amqp_1_0_released, {
            "Released", "amqp.delivery-state.released",
            FT_NONE, BASE_NONE, NULL, 0,
            "Released messages", HFILL}},
        {&hf_amqp_1_0_modified, {
            "Modified", "amqp.delivery-state.modified",
            FT_NONE, BASE_NONE, NULL, 0,
            "Modified messages", HFILL}},
        {&hf_amqp_1_0_condition, {
            "Condition", "amqp.error.condition",
            FT_STRING, BASE_NONE, NULL, 0,
            "Error condition", HFILL}},
        {&hf_amqp_1_0_description, {
            "Description", "amqp.error.description",
            FT_STRING, BASE_NONE, NULL, 0,
            "Error description", HFILL}},
        {&hf_amqp_1_0_info, {
            "Info", "amqp.error.info",
            FT_NONE, BASE_NONE, NULL, 0,
            "Error info", HFILL}},
        {&hf_amqp_1_0_address, {
            "Address", "amqp.performative.arguments.address",
            FT_NONE, BASE_NONE, NULL, 0,
            "Address of a node", HFILL}},
        {&hf_amqp_1_0_durable, {
            "Durable", "amqp.message.durable",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Message durability", HFILL}},
        {&hf_amqp_1_0_terminusDurable, {
            "Terminus-Durable", "amqp.performative.arguments.terminusDurable",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_terminus_durable_value), 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_priority, {
            "Priority", "amqp.message.priority",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Message priority", HFILL}},
        {&hf_amqp_1_0_ttl, {
            "Ttl", "amqp.message.ttl",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Time to live", HFILL}},
        {&hf_amqp_1_0_firstAcquirer, {
            "First-Acquirer", "amqp.message.firstAcquirer",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_expiryPolicy, {
            "Expiry-Policy", "amqp.properties.expiryPolicy",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_timeout, {
            "Timeout", "amqp.properties.timeout",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Duration that an expiring target will be retained", HFILL}},
        {&hf_amqp_1_0_dynamic, {
            "Dynamic", "amqp.properties.dynamic",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            "Dynamic creation of a remote node", HFILL}},
        {&hf_amqp_1_0_dynamicNodeProperties, {
            "Dynamic-Node-Properties", "amqp.properties.dynamicNodeProperties",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_distributionMode, {
            "Distribution-Mode", "amqp.properties.distributionMode",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_filter, {
            "Filter", "amqp.properties.filter",
            FT_NONE, BASE_NONE, NULL, 0,
            "Predicates to filter messages admitted to the link", HFILL}},
        {&hf_amqp_1_0_defaultOutcome, {
            "Default-Outcome", "amqp.properties.defaultOutcome",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_outcomes, {
            "Outcomes", "amqp.properties.outcomes",
            FT_NONE, BASE_NONE, NULL, 0,
            "Outcomes descriptors for the link", HFILL}},
        {&hf_amqp_1_0_capabilities, {
            "Capabilities", "amqp.properties.capabilities",
            FT_NONE, BASE_NONE, NULL, 0,
            "Extension capabilities of the sender", HFILL}},
        {&hf_amqp_1_0_messageId, {
            "Message-Id", "amqp.message.messageId",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_userId, {
            "User-Id", "amqp.message.userId",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_to, {
            "To", "amqp.message.to",
            FT_NONE, BASE_NONE, NULL, 0,
            "Destination address of the message", HFILL}},
        {&hf_amqp_1_0_subject, {
            "Subject", "amqp.message.subject",
            FT_STRING, BASE_NONE, NULL, 0,
            "Message subject", HFILL}},
        {&hf_amqp_1_0_replyTo, {
            "Reply-To", "amqp.message.replyTo",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_correlationId, {
            "Correlation-Id", "amqp.message.correlationId",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_contentType, {
            "Content-Type", "amqp.message.contentType",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_contentEncoding, {
            "Content-Encoding", "amqp.message.contentEncoding",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_absoluteExpiryTime, {
            "Expiry-Time", "amqp.message.expiryTime",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0,
            "Absolute expiry time", HFILL}},
        {&hf_amqp_1_0_creationTime, {
            "Creation-Time", "amqp.message.creationTime",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_groupId, {
            "Group-Id", "amqp.message.groupId",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_groupSequence, {
            "Group-Sequence", "amqp.message.groupSequence",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_replyToGroupId, {
            "Reply-To-Group-Id", "amqp.message.replyToGroupId",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_mechanisms, {
            "Mechanisms", "amqp.sasl.mechanisms",
            FT_NONE, BASE_NONE, NULL, 0,
            "Supported security mechanisms", HFILL}},
        {&hf_amqp_1_0_mechanism, {
            "Mechanism", "amqp.sasl.mechanism",
            FT_STRING, BASE_NONE, NULL, 0,
            "Chosen security mechanism", HFILL}},
        {&hf_amqp_1_0_initResponse, {
            "Init-Response", "amqp.sasl.initResponse",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_saslChallenge, {
            "Challenge", "amqp.sasl.challenge",
            FT_BYTES, BASE_NONE, NULL, 0,
            "SASL challenge", HFILL}},
        {&hf_amqp_1_0_saslResponse, {
            "Response", "amqp.sasl.response",
            FT_BYTES, BASE_NONE, NULL, 0,
            "SASL response", HFILL}},
        {&hf_amqp_1_0_saslCode, {
            "Code", "amqp.sasl.saslCode",
            FT_UINT8, BASE_DEC, VALS(amqp_1_0_SASL_code_value), 0,
            "SASL outcome code", HFILL}},
        {&hf_amqp_1_0_saslAdditionalData, {
            "Additional-Data", "amqp.sasl.addData",
            FT_BYTES, BASE_NONE, NULL, 0,
            "SASL outcome additional data", HFILL}},
        {&hf_amqp_1_0_outgoingLocales_sym, {
            "Outgoing-Locales", "amqp.performative.arguments.outgoingLocales_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_incomingLocales_sym, {
            "Incoming-Locales", "amqp.performative.arguments.incomingLocales_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_offeredCapabilities_sym, {
            "Offered-Capabilities", "amqp.arguments.offeredCapabilities_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_desiredCapabilities_sym, {
            "Desired-Capabilities", "amqp.performative.arguments.desiredCapabilities_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_address_str, {
            "Address", "amqp.performative.arguments.address.string",
            FT_STRING, BASE_NONE, NULL, 0,
            "Address of a node", HFILL}},
        {&hf_amqp_1_0_source_str, {
            "Source", "amqp.performative.arguments.source.string",
            FT_STRING, BASE_NONE, NULL, 0,
            "Source for messages", HFILL}},
        {&hf_amqp_1_0_target_str, {
            "Target", "amqp.performative.arguments.target.string",
            FT_STRING, BASE_NONE, NULL, 0,
            "Target for messages", HFILL}},
        {&hf_amqp_1_0_outcomes_sym, {
            "Outcomes", "amqp.properties.outcomes_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            "Outcomes descriptors for the link", HFILL}},
        {&hf_amqp_1_0_capabilities_sym, {
            "Capabilities", "amqp.properties.capabilities_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            "Extension capabilities of the sender", HFILL}},
        {&hf_amqp_1_0_messageId_uint, {
            "Message-Id", "amqp.message.messageId.uint",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_messageId_str, {
            "Message-Id", "amqp.message.messageId.string",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_messageId_bin, {
            "Message-Id", "amqp.message.messageId.bytes",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_messageId_uuid, {
            "Message-Id", "amqp.message.messageId.guid",
            FT_GUID, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_correlationId_uint, {
            "Correlation-Id", "amqp.message.correlationId.uint",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_correlationId_str, {
            "Correlation-Id", "amqp.message.correlationId.string",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_correlationId_bin, {
            "Correlation-Id", "amqp.message.correlationId.bytes",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_correlationId_uuid, {
            "Correlation-Id", "amqp.message.correlationId.guid",
            FT_GUID, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_to_str, {
            "To", "amqp.message.to.string",
            FT_STRING, BASE_NONE, NULL, 0,
            "Destination address of the message", HFILL}},
        {&hf_amqp_1_0_replyTo_str, {
            "Reply-To", "amqp.message.replyTo.string",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_1_0_mechanisms_sym, {
            "Mechanisms", "amqp.sasl.mechanisms_sym",
            FT_STRING, BASE_NONE, NULL, 0,
            "Supported security mechanisms", HFILL}},
        {&hf_amqp_0_10_format, {
            "Format", "amqp.format",
            FT_UINT8, BASE_DEC, NULL, 0xc0,
            "Framing version", HFILL}},
        {&hf_amqp_0_10_position, {
            "Position", "amqp.frame-position",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_frame_position), 0x0f,
            "Framing position", HFILL}},
        {&hf_amqp_0_10_type, {
            "Type", "amqp.type",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_frame_types), 0x0,
            "Frame type", HFILL}},
        {&hf_amqp_0_10_size, {
            "Length", "amqp.length",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            "Length of the frame", HFILL}},
        {&hf_amqp_0_10_track, {
            "Track", "amqp.track-number",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_frame_tracks), 0x0,
            "Track number", HFILL}},
        {&hf_amqp_0_10_class, {
            "Class", "amqp.class",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_class), 0x0,
            "Class ID", HFILL}},
        {&hf_amqp_0_10_connection_method, {
            "Method", "amqp.connection.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_connection_methods), 0x0,
            "Connection Class Method", HFILL}},
        {&hf_amqp_0_10_session_method, {
            "Method", "amqp.session.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_session_methods), 0x0,
            "Session Class Method", HFILL}},
        {&hf_amqp_0_10_execution_method, {
            "Method", "amqp.execution.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_execution_methods), 0x0,
            "Execution Class Method", HFILL}},
        {&hf_amqp_0_10_message_method, {
            "Method", "amqp.message.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_message_methods), 0x0,
            "Message Class Method", HFILL}},
        {&hf_amqp_0_10_tx_method, {
            "Method", "amqp.tx.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_tx_methods), 0x0,
            "Tx Class Method", HFILL}},
        {&hf_amqp_0_10_dtx_method, {
            "Method", "amqp.dtx.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_dtx_methods), 0x0,
            "Dtx Class Method", HFILL}},
        {&hf_amqp_0_10_exchange_method, {
            "Method", "amqp.exchange.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_exchange_methods), 0x0,
            "Exchange Class Method", HFILL}},
        {&hf_amqp_0_10_queue_method, {
            "Method", "amqp.queue.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_queue_methods), 0x0,
            "Queue Class Method", HFILL}},
        {&hf_amqp_0_10_file_method, {
            "Method", "amqp.file.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_file_methods), 0x0,
            "File Class Method", HFILL}},
        {&hf_amqp_0_10_stream_method, {
            "Method", "amqp.stream.method",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_stream_methods), 0x0,
            "Stream Class Method", HFILL}},
        {&hf_amqp_0_10_message_body, {
            "Message body", "amqp.message-body",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Message body content", HFILL}},
        {&hf_amqp_0_10_dtx_xid, {
            "Xid", "amqp.dtx.xid",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Dtx transaction id", HFILL}},
        {&hf_amqp_0_10_dtx_xid_format, {
            "Format", "amqp.dtx.xid.format",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Implementation-specific xid format code", HFILL}},
        {&hf_amqp_0_10_dtx_xid_global_id, {
            "Global-id", "amqp.dtx.xid.global-id",
            FT_UINT_BYTES, BASE_NONE, NULL, 0x0,
            "Global transaction id", HFILL}},
        {&hf_amqp_0_10_dtx_xid_branch_id, {
            "Branch-id", "amqp.dtx.xid.branch-id",
            FT_UINT_BYTES, BASE_NONE, NULL, 0x0,
            "Transaction branch qualifier", HFILL}},
        {&hf_amqp_0_10_struct32_size, {
            "Size", "amqp.struct32_size",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct32, {
            "struct", "amqp.struct32",
            FT_UINT16, BASE_HEX, VALS(amqp_0_10_struct32_vals), 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct32_class, {
            "Class", "amqp.struct32.class",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_class), 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct32_struct, {
            "Struct", "amqp.struct32.struct",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct32_padding, {
            "Padding", "amqp.struct32.padding",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_array_type, {
            "Type", "amqp.array.type",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_array_type_vals), 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_array_element_count, {
            "Element count", "amqp.array.element_count",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_array_string, {
            "String", "amqp.array.string",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_discard_unroutable, {
            "Discard-unroutable", "amqp.message.delivery-properties.discard-unroutable",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x01,
            "Discard message if unroutable", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_immediate, {
            "Immediate", "amqp.message.delivery-properties.immediate",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Consider unroutable if can't be routed immediately", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_redelivered, {
            "Redelivered", "amqp.message.delivery-properties.redelivered",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x04,
            "Message may have been previously delivered", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_priority, {
            "Delivery-priority", "amqp.message.delivery-properties.delivery-priority",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_struct_delivery_properties_priorities), 0x0,
            "Message delivery priority", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_mode, {
            "Delivery-mode", "amqp.message.delivery-properties.delivery-mode",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_struct_delivery_properties_modes), 0x0,
            "Message delivery persistence mode", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_ttl, {
            "TTL", "amqp.message.delivery-properties.ttl",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Message time-to-live in msec", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_timestamp, {
            "Timestamp", "amqp.message.delivery-properties.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            "Time of arrival at broker", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_expiration, {
            "Expiration", "amqp.message.delivery-properties.expiration",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            "Expiration time calculated by broker", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_exchange, {
            "Exchange", "amqp.message.delivery-properties.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Originating exchange", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_routing_key, {
            "Routing-key", "amqp.message.delivery-properties.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Message routing key", HFILL}},
        {&hf_amqp_0_10_struct_delivery_properties_resume_ttl, {
            "Resume-ttl", "amqp.message.delivery-properties.resume-ttl",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "TTL to use when resuming", HFILL}},
        {&hf_amqp_0_10_struct_fragment_properties_first, {
            "First", "amqp.message.fragment-properties.first",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x01,
            "Fragment contains the start of the message", HFILL}},
        {&hf_amqp_0_10_struct_fragment_properties_last, {
            "Last", "amqp.message.fragment-properties.last",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Fragment contains the end of the message", HFILL}},
        {&hf_amqp_0_10_struct_fragment_properties_size, {
            "Fragment-size", "amqp.message.fragment-properties.fragment-size",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Size of the message fragment", HFILL}},
#if 0
        {&hf_amqp_0_10_struct_message_properties, {
            "message.message-properties", "amqp.message.message-properties",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Message properties struct", HFILL}},
#endif
        {&hf_amqp_0_10_struct_message_properties_content_len, {
            "Content-length", "amqp.message.message-properties.content-length",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Length of associated message", HFILL}},
        {&hf_amqp_0_10_struct_message_properties_message_id, {
            "Message-id", "amqp.message.message-properties.message-id",
            FT_GUID, BASE_NONE, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_message_properties_correlation, {
            "Correlation-id", "amqp.message.message-properties.correlation-id",
            FT_UINT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_message_properties_reply_to, {
            "Reply-to", "amqp.message.message-properties.reply-to",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Address to reply to", HFILL}},
        {&hf_amqp_0_10_struct_message_properties_content_type, {
            "Content-type", "amqp.message.message-properties.content-type",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "MIME content type", HFILL}},
        {&hf_amqp_0_10_struct_message_properties_content_encoding, {
            "Content-encoding", "amqp.message.message-properties.content-encoding",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "MIME content encoding method", HFILL}},
        {&hf_amqp_0_10_struct_message_properties_user_id, {
            "User-id", "amqp.message.message-properties.user-id",
            FT_UINT_BYTES, BASE_NONE, NULL, 0x0,
            "Creating user id", HFILL}},
        {&hf_amqp_0_10_struct_message_properties_app_id, {
            "App-id", "amqp.message.message-properties.app-id",
            FT_UINT_BYTES, BASE_NONE, NULL, 0x0,
            "Creating user id", HFILL}},
        {&hf_amqp_0_10_struct_message_properties_application_headers, {
            "Application-headers", "amqp.message.message-properties.application-headers",
            FT_NONE, BASE_NONE, NULL, 0,
            "Application-private headers", HFILL}},
        {&hf_amqp_0_10_struct_reply_to_exchange, {
            "Exchange", "amqp.message.message-properties.reply-to.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Exchange to reply to", HFILL}},
        {&hf_amqp_0_10_struct_reply_to_routing_key, {
            "Routing-key", "amqp.message.message-properties.reply-to.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Routing key to reply with", HFILL}},
        {&hf_amqp_0_10_struct_acquired_transfers, {
            "Transfers", "amqp.message.acquired.transfers",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Command set", HFILL}},
        {&hf_amqp_0_10_struct_resume_result_offset, {
            "Offset", "amqp.message.resume-result.offset",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Amount of data already transferred", HFILL}},
        {&hf_amqp_0_10_struct_exchange_query_result_durable, {
            "Durable", "amqp.exchange.exchange-query-result.durable",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Exchange is durable", HFILL}},
        {&hf_amqp_0_10_struct_exchange_query_result_not_found, {
            "Not-found", "amqp.exchange.exchange-query-result.not-found",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Exchange was not found", HFILL}},
        {&hf_amqp_0_10_struct_exchange_bound_result_exchange_not_found, {
            "Exchange-not-found", "amqp.exchange.exchange-bound-result.exchange-not-found",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_exchange_bound_result_queue_not_found, {
            "Queue-not-found", "amqp.exchange.exchange-bound-result.queue-not-found",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_exchange_bound_result_queue_not_matched, {
            "Queue-not-matched", "amqp.exchange.exchange-bound-result.queue-not-matched",
            FT_BOOLEAN, 8, NULL, 0x04,
            "No binding from exchange to queue", HFILL}},
        {&hf_amqp_0_10_struct_exchange_bound_result_key_not_matched, {
            "Key-not-matched", "amqp.exchange.exchange-bound-result.key-not-matched",
            FT_BOOLEAN, 8, NULL, 0x08,
            "No binding from exchange with binding-key", HFILL}},
        {&hf_amqp_0_10_struct_exchange_bound_result_args_not_matched, {
            "Args-not-matched", "amqp.exchange.exchange-bound-result.args-not-matched",
            FT_BOOLEAN, 8, NULL, 0x10,
            "No binding from exchange with specified arguments", HFILL}},
        {&hf_amqp_0_10_struct_queue_query_result_durable, {
            "Durable", "amqp.queue.queue-query-result.durable",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x04,
            "Queue is durable", HFILL}},
        {&hf_amqp_0_10_struct_queue_query_result_exclusive, {
            "Exclusive", "amqp.queue.queue-query-result.exclusive",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x08,
            "Queue created exclusive-use", HFILL}},
        {&hf_amqp_0_10_struct_queue_query_result_auto_delete, {
            "Auto-delete", "amqp.queue.queue-query-result.auto-delete",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x10,
            "Queue created auto-delete", HFILL}},
        {&hf_amqp_0_10_struct_queue_query_result_message_count, {
            "Message-count", "amqp.queue.queue-query-result.message-count",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Number of messages in the queue", HFILL}},
        {&hf_amqp_0_10_struct_queue_query_result_subscriber_count, {
            "Subscriber-count", "amqp.queue.queue-query-result.subscriber-count",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Number of subscribers for the queue", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_content_type, {
            "Content-type", "amqp.file.file-properties.content-type",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "MIME content type", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_content_encoding, {
            "Content-encoding", "amqp.file.file-properties.content-encoding",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "MIME content encoding", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_headers, {
            "Headers", "amqp.file.file-properties.headers",
            FT_NONE, BASE_NONE, NULL, 0,
            "Message header fields", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_priority, {
            "Priority", "amqp.file.file-properties.priority",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Message priority, 0 to 9", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_reply_to, {
            "Reply-to", "amqp.file.file-properties.reply-to",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Destination to reply to", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_message_id, {
            "Message-id", "amqp.file.file-properties.message-id",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Application message identifier", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_filename, {
            "Filename", "amqp.file.file-properties.filename",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Message filename", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_timestamp, {
            "Timestamp", "amqp.file.file-properties.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            "Message timestamp", HFILL}},
        {&hf_amqp_0_10_struct_file_properties_cluster_id, {
            "Cluster-id", "amqp.file.file-properties.cluster-id",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "Intra-cluster routing identifier", HFILL}},
        {&hf_amqp_0_10_struct_stream_properties_content_type, {
            "Content-type", "amqp.stream.stream-properties.content-type",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "MIME content type", HFILL}},
        {&hf_amqp_0_10_struct_stream_properties_content_encoding, {
            "Content-encoding", "amqp.stream.stream-properties.content-encoding",
            FT_UINT_STRING, BASE_NONE, NULL, 0x0,
            "MIME content encoding", HFILL}},
        {&hf_amqp_0_10_struct_stream_properties_headers, {
            "Headers", "amqp.stream.stream-properties.headers",
            FT_NONE, BASE_NONE, NULL, 0,
            "Message header fields", HFILL}},
        {&hf_amqp_0_10_struct_stream_properties_priority, {
            "Priority", "amqp.stream.stream-properties.priority",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Message priority, 0 to 9", HFILL}},
        {&hf_amqp_0_10_struct_stream_properties_timestamp, {
            "Timestamp", "amqp.stream.stream-properties.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            "Message timestamp", HFILL}},
        {&hf_amqp_0_10_argument_packing_flags, {
            "Packing Flags", "amqp.struct.packing",
            FT_UINT16, BASE_HEX, NULL, 0xffff,
            "Argument Struct Packing Flags", HFILL}},
        {&hf_amqp_0_10_session_header, {
            "Session header", "amqp.session.header",
            FT_UINT16, BASE_HEX, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_session_header_sync, {
            "Sync", "amqp.session.header.sync",
            FT_BOOLEAN, 8, TFS(&amqp_0_10_session_header_sync), 0x01,
            "Sync requested", HFILL}},
        {&hf_amqp_0_10_method_session_attach_name, {
            "Session Name", "amqp.session.attach.name",
            FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_session_attach_name_size, {
            "Size", "amqp.session.attach.name.size",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_session_attach_force, {
            "Session forced", "amqp.session.attach.force",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_session_detached_code, {
            "Code", "amqp.session.detached.code",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_method_session_detached_codes), 0x0,
            "Reason for detach", HFILL}},
        {&hf_amqp_0_10_method_session_timeout, {
            "Timeout", "amqp.session.timeout",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Session timeout (seconds)", HFILL}},
        {&hf_amqp_0_10_method_session_completed_timely, {
            "Timely-reply", "amqp.session.completed.timely-reply",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Timely reply requested", HFILL}},
        {&hf_amqp_0_10_method_session_flush_expected, {
            "Expected", "amqp.session.flush.expected",
            FT_BOOLEAN, 8, TFS(&tfs_set_notset), 0x01,
            "Request notification of expected commands", HFILL}},
        {&hf_amqp_0_10_method_session_flush_confirmed, {
            "Confirmed", "amqp.session.flush.confirmed",
            FT_BOOLEAN, 8, TFS(&tfs_set_notset), 0x02,
            "Request notification of confirmed commands", HFILL}},
        {&hf_amqp_0_10_method_session_flush_completed, {
            "Completed", "amqp.session.flush.completed",
            FT_BOOLEAN, 8, TFS(&tfs_set_notset), 0x04,
            "Request notification of completed commands", HFILL}},
        {&hf_amqp_0_10_method_session_command_point_id, {
            "Command-id", "amqp.session.command_point.command_id",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Next command's sequence number", HFILL}},
        {&hf_amqp_0_10_method_session_command_point_offset, {
            "Command-offset", "amqp.session.command_point.command_offset",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Byte offset within command", HFILL}},
        {&hf_amqp_0_10_method_session_commands, {
            "Commands", "amqp.session.expected.commands",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Command set", HFILL}},
        {&hf_amqp_0_10_method_session_fragments, {
            "Fragments", "amqp.session.expected.fragments",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Command Fragments", HFILL}},
        {&hf_amqp_0_10_method_execution_command_id, {
            "Command-id", "amqp.execution.command_id",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Command's sequence number", HFILL}},
        {&hf_amqp_0_10_method_execution_exception_error, {
            "Error-code", "amqp.execution.exception.error-code",
            FT_UINT16, BASE_DEC, VALS(amqp_0_10_method_execution_exception_errors), 0x0,
            "Exception error code", HFILL}},
        {&hf_amqp_0_10_method_execution_field_index, {
            "Field-index", "amqp.execution.exception.field-index",
            FT_UINT8, BASE_DEC, NULL, 0x0,
            "0-based index of exceptional field", HFILL}},
        {&hf_amqp_0_10_method_execution_description, {
            "Description", "amqp.execution.exception.description",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Description of exception", HFILL}},
        {&hf_amqp_0_10_method_execution_error_info, {
            "Error-info", "amqp.execution.exception.error-info",
            FT_NONE, BASE_NONE, NULL, 0,
            "client-properties", HFILL}},
        {&hf_amqp_0_10_method_message_transfer_destination, {
            "Destination", "amqp.message.transfer.destination",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message destination", HFILL}},
        {&hf_amqp_0_10_method_message_transfer_accept_mode, {
            "Accept-mode", "amqp.message.transfer.accept-mode",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_message_transfer_accept_modes), 0x0,
            "Message accept mode", HFILL}},
        {&hf_amqp_0_10_method_message_transfer_acquire_mode, {
            "Acquire-mode", "amqp.message.transfer.acquire-mode",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_message_transfer_acquire_modes), 0x0,
            "Message acquire mode", HFILL}},
        {&hf_amqp_0_10_method_message_accept_transfers, {
            "Commands", "amqp.message.accept.transfers",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Previously transferred messages", HFILL}},
        {&hf_amqp_0_10_method_message_transfer_reject_code, {
            "Reject-code", "amqp.message.reject.reject-code",
            FT_UINT16, BASE_DEC, VALS(amqp_0_10_message_transfer_reject_codes), 0x0,
            "Message reject code", HFILL}},
        {&hf_amqp_0_10_method_message_reject_text, {
            "Text", "amqp.message.reject.text",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Reject description", HFILL}},
        {&hf_amqp_0_10_method_message_release_set_redelivered, {
            "Set-redelivered", "amqp.message.release.set-redelivered",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Mark redelivered on next transfer from queue", HFILL}},
        {&hf_amqp_0_10_method_message_dest, {
            "Destination", "amqp.message.destination",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message destination", HFILL}},
        {&hf_amqp_0_10_method_message_resume_id, {
            "Resume-Id", "amqp.message.resume.id",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message id to resume", HFILL}},
        {&hf_amqp_0_10_method_message_subscribe_queue, {
            "Queue", "amqp.message.subscribe.queue",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Queue to subscribe to", HFILL}},
        {&hf_amqp_0_10_method_message_subscribe_exclusive, {
            "Exclusive", "amqp.message.subscribe.exclusive",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x10,
            "Request exclusive subscription", HFILL}},
        {&hf_amqp_0_10_method_message_subscribe_resume_ttl, {
            "Resume-ttl", "amqp.message.subscribe.resume_ttl",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "TTL to use when resuming", HFILL}},
        {&hf_amqp_0_10_method_message_subscribe_args, {
            "Extended arguments", "amqp.message.subscribe.arguments",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Implementation-specific arguments", HFILL}},
        {&hf_amqp_0_10_method_message_flow_mode, {
            "Flow-mode", "amqp.message.flow-mode",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_message_flow_modes), 0x0,
            "Method for allocating message flow credit", HFILL}},
        {&hf_amqp_0_10_method_message_credit_unit, {
            "Credit-unit", "amqp.message.flow.credit-unit",
            FT_UINT8, BASE_DEC, VALS(amqp_0_10_message_credit_units), 0x0,
            "Unit of message flow value", HFILL}},
        {&hf_amqp_0_10_method_message_credit_value, {
            "Value", "amqp.message.flow.value",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Message flow value", HFILL}},
        {&hf_amqp_0_10_method_dtx_start_join, {
            "Join", "amqp.dtx.start.join",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Join with existing xid", HFILL}},
        {&hf_amqp_0_10_method_dtx_start_resume, {
            "Resume", "amqp.dtx.start.resume",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x04,
            "Resume suspended transaction", HFILL}},
        {&hf_amqp_0_10_method_dtx_end_fail, {
            "Fail", "amqp.dtx.end.fail",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "This portion of work has failed", HFILL}},
        {&hf_amqp_0_10_method_dtx_end_suspend, {
            "Suspend", "amqp.dtx.end.suspend",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x04,
            "Temporarily suspending transaction", HFILL}},
        {&hf_amqp_0_10_method_dtx_commit_one_phase, {
            "One-phase", "amqp.dtx.commit.one-phase",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Use one-phase optimization", HFILL}},
        {&hf_amqp_0_10_method_dtx_set_timeout_timeout, {
            "Timeout", "amqp.dtx.set-timeout.timeout",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Transaction timeout value in seconds", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_exchange, {
            "Exchange", "amqp.exchange.declare.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange to declare", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_type, {
            "Type", "amqp.exchange.declare.type",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Type of exchange to declare", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_alt_exchange, {
            "Alternate-exchange", "amqp.exchange.declare.alternate-exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Alternate exchange for unroutable messages", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_passive, {
            "Passive", "amqp.exchange.declare.passive",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x08,
            "Do not create the exchange", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_durable, {
            "Durable", "amqp.exchange.declare.durable",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x10,
            "Create a durable exchange", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_auto_delete, {
            "Auto-delete", "amqp.exchange.declare.auto-delete",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x20,
            "Delete exchange when last binding removed", HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_arguments, {
            "Arguments", "amqp.exchange.declare.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            "Declaration arguments", HFILL}},
        {&hf_amqp_0_10_method_exchange_delete_if_unused, {
            "If-unused", "amqp.exchange.delete.if-unused",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Delete exchange only if it has no queue bindings", HFILL}},
        {&hf_amqp_0_10_method_exchange_bind_queue, {
            "Queue", "amqp.exchange.bind.queue",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Queue to bind to", HFILL}},
        {&hf_amqp_0_10_method_exchange_binding_key, {
            "Binding-key", "amqp.exchange.bind.binding-key",
            FT_STRING, BASE_NONE, NULL, 0,
            "Binding between exchange and queue", HFILL}},
        {&hf_amqp_0_10_method_queue_name, {
            "Queue", "amqp.queue.declare.queue",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Queue name", HFILL}},
        {&hf_amqp_0_10_method_queue_alt_exchange, {
            "Alternate-exchange", "amqp.queue.declare.alternate-exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_queue_declare_passive, {
            "Passive", "amqp.queue.declare.passive",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x04,
            "Do not create the queue", HFILL}},
        {&hf_amqp_0_10_method_queue_declare_durable, {
            "Durable", "amqp.queue.declare.durable",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x08,
            "Create a durable queue", HFILL}},
        {&hf_amqp_0_10_method_queue_declare_exclusive, {
            "Exclusive", "amqp.queue.declare.exclusive",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x10,
            "Create a queue usable from only one session", HFILL}},
        {&hf_amqp_0_10_method_queue_declare_auto_delete, {
            "Auto-delete", "amqp.queue.declare.auto-delete",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x20,
            "Delete queue when all uses completed", HFILL}},
        {&hf_amqp_0_10_method_queue_declare_arguments, {
            "Arguments", "amqp.queue.declare.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            "Declaration arguments", HFILL}},
        {&hf_amqp_0_10_method_queue_delete_if_unused, {
            "If-unused", "amqp.queue.delete.if-unused",
            FT_BOOLEAN, 8, NULL, 0x02,
            "Delete the queue only if there are no consumers", HFILL}},
        {&hf_amqp_0_10_method_queue_delete_if_empty, {
            "If-empty", "amqp.queue.delete.if-empty",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Delete queue only if empty", HFILL}},
        {&hf_amqp_0_10_method_file_qos_prefetch_size, {
            "Prefetch-size", "amqp.file.qos.prefetch-size",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Pre-fetch window size in octets", HFILL}},
        {&hf_amqp_0_10_method_file_qos_prefetch_count, {
            "Prefetch-count", "amqp.file.qos.prefetch-count",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            "Pre-fetch window size in messages", HFILL}},
        {&hf_amqp_0_10_method_file_qos_global, {
            "Global", "amqp.file.qos.global",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Apply QoS to entire connection", HFILL}},
        {&hf_amqp_0_10_method_file_consumer_tag, {
            "Consumer-tag", "amqp.file.consumer-tag",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Consumer tag", HFILL}},
        {&hf_amqp_0_10_method_file_consume_no_local, {
            "No-local", "amqp.file.consume.no-local",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Don't send messages to connection that publishes them", HFILL}},
        {&hf_amqp_0_10_method_file_consume_no_ack, {
            "No-ack", "amqp.file.consume.no-ack",
            FT_BOOLEAN, 8, NULL, 0x08,
            "No acknowledgement needed", HFILL}},
        {&hf_amqp_0_10_method_file_consume_exclusive, {
            "Exclusive", "amqp.file.consume.exclusive",
            FT_BOOLEAN, 8, NULL, 0x10,
            "Request exclusive access", HFILL}},
        {&hf_amqp_0_10_method_file_consume_nowait, {
            "Nowait", "amqp.file.consume.nowait",
            FT_BOOLEAN, 8, NULL, 0x20,
            "Do not send a reply", HFILL}},
        {&hf_amqp_0_10_method_file_consume_arguments, {
            "Arguments", "amqp.file.consume.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            "Arguments for consuming", HFILL}},
        {&hf_amqp_0_10_method_file_identifier, {
            "Identifier", "amqp.file.identifier",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Staging identifier", HFILL}},
        {&hf_amqp_0_10_method_file_open_content_size, {
            "Content-size", "amqp.file.open.content-size",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Message content size in octets", HFILL}},
        {&hf_amqp_0_10_method_file_open_ok_staged_size, {
            "Staged-size", "amqp.file.open_ok.staged-size",
            FT_UINT64, BASE_DEC, NULL, 0x0,
            "Amount of previously staged content in octets", HFILL}},
        {&hf_amqp_0_10_method_file_publish_exchange, {
            "Exchange", "amqp.file.publish.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange to publish to", HFILL}},
        {&hf_amqp_0_10_method_file_publish_routing_key, {
            "Routing-key", "amqp.file.publish.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message routing key", HFILL}},
        {&hf_amqp_0_10_method_file_publish_mandatory, {
            "Mandatory", "amqp.file.publish.mandatory",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Mandatory routing", HFILL}},
        {&hf_amqp_0_10_method_file_publish_immediate, {
            "Immediate", "amqp.file.publish.immediate",
            FT_BOOLEAN, 8, NULL, 0x08,
            "Request immediate delivery", HFILL}},
        {&hf_amqp_0_10_method_file_return_reply_code, {
            "Reply-code", "amqp.file.return.reply-code",
            FT_UINT16, BASE_DEC, VALS(amqp_0_10_file_return_codes), 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_file_return_reply_text, {
            "Reply-text", "amqp.file.return.reply-text",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Localized reply text", HFILL}},
        {&hf_amqp_0_10_method_file_return_exchange, {
            "Exchange", "amqp.file.return.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange the original message was published to", HFILL}},
        {&hf_amqp_0_10_method_file_return_routing_key, {
            "Routing-key", "amqp.file.return.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message routing key", HFILL}},
        {&hf_amqp_0_10_method_file_deliver_consumer_tag, {
            "Consumer-tag", "amqp.file.deliver.consumer-tag",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_file_deliver_delivery_tag, {
            "Delivery-tag", "amqp.file.deliver.delivery-tag",
            FT_UINT64, BASE_HEX, NULL, 0,
            "Server-assigned, session-specific delivery tag", HFILL}},
        {&hf_amqp_0_10_method_file_deliver_redelivered, {
            "Redelivered", "amqp.file.deliver.redelivered",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x04,
            "Possible duplicate delivery", HFILL}},
        {&hf_amqp_0_10_method_file_deliver_exchange, {
            "Exchange", "amqp.file.deliver.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange the original message was published to", HFILL}},
        {&hf_amqp_0_10_method_file_deliver_routing_key, {
            "Routing-key", "amqp.file.deliver.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message routing key", HFILL}},
        {&hf_amqp_0_10_method_file_ack_delivery_tag, {
            "Delivery-tag", "amqp.file.ack.delivery-tag",
            FT_UINT64, BASE_HEX, NULL, 0,
            "Identifier of message being acknowledged", HFILL}},
        {&hf_amqp_0_10_method_file_ack_multiple, {
            "Multiple", "amqp.file.ack.multiple",
            FT_BOOLEAN, 8, NULL, 0x02,
            "Acknowledge multiple messages", HFILL}},
        {&hf_amqp_0_10_method_file_reject_delivery_tag, {
            "Delivery-tag", "amqp.file.reject.delivery-tag",
            FT_UINT64, BASE_HEX, NULL, 0,
            "Identifier of message to be rejected", HFILL}},
        {&hf_amqp_0_10_method_file_reject_requeue, {
            "Requeue", "amqp.file.reject.requeue",
            FT_BOOLEAN, 8, TFS(&tfs_yes_no), 0x02,
            "Requeue the message", HFILL}},
        {&hf_amqp_0_10_method_stream_qos_prefetch_size, {
            "Prefetch-size", "amqp.stream.qos.prefetch-size",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Pre-fetch window size in octets", HFILL}},
        {&hf_amqp_0_10_method_stream_qos_prefetch_count, {
            "Prefetch-count", "amqp.stream.qos.prefetch-count",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            "Pre-fetch window size in messages", HFILL}},
#if 0
        {&hf_amqp_0_10_method_stream_qos_consume_rate, {
            "Prefetch-size", "amqp.stream.qos.consume_rate",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Desired transfer rate in octets/second", HFILL}},
#endif
        {&hf_amqp_0_10_method_stream_qos_global, {
            "Global", "amqp.stream.qos.global",
            FT_BOOLEAN, 8, NULL, 0x08,
            "Apply QoS to entire connection", HFILL}},
        {&hf_amqp_0_10_method_stream_consumer_tag, {
            "Consumer-tag", "amqp.stream.consumer-tag",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_stream_consume_no_local, {
            "No-local", "amqp.stream.consume.no-local",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Don't send messages to connection that publishes them", HFILL}},
        {&hf_amqp_0_10_method_stream_consume_exclusive, {
            "Exclusive", "amqp.stream.consume.exclusive",
            FT_BOOLEAN, 8, NULL, 0x08,
            "Request exclusive access", HFILL}},
        {&hf_amqp_0_10_method_stream_consume_nowait, {
            "Nowait", "amqp.stream.consume.nowait",
            FT_BOOLEAN, 8, NULL, 0x10,
            "Do not send a reply", HFILL}},
        {&hf_amqp_0_10_method_stream_consume_arguments, {
            "Arguments", "amqp.stream.consume.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            "Arguments for consuming", HFILL}},
        {&hf_amqp_0_10_method_stream_publish_exchange, {
            "Exchange", "amqp.stream.publish.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange to publish to", HFILL}},
        {&hf_amqp_0_10_method_stream_publish_routing_key, {
            "Routing-key", "amqp.stream.publish.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message routing key", HFILL}},
        {&hf_amqp_0_10_method_stream_publish_mandatory, {
            "Mandatory", "amqp.stream.publish.mandatory",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Mandatory routing", HFILL}},
        {&hf_amqp_0_10_method_stream_publish_immediate, {
            "Immediate", "amqp.stream.publish.immediate",
            FT_BOOLEAN, 8, NULL, 0x08,
            "Request immediate delivery", HFILL}},
        {&hf_amqp_0_10_method_stream_return_reply_code, {
            "Reply-code", "amqp.stream.return.reply-code",
            FT_UINT16, BASE_DEC, VALS(amqp_0_10_stream_return_codes), 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_stream_return_reply_text, {
            "Reply-text", "amqp.stream.return.reply-text",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Localized reply text", HFILL}},
        {&hf_amqp_0_10_method_stream_return_exchange, {
            "Exchange", "amqp.stream.return.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange the original message was published to", HFILL}},
        {&hf_amqp_0_10_method_stream_return_routing_key, {
            "Routing-key", "amqp.stream.return.routing-key",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Message routing key", HFILL}},
        {&hf_amqp_0_10_method_stream_deliver_consumer_tag, {
            "Consumer-tag", "amqp.stream.deliver.consumer-tag",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_stream_deliver_delivery_tag, {
            "Delivery-tag", "amqp.stream.deliver.delivery-tag",
            FT_UINT64, BASE_HEX, NULL, 0,
            "Server-assigned, session-specific delivery tag", HFILL}},
        {&hf_amqp_0_10_method_stream_deliver_exchange, {
            "Exchange", "amqp.stream.deliver.exchange",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Exchange the original message was published to", HFILL}},
        {&hf_amqp_0_10_method_stream_deliver_queue, {
            "Queue", "amqp.stream.deliver.queue",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            "Name of the queue the message came from", HFILL}},
        {&hf_amqp_channel, {
            "Channel", "amqp.channel",
            FT_UINT16, BASE_DEC, NULL, 0x0,
            "Channel ID", HFILL}},
        {&hf_amqp_reserved, {
            "Reserved", "amqp.reserved",
            FT_UINT32, BASE_HEX, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_0_9_type, {
            "Type", "amqp.type",
            FT_UINT8, BASE_DEC, VALS(amqp_0_9_frame_types), 0x0,
            "Frame type", HFILL}},
        {&hf_amqp_0_9_length, {
            "Length", "amqp.length",
            FT_UINT32, BASE_DEC, NULL, 0x0,
            "Length of the frame", HFILL}},
        {&hf_amqp_0_9_method_class_id, {
            "Class", "amqp.method.class",
            FT_UINT16, BASE_DEC, VALS(amqp_0_9_method_classes), 0x0,
            "Class ID", HFILL}},
        {&hf_amqp_method_connection_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_connection_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_channel_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_channel_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_access_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_access_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_exchange_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_exchange_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_queue_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_queue_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_basic_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_basic_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_file_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_file_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_stream_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_stream_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_tx_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_tx_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_dtx_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_dtx_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_tunnel_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_tunnel_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_confirm_method_id, {
            "Method", "amqp.method.method",
            FT_UINT16, BASE_DEC, VALS(amqp_method_confirm_methods), 0x0,
            "Method ID", HFILL}},
        {&hf_amqp_method_arguments, {
            "Arguments", "amqp.method.arguments",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Method arguments", HFILL}},
        {&hf_amqp_method_connection_start_version_major, {
            "Version-Major", "amqp.method.arguments.version_major",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_version_minor, {
            "Version-Minor", "amqp.method.arguments.version_minor",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_server_properties, {
            "Server-Properties", "amqp.method.arguments.server_properties",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_9_method_connection_start_mechanisms, {
            "Mechanisms", "amqp.method.arguments.mechanisms",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_start_mechanisms, {
            "Mechanisms", "amqp.method.arguments.mechanisms",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Supported security mechanisms", HFILL}},
        {&hf_amqp_0_9_method_connection_start_locales, {
            "Locales", "amqp.method.arguments.locales",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_start_locales, {
            "Locales", "amqp.method.arguments.locales",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Supported message locales", HFILL}},
        {&hf_amqp_method_connection_start_ok_client_properties, {
            "Client-Properties", "amqp.method.arguments.client_properties",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_ok_mechanism, {
            "Mechanism", "amqp.method.arguments.mechanism",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_ok_response, {
            "Response", "amqp.method.arguments.response",
            FT_UINT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_ok_locale, {
            "Locale", "amqp.method.arguments.locale",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_secure_challenge, {
            "Challenge", "amqp.method.arguments.challenge",
            FT_UINT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_secure_ok_response, {
            "Response", "amqp.method.arguments.response",
            FT_UINT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_tune_channel_max, {
            "Channel-Max", "amqp.method.arguments.channel_max",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_9_method_connection_tune_frame_max, {
            "Frame-Max", "amqp.method.arguments.frame_max",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_tune_frame_max, {
            "Frame-Max", "amqp.method.arguments.frame_max",
            FT_UINT16, BASE_DEC, NULL, 0,
            "Server-proposed maximum frame size", HFILL}},
        {&hf_amqp_0_9_method_connection_tune_heartbeat, {
            "Heartbeat", "amqp.method.arguments.heartbeat",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_tune_heartbeat_min, {
            "Heartbeat-Min", "amqp.method.arguments.heartbeat_min",
             FT_UINT16, BASE_DEC, NULL, 0,
            "Minimum heartbeat delay (seconds)", HFILL}},
        {&hf_amqp_0_10_method_connection_tune_heartbeat_max, {
            "Heartbeat-Max", "amqp.method.arguments.heartbeat_max",
             FT_UINT16, BASE_DEC, NULL, 0,
            "Maximum heartbeat delay (seconds)", HFILL}},
        {&hf_amqp_method_connection_tune_ok_channel_max, {
            "Channel-Max", "amqp.method.arguments.channel_max",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_9_method_connection_tune_ok_frame_max, {
            "Frame-Max", "amqp.method.arguments.frame_max",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_tune_ok_frame_max, {
            "Frame-Max", "amqp.method.arguments.frame_max",
            FT_UINT16, BASE_DEC, NULL, 0,
            "Negotiated maximum frame size", HFILL}},
        {&hf_amqp_method_connection_tune_ok_heartbeat, {
            "Heartbeat", "amqp.method.arguments.heartbeat",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_open_virtual_host, {
            "Virtual-Host", "amqp.method.arguments.virtual_host",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_9_method_connection_open_capabilities, {
            "Capabilities", "amqp.method.arguments.capabilities",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_open_capabilities, {
            "Capabilities", "amqp.method.arguments.capabilities",
            FT_STRING, BASE_NONE, NULL, 0,
            "Required capabilities", HFILL}},
        {&hf_amqp_0_9_method_connection_open_insist, {
            "Insist", "amqp.method.arguments.insist",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_open_insist, {
            "Insist", "amqp.method.arguments.insist",
            FT_BOOLEAN, 8, NULL, 0x04,
            "Client insists on this server", HFILL}},
        {&hf_amqp_0_9_method_connection_open_ok_known_hosts, {
            "Known-Hosts", "amqp.method.arguments.known_hosts",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_open_ok_known_hosts, {
            "Known-Hosts", "amqp.method.arguments.known_hosts_bytes",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Equivalent or alternate hosts for reconnection", HFILL}},
        {&hf_amqp_method_connection_redirect_host, {
            "Host", "amqp.method.arguments.host",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_9_method_connection_redirect_known_hosts, {
            "Known-Hosts", "amqp.method.arguments.known_hosts",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_redirect_known_hosts, {
            "Known-Hosts", "amqp.method.arguments.known_hosts_bytes",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Equivalent or alternate hosts to redirect to", HFILL}},
        {&hf_amqp_0_9_method_connection_close_reply_code, {
            "Reply-Code", "amqp.method.arguments.reply_code",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_close_reply_code, {
            "Reply-Code", "amqp.method.arguments.reply_code",
             FT_UINT16, BASE_DEC,
            VALS(amqp_0_10_method_connection_close_reply_codes), 0,
            "Close reason", HFILL}},
        {&hf_amqp_method_connection_close_reply_text, {
            "Reply-Text", "amqp.method.arguments.reply_text",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_close_class_id, {
            "Class-Id", "amqp.method.arguments.class_id",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_close_method_id, {
            "Method-Id", "amqp.method.arguments.method_id",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_blocked_reason, {
            "Reason", "amqp.method.arguments.reason",
             FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_open_out_of_band, {
            "Out-Of-Band", "amqp.method.arguments.out_of_band",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_open_ok_channel_id, {
            "Channel-Id", "amqp.method.arguments.channel_id",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_flow_active, {
            "Active", "amqp.method.arguments.active",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_channel_flow_ok_active, {
            "Active", "amqp.method.arguments.active",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_channel_close_reply_code, {
            "Reply-Code", "amqp.method.arguments.reply_code",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_close_reply_text, {
            "Reply-Text", "amqp.method.arguments.reply_text",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_close_class_id, {
            "Class-Id", "amqp.method.arguments.class_id",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_close_method_id, {
            "Method-Id", "amqp.method.arguments.method_id",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_channel_resume_channel_id, {
            "Channel-Id", "amqp.method.arguments.channel_id",
            FT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_realm, {
            "Realm", "amqp.method.arguments.realm",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_exclusive, {
            "Exclusive", "amqp.method.arguments.exclusive",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_passive, {
            "Passive", "amqp.method.arguments.passive",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_active, {
            "Active", "amqp.method.arguments.active",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_write, {
            "Write", "amqp.method.arguments.write",
            FT_BOOLEAN, 8, NULL, 0x08,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_read, {
            "Read", "amqp.method.arguments.read",
            FT_BOOLEAN, 8, NULL, 0x10,
            NULL, HFILL}},
        {&hf_amqp_method_access_request_ok_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_type, {
            "Type", "amqp.method.arguments.type",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_passive, {
            "Passive", "amqp.method.arguments.passive",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_durable, {
            "Durable", "amqp.method.arguments.durable",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_auto_delete, {
            "Auto-Delete", "amqp.method.arguments.auto_delete",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_internal, {
            "Internal", "amqp.method.arguments.internal",
            FT_BOOLEAN, 8, NULL, 0x08,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x10,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_declare_arguments, {
            "Arguments", "amqp.method.arguments.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_bind_destination, {
            "Destination", "amqp.method.arguments.destination",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_bind_source, {
            "Destination", "amqp.method.arguments.source",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_bind_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_bind_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_bind_arguments, {
            "Arguments", "amqp.method.arguments.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_delete_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_delete_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_delete_if_unused, {
            "If-Unused", "amqp.method.arguments.if_unused",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_exchange_delete_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_passive, {
            "Passive", "amqp.method.arguments.passive",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_durable, {
            "Durable", "amqp.method.arguments.durable",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_exclusive, {
            "Exclusive", "amqp.method.arguments.exclusive",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_auto_delete, {
            "Auto-Delete", "amqp.method.arguments.auto_delete",
            FT_BOOLEAN, 8, NULL, 0x08,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x10,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_arguments, {
            "Arguments", "amqp.method.arguments.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_ok_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_ok_message_count, {
            "Message-Count", "amqp.method.arguments.message_count",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_declare_ok_consumer_count, {
            "Consumer-Count", "amqp.method.arguments.consumer_count",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_bind_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_bind_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_bind_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_bind_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_bind_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_queue_bind_arguments, {
            "Arguments", "amqp.method.arguments.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_unbind_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_unbind_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_unbind_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_unbind_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_unbind_arguments, {
            "Arguments", "amqp.method.arguments.arguments",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_purge_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_purge_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_purge_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_queue_purge_ok_message_count, {
            "Message-Count", "amqp.method.arguments.message_count",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_delete_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_delete_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_queue_delete_if_unused, {
            "If-Unused", "amqp.method.arguments.if_unused",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_queue_delete_if_empty, {
            "If-Empty", "amqp.method.arguments.if_empty",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_queue_delete_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_queue_delete_ok_message_count, {
            "Message-Count", "amqp.method.arguments.message_count",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_qos_prefetch_size, {
            "Prefetch-Size", "amqp.method.arguments.prefetch_size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_qos_prefetch_count, {
            "Prefetch-Count", "amqp.method.arguments.prefetch_count",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_qos_global, {
            "Global", "amqp.method.arguments.global",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_no_local, {
            "No-Local", "amqp.method.arguments.no_local",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_no_ack, {
            "No-Ack", "amqp.method.arguments.no_ack",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_exclusive, {
            "Exclusive", "amqp.method.arguments.exclusive",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x08,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_filter, {
            "Filter", "amqp.method.arguments.filter",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_consume_ok_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_cancel_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_cancel_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_cancel_ok_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_publish_number, {
            "Publish-Number", "amqp.method.arguments.publish_number",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_publish_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_publish_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_publish_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_publish_mandatory, {
            "Mandatory", "amqp.method.arguments.mandatory",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_publish_immediate, {
            "Immediate", "amqp.method.arguments.immediate",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_basic_return_reply_code, {
            "Reply-Code", "amqp.method.arguments.reply_code",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_return_reply_text, {
            "Reply-Text", "amqp.method.arguments.reply_text",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_return_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_return_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_deliver_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_deliver_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_deliver_redelivered, {
            "Redelivered", "amqp.method.arguments.redelivered",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_deliver_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_deliver_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_no_ack, {
            "No-Ack", "amqp.method.arguments.no_ack",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_ok_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_ok_redelivered, {
            "Redelivered", "amqp.method.arguments.redelivered",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_ok_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_ok_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_ok_message_count, {
            "Message-Count", "amqp.method.arguments.message_count",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_get_empty_cluster_id, {
            "Cluster-Id", "amqp.method.arguments.cluster_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_ack_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_ack_multiple, {
            "Multiple", "amqp.method.arguments.multiple",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_reject_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_reject_requeue, {
            "Requeue", "amqp.method.arguments.requeue",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_recover_requeue, {
            "Requeue", "amqp.method.arguments.requeue",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_nack_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_basic_nack_multiple, {
            "Multiple", "amqp.method.arguments.multiple",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_basic_nack_requeue, {
            "Requeue", "amqp.method.arguments.requeue",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_file_qos_prefetch_size, {
            "Prefetch-Size", "amqp.method.arguments.prefetch_size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_qos_prefetch_count, {
            "Prefetch-Count", "amqp.method.arguments.prefetch_count",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_qos_global, {
            "Global", "amqp.method.arguments.global",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_no_local, {
            "No-Local", "amqp.method.arguments.no_local",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_no_ack, {
            "No-Ack", "amqp.method.arguments.no_ack",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_exclusive, {
            "Exclusive", "amqp.method.arguments.exclusive",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x08,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_filter, {
            "Filter", "amqp.method.arguments.filter",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_consume_ok_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_cancel_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_cancel_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_file_cancel_ok_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_open_identifier, {
            "Identifier", "amqp.method.arguments.identifier",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_open_content_size, {
            "Content-Size", "amqp.method.arguments.content_size",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_open_ok_staged_size, {
            "Staged-Size", "amqp.method.arguments.staged_size",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_publish_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_publish_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_publish_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_publish_mandatory, {
            "Mandatory", "amqp.method.arguments.mandatory",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_file_publish_immediate, {
            "Immediate", "amqp.method.arguments.immediate",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_file_publish_identifier, {
            "Identifier", "amqp.method.arguments.identifier",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_return_reply_code, {
            "Reply-Code", "amqp.method.arguments.reply_code",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_return_reply_text, {
            "Reply-Text", "amqp.method.arguments.reply_text",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_return_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_return_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_deliver_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_deliver_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_deliver_redelivered, {
            "Redelivered", "amqp.method.arguments.redelivered",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_file_deliver_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_deliver_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_deliver_identifier, {
            "Identifier", "amqp.method.arguments.identifier",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_ack_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_ack_multiple, {
            "Multiple", "amqp.method.arguments.multiple",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_file_reject_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_file_reject_requeue, {
            "Requeue", "amqp.method.arguments.requeue",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_stream_qos_prefetch_size, {
            "Prefetch-Size", "amqp.method.arguments.prefetch_size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_qos_prefetch_count, {
            "Prefetch-Count", "amqp.method.arguments.prefetch_count",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_qos_consume_rate, {
            "Consume-Rate", "amqp.method.arguments.consume_rate",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_qos_global, {
            "Global", "amqp.method.arguments.global",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_no_local, {
            "No-Local", "amqp.method.arguments.no_local",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_exclusive, {
            "Exclusive", "amqp.method.arguments.exclusive",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x04,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_filter, {
            "Filter", "amqp.method.arguments.filter",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_consume_ok_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_cancel_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_cancel_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_stream_cancel_ok_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_publish_ticket, {
            "Ticket", "amqp.method.arguments.ticket",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_publish_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_publish_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_publish_mandatory, {
            "Mandatory", "amqp.method.arguments.mandatory",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_method_stream_publish_immediate, {
            "Immediate", "amqp.method.arguments.immediate",
            FT_BOOLEAN, 8, NULL, 0x02,
            NULL, HFILL}},
        {&hf_amqp_method_stream_return_reply_code, {
            "Reply-Code", "amqp.method.arguments.reply_code",
             FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_return_reply_text, {
            "Reply-Text", "amqp.method.arguments.reply_text",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_return_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_return_routing_key, {
            "Routing-Key", "amqp.method.arguments.routing_key",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_deliver_consumer_tag, {
            "Consumer-Tag", "amqp.method.arguments.consumer_tag",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_deliver_delivery_tag, {
            "Delivery-Tag", "amqp.method.arguments.delivery_tag",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_deliver_exchange, {
            "Exchange", "amqp.method.arguments.exchange",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_stream_deliver_queue, {
            "Queue", "amqp.method.arguments.queue",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_dtx_start_dtx_identifier, {
            "Dtx-Identifier", "amqp.method.arguments.dtx_identifier",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_tunnel_request_meta_data, {
            "Meta-Data", "amqp.method.arguments.meta_data",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_confirm_select_nowait, {
            "Nowait", "amqp.method.arguments.nowait",
            FT_BOOLEAN, 8, NULL, 0x01,
            NULL, HFILL}},
        {&hf_amqp_field, {
            "Field", "amqp.field",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_name, {
            "Name", "amqp.field.name",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_type, {
            "Type", "amqp.field.type",
            FT_CHAR, BASE_HEX, VALS(amqp_0_9_field_type_vals), 0,
            NULL, HFILL}},
        {&hf_amqp_field_integer, {
            "Value", "amqp.field.integer",
            FT_INT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_unsigned_integer, {
            "Value", "amqp.field.unsigned_integer",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_string, {
            "Value", "amqp.field.string",
            FT_UINT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_boolean, {
            "Value", "amqp.field.boolean",
            FT_BOOLEAN, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_byte, {
            "Value", "amqp.field.byte",
            FT_INT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_unsigned_byte, {
            "Value", "amqp.field.unsigned_byte",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_short_int, {
            "Value", "amqp.field.short_int",
            FT_INT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_short_uint, {
            "Value", "amqp.field.short_uint",
            FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_long_int, {
            "Value", "amqp.field.long_int",
            FT_INT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_float, {
            "Value", "amqp.field.float",
            FT_FLOAT, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_double, {
            "Value", "amqp.field.double",
            FT_DOUBLE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_decimal, {
            "Value", "amqp.field.decimal",
            FT_DOUBLE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_field_timestamp, {
            "Value", "amqp.field.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_field_byte_array, {
            "Value", "amqp.field.byte_array",
            FT_UINT_BYTES, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_class_id, {
            "Class ID", "amqp.header.class",
            FT_UINT16, BASE_DEC, VALS(amqp_0_9_method_classes), 0,
            NULL, HFILL}},
        {&hf_amqp_header_weight, {
            "Weight", "amqp.header.weight",
            FT_UINT16, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_body_size, {
            "Body size", "amqp.header.body-size",
            FT_UINT64, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_property_flags, {
            "Property flags", "amqp.header.property-flags",
            FT_UINT16, BASE_HEX, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_properties, {
            "Properties", "amqp.header.properties",
            FT_NONE, BASE_NONE, NULL, 0x0,
            "Message properties", HFILL}},
        {&hf_amqp_header_basic_content_type, {
            "Content-Type", "amqp.method.properties.content_type",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_content_encoding, {
            "Content-Encoding", "amqp.method.properties.content_encoding",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_headers, {
            "Headers", "amqp.method.properties.headers",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_delivery_mode, {
            "Delivery-Mode", "amqp.method.properties.delivery_mode",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_priority, {
            "Priority", "amqp.method.properties.priority",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_correlation_id, {
            "Correlation-Id", "amqp.method.properties.correlation_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_reply_to, {
            "Reply-To", "amqp.method.properties.reply_to",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_expiration, {
            "Expiration", "amqp.method.properties.expiration",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_message_id, {
            "Message-Id", "amqp.method.properties.message_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_timestamp, {
            "Timestamp", "amqp.method.properties.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_type, {
            "Type", "amqp.method.properties.type",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_user_id, {
            "User-Id", "amqp.method.properties.user_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_app_id, {
            "App-Id", "amqp.method.properties.app_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_basic_cluster_id, {
            "Cluster-Id", "amqp.method.properties.cluster_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_content_type, {
            "Content-Type", "amqp.method.properties.content_type",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_content_encoding, {
            "Content-Encoding", "amqp.method.properties.content_encoding",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_headers, {
            "Headers", "amqp.method.properties.headers",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_priority, {
            "Priority", "amqp.method.properties.priority",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_reply_to, {
            "Reply-To", "amqp.method.properties.reply_to",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_message_id, {
            "Message-Id", "amqp.method.properties.message_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_filename, {
            "Filename", "amqp.method.properties.filename",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_file_timestamp, {
            "Timestamp", "amqp.method.properties.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_header_file_cluster_id, {
            "Cluster-Id", "amqp.method.properties.cluster_id",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_stream_content_type, {
            "Content-Type", "amqp.method.properties.content_type",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_stream_content_encoding, {
            "Content-Encoding", "amqp.method.properties.content_encoding",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_stream_headers, {
            "Headers", "amqp.method.properties.headers",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_stream_priority, {
            "Priority", "amqp.method.properties.priority",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_stream_timestamp, {
            "Timestamp", "amqp.method.properties.timestamp",
            FT_ABSOLUTE_TIME, ABSOLUTE_TIME_UTC, NULL, 0x0,
            NULL, HFILL}},
        {&hf_amqp_header_tunnel_headers, {
            "Headers", "amqp.method.properties.headers",
            FT_NONE, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_tunnel_proxy_name, {
            "Proxy-Name", "amqp.method.properties.proxy_name",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_tunnel_data_name, {
            "Data-Name", "amqp.method.properties.data_name",
            FT_STRING, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_tunnel_durable, {
            "Durable", "amqp.method.properties.durable",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_header_tunnel_broadcast, {
            "Broadcast", "amqp.method.properties.broadcast",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_dtx_xa_status, {
            "DTX  xa-status", "amqp.dtx.xa-status",
            FT_UINT16, BASE_DEC, VALS(amqp_0_10_xa_status), 0,
            NULL, HFILL}},
        {&hf_amqp_payload, {
            "Payload", "amqp.payload",
            FT_BYTES, BASE_NONE, NULL, 0,
            "Message payload", HFILL}},
        {&hf_amqp_init_protocol, {
            "Protocol", "amqp.init.protocol",
            FT_STRING, BASE_NONE, NULL, 0,
            "Protocol name", HFILL}},
        {&hf_amqp_init_id_major, {
            "Protocol ID Major", "amqp.init.id_major",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_init_id_minor, {
            "Protocol ID Minor", "amqp.init.id_minor",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_init_id, {
            "Protocol-ID", "amqp.init.id",
            FT_UINT8, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_init_version_major, {
            "Version Major", "amqp.init.version_major",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Protocol version major", HFILL}},
        {&hf_amqp_init_version_minor, {
            "Version Minor", "amqp.init.version_minor",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Protocol version minor", HFILL}},
        {&hf_amqp_init_version_revision, {
            "Version-Revision", "amqp.init.version_revision",
            FT_UINT8, BASE_DEC, NULL, 0,
            "Protocol version revision", HFILL}},
        {&hf_amqp_message_in, {
            "Message in frame", "amqp.message_in",
            FT_FRAMENUM, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_ack_in, {
            "Ack in frame", "amqp.ack_in",
            FT_FRAMENUM, BASE_NONE, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_server_properties_size, {
            "Size", "amqp.method.connection_start.server_properties.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_start_mechanisms_size, {
            "Size", "amqp.method.connection_start.server_properties.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_start_locales_size, {
            "Size", "amqp.method.connection_start.locales.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_method_connection_start_ok_client_properties_size, {
            "Size", "amqp.method.connection_start.ok_client_properties.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_open_capabilities_size, {
            "Size", "amqp.method.connection_open.capabilities.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_open_ok_known_hosts_size, {
            "Size", "amqp.method.connection_open.ok_known_hosts.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_connection_redirect_known_hosts_size, {
            "Size", "amqp.method.connection_redirect.known_hosts.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_execution_error_info_size, {
            "Size", "amqp.method.execution.error_info.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_exchange_declare_arguments_size, {
            "Size", "amqp.method.exchange.declare_argument.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_queue_declare_arguments_size, {
            "Size", "amqp.method.queue.declare_argument.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_file_consume_arguments_size, {
            "Size", "amqp.method.file.consume_arguments.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_method_stream_consume_arguments_size, {
            "Size", "amqp.method.stream.consume_arguments.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_message_properties_application_headers_size, {
            "Size", "amqp.struct.message_properties.application_headers.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_file_properties_headers_size, {
            "Size", "amqp.struct.file.properties_headers.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_stream_properties_headers_size, {
            "Size", "amqp.struct.stream.properties_headers.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
        {&hf_amqp_0_10_struct_dtx_recover_result_size, {
            "Size", "amqp.struct.dtx_recover.result.size",
            FT_UINT32, BASE_DEC, NULL, 0,
            NULL, HFILL}},
    };

    /*  Setup of protocol subtree array  */

    static int *ett [] = {
         &ett_amqp,
         &ett_header,
         &ett_args,
         &ett_props,
         &ett_field_table,
         &ett_amqp_init,
         &ett_amqp_0_9_field,
         &ett_amqp_0_10_map,
         &ett_amqp_0_10_array,
         &ett_amqp_0_10_struct,
         &ett_amqp_1_0_array,
         &ett_amqp_1_0_map,
         &ett_amqp_1_0_list
    };

    static ei_register_info ei[] = {
        { &ei_amqp_connection_error, { "amqp.connection.error", PI_RESPONSE_CODE, PI_WARN, "Connection error", EXPFILL }},
        { &ei_amqp_channel_error, { "amqp.channel.error", PI_RESPONSE_CODE, PI_WARN, "Channel error", EXPFILL }},
        { &ei_amqp_message_undeliverable, { "amqp.message.undeliverable", PI_RESPONSE_CODE, PI_WARN, "Message was not delivered", EXPFILL }},
        { &ei_amqp_bad_flag_value, { "amqp.bad_flag_value", PI_PROTOCOL, PI_WARN, "Bad flag value", EXPFILL }},
        { &ei_amqp_bad_length, { "amqp.bad_length", PI_MALFORMED, PI_ERROR, "Bad frame length", EXPFILL }},
        { &ei_amqp_field_short, { "amqp.field_short", PI_PROTOCOL, PI_ERROR, "Field is cut off by the end of the field table", EXPFILL }},
        { &ei_amqp_invalid_class_code, { "amqp.unknown.class_code", PI_PROTOCOL, PI_WARN, "Invalid class code", EXPFILL }},
        { &ei_amqp_unknown_command_class, { "amqp.unknown.command_class", PI_PROTOCOL, PI_ERROR, "Unknown command/control class", EXPFILL }},
        { &ei_amqp_unknown_frame_type, { "amqp.unknown.frame_type", PI_PROTOCOL, PI_ERROR, "Unknown frame type", EXPFILL }},
        { &ei_amqp_unknown_connection_method, { "amqp.unknown.method.connection", PI_PROTOCOL, PI_ERROR, "Unknown connection method", EXPFILL }},
        { &ei_amqp_unknown_channel_method, { "amqp.unknown.method.channel", PI_PROTOCOL, PI_ERROR, "Unknown channel method", EXPFILL }},
        { &ei_amqp_unknown_access_method, { "amqp.unknown.method.access", PI_PROTOCOL, PI_ERROR, "Unknown access method", EXPFILL }},
        { &ei_amqp_unknown_exchange_method, { "amqp.unknown.method.exchange", PI_PROTOCOL, PI_ERROR, "Unknown exchange method", EXPFILL }},
        { &ei_amqp_unknown_queue_method, { "amqp.unknown.method.queue", PI_PROTOCOL, PI_ERROR, "Unknown queue method", EXPFILL }},
        { &ei_amqp_unknown_basic_method, { "amqp.unknown.method.basic", PI_PROTOCOL, PI_ERROR, "Unknown basic method", EXPFILL }},
        { &ei_amqp_unknown_file_method, { "amqp.unknown.method.file", PI_PROTOCOL, PI_ERROR, "Unknown file method", EXPFILL }},
        { &ei_amqp_unknown_stream_method, { "amqp.unknown.method.stream", PI_PROTOCOL, PI_ERROR, "Unknown stream method", EXPFILL }},
        { &ei_amqp_unknown_tx_method, { "amqp.unknown.method.tx", PI_PROTOCOL, PI_ERROR, "Unknown tx method", EXPFILL }},
        { &ei_amqp_unknown_dtx_method, { "amqp.unknown.method.dtx", PI_PROTOCOL, PI_ERROR, "Unknown dtx method", EXPFILL }},
        { &ei_amqp_unknown_tunnel_method, { "amqp.unknown.method.tunnel", PI_PROTOCOL, PI_ERROR, "Unknown tunnel method", EXPFILL }},
        { &ei_amqp_unknown_confirm_method, { "amqp.unknown.method.confirm", PI_PROTOCOL, PI_ERROR, "Unknown confirm method", EXPFILL }},
        { &ei_amqp_unknown_method_class, { "amqp.unknown.method.class", PI_PROTOCOL, PI_ERROR, "Unknown method class", EXPFILL }},
        { &ei_amqp_unknown_header_class, { "amqp.unknown.header_class", PI_PROTOCOL, PI_ERROR, "Unknown header class", EXPFILL }},
        { &ei_amqp_unknown_sasl_command, { "amqp.unknown.sasl_command", PI_PROTOCOL, PI_ERROR, "Unknown SASL command", EXPFILL }},
        { &ei_amqp_unknown_amqp_command, { "amqp.unknown.amqp_command", PI_PROTOCOL, PI_ERROR, "Unknown AMQP command", EXPFILL }},
        { &ei_amqp_unknown_amqp_type,  { "amqp.unknown.amqp_type", PI_PROTOCOL, PI_ERROR, "Unknown AMQP type", EXPFILL }},
        { &ei_amqp_invalid_number_of_params, { "amqp.invalid.params_number", PI_PROTOCOL, PI_ERROR, "Invalid number of parameters", EXPFILL }},
        { &ei_amqp_size_exceeds_65K, { "amqp.size_exceeds_65K", PI_PROTOCOL, PI_WARN, "Size field exceeds 65K; Dissection limited to 65K", EXPFILL}},
        { &ei_amqp_array_type_unknown, { "amqp.array_type_unknown", PI_PROTOCOL, PI_WARN, "Array type unknown", EXPFILL}},
    };


    static uat_field_t amqp_message_decode_flds[] = {
        UAT_FLD_VS(message_decode, match_criteria, "Match criteria", match_criteria, "Match criteria"),
        UAT_FLD_CSTRING(message_decode, topic_pattern, "Topic pattern", "Pattern to match for the topic"),
        UAT_FLD_DISSECTOR(message_decode, payload_proto, "Payload dissector",
                      "Dissector to be used for the message part of the matching topic"),
        UAT_FLD_CSTRING(message_decode, topic_more_info, "Additional Data", "Additional Data to pass to the dissector"),
        UAT_END_FIELDS
    };

    uat_t *message_uat = uat_new("Message Decoding",
                               sizeof(amqp_message_decode_t),
                               "amqp_message_decoding",
                               true,
                               &amqp_message_decodes,
                               &num_amqp_message_decodes,
                               UAT_AFFECTS_DISSECTION, /* affects dissection of packets, but not set of named fields */
                               "ChamqpMessageDecoding",
                               amqp_message_decode_copy_cb,
                               amqp_message_decode_update_cb,
                               amqp_message_decode_free_cb,
                               NULL,
                               NULL,
                               amqp_message_decode_flds);


    expert_module_t* expert_amqp;
    module_t *amqp_module;

    /* Decode As handling */
    static build_valid_func amqp_da_build_value[1] = {amqp_value};
    static decode_as_value_t amqp_da_values = {amqp_prompt, 1, amqp_da_build_value};
    static decode_as_t amqp_da = {"amqp", "amqp.version", 1, 0, &amqp_da_values, NULL, NULL,
                                decode_as_default_populate_list, decode_as_default_reset, decode_as_default_change, NULL};

    proto_amqp = proto_register_protocol("Advanced Message Queuing Protocol", "AMQP", "amqp");

    /* Allows versions to be handled through Decode As */
    proto_amqpv0_9 = proto_register_protocol_in_name_only("AMQP Version 0.9", "Version 0.9", "amqp.version.v0_9", proto_amqp, FT_BYTES);
    proto_amqpv0_10 = proto_register_protocol_in_name_only("AMQP Version 0.10", "Version 0.10", "amqp.version.v0_10", proto_amqp, FT_BYTES);
    proto_amqpv1_0 = proto_register_protocol_in_name_only("AMQP Version 1.0", "Version 1.0", "amqp.version.v1_0", proto_amqp, FT_BYTES);

    amqp_tcp_handle = register_dissector("amqp", dissect_amqp, proto_amqp);
    proto_register_field_array(proto_amqp, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    expert_amqp = expert_register_protocol(proto_amqp);
    expert_register_field_array(expert_amqp, ei, array_length(ei));

    version_table = register_dissector_table("amqp.version", "AMQP versions", proto_amqp, FT_UINT8, BASE_DEC);

    amqp_module = prefs_register_protocol(proto_amqp, proto_reg_handoff_amqp);

    prefs_register_uint_preference(amqp_module, "tls.port",
                                   "AMQPS listening TCP Port",
                                   "Set the TCP port for AMQP over SSL/TLS"
                                   "(if other than the default of 5671)",
                                   10, &amqps_port);
    prefs_register_obsolete_preference(amqp_module, "ssl.port");

    register_decode_as(&amqp_da);

    prefs_register_uat_preference(amqp_module, "message_decode_table",
                                "Message Decoding",
                                "A table that enumerates custom message decodes to be used for a certain topic",
                                message_uat);
}

void
proto_reg_handoff_amqp(void)
{
    static unsigned old_amqps_port = 0;
    static bool initialize = false;

    if (!initialize) {
        /* Register TCP port for dissection */
        dissector_add_uint_with_preference("tcp.port", AMQP_PORT, amqp_tcp_handle);

        dissector_add_uint("amqp.version", AMQP_V0_9, create_dissector_handle( dissect_amqpv0_9, proto_amqpv0_9 ));
        dissector_add_uint("amqp.version", AMQP_V0_10, create_dissector_handle( dissect_amqpv0_10, proto_amqpv0_10 ));
        dissector_add_uint("amqp.version", AMQP_V1_0, create_dissector_handle( dissect_amqpv1_0, proto_amqpv1_0 ));

        media_type_subdissector_table = find_dissector_table ("media_type");

        initialize = true;
    }

    /* Register for SSL/TLS payload dissection */
    if (old_amqps_port != amqps_port) {
        if (old_amqps_port != 0)
            ssl_dissector_delete(old_amqps_port, amqp_tcp_handle);
        ssl_dissector_add(amqps_port, amqp_tcp_handle);
        old_amqps_port = amqps_port;
    }
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
