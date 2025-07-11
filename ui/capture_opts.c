/* capture_opts.c
 * Routines for capture options setting
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>

#include <wireshark.h>

#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef HAVE_LIBPCAP

#include <errno.h>

#include <ws_exit_codes.h>

#include "capture_opts.h"
#include "ringbuffer.h"

#include <wiretap/wtap.h> /* For WTAP_MAX_PACKET_SIZE_STANDARD */

#include <wsutil/clopts_common.h>
#include <wsutil/cmdarg_err.h>
#include <wsutil/file_util.h>
#include <wsutil/ws_pipe.h>
#include <wsutil/ws_assert.h>

#ifdef _WIN32
#include <wsutil/win32-utils.h>
#endif

#include "capture/capture_ifinfo.h"
#include "capture/capture-pcap-util.h"

static bool capture_opts_output_to_pipe(const char *save_file, bool *is_pipe);


void
capture_opts_init(capture_options *capture_opts, GList *(*get_iface_list)(int *, char **))
{
    capture_opts->get_iface_list                  = get_iface_list;
    capture_opts->ifaces                          = g_array_new(FALSE, FALSE, sizeof(interface_options));
    capture_opts->all_ifaces                      = g_array_new(FALSE, FALSE, sizeof(interface_t));
    capture_opts->num_selected                    = 0;
    capture_opts->default_options.name            = NULL;
    capture_opts->default_options.descr           = NULL;
    capture_opts->default_options.ifname          = NULL;
    capture_opts->default_options.hardware        = NULL;
    capture_opts->default_options.display_name    = NULL;
    capture_opts->default_options.cfilter         = NULL;
    capture_opts->default_options.has_snaplen     = false;
    capture_opts->default_options.snaplen         = WTAP_MAX_PACKET_SIZE_STANDARD;
    capture_opts->default_options.linktype        = -1; /* use interface default */
    capture_opts->default_options.promisc_mode    = true;
    capture_opts->default_options.if_type         = IF_WIRED;
    capture_opts->default_options.extcap          = NULL;
    capture_opts->default_options.extcap_fifo     = NULL;
    capture_opts->default_options.extcap_args     = NULL;
    capture_opts->default_options.extcap_pid      = WS_INVALID_PID;
    capture_opts->default_options.extcap_pipedata = NULL;
    capture_opts->default_options.extcap_stderr   = NULL;
    capture_opts->default_options.extcap_stdout_watch = 0;
    capture_opts->default_options.extcap_stderr_watch = 0;
#ifdef _WIN32
    capture_opts->default_options.extcap_pipe_h   = INVALID_HANDLE_VALUE;
    capture_opts->default_options.extcap_control_in_h  = INVALID_HANDLE_VALUE;
    capture_opts->default_options.extcap_control_out_h = INVALID_HANDLE_VALUE;
#endif
    capture_opts->default_options.extcap_control_in  = NULL;
    capture_opts->default_options.extcap_control_out = NULL;
    capture_opts->default_options.buffer_size     = DEFAULT_CAPTURE_BUFFER_SIZE;
    capture_opts->default_options.monitor_mode    = false;
#ifdef HAVE_PCAP_REMOTE
    capture_opts->default_options.src_type        = CAPTURE_IFLOCAL;
    capture_opts->default_options.remote_host     = NULL;
    capture_opts->default_options.remote_port     = NULL;
    capture_opts->default_options.auth_type       = CAPTURE_AUTH_NULL;
    capture_opts->default_options.auth_username   = NULL;
    capture_opts->default_options.auth_password   = NULL;
    capture_opts->default_options.datatx_udp      = false;
    capture_opts->default_options.nocap_rpcap     = true;
    capture_opts->default_options.nocap_local     = false;
#endif
#ifdef HAVE_PCAP_SETSAMPLING
    capture_opts->default_options.sampling_method = CAPTURE_SAMP_NONE;
    capture_opts->default_options.sampling_param  = 0;
#endif
    capture_opts->default_options.timestamp_type  = NULL;
    capture_opts->saving_to_file                  = false;
    capture_opts->save_file                       = NULL;
    capture_opts->group_read_access               = false;
    capture_opts->use_pcapng                      = true;             /* Save as pcapng by default */
    capture_opts->update_interval                 = DEFAULT_UPDATE_INTERVAL; /* 100 ms */
    capture_opts->real_time_mode                  = true;
    capture_opts->show_info                       = true;
    capture_opts->restart                         = false;
    capture_opts->orig_save_file                  = NULL;

    capture_opts->multi_files_on                  = false;
    capture_opts->has_file_duration               = false;
    capture_opts->file_duration                   = 60.0;             /* 1 min */
    capture_opts->has_file_interval               = false;
    capture_opts->has_nametimenum                 = false;
    capture_opts->file_interval                   = 60;               /* 1 min */
    capture_opts->has_file_packets                = false;
    capture_opts->file_packets                    = 0;
    capture_opts->has_ring_num_files              = false;
    capture_opts->ring_num_files                  = RINGBUFFER_MIN_NUM_FILES;

    capture_opts->has_autostop_files              = false;
    capture_opts->autostop_files                  = 1;
    capture_opts->has_autostop_packets            = false;
    capture_opts->autostop_packets                = 0;
    capture_opts->has_autostop_written_packets    = false;
    capture_opts->autostop_written_packets        = 0;
    capture_opts->has_autostop_filesize           = false;
    capture_opts->autostop_filesize               = 1000;             /* 1 MB */
    capture_opts->has_autostop_duration           = false;
    capture_opts->autostop_duration               = 60.0;             /* 1 min */

    capture_opts->output_to_pipe                  = false;
    capture_opts->capture_child                   = false;
    capture_opts->stop_after_extcaps              = false;
    capture_opts->wait_for_extcap_cbs             = false;
    capture_opts->print_file_names                = false;
    capture_opts->print_name_to                   = NULL;
    capture_opts->temp_dir                        = NULL;
    capture_opts->compress_type                   = NULL;
    capture_opts->closed_msg                      = NULL;
    capture_opts->extcap_terminate_id             = 0;
    capture_opts->capture_filters_list            = NULL;
}

void
capture_opts_cleanup(capture_options *capture_opts)
{
    if (!capture_opts)
        return;

    if (capture_opts->ifaces) {
        while (capture_opts->ifaces->len > 0) {
            capture_opts_del_iface(capture_opts, 0);
        }
        g_array_free(capture_opts->ifaces, TRUE);
        capture_opts->ifaces = NULL;
    }
    if (capture_opts->all_ifaces) {
        while (capture_opts->all_ifaces->len > 0) {
            interface_t *device = &g_array_index(capture_opts->all_ifaces, interface_t, 0);
            capture_opts_free_interface_t(device);
            capture_opts->all_ifaces = g_array_remove_index(capture_opts->all_ifaces, 0);
        }
        g_array_free(capture_opts->all_ifaces, TRUE);
        capture_opts->all_ifaces = NULL;
    }
    g_free(capture_opts->save_file);
    g_free(capture_opts->temp_dir);

    if (capture_opts->closed_msg) {
        g_free(capture_opts->closed_msg);
        capture_opts->closed_msg = NULL;
    }
    if (capture_opts->extcap_terminate_id > 0) {
        g_source_remove(capture_opts->extcap_terminate_id);
        capture_opts->extcap_terminate_id = 0;
    }

    if (capture_opts->capture_filters_list) {
        ws_filter_list_free(capture_opts->capture_filters_list);
        capture_opts->capture_filters_list = NULL;
    }
}

