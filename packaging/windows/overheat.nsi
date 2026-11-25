; Overheat installer (NSIS 3, Modern UI 2). Built by packaging/windows/build.sh:
;   makensis -DVERSION=x.y.z -DSTAGE=<staged dir> -DOUTFILE=<setup.exe> overheat.nsi
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!define APPNAME "Overheat"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Overheat"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
BrandingText "${APPNAME} ${VERSION}"

!define MUI_ICON "${STAGE}\overheat.ico"
!define MUI_UNICON "${STAGE}\overheat.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\overheat.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APPNAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" ""

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${APPNAME} requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all   ; machine-wide install: all-users Start menu / desktop
FunctionEnd

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\overheat.exe,0"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${APPNAME}"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Start Menu shortcut" SecStartMenu
  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\overheat.exe" "" "$INSTDIR\overheat.exe" 0
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\overheat.exe" "" "$INSTDIR\overheat.exe" 0
SectionEnd

Section "Open with Overheat (Explorer context menu)" SecContext
  WriteRegStr HKCR "*\shell\${APPNAME}" "" "Open with ${APPNAME}"
  WriteRegStr HKCR "*\shell\${APPNAME}" "Icon" "$INSTDIR\overheat.exe,0"
  WriteRegStr HKCR "*\shell\${APPNAME}\command" "" '"$INSTDIR\overheat.exe" "%1"'
  WriteRegStr HKCR "Directory\shell\${APPNAME}" "" "Open with ${APPNAME}"
  WriteRegStr HKCR "Directory\shell\${APPNAME}" "Icon" "$INSTDIR\overheat.exe,0"
  WriteRegStr HKCR "Directory\shell\${APPNAME}\command" "" '"$INSTDIR\overheat.exe" "%1"'
  WriteRegStr HKCR "Directory\Background\shell\${APPNAME}" "" "Open with ${APPNAME}"
  WriteRegStr HKCR "Directory\Background\shell\${APPNAME}" "Icon" "$INSTDIR\overheat.exe,0"
  WriteRegStr HKCR "Directory\Background\shell\${APPNAME}\command" "" '"$INSTDIR\overheat.exe" "%V"'
SectionEnd

Section "Add to PATH" SecPath
  ; Appends to the machine PATH (read-modify-write through the registry).
  ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  StrCmp $0 "" 0 +2
    StrCpy $0 "$INSTDIR"
  StrCpy $0 "$0;$INSTDIR"
  WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$0"
  WriteRegStr HKLM "Software\${APPNAME}" "AddedToPath" "1"
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=500
SectionEnd

LangString DESC_Main ${LANG_ENGLISH} "The editor and its libraries."
LangString DESC_Start ${LANG_ENGLISH} "Add Overheat to the Start menu."
LangString DESC_Desk ${LANG_ENGLISH} "Put a shortcut on the desktop."
LangString DESC_Ctx ${LANG_ENGLISH} "Add 'Open with Overheat' to files and folders in Explorer."
LangString DESC_Path ${LANG_ENGLISH} "Run 'overheat' from any terminal."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_Main)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} $(DESC_Start)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} $(DESC_Desk)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecContext} $(DESC_Ctx)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} $(DESC_Path)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.RemoveFromPath
  ; Remove ";$INSTDIR" (or a leading "$INSTDIR;") from the machine PATH.
  ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  StrLen $2 "$INSTDIR"
  StrCpy $3 ""          ; result
  StrCpy $4 0           ; index
  loop:
    StrCpy $5 $0 1 $4
    StrCmp $5 "" done
    StrCpy $6 $0 $2 $4
    StrCmp $6 "$INSTDIR" 0 keep
      IntOp $7 $4 + $2
      StrCpy $8 $0 1 $7
      StrCmp $8 ";" 0 +2
        IntOp $7 $7 + 1
      StrCmp $8 "" 0 +4
        StrLen $9 $3
        IntOp $9 $9 - 1
        StrCpy $3 $3 $9  ; drop the separator before a trailing entry
      StrCpy $4 $7
      Goto loop
    keep:
    StrCpy $3 "$3$5"
    IntOp $4 $4 + 1
    Goto loop
  done:
  WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$3"
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=500
FunctionEnd

Section "Uninstall"
  ReadRegStr $0 HKLM "Software\${APPNAME}" "AddedToPath"
  StrCmp $0 "1" 0 +2
    Call un.RemoveFromPath
  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKCR "*\shell\${APPNAME}"
  DeleteRegKey HKCR "Directory\shell\${APPNAME}"
  DeleteRegKey HKCR "Directory\Background\shell\${APPNAME}"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
  ; Only remove what we installed; user settings live in %APPDATA%\Overheat.
  RMDir /r "$INSTDIR\fonts"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\overheat.exe"
  Delete "$INSTDIR\overheat.ico"
  Delete "$INSTDIR\*.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
