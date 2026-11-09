; CTD OBS Windows installer (NSIS 3, Modern UI 2)
;
; Built by .github/scripts/Package-Windows.ps1 from the files that go into the zip package.
; Upstream OBS Studio no longer ships its NSIS script in the source tree, so this one is written
; for CTD OBS in the same style: Welcome -> License (GPL) -> Location -> Install -> Finish.
;
; Defines passed by the packaging script (makensis /D...):
;   STAGING_DIR      Folder with the unpacked package (bin\, core\, data\, ...)
;   OUTPUT_FILE      Path of the installer to write
;   VERSION          Display version, e.g. 33.0.1-abcdef1
;   VERSION_NUMERIC  Four-part file version, e.g. 33.0.1.42
;   VCAM_DIR         Folder of the virtual camera DLLs relative to the install folder
;   VCREDIST_FILE    Path of vc_redist.x64.exe to bundle
;   VCREDIST_MINOR   Minor version of that runtime (e.g. 50 for 14.50)
;   ITEMS_FILE       Generated include that removes the installed top-level files and folders
;   ICON_FILE        CTD OBS icon
;   LICENSE_FILE     GPL v2 text
;   SIGN_CMD         Optional. SIGNING HOOK: command that signs "%1" (installer and uninstaller)
;
; Side by side with stock OBS: this script only uses its own install folder, its own registry
; keys (HKLM\Software\CTD OBS and the "CTD OBS" uninstall entry) and the CTD OBS virtual camera
; DLLs, which have their own CLSID. It never reads or writes OBS Studio's keys, files or camera.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "nsDialogs.nsh"

!define APP_NAME "CTD OBS"
!define APP_PUBLISHER "CineTech Digital"
!define APP_EXE "ctd-obs64.exe"
!define APP_EXE_DIR "bin\64bit"
!define APP_REG_KEY "Software\CTD OBS"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CTD OBS"
!define UNINSTALLER "uninstall.exe"
!define VCAM_DLL32 "ctd-obs-virtualcam-module32.dll"
!define VCAM_DLL64 "ctd-obs-virtualcam-module64.dll"
!define SETTINGS_DIR "ctd-obs"
!define VCAM_RES_FILE "ctd-obs-virtualcam.txt"

!ifndef STAGING_DIR | OUTPUT_FILE | VERSION | VERSION_NUMERIC | VCAM_DIR | VCREDIST_FILE | VCREDIST_MINOR | ITEMS_FILE | ICON_FILE | LICENSE_FILE
  !error "Missing define; build this through .github/scripts/Package-Windows.ps1"
!endif

; ---------------------------------------------------------------------------------------------
; SIGNING HOOK
; Code signing is not set up yet. When it is, pass /DSIGN_CMD=<command> (for example a signtool
; call that takes the file as %1) and both the installer and the embedded uninstaller are signed
; as they are written. The binaries inside should be signed before this script runs; see the
; SIGNING HOOK in Package-Windows.ps1.
!ifdef SIGN_CMD
  !finalize '${SIGN_CMD}'
  !uninstfinalize '${SIGN_CMD}'
!endif
; ---------------------------------------------------------------------------------------------

Name "${APP_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${APP_REG_KEY}" ""
BrandingText "${APP_NAME} ${VERSION}"

VIProductVersion "${VERSION_NUMERIC}"
VIFileVersion "${VERSION_NUMERIC}"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL v2 or later; based on OBS Studio"

!define MUI_ICON "${ICON_FILE}"
!define MUI_UNICON "${ICON_FILE}"
!define MUI_ABORTWARNING

; Installer pages
!define MUI_WELCOMEPAGE_TEXT "This will install ${APP_NAME} ${VERSION} on your computer.$\r$\n$\r$\n${APP_NAME} installs beside OBS Studio and does not change it, its settings or its virtual camera.$\r$\n$\r$\nClose ${APP_NAME} before you continue.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!define MUI_LICENSEPAGE_TEXT_BOTTOM "${APP_NAME} is free software under the GNU General Public License. You do not have to accept it to use the program; it sets out your rights to copy and change it. Click Next to continue."
!define MUI_LICENSEPAGE_BUTTON "&Next >"
!insertmacro MUI_PAGE_LICENSE "${LICENSE_FILE}"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckInstallDir
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!define MUI_FINISHPAGE_SHOWREADME
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Create a desktop shortcut"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION CreateDesktopShortcut
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_CONFIRM
UninstPage custom un.OptionsPage un.OptionsPageLeave
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Var RemoveSettings
Var RemoveSettingsCheckbox

; ---------------------------------------------------------------------------------------------
; Shared helpers

