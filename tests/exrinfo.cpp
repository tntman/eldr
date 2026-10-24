// exrinfo image [x y]: prints an image's size, channel names, string
// metadata (name=value, one per line) and, given a pixel (x right, y down),
// its channel values. For tests/test_exr.py (no OpenImageIO in Python).
#include <OpenImageIO/imageio.h>

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
        in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data());
        const int x = std::atoi(argv[2]), y = std::atoi(argv[3]);
        for (int c = 0; c < spec.nchannels; ++c) {
            std::printf("pixel %s %.9g\n", spec.channelnames[c].c_str(),
                        pixels[(size_t(y) * spec.width + x) * spec.nchannels + c]);
        }
    }
    return 0;
}
