/* prefs.c
 * Routines for handling preferences
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#define WS_LOG_DOMAIN LOG_DOMAIN_EPAN

#include "ws_diag_control.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <glib.h>

#include <stdio.h>
#include <wsutil/application_flavor.h>
#include <wsutil/filesystem.h>
#include <epan/addr_resolv.h>
#include <epan/oids.h>
#include <epan/maxmind_db.h>
#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/proto.h>
#include <epan/strutil.h>
#include <epan/column.h>
#include <epan/decode_as.h>
#include <ui/capture_opts.h>
#include <wsutil/file_util.h>
#include <wsutil/report_message.h>
#include <wsutil/wslog.h>
#include <wsutil/ws_assert.h>
#include <wsutil/array.h>

#include <epan/prefs-int.h>
#include <epan/uat-int.h>

#include "epan/filter_expressions.h"

#include "epan/wmem_scopes.h"
#include <epan/stats_tree.h>

#define REG_HKCU_WIRESHARK_KEY "Software\\Wireshark"

/*
 * Module alias.
 */
typedef struct pref_module_alias {
    const char *name;           /**< name of module alias */
    module_t *module;           /**< module for which it's an alias */
} module_alias_t;

/* Internal functions */
static module_t *find_subtree(module_t *parent, const char *tilte);
static module_t *prefs_register_module_or_subtree(module_t *parent,
    const char *name, const char *title, const char *description, const char *help,
    bool is_subtree, void (*apply_cb)(void), bool use_gui);
static void prefs_register_modules(void);
static module_t *prefs_find_module_alias(const char *name);
static prefs_set_pref_e set_pref(char*, const char*, void *, bool);
static void free_col_info(GList *);
static void pre_init_prefs(void);
static bool prefs_is_column_visible(const char *cols_hidden, int col);
static bool prefs_is_column_fmt_visible(const char *cols_hidden, fmt_data *cfmt);
static unsigned prefs_module_list_foreach(wmem_tree_t *module_list, module_cb callback,
                          void *user_data, bool skip_obsolete);
static int find_val_for_string(const char *needle, const enum_val_t *haystack, int default_value);

#define PF_NAME         "preferences"
#define OLD_GPF_NAME    "wireshark.conf" /* old name for global preferences file */

static bool prefs_initialized;
static char *gpf_path;
static char *cols_hidden_list;
static char *cols_hidden_fmt_list;
static bool gui_theme_is_dark;

/*
 * XXX - variables to allow us to attempt to interpret the first
 * "mgcp.{tcp,udp}.port" in a preferences file as
 * "mgcp.{tcp,udp}.gateway_port" and the second as
 * "mgcp.{tcp,udp}.callagent_port".
 */
static int mgcp_tcp_port_count;
static int mgcp_udp_port_count;

e_prefs prefs;

static const enum_val_t gui_console_open_type[] = {
    {"NEVER", "NEVER", LOG_CONSOLE_OPEN_NEVER},
    {"AUTOMATIC", "AUTOMATIC", LOG_CONSOLE_OPEN_AUTO},
    {"ALWAYS", "ALWAYS", LOG_CONSOLE_OPEN_ALWAYS},
    {NULL, NULL, -1}
};

static const enum_val_t gui_version_placement_type[] = {
    {"WELCOME", "WELCOME", version_welcome_only},
    {"TITLE", "TITLE", version_title_only},
    {"BOTH", "BOTH", version_both},
    {"NEITHER", "NEITHER", version_neither},
    {NULL, NULL, -1}
};

static const enum_val_t gui_fileopen_style[] = {
    {"LAST_OPENED", "LAST_OPENED", FO_STYLE_LAST_OPENED},
    {"SPECIFIED", "SPECIFIED", FO_STYLE_SPECIFIED},
    {"CWD", "CWD", FO_STYLE_CWD},
    {NULL, NULL, -1}
};

static const enum_val_t gui_toolbar_style[] = {
    {"ICONS", "ICONS", 0},
    {"TEXT", "TEXT", 1},
    {"BOTH", "BOTH", 2},
    {NULL, NULL, -1}
};

static const enum_val_t gui_layout_content[] = {
    {"NONE", "NONE", 0},
    {"PLIST", "PLIST", 1},
    {"PDETAILS", "PDETAILS", 2},
    {"PBYTES", "PBYTES", 3},
    {"PDIAGRAM", "PDIAGRAM", 4},
    {NULL, NULL, -1}
};

static const enum_val_t gui_packet_dialog_layout[] = {
    {"vertical", "Vertical (Stacked)", layout_vertical},
    {"horizontal", "Horizontal (Side-by-side)", layout_horizontal},
    {NULL, NULL, -1}
};

static const enum_val_t gui_update_channel[] = {
    {"DEVELOPMENT", "DEVELOPMENT", UPDATE_CHANNEL_DEVELOPMENT},
    {"STABLE", "STABLE", UPDATE_CHANNEL_STABLE},
    {NULL, NULL, -1}
};

static const enum_val_t gui_selection_style[] = {
    {"DEFAULT", "DEFAULT",   COLOR_STYLE_DEFAULT},
    {"FLAT",    "FLAT",      COLOR_STYLE_FLAT},
    {"GRADIENT", "GRADIENT", COLOR_STYLE_GRADIENT},
    {NULL, NULL, -1}
};

static const enum_val_t gui_color_scheme[] = {
    {"system",  "System Default",   COLOR_SCHEME_DEFAULT},
    {"light",   "Light Mode",       COLOR_SCHEME_LIGHT},
    {"dark",    "Dark Mode",        COLOR_SCHEME_DARK},
    {NULL, NULL, -1}
};

static const enum_val_t gui_packet_list_copy_format_options_for_keyboard_shortcut[] = {
    {"TEXT", "Text", COPY_FORMAT_TEXT},
    {"CSV",  "CSV",  COPY_FORMAT_CSV},
    {"YAML", "YAML", COPY_FORMAT_YAML},
    {"HTML", "HTML", COPY_FORMAT_HTML},
    {NULL, NULL, -1}
};

/* None : Historical behavior, no deinterlacing */
#define CONV_DEINT_CHOICE_NONE 0
/* MI : MAC & Interface */
#define CONV_DEINT_CHOICE_MI CONV_DEINT_KEY_MAC + CONV_DEINT_KEY_INTERFACE
/* VM : VLAN & MAC */
#define CONV_DEINT_CHOICE_VM CONV_DEINT_KEY_VLAN + CONV_DEINT_KEY_MAC
/* VMI : VLAN & MAC & Interface */
#define CONV_DEINT_CHOICE_VMI CONV_DEINT_KEY_VLAN + CONV_DEINT_KEY_MAC + CONV_DEINT_KEY_INTERFACE

static const enum_val_t conv_deint_options[] = {
    {"NONE", "NONE", CONV_DEINT_CHOICE_NONE},
    {".MI", ".MI", CONV_DEINT_CHOICE_MI },
    {"VM.", "VM.", CONV_DEINT_CHOICE_VM },
    {"VMI", "VMI", CONV_DEINT_CHOICE_VMI },
    {NULL, NULL, -1}
};

static const enum_val_t abs_time_format_options[] = {
    {"NEVER", "Never", ABS_TIME_ASCII_NEVER},
    {"TREE", "Protocol tree only", ABS_TIME_ASCII_TREE},
    {"COLUMN", "Protocol tree and columns", ABS_TIME_ASCII_COLUMN},
    {"ALWAYS", "Always", ABS_TIME_ASCII_ALWAYS},
    {NULL, NULL, -1}
};

static int num_capture_cols = 7;
static const char *capture_cols[7] = {
    "INTERFACE",
    "LINK",
    "PMODE",
    "SNAPLEN",
    "MONITOR",
    "BUFFER",
    "FILTER"
};
#define CAPTURE_COL_TYPE_DESCRIPTION \
    "Possible values: INTERFACE, LINK, PMODE, SNAPLEN, MONITOR, BUFFER, FILTER\n"

static const enum_val_t gui_packet_list_elide_mode[] = {
    {"LEFT", "LEFT", ELIDE_LEFT},
    {"RIGHT", "RIGHT", ELIDE_RIGHT},
    {"MIDDLE", "MIDDLE", ELIDE_MIDDLE},
    {"NONE", "NONE", ELIDE_NONE},
    {NULL, NULL, -1}
};

/** Struct to hold preference data */
struct preference {
    const char *name;                /**< name of preference */
    const char *title;               /**< title to use in GUI */
    const char *description;         /**< human-readable description of preference */
    int ordinal;                     /**< ordinal number of this preference */
    pref_type_e type;                /**< type of that preference */
    bool obsolete;                   /**< obsolete preference flag */
    unsigned int effect_flags;       /**< Flags of types effected by preference (PREF_TYPE_DISSECTION, PREF_EFFECT_CAPTURE, etc).
                                          Flags must be non-zero to ensure saving to disk */
    union {                          /* The Qt preference code assumes that these will all be pointers (and unique) */
        unsigned *uint;
        bool *boolp;
        int *enump;
        char **string;
        range_t **range;
        struct epan_uat* uat;
        color_t *colorp;
        GList** list;
    } varp;                          /**< pointer to variable storing the value */
    union {
        unsigned uint;
        bool boolval;
        int enumval;
        char *string;
        range_t *range;
        color_t color;
        GList* list;
    } stashed_val;                     /**< original value, when editing from the GUI */
    union {
        unsigned uint;
        bool boolval;
        int enumval;
        char *string;
        range_t *range;
        color_t color;
        GList* list;
    } default_val;                   /**< the default value of the preference */
    union {
      unsigned base;                 /**< input/output base, for PREF_UINT */
      uint32_t max_value;            /**< maximum value of a range */
      struct {
        const enum_val_t *enumvals;  /**< list of name & values */
        bool radio_buttons;          /**< true if it should be shown as
                                          radio buttons rather than as an
                                          option menu or combo box in
                                          the preferences tab */
      } enum_info;                   /**< for PREF_ENUM */
    } info;                          /**< display/text file information */
    struct pref_custom_cbs custom_cbs;   /**< for PREF_CUSTOM */
    const char *dissector_table;     /**< for PREF_DECODE_AS_RANGE */
    const char *dissector_desc;      /**< for PREF_DECODE_AS_RANGE */
};

const char* prefs_get_description(pref_t *pref)
{
    return pref->description;
}

const char* prefs_get_title(pref_t *pref)
{
    return pref->title;
}

int prefs_get_type(pref_t *pref)
{
    return pref->type;
}

const char* prefs_get_name(pref_t *pref)
{
    return pref->name;
}

uint32_t prefs_get_max_value(pref_t *pref)
{
    return pref->info.max_value;
}

const char* prefs_get_dissector_table(pref_t *pref)
{
    return pref->dissector_table;
}

static const char* prefs_get_dissector_description(pref_t *pref)
{
    return pref->dissector_desc;
}

/*
 * List of all modules with preference settings.
 */
static wmem_tree_t *prefs_modules;

/*
 * List of all modules that should show up at the top level of the
 * tree in the preference dialog box.
 */
static wmem_tree_t *prefs_top_level_modules;

/*
 * List of aliases for modules.
 */
static wmem_tree_t *prefs_module_aliases;

/** Sets up memory used by proto routines. Called at program startup */
void
prefs_init(void)
{
    memset(&prefs, 0, sizeof(prefs));
    prefs_modules = wmem_tree_new(wmem_epan_scope());
    prefs_top_level_modules = wmem_tree_new(wmem_epan_scope());
    prefs_module_aliases = wmem_tree_new(wmem_epan_scope());
}

/*
 * Free the strings for a string-like preference.
 */
static void
free_string_like_preference(pref_t *pref)
{
    g_free(*pref->varp.string);
    *pref->varp.string = NULL;
    g_free(pref->default_val.string);
    pref->default_val.string = NULL;
}

static void
free_pref(void *data, void *user_data _U_)
{
    pref_t *pref = (pref_t *)data;

    switch (pref->type) {
    case PREF_BOOL:
    case PREF_ENUM:
    case PREF_UINT:
    case PREF_STATIC_TEXT:
    case PREF_UAT:
    case PREF_COLOR:
        break;
    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        free_string_like_preference(pref);
        break;
    case PREF_RANGE:
    case PREF_DECODE_AS_RANGE:
        wmem_free(wmem_epan_scope(), *pref->varp.range);
        *pref->varp.range = NULL;
        wmem_free(wmem_epan_scope(), pref->default_val.range);
        pref->default_val.range = NULL;
        break;
    case PREF_CUSTOM:
        if (strcmp(pref->name, "columns") == 0)
          pref->stashed_val.boolval = true;
        pref->custom_cbs.free_cb(pref);
        break;
    /* non-generic preferences */
    case PREF_PROTO_TCP_SNDAMB_ENUM:
        break;
    }

    g_free(pref);
}

static unsigned
free_module_prefs(module_t *module, void *data _U_)
{
    if (module->prefs) {
        g_list_foreach(module->prefs, free_pref, NULL);
        g_list_free(module->prefs);
    }
    module->prefs = NULL;
    module->numprefs = 0;
    if (module->submodules) {
        prefs_module_list_foreach(module->submodules, free_module_prefs, NULL, false);
    }
    /*  We don't free the actual module: its submodules pointer points to
        a wmem_tree and the module itself is stored in a wmem_tree
     */

    return 0;
}

/** Frees memory used by proto routines. Called at program shutdown */
void
prefs_cleanup(void)
{
    /*  This isn't strictly necessary since we're exiting anyway, but let's
     *  do what clean up we can.
     */
    prefs_module_list_foreach(prefs_modules, free_module_prefs, NULL, false);

    /* Clean the uats */
    uat_cleanup();

    /* Shut down mmdbresolve */
    maxmind_db_pref_cleanup();

    g_free(prefs.saved_at_version);
    g_free(gpf_path);
    gpf_path = NULL;
}

void prefs_set_gui_theme_is_dark(bool is_dark)
{
    gui_theme_is_dark = is_dark;
}

/*
 * Register a module that will have preferences.
 * Specify the module under which to register it or NULL to register it
 * at the top level, the name used for the module in the preferences file,
 * the title used in the tab for it in a preferences dialog box, and a
 * routine to call back when we apply the preferences.
 */
static module_t *
prefs_register_module(module_t *parent, const char *name, const char *title,
                      const char *description, const char *help, void (*apply_cb)(void),
                      const bool use_gui)
{
    return prefs_register_module_or_subtree(parent, name, title, description, help,
                                            false, apply_cb, use_gui);
}

static void
prefs_deregister_module(module_t *parent, const char *name, const char *title)
{
    /* Remove this module from the list of all modules */
    module_t *module = (module_t *)wmem_tree_remove_string(prefs_modules, name, WMEM_TREE_STRING_NOCASE);

    if (!module)
        return;

    if (parent == NULL) {
        /* Remove from top */
        wmem_tree_remove_string(prefs_top_level_modules, title, WMEM_TREE_STRING_NOCASE);
    } else if (parent->submodules) {
        /* Remove from parent */
        wmem_tree_remove_string(parent->submodules, title, WMEM_TREE_STRING_NOCASE);
    }

    free_module_prefs(module, NULL);
    wmem_free(wmem_epan_scope(), module);
}

/*
 * Register a subtree that will have modules under it.
 * Specify the module under which to register it or NULL to register it
 * at the top level and the title used in the tab for it in a preferences
 * dialog box.
 */
static module_t *
prefs_register_subtree(module_t *parent, const char *title, const char *description,
                       void (*apply_cb)(void))
{
    return prefs_register_module_or_subtree(parent, NULL, title, description, NULL,
                                            true, apply_cb,
                                            parent ? parent->use_gui : false);
}

static module_t *
prefs_register_module_or_subtree(module_t *parent, const char *name,
                                 const char *title, const char *description,
                                 const char *help,
                                 bool is_subtree, void (*apply_cb)(void),
                                 bool use_gui)
{
    module_t *module;

    /* this module may have been created as a subtree item previously */
    if ((module = find_subtree(parent, title))) {
        /* the module is currently a subtree */
        module->name = name;
        module->apply_cb = apply_cb;
        module->description = description;
        module->help = help;

        /* Registering it as a module (not just as a subtree) twice is an
         * error in the code for the same reason as below. */
        if (prefs_find_module(name) != NULL) {
            ws_error("Preference module \"%s\" is being registered twice", name);
        }
        wmem_tree_insert_string(prefs_modules, name, module,
                              WMEM_TREE_STRING_NOCASE);

        return module;
    }

    module = wmem_new(wmem_epan_scope(), module_t);
    module->name = name;
    module->title = title;
    module->description = description;
    module->help = help;
    module->apply_cb = apply_cb;
    module->prefs = NULL;    /* no preferences, to start */
    module->parent = parent;
    module->submodules = NULL;    /* no submodules, to start */
    module->numprefs = 0;
    module->prefs_changed_flags = 0;
    module->obsolete = false;
    module->use_gui = use_gui;
    /* A module's preferences affects dissection unless otherwise told */
    module->effect_flags = PREF_EFFECT_DISSECTION;

    /*
     * Do we have a module name?
     */
    if (name != NULL) {

        /* Accept any letter case to conform with protocol names. ASN1 protocols
         * don't use lower case names, so we can't require lower case. */
        if (module_check_valid_name(name, false) != '\0') {
                ws_error("Preference module \"%s\" contains invalid characters", name);
        }

        /*
         * Make sure there's not already a module with that
         * name.  Crash if there is, as that's an error in the
         * code, and the code has to be fixed not to register
         * more than one module with the same name.
         *
         * We search the list of all modules; the subtree stuff
         * doesn't require preferences in subtrees to have names
         * that reflect the subtree they're in (that would require
         * protocol preferences to have a bogus "protocol.", or
         * something such as that, to be added to all their names).
         */
        if (prefs_find_module(name) != NULL)
            ws_error("Preference module \"%s\" is being registered twice", name);

        /*
         * Insert this module in the list of all modules.
         */
        wmem_tree_insert_string(prefs_modules, name, module, WMEM_TREE_STRING_NOCASE);
    } else {
        /*
         * This has no name, just a title; check to make sure it's a
         * subtree, and crash if it's not.
         */
        if (!is_subtree)
            ws_error("Preferences module with no name is being registered at the top level");
    }

    /*
     * Insert this module into the appropriate place in the display
     * tree.
     */
    if (parent == NULL) {
        /*
         * It goes at the top.
         */
        wmem_tree_insert_string(prefs_top_level_modules, title, module, WMEM_TREE_STRING_NOCASE);
    } else {
        /*
         * It goes into the list for this module.
         */

        if (parent->submodules == NULL)
            parent->submodules = wmem_tree_new(wmem_epan_scope());

        wmem_tree_insert_string(parent->submodules, title, module, WMEM_TREE_STRING_NOCASE);
    }

    return module;
}

void
prefs_register_module_alias(const char *name, module_t *module)
{
    module_alias_t *alias;

    /*
     * Accept any name that can occur in protocol names. We allow upper-case
     * letters, to handle the Diameter dissector having used "Diameter" rather
     * than "diameter" as its preference module name in the past.
     *
     * Crash if the name is invalid, as that's an error in the code, but the name
     * can be used on the command line, and shouldn't require quoting, etc.
     */
    if (module_check_valid_name(name, false) != '\0') {
        ws_error("Preference module alias \"%s\" contains invalid characters", name);
    }

    /*
     * Make sure there's not already an alias with that
     * name.  Crash if there is, as that's an error in the
     * code, and the code has to be fixed not to register
     * more than one alias with the same name.
     *
     * We search the list of all aliases.
     */
    if (prefs_find_module_alias(name) != NULL)
        ws_error("Preference module alias \"%s\" is being registered twice", name);

    alias = wmem_new(wmem_epan_scope(), module_alias_t);
    alias->name = name;
    alias->module = module;

    /*
     * Insert this module in the list of all modules.
     */
    wmem_tree_insert_string(prefs_module_aliases, name, alias, WMEM_TREE_STRING_NOCASE);
}

/*
 * Register that a protocol has preferences.
 */
module_t *protocols_module;

module_t *
prefs_register_protocol(int id, void (*apply_cb)(void))
{
    protocol_t *protocol;

    /*
     * Have we yet created the "Protocols" subtree?
     */
    if (protocols_module == NULL) {
        /*
         * No.  Register Protocols subtree as well as any preferences
         * for non-dissector modules.
         */
        pre_init_prefs();
        prefs_register_modules();
    }
    protocol = find_protocol_by_id(id);
    if (protocol == NULL)
        ws_error("Protocol preferences being registered with an invalid protocol ID");
    return prefs_register_module(protocols_module,
                                 proto_get_protocol_filter_name(id),
                                 proto_get_protocol_short_name(protocol),
                                 proto_get_protocol_name(id), NULL, apply_cb, true);
}

void
prefs_deregister_protocol (int id)
{
    protocol_t *protocol = find_protocol_by_id(id);
    if (protocol == NULL)
        ws_error("Protocol preferences being de-registered with an invalid protocol ID");
    prefs_deregister_module (protocols_module,
                             proto_get_protocol_filter_name(id),
                             proto_get_protocol_short_name(protocol));
}

module_t *
prefs_register_protocol_subtree(const char *subtree, int id, void (*apply_cb)(void))
{
    protocol_t *protocol;
    module_t   *subtree_module;
    module_t   *new_module;
    char       *sep = NULL, *ptr = NULL, *orig = NULL;

    /*
     * Have we yet created the "Protocols" subtree?
     * XXX - can we just do this by registering Protocols/{subtree}?
     * If not, why not?
     */
    if (protocols_module == NULL) {
        /*
         * No.  Register Protocols subtree as well as any preferences
         * for non-dissector modules.
         */
        pre_init_prefs();
        prefs_register_modules();
    }

    subtree_module = protocols_module;

    if (subtree) {
        /* take a copy of the buffer, orig keeps a base pointer while ptr
         * walks through the string */
        orig = ptr = g_strdup(subtree);

        while (ptr && *ptr) {

            if ((sep = strchr(ptr, '/')))
                *sep++ = '\0';

            if (!(new_module = find_subtree(subtree_module, ptr))) {
                /*
                 * There's no such module; create it, with the description
                 * being the name (if it's later registered explicitly
                 * with a description, that will override it).
                 */
                ptr = wmem_strdup(wmem_epan_scope(), ptr);
                new_module = prefs_register_subtree(subtree_module, ptr, ptr, NULL);
            }

            subtree_module = new_module;
            ptr = sep;

        }

        g_free(orig);
    }

    protocol = find_protocol_by_id(id);
    if (protocol == NULL)
        ws_error("Protocol subtree being registered with an invalid protocol ID");
    return prefs_register_module(subtree_module,
                                 proto_get_protocol_filter_name(id),
                                 proto_get_protocol_short_name(protocol),
                                 proto_get_protocol_name(id), NULL, apply_cb, true);
}


/*
 * Register that a protocol used to have preferences but no longer does,
 * by creating an "obsolete" module for it.
 */
module_t *
prefs_register_protocol_obsolete(int id)
{
    module_t *module;
    protocol_t *protocol;

    /*
     * Have we yet created the "Protocols" subtree?
     */
    if (protocols_module == NULL) {
        /*
         * No.  Register Protocols subtree as well as any preferences
         * for non-dissector modules.
         */
        pre_init_prefs();
        prefs_register_modules();
    }
    protocol = find_protocol_by_id(id);
    if (protocol == NULL)
        ws_error("Protocol being registered with an invalid protocol ID");
    module = prefs_register_module(protocols_module,
                                   proto_get_protocol_filter_name(id),
                                   proto_get_protocol_short_name(protocol),
                                   proto_get_protocol_name(id), NULL, NULL, true);
    module->obsolete = true;
    return module;
}

/*
 * Register that a statistical tap has preferences.
 *
 * "name" is a name for the tap to use on the command line with "-o"
 * and in preference files.
 *
 * "title" is a short human-readable name for the tap.
 *
 * "description" is a longer human-readable description of the tap.
 */
module_t *stats_module;

module_t *
prefs_register_stat(const char *name, const char *title,
                    const char *description, void (*apply_cb)(void))
{
    /*
     * Have we yet created the "Statistics" subtree?
     */
    if (stats_module == NULL) {
        /*
         * No.  Register Statistics subtree as well as any preferences
         * for non-dissector modules.
         */
        pre_init_prefs();
        prefs_register_modules();
    }

    return prefs_register_module(stats_module, name, title, description, NULL,
                                 apply_cb, true);
}

/*
 * Register that a codec has preferences.
 *
 * "name" is a name for the codec to use on the command line with "-o"
 * and in preference files.
 *
 * "title" is a short human-readable name for the codec.
 *
 * "description" is a longer human-readable description of the codec.
 */
module_t *codecs_module;

module_t *
prefs_register_codec(const char *name, const char *title,
                     const char *description, void (*apply_cb)(void))
{
    /*
     * Have we yet created the "Codecs" subtree?
     */
    if (codecs_module == NULL) {
        /*
         * No.  Register Codecs subtree as well as any preferences
         * for non-dissector modules.
         */
        pre_init_prefs();
        prefs_register_modules();
    }

    return prefs_register_module(codecs_module, name, title, description, NULL,
                                 apply_cb, true);
}

module_t *
prefs_find_module(const char *name)
{
    return (module_t *)wmem_tree_lookup_string(prefs_modules, name, WMEM_TREE_STRING_NOCASE);
}

static module_t *
find_subtree(module_t *parent, const char *name)
{
    return (module_t *)wmem_tree_lookup_string(parent ? parent->submodules : prefs_top_level_modules, name, WMEM_TREE_STRING_NOCASE);
}

/*
 * Call a callback function, with a specified argument, for each module
 * in a list of modules.  If the list is NULL, searches the top-level
 * list in the display tree of modules.  If any callback returns a
 * non-zero value, we stop and return that value, otherwise we
 * return 0.
 *
 * Normally "obsolete" modules are ignored; their sole purpose is to allow old
 * preferences for dissectors that no longer have preferences to be
 * silently ignored in preference files.  Does not ignore subtrees,
 * as this can be used when walking the display tree of modules.
 */

typedef struct {
    module_cb callback;
    void *user_data;
    unsigned ret;
    bool skip_obsolete;
} call_foreach_t;

static bool
call_foreach_cb(const void *key _U_, void *value, void *data)
{
    module_t *module = (module_t*)value;
    call_foreach_t *call_data = (call_foreach_t*)data;

    if (!call_data->skip_obsolete || !module->obsolete)
        call_data->ret = (*call_data->callback)(module, call_data->user_data);

    return (call_data->ret != 0);
}

static unsigned
prefs_module_list_foreach(wmem_tree_t *module_list, module_cb callback,
                          void *user_data, bool skip_obsolete)
{
    call_foreach_t call_data;

    if (module_list == NULL)
        module_list = prefs_top_level_modules;

    call_data.callback = callback;
    call_data.user_data = user_data;
    call_data.ret = 0;
    call_data.skip_obsolete = skip_obsolete;
    wmem_tree_foreach(module_list, call_foreach_cb, &call_data);
    return call_data.ret;
}

/*
 * Returns true if module has any submodules
 */
bool
prefs_module_has_submodules(module_t *module)
{
    if (module->submodules == NULL) {
        return false;
    }

    if (wmem_tree_is_empty(module->submodules)) {
        return false;
    }

    return true;
}

/*
 * Call a callback function, with a specified argument, for each module
 * in the list of all modules.  (This list does not include subtrees.)
 *
 * Ignores "obsolete" modules; their sole purpose is to allow old
 * preferences for dissectors that no longer have preferences to be
 * silently ignored in preference files.
 */
unsigned
prefs_modules_foreach(module_cb callback, void *user_data)
{
    return prefs_module_list_foreach(prefs_modules, callback, user_data, true);
}

/*
 * Call a callback function, with a specified argument, for each submodule
 * of specified modules.  If the module is NULL, goes through the top-level
 * list in the display tree of modules.
 *
 * Ignores "obsolete" modules; their sole purpose is to allow old
 * preferences for dissectors that no longer have preferences to be
 * silently ignored in preference files.  Does not ignore subtrees,
 * as this can be used when walking the display tree of modules.
 */
unsigned
prefs_modules_foreach_submodules(module_t *module, module_cb callback,
                                 void *user_data)
{
    return prefs_module_list_foreach((module)?module->submodules:prefs_top_level_modules, callback, user_data, true);
}

static bool
call_apply_cb(const void *key _U_, void *value, void *data _U_)
{
    module_t *module = (module_t *)value;

    if (module->obsolete)
        return false;
    if (module->prefs_changed_flags) {
        if (module->apply_cb != NULL)
            (*module->apply_cb)();
        module->prefs_changed_flags = 0;
    }
    if (module->submodules)
        wmem_tree_foreach(module->submodules, call_apply_cb, NULL);
    return false;
}

/*
 * Call the "apply" callback function for each module if any of its
 * preferences have changed, and then clear the flag saying its
 * preferences have changed, as the module has been notified of that
 * fact.
 */
void
prefs_apply_all(void)
{
    wmem_tree_foreach(prefs_modules, call_apply_cb, NULL);
}

/*
 * Call the "apply" callback function for a specific module if any of
 * its preferences have changed, and then clear the flag saying its
 * preferences have changed, as the module has been notified of that
 * fact.
 */
void
prefs_apply(module_t *module)
{
    if (module && module->prefs_changed_flags)
        call_apply_cb(NULL, module, NULL);
}

static module_t *
prefs_find_module_alias(const char *name)
{
    module_alias_t *alias;

    alias = (module_alias_t *)wmem_tree_lookup_string(prefs_module_aliases, name, WMEM_TREE_STRING_NOCASE);
    if (alias == NULL)
        return NULL;
    return alias->module;
}

/*
 * Register a preference in a module's list of preferences.
 * If it has a title, give it an ordinal number; otherwise, it's a
 * preference that won't show up in the UI, so it shouldn't get an
 * ordinal number (the ordinal should be the ordinal in the set of
 * *visible* preferences).
 */
