;
; wireshark.nsi
;

; Set the compression mechanism first.
; As of NSIS 2.07, solid compression which makes installer about 1MB smaller
; is no longer the default, so use the /SOLID switch.
; This unfortunately is unknown to NSIS prior to 2.07 and creates an error.
; So if you get an error here, please update to at least NSIS 2.07!
SetCompressor /SOLID lzma
SetCompressorDictSize 64 ; MB

!include "wireshark-common.nsh"
!include 'LogicLib.nsh'
!include "StrFunc.nsh"
!include "WordFunc.nsh"

${StrRep}
${UnStrRep}

; See https://nsis.sourceforge.io/Check_if_a_file_exists_at_compile_time for documentation
!macro !defineifexist _VAR_NAME _FILE_NAME
  !tempfile _TEMPFILE
  !ifdef NSIS_WIN32_MAKENSIS
    ; Windows - cmd.exe
    !system 'if exist "${_FILE_NAME}" echo !define ${_VAR_NAME} > "${_TEMPFILE}"'
  !else
    ; Posix - sh
    !system 'if [ -e "${_FILE_NAME}" ]; then echo "!define ${_VAR_NAME}" > "${_TEMPFILE}"; fi'
  !endif
  !include '${_TEMPFILE}'
  !delfile '${_TEMPFILE}'
  !undef _TEMPFILE
!macroend
!define !defineifexist "!insertmacro !defineifexist"

; ============================================================================
; Header configuration
; ============================================================================

; The file to write
OutFile "${OUTFILE_DIR}\${PROGRAM_NAME}-${VERSION}-${WIRESHARK_TARGET_PLATFORM}.exe"
; Installer icon
Icon "${TOP_SRC_DIR}\resources\icons\wiresharkinst.ico"
; Uninstaller icon
UninstallIcon "${TOP_SRC_DIR}\resources\icons\wiresharkinst.ico"

; ============================================================================
; Modern UI
; ============================================================================
; The modern user interface will look much better than the common one.
; However, as the development of the modern UI is still going on, and the script
; syntax changes, you will need exactly that NSIS version, which this script is
; made for. This is the current (December 2003) latest version: V2.0b4
; If you are using a different version, it's not predictable what will happen.

!include "MUI2.nsh"
!include "InstallOptions.nsh"
;!addplugindir ".\Plugins"

!define MUI_ICON "${TOP_SRC_DIR}\resources\icons\wiresharkinst.ico"
!define MUI_UNICON "${TOP_SRC_DIR}\resources\icons\wiresharkinst.ico"
BrandingText "Wireshark${U+00ae} Installer"

!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_FINISHPAGE_NOAUTOCLOSE
!define MUI_WELCOMEPAGE_TITLE_3LINES
!define MUI_WELCOMEPAGE_TEXT "This wizard will guide you through the installation of ${PROGRAM_NAME}.$\r$\n$\r$\nBefore starting the installation, make sure ${PROGRAM_NAME} is not running.$\r$\n$\r$\nClick 'Next' to continue."
;!define MUI_FINISHPAGE_LINK "Install Npcap to be able to capture packets from a network."
;!define MUI_FINISHPAGE_LINK_LOCATION "https://npcap.com/"

; NSIS shows Readme files by opening the Readme file with the default application for
; the file's extension. "README.win32" won't work in most cases, because extension "win32"
; is usually not associated with an appropriate text editor. We should use extension "txt"
; for a text file or "html" for an html README file.
!define MUI_FINISHPAGE_TITLE_3LINES
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\Wireshark Release Notes.html"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the release notes"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
; NSIS runs as Administrator and will run Wireshark as Administrator
; if these are enabled.
;!define MUI_FINISHPAGE_RUN "$INSTDIR\${PROGRAM_NAME_PATH}"
;!define MUI_FINISHPAGE_RUN_NOTCHECKED

!define MUI_PAGE_CUSTOMFUNCTION_SHOW myShowCallback

; ============================================================================
; MUI Pages
; ============================================================================

!insertmacro MUI_PAGE_WELCOME

!define MUI_LICENSEPAGE_TEXT_TOP "Wireshark is distributed under the GNU General Public License."
!define MUI_LICENSEPAGE_TEXT_BOTTOM "This is not an end user license agreement (EULA). It is provided here for informational purposes only."
!define MUI_LICENSEPAGE_BUTTON "Noted"
!insertmacro MUI_PAGE_LICENSE "${STAGING_DIR}\COPYING.txt"

Page custom DisplayDonatePage

