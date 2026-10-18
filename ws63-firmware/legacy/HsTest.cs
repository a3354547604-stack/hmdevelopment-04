using System;
using System.IO;
using System.IO.Ports;
using System.Threading;

class HsTest
{
    const uint START_FLAG = 0xDEADBEEF;
    const byte TYPE_ROMBOOT_HS = 0xF0;
    const ushort INTER_DATA_LEN = 0x12;

    static ushort Crc16Ccitt(byte[] buf, int off, int len)
    {
        ushort crc = 0;
        for (int i = 0; i < len; i++)
        {
            crc ^= (ushort)(buf[off + i] << 8);
            for (int b = 0; b < 8; b++)
            {
                if ((crc & 0x8000) != 0) crc = (ushort)((crc << 1) ^ 0x1021);
                else crc = (ushort)(crc << 1);
            }
        }
        return crc;
    }

    static byte[] MakeHandshake(uint baud)
    {
        byte[] p = new byte[INTER_DATA_LEN];
        p[0] = 0xEF; p[1] = 0xBE; p[2] = 0xAD; p[3] = 0xDE;
        p[4] = (byte)(INTER_DATA_LEN & 0xFF);
        p[5] = (byte)(INTER_DATA_LEN >> 8);
        p[6] = TYPE_ROMBOOT_HS;
        p[7] = 0x00;
        p[8] = (byte)(baud & 0xFF);
        p[9] = (byte)((baud >> 8) & 0xFF);
        p[10] = (byte)((baud >> 16) & 0xFF);
        p[11] = (byte)((baud >> 24) & 0xFF);
        p[12] = 8;   // databit
        p[13] = 1;   // stopbit
        p[14] = 0;   // parity none
        p[15] = 0;   // flow ctrl none
        ushort crc = Crc16Ccitt(p, 0, 16);
        p[16] = (byte)(crc & 0xFF);
        p[17] = (byte)(crc >> 8);
        return p;
    }

    static void HexDump(byte[] b, string tag)
    {
        Console.WriteLine("===== " + tag + " : " + b.Length + " bytes =====");
        for (int i = 0; i < b.Length; i += 16)
        {
            int n = Math.Min(16, b.Length - i);
            string hex = "", asc = "";
            for (int j = 0; j < n; j++)
            {
                byte v = b[i + j];
                hex += v.ToString("X2") + " ";
                asc += (v >= 32 && v < 127) ? (char)v : '.';
            }
            Console.WriteLine(string.Format("  {0:X4}  {1,-48} {2}", i, hex, asc));
        }
        Console.WriteLine("===== END =====");
    }

    static void Main(string[] args)
    {
        string portName = args.Length > 0 ? args[0] : "COM14";
        int baud = args.Length > 1 ? int.Parse(args[1]) : 115200;

        SerialPort sp = new SerialPort(portName, baud, Parity.None, 8, StopBits.One);
        sp.ReadTimeout = 300;
        sp.WriteTimeout = 1000;
        sp.DtrEnable = false;
        sp.RtsEnable = false;

        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] opening " + portName + " @" + baud);
        sp.Open();
        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] OPEN OK. DTR=" + sp.DtrEnable + " RTS=" + sp.RtsEnable);

        // baseline
        Thread.Sleep(300);
        sp.DiscardInBuffer();
        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] baseline drain 700ms...");
        byte[] base_ = Drain(sp, 700);
        Console.WriteLine("  baseline bytes = " + base_.Length);

        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] ASSERT RTS (hold board in reset) 300ms");
        sp.RtsEnable = true;
        Thread.Sleep(300);

        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] RELEASE RTS -> board boots; spamming handshake");
        sp.DiscardInBuffer();
        sp.RtsEnable = false;

        byte[] hs = MakeHandshake(115200);
        Console.WriteLine("  handshake packet = " + BitConverter.ToString(hs));
        Console.WriteLine("  crc16 = 0x" + Crc16Ccitt(hs, 0, 16).ToString("X4"));

        MemoryStream rx = new MemoryStream();
        DateTime t0 = DateTime.Now;
        int sent = 0;
        while ((DateTime.Now - t0).TotalMilliseconds < 6000)
        {
            try { sp.Write(hs, 0, hs.Length); sent++; } catch (Exception e) { Console.WriteLine("  write err " + e.Message); }
            byte[] chunk = Drain(sp, 15);
            rx.Write(chunk, 0, chunk.Length);
            if (rx.Length > 0 && sent % 50 == 0)
                Console.WriteLine("  [" + (DateTime.Now - t0).TotalMilliseconds.ToString("F0") + "ms] sent=" + sent + " rx=" + rx.Length);
        }
        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] sent " + sent + " packets, received " + rx.Length + " bytes");
        HexDump(rx.ToArray(), "RESPONSE");

        sp.Close();
        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] DONE");
    }

    static byte[] Drain(SerialPort sp, int ms)
    {
        MemoryStream ms2 = new MemoryStream();
        DateTime end = DateTime.Now.AddMilliseconds(ms);
        while (DateTime.Now < end)
        {
            try
            {
                int n = sp.BytesToRead;
                if (n > 0)
                {
                    byte[] b = new byte[n];
                    int r = sp.Read(b, 0, n);
                    ms2.Write(b, 0, r);
                }
                else Thread.Sleep(5);
            }
            catch { Thread.Sleep(5); }
        }
        return ms2.ToArray();
    }
}
