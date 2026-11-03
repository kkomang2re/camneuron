# 캠뉴런 통신 프로토콜 v3

폰 앱과 OBS 플러그인이 함께 따르는 기준 문서입니다. 둘 중 한쪽을 바꾸면 이 문서와 다른 쪽도 같이 바꿔야 합니다.

- 전송: TCP 한 개. 영상과 양방향 제어 메시지를 같은 연결로 주고받습니다 (USB adb 터널 하나로 충분하도록).
- 기본 포트: **9610**
- 역할: **폰이 서버**, PC(OBS 플러그인)가 클라이언트
  - Wi-Fi: PC가 `폰IP:9610`으로 접속
  - USB: PC가 `adb forward tcp:9610 tcp:9610` 실행 후 `127.0.0.1:9610`으로 접속
- 모든 정수는 **big-endian**
- 폰은 한 번에 PC 하나만 받습니다. 새 PC가 승인(1.1)을 통과하면 기존 연결을 끊습니다.

v1과 달라진 점: 버전 2, Hello의 7번 바이트가 flags(CBR), 제어 스트림(stream_id 2)과 JSON 메시지 추가.
v2에 나중에 더한 것: 폰 마이크 소리(stream_id 1, Hello flags bit1, `mic` 키). 모르는 stream_id와 flag는 무시하므로 버전은 그대로입니다.
v2와 달라진 점: 버전 3, Hello와 Handshake 사이에 PC 승인(1.1). 승인하지 않은 PC는 영상·소리를 받을 수 없습니다.

접속 순서: PC가 Hello → 폰이 `auth_challenge` → PC가 `auth` → 폰이 `auth_result` → (승인되면) 폰이 Handshake → 패킷 반복.

**버전이 다를 때:** Hello의 버전이 폰과 다르면 폰은 자기 버전을 담은 Handshake(나머지 값 0, 이름 없음)를 보내고 끊습니다.
PC는 Hello 다음 첫 4바이트가 `"PCAM"`이면 Handshake로 보고 버전이 맞지 않는다고 알립니다. 패킷 헤더의 첫 바이트는 stream_id(0~2)라서
`"PCAM"`과 헷갈리지 않습니다. (v2 폰은 버전이 다르면 아무것도 보내지 않고 끊습니다.)

## 1. Hello (PC → 폰, 28바이트)

PC가 접속 직후 원하는 스트림 설정을 보냅니다. 폰은 지원 범위 안에서 가장 가까운 값을 고릅니다.

| 오프셋 | 크기 | 필드 | 값 |
|---|---|---|---|
| 0 | 4 | magic | `"PCAM"` |
| 4 | 2 | version | `3` |
| 6 | 1 | facing | `0` = 후면, `1` = 전면 |
| 7 | 1 | flags | bit0 `CBR` (고정 비트레이트, 지원 안 하면 VBR), bit1 `MIC` (폰 마이크 소리를 보냄) |
| 8 | 4 | video_codec | fourcc `'h264'` (0x68323634) 또는 `'h265'` (0x68323635) |
| 12 | 4 | width | 예: 1920 |
| 16 | 4 | height | 예: 1080 |
| 20 | 2 | fps | 예: 30 |
| 22 | 2 | reserved | `0` |
| 24 | 4 | bitrate | bps, 예: 10000000 |

## 1.1 PC 승인 (Hello 다음, Handshake 전)

같은 Wi-Fi의 아무 기기나 폰 카메라와 마이크를 볼 수 없도록, 폰은 승인한 PC에만 영상을 보냅니다. 처음 보는 PC는 폰 화면에 뜬
6자리 코드를 OBS에 입력해야 하고, 그때 받은 토큰으로 다음부터는 묻지 않고 연결합니다. 아래 메시지는 모두 제어 패킷(3장의 패킷 형식,
stream_id 2)입니다.

1. 폰 → PC `auth_challenge`: 접속마다 새로 만든 16바이트 난수와 폰 ID (8장의 `id`와 같음)
   ```json
   {"type":"auth_challenge","nonce":"9f86d081884c7d659a2feaa0c55ad015","phone_id":"1b4e28ba-…"}
   ```
