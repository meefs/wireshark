# CMakeLists.txt
#
# Wireshark - Network traffic analyzer
# By Gerald Combs <gerald@wireshark.org>
# Copyright 1998 Gerald Combs
#
# SPDX-License-Identifier: GPL-2.0-or-later
#

include(WiresharkPlugin)

# Plugin name and version info (major minor micro extra)
set_module_info(mate 1 0 1 0)

set(DISSECTOR_SRC
	packet-mate.c
)

set(DISSECTOR_SUPPORT_SRC
	mate_setup.c
	mate_runtime.c
	mate_util.c
)

add_lemon_files(LEMON_FILES GENERATED_FILES
	mate_grammar.lemon
)
add_lex_files(LEX_FILES GENERATED_FILES
	mate_parser.l
)

set(PLUGIN_FILES
	plugin.c
	${DISSECTOR_SRC}
	${DISSECTOR_SUPPORT_SRC}
	${GENERATED_FILES}
)

set_source_files_properties(
	${DISSECTOR_SRC}
	${DISSECTOR_SUPPORT_SRC}
	PROPERTIES
	COMPILE_FLAGS "${WERROR_COMMON_FLAGS}"
)

register_plugin_files(plugin.c
	plugin
	${DISSECTOR_SRC}
	${DISSECTOR_SUPPORT_SRC}
)

add_wireshark_plugin_library(mate epan)

target_link_libraries(mate epan)

install_plugin(mate epan)

file(GLOB DISSECTOR_HEADERS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*.h")
CHECKAPI(
	NAME
	  mate
	SWITCHES
	  --group dissectors-prohibited
	  --group dissectors-restricted
	SOURCES
	  ${DISSECTOR_SRC}
	  ${DISSECTOR_SUPPORT_SRC}
	  ${DISSECTOR_HEADERS}
# LEX files commented out due to use of malloc, free etc.
#	  ${LEX_FILES}
	  ${LEMON_FILES}
)