static pref_t *
register_preference(module_t *module, const char *name, const char *title,
                    const char *description, pref_type_e type, bool obsolete)
{
    pref_t *preference;
    const char *p;
    const char *name_prefix = (module->name != NULL) ? module->name : module->parent->name;

    preference = g_new(pref_t,1);
    preference->name = name;
    preference->title = title;
    preference->description = description;
    preference->type = type;
    preference->obsolete = obsolete;
    /* Default to module's preference effects */
    preference->effect_flags = module->effect_flags;

    if (title != NULL)
        preference->ordinal = module->numprefs;
    else
        preference->ordinal = -1;    /* no ordinal for you */

    /*
     * Make sure that only lower-case ASCII letters, numbers,
     * underscores, and dots appear in the preference name.
     *
     * Crash if there is, as that's an error in the code;
     * you can make the title and description nice strings
     * with capitalization, white space, punctuation, etc.,
     * but the name can be used on the command line,
     * and shouldn't require quoting, shifting, etc.
     */
    for (p = name; *p != '\0'; p++)
        if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '_' || *p == '.'))
            ws_error("Preference \"%s.%s\" contains invalid characters", module->name, name);

    /*
     * Make sure there's not already a preference with that
     * name.  Crash if there is, as that's an error in the
     * code, and the code has to be fixed not to register
     * more than one preference with the same name.
     */
    if (prefs_find_preference(module, name) != NULL)
        ws_error("Preference %s has already been registered", name);

    if ((!preference->obsolete) &&
        /* Don't compare if it's a subtree */
        (module->name != NULL)) {
        /*
         * Make sure the preference name doesn't begin with the
         * module name, as that's redundant and Just Silly.
         */
        if (!((strncmp(name, module->name, strlen(module->name)) != 0) ||
            (((name[strlen(module->name)]) != '.') && ((name[strlen(module->name)]) != '_'))))
            ws_error("Preference %s begins with the module name", name);
    }

    /* The title shows up in the preferences dialog. Make sure it's UI-friendly. */
    if (preference->title) {
        const char *cur_char;
        if (preference->type != PREF_STATIC_TEXT && g_utf8_strlen(preference->title, -1) > 80) { // Arbitrary.
            ws_error("Title for preference %s.%s is too long: %s", name_prefix, preference->name, preference->title);
        }

        if (!g_utf8_validate(preference->title, -1, NULL)) {
            ws_error("Title for preference %s.%s isn't valid UTF-8.", name_prefix, preference->name);
        }

        for (cur_char = preference->title; *cur_char; cur_char = g_utf8_next_char(cur_char)) {
            if (!g_unichar_isprint(g_utf8_get_char(cur_char))) {
                ws_error("Title for preference %s.%s isn't printable UTF-8.", name_prefix, preference->name);
            }
        }
    }

    if (preference->description) {
        if (!g_utf8_validate(preference->description, -1, NULL)) {
            ws_error("Description for preference %s.%s isn't valid UTF-8.", name_prefix, preference->name);
        }
    }

    /*
     * We passed all of our checks. Add the preference.
     */
    module->prefs = g_list_append(module->prefs, preference);
    if (title != NULL)
        module->numprefs++;

    return preference;
}

/*
 * Find a preference in a module's list of preferences, given the module
 * and the preference's name.
 */
typedef struct {
    GList *list_entry;
    const char *name;
    module_t *submodule;
} find_pref_arg_t;

static int
preference_match(const void *a, const void *b)
{
    const pref_t *pref = (const pref_t *)a;
    const char *name = (const char *)b;

    return strcmp(name, pref->name);
}

static bool
module_find_pref_cb(const void *key _U_, void *value, void *data)
{
    find_pref_arg_t* arg = (find_pref_arg_t*)data;
    GList *list_entry;
    module_t *module = (module_t *)value;

    if (module == NULL)
        return false;

    list_entry = g_list_find_custom(module->prefs, arg->name,
        preference_match);

    if (list_entry == NULL)
        return false;

    arg->list_entry = list_entry;
    arg->submodule = module;
    return true;
}

/* Tries to find a preference, setting containing_module to the (sub)module
 * holding this preference. */
static pref_t *
prefs_find_preference_with_submodule(module_t *module, const char *name,
        module_t **containing_module)
{
    find_pref_arg_t arg;
    GList *list_entry;

    if (module == NULL)
        return NULL;    /* invalid parameters */

    list_entry = g_list_find_custom(module->prefs, name,
        preference_match);
    arg.submodule = NULL;

    if (list_entry == NULL)
    {
        arg.list_entry = NULL;
        if (module->submodules != NULL)
        {
            arg.name = name;
            wmem_tree_foreach(module->submodules, module_find_pref_cb, &arg);
        }

        list_entry = arg.list_entry;
    }

    if (list_entry == NULL)
        return NULL;    /* no such preference */

    if (containing_module)
        *containing_module = arg.submodule ? arg.submodule : module;

    return (pref_t *) list_entry->data;
}

pref_t *
prefs_find_preference(module_t *module, const char *name)
{
    return prefs_find_preference_with_submodule(module, name, NULL);
}

/*
 * Returns true if the given protocol has registered preferences
 */
bool
prefs_is_registered_protocol(const char *name)
{
    module_t *m = prefs_find_module(name);

    return (m != NULL && !m->obsolete);
}

/*
 * Returns the module title of a registered protocol
 */
const char *
prefs_get_title_by_name(const char *name)
{
    module_t *m = prefs_find_module(name);

    return (m != NULL && !m->obsolete) ? m->title : NULL;
}

/*
 * Register a preference with an unsigned integral value.
 */
void
prefs_register_uint_preference(module_t *module, const char *name,
                               const char *title, const char *description,
                               unsigned base, unsigned *var)
{
    pref_t *preference;

    preference = register_preference(module, name, title, description,
                                     PREF_UINT, false);
    preference->varp.uint = var;
    preference->default_val.uint = *var;
    ws_assert(base > 0 && base != 1 && base < 37);
    preference->info.base = base;
}

/*
 * XXX Add a prefs_register_{uint16|port}_preference which sets max_value?
 */


/*
 * Register a "custom" preference with a unsigned integral value.
 * XXX - This should be temporary until we can find a better way
 * to do "custom" preferences
 */
static void
prefs_register_uint_custom_preference(module_t *module, const char *name,
                                      const char *title, const char *description,
                                      struct pref_custom_cbs* custom_cbs, unsigned *var)
{
    pref_t *preference;

    preference = register_preference(module, name, title, description,
                                     PREF_CUSTOM, false);

    preference->custom_cbs = *custom_cbs;
    preference->varp.uint = var;
    preference->default_val.uint = *var;
}

/*
 * Register a preference with an Boolean value.
 */
void
prefs_register_bool_preference(module_t *module, const char *name,
                               const char *title, const char *description,
                               bool *var)
{
    pref_t *preference;

    preference = register_preference(module, name, title, description,
                                     PREF_BOOL, false);
    preference->varp.boolp = var;
    preference->default_val.boolval = *var;
}

unsigned int prefs_set_bool_value(pref_t *pref, bool value, pref_source_t source)
{
    unsigned int changed = 0;

    switch (source)
    {
    case pref_default:
        if (pref->default_val.boolval != value) {
            pref->default_val.boolval = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    case pref_stashed:
        if (pref->stashed_val.boolval != value) {
            pref->stashed_val.boolval = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    case pref_current:
        if (*pref->varp.boolp != value) {
            *pref->varp.boolp = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return changed;
}

void prefs_invert_bool_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        pref->default_val.boolval = !pref->default_val.boolval;
        break;
    case pref_stashed:
        pref->stashed_val.boolval = !pref->stashed_val.boolval;
        break;
    case pref_current:
        *pref->varp.boolp = !(*pref->varp.boolp);
        break;
    default:
        ws_assert_not_reached();
        break;
    }
}

bool prefs_get_bool_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return pref->default_val.boolval;
    case pref_stashed:
        return pref->stashed_val.boolval;
    case pref_current:
        return *pref->varp.boolp;
    default:
        ws_assert_not_reached();
        break;
    }

    return false;
}

/*
 * Register a preference with an enumerated value.
 */
/*
 * XXX Should we get rid of the radio_buttons parameter and make that
 * behavior automatic depending on the number of items?
 */
void
prefs_register_enum_preference(module_t *module, const char *name,
                               const char *title, const char *description,
                               int *var, const enum_val_t *enumvals,
                               bool radio_buttons)
{
    pref_t *preference;

    /* Validate that the "name one would use on the command line for the value"
     * doesn't require quoting, etc. It's all treated case-insensitively so we
     * don't care about upper vs lower case.
     */
    for (size_t i = 0; enumvals[i].name != NULL; i++) {
        for (const char *p = enumvals[i].name; *p != '\0'; p++)
            if (!(g_ascii_isalnum(*p) || *p == '_' || *p == '.' || *p == '-'))
                ws_error("Preference \"%s.%s\" enum value name \"%s\" contains invalid characters",
                    module->name, name, enumvals[i].name);
    }


    preference = register_preference(module, name, title, description,
                                     PREF_ENUM, false);
    preference->varp.enump = var;
    preference->default_val.enumval = *var;
    preference->info.enum_info.enumvals = enumvals;
    preference->info.enum_info.radio_buttons = radio_buttons;
}

unsigned int prefs_set_enum_value(pref_t *pref, int value, pref_source_t source)
{
    unsigned int changed = 0;

    switch (source)
    {
    case pref_default:
        if (pref->default_val.enumval != value) {
            pref->default_val.enumval = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    case pref_stashed:
        if (pref->stashed_val.enumval != value) {
            pref->stashed_val.enumval = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    case pref_current:
        if (*pref->varp.enump != value) {
            *pref->varp.enump = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return changed;
}

unsigned int prefs_set_enum_string_value(pref_t *pref, const char *value, pref_source_t source)
{
    int enum_val = find_val_for_string(value, pref->info.enum_info.enumvals, *pref->varp.enump);

    return prefs_set_enum_value(pref, enum_val, source);
}

int prefs_get_enum_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return pref->default_val.enumval;
    case pref_stashed:
        return pref->stashed_val.enumval;
    case pref_current:
        return *pref->varp.enump;
    default:
        ws_assert_not_reached();
        break;
    }

    return 0;
}

const enum_val_t* prefs_get_enumvals(pref_t *pref)
{
    return pref->info.enum_info.enumvals;
}

bool prefs_get_enum_radiobuttons(pref_t *pref)
{
    return pref->info.enum_info.radio_buttons;
}

/*
 * For use by UI code that sets preferences.
 */
unsigned int
prefs_set_custom_value(pref_t *pref, const char *value, pref_source_t source _U_)
{
    /* XXX - support pref source for custom preferences */
    unsigned int changed = 0;
    pref->custom_cbs.set_cb(pref, value, &changed);
    return changed;
}

static void
register_string_like_preference(module_t *module, const char *name,
                                const char *title, const char *description,
                                char **var, pref_type_e type,
                                struct pref_custom_cbs* custom_cbs,
                                bool free_tmp)
{
    pref_t *pref;
    char *tmp;

    pref = register_preference(module, name, title, description, type, false);

    /*
     * String preference values should be non-null (as you can't
     * keep them null after using the preferences GUI, you can at best
     * have them be null strings) and freeable (as we free them
     * if we change them).
     *
     * If the value is a null pointer, make it a copy of a null
     * string, otherwise make it a copy of the value.
     */
    tmp = *var;
    if (*var == NULL) {
        *var = g_strdup("");
    } else {
        *var = g_strdup(*var);
    }
    if (free_tmp) {
        g_free(tmp);
    }
    pref->varp.string = var;
    pref->default_val.string = g_strdup(*var);
    pref->stashed_val.string = NULL;
    if (type == PREF_CUSTOM) {
        ws_assert(custom_cbs);
        pref->custom_cbs = *custom_cbs;
    }
}

/*
 * Assign to a string preference.
 */
static void
pref_set_string_like_pref_value(pref_t *pref, const char *value)
{
DIAG_OFF(cast-qual)
    g_free((void *)*pref->varp.string);
DIAG_ON(cast-qual)
    *pref->varp.string = g_strdup(value);
}

/*
 * For use by UI code that sets preferences.
 */
unsigned int
prefs_set_string_value(pref_t *pref, const char* value, pref_source_t source)
{
    unsigned int changed = 0;

    switch (source)
    {
    case pref_default:
        if (*pref->default_val.string) {
            if (strcmp(pref->default_val.string, value) != 0) {
                changed = prefs_get_effect_flags(pref);
                g_free(pref->default_val.string);
                pref->default_val.string = g_strdup(value);
            }
        } else if (value) {
            pref->default_val.string = g_strdup(value);
        }
        break;
    case pref_stashed:
        if (pref->stashed_val.string) {
            if (strcmp(pref->stashed_val.string, value) != 0) {
                changed = prefs_get_effect_flags(pref);
                g_free(pref->stashed_val.string);
                pref->stashed_val.string = g_strdup(value);
            }
        } else if (value) {
            pref->stashed_val.string = g_strdup(value);
        }
        break;
    case pref_current:
        if (*pref->varp.string) {
            if (strcmp(*pref->varp.string, value) != 0) {
                changed = prefs_get_effect_flags(pref);
                pref_set_string_like_pref_value(pref, value);
            }
        } else if (value) {
            pref_set_string_like_pref_value(pref, value);
        }
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return changed;
}

char* prefs_get_string_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return pref->default_val.string;
    case pref_stashed:
        return pref->stashed_val.string;
    case pref_current:
        return *pref->varp.string;
    default:
        ws_assert_not_reached();
        break;
    }

    return NULL;
}

/*
 * Reset the value of a string-like preference.
 */
static void
reset_string_like_preference(pref_t *pref)
{
    g_free(*pref->varp.string);
    *pref->varp.string = g_strdup(pref->default_val.string);
}

/*
 * Register a preference with a character-string value.
 */
void
prefs_register_string_preference(module_t *module, const char *name,
                                 const char *title, const char *description,
                                 const char **var)
{
DIAG_OFF(cast-qual)
    register_string_like_preference(module, name, title, description,
                                    (char **)var, PREF_STRING, NULL, false);
DIAG_ON(cast-qual)
}

/*
 * Register a preference with a file name (string) value.
 */
void
prefs_register_filename_preference(module_t *module, const char *name,
                                   const char *title, const char *description,
                                   const char **var, bool for_writing)
{
DIAG_OFF(cast-qual)
    register_string_like_preference(module, name, title, description, (char **)var,
                                    for_writing ? PREF_SAVE_FILENAME : PREF_OPEN_FILENAME, NULL, false);
DIAG_ON(cast-qual)
}

/*
 * Register a preference with a directory name (string) value.
 */
void
prefs_register_directory_preference(module_t *module, const char *name,
                                   const char *title, const char *description,
                                   const char **var)
{
DIAG_OFF(cast-qual)
    register_string_like_preference(module, name, title, description,
                                    (char **)var, PREF_DIRNAME, NULL, false);
DIAG_ON(cast-qual)
}

/* Refactoring to handle both PREF_RANGE and PREF_DECODE_AS_RANGE */
static pref_t*
prefs_register_range_preference_common(module_t *module, const char *name,
                                const char *title, const char *description,
                                range_t **var, uint32_t max_value, pref_type_e type)
{
    pref_t *preference;

    preference = register_preference(module, name, title, description, type, false);
    preference->info.max_value = max_value;

    /*
     * Range preference values should be non-null (as you can't
     * keep them null after using the preferences GUI, you can at best
     * have them be empty ranges) and freeable (as we free them
     * if we change them).
     *
     * If the value is a null pointer, make it an empty range.
     */
    if (*var == NULL)
        *var = range_empty(wmem_epan_scope());
    preference->varp.range = var;
    preference->default_val.range = range_copy(wmem_epan_scope(), *var);
    preference->stashed_val.range = NULL;

    return preference;
}

/*
 * Register a preference with a ranged value.
 */
void
prefs_register_range_preference(module_t *module, const char *name,
                                const char *title, const char *description,
                                range_t **var, uint32_t max_value)
{
    prefs_register_range_preference_common(module, name, title,
                description, var, max_value, PREF_RANGE);
}

bool
prefs_set_range_value_work(pref_t *pref, const char *value,
                           bool return_range_errors, unsigned int *changed_flags)
{
    range_t *newrange;

    if (range_convert_str_work(wmem_epan_scope(), &newrange, value, pref->info.max_value,
                               return_range_errors) != CVT_NO_ERROR) {
        return false;        /* number was bad */
    }

    if (!ranges_are_equal(*pref->varp.range, newrange)) {
        *changed_flags |= prefs_get_effect_flags(pref);
        wmem_free(wmem_epan_scope(), *pref->varp.range);
        *pref->varp.range = newrange;
    } else {
        wmem_free(wmem_epan_scope(), newrange);
    }
    return true;
}

/*
 * For use by UI code that sets preferences.
 */
unsigned int
prefs_set_stashed_range_value(pref_t *pref, const char *value)
{
    range_t *newrange;

    if (range_convert_str_work(wmem_epan_scope(), &newrange, value, pref->info.max_value,
                               true) != CVT_NO_ERROR) {
        return 0;        /* number was bad */
    }

    if (!ranges_are_equal(pref->stashed_val.range, newrange)) {
        wmem_free(wmem_epan_scope(), pref->stashed_val.range);
        pref->stashed_val.range = newrange;
    } else {
        wmem_free(wmem_epan_scope(), newrange);
    }
    return prefs_get_effect_flags(pref);

}

bool prefs_add_list_value(pref_t *pref, void* value, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        pref->default_val.list = g_list_prepend(pref->default_val.list, value);
        break;
    case pref_stashed:
        pref->stashed_val.list = g_list_prepend(pref->stashed_val.list, value);
        break;
    case pref_current:
        *pref->varp.list = g_list_prepend(*pref->varp.list, value);
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return true;
}

GList* prefs_get_list_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return pref->default_val.list;
    case pref_stashed:
        return pref->stashed_val.list;
    case pref_current:
        return *pref->varp.list;
    default:
        ws_assert_not_reached();
        break;
    }

    return NULL;
}

bool prefs_set_range_value(pref_t *pref, range_t *value, pref_source_t source)
{
    bool changed = false;

    switch (source)
    {
    case pref_default:
        if (!ranges_are_equal(pref->default_val.range, value)) {
            wmem_free(wmem_epan_scope(), pref->default_val.range);
            pref->default_val.range = range_copy(wmem_epan_scope(), value);
            changed = true;
        }
        break;
    case pref_stashed:
        if (!ranges_are_equal(pref->stashed_val.range, value)) {
            wmem_free(wmem_epan_scope(), pref->stashed_val.range);
            pref->stashed_val.range = range_copy(wmem_epan_scope(), value);
            changed = true;
        }
        break;
    case pref_current:
        if (!ranges_are_equal(*pref->varp.range, value)) {
            wmem_free(wmem_epan_scope(), *pref->varp.range);
            *pref->varp.range = range_copy(wmem_epan_scope(), value);
            changed = true;
        }
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return changed;
}

range_t* prefs_get_range_value_real(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return pref->default_val.range;
    case pref_stashed:
        return pref->stashed_val.range;
    case pref_current:
        return *pref->varp.range;
    default:
        ws_assert_not_reached();
        break;
    }

    return NULL;
}

range_t* prefs_get_range_value(const char *module_name, const char* pref_name)
{
    pref_t *pref = prefs_find_preference(prefs_find_module(module_name), pref_name);
    if (pref == NULL) {
        return NULL;
    }
    return prefs_get_range_value_real(pref, pref_current);
}

void
prefs_range_add_value(pref_t *pref, uint32_t val)
{
    range_add_value(wmem_epan_scope(), pref->varp.range, val);
}

void
prefs_range_remove_value(pref_t *pref, uint32_t val)
{
    range_remove_value(wmem_epan_scope(), pref->varp.range, val);
}

/*
 * Register a static text 'preference'.  It can be used to add explanatory
 * text inline with other preferences in the GUI.
 * Note: Static preferences are not saved to the preferences file.
 */
void
prefs_register_static_text_preference(module_t *module, const char *name,
                                      const char *title,
                                      const char *description)
{
    register_preference(module, name, title, description, PREF_STATIC_TEXT, false);
}

/*
 * Register a uat 'preference'. It adds a button that opens the uat's window in the
 * preferences tab of the module.
 */
extern void
prefs_register_uat_preference(module_t *module, const char *name,
                              const char *title, const char *description,
                              uat_t* uat)
{
    pref_t* preference = register_preference(module, name, title, description, PREF_UAT, false);

    preference->varp.uat = uat;
}

struct epan_uat* prefs_get_uat_value(pref_t *pref)
{
    return pref->varp.uat;
}

/*
 * Register a color preference.
 */
void
prefs_register_color_preference(module_t *module, const char *name,
                                const char *title, const char *description,
                                color_t *color)
{
    pref_t* preference = register_preference(module, name, title, description, PREF_COLOR, false);

    preference->varp.colorp = color;
    preference->default_val.color = *color;
}

bool prefs_set_color_value(pref_t *pref, color_t value, pref_source_t source)
{
    bool changed = false;

    switch (source)
    {
    case pref_default:
        if ((pref->default_val.color.red != value.red) ||
            (pref->default_val.color.green != value.green) ||
            (pref->default_val.color.blue != value.blue)) {
            changed = true;
            pref->default_val.color = value;
        }
        break;
    case pref_stashed:
        if ((pref->stashed_val.color.red != value.red) ||
            (pref->stashed_val.color.green != value.green) ||
            (pref->stashed_val.color.blue != value.blue)) {
            changed = true;
            pref->stashed_val.color = value;
        }
        break;
    case pref_current:
        if ((pref->varp.colorp->red != value.red) ||
            (pref->varp.colorp->green != value.green) ||
            (pref->varp.colorp->blue != value.blue)) {
            changed = true;
            *pref->varp.colorp = value;
        }
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return changed;
}

color_t* prefs_get_color_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return &pref->default_val.color;
    case pref_stashed:
        return &pref->stashed_val.color;
    case pref_current:
        return pref->varp.colorp;
    default:
        ws_assert_not_reached();
        break;
    }

    return NULL;
}

/*
 * Register a "custom" preference with a list.
 * XXX - This should be temporary until we can find a better way
 * to do "custom" preferences
 */
typedef void (*pref_custom_list_init_cb) (pref_t* pref, GList** value);

static void
prefs_register_list_custom_preference(module_t *module, const char *name,
                                      const char *title, const char *description,
                                      struct pref_custom_cbs* custom_cbs,
                                      pref_custom_list_init_cb init_cb,
                                      GList** list)
{
    pref_t* preference = register_preference(module, name, title, description, PREF_CUSTOM, false);

    preference->custom_cbs = *custom_cbs;
    init_cb(preference, list);
}

/*
 * Register a custom preference.
 */
void
prefs_register_custom_preference(module_t *module, const char *name,
                                 const char *title, const char *description,
                                 struct pref_custom_cbs* custom_cbs,
                                 void **custom_data _U_)
{
    pref_t* preference = register_preference(module, name, title, description, PREF_CUSTOM, false);

    preference->custom_cbs = *custom_cbs;
    /* XXX - wait until we can handle void** pointers
    preference->custom_cbs.init_cb(preference, custom_data);
    */
}

/*
 * Register a dedicated TCP preference for SEQ analysis overriding.
 * This is similar to the data structure from enum preference, except
 * that when a preference dialog is used, the stashed value is the list
 * of frame data pointers whose sequence analysis override will be set
 * to the current value if the dialog is accepted.
 *
 * We don't need to read or write the value from the preferences file
 * (or command line), because the override is reset to the default (0)
 * for each frame when a new capture file is loaded.
 */
void
prefs_register_custom_preference_TCP_Analysis(module_t *module, const char *name,
                               const char *title, const char *description,
                               int *var, const enum_val_t *enumvals,
                               bool radio_buttons)
{
    pref_t *preference;

    preference = register_preference(module, name, title, description,
                                     PREF_PROTO_TCP_SNDAMB_ENUM, false);
    preference->varp.enump = var;
    preference->default_val.enumval = *var;
    preference->stashed_val.list = NULL;
    preference->info.enum_info.enumvals = enumvals;
    preference->info.enum_info.radio_buttons = radio_buttons;
}

/*
 * Register a (internal) "Decode As" preference with a ranged value.
 */
void prefs_register_decode_as_range_preference(module_t *module, const char *name,
    const char *title, const char *description, range_t **var,
    uint32_t max_value, const char *dissector_table, const char *dissector_description)
{
    pref_t *preference;

    preference = prefs_register_range_preference_common(module, name, title,
                description, var, max_value, PREF_DECODE_AS_RANGE);
    preference->dissector_desc = dissector_description;
    preference->dissector_table = dissector_table;
}

/*
 * Register a preference with password value.
 */
void
prefs_register_password_preference(module_t *module, const char *name,
                                 const char *title, const char *description,
                                 const char **var)
{
DIAG_OFF(cast-qual)
    register_string_like_preference(module, name, title, description,
                                    (char **)var, PREF_PASSWORD, NULL, false);
DIAG_ON(cast-qual)
}

/*
 * Register a preference with a dissector name.
 */
void
prefs_register_dissector_preference(module_t *module, const char *name,
                                    const char *title, const char *description,
                                    const char **var)
{
DIAG_OFF(cast-qual)
    register_string_like_preference(module, name, title, description,
                                    (char **)var, PREF_DISSECTOR, NULL, false);
DIAG_ON(cast-qual)
}

bool prefs_add_decode_as_value(pref_t *pref, unsigned value, bool replace)
{
    switch(pref->type)
    {
    case PREF_DECODE_AS_RANGE:
        if (replace)
        {
            /* If range has single value, replace it */
            if (((*pref->varp.range)->nranges == 1) &&
                ((*pref->varp.range)->ranges[0].low == (*pref->varp.range)->ranges[0].high)) {
                wmem_free(wmem_epan_scope(), *pref->varp.range);
                *pref->varp.range = range_empty(wmem_epan_scope());
            }
        }

        prefs_range_add_value(pref, value);
        break;
    default:
        /* XXX - Worth asserting over? */
        break;
    }

    return true;
}

bool prefs_remove_decode_as_value(pref_t *pref, unsigned value, bool set_default _U_)
{
    switch(pref->type)
    {
    case PREF_DECODE_AS_RANGE:
        /* XXX - We could set to the default if the value is the only one
         * in the range.
         */
        prefs_range_remove_value(pref, value);
        break;
    default:
        break;
    }

    return true;
}

/*
 * Register a preference that used to be supported but no longer is.
 */
void
prefs_register_obsolete_preference(module_t *module, const char *name)
{
    register_preference(module, name, NULL, NULL, PREF_STATIC_TEXT, true);
}

bool
prefs_is_preference_obsolete(pref_t *pref)
{
    return pref->obsolete;
}

void
prefs_set_preference_effect_fields(module_t *module, const char *name)
{
    pref_t * pref = prefs_find_preference(module, name);
    if (pref) {
        prefs_set_effect_flags(pref, prefs_get_effect_flags(pref) | PREF_EFFECT_FIELDS);
    }
}

unsigned
pref_stash(pref_t *pref, void *unused _U_)
{
    ws_assert(!pref->obsolete);

    switch (pref->type) {

    case PREF_UINT:
        pref->stashed_val.uint = *pref->varp.uint;
        break;

    case PREF_BOOL:
        pref->stashed_val.boolval = *pref->varp.boolp;
        break;

    case PREF_ENUM:
        pref->stashed_val.enumval = *pref->varp.enump;
        break;

    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        g_free(pref->stashed_val.string);
        pref->stashed_val.string = g_strdup(*pref->varp.string);
        break;

    case PREF_DECODE_AS_RANGE:
    case PREF_RANGE:
        wmem_free(wmem_epan_scope(), pref->stashed_val.range);
        pref->stashed_val.range = range_copy(wmem_epan_scope(), *pref->varp.range);
        break;

    case PREF_COLOR:
        pref->stashed_val.color = *pref->varp.colorp;
        break;

    case PREF_STATIC_TEXT:
    case PREF_UAT:
    case PREF_CUSTOM:
    case PREF_PROTO_TCP_SNDAMB_ENUM:
        break;

    default:
        ws_assert_not_reached();
        break;
    }
    return 0;
}

unsigned
pref_unstash(pref_t *pref, void *unstash_data_p)
{
    pref_unstash_data_t *unstash_data = (pref_unstash_data_t *)unstash_data_p;
    dissector_table_t sub_dissectors = NULL;
    dissector_handle_t handle = NULL;

    ws_assert(!pref->obsolete);

    /* Revert the preference to its saved value. */
    switch (pref->type) {

    case PREF_UINT:
        if (*pref->varp.uint != pref->stashed_val.uint) {
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
            *pref->varp.uint = pref->stashed_val.uint;
        }
        break;

    case PREF_BOOL:
        if (*pref->varp.boolp != pref->stashed_val.boolval) {
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
            *pref->varp.boolp = pref->stashed_val.boolval;
        }
        break;

    case PREF_ENUM:
        if (*pref->varp.enump != pref->stashed_val.enumval) {
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
            *pref->varp.enump = pref->stashed_val.enumval;
        }
        break;

    case PREF_PROTO_TCP_SNDAMB_ENUM:
    {
        /* The preference dialogs are modal so the frame_data pointers should
         * still be valid; otherwise we could store the frame numbers to
         * change.
         */
        frame_data *fdata;
        for (GList* elem = pref->stashed_val.list; elem != NULL; elem = elem->next) {
            fdata = (frame_data*)elem->data;
            if (fdata->tcp_snd_manual_analysis != *pref->varp.enump) {
                unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
                fdata->tcp_snd_manual_analysis = *pref->varp.enump;
            }
        }
        break;
    }
    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        if (strcmp(*pref->varp.string, pref->stashed_val.string) != 0) {
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
            g_free(*pref->varp.string);
            *pref->varp.string = g_strdup(pref->stashed_val.string);
        }
        break;

    case PREF_DECODE_AS_RANGE:
    {
        const char* table_name = prefs_get_dissector_table(pref);
        if (!ranges_are_equal(*pref->varp.range, pref->stashed_val.range)) {
            uint32_t i, j;
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);

            if (unstash_data->handle_decode_as) {
                sub_dissectors = find_dissector_table(table_name);
                if (sub_dissectors != NULL) {
                    const char *handle_desc = prefs_get_dissector_description(pref);
                    // It should perhaps be possible to get this via dissector name.
                    handle = dissector_table_get_dissector_handle(sub_dissectors, handle_desc);
                    if (handle != NULL) {
                        /* Set the current handle to NULL for all the old values
                         * in the dissector table. If there isn't an initial
                         * handle, this actually deletes the entry. (If there
                         * is an initial entry, keep it around so that the
                         * user can see the original value.)
                         *
                         * XXX - If there's an initial handle which is not this,
                         * reset it instead? At least this leaves the initial
                         * handle visible in the Decode As table.
                         */
                        for (i = 0; i < (*pref->varp.range)->nranges; i++) {
                            for (j = (*pref->varp.range)->ranges[i].low; j < (*pref->varp.range)->ranges[i].high; j++) {
                                dissector_change_uint(table_name, j, NULL);
                                decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(j), NULL, NULL);
                            }

                            dissector_change_uint(table_name, (*pref->varp.range)->ranges[i].high, NULL);
                            decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER((*pref->varp.range)->ranges[i].high), NULL, NULL);
                        }
                    }
                }
            }

            wmem_free(wmem_epan_scope(), *pref->varp.range);
            *pref->varp.range = range_copy(wmem_epan_scope(), pref->stashed_val.range);

            if (unstash_data->handle_decode_as) {
                if ((sub_dissectors != NULL) && (handle != NULL)) {

                    /* Add new values to the dissector table */
                    for (i = 0; i < (*pref->varp.range)->nranges; i++) {

                        for (j = (*pref->varp.range)->ranges[i].low; j < (*pref->varp.range)->ranges[i].high; j++) {
                            dissector_change_uint(table_name, j, handle);
                            decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(j), NULL, NULL);
                        }

                        dissector_change_uint(table_name, (*pref->varp.range)->ranges[i].high, handle);
                        decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER((*pref->varp.range)->ranges[i].high), NULL, NULL);
                    }
                }
            }
        }
        break;
    }
    case PREF_RANGE:
        if (!ranges_are_equal(*pref->varp.range, pref->stashed_val.range)) {
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
            wmem_free(wmem_epan_scope(), *pref->varp.range);
            *pref->varp.range = range_copy(wmem_epan_scope(), pref->stashed_val.range);
        }
    break;

    case PREF_COLOR:
        if ((pref->varp.colorp->blue != pref->stashed_val.color.blue) ||
            (pref->varp.colorp->red != pref->stashed_val.color.red) ||
            (pref->varp.colorp->green != pref->stashed_val.color.green)) {
            unstash_data->module->prefs_changed_flags |= prefs_get_effect_flags(pref);
            *pref->varp.colorp = pref->stashed_val.color;
        }
        break;

    case PREF_STATIC_TEXT:
    case PREF_UAT:
    case PREF_CUSTOM:
        break;

    default:
        ws_assert_not_reached();
        break;
    }
    return 0;
}

