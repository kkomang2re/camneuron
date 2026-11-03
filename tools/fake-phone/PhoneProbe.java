import java.io.DataInputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.security.GeneralSecurityException;
import java.util.Properties;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/**
 * OBS 없이 폰 앱에 접속해 스트림을 점검하는 도구 (protocol/PROTOCOL.md v3의 PC 쪽).
 * 받은 영상을 Annex-B 파일로 저장하고 fps, 비트레이트, 키프레임 수, 촬영→수신 지연을 출력한다.
 * 제어 메시지(caps/state/stats)를 보여 주고, 지정하면 3초 뒤 set 메시지를 보낸다.
 *
 *   java -Dfile.encoding=UTF-8 PhoneProbe.java <폰IP[:포트]> [초=10] [출력.h264=probe.h264] [가로=1920] [세로=1080]
 *        [fps=30] [h264|h265] [Mbps=10] [set JSON, 예: {"type":"set","zoom":2.0}]
 * 처음에는 폰 화면에 뜬 연결 코드를 -Dcode=123456으로 주고 실행한다. 받은 토큰은 홈 폴더의 .phonecam-probe-tokens에 저장해
 * 다음부터는 코드 없이 연결된다.
 */
public class PhoneProbe {
    private static final int MAGIC = 0x5043414D; // "PCAM"
    private static final Pattern LONG_FIELD = Pattern.compile("\"(id|clock_us)\"\\s*:\\s*(-?\\d+)");

    private static OutputStream out;
    private static long pingSentUs;
    private static long clockOffsetUs;
    private static long bestRttUs = Long.MAX_VALUE;

    public static void main(String[] args) throws IOException {
        if (args.length < 1) {
            System.err.println("usage: java PhoneProbe.java <phone-ip> [seconds] [out.h264] [width] [height] [fps] [h264|h265] [mbps] [set-json]");
            System.exit(1);
        }
        String host = args[0];
        int port = 9610;
        if (host.contains(":")) {
            port = Integer.parseInt(host.substring(host.indexOf(':') + 1));
            host = host.substring(0, host.indexOf(':'));
        }
        int seconds = args.length > 1 ? Integer.parseInt(args[1]) : 10;
        String outPath = args.length > 2 ? args[2] : "probe.h264";
        int width = args.length > 3 ? Integer.parseInt(args[3]) : 1920;
        int height = args.length > 4 ? Integer.parseInt(args[4]) : 1080;
        int fps = args.length > 5 ? Integer.parseInt(args[5]) : 30;
        int codec = args.length > 6 && args[6].equals("h265") ? 0x68323635 : 0x68323634;
        int mbps = args.length > 7 ? Integer.parseInt(args[7]) : 10;
        String setJson = args.length > 8 ? args[8] : null;

        try (Socket socket = new Socket(); OutputStream file = new FileOutputStream(outPath)) {
            long connectStart = System.nanoTime();
            socket.connect(new InetSocketAddress(host, port), 3000);
            socket.setTcpNoDelay(true);
            socket.setSoTimeout(5000);
            out = socket.getOutputStream();

            out.write(ByteBuffer.allocate(28)
                    .putInt(MAGIC).putShort((short) 3).put((byte) 0).put((byte) 1) // 후면, CBR
                    .putInt(codec).putInt(width).putInt(height).putShort((short) fps).putShort((short) 0)
                    .putInt(mbps * 1_000_000).array());

            DataInputStream in = new DataInputStream(socket.getInputStream());
            authenticate(in);
            if (in.readInt() != MAGIC) throw new IOException("bad handshake magic");
            int version = in.readUnsignedShort();
            int videoCodec = in.readInt();
            in.readInt(); // audio codec
            int gotWidth = in.readInt();
            int gotHeight = in.readInt();
            int gotFps = in.readUnsignedShort();
            byte[] name = new byte[in.readUnsignedShort()];
            in.readFully(name);
            System.out.printf("handshake: v%d '%s' %dx%d@%d %s%n", version, new String(name, StandardCharsets.UTF_8),
                    gotWidth, gotHeight, gotFps, videoCodec == 0x68323635 ? "HEVC" : "H.264");

            long firstFrameNanos = -1;
            long start = System.nanoTime();
            long lastPing = 0;
            boolean setSent = setJson == null;
            long bytes = 0, latencySum = 0;
            int frames = 0, keyFrames = 0, configs = 0, latencyCount = 0;
            long firstPts = -1, lastPts = -1;
            while (System.nanoTime() - start < seconds * 1_000_000_000L) {
                if (System.nanoTime() - lastPing > 1_000_000_000L) {
                    lastPing = System.nanoTime();
                    pingSentUs = lastPing / 1000;
                    sendControl("{\"type\":\"ping\",\"id\":1}");
                }
                if (!setSent && System.nanoTime() - start > 3_000_000_000L) {
                    System.out.println("sending: " + setJson);
                    sendControl(setJson);
                    setSent = true;
                }

                int streamId = in.readUnsignedByte();
                int flags = in.readUnsignedByte();
                in.readUnsignedShort();
                int size = in.readInt();
                long pts = in.readLong();
                byte[] payload = new byte[size];
                in.readFully(payload);
                long nowUs = System.nanoTime() / 1000;

                if (streamId == 2) {
                    handleControl(new String(payload, StandardCharsets.UTF_8), nowUs);
                    continue;
                }
                file.write(payload);
                bytes += size;
                if ((flags & 1) != 0) {
                    configs++;
                    continue;
                }
                if (firstFrameNanos < 0) firstFrameNanos = System.nanoTime() - connectStart;
                if ((flags & 2) != 0) keyFrames++;
                if (firstPts < 0) firstPts = pts;
                lastPts = pts;
                frames++;
                if (bestRttUs != Long.MAX_VALUE) {
                    long latency = nowUs - (pts - clockOffsetUs);
                    if (latency > 0 && latency < 5_000_000) {
                        latencySum += latency;
                        latencyCount++;
                    }
                }
            }
            double elapsed = (System.nanoTime() - start) / 1e9;
            System.out.printf("received %d frames in %.1fs (%.1f fps, pts span %.1fs), %d keyframes, %d config packets%n",
                    frames, elapsed, frames / elapsed, (lastPts - firstPts) / 1e6, keyFrames, configs);
            System.out.printf("bitrate %.2f Mbps, first frame %d ms after connect, saved to %s%n",
                    bytes * 8 / elapsed / 1e6, firstFrameNanos / 1_000_000, outPath);
            if (latencyCount > 0) {
                System.out.printf("capture->receive latency avg %.1f ms (clock sync rtt %.1f ms)%n",
                        latencySum / (double) latencyCount / 1000, bestRttUs / 1000.0);
            }
        }
    }

