import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * 폰 없이 OBS 플러그인을 시험하는 가짜 폰 서버 (protocol/PROTOCOL.md v3).
 *
 * H.264 Annex-B 파일을 액세스 유닛 구분자(AUD) 기준으로 잘라 실시간 속도로 반복 전송하고,
 * caps/state/stats를 보내며 set을 받으면 상태에 반영해 돌려준다. ping에는 pts와 같은 시계로 답한다.
 * PC가 마이크를 켜면 1초마다 0.1초 삐 소리(PCM)를 보내고, -Dopus=파일.opus를 주면 그 Ogg Opus 파일을 보낸다.
 * 테스트 영상 만들기 (x264의 aud=1로 AUD를 넣고, B-프레임은 끈다):
 *   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 10 -c:v libx264 -bf 0 -g 30 -x264-params aud=1 test.h264
 * 테스트 소리 만들기 (20ms 프레임):
 *   ffmpeg -f lavfi -i sine=frequency=440:beep_factor=4:sample_rate=48000 -t 10 -ac 2 -c:a libopus -frame_duration 20 test.opus
 * 실행 (JDK 11 이상):
 *   java -Dfile.encoding=UTF-8 [-Dopus=test.opus] [-Dpaircode=123456] [-Dfree=true] [-Dheat=2] FakePhone.java test.h264 1280 720 30 [포트]
 * PC 승인: 처음 보는 PC에는 연결 코드를 콘솔에 출력한다 (-Dpaircode로 고정 가능). 받은 토큰은 이 프로세스가 끝나면 잊는다.
 */
public class FakePhone {
    private static final int MAGIC = 0x5043414D; // "PCAM"
    private static final int VERSION = 3;
    private static final int CODEC_H264 = 0x68323634;
    private static final int AUDIO_OPUS = 0x6F707573;
    private static final int AUDIO_PCM = 0x70636D20;
    private static final int STREAM_VIDEO = 0;
    private static final int STREAM_AUDIO = 1;
    private static final int STREAM_CONTROL = 2;
    private static final int FLAG_CONFIG = 1;
    private static final int FLAG_KEYFRAME = 2;
    private static final int HELLO_FLAG_MIC = 2;
    private static final int SAMPLE_RATE = 48000;
    private static final int CHUNK_FRAMES = SAMPLE_RATE / 50; // 20ms
    private static final Pattern FIELD = Pattern.compile("\"(\\w+)\"\\s*:\\s*(\"[^\"]*\"|[-0-9.eE]+|true|false)");
    private static final SecureRandom RANDOM = new SecureRandom();
    private static final String PAIR_CODE = System.getProperty("paircode", String.format("%06d", RANDOM.nextInt(1_000_000)));
    private static final Map<String, byte[]> TOKENS = new ConcurrentHashMap<>();

    public static void main(String[] args) throws IOException {
        if (args.length < 4) {
            System.err.println("usage: java FakePhone.java <file.h264> <width> <height> <fps> [port]");
            System.exit(1);
        }
        List<byte[]> units = splitAccessUnits(Files.readAllBytes(Path.of(args[0])));
        int width = Integer.parseInt(args[1]);
        int height = Integer.parseInt(args[2]);
        int fps = Integer.parseInt(args[3]);
        int port = args.length > 4 ? Integer.parseInt(args[4]) : 9610;
        String opusFile = System.getProperty("opus");
        List<byte[]> opus = opusFile != null ? readOggPackets(Files.readAllBytes(Path.of(opusFile))) : null;
        System.out.printf("%d access units, audio: %s, listening on port %d%n", units.size(),
                opus != null ? "Opus (" + (opus.size() - 2) + " packets)" : "PCM beeps", port);

        startDiscoveryResponder(port);
        try (ServerSocket server = new ServerSocket(port)) {
            while (true) {
                try (Socket socket = server.accept()) {
                    System.out.println("client connected: " + socket.getRemoteSocketAddress());
                    new Session(socket, units, width, height, fps, opus).run();
                } catch (IOException e) {
                    System.out.println("client disconnected: " + e.getMessage());
                }
            }
        }
    }

