param(
    [Parameter(Mandatory = $true)]
    [uint32] $ProcessId,
    [int] $Samples = 10,
    [int] $IntervalMs = 200,
    [switch] $AllDevices
)

Add-Type @'
using System;
using System.Runtime.InteropServices;

[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
class MMDeviceEnumeratorComObject {}

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice endpoint);
}

[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
    int Activate(ref Guid iid, int clsCtx, IntPtr activationParams,
                 [MarshalAs(UnmanagedType.IUnknown)] out object instance);
    int OpenPropertyStore(int access, out IntPtr properties);
    int GetId(out IntPtr id);
}

[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection {
    int GetCount(out int count);
    int Item(int index, out IMMDevice device);
}

[ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2 {
    int GetAudioSessionControl(IntPtr guid, int flags, out IAudioSessionControl2 session);
    int GetSimpleAudioVolume(IntPtr guid, int flags, out IntPtr volume);
    int GetSessionEnumerator(out IAudioSessionEnumerator sessions);
    int RegisterSessionNotification(IntPtr notification);
    int UnregisterSessionNotification(IntPtr notification);
    int RegisterDuckNotification([MarshalAs(UnmanagedType.LPWStr)] string id, IntPtr notification);
    int UnregisterDuckNotification(IntPtr notification);
}

[ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator {
    int GetCount(out int count);
    int GetSession(int index, out IAudioSessionControl2 session);
}

[ComImport, Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2 {
    int GetState(out int state);
    int GetDisplayName(out IntPtr name);
    int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr context);
    int GetIconPath(out IntPtr path);
    int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string path, IntPtr context);
    int GetGroupingParam(out Guid groupingParam);
    int SetGroupingParam(ref Guid groupingParam, IntPtr context);
    int RegisterAudioSessionNotification(IntPtr notification);
    int UnregisterAudioSessionNotification(IntPtr notification);
    int GetSessionIdentifier(out IntPtr id);
    int GetSessionInstanceIdentifier(out IntPtr id);
    int GetProcessId(out uint processId);
    int IsSystemSoundsSession();
    int SetDuckingPreference(bool optOut);
}

[ComImport, Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface ISimpleAudioVolume {
    int SetMasterVolume(float level, IntPtr context);
    int GetMasterVolume(out float level);
    int SetMute(bool mute, IntPtr context);
    int GetMute(out bool mute);
}

[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation {
    int GetPeakValue(out float peak);
    int GetMeteringChannelCount(out int count);
    int GetChannelsPeakValues(int count, [Out] float[] peaks);
    int QueryHardwareSupport(out int mask);
}

public static class AudioSessionProbe {
    public static string Read(uint wantedProcessId, bool allDevices) {
        var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorComObject();
        var results = new System.Collections.Generic.List<string>();
        System.Collections.Generic.List<IMMDevice> devices =
            new System.Collections.Generic.List<IMMDevice>();
        if (allDevices) {
            IMMDeviceCollection collection;
            Marshal.ThrowExceptionForHR(
                enumerator.EnumAudioEndpoints(0, 1, out collection));
            int deviceCount;
            collection.GetCount(out deviceCount);
            for (int index = 0; index < deviceCount; index++) {
                IMMDevice device;
                if (collection.Item(index, out device) == 0)
                    devices.Add(device);
            }
        } else {
            IMMDevice device;
            Marshal.ThrowExceptionForHR(
                enumerator.GetDefaultAudioEndpoint(0, 1, out device));
            devices.Add(device);
        }

        foreach (IMMDevice device in devices) {
            IntPtr idPointer;
            string endpointId = "";
            if (device.GetId(out idPointer) == 0 && idPointer != IntPtr.Zero) {
                endpointId = Marshal.PtrToStringUni(idPointer);
                Marshal.FreeCoTaskMem(idPointer);
            }

            Guid iid = typeof(IAudioSessionManager2).GUID;
            object instance;
            if (device.Activate(ref iid, 1, IntPtr.Zero, out instance) != 0)
                continue;
            var manager = (IAudioSessionManager2)instance;
            IAudioSessionEnumerator sessions;
            if (manager.GetSessionEnumerator(out sessions) != 0)
                continue;
            int count;
            sessions.GetCount(out count);

            for (int index = 0; index < count; index++) {
                IAudioSessionControl2 session;
                if (sessions.GetSession(index, out session) != 0)
                    continue;
                uint processId;
                session.GetProcessId(out processId);
                if (processId != wantedProcessId)
                    continue;

                int state;
                session.GetState(out state);
                float volume = -1;
                float peak = -1;
                bool mute = false;
                try {
                    var simpleVolume = (ISimpleAudioVolume)session;
                    simpleVolume.GetMasterVolume(out volume);
                    simpleVolume.GetMute(out mute);
                } catch {
                }
                try {
                    var meter = (IAudioMeterInformation)session;
                    meter.GetPeakValue(out peak);
                } catch {
                }
                results.Add(string.Format(
                    "endpoint={0} pid={1} state={2} volume={3:F3} mute={4} peak={5:F6}",
                    endpointId, processId, state, volume, mute, peak));
            }
        }
        if (results.Count > 0) {
            return string.Join("; ", results.ToArray());
        }
        return string.Format("pid={0} state=absent", wantedProcessId);
    }
}
'@

for ($index = 0; $index -lt $Samples; $index++) {
    [AudioSessionProbe]::Read($ProcessId, [bool]$AllDevices)
    if ($index + 1 -lt $Samples) {
        Start-Sleep -Milliseconds $IntervalMs
    }
}
