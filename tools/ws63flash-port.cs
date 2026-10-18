// tools/ws63flash-port.cs -- byte-for-byte copy of ws63-firmware/flasher/ws63flash.cs
// EXCEPT that the serial port and the log path come from argv (args[2], args[3]).
//
// WHY A COPY: the canonical flasher hard-codes port = "COM14" (its line 455) and the
// canonical file lives in ws63-firmware/flasher/, which is outside this task's inScope.
// The smoke test needs to flash a board that enumerates as COM15, so the port is passed
// in instead. Everything else (RTS reset dance, handshake, CRC16, YMODEM, -app image
// selection) is the proven code path verbatim -- do not "improve" it here.
// Build: C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /out:tools\ws63flash-port.exe tools\ws63flash-port.cs
// Keep in sync with ws63-firmware/flasher/ws63flash.cs; if that file changes, re-copy.

// WS63 custom flasher - speaks the ROM bootloader UART protocol directly.
// Replaces BurnTool (which needs a human to press RST).
//
// Protocol (extracted from SDK source bootloader/commonboot):
//   Stage 1: host spams a ROMBOOT handshake frame (type 0xF0, packet_size 0x12,
//            8-byte uart_param). ROM replies with 3 ACK frames.
//   Stage 2: ROM sends 'C' (0x43) asking for the loaderboot via YMODEM.
//   Stage 3: loaderboot boots, sends ACK + "Load Cmd Loop", then accepts
//            CMD_DL_IMAGE frames; each is followed by a YMODEM file transfer.
//   Stage 4: CMD_RESET.
//
// Frame layout (little-endian):  EF BE AD DE | size(2) | type | ~type | payload | crc16(2, low first)
//   size = 8 + payload_len + 2
// YMODEM packet: SOH/STX | blk | ~blk | data | crc16_hi | crc16_lo

using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Threading;

class Img
{
    public string Name = "";
    public uint Offset;
    public uint Size;
    public uint Addr;
    public uint Erase;
    public uint Efuse;
}

class Ws63Flash
{
    const byte Y_SOH = 0x01;
    const byte Y_STX = 0x02;
    const byte Y_EOT = 0x04;
    const byte Y_ACK = 0x06;
    const byte Y_C = 0x43;

    const byte T_ROMBOOT_HS = 0xF0;
    const byte T_ACK = 0xE1;
    const byte T_DL_IMAGE = 0xD2;
    const byte T_RESET = 0x87;

    static SerialPort sp;
    static List<byte> rx = new List<byte>();
    static int rxPos = 0;
    static readonly object rxLock = new object();
    static volatile bool rxRun = false;
    static StreamWriter logw;
    static int textCarry = 0;
    static int rxTotal = 0;

    // ---------------- logging ----------------
    static void Log(string s)
    {
        Console.WriteLine(s);
        if (logw != null) { try { logw.WriteLine(s); logw.Flush(); } catch { } }
    }

    static void LogRx(byte[] b, int off, int len)
    {
        StringBuilder hex = new StringBuilder();
        StringBuilder asc = new StringBuilder();
        for (int i = 0; i < len; i++)
        {
            byte v = b[off + i];
            hex.Append(v.ToString("X2")).Append(' ');
            asc.Append(v >= 0x20 && v < 0x7F ? (char)v : '.');
        }
        Log("  [RX " + len + "] " + hex.ToString().TrimEnd() + "   |" + asc.ToString() + "|");
    }

    // ---------------- CRC ----------------
    static ushort Crc16(byte[] b, int off, int len)
    {
        ushort crc = 0;
        for (int i = 0; i < len; i++)
        {
            crc ^= (ushort)(b[off + i] << 8);
            for (int j = 0; j < 8; j++)
            {
                if ((crc & 0x8000) != 0) crc = (ushort)((crc << 1) ^ 0x1021);
                else crc = (ushort)(crc << 1);
            }
        }
        return crc;
    }

    static byte[] Frame(byte type, byte[] payload)
    {
        int n = 8 + payload.Length + 2;
        byte[] f = new byte[n];
        f[0] = 0xEF; f[1] = 0xBE; f[2] = 0xAD; f[3] = 0xDE;
        f[4] = (byte)(n & 0xFF); f[5] = (byte)((n >> 8) & 0xFF);
        f[6] = type;
        f[7] = 0x00;   // verified: the ROM accepts pad=0 (HsTest); ~type is rejected
        Array.Copy(payload, 0, f, 8, payload.Length);
        ushort cs = Crc16(f, 0, n - 2);
        f[n - 2] = (byte)(cs & 0xFF);
        f[n - 1] = (byte)(cs >> 8);
        return f;
    }

