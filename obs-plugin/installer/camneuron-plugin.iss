; 캠뉴런 PC 플러그인 설치 프로그램 (Inno Setup 6). 직접 부르지 말고 obs-plugin\scripts\package.ps1로 만든다.
; package.ps1이 넘기는 값: AppVersion(CMakeLists.txt), StageDir(넣을 파일), ObsTarget(빌드에 쓴 OBS 버전),
; AvcodecDll/AvutilDll(플러그인이 쓰는 OBS의 FFmpeg DLL 이름).
; 이 파일은 UTF-8(BOM 포함)이어야 Inno Setup이 한글을 바로 읽는다.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage"
#endif
#ifndef ObsTarget
  #define ObsTarget "33"
#endif
#ifndef AvcodecDll
  #define AvcodecDll "avcodec-62.dll"
#endif
#ifndef AvutilDll
  #define AvutilDll "avutil-60.dll"
#endif

[Setup]
; 버전이 바뀌어도 같은 값이어야 업데이트로 인식한다.
AppId={{4069CADA-88D7-468A-9AC8-30DA0458CC18}
AppName={cm:AppName}
AppVersion={#AppVersion}
AppVerName={cm:AppName} {#AppVersion}
AppPublisher=CamNeuron
VersionInfoVersion={#AppVersion}
; OBS Studio 28 이상이 모든 사용자의 플러그인을 읽는 곳. 폴더 이름은 예전 이름(obs-phonecam) 그대로 둔다.
DefaultDirName={commonappdata}\obs-studio\plugins\obs-phonecam
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
PrivilegesRequired=admin
; 시험할 때 /CURRENTUSER로 관리자 권한 없이 설치할 수 있게 한다.
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; 켜져 있는 OBS가 플러그인 DLL을 쥐고 있으면 닫을지 묻는다.
CloseApplications=yes
RestartApplications=no
OutputBaseFilename=CamNeuron-PC-Plugin-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={cm:AppName}
; 제거 프로그램은 플러그인 파일과 섞이지 않게 하위 폴더에 둔다.
UninstallFilesDir={app}\uninstall

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
korean.FinishedLabelNoIcons=캠뉴런 PC 플러그인을 설치했습니다.%n%nOBS Studio를 켜고 소스 추가(+) → '캠뉴런 (Android 폰 카메라)'를 고르세요. 폰에서는 캠뉴런 앱을 열어 두면 같은 Wi-Fi에서 자동으로 찾습니다.
english.FinishedLabelNoIcons=The CamNeuron PC plugin is installed.%n%nStart OBS Studio and add a source (+) > 'CamNeuron (Android phone camera)'. Open the CamNeuron app on the phone and OBS finds it on the same Wi-Fi.

[CustomMessages]
korean.AppName=캠뉴런 PC 플러그인
english.AppName=CamNeuron PC Plugin
korean.ObsNotFound=OBS Studio가 설치돼 있지 않은 것 같습니다.%n%n먼저 obsproject.com에서 OBS Studio를 설치하세요. 그래도 플러그인을 설치할까요?
english.ObsNotFound=OBS Studio does not seem to be installed.%n%nInstall OBS Studio from obsproject.com first. Install the plugin anyway?
korean.ObsVersion=이 플러그인은 OBS Studio {#ObsTarget}에 맞춰 만들었습니다. 설치된 OBS(%1)에는 필요한 파일({#AvcodecDll})이 없어 플러그인을 불러오지 못합니다.%n%n캠뉴런 다운로드 페이지에서 이 OBS 버전에 맞는 플러그인을 받거나 OBS를 업데이트하세요. 그래도 설치할까요?
english.ObsVersion=This plugin is built for OBS Studio {#ObsTarget}. The installed OBS (%1) lacks {#AvcodecDll}, so it cannot load the plugin.%n%nGet the matching plugin from the CamNeuron download page or update OBS. Install anyway?

[Files]
Source: "{#StageDir}\obs-phonecam\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
function ObsDir(): String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) then
    RegQueryStringValue(HKLM32, 'SOFTWARE\OBS Studio', '', Result);
end;

function InitializeSetup(): Boolean;
var
  Bin, Version: String;
begin
  Result := True;
  Bin := AddBackslash(ObsDir()) + 'bin\64bit\';
  if (ObsDir() = '') or not FileExists(Bin + 'obs64.exe') then
  begin
    Result := SuppressibleMsgBox(CustomMessage('ObsNotFound'), mbConfirmation, MB_YESNO, IDYES) = IDYES;
    exit;
  end;
  { 플러그인은 OBS에 들어 있는 FFmpeg DLL을 그대로 쓴다. DLL 이름에 버전이 붙어 있어 OBS가 FFmpeg를 올리면 이름이 바뀐다. }
  if not FileExists(Bin + '{#AvcodecDll}') or not FileExists(Bin + '{#AvutilDll}') then
  begin
    if not GetVersionNumbersString(Bin + 'obs64.exe', Version) then
      Version := '?';
    Result := SuppressibleMsgBox(FmtMessage(CustomMessage('ObsVersion'), [Version]), mbConfirmation, MB_YESNO, IDNO) = IDYES;
  end;
end;
