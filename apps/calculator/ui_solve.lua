-- ui_solve.lua — Equation solver tab UI
-- Requires PicOS display API (disp), used from main.lua

local Solver = require("calc_solver")

local UISolve = {}

-- Solver modes and their field definitions
local MODES = {
    {id = "quad",  label = "Quad",  fields = {"a", "b", "c"},                 desc = "ax^2+bx+c=0"},
    {id = "lin2",  label = "Lin2",  fields = {"a1","b1","c1","a2","b2","c2"}, desc = "2x2 linear system"},
    {id = "lin3",  label = "Lin3",  fields = {"a1","b1","c1","d1","a2","b2","c2","d2","a3","b3","c3","d3"}, desc = "3x3 linear system"},
    {id = "cubic", label = "Cubic", fields = {"a","b","c","d"},               desc = "ax^3+bx^2+cx+d=0"},
    {id = "newton",label = "f(x)=0",fields = {"expr","x0"},                  desc = "Newton's method"},
}

local mode_idx = 1
local field_values = {}  -- field name -> string
local active_field = 1
local results = nil       -- string or nil
local result_error = false

local function current_mode()
    return MODES[mode_idx]
end

local function reset_fields()
    field_values = {}
    for _, f in ipairs(current_mode().fields) do
        field_values[f] = ""
    end
    active_field = 1
    results = nil
    result_error = false
end

-- Initialize on first load
reset_fields()

---------------------------------------------------------------------------
-- Grid
---------------------------------------------------------------------------