2. PC → 폰 `auth`
   ```json
   {"type":"auth","pc_id":"0d3c…","pc_name":"DESKTOP-ABC","proof":"<64자 hex>","code":"482913"}
   ```
   | 키 | 값 |
   |---|---|
   | `pc_id` | PC(OBS 설치)마다 한 번 만든 고정 ID. 64자 이하 |
   | `pc_name` | 폰 화면에 보여 줄 PC 이름. 64자 이하 |
   | `proof` | 이 폰(`phone_id`)에게 받은 토큰이 있을 때만. `HMAC-SHA256(key = 토큰 32바이트, data = nonce 16바이트)`의 hex |
   | `code` | 사용자가 OBS에 입력한 6자리 코드가 있을 때만 |
3. 폰 → PC `auth_result`
   - 승인: `{"type":"auth_result","ok":true}`. 코드로 새로 승인했으면 `"token":"<32바이트 hex>"`가 붙고, PC는 이 토큰을 `phone_id`별로 저장합니다.
     이어서 Handshake가 옵니다.
   - 거절: `{"type":"auth_result","ok":false,"reason":"…"}`를 보내고 폰이 연결을 끊습니다.

     | `reason` | 뜻 | PC 동작 |
     |---|---|---|
     | `pair_required` | 이 PC를 모름 (토큰이 없거나 폰에서 지웠음). 폰 화면에 PC 이름과 코드가 뜬다 | 갖고 있던 그 폰의 토큰을 지우고 코드 입력을 안내. 1초마다 다시 접속해 요청을 유지 |
     | `bad_code` | 코드가 틀림. `attempts_left` = 잠기기 전까지 남은 횟수 | 같은 코드를 다시 보내지 않음 |
     | `locked` | 틀린 코드가 5번 쌓여 `retry_s`초 동안 코드를 받지 않음. 코드도 새로 바뀜 | 안내만 하고 계속 다시 접속 |

폰 쪽 규칙:
- 코드는 `pair_required` 요청이 처음 올 때 만들고, 승인에 성공하거나 5번 틀리면 새로 만듭니다. 마지막 요청 뒤 1분이 지나면 화면에서 지우고 코드도 버립니다.
- 잠금 시간은 30초에서 시작해 잠길 때마다 두 배(최대 1시간)가 되고, 코드로 승인에 성공하면 처음으로 돌아갑니다. 토큰 확인은 잠긴 동안에도 됩니다.
- 승인을 마치기 전에는 기존 연결을 끊지 않습니다. 각 단계는 10초 안에 와야 하고, 승인 중인 접속은 동시에 4개까지만 받습니다.
- 폰 설정의 "허용한 PC → 모두 지우기"로 승인을 취소할 수 있고, 지금 연결된 PC도 끊깁니다. 승인한 PC는 최근 20대까지 기억합니다.

영상과 소리 자체는 암호화하지 않습니다. 같은 네트워크에서 패킷을 엿볼 수 있는 사람은 화면을 볼 수 있습니다.
토큰은 처음 승인할 때 한 번만 오가고, 그 뒤로는 접속마다 다른 HMAC만 오갑니다.

## 2. Handshake (폰 → PC, 26바이트 + 이름)

| 오프셋 | 크기 | 필드 | 값 |
|---|---|---|---|
| 0 | 4 | magic | `"PCAM"` |
| 4 | 2 | version | `3` |
| 6 | 4 | video_codec | fourcc (`'h264'` / `'h265'`) |
| 10 | 4 | audio_codec | 마이크를 켜면 쓸 코덱: `'opus'` (0x6F707573) 또는 `'pcm '` (0x70636D20). `0` = 마이크 권한 없음 |
| 14 | 4 | width | |
| 18 | 4 | height | |
| 22 | 2 | fps | |
| 24 | 2 | name_len | 기기 이름 바이트 수 |
| 26 | name_len | device_name | UTF-8 |

## 3. 패킷 (양방향, 반복)

| 오프셋 | 크기 | 필드 | 값 |
|---|---|---|---|
| 0 | 1 | stream_id | `0` = 영상(폰→PC), `1` = 소리(폰→PC), `2` = 제어(양방향) |
| 1 | 1 | flags | 영상: bit0 `CONFIG` (SPS/PPS/VPS), bit1 `KEYFRAME`. 소리: bit0 `CONFIG` |
| 2 | 2 | reserved | `0` |
| 4 | 4 | size | payload 바이트 수 (최대 16 MiB) |
| 8 | 8 | pts_us | 영상: 촬영 시각(µs, 폰의 카메라 시계). 소리: 첫 샘플을 녹음한 시각(영상과 같은 시계). 제어: 0 |
| 16 | size | payload | 영상은 Annex-B, 소리는 아래 3.1, 제어는 UTF-8 JSON 객체 |