void
reset_stashed_pref(pref_t *pref) {

    ws_assert(!pref->obsolete);

    switch (pref->type) {

    case PREF_UINT:
        pref->stashed_val.uint = pref->default_val.uint;
        break;

    case PREF_BOOL:
        pref->stashed_val.boolval = pref->default_val.boolval;
        break;

    case PREF_ENUM:
        pref->stashed_val.enumval = pref->default_val.enumval;
        break;

    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        g_free(pref->stashed_val.string);
        pref->stashed_val.string = g_strdup(pref->default_val.string);
        break;

    case PREF_DECODE_AS_RANGE:
    case PREF_RANGE:
        wmem_free(wmem_epan_scope(), pref->stashed_val.range);
        pref->stashed_val.range = range_copy(wmem_epan_scope(), pref->default_val.range);
        break;

    case PREF_PROTO_TCP_SNDAMB_ENUM:
        if (pref->stashed_val.list != NULL) {
            g_list_free(pref->stashed_val.list);
            pref->stashed_val.list = NULL;
        }
        break;

    case PREF_COLOR:
        memcpy(&pref->stashed_val.color, &pref->default_val.color, sizeof(color_t));
        break;

    case PREF_STATIC_TEXT:
    case PREF_UAT:
    case PREF_CUSTOM:
        break;

    default:
        ws_assert_not_reached();
        break;
    }
}

unsigned
pref_clean_stash(pref_t *pref, void *unused _U_)
{
    ws_assert(!pref->obsolete);

    switch (pref->type) {

    case PREF_UINT:
        break;

    case PREF_BOOL:
        break;

    case PREF_ENUM:
        break;

    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        if (pref->stashed_val.string != NULL) {
            g_free(pref->stashed_val.string);
            pref->stashed_val.string = NULL;
        }
        break;

    case PREF_DECODE_AS_RANGE:
    case PREF_RANGE:
        if (pref->stashed_val.range != NULL) {
            wmem_free(wmem_epan_scope(), pref->stashed_val.range);
            pref->stashed_val.range = NULL;
        }
        break;

    case PREF_STATIC_TEXT:
    case PREF_UAT:
    case PREF_COLOR:
    case PREF_CUSTOM:
        break;

    case PREF_PROTO_TCP_SNDAMB_ENUM:
        if (pref->stashed_val.list != NULL) {
            g_list_free(pref->stashed_val.list);
            pref->stashed_val.list = NULL;
        }
        break;

    default:
        ws_assert_not_reached();
        break;
    }
    return 0;
}

/*
 * Call a callback function, with a specified argument, for each preference
 * in a given module.
 *
 * If any of the callbacks return a non-zero value, stop and return that
 * value, otherwise return 0.
 */
unsigned
prefs_pref_foreach(module_t *module, pref_cb callback, void *user_data)
{
    GList *elem;
    pref_t *pref;
    unsigned ret;

    for (elem = g_list_first(module->prefs); elem != NULL; elem = g_list_next(elem)) {
        pref = (pref_t *)elem->data;
        if (!pref || pref->obsolete) {
            /*
             * This preference is no longer supported; it's
             * not a real preference, so we don't call the
             * callback for it (i.e., we treat it as if it
             * weren't found in the list of preferences,
             * and we weren't called in the first place).
             */
            continue;
        }

        ret = (*callback)(pref, user_data);
        if (ret != 0)
            return ret;
    }
    return 0;
}

static const enum_val_t st_sort_col_vals[] = {
    { "name",    "Node name (topic/item)", ST_SORT_COL_NAME },
    { "count",   "Item count", ST_SORT_COL_COUNT },
    { "average", "Average value of the node", ST_SORT_COL_AVG },
    { "min",     "Minimum value of the node", ST_SORT_COL_MIN },
    { "max",     "Maximum value of the node", ST_SORT_COL_MAX },
    { "burst",   "Burst rate of the node", ST_SORT_COL_BURSTRATE },
    { NULL,      NULL,         0 }
};

static const enum_val_t st_format_vals[] = {
    { "text",  "Plain text",             ST_FORMAT_PLAIN },
    { "csv",   "Comma separated values", ST_FORMAT_CSV   },
    { "xml",   "XML document",           ST_FORMAT_XML   },
    { "yaml",  "YAML document",          ST_FORMAT_YAML  },
    { NULL,    NULL,                     0 }
};

static void
stats_callback(void)
{
    /* Test for a sane tap update interval */
    if (prefs.tap_update_interval < 100 || prefs.tap_update_interval > 10000)
        prefs.tap_update_interval = TAP_UPDATE_DEFAULT_INTERVAL;

    /* burst resolution can't be less than 1 (ms) */
    if (prefs.st_burst_resolution < 1) {
        prefs.st_burst_resolution = 1;
    }
    else if (prefs.st_burst_resolution > ST_MAX_BURSTRES) {
        prefs.st_burst_resolution = ST_MAX_BURSTRES;
    }
    /* make sure burst window value makes sense */
    if (prefs.st_burst_windowlen < prefs.st_burst_resolution) {
        prefs.st_burst_windowlen = prefs.st_burst_resolution;
    }
    /* round burst window down to multiple of resolution */
    prefs.st_burst_windowlen -= prefs.st_burst_windowlen%prefs.st_burst_resolution;
    if ((prefs.st_burst_windowlen/prefs.st_burst_resolution) > ST_MAX_BURSTBUCKETS) {
        prefs.st_burst_windowlen = prefs.st_burst_resolution*ST_MAX_BURSTBUCKETS;
    }
}

static void
gui_callback(void)
{
    /* Ensure there is at least one file count */
    if (prefs.gui_recent_files_count_max == 0)
      prefs.gui_recent_files_count_max = 10;

    /* Ensure there is at least one display filter entry */
    if (prefs.gui_recent_df_entries_max == 0)
      prefs.gui_recent_df_entries_max = 10;

    /* number of decimal places should be between 2 and 10 */
    if (prefs.gui_decimal_places1 < 2) {
        prefs.gui_decimal_places1 = 2;
    } else if (prefs.gui_decimal_places1 > 10) {
        prefs.gui_decimal_places1 = 10;
    }
    /* number of decimal places should be between 2 and 10 */
    if (prefs.gui_decimal_places2 < 2) {
        prefs.gui_decimal_places2 = 2;
    } else if (prefs.gui_decimal_places2 > 10) {
        prefs.gui_decimal_places2 = 10;
    }
    /* number of decimal places should be between 2 and 10 */
    if (prefs.gui_decimal_places3 < 2) {
        prefs.gui_decimal_places3 = 2;
    } else if (prefs.gui_decimal_places3 > 10) {
        prefs.gui_decimal_places3 = 10;
    }
}

static void
gui_layout_callback(void)
{
    if (prefs.gui_layout_type == layout_unused ||
        prefs.gui_layout_type >= layout_type_max) {
      /* XXX - report an error?  It's not a syntax error - we'd need to
         add a way of reporting a *semantic* error. */
      prefs.gui_layout_type = layout_type_2;
    }
}

/******************************************************
 * All custom preference function callbacks
 ******************************************************/
static void custom_pref_no_cb(pref_t* pref _U_) {}

/*
 * Column preference functions
 */
#define PRS_COL_HIDDEN_FMT               "column.hidden"
#define PRS_COL_HIDDEN                   "column.hide"
#define PRS_COL_FMT                      "column.format"
#define PRS_COL_NUM                      "column.number"
static module_t *gui_column_module;

static prefs_set_pref_e
column_hidden_set_cb(pref_t* pref, const char* value, unsigned int* changed_flags)
{
    GList       *clp;
    fmt_data    *cfmt;
    pref_t  *format_pref;

    /*
     * Prefer the new preference to the old format-based preference if we've
     * read it. (We probably could just compare the string to NULL and "".)
     */
    prefs.cols_hide_new = true;

    (*changed_flags) |= prefs_set_string_value(pref, value, pref_current);

    /*
     * Set the "visible" flag for the existing columns; we need to
     * do this if we set PRS_COL_HIDDEN but don't set PRS_COL_FMT
     * after setting it (which might be the case if, for example, we
     * set PRS_COL_HIDDEN on the command line).
     */
    format_pref = prefs_find_preference(gui_column_module, PRS_COL_FMT);
    clp = (format_pref) ? *format_pref->varp.list : NULL;
    int cidx = 1;
    while (clp) {
      cfmt = (fmt_data *)clp->data;
      cfmt->visible = prefs_is_column_visible(*pref->varp.string, cidx);
      cidx++;
      clp = clp->next;
    }

    return PREFS_SET_OK;
}

static const char *
column_hidden_type_name_cb(void)
{
    return "Packet list hidden columns";
}

static char *
column_hidden_type_description_cb(void)
{
    return g_strdup("List all column indices (1-indexed) to hide in the packet list.");
}

static char *
column_hidden_to_str_cb(pref_t* pref, bool default_val)
{
    GString     *cols_hidden;
    GList       *clp;
    fmt_data    *cfmt;
    pref_t  *format_pref;
    int          cidx = 1;

    if (default_val)
        return g_strdup(pref->default_val.string);

    cols_hidden = g_string_new("");
    format_pref = prefs_find_preference(gui_column_module, PRS_COL_FMT);
    clp = (format_pref) ? *format_pref->varp.list : NULL;
    while (clp) {
        cfmt = (fmt_data *) clp->data;
        if (!cfmt->visible) {
            if (cols_hidden->len)
                g_string_append (cols_hidden, ",");
            g_string_append_printf (cols_hidden, "%i", cidx);
        }
        clp = clp->next;
        cidx++;
    }

    return g_string_free (cols_hidden, FALSE);
}

static bool
column_hidden_is_default_cb(pref_t* pref)
{
    char *cur_hidden_str = column_hidden_to_str_cb(pref, false);
    bool is_default = g_strcmp0(cur_hidden_str, pref->default_val.string) == 0;

    g_free(cur_hidden_str);
    return is_default;
}

static prefs_set_pref_e
column_hidden_fmt_set_cb(pref_t* pref, const char* value, unsigned int* changed_flags)
{
    GList       *clp;
    fmt_data    *cfmt;
    pref_t  *format_pref;

    (*changed_flags) |= prefs_set_string_value(pref, value, pref_current);

    /*
     * Set the "visible" flag for the existing columns; we need to
     * do this if we set PRS_COL_HIDDEN_FMT but don't set PRS_COL_FMT
     * after setting it (which might be the case if, for example, we
     * set PRS_COL_HIDDEN_FMT on the command line; it shouldn't happen
     * when reading the configuration file because we write (both of)
     * the hidden column prefs before the column format prefs.)
     */
    format_pref = prefs_find_preference(gui_column_module, PRS_COL_FMT);
    clp = (format_pref) ? *format_pref->varp.list : NULL;
    while (clp) {
      cfmt = (fmt_data *)clp->data;
      cfmt->visible = prefs_is_column_fmt_visible(*pref->varp.string, cfmt);
      clp = clp->next;
    }

    return PREFS_SET_OK;
}

static const char *
column_hidden_fmt_type_name_cb(void)
{
    return "Packet list hidden column formats (deprecated)";
}

static char *
column_hidden_fmt_type_description_cb(void)
{
    return g_strdup("List all column formats to hide in the packet list. Deprecated in favor of the index-based preference.");
}

static char *
column_hidden_fmt_to_str_cb(pref_t* pref, bool default_val)
{
    GString     *cols_hidden;
    GList       *clp;
    fmt_data    *cfmt;
    pref_t  *format_pref;

    if (default_val)
        return g_strdup(pref->default_val.string);

    cols_hidden = g_string_new("");
    format_pref = prefs_find_preference(gui_column_module, PRS_COL_FMT);
    clp = (format_pref) ? *format_pref->varp.list : NULL;
    while (clp) {
        char *prefs_fmt;
        cfmt = (fmt_data *) clp->data;
        if (!cfmt->visible) {
            if (cols_hidden->len)
                g_string_append (cols_hidden, ",");
            prefs_fmt = column_fmt_data_to_str(cfmt);
            g_string_append(cols_hidden, prefs_fmt);
            g_free(prefs_fmt);
        }
        clp = clp->next;
    }

    return g_string_free (cols_hidden, FALSE);
}

static bool
column_hidden_fmt_is_default_cb(pref_t* pref)
{
    char *cur_hidden_str = column_hidden_fmt_to_str_cb(pref, false);
    bool is_default = g_strcmp0(cur_hidden_str, pref->default_val.string) == 0;

    g_free(cur_hidden_str);
    return is_default;
}

/* Number of columns "preference".  This is only used internally and is not written to the
 * preference file
 */
static void
column_num_reset_cb(pref_t* pref)
{
    *pref->varp.uint = pref->default_val.uint;
}

static prefs_set_pref_e
column_num_set_cb(pref_t* pref _U_, const char* value _U_, unsigned int* changed_flags _U_)
{
    /* Don't write this to the preferences file */
    return PREFS_SET_OK;
}

static const char *
column_num_type_name_cb(void)
{
    return NULL;
}

static char *
column_num_type_description_cb(void)
{
    return g_strdup("");
}

static bool
column_num_is_default_cb(pref_t* pref _U_)
{
    return true;
}

static char *
column_num_to_str_cb(pref_t* pref _U_, bool default_val _U_)
{
    return g_strdup("");
}

/*
 * Column format custom preference functions
 */
static void
column_format_init_cb(pref_t* pref, GList** value)
{
    fmt_data *src_cfmt, *dest_cfmt;
    GList *entry;

    pref->varp.list = value;

    pref->default_val.list = NULL;
    for (entry = *pref->varp.list; entry != NULL; entry = g_list_next(entry)) {
        src_cfmt = (fmt_data *)entry->data;
        dest_cfmt = g_new(fmt_data,1);
        dest_cfmt->title = g_strdup(src_cfmt->title);
        dest_cfmt->fmt = src_cfmt->fmt;
        if (src_cfmt->custom_fields) {
            dest_cfmt->custom_fields = g_strdup(src_cfmt->custom_fields);
            dest_cfmt->custom_occurrence = src_cfmt->custom_occurrence;
        } else {
            dest_cfmt->custom_fields = NULL;
            dest_cfmt->custom_occurrence = 0;
        }
        dest_cfmt->visible = src_cfmt->visible;
        dest_cfmt->display = src_cfmt->display;
        pref->default_val.list = g_list_append(pref->default_val.list, dest_cfmt);
    }

    column_register_fields();
}

static void
column_format_free_cb(pref_t* pref)
{
    free_col_info(*pref->varp.list);
    free_col_info(pref->default_val.list);
}

static void
column_format_reset_cb(pref_t* pref)
{
    fmt_data *src_cfmt, *dest_cfmt;
    GList *entry;
    pref_t  *col_num_pref;

    free_col_info(*pref->varp.list);
    *pref->varp.list = NULL;

    for (entry = pref->default_val.list; entry != NULL; entry = g_list_next(entry)) {
        src_cfmt = (fmt_data *)entry->data;
        dest_cfmt = g_new(fmt_data,1);
        dest_cfmt->title = g_strdup(src_cfmt->title);
        dest_cfmt->fmt = src_cfmt->fmt;
        if (src_cfmt->custom_fields) {
            dest_cfmt->custom_fields = g_strdup(src_cfmt->custom_fields);
            dest_cfmt->custom_occurrence = src_cfmt->custom_occurrence;
        } else {
            dest_cfmt->custom_fields = NULL;
            dest_cfmt->custom_occurrence = 0;
        }
        dest_cfmt->visible = src_cfmt->visible;
        dest_cfmt->display = src_cfmt->display;
        *pref->varp.list = g_list_append(*pref->varp.list, dest_cfmt);
    }

    col_num_pref = prefs_find_preference(gui_column_module, PRS_COL_NUM);
    ws_assert(col_num_pref != NULL); /* Should never happen */
    column_num_reset_cb(col_num_pref);
}

static prefs_set_pref_e
column_format_set_cb(pref_t* pref, const char* value, unsigned int* changed_flags _U_)
{
    GList    *col_l, *col_l_elt;
    fmt_data *cfmt;
    int      llen;
    pref_t   *hidden_pref, *col_num_pref;

    col_l = prefs_get_string_list(value);
    if (col_l == NULL)
      return PREFS_SET_SYNTAX_ERR;
    if ((g_list_length(col_l) % 2) != 0) {
      /* A title didn't have a matching format.  */
      prefs_clear_string_list(col_l);
      return PREFS_SET_SYNTAX_ERR;
    }
    /* Check to make sure all column formats are valid.  */
    col_l_elt = g_list_first(col_l);
    while (col_l_elt) {
      fmt_data cfmt_check;

      /* Go past the title.  */
      col_l_elt = col_l_elt->next;

      /* Some predefined columns have been migrated to use custom columns.
       * We'll convert these silently here */
      try_convert_to_custom_column((char **)&col_l_elt->data);

      /* Parse the format to see if it's valid.  */
      if (!parse_column_format(&cfmt_check, (char *)col_l_elt->data)) {
        /* It's not a valid column format.  */
        prefs_clear_string_list(col_l);
        return PREFS_SET_SYNTAX_ERR;
      }
      if (cfmt_check.fmt == COL_CUSTOM) {
        /* We don't need the custom column field on this pass. */
        g_free(cfmt_check.custom_fields);
      }

      /* Go past the format.  */
      col_l_elt = col_l_elt->next;
    }

    /* They're all valid; process them. */
    free_col_info(*pref->varp.list);
    *pref->varp.list = NULL;
    if (prefs.cols_hide_new) {
      hidden_pref = prefs_find_preference(gui_column_module, PRS_COL_HIDDEN);
    } else {
      hidden_pref = prefs_find_preference(gui_column_module, PRS_COL_HIDDEN_FMT);
    }
    ws_assert(hidden_pref != NULL); /* Should never happen */
    col_num_pref = prefs_find_preference(gui_column_module, PRS_COL_NUM);
    ws_assert(col_num_pref != NULL); /* Should never happen */
    llen             = g_list_length(col_l);
    *col_num_pref->varp.uint = llen / 2;
    col_l_elt = g_list_first(col_l);
    int cidx = 1;
    while (col_l_elt) {
      cfmt           = g_new(fmt_data,1);
      cfmt->title    = g_strdup((char *)col_l_elt->data);
      col_l_elt      = col_l_elt->next;
      parse_column_format(cfmt, (char *)col_l_elt->data);
      if (prefs.cols_hide_new) {
        cfmt->visible   = prefs_is_column_visible(*hidden_pref->varp.string, cidx);
      } else {
        cfmt->visible   = prefs_is_column_fmt_visible(*hidden_pref->varp.string, cfmt);
      }
      col_l_elt      = col_l_elt->next;
      *pref->varp.list = g_list_append(*pref->varp.list, cfmt);
      cidx++;
    }

    prefs_clear_string_list(col_l);
    free_string_like_preference(hidden_pref);
    column_register_fields();
    return PREFS_SET_OK;
}


static const char *
column_format_type_name_cb(void)
{
    return "Packet list column format";
}

static char *
column_format_type_description_cb(void)
{
    return g_strdup("Each pair of strings consists of a column title and its format");
}

static bool
column_format_is_default_cb(pref_t* pref)
{
    GList       *clp = *pref->varp.list,
                *pref_col = g_list_first(clp),
                *def_col = g_list_first(pref->default_val.list);
    fmt_data    *cfmt, *def_cfmt;
    bool        is_default = true;
    pref_t      *col_num_pref;

    /* See if the column data has changed from the default */
    col_num_pref = prefs_find_preference(gui_column_module, PRS_COL_NUM);
    if (col_num_pref && *col_num_pref->varp.uint != col_num_pref->default_val.uint) {
        is_default = false;
    } else {
        while (pref_col && def_col) {
            cfmt = (fmt_data *) pref_col->data;
            def_cfmt = (fmt_data *) def_col->data;
            if ((g_strcmp0(cfmt->title, def_cfmt->title) != 0) ||
                    (cfmt->fmt != def_cfmt->fmt) ||
                    (((cfmt->fmt == COL_CUSTOM) && (cfmt->custom_fields)) &&
                     ((g_strcmp0(cfmt->custom_fields, def_cfmt->custom_fields) != 0) ||
                      (cfmt->display != def_cfmt->display)))) {
                is_default = false;
                break;
            }

            pref_col = pref_col->next;
            def_col = def_col->next;
        }
    }

    return is_default;
}

static char *
column_format_to_str_cb(pref_t* pref, bool default_val)
{
    GList       *pref_l = default_val ? pref->default_val.list : *pref->varp.list;
    GList       *clp = g_list_first(pref_l);
    GList       *col_l;
    fmt_data    *cfmt;
    char        *column_format_str;

    col_l = NULL;
    while (clp) {
        cfmt = (fmt_data *) clp->data;
        col_l = g_list_append(col_l, g_strdup(cfmt->title));
        col_l = g_list_append(col_l, column_fmt_data_to_str(cfmt));
        clp = clp->next;
    }

    column_format_str = join_string_list(col_l);
    prefs_clear_string_list(col_l);
    return column_format_str;
}


/******  Capture column custom preference functions  ******/

/* This routine is only called when Wireshark is started, NOT when another profile is selected.
   Copy the pref->capture_columns list (just loaded with the capture_cols[] struct values)
   to prefs->default_val.list.
*/
static void
capture_column_init_cb(pref_t* pref, GList** capture_cols_values)
{
    GList   *ccv_list = *capture_cols_values,
            *dlist = NULL;

    /*  */
    while (ccv_list) {
        dlist = g_list_append(dlist, g_strdup((char *)ccv_list->data));
        ccv_list = ccv_list->next;
    }

    pref->default_val.list = dlist;
    pref->varp.list = &prefs.capture_columns;
    pref->stashed_val.boolval = false;
}

/* Free the prefs->capture_columns list strings and remove the list entries.
   Note that since pref->varp.list points to &prefs.capture_columns, it is
   also freed.
*/
static void
capture_column_free_cb(pref_t* pref)
{
    prefs_clear_string_list(prefs.capture_columns);
    prefs.capture_columns = NULL;

    if (pref->stashed_val.boolval == true) {
      prefs_clear_string_list(pref->default_val.list);
      pref->default_val.list = NULL;
    }
}

/* Copy pref->default_val.list to *pref->varp.list.
*/
static void
capture_column_reset_cb(pref_t* pref)
{
    GList *vlist = NULL, *dlist;

    /* Free the column name strings and remove the links from *pref->varp.list */
    prefs_clear_string_list(*pref->varp.list);

    for (dlist = pref->default_val.list; dlist != NULL; dlist = g_list_next(dlist)) {
      vlist = g_list_append(vlist, g_strdup((char *)dlist->data));
    }
    *pref->varp.list = vlist;
}

static prefs_set_pref_e
capture_column_set_cb(pref_t* pref, const char* value, unsigned int* changed_flags _U_)
{
    GList *col_l  = prefs_get_string_list(value);
    GList *col_l_elt;
    char *col_name;
    int i;

    if (col_l == NULL)
      return PREFS_SET_SYNTAX_ERR;

    capture_column_free_cb(pref);

    /* If value (the list of capture.columns read from preferences) is empty, set capture.columns
       to the full list of valid capture column names. */
    col_l_elt = g_list_first(col_l);
    if (!(*(char *)col_l_elt->data)) {
        for (i = 0; i < num_capture_cols; i++) {
          col_name = g_strdup(capture_cols[i]);
          prefs.capture_columns = g_list_append(prefs.capture_columns, col_name);
        }
    }

    /* Verify that all the column names are valid. If not, use the entire list of valid columns.
     */
    while (col_l_elt) {
      bool found_match = false;
      col_name = (char *)col_l_elt->data;

      for (i = 0; i < num_capture_cols; i++) {
        if (strcmp(col_name, capture_cols[i])==0) {
          found_match = true;
          break;
        }
      }
      if (!found_match) {
        /* One or more cols are invalid so use the entire list of valid cols. */
        for (i = 0; i < num_capture_cols; i++) {
          col_name = g_strdup(capture_cols[i]);
          prefs.capture_columns = g_list_append(prefs.capture_columns, col_name);
        }
        pref->varp.list = &prefs.capture_columns;
        prefs_clear_string_list(col_l);
        return PREFS_SET_SYNTAX_ERR;
      }
      col_l_elt = col_l_elt->next;
    }

    col_l_elt = g_list_first(col_l);
    while (col_l_elt) {
      col_name = (char *)col_l_elt->data;
      prefs.capture_columns = g_list_append(prefs.capture_columns, col_name);
      col_l_elt = col_l_elt->next;
    }
    pref->varp.list = &prefs.capture_columns;
    g_list_free(col_l);
    return PREFS_SET_OK;
}


static const char *
capture_column_type_name_cb(void)
{
    return "Column list";
}

static char *
capture_column_type_description_cb(void)
{
    return g_strdup(
        "List of columns to be displayed in the capture options dialog.\n"
        CAPTURE_COL_TYPE_DESCRIPTION);
}

static bool
capture_column_is_default_cb(pref_t* pref)
{
    GList   *pref_col = g_list_first(prefs.capture_columns),
            *def_col = g_list_first(pref->default_val.list);
    bool is_default = true;

    /* See if the column data has changed from the default */
    while (pref_col && def_col) {
        if (strcmp((char *)pref_col->data, (char *)def_col->data) != 0) {
            is_default = false;
            break;
        }
        pref_col = pref_col->next;
        def_col = def_col->next;
    }

    /* Ensure the same column count */
    if (((pref_col == NULL) && (def_col != NULL)) ||
        ((pref_col != NULL) && (def_col == NULL)))
        is_default = false;

    return is_default;
}

static char *
capture_column_to_str_cb(pref_t* pref, bool default_val)
{

    GList       *pref_l = default_val ? pref->default_val.list : prefs.capture_columns;
    GList       *clp = g_list_first(pref_l);
    GList       *col_l = NULL;
    char        *col;
    char        *capture_column_str;

    while (clp) {
        col = (char *) clp->data;
        col_l = g_list_append(col_l, g_strdup(col));
        clp = clp->next;
    }

    capture_column_str = join_string_list(col_l);
    prefs_clear_string_list(col_l);
    return capture_column_str;
}

static prefs_set_pref_e
colorized_frame_set_cb(pref_t* pref, const char* value, unsigned int* changed_flags)
{
    (*changed_flags) |= prefs_set_string_value(pref, value, pref_current);
    return PREFS_SET_OK;
}

static const char *
colorized_frame_type_name_cb(void)
{
   /* Don't write the colors of the 10 easy-access-colorfilters to the preferences
    * file until the colors can be changed in the GUI. Currently this is not really
    * possible since the STOCK-icons for these colors are hardcoded.
    *
    * XXX Find a way to change the colors of the STOCK-icons on the fly and then
    *     add these 10 colors to the list of colors that can be changed through
    *     the preferences.
    *
    */
    return NULL;
}

static char *
colorized_frame_type_description_cb(void)
{
    return g_strdup("");
}

static bool
colorized_frame_is_default_cb(pref_t* pref _U_)
{
    return true;
}

static char *
colorized_frame_to_str_cb(pref_t* pref _U_, bool default_val _U_)
{
    return g_strdup("");
}

/*
 * Register all non-dissector modules' preferences.
 */
static module_t *gui_module;
static module_t *gui_color_module;
static module_t *nameres_module;

