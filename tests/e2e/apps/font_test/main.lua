-- Font fixture. One page per font; Right advances, Esc exits.
-- Pages 0-3 are the byte-exact golden set for the renderer rewrite:
-- do not change what they draw without regenerating the goldens on the
-- ORIGINAL renderer.
local pc, disp, input = picocalc, picocalc.display, picocalc.input
local BLACK, WHITE = 0x0000, 0xFFFF
local YELLOW = disp.rgb(255, 255, 0)
local BLUE   = disp.rgb(0, 0, 255)
local SAMPLE = "The quick brown fox jumps over 13 lazy dogs."

local pages = {
  { id = disp.FONT_6X8,              name = "6x8" },
  { id = disp.FONT_8X12,             name = "8x12" },
  { id = disp.FONT_SCIENTIFICA,      name = "scientifica" },
  { id = disp.FONT_SCIENTIFICA_BOLD, name = "scientifica-bold" },
}

local function drawBuiltinPage(p)
  disp.clear(BLACK)
  disp.setFont(p.id)
  local fw, fh = disp.getFontWidth(), disp.getFontHeight()
  local y = 2
  disp.drawText(2, y, p.name .. " " .. fw .. "x" .. fh, WHITE, BLACK)
  y = y + fh + 2
  -- Printable ASCII, 32 per row
  for row = 0, 2 do
    local s = {}
    for c = 0x20 + row * 32, math.min(0x7E, 0x20 + row * 32 + 31) do
      s[#s + 1] = string.char(c)
    end
    disp.drawText(2, y, table.concat(s), WHITE, BLACK)
    y = y + fh
  end
  y = y + 4
  -- Opaque vs transparent over a blue bar
  disp.fillRect(0, y, 320, fh, BLUE)
  disp.drawText(2, y, "opaque", WHITE, BLACK)
  disp.drawText(120, y, "transparent", WHITE, false)
  y = y + fh + 4
  -- Clipped: only x 40..119 of the sample survives
  disp.setClipRect(40, y, 80, fh)
  disp.drawText(2, y, SAMPLE, YELLOW, BLACK)
  disp.clearClipRect()
  y = y + fh + 4
  -- Partly off-screen on both sides
  disp.drawText(-7, y, SAMPLE, WHITE, BLACK)
  disp.drawText(300, y + fh, SAMPLE, WHITE, BLACK)
  y = y + 2 * fh + 4
  local drawn = disp.drawText(2, y, SAMPLE, YELLOW, BLACK)
  disp.flush()
  pc.sys.log(("FT:WIDTH %s %d %d"):format(p.name, drawn, disp.textWidth(SAMPLE)))
end

local function drawPage(n)
  local p = pages[n + 1]
  if p.draw then p.draw(p) else drawBuiltinPage(p) end
  pc.sys.log(("FT:PAGE %d %s"):format(n, p.name))
end

local page = 0
drawPage(page)
while true do
  input.update()
  local pressed = input.getButtonsPressed()
  if (pressed & input.BTN_ESC) ~= 0 then break end
  if (pressed & input.BTN_RIGHT) ~= 0 then
    page = (page + 1) % #pages
    drawPage(page)
  end
  pc.sys.sleep(10)
end
disp.setFont(disp.FONT_6X8)
pc.sys.log("FT:DONE")