!insertmacro MUI_PAGE_COMPONENTS
!ifdef QT_DIR
Page custom DisplayAdditionalTasksPage LeaveAdditionalTasksPage
!endif
!insertmacro MUI_PAGE_DIRECTORY
Page custom DisplayNpcapPage
Page custom DisplayUSBPcapPage
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; Uninstall stuff (NSIS 2.08: "\r\n" don't work here)
!define MUI_UNCONFIRMPAGE_TEXT_TOP "The following ${PROGRAM_NAME} installation will be removed. Click 'Next' to continue."
; Uninstall stuff (this text isn't used with the MODERN_UI!)
;UninstallText "This will uninstall ${PROGRAM_NAME}.\r\nBefore starting the uninstallation, make sure ${PROGRAM_NAME} is not running.\r\nClick 'Next' to continue."

!define MUI_UNFINISHPAGE_NOAUTOCLOSE
!define MUI_WELCOMEPAGE_TITLE_3LINES
!define MUI_FINISHPAGE_TITLE_3LINES

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

; ============================================================================
; MUI Languages
; ============================================================================

!insertmacro MUI_LANGUAGE "English"

; ============================================================================
; Reserve Files
; ============================================================================

  ;Things that need to be extracted on first (keep these lines before any File command!)
  ;Only useful for BZIP2 compression

  ; Old Modern 1 UI: https://nsis.sourceforge.io/Docs/Modern%20UI/Readme.html
  ; To do: Upgrade to the Modern 2 UI:
  ;ReserveFile "AdditionalTasksPage.ini"
  ReserveFile "DonatePage.ini"
  ReserveFile "NpcapPage.ini"
  ReserveFile "USBPcapPage.ini"
  ReserveFile /plugin InstallOptions.dll

  ; Modern UI 2 / nsDialog pages.
  ; https://nsis.sourceforge.io/Docs/Modern%20UI%202/Readme.html
  ; https://nsis.sourceforge.io/Docs/nsDialogs/Readme.html
  !ifdef QT_DIR
  !include "wireshark-additional-tasks.nsdinc"
  !endif

; ============================================================================
; Section macros
; ============================================================================
!include "Sections.nsh"

; ============================================================================
; Command Line
; ============================================================================
!include "FileFunc.nsh"

!insertmacro GetParameters
!insertmacro GetOptions

; ========= Install extcap binary and help file =========
!macro InstallExtcap EXTCAP_NAME

  SetOutPath $INSTDIR
  File "${STAGING_DIR}\${EXTCAP_NAME}.html"
  SetOutPath $INSTDIR\extcap
  File "${STAGING_DIR}\extcap\wireshark\${EXTCAP_NAME}.exe"

!macroend

; ========= Check if silent mode install of /EXTRACOMPONENTS =========
!macro CheckExtrasFlag EXTRAS_NAME
  !define EXTRAS_FLAG ${__LINE__}
Section
  IfSilent +1 skip_${EXTRAS_FLAG}
  push $R0
  push $R1
  push $R2
  ${GetParameters} $R0
  ${GetOptions} $R0 "/EXTRACOMPONENTS=" $R1
  IfErrors popreg_${EXTRAS_FLAG}
  ${WordFind} $R1 "," "E+1" $R0

; No delimiters found - check for single word match
  ${If} $R0 = 1
    StrCmp $R1 ${EXTRAS_NAME} install_${EXTRAS_FLAG} popreg_${EXTRAS_FLAG}
  ${ENDIF}

; Loop through all delimited words checking for match
  IntOp $R2 0 + 1
  ${While} $R0 != 2
    StrCmp $R0 ${EXTRAS_NAME} install_${EXTRAS_FLAG} 0
    IntOp $R2 $R2 + 1
    ${WordFind} $R1 "," "E+$R2" $R0
  ${EndWhile}
  Goto popreg_${EXTRAS_FLAG}

install_${EXTRAS_FLAG}:
  !insertmacro InstallExtcap ${EXTRAS_NAME}
popreg_${EXTRAS_FLAG}:
  pop $R2
  pop $R1
  pop $R0
skip_${EXTRAS_FLAG}:
  !undef EXTRAS_FLAG
SectionEnd
!macroend

; ============================================================================
; Component page configuration
; ============================================================================
ComponentText "The following components are available for installation."

; ============================================================================
; Directory selection page configuration
; ============================================================================
; The text to prompt the user to enter a directory
DirText "Choose a directory in which to install ${PROGRAM_NAME}."

; The default installation directory
InstallDir $PROGRAMFILES64\${PROGRAM_NAME}

; See if this is an upgrade; if so, use the old InstallDir as default
InstallDirRegKey HKEY_LOCAL_MACHINE SOFTWARE\${PROGRAM_NAME} InstallDir


; ============================================================================
; Install page configuration
; ============================================================================
ShowInstDetails show

; ============================================================================
; Functions and macros
; ============================================================================

Var EXTENSION
; https://docs.microsoft.com/en-us/windows/win32/shell/fa-file-types
Function Associate
  Push $R0
  !insertmacro PushFileExtensions

  Pop $EXTENSION

  ${DoUntil} $EXTENSION == ${FILE_EXTENSION_MARKER}
    ReadRegStr $R0 HKCR $EXTENSION ""
    StrCmp $R0 "" Associate.doRegister
    Goto Associate.end

Associate.doRegister:
    ;The extension is not associated to any program, we can do the link
    WriteRegStr HKCR $EXTENSION "" ${WIRESHARK_ASSOC}
    DetailPrint "Registered file type: $EXTENSION"

Associate.end:
    Pop $EXTENSION
  ${Loop}

  Pop $R0
FunctionEnd

; Control states
Var START_MENU_STATE
Var DESKTOP_ICON_STATE
Var FILE_ASSOCIATE_STATE

; NSIS
Var OLD_UNINSTALLER
Var OLD_INSTDIR
Var OLD_DISPLAYNAME
Var TMP_UNINSTALLER

; WiX
Var REGISTRY_BITS
Var TMP_PRODUCT_GUID
Var WIX_DISPLAYNAME
Var WIX_DISPLAYVERSION
Var WIX_UNINSTALLSTRING

; ============================================================================
; Platform and OS version checks
; ============================================================================

!include x64.nsh
!include WinVer.nsh
!include WinMessages.nsh

Function .onInit
  ; http://forums.winamp.com/printthread.php?s=16ffcdd04a8c8d52bee90c0cae273ac5&threadid=262873
  ${IfNot} ${RunningX64}
    MessageBox MB_OK "Wireshark only runs on 64 bit machines.$\nTry installing a 32 bit version (3.6 or earlier) instead." /SD IDOK
    Abort
  ${EndIf}

  !if ${WIRESHARK_TARGET_PLATFORM} == "x64"
    ${If} ${IsNativeARM64}
      MessageBox MB_OK "You're installing the x64 version of Wireshark on an Arm64 system.$\nWe recommend using the native Arm64 installer instead." /SD IDOK
    ${EndIf}
  !endif

  !if ${WIRESHARK_TARGET_PLATFORM} == "arm64"
    ${IfNot} ${IsNativeARM64}
      MessageBox MB_OK "You're trying to install the Arm64 version of Wireshark on an x64 system.$\nTry the native x64 installer instead." /SD IDOK
      Abort
    ${EndIf}
  !endif

  ; This should match the following:
  ; - The NTDDI_VERSION and _WIN32_WINNT parts of cmakeconfig.h.in
  ; - The <compatibility><application> section in image\wireshark.exe.manifest.in
  ; - The VersionNT parts of packaging\wix\Prerequisites.wxi

  ; Uncomment to test.
  ; MessageBox MB_OK "You're running Windows $R0."

${If} ${AtMostWinME}
  MessageBox MB_OK \
    "Windows 95, 98, and ME are no longer supported.$\nPlease install Ethereal 0.99.0 instead." \
    /SD IDOK
  Quit
${EndIf}

${If} ${IsWinNT4}
  MessageBox MB_OK \
    "Windows NT 4.0 is no longer supported.$\nPlease install Wireshark 0.99.4 instead." \
    /SD IDOK
  Quit
${EndIf}

${If} ${IsWin2000}
  MessageBox MB_OK \
    "Windows 2000 is no longer supported.$\nPlease install Wireshark 1.2 or 1.0 instead." \
    /SD IDOK
  Quit
${EndIf}

${If} ${IsWinXP}
${OrIf} ${IsWin2003}
  MessageBox MB_OK \
    "Windows XP and Server 2003 are no longer supported.$\nPlease install ${PROGRAM_NAME} 1.12 or 1.10 instead." \
    /SD IDOK
  Quit
${EndIf}

${If} ${IsWinVista}
${OrIf} ${IsWin2008}
  MessageBox MB_OK \
    "Windows Vista and Server 2008 are no longer supported.$\nPlease install ${PROGRAM_NAME} 2.2 instead." \
    /SD IDOK
  Quit
${EndIf}

${If} ${AtMostWin8.1}
${OrIf} ${AtMostWin2012R2}
  MessageBox MB_OK \
    "Windows 7, 8, 8.1, Server 2008R2, and Server 2012 are no longer supported.$\nPlease install ${PROGRAM_NAME} 4.0 instead." \
    /SD IDOK
  Quit
${EndIf}

!insertmacro IsWiresharkRunning

  ; Default control values.
  StrCpy $START_MENU_STATE ${BST_CHECKED}
  StrCpy $DESKTOP_ICON_STATE ${BST_UNCHECKED}
  StrCpy $FILE_ASSOCIATE_STATE ${BST_CHECKED}

  ; Copied from https://nsis.sourceforge.io/Auto-uninstall_old_before_installing_new
  ReadRegStr $OLD_UNINSTALLER HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PROGRAM_NAME}" \
    "UninstallString"
  StrCmp $OLD_UNINSTALLER "" check_wix

  ReadRegStr $OLD_INSTDIR HKLM \
    "Software\Microsoft\Windows\CurrentVersion\App Paths\${PROGRAM_NAME}.exe" \
    "Path"
  StrCmp $OLD_INSTDIR "" check_wix

  ReadRegStr $OLD_DISPLAYNAME HKLM \
    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PROGRAM_NAME}" \
    "DisplayName"
  StrCmp $OLD_DISPLAYNAME "" done

  ; We're reinstalling. Flip our control states according to what the
  ; user chose before.
  ; (we use the "all users" start menu, so select it first)
  SetShellVarContext all
  ; MessageBox MB_OK|MB_ICONINFORMATION "oninit 1 sm $START_MENU_STATE di $DESKTOP_ICON_STATE"
  ${IfNot} ${FileExists} $SMPROGRAMS\${PROGRAM_NAME}.lnk
    StrCpy $START_MENU_STATE ${BST_UNCHECKED}
  ${Endif}
  ${If} ${FileExists} $DESKTOP\${PROGRAM_NAME}.lnk
    StrCpy $DESKTOP_ICON_STATE ${BST_CHECKED}
  ${Endif}
  ; Leave FILE_ASSOCIATE_STATE checked.
  ; MessageBox MB_OK|MB_ICONINFORMATION "oninit 2 sm $START_MENU_STATE $SMPROGRAMS\${PROGRAM_NAME}\${PROGRAM_NAME}.lnk \
  ;   $\ndi $DESKTOP_ICON_STATE $DESKTOP\${PROGRAM_NAME}.lnk

  MessageBox MB_YESNOCANCEL|MB_ICONQUESTION \
    "$OLD_DISPLAYNAME is already installed.\
     $\n$\nWould you like to uninstall it first?" \
      /SD IDYES \
      IDYES prep_nsis_uninstaller \
      IDNO done
  Abort

; Copy the uninstaller to $TEMP and run it.
; The uninstaller normally does this by itself, but doesn't wait around
; for the executable to finish, which means ExecWait won't work correctly.
prep_nsis_uninstaller:
  ClearErrors
  StrCpy $TMP_UNINSTALLER "$TEMP\${PROGRAM_NAME}_uninstaller.exe"
  ; ...because we surround UninstallString in quotes.
  StrCpy $0 $OLD_UNINSTALLER -1 1
  StrCpy $1 "$TEMP\${PROGRAM_NAME}_uninstaller.exe"
  StrCpy $2 1
  System::Call 'kernel32::CopyFile(t r0, t r1, b r2) 1'
  ExecWait "$TMP_UNINSTALLER /S _?=$OLD_INSTDIR"

  Delete "$TMP_UNINSTALLER"

; Look for a WiX-installed package.

check_wix:
  StrCpy $REGISTRY_BITS 64
  SetRegView 64
  check_wix_restart:
    StrCpy $0 0
  wix_reg_enum_loop:
    EnumRegKey $TMP_PRODUCT_GUID HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall" $0
    StrCmp $TMP_PRODUCT_GUID "" wix_enum_reg_done
    IntOp $0 $0 + 1
    ReadRegStr $WIX_DISPLAYNAME HKLM \
      "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$TMP_PRODUCT_GUID" \
      "DisplayName"
    ; MessageBox MB_OK|MB_ICONINFORMATION "Reading HKLM SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$1 DisplayName = $2"
    ; Look for "Wireshark".
    StrCmp $WIX_DISPLAYNAME "${PROGRAM_NAME}" wix_found wix_reg_enum_loop

    wix_found:
      ReadRegStr $WIX_DISPLAYVERSION HKLM \
        "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$TMP_PRODUCT_GUID" \
        "DisplayVersion"
      ReadRegStr $WIX_UNINSTALLSTRING HKLM \
        "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$TMP_PRODUCT_GUID" \
        "UninstallString"
      StrCmp $WIX_UNINSTALLSTRING "" done
      MessageBox MB_YESNOCANCEL|MB_ICONQUESTION \
        "$WIX_DISPLAYNAME $WIX_DISPLAYVERSION (msi) is already installed.\
         $\n$\nWould you like to uninstall it first?" \
          /SD IDYES \
          IDYES prep_wix_uninstaller \
          IDNO done
      Abort

      ; Run the WiX-provided UninstallString.
      prep_wix_uninstaller:
        ClearErrors
        ExecWait "$WIX_UNINSTALLSTRING"

      Goto done

  wix_enum_reg_done:
    ; MessageBox MB_OK|MB_ICONINFORMATION "Checked $0 $REGISTRY_BITS bit keys"
    IntCmp $REGISTRY_BITS 32 done
    StrCpy $REGISTRY_BITS 32
    SetRegView 32
    Goto check_wix_restart

done:

  ; Command line parameters
  ${GetParameters} $R0

  ${GetOptions} $R0 "/desktopicon=" $R1
  ${If} $R1 == "yes"
    StrCpy $DESKTOP_ICON_STATE ${BST_CHECKED}
  ${ElseIf} $R1 == "no"
    StrCpy $DESKTOP_ICON_STATE ${BST_UNCHECKED}
  ${Endif}

  ;Extract InstallOptions INI files
  ;!insertmacro INSTALLOPTIONS_EXTRACT "AdditionalTasksPage.ini"
  !insertmacro INSTALLOPTIONS_EXTRACT "DonatePage.ini"
  !insertmacro INSTALLOPTIONS_EXTRACT "NpcapPage.ini"
  !insertmacro INSTALLOPTIONS_EXTRACT "USBPcapPage.ini"