static void
prefs_register_modules(void)
{
    module_t *printing, *capture_module, *console_module,
        *gui_layout_module, *gui_font_module;
    module_t *extcap_module;
    unsigned int layout_gui_flags;
    struct pref_custom_cbs custom_cbs;

    if (protocols_module != NULL) {
        /* Already setup preferences */
        return;
    }

    /* GUI
     * These are "simple" GUI preferences that can be read/written using the
     * preference module API.  These preferences still use their own
     * configuration screens for access, but this cuts down on the
     * preference "string compare list" in set_pref()
     */
    extcap_module = prefs_register_module(NULL, "extcap", "Extcap Utilities",
        "Extcap Utilities", NULL, NULL, false);

    /* Setting default value to true */
    prefs.extcap_save_on_start = true;
    prefs_register_bool_preference(extcap_module, "gui_save_on_start",
                                   "Save arguments on start of capture",
                                   "Save arguments on start of capture",
                                   &prefs.extcap_save_on_start);

    /* GUI
     * These are "simple" GUI preferences that can be read/written using the
     * preference module API.  These preferences still use their own
     * configuration screens for access, but this cuts down on the
     * preference "string compare list" in set_pref()
     */
    gui_module = prefs_register_module(NULL, "gui", "User Interface",
        "User Interface", NULL, &gui_callback, false);
    /*
     * The GUI preferences don't affect dissection in general.
     * Any changes are signaled in other ways, so PREF_EFFECT_GUI doesn't
     * explicitly do anything, but wslua_set_preference expects *some*
     * effect flag to be set if the preference was changed.
     * We have to do this again for all the submodules (except for the
     * layout submodule, which has its own effect flag).
     */
    unsigned gui_effect_flags = prefs_get_module_effect_flags(gui_module);
    gui_effect_flags |= PREF_EFFECT_GUI;
    gui_effect_flags &= (~PREF_EFFECT_DISSECTION);
    prefs_set_module_effect_flags(gui_module, gui_effect_flags);

    /*
     * gui.console_open is stored in the registry in addition to the
     * preferences file. It is also read independently by ws_log_init()
     * for early log initialization of the console.
     */
    prefs_register_enum_preference(gui_module, "console_open",
                       "Open a console window",
                       "Open a console window (Windows only)",
                       (int *)&ws_log_console_open, gui_console_open_type, false);

    prefs_register_obsolete_preference(gui_module, "scrollbar_on_right");
    prefs_register_obsolete_preference(gui_module, "packet_list_sel_browse");
    prefs_register_obsolete_preference(gui_module, "protocol_tree_sel_browse");
    prefs_register_obsolete_preference(gui_module, "tree_view_altern_colors");
    prefs_register_obsolete_preference(gui_module, "expert_composite_eyecandy");
    prefs_register_obsolete_preference(gui_module, "filter_toolbar_show_in_statusbar");

    prefs_register_bool_preference(gui_module, "restore_filter_after_following_stream",
                                   "Restore current display filter after following a stream",
                                   "Restore current display filter after following a stream?",
                                   &prefs.restore_filter_after_following_stream);

    prefs_register_obsolete_preference(gui_module, "protocol_tree_line_style");

    prefs_register_obsolete_preference(gui_module, "protocol_tree_expander_style");

    prefs_register_obsolete_preference(gui_module, "hex_dump_highlight_style");

    prefs_register_obsolete_preference(gui_module, "packet_editor.enabled");

    gui_column_module = prefs_register_subtree(gui_module, "Columns", "Columns", NULL);
    prefs_set_module_effect_flags(gui_column_module, gui_effect_flags);
    /* For reading older preference files with "column." preferences */
    prefs_register_module_alias("column", gui_column_module);


    custom_cbs.free_cb = free_string_like_preference;
    custom_cbs.reset_cb = reset_string_like_preference;
    custom_cbs.set_cb = column_hidden_set_cb;
    custom_cbs.type_name_cb = column_hidden_type_name_cb;
    custom_cbs.type_description_cb = column_hidden_type_description_cb;
    custom_cbs.is_default_cb = column_hidden_is_default_cb;
    custom_cbs.to_str_cb = column_hidden_to_str_cb;
    register_string_like_preference(gui_column_module, PRS_COL_HIDDEN, "Packet list hidden columns",
        "List all column indices (1-indexed) to hide in the packet list",
        &cols_hidden_list, PREF_CUSTOM, &custom_cbs, false);

    custom_cbs.set_cb = column_hidden_fmt_set_cb;
    custom_cbs.type_name_cb = column_hidden_fmt_type_name_cb;
    custom_cbs.type_description_cb = column_hidden_fmt_type_description_cb;
    custom_cbs.is_default_cb = column_hidden_fmt_is_default_cb;
    custom_cbs.to_str_cb = column_hidden_fmt_to_str_cb;

    register_string_like_preference(gui_column_module, PRS_COL_HIDDEN_FMT, "Packet list hidden column formats (deprecated)",
        "List all column formats to hide in the packet list; deprecated in favor of the index-based preference",
        &cols_hidden_fmt_list, PREF_CUSTOM, &custom_cbs, false);

    custom_cbs.free_cb = column_format_free_cb;
    custom_cbs.reset_cb = column_format_reset_cb;
    custom_cbs.set_cb = column_format_set_cb;
    custom_cbs.type_name_cb = column_format_type_name_cb;
    custom_cbs.type_description_cb = column_format_type_description_cb;
    custom_cbs.is_default_cb = column_format_is_default_cb;
    custom_cbs.to_str_cb = column_format_to_str_cb;

    prefs_register_list_custom_preference(gui_column_module, PRS_COL_FMT, "Packet list column format",
        "Each pair of strings consists of a column title and its format", &custom_cbs,
        column_format_init_cb, &prefs.col_list);

    /* Number of columns.  This is only used internally and is not written to the
     * preference file
     */
    custom_cbs.free_cb = custom_pref_no_cb;
    custom_cbs.reset_cb = column_num_reset_cb;
    custom_cbs.set_cb = column_num_set_cb;
    custom_cbs.type_name_cb = column_num_type_name_cb;
    custom_cbs.type_description_cb = column_num_type_description_cb;
    custom_cbs.is_default_cb = column_num_is_default_cb;
    custom_cbs.to_str_cb = column_num_to_str_cb;
    prefs_register_uint_custom_preference(gui_column_module, PRS_COL_NUM, "Number of columns",
        "Number of columns in col_list", &custom_cbs, &prefs.num_cols);

    /* User Interface : Font */
    gui_font_module = prefs_register_subtree(gui_module, "Font", "Font", NULL);
    prefs_set_module_effect_flags(gui_font_module, gui_effect_flags);

    prefs_register_obsolete_preference(gui_font_module, "font_name");

    prefs_register_obsolete_preference(gui_font_module, "gtk2.font_name");

    register_string_like_preference(gui_font_module, "qt.font_name", "Font name",
        "Font name for packet list, protocol tree, and hex dump panes. (Qt)",
        &prefs.gui_font_name, PREF_STRING, NULL, true);

    /* User Interface : Colors */
    gui_color_module = prefs_register_subtree(gui_module, "Colors", "Colors", NULL);
    unsigned gui_color_effect_flags = gui_effect_flags | PREF_EFFECT_GUI_COLOR;
    prefs_set_module_effect_flags(gui_color_module, gui_color_effect_flags);

    prefs_register_enum_preference(gui_color_module, "color_scheme", "Color scheme", "Color scheme",
        &prefs.gui_color_scheme, gui_color_scheme, false);

    prefs_register_color_preference(gui_color_module, "active_frame.fg", "Foreground color for an active selected item",
        "Foreground color for an active selected item", &prefs.gui_active_fg);

    prefs_register_color_preference(gui_color_module, "active_frame.bg", "Background color for an active selected item",
        "Background color for an active selected item", &prefs.gui_active_bg);

    prefs_register_enum_preference(gui_color_module, "active_frame.style", "Color style for an active selected item",
        "Color style for an active selected item", &prefs.gui_active_style, gui_selection_style, false);

    prefs_register_color_preference(gui_color_module, "inactive_frame.fg", "Foreground color for an inactive selected item",
        "Foreground color for an inactive selected item", &prefs.gui_inactive_fg);

    prefs_register_color_preference(gui_color_module, "inactive_frame.bg", "Background color for an inactive selected item",
        "Background color for an inactive selected item", &prefs.gui_inactive_bg);

    prefs_register_enum_preference(gui_color_module, "inactive_frame.style", "Color style for an inactive selected item",
        "Color style for an inactive selected item", &prefs.gui_inactive_style, gui_selection_style, false);

    prefs_register_color_preference(gui_color_module, "marked_frame.fg", "Color preferences for a marked frame",
        "Color preferences for a marked frame", &prefs.gui_marked_fg);

    prefs_register_color_preference(gui_color_module, "marked_frame.bg", "Color preferences for a marked frame",
        "Color preferences for a marked frame", &prefs.gui_marked_bg);

    prefs_register_color_preference(gui_color_module, "ignored_frame.fg", "Color preferences for a ignored frame",
        "Color preferences for a ignored frame", &prefs.gui_ignored_fg);

    prefs_register_color_preference(gui_color_module, "ignored_frame.bg", "Color preferences for a ignored frame",
        "Color preferences for a ignored frame", &prefs.gui_ignored_bg);

    prefs_register_color_preference(gui_color_module, "stream.client.fg", "TCP stream window color preference",
        "TCP stream window color preference", &prefs.st_client_fg);

    prefs_register_color_preference(gui_color_module, "stream.client.bg", "TCP stream window color preference",
        "TCP stream window color preference", &prefs.st_client_bg);

    prefs_register_color_preference(gui_color_module, "stream.server.fg", "TCP stream window color preference",
        "TCP stream window color preference", &prefs.st_server_fg);

    prefs_register_color_preference(gui_color_module, "stream.server.bg", "TCP stream window color preference",
        "TCP stream window color preference", &prefs.st_server_bg);

    custom_cbs.free_cb = free_string_like_preference;
    custom_cbs.reset_cb = reset_string_like_preference;
    custom_cbs.set_cb = colorized_frame_set_cb;
    custom_cbs.type_name_cb = colorized_frame_type_name_cb;
    custom_cbs.type_description_cb = colorized_frame_type_description_cb;
    custom_cbs.is_default_cb = colorized_frame_is_default_cb;
    custom_cbs.to_str_cb = colorized_frame_to_str_cb;
    register_string_like_preference(gui_column_module, "colorized_frame.fg", "Colorized Foreground",
        "Filter Colorized Foreground",
        &prefs.gui_colorized_fg, PREF_CUSTOM, &custom_cbs, true);

    custom_cbs.free_cb = free_string_like_preference;
    custom_cbs.reset_cb = reset_string_like_preference;
    custom_cbs.set_cb = colorized_frame_set_cb;
    custom_cbs.type_name_cb = colorized_frame_type_name_cb;
    custom_cbs.type_description_cb = colorized_frame_type_description_cb;
    custom_cbs.is_default_cb = colorized_frame_is_default_cb;
    custom_cbs.to_str_cb = colorized_frame_to_str_cb;
    register_string_like_preference(gui_column_module, "colorized_frame.bg", "Colorized Background",
        "Filter Colorized Background",
        &prefs.gui_colorized_bg, PREF_CUSTOM, &custom_cbs, true);

    prefs_register_color_preference(gui_color_module, "color_filter_fg.valid", "Valid color filter foreground",
        "Valid color filter foreground", &prefs.gui_filter_valid_fg);
    prefs_register_color_preference(gui_color_module, "color_filter_bg.valid", "Valid color filter background",
        "Valid color filter background", &prefs.gui_filter_valid_bg);

    prefs_register_color_preference(gui_color_module, "color_filter_fg.invalid", "Invalid color filter foreground",
        "Invalid color filter foreground", &prefs.gui_filter_invalid_fg);
    prefs_register_color_preference(gui_color_module, "color_filter_bg.invalid", "Invalid color filter background",
        "Invalid color filter background", &prefs.gui_filter_invalid_bg);

    prefs_register_color_preference(gui_color_module, "color_filter_fg.deprecated", "Deprecated color filter foreground",
        "Deprecated color filter foreground", &prefs.gui_filter_deprecated_fg);
    prefs_register_color_preference(gui_color_module, "color_filter_bg.deprecated", "Deprecated color filter background",
        "Deprecated color filter background", &prefs.gui_filter_deprecated_bg);

    prefs_register_enum_preference(gui_module, "fileopen.style",
                       "Where to start the File Open dialog box",
                       "Where to start the File Open dialog box",
                       &prefs.gui_fileopen_style, gui_fileopen_style, false);

    prefs_register_uint_preference(gui_module, "recent_files_count.max",
                                   "The max. number of items in the open recent files list",
                                   "The max. number of items in the open recent files list",
                                   10,
                                   &prefs.gui_recent_files_count_max);

    prefs_register_uint_preference(gui_module, "recent_display_filter_entries.max",
                                   "The max. number of entries in the display filter list",
                                   "The max. number of entries in the display filter list",
                                   10,
                                   &prefs.gui_recent_df_entries_max);

    register_string_like_preference(gui_module, "fileopen.dir", "Start Directory",
        "Directory to start in when opening File Open dialog.",
        &prefs.gui_fileopen_dir, PREF_DIRNAME, NULL, true);

    prefs_register_obsolete_preference(gui_module, "fileopen.remembered_dir");

    prefs_register_uint_preference(gui_module, "fileopen.preview",
                                   "The preview timeout in the File Open dialog",
                                   "The preview timeout in the File Open dialog",
                                   10,
                                   &prefs.gui_fileopen_preview);

    register_string_like_preference(gui_module, "tlskeylog_command", "Program to launch with TLS Keylog",
        "Program path or command line to launch with SSLKEYLOGFILE",
        &prefs.gui_tlskeylog_command, PREF_STRING, NULL, true);

    prefs_register_bool_preference(gui_module, "ask_unsaved",
                                   "Ask to save unsaved capture files",
                                   "Ask to save unsaved capture files?",
                                   &prefs.gui_ask_unsaved);

    prefs_register_bool_preference(gui_module, "autocomplete_filter",
                                   "Display autocompletion for filter text",
                                   "Display an autocomplete suggestion for display and capture filter controls",
                                   &prefs.gui_autocomplete_filter);

    prefs_register_bool_preference(gui_module, "find_wrap",
                                   "Wrap to beginning/end of file during search",
                                   "Wrap to beginning/end of file during search?",
                                   &prefs.gui_find_wrap);

    prefs_register_obsolete_preference(gui_module, "use_pref_save");

    prefs_register_bool_preference(gui_module, "geometry.save.position",
                                   "Save window position at exit",
                                   "Save window position at exit?",
                                   &prefs.gui_geometry_save_position);

    prefs_register_bool_preference(gui_module, "geometry.save.size",
                                   "Save window size at exit",
                                   "Save window size at exit?",
                                   &prefs.gui_geometry_save_size);

    prefs_register_bool_preference(gui_module, "geometry.save.maximized",
                                   "Save window maximized state at exit",
                                   "Save window maximized state at exit?",
                                   &prefs.gui_geometry_save_maximized);

    prefs_register_obsolete_preference(gui_module, "macosx_style");

    prefs_register_obsolete_preference(gui_module, "geometry.main.x");
    prefs_register_obsolete_preference(gui_module, "geometry.main.y");
    prefs_register_obsolete_preference(gui_module, "geometry.main.width");
    prefs_register_obsolete_preference(gui_module, "geometry.main.height");
    prefs_register_obsolete_preference(gui_module, "toolbar_main_show");

    prefs_register_enum_preference(gui_module, "toolbar_main_style",
                       "Main Toolbar style",
                       "Main Toolbar style",
                       &prefs.gui_toolbar_main_style, gui_toolbar_style, false);

    prefs_register_obsolete_preference(gui_module, "toolbar_filter_style");
    prefs_register_obsolete_preference(gui_module, "webbrowser");

    prefs_register_bool_preference(gui_module, "update.enabled",
                                   "Check for updates",
                                   "Check for updates (Windows and macOS only)",
                                   &prefs.gui_update_enabled);

    prefs_register_enum_preference(gui_module, "update.channel",
                       "Update channel",
                       "The type of update to fetch. You should probably leave this set to STABLE.",
                       (int*)(void*)(&prefs.gui_update_channel), gui_update_channel, false);

    prefs_register_uint_preference(gui_module, "update.interval",
                                   "How often to check for software updates",
                                   "How often to check for software updates in seconds",
                                   10,
                                   &prefs.gui_update_interval);

    prefs_register_uint_preference(gui_module, "debounce.timer",
                                   "How long to wait before processing computationally intensive user input",
                                   "How long to wait (in milliseconds) before processing "
                                   "computationally intensive user input. "
                                   "If you type quickly, consider lowering the value for a 'snappier' "
                                   "experience. "
                                   "If you type slowly, consider increasing the value to avoid performance issues. "
                                   "This is currently used to delay searches in View -> Internals -> Supported Protocols "
                                   "and Preferences -> Advanced menu.",
                                   10,
                                   &prefs.gui_debounce_timer);

    register_string_like_preference(gui_module, "window_title", "Custom window title",
        "Custom window title to be appended to the existing title\n"
        "%C = capture comment from command line\n"
        "%F = file path of the capture file\n"
        "%P = profile name\n"
        "%S = a conditional separator (\" - \") that only shows when surrounded by variables with values or static text\n"
        "%V = version info",
        &prefs.gui_window_title, PREF_STRING, NULL, true);

    register_string_like_preference(gui_module, "prepend_window_title", "Custom window title prefix",
        "Custom window title to be prepended to the existing title\n"
        "%C = capture comment from command line\n"
        "%F = file path of the capture file\n"
        "%P = profile name\n"
        "%S = a conditional separator (\" - \") that only shows when surrounded by variables with values or static text\n"
        "%V = version info",
        &prefs.gui_prepend_window_title, PREF_STRING, NULL, true);

    register_string_like_preference(gui_module, "start_title", "Custom start page title",
        "Custom start page title",
        &prefs.gui_start_title, PREF_STRING, NULL, true);

    prefs_register_enum_preference(gui_module, "version_placement",
                       "Show version in the start page and/or main screen's title bar",
                       "Show version in the start page and/or main screen's title bar",
                       (int*)(void*)(&prefs.gui_version_placement), gui_version_placement_type, false);

    prefs_register_obsolete_preference(gui_module, "auto_scroll_on_expand");
    prefs_register_obsolete_preference(gui_module, "auto_scroll_percentage");

    prefs_register_uint_preference(gui_module, "max_export_objects",
                                   "Maximum number of exported objects",
                                   "The maximum number of objects that can be exported",
                                   10,
                                   &prefs.gui_max_export_objects);
    prefs_register_uint_preference(gui_module, "max_tree_items",
                                   "Maximum number of tree items",
                                   "The maximum number of items that can be added to the dissection tree (Increase with caution)",
                                   10,
                                   &prefs.gui_max_tree_items);
    /*
     * Used independently by proto_tree_add_node, call_dissector*, dissector_try_heuristic,
     * and increment_dissection_depth.
     */
    prefs_register_uint_preference(gui_module, "max_tree_depth",
                                   "Maximum dissection depth",
                                   "The maximum depth for dissection tree and protocol layer checks. (Increase with caution)",
                                   10,
                                   &prefs.gui_max_tree_depth);

    prefs_register_bool_preference(gui_module, "welcome_page.show_recent",
                                   "Show recent files on the welcome page",
                                   "This will enable or disable the 'Open' list on the welcome page.",
                                   &prefs.gui_welcome_page_show_recent);

    /* User Interface : Layout */
    gui_layout_module = prefs_register_subtree(gui_module, "Layout", "Layout", gui_layout_callback);
    /* Adjust the preference effects of layout GUI for better handling of preferences at Wireshark (GUI) level */
    layout_gui_flags = prefs_get_module_effect_flags(gui_layout_module);
    layout_gui_flags |= PREF_EFFECT_GUI_LAYOUT;
    layout_gui_flags &= (~PREF_EFFECT_DISSECTION);

    prefs_register_uint_preference(gui_layout_module, "layout_type",
                                   "Layout type",
                                   "Layout type (1-6)",
                                   10,
                                   (unsigned*)(void*)(&prefs.gui_layout_type));
    prefs_set_effect_flags_by_name(gui_layout_module, "layout_type", layout_gui_flags);

    prefs_register_enum_preference(gui_layout_module, "layout_content_1",
                       "Layout content of the pane 1",
                       "Layout content of the pane 1",
                       (int*)(void*)(&prefs.gui_layout_content_1), gui_layout_content, false);
    prefs_set_effect_flags_by_name(gui_layout_module, "layout_content_1", layout_gui_flags);

    prefs_register_enum_preference(gui_layout_module, "layout_content_2",
                       "Layout content of the pane 2",
                       "Layout content of the pane 2",
                       (int*)(void*)(&prefs.gui_layout_content_2), gui_layout_content, false);
    prefs_set_effect_flags_by_name(gui_layout_module, "layout_content_2", layout_gui_flags);

    prefs_register_enum_preference(gui_layout_module, "layout_content_3",
                       "Layout content of the pane 3",
                       "Layout content of the pane 3",
                       (int*)(void*)(&prefs.gui_layout_content_3), gui_layout_content, false);
    prefs_set_effect_flags_by_name(gui_layout_module, "layout_content_3", layout_gui_flags);

    prefs_register_bool_preference(gui_layout_module, "packet_list_separator.enabled",
                                   "Enable Packet List Separator",
                                   "Enable Packet List Separator",
                                   &prefs.gui_packet_list_separator);

    prefs_register_bool_preference(gui_layout_module, "packet_header_column_definition.enabled",
                                    "Show column definition in packet list header",
                                    "Show column definition in packet list header",
                                    &prefs.gui_packet_header_column_definition);

    /* packet_list_hover_style affects the colors, not the layout.
     * It's in the layout module to group it with the other packet list
     * preferences for the user's benefit with the dialog.
     */
    prefs_register_bool_preference(gui_layout_module, "packet_list_hover_style.enabled",
                                   "Enable Packet List mouse-over colorization",
                                   "Enable Packet List mouse-over colorization",
                                   &prefs.gui_packet_list_hover_style);
    prefs_set_effect_flags_by_name(gui_layout_module, "packet_list_hover_style.enabled", gui_color_effect_flags);

    prefs_register_bool_preference(gui_layout_module, "show_selected_packet.enabled",
                                   "Show selected packet in the Status Bar",
                                   "Show selected packet in the Status Bar",
                                   &prefs.gui_show_selected_packet);

    prefs_register_bool_preference(gui_layout_module, "show_file_load_time.enabled",
                                   "Show file load time in the Status Bar",
                                   "Show file load time in the Status Bar",
                                   &prefs.gui_show_file_load_time);

    prefs_register_enum_preference(gui_layout_module, "packet_dialog_layout",
                                   "Packet Dialog layout",
                                   "Packet Dialog layout",
                                   (unsigned*)(void*)(&prefs.gui_packet_dialog_layout), gui_packet_dialog_layout, false);

    prefs_register_enum_preference(gui_module, "packet_list_elide_mode",
                       "Elide mode",
                       "The position of \"...\" (ellipsis) in packet list text.",
                       (int*)(void*)(&prefs.gui_packet_list_elide_mode), gui_packet_list_elide_mode, false);
    prefs_register_uint_preference(gui_module, "decimal_places1",
            "Count of decimal places for values of type 1",
            "Sets the count of decimal places for values of type 1."
            "Type 1 values are defined by authors."
            "Value can be in range 2 to 10.",
            10,&prefs.gui_decimal_places1);

    prefs_register_uint_preference(gui_module, "decimal_places2",
            "Count of decimal places for values of type 2",
            "Sets the count of decimal places for values of type 2."
            "Type 2 values are defined by authors."
            "Value can be in range 2 to 10.",
            10,&prefs.gui_decimal_places2);

    prefs_register_uint_preference(gui_module, "decimal_places3",
            "Count of decimal places for values of type 3",
            "Sets the count of decimal places for values of type 3."
            "Type 3 values are defined by authors."
            "Value can be in range 2 to 10.",
            10,&prefs.gui_decimal_places3);

    prefs_register_bool_preference(gui_module, "rtp_player_use_disk1",
            "RTP Player saves temporary data to disk",
            "If set to true, RTP Player saves temporary data to "
            "temp files on disk. If not set, it uses memory."
            "Every stream uses one file therefore you might touch "
            "OS limit for count of opened files."
            "When ui.rtp_player_use_disk2 is set to true too, it uses "
            " two files per RTP stream together."
            ,&prefs.gui_rtp_player_use_disk1);

    prefs_register_bool_preference(gui_module, "rtp_player_use_disk2",
            "RTP Player saves temporary dictionary for data to disk",
            "If set to true, RTP Player saves temporary dictionary to "
            "temp files on disk. If not set, it uses memory."
            "Every stream uses one file therefore you might touch "
            "OS limit for count of opened files."
            "When ui.rtp_player_use_disk1 is set to true too, it uses "
            " two files per RTP stream."
            ,&prefs.gui_rtp_player_use_disk2);

    prefs_register_enum_preference(gui_layout_module, "gui_packet_list_copy_format_options_for_keyboard_shortcut",
                                   "Allows text to be copied with selected format",
                                   "Allows text to be copied with selected format when copied via keyboard",
                                   (int*)(void*)(&prefs.gui_packet_list_copy_format_options_for_keyboard_shortcut),
                                   gui_packet_list_copy_format_options_for_keyboard_shortcut, false);

    prefs_register_bool_preference(gui_layout_module, "gui_packet_list_copy_text_with_aligned_columns",
                                   "Allows text to be copied with aligned columns",
                                   "Allows text to be copied with aligned columns when copied via menu or keyboard",
                                   &prefs.gui_packet_list_copy_text_with_aligned_columns);

    prefs_register_bool_preference(gui_layout_module, "packet_list_show_related",
                                   "Show Related Packets",
                                   "Show related packet indicators in the first column",
                                   &prefs.gui_packet_list_show_related);

    prefs_register_bool_preference(gui_layout_module, "packet_list_show_minimap",
                                   "Enable Intelligent Scroll Bar",
                                   "Show the intelligent scroll bar (a minimap of packet list colors in the scrollbar)",
                                   &prefs.gui_packet_list_show_minimap);

    prefs_register_bool_preference(gui_module, "packet_list_is_sortable",
                                   "Allow packet list to be sortable",
                                   "To prevent sorting by mistake (which can take some time to calculate), it can be disabled",
                                   &prefs.gui_packet_list_sortable);

    prefs_register_uint_preference(gui_module, "packet_list_cached_rows_max",
                                   "Maximum cached rows",
                                   "Maximum number of rows that can be sorted by columns that require dissection. Increasing this increases memory consumption by caching column text",
                                   10,
                                   &prefs.gui_packet_list_cached_rows_max);

    prefs_register_bool_preference(gui_module, "interfaces_show_hidden",
                                   "Show hidden interfaces",
                                   "Show all interfaces, including interfaces marked as hidden",
                                   &prefs.gui_interfaces_show_hidden);

    prefs_register_bool_preference(gui_module, "interfaces_remote_display",
                                   "Show Remote interfaces",
                                   "Show remote interfaces in the interface selection",
                                   &prefs.gui_interfaces_remote_display);

    register_string_like_preference(gui_module, "interfaces_hidden_types", "Hide interface types in list",
        "Hide the given interface types in the startup list.\n"
        "A comma-separated string of interface type values (e.g. 5,9).\n"
         "0 = Wired,\n"
         "1 = AirPCAP,\n"
         "2 = Pipe,\n"
         "3 = STDIN,\n"
         "4 = Bluetooth,\n"
         "5 = Wireless,\n"
         "6 = Dial-Up,\n"
         "7 = USB,\n"
         "8 = External Capture,\n"
         "9 = Virtual",
        &prefs.gui_interfaces_hide_types, PREF_STRING, NULL, true);

    prefs_register_bool_preference(gui_module, "io_graph_automatic_update",
        "Enables automatic updates for IO Graph",
        "Enables automatic updates for IO Graph",
        &prefs.gui_io_graph_automatic_update);

    prefs_register_bool_preference(gui_module, "io_graph_enable_legend",
        "Enables the legend of IO Graph",
        "Enables the legend of IO Graph",
        &prefs.gui_io_graph_enable_legend);

    prefs_register_bool_preference(gui_module, "plot_automatic_update",
        "Enables automatic updates for Plot",
        "Enables automatic updates for Plot",
        &prefs.gui_plot_automatic_update);

    prefs_register_bool_preference(gui_module, "plot_enable_legend",
        "Enables the legend of Plot",
        "Enables the legend of Plot",
        &prefs.gui_plot_enable_legend);

    prefs_register_bool_preference(gui_module, "show_byteview_in_dialog",
        "Show the byte view in the packet details dialog",
        "Show the byte view in the packet details dialog",
        &prefs.gui_packet_details_show_byteview);

    /* Console
     * These are preferences that can be read/written using the
     * preference module API.  These preferences still use their own
     * configuration screens for access, but this cuts down on the
     * preference "string compare list" in set_pref()
     */
    console_module = prefs_register_module(NULL, "console", "Console",
        "Console logging and debugging output", NULL, NULL, false);

    prefs_register_obsolete_preference(console_module, "log.level");

    prefs_register_bool_preference(console_module, "incomplete_dissectors_check_debug",
                                   "Print debug line for incomplete dissectors",
                                   "Look for dissectors that left some bytes undecoded (debug)",
                                   &prefs.incomplete_dissectors_check_debug);

    /* Display filter Expressions
     * This used to be an array of individual fields that has now been
     * converted to a UAT.  Just make it part of the GUI category even
     * though the name of the preference will never be seen in preference
     * file
     */
    filter_expression_register_uat(gui_module);

    /* Capture
     * These are preferences that can be read/written using the
     * preference module API.  These preferences still use their own
     * configuration screens for access, but this cuts down on the
     * preference "string compare list" in set_pref()
     */
    capture_module = prefs_register_module(NULL, "capture", "Capture",
        "Capture preferences", NULL, NULL, false);
    /* Capture preferences don't affect dissection */
    prefs_set_module_effect_flags(capture_module, PREF_EFFECT_CAPTURE);

    register_string_like_preference(capture_module, "device", "Default capture device",
        "Default capture device",
        &prefs.capture_device, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_linktypes", "Interface link-layer header type",
        "Interface link-layer header types (Ex: en0(1),en1(143),...)",
        &prefs.capture_devices_linktypes, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_descr", "Interface descriptions",
        "Interface descriptions (Ex: eth0(eth0 descr),eth1(eth1 descr),...)",
        &prefs.capture_devices_descr, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_hide", "Hide interface",
        "Hide interface? (Ex: eth0,eth3,...)",
        &prefs.capture_devices_hide, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_monitor_mode", "Capture in monitor mode",
        "By default, capture in monitor mode on interface? (Ex: eth0,eth3,...)",
        &prefs.capture_devices_monitor_mode, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_buffersize", "Interface buffer size",
        "Interface buffer size (Ex: en0(1),en1(143),...)",
        &prefs.capture_devices_buffersize, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_snaplen", "Interface snap length",
        "Interface snap length (Ex: en0(65535),en1(1430),...)",
        &prefs.capture_devices_snaplen, PREF_STRING, NULL, false);

    register_string_like_preference(capture_module, "devices_pmode", "Interface promiscuous mode",
        "Interface promiscuous mode (Ex: en0(0),en1(1),...)",
        &prefs.capture_devices_pmode, PREF_STRING, NULL, false);

    prefs_register_bool_preference(capture_module, "prom_mode", "Capture in promiscuous mode",
        "Capture in promiscuous mode?", &prefs.capture_prom_mode);

    prefs_register_bool_preference(capture_module, "monitor_mode", "Capture in monitor mode on 802.11 devices",
        "Capture in monitor mode on all 802.11 devices that support it?", &prefs.capture_monitor_mode);

    register_string_like_preference(capture_module, "devices_filter", "Interface capture filter",
        "Interface capture filter (Ex: en0(tcp),en1(udp),...)",
        &prefs.capture_devices_filter, PREF_STRING, NULL, false);

    prefs_register_bool_preference(capture_module, "pcap_ng", "Capture in pcapng format",
        "Capture in pcapng format?", &prefs.capture_pcap_ng);

    prefs_register_bool_preference(capture_module, "real_time_update", "Update packet list in real time during capture",
        "Update packet list in real time during capture?", &prefs.capture_real_time);

    prefs_register_uint_preference(capture_module, "update_interval",
                                   "Capture update interval",
                                   "Capture update interval in ms",
                                   10,
                                   &prefs.capture_update_interval);

    prefs_register_bool_preference(capture_module, "no_interface_load", "Don't load interfaces on startup",
        "Don't automatically load capture interfaces on startup", &prefs.capture_no_interface_load);

    prefs_register_bool_preference(capture_module, "no_extcap", "Disable external capture interfaces",
        "Disable external capture modules (extcap)", &prefs.capture_no_extcap);

    prefs_register_obsolete_preference(capture_module, "auto_scroll");

    prefs_register_bool_preference(capture_module, "show_info", "Show capture information dialog while capturing",
        "Show capture information dialog while capturing?", &prefs.capture_show_info);

    prefs_register_obsolete_preference(capture_module, "syntax_check_filter");

    custom_cbs.free_cb = capture_column_free_cb;
    custom_cbs.reset_cb = capture_column_reset_cb;
    custom_cbs.set_cb = capture_column_set_cb;
    custom_cbs.type_name_cb = capture_column_type_name_cb;
    custom_cbs.type_description_cb = capture_column_type_description_cb;
    custom_cbs.is_default_cb = capture_column_is_default_cb;
    custom_cbs.to_str_cb = capture_column_to_str_cb;
    prefs_register_list_custom_preference(capture_module, "columns", "Capture options dialog column list",
        "List of columns to be displayed", &custom_cbs, capture_column_init_cb, &prefs.capture_columns);

    /* Name Resolution */
    nameres_module = prefs_register_module(NULL, "nameres", "Name Resolution",
        "Name Resolution", "ChCustPreferencesSection.html#ChCustPrefsNameSection", addr_resolve_pref_apply, true);
    addr_resolve_pref_init(nameres_module);
    oid_pref_init(nameres_module);
    maxmind_db_pref_init(nameres_module);

    /* Printing
     * None of these have any effect; we keep them as obsolete preferences
     * in order to avoid errors when reading older preference files.
     */
    printing = prefs_register_module(NULL, "print", "Printing",
        "Printing", NULL, NULL, false);
    prefs_register_obsolete_preference(printing, "format");
    prefs_register_obsolete_preference(printing, "command");
    prefs_register_obsolete_preference(printing, "file");

    /* Codecs */
    codecs_module = prefs_register_module(NULL, "codecs", "Codecs",
        "Codecs", NULL, NULL, true);

    /* Statistics */
    stats_module = prefs_register_module(NULL, "statistics", "Statistics",
        "Statistics", "ChCustPreferencesSection.html#_statistics", &stats_callback, true);

    prefs_register_uint_preference(stats_module, "update_interval",
                                   "Tap update interval in ms",
                                   "Determines time between tap updates",
                                   10,
                                   &prefs.tap_update_interval);

    prefs_register_uint_preference(stats_module, "flow_graph_max_export_items",
                                   "Maximum Flow Graph items to export as image",
                                   "The maximum number of Flow Graph items (frames) "
                                   "to include when exporting the graph as an image. "
                                   "Note that some formats (e.g., JPEG) have inherent "
                                   "pixel limits and image viewers might be unable to "
                                   "handle very large images.",
                                   10,
                                   &prefs.flow_graph_max_export_items);

    prefs_register_bool_preference(stats_module, "st_enable_burstinfo",
            "Enable the calculation of burst information",
            "If enabled burst rates will be calculated for statistics that use the stats_tree system. "
            "Burst rates are calculated over a much shorter time interval than the rate column.",
            &prefs.st_enable_burstinfo);

    prefs_register_bool_preference(stats_module, "st_burst_showcount",
            "Show burst count for item rather than rate",
            "If selected the stats_tree statistics nodes will show the count of events "
            "within the burst window instead of a burst rate. Burst rate is calculated "
            "as number of events within burst window divided by the burst windown length.",
            &prefs.st_burst_showcount);

    prefs_register_uint_preference(stats_module, "st_burst_resolution",
            "Burst rate resolution (ms)",
            "Sets the duration of the time interval into which events are grouped when calculating "
            "the burst rate. Higher resolution (smaller number) increases processing overhead.",
            10,&prefs.st_burst_resolution);

    prefs_register_uint_preference(stats_module, "st_burst_windowlen",
            "Burst rate window size (ms)",
            "Sets the duration of the sliding window during which the burst rate is "
            "measured. Longer window relative to burst rate resolution increases "
            "processing overhead. Will be truncated to a multiple of burst resolution.",
            10,&prefs.st_burst_windowlen);

    prefs_register_enum_preference(stats_module, "st_sort_defcolflag",
            "Default sort column for stats_tree stats",
            "Sets the default column by which stats based on the stats_tree "
            "system is sorted.",
            &prefs.st_sort_defcolflag, st_sort_col_vals, false);

    prefs_register_bool_preference(stats_module, "st_sort_defdescending",
            "Default stats_tree sort order is descending",
            "When selected, statistics based on the stats_tree system will by default "
            "be sorted in descending order.",
            &prefs.st_sort_defdescending);

    prefs_register_bool_preference(stats_module, "st_sort_casesensitve",
            "Case sensitive sort of stats_tree item names",
            "When selected, the item/node names of statistics based on the stats_tree "
            "system will be sorted taking case into account. Else the case of the name "
            "will be ignored.",
            &prefs.st_sort_casesensitve);

    prefs_register_bool_preference(stats_module, "st_sort_rng_nameonly",
            "Always sort 'range' nodes by name",
            "When selected, the stats_tree nodes representing a range of values "
            "(0-49, 50-100, etc.) will always be sorted by name (the range of the "
            "node). Else range nodes are sorted by the same column as the rest of "
            " the tree.",
            &prefs.st_sort_rng_nameonly);

    prefs_register_bool_preference(stats_module, "st_sort_rng_fixorder",
            "Always sort 'range' nodes in ascending order",
            "When selected, the stats_tree nodes representing a range of values "
            "(0-49, 50-100, etc.) will always be sorted ascending; else it follows "
            "the sort direction of the tree. Only effective if \"Always sort "
            "'range' nodes by name\" is also selected.",
            &prefs.st_sort_rng_fixorder);

    prefs_register_bool_preference(stats_module, "st_sort_showfullname",
            "Display the full stats_tree plug-in name",
            "When selected, the full name (including menu path) of the stats_tree "
            "plug-in is show in windows. If cleared the plug-in name is shown "
            "without menu path (only the part of the name after last '/' character.)",
            &prefs.st_sort_showfullname);

    prefs_register_enum_preference(stats_module, "output_format",
            "Default output format",
            "Sets the default output format for statistical data. Only supported "
            "by taps using the stats_tree system currently; other taps may honor "
            "this preference in the future. ",
            &prefs.st_format, st_format_vals, false);

    module_t *conv_module;
    // avoid using prefs_register_stat to prevent lint complaint about recursion
    conv_module = prefs_register_module(stats_module, "conv", "Conversations",
            "Conversations & Endpoints", NULL, NULL, true);
    prefs_register_bool_preference(conv_module, "machine_readable",
            "Display exact (machine-readable) byte counts",
            "When enabled, exact machine-readable byte counts are displayed. "
            "When disabled, human readable numbers with SI prefixes are displayed.",
            &prefs.conv_machine_readable);

    /* Protocols */
    protocols_module = prefs_register_module(NULL, "protocols", "Protocols",
                                             "Protocols", "ChCustPreferencesSection.html#ChCustPrefsProtocolsSection", NULL, true);

    prefs_register_bool_preference(protocols_module, "display_hidden_proto_items",
                                   "Display hidden protocol items",
                                   "Display all hidden protocol items in the packet list.",
                                   &prefs.display_hidden_proto_items);

    prefs_register_bool_preference(protocols_module, "display_byte_fields_with_spaces",
                                   "Display byte fields with a space character between bytes",
                                   "Display all byte fields with a space character between each byte in the packet list.",
                                   &prefs.display_byte_fields_with_spaces);

    /*
     * Note the -t /  option only affects the display of the packet timestamp
     * in the default time column; this is for all other absolute times.
     */
    prefs_register_enum_preference(protocols_module, "display_abs_time_ascii",
                                   "Format absolute times like asctime",
                                   "When to format absolute times similar to asctime instead of ISO 8601, for backwards compatibility with older Wireshark.",
                                   (int*)&prefs.display_abs_time_ascii, abs_time_format_options, false);

    prefs_register_bool_preference(protocols_module, "enable_incomplete_dissectors_check",
                                   "Look for incomplete dissectors",
                                   "Look for dissectors that left some bytes undecoded.",
                                   &prefs.enable_incomplete_dissectors_check);

    prefs_register_bool_preference(protocols_module, "strict_conversation_tracking_heuristics",
                                   "Enable stricter conversation tracking heuristics",
                                   "Protocols may use things like VLAN ID or interface ID to narrow the potential for duplicate conversations. "
                                   "Currently ICMP and ICMPv6 use this preference to add VLAN ID to conversation tracking, and IPv4 uses this preference to take VLAN ID into account during reassembly",
                                   &prefs.strict_conversation_tracking_heuristics);

    prefs_register_bool_preference(protocols_module, "ignore_dup_frames",
                                   "Ignore duplicate frames",
                                   "Ignore frames that are exact duplicates of any previous frame.",
                                   &prefs.ignore_dup_frames);

    prefs_register_enum_preference(protocols_module, "conversation_deinterlacing_key",
                                   "Deinterlacing conversations key",
                                   "Separate into different conversations frames that look like duplicates but have different Interface, MAC, or VLAN field values.",
                                   (int *)&prefs.conversation_deinterlacing_key, conv_deint_options, false);

    prefs_register_uint_preference(protocols_module, "ignore_dup_frames_cache_entries",
            "The max number of hashes to keep in memory for determining duplicates frames",
            "If \"Ignore duplicate frames\" is set, this setting sets the maximum number "
            "of cache entries to maintain. A 0 means no limit.",
            10, &prefs.ignore_dup_frames_cache_entries);


    /* Obsolete preferences
     * These "modules" were reorganized/renamed to correspond to their GUI
     * configuration screen within the preferences dialog
     */

    /* taps is now part of the stats module */
    prefs_register_module(NULL, "taps", "TAPS", "TAPS", NULL, NULL, false);
    /* packet_list is now part of the protocol (parent) module */
    prefs_register_module(NULL, "packet_list", "PACKET_LIST", "PACKET_LIST", NULL, NULL, false);
    /* stream is now part of the gui module */
    prefs_register_module(NULL, "stream", "STREAM", "STREAM", NULL, NULL, false);

}

