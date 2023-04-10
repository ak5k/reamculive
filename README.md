# ReaMCULive
## REAPER Mackie Control Universal control surface extension for live use.
* Works with any MCU compatible controller.
* Supports multiple MCU units and extenders.
* ReaScript API to interact with device(s).
    * Default behavior can be disabled (recommended).
    * Get and send MIDI messages also to non-MCU devices via API.
    * Build/customize your own implementation.
* OOTB modified MCU implementation to better suite live needs.
    * WIP
    * Mostly fixed assignments for solid and safe workflow.
    * Works nicely in studio production too.
    * Works nicely in tandem with ReaSolotus.
        * To get discrete solo/monitoring bus.
    * 'Console split' modes for sends, EQ, plugins ...
        * With multiple MCUs.
    * Rec arm buttos as fixed bank/page/layer shortcuts.
    * Assignable buttons via ReaScript API.
    * Sends as 'Sends on faders' to selected target track.
        * Mute enables/disables send to selected target track.
        * V-Pots as send pans.
    * Show/hide tracks from REAPER Mixer Control Panel (MCP).
    * If track with name containing words 'mcu' and 'live' is found, Master fader gets attached to it.

```
MCULive_GetButtonState   
MCULive_GetDevice        
MCULive_GetEncoderValue  
MCULive_GetFaderValue    
MCULive_GetMIDIMessage   
MCULive_Map    	         
MCULive_Reset    	       
MCULive_SendMIDIMessage  
MCULive_SetButtonPassthrough    	
MCULive_SetButtonPressOnly    	
MCULive_SetButtonState    	
MCULive_SetDefault    	
MCULive_SetDisplay    	
MCULive_SetEncoderValue    	
MCULive_SetFaderValue    	
MCULive_SetMeterValue    	
MCULive_SetOption    	
```