/* log content of capture_opts */
void
capture_opts_log(const char *log_domain, enum ws_log_level log_level, capture_options *capture_opts) {
    unsigned i;

    ws_log(log_domain, log_level, "CAPTURE OPTIONS     :");

    for (i = 0; i < capture_opts->ifaces->len; i++) {
        interface_options *interface_opts;

        interface_opts = &g_array_index(capture_opts->ifaces, interface_options, i);
        ws_log(log_domain, log_level, "Interface name[%02d]  : %s", i, interface_opts->name ? interface_opts->name : "(unspecified)");
        ws_log(log_domain, log_level, "Interface description[%02d] : %s", i, interface_opts->descr ? interface_opts->descr : "(unspecified)");
        ws_log(log_domain, log_level, "Interface vendor description[%02d] : %s", i, interface_opts->hardware ? interface_opts->hardware : "(unspecified)");
        ws_log(log_domain, log_level, "Display name[%02d]: %s", i, interface_opts->display_name ? interface_opts->display_name : "(unspecified)");
        ws_log(log_domain, log_level, "Capture filter[%02d]  : %s", i, interface_opts->cfilter ? interface_opts->cfilter : "(unspecified)");
        ws_log(log_domain, log_level, "Snap length[%02d] (%u) : %d", i, interface_opts->has_snaplen, interface_opts->snaplen);
        ws_log(log_domain, log_level, "Link Type[%02d]       : %d", i, interface_opts->linktype);
        ws_log(log_domain, log_level, "Promiscuous Mode[%02d]: %s", i, interface_opts->promisc_mode?"TRUE":"FALSE");
        ws_log(log_domain, log_level, "Extcap[%02d]          : %s", i, interface_opts->extcap ? interface_opts->extcap : "(unspecified)");
        ws_log(log_domain, log_level, "Extcap FIFO[%02d]     : %s", i, interface_opts->extcap_fifo ? interface_opts->extcap_fifo : "(unspecified)");
        ws_log(log_domain, log_level, "Extcap PID[%02d]      : %"PRIdMAX, i, (intmax_t)interface_opts->extcap_pid);
        ws_log(log_domain, log_level, "Buffer size[%02d]     : %d (MB)", i, interface_opts->buffer_size);
        ws_log(log_domain, log_level, "Monitor Mode[%02d]    : %s", i, interface_opts->monitor_mode?"TRUE":"FALSE");
#ifdef HAVE_PCAP_REMOTE
        ws_log(log_domain, log_level, "Capture source[%02d]  : %s", i,
            interface_opts->src_type == CAPTURE_IFLOCAL ? "Local interface" :
            interface_opts->src_type == CAPTURE_IFREMOTE ? "Remote interface" :
            "Unknown");
        if (interface_opts->src_type == CAPTURE_IFREMOTE) {
            ws_log(log_domain, log_level, "Remote host[%02d]     : %s", i, interface_opts->remote_host ? interface_opts->remote_host : "(unspecified)");
            ws_log(log_domain, log_level, "Remote port[%02d]     : %s", i, interface_opts->remote_port ? interface_opts->remote_port : "(unspecified)");
        }
        ws_log(log_domain, log_level, "Authentication[%02d]  : %s", i,
            interface_opts->auth_type == CAPTURE_AUTH_NULL ? "Null" :
            interface_opts->auth_type == CAPTURE_AUTH_PWD ? "By username/password" :
            "Unknown");
        if (interface_opts->auth_type == CAPTURE_AUTH_PWD) {
            ws_log(log_domain, log_level, "Auth username[%02d]   : %s", i, interface_opts->auth_username ? interface_opts->auth_username : "(unspecified)");
            ws_log(log_domain, log_level, "Auth password[%02d]   : <hidden>", i);
        }
        ws_log(log_domain, log_level, "UDP data tfer[%02d]   : %u", i, interface_opts->datatx_udp);
        ws_log(log_domain, log_level, "No cap. RPCAP[%02d]   : %u", i, interface_opts->nocap_rpcap);
        ws_log(log_domain, log_level, "No cap. local[%02d]   : %u", i, interface_opts->nocap_local);
#endif
#ifdef HAVE_PCAP_SETSAMPLING
        ws_log(log_domain, log_level, "Sampling meth.[%02d]  : %d", i, interface_opts->sampling_method);
        ws_log(log_domain, log_level, "Sampling param.[%02d] : %d", i, interface_opts->sampling_param);
#endif
        ws_log(log_domain, log_level, "Timestamp type [%02d] : %s", i, interface_opts->timestamp_type);
    }
    ws_log(log_domain, log_level, "Interface name[df]  : %s", capture_opts->default_options.name ? capture_opts->default_options.name : "(unspecified)");
    ws_log(log_domain, log_level, "Interface Descr[df] : %s", capture_opts->default_options.descr ? capture_opts->default_options.descr : "(unspecified)");
    ws_log(log_domain, log_level, "Interface Hardware Descr[df] : %s", capture_opts->default_options.hardware ? capture_opts->default_options.hardware : "(unspecified)");
    ws_log(log_domain, log_level, "Interface display name[df] : %s", capture_opts->default_options.display_name ? capture_opts->default_options.display_name : "(unspecified)");
    ws_log(log_domain, log_level, "Capture filter[df]  : %s", capture_opts->default_options.cfilter ? capture_opts->default_options.cfilter : "(unspecified)");
    ws_log(log_domain, log_level, "Snap length[df] (%u) : %d", capture_opts->default_options.has_snaplen, capture_opts->default_options.snaplen);
    ws_log(log_domain, log_level, "Link Type[df]       : %d", capture_opts->default_options.linktype);
    ws_log(log_domain, log_level, "Promiscuous Mode[df]: %s", capture_opts->default_options.promisc_mode?"TRUE":"FALSE");
    ws_log(log_domain, log_level, "Extcap[df]          : %s", capture_opts->default_options.extcap ? capture_opts->default_options.extcap : "(unspecified)");
    ws_log(log_domain, log_level, "Extcap FIFO[df]     : %s", capture_opts->default_options.extcap_fifo ? capture_opts->default_options.extcap_fifo : "(unspecified)");
    ws_log(log_domain, log_level, "Buffer size[df]     : %d (MB)", capture_opts->default_options.buffer_size);
    ws_log(log_domain, log_level, "Monitor Mode[df]    : %s", capture_opts->default_options.monitor_mode?"TRUE":"FALSE");
#ifdef HAVE_PCAP_REMOTE
    ws_log(log_domain, log_level, "Capture source[df]  : %s",
        capture_opts->default_options.src_type == CAPTURE_IFLOCAL ? "Local interface" :
        capture_opts->default_options.src_type == CAPTURE_IFREMOTE ? "Remote interface" :
        "Unknown");
    if (capture_opts->default_options.src_type == CAPTURE_IFREMOTE) {
        ws_log(log_domain, log_level, "Remote host[df]     : %s", capture_opts->default_options.remote_host ? capture_opts->default_options.remote_host : "(unspecified)");
        ws_log(log_domain, log_level, "Remote port[df]     : %s", capture_opts->default_options.remote_port ? capture_opts->default_options.remote_port : "(unspecified)");
    }
    ws_log(log_domain, log_level, "Authentication[df]  : %s",
        capture_opts->default_options.auth_type == CAPTURE_AUTH_NULL ? "Null" :
        capture_opts->default_options.auth_type == CAPTURE_AUTH_PWD ? "By username/password" :
        "Unknown");
    if (capture_opts->default_options.auth_type == CAPTURE_AUTH_PWD) {
        ws_log(log_domain, log_level, "Auth username[df]   : %s", capture_opts->default_options.auth_username ? capture_opts->default_options.auth_username : "(unspecified)");
        ws_log(log_domain, log_level, "Auth password[df]   : <hidden>");
    }
    ws_log(log_domain, log_level, "UDP data tfer[df]   : %u", capture_opts->default_options.datatx_udp);
    ws_log(log_domain, log_level, "No cap. RPCAP[df]   : %u", capture_opts->default_options.nocap_rpcap);
    ws_log(log_domain, log_level, "No cap. local[df]   : %u", capture_opts->default_options.nocap_local);
#endif
#ifdef HAVE_PCAP_SETSAMPLING
    ws_log(log_domain, log_level, "Sampling meth. [df] : %d", capture_opts->default_options.sampling_method);
    ws_log(log_domain, log_level, "Sampling param.[df] : %d", capture_opts->default_options.sampling_param);
#endif
    ws_log(log_domain, log_level, "Timestamp type [df] : %s", capture_opts->default_options.timestamp_type ? capture_opts->default_options.timestamp_type : "(unspecified)");
    ws_log(log_domain, log_level, "SavingToFile        : %u", capture_opts->saving_to_file);
    ws_log(log_domain, log_level, "SaveFile            : %s", (capture_opts->save_file) ? capture_opts->save_file : "");
    ws_log(log_domain, log_level, "GroupReadAccess     : %u", capture_opts->group_read_access);
    ws_log(log_domain, log_level, "Fileformat          : %s", (capture_opts->use_pcapng) ? "PCAPNG" : "PCAP");
    ws_log(log_domain, log_level, "UpdateInterval      : %u (ms)", capture_opts->update_interval);
    ws_log(log_domain, log_level, "RealTimeMode        : %u", capture_opts->real_time_mode);
    ws_log(log_domain, log_level, "ShowInfo            : %u", capture_opts->show_info);

    ws_log(log_domain, log_level, "MultiFilesOn        : %u", capture_opts->multi_files_on);
    ws_log(log_domain, log_level, "FileDuration    (%u) : %.3f", capture_opts->has_file_duration, capture_opts->file_duration);
    ws_log(log_domain, log_level, "FileInterval    (%u) : %u", capture_opts->has_file_interval, capture_opts->file_interval);
    ws_log(log_domain, log_level, "FilePackets     (%u) : %u", capture_opts->has_file_packets, capture_opts->file_packets);
    ws_log(log_domain, log_level, "FileNameType        : %s", (capture_opts->has_nametimenum) ? "prefix_time_num.suffix"  : "prefix_num_time.suffix");
    ws_log(log_domain, log_level, "RingNumFiles    (%u) : %u", capture_opts->has_ring_num_files, capture_opts->ring_num_files);
    ws_log(log_domain, log_level, "RingPrintFiles  (%u) : %s", capture_opts->print_file_names, (capture_opts->print_file_names ? capture_opts->print_name_to : ""));

    ws_log(log_domain, log_level, "AutostopFiles   (%u) : %u", capture_opts->has_autostop_files, capture_opts->autostop_files);
    ws_log(log_domain, log_level, "AutostopPackets (%u) : %u", capture_opts->has_autostop_packets, capture_opts->autostop_packets);
    ws_log(log_domain, log_level, "AutostopWrittenPackets (%u) : %u", capture_opts->has_autostop_written_packets, capture_opts->autostop_written_packets);
    ws_log(log_domain, log_level, "AutostopFilesize(%u) : %u (KB)", capture_opts->has_autostop_filesize, capture_opts->autostop_filesize);
    ws_log(log_domain, log_level, "AutostopDuration(%u) : %.3f", capture_opts->has_autostop_duration, capture_opts->autostop_duration);
    ws_log(log_domain, log_level, "Temporary Directory  : %s", capture_opts->temp_dir && capture_opts->temp_dir[0] ? capture_opts->temp_dir : g_get_tmp_dir());
}