    static byte[] HandshakePayload(uint baud)
    {
        byte[] p = new byte[8];
        p[0] = (byte)(baud & 0xFF);
        p[1] = (byte)((baud >> 8) & 0xFF);
        p[2] = (byte)((baud >> 16) & 0xFF);
        p[3] = (byte)((baud >> 24) & 0xFF);
        p[4] = 8;   // databit
        p[5] = 1;   // stopbit
        p[6] = 0;   // parity none
        p[7] = 0;   // flow none
        return p;
    }

    static byte[] DlImagePayload(Img im)
    {
        byte[] p = new byte[13];
        BitConverter.GetBytes(im.Addr).CopyTo(p, 0);
        BitConverter.GetBytes(im.Size).CopyTo(p, 4);
        BitConverter.GetBytes(im.Erase).CopyTo(p, 8);
        p[12] = (byte)im.Efuse;
        // uapi_sfc_reg_erase rejects any range whose end is not 4KB aligned, so round up.
        uint es = im.Erase;
        if (es != 0) { es = (es + 0xFFF) & 0xFFFFF000u; }
        p[8] = (byte)(es & 0xFF); p[9] = (byte)((es >> 8) & 0xFF);
        p[10] = (byte)((es >> 16) & 0xFF); p[11] = (byte)((es >> 24) & 0xFF);
        return p;
    }

    // ---------------- rx buffer ----------------
    static void RxThread()
    {
        byte[] tmp = new byte[8192];
        while (rxRun)
        {
            try
            {
                int n = sp.Read(tmp, 0, tmp.Length);
                if (n > 0)
                {
                    lock (rxLock) { for (int i = 0; i < n; i++) rx.Add(tmp[i]); rxTotal += n; }
                }
            }
            catch (TimeoutException) { }
            catch (Exception) { Thread.Sleep(5); }   // never exit: DiscardInBuffer can race a pending read
        }
    }

    static int Avail() { lock (rxLock) { return rx.Count - rxPos; } }

    static byte Get()
    {
        lock (rxLock) { return rx[rxPos++]; }
    }

    static void ClearRx() { lock (rxLock) { rx.Clear(); rxPos = 0; } }

    static void Compact()
    {
        lock (rxLock) { if (rxPos > 8192) { rx.RemoveRange(0, rxPos); rxPos = 0; } }
    }

    // Wait for a specific byte, logging any text seen on the way. Consumes up to and incl. 'want'.
    static bool WaitByte(byte want, int ms)
    {
        int t0 = Environment.TickCount;
        StringBuilder txt = new StringBuilder();
        while (Environment.TickCount - t0 < ms)
        {
            if (Avail() > 0)
            {
                byte b = Get();
                if (b == want) { FlushText(txt); return true; }
                if (b >= 0x20 && b < 0x7F) txt.Append((char)b);
                else
                {
                    if (b == 0x0D || b == 0x0A) { FlushText(txt); }
                    else { FlushText(txt); Log("  [rx raw] " + b.ToString("X2")); }
                }
            }
            else Thread.Sleep(2);
        }
        FlushText(txt);
        return false;
    }

    static void FlushText(StringBuilder txt)
    {
        if (txt.Length > 0) { Log("  [dev] " + txt.ToString()); txt.Length = 0; }
    }

    static StringBuilder pendingText = new StringBuilder();

    static void NoteSkipped(byte[] b, int off, int len)
    {
        for (int i = 0; i < len; i++)
        {
            byte v = b[off + i];
            if (v >= 0x20 && v < 0x7F) pendingText.Append((char)v);
            else if (v == 0x0D || v == 0x0A) pendingText.Append((char)v);
            else pendingText.Append("<").Append(v.ToString("X2")).Append(">");
        }
    }

    static void FlushPending()
    {
        if (pendingText.Length > 0) { Log("  [dev] " + pendingText.ToString().Replace("\r\n", "\\n")); pendingText.Length = 0; }
    }

