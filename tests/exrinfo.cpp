// exrinfo image [x y [w h]] (w h: pixel values are the mean over that box): prints an image's size, its data window's origin
// and display window's size with the pixel aspect ratio, channel names,
// string metadata (name=value, one per line) and, given a pixel of the data
// window (x right, y down), its channel values. For tests/test_exr.py (no OpenImageIO in Python).
#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: exrinfo image [x y]\n");
        return 2;
    }
    auto in = OIIO::ImageInput::open(argv[1]);
    if (!in) {
        std::fprintf(stderr, "can't open %s: %s\n", argv[1], OIIO::geterror().c_str());
        return 1;
    }
    const OIIO::ImageSpec &spec = in->spec();
    std::printf("size %d %d\n", spec.width, spec.height);
    std::printf("window %d %d %d %d %g\n", spec.x, spec.y, spec.full_width, spec.full_height,
                spec.get_float_attribute("PixelAspectRatio", 1.0f));
    for (const std::string &c : spec.channelnames) {
        std::printf("channel %s\n", c.c_str());
    }
    for (const OIIO::ParamValue &p : spec.extra_attribs) {
        if (p.type() == OIIO::TypeDesc::STRING) {
            std::printf("meta %s=%s\n", p.name().c_str(), p.get_string().c_str());
        }
    }
    if (argc >= 4) {
        std::vector<float> pixels(size_t(spec.width) * spec.height * spec.nchannels);
        const int x = std::atoi(argv[2]), y = std::atoi(argv[3]);
        if (x < 0 || y < 0 || x >= spec.width || y >= spec.height) {
            std::fprintf(stderr, "pixel %d %d is outside the %dx%d data window\n", x, y, spec.width, spec.height);
            return 1;
        }
        if (!in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data())) {
            std::fprintf(stderr, "can't read %s: %s\n", argv[1], in->geterror().c_str());
            return 1;
        }
        const int w = argc >= 6 ? std::max(1, std::atoi(argv[4])) : 1, h = argc >= 6 ? std::max(1, std::atoi(argv[5])) : 1;
        if (x + w > spec.width || y + h > spec.height) {
            std::fprintf(stderr, "box %d %d %dx%d is outside the %dx%d data window\n", x, y, w, h, spec.width, spec.height);
            return 1;
        }
        for (int c = 0; c < spec.nchannels; ++c) {
            double sum = 0;
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i)
                    sum += pixels[((size_t(y) + j) * spec.width + x + i) * spec.nchannels + c];
            std::printf("pixel %s %.9g\n", spec.channelnames[c].c_str(), sum / (double(w) * h));
        }
    }
    return 0;
}
