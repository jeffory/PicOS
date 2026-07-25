-- Repeat Test — fixture for input.setRepeat / input.getButtonsRepeated.
--
-- getButtonsPressed() emits exactly one edge per physical press, so every grid
-- app hand-rolled a delay/rate timer. This checks the built-in version: while
-- RIGHT is held, repeated edges must appear after the delay and then at the
-- configured rate, while getButtonsPressed() still reports only the one edge.
--
-- Logs a running count of both so the harness can compare them.

local pc    = picocalc
local input = pc.input

local DELAY, RATE = 200, 50
input.setRepeat(DELAY, RATE)

pc.sys.log(("RP:CONFIG delay=%d rate=%d"):format(DELAY, RATE))
pc.sys.log(("RP:API setRepeat=%s getButtonsRepeated=%s")
    :format(type(input.setRepeat), type(input.getButtonsRepeated)))

local pressed_count  = 0   -- from getButtonsPressed(): expected to stay at 1
local repeated_count = 0   -- from getButtonsRepeated(): expected to grow
local first_repeat_ms = nil
local press_ms = nil

pc.sys.log("RP:READY")

while true do
    input.update()

    local pressed  = input.getButtonsPressed()
    -- Must be called at most once per frame: it advances the repeat clocks.
    local repeated = input.getButtonsRepeated()

    if pressed & input.BTN_ESC ~= 0 then
        pc.sys.log(("RP:RESULT pressed=%d repeated=%d delay_measured=%s")
            :format(pressed_count, repeated_count,
                    (first_repeat_ms and press_ms)
                        and tostring(first_repeat_ms - press_ms) or "nil"))
        pc.sys.log("RP:DONE")
        return
    end

    if pressed & input.BTN_RIGHT ~= 0 then
        pressed_count = pressed_count + 1
        press_ms = pc.sys.getTimeMs()
        pc.sys.log(("RP:PRESS n=%d"):format(pressed_count))
    end

    if repeated & input.BTN_RIGHT ~= 0 then
        repeated_count = repeated_count + 1
        -- The first repeated edge coincides with the press edge; the SECOND is
        -- the first synthetic repeat, so that is the one timed against the delay.
        if repeated_count == 2 then
            first_repeat_ms = pc.sys.getTimeMs()
        end
        pc.sys.log(("RP:REPEAT n=%d"):format(repeated_count))
    end

    pc.sys.sleep(16)
end