    private static final class Session {
        private final Socket socket;
        private final List<byte[]> units;
        private final int width, height, fps;
        private final List<byte[]> opus; // [OpusHead, OpusTags, 소리 패킷...] 또는 null(PCM)
        private final Map<String, String> state = new LinkedHashMap<>();
        private final long startNanos = System.nanoTime();
        private OutputStream out;
        private Thread audio;
        private volatile boolean audioRunning;

        Session(Socket socket, List<byte[]> units, int width, int height, int fps, List<byte[]> opus) {
            this.socket = socket;
            this.units = units;
            this.width = width;
            this.height = height;
            this.fps = fps;
            this.opus = opus;
        }

        void run() throws IOException {
            socket.setTcpNoDelay(true);
            DataInputStream in = new DataInputStream(socket.getInputStream());
            if (in.readInt() != MAGIC) throw new IOException("bad hello magic");
            int version = in.readUnsignedShort();
            int facing = in.readUnsignedByte();
            int flags = in.readUnsignedByte();
            int codec = in.readInt();
            int reqWidth = in.readInt();
            int reqHeight = in.readInt();
            int reqFps = in.readUnsignedShort();
            in.readUnsignedShort();
            int bitrate = in.readInt();
            boolean mic = (flags & HELLO_FLAG_MIC) != 0;
            System.out.printf("hello: v%d facing=%d cbr=%d mic=%b codec=%08x %dx%d@%d %d bps (sending %dx%d@%d H.264)%n",
                    version, facing, flags & 1, mic, codec, reqWidth, reqHeight, reqFps, bitrate, width, height, fps);
            out = socket.getOutputStream();
            if (version != VERSION) {
                // 버전이 다르면 이 폰의 버전만 담은 Handshake로 답하고 끊는다.
                write(ByteBuffer.allocate(26).putInt(MAGIC).putShort((short) VERSION).array());
                return;
            }
            if (!authenticate(in)) return;

            state.put("facing", facing == 1 ? "\"front\"" : "\"back\"");
            state.put("zoom", "1.0");
            state.put("focus_mode", "\"continuous\"");
            state.put("focus_distance", "0.0");
            state.put("ev", "0.0");
            state.put("ae_lock", "false");
            state.put("awb_lock", "false");
            state.put("wb", "\"auto\"");
            state.put("torch", "false");
            state.put("stabilization", "false");
            state.put("bitrate", Integer.toString(bitrate));
            state.put("width", Integer.toString(width));
            state.put("height", Integer.toString(height));
            state.put("fps", Integer.toString(fps));
            state.put("mic", Boolean.toString(mic));
            // -Dheat=1~3: 폰이 뜨거워 요청(Hello)보다 낮게 보내는 척한다. 요청 값은 width/height, 실제 값은 send_*.
            // -Dfree=true: 무료 버전 폰처럼 요청(Hello)보다 낮게 보낸다고 알린다 (OBS 속성의 안내 확인용).
            if (Boolean.getBoolean("free")) {
                state.put("width", Integer.toString(reqWidth));
                state.put("height", Integer.toString(reqHeight));
                state.put("fps", Integer.toString(reqFps));
                state.put("free_limit", "true");
                state.put("send_width", Integer.toString(width));
                state.put("send_height", Integer.toString(height));
                state.put("send_fps", Integer.toString(fps));
            }
            int heat = Integer.getInteger("heat", 0);
            if (heat > 0) {
                state.put("width", Integer.toString(reqWidth));
                state.put("height", Integer.toString(reqHeight));
                state.put("fps", Integer.toString(reqFps));
                state.put("heat_level", Integer.toString(heat));
                state.put("send_width", Integer.toString(width));
                state.put("send_height", Integer.toString(height));
                state.put("send_fps", Integer.toString(fps));
                state.put("send_bitrate", Integer.toString(bitrate * 6 / 10));
            }

            byte[] name = "FakePhone".getBytes(StandardCharsets.UTF_8);
            write(ByteBuffer.allocate(26 + name.length)
                    .putInt(MAGIC).putShort((short) VERSION).putInt(CODEC_H264).putInt(opus != null ? AUDIO_OPUS : AUDIO_PCM)
                    .putInt(width).putInt(height).putShort((short) fps).putShort((short) name.length).put(name)
                    .array());
            sendControl("{\"type\":\"caps\",\"facing\":\"back\",\"has_front\":true,\"has_back\":true,"
                    + "\"zoom_min\":0.6,\"zoom_max\":10.0,\"ev_min\":-2.0,\"ev_max\":2.0,\"ev_step\":0.1,"
                    + "\"manual_focus\":true,\"torch\":true,\"stabilization\":true,"
                    + "\"wb_modes\":\"auto,incandescent,fluorescent,daylight,cloudy\"}");
            sendState();
            // 후원 알림을 송출에 포함한 상태. OBS 플러그인이 브라우저 화면을 합성하는지 시험한다.
            sendControl("{\"type\":\"overlays\",\"reference_width\":720,"
                    + "\"chat_url\":\"\",\"chat_visible\":true,\"chat_stream\":false,\"chat_x\":0.7,\"chat_y\":0.05,"
                    + "\"chat_w\":0.28,\"chat_h\":0.6,\"chat_zoom\":1.0,\"chat_opacity\":0.9,"
                    + "\"alert_url\":\"https://example.com/\",\"alert_visible\":true,\"alert_stream\":true,"
                    + "\"alert_x\":0.25,\"alert_y\":0.05,\"alert_w\":0.5,\"alert_h\":0.35,\"alert_zoom\":1.0,"
                    + "\"alert_opacity\":0.9}");

            Thread reader = new Thread(() -> readControl(in), "reader");
            reader.setDaemon(true);
            reader.start();
            setMic(mic);

            try {
                long frameNanos = 1_000_000_000L / fps;
                for (long frame = 0; ; frame++) {
                    byte[] unit = units.get((int) (frame % units.size()));
                    long ptsUs = (System.nanoTime() - startNanos) / 1000; // ping 응답과 같은 시계
                    ByteBuffer packet = ByteBuffer.allocate(16 + unit.length);
                    packet.put((byte) STREAM_VIDEO).put((byte) (isKeyFrame(unit) ? FLAG_KEYFRAME : 0)).putShort((short) 0)
                            .putInt(unit.length).putLong(ptsUs).put(unit);
                    write(packet.array());
                    if (frame % fps == 0) {
                        sendControl(String.format("{\"type\":\"stats\",\"fps\":%d,\"bitrate\":%s,\"dropped\":0,"
                                + "\"encode_ms\":5,\"battery\":77,\"temp\":30.5,\"thermal\":0}", fps, state.get("bitrate")));
                    }

                    long sleepNanos = startNanos + (frame + 1) * frameNanos - System.nanoTime();
                    if (sleepNanos > 0) {
                        try {
                            Thread.sleep(sleepNanos / 1_000_000, (int) (sleepNanos % 1_000_000));
                        } catch (InterruptedException e) {
                            return;
                        }
                    }
                }
            } finally {
                setMic(false);
            }
        }