    // Non-blocking scan for one complete CRC-valid frame. Skipped bytes go to pendingText.
    static bool TryFindFrame(out byte type, out byte[] payload)
    {
        type = 0; payload = null;
        Compact();
        int found = -1, fsize = 0;
        bool incomplete = false;
        lock (rxLock)
        {
            for (int i = rxPos; i + 8 <= rx.Count; i++)
            {
                if (rx[i] == 0xEF && rx[i + 1] == 0xBE && rx[i + 2] == 0xAD && rx[i + 3] == 0xDE)
                {
                    int size = rx[i + 4] | (rx[i + 5] << 8);
                    if (size >= 10 && size <= 1024)
                    {
                        if (i + size <= rx.Count) { found = i; fsize = size; }
                        else incomplete = true;
                        break;
                    }
                }
            }
        }
        if (found < 0)
        {
            if (!incomplete)
            {
                // no plausible frame start: keep only the last 3 bytes (possible partial magic)
                byte[] all;
                int keep;
                lock (rxLock)
                {
                    int n = rx.Count - rxPos;
                    if (n <= 3) return false;
                    keep = 3;
                    int drop = n - keep;
                    all = new byte[drop];
                    for (int i = 0; i < drop; i++) all[i] = rx[rxPos + i];
                    rxPos += drop;
                }
                NoteSkipped(all, 0, all.Length);
            }
            return false;
        }
        byte[] f = new byte[fsize];
        lock (rxLock)
        {
            for (int k = 0; k < fsize; k++) f[k] = rx[found + k];
        }
        ushort cs = Crc16(f, 0, fsize - 2);
        ushort fs = (ushort)(f[fsize - 2] | (f[fsize - 1] << 8));
        if (cs != fs) return false;
        byte[] pre = null;
        int preLen = found - rxPos;
        if (preLen > 0) { pre = new byte[preLen]; lock (rxLock) { for (int k = 0; k < preLen; k++) pre[k] = rx[rxPos + k]; } }
        lock (rxLock) { rxPos = found + fsize; }
        if (pre != null) NoteSkipped(pre, 0, pre.Length);
        type = f[6];
        payload = new byte[fsize - 10];
        Array.Copy(f, 8, payload, 0, fsize - 10);
        return true;
    }

    // Find a complete, CRC-valid frame. Skips/logs anything else.
    static bool FindFrame(int ms, out byte type, out byte[] payload)
    {
        type = 0; payload = null;
        int t0 = Environment.TickCount;
        List<byte> skip = new List<byte>();
        while (Environment.TickCount - t0 < ms)
        {
            Compact();
            int found = -1, fsize = 0;
            lock (rxLock)
            {
                for (int i = rxPos; i + 8 <= rx.Count; i++)
                {
                    if (rx[i] == 0xEF && rx[i + 1] == 0xBE && rx[i + 2] == 0xAD && rx[i + 3] == 0xDE)
                    {
                        int size = rx[i + 4] | (rx[i + 5] << 8);
                        if (size >= 10 && size <= 1024 && i + size <= rx.Count) { found = i; fsize = size; break; }
                    }
                }
            }
            if (found >= 0)
            {
                byte[] f = new byte[fsize];
                lock (rxLock) { for (int k = 0; k < fsize; k++) f[k] = rx[found + k]; }
                ushort cs = Crc16(f, 0, fsize - 2);
                ushort fs = (ushort)(f[fsize - 2] | (f[fsize - 1] << 8));
                if (cs == fs)
                {
                    // log skipped bytes
                    lock (rxLock)
                    {
                        for (int i = rxPos; i < found; i++) skip.Add(rx[i]);
                        rxPos = found + fsize;
                    }
                    if (skip.Count > 0) { LogRx(skip.ToArray(), 0, skip.Count); skip.Clear(); }
                    type = f[6];
                    payload = new byte[fsize - 10];
                    Array.Copy(f, 8, payload, 0, fsize - 10);
                    Log("  [frame] type=0x" + type.ToString("X2") + " size=" + fsize +
                        " payload=" + BitConverter.ToString(payload));
                    return true;
                }
            }
            Thread.Sleep(2);
        }
        lock (rxLock)
        {
            for (int i = rxPos; i < rx.Count; i++) skip.Add(rx[i]);
            rxPos = rx.Count;
        }
        if (skip.Count > 0) { LogRx(skip.ToArray(), 0, skip.Count); skip.Clear(); }
        return false;
    }