/*
 * Given a string of the form "<autostop criterion>:<value>", as might appear
 * as an argument to a "-a" option, parse it and set the criterion in
 * question.  Return an indication of whether it succeeded or failed
 * in some fashion.
 */
static bool
set_autostop_criterion(capture_options *capture_opts, const char *autostoparg)
{
    char *p, *colonp;

    colonp = strchr(autostoparg, ':');
    if (colonp == NULL)
        return false;

    p = colonp;
    *p++ = '\0';

    /*
     * Skip over any white space (there probably won't be any, but
     * as we allow it in the preferences file, we might as well
     * allow it here).
     */
    while (g_ascii_isspace(*p))
        p++;
    if (*p == '\0') {
        /*
         * Put the colon back, so if our caller uses, in an
         * error message, the string they passed us, the message
         * looks correct.
         */
        *colonp = ':';
        return false;
    }
    if (strcmp(autostoparg,"duration") == 0) {
        capture_opts->has_autostop_duration = true;
        if (!get_positive_double(p,"autostop duration",&capture_opts->autostop_duration))
            return false;
    } else if (strcmp(autostoparg,"filesize") == 0) {
        capture_opts->has_autostop_filesize = true;
        if (!get_nonzero_uint32(p,"autostop filesize",&capture_opts->autostop_filesize))
            return false;
    } else if (strcmp(autostoparg,"files") == 0) {
        capture_opts->multi_files_on = true;
        capture_opts->has_autostop_files = true;
        if (!get_positive_int(p,"autostop files",&capture_opts->autostop_files))
            return false;
    } else if (strcmp(autostoparg,"packets") == 0) {
        capture_opts->has_autostop_written_packets = true;
        if (!get_positive_int(p,"packet write count",&capture_opts->autostop_written_packets))
            return false;
    } else if (strcmp(autostoparg,"events") == 0) {
        capture_opts->has_autostop_written_packets = true;
        if (!get_positive_int(p,"event write count",&capture_opts->autostop_written_packets))
            return false;
    } else {
        return false;
    }
    *colonp = ':'; /* put the colon back */
    return true;
}

static bool get_filter_arguments(capture_options* capture_opts, const char* arg)
{
    char* colonp = NULL;
    char* val;
    char* filter_exp = NULL;

    /* In capture child mode, any named filter given by "predef:<name>" should
       already have been replaced with the filter text by the calling program. */
    if (!capture_opts->capture_child) {
        colonp = strchr(arg, ':');
        if (colonp) {
            val = colonp;
            *val = '\0';
            val++;
            if (strcmp(arg, "predef") == 0) {
                GList* filterItem;

                if (capture_opts->capture_filters_list == NULL)
                    capture_opts->capture_filters_list = ws_filter_list_read(CFILTER_LIST);
                filterItem = capture_opts->capture_filters_list->list;
                while (filterItem != NULL) {
                    filter_def* filterDef;

                    filterDef = (filter_def*)filterItem->data;
                    if (g_ascii_strcasecmp(val, filterDef->name) == 0) {
                        filter_exp = g_strdup(filterDef->strval);
                        break;
                    }
                    filterItem = filterItem->next;
                }
            }
        }
    }

    if (filter_exp == NULL) {
        /* No filter expression found yet; fallback to previous implementation
           and assume the arg contains a filter expression */
        if (colonp) {
            *colonp = ':';      /* restore colon */
        }
        filter_exp = g_strdup(arg);
    }

    if (capture_opts->ifaces->len > 0) {
        interface_options *interface_opts;

        interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
        g_free(interface_opts->cfilter);
        interface_opts->cfilter = filter_exp;
        return true;
    }
    else {
        g_free(capture_opts->default_options.cfilter);
        capture_opts->default_options.cfilter = filter_exp;
        return true;
    }
}

void
capture_opts_list_file_types(void) {

    cmdarg_err("The available capture file types for the \"-F\" flag are:\n");
    cmdarg_err_cont("%s", "    pcap - Wireshark/tcpdump/... - pcap");
    cmdarg_err_cont("%s", "    pcapng - Wireshark/... - pcapng");
}

static bool get_file_type_argument(capture_options* capture_opts _U_, const
 char* arg)
{
    if (strcmp(arg, "pcapng") == 0) {
        capture_opts->use_pcapng = true;
    } else if (strcmp(arg, "pcap") == 0) {
        capture_opts->use_pcapng = false;
    } else {
        return false;
    }
    return true;
}

/*
 * Given a string of the form "<ring buffer file>:<duration>", as might appear
 * as an argument to a "-b" option, parse it and set the arguments in
 * question.  Return an indication of whether it succeeded or failed
 * in some fashion.
 */