- 영상의 첫 패킷은 `CONFIG`, 그다음은 `KEYFRAME`입니다. 카메라 전환 등으로 인코더가 바뀌면 다시 `CONFIG`부터 옵니다 (해상도가 바뀔 수 있음).
- 받는 쪽은 `CONFIG` 내용을 보관했다가 다음 영상 패킷 앞에 붙여 디코더에 넣습니다.
- 모르는 `stream_id`와 모르는 JSON `type`은 무시합니다 (하위 호환).

### 3.1 소리 (stream_id 1)

마이크가 켜질 때마다 `CONFIG` 패킷이 먼저 오고, 그 뒤로 압축된 소리 패킷(보통 20ms씩)이 옵니다. 마이크를 끄면 소리 패킷이 오지 않습니다.

`CONFIG` payload:

| 오프셋 | 크기 | 필드 | 값 |
|---|---|---|---|
| 0 | 4 | codec | `'opus'` 또는 `'pcm '` |
| 4 | 4 | sample_rate | 예: 48000 |
| 8 | 1 | channels | 1 또는 2 |
| 9 | 3 | reserved | `0` |
| 12 | 나머지 | codec_header | Opus: `OpusHead` (RFC 7845, 19바이트). PCM: 없음 |

소리 패킷 payload: Opus는 패킷 하나, PCM은 16비트 little-endian 샘플을 채널 순서대로 섞어 놓은 것(L R L R …).
Opus는 Android 10 이상에서 쓰고, Opus 인코더가 없는 폰은 PCM으로 보냅니다.

PC는 소리와 영상을 같은 시계의 타임스탬프로 OBS에 넘기므로 OBS가 립싱크를 맞춥니다.

## 4. 제어 메시지 (stream_id 2, JSON)

모든 메시지에 `"type"`이 있습니다. 값의 의미:

| 키 | 값 |
|---|---|
| `facing` | `"back"` / `"front"` |
| `zoom` | 배율 (예: 0.6 = 초광각, 1.0, 3.0). `caps`의 범위 안 |
| `focus_mode` | `"continuous"` (계속 자동) / `"auto"` (한 번 맞추고 고정) / `"manual"` |
| `focus_distance` | 0.0 (무한대) ~ 1.0 (가장 가까움). `manual`일 때 사용 |
| `ev` | 노출 보정 (EV 단위, 예: -1.5) |
| `ae_lock`, `awb_lock` | 노출 / 화이트밸런스 잠금 |
| `wb` | `"auto"` / `"incandescent"` / `"fluorescent"` / `"daylight"` / `"cloudy"` |
| `torch` | 손전등 |
| `stabilization` | 전자식 손떨림 보정 (켜면 화면이 약간 잘리고 지연이 늘 수 있음) |
| `bitrate` | bps. 연결을 끊지 않고 바로 바뀜 |
| `width`, `height`, `fps` | 송출 해상도·프레임. 바꾸면 연결은 유지한 채 인코더를 새로 만든다 (`CONFIG`부터 다시 옴) |
| `codec` | `"h264"` / `"h265"`. 바꾸면 위와 같이 인코더를 새로 만든다 |
| `rate_control` | `"cbr"` / `"vbr"`. 바꾸면 위와 같이 인코더를 새로 만든다 |
| `mic` | 폰 마이크 소리를 보냄. 폰에 마이크 권한이 없으면 켜도 `state`에 `false`로 돌아온다 |

채팅·후원 창은 `set`에 `chat_url`, `alert_url`, `chat_stream`, `alert_stream`을 넣어 바꿀 수 있습니다 (OBS 속성 창에서 넣은 URL). 위치·크기는 폰에서만 정합니다. 접속 직후에는 폰에 저장된 값을 우선하고, 폰에 URL이 비어 있을 때만 PC가 자기 URL을 보냅니다.
무료 버전 폰은 `chat_stream`·`alert_stream`을 켜지 않고, 켜는 `set`을 받으면 꺼진 값의 `overlays`를 다시 보내 PC 설정도 되돌립니다 (송출 합성은 프로 기능).

`set`의 송출 형식 값은 요청이고, 폰은 카메라와 인코더가 지원하는 가장 가까운 값으로 맞춘 뒤 실제 값을 `state`로 알려 줍니다. 인코더를 새로 만들 때 폰은 새 `state`를 새 `CONFIG`보다 먼저 보내므로, PC는 `state`의 `codec`이 바뀌었으면 디코더를 바꿔 둡니다.

