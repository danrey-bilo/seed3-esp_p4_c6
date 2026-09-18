using System.Globalization;
using System.Runtime.InteropServices;

internal static partial class Program
{
    private static uint RequestedRate = 48000;
    private static ushort RequestedBits = 24;
    private static long RequestedPeriod = 200000; // legacy default 20 ms; --period min queries driver

    private static string[] ParseOptions(string[] args)
    {
        var positional = new List<string>();
        for (int i = 0; i < args.Length; ++i)
        {
            if (args[i] == "--rate") RequestedRate = uint.Parse(args[++i], CultureInfo.InvariantCulture);
            else if (args[i] == "--bits") RequestedBits = ushort.Parse(args[++i], CultureInfo.InvariantCulture);
            else if (args[i] == "--period")
            {
                string value = args[++i];
                RequestedPeriod = value == "min" ? 0 : checked((long)(double.Parse(value, CultureInfo.InvariantCulture) * 10000));
            }
            else positional.Add(args[i]);
        }
        if (RequestedBits != 16 && RequestedBits != 24) throw new ArgumentException("--bits must be 16 or 24");
        if (RequestedRate is not (44100 or 48000 or 88200 or 96000 or 176400 or 192000)) throw new ArgumentException("unsupported --rate");
        if (RequestedPeriod < 0) throw new ArgumentException("--period must be min or a positive number of milliseconds");
        return positional.ToArray();
    }

    private static IntPtr PcmFormat(uint rate, ushort validBits)
    {
        ushort containerBits = validBits == 16 ? (ushort)16 : (ushort)32;
        ushort block = (ushort)(containerBits / 8 * 2);
        byte[] pcm = new byte[40];
        BitConverter.GetBytes((ushort)0xfffe).CopyTo(pcm, 0);
        BitConverter.GetBytes((ushort)2).CopyTo(pcm, 2);
        BitConverter.GetBytes(rate).CopyTo(pcm, 4);
        BitConverter.GetBytes(rate * block).CopyTo(pcm, 8);
        BitConverter.GetBytes(block).CopyTo(pcm, 12);
        BitConverter.GetBytes(containerBits).CopyTo(pcm, 14);
        BitConverter.GetBytes((ushort)22).CopyTo(pcm, 16);
        BitConverter.GetBytes(validBits).CopyTo(pcm, 18);
        BitConverter.GetBytes(3u).CopyTo(pcm, 20);
        new Guid("00000001-0000-0010-8000-00aa00389b71").ToByteArray().CopyTo(pcm, 24);
        IntPtr pointer = Marshal.AllocCoTaskMem(pcm.Length);
        Marshal.Copy(pcm, 0, pointer, pcm.Length);
        return pointer;
    }

    private static void InitializeStream(IMMDevice device, ref object instance, ref IAudioClient client,
                                         IntPtr format, bool exclusive, string label)
    {
        Check(client.GetDevicePeriod(out long normal, out long minimum), "GetDevicePeriod");
        long period = RequestedPeriod == 0 ? minimum : RequestedPeriod;
        if (period <= 0) throw new InvalidOperationException("Driver did not report a usable minimum period");
        if (exclusive && RequestedPeriod == 0)
        {
            // At 44.1k a nominal 3ms period rounds DOWN to 132 frames in this
            // Windows USB driver, below its advertised minimum. Request
            // the first whole-frame duration >= the driver's minimum instead.
            uint rate = Marshal.PtrToStructure<WaveFormatEx>(format).SamplesPerSec;
            long frames = checked((minimum * rate + 9999999) / 10000000);
            period = checked((frames * 10000000 + rate - 1) / rate);
        }
        Console.WriteLine($"{label}_period default={normal / 10000.0:F3}ms minimum={minimum / 10000.0:F3}ms requested={period / 10000.0:F3}ms");
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            Guid session = Guid.Empty;
            int hr = client.Initialize(exclusive ? AudioClientShareMode.Exclusive : AudioClientShareMode.Shared,
                AudioClientStreamFlags.EventCallback | AudioClientStreamFlags.NoPersist,
                period, exclusive ? period : 0, format, ref session);
            if (hr == unchecked((int)0x88890019) && exclusive) // AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
            {
                Check(client.GetBufferSize(out uint alignedFrames), "GetBufferSize after alignment request");
                uint rate = Marshal.PtrToStructure<WaveFormatEx>(format).SamplesPerSec;
                period = (long)Math.Round(alignedFrames * 10000000.0 / rate);
                Marshal.ReleaseComObject(instance);
                Guid iid = typeof(IAudioClient).GUID;
                Check(device.Activate(ref iid, ClsctxAll, IntPtr.Zero, out instance), "Reactivate for aligned buffer");
                client = (IAudioClient)instance;
                Console.WriteLine($"{label}_alignment frames={alignedFrames} period={period / 10000.0:F6}ms");
                continue;
            }
            Check(hr, $"{label} Initialize");
            return;
        }
        throw new InvalidOperationException("Could not negotiate an aligned exclusive buffer");
    }

    private static int Probe(string[] endpoints)
    {
        if (endpoints.Length is < 1 or > 2) throw new ArgumentException("--probe CAPTURE_GUID [RENDER_GUID]");
        bool all = true;
        for (int direction = 0; direction < endpoints.Length; ++direction)
        {
            IMMDevice device = FindCaptureDevice(endpoints[direction], direction == 0 ? EDataFlow.Capture : EDataFlow.Render);
            Guid iid = typeof(IAudioClient).GUID;
            Check(device.Activate(ref iid, ClsctxAll, IntPtr.Zero, out object instance), "Probe Activate");
            try
            {
                var client = (IAudioClient)instance;
                Check(client.GetDevicePeriod(out long normal, out long minimum), "Probe period");
                Console.WriteLine($"PROBE direction={(direction == 0 ? "capture" : "playback")} default_ms={normal / 10000.0:F3} minimum_ms={minimum / 10000.0:F3}");
                foreach (uint rate in new uint[] {44100, 48000, 88200, 96000})
                foreach (ushort bits in new ushort[] {16, 24})
                {
                    IntPtr format = PcmFormat(rate, bits);
                    try
                    {
                        int hr = client.IsFormatSupported(AudioClientShareMode.Exclusive, format, IntPtr.Zero);
                        Console.WriteLine($"format rate={rate} valid_bits={bits} supported={hr == 0} hr=0x{hr:X8}");
                        all &= hr == 0;
                    }
                    finally { Marshal.FreeCoTaskMem(format); }
                }
            }
            finally { Marshal.ReleaseComObject(instance); Marshal.ReleaseComObject(device); }
        }
        return all ? 0 : 3;
    }

    private sealed class AudioScheduling : IDisposable
    {
        [DllImport("avrt.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr AvSetMmThreadCharacteristics(string name, out uint index);
        [DllImport("avrt.dll", SetLastError = true)]
        private static extern bool AvRevertMmThreadCharacteristics(IntPtr handle);
        private readonly IntPtr handle = AvSetMmThreadCharacteristics("Pro Audio", out _);
        public void Dispose() { if (handle != IntPtr.Zero) AvRevertMmThreadCharacteristics(handle); }
    }
}
