# stb_image

`stb_image.h` is pinned to [nothings/stb `4e9b368bfc071e97206ea5c6adba68dd0cbf8897`](https://github.com/nothings/stb/blob/4e9b368bfc071e97206ea5c6adba68dd0cbf8897/stb_image.h).
Upstream SHA-256 before the local patch: `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`.
The file includes its public-domain or MIT license grant. Engine compiles only
its Radiance HDR decoder, with stdio disabled and dimensions bounded before
pixel decode.

Local decoder patch: `stbi__hdr_load` checks every four-byte flat pixel,
four-byte RLE scanline header, run count/value, and literal byte read for EOF.
Upstream used an unchecked `stbi__getn` for flat pixels and `stbi__get8` for
RLE bytes; the latter returns zero at EOF. A truncated flat pixel could use
uninitialized stack bytes, while a truncated RLE run or dump could be accepted
as a zero component. Each failed read now releases both decoder allocations
before returning a decode error. `AssetFoundationTests.cpp` imports complete
flat, legacy wide-flat, RLE-run, and RLE-literal images, then rejects every
shorter byte prefix after each valid header. Keep this patch when updating the
pinned upstream file until upstream provides equivalent exact-read handling.