/* Parse through a list of comma-separated, possibly quoted strings.
   Return a list of the string data. */
GList *
prefs_get_string_list(const char *str)
{
    enum { PRE_STRING, IN_QUOT, NOT_IN_QUOT };

    int       state = PRE_STRING, i = 0;
    bool      backslash = false;
    unsigned char    cur_c;
    const size_t default_size = 64;
    GString  *slstr = NULL;
    GList    *sl = NULL;

    /* Allocate a buffer for the first string.   */
    slstr = g_string_sized_new(default_size);

    for (;;) {
        cur_c = str[i];
        if (cur_c == '\0') {
            /* It's the end of the input, so it's the end of the string we
               were working on, and there's no more input. */
            if (state == IN_QUOT || backslash) {
                /* We were in the middle of a quoted string or backslash escape,
                   and ran out of characters; that's an error.  */
                g_string_free(slstr, TRUE);
                prefs_clear_string_list(sl);
                return NULL;
            }
            if (slstr->len > 0)
                sl = g_list_append(sl, g_string_free(slstr, FALSE));
            else
                g_string_free(slstr, TRUE);
            break;
        }
        if (cur_c == '"' && !backslash) {
            switch (state) {
            case PRE_STRING:
                /* We hadn't yet started processing a string; this starts the
                   string, and we're now quoting.  */
                state = IN_QUOT;
                break;
            case IN_QUOT:
                /* We're in the middle of a quoted string, and we saw a quotation
                   mark; we're no longer quoting.   */
                state = NOT_IN_QUOT;
                break;
            case NOT_IN_QUOT:
                /* We're working on a string, but haven't seen a quote; we're
                   now quoting.  */
                state = IN_QUOT;
                break;
            default:
                break;
            }
        } else if (cur_c == '\\' && !backslash) {
            /* We saw a backslash, and the previous character wasn't a
               backslash; escape the next character.

               This also means we've started a new string. */
            backslash = true;
            if (state == PRE_STRING)
                state = NOT_IN_QUOT;
        } else if (cur_c == ',' && state != IN_QUOT && !backslash) {
            /* We saw a comma, and we're not in the middle of a quoted string
               and it wasn't preceded by a backslash; it's the end of
               the string we were working on...  */
            if (slstr->len > 0) {
                sl = g_list_append(sl, g_string_free(slstr, FALSE));
                slstr = g_string_sized_new(default_size);
            }

            /* ...and the beginning of a new string.  */
            state = PRE_STRING;
        } else if (!g_ascii_isspace(cur_c) || state != PRE_STRING) {
            /* Either this isn't a white-space character, or we've started a
               string (i.e., already seen a non-white-space character for that
               string and put it into the string).

               The character is to be put into the string; do so.  */
            g_string_append_c(slstr, cur_c);

            /* If it was backslash-escaped, we're done with the backslash escape.  */
            backslash = false;
        }
        i++;
    }
    return(sl);
}

char *join_string_list(GList *sl)
{
    GString      *joined_str = g_string_new("");
    GList        *cur, *first;
    char         *str;
    unsigned      item_count = 0;

    cur = first = g_list_first(sl);
    while (cur) {
        item_count++;
        str = (char *)cur->data;

        if (cur != first)
            g_string_append_c(joined_str, ',');

        if (item_count % 2) {
            /* Wrap the line.  */
            g_string_append(joined_str, "\n\t");
        } else
            g_string_append_c(joined_str, ' ');

        g_string_append_c(joined_str, '"');
        while (*str) {
            gunichar uc = g_utf8_get_char (str);

            if (uc == '"' || uc == '\\')
                g_string_append_c(joined_str, '\\');

            if (g_unichar_isprint(uc))
                g_string_append_unichar (joined_str, uc);

            str = g_utf8_next_char (str);
        }

        g_string_append_c(joined_str, '"');

        cur = cur->next;
    }
    return g_string_free(joined_str, FALSE);
}

void
prefs_clear_string_list(GList *sl)
{
    g_list_free_full(sl, g_free);
}

/*
 * Takes a string, a pointer to an array of "enum_val_t"s, and a default int
 * value.
 * The array must be terminated by an entry with a null "name" string.
 *
 * If the string matches a "name" string in an entry, the value from that
 * entry is returned.
 *
 * Otherwise, if a string matches a "description" string in an entry, the
 * value from that entry is returned; we do that for backwards compatibility,
 * as we used to have only a "name" string that was used both for command-line
 * and configuration-file values and in the GUI (which meant either that
 * the GUI had what might be somewhat cryptic values to select from or that
 * the "-o" flag took long strings, often with spaces in them).
 *
 * Otherwise, the default value that was passed as the third argument is
 * returned.
 */
static int
find_val_for_string(const char *needle, const enum_val_t *haystack,
                    int default_value)
{
    int i;

    for (i = 0; haystack[i].name != NULL; i++) {
        if (g_ascii_strcasecmp(needle, haystack[i].name) == 0) {
            return haystack[i].value;
        }
    }
    for (i = 0; haystack[i].name != NULL; i++) {
        if (g_ascii_strcasecmp(needle, haystack[i].description) == 0) {
            return haystack[i].value;
        }
    }
    return default_value;
}

/* Preferences file format:
 * - Configuration directives start at the beginning of the line, and
 *   are terminated with a colon.
 * - Directives can be continued on the next line by preceding them with
 *   whitespace.
 *
 * Example:

# This is a comment line
print.command: lpr
print.file: /a/very/long/path/
            to/wireshark-out.ps
 *
 */

/* Initialize non-dissector preferences to wired-in default values Called
 * at program startup and any time the profile changes. (The dissector
 * preferences are assumed to be set to those values by the dissectors.)
 * They may be overridden by the global preferences file or the user's
 * preferences file.
 */
static void
init_prefs(void)
{
    if (prefs_initialized)
        return;

    uat_load_all();

    /*
     * Ensure the "global" preferences have been initialized so the
     * preference API has the proper default values to work from
     */
    pre_init_prefs();

    prefs_register_modules();

    prefs_initialized = true;
}

/*
 * Initialize non-dissector preferences used by the "register preference" API
 * to default values so the default values can be used when registered.
 *
 * String, filename, and directory preferences will be g_freed so they must
 * be g_mallocated.
 */
static void
pre_init_prefs(void)
{
    int         i;
    char        *col_name;
    fmt_data    *cfmt;
    static const char *col_fmt_packets[] = {
        "No.",      "%m", "Time",        "%t",
        "Source",   "%s", "Destination", "%d",
        "Protocol", "%p", "Length",      "%L",
        "Info",     "%i" };
    static const char **col_fmt = col_fmt_packets;
    int num_cols = 7;

    if (application_flavor_is_stratoshark()) {
        static const char *col_fmt_logs[] = {
            "No.",              "%m",
            "Time",             "%t",
            "Event name",       "%Cus:sysdig.event_name:0:R",
            "Dir",              "%Cus:evt.dir:0:R",
            "Proc Name",        "%Cus:proc.name:0:R",
            "PID",              "%Cus:proc.pid:0:R",
            "TID",              "%Cus:thread.tid:0:R",
            "FD",               "%Cus:fd.num:0:R",
            "FD Name",          "%Cus:fd.name:0:R",
            "Container Name",   "%Cus:container.name:0:R",
            "Arguments",        "%Cus:evt.args:0:R",
            "Info",             "%i"
            };
        col_fmt = col_fmt_logs;
        num_cols = 12;
    }

    prefs.restore_filter_after_following_stream = false;
    prefs.gui_toolbar_main_style = TB_STYLE_ICONS;
    /* We try to find the best font in the Qt code */
    g_free(prefs.gui_font_name);
    prefs.gui_font_name              = g_strdup("");
    prefs.gui_active_fg.red          =         0;
    prefs.gui_active_fg.green        =         0;
    prefs.gui_active_fg.blue         =         0;
    prefs.gui_active_bg.red          =     52223;
    prefs.gui_active_bg.green        =     59647;
    prefs.gui_active_bg.blue         =     65535;
    prefs.gui_active_style           = COLOR_STYLE_DEFAULT;
    prefs.gui_inactive_fg.red        =         0;
    prefs.gui_inactive_fg.green      =         0;
    prefs.gui_inactive_fg.blue       =         0;
    prefs.gui_inactive_bg.red        =     61439;
    prefs.gui_inactive_bg.green      =     61439;
    prefs.gui_inactive_bg.blue       =     61439;
    prefs.gui_inactive_style         = COLOR_STYLE_DEFAULT;
    prefs.gui_marked_fg.red          =     65535;
    prefs.gui_marked_fg.green        =     65535;
    prefs.gui_marked_fg.blue         =     65535;
    prefs.gui_marked_bg.red          =         0;
    prefs.gui_marked_bg.green        =      8224;
    prefs.gui_marked_bg.blue         =     10794;
    prefs.gui_ignored_fg.red         =     32767;
    prefs.gui_ignored_fg.green       =     32767;
    prefs.gui_ignored_fg.blue        =     32767;
    prefs.gui_ignored_bg.red         =     65535;
    prefs.gui_ignored_bg.green       =     65535;
    prefs.gui_ignored_bg.blue        =     65535;
    g_free(prefs.gui_colorized_fg);
    prefs.gui_colorized_fg           = g_strdup("000000,000000,000000,000000,000000,000000,000000,000000,000000,000000");
    g_free(prefs.gui_colorized_bg);
    prefs.gui_colorized_bg           = g_strdup("ffc0c0,ffc0ff,e0c0e0,c0c0ff,c0e0e0,c0ffff,c0ffc0,ffffc0,e0e0c0,e0e0e0");
    prefs.st_client_fg.red           = 32767;
    prefs.st_client_fg.green         =     0;
    prefs.st_client_fg.blue          =     0;
    prefs.st_client_bg.red           = 64507;
    prefs.st_client_bg.green         = 60909;
    prefs.st_client_bg.blue          = 60909;
    prefs.st_server_fg.red           =     0;
    prefs.st_server_fg.green         =     0;
    prefs.st_server_fg.blue          = 32767;
    prefs.st_server_bg.red           = 60909;
    prefs.st_server_bg.green         = 60909;
    prefs.st_server_bg.blue          = 64507;

    if (gui_theme_is_dark) {
        // Green, red and yellow with HSV V = 84
        prefs.gui_filter_valid_bg.red         = 0x0000; /* dark green */
        prefs.gui_filter_valid_bg.green       = 0x66ff;
        prefs.gui_filter_valid_bg.blue        = 0x0000;
        prefs.gui_filter_valid_fg.red         = 0xFFFF;
        prefs.gui_filter_valid_fg.green       = 0xFFFF;
        prefs.gui_filter_valid_fg.blue        = 0xFFFF;
        prefs.gui_filter_invalid_bg.red       = 0x66FF; /* dark red */
        prefs.gui_filter_invalid_bg.green     = 0x0000;
        prefs.gui_filter_invalid_bg.blue      = 0x0000;
        prefs.gui_filter_invalid_fg.red       = 0xFFFF;
        prefs.gui_filter_invalid_fg.green     = 0xFFFF;
        prefs.gui_filter_invalid_fg.blue      = 0xFFFF;
        prefs.gui_filter_deprecated_bg.red    = 0x66FF; /* dark yellow / olive */
        prefs.gui_filter_deprecated_bg.green  = 0x66FF;
        prefs.gui_filter_deprecated_bg.blue   = 0x0000;
        prefs.gui_filter_deprecated_fg.red    = 0xFFFF;
        prefs.gui_filter_deprecated_fg.green  = 0xFFFF;
        prefs.gui_filter_deprecated_fg.blue   = 0xFFFF;
    } else {
        // Green, red and yellow with HSV V = 20
        prefs.gui_filter_valid_bg.red         = 0xAFFF; /* light green */
        prefs.gui_filter_valid_bg.green       = 0xFFFF;
        prefs.gui_filter_valid_bg.blue        = 0xAFFF;
        prefs.gui_filter_valid_fg.red         = 0x0000;
        prefs.gui_filter_valid_fg.green       = 0x0000;
        prefs.gui_filter_valid_fg.blue        = 0x0000;
        prefs.gui_filter_invalid_bg.red       = 0xFFFF; /* light red */
        prefs.gui_filter_invalid_bg.green     = 0xAFFF;
        prefs.gui_filter_invalid_bg.blue      = 0xAFFF;
        prefs.gui_filter_invalid_fg.red       = 0x0000;
        prefs.gui_filter_invalid_fg.green     = 0x0000;
        prefs.gui_filter_invalid_fg.blue      = 0x0000;
        prefs.gui_filter_deprecated_bg.red    = 0xFFFF; /* light yellow */
        prefs.gui_filter_deprecated_bg.green  = 0xFFFF;
        prefs.gui_filter_deprecated_bg.blue   = 0xAFFF;
        prefs.gui_filter_deprecated_fg.red    = 0x0000;
        prefs.gui_filter_deprecated_fg.green  = 0x0000;
        prefs.gui_filter_deprecated_fg.blue   = 0x0000;
    }

    prefs.gui_geometry_save_position = true;
    prefs.gui_geometry_save_size     = true;
    prefs.gui_geometry_save_maximized= true;
    prefs.gui_fileopen_style         = FO_STYLE_LAST_OPENED;
    prefs.gui_recent_df_entries_max  = 10;
    prefs.gui_recent_files_count_max = 10;
    g_free(prefs.gui_fileopen_dir);
    prefs.gui_fileopen_dir           = g_strdup(get_persdatafile_dir());
    prefs.gui_fileopen_preview       = 3;
    g_free(prefs.gui_tlskeylog_command);
    prefs.gui_tlskeylog_command      = g_strdup("");
    prefs.gui_ask_unsaved            = true;
    prefs.gui_autocomplete_filter    = true;
    prefs.gui_find_wrap              = true;
    prefs.gui_update_enabled         = true;
    prefs.gui_update_channel         = UPDATE_CHANNEL_STABLE;
    prefs.gui_update_interval        = 60*60*24; /* Seconds */
    prefs.gui_debounce_timer         = 400; /* milliseconds */
    g_free(prefs.gui_window_title);
    prefs.gui_window_title           = g_strdup("");
    g_free(prefs.gui_prepend_window_title);
    prefs.gui_prepend_window_title   = g_strdup("");
    g_free(prefs.gui_start_title);
    prefs.gui_start_title            = g_strdup("The World's Most Popular Network Protocol Analyzer");
    prefs.gui_version_placement      = version_both;
    prefs.gui_welcome_page_show_recent = true;
    prefs.gui_layout_type            = layout_type_2;
    prefs.gui_layout_content_1       = layout_pane_content_plist;
    prefs.gui_layout_content_2       = layout_pane_content_pdetails;
    prefs.gui_layout_content_3       = layout_pane_content_pbytes;
    prefs.gui_packet_list_elide_mode = ELIDE_RIGHT;
    prefs.gui_packet_list_copy_format_options_for_keyboard_shortcut = COPY_FORMAT_TEXT;
    prefs.gui_packet_list_copy_text_with_aligned_columns = false;
    prefs.gui_packet_list_show_related = true;
    prefs.gui_packet_list_show_minimap = true;
    prefs.gui_packet_list_sortable     = true;
    prefs.gui_packet_list_cached_rows_max = 10000;
    g_free (prefs.gui_interfaces_hide_types);
    prefs.gui_interfaces_hide_types = g_strdup("");
    prefs.gui_interfaces_show_hidden = false;
    prefs.gui_interfaces_remote_display = true;
    prefs.gui_packet_list_separator = false;
    prefs.gui_packet_header_column_definition = true;
    prefs.gui_packet_list_hover_style = true;
    prefs.gui_show_selected_packet = false;
    prefs.gui_show_file_load_time = false;
    prefs.gui_max_export_objects     = 1000;
    prefs.gui_max_tree_items = 1 * 1000 * 1000;
    prefs.gui_max_tree_depth = 5 * 100;
    prefs.gui_decimal_places1 = DEF_GUI_DECIMAL_PLACES1;
    prefs.gui_decimal_places2 = DEF_GUI_DECIMAL_PLACES2;
    prefs.gui_decimal_places3 = DEF_GUI_DECIMAL_PLACES3;

    if (prefs.col_list) {
        free_col_info(prefs.col_list);
        prefs.col_list = NULL;
    }
    for (i = 0; i < num_cols; i++) {
        cfmt = g_new0(fmt_data,1);
        cfmt->title = g_strdup(col_fmt[i * 2]);
        cfmt->visible = true;
        cfmt->display = COLUMN_DISPLAY_STRINGS;
        parse_column_format(cfmt, col_fmt[(i * 2) + 1]);
        prefs.col_list = g_list_append(prefs.col_list, cfmt);
    }
    prefs.num_cols  = num_cols;

/* set the default values for the capture dialog box */
    prefs.capture_prom_mode             = true;
    prefs.capture_monitor_mode          = false;
    prefs.capture_pcap_ng               = true;
    prefs.capture_real_time             = true;
    prefs.capture_update_interval       = DEFAULT_UPDATE_INTERVAL;
    prefs.capture_no_extcap             = false;
    prefs.capture_show_info             = false;

    if (!prefs.capture_columns) {
        /* First time through */
        for (i = 0; i < num_capture_cols; i++) {
            col_name = g_strdup(capture_cols[i]);
            prefs.capture_columns = g_list_append(prefs.capture_columns, col_name);
        }
    }

/* set the default values for the tap/statistics dialog box */
    prefs.tap_update_interval    = TAP_UPDATE_DEFAULT_INTERVAL;
    prefs.flow_graph_max_export_items = 1000;
    prefs.st_enable_burstinfo = true;
    prefs.st_burst_showcount = false;
    prefs.st_burst_resolution = ST_DEF_BURSTRES;
    prefs.st_burst_windowlen = ST_DEF_BURSTLEN;
    prefs.st_sort_casesensitve = true;
    prefs.st_sort_rng_fixorder = true;
    prefs.st_sort_rng_nameonly = true;
    prefs.st_sort_defcolflag = ST_SORT_COL_COUNT;
    prefs.st_sort_defdescending = true;
    prefs.st_sort_showfullname = false;
    prefs.conv_machine_readable = false;

    /* protocols */
    prefs.display_hidden_proto_items = false;
    prefs.display_byte_fields_with_spaces = false;
    prefs.display_abs_time_ascii = ABS_TIME_ASCII_TREE;
    prefs.ignore_dup_frames = false;
    prefs.ignore_dup_frames_cache_entries = 10000;

    /* set the default values for the io graph dialog */
    prefs.gui_io_graph_automatic_update = true;
    prefs.gui_io_graph_enable_legend = true;

    /* set the default values for the plot dialog */
    prefs.gui_plot_automatic_update = true;
    prefs.gui_plot_enable_legend = true;

    /* set the default values for the packet dialog */
    prefs.gui_packet_dialog_layout   = layout_vertical;
    prefs.gui_packet_details_show_byteview = true;
}

/*
 * Reset a single dissector preference.
 */
void
reset_pref(pref_t *pref)
{
    if (!pref) return;

    /*
     * This preference is no longer supported; it's not a
     * real preference, so we don't reset it (i.e., we
     * treat it as if it weren't found in the list of
     * preferences, and we weren't called in the first place).
     */
    if (pref->obsolete)
        return;

    switch (pref->type) {

    case PREF_UINT:
        *pref->varp.uint = pref->default_val.uint;
        break;

    case PREF_BOOL:
        *pref->varp.boolp = pref->default_val.boolval;
        break;

    case PREF_ENUM:
    case PREF_PROTO_TCP_SNDAMB_ENUM:
        *pref->varp.enump = pref->default_val.enumval;
        break;

    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        reset_string_like_preference(pref);
        break;

    case PREF_RANGE:
    case PREF_DECODE_AS_RANGE:
        wmem_free(wmem_epan_scope(), *pref->varp.range);
        *pref->varp.range = range_copy(wmem_epan_scope(), pref->default_val.range);
        break;

    case PREF_STATIC_TEXT:
    case PREF_UAT:
        /* Nothing to do */
        break;

    case PREF_COLOR:
        *pref->varp.colorp = pref->default_val.color;
        break;

    case PREF_CUSTOM:
        pref->custom_cbs.reset_cb(pref);
        break;
    }
}

static void
reset_pref_cb(void *data, void *user_data)
{
    pref_t *pref = (pref_t *) data;
    module_t *module = (module_t *)user_data;

    if (pref && (pref->type == PREF_RANGE || pref->type == PREF_DECODE_AS_RANGE)) {
        /*
         * Some dissectors expect the range (returned via prefs_get_range_value)
         * to remain valid if it has not changed. If it did change, then we
         * should set "prefs_changed_flags" to ensure that the preference apply
         * callback is invoked. That callback will notify dissectors that it
         * should no longer assume the range to be valid.
         */
        if (ranges_are_equal(*pref->varp.range, pref->default_val.range)) {
            /* Optimization: do not invoke apply callback if nothing changed. */
            return;
        }
        module->prefs_changed_flags |= prefs_get_effect_flags(pref);
    }
    reset_pref(pref);
}

/*
 * Reset all preferences for a module.
 */