    /** PC 승인 (PROTOCOL.md 1.1). 폰이 거절하면 이유를 출력하고 끝낸다. */
    private static void authenticate(DataInputStream in) throws IOException {
        String challenge = readControl(in);
        String phoneId = stringField(challenge, "phone_id");
        byte[] nonce = unhex(stringField(challenge, "nonce"));
        Path tokenFile = Path.of(System.getProperty("user.home"), ".phonecam-probe-tokens");
        Properties tokens = new Properties();
        if (tokenFile.toFile().exists()) {
            try (FileInputStream f = new FileInputStream(tokenFile.toFile())) {
                tokens.load(f);
            }
        }
        StringBuilder auth = new StringBuilder("{\"type\":\"auth\",\"pc_id\":\"phone-probe\",\"pc_name\":\"PhoneProbe\"");
        String token = tokens.getProperty(phoneId);
        if (token != null) auth.append(",\"proof\":\"").append(hex(hmac(unhex(token), nonce))).append('"');
        String code = System.getProperty("code");
        if (code != null) auth.append(",\"code\":\"").append(code).append('"');
        sendControl(auth.append('}').toString());

        String result = readControl(in);
        if (!result.contains("\"ok\":true")) {
            System.out.println("phone refused: " + result
                    + (result.contains("pair_required") ? " -> run again with -Dcode=<code shown on the phone>" : ""));
            System.exit(2);
        }
        String newToken = stringField(result, "token");
        if (newToken != null) {
            tokens.setProperty(phoneId, newToken);
            try (FileOutputStream f = new FileOutputStream(tokenFile.toFile())) {
                tokens.store(f, "PhoneProbe pairing tokens");
            }
            System.out.println("paired; token saved to " + tokenFile);
        }
    }

    private static String readControl(DataInputStream in) throws IOException {
        if (in.readUnsignedByte() != 2) throw new IOException("expected a control message (old phone app?)");
        in.readUnsignedByte();
        in.readUnsignedShort();
        byte[] payload = new byte[in.readInt()];
        in.readLong();
        in.readFully(payload);
        return new String(payload, StandardCharsets.UTF_8);
    }

    private static String stringField(String json, String key) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
        return m.find() ? m.group(1) : null;
    }

    private static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder();
        for (byte b : bytes) s.append(String.format("%02x", b));
        return s.toString();
    }

    private static byte[] unhex(String text) {
        byte[] out = new byte[text.length() / 2];
        for (int i = 0; i < out.length; i++) out[i] = (byte) Integer.parseInt(text.substring(2 * i, 2 * i + 2), 16);
        return out;
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

    private static void handleControl(String json, long nowUs) {
        if (json.contains("\"pong\"")) {
            Matcher m = LONG_FIELD.matcher(json);
            long clock = 0;
            while (m.find()) if (m.group(1).equals("clock_us")) clock = Long.parseLong(m.group(2));
            long rtt = nowUs - pingSentUs;
            if (rtt < bestRttUs) {
                bestRttUs = rtt;
                clockOffsetUs = clock - (pingSentUs + nowUs) / 2;
            }
            return;
        }
        System.out.println("control: " + json);
    }

    private static void sendControl(String json) throws IOException {
        byte[] payload = json.getBytes(StandardCharsets.UTF_8);
        out.write(ByteBuffer.allocate(16 + payload.length)
                .put((byte) 2).put((byte) 0).putShort((short) 0).putInt(payload.length).putLong(0).put(payload)
                .array());
    }
}
