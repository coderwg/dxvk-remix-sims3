// The Sims 3 camera hook: the lamp reporter, a script mod.
//
// The hook (the patched d3d9.dll) sees only what the game draws, and the game never draws "this
// lamp is on". The game's scripts know every lamp exactly: a LightGameObject with its place and
// turn in the world, the keys of its object and of its model, whether it is on, its colour (a
// preset or a custom one) and its level. This mod reads those and hands them to the hook. It
// changes nothing in the game and keeps nothing in a save: its task is not persistable and its
// fields are plain statics.
//
// The channel (the technique of the S3IO project): a script cannot write files, but it can
// allocate unmanaged memory. The reporter allocates one block, marks its head with a signature
// and the block's own address, and rewrites the lamps near the camera a few times a second.
// The hook, in the same process, finds the block by the signature and reads it every frame.
//
// Block layout, version 2 (little endian):
//   0  'S3RX'   4  'LAMP'   8  'v002'   12  the block's own address
//   16 version (2)   20 sequence (odd while the records are being written)
//   24 lamps in the block   28 floats per record (24)   32 capacity   36 updates so far
//   40 world loaded (1) or not (0)   44 ints per record (8)
//   48 where the floats start   52 where the ints start   56..63 reserved
//   floats: record 0, the frame: camera target x y z, the radius asked, the game's intensity
//      levels dim, normal, bright, lamps found before the cap, then zeros;
//      then one record per lamp:
//        0..2  x y z    3..5  r g b    6 intensity    7 the engine's dimmer    8 on (1/0)
//        9 the colour preset (LightColor)    10 emits light (1/0)    11 floor level
//        12..23 the object's transform as three rows of four: row . (x, y, z, 1) carries a
//               point of the model into the world (right, up, at and position, by component)
//   ints: one record per lamp:
//        0,1 the object's id (low, high)    2,3 the instance of its catalog model key
//        4 that key's type    5,6 the instance of its resource key    7 that key's type
using System;
using System.Reflection;
using System.Runtime.InteropServices;
using Sims3.SimIFace;
using Sims3.Gameplay;
using Sims3.Gameplay.Abstracts;
using Sims3.UI;

// Without this the game does not read the assembly's tuning, never touches the class below, and
// the mod is loaded but never started (run 149: every working script mod carries it).
[assembly: Tunable]

namespace Sims3RtxHook
{
    public class LampReporter
    {
        [Tunable]
        protected static bool kInstantiator = false;

        const int kCapacity = 512;
        const int kFloats = 24;
        const int kInts = 8;
        const int kHead = 64;
        const int kFloatsAt = kHead;
        const int kIntsAt = kHead + (kCapacity + 1) * kFloats * 4;
        const int kBytes = kIntsAt + kCapacity * kInts * 4;
        const float kRadius = 120f;
        const uint kSleepTicks = 3;

        static IntPtr sBlock = IntPtr.Zero;
        static ObjectGuid sTask = ObjectGuid.InvalidObjectGuid;
        static float[] sFloats = new float[(kCapacity + 1) * kFloats];
        static int[] sInts = new int[kCapacity * kInts];
        static int sSequence = 0;
        static int sUpdates = 0;
        static float sDim = 0f, sNormal = 0f, sBright = 0f;
        static bool sSaid = false;

        static LampReporter()
        {
            World.OnWorldLoadFinishedEventHandler += new EventHandler(OnWorldLoadFinished);
            World.OnWorldQuitEventHandler += new EventHandler(OnWorldQuit);
        }

