width = 150
height = 106
patch = 12.5
outer = 4
inner = 2.5
gap = 19
nx = 8
ny = 6

vertical_outer = (height - ny * patch - (ny - 1) * inner) / 2.0

radius = patch / height / 2.0
print(f"{radius:.3f}")

for ix in range(nx):
    for iy in range(ny):
        px = outer + ix * (patch + inner) + 0.5 * patch
        if ix >= nx // 2:
            px += gap + 2 * outer - inner
        py = vertical_outer + iy * (patch + inner) + 0.5 * patch

        px /= width
        py /= height
        print(f"{px:.3f}, {py:.3f}")