static bool
reset_module_prefs(const void *key _U_, void *value, void *data _U_)
{
    module_t *module = (module_t *)value;
    g_list_foreach(module->prefs, reset_pref_cb, module);
    return false;
}

/* Reset preferences */
void
prefs_reset(void)
{
    prefs_initialized = false;
    g_free(prefs.saved_at_version);
    prefs.saved_at_version = NULL;

    /*
     * Unload all UAT preferences.
     */
    uat_unload_all();

    /*
     * Unload any loaded MIBs.
     */
    oids_cleanup();

    /*
     * Reset the non-dissector preferences.
     */
    init_prefs();

    /*
     * Reset the non-UAT dissector preferences.
     */
    wmem_tree_foreach(prefs_modules, reset_module_prefs, NULL);
}

#ifdef _WIN32
static void
read_registry(void)
{
    HKEY hTestKey;
    DWORD data;
    DWORD data_size = sizeof(DWORD);
    DWORD ret;

    ret = RegOpenKeyExA(HKEY_CURRENT_USER, REG_HKCU_WIRESHARK_KEY, 0, KEY_READ, &hTestKey);
    if (ret != ERROR_SUCCESS && ret != ERROR_FILE_NOT_FOUND) {
        ws_noisy("Cannot open HKCU "REG_HKCU_WIRESHARK_KEY": 0x%lx", ret);
        return;
    }

    ret = RegQueryValueExA(hTestKey, LOG_HKCU_CONSOLE_OPEN, NULL, NULL, (LPBYTE)&data, &data_size);
    if (ret == ERROR_SUCCESS) {
        ws_log_console_open = (ws_log_console_open_pref)data;
        ws_noisy("Got "LOG_HKCU_CONSOLE_OPEN" from Windows registry: %d", ws_log_console_open);
    }
    else if (ret != ERROR_FILE_NOT_FOUND) {
        ws_noisy("Error reading registry key "LOG_HKCU_CONSOLE_OPEN": 0x%lx", ret);
    }

    RegCloseKey(hTestKey);
}
#endif

void
prefs_read_module(const char *module)
{
    int         err;
    char        *pf_path;
    FILE        *pf;

    module_t *target_module = prefs_find_module(module);
    if (!target_module) {
        return;
    }

    /* Construct the pathname of the user's preferences file for the module. */
    char *pf_name = wmem_strdup_printf(NULL, "%s.cfg", module);
    pf_path = get_persconffile_path(pf_name, true);
    wmem_free(NULL, pf_name);

    /* Read the user's module preferences file, if it exists and is not a dir. */
    if (!test_for_regular_file(pf_path) || ((pf = ws_fopen(pf_path, "r")) == NULL)) {
        g_free(pf_path);
        /* Fall back to the user's generic preferences file. */
        pf_path = get_persconffile_path(PF_NAME, true);
        pf = ws_fopen(pf_path, "r");
    }

    if (pf != NULL) {
        /* We succeeded in opening it; read it. */
        err = read_prefs_file(pf_path, pf, set_pref, target_module);
        if (err != 0) {
            /* We had an error reading the file; report it. */
            report_warning("Error reading your preferences file \"%s\": %s.",
                           pf_path, g_strerror(err));
        } else
            g_free(pf_path);
        fclose(pf);
    } else {
        /* We failed to open it.  If we failed for some reason other than
           "it doesn't exist", return the errno and the pathname, so our
           caller can report the error. */
        if (errno != ENOENT) {
            report_warning("Can't open your preferences file \"%s\": %s.",
                           pf_path, g_strerror(errno));
        } else
            g_free(pf_path);
    }

    return;
}

/* Read the preferences file, fill in "prefs", and return a pointer to it.

   If we got an error (other than "it doesn't exist") we report it through
   the UI. */
e_prefs *
read_prefs(void)
{
    int         err;
    char        *pf_path;
    FILE        *pf;

    /* clean up libsmi structures before reading prefs */
    oids_cleanup();

    init_prefs();

#ifdef _WIN32
    read_registry();
#endif

    /*
     * If we don't already have the pathname of the global preferences
     * file, construct it.  Then, in either case, try to open the file.
     */
    if (gpf_path == NULL) {
        /*
         * We don't have the path; try the new path first, and, if that
         * file doesn't exist, try the old path.
         */
        gpf_path = get_datafile_path(PF_NAME);
        if ((pf = ws_fopen(gpf_path, "r")) == NULL && errno == ENOENT) {
            /*
             * It doesn't exist by the new name; try the old name.
             */
            g_free(gpf_path);
            gpf_path = get_datafile_path(OLD_GPF_NAME);
            pf = ws_fopen(gpf_path, "r");
        }
    } else {
        /*
         * We have the path; try it.
         */
        pf = ws_fopen(gpf_path, "r");
    }

    /*
     * If we were able to open the file, read it.
     * XXX - if it failed for a reason other than "it doesn't exist",
     * report the error.
     */
    if (pf != NULL) {
        /*
         * Start out the counters of "mgcp.{tcp,udp}.port" entries we've
         * seen.
         */
        mgcp_tcp_port_count = 0;
        mgcp_udp_port_count = 0;

        /* We succeeded in opening it; read it. */
        err = read_prefs_file(gpf_path, pf, set_pref, NULL);
        if (err != 0) {
            /* We had an error reading the file; report it. */
            report_warning("Error reading global preferences file \"%s\": %s.",
                           gpf_path, g_strerror(err));
        }
        fclose(pf);
    } else {
        /* We failed to open it.  If we failed for some reason other than
           "it doesn't exist", report the error. */
        if (errno != ENOENT) {
            if (errno != 0) {
                report_warning("Can't open global preferences file \"%s\": %s.",
                               gpf_path, g_strerror(errno));
            }
        }
    }

    /* Construct the pathname of the user's preferences file. */
    pf_path = get_persconffile_path(PF_NAME, true);

    /* Read the user's preferences file, if it exists. */
    if ((pf = ws_fopen(pf_path, "r")) != NULL) {
        /*
         * Start out the counters of "mgcp.{tcp,udp}.port" entries we've
         * seen.
         */
        mgcp_tcp_port_count = 0;
        mgcp_udp_port_count = 0;

        /* We succeeded in opening it; read it. */
        err = read_prefs_file(pf_path, pf, set_pref, NULL);
        if (err != 0) {
            /* We had an error reading the file; report it. */
            report_warning("Error reading your preferences file \"%s\": %s.",
                           pf_path, g_strerror(err));
        } else
            g_free(pf_path);
        fclose(pf);
    } else {
        /* We failed to open it.  If we failed for some reason other than
           "it doesn't exist", return the errno and the pathname, so our
           caller can report the error. */
        if (errno != ENOENT) {
            report_warning("Can't open your preferences file \"%s\": %s.",
                           pf_path, g_strerror(errno));
        } else
            g_free(pf_path);
    }

    /* load SMI modules if needed */
    oids_init();

    return &prefs;
}

/* read the preferences file (or similar) and call the callback
 * function to set each key/value pair found */
int
read_prefs_file(const char *pf_path, FILE *pf,
                pref_set_pair_cb pref_set_pair_fct, void *private_data)
{
    enum {
        START,    /* beginning of a line */
        IN_VAR,   /* processing key name */
        PRE_VAL,  /* finished processing key name, skipping white space before value */
        IN_VAL,   /* processing value */
        IN_SKIP   /* skipping to the end of the line */
    } state = START;
    int       got_c;
    GString  *cur_val;
    GString  *cur_var;
    bool      got_val = false;
    int       fline = 1, pline = 1;
    char      hint[] = "(save preferences to remove this warning)";
    char      ver[128];

    cur_val = g_string_new("");
    cur_var = g_string_new("");

    /* Try to read in the profile name in the first line of the preferences file. */
    if (fscanf(pf, "# Configuration file for %127[^\r\n]", ver) == 1) {
        /* Assume trailing period and remove it */
        g_free(prefs.saved_at_version);
        prefs.saved_at_version = g_strndup(ver, strlen(ver) - 1);
    }
    rewind(pf);

    while ((got_c = ws_getc_unlocked(pf)) != EOF) {
        if (got_c == '\r') {
            /* Treat CR-LF at the end of a line like LF, so that if we're reading
             * a Windows-format file on UN*X, we handle it the same way we'd handle
             * a UN*X-format file. */
            got_c = ws_getc_unlocked(pf);
            if (got_c == EOF)
                break;
            if (got_c != '\n') {
                /* Put back the character after the CR, and process the CR normally. */
                ungetc(got_c, pf);
                got_c = '\r';
            }
        }
        if (got_c == '\n') {
            state = START;
            fline++;
            continue;
        }

        switch (state) {
        case START:
            if (g_ascii_isalnum(got_c)) {
                if (cur_var->len > 0) {
                    if (got_val) {
                        if (cur_val->len > 0) {
                            if (cur_val->str[cur_val->len-1] == ',') {
                                /*
                                 * If the pref has a trailing comma, eliminate it.
                                 */
                                cur_val->str[cur_val->len-1] = '\0';
                                ws_warning("%s line %d: trailing comma in \"%s\" %s", pf_path, pline, cur_var->str, hint);
                            }
                        }
                        /* Call the routine to set the preference; it will parse
                           the value as appropriate.

                           Since we're reading a file, rather than processing
                           explicit user input, for range preferences, silently
                           lower values in excess of the range's maximum, rather
                           than reporting errors and failing. */
                        switch (pref_set_pair_fct(cur_var->str, cur_val->str, private_data, false)) {

                        case PREFS_SET_OK:
                            break;

                        case PREFS_SET_SYNTAX_ERR:
                            report_warning("Syntax error in preference \"%s\" at line %d of\n%s %s",
                                       cur_var->str, pline, pf_path, hint);
                            break;

                        case PREFS_SET_NO_SUCH_PREF:
                            ws_warning("No such preference \"%s\" at line %d of\n%s %s",
                                       cur_var->str, pline, pf_path, hint);
                            prefs.unknown_prefs = true;
                            break;

                        case PREFS_SET_OBSOLETE:
                            /*
                             * If an attempt is made to save the
                             * preferences, a popup warning will be
                             * displayed stating that obsolete prefs
                             * have been detected and the user will
                             * be given the opportunity to save these
                             * prefs under a different profile name.
                             * The prefs in question need to be listed
                             * in the console window so that the
                             * user can make an informed choice.
                             */
                            ws_warning("Obsolete preference \"%s\" at line %d of\n%s %s",
                                       cur_var->str, pline, pf_path, hint);
                            prefs.unknown_prefs = true;
                            break;
                        }
                    } else {
                        ws_warning("Incomplete preference at line %d: of\n%s %s", pline, pf_path, hint);
                    }
                }
                state      = IN_VAR;
                got_val    = false;
                g_string_truncate(cur_var, 0);
                g_string_append_c(cur_var, (char) got_c);
                pline = fline;
            } else if (g_ascii_isspace(got_c) && cur_var->len > 0 && got_val) {
                state = PRE_VAL;
            } else if (got_c == '#') {
                state = IN_SKIP;
            } else {
                ws_warning("Malformed preference at line %d of\n%s %s", fline, pf_path, hint);
            }
            break;
        case IN_VAR:
            if (got_c != ':') {
                g_string_append_c(cur_var, (char) got_c);
            } else {
                /* This is a colon (':') */
                state   = PRE_VAL;
                g_string_truncate(cur_val, 0);
                /*
                 * Set got_val to true to accommodate prefs such as
                 * "gui.fileopen.dir" that do not require a value.
                 */
                got_val = true;
            }
            break;
        case PRE_VAL:
            if (!g_ascii_isspace(got_c)) {
                state = IN_VAL;
                g_string_append_c(cur_val, (char) got_c);
            }
            break;
        case IN_VAL:
            g_string_append_c(cur_val, (char) got_c);
            break;
        case IN_SKIP:
            break;
        }
    }
    if (cur_var->len > 0) {
        if (got_val) {
            /* Call the routine to set the preference; it will parse
               the value as appropriate.

               Since we're reading a file, rather than processing
               explicit user input, for range preferences, silently
               lower values in excess of the range's maximum, rather
               than reporting errors and failing. */
            switch (pref_set_pair_fct(cur_var->str, cur_val->str, private_data, false)) {

            case PREFS_SET_OK:
                break;

            case PREFS_SET_SYNTAX_ERR:
                ws_warning("Syntax error in preference %s at line %d of\n%s %s",
                           cur_var->str, pline, pf_path, hint);
                break;

            case PREFS_SET_NO_SUCH_PREF:
                ws_warning("No such preference \"%s\" at line %d of\n%s %s",
                           cur_var->str, pline, pf_path, hint);
                prefs.unknown_prefs = true;
                break;

            case PREFS_SET_OBSOLETE:
                prefs.unknown_prefs = true;
                break;
            }
        } else {
            ws_warning("Incomplete preference at line %d of\n%s %s",
                       pline, pf_path, hint);
        }
    }

    g_string_free(cur_val, TRUE);
    g_string_free(cur_var, TRUE);

    if (ferror(pf))
        return errno;
    else
        return 0;
}

/*
 * If we were handed a preference starting with "uat:", try to turn it into
 * a valid uat entry.
 */
static bool
prefs_set_uat_pref(char *uat_entry, char **errmsg) {
    char *p, *colonp;
    uat_t *uat;
    bool ret;

    colonp = strchr(uat_entry, ':');
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

    uat = uat_find(uat_entry);
    *colonp = ':';
    if (uat == NULL) {
        *errmsg = g_strdup("Unknown preference");
        return false;
    }

    ret = uat_load_str(uat, p, errmsg);
    return ret;
}

/*
 * Given a string of the form "<pref name>:<pref value>", as might appear
 * as an argument to a "-o" option, parse it and set the preference in
 * question.  Return an indication of whether it succeeded or failed
 * in some fashion.
 */
prefs_set_pref_e
prefs_set_pref(char *prefarg, char **errmsg)
{
    char *p, *colonp;
    prefs_set_pref_e ret;

    /*
     * Set the counters of "mgcp.{tcp,udp}.port" entries we've
     * seen to values that keep us from trying to interpret them
     * as "mgcp.{tcp,udp}.gateway_port" or "mgcp.{tcp,udp}.callagent_port",
     * as, from the command line, we have no way of guessing which
     * the user had in mind.
     */
    mgcp_tcp_port_count = -1;
    mgcp_udp_port_count = -1;

    *errmsg = NULL;

    colonp = strchr(prefarg, ':');
    if (colonp == NULL)
        return PREFS_SET_SYNTAX_ERR;

    p = colonp;
    *p++ = '\0';

    /*
     * Skip over any white space (there probably won't be any, but
     * as we allow it in the preferences file, we might as well
     * allow it here).
     */
    while (g_ascii_isspace(*p))
        p++;
    /* The empty string is a legal value for range preferences (PREF_RANGE,
     * PREF_DECODE_AS_RANGE), and string-like preferences (PREF_STRING,
     * PREF_SAVE_FILENAME, PREF_OPEN_FILENAME, PREF_DIRNAME), indeed often
     * not just useful but the default. A user might have a value saved
     * to their preference file but want to override it to default behavior.
     * Individual preference handlers of those types should be prepared to
     * deal with an empty string. For other types, it is up to set_pref() to
     * test for the empty string and set PREFS_SET_SYNTAX_ERROR there.
     */
    if (strcmp(prefarg, "uat")) {
        ret = set_pref(prefarg, p, NULL, true);
    } else {
        ret = prefs_set_uat_pref(p, errmsg) ? PREFS_SET_OK : PREFS_SET_SYNTAX_ERR;
    }
    *colonp = ':';    /* put the colon back */
    return ret;
}

unsigned prefs_get_uint_value(pref_t *pref, pref_source_t source)
{
    switch (source)
    {
    case pref_default:
        return pref->default_val.uint;
    case pref_stashed:
        return pref->stashed_val.uint;
    case pref_current:
        return *pref->varp.uint;
    default:
        ws_assert_not_reached();
        break;
    }

    return 0;
}

char* prefs_get_password_value(pref_t *pref, pref_source_t source)
{
    return prefs_get_string_value(pref, source);
}


