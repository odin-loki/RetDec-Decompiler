; retdec.nsi — Full NSIS installer for RetDec Enhanced Decompiler Suite
;
; Build this installer AFTER running scripts/build-windows-installer.ps1 (or
; scripts/bundle-windows.sh on Linux/WSL) to populate BUNDLE_DIR:
;   bin/*.exe, bin/*.dll, platforms/, imageformats/, share/retdec/
;
; Usage:
;   makensis /DVERSION=5.0 /DBUNDLE_DIR=..\..\dist\windows-bundle retdec.nsi
;
; Requires:
;   NSIS 3.x (https://nsis.sourceforge.io/)
;   Modern UI 2 plugin (included with NSIS)
;   EnVar plug-in for PATH updates (NOT bundled with NSIS):
;     https://nsis.sourceforge.io/EnVar_plug-in
;     Install EnVar.dll into ${NSISDIR}\Plugins\x86-unicode\
;     Install EnVar.nsh  into ${NSISDIR}\Include\
;   Without EnVar, comment out EnVar:: lines and PATH will not be updated.

; ─── Version / identity ───────────────────────────────────────────────────────
!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef BUNDLE_DIR
  !define BUNDLE_DIR "bundle"
!endif

!define PRODUCT_NAME        "RetDec"
!define PRODUCT_FULL_NAME   "RetDec Enhanced Decompiler Suite"
!define PRODUCT_VERSION     "${VERSION}"
!define PRODUCT_PUBLISHER   "RetDec Contributors"
!define PRODUCT_URL         "https://github.com/odin-loki/RetDec-Decompiler"
!define PRODUCT_REG_KEY     "Software\RetDec"
!define PRODUCT_UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\RetDec"

; ─── Compiler settings ────────────────────────────────────────────────────────
SetCompressor      /SOLID lzma
SetCompressorDictSize 64
Unicode            True
RequestExecutionLevel admin

; ─── Modern UI 2 ──────────────────────────────────────────────────────────────
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define MUI_ABORTWARNING
!define MUI_ICON         "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON       "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${NSISDIR}\Contrib\Graphics\Wizard\win.bmp"

; Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\retdec-gui.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch RetDec GUI"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ─── Installer metadata ───────────────────────────────────────────────────────
Name              "${PRODUCT_FULL_NAME} ${PRODUCT_VERSION}"
OutFile           "retdec-${VERSION}-windows-x64-setup.exe"
InstallDir        "$PROGRAMFILES64\RetDec"
InstallDirRegKey  HKLM "${PRODUCT_REG_KEY}" "InstallDir"
BrandingText      "${PRODUCT_FULL_NAME} ${PRODUCT_VERSION}"

; ─── Version info block (PE metadata) ─────────────────────────────────────────
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "${PRODUCT_FULL_NAME}"
VIAddVersionKey "ProductVersion"  "${PRODUCT_VERSION}"
VIAddVersionKey "CompanyName"     "${PRODUCT_PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "Copyright (C) RetDec Contributors"
VIAddVersionKey "FileDescription" "RetDec Windows Installer"
VIAddVersionKey "FileVersion"     "${VERSION}"

; ─── Sections ─────────────────────────────────────────────────────────────────

Section "RetDec Core & CLI tools" SEC_CORE
  SectionIn RO   ; required
  SetOutPath "$INSTDIR\bin"

  ; Core binaries from bundle (MSVC native + MinGW cross names)
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-decompiler.exe"
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-fileinfo.exe"
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-ar-extractor.exe"
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-ar-extractortool.exe"
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-unpacker.exe"
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-utils.exe"

  ; MinGW runtime DLLs (cross-compile bundle only)
  File /nonfatal "${BUNDLE_DIR}\bin\libstdc++-6.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\libgcc_s_seh-1.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\libwinpthread-1.dll"

  ; MSVC runtime (native bundle)
  File /nonfatal "${BUNDLE_DIR}\bin\msvcp140.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\vcruntime140.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\vcruntime140_1.dll"

  ; Support data
  SetOutPath "$INSTDIR\share\retdec"
  File /r /nonfatal "${BUNDLE_DIR}\share\retdec\*.*"

  ; Registry: install dir + uninstall entry
  WriteRegStr   HKLM "${PRODUCT_REG_KEY}" "InstallDir"  "$INSTDIR"
  WriteRegStr   HKLM "${PRODUCT_REG_KEY}" "Version"     "${PRODUCT_VERSION}"
  WriteRegStr   HKLM "${PRODUCT_UNINST_KEY}" "DisplayName"          "${PRODUCT_FULL_NAME}"
  WriteRegStr   HKLM "${PRODUCT_UNINST_KEY}" "UninstallString"      "$INSTDIR\uninstall.exe"
  WriteRegStr   HKLM "${PRODUCT_UNINST_KEY}" "DisplayIcon"          "$INSTDIR\bin\retdec-gui.exe"
  WriteRegStr   HKLM "${PRODUCT_UNINST_KEY}" "Publisher"            "${PRODUCT_PUBLISHER}"
  WriteRegStr   HKLM "${PRODUCT_UNINST_KEY}" "URLInfoAbout"         "${PRODUCT_URL}"
  WriteRegStr   HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion"       "${PRODUCT_VERSION}"
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoModify"             1
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "NoRepair"             1

  ; Estimated install size
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${PRODUCT_UNINST_KEY}" "EstimatedSize" $0

  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; PATH: add $INSTDIR\bin to system PATH (requires EnVar plug-in — see header)
  EnVar::AddValue "PATH" "$INSTDIR\bin"
SectionEnd

Section "RetDec GUI (Qt6)" SEC_GUI
  SetOutPath "$INSTDIR\bin"
  File /nonfatal "${BUNDLE_DIR}\bin\retdec-gui.exe"

  ; Qt6 DLLs
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6Core.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6Gui.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6Widgets.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6Network.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6OpenGL.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6Charts.dll"
  File /nonfatal "${BUNDLE_DIR}\bin\Qt6Svg.dll"

  ; Qt platform plugins
  SetOutPath "$INSTDIR\bin\platforms"
  File /nonfatal "${BUNDLE_DIR}\platforms\qwindows.dll"
  File /nonfatal "${BUNDLE_DIR}\platforms\qoffscreen.dll"

  ; Qt image format plugins
  SetOutPath "$INSTDIR\bin\imageformats"
  File /r /nonfatal "${BUNDLE_DIR}\imageformats\*.dll"

  ; Start Menu shortcut
  CreateDirectory "$SMPROGRAMS\RetDec"
  CreateShortcut  "$SMPROGRAMS\RetDec\RetDec GUI.lnk"      "$INSTDIR\bin\retdec-gui.exe"
  CreateShortcut  "$SMPROGRAMS\RetDec\Uninstall RetDec.lnk" "$INSTDIR\uninstall.exe"

  ; Desktop shortcut
  CreateShortcut "$DESKTOP\RetDec GUI.lnk" "$INSTDIR\bin\retdec-gui.exe"
SectionEnd

Section "OpenCL Runtime" SEC_OCL
  SetOutPath "$INSTDIR\bin"
  File /nonfatal "${BUNDLE_DIR}\bin\OpenCL.dll"
SectionEnd

; Optional: register "Open with RetDec GUI" for .exe files.
; Disabled by default — uncomment the Section / SectionEnd block to enable.
; Requires admin; associates ProgID RetDec.exe with retdec-gui.exe.
;
; Section "Associate .exe with RetDec GUI" SEC_ASSOC
;   WriteRegStr HKCR "RetDec.exe" "" "RetDec Binary"
;   WriteRegStr HKCR "RetDec.exe\DefaultIcon" "" "$INSTDIR\bin\retdec-gui.exe,0"
;   WriteRegStr HKCR "RetDec.exe\shell\open\command" "" '"$INSTDIR\bin\retdec-gui.exe" "%1"'
;   WriteRegStr HKCR ".exe\OpenWithProgids" "RetDec.exe" ""
; SectionEnd

; ─── Section descriptions ──────────────────────────────────────────────────────
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_CORE} "RetDec decompiler, fileinfo, and command-line tools. Required."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_GUI}  "Qt6-based graphical user interface with tri-pane code view, CFG visualiser, and AI assistant panel."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_OCL}  "OpenCL ICD runtime for GPU-accelerated analysis. Only needed if no GPU driver is installed."
  ; !insertmacro MUI_DESCRIPTION_TEXT ${SEC_ASSOC} "Adds RetDec GUI to the Open with menu for .exe files."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ─── Uninstaller ──────────────────────────────────────────────────────────────
Section "Uninstall"
  ; Remove shortcuts
  Delete "$SMPROGRAMS\RetDec\RetDec GUI.lnk"
  Delete "$SMPROGRAMS\RetDec\Uninstall RetDec.lnk"
  RMDir  "$SMPROGRAMS\RetDec"
  Delete "$DESKTOP\RetDec GUI.lnk"

  ; Remove file association (if SEC_ASSOC was enabled)
  ; DeleteRegValue HKCR ".exe\OpenWithProgids" "RetDec.exe"
  ; DeleteRegKey HKCR "RetDec.exe"

  ; Remove files
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\share"
  Delete    "$INSTDIR\uninstall.exe"
  RMDir     "$INSTDIR"

  ; Remove PATH entry (requires EnVar plug-in — see header)
  EnVar::DeleteValue "PATH" "$INSTDIR\bin"

  ; Remove registry keys
  DeleteRegKey HKLM "${PRODUCT_UNINST_KEY}"
  DeleteRegKey HKLM "${PRODUCT_REG_KEY}"
SectionEnd
