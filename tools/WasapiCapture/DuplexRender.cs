using System.Runtime.InteropServices;

[ComImport, Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioRenderClient
{
    [PreserveSig] int GetBuffer(uint frames, out IntPtr data);
    [PreserveSig] int ReleaseBuffer(uint frames, uint flags);
}

internal static partial class Program
{
    private static int RunRenderOnly(string[] args)
    {
        if (args.Length is < 1 or > 3)
            throw new ArgumentException("--render-only RENDER_GUID [SECONDS] [exclusive]");
        double seconds = args.Length > 1
            ? double.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture)
            : 5.0;
        if (seconds <= 0) throw new ArgumentOutOfRangeException(nameof(seconds));
        bool exclusive = args.Length > 2 && args[2] == "exclusive";
        using var stop = new CancellationTokenSource(TimeSpan.FromSeconds(seconds));
        using var started = new ManualResetEventSlim();
        Render(args[0], stop.Token, started, exclusive);
        return 0;
    }

    // Independent shared RAW playback stream. Does not change Windows defaults or volume.
    private static void Render(string endpoint, CancellationToken stop, ManualResetEventSlim started, bool exclusive = false)
    {
        IMMDevice device = FindCaptureDevice(endpoint, EDataFlow.Render);
        Guid clientIid = typeof(IAudioClient).GUID;
        Check(device.Activate(ref clientIid, ClsctxAll, IntPtr.Zero, out object instance), "Render Activate");
        var client = (IAudioClient)instance;
        var properties = new AudioClientProperties {
            Size = (uint)Marshal.SizeOf<AudioClientProperties>(), Category = 8, Options = 1
        };
        IntPtr formatPointer;
        if (exclusive) formatPointer = PcmFormat(RequestedRate, RequestedBits);
        else {
            Check(((IAudioClient2)instance).SetClientProperties(ref properties), "Render RAW properties");
            Check(client.GetMixFormat(out formatPointer), "Render mix format");
        }
        bool running = false;
        try
        {
            WaveFormatEx fmt = Marshal.PtrToStructure<WaveFormatEx>(formatPointer);
            bool floating = fmt.FormatTag == 3 || (fmt.FormatTag == 0xfffe && Marshal.ReadInt32(formatPointer, 24) == 3);
            if (fmt.Channels != 2 || (fmt.BitsPerSample != 16 && fmt.BitsPerSample != 24 && fmt.BitsPerSample != 32))
                throw new InvalidOperationException("Render must be stereo PCM16/PCM24/PCM32 or float32");
            InitializeStream(device, ref instance, ref client, formatPointer, exclusive, "render");
            using var ready = new EventWaitHandle(false, EventResetMode.AutoReset);
            Check(client.SetEventHandle(ready.SafeWaitHandle.DangerousGetHandle()), "Render event");
            Check(client.GetBufferSize(out uint capacity), "Render buffer size");
            Guid renderIid = typeof(IAudioRenderClient).GUID;
            Check(client.GetService(ref renderIid, out object renderInstance), "Render GetService");
            var render = (IAudioRenderClient)renderInstance;
            var samples = new byte[capacity * fmt.BlockAlign];
            ulong sent = 0;
            uint emptyEvents = 0;
            void Fill(uint frames)
            {
                Check(render.GetBuffer(frames, out IntPtr data), "Render GetBuffer");
                for (uint f = 0; f < frames; ++f)
                {
                    for (uint c = 0; c < 2; ++c)
                    {
                        float v = (float)(0.125 * Math.Sin(2 * Math.PI * (c == 0 ? 401 : 601) * (sent + f) / fmt.SamplesPerSec));
                        int offset = checked((int)((2 * f + c) * (fmt.BitsPerSample / 8)));
                        if (fmt.BitsPerSample == 16) BitConverter.TryWriteBytes(samples.AsSpan(offset), (short)(v * 32767));
                        else if (fmt.BitsPerSample == 24) {
                            int packed = (int)(v * 8388608.0);
                            samples[offset] = (byte)packed;
                            samples[offset + 1] = (byte)(packed >> 8);
                            samples[offset + 2] = (byte)(packed >> 16);
                        }
                        else BitConverter.TryWriteBytes(samples.AsSpan(offset), floating ? BitConverter.SingleToInt32Bits(v) : (int)(v * 2147483648.0) & ~255);
                    }
                }
                Marshal.Copy(samples, 0, data, checked((int)frames * fmt.BlockAlign));
                Check(render.ReleaseBuffer(frames, 0), "Render ReleaseBuffer");
                sent += frames;
            }
            Fill(capacity);
            using var scheduling = new AudioScheduling();
            Check(client.Start(), "Render Start");
            running = true;
            started.Set();
            Console.WriteLine($"render started: capacity={capacity}frames rate={fmt.SamplesPerSec} bits={fmt.BitsPerSample} exclusive={exclusive} tones=401/601Hz peak=0.125");
            while (!stop.IsCancellationRequested)
            {
                if (!ready.WaitOne(1000))
                {
                    if (stop.IsCancellationRequested) break;
                    throw new TimeoutException("Render event stopped for 1 second");
                }
                if (exclusive) { Fill(capacity); continue; }
                Check(client.GetCurrentPadding(out uint padding), "Render padding");
                if (padding == 0) ++emptyEvents;
                if (padding < capacity) Fill(capacity - padding);
            }
            Console.WriteLine($"render submitted={sent}frames empty_engine_events={(exclusive ? "not-applicable-exclusive" : emptyEvents.ToString())}");
            Marshal.ReleaseComObject(renderInstance);
        }
        finally
        {
            if (running) client.Stop();
            Marshal.FreeCoTaskMem(formatPointer);
            Marshal.ReleaseComObject(instance);
            Marshal.ReleaseComObject(device);
        }
    }
}