function UISolve.get_grid()
    local m = current_mode()
    local rows = {}

    -- Row 1: mode buttons
    local mode_row = {}
    for i, md in ipairs(MODES) do
        local label = md.label
        if i == mode_idx then label = ">" .. label end
        mode_row[#mode_row + 1] = {label, "cmd:solve_mode" .. i}
    end
    -- Pad to 6 columns
    mode_row[#mode_row + 1] = {"Solve", "cmd:solve_run"}
    rows[#rows + 1] = mode_row

    -- Row 2: field labels (scrollable if >6)
    local field_row = {}
    local fields = m.fields
    local max_show = math.min(#fields, 6)
    for i = 1, max_show do
        local label = fields[i]
        if i == active_field then label = "[" .. label .. "]" end
        field_row[#field_row + 1] = {label, "cmd:solve_field" .. i}
    end
    while #field_row < 6 do
        field_row[#field_row + 1] = {"", "cmd:nop"}
    end
    rows[#rows + 1] = field_row

    -- Rows 3-6: number pad
    rows[#rows + 1] = {{"7","d:7"},{"8","d:8"},{"9","d:9"},{"/","ins:/"},{"(","ins:("},  {")","ins:)"}}
    rows[#rows + 1] = {{"4","d:4"},{"5","d:5"},{"6","d:6"},{"*","ins:*"},{"x","ins:x"},  {"^","ins:^"}}
    rows[#rows + 1] = {{"1","d:1"},{"2","d:2"},{"3","d:3"},{"-","ins:-"},{"<-","cmd:bs"}, {"CE","cmd:ce"}}
    rows[#rows + 1] = {{"+/-","cmd:neg"},{"0","d:0"},{".", "d:."},{"+","ins:+"},{"C","cmd:clear"},{"=","cmd:solve_run"}}

    return rows
end

---------------------------------------------------------------------------
-- Drawing
---------------------------------------------------------------------------

function UISolve.draw(disp_api, colors, layout)
    local DISPLAY_BG = colors.display_bg
    local EXPR_COL = colors.expr
    local RESULT_COL = colors.result
    local ERROR_COL = colors.error
    local DIM = colors.dim
    local ACCENT = colors.accent
    local FG = colors.fg

    local y = layout.display_top

    -- Background
    disp_api.fillRect(0, y, 320, layout.display_h, DISPLAY_BG)

    disp_api.setFont(0) -- 6x8

    local m = current_mode()

    -- Equation template
    disp_api.drawText(4, y + 2, m.desc, DIM, DISPLAY_BG)

    -- Field values (show active field highlighted)
    local fy = y + 12
    local fields = m.fields
    for i, f in ipairs(fields) do
        local val = field_values[f] or ""
        if val == "" then val = "_" end
        local col = (i == active_field) and ACCENT or EXPR_COL
        local text = f .. "=" .. val
        -- Layout: 2 columns if many fields
        local fx, fyr
        if #fields <= 4 then
            fx = 4
            fyr = fy + (i - 1) * 10
        else
            local col_idx = (i - 1) % 2
            local row_idx = math.floor((i - 1) / 2)
            fx = 4 + col_idx * 160
            fyr = fy + row_idx * 10
        end
        disp_api.drawText(fx, fyr, text, col, DISPLAY_BG)
    end

    -- Results
    if results then
        local ry = y + layout.display_h - 12
        local col = result_error and ERROR_COL or RESULT_COL
        disp_api.drawText(4, ry, results, col, DISPLAY_BG)
    end
end

---------------------------------------------------------------------------
-- Action handling
---------------------------------------------------------------------------

--- Handle solver-specific actions. Returns true if handled.
function UISolve.handle_action(action, expression)
    -- Mode switching
    local mode_match = action:match("^cmd:solve_mode(%d+)$")
    if mode_match then
        local idx = tonumber(mode_match)
        if idx >= 1 and idx <= #MODES then
            mode_idx = idx
            reset_fields()
        end
        return true, ""
    end

    -- Field switching
    local field_match = action:match("^cmd:solve_field(%d+)$")
    if field_match then
        local idx = tonumber(field_match)
        local m = current_mode()
        if idx >= 1 and idx <= #m.fields then
            -- Save current expression to current field
            local cur_field = m.fields[active_field]
            field_values[cur_field] = expression
            active_field = idx
            -- Return the new field's value as expression
            return true, field_values[m.fields[idx]] or ""
        end
        return true, expression
    end

    -- Solve
    if action == "cmd:solve_run" then
        -- Save current expression to active field
        local m = current_mode()
        field_values[m.fields[active_field]] = expression

        UISolve.solve(nil, false)  -- engine/complex passed later
        return true, expression
    end

    -- Field navigation (tab through fields on eval)
    if action == "cmd:eval" then
        local m = current_mode()
        field_values[m.fields[active_field]] = expression
        if active_field < #m.fields then
            active_field = active_field + 1
            return true, field_values[m.fields[active_field]] or ""
        else
            -- On last field, solve
            UISolve.solve(nil, false)
            return true, expression
        end
    end

    if action == "cmd:nop" then
        return true, expression
    end

    return false, expression
end

---------------------------------------------------------------------------
-- Solve with engine
---------------------------------------------------------------------------

function UISolve.solve(engine, complex_mode, Complex_mod)
    local m = current_mode()
    results = nil
    result_error = false

    local function num(name)
        local v = tonumber(field_values[name])
        if not v then
            results = "Invalid: " .. name
            result_error = true
            return nil
        end
        return v
    end

    if m.id == "quad" then
        local a, b, c = num("a"), num("b"), num("c")
        if not a then return end
        local C = complex_mode and Complex_mod or nil
        local roots, err = Solver.solve_quadratic(a, b, c, C)
        if not roots then
            results = err
            result_error = true
        else
            local parts = {}
            for _, r in ipairs(roots) do
                if type(r) == "table" and r.re ~= nil then
                    -- Complex root
                    parts[#parts + 1] = Complex_mod.format(r)
                else
                    parts[#parts + 1] = string.format("%.10g", r)
                end
            end
            results = "x=" .. table.concat(parts, ", ")
        end

    elseif m.id == "lin2" then
        local a1, b1, c1 = num("a1"), num("b1"), num("c1")
        local a2, b2, c2 = num("a2"), num("b2"), num("c2")
        if not a1 then return end
        local sol, err = Solver.solve_linear_2x2({{a1,b1},{a2,b2}}, {c1,c2})
        if not sol then
            results = err
            result_error = true
        else
            results = string.format("x=%.10g, y=%.10g", sol[1], sol[2])
        end

    elseif m.id == "lin3" then
        local a1,b1,c1,d1 = num("a1"),num("b1"),num("c1"),num("d1")
        local a2,b2,c2,d2 = num("a2"),num("b2"),num("c2"),num("d2")
        local a3,b3,c3,d3 = num("a3"),num("b3"),num("c3"),num("d3")
        if not a1 then return end
        local sol, err = Solver.solve_linear_3x3(
            {{a1,b1,c1},{a2,b2,c2},{a3,b3,c3}}, {d1,d2,d3}
        )
        if not sol then
            results = err
            result_error = true
        else
            results = string.format("x=%.6g y=%.6g z=%.6g", sol[1], sol[2], sol[3])
        end

    elseif m.id == "cubic" then
        local a, b, c, d = num("a"), num("b"), num("c"), num("d")
        if not a then return end
        local C = complex_mode and Complex_mod or nil
        local roots, err = Solver.solve_cubic(a, b, c, d, C)
        if not roots then
            results = err
            result_error = true
        else
            local parts = {}
            for _, r in ipairs(roots) do
                if type(r) == "table" and r.re ~= nil then
                    parts[#parts + 1] = Complex_mod.format(r)
                else
                    parts[#parts + 1] = string.format("%.10g", r)
                end
            end
            results = "x=" .. table.concat(parts, ", ")
        end

    elseif m.id == "newton" then
        local expr_str = field_values["expr"] or ""
        local x0 = num("x0")
        if not x0 then return end
        if expr_str == "" then
            results = "Enter expression"
            result_error = true
            return
        end
        if not engine then
            results = "Engine not available"
            result_error = true
            return
        end
        local root, err = Solver.solve_newton(engine, expr_str, x0)
        if not root then
            results = err
            result_error = true
        else
            results = string.format("x=%.10g", root)
        end
    end
end

--- External entry point for solving with an engine reference
function UISolve.solve_with_engine(engine, complex_mode, Complex_mod)
    UISolve.solve(engine, complex_mode, Complex_mod)
end

--- Get current field value for expression display
function UISolve.get_active_expression()
    local m = current_mode()
    return field_values[m.fields[active_field]] or ""
end

--- Set the active field value (from expression input)
function UISolve.set_active_expression(expr)
    local m = current_mode()
    field_values[m.fields[active_field]] = expr
end

return UISolve