    // ---------------- YMODEM send ----------------
    static void SendYmodemPacket(byte hdr, byte blk, byte[] data, int dataLen)
    {
        byte[] pkt = new byte[3 + dataLen + 2];
        pkt[0] = hdr;
        pkt[1] = blk;
        pkt[2] = (byte)(0xFF - blk);
        Array.Copy(data, 0, pkt, 3, dataLen);
        ushort cs = Crc16(data, 0, dataLen);
        pkt[3 + dataLen] = (byte)(cs >> 8);     // high byte first
        pkt[4 + dataLen] = (byte)(cs & 0xFF);
        sp.Write(pkt, 0, pkt.Length);
    }

    static bool WaitAck(int ms)
    {
        int t0 = Environment.TickCount;
        while (Environment.TickCount - t0 < ms)
        {
            if (Avail() > 0)
            {
                byte b = Get();
                if (b == Y_ACK) return true;
                if (b == Y_C) continue;          // receiver still asking
                if (b == 0x15) return false;     // NAK
                if (b == 0x18) return false;     // CAN
                if (b >= 0x20 && b < 0x7F) Log("  [dev] " + (char)b);
                else Log("  [rx raw] " + b.ToString("X2"));
            }
            else Thread.Sleep(2);
        }
        return false;
    }

    // Send one file over YMODEM. Returns true when the receiver acked the final null packet.
    static bool YmodemSend(string name, byte[] data)
    {
        Log("  [ymodem] waiting for 'C' ...");
        if (!WaitByte(Y_C, 30000)) { Log("  [ymodem] FAIL: no 'C'"); return false; }

        byte[] hdr = new byte[128];
        byte[] nb = Encoding.ASCII.GetBytes(name);
        int nlen = Math.Min(nb.Length, 90);
        Array.Copy(nb, 0, hdr, 0, nlen);
        byte[] lb = Encoding.ASCII.GetBytes(data.Length.ToString());
        Array.Copy(lb, 0, hdr, nlen + 1, lb.Length);
        SendYmodemPacket(Y_SOH, 0x00, hdr, 128);
        Log("  [ymodem] sent header pkt: name=" + name + " len=" + data.Length);
        if (!WaitAck(15000)) { Log("  [ymodem] FAIL: no ACK for header"); return false; }
        Log("  [ymodem] header acked");

        int blk = 1;
        int off = 0;
        byte[] buf = new byte[1024];
        while (off < data.Length)
        {
            int n = Math.Min(1024, data.Length - off);
            Array.Copy(data, off, buf, 0, n);
            for (int i = n; i < 1024; i++) buf[i] = 0x1A;
            SendYmodemPacket(Y_STX, (byte)blk, buf, 1024);
            if (!WaitAck(20000))
            {
                Log("  [ymodem] FAIL: no ACK for block " + blk + " (off=" + off + ")");
                return false;
            }
            off += n;
            blk = (blk + 1) & 0xFF;
            if ((blk & 0x3F) == 0 || off >= data.Length)
                Log("  [ymodem] " + off + "/" + data.Length + " bytes");
        }

        // EOT
        sp.Write(new byte[] { Y_EOT }, 0, 1);
        Log("  [ymodem] sent EOT");
        if (!WaitByte(Y_C, 15000)) { Log("  [ymodem] FAIL: no 'C' after EOT"); return false; }
        byte[] zero = new byte[128];
        SendYmodemPacket(Y_SOH, 0x00, zero, 128);
        if (!WaitAck(15000)) { Log("  [ymodem] FAIL: no ACK for final pkt"); return false; }
        Log("  [ymodem] file transfer complete");
        return true;
    }