FunctionEnd

!ifdef QT_DIR
Function DisplayAdditionalTasksPage
  Call fnc_AdditionalTasksPage_Show
FunctionEnd
!endif

Function DisplayDonatePage
  !insertmacro MUI_HEADER_TEXT "Your donations keep these releases coming" "Donate today"
  !insertmacro INSTALLOPTIONS_DISPLAY "DonatePage.ini"
FunctionEnd

Function DisplayNpcapPage
  !insertmacro MUI_HEADER_TEXT "Packet Capture" "Wireshark requires Npcap to capture live network data."
  !insertmacro INSTALLOPTIONS_DISPLAY "NpcapPage.ini"
FunctionEnd

Function DisplayUSBPcapPage
  !insertmacro MUI_HEADER_TEXT "USB Capture" "USBPcap is required to capture USB traffic. Should USBPcap be installed (experimental)?"
  !insertmacro INSTALLOPTIONS_DISPLAY "USBPcapPage.ini"
FunctionEnd

; ============================================================================
; Installation execution commands
; ============================================================================

Var USBPCAP_UNINSTALL ;declare variable for holding the value of a registry key
;Var WIRESHARK_UNINSTALL ;declare variable for holding the value of a registry key

Section "-Required"
;-------------------------------------------

;
; Install for every user
;
SetShellVarContext all

SetOutPath $INSTDIR
!ifndef SKIP_UNINSTALLER
WriteUninstaller "$INSTDIR\${UNINSTALLER_NAME}"
!endif
File "${STAGING_DIR}\libwiretap.dll"
File "${STAGING_DIR}\libwireshark.dll"
File "${STAGING_DIR}\libwsutil.dll"

!include wireshark-manifest.nsh

File "${STAGING_DIR}\COPYING.txt"
File "${STAGING_DIR}\README.txt"
File "${STAGING_DIR}\wka"
File "${STAGING_DIR}\pdml2html.xsl"
File "${STAGING_DIR}\ws.css"
File "${STAGING_DIR}\wireshark.html"
File "${STAGING_DIR}\wireshark-filter.html"
File "${STAGING_DIR}\dumpcap.exe"
File "${STAGING_DIR}\dumpcap.html"
File "${STAGING_DIR}\extcap.html"
File "${STAGING_DIR}\ipmap.html"
File "${STAGING_DIR}\Wireshark Release Notes.html"

!ifdef USE_VCREDIST
; C-runtime redistributable
; vc_redist.x64.exe or vc_redist.x86.exe - copy and execute the redistributable installer
File "${VCREDIST_DIR}\${VCREDIST_EXE}"
; If the user already has the redistributable installed they will see a
; Big Ugly Dialog by default, asking if they want to uninstall or repair.
; Ideally we should add a checkbox for this somewhere. In the meantime,
; just do a "quiet" install.

; http://asawicki.info/news_1597_installing_visual_c_redistributable_package_from_command_line.html
ExecWait '"$INSTDIR\${VCREDIST_EXE}" /install /quiet /norestart' $0
DetailPrint "${VCREDIST_EXE} returned $0"

; https://docs.microsoft.com/en-us/windows/desktop/Msi/error-codes
!define ERROR_SUCCESS 0
!define ERROR_SUCCESS_REBOOT_INITIATED 1641
!define ERROR_PRODUCT_VERSION 1638
!define ERROR_SUCCESS_REBOOT_REQUIRED 3010
${Switch} $0
  ${Case} ${ERROR_SUCCESS}
  ${Case} ${ERROR_PRODUCT_VERSION}
    ${Break}
  ${Case} ${ERROR_SUCCESS_REBOOT_INITIATED} ; Shouldn't happen.
  ${Case} ${ERROR_SUCCESS_REBOOT_REQUIRED}
    SetRebootFlag true
    ${Break}
  ${Default}
      MessageBox MB_OK "The Visual C++ Redistributable installer failed with error $0.$\nUnable to continue installation." /SD IDOK
      Abort
    ${Break}
${EndSwitch}

Delete "$INSTDIR\${VCREDIST_EXE}"
!endif


; global config files - don't overwrite if already existing
;IfFileExists cfilters dont_overwrite_cfilters
File "${STAGING_DIR}\cfilters"
;dont_overwrite_cfilters:
;IfFileExists colorfilters dont_overwrite_colorfilters
File "${STAGING_DIR}\colorfilters"
;dont_overwrite_colorfilters:
;IfFileExists dfilters dont_overwrite_dfilters
File "${STAGING_DIR}\dfilters"
;dont_overwrite_dfilters:
;IfFileExists smi_modules dont_overwrite_smi_modules
File "${STAGING_DIR}\smi_modules"
;dont_overwrite_smi_modules:


;
; Install the Diameter DTD and XML files in the "diameter" subdirectory
; of the installation directory.
;
SetOutPath $INSTDIR\diameter
File "${STAGING_DIR}\diameter\AlcatelLucent.xml"
File "${STAGING_DIR}\diameter\chargecontrol.xml"
File "${STAGING_DIR}\diameter\Cisco.xml"
File "${STAGING_DIR}\diameter\CiscoSystems.xml"
File "${STAGING_DIR}\diameter\Custom.xml"
File "${STAGING_DIR}\diameter\dictionary.dtd"
File "${STAGING_DIR}\diameter\dictionary.xml"
File "${STAGING_DIR}\diameter\eap.xml"
File "${STAGING_DIR}\diameter\Ericsson.xml"
File "${STAGING_DIR}\diameter\etsie2e4.xml"
File "${STAGING_DIR}\diameter\HP.xml"
File "${STAGING_DIR}\diameter\Huawei.xml"
File "${STAGING_DIR}\diameter\Inovar.xml"
File "${STAGING_DIR}\diameter\Juniper.xml"
File "${STAGING_DIR}\diameter\Metaswitch.xml"
File "${STAGING_DIR}\diameter\Microsoft.xml"
File "${STAGING_DIR}\diameter\mobileipv4.xml"
File "${STAGING_DIR}\diameter\mobileipv6.xml"
File "${STAGING_DIR}\diameter\nasreq.xml"
File "${STAGING_DIR}\diameter\Nokia.xml"
File "${STAGING_DIR}\diameter\NokiaSolutionsAndNetworks.xml"
File "${STAGING_DIR}\diameter\Oracle.xml"
File "${STAGING_DIR}\diameter\Siemens.xml"
File "${STAGING_DIR}\diameter\sip.xml"
File "${STAGING_DIR}\diameter\Starent.xml"
File "${STAGING_DIR}\diameter\sunping.xml"
File "${STAGING_DIR}\diameter\Telefonica.xml"
File "${STAGING_DIR}\diameter\TGPP.xml"
File "${STAGING_DIR}\diameter\TGPP2.xml"
File "${STAGING_DIR}\diameter\Travelping.xml"
File "${STAGING_DIR}\diameter\Vodafone.xml"
File "${STAGING_DIR}\diameter\VerizonWireless.xml"
!include "custom_diameter_xmls.txt"
SetOutPath $INSTDIR

