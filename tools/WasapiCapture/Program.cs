using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

internal enum EDataFlow
{
    Render,
    Capture,
    All,
}

[Flags]
internal enum DeviceState : uint
{
    Active = 0x00000001,
}

internal enum AudioClientShareMode
{
    Shared,
    Exclusive,
}

[Flags]
internal enum AudioClientStreamFlags : uint
{
    EventCallback = 0x00040000,
    NoPersist = 0x00080000,
}

[Flags]
internal enum AudioClientBufferFlags : uint
{
    DataDiscontinuity = 0x1,
    Silent = 0x2,
    TimestampError = 0x4,
}

[StructLayout(LayoutKind.Sequential, Pack = 2)]
internal struct WaveFormatEx
{
    public ushort FormatTag;
    public ushort Channels;
    public uint SamplesPerSec;
    public uint AvgBytesPerSec;
    public ushort BlockAlign;
    public ushort BitsPerSample;
    public ushort ExtraSize;
}

[ComImport]
[Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
internal sealed class MMDeviceEnumeratorComObject
{
}

[ComImport]
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMMDeviceEnumerator
{
    [PreserveSig]
    int EnumAudioEndpoints(EDataFlow dataFlow, DeviceState stateMask,
                           out IMMDeviceCollection devices);
    [PreserveSig]
    int GetDefaultAudioEndpoint(EDataFlow dataFlow, int role,
                                out IMMDevice endpoint);
    [PreserveSig]
    int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id,
                  out IMMDevice device);
    [PreserveSig]
    int RegisterEndpointNotificationCallback(IntPtr callback);
    [PreserveSig]
    int UnregisterEndpointNotificationCallback(IntPtr callback);
}

