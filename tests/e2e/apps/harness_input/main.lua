-- Harness fixture: echo chars and Enter, present a frame every loop.
-- 'q' returns to the launcher.
local pc = picocalc
local input = pc.input
pc.sys.log("H:INPUT_READY")
local frame = 0
while true do
  input.update()
  local ch = input.getChar()
  if ch and ch ~= "" then
    pc.sys.log("H:CHAR " .. ch)
    if ch == "q" then return end
  end
  if (input.getButtonsPressed() & input.BTN_ENTER) ~= 0 then
    pc.sys.log("H:ENTER")
  end
  frame = frame + 1
  pc.display.clear(pc.display.BLACK)
  pc.display.drawText(8, 8, "frame " .. frame, pc.display.WHITE)
  pc.display.flush()
  pc.sys.sleep(10)
end
