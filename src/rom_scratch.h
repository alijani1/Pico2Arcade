/* rom_scratch.h — temporary heap buffer for the ROM loaders.
 *
 * The loaders need several KB of scratch while decoding a game's zip.
 * These used to be function-local `static` arrays, which kept ~92 KB of
 * RAM reserved for the whole session even though each is only used once
 * at game load. RomScratch allocates on construction and frees when it
 * goes out of scope, so every early `return` releases it too.
 *
 * calloc (not malloc): some loaders accept files shorter than the buffer,
 * and the old static arrays read as zero past the data. */
#pragma once

#include <stdint.h>
#include <stdlib.h>

class RomScratch {
public:
    explicit RomScratch(size_t n) : p_((uint8_t *)calloc(n, 1)) {}
    ~RomScratch() { free(p_); }
    RomScratch(const RomScratch &) = delete;
    RomScratch &operator=(const RomScratch &) = delete;

    operator uint8_t *() const { return p_; }       /* use like an array */
    uint8_t &operator[](size_t i) const { return p_[i]; }

private:
    uint8_t *p_;
};