        /** 마이크를 켜면 소리 스레드를 돌린다. 켤 때마다 CONFIG부터 보낸다. */
        private synchronized void setMic(boolean on) {
            if (on == audioRunning) return;
            audioRunning = on;
            if (on) {
                audio = new Thread(this::sendAudio, "audio");
                audio.setDaemon(true);
                audio.start();
            } else if (audio != null) {
                audio.interrupt();
                audio = null;
            }
            System.out.println("mic " + (on ? "on" : "off"));
        }

        /** 20ms마다 소리 패킷 하나. pts는 그 20ms의 첫 샘플 시각이고, 녹음이 끝난 뒤에 보낸다 (실제 폰과 같게). */
        private void sendAudio() {
            try {
                int channels = opus != null ? opus.get(0)[9] : 2;
                byte[] head = opus != null ? opus.get(0) : new byte[0];
                ByteBuffer config = ByteBuffer.allocate(12 + head.length)
                        .putInt(opus != null ? AUDIO_OPUS : AUDIO_PCM).putInt(SAMPLE_RATE).put((byte) channels)
                        .put(new byte[3]).put(head);
                writeAudio(FLAG_CONFIG, 0, config.array());

                long audioStart = System.nanoTime();
                for (long chunk = 0; audioRunning; chunk++) {
                    long chunkStart = audioStart + chunk * 20_000_000L;
                    long sleepNanos = chunkStart + 20_000_000L - System.nanoTime();
                    if (sleepNanos > 0) Thread.sleep(sleepNanos / 1_000_000, (int) (sleepNanos % 1_000_000));
                    byte[] payload = opus != null
                            ? opus.get(2 + (int) (chunk % (opus.size() - 2)))
                            : beep(chunkStart - startNanos, channels);
                    writeAudio(0, (chunkStart - startNanos) / 1000, payload);
                }
            } catch (InterruptedException | IOException e) {
                // 마이크 꺼짐 또는 연결 종료
            }
        }

