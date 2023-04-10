devices = reaper.MCULive_GetDevice(-1,0)
for devId = 0, devices -1 do
  -- disable built-in stuff
  reaper.MCULive_SetDefault(devId, false)
  -- disable buttons passthrough as MIDI
  for j = 0, 255 do reaper.MCULive_SetButtonPassthrough(devId, j, false) end
end

function f()
    for devId = 0, devices -1 do
      size = reaper.MCULive_GetMIDIMessage(devId, 0)
      if (size > 0) then
        for msgIdx = 0, size - 1 do
            size, status, data1, data2, frame_offset, msg =
                reaper.MCULive_GetMIDIMessage(devId, msgIdx)
            -- handle status, data1, data2, frame_offset
            -- check msg for long message
            -- size is size of buffer/queue currently available
            -- new buffer available at next defer cycle
        end
      end
      -- send messages to MIDI devices
      reaper.MCULive_SendMIDIMessage(devId,0,0,0)
    end
    reaper.defer(f)
end
f()
function x() end
reaper.atexit(x)
