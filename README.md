# ReaMCULive
## REAPER programmable ReaScript API control surface extension. Built-in extras for live MCU usage.(WIP).
* Works with any MIDI and MCU compatible controller.
* ReaScript API to interact with device(s).
    * Get and send MIDI messages to devices via API.
        * See MIDI input monitoring example script.
    * Build/customize your own perfect control surface implementation.
    * Default built-in behavior can be disabled (recommended for scripting).
* OOTB modified MCU implementation to better suite live needs (WIP).
    * Supports multiple MCU units and extenders.
    * Mostly fixed assignments for solid and safe workflow.
    * Works nicely in studio production too.
    * Works nicely in tandem with ReaSolotus.
        * To get discrete solo/monitoring bus.
    * 'Console split' modes for sends (EQ, inst & FXs WIP)
        * With multiple MCUs.
    * Rec arm buttos as fixed bank/page/layer shortcuts.
    * Assignable buttons via ReaScript API.
    * Sends as 'Sends on faders' to selected target track.
        * Mute enables/disables send to selected target track.
        * V-Pots as send pans.
    * Show/hide tracks from REAPER Mixer Control Panel (MCP).
    * If track with name containing words 'mcu' and 'live' is found, Master fader gets attached to it.

```
MCULive_GetButtonValue
MCULive_GetDevice        
MCULive_GetEncoderValue  
MCULive_GetFaderValue    
MCULive_GetMIDIMessage   
MCULive_Map    	         
MCULive_Reset    	       
MCULive_SendMIDIMessage  
MCULive_SetButtonPassthrough    	
MCULive_SetButtonPressOnly    	
MCULive_SetButtonValue   	
MCULive_SetDefault    	
MCULive_SetDisplay    	
MCULive_SetEncoderValue    	
MCULive_SetFaderValue    	
MCULive_SetMeterValue    	
MCULive_SetOption    	
```