        /**
         * 영상 pts 기준으로 매초 처음 0.1초 동안 1kHz 소리, 나머지는 조용함 (16비트 little-endian).
         * 매초 처음 0.1초가 하얀 영상을 보내면 녹화본에서 소리와 화면이 맞는지 잴 수 있다.
         */
        private byte[] beep(long chunkStartNs, int channels) {
            ByteBuffer pcm = ByteBuffer.allocate(CHUNK_FRAMES * channels * 2).order(ByteOrder.LITTLE_ENDIAN);
            for (int i = 0; i < CHUNK_FRAMES; i++) {
                long ns = chunkStartNs + i * 1_000_000_000L / SAMPLE_RATE;
                boolean on = ns % 1_000_000_000L < 100_000_000L;
                short value = on ? (short) (Math.sin(2 * Math.PI * 1000 * ns / 1e9) * 8000) : 0;
                for (int c = 0; c < channels; c++) pcm.putShort(value);
            }
            return pcm.array();
        }

        private void writeAudio(int flags, long ptsUs, byte[] payload) throws IOException {
            write(ByteBuffer.allocate(16 + payload.length)
                    .put((byte) STREAM_AUDIO).put((byte) flags).putShort((short) 0)
                    .putInt(payload.length).putLong(ptsUs).put(payload).array());
        }

        private void readControl(DataInputStream in) {
            try {
                while (true) {
                    int streamId = in.readUnsignedByte();
                    in.readUnsignedByte();
                    in.readUnsignedShort();
                    int size = in.readInt();
                    in.readLong();
                    byte[] payload = new byte[size];
                    in.readFully(payload);
                    if (streamId != STREAM_CONTROL) continue;
                    handle(new String(payload, StandardCharsets.UTF_8));
                }
            } catch (IOException e) {
                // 연결 종료
            }
        }

