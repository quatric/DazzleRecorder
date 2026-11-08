; Dazzle Capture for Windows: the app, its bundled ffmpeg, and the Dazzle driver.
;   makensis -DARCH=x86_64|x86|aarch64 -DVERSION=0.4.0 -DSRC=dist\x86_64-windows-gnu \
;            -DOUTFILE=DazzleRecorder-x86_64-windows-setup.exe installer/windows.nsi
; The driver is installed by dazzle-driver.exe (src/driver_win32.c): WinUSB on
; Windows 8.1 and later, libusbK on Windows 7 and 8.0, on the video interface only.

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!ifndef ARCH | VERSION | SRC | OUTFILE
  !error "define ARCH, VERSION, SRC and OUTFILE"
!endif

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\DazzleRecorder"

Name "Dazzle Capture"
Caption "Dazzle Capture ${VERSION} Setup"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
!if "${ARCH}" == "x86"
  InstallDir "$PROGRAMFILES\Dazzle Capture"
!else
  InstallDir "$PROGRAMFILES64\Dazzle Capture"
!endif
InstallDirRegKey HKLM "Software\DazzleRecorder" "InstallDir"
BrandingText "DazzleRecorder ${VERSION}"

!define MUI_COMPONENTSPAGE_SMALLDESC
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\Dazzle Capture.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Dazzle Capture"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
!if "${ARCH}" == "x86_64"
  ${IfNot} ${IsNativeAMD64}
    MessageBox MB_ICONSTOP "This installer is for 64-bit Intel/AMD Windows.$\nUse the x86 installer on 32-bit Windows, or the aarch64 one on Windows on Arm."
    Abort
  ${EndIf}
  SetRegView 64
!else if "${ARCH}" == "aarch64"
  ${IfNot} ${IsNativeARM64}
    MessageBox MB_ICONSTOP "This installer is for Windows on Arm. Use the x86_64 installer on Intel/AMD PCs."
    Abort
  ${EndIf}
  SetRegView 64
!else
  ${If} ${RunningX64}
    MessageBox MB_ICONSTOP "This is the 32-bit installer, which cannot install a driver on 64-bit Windows.$\nUse the x86_64 installer (or aarch64 on Windows on Arm)."
    Abort
  ${EndIf}
!endif
FunctionEnd

Function un.onInit
!if "${ARCH}" != "x86"
  SetRegView 64
!endif
FunctionEnd

Section "Dazzle Capture" SecApp
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRC}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Dazzle Capture.lnk" "$INSTDIR\Dazzle Capture.exe"
  WriteRegStr HKLM "Software\DazzleRecorder" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "Dazzle Capture"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "DazzleRecorder"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\Dazzle Capture.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Dazzle driver" SecDriver
  DetailPrint "Installing the Dazzle driver..."
  nsExec::ExecToLog '"$INSTDIR\dazzle-driver.exe" install --quiet'
  Pop $0
  ${If} $0 == 0
    DetailPrint "The Dazzle driver is installed."
  ${ElseIf} $0 == 2
    DetailPrint "The driver is ready; it will be used when the Dazzle is plugged in."
  ${Else}
    MessageBox MB_ICONEXCLAMATION "The Dazzle driver could not be installed (code $0).$\nDazzle Capture will offer to install it again when it starts."
  ${EndIf}
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\Dazzle Capture.lnk" "$INSTDIR\Dazzle Capture.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "Dazzle Capture, the dvc100-capture command line tool and ffmpeg for recording."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver} "Lets Dazzle Capture use the Dazzle: WinUSB (Windows 8.1 and later) or libusbK (Windows 7) for its video interface, as Zadig would. Its sound keeps using Windows' own driver."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut to Dazzle Capture on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  MessageBox MB_YESNO|MB_ICONQUESTION "Also remove the Dazzle driver?$\n(Windows then goes back to the driver it had before.)" IDNO keep_driver
    nsExec::ExecToLog '"$INSTDIR\dazzle-driver.exe" uninstall --quiet'
    Pop $0
  keep_driver:
  Delete "$INSTDIR\Dazzle Capture.exe"
  Delete "$INSTDIR\dvc100-capture.exe"
  Delete "$INSTDIR\dazzle-driver.exe"
  Delete "$INSTDIR\ffmpeg.exe"
  Delete "$INSTDIR\libusbK.dll"
  Delete "$INSTDIR\driver\libusbK\libusbK.sys"
  Delete "$INSTDIR\driver\libusbK\WdfCoInstaller01009.dll"
  RMDir "$INSTDIR\driver\libusbK"
  RMDir "$INSTDIR\driver"
  Delete "$INSTDIR\licenses\*.txt"
  Delete "$INSTDIR\licenses\*.LGPLv2.1.txt"
  RMDir "$INSTDIR\licenses"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Dazzle Capture.lnk"
  Delete "$DESKTOP\Dazzle Capture.lnk"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegValue HKLM "Software\DazzleRecorder" "InstallDir" ; the driver record stays if the driver was kept
SectionEnd
