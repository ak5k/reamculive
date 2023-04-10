# ReaMCULive
## REAPER Mackie Control Universal control surface extension for live use.
* Works with any MCU compatible controller.
* ReaScript API to interact with MCU device(s).
    * Build/customize your own implementation.
* OOTB modified MCU implementation to better suite live needs.
    * Supports multiple MCU base units and extenders.
    * 'Split modes' for sends, EQ, plugins ...
    * Rec arm buttos as fixed bank/page/layer shortcuts.
    * Assignable buttons via ReaScript API.
    * Sends as 'Sends on faders' to selected target track.
        * Mute enables/disables send to selected target track.
        * V-Pots as send pans.
    * Works nicely in tandem with ReaSolotus.
    * If track with name containing words 'mcu' and 'live' is found, Master fader gets attached to it.