    // ---------------- fwpkg ----------------
    static List<Img> ParseFwpkg(string path, out byte[] raw)
    {
        raw = File.ReadAllBytes(path);
        uint magic = BitConverter.ToUInt32(raw, 0);
        ushort cnt = BitConverter.ToUInt16(raw, 6);
        List<Img> list = new List<Img>();
        Log("fwpkg magic=0x" + magic.ToString("X8") + " count=" + cnt + " size=" + raw.Length);
        for (int i = 0; i < cnt; i++)
        {
            int o = 12 + i * 52;
            Img im = new Img();
            int e = o;
            while (e < o + 32 && raw[e] != 0) e++;
            im.Name = Encoding.ASCII.GetString(raw, o, e - o);
            im.Offset = BitConverter.ToUInt32(raw, o + 32);
            im.Size = BitConverter.ToUInt32(raw, o + 36);
            im.Addr = BitConverter.ToUInt32(raw, o + 40);
            im.Erase = BitConverter.ToUInt32(raw, o + 44);
            im.Efuse = BitConverter.ToUInt32(raw, o + 48);
            list.Add(im);
            Log("  [" + i + "] " + im.Name.PadRight(30) + " off=0x" + im.Offset.ToString("X") +
                " size=0x" + im.Size.ToString("X") + " addr=0x" + im.Addr.ToString("X") +
                " erase=0x" + im.Erase.ToString("X") + " efuse=" + im.Efuse);
        }
        return list;
    }

    static byte[] Slice(byte[] raw, Img im)
    {
        byte[] b = new byte[im.Size];
        Array.Copy(raw, (int)im.Offset, b, 0, (int)im.Size);
        return b;
    }

