devices = reaper.MCULive_GetDevice(-1,0)
for i = 0, devices -1 do
  -- disable built-in stuff
  reaper.MCULive_SetDefault(0, false)
  -- disable buttons passthrough as MIDI
  for j = 0, 255 do reaper.MCULive_SetButtonPassthrough(0, j, false) end
end

function f()
    size = reaper.MCULive_GetMIDIMessage(0, 0)
    if (size > 0) then
        for i = 0, size - 1 do
            size, status, data1, data2, frame_offset, msg =
                reaper.MCULive_GetMIDIMessage(0, i)
            -- handle status, data1, data2, frame_offset
            -- check msg for long message
            -- size is size of buffer/queue currently available
            -- new buffer available at next defer cycle
        end
    end
    reaper.defer(f)
end
f()
function x() end
reaper.atexit(x)
