// Tile-based adaptive sampling (Frame::adaptiveTiles): film.comp stamps each
// 16 x 16 tile in which a pixel still took a sample with that film frame's
// number. With frame.tiles.x > 0 (the last film frame's stamp), a tile none
// of whose 3 x 3 tiles has it skips its shading and rays this sample: its
// pixels stopped, and so did the film filter's footprint around them (1
// pixel) and the final gather's denoise (2), well inside the tile around.
// frame.tiles.yz: tiles across, down.
layout(std430, set = 0, binding = 91) readonly buffer FilmTiles { uint filmTiles[]; };

bool TileSkipped(ivec2 pixel)
{
    if (frame.tiles.x == 0u) {
        return false;
    }
    ivec2 tile = pixel >> 4, count = ivec2(frame.tiles.yz);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            ivec2 t = tile + ivec2(x, y);
            if (all(greaterThanEqual(t, ivec2(0))) && all(lessThan(t, count)) &&
                filmTiles[t.y * count.x + t.x] >= frame.tiles.x) {
                return false;
            }
        }
    }
    return true;
}