static bool
get_ring_arguments(capture_options *capture_opts, const char *arg)
{
    char *p = NULL, *colonp;

    colonp = strchr(arg, ':');
    if (colonp == NULL)
        return false;

    p = colonp;
    *p++ = '\0';

    /*
     * Skip over any white space (there probably won't be any, but
     * as we allow it in the preferences file, we might as well
     * allow it here).
     */
    while (g_ascii_isspace(*p))
        p++;
    if (*p == '\0') {
        /*
         * Put the colon back, so if our caller uses, in an
         * error message, the string they passed us, the message
         * looks correct.
         */
        *colonp = ':';
        return false;
    }

    if (strcmp(arg,"files") == 0) {
        capture_opts->has_ring_num_files = true;
        if (!get_nonzero_uint32(p, "number of ring buffer files",&capture_opts->ring_num_files))
            return false;
    } else if (strcmp(arg,"filesize") == 0) {
        capture_opts->has_autostop_filesize = true;
        if (!get_nonzero_uint32(p, "ring buffer filesize",&capture_opts->autostop_filesize))
            return false;
    } else if (strcmp(arg,"duration") == 0) {
        capture_opts->has_file_duration = true;
        if (!get_positive_double(p, "ring buffer duration",&capture_opts->file_duration))
            return false;
    } else if (strcmp(arg,"interval") == 0) {
        capture_opts->has_file_interval = true;
        if (!get_positive_int(p, "ring buffer interval",&capture_opts->file_interval))
            return false;
    } else if (strcmp(arg,"nametimenum") == 0) {
        int val;
        if (!get_positive_int(p, "file name: time before num", &val))
            return false;
        capture_opts->has_nametimenum = (val > 1);
    } else if (strcmp(arg,"packets") == 0) {
        capture_opts->has_file_packets = true;
        if (!get_positive_int(p, "ring buffer packet count",&capture_opts->file_packets))
            return false;
    } else if (strcmp(arg,"events") == 0) {
        capture_opts->has_file_packets = true;
        if (!get_positive_int(p, "ring buffer event count",&capture_opts->file_packets))
            return false;
    } else if (strcmp(arg,"printname") == 0) {
        capture_opts->print_file_names = true;
        capture_opts->print_name_to = g_strdup(p);
    }
    else {
        return false;
    }
    *colonp = ':';    /* put the colon back */
    return true;
}

#ifdef HAVE_PCAP_SETSAMPLING
/*
 * Given a string of the form "<sampling type>:<value>", as might appear
 * as an argument to a "-m" option, parse it and set the arguments in
 * question.  Return an indication of whether it succeeded or failed
 * in some fashion.
 */
static bool
get_sampling_arguments(capture_options *capture_opts, const char *arg)
{
    char *p = NULL, *colonp;

    colonp = strchr(arg, ':');
    if (colonp == NULL)
        return false;

    p = colonp;
    *p++ = '\0';

    while (g_ascii_isspace(*p))
        p++;
    if (*p == '\0') {
        *colonp = ':';
        return false;
    }

    if (strcmp(arg, "count") == 0) {
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->sampling_method = CAPTURE_SAMP_BY_COUNT;
            if (!get_positive_int(p, "sampling count", &interface_opts->sampling_param))
                return false;
        } else {
            capture_opts->default_options.sampling_method = CAPTURE_SAMP_BY_COUNT;
            if (!get_positive_int(p, "sampling count", &capture_opts->default_options.sampling_param))
                return false;
        }
    } else if (strcmp(arg, "timer") == 0) {
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->sampling_method = CAPTURE_SAMP_BY_TIMER;
            if (!get_positive_int(p, "sampling timer", &interface_opts->sampling_param))
                return false;
        } else {
            capture_opts->default_options.sampling_method = CAPTURE_SAMP_BY_TIMER;
            if (!get_positive_int(p, "sampling timer", &capture_opts->default_options.sampling_param))
                return false;
        }
    }
    *colonp = ':';
    return true;
}
#endif

#ifdef HAVE_PCAP_REMOTE
/*
 * Given a string of the form "<username>:<password>", as might appear
 * as an argument to a "-A" option, parse it and set the arguments in
 * question.  Return an indication of whether it succeeded or failed
 * in some fashion.
 */
static bool
get_auth_arguments(capture_options *capture_opts, const char *arg)
{
    char *p = NULL, *colonp;

    colonp = strchr(arg, ':');
    if (colonp == NULL)
        return false;

    p = colonp;
    *p++ = '\0';

    while (g_ascii_isspace(*p))
        p++;

    if (capture_opts->ifaces->len > 0) {
        interface_options *interface_opts;

        interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
        interface_opts->auth_type = CAPTURE_AUTH_PWD;
        interface_opts->auth_username = g_strdup(arg);
        interface_opts->auth_password = g_strdup(p);
    } else {
        capture_opts->default_options.auth_type = CAPTURE_AUTH_PWD;
        capture_opts->default_options.auth_username = g_strdup(arg);
        capture_opts->default_options.auth_password = g_strdup(p);
    }
    *colonp = ':';
    return true;
}
#endif

#ifdef _WIN32
static char *
capture_opts_generate_display_name(const char *friendly_name,
                                   const char *name _U_)
{
    /*
     * Display the friendly name rather than the not-so-friendly
     * GUID-based interface name.
     */
    return g_strdup(friendly_name);
}
#else
static char *
capture_opts_generate_display_name(const char *friendly_name,
                                   const char *name)
{
    /*
     * On UN*X, however, users are more used to interface names,
     * and may find it helpful to see them.
     */
    return ws_strdup_printf("%s: %s", friendly_name, name);
}
#endif

static void
fill_in_interface_opts_defaults(interface_options *interface_opts, const capture_options *capture_opts)
{

    interface_opts->cfilter = g_strdup(capture_opts->default_options.cfilter);
    interface_opts->snaplen = capture_opts->default_options.snaplen;
    interface_opts->has_snaplen = capture_opts->default_options.has_snaplen;
    interface_opts->linktype = capture_opts->default_options.linktype;
    interface_opts->promisc_mode = capture_opts->default_options.promisc_mode;
    interface_opts->extcap_fifo = g_strdup(capture_opts->default_options.extcap_fifo);
    interface_opts->extcap_args = NULL;
    interface_opts->extcap_pid = WS_INVALID_PID;
    interface_opts->extcap_pipedata = NULL;
    interface_opts->extcap_stderr = NULL;
    interface_opts->extcap_stdout_watch = 0;
    interface_opts->extcap_stderr_watch = 0;
#ifdef _WIN32
    interface_opts->extcap_pipe_h = INVALID_HANDLE_VALUE;
    interface_opts->extcap_control_in_h = INVALID_HANDLE_VALUE;
    interface_opts->extcap_control_out_h = INVALID_HANDLE_VALUE;
#endif
    interface_opts->extcap_control_in = g_strdup(capture_opts->default_options.extcap_control_in);
    interface_opts->extcap_control_out = g_strdup(capture_opts->default_options.extcap_control_out);
    interface_opts->buffer_size = capture_opts->default_options.buffer_size;
    interface_opts->monitor_mode = capture_opts->default_options.monitor_mode;
#ifdef HAVE_PCAP_REMOTE
    interface_opts->src_type = capture_opts->default_options.src_type;
    interface_opts->remote_host = g_strdup(capture_opts->default_options.remote_host);
    interface_opts->remote_port = g_strdup(capture_opts->default_options.remote_port);
    interface_opts->auth_type = capture_opts->default_options.auth_type;
    interface_opts->auth_username = g_strdup(capture_opts->default_options.auth_username);
    interface_opts->auth_password = g_strdup(capture_opts->default_options.auth_password);
    interface_opts->datatx_udp = capture_opts->default_options.datatx_udp;
    interface_opts->nocap_rpcap = capture_opts->default_options.nocap_rpcap;
    interface_opts->nocap_local = capture_opts->default_options.nocap_local;
#endif
#ifdef HAVE_PCAP_SETSAMPLING
    interface_opts->sampling_method = capture_opts->default_options.sampling_method;
    interface_opts->sampling_param  = capture_opts->default_options.sampling_param;
#endif
    interface_opts->timestamp_type  = g_strdup(capture_opts->default_options.timestamp_type);
}

