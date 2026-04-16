# Calculator App - Missing Features TODO

Comparison against the Casio fx-CG100 ClassWiz Color Graph calculator.
Features are grouped by category and roughly ordered by implementation complexity.
Only includes features feasible on PicOS (320x320 LCD, Lua 5.4, embedded constraints).

---

## Mathematical Functions

### Currently Missing from calc_functions.lua
- [ ] GCD (greatest common divisor)
- [ ] LCM (least common multiple)
- [ ] Integer quotient (integer division)
- [ ] Prime factorization display
- [ ] Frac() - fractional part extraction
- [ ] Int() - integer part extraction
- [ ] atan2(y,x) - two-argument arctangent
- [ ] Summation: sum(expr, var, start, end)
- [ ] Product: prod(expr, var, start, end)
- [ ] Numerical differentiation: deriv(expr, x, point)
- [ ] Numerical integration: integ(expr, x, a, b)
- [ ] Rectangular-to-polar coordinate conversion
- [ ] Polar-to-rectangular coordinate conversion
- [ ] Random number generation: rand(), randint(a,b)
- [ ] DMS (degrees-minutes-seconds) input/output

---

## Complex Number Support (New Mode/Tab)

- [ ] Complex number literal parsing (a+bi form)
- [ ] Complex arithmetic (+, -, *, /, ^)
- [ ] Complex display toggle: rectangular (a+bi) vs polar (r,theta)
- [ ] abs() / modulus for complex numbers
- [ ] arg() - argument/angle
- [ ] conj() - conjugate
- [ ] real() - real part extraction
- [ ] imag() - imaginary part extraction

---

## Equation Solver (New Tab)

### Simultaneous Equations
- [ ] 2-unknown linear system solver
- [ ] 3-unknown linear system solver
- [ ] 4-unknown linear system solver (stretch)

### Polynomial Solver
- [ ] Quadratic equation solver (ax^2+bx+c=0) with real/complex roots
- [ ] Cubic equation solver
- [ ] Quartic equation solver (stretch)

### Numerical Solver
- [ ] Newton's method solver for arbitrary f(x)=0
- [ ] User-configurable initial guess and tolerance

---

## Matrix & Vector Operations (New Tab)

### Matrix Entry & Display
- [ ] Matrix editor UI (grid input)
- [ ] Matrix variable storage (at least A-F)
- [ ] Matrix display formatting

### Matrix Operations
- [ ] Addition, subtraction, scalar multiplication
- [ ] Matrix multiplication
- [ ] Determinant
- [ ] Inverse
- [ ] Transpose
- [ ] Identity matrix generation
- [ ] Row echelon form (REF)
- [ ] Reduced row echelon form (RREF)

### Vector Operations
- [ ] Dot product
- [ ] Cross product
- [ ] Magnitude / norm
- [ ] Unit vector

---

## Graphing (New Tab or Separate App)

### Basic Graphing
- [ ] y=f(x) function input and plotting
- [ ] Graph viewport (Xmin, Xmax, Ymin, Ymax, scale)
- [ ] Axis drawing with grid lines and tick marks
- [ ] Pixel-level function rendering on 320x320 display
- [ ] Multiple simultaneous functions (color-coded)
- [ ] Pan/scroll with arrow keys
- [ ] Zoom in / zoom out

### Graph Types
- [ ] Polar graphs: r=f(theta)
- [ ] Parametric graphs: x(t), y(t)
- [ ] Inequality graphing with shading (y>, y<, etc.)

### Graph Analysis (G-Solve)
- [ ] Trace mode: cursor follows curve, displays (x,y)
- [ ] Find roots (x-intercepts / zeros)
- [ ] Find local minimum
- [ ] Find local maximum
- [ ] Find intersection of two curves
- [ ] Definite integral visualization (shaded area)
- [ ] Tangent line at a point
- [ ] Normal line at a point

### Table Generation
- [ ] Auto-generate x/y value table from function expression
- [ ] User-defined start, end, step values
- [ ] Tab to switch between graph view and table view

---

## Statistics Enhancements

### Data Entry Improvements
- [ ] Multi-list data entry (separate x and y lists)
- [ ] Edit/modify individual data points (not just delete last)
- [ ] Named dataset storage

### Two-Variable Statistics (Engine Exists, UI Missing)
- [ ] Expose bivariate data entry UI (x,y pairs)
- [ ] Display regression equation, r, r^2

### Additional Regression Types
- [ ] Quadratic regression (ax^2+bx+c)
- [ ] Cubic regression
- [ ] Logarithmic regression (a+b*ln(x))
- [ ] Exponential regression (a*e^(bx))
- [ ] Power regression (a*x^b)

### Statistical Plots
- [ ] Scatter plot
- [ ] Histogram
- [ ] Box-and-whisker plot

---

## Probability Distributions (New Tab or Sub-Mode)

### Continuous Distributions
- [ ] Normal distribution: PDF, CDF, inverse CDF
- [ ] Student-t distribution: PDF, CDF
- [ ] Chi-square distribution: PDF, CDF
- [ ] F distribution: PDF, CDF