접속 직후에는 Hello로 보낸 형식과 마이크 켜짐 여부를 폰이 적용하고, PC는 나머지 카메라 값만 `set`으로 보냅니다.

### 폰 → PC

- `caps`: 연결 직후와 카메라 전환 후. 현재 카메라가 지원하는 범위
  ```json
  {"type":"caps","facing":"back","has_front":true,"has_back":true,"zoom_min":0.6,"zoom_max":10.0,
   "ev_min":-2.0,"ev_max":2.0,"ev_step":0.1,"manual_focus":true,"torch":true,"stabilization":true,
   "wb_modes":"auto,incandescent,fluorescent,daylight,cloudy"}
  ```
  `wb_modes`는 쉼표로 구분한 문자열입니다 (OBS의 JSON 처리기가 문자열 배열을 읽지 못해서).
- `state`: 연결 직후와 값이 바뀔 때마다 (폰 화면에서 바꾼 것도 포함, 초당 최대 10회). 위 표의 모든 키.
  송출 형식(`width`, `height`, `fps`, `bitrate`)은 요청을 폰이 지원하는 값으로 맞춘 것이고, 발열로 낮추기 전 값입니다 (OBS 설정에 남는 값).
  실제로 보내는 값은 아래 키로 함께 옵니다:
  | 키 | 값 |
  |---|---|
  | `heat_level` | 0 = 정상, 1 = 30fps 이하·비트레이트 80%, 2 = 1080p 이하·60%, 3 = 720p 이하·40% |
  | `free_limit` | `true`면 폰 앱이 무료 버전이라 요청보다 낮게 보내는 중 (720p 30fps H.264까지). 요청 값은 그대로 두고 실제 값은 `send_*`. PC는 안내만 한다 |
  | `send_width`, `send_height`, `send_fps`, `send_bitrate` | 지금 실제로 보내는 형식. 화면 크기 계산(채팅·후원 창 합성 위치)에는 이 값을 씀 |

  폰은 발열 단계(Android THERMAL_STATUS)나 배터리 온도가 5초 넘게 높으면 그 단계로 낮추고, 식은 상태가 3분 이어질 때마다
  한 단계씩 되돌립니다. 폰 설정의 "발열 대응"을 끄면 바로 원래대로 돌아갑니다.
- `stats`: 1초마다
  ```json
  {"type":"stats","fps":29.9,"bitrate":9800000,"dropped":0,"encode_ms":18.2,"battery":83,"temp":34.5,"thermal":0}
  ```
  `encode_ms`는 촬영부터 인코딩 완료까지의 평균 시간, `thermal`은 Android 발열 단계(0 = 정상, 3 이상 = 심함)
- `overlays`: 연결 직후와 채팅·후원 창 설정이 바뀔 때마다. 키는 `chat_`, `alert_` 접두어 + `url`, `visible`, `stream`(송출에 포함),
  `x`, `y`, `w`, `h`(영상 기준 0~1), `zoom`(CSS 배율), `opacity`. `reference_width`는 폰 화면에서 영상이 그려지는 폭(CSS px)으로,
  PC는 브라우저 크기를 `w × reference_width` CSS px로 만들고 영상 폭에 맞춰 확대해 그려 폰과 같은 모양을 만든다.
  ```json
  {"type":"overlays","reference_width":732,"chat_url":"https://…","chat_visible":true,"chat_stream":false,
   "chat_x":0.7,"chat_y":0.05,"chat_w":0.28,"chat_h":0.6,"chat_zoom":1.0,"chat_opacity":0.9,"alert_url":"", …}
  ```
- `pong`: `ping`에 대한 답. `clock_us`는 영상 `pts_us`와 같은 시계의 현재 값
  ```json
  {"type":"pong","id":7,"clock_us":123456789012}
  ```

### PC → 폰

- `set`: 위 표의 키 중 바꿀 것만
  ```json
  {"type":"set","zoom":2.0,"torch":true}
  ```
- `focus_at`: 화면의 한 점에 초점·노출을 맞추고 `focus_mode`를 `auto`로 바꿈. 좌표는 0~1 (왼쪽 위가 0,0)
  ```json
  {"type":"focus_at","x":0.5,"y":0.5}
  ```
- `ping`: 2초마다. 폰 시계와 PC 시계의 차이를 재서 지연을 계산하는 데 씀
  ```json
  {"type":"ping","id":7}
  ```