static void
fill_in_interface_opts_from_ifinfo(interface_options *interface_opts,
                                   const if_info_t *if_info)
{
    interface_opts->name = g_strdup(if_info->name);

    interface_opts->hardware = g_strdup(if_info->vendor_description);
    /* XXX: ui/capture_ui_utils.c get_interface_descriptive_name()
     * does several things different in setting descr (and thus
     * display name):
     * 1. It checks for a user-supplied description via
     * capture_dev_user_descr_find(if_info->name), including a
     * long-standing -X option of "stdin_descr" that dates back to 1.0
     * 2. If we don't have a friendly name, but do have a vendor
     * description (set to hardware above), that is used as the
     * description.
     *
     * Perhaps we don't want to introduce a dependency on the prefs
     * and ex-opts here. We could do 2 here, though.
     *
     * Because we always set interface_opts->display_name here, it is
     * never NULL when get_iface_list_string is called, so that never
     * calls get_interface_descriptive_name(). (And thus, we never
     * actually use the vendor description in the display name/descr
     * as a fallback.)
     */
    if (if_info->friendly_name != NULL) {
        /*
         * We have a friendly name; remember it as the
         * description...
         */
        interface_opts->descr = g_strdup(if_info->friendly_name);
        /*
         * ...and use it in the console display name.
         */
        interface_opts->display_name = capture_opts_generate_display_name(if_info->friendly_name, if_info->name);
    } else {
        /* fallback to the interface name */
        interface_opts->descr = NULL;
        interface_opts->display_name = g_strdup(if_info->name);
    }
    interface_opts->ifname = NULL;
    interface_opts->if_type = if_info->type;
    interface_opts->extcap = g_strdup(if_info->extcap);
}

static if_info_t*
find_ifinfo_by_name(GList *if_list, const char *name)
{
    GList       *if_entry;
    if_info_t   *matched_if_info;
    size_t      prefix_length;

    matched_if_info = NULL;
    if (if_list != NULL) {
        /*
         * Try and do an exact match (case insensitive) on  the
         * interface name, the interface description, and the
         * hardware description.
         */
        for (if_entry = g_list_first(if_list); if_entry != NULL;
             if_entry = g_list_next(if_entry))
        {
            if_info_t *if_info = (if_info_t *)if_entry->data;

            /*
             * Does the specified name match the interface name
             * with a case-insensitive match?
             */
            if (g_ascii_strcasecmp(if_info->name, name) == 0) {
                /*
                 * Yes.
                 */
                matched_if_info = if_info;
                break;
            }

            /*
             * Does this interface have a friendly name and, if so,
             * does the specified name match the friendly name with
             * a case-insensitive match?
             */
            if (if_info->friendly_name != NULL &&
                g_ascii_strcasecmp(if_info->friendly_name, name) == 0) {
                /*
                 * Yes.
                 */
                matched_if_info = if_info;
                break;
            }

#ifdef _WIN32
            /*
             * On Windows, we store interface names in preferences as:
             *  friendlyname (name)
             * Do we have a case-insensitive match for that?
             */
            if (if_info->friendly_name != NULL) {
                GString* combined_name = g_string_new(if_info->friendly_name);
                g_string_append_printf(combined_name, " (%s)", if_info->name);
                if (g_ascii_strcasecmp(combined_name->str, name) == 0) {
                    /*
                     * Yes.
                     */
                    matched_if_info = if_info;
                }
                g_string_free(combined_name, TRUE);
                if (matched_if_info != NULL) {
                    break;
                }
            }
#endif
        }

        if (matched_if_info == NULL) {
            /*
             * We didn't find it; attempt a case-insensitive prefix match
             * of the friendly name.
             */
            prefix_length = strlen(name);
            for (if_entry = g_list_first(if_list); if_entry != NULL;
                 if_entry = g_list_next(if_entry))
            {
                if_info_t *if_info = (if_info_t *)if_entry->data;

                if (if_info->friendly_name != NULL &&
                    g_ascii_strncasecmp(if_info->friendly_name, name, prefix_length) == 0) {
                    /*
                     * We found an interface whose friendly name matches
                     * with a case-insensitive prefix match.
                     */
                    matched_if_info = if_info;
                    break;
                }
            }
        }
    }

    return matched_if_info;
}

static int
capture_opts_add_iface_opt(capture_options *capture_opts, const char *optarg_str_p)
{
    long        adapter_index;
    char        *p;
    GList       *if_list;
    if_info_t   *if_info;
    int         err;
    char        *err_str;
    interface_options interface_opts;

    /*
     * If the argument is a number, treat it as an index into the list
     * of adapters, as printed by "tshark -D".
     *
     * This should be OK on UN*X systems, as interfaces shouldn't have
     * names that begin with digits.  It can be useful on Windows, where
     * more than one interface can have the same name.
     *
     * XXX - "shouldn't have names that begin with digits" is not true
     * on Linux; see
     *
     *    https://github.com/the-tcpdump-group/tcpdump/issues/522
     *
     * tcpdump handles that by trying to open the device by name and,
     * if that fails *and* the name is a syntactically valid number
     * (optional sign, followed by decimal digits), reports an error
     * if it's not a valid interface index, and otherwise uses it as
     * an interface index.
     */
    adapter_index = strtol(optarg_str_p, &p, 10);
    if (p != NULL && *p == '\0') {
        if (adapter_index < 0) {
            cmdarg_err("The specified adapter index is a negative number");
            return 1;
        }
        if (adapter_index > INT_MAX) {
            cmdarg_err("The specified adapter index is too large (greater than %d)",
                       INT_MAX);
            return 1;
        }
        if (adapter_index == 0) {
            cmdarg_err("There is no interface with that adapter index");
            return 1;
        }
        if_list = capture_opts->get_iface_list(&err, &err_str);
        if (if_list == NULL) {
            if (err == 0)
                cmdarg_err("There are no interfaces on which a capture can be done");
            else {
                cmdarg_err("%s", err_str);
                g_free(err_str);
            }
            return 2;
        }
        if_info = (if_info_t *)g_list_nth_data(if_list, (int)(adapter_index - 1));
        if (if_info == NULL) {
            cmdarg_err("There is no interface with that adapter index");
            free_interface_list(if_list);
            return 1;
        }
        fill_in_interface_opts_from_ifinfo(&interface_opts, if_info);
        free_interface_list(if_list);
    } else if (capture_opts->capture_child) {
        /*
         * In Wireshark capture child mode, so the exact interface name
         * is supplied, and we don't need to look it up.
         */
	if_info = if_info_get(optarg_str_p);
	fill_in_interface_opts_from_ifinfo(&interface_opts, if_info);
        if_info_free(if_info);
    } else if (g_strcmp0(optarg_str_p, "-") == 0) {
        /*
         * Standard input. Don't bother to retrieve the interface_list;
         * assume that there isn't a device named "-". (Retrieving the
         * interface list involves spawning a privileged dumpcap process.)
         */
        interface_opts.name = g_strdup(optarg_str_p);
        interface_opts.descr = g_strdup("Standard input");
        interface_opts.hardware = NULL;
        interface_opts.display_name = g_strdup(interface_opts.descr);
        interface_opts.ifname = NULL;
        interface_opts.if_type = IF_STDIN;
        interface_opts.extcap = g_strdup(capture_opts->default_options.extcap);
#ifdef _WIN32
    } else if (win32_is_pipe_name(optarg_str_p)) {
        /*
         * Special named pipe name on Windows.
         * https://learn.microsoft.com/en-us/windows/win32/ipc/pipe-names
         * Don't bother retrieving the interface list.
         */
        interface_opts.name = g_strdup(optarg_str_p);
        interface_opts.descr = NULL;
        interface_opts.hardware = NULL;
        interface_opts.display_name = g_strdup(optarg_str_p);
        interface_opts.ifname = NULL;
        interface_opts.if_type = IF_PIPE;
        interface_opts.extcap = g_strdup(capture_opts->default_options.extcap);
#endif
    } else {
        /*
         * Search for that name in the interface list and, if we found
         * it, fill in fields in the interface_opts structure.
         *
         * XXX - if we can't get the interface list, we don't report
         * an error, as, on Windows, that might be due to WinPcap or
         * Npcap not being installed, but the specified "interface"
         * might be the standard input ("-") or a pipe, and dumpcap
         * should support capturing from the standard input or from
         * a pipe even if there's no capture support from *pcap.
         *
         * Perhaps doing something similar to what was suggested
         * for numerical interfaces should be done.
         *
         * XXX: If we ever save pipe settings permanently, it should be
         * capture_interface_list that tries to check saved pipes (or
         * extcaps), possibly before retrieving the list.
         */
        if_list = capture_opts->get_iface_list(&err, &err_str);
        if_info = find_ifinfo_by_name(if_list, optarg_str_p);
        if (if_info != NULL) {
            /*
             * We found the interface in the list; fill in the
             * interface_opts structure from its if_info.
             */
            fill_in_interface_opts_from_ifinfo(&interface_opts, if_info);
        } else {
            /*
             * We didn't find the interface in the list; just use
             * the specified name, so that, for example, if an
             * interface doesn't show up in the list for some
             * reason, the user can try specifying it explicitly
             * for testing purposes.
             */
            interface_opts.name = g_strdup(optarg_str_p);
            interface_opts.descr = NULL;
            interface_opts.hardware = NULL;
            interface_opts.display_name = g_strdup(optarg_str_p);
            interface_opts.ifname = NULL;
            interface_opts.if_type = capture_opts->default_options.if_type;
            interface_opts.extcap = g_strdup(capture_opts->default_options.extcap);
        }
        free_interface_list(if_list);
    }

    fill_in_interface_opts_defaults(&interface_opts, capture_opts);

    g_array_append_val(capture_opts->ifaces, interface_opts);

    return 0;
}