;
; Install the RADIUS directory files in the "radius" subdirectory
; of the installation directory.
;
SetOutPath $INSTDIR\radius
File "${STAGING_DIR}\radius\README.radius_dictionary"
File "${STAGING_DIR}\radius\custom.includes"
File "${STAGING_DIR}\radius\dictionary"
File "${STAGING_DIR}\radius\dictionary.3com"
File "${STAGING_DIR}\radius\dictionary.3gpp"
File "${STAGING_DIR}\radius\dictionary.3gpp2"
File "${STAGING_DIR}\radius\dictionary.5x9"
File "${STAGING_DIR}\radius\dictionary.acc"
File "${STAGING_DIR}\radius\dictionary.acme"
File "${STAGING_DIR}\radius\dictionary.actelis"
File "${STAGING_DIR}\radius\dictionary.adtran"
File "${STAGING_DIR}\radius\dictionary.adva"
File "${STAGING_DIR}\radius\dictionary.aerohive"
File "${STAGING_DIR}\radius\dictionary.airespace"
File "${STAGING_DIR}\radius\dictionary.alcatel"
File "${STAGING_DIR}\radius\dictionary.alcatel-lucent.aaa"
File "${STAGING_DIR}\radius\dictionary.alcatel.esam"
File "${STAGING_DIR}\radius\dictionary.alcatel.sr"
File "${STAGING_DIR}\radius\dictionary.alphion"
File "${STAGING_DIR}\radius\dictionary.alteon"
File "${STAGING_DIR}\radius\dictionary.altiga"
File "${STAGING_DIR}\radius\dictionary.alvarion"
File "${STAGING_DIR}\radius\dictionary.alvarion.wimax.v2_2"
File "${STAGING_DIR}\radius\dictionary.apc"
File "${STAGING_DIR}\radius\dictionary.aptilo"
File "${STAGING_DIR}\radius\dictionary.aptis"
File "${STAGING_DIR}\radius\dictionary.arbor"
File "${STAGING_DIR}\radius\dictionary.arista"
File "${STAGING_DIR}\radius\dictionary.aruba"
File "${STAGING_DIR}\radius\dictionary.ascend"
File "${STAGING_DIR}\radius\dictionary.ascend.illegal"
File "${STAGING_DIR}\radius\dictionary.ascend.illegal.extended"
File "${STAGING_DIR}\radius\dictionary.asn"
File "${STAGING_DIR}\radius\dictionary.audiocodes"
File "${STAGING_DIR}\radius\dictionary.avaya"
File "${STAGING_DIR}\radius\dictionary.azaire"
File "${STAGING_DIR}\radius\dictionary.bay"
File "${STAGING_DIR}\radius\dictionary.bintec"
File "${STAGING_DIR}\radius\dictionary.bigswitch"
File "${STAGING_DIR}\radius\dictionary.bintec"
File "${STAGING_DIR}\radius\dictionary.bluecoat"
File "${STAGING_DIR}\radius\dictionary.boingo"
File "${STAGING_DIR}\radius\dictionary.bristol"
File "${STAGING_DIR}\radius\dictionary.broadsoft"
File "${STAGING_DIR}\radius\dictionary.brocade"
File "${STAGING_DIR}\radius\dictionary.bskyb"
File "${STAGING_DIR}\radius\dictionary.bt"
File "${STAGING_DIR}\radius\dictionary.cablelabs"
File "${STAGING_DIR}\radius\dictionary.cabletron"
File "${STAGING_DIR}\radius\dictionary.calix"
File "${STAGING_DIR}\radius\dictionary.cambium"
File "${STAGING_DIR}\radius\dictionary.camiant"
File "${STAGING_DIR}\radius\dictionary.centec"
File "${STAGING_DIR}\radius\dictionary.checkpoint"
File "${STAGING_DIR}\radius\dictionary.chillispot"
File "${STAGING_DIR}\radius\dictionary.ciena"
File "${STAGING_DIR}\radius\dictionary.cisco"
File "${STAGING_DIR}\radius\dictionary.cisco.asa"
File "${STAGING_DIR}\radius\dictionary.cisco.bbsm"
File "${STAGING_DIR}\radius\dictionary.cisco.vpn3000"
File "${STAGING_DIR}\radius\dictionary.cisco.vpn5000"
File "${STAGING_DIR}\radius\dictionary.citrix"
File "${STAGING_DIR}\radius\dictionary.ckey"
File "${STAGING_DIR}\radius\dictionary.clavister"
File "${STAGING_DIR}\radius\dictionary.cnergee"
File "${STAGING_DIR}\radius\dictionary.colubris"
File "${STAGING_DIR}\radius\dictionary.columbia_university"
File "${STAGING_DIR}\radius\dictionary.compat"
File "${STAGING_DIR}\radius\dictionary.compatible"
File "${STAGING_DIR}\radius\dictionary.cosine"
File "${STAGING_DIR}\radius\dictionary.covaro"
File "${STAGING_DIR}\radius\dictionary.dante"
File "${STAGING_DIR}\radius\dictionary.dellemc"
File "${STAGING_DIR}\radius\dictionary.digium"
File "${STAGING_DIR}\radius\dictionary.dlink"
File "${STAGING_DIR}\radius\dictionary.dragonwave"
File "${STAGING_DIR}\radius\dictionary.efficientip"
File "${STAGING_DIR}\radius\dictionary.eleven"
File "${STAGING_DIR}\radius\dictionary.eltex"
File "${STAGING_DIR}\radius\dictionary.enterasys"
File "${STAGING_DIR}\radius\dictionary.epygi"
File "${STAGING_DIR}\radius\dictionary.equallogic"
File "${STAGING_DIR}\radius\dictionary.ericsson"
File "${STAGING_DIR}\radius\dictionary.ericsson.ab"
File "${STAGING_DIR}\radius\dictionary.ericsson.packet.core.networks"
File "${STAGING_DIR}\radius\dictionary.erx"
File "${STAGING_DIR}\radius\dictionary.extreme"
File "${STAGING_DIR}\radius\dictionary.f5"
File "${STAGING_DIR}\radius\dictionary.fdxtended"
File "${STAGING_DIR}\radius\dictionary.force10"
File "${STAGING_DIR}\radius\dictionary.fortinet"
File "${STAGING_DIR}\radius\dictionary.foundry"
File "${STAGING_DIR}\radius\dictionary.freedhcp"
File "${STAGING_DIR}\radius\dictionary.freeradius"
File "${STAGING_DIR}\radius\dictionary.freeradius.evs5"
File "${STAGING_DIR}\radius\dictionary.freeradius.internal"
File "${STAGING_DIR}\radius\dictionary.freeswitch"
File "${STAGING_DIR}\radius\dictionary.gandalf"
File "${STAGING_DIR}\radius\dictionary.garderos"
File "${STAGING_DIR}\radius\dictionary.gemtek"
File "${STAGING_DIR}\radius\dictionary.h3c"
File "${STAGING_DIR}\radius\dictionary.hillstone"
File "${STAGING_DIR}\radius\dictionary.hp"
File "${STAGING_DIR}\radius\dictionary.huawei"
File "${STAGING_DIR}\radius\dictionary.iana"
File "${STAGING_DIR}\radius\dictionary.identity_engines"
File "${STAGING_DIR}\radius\dictionary.iea"
File "${STAGING_DIR}\radius\dictionary.infinera"
File "${STAGING_DIR}\radius\dictionary.infoblox"
File "${STAGING_DIR}\radius\dictionary.infonet"
File "${STAGING_DIR}\radius\dictionary.ingate"
File "${STAGING_DIR}\radius\dictionary.ipunplugged"
File "${STAGING_DIR}\radius\dictionary.issanni"
File "${STAGING_DIR}\radius\dictionary.itk"
File "${STAGING_DIR}\radius\dictionary.jradius"
File "${STAGING_DIR}\radius\dictionary.juniper"
File "${STAGING_DIR}\radius\dictionary.karlnet"
File "${STAGING_DIR}\radius\dictionary.kineto"
File "${STAGING_DIR}\radius\dictionary.lancom"
File "${STAGING_DIR}\radius\dictionary.lantronix"
File "${STAGING_DIR}\radius\dictionary.livingston"
File "${STAGING_DIR}\radius\dictionary.localweb"
File "${STAGING_DIR}\radius\dictionary.lucent"
File "${STAGING_DIR}\radius\dictionary.manzara"
File "${STAGING_DIR}\radius\dictionary.meinberg"
File "${STAGING_DIR}\radius\dictionary.mellanox"
File "${STAGING_DIR}\radius\dictionary.meraki"
File "${STAGING_DIR}\radius\dictionary.merit"
File "${STAGING_DIR}\radius\dictionary.meru"
File "${STAGING_DIR}\radius\dictionary.microsemi"
File "${STAGING_DIR}\radius\dictionary.microsoft"
File "${STAGING_DIR}\radius\dictionary.mikrotik"
File "${STAGING_DIR}\radius\dictionary.mimosa"
File "${STAGING_DIR}\radius\dictionary.motorola"
File "${STAGING_DIR}\radius\dictionary.motorola.illegal"
File "${STAGING_DIR}\radius\dictionary.motorola.wimax"
File "${STAGING_DIR}\radius\dictionary.navini"
File "${STAGING_DIR}\radius\dictionary.net"
File "${STAGING_DIR}\radius\dictionary.netelastic"
File "${STAGING_DIR}\radius\dictionary.netscreen"
File "${STAGING_DIR}\radius\dictionary.networkphysics"
File "${STAGING_DIR}\radius\dictionary.nexans"
File "${STAGING_DIR}\radius\dictionary.nile"
File "${STAGING_DIR}\radius\dictionary.nokia"
File "${STAGING_DIR}\radius\dictionary.nokia.conflict"
File "${STAGING_DIR}\radius\dictionary.nomadix"
File "${STAGING_DIR}\radius\dictionary.nortel"
File "${STAGING_DIR}\radius\dictionary.ntua"
File "${STAGING_DIR}\radius\dictionary.openser"
File "${STAGING_DIR}\radius\dictionary.openwifi"
File "${STAGING_DIR}\radius\dictionary.packeteer"
File "${STAGING_DIR}\radius\dictionary.paloalto"
File "${STAGING_DIR}\radius\dictionary.patton"
File "${STAGING_DIR}\radius\dictionary.perle"
File "${STAGING_DIR}\radius\dictionary.pfsense"
File "${STAGING_DIR}\radius\dictionary.pica8"
File "${STAGING_DIR}\radius\dictionary.propel"
File "${STAGING_DIR}\radius\dictionary.prosoft"
File "${STAGING_DIR}\radius\dictionary.proxim"
File "${STAGING_DIR}\radius\dictionary.purewave"
File "${STAGING_DIR}\radius\dictionary.quiconnect"
File "${STAGING_DIR}\radius\dictionary.quintum"
File "${STAGING_DIR}\radius\dictionary.rcntec"
File "${STAGING_DIR}\radius\dictionary.redcreek"
File "${STAGING_DIR}\radius\dictionary.rfc2865"
File "${STAGING_DIR}\radius\dictionary.rfc2866"
File "${STAGING_DIR}\radius\dictionary.rfc2867"
File "${STAGING_DIR}\radius\dictionary.rfc2868"
File "${STAGING_DIR}\radius\dictionary.rfc2869"
File "${STAGING_DIR}\radius\dictionary.rfc3162"
File "${STAGING_DIR}\radius\dictionary.rfc3576"
File "${STAGING_DIR}\radius\dictionary.rfc3580"
File "${STAGING_DIR}\radius\dictionary.rfc4072"
File "${STAGING_DIR}\radius\dictionary.rfc4372"
File "${STAGING_DIR}\radius\dictionary.rfc4603"
File "${STAGING_DIR}\radius\dictionary.rfc4675"
File "${STAGING_DIR}\radius\dictionary.rfc4679"
File "${STAGING_DIR}\radius\dictionary.rfc4818"
File "${STAGING_DIR}\radius\dictionary.rfc4849"
File "${STAGING_DIR}\radius\dictionary.rfc5090"
File "${STAGING_DIR}\radius\dictionary.rfc5176"
File "${STAGING_DIR}\radius\dictionary.rfc5447"
File "${STAGING_DIR}\radius\dictionary.rfc5580"
File "${STAGING_DIR}\radius\dictionary.rfc5607"
File "${STAGING_DIR}\radius\dictionary.rfc5904"
File "${STAGING_DIR}\radius\dictionary.rfc6519"
File "${STAGING_DIR}\radius\dictionary.rfc6572"
File "${STAGING_DIR}\radius\dictionary.rfc6677"
File "${STAGING_DIR}\radius\dictionary.rfc6911"
File "${STAGING_DIR}\radius\dictionary.rfc6929"
File "${STAGING_DIR}\radius\dictionary.rfc6930"
File "${STAGING_DIR}\radius\dictionary.rfc7055"
File "${STAGING_DIR}\radius\dictionary.rfc7155"
File "${STAGING_DIR}\radius\dictionary.rfc7268"
File "${STAGING_DIR}\radius\dictionary.rfc7499"
File "${STAGING_DIR}\radius\dictionary.rfc7930"
File "${STAGING_DIR}\radius\dictionary.rfc8045"
File "${STAGING_DIR}\radius\dictionary.rfc8559"
File "${STAGING_DIR}\radius\dictionary.riverbed"
File "${STAGING_DIR}\radius\dictionary.riverstone"
File "${STAGING_DIR}\radius\dictionary.roaringpenguin"
File "${STAGING_DIR}\radius\dictionary.ruckus"
File "${STAGING_DIR}\radius\dictionary.ruggedcom"
File "${STAGING_DIR}\radius\dictionary.sangoma"
File "${STAGING_DIR}\radius\dictionary.sg"
File "${STAGING_DIR}\radius\dictionary.shasta"
File "${STAGING_DIR}\radius\dictionary.shiva"
File "${STAGING_DIR}\radius\dictionary.siemens"
File "${STAGING_DIR}\radius\dictionary.slipstream"
File "${STAGING_DIR}\radius\dictionary.smartsharesystems"
File "${STAGING_DIR}\radius\dictionary.sofaware"
File "${STAGING_DIR}\radius\dictionary.softbank"
File "${STAGING_DIR}\radius\dictionary.sonicwall"
File "${STAGING_DIR}\radius\dictionary.springtide"
File "${STAGING_DIR}\radius\dictionary.starent"
File "${STAGING_DIR}\radius\dictionary.starent.vsa1"
File "${STAGING_DIR}\radius\dictionary.surfnet"
File "${STAGING_DIR}\radius\dictionary.symbol"
File "${STAGING_DIR}\radius\dictionary.t_systems_nova"
File "${STAGING_DIR}\radius\dictionary.telebit"
File "${STAGING_DIR}\radius\dictionary.telkom"
File "${STAGING_DIR}\radius\dictionary.telrad"
File "${STAGING_DIR}\radius\dictionary.terena"
File "${STAGING_DIR}\radius\dictionary.tplink"
File "${STAGING_DIR}\radius\dictionary.trapeze"
File "${STAGING_DIR}\radius\dictionary.travelping"
File "${STAGING_DIR}\radius\dictionary.tripplite"
File "${STAGING_DIR}\radius\dictionary.tropos"
File "${STAGING_DIR}\radius\dictionary.ukerna"
File "${STAGING_DIR}\radius\dictionary.unisphere"
File "${STAGING_DIR}\radius\dictionary.unix"
File "${STAGING_DIR}\radius\dictionary.usr"
File "${STAGING_DIR}\radius\dictionary.usr.illegal"
File "${STAGING_DIR}\radius\dictionary.utstarcom"
File "${STAGING_DIR}\radius\dictionary.valemount"
File "${STAGING_DIR}\radius\dictionary.vasexperts"
File "${STAGING_DIR}\radius\dictionary.verizon"
File "${STAGING_DIR}\radius\dictionary.versanet"
File "${STAGING_DIR}\radius\dictionary.walabi"
File "${STAGING_DIR}\radius\dictionary.waverider"
File "${STAGING_DIR}\radius\dictionary.wichorus"
File "${STAGING_DIR}\radius\dictionary.wifialliance"
File "${STAGING_DIR}\radius\dictionary.wimax"
File "${STAGING_DIR}\radius\dictionary.wimax.alvarion"
File "${STAGING_DIR}\radius\dictionary.wimax.wichorus"
File "${STAGING_DIR}\radius\dictionary.wispr"
File "${STAGING_DIR}\radius\dictionary.xedia"
File "${STAGING_DIR}\radius\dictionary.xylan"
File "${STAGING_DIR}\radius\dictionary.yubico"
File "${STAGING_DIR}\radius\dictionary.zeus"
File "${STAGING_DIR}\radius\dictionary.zte"
File "${STAGING_DIR}\radius\dictionary.zyxel"
!include "custom_radius_dict.txt"
SetOutPath $INSTDIR