    // ---------------- main ----------------
    static int Main(string[] args)
    {
        // [t81] 默认路径不再写死本机账户名：提交包自检会把 `C:\Users\<账户名>\…` 判成身份泄漏。
        // 默认值改成不含账户名的占位路径；优先级 argv > 环境变量 > 占位默认值（行为不变）。
        string pkgPath = args.Length > 0 ? args[0]
            : (Environment.GetEnvironmentVariable("WS63_FWPKG")
               ?? @"C:\ws63-tools\fw\ws63-liteos-app_all.fwpkg");
        string mode = args.Length > 1 ? args[1] : "app";
        // [tools/ws63flash-port] port + log path are parameters so a board on a
        // non-COM14 port can be flashed without touching ws63-firmware/flasher/.
        string port = args.Length > 2 ? args[2] : "COM14";
        string logPath = args.Length > 3 ? args[3]
            : (Environment.GetEnvironmentVariable("WS63_FLASH_LOG")
               ?? @"C:\ws63-tools\flasher-log.txt");
        int baud = 115200;

        logw = new StreamWriter(logPath, false);
        Log("=== ws63flash-port: port=" + port + " mode=" + mode + " pkg=" + pkgPath + " ===");

        byte[] raw;
        List<Img> imgs = ParseFwpkg(pkgPath, out raw);
        if (mode == "list") { logw.Close(); return 0; }

        sp = new SerialPort(port, baud, Parity.None, 8, StopBits.One);
        sp.Handshake = Handshake.None;
        sp.DtrEnable = false;
        sp.RtsEnable = false;
        sp.ReadTimeout = 50;
        sp.WriteTimeout = 5000;
        sp.Open();
        Log("opened " + port + " @" + baud);

        rxRun = true;
        Thread th = new Thread(new ThreadStart(RxThread));
        th.IsBackground = true;
        th.Start();

        // --- reset the board (mirrors the verified HsTest sequence exactly) ---
        Thread.Sleep(300);
        Log("baseline drain 700ms ...");
        Thread.Sleep(700);
        Log("asserting RTS (hold board in reset) 300ms ...");
        sp.RtsEnable = true;
        Thread.Sleep(300);
        try { sp.DiscardInBuffer(); } catch { }
        ClearRx();

        // --- stage 1: handshake (tight loop, no logging) ---
        byte[] hs = Frame(T_ROMBOOT_HS, HandshakePayload((uint)baud));
        Log("handshake packet = " + BitConverter.ToString(hs));
        int acks = 0;
        bool hsOk = false;
        int sent = 0;
        sp.RtsEnable = false;      // release -> board boots and the ROM listens
        Log("RTS released (rts=" + sp.RtsEnable + ")");
        DateTime t0 = DateTime.Now;
        while ((DateTime.Now - t0).TotalMilliseconds < 15000)
        {
            try { sp.Write(hs, 0, hs.Length); sent++; } catch { }
            Thread.Sleep(10);
            if (sent % 200 == 0) Log("  [" + (int)(DateTime.Now - t0).TotalMilliseconds + "ms] sent=" + sent + " rxTotal=" + rxTotal + " avail=" + Avail());
            byte ty; byte[] pl;
            while (TryFindFrame(out ty, out pl))
            {
                if (ty == T_ACK) { acks++; if (acks >= 3) { hsOk = true; break; } }
            }
            if (hsOk) break;
        }
        Log("handshake: sent=" + sent + " acks=" + acks);
        FlushPending();
        if (!hsOk)
        {
            Log("FAIL: handshake not accepted (acks=" + acks + ")  isOpen=" + sp.IsOpen + " rts=" + sp.RtsEnable + " dtr=" + sp.DtrEnable + " pending=" + pendingText.Length);
            Log("post-fail: releasing RTS and listening 6s to see if the board boots ...");
            sp.RtsEnable = false;
            Thread.Sleep(6000);
            FlushPending();
            Cleanup(); return 2;
        }
        Log("handshake OK (3 ACKs)");

        // --- stage 2: send loaderboot to the ROM ---
        Img lb = imgs[0];
        Log("stage 2: YMODEM loaderboot " + lb.Name + " (" + lb.Size + " bytes)");
        if (!YmodemSend(lb.Name, Slice(raw, lb))) { Log("FAIL: loaderboot transfer"); Cleanup(); return 3; }

        // --- stage 3: wait for loaderboot cmd loop ---
        Log("stage 3: waiting for loaderboot ...");
        byte t3; byte[] p3;
        if (FindFrame(20000, out t3, out p3))
            Log("  loaderboot: type=0x" + t3.ToString("X2") + " payload=" + BitConverter.ToString(p3));
        else Log("  (no frame from loaderboot)");
        WaitByte(0xFF, 1500);   // drain a bit

        // --- stage 4: flash requested images ---
        List<Img> todo = new List<Img>();
        for (int i = 0; i < imgs.Count; i++)
        {
            if (i == 0) continue;                       // loaderboot already sent to RAM
            if (mode == "all") todo.Add(imgs[i]);
            else if (mode == "app" && imgs[i].Name.ToLower().Contains("-app")) todo.Add(imgs[i]);
        }
        Log("stage 4: flashing " + todo.Count + " image(s)");
        foreach (Img im in todo)
        {
            Log("--- flashing " + im.Name + " addr=0x" + im.Addr.ToString("X") +
                " size=" + im.Size + " erase=0x" + im.Erase.ToString("X") + " (rounded to 4KB)");
            ClearRx();
            byte[] cmd = Frame(T_DL_IMAGE, DlImagePayload(im));
            sp.Write(cmd, 0, cmd.Length);
            byte ta; byte[] pa;
            if (!FindFrame(180000, out ta, out pa)) { Log("FAIL: no ACK for CMD_DL_IMAGE"); Cleanup(); return 4; }
            if (ta != T_ACK || pa.Length < 1 || pa[0] != 0x5A)
            { Log("FAIL: CMD_DL_IMAGE rejected type=0x" + ta.ToString("X2") + " payload=" + BitConverter.ToString(pa)); Cleanup(); return 4; }
            Log("  erase done, ACK ok");
            if (!YmodemSend(im.Name, Slice(raw, im))) { Log("FAIL: image transfer"); Cleanup(); return 5; }
            // wait for the post-download ACK frame
            byte tb; byte[] pb;
            if (FindFrame(15000, out tb, out pb)) Log("  post-download frame type=0x" + tb.ToString("X2") + " payload=" + BitConverter.ToString(pb));
            Log("  OK: " + im.Name);
        }

        // --- stage 5: reset ---
        Log("stage 5: CMD_RESET");
        ClearRx();
        byte[] rst = Frame(T_RESET, new byte[0]);
        sp.Write(rst, 0, rst.Length);
        FindFrame(5000, out t3, out p3);
        Thread.Sleep(1500);

        Log("=== DONE ===");
        Log("listening for 12s on " + port + " ...");
        int t4 = Environment.TickCount;
        while (Environment.TickCount - t4 < 12000)
        {
            if (Avail() > 0) { byte b = Get(); Log("  [boot] " + (b >= 0x20 && b < 0x7F ? ((char)b).ToString() : ("<" + b.ToString("X2") + ">"))); }
            else Thread.Sleep(2);
        }

        Cleanup();
        return 0;
    }

    static void Cleanup()
    {
        try { rxRun = false; } catch { }
        try { if (sp != null && sp.IsOpen) sp.Close(); } catch { }
        try { if (logw != null) logw.Close(); } catch { }
    }
}