int
capture_opts_add_opt(capture_options *capture_opts, int opt, const char *optarg_str_p)
{
    int status, snaplen;
    ws_statb64 fstat;

    switch(opt) {
    case 'a':        /* autostop criteria */
        if (set_autostop_criterion(capture_opts, optarg_str_p) == false) {
            cmdarg_err("Invalid or unknown -a flag \"%s\"", optarg_str_p);
            return 1;
        }
        break;
#ifdef HAVE_PCAP_REMOTE
    case 'A':
        if (get_auth_arguments(capture_opts, optarg_str_p) == false) {
            cmdarg_err("Invalid or unknown -A arg \"%s\"", optarg_str_p);
            return 1;
        }
        break;
#endif
    case 'b':        /* Ringbuffer option */
        // XXX libscap and libsinsp don't support this, so we should probably error out if our flavor is Stratoshark.
        capture_opts->multi_files_on = true;
        if (get_ring_arguments(capture_opts, optarg_str_p) == false) {
            cmdarg_err("Invalid or unknown -b arg \"%s\"", optarg_str_p);
            return 1;
        }
        break;
    case 'B':        /* Buffer size */
        // XXX Should we error out if our flavor is Stratoshark?
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            if (!get_positive_int(optarg_str_p, "buffer size", &interface_opts->buffer_size))
                return 1;
        } else {
            if (!get_positive_int(optarg_str_p, "buffer size", &capture_opts->default_options.buffer_size))
                return 1;
        }
        break;
    case 'c':        /* Capture n packets */
        /* XXX Use set_autostop_criterion instead? */
        capture_opts->has_autostop_packets = true;
        if (!get_positive_int(optarg_str_p, "packet count", &capture_opts->autostop_packets))
            return 1;
        break;
    case 'f':        /* capture filter */
        get_filter_arguments(capture_opts, optarg_str_p);
        break;
    case 'F':        /* capture file type */
        if (get_file_type_argument(capture_opts, optarg_str_p) == false) {
            capture_opts_list_file_types();
            return 1;
        }
        break;
    case 'g':        /* enable group read access on the capture file(s) */
        capture_opts->group_read_access = true;
        break;
    case 'H':        /* Hide capture info dialog box */
        capture_opts->show_info = false;
        break;
    case LONGOPT_SET_TSTAMP_TYPE:        /* Set capture time stamp type */
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            g_free(interface_opts->timestamp_type);
            interface_opts->timestamp_type = g_strdup(optarg_str_p);
        } else {
            g_free(capture_opts->default_options.timestamp_type);
            capture_opts->default_options.timestamp_type = g_strdup(optarg_str_p);
        }
        break;
    case 'i':        /* Use interface x */
        status = capture_opts_add_iface_opt(capture_opts, optarg_str_p);
        if (status != 0) {
            return status;
        }
        break;
    case 'I':        /* Capture in monitor mode */
        // XXX Should we error out if our flavor is Stratoshark?
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->monitor_mode = true;
        } else {
            capture_opts->default_options.monitor_mode = true;
        }
        break;
    case 'l':        /* tshark "Line-buffer" standard output */
        capture_opts->update_interval = 0;
        /* Wireshark uses 'l' for Automatic scrolling in live capture mode,
         * but ui/commandline.c should not and does not call this function
         * for 'l'.
         */
        break;
#ifdef HAVE_PCAP_SETSAMPLING
    case 'm':
        if (get_sampling_arguments(capture_opts, optarg_str_p) == false) {
            cmdarg_err("Invalid or unknown -m arg \"%s\"", optarg_str_p);
            return 1;
        }
        break;
#endif
    case 'n':        /* Use pcapng format */
        cmdarg_err("'-n' is deprecated; use '-F pcapng' to set the output format to pcapng.");
        capture_opts->use_pcapng = true;
        break;
    case 'p':        /* Don't capture in promiscuous mode */
        // XXX Should we error out if our flavor is Stratoshark?
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->promisc_mode = false;
        } else {
            capture_opts->default_options.promisc_mode = false;
        }
        break;
    case 'P':        /* Use pcap format */
        cmdarg_err("'-P' is deprecated; use '-F pcap' to set the output format to pcap.");
        capture_opts->use_pcapng = false;
        break;
#ifdef HAVE_PCAP_REMOTE
    case 'r':
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->nocap_rpcap = false;
        } else {
            capture_opts->default_options.nocap_rpcap = false;
        }
        break;
#endif
    case 's':        /* Set the snapshot (capture) length */
        // XXX Should we error out if our flavor is Stratoshark?
        if (!get_natural_int(optarg_str_p, "snapshot length", &snaplen))
            return 1;
        /*
         * Make a snapshot length of 0 equivalent to the maximum packet
         * length, mirroring what tcpdump does.
         */
        if (snaplen == 0)
            snaplen = WTAP_MAX_PACKET_SIZE_STANDARD;
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->has_snaplen = true;
            interface_opts->snaplen = snaplen;
        } else {
            capture_opts->default_options.snaplen = snaplen;
            capture_opts->default_options.has_snaplen = true;
        }
        break;
    case 'S':        /* "Real-Time" mode: used for following file ala tail -f */
        capture_opts->real_time_mode = true;
        break;
#ifdef HAVE_PCAP_REMOTE
    case 'u':
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->datatx_udp = true;
        } else {
            capture_opts->default_options.datatx_udp = true;
        }
        break;
#endif
    case 'w':        /* Write to capture file x */
        capture_opts->saving_to_file = true;
        g_free(capture_opts->save_file);
        capture_opts->save_file = g_strdup(optarg_str_p);
        capture_opts->orig_save_file = g_strdup(optarg_str_p);
        status = capture_opts_output_to_pipe(capture_opts->save_file, &capture_opts->output_to_pipe);
        return status;
    case 'y':        /* Set the pcap data link type */
        if (capture_opts->ifaces->len > 0) {
            interface_options *interface_opts;

            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, capture_opts->ifaces->len - 1);
            interface_opts->linktype = linktype_name_to_val(optarg_str_p);
            if (interface_opts->linktype == -1) {
                cmdarg_err("The specified data link type \"%s\" isn't valid",
                           optarg_str_p);
                return 1;
            }
        } else {
            capture_opts->default_options.linktype = linktype_name_to_val(optarg_str_p);
            if (capture_opts->default_options.linktype == -1) {
                cmdarg_err("The specified data link type \"%s\" isn't valid",
                           optarg_str_p);
                return 1;
            }
        }
        break;
    case LONGOPT_COMPRESS_TYPE:  /* compress type */
        if (capture_opts->compress_type) {
            cmdarg_err("--compress-type can be set only once");
            return 1;
        }
        if (!wtap_can_write_compression_type(wtap_name_to_compression_type(optarg_str_p))) {
            cmdarg_err("\"%s\" isn't a valid output compression mode", optarg_str_p);
            cmdarg_err("The available output compression type(s) are:");
            GSList *output_compression_types;
            output_compression_types = wtap_get_all_output_compression_type_names_list();
            for (GSList *compression_type = output_compression_types;
                compression_type != NULL;
                compression_type = g_slist_next(compression_type)) {

                cmdarg_err_cont("    %s", (const char*)compression_type->data);
            }
            g_slist_free(output_compression_types);
            return 1;
        }
        capture_opts->compress_type = g_strdup(optarg_str_p);
        break;
    case LONGOPT_CAPTURE_TMPDIR:  /* capture temporary directory */
        if (capture_opts->temp_dir) {
            cmdarg_err("--temp-dir can be set only once");
            return 1;
        }
        if (ws_stat64(optarg_str_p, &fstat) < 0) {
            cmdarg_err("Can't set temporary directory %s: %s",
                    optarg_str_p, g_strerror(errno));
            return 1;
        }
        if (!S_ISDIR(fstat.st_mode)) {
            cmdarg_err("Can't set temporary directory %s: not a directory",
                    optarg_str_p);
            return 1;
        }
