; Minimal NSIS installer for the cross-built retdec-gui stub (PE).
; Before compiling: copy retdec-gui.exe into this directory next to the .nsi file.
; Example: makensis retdec-gui-stub.nsi

!define PRODUCT_NAME "RetDec GUI (stub)"
!define PRODUCT_VERSION "0.0.0"

Name "${PRODUCT_NAME}"
OutFile "retdec-gui-stub-setup.exe"
InstallDir "$PROGRAMFILES64\RetDecGuiStub"
RequestExecutionLevel admin

Page directory
Page instfiles

Section "Main"
  SetOutPath $INSTDIR
  File "retdec-gui.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\retdec-gui.exe"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