!macro WaitForAppClosed UN
Function ${UN}WaitForAppClosed
  check:
    nsExec::ExecToStack 'cmd.exe /c tasklist /FI "IMAGENAME eq ${APP_EXE}" /NH | find /I "${APP_EXE}"'
    Pop $0
    Pop $1
    ${If} $0 == 0
      ; Silent runs stop here (default answer Cancel) instead of closing the user's work.
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${APP_NAME} is running. Close it, then click Retry." /SD IDCANCEL IDRETRY check
      Abort
    ${EndIf}
FunctionEnd
!macroend
!insertmacro WaitForAppClosed ""
!insertmacro WaitForAppClosed "un."

; Same steps as virtualcam-install.bat / virtualcam-uninstall.bat, without the console window.
!macro VirtualCam UN
!if "${UN}" == ""
Function RegisterVirtualCam
  ${If} ${FileExists} "$INSTDIR\${VCAM_DIR}\${VCAM_DLL32}"
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /i /s "$INSTDIR\${VCAM_DIR}\${VCAM_DLL32}"' $0
    DetailPrint "32-bit CTD OBS Virtual Camera: regsvr32 returned $0"
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\${VCAM_DIR}\${VCAM_DLL64}"
    ${DisableX64FSRedirection}
    ExecWait '"$WINDIR\System32\regsvr32.exe" /i /s "$INSTDIR\${VCAM_DIR}\${VCAM_DLL64}"' $0
    ${EnableX64FSRedirection}
    DetailPrint "64-bit CTD OBS Virtual Camera: regsvr32 returned $0"
    ${If} $0 != 0
      MessageBox MB_OK|MB_ICONEXCLAMATION "The CTD OBS Virtual Camera could not be registered (code $0). Everything else is installed; you can retry later with ${VCAM_DIR}\virtualcam-install.bat as administrator." /SD IDOK
    ${EndIf}
  ${EndIf}
FunctionEnd
!endif

Function ${UN}UnregisterVirtualCam
  ${If} ${FileExists} "$INSTDIR\${VCAM_DIR}\${VCAM_DLL32}"
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /u /s "$INSTDIR\${VCAM_DIR}\${VCAM_DLL32}"' $0
    DetailPrint "Unregister 32-bit CTD OBS Virtual Camera: $0"
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\${VCAM_DIR}\${VCAM_DLL64}"
    ${DisableX64FSRedirection}
    ExecWait '"$WINDIR\System32\regsvr32.exe" /u /s "$INSTDIR\${VCAM_DIR}\${VCAM_DLL64}"' $0
    ${EnableX64FSRedirection}
    DetailPrint "Unregister 64-bit CTD OBS Virtual Camera: $0"
  ${EndIf}
FunctionEnd
!macroend
!insertmacro VirtualCam ""
!insertmacro VirtualCam "un."

; Removes exactly the top-level files and folders this installer put down (list generated from
; the package at build time), then the folder itself only if it is empty.
!macro RemoveInstalledFiles UN
Function ${UN}RemoveInstalledFiles
  !include "${ITEMS_FILE}"
  Delete "$INSTDIR\${UNINSTALLER}"
  RMDir "$INSTDIR"
FunctionEnd
!macroend
!insertmacro RemoveInstalledFiles ""
!insertmacro RemoveInstalledFiles "un."

; ---------------------------------------------------------------------------------------------
; Installer

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs 64-bit Windows 10 or later." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
  ; Keep an existing install's folder (InstallDirRegKey reads the 32-bit view, so read again).
  ReadRegStr $0 HKLM "${APP_REG_KEY}" ""
  ${If} $0 != ""
    StrCpy $INSTDIR $0
  ${EndIf}
FunctionEnd

Function CheckInstallDir
  ; Never install over stock OBS Studio.
  ${If} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
    MessageBox MB_OK|MB_ICONEXCLAMATION "This folder contains OBS Studio. Choose a different folder for ${APP_NAME} so both keep working." /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Function InstallVCRedist
  ; Same idea as OBS's installer: only run the Microsoft runtime setup when the installed x64
  ; Visual C++ 2015-2022+ runtime is missing or older than the one CTD OBS was built with.
  ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64" "Installed"
  ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64" "Minor"
  ${If} $0 == 1
  ${AndIf} $1 >= ${VCREDIST_MINOR}
    DetailPrint "Visual C++ runtime 14.$1 already installed"
    Return
  ${EndIf}
  DetailPrint "Installing the Microsoft Visual C++ runtime..."
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "/oname=vc_redist.x64.exe" "${VCREDIST_FILE}"
  ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
  Delete "$PLUGINSDIR\vc_redist.x64.exe"
  ; 0 = ok, 1638 = a newer version is installed, 3010 = ok but needs a restart
  ${If} $0 == 3010
    SetRebootFlag true
  ${ElseIf} $0 != 0
  ${AndIf} $0 != 1638
    MessageBox MB_OK|MB_ICONEXCLAMATION "The Microsoft Visual C++ runtime could not be installed (code $0). ${APP_NAME} may not start until it is installed." /SD IDOK
  ${EndIf}