- `pc_stats`: 1초마다. 폰 화면에 표시할 PC 쪽 측정값. `latency_ms`가 -1이면 아직 모름
  ```json
  {"type":"pc_stats","latency_ms":85,"fps":30.0}
  ```
- `keyframe`: 키프레임 즉시 요청

## 5. 지연 측정

1. PC가 `ping`을 보낸 시각 t0, `pong`을 받은 시각 t1, 폰 시계 값 P로 `offset = P - (t0 + t1) / 2`를 구합니다. 왕복 시간이 가장 짧았던 측정값을 씁니다.
2. 프레임을 디코딩한 순간 `지연 = 지금 - (pts_us - offset)`. 촬영부터 디코딩까지의 시간이며, OBS 렌더링과 모니터 표시 시간(보통 1~2프레임)은 빠집니다.

## 6. 혼잡 제어 (폰 쪽 동작)

Wi-Fi가 느려져 송신 대기열의 영상이 약 1/3초 분량을 넘으면:

1. 대기열의 영상·소리 패킷을 모두 버림 (`CONFIG`와 제어 메시지는 유지)
2. 인코더에 키프레임을 즉시 요청
3. 다음 `KEYFRAME`이 올 때까지 새 영상 패킷을 버림. 소리는 키프레임을 기다리지 않고 계속 보냄

소리 패킷만 약 1/3초 분량 넘게 쌓여도(영상이 멈춘 채 네트워크가 막힌 경우) 쌓인 소리를 버립니다.

## 7. 연결 유지

- 폰은 화면이 멈춰 있어도 최소 100ms마다 프레임을 보냅니다 (인코더 반복 프레임).
- PC는 5초 동안 아무것도 받지 못하면 연결이 끊긴 것으로 보고 다시 접속합니다.

## 8. 폰 찾기 (UDP 브로드캐스트)

PC가 폰의 IP를 몰라도 같은 네트워크에서 찾을 수 있게 합니다. mDNS 대신 브로드캐스트를 쓰는 이유: Windows에서 mDNS 응답을 받으려면
시스템 DNS 서비스나 방화벽 규칙에 기대야 하지만, 브로드캐스트에 대한 응답은 Windows 방화벽이 기본으로 들여보냅니다.

1. PC는 UDP **9610**으로 `{"type":"discover","version":3}`를 보냅니다. 받는 곳은 `255.255.255.255`와 어댑터마다의 서브넷 브로드캐스트
   (예: `192.168.0.255`, Windows는 255.255.255.255를 기본 경로 어댑터로만 보냄)입니다. UDP라 잃어버릴 수 있으므로 0.3초 뒤 한 번 더 보내고,
   0.8초 동안 답을 모읍니다.
2. 폰(송출 서비스가 켜져 있는 동안)은 보낸 PC에게만 답합니다:
   ```json
   {"type":"phone","version":3,"id":"1b4e28ba-…","name":"Galaxy S25 Ultra","port":9610,"busy":false,"wifi_ip":"192.168.0.12"}
   ```
   | 키 | 값 |
   |---|---|
   | `id` | 앱을 처음 실행할 때 만든 고정 ID (앱을 지우기 전까지 같음) |
   | `name` | 폰 설정의 기기 이름 |
   | `port` | 송출 TCP 포트 |
   | `busy` | 지금 다른 PC가 연결 중 |
   | `wifi_ip` | 폰의 Wi-Fi 주소. USB 테더링 등으로 여러 경로에서 답이 오면 PC는 이 주소로 온 답을 고른다 (케이블을 뽑아도 끊기지 않게) |

   PC는 답이 온 주소(보낸 쪽 IP)로 접속합니다.
3. 폰은 TCP 접속 직후 제어 메시지 `{"type":"device","id":"…","name":"…"}`도 보냅니다. 주소를 직접 넣어 연결한 PC도 이 ID를 기억해,
   나중에 폰의 IP가 바뀌면 다시 찾을 수 있습니다.

PC(OBS 플러그인)의 동작:
- 주소가 비어 있으면 찾아서 전에 연결한 폰(ID)에, 없으면 이 PC를 승인한(토큰이 있는) 폰, 그다음 다른 PC와 연결 중이 아닌 첫 폰 순으로
  접속하고 주소와 ID를 설정에 적습니다.
- 저장된 주소로 3번 연달아 접속하지 못하면 같은 ID의 폰을 찾고, 다른 주소에 있으면 그 주소로 바꿉니다 (ID를 모르면 폰이 하나만 있을 때만).