#ifdef S_IRWXU
        if ((fstat.st_mode & S_IRWXU) != S_IRWXU) {
            cmdarg_err("Can't set temporary directory %s: not a writable directory",
                    optarg_str_p);
            return 1;
        }
#endif /* S_IRWXU */
        capture_opts->temp_dir = g_strdup(optarg_str_p);
        break;
    case LONGOPT_UPDATE_INTERVAL:  /* capture update interval */
        if (!get_natural_int(optarg_str_p, "update interval", &capture_opts->update_interval))
            return false;
        break;
    default:
        /* the caller is responsible to send us only the right opt's */
        ws_assert_not_reached();
    }

    return 0;
}

int
capture_opts_print_if_capabilities(if_capabilities_t *caps,
                                   const interface_options *interface_opts,
                                   int queries)
{
    GList *lt_entry, *ts_entry;

    if (caps->primary_msg) {
        cmdarg_err("The capabilities of the capture device "
                   "\"%s\" could not be obtained (%s).%s%s",
                   interface_opts->name, caps->primary_msg,
                   caps->secondary_msg ? "\n" : "",
                   caps->secondary_msg ? caps->secondary_msg : "");
        return WS_EXIT_INVALID_CAPABILITY;
    }

    if (queries & CAPS_QUERY_LINK_TYPES) {
        if (interface_opts->monitor_mode && caps->can_set_rfmon) {
            lt_entry = caps->data_link_types_rfmon;
        } else {
            lt_entry = caps->data_link_types;
        }
        if (lt_entry == NULL) {
            cmdarg_err("The capture device \"%s\" has no data link types.",
                       interface_opts->name);
            return WS_EXIT_IFACE_HAS_NO_LINK_TYPES;
        }
        if (caps->can_set_rfmon)
            printf("Data link types of interface %s when %sin monitor mode (use option -y to set):\n",
                   interface_opts->name,
                   (interface_opts->monitor_mode) ? "" : "not ");
        else
            printf("Data link types of interface %s (use option -y to set):\n",
                   interface_opts->name);
        for (; lt_entry != NULL;
             lt_entry = g_list_next(lt_entry)) {
            data_link_info_t *data_link_info = (data_link_info_t *)lt_entry->data;
            printf("  %s", data_link_info->name);
            if (data_link_info->description != NULL)
                printf(" (%s)", data_link_info->description);
            else
                printf(" (not supported)");
            printf("\n");
        }
    }

    if (queries & CAPS_QUERY_TIMESTAMP_TYPES) {
        if (caps->timestamp_types == NULL) {
            cmdarg_err("The capture device \"%s\" has no timestamp types.",
                       interface_opts->name);
            return WS_EXIT_IFACE_HAS_NO_TIMESTAMP_TYPES;
        }
        printf("Timestamp types of the interface (use option --time-stamp-type to set):\n");
        for (ts_entry = caps->timestamp_types; ts_entry != NULL;
             ts_entry = g_list_next(ts_entry)) {
            timestamp_info_t *timestamp = (timestamp_info_t *)ts_entry->data;
            printf("  %s", timestamp->name);
            if (timestamp->description != NULL)
                printf(" (%s)", timestamp->description);
            else
                printf(" (none)");
            printf("\n");
        }
    }
    return EXIT_SUCCESS;
}

/* Print an ASCII-formatted list of interfaces. */
void
capture_opts_print_interfaces(GList *if_list)
{
    int         i;
    GList       *if_entry;
    if_info_t   *if_info;

    i = 1;  /* Interface id number */
    for (if_entry = g_list_first(if_list); if_entry != NULL;
         if_entry = g_list_next(if_entry)) {
        if_info = (if_info_t *)if_entry->data;
        printf("%d. %s", i++, if_info->name);

        /* Print the interface friendly name, if it exists;
          if not, fall back to the vendor description, if it exists. */
        if (if_info->friendly_name != NULL){
            printf(" (%s)", if_info->friendly_name);
        } else {
            if (if_info->vendor_description != NULL)
                printf(" (%s)", if_info->vendor_description);
        }
        printf("\n");
    }
}


void
capture_opts_trim_snaplen(capture_options *capture_opts, int snaplen_min)
{
    unsigned i;
    interface_options *interface_opts;

    if (capture_opts->ifaces->len > 0) {
        for (i = 0; i < capture_opts->ifaces->len; i++) {
            interface_opts = &g_array_index(capture_opts->ifaces, interface_options, 0);
            if (interface_opts->snaplen < 1)
                interface_opts->snaplen = WTAP_MAX_PACKET_SIZE_STANDARD;
            else if (interface_opts->snaplen < snaplen_min)
                interface_opts->snaplen = snaplen_min;
        }
    } else {
        if (capture_opts->default_options.snaplen < 1)
            capture_opts->default_options.snaplen = WTAP_MAX_PACKET_SIZE_STANDARD;
        else if (capture_opts->default_options.snaplen < snaplen_min)
            capture_opts->default_options.snaplen = snaplen_min;
    }
}


void
capture_opts_trim_ring_num_files(capture_options *capture_opts)
{
    /* Check the value range of the ring_num_files parameter */
    if (capture_opts->ring_num_files > RINGBUFFER_MAX_NUM_FILES) {
        cmdarg_err("Too many ring buffer files (%u). Reducing to %u.\n", capture_opts->ring_num_files, RINGBUFFER_MAX_NUM_FILES);
        capture_opts->ring_num_files = RINGBUFFER_MAX_NUM_FILES;
    } else if (capture_opts->ring_num_files > RINGBUFFER_WARN_NUM_FILES) {
        cmdarg_err("%u is a lot of ring buffer files.\n", capture_opts->ring_num_files);
    }
#if RINGBUFFER_MIN_NUM_FILES > 0
    else if (capture_opts->ring_num_files < RINGBUFFER_MIN_NUM_FILES) {
        cmdarg_err("Too few ring buffer files (%u). Increasing to %u.\n", capture_opts->ring_num_files, RINGBUFFER_MIN_NUM_FILES);
        capture_opts->ring_num_files = RINGBUFFER_MIN_NUM_FILES;
    }
#endif
}

/*
 * If no interface was specified explicitly, pick a default.
 */
int
capture_opts_default_iface_if_necessary(capture_options *capture_opts,
                                        const char *capture_device)
{
    int status;

    /* Did the user specify an interface to use? */
    if (capture_opts->num_selected != 0 || capture_opts->ifaces->len != 0) {
        /* yes they did, return immediately - nothing further to do here */
        return 0;
    }

    /* No - is a default specified in the preferences file? */
    if (capture_device != NULL) {
        /* Yes - use it. */
        status = capture_opts_add_iface_opt(capture_opts, capture_device);
        return status;
    }
    /* No default in preferences file, just pick the first interface from the list of interfaces. */
    return capture_opts_add_iface_opt(capture_opts, "1");
}

#ifndef S_IFIFO
#define S_IFIFO _S_IFIFO
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode)  (((mode) & S_IFMT) == S_IFIFO)
#endif