FunctionEnd

Section "CTD OBS" SecMain
  SectionIn RO
  SetRegView 64
  SetShellVarContext all

  Call WaitForAppClosed
  Call InstallVCRedist

  ; Upgrade: unregister the old camera and remove the old version's files first, so files
  ; dropped from the new version do not linger. Only done when this folder is our install.
  ReadRegStr $0 HKLM "${APP_REG_KEY}" ""
  ${If} $0 == $INSTDIR
  ${AndIf} ${FileExists} "$INSTDIR\${APP_EXE_DIR}\${APP_EXE}"
    DetailPrint "Removing the previous version..."
    Call UnregisterVirtualCam
    Call RemoveInstalledFiles
  ${EndIf}

  SetOutPath "$INSTDIR"
  File /r "${STAGING_DIR}\*.*"

  Call RegisterVirtualCam

  WriteUninstaller "$INSTDIR\${UNINSTALLER}"

  ; Start menu (all users). Working folder is the exe folder, as OBS expects.
  SetOutPath "$INSTDIR\${APP_EXE_DIR}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE_DIR}\${APP_EXE}" "" "$INSTDIR\${APP_EXE_DIR}\${APP_EXE}" 0

  WriteRegStr HKLM "${APP_REG_KEY}" "" "$INSTDIR"
  WriteRegStr HKLM "${APP_REG_KEY}" "Version" "${VERSION}"

  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE_DIR}\${APP_EXE},0"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\${UNINSTALLER}"'
  WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\${UNINSTALLER}" /S'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" "$0"
SectionEnd

Function LaunchApp
  ; The installer runs as administrator; start the app through Explorer so it runs as the
  ; signed-in user, via the Start menu shortcut so its working folder is right.
  Exec '"$WINDIR\explorer.exe" "$SMPROGRAMS\${APP_NAME}.lnk"'
FunctionEnd

Function CreateDesktopShortcut
  SetShellVarContext all
  SetOutPath "$INSTDIR\${APP_EXE_DIR}"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE_DIR}\${APP_EXE}" "" "$INSTDIR\${APP_EXE_DIR}\${APP_EXE}" 0
FunctionEnd

; ---------------------------------------------------------------------------------------------
; Uninstaller
;
; Silent uninstall keeps settings; add /REMOVESETTINGS to delete them too.

Function un.onInit
  SetRegView 64
  StrCpy $RemoveSettings 0
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/REMOVESETTINGS" $1
  ${IfNot} ${Errors}
    StrCpy $RemoveSettings 1
  ${EndIf}
FunctionEnd

Function un.OptionsPage
  !insertmacro MUI_HEADER_TEXT "Settings" "Choose whether to keep your ${APP_NAME} settings."
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  SetShellVarContext current
  ${NSD_CreateLabel} 0 0 100% 36u "Your scenes, profiles and settings are in $APPDATA\${SETTINGS_DIR}. They are kept unless you tick the box, so a later install of ${APP_NAME} picks them up again."
  Pop $0
  ${NSD_CreateCheckbox} 0 44u 100% 12u "&Remove settings"
  Pop $RemoveSettingsCheckbox
  ${If} $RemoveSettings == 1
    ${NSD_Check} $RemoveSettingsCheckbox
  ${EndIf}
  nsDialogs::Show
FunctionEnd

Function un.OptionsPageLeave
  ${NSD_GetState} $RemoveSettingsCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $RemoveSettings 1
  ${Else}
    StrCpy $RemoveSettings 0
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetRegView 64

  Call un.WaitForAppClosed
  Call un.UnregisterVirtualCam

  SetShellVarContext all
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  ; Leave the install folder so its files can be removed.
  SetOutPath "$TEMP"
  Call un.RemoveInstalledFiles

  DeleteRegKey HKLM "${UNINSTALL_KEY}"
  DeleteRegKey HKLM "${APP_REG_KEY}"

  ${If} $RemoveSettings == 1
    ; Settings belong to the user who runs the uninstaller.
    SetShellVarContext current
    DetailPrint "Removing settings in $APPDATA\${SETTINGS_DIR}"
    RMDir /r "$APPDATA\${SETTINGS_DIR}"
    Delete "$APPDATA\${VCAM_RES_FILE}"
  ${Else}
    SetShellVarContext current
    DetailPrint "Keeping settings in $APPDATA\${SETTINGS_DIR}"
  ${EndIf}
SectionEnd