        /** PC 승인 (PROTOCOL.md 1.1). 실제 폰처럼 토큰(HMAC)이나 코드를 확인한다. 잠금은 흉내 내지 않는다. */
        private boolean authenticate(DataInputStream in) throws IOException {
            byte[] nonce = new byte[16];
            RANDOM.nextBytes(nonce);
            sendControl("{\"type\":\"auth_challenge\",\"nonce\":\"" + hex(nonce) + "\",\"phone_id\":\"fake-phone\"}");
            if (in.readUnsignedByte() != STREAM_CONTROL) throw new IOException("expected auth");
            in.readUnsignedByte();
            in.readUnsignedShort();
            byte[] payload = new byte[in.readInt()];
            in.readLong();
            in.readFully(payload);
            Map<String, String> auth = fields(new String(payload, StandardCharsets.UTF_8));
            String pcId = unquote(auth.get("pc_id"));
            String pcName = unquote(auth.get("pc_name"));
            String proof = unquote(auth.get("proof"));
            String code = unquote(auth.get("code"));

            byte[] token = pcId != null ? TOKENS.get(pcId) : null;
            if (token != null && proof != null && proof.equals(hex(hmac(token, nonce)))) {
                System.out.println("auth: PC '" + pcName + "' known (token)");
                sendControl("{\"type\":\"auth_result\",\"ok\":true}");
                return true;
            }
            if (pcId != null && PAIR_CODE.equals(code)) {
                byte[] fresh = new byte[32];
                RANDOM.nextBytes(fresh);
                TOKENS.put(pcId, fresh);
                System.out.println("auth: PC '" + pcName + "' paired with the code");
                sendControl("{\"type\":\"auth_result\",\"ok\":true,\"token\":\"" + hex(fresh) + "\"}");
                return true;
            }
            if (code != null) {
                System.out.println("auth: PC '" + pcName + "' sent a wrong code " + code);
                sendControl("{\"type\":\"auth_result\",\"ok\":false,\"reason\":\"bad_code\",\"attempts_left\":4}");
                return false;
            }
            System.out.println("auth: PC '" + pcName + "' asks to pair (proof " + (proof != null ? "rejected" : "none")
                    + "). Pairing code: " + PAIR_CODE);
            sendControl("{\"type\":\"auth_result\",\"ok\":false,\"reason\":\"pair_required\"}");
            return false;
        }

        private void handle(String json) throws IOException {
            Map<String, String> fields = fields(json);
            String type = fields.getOrDefault("type", "");
            if (type.equals("\"ping\"")) {
                sendControl("{\"type\":\"pong\",\"id\":" + fields.get("id") + ",\"clock_us\":"
                        + (System.nanoTime() - startNanos) / 1000 + "}");
                return;
            }
            System.out.println("received: " + json);
            if (type.equals("\"set\"")) {
                fields.remove("type");
                synchronized (state) {
                    state.putAll(fields);
                }
                if (fields.containsKey("mic")) setMic(fields.get("mic").equals("true"));
                sendState();
            }
        }

        private void sendState() throws IOException {
            StringBuilder json = new StringBuilder("{\"type\":\"state\"");
            synchronized (state) {
                state.forEach((k, v) -> json.append(",\"").append(k).append("\":").append(v));
            }
            sendControl(json.append('}').toString());
        }

        private void sendControl(String json) throws IOException {
            byte[] payload = json.getBytes(StandardCharsets.UTF_8);
            write(ByteBuffer.allocate(16 + payload.length)
                    .put((byte) STREAM_CONTROL).put((byte) 0).putShort((short) 0)
                    .putInt(payload.length).putLong(0).put(payload).array());
        }

        private synchronized void write(byte[] bytes) throws IOException {
            out.write(bytes);
        }
    }

    /** 평평한 JSON 객체의 키와 값 (문자열 값은 따옴표째). */
    private static Map<String, String> fields(String json) {
        Map<String, String> fields = new LinkedHashMap<>();
        Matcher m = FIELD.matcher(json);
        while (m.find()) fields.put(m.group(1), m.group(2));
        return fields;
    }

    private static String unquote(String value) {
        return value != null && value.length() >= 2 && value.startsWith("\"") ? value.substring(1, value.length() - 1) : null;
    }