;
; install the dtds in the dtds subdirectory
;
SetOutPath $INSTDIR\dtds
File "${STAGING_DIR}\dtds\dc.dtd"
File "${STAGING_DIR}\dtds\itunes.dtd"
File "${STAGING_DIR}\dtds\mscml.dtd"
File "${STAGING_DIR}\dtds\pocsettings.dtd"
File "${STAGING_DIR}\dtds\presence.dtd"
File "${STAGING_DIR}\dtds\reginfo.dtd"
File "${STAGING_DIR}\dtds\rlmi.dtd"
File "${STAGING_DIR}\dtds\rss.dtd"
File "${STAGING_DIR}\dtds\smil.dtd"
File "${STAGING_DIR}\dtds\xcap-caps.dtd"
File "${STAGING_DIR}\dtds\xcap-error.dtd"
File "${STAGING_DIR}\dtds\watcherinfo.dtd"
SetOutPath $INSTDIR

; Create the extcap directory
CreateDirectory $INSTDIR\extcap

;
; install the protobuf .proto definitions in the protobuf subdirectory
;
SetOutPath $INSTDIR\protobuf
File "${STAGING_DIR}\protobuf\*.proto"

; Install the TPNCP DAT file in the "tpncp" subdirectory
; of the installation directory.
SetOutPath $INSTDIR\tpncp
File "${STAGING_DIR}\tpncp\tpncp.dat"

;
; install the wimaxasncp TLV definitions in the wimaxasncp subdirectory
;
SetOutPath $INSTDIR\wimaxasncp
File "${STAGING_DIR}\wimaxasncp\dictionary.xml"
File "${STAGING_DIR}\wimaxasncp\dictionary.dtd"
SetOutPath $INSTDIR

; Write the installation path into the registry for InstallDirRegKey
WriteRegStr HKEY_LOCAL_MACHINE SOFTWARE\${PROGRAM_NAME} InstallDir "$INSTDIR"

; Write the uninstall keys for Windows
; https://nsis.sourceforge.io/Add_uninstall_information_to_Add/Remove_Programs
; https://docs.microsoft.com/en-us/previous-versions/ms954376(v=msdn.10)
; https://docs.microsoft.com/en-us/windows/win32/msi/uninstall-registry-key
!define UNINSTALL_PATH "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PROGRAM_NAME}"

WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "Comments" "${DISPLAY_NAME}"
!ifdef QT_DIR
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "DisplayIcon" "$INSTDIR\${PROGRAM_NAME_PATH},0"
!endif
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "DisplayName" "${DISPLAY_NAME}"
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "DisplayVersion" "${VERSION}"
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "HelpLink" "https://ask.wireshark.org/"
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "InstallLocation" "$INSTDIR"
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "Publisher" "The Wireshark developer community, https://www.wireshark.org"
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "URLInfoAbout" "https://www.wireshark.org"
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "URLUpdateInfo" "https://www.wireshark.org/download.html"

WriteRegDWORD HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "NoModify" 1
WriteRegDWORD HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "NoRepair" 1
WriteRegDWORD HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "VersionMajor" ${MAJOR_VERSION}
WriteRegDWORD HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "VersionMinor" ${MINOR_VERSION}

WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "UninstallString" '"$INSTDIR\${UNINSTALLER_NAME}"'
WriteRegStr HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "QuietUninstallString" '"$INSTDIR\${UNINSTALLER_NAME}" /S'

; To quote https://web.archive.org/web/20150911221413/http://download.microsoft.com/download/0/4/6/046bbd36-0812-4c22-a870-41911c6487a6/WindowsUserExperience.pdf:
; "Do not include Readme, Help, or Uninstall entries on the Programs menu."
Delete "$SMPROGRAMS\${PROGRAM_NAME}\Wireshark Web Site.lnk"

; Create file extensions if the Associated Tasks page check box
; is checked.
${If} $FILE_ASSOCIATE_STATE == ${BST_CHECKED}
WriteRegStr HKCR ${WIRESHARK_ASSOC} "" "Wireshark capture file"
WriteRegStr HKCR "${WIRESHARK_ASSOC}\Shell\open\command" "" '"$INSTDIR\${PROGRAM_NAME_PATH}" "%1"'
WriteRegStr HKCR "${WIRESHARK_ASSOC}\DefaultIcon" "" '"$INSTDIR\${PROGRAM_NAME_PATH}",1'
; We refresh the icon cache down in -Finally.
Call Associate
; If you add something here be sure to sync it with the uninstall section and the
; AdditionalTasks page
${Endif}

; if running as a silent installer, don't try to install npcap
IfSilent SecRequired_skip_Npcap

; Install Npcap (depending on npcap page setting)
ReadINIStr $0 "$PLUGINSDIR\NpcapPage.ini" "Field 4" "State"
StrCmp $0 "0" SecRequired_skip_Npcap
SetOutPath $INSTDIR
File "${EXTRA_INSTALLER_DIR}\npcap-${NPCAP_PACKAGE_VERSION}.exe"
ExecWait '"$INSTDIR\npcap-${NPCAP_PACKAGE_VERSION}.exe" /winpcap_mode=no /loopback_support=no' $0
DetailPrint "Npcap installer returned $0"
SecRequired_skip_Npcap:

; If running as a silent installer, don't try to install USBPcap
IfSilent SecRequired_skip_USBPcap

