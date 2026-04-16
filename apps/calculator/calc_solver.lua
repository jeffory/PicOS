-- calc_solver.lua — Equation solving algorithms
-- Pure Lua 5.4, no PicOS dependencies. Testable on host.

local Solver = {}

---------------------------------------------------------------------------
-- Linear system: 2 unknowns (Cramer's rule)
---------------------------------------------------------------------------

--- Solve a1*x + b1*y = c1, a2*x + b2*y = c2
-- coeffs = {{a1,b1},{a2,b2}}, consts = {c1,c2}
-- Returns {x, y} or nil, error
function Solver.solve_linear_2x2(coeffs, consts)
    local a1, b1 = coeffs[1][1], coeffs[1][2]
    local a2, b2 = coeffs[2][1], coeffs[2][2]
    local c1, c2 = consts[1], consts[2]

    local det = a1 * b2 - a2 * b1
    if math.abs(det) < 1e-14 then
        return nil, "No unique solution (singular system)"
    end

    local x = (c1 * b2 - c2 * b1) / det
    local y = (a1 * c2 - a2 * c1) / det
    return {x, y}
end

---------------------------------------------------------------------------
-- Linear system: 3 unknowns (Gaussian elimination with partial pivoting)
---------------------------------------------------------------------------

--- Solve 3x3 system.
-- coeffs = {{a11,a12,a13},{a21,a22,a23},{a31,a32,a33}}, consts = {b1,b2,b3}
-- Returns {x1, x2, x3} or nil, error
function Solver.solve_linear_3x3(coeffs, consts)
    -- Build augmented matrix
    local m = {}
    for i = 1, 3 do
        m[i] = {coeffs[i][1], coeffs[i][2], coeffs[i][3], consts[i]}
    end

    -- Forward elimination with partial pivoting
    for col = 1, 3 do
        -- Find pivot
        local max_val, max_row = math.abs(m[col][col]), col
        for row = col + 1, 3 do
            if math.abs(m[row][col]) > max_val then
                max_val = math.abs(m[row][col])
                max_row = row
            end
        end
        if max_val < 1e-14 then
            return nil, "No unique solution (singular system)"
        end
        -- Swap rows
        if max_row ~= col then
            m[col], m[max_row] = m[max_row], m[col]
        end
        -- Eliminate
        for row = col + 1, 3 do
            local factor = m[row][col] / m[col][col]
            for j = col, 4 do
                m[row][j] = m[row][j] - factor * m[col][j]
            end
        end
    end

    -- Back substitution
    local x = {0, 0, 0}
    for i = 3, 1, -1 do
        local sum = m[i][4]
        for j = i + 1, 3 do
            sum = sum - m[i][j] * x[j]
        end
        if math.abs(m[i][i]) < 1e-14 then
            return nil, "No unique solution (singular system)"
        end
        x[i] = sum / m[i][i]
    end
    return x
end

---------------------------------------------------------------------------
-- Quadratic: ax^2 + bx + c = 0
---------------------------------------------------------------------------

--- Returns table of roots. If Complex module provided, returns complex roots
--- for negative discriminant. Otherwise returns nil, error.
function Solver.solve_quadratic(a, b, c, Complex)
    if math.abs(a) < 1e-14 then
        -- Linear: bx + c = 0
        if math.abs(b) < 1e-14 then
            return nil, "Not an equation (a=0, b=0)"
        end
        return {-c / b}
    end

    local disc = b * b - 4 * a * c

    if disc >= 0 then
        local sd = math.sqrt(disc)
        return {(-b + sd) / (2 * a), (-b - sd) / (2 * a)}
    else
        -- Negative discriminant
        if not Complex then
            return nil, "No real roots (discriminant < 0)"
        end
        local real_part = -b / (2 * a)
        local imag_part = math.sqrt(-disc) / (2 * a)
        return {
            Complex.new(real_part, imag_part),
            Complex.new(real_part, -imag_part),
        }
    end
end

---------------------------------------------------------------------------
-- Cubic: ax^3 + bx^2 + cx + d = 0 (Cardano's method)
---------------------------------------------------------------------------

function Solver.solve_cubic(a, b, c, d, Complex)
    if math.abs(a) < 1e-14 then
        return Solver.solve_quadratic(b, c, d, Complex)
    end

    -- Normalize: x^3 + px + q = 0 via substitution x = t - b/(3a)
    local b_a = b / a
    local c_a = c / a
    local d_a = d / a

    local p = c_a - b_a * b_a / 3
    local q = d_a - b_a * c_a / 3 + 2 * b_a * b_a * b_a / 27

    local disc = q * q / 4 + p * p * p / 27
    local shift = -b_a / 3

    if disc > 1e-14 then
        -- One real root, two complex conjugate
        local sd = math.sqrt(disc)
        local u_arg = -q / 2 + sd
        local v_arg = -q / 2 - sd
        local u = u_arg >= 0 and u_arg ^ (1/3) or -((-u_arg) ^ (1/3))
        local v = v_arg >= 0 and v_arg ^ (1/3) or -((-v_arg) ^ (1/3))
        local x1 = u + v + shift

        if Complex then
            local re = -(u + v) / 2 + shift
            local im = (u - v) * math.sqrt(3) / 2
            return {x1, Complex.new(re, im), Complex.new(re, -im)}
        else
            return {x1}
        end
    elseif math.abs(disc) <= 1e-14 then
        -- All real, at least two equal
        local u
        if math.abs(q) < 1e-14 then
            return {shift, shift, shift}
        end
        u = q >= 0 and -((q / 2) ^ (1/3)) or (((-q) / 2) ^ (1/3))
        return {2 * u + shift, -u + shift, -u + shift}
    else
        -- Three distinct real roots (casus irreducibilis)
        local r = math.sqrt(-p * p * p / 27)
        local theta = math.acos(-q / (2 * r)) / 3
        local m = 2 * (r ^ (1/3))
        return {
            m * math.cos(theta) + shift,
            m * math.cos(theta + 2 * math.pi / 3) + shift,
            m * math.cos(theta + 4 * math.pi / 3) + shift,
        }
    end
end

---------------------------------------------------------------------------
-- Newton's method for f(x) = 0
---------------------------------------------------------------------------

--- Solve f(x) = 0 numerically.
-- engine: calc_engine instance (used to evaluate the expression)
-- expr: string expression in terms of x
-- x0: initial guess
-- tol: convergence tolerance (default 1e-10)
-- max_iter: max iterations (default 100)
function Solver.solve_newton(engine, expr, x0, tol, max_iter)
    tol = tol or 1e-10
    max_iter = max_iter or 100

    local x = x0
    local old_var = engine.variables["x"]

    for iter = 1, max_iter do
        -- Evaluate f(x)
        engine:register_variable("x", function() return x end)
        local fx, err = engine:evaluate(expr)
        if not fx then
            engine.variables["x"] = old_var
            return nil, "Evaluation error at x=" .. x .. ": " .. (err or "unknown")
        end

        if math.abs(fx) < tol then
            engine.variables["x"] = old_var
            return x
        end

        -- Numerical derivative: f'(x) ≈ (f(x+h) - f(x-h)) / (2h)
        local h = math.max(1e-8, math.abs(x) * 1e-8)

        engine:register_variable("x", function() return x + h end)
        local fxph, err2 = engine:evaluate(expr)
        if not fxph then
            engine.variables["x"] = old_var
            return nil, "Derivative evaluation error: " .. (err2 or "unknown")
        end

        engine:register_variable("x", function() return x - h end)
        local fxmh, err3 = engine:evaluate(expr)
        if not fxmh then
            engine.variables["x"] = old_var
            return nil, "Derivative evaluation error: " .. (err3 or "unknown")
        end

        local fpx = (fxph - fxmh) / (2 * h)
        if math.abs(fpx) < 1e-20 then
            engine.variables["x"] = old_var
            return nil, "Derivative near zero; method may not converge"
        end

        x = x - fx / fpx
    end

    engine.variables["x"] = old_var
    return nil, "Did not converge in " .. max_iter .. " iterations"
end

return Solver