        static float Level(string name)
        {
            try
            {
                FieldInfo f = typeof(LightGameObject).GetField(name, BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
                if (f != null) return (float) f.GetValue(null);
            }
            catch (Exception) { }
            return 0f;
        }

        static void OnWorldLoadFinished(object sender, EventArgs e)
        {
            try
            {
                if (sBlock == IntPtr.Zero)
                {
                    IntPtr p = Marshal.AllocHGlobal(kBytes);
                    for (int i = 0; i < kHead; i += 4) Marshal.WriteInt32(p, i, 0);
                    Marshal.WriteInt32(p, 16, 2);
                    Marshal.WriteInt32(p, 28, kFloats);
                    Marshal.WriteInt32(p, 32, kCapacity);
                    Marshal.WriteInt32(p, 44, kInts);
                    Marshal.WriteInt32(p, 48, kFloatsAt);
                    Marshal.WriteInt32(p, 52, kIntsAt);
                    Marshal.WriteInt32(p, 12, p.ToInt32());
                    Marshal.WriteInt32(p, 8, 0x32303076);    // 'v002'
                    Marshal.WriteInt32(p, 4, 0x504D414C);    // 'LAMP'
                    Marshal.WriteInt32(p, 0, 0x58523353);    // 'S3RX', last: the head is whole when the signature stands
                    sBlock = p;
                }
                sDim = Level("kIntensityDim"); sNormal = Level("kIntensityNormal"); sBright = Level("kIntensityBright");
                Marshal.WriteInt32(sBlock, 40, 1);
                if (sTask == ObjectGuid.InvalidObjectGuid) sTask = Simulator.AddObject(new ReportTask());
            }
            catch (Exception) { }
        }

        static void OnWorldQuit(object sender, EventArgs e)
        {
            try
            {
                if (sTask != ObjectGuid.InvalidObjectGuid) { Simulator.DestroyObject(sTask); sTask = ObjectGuid.InvalidObjectGuid; }
                if (sBlock != IntPtr.Zero)
                {
                    Marshal.WriteInt32(sBlock, 20, ++sSequence);
                    Marshal.WriteInt32(sBlock, 24, 0);
                    Marshal.WriteInt32(sBlock, 40, 0);
                    Marshal.WriteInt32(sBlock, 20, ++sSequence);
                }
            }
            catch (Exception) { }
        }

        public static void Report()
        {
            if (sBlock == IntPtr.Zero) return;
            Vector3 cam = CameraController.GetTarget();
            LightGameObject[] lamps = Sims3.Gameplay.Queries.GetObjects<LightGameObject>(cam, kRadius);
            float[] f = sFloats; int[] d = sInts;
            int found = lamps == null ? 0 : lamps.Length;
            for (int i = 0; i < kFloats; ++i) f[i] = 0f;
            f[0] = cam.x; f[1] = cam.y; f[2] = cam.z; f[3] = kRadius;
            f[4] = sDim; f[5] = sNormal; f[6] = sBright; f[7] = (float) found;
            int n = 0;
            for (int i = 0; i < found && n < kCapacity; ++i)
            {
                LightGameObject lamp = lamps[i];
                if (lamp == null) continue;
                try
                {
                    if (!lamp.InWorld) continue;
                    ObjectGuid id = lamp.ObjectId;
                    Vector3 p = lamp.Position;
                    LightGameObject.LightColor c = lamp.Color;
                    float r = 1f, g = 1f, b = 1f;
                    if (c == LightGameObject.LightColor.CustomColor) { r = lamp.CustomColorRed; g = lamp.CustomColorGreen; b = lamp.CustomColorBlue; }
                    else if (c != LightGameObject.LightColor.Default) { Vector3 v = LightGameObject.GetColorVec(c); r = v.x; g = v.y; b = v.z; }
                    Matrix44 m = Objects.GetTransform(id);
                    ResourceKey model = lamp.CatalogModelKey;
                    ResourceKey key = lamp.GetResourceKey();
                    int o = (n + 1) * kFloats;
                    f[o] = p.x; f[o + 1] = p.y; f[o + 2] = p.z;
                    f[o + 3] = r; f[o + 4] = g; f[o + 5] = b;
                    f[o + 6] = lamp.Intensity;
                    f[o + 7] = World.LightGetDimmer(id);
                    f[o + 8] = lamp.IsLightOn() ? 1f : 0f;
                    f[o + 9] = (float) (int) c;
                    f[o + 10] = lamp.EmitsLight ? 1f : 0f;
                    f[o + 11] = (float) lamp.Level;
                    f[o + 12] = m.right.x; f[o + 13] = m.up.x; f[o + 14] = m.at.x; f[o + 15] = m.pos.x;
                    f[o + 16] = m.right.y; f[o + 17] = m.up.y; f[o + 18] = m.at.y; f[o + 19] = m.pos.y;
                    f[o + 20] = m.right.z; f[o + 21] = m.up.z; f[o + 22] = m.at.z; f[o + 23] = m.pos.z;
                    int q = n * kInts;
                    ulong v64 = id.Value;
                    d[q] = (int) (uint) (v64 & 0xFFFFFFFFul); d[q + 1] = (int) (uint) (v64 >> 32);
                    d[q + 2] = (int) (uint) (model.InstanceId & 0xFFFFFFFFul); d[q + 3] = (int) (uint) (model.InstanceId >> 32);
                    d[q + 4] = (int) model.TypeId;
                    d[q + 5] = (int) (uint) (key.InstanceId & 0xFFFFFFFFul); d[q + 6] = (int) (uint) (key.InstanceId >> 32);
                    d[q + 7] = (int) key.TypeId;
                    ++n;
                }
                catch (Exception) { }
            }
            Marshal.WriteInt32(sBlock, 20, ++sSequence);                 // odd: being written
            Marshal.Copy(f, 0, new IntPtr(sBlock.ToInt32() + kFloatsAt), (n + 1) * kFloats);
            if (n > 0) Marshal.Copy(d, 0, new IntPtr(sBlock.ToInt32() + kIntsAt), n * kInts);
            Marshal.WriteInt32(sBlock, 24, n);
            Marshal.WriteInt32(sBlock, 36, ++sUpdates);
            Marshal.WriteInt32(sBlock, 20, ++sSequence);                 // even: whole
            if (!sSaid && sUpdates >= 20)
            {
                // once per game session, a sign of life: the mod runs, and how many lamps it sees
                sSaid = true;
                try { StyledNotification.Show(new StyledNotification.Format("RTX lamp reporter: running, " + n + " lamps near the camera.", StyledNotification.NotificationStyle.kSystemMessage)); }
                catch (Exception) { }
            }
        }

        [Persistable(false)]
        public class ReportTask : Task
        {
            public override void Simulate()
            {
                while (true)
                {
                    try { LampReporter.Report(); }
                    catch (Exception) { }
                    Simulator.Sleep(kSleepTicks);   // outside the catch: the game ends a task through it
                }
            }
        }
    }
}
