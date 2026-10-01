// The Sims 3 camera hook: the household's home lot in full detail in the map view.
//
// In the map view (the neighbourhood view) the game draws every lot as its stand-in, a low model
// with a small picture and a flat ground plate, except one: the "active lot" while the game's
// map-view switch is on (Camera.SetMapViewActiveLotMode, the engine's "allow high LOD"). The game
// itself uses the pair only for a moment, to have a lot loaded in full while the camera zooms in
// from the map to it (LiveModeState.WaitForLotLoad, PlayFlowModel). Otherwise the camera's own task
// (Camera.Simulate) sets the active lot to none once the camera is high above the ground.
//
// While the map view is open in live mode, this keeps the active household's home lot as the active
// lot with the game's override (Camera.SetActiveLotOverride, used by the game's play flow the same
// way) and turns the switch on. When the map view closes it clears the override, and turns the
// switch off again as soon as no game flow holds the active lot. It never acts while a game flow
// holds it (LotManager.ActiveLotLocked), and keeps nothing in a save: plain statics.
using System;
using Sims3.SimIFace;
using Sims3.Gameplay;
using Sims3.Gameplay.CAS;
using Sims3.Gameplay.Core;

namespace Sims3RtxHook
{
    public static class HomeLotInMapView
    {
        static Lot sPinned = null;       // the lot this keeps as the active lot, null when none
        static bool sSwitchOn = false;   // this turned the map-view switch on and still has to turn it off

        // A few times a second, from the reporter's task.
        public static void Update()
        {
            Lot home = null;
            if (GameStates.IsLiveState && CameraController.IsMapViewModeEnabled())
            {
                Household household = Household.ActiveHousehold;
                if (household != null) home = household.LotHome;
            }
            if (home != null)
            {
                if (home == sPinned || LotManager.ActiveLotLocked) return;
                Sims3.Gameplay.Core.Camera.SetMapViewActiveLotMode(true);
                sSwitchOn = true;
                Sims3.Gameplay.Core.Camera.SetActiveLotOverride(home.LotId);
                sPinned = home;
                return;
            }
            if (sPinned != null)
            {
                Sims3.Gameplay.Core.Camera.SetActiveLotOverride(0);
                sPinned = null;
            }
            if (sSwitchOn && !LotManager.ActiveLotLocked)
            {
                Sims3.Gameplay.Core.Camera.SetMapViewActiveLotMode(false);
                sSwitchOn = false;
            }
        }

        // When the world closes: undo both at once.
        public static void Release()
        {
            try { if (sPinned != null) Sims3.Gameplay.Core.Camera.SetActiveLotOverride(0); } catch (Exception) { }
            try { if (sSwitchOn) Sims3.Gameplay.Core.Camera.SetMapViewActiveLotMode(false); } catch (Exception) { }
            sPinned = null;
            sSwitchOn = false;
        }
    }
}