    private static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format("%02x", b));
        return s.toString();
    }

    private static byte[] hmac(byte[] key, byte[] data) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(key, "HmacSHA256"));
            return mac.doFinal(data);
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }

    /** OBS의 "폰 찾기" 브로드캐스트(UDP 9610)에 답한다 (PROTOCOL.md 8장). 이미 쓰는 중이면 건너뛴다. */
    private static void startDiscoveryResponder(int tcpPort) {
        java.net.DatagramSocket socket;
        try {
            socket = new java.net.DatagramSocket(null);
            socket.setReuseAddress(true);
            socket.bind(new java.net.InetSocketAddress(9610));
        } catch (IOException e) {
            System.out.println("discovery responder unavailable: " + e.getMessage());
            return;
        }
        Thread thread = new Thread(() -> {
            byte[] buffer = new byte[512];
            while (true) {
                try {
                    java.net.DatagramPacket packet = new java.net.DatagramPacket(buffer, buffer.length);
                    socket.receive(packet);
                    String request = new String(packet.getData(), 0, packet.getLength(), StandardCharsets.UTF_8);
                    if (!request.contains("\"discover\"")) continue;
                    byte[] reply = String.format("{\"type\":\"phone\",\"version\":%d,\"id\":\"fake-phone\","
                            + "\"name\":\"FakePhone\",\"port\":%d,\"busy\":false,\"wifi_ip\":\"\"}", VERSION, tcpPort)
                            .getBytes(StandardCharsets.UTF_8);
                    socket.send(new java.net.DatagramPacket(reply, reply.length, packet.getSocketAddress()));
                    System.out.println("answered discovery from " + packet.getAddress().getHostAddress());
                } catch (IOException e) {
                    return;
                }
            }
        }, "discovery");
        thread.setDaemon(true);
        thread.start();
    }

    /** Ogg 페이지를 이어 붙여 패킷으로 나눈다 (RFC 3533). Opus는 [OpusHead, OpusTags, 소리 패킷...]. */
    private static List<byte[]> readOggPackets(byte[] data) {
        List<byte[]> packets = new ArrayList<>();
        java.io.ByteArrayOutputStream current = new java.io.ByteArrayOutputStream();
        int pos = 0;
        while (pos + 27 <= data.length) {
            if (data[pos] != 'O' || data[pos + 1] != 'g' || data[pos + 2] != 'g' || data[pos + 3] != 'S') {
                throw new IllegalArgumentException("not an Ogg file");
            }
            int segments = data[pos + 26] & 0xFF;
            int body = pos + 27 + segments;
            for (int i = 0; i < segments; i++) {
                int length = data[pos + 27 + i] & 0xFF;
                current.write(data, body, length);
                body += length;
                if (length < 255) { // 255보다 짧은 조각이 패킷의 끝
                    packets.add(current.toByteArray());
                    current.reset();
                }
            }
            pos = body;
        }
        if (packets.size() < 3 || !new String(packets.get(0), 0, 8, StandardCharsets.US_ASCII).equals("OpusHead")) {
            throw new IllegalArgumentException("not an Ogg Opus file");
        }
        return packets;
    }

    /** 00 00 01 09 (AUD) 위치마다 자른다. 앞에 00이 하나 더 있으면 4바이트 시작 코드로 본다. */
    private static List<byte[]> splitAccessUnits(byte[] data) {
        List<Integer> starts = new ArrayList<>();
        for (int i = 0; i + 3 < data.length; i++) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && (data[i + 3] & 0x1F) == 9) {
                starts.add(i > 0 && data[i - 1] == 0 ? i - 1 : i);
            }
        }
        if (starts.isEmpty()) throw new IllegalArgumentException("no AUD found; encode with -x264-params aud=1");
        List<byte[]> units = new ArrayList<>();
        for (int i = 0; i < starts.size(); i++) {
            int end = i + 1 < starts.size() ? starts.get(i + 1) : data.length;
            byte[] unit = new byte[end - starts.get(i)];
            System.arraycopy(data, starts.get(i), unit, 0, unit.length);
            units.add(unit);
        }
        return units;
    }

    /** NAL 유형 5(IDR)가 들어 있으면 키프레임. */
    private static boolean isKeyFrame(byte[] unit) {
        for (int i = 0; i + 3 < unit.length; i++) {
            if (unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1 && (unit[i + 3] & 0x1F) == 5) return true;
        }
        return false;
    }
}