unsigned int prefs_set_uint_value(pref_t *pref, unsigned value, pref_source_t source)
{
    unsigned int changed = 0;
    switch (source)
    {
    case pref_default:
        if (pref->default_val.uint != value) {
            pref->default_val.uint = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    case pref_stashed:
        if (pref->stashed_val.uint != value) {
            pref->stashed_val.uint = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    case pref_current:
        if (*pref->varp.uint != value) {
            *pref->varp.uint = value;
            changed = prefs_get_effect_flags(pref);
        }
        break;
    default:
        ws_assert_not_reached();
        break;
    }

    return changed;
}

/*
 * For use by UI code that sets preferences.
 */
unsigned int
prefs_set_password_value(pref_t *pref, const char* value, pref_source_t source)
{
    return prefs_set_string_value(pref, value, source);
}


unsigned prefs_get_uint_base(pref_t *pref)
{
    return pref->info.base;
}

/*
 * Returns true if the given device is hidden
 */
bool
prefs_is_capture_device_hidden(const char *name)
{
    char *tok, *devices;
    size_t len;

    if (prefs.capture_devices_hide && name) {
        devices = g_strdup (prefs.capture_devices_hide);
        len = strlen (name);
        for (tok = strtok (devices, ","); tok; tok = strtok(NULL, ",")) {
            if (strlen (tok) == len && strcmp (name, tok) == 0) {
                g_free (devices);
                return true;
            }
        }
        g_free (devices);
    }

    return false;
}

/*
 * Returns true if the given column is visible (not hidden)
 */
static bool
prefs_is_column_visible(const char *cols_hidden, int col)
{
    char *tok, *cols, *p;
    int cidx;

    /*
     * Do we have a list of hidden columns?
     */
    if (cols_hidden) {
        /*
         * Yes - check the column against each of the ones in the
         * list.
         */
        cols = g_strdup(cols_hidden);
        for (tok = strtok(cols, ","); tok; tok = strtok(NULL, ",")) {
            tok = g_strstrip(tok);

            cidx = (int)strtol(tok, &p, 10);
            if (p == tok || *p != '\0') {
                continue;
            }
            if (cidx != col) {
                continue;
            }
            /*
             * OK, they match, so it's one of the hidden fields,
             * hence not visible.
             */
            g_free(cols);
            return false;
        }
        g_free(cols);
    }

    /*
     * No - either there are no hidden columns or this isn't one
     * of them - so it is visible.
     */
    return true;
}

/*
 * Returns true if the given column is visible (not hidden)
 */
static bool
prefs_is_column_fmt_visible(const char *cols_hidden, fmt_data *cfmt)
{
    char *tok, *cols;
    fmt_data cfmt_hidden;

    /*
     * Do we have a list of hidden columns?
     */
    if (cols_hidden) {
        /*
         * Yes - check the column against each of the ones in the
         * list.
         */
        cols = g_strdup(cols_hidden);
        for (tok = strtok(cols, ","); tok; tok = strtok(NULL, ",")) {
            tok = g_strstrip(tok);

            /*
             * Parse this column format.
             */
            if (!parse_column_format(&cfmt_hidden, tok)) {
                /*
                 * It's not valid; ignore it.
                 */
                continue;
            }

            /*
             * Does it match the column?
             */
            if (cfmt->fmt != cfmt_hidden.fmt) {
                /* No. */
                g_free(cfmt_hidden.custom_fields);
                cfmt_hidden.custom_fields = NULL;
                continue;
            }
            if (cfmt->fmt == COL_CUSTOM) {
                /*
                 * A custom column has to have the same custom field
                 * and occurrence.
                 */
                if (cfmt_hidden.custom_fields && cfmt->custom_fields) {
                    if (strcmp(cfmt->custom_fields,
                               cfmt_hidden.custom_fields) != 0) {
                        /* Different fields. */
                        g_free(cfmt_hidden.custom_fields);
                        cfmt_hidden.custom_fields = NULL;
                        continue;
                    }
                    if (cfmt->custom_occurrence != cfmt_hidden.custom_occurrence) {
                        /* Different occurrences settings. */
                        g_free(cfmt_hidden.custom_fields);
                        cfmt_hidden.custom_fields = NULL;
                        continue;
                    }
                }
            }

            /*
             * OK, they match, so it's one of the hidden fields,
             * hence not visible.
             */
            g_free(cfmt_hidden.custom_fields);
            g_free(cols);
            return false;
        }
        g_free(cols);
    }

    /*
     * No - either there are no hidden columns or this isn't one
     * of them - so it is visible.
     */
    return true;
}

/*
 * Returns true if the given device should capture in monitor mode by default
 */
bool
prefs_capture_device_monitor_mode(const char *name)
{
    char *tok, *devices;
    size_t len;

    if (prefs.capture_devices_monitor_mode && name) {
        devices = g_strdup (prefs.capture_devices_monitor_mode);
        len = strlen (name);
        for (tok = strtok (devices, ","); tok; tok = strtok(NULL, ",")) {
            if (strlen (tok) == len && strcmp (name, tok) == 0) {
                g_free (devices);
                return true;
            }
        }
        g_free (devices);
    }

    return false;
}

/*
 * Returns true if the user has marked this column as visible
 */
bool
prefs_capture_options_dialog_column_is_visible(const char *column)
{
    GList *curr;
    char *col;

    for (curr = g_list_first(prefs.capture_columns); curr; curr = g_list_next(curr)) {
        col = (char *)curr->data;
        if (col && (g_ascii_strcasecmp(col, column) == 0)) {
            return true;
        }
    }
    return false;
}

bool
prefs_has_layout_pane_content (layout_pane_content_e layout_pane_content)
{
    return ((prefs.gui_layout_content_1 == layout_pane_content) ||
            (prefs.gui_layout_content_2 == layout_pane_content) ||
            (prefs.gui_layout_content_3 == layout_pane_content));
}

#define PRS_GUI_FILTER_LABEL             "gui.filter_expressions.label"
#define PRS_GUI_FILTER_EXPR              "gui.filter_expressions.expr"
#define PRS_GUI_FILTER_ENABLED           "gui.filter_expressions.enabled"

/*
 * Extract the red, green, and blue components of a 24-bit RGB value
 * and convert them from [0,255] to [0,65535].
 */
#define RED_COMPONENT(x)   (uint16_t) (((((x) >> 16) & 0xff) * 65535 / 255))
#define GREEN_COMPONENT(x) (uint16_t) (((((x) >>  8) & 0xff) * 65535 / 255))
#define BLUE_COMPONENT(x)  (uint16_t) ( (((x)        & 0xff) * 65535 / 255))

char
string_to_name_resolve(const char *string, e_addr_resolve *name_resolve)
{
    char c;

    memset(name_resolve, 0, sizeof(e_addr_resolve));
    while ((c = *string++) != '\0') {
        switch (c) {
        case 'g':
            name_resolve->maxmind_geoip = true;
            break;
        case 'm':
            name_resolve->mac_name = true;
            break;
        case 'n':
            name_resolve->network_name = true;
            break;
        case 'N':
            name_resolve->use_external_net_name_resolver = true;
            break;
        case 't':
            name_resolve->transport_name = true;
            break;
        case 'd':
            name_resolve->dns_pkt_addr_resolution = true;
            break;
        case 's':
            name_resolve->handshake_sni_addr_resolution = true;
            break;
        case 'v':
            name_resolve->vlan_name = true;
            break;
        default:
            /*
             * Unrecognized letter.
             */
            return c;
        }
    }
    return '\0';
}

static bool
deprecated_heur_dissector_pref(char *pref_name, const char *value)
{
    struct heur_pref_name
    {
        const char* pref_name;
        const char* short_name;
        bool      more_dissectors; /* For multiple dissectors controlled by the same preference */
    };

    struct heur_pref_name heur_prefs[] = {
        {"acn.heuristic_acn", "acn_udp", 0},
        {"bfcp.enable", "bfcp_tcp", 1},
        {"bfcp.enable", "bfcp_udp", 0},
        {"bt-dht.enable", "bittorrent_dht_udp", 0},
        {"bt-utp.enable", "bt_utp_udp", 0},
        {"cattp.enable", "cattp_udp", 0},
        {"cfp.enable", "fp_eth", 0},
        {"dicom.heuristic", "dicom_tcp", 0},
        {"dnp3.heuristics", "dnp3_tcp", 1},
        {"dnp3.heuristics", "dnp3_udp", 0},
        {"dvb-s2_modeadapt.enable", "dvb_s2_udp", 0},
        {"esl.enable", "esl_eth", 0},
        {"fp.udp_heur", "fp_udp", 0},
        {"gvsp.enable_heuristic", "gvsp_udp", 0},
        {"hdcp2.enable", "hdcp2_tcp", 0},
        {"hislip.enable_heuristic", "hislip_tcp", 0},
        {"infiniband.dissect_eoib", "mellanox_eoib", 1},
        {"infiniband.identify_payload", "eth_over_ib", 0},
        {"jxta.udp.heuristic", "jxta_udp", 0},
        {"jxta.tcp.heuristic", "jxta_tcp", 0},
        {"jxta.sctp.heuristic", "jxta_sctp", 0},
        {"mac-lte.heuristic_mac_lte_over_udp", "mac_lte_udp", 0},
        {"mbim.bulk_heuristic", "mbim_usb_bulk", 0},
        {"norm.heuristic_norm", "rmt_norm_udp", 0},
        {"openflow.heuristic", "openflow_tcp", 0},
        {"pdcp-lte.heuristic_pdcp_lte_over_udp", "pdcp_lte_udp", 0},
        {"rlc.heuristic_rlc_over_udp", "rlc_udp", 0},
        {"rlc-lte.heuristic_rlc_lte_over_udp", "rlc_lte_udp", 0},
        {"rtcp.heuristic_rtcp", "rtcp_udp", 1},
        {"rtcp.heuristic_rtcp", "rtcp_stun", 0},
        {"rtp.heuristic_rtp", "rtp_udp", 1},
        {"rtp.heuristic_rtp", "rtp_stun", 0},
        {"teredo.heuristic_teredo", "teredo_udp", 0},
        {"vssmonitoring.use_heuristics", "vssmonitoring_eth", 0},
        {"xml.heuristic", "xml_http", 1},
        {"xml.heuristic", "xml_sip", 1},
        {"xml.heuristic", "xml_media", 0},
        {"xml.heuristic_tcp", "xml_tcp", 0},
        {"xml.heuristic_udp", "xml_udp", 0},
    };

    unsigned int i;
    heur_dtbl_entry_t* heuristic;


    for (i = 0; i < array_length(heur_prefs); i++)
    {
        if (strcmp(pref_name, heur_prefs[i].pref_name) == 0)
        {
            heuristic = find_heur_dissector_by_unique_short_name(heur_prefs[i].short_name);
            if (heuristic != NULL) {
                heuristic->enabled = ((g_ascii_strcasecmp(value, "true") == 0) ? true : false);
            }

            if (!heur_prefs[i].more_dissectors)
                return true;
        }
    }


    return false;
}

static bool
deprecated_enable_dissector_pref(char *pref_name, const char *value)
{
    struct dissector_pref_name
    {
        const char* pref_name;
        const char* short_name;
    };

    struct dissector_pref_name dissector_prefs[] = {
        {"transum.tsumenabled", "TRANSUM"},
        {"snort.enable_snort_dissector", "Snort"},
        {"prp.enable", "PRP"},
    };

    unsigned int i;
    int proto_id;

    for (i = 0; i < array_length(dissector_prefs); i++)
    {
        if (strcmp(pref_name, dissector_prefs[i].pref_name) == 0)
        {
            proto_id = proto_get_id_by_short_name(dissector_prefs[i].short_name);
            if (proto_id >= 0)
                proto_set_decoding(proto_id, ((g_ascii_strcasecmp(value, "true") == 0) ? true : false));
            return true;
        }
    }

    return false;
}

static bool
deprecated_port_pref(char *pref_name, const char *value)
{
    struct port_pref_name
    {
        const char* pref_name;
        const char* module_name;    /* the protocol filter name */
        const char* table_name;
        unsigned base;
    };

    struct obsolete_pref_name
    {
        const char* pref_name;
    };

    /* For now this is only supporting TCP/UDP port and RTP payload
     * types dissector preferences, which are assumed to be decimal */
    /* module_name is the filter name of the destination port preference,
     * which is usually the same as the original module but not
     * necessarily (e.g., if the preference is for what is now a PINO.)
     * XXX:  Most of these were changed pre-2.0. Can we end support
     * for migrating legacy preferences at some point?
     */
    struct port_pref_name port_prefs[] = {
        /* TCP */
        {"cmp.tcp_alternate_port", "cmp", "tcp.port", 10},
        {"h248.tcp_port", "h248", "tcp.port", 10},
        {"cops.tcp.cops_port", "cops", "tcp.port", 10},
        {"dhcpfo.tcp_port", "dhcpfo", "tcp.port", 10},
        {"enttec.tcp_port", "enttec", "tcp.port", 10},
        {"forces.tcp_alternate_port", "forces", "tcp.port", 10},
        {"ged125.tcp_port", "ged125", "tcp.port", 10},
        {"hpfeeds.dissector_port", "hpfeeds", "tcp.port", 10},
        {"lsc.port", "lsc", "tcp.port", 10},
        {"megaco.tcp.txt_port", "megaco", "tcp.port", 10},
        {"netsync.tcp_port", "netsync", "tcp.port", 10},
        {"osi.tpkt_port", "osi", "tcp.port", 10},
        {"rsync.tcp_port", "rsync", "tcp.port", 10},
        {"sametime.tcp_port", "sametime", "tcp.port", 10},
        {"sigcomp.tcp.port2", "sigcomp", "tcp.port", 10},
        {"synphasor.tcp_port", "synphasor", "tcp.port", 10},
        {"tipc.alternate_port", "tipc", "tcp.port", 10},
        {"vnc.alternate_port", "vnc", "tcp.port", 10},
        {"scop.port", "scop", "tcp.port", 10},
        {"scop.port_secure", "scop", "tcp.port", 10},
        {"tpncp.tcp.trunkpack_port", "tpncp", "tcp.port", 10},
        /* UDP */
        {"h248.udp_port", "h248", "udp.port", 10},
        {"actrace.udp_port", "actrace", "udp.port", 10},
        {"brp.port", "brp", "udp.port", 10},
        {"bvlc.additional_udp_port", "bvlc", "udp.port", 10},
        {"capwap.udp.port.control", "capwap", "udp.port", 10},
        {"capwap.udp.port.data", "capwap", "udp.port", 10},
        {"coap.udp_port", "coap", "udp.port", 10},
        {"enttec.udp_port", "enttec", "udp.port", 10},
        {"forces.udp_alternate_port", "forces", "udp.port", 10},
        {"ldss.udp_port", "ldss", "udp.port", 10},
        {"lmp.udp_port", "lmp", "udp.port", 10},
        {"ltp.port", "ltp", "udp.port", 10},
        {"lwres.udp.lwres_port", "lwres", "udp.port", 10},
        {"megaco.udp.txt_port", "megaco", "udp.port", 10},
        {"pfcp.port_pfcp", "pfcp", "udp.port", 10},
        {"pgm.udp.encap_ucast_port", "pgm", "udp.port", 10},
        {"pgm.udp.encap_mcast_port", "pgm", "udp.port", 10},
        {"quic.udp.quic.port", "quic", "udp.port", 10},
        {"quic.udp.quics.port", "quic", "udp.port", 10},
        {"radius.alternate_port", "radius", "udp.port", 10},
        {"rdt.default_udp_port", "rdt", "udp.port", 10},
        {"alc.default.udp_port", "alc", "udp.port", 10},
        {"sigcomp.udp.port2", "sigcomp", "udp.port", 10},
        {"synphasor.udp_port", "synphasor", "udp.port", 10},
        {"tdmop.udpport", "tdmop", "udp.port", 10},
        {"uaudp.port1", "uaudp", "udp.port", 10},
        {"uaudp.port2", "uaudp", "udp.port", 10},
        {"uaudp.port3", "uaudp", "udp.port", 10},
        {"uaudp.port4", "uaudp", "udp.port", 10},
        {"uhd.dissector_port", "uhd", "udp.port", 10},
        {"vrt.dissector_port", "vrt", "udp.port", 10},
        {"tpncp.udp.trunkpack_port", "tpncp", "udp.port", 10},
        /* SCTP */
        {"hnbap.port", "hnbap", "sctp.port", 10},
        {"m2pa.port", "m2pa", "sctp.port", 10},
        {"megaco.sctp.txt_port", "megaco", "sctp.port", 10},
        {"rua.port", "rua", "sctp.port", 10},
        /* SCTP PPI */
        {"lapd.sctp_payload_protocol_identifier", "lapd", "sctp.ppi", 10},
        /* SCCP SSN */
        {"ranap.sccp_ssn", "ranap", "sccp.ssn", 10},
    };

    struct port_pref_name port_range_prefs[] = {
        /* TCP */
        {"couchbase.tcp.ports", "couchbase", "tcp.port", 10},
        {"gsm_ipa.tcp_ports", "gsm_ipa", "tcp.port", 10},
        {"kafka.tcp.ports", "kafka", "tcp.port", 10},
        {"kt.tcp.ports", "kt", "tcp.port", 10},
        {"memcache.tcp.ports", "memcache", "tcp.port", 10},
        {"mrcpv2.tcp.port_range", "mrcpv2", "tcp.port", 10},
        {"pdu_transport.ports.tcp", "pdu_transport", "tcp.port", 10},
        {"rtsp.tcp.port_range", "rtsp", "tcp.port", 10},
        {"sip.tcp.ports", "sip", "tcp.port", 10},
        {"someip.ports.tcp", "someip", "tcp.port", 10},
        {"tds.tcp_ports", "tds", "tcp.port", 10},
        {"tpkt.tcp.ports", "tpkt", "tcp.port", 10},
        {"uma.tcp.ports", "uma", "tcp.port", 10},
        /* UDP */
        {"aruba_erm.udp.ports", "arubs_erm", "udp.port", 10},
        {"diameter.udp.ports", "diameter", "udp.port", 10},
        {"dmp.udp_ports", "dmp", "udp.port", 10},
        {"dns.udp.ports", "dns", "udp.port", 10},
        {"gsm_ipa.udp_ports", "gsm_ipa", "udp.port", 10},
        {"hcrt.dissector_udp_port", "hcrt", "udp.port", 10},
        {"memcache.udp.ports", "memcache", "udp.port", 10},
        {"nb_rtpmux.udp_ports", "nb_rtpmux", "udp.port", 10},
        {"gprs-ns.udp.ports", "gprs-ns", "udp.port", 10},
        {"p_mul.udp_ports", "p_mul", "udp.port", 10},
        {"pdu_transport.ports.udp", "pdu_transport", "udp.port", 10},
        {"radius.ports", "radius", "udp.port", 10},
        {"sflow.ports", "sflow", "udp.port", 10},
        {"someip.ports.udp", "someip", "udp.port", 10},
        {"sscop.udp.ports", "sscop", "udp.port", 10},
        {"tftp.udp_ports", "tftp", "udp.port", 10},
        {"tipc.udp.ports", "tipc", "udp.port", 10},
        /* RTP */
        {"amr.dynamic.payload.type", "amr", "rtp.pt", 10},
        {"amr.wb.dynamic.payload.type", "amr_wb", "rtp.pt", 10},
        {"dvb-s2_modeadapt.dynamic.payload.type", "dvb-s2_modeadapt", "rtp.pt", 10},
        {"evs.dynamic.payload.type", "evs", "rtp.pt", 10},
        {"h263p.dynamic.payload.type", "h263p", "rtp.pt", 10},
        {"h264.dynamic.payload.type", "h264", "rtp.pt", 10},
        {"h265.dynamic.payload.type", "h265", "rtp.pt", 10},
        {"ismacryp.dynamic.payload.type", "ismacryp", "rtp.pt", 10},
        {"iuup.dynamic.payload.type", "iuup", "rtp.pt", 10},
        {"lapd.rtp_payload_type", "lapd", "rtp.pt", 10},
        {"mp4ves.dynamic.payload.type", "mp4ves", "rtp.pt", 10},
        {"mtp2.rtp_payload_type", "mtp2", "rtp.pt", 10},
        {"opus.dynamic.payload.type", "opus", "rtp.pt", 10},
        {"rtp.rfc2198_payload_type", "rtp_rfc2198", "rtp.pt", 10},
        {"rtpevent.event_payload_type_value", "rtpevent", "rtp.pt", 10},
        {"rtpevent.cisco_nse_payload_type_value", "rtpevent", "rtp.pt", 10},
        {"rtpmidi.midi_payload_type_value", "rtpmidi", "rtp.pt", 10},
        {"vp8.dynamic.payload.type", "vp8", "rtp.pt", 10},
        /* SCTP */
        {"diameter.sctp.ports", "diameter", "sctp.port", 10},
        {"sgsap.sctp_ports", "sgsap", "sctp.port", 10},
        /* SCCP SSN */
        {"pcap.ssn", "pcap", "sccp.ssn", 10},
    };

    /* These are subdissectors of TPKT/OSITP that used to have a
       TCP port preference even though they were never
       directly on TCP.  Convert them to use Decode As
       with the TPKT dissector handle */
    struct port_pref_name tpkt_subdissector_port_prefs[] = {
        {"dap.tcp.port", "dap", "tcp.port", 10},
        {"disp.tcp.port", "disp", "tcp.port", 10},
        {"dop.tcp.port", "dop", "tcp.port", 10},
        {"dsp.tcp.port", "dsp", "tcp.port", 10},
        {"p1.tcp.port", "p1", "tcp.port", 10},
        {"p7.tcp.port", "p7", "tcp.port", 10},
        {"rdp.tcp.port", "rdp", "tcp.port", 10},
    };

    /* These are obsolete preferences from the dissectors' view,
       (typically because of a switch from a single value to a
       range value) but the name of the preference conflicts
       with the generated preference name from the dissector table.
       Don't allow the obsolete preference through to be handled */
    struct obsolete_pref_name obsolete_prefs[] = {
        {"diameter.tcp.port"},
        {"kafka.tcp.port"},
        {"mrcpv2.tcp.port"},
        {"rtsp.tcp.port"},
        {"sip.tcp.port"},
        {"t38.tcp.port"},
    };

    unsigned int i;
    unsigned uval;
    dissector_table_t sub_dissectors;
    dissector_handle_t handle, tpkt_handle;
    module_t *module;
    pref_t *pref;

    static bool sanity_checked;
    if (!sanity_checked) {
        sanity_checked = true;
        for (i = 0; i < G_N_ELEMENTS(port_prefs); i++) {
            module = prefs_find_module(port_prefs[i].module_name);
            if (!module) {
                ws_warning("Deprecated ports pref check - module '%s' not found", port_prefs[i].module_name);
                continue;
            }
            pref = prefs_find_preference(module, port_prefs[i].table_name);
            if (!pref) {
                ws_warning("Deprecated ports pref '%s.%s' not found", module->name, port_prefs[i].table_name);
                continue;
            }
            if (pref->type != PREF_DECODE_AS_RANGE) {
                ws_warning("Deprecated ports pref '%s.%s' has wrong type: %#x (%s)", module->name, port_prefs[i].table_name, pref->type, prefs_pref_type_name(pref));
            }
        }
    }

    for (i = 0; i < G_N_ELEMENTS(port_prefs); i++) {
        if (strcmp(pref_name, port_prefs[i].pref_name) == 0) {
            if (!ws_basestrtou32(value, NULL, &uval, port_prefs[i].base))
                return false;        /* number was bad */

            module = prefs_find_module(port_prefs[i].module_name);
            pref = prefs_find_preference(module, port_prefs[i].table_name);
            if (pref != NULL) {
                module->prefs_changed_flags |= prefs_get_effect_flags(pref);
                if (pref->type == PREF_DECODE_AS_RANGE) {
                    // The legacy preference was a port number, but the new
                    // preference is a port range. Add to existing range.
                    if (uval) {
                        prefs_range_add_value(pref, uval);
                    }
                }
            }

            /* If the value is zero, it wouldn't add to the Decode As tables */
            if (uval != 0)
            {
                sub_dissectors = find_dissector_table(port_prefs[i].table_name);
                if (sub_dissectors != NULL) {
                    handle = dissector_table_get_dissector_handle(sub_dissectors, module->title);
                    if (handle != NULL) {
                        dissector_change_uint(port_prefs[i].table_name, uval, handle);
                        decode_build_reset_list(port_prefs[i].table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(uval), NULL, NULL);
                    }
                }
            }

            return true;
        }
    }

    for (i = 0; i < array_length(port_range_prefs); i++)
    {
        if (strcmp(pref_name, port_range_prefs[i].pref_name) == 0)
        {
            uint32_t range_i, range_j;

            sub_dissectors = find_dissector_table(port_range_prefs[i].table_name);
            if (sub_dissectors != NULL) {
                switch (dissector_table_get_type(sub_dissectors)) {
                case FT_UINT8:
                case FT_UINT16:
                case FT_UINT24:
                case FT_UINT32:
                    break;

                default:
                    ws_error("The dissector table %s (%s) is not an integer type - are you using a buggy plugin?", port_range_prefs[i].table_name, get_dissector_table_ui_name(port_range_prefs[i].table_name));
                    ws_assert_not_reached();
                }

                module = prefs_find_module(port_range_prefs[i].module_name);
                pref = prefs_find_preference(module, port_range_prefs[i].table_name);
                if (pref != NULL)
                {
                    if (!prefs_set_range_value_work(pref, value, true, &module->prefs_changed_flags))
                    {
                        return false;        /* number was bad */
                    }

                    handle = dissector_table_get_dissector_handle(sub_dissectors, module->title);
                    if (handle != NULL) {

                        for (range_i = 0; range_i < (*pref->varp.range)->nranges; range_i++) {
                            for (range_j = (*pref->varp.range)->ranges[range_i].low; range_j < (*pref->varp.range)->ranges[range_i].high; range_j++) {
                                dissector_change_uint(port_range_prefs[i].table_name, range_j, handle);
                                decode_build_reset_list(port_range_prefs[i].table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(range_j), NULL, NULL);
                            }

                            dissector_change_uint(port_range_prefs[i].table_name, (*pref->varp.range)->ranges[range_i].high, handle);
                            decode_build_reset_list(port_range_prefs[i].table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER((*pref->varp.range)->ranges[range_i].high), NULL, NULL);
                        }
                    }
                }
            }

            return true;
        }
    }

    for (i = 0; i < array_length(tpkt_subdissector_port_prefs); i++)
    {
        if (strcmp(pref_name, tpkt_subdissector_port_prefs[i].pref_name) == 0)
        {
            /* XXX - give an error if it doesn't fit in a unsigned? */
            if (!ws_basestrtou32(value, NULL, &uval, tpkt_subdissector_port_prefs[i].base))
                return false;        /* number was bad */

            /* If the value is 0 or 102 (default TPKT port), don't add to the Decode As tables */
            if ((uval != 0) && (uval != 102))
            {
                tpkt_handle = find_dissector("tpkt");
                if (tpkt_handle != NULL) {
                    dissector_change_uint(tpkt_subdissector_port_prefs[i].table_name, uval, tpkt_handle);
                }
            }

            return true;
        }
    }

    for (i = 0; i < array_length(obsolete_prefs); i++)
    {
        if (strcmp(pref_name, obsolete_prefs[i].pref_name) == 0)
        {
            /* Just ignore the preference */
            return true;
        }
    }
    return false;
}

static prefs_set_pref_e
set_pref(char *pref_name, const char *value, void *private_data,
         bool return_range_errors)
{
    unsigned cval;
    unsigned uval;
    bool     bval;
    int      enum_val;
    char     *dotp, *last_dotp;
    static char *filter_label = NULL;
    static bool filter_enabled = false;
    module_t *module, *containing_module, *target_module;
    pref_t   *pref;
    bool converted_pref = false;

    target_module = (module_t*)private_data;

    //The PRS_GUI field names are here for backwards compatibility
    //display filters have been converted to a UAT.
    if (strcmp(pref_name, PRS_GUI_FILTER_LABEL) == 0) {
        /* Assume that PRS_GUI_FILTER_EXPR follows this preference. In case of
         * malicious preference files, free the previous value to limit the size
         * of leaked memory.  */
        g_free(filter_label);
        filter_label = g_strdup(value);
    } else if (strcmp(pref_name, PRS_GUI_FILTER_ENABLED) == 0) {
        filter_enabled = (strcmp(value, "TRUE") == 0) ? true : false;
    } else if (strcmp(pref_name, PRS_GUI_FILTER_EXPR) == 0) {
        /* Comments not supported for "old" preference style */
        filter_expression_new(filter_label, value, "", filter_enabled);
        g_free(filter_label);
        filter_label = NULL;
        /* Remember to save the new UAT to file. */
        prefs.filter_expressions_old = true;
    } else if (strcmp(pref_name, "gui.version_in_start_page") == 0) {
        /* Convert deprecated value to closest current equivalent */
        if (g_ascii_strcasecmp(value, "true") == 0) {
            prefs.gui_version_placement = version_both;
        } else {
            prefs.gui_version_placement = version_neither;
        }
    } else if (strcmp(pref_name, "name_resolve") == 0 ||
               strcmp(pref_name, "capture.name_resolve") == 0) {
        /*
         * Handle the deprecated name resolution options.
         *
         * "TRUE" and "FALSE", for backwards compatibility, are synonyms for
         * RESOLV_ALL and RESOLV_NONE.
         *
         * Otherwise, we treat it as a list of name types we want to resolve.
         */
        if (g_ascii_strcasecmp(value, "true") == 0) {
            gbl_resolv_flags.mac_name = true;
            gbl_resolv_flags.network_name = true;
            gbl_resolv_flags.transport_name = true;
        }
        else if (g_ascii_strcasecmp(value, "false") == 0) {
            disable_name_resolution();
        }
        else {
            /* start out with none set */
            disable_name_resolution();
            if (string_to_name_resolve(value, &gbl_resolv_flags) != '\0')
                return PREFS_SET_SYNTAX_ERR;
        }
    } else if (deprecated_heur_dissector_pref(pref_name, value)) {
         /* Handled within deprecated_heur_dissector_pref() if found */
    } else if (deprecated_enable_dissector_pref(pref_name, value)) {
         /* Handled within deprecated_enable_dissector_pref() if found */
    } else if (deprecated_port_pref(pref_name, value)) {
         /* Handled within deprecated_port_pref() if found */
    } else if (strcmp(pref_name, "console.log.level") == 0) {
        /* Handled on the command line within ws_log_parse_args() */
        return PREFS_SET_OK;
    } else {
        /* Handle deprecated "global" options that don't have a module
         * associated with them
         */
        if ((strcmp(pref_name, "name_resolve_concurrency") == 0) ||
            (strcmp(pref_name, "name_resolve_load_smi_modules") == 0)  ||
            (strcmp(pref_name, "name_resolve_suppress_smi_errors") == 0)) {
            module = nameres_module;
            dotp = pref_name;
        } else {
            /* To which module does this preference belong? */
            module = NULL;
            last_dotp = pref_name;
            while (!module) {
                dotp = strchr(last_dotp, '.');
                if (dotp == NULL) {
                    /* Either there's no such module, or no module was specified.
                       In either case, that means there's no such preference. */
                    return PREFS_SET_NO_SUCH_PREF;
                }
                *dotp = '\0'; /* separate module and preference name */
                module = prefs_find_module(pref_name);

                /*
                 * XXX - "Diameter" rather than "diameter" was used in earlier
                 * versions of Wireshark; if we didn't find the module, and its name
                 * was "Diameter", look for "diameter" instead.
                 *
                 * In addition, the BEEP protocol used to be the BXXP protocol,
                 * so if we didn't find the module, and its name was "bxxp",
                 * look for "beep" instead.
                 *
                 * Also, the preferences for GTP v0 and v1 were combined under
                 * a single "gtp" heading, and the preferences for SMPP were
                 * moved to "smpp-gsm-sms" and then moved to "gsm-sms-ud".
                 * However, SMPP now has its own preferences, so we just map
                 * "smpp-gsm-sms" to "gsm-sms-ud", and then handle SMPP below.
                 *
                 * We also renamed "dcp" to "dccp", "x.25" to "x25", "x411" to "p1"
                 * and "nsip" to "gprs_ns".
                 *
                 * The SynOptics Network Management Protocol (SONMP) is now known by
                 * its modern name, the Nortel Discovery Protocol (NDP).
                 */
                if (module == NULL) {
                    /*
                     * See if there's a backwards-compatibility name
                     * that maps to this module.
                     */
                    module = prefs_find_module_alias(pref_name);
                    if (module == NULL) {
                        /*
                         * There's no alias for the module; see if the
                         * module name matches any protocol aliases.
                         */
                        header_field_info *hfinfo = proto_registrar_get_byalias(pref_name);
                        if (hfinfo) {
                            module = (module_t *) wmem_tree_lookup_string(prefs_modules, hfinfo->abbrev, WMEM_TREE_STRING_NOCASE);
                        }
                    }
                    if (module == NULL) {
                        /*
                         * There aren't any aliases.  Was the module
                         * removed rather than renamed?
                         */
                        if (strcmp(pref_name, "etheric") == 0 ||
                            strcmp(pref_name, "isup_thin") == 0) {
                            /*
                             * The dissectors for these protocols were
                             * removed as obsolete on 2009-07-70 in change
                             * 739bfc6ff035583abb9434e0e988048de38a8d9a.
                             */
                            return PREFS_SET_OBSOLETE;
                        }
                    }
                    if (module) {
                        converted_pref = true;
                        prefs.unknown_prefs = true;
                    }
                }
                *dotp = '.';                /* put the preference string back */
                dotp++;                     /* skip past separator to preference name */
                last_dotp = dotp;
            }
        }

        /* The pref is located in the module or a submodule.
         * Assume module, then search for a submodule holding the pref. */
        containing_module = module;
        pref = prefs_find_preference_with_submodule(module, dotp, &containing_module);

        if (pref == NULL) {
            prefs.unknown_prefs = true;

            /* "gui" prefix was added to column preferences for better organization
             * within the preferences file
             */
            if (module == gui_column_module) {
                /* While this has a subtree, there is no apply callback, so no
                 * need to use prefs_find_preference_with_submodule to update
                 * containing_module. It would not be useful. */
                pref = prefs_find_preference(module, pref_name);
            }
            else if (strcmp(module->name, "mgcp") == 0) {
                /*
                 * XXX - "mgcp.display raw text toggle" and "mgcp.display dissect tree"
                 * rather than "mgcp.display_raw_text" and "mgcp.display_dissect_tree"
                 * were used in earlier versions of Wireshark; if we didn't find the
                 * preference, it was an MGCP preference, and its name was
                 * "display raw text toggle" or "display dissect tree", look for
                 * "display_raw_text" or "display_dissect_tree" instead.
                 *
                 * "mgcp.tcp.port" and "mgcp.udp.port" are harder to handle, as both
                 * the gateway and callagent ports were given those names; we interpret
                 * the first as "mgcp.{tcp,udp}.gateway_port" and the second as
                 * "mgcp.{tcp,udp}.callagent_port", as that's the order in which
                 * they were registered by the MCCP dissector and thus that's the
                 * order in which they were written to the preferences file.  (If
                 * we're not reading the preferences file, but are handling stuff
                 * from a "-o" command-line option, we have no clue which the user
                 * had in mind - they should have used "mgcp.{tcp,udp}.gateway_port"
                 * or "mgcp.{tcp,udp}.callagent_port" instead.)
                 */
                if (strcmp(dotp, "display raw text toggle") == 0)
                    pref = prefs_find_preference(module, "display_raw_text");
                else if (strcmp(dotp, "display dissect tree") == 0)
                    pref = prefs_find_preference(module, "display_dissect_tree");
                else if (strcmp(dotp, "tcp.port") == 0) {
                    mgcp_tcp_port_count++;
                    if (mgcp_tcp_port_count == 1) {
                        /* It's the first one */
                        pref = prefs_find_preference(module, "tcp.gateway_port");
                    } else if (mgcp_tcp_port_count == 2) {
                        /* It's the second one */
                        pref = prefs_find_preference(module, "tcp.callagent_port");
                    }
                    /* Otherwise it's from the command line, and we don't bother
                       mapping it. */
                } else if (strcmp(dotp, "udp.port") == 0) {
                    mgcp_udp_port_count++;
                    if (mgcp_udp_port_count == 1) {
                        /* It's the first one */
                        pref = prefs_find_preference(module, "udp.gateway_port");
                    } else if (mgcp_udp_port_count == 2) {
                        /* It's the second one */
                        pref = prefs_find_preference(module, "udp.callagent_port");
                    }
                    /* Otherwise it's from the command line, and we don't bother
                       mapping it. */
                }
            } else if (strcmp(module->name, "smb") == 0) {
                /* Handle old names for SMB preferences. */
                if (strcmp(dotp, "smb.trans.reassembly") == 0)
                    pref = prefs_find_preference(module, "trans_reassembly");
                else if (strcmp(dotp, "smb.dcerpc.reassembly") == 0)
                    pref = prefs_find_preference(module, "dcerpc_reassembly");
            } else if (strcmp(module->name, "ndmp") == 0) {
                /* Handle old names for NDMP preferences. */
                if (strcmp(dotp, "ndmp.desegment") == 0)
                    pref = prefs_find_preference(module, "desegment");
            } else if (strcmp(module->name, "diameter") == 0) {
                /* Handle old names for Diameter preferences. */
                if (strcmp(dotp, "diameter.desegment") == 0)
                    pref = prefs_find_preference(module, "desegment");
            } else if (strcmp(module->name, "pcli") == 0) {
                /* Handle old names for PCLI preferences. */
                if (strcmp(dotp, "pcli.udp_port") == 0)
                    pref = prefs_find_preference(module, "udp_port");
            } else if (strcmp(module->name, "artnet") == 0) {
                /* Handle old names for ARTNET preferences. */
                if (strcmp(dotp, "artnet.udp_port") == 0)
                    pref = prefs_find_preference(module, "udp_port");
            } else if (strcmp(module->name, "mapi") == 0) {
                /* Handle old names for MAPI preferences. */
                if (strcmp(dotp, "mapi_decrypt") == 0)
                    pref = prefs_find_preference(module, "decrypt");
            } else if (strcmp(module->name, "fc") == 0) {
                /* Handle old names for Fibre Channel preferences. */
                if (strcmp(dotp, "reassemble_fc") == 0)
                    pref = prefs_find_preference(module, "reassemble");
                else if (strcmp(dotp, "fc_max_frame_size") == 0)
                    pref = prefs_find_preference(module, "max_frame_size");
            } else if (strcmp(module->name, "fcip") == 0) {
                /* Handle old names for Fibre Channel-over-IP preferences. */
                if (strcmp(dotp, "desegment_fcip_messages") == 0)
                    pref = prefs_find_preference(module, "desegment");
                else if (strcmp(dotp, "fcip_port") == 0)
                    pref = prefs_find_preference(module, "target_port");
            } else if (strcmp(module->name, "gtp") == 0) {
                /* Handle old names for GTP preferences. */
                if (strcmp(dotp, "gtpv0_port") == 0)
                    pref = prefs_find_preference(module, "v0_port");
                else if (strcmp(dotp, "gtpv1c_port") == 0)
                    pref = prefs_find_preference(module, "v1c_port");
                else if (strcmp(dotp, "gtpv1u_port") == 0)
                    pref = prefs_find_preference(module, "v1u_port");
                else if (strcmp(dotp, "gtp_dissect_tpdu") == 0)
                    pref = prefs_find_preference(module, "dissect_tpdu");
                else if (strcmp(dotp, "gtpv0_dissect_cdr_as") == 0)
                    pref = prefs_find_preference(module, "v0_dissect_cdr_as");
                else if (strcmp(dotp, "gtpv0_check_etsi") == 0)
                    pref = prefs_find_preference(module, "v0_check_etsi");
                else if (strcmp(dotp, "gtpv1_check_etsi") == 0)
                    pref = prefs_find_preference(module, "v1_check_etsi");
            } else if (strcmp(module->name, "ip") == 0) {
                /* Handle old names for IP preferences. */
                if (strcmp(dotp, "ip_summary_in_tree") == 0)
                    pref = prefs_find_preference(module, "summary_in_tree");
            } else if (strcmp(module->name, "iscsi") == 0) {
                /* Handle old names for iSCSI preferences. */
                if (strcmp(dotp, "iscsi_port") == 0)
                    pref = prefs_find_preference(module, "target_port");
            } else if (strcmp(module->name, "lmp") == 0) {
                /* Handle old names for LMP preferences. */
                if (strcmp(dotp, "lmp_version") == 0)
                    pref = prefs_find_preference(module, "version");
            } else if (strcmp(module->name, "mtp3") == 0) {
                /* Handle old names for MTP3 preferences. */
                if (strcmp(dotp, "mtp3_standard") == 0)
                    pref = prefs_find_preference(module, "standard");
                else if (strcmp(dotp, "net_addr_format") == 0)
                    pref = prefs_find_preference(module, "addr_format");
            } else if (strcmp(module->name, "nlm") == 0) {
                /* Handle old names for NLM preferences. */
                if (strcmp(dotp, "nlm_msg_res_matching") == 0)
                    pref = prefs_find_preference(module, "msg_res_matching");
            } else if (strcmp(module->name, "ppp") == 0) {
                /* Handle old names for PPP preferences. */
                if (strcmp(dotp, "ppp_fcs") == 0)
                    pref = prefs_find_preference(module, "fcs_type");
                else if (strcmp(dotp, "ppp_vj") == 0)
                    pref = prefs_find_preference(module, "decompress_vj");
            } else if (strcmp(module->name, "rsvp") == 0) {
                /* Handle old names for RSVP preferences. */
                if (strcmp(dotp, "rsvp_process_bundle") == 0)
                    pref = prefs_find_preference(module, "process_bundle");
            } else if (strcmp(module->name, "tcp") == 0) {
                /* Handle old names for TCP preferences. */
                if (strcmp(dotp, "tcp_summary_in_tree") == 0)
                    pref = prefs_find_preference(module, "summary_in_tree");
                else if (strcmp(dotp, "tcp_analyze_sequence_numbers") == 0)
                    pref = prefs_find_preference(module, "analyze_sequence_numbers");
                else if (strcmp(dotp, "tcp_relative_sequence_numbers") == 0)
                    pref = prefs_find_preference(module, "relative_sequence_numbers");
                else if (strcmp(dotp, "dissect_experimental_options_with_magic") == 0)
                    pref = prefs_find_preference(module, "dissect_experimental_options_rfc6994");
            } else if (strcmp(module->name, "udp") == 0) {
                /* Handle old names for UDP preferences. */
                if (strcmp(dotp, "udp_summary_in_tree") == 0)
                    pref = prefs_find_preference(module, "summary_in_tree");
            } else if (strcmp(module->name, "ndps") == 0) {
                /* Handle old names for NDPS preferences. */
                if (strcmp(dotp, "desegment_ndps") == 0)
                    pref = prefs_find_preference(module, "desegment_tcp");
            } else if (strcmp(module->name, "http") == 0) {
                /* Handle old names for HTTP preferences. */
                if (strcmp(dotp, "desegment_http_headers") == 0)
                    pref = prefs_find_preference(module, "desegment_headers");
                else if (strcmp(dotp, "desegment_http_body") == 0)
                    pref = prefs_find_preference(module, "desegment_body");
            } else if (strcmp(module->name, "smpp") == 0) {
                /* Handle preferences that moved from SMPP. */
                module_t *new_module = prefs_find_module("gsm-sms-ud");
                if (new_module) {
                    if (strcmp(dotp, "port_number_udh_means_wsp") == 0) {
                        pref = prefs_find_preference(new_module, "port_number_udh_means_wsp");
                        containing_module = new_module;
                    } else if (strcmp(dotp, "try_dissect_1st_fragment") == 0) {
                        pref = prefs_find_preference(new_module, "try_dissect_1st_fragment");
                        containing_module = new_module;
                    }
                }
            } else if (strcmp(module->name, "asn1") == 0) {
                /* Handle old generic ASN.1 preferences (it's not really a
                   rename, as the new preferences support multiple ports,
                   but we might as well copy them over). */
                if (strcmp(dotp, "tcp_port") == 0)
                    pref = prefs_find_preference(module, "tcp_ports");
                else if (strcmp(dotp, "udp_port") == 0)
                    pref = prefs_find_preference(module, "udp_ports");
                else if (strcmp(dotp, "sctp_port") == 0)
                    pref = prefs_find_preference(module, "sctp_ports");
            } else if (strcmp(module->name, "llcgprs") == 0) {
                if (strcmp(dotp, "ignore_cipher_bit") == 0)
                    pref = prefs_find_preference(module, "autodetect_cipher_bit");
            } else if (strcmp(module->name, "erf") == 0) {
                if (strcmp(dotp, "erfeth") == 0) {
                    /* Handle the old "erfeth" preference; map it to the new
                       "ethfcs" preference, and map the values to those for
                       the new preference. */
                    pref = prefs_find_preference(module, "ethfcs");
                    if (strcmp(value, "ethfcs") == 0 || strcmp(value, "Ethernet with FCS") == 0)
                        value = "TRUE";
                    else if (strcmp(value, "eth") == 0 || strcmp(value, "Ethernet") == 0)
                        value = "FALSE";
                    else if (strcmp(value, "raw") == 0 || strcmp(value, "Raw data") == 0)
                        value = "TRUE";
                } else if (strcmp(dotp, "erfatm") == 0) {
                    /* Handle the old "erfatm" preference; map it to the new
                       "aal5_type" preference, and map the values to those for
                       the new preference. */
                    pref = prefs_find_preference(module, "aal5_type");
                    if (strcmp(value, "atm") == 0 || strcmp(value, "ATM") == 0)
                        value = "guess";
                    else if (strcmp(value, "llc") == 0 || strcmp(value, "LLC") == 0)
                        value = "llc";
                    else if (strcmp(value, "raw") == 0 || strcmp(value, "Raw data") == 0)
                        value = "guess";
                } else if (strcmp(dotp, "erfhdlc") == 0) {
                    /* Handle the old "erfhdlc" preference; map it to the new
                       "hdlc_type" preference, and map the values to those for
                       the new preference. */
                    pref = prefs_find_preference(module, "hdlc_type");
                    if (strcmp(value, "chdlc") == 0 || strcmp(value, "Cisco HDLC") == 0)
                        value = "chdlc";
                    else if (strcmp(value, "ppp") == 0 || strcmp(value, "PPP serial") == 0)
                        value = "ppp";
                    else if (strcmp(value, "fr") == 0 || strcmp(value, "Frame Relay") == 0)
                        value = "frelay";
                    else if (strcmp(value, "mtp2") == 0 || strcmp(value, "SS7 MTP2") == 0)
                        value = "mtp2";
                    else if (strcmp(value, "raw") == 0 || strcmp(value, "Raw data") == 0)
                        value = "guess";
                }
            } else if (strcmp(module->name, "eth") == 0) {
                /* "eth.qinq_ethertype" has been changed(restored) to "vlan.qinq.ethertype" */
                if (strcmp(dotp, "qinq_ethertype") == 0) {
                    module_t *new_module = prefs_find_module("vlan");
                    if (new_module) {
                        pref = prefs_find_preference(new_module, "qinq_ethertype");
                        containing_module = new_module;
                    }
                }
            } else if (strcmp(module->name, "taps") == 0) {
                /* taps preferences moved to "statistics" module */
                if (strcmp(dotp, "update_interval") == 0)
                    pref = prefs_find_preference(stats_module, dotp);
            } else if (strcmp(module->name, "packet_list") == 0) {
                /* packet_list preferences moved to protocol module */
                if (strcmp(dotp, "display_hidden_proto_items") == 0)
                    pref = prefs_find_preference(protocols_module, dotp);
            } else if (strcmp(module->name, "stream") == 0) {
                /* stream preferences moved to gui color module */
                if ((strcmp(dotp, "client.fg") == 0) ||
                    (strcmp(dotp, "client.bg") == 0) ||
                    (strcmp(dotp, "server.fg") == 0) ||
                    (strcmp(dotp, "server.bg") == 0))
                    pref = prefs_find_preference(gui_color_module, pref_name);
            } else if (strcmp(module->name, "nameres") == 0) {
                if (strcmp(pref_name, "name_resolve_concurrency") == 0) {
                    pref = prefs_find_preference(nameres_module, pref_name);
                } else if (strcmp(pref_name, "name_resolve_load_smi_modules") == 0) {
                    pref = prefs_find_preference(nameres_module, "load_smi_modules");
                } else if (strcmp(pref_name, "name_resolve_suppress_smi_errors") == 0) {
                    pref = prefs_find_preference(nameres_module, "suppress_smi_errors");
                }
            } else if (strcmp(module->name, "extcap") == 0) {
                /* Handle the old "sshdump.remotesudo" preference; map it to the new
                  "sshdump.remotepriv" preference, and map the boolean values to the
                  appropriate strings of the new preference. */
                if (strcmp(dotp, "sshdump.remotesudo") == 0) {
                    pref = prefs_find_preference(module, "sshdump.remotepriv");
                    if (g_ascii_strcasecmp(value, "true") == 0)
                        value = "sudo";
                    else
                        value = "none";
                }
            }
            if (pref) {
                converted_pref = true;
            }
        }
        if (pref == NULL ) {
            if (strcmp(module->name, "extcap") == 0 && g_list_length(module->prefs) <= 1) {
                    /*
                    * Assume that we've skipped extcap preference registration
                    * and that only extcap.gui_save_on_start is loaded.
                    */
                    return PREFS_SET_OK;
                }
            return PREFS_SET_NO_SUCH_PREF;    /* no such preference */
        }

        if (target_module && target_module != containing_module) {
            /* Ignore */
            return PREFS_SET_OK;
        }

        if (pref->obsolete)
            return PREFS_SET_OBSOLETE;        /* no such preference any more */

        if (converted_pref) {
            ws_warning("Preference \"%s\" has been converted to \"%s.%s\"\n"
                       "Save your preferences to make this change permanent.",
                       pref_name, module->name ? module->name : module->parent->name, prefs_get_name(pref));
        }

        switch (pref->type) {

        case PREF_UINT:
            if (!ws_basestrtou32(value, NULL, &uval, pref->info.base))
                return PREFS_SET_SYNTAX_ERR;        /* number was bad */
            if (*pref->varp.uint != uval) {
                containing_module->prefs_changed_flags |= prefs_get_effect_flags(pref);
                *pref->varp.uint = uval;
            }
            break;
        case PREF_BOOL:
            /* XXX - give an error if it's neither "true" nor "false"? */
            if (g_ascii_strcasecmp(value, "true") == 0)
                bval = true;
            else
                bval = false;
            if (*pref->varp.boolp != bval) {
                containing_module->prefs_changed_flags |= prefs_get_effect_flags(pref);
                *pref->varp.boolp = bval;
            }
            break;

        case PREF_ENUM:
            /* XXX - give an error if it doesn't match? */
            enum_val = find_val_for_string(value, pref->info.enum_info.enumvals,
                                           *pref->varp.enump);
            if (*pref->varp.enump != enum_val) {
                containing_module->prefs_changed_flags |= prefs_get_effect_flags(pref);
                *pref->varp.enump = enum_val;
            }
            break;

        case PREF_STRING:
        case PREF_SAVE_FILENAME:
        case PREF_OPEN_FILENAME:
        case PREF_DIRNAME:
        case PREF_DISSECTOR:
            containing_module->prefs_changed_flags |= prefs_set_string_value(pref, value, pref_current);
            break;

        case PREF_PASSWORD:
            /* Read value is every time empty */
            containing_module->prefs_changed_flags |= prefs_set_string_value(pref, "", pref_current);
            break;

        case PREF_RANGE:
        {
            if (!prefs_set_range_value_work(pref, value, return_range_errors,
                                            &containing_module->prefs_changed_flags))
                return PREFS_SET_SYNTAX_ERR;        /* number was bad */
            break;
        }
        case PREF_DECODE_AS_RANGE:
        {
            /* This is for backwards compatibility in case any of the preferences
               that shared the "Decode As" preference name and used to be PREF_RANGE
               are now applied directly to the Decode As funtionality */
            range_t *newrange;
            dissector_table_t sub_dissectors;
            dissector_handle_t handle;
            uint32_t i, j;

            if (range_convert_str_work(wmem_epan_scope(), &newrange, value, pref->info.max_value,
                                       return_range_errors) != CVT_NO_ERROR) {
                return PREFS_SET_SYNTAX_ERR;        /* number was bad */
            }

            if (!ranges_are_equal(*pref->varp.range, newrange)) {
                wmem_free(wmem_epan_scope(), *pref->varp.range);
                *pref->varp.range = newrange;
                containing_module->prefs_changed_flags |= prefs_get_effect_flags(pref);

                const char* table_name = prefs_get_dissector_table(pref);
                sub_dissectors = find_dissector_table(table_name);
                if (sub_dissectors != NULL) {
                    handle = dissector_table_get_dissector_handle(sub_dissectors, module->title);
                    if (handle != NULL) {
                        /* Delete all of the old values from the dissector table */
                        for (i = 0; i < (*pref->varp.range)->nranges; i++) {
                            for (j = (*pref->varp.range)->ranges[i].low; j < (*pref->varp.range)->ranges[i].high; j++) {
                                dissector_delete_uint(table_name, j, handle);
                                decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(j), NULL, NULL);
                            }

                            dissector_delete_uint(table_name, (*pref->varp.range)->ranges[i].high, handle);
                            decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER((*pref->varp.range)->ranges[i].high), NULL, NULL);
                        }

                        /* Add new values to the dissector table */
                        for (i = 0; i < newrange->nranges; i++) {
                            for (j = newrange->ranges[i].low; j < newrange->ranges[i].high; j++) {
                                dissector_change_uint(table_name, j, handle);
                                decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(j), NULL, NULL);
                            }

                            dissector_change_uint(table_name, newrange->ranges[i].high, handle);
                            decode_build_reset_list(table_name, dissector_table_get_type(sub_dissectors), GUINT_TO_POINTER(newrange->ranges[i].high), NULL, NULL);
                        }

                        /* XXX - Do we save the decode_as_entries file here? */
                    }
                }
            } else {
                wmem_free(wmem_epan_scope(), newrange);
            }
            break;
        }

        case PREF_COLOR:
        {
            if (!ws_hexstrtou32(value, NULL, &cval))
                return PREFS_SET_SYNTAX_ERR;        /* number was bad */
            if ((pref->varp.colorp->red != RED_COMPONENT(cval)) ||
                (pref->varp.colorp->green != GREEN_COMPONENT(cval)) ||
                (pref->varp.colorp->blue != BLUE_COMPONENT(cval))) {
                containing_module->prefs_changed_flags |= prefs_get_effect_flags(pref);
                pref->varp.colorp->red   = RED_COMPONENT(cval);
                pref->varp.colorp->green = GREEN_COMPONENT(cval);
                pref->varp.colorp->blue  = BLUE_COMPONENT(cval);
            }
            break;
        }

        case PREF_CUSTOM:
            return pref->custom_cbs.set_cb(pref, value, &containing_module->prefs_changed_flags);

        case PREF_STATIC_TEXT:
        case PREF_UAT:
            break;

        case PREF_PROTO_TCP_SNDAMB_ENUM:
        {
            /* There's no point in setting the TCP sequence override
             * value from the command line, because the pref is different
             * for each frame and reset to the default (0) for each new
             * file.
             */
            break;
        }
        }
    }

    return PREFS_SET_OK;
}

