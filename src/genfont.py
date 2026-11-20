"""Generate antialiased, four-bit alpha masks for Pocket Boy's UI."""
from PIL import Image, ImageDraw, ImageFont


def masks(name, images):
    width, height = images[0].size
    stride = (width + 1) // 2
    print(f"static const unsigned char {name}[{len(images)}][{height}][{stride}] = {{")
    for image in images:
        print("    {")
        for y in range(height):
            row = [round(image.getpixel((x, y)) / 17) for x in range(width)]
            if width % 2:
                row.append(0)
            print("        {" + ", ".join(f"0x{row[x + 1] * 16 + row[x]:02x}"
                                            for x in range(0, len(row), 2)) + "},")
        print("    },")
    print("};")


for size, width, height, suffix in ((15, 16, 17, ""), (20, 26, 24, "_20"), (24, 26, 28, "_24")):
    font = ImageFont.truetype("LiberationSans-Regular.ttf", size)
    characters = [chr(c) for c in range(32, 127)]
    images = []
    for char in characters:
        image = Image.new("L", (width, height))
        ImageDraw.Draw(image).text((0, 0), char, font=font, fill=255)
        images.append(image)
    masks("glyphs" + suffix, images)
    print(f"static const unsigned char advance{suffix}[95] = {{")
    print("    " + ", ".join(str(max(3, round(font.getlength(char)))) for char in characters))
    print("};")


def heart(size, outline):
    scale = 4
    image = Image.new("L", (size * scale, size * scale))
    point = (9, 4)
    path = [point]
    for a, b, end in (
        ((6, 0), (1, 1), (1, 6)),
        ((1, 10), (5, 13), (9, 17)),
        ((13, 13), (17, 10), (17, 6)),
        ((17, 1), (12, 0), (9, 4)),
    ):
        for i in range(1, 17):
            t = i / 16
            u = 1 - t
            path.append(tuple(u**3 * point[d] + 3*u*u*t * a[d] + 3*u*t*t * b[d] + t**3 * end[d]
                              for d in range(2)))
        point = end
    points = [(round(x * scale), round(y * scale)) for x, y in path]
    draw = ImageDraw.Draw(image)
    if outline:
        draw.line(points, fill=255, width=6, joint="curve")
    else:
        draw.polygon(points, fill=255)
    return image.resize((size, size), Image.Resampling.LANCZOS)


masks("heart_mask", [heart(18, True), heart(18, False)])