ReadINIStr $0 "$PLUGINSDIR\USBPcapPage.ini" "Field 4" "State"
StrCmp $0 "0" SecRequired_skip_USBPcap
SetOutPath $INSTDIR
File "${EXTRA_INSTALLER_DIR}\USBPcapSetup-${USBPCAP_PACKAGE_VERSION}.exe"
ExecWait '"$INSTDIR\USBPcapSetup-${USBPCAP_PACKAGE_VERSION}.exe"' $0
DetailPrint "USBPcap installer returned $0"
${If} $0 == "0"
  ${If} ${RunningX64}
    ${DisableX64FSRedirection}
    SetRegView 64
  ${EndIf}
  ReadRegStr $USBPCAP_UNINSTALL HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\Uninstall\USBPcap" "UninstallString"
  ${If} ${RunningX64}
    ${EnableX64FSRedirection}
    SetRegView 32
  ${EndIf}
  ${StrRep} $0 '$USBPCAP_UNINSTALL' 'Uninstall.exe' 'USBPcapCMD.exe'
  ${StrRep} $1 '$0' '"' ''
  CopyFiles  /SILENT $1 $INSTDIR\extcap
  SetRebootFlag true
${EndIf}
SecRequired_skip_USBPcap:

; If no user profile exists for Wireshark but for Ethereal, copy it over
SetShellVarContext current
IfFileExists $APPDATA\Wireshark profile_done
IfFileExists $APPDATA\Ethereal 0 profile_done
;MessageBox MB_YESNO "This seems to be the first time you use Wireshark. Copy over the personal settings from Ethereal?" /SD IDYES IDNO profile_done
CreateDirectory $APPDATA\Wireshark
CopyFiles $APPDATA\Ethereal\*.* $APPDATA\Wireshark
profile_done:
SetShellVarContext all

SectionEnd ; "Required"

!ifdef QT_DIR
Section "${PROGRAM_NAME}" SecWiresharkQt
;-------------------------------------------
; by default, Wireshark.exe is installed
SetOutPath $INSTDIR
File "${QT_DIR}\${PROGRAM_NAME_PATH}"
File /r "${QT_DIR}\translations"
; Write an entry for ShellExecute
WriteRegStr HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\App Paths\${PROGRAM_NAME_PATH}" "" '$INSTDIR\${PROGRAM_NAME_PATH}'
WriteRegStr HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\App Paths\${PROGRAM_NAME_PATH}" "Path" '$INSTDIR'

!ifndef SKIP_NSIS_QT_DLLS
!include wireshark-qt-manifest.nsh
!endif

; Is the Start Menu check box checked?
${If} $START_MENU_STATE == ${BST_CHECKED}
  CreateShortCut "$SMPROGRAMS\${PROGRAM_NAME}.lnk" "$INSTDIR\${PROGRAM_NAME_PATH}" "" "$INSTDIR\${PROGRAM_NAME_PATH}" 0 "" "" "${PROGRAM_FULL_NAME}"
${Endif}

${If} $DESKTOP_ICON_STATE == ${BST_CHECKED}
  CreateShortCut "$DESKTOP\${PROGRAM_NAME}.lnk" "$INSTDIR\${PROGRAM_NAME_PATH}" "" "$INSTDIR\${PROGRAM_NAME_PATH}" 0 "" "" "${PROGRAM_FULL_NAME}"
${Endif}

SectionEnd ; "SecWiresharkQt"
!endif


Section "TShark" SecTShark
;-------------------------------------------
SetOutPath $INSTDIR
File "${STAGING_DIR}\tshark.exe"
File "${STAGING_DIR}\tshark.html"
SectionEnd

Section "-Plugins & Extensions"

SetOutPath '$INSTDIR\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs'
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\g711.dll"
!ifdef SPANDSP_FOUND
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\g722.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\g726.dll"
!endif
!ifdef BCG729_FOUND
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\g729.dll"
!endif
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\l16mono.dll"
!ifdef SBC_FOUND
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\sbc.dll"
!endif
!ifdef ILBC_FOUND
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\ilbc.dll"
!endif
!ifdef OPUS_FOUND
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\opus_dec.dll"
!endif
!ifdef AMRNB_FOUND
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\codecs\amrnb.dll"
!endif

; This should be a function or macro
SetOutPath '$INSTDIR\profiles\Bluetooth'
File "${STAGING_DIR}\profiles\Bluetooth\colorfilters"
File "${STAGING_DIR}\profiles\Bluetooth\preferences"
SetOutPath '$INSTDIR\profiles\Classic'
File "${STAGING_DIR}\profiles\Classic\colorfilters"
SetOutPath '$INSTDIR\profiles\No Reassembly'
File "${STAGING_DIR}\profiles\No Reassembly\preferences"

SetOutPath '$INSTDIR\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan'
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\ethercat.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\gryphon.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\ipaddr.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\irda.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\opcua.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\profinet.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\unistim.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\wimax.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\wimaxasncp.dll"
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\wimaxmacphy.dll"
!include "custom_plugins.txt"

SetOutPath '$INSTDIR\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\wiretap'
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\wiretap\usbdump.dll"

SetOutPath '$INSTDIR\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan'
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\mate.dll"

!ifdef SMI_DIR
SetOutPath '$INSTDIR\snmp\mibs'
File "${SMI_DIR}\share\mibs\iana\*"
File "${SMI_DIR}\share\mibs\ietf\*"
File "${SMI_DIR}\share\mibs\irtf\*"
File "${SMI_DIR}\share\mibs\tubs\*"
File "${SMI_DIR}\share\pibs\*"
File "${SMI_DIR}\share\yang\*.yang"
!include "custom_mibs.txt"
!endif

SetOutPath '$INSTDIR\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan'
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\transum.dll"

SetOutPath '$INSTDIR\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan'
File "${STAGING_DIR}\plugins\${MAJOR_VERSION}.${MINOR_VERSION}\epan\stats_tree.dll"

SectionEnd ; "Plugins / Extensions"

Section "-Additional command line tools"

SetOutPath $INSTDIR
File "${STAGING_DIR}\capinfos.exe"
File "${STAGING_DIR}\capinfos.html"

File "${STAGING_DIR}\captype.exe"
File "${STAGING_DIR}\captype.html"

File "${STAGING_DIR}\editcap.exe"
File "${STAGING_DIR}\editcap.html"

File "${STAGING_DIR}\mergecap.exe"
File "${STAGING_DIR}\mergecap.html"

!ifdef MMDBRESOLVE_EXE
File "${STAGING_DIR}\mmdbresolve.html"
File "${STAGING_DIR}\mmdbresolve.exe"
!endif

File "${STAGING_DIR}\randpkt.exe"
File "${STAGING_DIR}\randpkt.html"

File "${STAGING_DIR}\rawshark.exe"
File "${STAGING_DIR}\rawshark.html"

File "${STAGING_DIR}\reordercap.exe"
File "${STAGING_DIR}\reordercap.html"

File "${STAGING_DIR}\sharkd.exe"
;File "${STAGING_DIR}\sharkd.html"

File "${STAGING_DIR}\text2pcap.exe"
File "${STAGING_DIR}\text2pcap.html"

SectionEnd ; "Tools"

SectionGroup /e "External capture tools (extcap)" SecExtcapGroup

Section /o "Androiddump" SecAndroiddump
;-------------------------------------------
  !insertmacro InstallExtcap "androiddump"
SectionEnd
!insertmacro CheckExtrasFlag "androiddump"

!ifdef BUILD_etwdump
Section "Etwdump" SecEtwdump
;-------------------------------------------
  !insertmacro InstallExtcap "Etwdump"
SectionEnd
!insertmacro CheckExtrasFlag "Etwdump"
!endif

Section /o "Randpktdump" SecRandpktdump
;-------------------------------------------
  !insertmacro InstallExtcap "randpktdump"
SectionEnd
!insertmacro CheckExtrasFlag "randpktdump"

!ifdef LIBSSH_FOUND
Section /o "Sshdump, Ciscodump, and Wifidump" SecSshdump
;-------------------------------------------
  !insertmacro InstallExtcap "sshdump"
  !insertmacro InstallExtcap "ciscodump"
  !insertmacro InstallExtcap "wifidump"
SectionEnd
!insertmacro CheckExtrasFlag "sshdump"
!insertmacro CheckExtrasFlag "ciscodump"
!insertmacro CheckExtrasFlag "wifidump"
!endif

Section /o "UDPdump" SecUDPdump
;-------------------------------------------
  !insertmacro InstallExtcap "udpdump"
SectionEnd
!insertmacro CheckExtrasFlag "udpdump"

SectionGroupEnd ; "External Capture (extcap)"

Section "-Clear Partial Selected"
!insertmacro ClearSectionFlag ${SecExtcapGroup} ${SF_PSELECTED}
SectionEnd

!ifdef DOC_DIR
Section "-Documentation"

SetOutPath "$INSTDIR\Wireshark User's Guide"
File /r "${DOC_DIR}\wsug_html_chunked\*.*"

SectionEnd
!endif

Section "-Finally"

!insertmacro UpdateIcons