### Discrete Distributions
- [ ] Binomial: P(X=k), P(X<=k)
- [ ] Poisson: P(X=k), P(X<=k)
- [ ] Geometric distribution

---

## Financial Calculations (New Tab)

### Time Value of Money
- [ ] Compound interest solver (n, I%, PV, PMT, FV)
- [ ] Simple interest calculations

### Cash Flow Analysis
- [ ] Net Present Value (NPV)
- [ ] Internal Rate of Return (IRR)

### Amortization
- [ ] Loan payment schedule (principal vs interest breakdown)

### Other Financial
- [ ] Cost / Sell / Margin calculator
- [ ] Nominal-to-effective interest rate conversion
- [ ] Depreciation: straight-line, declining balance

---

## Base-N Enhancements

### Missing Bitwise Operations
- [ ] XNOR (exclusive NOR)
- [ ] NEG (two's complement negation)
- [ ] Configurable bit width display (8/16/32/64 bit)

### Display Improvements
- [ ] Grouped binary display (e.g., 1010 0110 instead of 10100110)
- [ ] Grouped hex display for long values

---

## Number Display & Formatting

### Natural Textbook Display
- [ ] Fraction display (stacked numerator/denominator)
- [ ] Root symbol rendering (radical with vinculum)
- [ ] Exponent superscript rendering
- [ ] Mixed number display (e.g., 2 1/3)

### Format Conversions
- [ ] Decimal-to-fraction conversion (e.g., 0.333... -> 1/3)
- [ ] Fraction-to-decimal toggle
- [ ] Improper fraction <-> mixed number toggle
- [ ] Fixed decimal places mode (FIX n)
- [ ] Scientific notation mode (SCI n)
- [ ] Engineering notation mode (ENG)

---

## Variable Storage

- [ ] Named variables A-Z (store/recall beyond single M)
- [ ] Variable list display showing all stored values
- [ ] Use variables in expressions (e.g., "2*A+B")
- [ ] Previous answer (PreAns) in addition to ANS

---

## Constants Library

### Physical Constants (Currently Only pi, e, phi)
- [ ] Speed of light (c)
- [ ] Gravitational constant (G)
- [ ] Planck constant (h)
- [ ] Boltzmann constant (k)
- [ ] Avogadro number (Na)
- [ ] Elementary charge (e)
- [ ] Electron mass (me)
- [ ] Proton mass (mp)
- [ ] Gravitational acceleration (g)
- [ ] Browsable constants picker UI

### Unit Conversions
- [ ] Length (m, ft, in, cm, km, mi)
- [ ] Mass (kg, lb, oz, g)
- [ ] Temperature (C, F, K)
- [ ] Volume (L, gal, mL)
- [ ] Energy (J, cal, eV, kWh)
- [ ] Pressure (Pa, atm, psi, bar)

---

## UI/UX Improvements

### Expression Editing
- [ ] Cursor positioning within expression (left/right arrows)
- [ ] Insert mode (add characters at cursor, not just append)
- [ ] Multi-line expression display for complex formulas
- [ ] Syntax highlighting (operators, functions, numbers in different colors)

### History Improvements
- [ ] Recall and re-edit past expressions (not just view result)
- [ ] Copy result to clipboard / insert into current expression
- [ ] Search/filter history

### Accessibility
- [ ] Larger font option / zoom mode
- [ ] Key repeat for held buttons

---

## Conic Sections (Stretch Goal)

- [ ] Parabola plotting from equation
- [ ] Circle plotting from equation
- [ ] Ellipse plotting from equation
- [ ] Hyperbola plotting from equation
- [ ] Identify conic type from general second-degree equation

---

## Recursion / Sequences (Stretch Goal)

- [ ] Define recursive sequence: a(n+1) = f(a(n))
- [ ] General term sequence: a(n) = f(n)
- [ ] Table of sequence values
- [ ] Sequence graph / cobweb diagram

---

## Probability Simulations (Stretch Goal)

- [ ] Dice roll simulator with frequency display
- [ ] Coin toss simulator
- [ ] Random number generator with distribution selection

---

## Notes

### What NOT to implement (hardware/platform limitations)
- Python/MicroPython environment (PicOS already runs Lua apps natively)
- 3D graphing (too computationally expensive for 200MHz RP2350)
- Spreadsheet (better as a separate PicOS app)
- Geometry construction tool (better as a separate PicOS app)
- USB file transfer (handled at OS level)
- QR code generation (not useful on this device)
- Exam mode (not relevant)

### Implementation Priority Suggestion
1. **High value, moderate effort**: Equation solver, graphing basics, complex numbers
2. **High value, low effort**: Missing math functions (GCD, LCM, etc.), variable storage A-Z, constants library
3. **Medium value**: Matrix operations, distributions, financial, natural display
4. **Nice to have**: Statistical plots, conic sections, recursion, probability sims