typedef struct {
    FILE     *pf;
    bool is_gui_module;
} write_gui_pref_arg_t;

const char *
prefs_pref_type_name(pref_t *pref)
{
    const char *type_name = "[Unknown]";

    if (!pref) {
        return type_name; /* ...or maybe assert? */
    }

    if (pref->obsolete) {
        type_name = "Obsolete";
    } else {
        switch (pref->type) {

        case PREF_UINT:
            switch (pref->info.base) {

            case 10:
                type_name = "Decimal";
                break;

            case 8:
                type_name = "Octal";
                break;

            case 16:
                type_name = "Hexadecimal";
                break;
            }
            break;

        case PREF_BOOL:
            type_name = "Boolean";
            break;

        case PREF_ENUM:
        case PREF_PROTO_TCP_SNDAMB_ENUM:
            type_name = "Choice";
            break;

        case PREF_STRING:
            type_name = "String";
            break;

        case PREF_SAVE_FILENAME:
        case PREF_OPEN_FILENAME:
            type_name = "Filename";
            break;

        case PREF_DIRNAME:
            type_name = "Directory";
            break;

        case PREF_RANGE:
            type_name = "Range";
            break;

        case PREF_COLOR:
            type_name = "Color";
            break;

        case PREF_CUSTOM:
            if (pref->custom_cbs.type_name_cb)
                return pref->custom_cbs.type_name_cb();
            type_name = "Custom";
            break;

        case PREF_DECODE_AS_RANGE:
            type_name = "Range (for Decode As)";
            break;

        case PREF_STATIC_TEXT:
            type_name = "Static text";
            break;

        case PREF_UAT:
            type_name = "UAT";
            break;

        case PREF_PASSWORD:
            type_name = "Password";
            break;

        case PREF_DISSECTOR:
            type_name = "Dissector";
            break;
        }
    }

    return type_name;
}

unsigned int
prefs_get_effect_flags(pref_t *pref)
{
    if (pref == NULL)
        return 0;

    return pref->effect_flags;
}

void
prefs_set_effect_flags(pref_t *pref, unsigned int flags)
{
    if (pref != NULL) {
        if (flags == 0) {
            ws_error("Setting \"%s\" preference effect flags to 0", pref->name);
        }
        pref->effect_flags = flags;
    }
}

void
prefs_set_effect_flags_by_name(module_t * module, const char *pref, unsigned int flags)
{
    prefs_set_effect_flags(prefs_find_preference(module, pref), flags);
}

unsigned int
prefs_get_module_effect_flags(module_t * module)
{
    if (module == NULL)
        return 0;

    return module->effect_flags;
}

void
prefs_set_module_effect_flags(module_t * module, unsigned int flags)
{
    if (module != NULL) {
        if (flags == 0) {
            ws_error("Setting module \"%s\" preference effect flags to 0", module->name);
        }
        module->effect_flags = flags;
    }
}

char *
prefs_pref_type_description(pref_t *pref)
{
    const char *type_desc = "An unknown preference type";

    if (!pref) {
        return ws_strdup_printf("%s.", type_desc); /* ...or maybe assert? */
    }

    if (pref->obsolete) {
        type_desc = "An obsolete preference";
    } else {
        switch (pref->type) {

        case PREF_UINT:
            switch (pref->info.base) {

            case 10:
                type_desc = "A decimal number";
                break;

            case 8:
                type_desc = "An octal number";
                break;

            case 16:
                type_desc = "A hexadecimal number";
                break;
            }
            break;

        case PREF_BOOL:
            type_desc = "true or false (case-insensitive)";
            break;

        case PREF_ENUM:
        case PREF_PROTO_TCP_SNDAMB_ENUM:
        {
            const enum_val_t *enum_valp = pref->info.enum_info.enumvals;
            GString *enum_str = g_string_new("One of: ");
            GString *desc_str = g_string_new("\nEquivalently, one of: ");
            bool distinct = false;
            while (enum_valp->name != NULL) {
                g_string_append(enum_str, enum_valp->name);
                g_string_append(desc_str, enum_valp->description);
                if (g_strcmp0(enum_valp->name, enum_valp->description) != 0) {
                    distinct = true;
                }
                enum_valp++;
                if (enum_valp->name != NULL) {
                    g_string_append(enum_str, ", ");
                    g_string_append(desc_str, ", ");
                }
            }
            if (distinct) {
                g_string_append(enum_str, desc_str->str);
            }
            g_string_free(desc_str, TRUE);
            g_string_append(enum_str, "\n(case-insensitive).");
            return g_string_free(enum_str, FALSE);
        }

        case PREF_STRING:
            type_desc = "A string";
            break;

        case PREF_SAVE_FILENAME:
        case PREF_OPEN_FILENAME:
            type_desc = "A path to a file";
            break;

        case PREF_DIRNAME:
            type_desc = "A path to a directory";
            break;

        case PREF_RANGE:
        {
            type_desc = "A string denoting an positive integer range (e.g., \"1-20,30-40\")";
            break;
        }

        case PREF_COLOR:
        {
            type_desc = "A six-digit hexadecimal RGB color triplet (e.g. fce94f)";
            break;
        }

        case PREF_CUSTOM:
            if (pref->custom_cbs.type_description_cb)
                return pref->custom_cbs.type_description_cb();
            type_desc = "A custom value";
            break;

        case PREF_DECODE_AS_RANGE:
            type_desc = "A string denoting an positive integer range for Decode As";
            break;

        case PREF_STATIC_TEXT:
            type_desc = "[Static text]";
            break;

        case PREF_UAT:
            type_desc = "Configuration data stored in its own file";
            break;

        case PREF_PASSWORD:
            type_desc = "Password (never stored on disk)";
            break;

        case PREF_DISSECTOR:
            type_desc = "A dissector name";
            break;

        default:
            break;
        }
    }

    return g_strdup(type_desc);
}

bool
prefs_pref_is_default(pref_t *pref)
{
    if (!pref) return false;

    if (pref->obsolete) {
        return false;
    }

    switch (pref->type) {

    case PREF_UINT:
        if (pref->default_val.uint == *pref->varp.uint)
            return true;
        break;

    case PREF_BOOL:
        if (pref->default_val.boolval == *pref->varp.boolp)
            return true;
        break;

    case PREF_ENUM:
    case PREF_PROTO_TCP_SNDAMB_ENUM:
        if (pref->default_val.enumval == *pref->varp.enump)
            return true;
        break;

    case PREF_STRING:
    case PREF_SAVE_FILENAME:
    case PREF_OPEN_FILENAME:
    case PREF_DIRNAME:
    case PREF_PASSWORD:
    case PREF_DISSECTOR:
        if (!(g_strcmp0(pref->default_val.string, *pref->varp.string)))
            return true;
        break;

    case PREF_DECODE_AS_RANGE:
    case PREF_RANGE:
    {
        if ((ranges_are_equal(pref->default_val.range, *pref->varp.range)))
            return true;
        break;
    }

    case PREF_COLOR:
    {
        if ((pref->default_val.color.red == pref->varp.colorp->red) &&
            (pref->default_val.color.green == pref->varp.colorp->green) &&
            (pref->default_val.color.blue == pref->varp.colorp->blue))
            return true;
        break;
    }

    case PREF_CUSTOM:
        return pref->custom_cbs.is_default_cb(pref);

    case PREF_STATIC_TEXT:
    case PREF_UAT:
        return false;
        /* ws_assert_not_reached(); */
        break;
    }

    return false;
}

char *
prefs_pref_to_str(pref_t *pref, pref_source_t source)
{
    const char *pref_text = "[Unknown]";
    void *valp; /* pointer to preference value */
    color_t *pref_color;
    char *tmp_value, *ret_value;

    if (!pref) {
        return g_strdup(pref_text);
    }

    switch (source) {
        case pref_default:
            valp = &pref->default_val;
            /* valp = &boolval, &enumval, etc. are implied by union property */
            pref_color = &pref->default_val.color;
            break;
        case pref_stashed:
            valp = &pref->stashed_val;
            /* valp = &boolval, &enumval, etc. are implied by union property */
            pref_color = &pref->stashed_val.color;
            break;
        case pref_current:
            valp = pref->varp.uint;
            /* valp = boolval, enumval, etc. are implied by union property */
            pref_color = pref->varp.colorp;
            break;
        default:
            return g_strdup(pref_text);
    }

    if (pref->obsolete) {
        pref_text = "[Obsolete]";
    } else {
        switch (pref->type) {

        case PREF_UINT:
        {
            unsigned pref_uint = *(unsigned *) valp;
            switch (pref->info.base) {

            case 10:
                return ws_strdup_printf("%u", pref_uint);

            case 8:
                return ws_strdup_printf("%#o", pref_uint);

            case 16:
                return ws_strdup_printf("%#x", pref_uint);
            }
            break;
        }

        case PREF_BOOL:
            return g_strdup((*(bool *) valp) ? "TRUE" : "FALSE");

        case PREF_ENUM:
        case PREF_PROTO_TCP_SNDAMB_ENUM:
        {
            int pref_enumval = *(int *) valp;
            const enum_val_t *enum_valp = pref->info.enum_info.enumvals;
            /*
            * TODO - We write the "description" value, because the "name" values
            * weren't validated to be command line friendly until 5.0, and a few
            * of them had to be changed. This allows older versions of Wireshark
            * to read preferences that they supported, as we supported either
            * the short name or the description when reading the preference files
            * or an "-o" option. Once 5.0 is the oldest supported version, switch
            * to writing the name below.
            */
            while (enum_valp->name != NULL) {
                if (enum_valp->value == pref_enumval)
                    return g_strdup(enum_valp->description);
                enum_valp++;
            }
            break;
        }

        case PREF_STRING:
        case PREF_SAVE_FILENAME:
        case PREF_OPEN_FILENAME:
        case PREF_DIRNAME:
        case PREF_PASSWORD:
        case PREF_DISSECTOR:
            return g_strdup(*(const char **) valp);

        case PREF_DECODE_AS_RANGE:
        case PREF_RANGE:
            /* Convert wmem to g_alloc memory */
            tmp_value = range_convert_range(NULL, *(range_t **) valp);
            ret_value = g_strdup(tmp_value);
            wmem_free(NULL, tmp_value);
            return ret_value;

        case PREF_COLOR:
            return ws_strdup_printf("%02x%02x%02x",
                    (pref_color->red * 255 / 65535),
                    (pref_color->green * 255 / 65535),
                    (pref_color->blue * 255 / 65535));

        case PREF_CUSTOM:
            if (pref->custom_cbs.to_str_cb)
                return pref->custom_cbs.to_str_cb(pref, source == pref_default ? true : false);
            pref_text = "[Custom]";
            break;

        case PREF_STATIC_TEXT:
            pref_text = "[Static text]";
            break;

        case PREF_UAT:
        {
            uat_t *uat = pref->varp.uat;
            if (uat && uat->filename)
                return ws_strdup_printf("[Managed in the file \"%s\"]", uat->filename);
            else
                pref_text = "[Managed in an unknown file]";
            break;
        }

        default:
            break;
        }
    }

    return g_strdup(pref_text);
}

/*
 * Write out a single dissector preference.
 */
static void
write_pref(void *data, void *user_data)
{
    pref_t *pref = (pref_t *)data;
    write_pref_arg_t *arg = (write_pref_arg_t *)user_data;
    char **desc_lines;
    int i;

    if (!pref || pref->obsolete) {
        /*
         * This preference is no longer supported; it's not a
         * real preference, so we don't write it out (i.e., we
         * treat it as if it weren't found in the list of
         * preferences, and we weren't called in the first place).
         */
        return;
    }

    switch (pref->type) {

    case PREF_STATIC_TEXT:
    case PREF_UAT:
        /* Nothing to do; don't bother printing the description */
        return;
    case PREF_DECODE_AS_RANGE:
        /* Data is saved through Decode As mechanism and not part of preferences file */
        return;
    case PREF_PROTO_TCP_SNDAMB_ENUM:
        /* Not written to the preference file because the override is only
         * for the lifetime of the capture file and there is no single
         * value to write.
         */
        return;
    default:
        break;
    }

    if (pref->type != PREF_CUSTOM || pref->custom_cbs.type_name_cb() != NULL) {
        /*
         * The prefix will either be the module name or the parent
         * name if it's a subtree
         */
        const char *name_prefix = (arg->module->name != NULL) ? arg->module->name : arg->module->parent->name;
        char *type_desc, *pref_text;
        const char * def_prefix = prefs_pref_is_default(pref) ? "#" : "";

        if (pref->type == PREF_CUSTOM)
            fprintf(arg->pf, "\n# %s", pref->custom_cbs.type_name_cb());
        fprintf(arg->pf, "\n");
        if (pref->description &&
                (g_ascii_strncasecmp(pref->description,"", 2) != 0)) {
            if (pref->type != PREF_CUSTOM) {
                /* We get duplicate lines otherwise. */

                desc_lines = g_strsplit(pref->description, "\n", 0);
                for (i = 0; desc_lines[i] != NULL; ++i) {
                    fprintf(arg->pf, "# %s\n", desc_lines[i]);
                }
                g_strfreev(desc_lines);
            }
        } else {
            fprintf(arg->pf, "# No description\n");
        }

        type_desc = prefs_pref_type_description(pref);
        desc_lines = g_strsplit(type_desc, "\n", 0);
        for (i = 0; desc_lines[i] != NULL; ++i) {
            fprintf(arg->pf, "# %s\n", desc_lines[i]);
        }
        g_strfreev(desc_lines);
        g_free(type_desc);

        pref_text = prefs_pref_to_str(pref, pref_current);
        fprintf(arg->pf, "%s%s.%s: ", def_prefix, name_prefix, pref->name);
        if (pref->type != PREF_PASSWORD)
        {
            desc_lines = g_strsplit(pref_text, "\n", 0);
            for (i = 0; desc_lines[i] != NULL; ++i) {
                fprintf(arg->pf, "%s%s\n", i == 0 ? "" : def_prefix, desc_lines[i]);
            }
            if (i == 0)
                fprintf(arg->pf, "\n");
            g_strfreev(desc_lines);
        } else {
            /* We never store password value */
            fprintf(arg->pf, "\n");
        }
        g_free(pref_text);
    }

}

static void
count_non_uat_pref(void *data, void *user_data)
{
    pref_t *pref = (pref_t *)data;
    int *arg = (int *)user_data;

    switch (pref->type)
    {
    case PREF_UAT:
    case PREF_DECODE_AS_RANGE:
    case PREF_PROTO_TCP_SNDAMB_ENUM:
        //These types are not written in preference file
        break;
    default:
        (*arg)++;
        break;
    }
}

static int num_non_uat_prefs(module_t *module)
{
    int num = 0;

    g_list_foreach(module->prefs, count_non_uat_pref, &num);

    return num;
}

/*
 * Write out all preferences for a module.
 */
static unsigned
write_module_prefs(module_t *module, void *user_data)
{
    write_gui_pref_arg_t *gui_pref_arg = (write_gui_pref_arg_t*)user_data;
    write_pref_arg_t arg;

    /* The GUI module needs to be explicitly called out so it
       can be written out of order */
    if ((module == gui_module) && (gui_pref_arg->is_gui_module != true))
        return 0;

    /* Write a header for the main modules and GUI sub-modules */
    if (((module->parent == NULL) || (module->parent == gui_module)) &&
        ((prefs_module_has_submodules(module)) ||
         (num_non_uat_prefs(module) > 0) ||
         (module->name == NULL))) {
         if ((module->name == NULL) && (module->parent != NULL)) {
            fprintf(gui_pref_arg->pf, "\n####### %s: %s ########\n", module->parent->title, module->title);
         } else {
            fprintf(gui_pref_arg->pf, "\n####### %s ########\n", module->title);
         }
    }

    arg.module = module;
    arg.pf = gui_pref_arg->pf;
    g_list_foreach(arg.module->prefs, write_pref, &arg);

    if (prefs_module_has_submodules(module))
        return prefs_modules_foreach_submodules(module, write_module_prefs, user_data);

    return 0;
}

#ifdef _WIN32
static void
write_registry(void)
{
    HKEY hTestKey;
    DWORD data;
    DWORD data_size;
    DWORD ret;

    ret = RegCreateKeyExA(HKEY_CURRENT_USER, REG_HKCU_WIRESHARK_KEY, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                            &hTestKey, NULL);
    if (ret != ERROR_SUCCESS) {
        ws_noisy("Cannot open HKCU "REG_HKCU_WIRESHARK_KEY": 0x%lx", ret);
        return;
    }

    data = ws_log_console_open;
    data_size = sizeof(DWORD);
    ret = RegSetValueExA(hTestKey, LOG_HKCU_CONSOLE_OPEN, 0, REG_DWORD, (const BYTE *)&data, data_size);
    if (ret == ERROR_SUCCESS) {
        ws_noisy("Wrote "LOG_HKCU_CONSOLE_OPEN" to Windows registry: 0x%lu", data);
    }
    else {
        ws_noisy("Error writing registry key "LOG_HKCU_CONSOLE_OPEN": 0x%lx", ret);
    }

    RegCloseKey(hTestKey);
}
#endif

/* Write out "prefs" to the user's preferences file, and return 0.

   If the preferences file path is NULL, write to stdout.

   If we got an error, stuff a pointer to the path of the preferences file
   into "*pf_path_return", and return the errno. */
int
write_prefs(char **pf_path_return)
{
    char        *pf_path;
    FILE        *pf;
    write_gui_pref_arg_t write_gui_pref_info;

    /* Needed for "-G defaultprefs" */
    init_prefs();

#ifdef _WIN32
    write_registry();
#endif

    /* To do:
     * - Split output lines longer than MAX_VAL_LEN
     * - Create a function for the preference directory check/creation
     *   so that duplication can be avoided with filter.c
     */

    if (pf_path_return != NULL) {
        pf_path = get_persconffile_path(PF_NAME, true);
        if ((pf = ws_fopen(pf_path, "w")) == NULL) {
            *pf_path_return = pf_path;
            return errno;
        }
        g_free(pf_path);
    } else {
        pf = stdout;
    }

    /*
     * If the preferences file is being written, be sure to write UAT files
     * first that were migrated from the preferences file.
     */
    if (pf_path_return != NULL) {
        if (prefs.filter_expressions_old) {
            char *err = NULL;
            prefs.filter_expressions_old = false;
            if (!uat_save(uat_get_table_by_name("Display expressions"), &err)) {
                ws_warning("Unable to save Display expressions: %s", err);
                g_free(err);
            }
        }

        module_t *extcap_module = prefs_find_module("extcap");
        if (extcap_module && !prefs.capture_no_extcap) {
            char *ext_path = get_persconffile_path("extcap.cfg", true);
            FILE *extf;
            if ((extf = ws_fopen(ext_path, "w")) == NULL) {
                if (errno != EISDIR) {
                    ws_warning("Unable to save extcap preferences \"%s\": %s",
                        ext_path, g_strerror(errno));
                }
                g_free(ext_path);
            } else {
                g_free(ext_path);

                fputs("# Extcap configuration file for Wireshark " VERSION ".\n"
                      "#\n"
                      "# This file is regenerated each time preferences are saved within\n"
                      "# Wireshark. Making manual changes should be safe, however.\n"
                      "# Preferences that have been commented out have not been\n"
                      "# changed from their default value.\n", extf);

                write_gui_pref_info.pf = extf;
                write_gui_pref_info.is_gui_module = false;

                write_module_prefs(extcap_module, &write_gui_pref_info);

                fclose(extf);
            }
        }
    }

    fputs("# Configuration file for Wireshark " VERSION ".\n"
          "#\n"
          "# This file is regenerated each time preferences are saved within\n"
          "# Wireshark. Making manual changes should be safe, however.\n"
          "# Preferences that have been commented out have not been\n"
          "# changed from their default value.\n", pf);

    /*
     * For "backwards compatibility" the GUI module is written first as it's
     * at the top of the file.  This is followed by all modules that can't
     * fit into the preferences read/write API.  Finally the remaining modules
     * are written in alphabetical order (including of course the protocol preferences)
     */
    write_gui_pref_info.pf = pf;
    write_gui_pref_info.is_gui_module = true;

    write_module_prefs(gui_module, &write_gui_pref_info);

    write_gui_pref_info.is_gui_module = false;
    prefs_modules_foreach_submodules(NULL, write_module_prefs, &write_gui_pref_info);

    fclose(pf);

    /* XXX - catch I/O errors (e.g. "ran out of disk space") and return
       an error indication, or maybe write to a new preferences file and
       rename that file on top of the old one only if there are not I/O
       errors. */
    return 0;
}

/** The col_list is only partly managed by the custom preference API
 * because its data is shared between multiple preferences, so
 * it's freed here
 */
static void
free_col_info(GList *list)
{
    fmt_data *cfmt;
    GList *list_head = list;

    while (list != NULL) {
        cfmt = (fmt_data *)list->data;

        g_free(cfmt->title);
        g_free(cfmt->custom_fields);
        g_free(cfmt);
        list = g_list_next(list);
    }
    g_list_free(list_head);
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