; Compute and write the installation directory size
${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
IntFmt $0 "0x%08X" $0
WriteRegDWORD HKEY_LOCAL_MACHINE "${UNINSTALL_PATH}" "EstimatedSize" "$0"

SectionEnd

; ============================================================================
; Section macros
; ============================================================================
!include "Sections.nsh"

; ============================================================================
; Uninstall page configuration
; ============================================================================
ShowUninstDetails show

; ============================================================================
; Functions and macros
; ============================================================================

Function un.Disassociate
  Push $R0
!insertmacro PushFileExtensions

  Pop $EXTENSION
  ${DoUntil} $EXTENSION == ${FILE_EXTENSION_MARKER}
    ReadRegStr $R0 HKCR $EXTENSION ""
    StrCmp $R0 ${WIRESHARK_ASSOC} un.Disassociate.doDeregister
    Goto un.Disassociate.end
un.Disassociate.doDeregister:
    ; The extension is associated with Wireshark so, we must destroy this!
    DeleteRegKey HKCR $EXTENSION
    DetailPrint "Deregistered file type: $EXTENSION"
un.Disassociate.end:
    Pop $EXTENSION
  ${Loop}

  Pop $R0
FunctionEnd

Section "-Required"
SectionEnd

!define EXECUTABLE_MARKER "EXECUTABLE_MARKER"
Var EXECUTABLE

Section /o "Un.USBPcap" un.SecUSBPcap
;-------------------------------------------
SectionIn 2
${If} ${RunningX64}
    ${DisableX64FSRedirection}
    SetRegView 64
${EndIf}
ReadRegStr $1 HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\Uninstall\USBPcap" "UninstallString"
${If} ${RunningX64}
    ${EnableX64FSRedirection}
    SetRegView 32
${EndIf}
${If} $1 != ""
    ${UnStrRep} $2 '$1' '\Uninstall.exe' ''
    ${UnStrRep} $3 '$2' '"' ''
    ExecWait '$1 _?=$3' $0
    DetailPrint "USBPcap uninstaller returned $0"
    ${If} $0 == "0"
        Delete "$3\Uninstall.exe"
        Delete "$INSTDIR\extcap\wireshark\USBPcapCMD.exe"
    ${EndIf}
${EndIf}
ClearErrors
SectionEnd


Section "Uninstall" un.SecUinstall
;-------------------------------------------
;
; UnInstall for every user
;
SectionIn 1 2
SetShellVarContext all

!insertmacro IsWiresharkRunning

Push "${EXECUTABLE_MARKER}"
Push "${PROGRAM_NAME}"
Push "capinfos"
Push "captype"
Push "dftest"
Push "dumpcap"
Push "editcap"
Push "mergecap"
Push "randpkt"
Push "rawshark"
Push "reordercap"
Push "sharkd"
Push "text2pcap"
Push "tshark"

!ifdef MMDBRESOLVE_EXE
Push "mmdbresolve"
!endif

Pop $EXECUTABLE
${DoUntil} $EXECUTABLE == ${EXECUTABLE_MARKER}

  ; IsWiresharkRunning should make sure everything is closed down so we *shouldn't* run
  ; into any problems here.
  Delete "$INSTDIR\$EXECUTABLE.exe"
  IfErrors 0 deletionSuccess
    MessageBox MB_OK "$EXECUTABLE.exe could not be removed. Is it in use?" /SD IDOK IDOK 0
    Abort "$EXECUTABLE.exe could not be removed. Aborting the uninstall process."

deletionSuccess:
  Pop $EXECUTABLE

${Loop}


DeleteRegKey HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PROGRAM_NAME}"
DeleteRegKey HKEY_LOCAL_MACHINE "Software\${PROGRAM_NAME}"
DeleteRegKey HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\App Paths\${PROGRAM_NAME}.exe"

Call un.Disassociate

DeleteRegKey HKCR ${WIRESHARK_ASSOC}
DeleteRegKey HKCR "${WIRESHARK_ASSOC}\Shell\open\command"
DeleteRegKey HKCR "${WIRESHARK_ASSOC}\DefaultIcon"

Delete "$INSTDIR\*.dll"
Delete "$INSTDIR\*.exe"
Delete "$INSTDIR\*.html"
Delete "$INSTDIR\*.qm"
Delete "$INSTDIR\accessible\*.*"
Delete "$INSTDIR\AUTHORS-SHORT"
Delete "$INSTDIR\COPYING*"
Delete "$INSTDIR\audio\*.*"
Delete "$INSTDIR\bearer\*.*"
Delete "$INSTDIR\diameter\*.*"
Delete "$INSTDIR\extcap\androiddump.*"
Delete "$INSTDIR\extcap\ciscodump.*"
Delete "$INSTDIR\extcap\etwdump.*"
Delete "$INSTDIR\extcap\randpktdump.*"
Delete "$INSTDIR\extcap\sshdump.*"
Delete "$INSTDIR\extcap\udpdump.*"
Delete "$INSTDIR\extcap\wifidump.*"
Delete "$INSTDIR\gpl-2.0-standalone.html"
Delete "$INSTDIR\Acknowledgements.md"
Delete "$INSTDIR\generic\*.*"
Delete "$INSTDIR\help\*.*"
Delete "$INSTDIR\iconengines\*.*"
Delete "$INSTDIR\imageformats\*.*"
Delete "$INSTDIR\mediaservice\*.*"
Delete "$INSTDIR\multimedia\*.*"
Delete "$INSTDIR\networkinformation\*.*"
Delete "$INSTDIR\platforms\*.*"
Delete "$INSTDIR\playlistformats\*.*"
Delete "$INSTDIR\printsupport\*.*"
Delete "$INSTDIR\share\glib-2.0\schemas\*.*"
Delete "$INSTDIR\snmp\*.*"
Delete "$INSTDIR\snmp\mibs\*.*"
Delete "$INSTDIR\styles\translations\*.*"
Delete "$INSTDIR\styles\*.*"
Delete "$INSTDIR\protobuf\*.*"
Delete "$INSTDIR\tls\*.*"
Delete "$INSTDIR\tpncp\*.*"
Delete "$INSTDIR\translations\*.*"
Delete "$INSTDIR\ui\*.*"
Delete "$INSTDIR\wimaxasncp\*.*"
Delete "$INSTDIR\ws.css"
; previous versions installed these files
Delete "$INSTDIR\*.manifest"
; previous versions installed this file
Delete "$INSTDIR\AUTHORS-SHORT-FORMAT"
Delete "$INSTDIR\README*"
Delete "$INSTDIR\NEWS.txt"
Delete "$INSTDIR\manuf"
Delete "$INSTDIR\wka"
Delete "$INSTDIR\services"
Delete "$INSTDIR\pdml2html.xsl"
Delete "$INSTDIR\pcrepattern.3.txt"
Delete "$INSTDIR\user-guide.chm"
Delete "$INSTDIR\example_snmp_users_file"
Delete "$INSTDIR\ipmap.html"
Delete "$INSTDIR\radius\*.*"
Delete "$INSTDIR\dtds\*.*"
Delete "$INSTDIR\browser_sslkeylog.lua"
Delete "$INSTDIR\console.lua"
Delete "$INSTDIR\dtd_gen.lua"
Delete "$INSTDIR\init.lua"
Delete "$INSTDIR\release-notes.html"
Delete "$INSTDIR\Wireshark Release Notes.html"

RMDir "$INSTDIR\accessible"
RMDir "$INSTDIR\audio"
RMDir "$INSTDIR\bearer"
RMDir "$INSTDIR\extcap"
RMDir "$INSTDIR\extcap"
RMDir "$INSTDIR\iconengines"
RMDir "$INSTDIR\imageformats"
RMDir "$INSTDIR\mediaservice"
RMDir "$INSTDIR\multimedia"
RMDir "$INSTDIR\networkinformation"
RMDir "$INSTDIR\platforms"
RMDir "$INSTDIR\playlistformats"
RMDir "$INSTDIR\printsupport"
RMDir "$INSTDIR\styles\translations"
RMDir "$INSTDIR\styles"
RMDir "$SMPROGRAMS\${PROGRAM_NAME}"
RMDir "$INSTDIR\help"
RMDir "$INSTDIR\generic"
RMDir /r "$INSTDIR\Wireshark User's Guide"
RMDir "$INSTDIR\diameter"
RMDir "$INSTDIR\snmp\mibs"
RMDir "$INSTDIR\snmp"
RMDir "$INSTDIR\radius"
RMDir "$INSTDIR\dtds"
RMDir "$INSTDIR\protobuf"
RMDir "$INSTDIR\tls"
RMDir "$INSTDIR\tpncp"
RMDir "$INSTDIR\translations"
RMDir "$INSTDIR\ui"
RMDir "$INSTDIR\wimaxasncp"
RMDir "$INSTDIR"

SectionEnd ; "Uinstall"

Section "Un.Plugins" un.SecPlugins
;-------------------------------------------
SectionIn 1 2
;Delete "$INSTDIR\plugins\${VERSION}\*.*"
;Delete "$INSTDIR\plugins\*.*"
;RMDir "$INSTDIR\plugins\${VERSION}"
;RMDir "$INSTDIR\plugins"
RMDir /r "$INSTDIR\plugins"
SectionEnd

Section "Un.Global Profiles" un.SecProfiles
;-------------------------------------------
SectionIn 1 2
RMDir /r "$INSTDIR\profiles"
SectionEnd

Section "Un.Global Settings" un.SecGlobalSettings
;-------------------------------------------
SectionIn 1 2
Delete "$INSTDIR\cfilters"
Delete "$INSTDIR\colorfilters"
Delete "$INSTDIR\dfilters"
Delete "$INSTDIR\enterprises.tsv"
Delete "$INSTDIR\smi_modules"
RMDir "$INSTDIR"
SectionEnd

Section /o "Un.Personal Settings" un.SecPersonalSettings
;-------------------------------------------
SectionIn 2
SetShellVarContext current
Delete "$APPDATA\${PROGRAM_NAME}\*.*"
RMDir "$APPDATA\${PROGRAM_NAME}"
DeleteRegKey HKCU "Software\${PROGRAM_NAME}"
SectionEnd

;VAR un.NPCAP_UNINSTALL

Section /o "Un.Npcap" un.SecNpcap
;-------------------------------------------
SectionIn 2
ReadRegStr $1 HKEY_LOCAL_MACHINE "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NpcapInst" "UninstallString"
;IfErrors un.lbl_npcap_notinstalled ;if RegKey is unavailable, Npcap is not installed
${If} $1 != ""
  ;MessageBox MB_OK "Npcap $1" /SD IDOK
  ExecWait '$1' $0
  DetailPrint "Npcap uninstaller returned $0"
  ;SetRebootFlag true
${EndIf}
;un.lbl_npcap_notinstalled:
SectionEnd