/* copied from filesystem.c */
static int
capture_opts_test_for_fifo(const char *path)
{
    ws_statb64 statb;

    if (ws_stat64(path, &statb) < 0)
        return errno;

    if (S_ISFIFO(statb.st_mode))
        return ESPIPE;
    else
        return 0;
}

static bool
capture_opts_output_to_pipe(const char *save_file, bool *is_pipe)
{
    int err;

    *is_pipe = false;

    if (save_file != NULL) {
        /* We're writing to a capture file. */
        if (strcmp(save_file, "-") == 0) {
            /* Writing to stdout. */
            /* XXX - should we check whether it's a pipe?  It's arguably
               silly to do "-w - >output_file" rather than "-w output_file",
               but by not checking we might be violating the Principle Of
               Least Astonishment. */
            *is_pipe = true;
        } else {
            /* not writing to stdout, test for a FIFO (aka named pipe) */
            err = capture_opts_test_for_fifo(save_file);
            switch (err) {

            case ENOENT:      /* it doesn't exist, so we'll be creating it,
                                 and it won't be a FIFO */
            case 0:           /* found it, but it's not a FIFO */
                break;

            case ESPIPE:      /* it is a FIFO */
                *is_pipe = true;
                break;

            default:          /* couldn't stat it              */
                break;          /* ignore: later attempt to open */
                /*  will generate a nice msg     */
            }
        }
    }

    return 0;
}

void
interface_opts_free(interface_options *interface_opts)
{
    if (interface_opts == NULL)
        return;

    g_free(interface_opts->name);
    g_free(interface_opts->descr);
    g_free(interface_opts->hardware);
    g_free(interface_opts->display_name);
    g_free(interface_opts->ifname);
    g_free(interface_opts->cfilter);
    g_free(interface_opts->timestamp_type);
    g_free(interface_opts->extcap);
    g_free(interface_opts->extcap_fifo);
    if (interface_opts->extcap_args)
        g_hash_table_unref(interface_opts->extcap_args);
    if (interface_opts->extcap_pid != WS_INVALID_PID)
        ws_warning("Extcap still running during interface delete");
    g_free(interface_opts->extcap_pipedata);
    if (interface_opts->extcap_stderr)
        g_string_free(interface_opts->extcap_stderr, TRUE);
    g_free(interface_opts->extcap_control_in);
    g_free(interface_opts->extcap_control_out);
#ifdef HAVE_PCAP_REMOTE
    if (interface_opts->src_type == CAPTURE_IFREMOTE) {
        g_free(interface_opts->remote_host);
        g_free(interface_opts->remote_port);
        g_free(interface_opts->auth_username);
        g_free(interface_opts->auth_password);
    }
#endif
}

void
capture_opts_del_iface(capture_options *capture_opts, unsigned if_index)
{
    interface_options *interface_opts;

    interface_opts = &g_array_index(capture_opts->ifaces, interface_options, if_index);
    /* XXX - check if found? */
    interface_opts_free(interface_opts);

    capture_opts->ifaces = g_array_remove_index(capture_opts->ifaces, if_index);
}

interface_options*
interface_opts_from_if_info(capture_options *capture_opts, const if_info_t *if_info)
{
    interface_options *interface_opts = g_new(interface_options, 1);

    fill_in_interface_opts_from_ifinfo(interface_opts, if_info);
    fill_in_interface_opts_defaults(interface_opts, capture_opts);

    return interface_opts;
}

/*
 * Add all non-hidden selected interfaces in the "all interfaces" list
 * to the list of interfaces for the capture.
 */
void
collect_ifaces(capture_options *capture_opts)
{
    unsigned i;
    interface_t *device;
    interface_options interface_opts;

    /* Empty out the existing list of interfaces. */
    for (i = capture_opts->ifaces->len; i != 0; i--)
        capture_opts_del_iface(capture_opts, i-1);

    /* Now fill the list up again. */
    for (i = 0; i < capture_opts->all_ifaces->len; i++) {
        device = &g_array_index(capture_opts->all_ifaces, interface_t, i);
        if (device->selected) {
            interface_opts.name = g_strdup(device->name);
            interface_opts.descr = g_strdup(device->if_info.friendly_name);
            interface_opts.ifname = NULL;
            interface_opts.hardware = g_strdup(device->if_info.vendor_description);
            interface_opts.display_name = g_strdup(device->display_name);
            interface_opts.linktype = device->active_dlt;
            interface_opts.cfilter = g_strdup(device->cfilter);
            interface_opts.timestamp_type = g_strdup(device->timestamp_type);
            interface_opts.snaplen = device->snaplen;
            interface_opts.has_snaplen = device->has_snaplen;
            interface_opts.promisc_mode = device->pmode;
            interface_opts.if_type = device->if_info.type;
            interface_opts.extcap = g_strdup(device->if_info.extcap);
            interface_opts.extcap_fifo = NULL;
            interface_opts.extcap_pipedata = NULL;
            interface_opts.extcap_args = device->external_cap_args_settings;
            interface_opts.extcap_pid = WS_INVALID_PID;
            if (interface_opts.extcap_args)
                g_hash_table_ref(interface_opts.extcap_args);
            interface_opts.extcap_pipedata = NULL;
            interface_opts.extcap_stderr = NULL;
#ifdef _WIN32
            interface_opts.extcap_pipe_h = INVALID_HANDLE_VALUE;
            interface_opts.extcap_control_in_h = INVALID_HANDLE_VALUE;
            interface_opts.extcap_control_out_h = INVALID_HANDLE_VALUE;
#endif
            interface_opts.extcap_control_in = NULL;
            interface_opts.extcap_control_out = NULL;
            interface_opts.buffer_size =  device->buffer;
            interface_opts.monitor_mode = device->monitor_mode_enabled;
#ifdef HAVE_PCAP_REMOTE
            interface_opts.src_type = CAPTURE_IFREMOTE;
            interface_opts.remote_host = g_strdup(device->remote_opts.remote_host_opts.remote_host);
            interface_opts.remote_port = g_strdup(device->remote_opts.remote_host_opts.remote_port);
            interface_opts.auth_type = device->remote_opts.remote_host_opts.auth_type;
            interface_opts.auth_username = g_strdup(device->remote_opts.remote_host_opts.auth_username);
            interface_opts.auth_password = g_strdup(device->remote_opts.remote_host_opts.auth_password);
            interface_opts.datatx_udp = device->remote_opts.remote_host_opts.datatx_udp;
            interface_opts.nocap_rpcap = device->remote_opts.remote_host_opts.nocap_rpcap;
            interface_opts.nocap_local = device->remote_opts.remote_host_opts.nocap_local;
#endif
#ifdef HAVE_PCAP_SETSAMPLING
            interface_opts.sampling_method = device->remote_opts.sampling_method;
            interface_opts.sampling_param  = device->remote_opts.sampling_param;
#endif
            g_array_append_val(capture_opts->ifaces, interface_opts);
        } else {
            continue;
        }
    }
}

void
capture_opts_free_link_row(void *elem)
{
    link_row* e = (link_row*)elem;
    if (e != NULL)
        g_free(e->name);
    g_free(elem);
}

void
capture_opts_free_interface_t(interface_t *device)
{
    if (device != NULL) {
        g_free(device->name);
        g_free(device->display_name);
        g_free(device->addresses);
        g_free(device->cfilter);
        g_free(device->timestamp_type);
        g_list_free_full(device->links, capture_opts_free_link_row);
#ifdef HAVE_PCAP_REMOTE
        g_free(device->remote_opts.remote_host_opts.remote_host);
        g_free(device->remote_opts.remote_host_opts.remote_port);
        g_free(device->remote_opts.remote_host_opts.auth_username);
        g_free(device->remote_opts.remote_host_opts.auth_password);
#endif
        g_free(device->if_info.name);
        g_free(device->if_info.friendly_name);
        g_free(device->if_info.vendor_description);
        g_slist_free_full(device->if_info.addrs, g_free);
        g_free(device->if_info.extcap);
        if (device->if_info.caps) {
                free_if_capabilities(device->if_info.caps);
        }
        if (device->external_cap_args_settings) {
                g_hash_table_unref(device->external_cap_args_settings);
        }
    }
}

#endif /* HAVE_LIBPCAP */

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
