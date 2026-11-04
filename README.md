# 캠뉴런 PC 플러그인 (CamNeuron PC Plugin)

안드로이드 폰 카메라를 OBS Studio의 소스로 쓰게 해 주는 **캠뉴런**의 PC 쪽 플러그인입니다.
폰에는 캠뉴런 앱을 설치합니다 (Google Play 출시 전에는 아래 체험판 APK).

## 바로 받기

| | 파일 | |
|---|---|---|
| **폰 앱 체험판** (Android 8.0 이상, 2027-01-31까지 프로) | [CamNeuron-0.2.3-beta.apk](https://github.com/kkomang2re/camneuron/releases/download/app-v0.2.3-beta/CamNeuron-0.2.3-beta.apk) | [설치 방법](https://kkomang2re.github.io/camneuron/beta.html) |
| **PC 플러그인** (Windows, OBS Studio 33) | [CamNeuron-PC-Plugin-0.2.3-Setup.exe](https://github.com/kkomang2re/camneuron/releases/download/plugin-v0.2.3/CamNeuron-PC-Plugin-0.2.3-Setup.exe) | [zip](https://github.com/kkomang2re/camneuron/releases/download/plugin-v0.2.3/CamNeuron-PC-Plugin-0.2.3-windows-x64.zip) |

폰에서 APK를 받아 설치할 때 "출처를 알 수 없는 앱"과 Play 프로텍트 안내가 나오면 [설치 방법](https://kkomang2re.github.io/camneuron/beta.html)을 보세요.

- 다운로드와 사용법: https://kkomang2re.github.io/camneuron/
- 개인정보처리방침: https://kkomang2re.github.io/camneuron/privacy.html
- 모든 버전: [Releases](https://github.com/kkomang2re/camneuron/releases) (체험판은 "Pre-release"로 표시)

The PC plugin that turns an Android phone running the CamNeuron app into an OBS Studio source (Wi-Fi or USB).

## 이 저장소에 있는 것

| 폴더 | 내용 |
|---|---|
| `obs-plugin/` | PC 플러그인 소스 (C, Windows x64, OBS Studio 33용) |
| `protocol/PROTOCOL.md` | 폰 앱과 플러그인의 통신 규격 (v3: PC 승인, 영상·소리, 제어) |
| `tools/fake-phone/` | 폰 없이 플러그인을 시험하는 가짜 폰과 폰 점검 도구 (Java 11 이상) |
| `docs/` | 다운로드 페이지와 개인정보처리방침 (GitHub Pages) |

폰 앱은 네트워크로만 통신하는 별개 프로그램이라 소스가 이 저장소에 없습니다 (Releases의 체험판 APK는 실행 파일만 배포).

## 빌드 (Windows, Visual Studio 2022 Build Tools, CMake 3.24 이상)

```powershell
powershell -ExecutionPolicy Bypass -File obs-plugin\scripts\setup-deps.ps1     # OBS 소스·obs-deps 받기 (처음 한 번)
cmake -S obs-plugin -B obs-plugin\build -G "Visual Studio 17 2022" -A x64
powershell -ExecutionPolicy Bypass -File obs-plugin\scripts\package.ps1        # 설치 프로그램(Inno Setup 6)과 zip → obs-plugin\dist
```

플러그인은 설치된 OBS와 같은 버전의 헤더·FFmpeg으로 빌드해야 합니다. OBS를 업데이트하면 `setup-deps.ps1`의 버전 값을 맞추고 다시 빌드하세요.

가짜 폰으로 시험하기:

```powershell
java "-Dfile.encoding=UTF-8" "-Dpaircode=123456" tools\fake-phone\FakePhone.java test.h264 1280 720 30
```

OBS 캠뉴런 소스의 주소를 `127.0.0.1`로 두고 연결 코드 칸에 `123456`을 넣으면 테스트 화면이 나옵니다.

## 라이선스와 상표

- PC 플러그인은 [GNU General Public License v2.0 이상](LICENSE)으로 배포합니다 (OBS Studio의 libobs를 쓰기 때문).
- OBS Studio는 OBS Project의, 치지직은 NAVER Corp.의 상표이며 캠뉴런은 이들과 관련이 없습니다.