Section "-Un.Finally"
;-------------------------------------------
SectionIn 1 2

!insertmacro UpdateIcons

; this test must be done after all other things uninstalled (e.g. Global Settings)
IfFileExists "$INSTDIR" 0 NoFinalErrorMsg
    MessageBox MB_OK "Unable to remove $INSTDIR." /SD IDOK IDOK 0 ; skipped if dir doesn't exist
NoFinalErrorMsg:
SectionEnd

; Sign our installer and uninstaller during compilation.
!ifdef ENABLE_SIGNED_NSIS
!finalize 'sign-wireshark.bat "%1"' = 0 ; %1 is replaced by the installer exe to be signed.
!uninstfinalize 'sign-wireshark.bat "%1"' = 0 ; %1 is replaced by the uninstaller exe to be signed.
!endif

; ============================================================================
; PLEASE MAKE SURE, THAT THE DESCRIPTIVE TEXT FITS INTO THE DESCRIPTION FIELD!
; ============================================================================
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
!ifdef QT_DIR
  !insertmacro MUI_DESCRIPTION_TEXT ${SecWiresharkQt} "The main network protocol analyzer application."
!endif
  !insertmacro MUI_DESCRIPTION_TEXT ${SecTShark} "Text based network protocol analyzer."

  !insertmacro MUI_DESCRIPTION_TEXT ${SecExtcapGroup} "External Capture Interfaces"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAndroiddump} "Provide capture interfaces from Android devices."
  !ifdef BUILD_etwdump
  !insertmacro MUI_DESCRIPTION_TEXT ${SecEtwdump} "Provide an interface to read Event Tracing for Windows (ETW) event trace (ETL)."
  !endif
  !insertmacro MUI_DESCRIPTION_TEXT ${SecRandpktdump} "Provide an interface to the random packet generator. (see also randpkt)"
  !ifdef LIBSSH_FOUND
  !insertmacro MUI_DESCRIPTION_TEXT ${SecSshdump} "Provide remote capture through SSH. (tcpdump, Cisco EPC, wifi)"
  !endif
  !insertmacro MUI_DESCRIPTION_TEXT ${SecUDPdump} "Provide capture interface to receive UDP packets streamed from network devices."

!insertmacro MUI_FUNCTION_DESCRIPTION_END

!insertmacro MUI_UNFUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecUinstall} "Uninstall all ${PROGRAM_NAME} components."
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecPlugins} "Uninstall all Plugins (even from previous ${PROGRAM_NAME} versions)."
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecProfiles} "Uninstall all global configuration profiles."
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecGlobalSettings} "Uninstall global settings like: $INSTDIR\cfilters"
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecPersonalSettings} "Delete personal configuration folder: $APPDATA\${PROGRAM_NAME}."
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecNpcap} "Call Npcap's uninstall program."
  !insertmacro MUI_DESCRIPTION_TEXT ${un.SecUSBPcap} "Call USBPcap's uninstall program."
!insertmacro MUI_UNFUNCTION_DESCRIPTION_END

; ============================================================================
; Callback functions
; ============================================================================
!ifdef QT_DIR

Var QT_SELECTED

; Called from fnc_AdditionalTasksPage_Create via DisplayAdditionalTasksPage.
Function InitAdditionalTasksPage
  ; We've created the Additional tasks page. Update our control states
  ; before they are shown.
  ; We set XXX_STATE -> XxxCheckBox here and go the other direction below.
  ${NSD_SetState} $hCtl_AdditionalTasksPage_StartMenuCheckBox $START_MENU_STATE
  ${NSD_SetState} $hCtl_AdditionalTasksPage_DesktopIconCheckBox $DESKTOP_ICON_STATE
  ${NSD_SetState} $hCtl_AdditionalTasksPage_AssociateExtensionsCheckBox $FILE_ASSOCIATE_STATE

  StrCpy $QT_SELECTED 0
  ${If} ${SectionIsSelected} ${SecWiresharkQt}
    StrCpy $QT_SELECTED 1
  ${Endif}
  EnableWindow $hCtl_AdditionalTasksPage_CreateShortcutsLabel $QT_SELECTED
  EnableWindow $hCtl_AdditionalTasksPage_StartMenuCheckBox $QT_SELECTED
  EnableWindow $hCtl_AdditionalTasksPage_DesktopIconCheckBox $QT_SELECTED

  EnableWindow $hCtl_AdditionalTasksPage_ExtensionsLabel $QT_SELECTED
  EnableWindow $hCtl_AdditionalTasksPage_AssociateExtensionsCheckBox $QT_SELECTED
  EnableWindow $hCtl_AdditionalTasksPage_FileExtensionsLabel $QT_SELECTED
FunctionEnd

Function LeaveAdditionalTasksPage
  ; We're leaving the Additional tasks page. Get our control states
  ; before they're destroyed.
  ; We set XxxCheckBox -> XXX_STATE here and go the other direction above.
  ${NSD_GetState} $hCtl_AdditionalTasksPage_StartMenuCheckBox $START_MENU_STATE
  ${NSD_GetState} $hCtl_AdditionalTasksPage_DesktopIconCheckBox $DESKTOP_ICON_STATE
  ${NSD_GetState} $hCtl_AdditionalTasksPage_AssociateExtensionsCheckBox $FILE_ASSOCIATE_STATE
FunctionEnd

!endif ; QT_DIR

Var NPCAP_NAME ; DisplayName from Npcap installation
Var WINPCAP_NAME ; DisplayName from WinPcap installation
Var NPCAP_DISPLAY_VERSION ; DisplayVersion from Npcap installation
Var USBPCAP_NAME ; DisplayName from USBPcap installation

Function myShowCallback

  ClearErrors
  ; detect if Npcap should be installed
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 4" "Text" "Install Npcap ${NPCAP_PACKAGE_VERSION}"
  ReadRegStr $NPCAP_NAME HKEY_LOCAL_MACHINE "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NpcapInst" "DisplayName"
  IfErrors 0 lbl_npcap_installed
  ; check also if WinPcap is installed
  ReadRegStr $WINPCAP_NAME HKEY_LOCAL_MACHINE "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\WinPcapInst" "DisplayName"
  IfErrors 0 lbl_winpcap_installed ;if RegKey is available, WinPcap is already installed
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 2" "Text" "Neither of these are installed"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 2" "Flags" "DISABLED"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 5" "Text" "(Use Add/Remove Programs first to uninstall any undetected old Npcap or WinPcap versions)"
  Goto lbl_npcap_done

lbl_npcap_installed:
  ReadRegStr $NPCAP_DISPLAY_VERSION HKEY_LOCAL_MACHINE "SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\NpcapInst" "DisplayVersion"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 1" "Text" "Currently installed Npcap version"
  StrCmp $NPCAP_NAME "Npcap" 0 +3
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 2" "Text" "Npcap $NPCAP_DISPLAY_VERSION"
  Goto +2
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 2" "Text" "$NPCAP_NAME"

  ; Compare the installed build against the one we have.
  StrCmp $NPCAP_DISPLAY_VERSION "" lbl_npcap_do_install ; Npcap wasn't installed improperly?
  ${VersionConvert} $NPCAP_DISPLAY_VERSION "" $R0 ; 0.99-r7 -> 0.99.114.7
  ${VersionConvert} "${NPCAP_PACKAGE_VERSION}" "" $R1
  ${VersionCompare} $R0 $R1 $1
  StrCmp $1 "2" lbl_npcap_do_install

  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 4" "State" "0"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 4" "Flags" "DISABLED"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 5" "Text" "If you wish to install Npcap, please uninstall $NPCAP_NAME manually first."
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 5" "Flags" "DISABLED"
  Goto lbl_npcap_done

lbl_winpcap_installed:
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 2" "Text" "$WINPCAP_NAME"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 4" "State" "1"
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 5" "Text" "The currently installed $WINPCAP_NAME may be uninstalled first."
  Goto lbl_npcap_done

lbl_npcap_do_install:
  ; seems to be an old version, install newer one
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 4" "State" "1"
  StrCmp $NPCAP_NAME "Npcap" 0 +3
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 5" "Text" "The currently installed Npcap $NPCAP_DISPLAY_VERSION will be uninstalled first."
  Goto +2
  WriteINIStr "$PLUGINSDIR\NpcapPage.ini" "Field 5" "Text" "The currently installed $NPCAP_NAME will be uninstalled first."

lbl_npcap_done:

  ; detect if USBPcap should be installed
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 4" "Text" "Install USBPcap ${USBPCAP_PACKAGE_VERSION}"
  ${If} ${RunningX64}
      ${DisableX64FSRedirection}
      SetRegView 64
  ${EndIf}
  ReadRegStr $USBPCAP_NAME HKEY_LOCAL_MACHINE "Software\Microsoft\Windows\CurrentVersion\Uninstall\USBPcap" "DisplayName"
  ${If} ${RunningX64}
      ${EnableX64FSRedirection}
      SetRegView 32
  ${EndIf}
  IfErrors 0 lbl_usbpcap_installed ;if RegKey is available, USBPcap is already installed
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 2" "Text" "USBPcap is currently not installed"
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 2" "Flags" "DISABLED"
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 5" "Text" "(Use Add/Remove Programs first to uninstall any undetected old USBPcap versions)"
  Goto lbl_usbpcap_done

lbl_usbpcap_installed:
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 2" "Text" "$USBPCAP_NAME"
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 4" "State" "0"
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 4" "Flags" "DISABLED"
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 5" "Text" "If you wish to install USBPcap ${USBPCAP_PACKAGE_VERSION}, please uninstall $USBPCAP_NAME manually first."
  WriteINIStr "$PLUGINSDIR\USBPcapPage.ini" "Field 5" "Flags" "DISABLED"
  Goto lbl_usbpcap_done

lbl_usbpcap_done:

FunctionEnd