[ComImport]
[Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMMDeviceCollection
{
    [PreserveSig]
    int GetCount(out uint count);
    [PreserveSig]
    int Item(uint index, out IMMDevice device);
}

[ComImport]
[Guid("D666063F-1587-4E43-81F1-B948E807363F")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMMDevice
{
    [PreserveSig]
    int Activate(ref Guid iid, uint classContext, IntPtr activationParameters,
                 [MarshalAs(UnmanagedType.IUnknown)] out object instance);
    [PreserveSig]
    int OpenPropertyStore(uint access, out IntPtr properties);
    [PreserveSig]
    int GetId(out IntPtr id);
    [PreserveSig]
    int GetState(out DeviceState state);
}

[ComImport]
[Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioClient
{
    [PreserveSig]
    int Initialize(AudioClientShareMode shareMode,
                   AudioClientStreamFlags streamFlags,
                   long bufferDuration,
                   long periodicity,
                   IntPtr format,
                   ref Guid audioSessionGuid);
    [PreserveSig]
    int GetBufferSize(out uint bufferFrames);
    [PreserveSig]
    int GetStreamLatency(out long latency);
    [PreserveSig]
    int GetCurrentPadding(out uint paddingFrames);
    [PreserveSig]
    int IsFormatSupported(AudioClientShareMode shareMode, IntPtr format,
                          IntPtr closestMatch);
    [PreserveSig]
    int GetMixFormat(out IntPtr format);
    [PreserveSig]
    int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
    [PreserveSig]
    int Start();
    [PreserveSig]
    int Stop();
    [PreserveSig]
    int Reset();
    [PreserveSig]
    int SetEventHandle(IntPtr eventHandle);
    [PreserveSig]
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
}

[ComImport]
[Guid("726778CD-F60A-4EDA-82DE-E47610CD78AA")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioClient2
{
    [PreserveSig]
    int Initialize(AudioClientShareMode shareMode,
                   AudioClientStreamFlags streamFlags,
                   long bufferDuration,
                   long periodicity,
                   IntPtr format,
                   ref Guid audioSessionGuid);
    [PreserveSig]
    int GetBufferSize(out uint bufferFrames);
    [PreserveSig]
    int GetStreamLatency(out long latency);
    [PreserveSig]
    int GetCurrentPadding(out uint paddingFrames);
    [PreserveSig]
    int IsFormatSupported(AudioClientShareMode shareMode, IntPtr format,
                          out IntPtr closestMatch);
    [PreserveSig]
    int GetMixFormat(out IntPtr format);
    [PreserveSig]
    int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
    [PreserveSig]
    int Start();
    [PreserveSig]
    int Stop();
    [PreserveSig]
    int Reset();
    [PreserveSig]
    int SetEventHandle(IntPtr eventHandle);
    [PreserveSig]
    int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
    [PreserveSig] int IsOffloadCapable(int category, [MarshalAs(UnmanagedType.Bool)] out bool capable);
    [PreserveSig] int SetClientProperties(ref AudioClientProperties properties);
    [PreserveSig] int GetBufferSizeLimits(IntPtr format, [MarshalAs(UnmanagedType.Bool)] bool eventDriven,
                                        out long minimum, out long maximum);
}

[StructLayout(LayoutKind.Sequential)]
internal struct AudioClientProperties
{
    public uint Size;
    public int Offload;
    public int Category;
    public uint Options;
}

[ComImport]
[Guid("5CDF2C82-841E-4546-9722-0CF74078229A")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioEndpointVolume
{
    [PreserveSig]
    int RegisterControlChangeNotify(IntPtr notify);
    [PreserveSig]
    int UnregisterControlChangeNotify(IntPtr notify);
    [PreserveSig]
    int GetChannelCount(out uint channelCount);
    [PreserveSig]
    int SetMasterVolumeLevel(float levelDb, IntPtr eventContext);
    [PreserveSig]
    int SetMasterVolumeLevelScalar(float level, IntPtr eventContext);
    [PreserveSig]
    int GetMasterVolumeLevel(out float levelDb);
    [PreserveSig]
    int GetMasterVolumeLevelScalar(out float level);
    [PreserveSig]
    int SetChannelVolumeLevel(uint channel, float levelDb, IntPtr eventContext);
    [PreserveSig]
    int SetChannelVolumeLevelScalar(uint channel, float level,
                                    IntPtr eventContext);
    [PreserveSig]
    int GetChannelVolumeLevel(uint channel, out float levelDb);
    [PreserveSig]
    int GetChannelVolumeLevelScalar(uint channel, out float level);
}

[ComImport]
[Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IAudioCaptureClient
{
    [PreserveSig]
    int GetBuffer(out IntPtr data, out uint frames,
                  out AudioClientBufferFlags flags,
                  out ulong devicePosition, out ulong qpcPosition);
    [PreserveSig]
    int ReleaseBuffer(uint frames);
    [PreserveSig]
    int GetNextPacketSize(out uint frames);
}

internal static partial class Program
{
    private const uint ClsctxAll = 23;
    [MTAThread]
    private static int Main(string[] args)
    {
        try {
            args = ParseOptions(args);
            return args.Length > 0 && (args[0] == "--probe" || args[0] == "--probe-24-only")
                ? Probe(args.Skip(1).ToArray(), args[0] == "--probe-24-only") : Run(args);
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"{error.GetType().Name}: {error.Message} (0x{error.HResult:X8})");
            return 2;
        }
    }

    private static void Check(int hr, string operation)
    {
        if (hr < 0)
        {
            throw new COMException(operation, hr);
        }
    }

    private static string GetDeviceId(IMMDevice device)
    {
        Check(device.GetId(out IntPtr value), "IMMDevice.GetId failed");
        try
        {
            return Marshal.PtrToStringUni(value) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeCoTaskMem(value);
        }
    }

    private static IMMDevice FindCaptureDevice(string idFragment, EDataFlow flow = EDataFlow.Capture)
    {
        Type enumeratorType = Type.GetTypeFromCLSID(
            new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E"),
            throwOnError: true)!;
        var enumerator = (IMMDeviceEnumerator)Activator.CreateInstance(enumeratorType)!;
        string endpointGuid = idFragment.Trim('{', '}');
        if (Guid.TryParse(endpointGuid, out _))
        {
            string fullId = $"{{0.0.{(flow == EDataFlow.Capture ? 1 : 0)}.00000000}}.{{{endpointGuid}}}";
            int getResult = enumerator.GetDevice(fullId, out IMMDevice directDevice);
            if (getResult >= 0)
            {
                Console.WriteLine($"capture={fullId}");
                return directDevice;
            }
        }
        Check(enumerator.EnumAudioEndpoints(flow, DeviceState.Active,
                                            out IMMDeviceCollection devices),
              "EnumAudioEndpoints failed");
        Check(devices.GetCount(out uint count), "GetCount failed");
        for (uint index = 0; index < count; ++index)
        {
            Check(devices.Item(index, out IMMDevice device), "Item failed");
            string id = GetDeviceId(device);
            Console.WriteLine($"capture[{index}]={id}");
            if (id.Contains(idFragment, StringComparison.OrdinalIgnoreCase))
            {
                return device;
            }
        }
        throw new InvalidOperationException($"Active capture endpoint containing '{idFragment}' was not found");
    }

    private static void WriteWaveHeader(BinaryWriter writer, byte[] format, uint dataBytes)
    {
        uint fmtPad = (uint)(format.Length & 1);
        writer.Write("RIFF"u8.ToArray());
        writer.Write(4U + 8U + (uint)format.Length + fmtPad + 8U + dataBytes);
        writer.Write("WAVE"u8.ToArray());
        writer.Write("fmt "u8.ToArray());
        writer.Write((uint)format.Length);
        writer.Write(format);
        if (fmtPad != 0)
        {
            writer.Write((byte)0);
        }
        writer.Write("data"u8.ToArray());
        writer.Write(dataBytes);
    }

    [MTAThread]
    private static int Run(string[] args)
    {
        if (!OperatingSystem.IsWindows())
        {
            Console.Error.WriteLine("This diagnostic is Windows-only.");
            return 2;
        }

        string idFragment = args.Length > 0 ? args[0] : "e1cf265c-a7be-4f45-b45f-550fdb664d48";
        string output = args.Length > 1 ? args[1] : "seed3-p4-wasapi.wav";
        double seconds = args.Length > 2 ? double.Parse(args[2], System.Globalization.CultureInfo.InvariantCulture) : 10.0;

        IMMDevice device = FindCaptureDevice(idFragment);
        Console.WriteLine($"selected={GetDeviceId(device)}");

        Guid endpointVolumeIid = typeof(IAudioEndpointVolume).GUID;
        Check(device.Activate(ref endpointVolumeIid, ClsctxAll, IntPtr.Zero,
                              out object endpointVolumeObject),
              "IMMDevice.Activate(IAudioEndpointVolume) failed");
        var endpointVolume = (IAudioEndpointVolume)endpointVolumeObject;
        Check(endpointVolume.GetMasterVolumeLevel(out float endpointLevelDb),
              "GetMasterVolumeLevel failed");
        Check(endpointVolume.GetMasterVolumeLevelScalar(out float endpointLevel),
              "GetMasterVolumeLevelScalar failed");
        Console.WriteLine($"endpoint_volume={endpointLevel:F6} ({endpointLevelDb:F2} dB)");
        Check(endpointVolume.GetChannelCount(out uint endpointChannels),
              "GetChannelCount failed");
        for (uint channel = 0; channel < endpointChannels; ++channel)
        {
            Check(endpointVolume.GetChannelVolumeLevel(channel,
                                                        out float channelLevelDb),
                  "GetChannelVolumeLevel failed");
            Check(endpointVolume.GetChannelVolumeLevelScalar(channel,
                                                              out float channelLevel),
                  "GetChannelVolumeLevelScalar failed");
            Console.WriteLine($"endpoint_channel[{channel}]={channelLevel:F6} ({channelLevelDb:F2} dB)");
        }

        Guid audioClientIid = typeof(IAudioClient).GUID;
        Check(device.Activate(ref audioClientIid, ClsctxAll, IntPtr.Zero,
                              out object clientObject),
              "IMMDevice.Activate(IAudioClient) failed");
        var audioClient = (IAudioClient)clientObject;
        bool exclusive = args.Length > 3 && args[3] == "exclusive";
        if (!exclusive)
        {
            var properties = new AudioClientProperties {
                Size = (uint)Marshal.SizeOf<AudioClientProperties>(), Category = 8, Options = 1
            }; // AudioCategory_Media; AUDCLNT_STREAMOPTIONS_RAW, NOT RATEADJUST
            Check(((IAudioClient2)clientObject).SetClientProperties(ref properties),
                  "SetClientProperties(RAW) failed");
        }
        Check(audioClient.GetMixFormat(out IntPtr formatPointer), "GetMixFormat failed");

        if (exclusive)
        {
            Marshal.FreeCoTaskMem(formatPointer);
            formatPointer = PcmFormat(RequestedRate, RequestedBits);
        }
        try
        {
            WaveFormatEx format = Marshal.PtrToStructure<WaveFormatEx>(formatPointer);
            int formatBytes = 18 + format.ExtraSize;
            byte[] formatBlob = new byte[formatBytes];
            Marshal.Copy(formatPointer, formatBlob, 0, formatBytes);
            Console.WriteLine($"format=tag=0x{format.FormatTag:X4} channels={format.Channels} rate={format.SamplesPerSec} bits={format.BitsPerSample} block={format.BlockAlign} extra={format.ExtraSize}");

            InitializeStream(device, ref clientObject, ref audioClient, formatPointer, exclusive, "capture");
            Check(audioClient.GetBufferSize(out uint bufferFrames), "GetBufferSize failed");
            Check(audioClient.GetStreamLatency(out long latency100ns), "GetStreamLatency failed");
            Console.WriteLine($"engine_buffer={bufferFrames}frames latency={latency100ns / 10000.0:F3}ms");

            using var ready = new EventWaitHandle(false, EventResetMode.AutoReset);
            SafeWaitHandle handle = ready.SafeWaitHandle;
            Check(audioClient.SetEventHandle(handle.DangerousGetHandle()), "SetEventHandle failed");
            Guid captureIid = typeof(IAudioCaptureClient).GUID;
            Check(audioClient.GetService(ref captureIid, out object captureObject),
                  "GetService(IAudioCaptureClient) failed");
            var capture = (IAudioCaptureClient)captureObject;

            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
            using var file = File.Create(output);
            using var writer = new BinaryWriter(file);
            WriteWaveHeader(writer, formatBlob, 0);
            writer.Flush();
            ulong totalFrames = 0;
            uint discontinuities = 0;
            uint silentPackets = 0;
            uint initialDiscontinuities = 0;
            uint timestampErrors = 0;
            uint positionErrors = 0;
            ulong expectedPosition = 0;
            bool havePosition = false;
            ulong targetFrames = checked((ulong)Math.Round(seconds * format.SamplesPerSec));
            if (targetFrames == 0 || targetFrames * format.BlockAlign > 0x7fffffc7)
                throw new ArgumentOutOfRangeException(nameof(seconds));
            // Diagnostic recordings are bounded to <2GiB. Allocate/touch once,
            // before Start; never hold a WASAPI buffer across filesystem I/O.
            byte[] recording = new byte[checked((int)(targetFrames * format.BlockAlign))];
            for (long page = 0; page < recording.Length; page += 4096) recording[(int)page] = 0;
            var elapsed = System.Diagnostics.Stopwatch.StartNew();
            DateTime deadline = DateTime.UtcNow.AddSeconds(seconds + 10);

            using var stopRender = new CancellationTokenSource();
            using var renderStarted = new ManualResetEventSlim();
            Task? renderTask = null;
            if (args.Length > 4)
            {
                renderTask = Task.Run(() => Render(args[4], stopRender.Token, renderStarted, exclusive));
                if (!renderStarted.Wait(10000))
                {
                    stopRender.Cancel();
                    renderTask.GetAwaiter().GetResult();
                    throw new TimeoutException("Render did not start");
                }
                if (renderTask.IsFaulted) renderTask.GetAwaiter().GetResult();
            }
            using var scheduling = new AudioScheduling();
            Check(audioClient.Start(), "IAudioClient.Start failed");
            try
            {
                while (totalFrames < targetFrames && DateTime.UtcNow < deadline)
                {
                    if (!ready.WaitOne(1000)) continue;
                    while (totalFrames < targetFrames)
                    {
                        int hr = capture.GetBuffer(out IntPtr data, out uint frames,
                                                out AudioClientBufferFlags flags,
                                                out ulong position, out _);
                        if (hr == 0x08890001) break; // AUDCLNT_S_BUFFER_EMPTY
                        Check(hr, "IAudioCaptureClient.GetBuffer failed");
                        try
                        {
                            uint keptFrames = (uint)Math.Min(frames, targetFrames - totalFrames);
                            int bytes = checked((int)(keptFrames * format.BlockAlign));
                            int offset = checked((int)(totalFrames * format.BlockAlign));
                            if (havePosition && position != expectedPosition) ++positionErrors;
                            expectedPosition = position + frames;
                            havePosition = true;
                            if ((flags & AudioClientBufferFlags.DataDiscontinuity) != 0)
                            {
                                if (totalFrames == 0) ++initialDiscontinuities;
                                else
                                {
                                    ++discontinuities;
                                    if (discontinuities <= 20) Console.WriteLine($"discontinuity at written={totalFrames} device_position={position}");
                                }
                            }
                            if ((flags & AudioClientBufferFlags.Silent) != 0 || data == IntPtr.Zero)
                            {
                                Array.Clear(recording, offset, bytes);
                                ++silentPackets;
                            }
                            else
                            {
                                Marshal.Copy(data, recording, offset, bytes);
                            }
                            if ((flags & AudioClientBufferFlags.TimestampError) != 0) ++timestampErrors;
                            totalFrames += keptFrames;
                        }
                        finally
                        {
                            Check(capture.ReleaseBuffer(frames), "ReleaseBuffer failed");
                        }
                        // Exclusive event mode exposes one complete buffer per
                        // event. GetNextPacketSize is SHARED-mode-only per MSDN.
                        if (exclusive) break;
                    }
                }
            }
            finally
            {
                Check(audioClient.Stop(), "IAudioClient.Stop failed");
                stopRender.Cancel();
                renderTask?.GetAwaiter().GetResult();
            }

            long dataBytes = checked((long)totalFrames * format.BlockAlign);
            file.Write(recording, 0, checked((int)dataBytes));
            file.Position = 0;
            WriteWaveHeader(writer, formatBlob, (uint)dataBytes);
            Console.WriteLine($"captured={totalFrames} target={targetFrames} bytes={dataBytes} elapsed={elapsed.Elapsed.TotalSeconds:F3}s discontinuities={discontinuities} initial_discontinuities={initialDiscontinuities} timestamp_errors={timestampErrors} position_errors={positionErrors} silent_packets={silentPackets}");
            Console.WriteLine($"output={Path.GetFullPath(output)}");
            return totalFrames == targetFrames && discontinuities == 0 && timestampErrors == 0 && positionErrors == 0 ? 0 : 3;
        }
        finally
        {
            Marshal.FreeCoTaskMem(formatPointer);
        }
    }
}
