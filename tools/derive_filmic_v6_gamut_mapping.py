# Derive equation for gamut mapping in Yrg in Filmic v6
# Based on equations of Yrg -> LMS conversion in
# src/common/colorspaces_inline_conversions.h

from sympy import *
from sympy.solvers.solveset import solveset_real

init_printing(use_unicode=True)

# ch = cos(hue), sh = sin(hue)
Y, c, ch, sh = symbols('Y c ch sh')

# Go from polar coordinates to Yrg
Yrg = Matrix([[Y, c * ch + 0.21902143, c * sh + 0.54371398]]).transpose()

# Form normalized rgb
r = Yrg[1, 0]
g = Yrg[2, 0]
rgb = Matrix([[r, g, 1 - r - g]]).transpose()

# Transform to normalized lms space
rgb_to_lms = Matrix([
    [0.95, 0.38, 0.00],
    [0.05, 0.62, 0.03],
    [0.00, 0.00, 0.97]
])
lms = rgb_to_lms * rgb

# Apply normalization based on luminance
coeff = Y / (0.68990272 * lms[0, 0] + 0.34832189 * lms[1, 0])
LMS = coeff * lms

# LMS is converted to target RGB.
# Here, A is a single row of the LMS -> RGB conversion matrix.
# Thus this corresponds to calculating one component of
# target RGB. Each component is calculated the same way, coefficients
# are just different.
a1, a2, a3 = symbols("a1 a2 a3")
A = Matrix([[a1, a2, a3]])
component = (A * LMS)[0, 0]

# k is the target RGB component value (black or white luminance in gamut mapping)
k = symbols('k')

# Solve for chroma that gives the desired extreme value k for one components.
# The chroma will be calculated for each component separately, and the smallest
# value will be chosen to stay in gamut.
pprint(solveset_real(component - k, c))
