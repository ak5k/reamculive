devices = reaper.MCULive_GetDevice(-1,0)
for devId = 0, devices -1 do
  -- disable built-in stuff
  reaper.MCULive_SetDefault(devId, false)
  -- disable buttons passthrough as MIDI
  for j = 0, 255 do reaper.MCULive_SetButtonPassthrough(devId, j, false) end
end

function f()
    
    -- for each device
    for devId = 0, devices - 1 do
      -- get size (or length, or number) of available MIDI messages in buffer/queue
      size = reaper.MCULive_GetMIDIMessage(devId, -1)
      
      count = 0 -- count messages in current buffer, just for the sake of it
      
      -- here we pull MIDI messages from buffer until there are no more left
      -- new MIDI buffer available at next defer cycle
      while size > 0 do
        size, status, data1, data2, frame_offset, msg = reaper.MCULive_GetMIDIMessage(devId, 0)
        -- here-do-whatever
        -- handle status, data1, data2, frame_offset
        -- check msg for long message
        -- e.g. reaper.ShowConsoleMsg(devId .. '\t' .. count .. '\t' .. frame_offset.. '\t' .. status .. '\t' .. data1 .. '\t' .. data2 .. '\n')
        count = count + 1 -- increase counter
      end
      
      -- send some messages to MIDI devices
      reaper.MCULive_SendMIDIMessage(devId,0,0,0)
    end
    reaper.defer(f)
end

f()

function x() end
reaper.atexit(x)
