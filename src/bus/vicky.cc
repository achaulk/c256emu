#include "bus/vicky.h"

#include <chrono>
#include <functional>
#include <thread>

#include "bus/int_controller.h"
#include "bus/keyboard.h"
#include "bus/sdl_to_atset_keymap.h"
#include "bus/system.h"
#include "bus/vicky_def.h"
#include "vicky.h"

namespace {

constexpr uint8_t kColsPerLine = 128;
constexpr uint8_t kTileSize = 16;
constexpr uint8_t kSpriteSize = 32;

using UniqueTexturePtr =
    std::unique_ptr<SDL_Texture, std::function<void(SDL_Texture *)>>;

bool Set16(const Address &addr, const Address &start_addr, uint16_t *dest,
           uint8_t v) {
  uint16_t o = addr.offset_ - start_addr.offset_;
  if (o > 1)
    return false;
  if (o == 0)
    *dest = (*dest & 0xff00) | v;
  else
    *dest = (*dest & 0x00ff) | (v << 8);
  return true;
}

bool Set24(const Address &addr, const Address &start_addr, uint32_t *dest,
           uint8_t v) {
  uint16_t o = addr.offset_ - start_addr.offset_;
  if (o > 2)
    return false;
  if (o == 0)
    *dest = (*dest & 0x00ffff00) | v;
  else if (o == 1)
    *dest = (*dest & 0x00ff00ff) | (v << 8);
  else if (o == 2)
    *dest = (*dest & 0x0000ffff) | (v << 16);
  return true;
}

} // namespace

Vicky::Vicky(System *system, InterruptController *int_controller)
    : is_dirty_(false), sys_(system), int_controller_(int_controller) {
  bzero(fg_colour_mem_, sizeof(fg_colour_mem_));
  bzero(bg_colour_mem_, sizeof(bg_colour_mem_));
  bzero(video_ram_, sizeof(video_ram_));
  bzero(tile_sets_, sizeof(tile_sets_));
  bzero(sprites_, sizeof(sprites_));
}

void Vicky::Start() {
  SDL_Init(SDL_INIT_VIDEO);
  window_ = SDL_CreateWindow("Vicky", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, kVickyBitmapWidth,
                             kVickyBitmapHeight, SDL_WINDOW_OPENGL);

  CHECK(window_);
  renderer_ = SDL_CreateRenderer(window_, -1, 0);
  vicky_texture_.reset(SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA8888,
                                         SDL_TEXTUREACCESS_STREAMING,
      kVickyBitmapWidth, kVickyBitmapHeight));

  pixel_format_.reset(SDL_AllocFormat(SDL_PIXELFORMAT_BGRA8888));
  CHECK(renderer_);
}

bool Vicky::DecodeAddress(const Address &from_addr, Address &to_addr) {
  if (from_addr.InRange(MASTER_CTRL_REG_L, VDMA_RESERVED_1) ||
      from_addr.InRange(FG_CHAR_LUT_PTR, CS_COLOUR_MEM_END)) {
    to_addr = from_addr;
    return true;
  }

  if (from_addr.InRange(Address(0xb0, 0000), Address(0xef, 0xffff))) {
    to_addr = from_addr;
    return true;
  }
  return false;
}

Vicky::~Vicky() {
  if (window_) {
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
  }
}

uint8_t Vicky::ReadByte(const Address &addr, uint8_t **address) { return 0; }

std::vector<SystemBusDevice::MemoryRegion> Vicky::GetMemoryRegions() {
  return {
      SystemBusDevice::MemoryRegion{0xb00000, 0xf00000, video_ram_},
      SystemBusDevice::MemoryRegion{CS_TEXT_MEM_PTR.AsInt(),
                                    CS_COLOR_MEM_PTR.WithOffset(-1).AsInt(),
                                    text_mem_},
      SystemBusDevice::MemoryRegion{CS_COLOR_MEM_PTR.AsInt(),
                                    BTX_START.WithOffset(-1).AsInt(),
                                    text_colour_mem_},
      SystemBusDevice::MemoryRegion{VKY_TXT_CURSOR_X_REG_L.AsInt(),
                                    VKY_TXT_CURSOR_X_REG_H.AsInt(),
                                    reinterpret_cast<uint8_t *>(&cursor_x_)},
      SystemBusDevice::MemoryRegion{VKY_TXT_CURSOR_Y_REG_L.AsInt(),
                                    VKY_TXT_CURSOR_Y_REG_H.AsInt(),
                                    reinterpret_cast<uint8_t *>(&cursor_y_)},
      SystemBusDevice::MemoryRegion{MOUSE_PTR_X_POS_L.AsInt(),
                                    MOUSE_PTR_X_POS_H.AsInt(),
                                    reinterpret_cast<uint8_t *>(&mouse_pos_x_)},
      SystemBusDevice::MemoryRegion{MOUSE_PTR_Y_POS_L.AsInt(),
                                    MOUSE_PTR_Y_POS_H.AsInt(),
                                    reinterpret_cast<uint8_t *>(&mouse_pos_y_)},
      SystemBusDevice::MemoryRegion{FONT_MEMORY_BANK0.AsInt(),
                                    FONT_MEMORY_BANK1.WithOffset(-1).AsInt(),
                                    font_bank_0_},
      SystemBusDevice::MemoryRegion{FONT_MEMORY_BANK1.AsInt(), 0xaf8fff,
                                    font_bank_1_},
      SystemBusDevice::MemoryRegion{MOUSE_PTR_GRAP0_START.AsInt(),
                                    MOUSE_PTR_GRAP0_END.WithOffset(-1).AsInt(),
                                    mouse_cursor_0_},
      SystemBusDevice::MemoryRegion{MOUSE_PTR_GRAP1_START.AsInt(),
                                    MOUSE_PTR_GRAP1_END.WithOffset(-1).AsInt(),
                                    mouse_cursor_1_},

  };
}

void Vicky::StoreByte(const Address &addr, uint8_t v, uint8_t **address) {
  uint16_t offset = addr.offset_;

  if (addr == BM_CONTROL_REG) {
    bitmap_enabled_ = v & 0x01;
    bitmap_lut_ = (v & 0b01110000) >> 4;
    return;
  }

  if (Set16(addr, MASTER_CTRL_REG_L, &mode_, v)) {
    LOG(INFO) << "Set mode: " << mode_ << " Address: " << addr;
    return;
  }

  if (addr.InRange(TL0_CONTROL_REG, TL3_MAP_Y_STRIDE_H)) {
    uint16_t sprite_offset = offset - TL0_CONTROL_REG.offset_;
    uint8_t tile_num = sprite_offset / 8;
    uint8_t register_num = sprite_offset % 8;
    if (register_num == 0) {
      tile_sets_[tile_num].enabled = v & 0x01;
      tile_sets_[tile_num].lut = (v & 0b00001110) >> 1;
      tile_sets_[tile_num].tiled_sheet = (v & 0x80);
    } else if (register_num == 1) {
      tile_sets_[tile_num].start_addr =
          (tile_sets_[tile_num].start_addr & 0x00ffff00) | v;
    } else if (register_num == 2) {
      tile_sets_[tile_num].start_addr =
          (tile_sets_[tile_num].start_addr & 0x00ff00ff) | (v << 8);
    } else if (register_num == 3) {
      tile_sets_[tile_num].start_addr =
          (tile_sets_[tile_num].start_addr & 0x0000ffff) | (v << 16);
    } else if (register_num == 4) {
      Binary::setLower8BitsOf16BitsValue(&tile_sets_[tile_num].stride_x, v);
    } else if (register_num == 5) {
      Binary::setHigher8BitsOf16BitsValue(&tile_sets_[tile_num].stride_x, v);
    } else if (register_num == 6) {
      Binary::setLower8BitsOf16BitsValue(&tile_sets_[tile_num].stride_y, v);
    } else if (register_num == 7) {
      Binary::setHigher8BitsOf16BitsValue(&tile_sets_[tile_num].stride_y, v);
    } else {
      LOG(ERROR) << "Unsupported tile reg: " << register_num;
    }
    return;
  }

  if (addr.InRange(TILE_MAP0, TILE_MAP3 + 2048)) {
    uint16_t tile_offset = offset - TILE_MAP0.offset_;
    uint8_t tile_num = tile_offset / 0x800;
    uint8_t map_offset = tile_offset % 0x800;
    tile_sets_[tile_num].tile_map.mem[map_offset] = v;
    return;
  }

  if (Set24(addr, BACKGROUND_COLOR_B, &background_bgr_.v, v)) {
    return;
  }

  if (addr.InRange(SP00_CONTROL_REG, SP31_CONTROL_REG + 8)) {
    uint16_t sprite_offset = offset - SP00_CONTROL_REG.offset_;
    uint16_t sprite_num = sprite_offset / 0x08;
    uint16_t register_num = sprite_offset % 0x08;
    if (register_num == 0) /* control register */ {
      uint8_t layer = (v & 0b01110000) >> 4;
      enabled_sprites_[layer] ^= (1UL << sprite_num);
      sprites_[sprite_num].lut = (v & 0b00001110) >> 1;
      sprites_[sprite_num].tile_striding = (v & 0x80);
    } else if (register_num == 1) {
      sprites_[sprite_num].start_addr =
          (sprites_[sprite_num].start_addr & 0x00ffff00) | v;
    } else if (register_num == 2) {
      sprites_[sprite_num].start_addr =
          (sprites_[sprite_num].start_addr & 0x00ff00ff) | (v << 8);
    } else if (register_num == 3) {
      sprites_[sprite_num].start_addr =
          (sprites_[sprite_num].start_addr & 0x0000ffff) | (v << 16);
    } else if (register_num == 4) {
      Binary::setLower8BitsOf16BitsValue(&sprites_[sprite_num].x, v);
    } else if (register_num == 5) {
      Binary::setHigher8BitsOf16BitsValue(&sprites_[sprite_num].x, v);
    } else if (register_num == 6) {
      Binary::setLower8BitsOf16BitsValue(&sprites_[sprite_num].y, v);
    } else if (register_num == 7) {
      Binary::setHigher8BitsOf16BitsValue(&sprites_[sprite_num].y, v);
    } else {
      LOG(ERROR) << "Unsupported sprite reg: " << register_num;
    }
    return;
  }

  // Palette set.
  if (addr.InRange(GRPH_LUT0_PTR, GAMMA_B_LUT_PTR - 1)) {
    uint8_t lut_idx = (offset - 0x2000) / 0x400;
    uint8_t palette_idx = (offset - 0x2000) % 0x400 / 4;
    uint8_t colour_idx = (offset - 0x2000) % 0x400 % 4;
    switch (colour_idx) {
    case 0:
      if (address)
        *address = &lut_[lut_idx][palette_idx].b;
      lut_[lut_idx][palette_idx].b = v;
      break;
    case 1:
      if (address)
        *address = &lut_[lut_idx][palette_idx].g;
      lut_[lut_idx][palette_idx].g = v;
      break;
    case 2:
      if (address)
        *address = &lut_[lut_idx][palette_idx].r;
      lut_[lut_idx][palette_idx].r = v;
      break;
    case 3:
      if (address)
        *address = &lut_[lut_idx][palette_idx].a;
      lut_[lut_idx][palette_idx].a = v;
      break;
    }
    return;
  }
  if (addr.InRange(GAMMA_B_LUT_PTR, GAMMA_G_LUT_PTR - 1)) {
    if (address)
      *address = reinterpret_cast<uint8_t *>(
          &gamma_b_[offset - GAMMA_B_LUT_PTR.offset_]);
    gamma_b_[offset - GAMMA_B_LUT_PTR.offset_] = ((uint16_t)v) << 8;
    SDL_SetWindowGammaRamp(window_, gamma_r_, gamma_g_, gamma_b_);

    return;
  }
  if (addr.InRange(GAMMA_G_LUT_PTR, GAMMA_R_LUT_PTR - 1)) {
    if (address)
      *address = reinterpret_cast<uint8_t *>(
          &gamma_g_[offset - GAMMA_B_LUT_PTR.offset_]);
    gamma_g_[offset - GAMMA_G_LUT_PTR.offset_] = ((uint16_t)v) << 8;
    SDL_SetWindowGammaRamp(window_, gamma_r_, gamma_g_, gamma_b_);

    return;
  }
  if (addr.InRange(GAMMA_R_LUT_PTR, TILE_MAP0 - 1)) {
    if (address)
      *address = reinterpret_cast<uint8_t *>(
          &gamma_r_[offset - GAMMA_B_LUT_PTR.offset_]);
    gamma_r_[offset - GAMMA_R_LUT_PTR.offset_] = ((uint16_t)v) << 8;
    SDL_SetWindowGammaRamp(window_, gamma_r_, gamma_g_, gamma_b_);

    return;
  }

  if (addr == VKY_TXT_CURSOR_CTRL_REG) {
    cursor_reg_ = v;
    return;
  }
  if (addr == VKY_TXT_CURSOR_CHAR_REG) {
    cursor_char_ = v;
    return;
  }
  if (addr == VKY_TXT_CURSOR_COLR_REG) {
    cursor_colour_ = v;
    return;
  }

  if (addr.InRange(FG_CHAR_LUT_PTR, BG_CHAR_LUT_PTR - 1)) {
    uint8_t palette_idx = (offset - FG_CHAR_LUT_PTR.offset_) % 0x400 / 4;
    uint8_t colour_idx = (offset - FG_CHAR_LUT_PTR.offset_) % 0x400 % 4;
    fg_colour_mem_[palette_idx] |= v << (8 * (3 - colour_idx));
    return;
  }
  if (addr.InRange(BG_CHAR_LUT_PTR, Address(0xaf, 0x1f7f))) {
    uint8_t palette_idx = (offset - BG_CHAR_LUT_PTR.offset_) % 0x400 / 4;
    uint8_t colour_idx = (offset - BG_CHAR_LUT_PTR.offset_) % 0x400 % 4;
    bg_colour_mem_[palette_idx] |= v << (8 * (3 - colour_idx));
    return;
  }

  if (addr == MOUSE_PTR_CTRL_REG_L) {
    mouse_cursor_enable_ = v & 0x01;
    mouse_cursor_select_ = v & 0x02;
    return;
  }
  if (addr == BORDER_CTRL_REG) {
    border_enabled_ = v & (1 << Border_Ctrl_Enable);
    return;
  }
  if (addr == BORDER_COLOR_B) {
    border_colour_.b = v;
    return;
  }
  if (addr == BORDER_COLOR_G) {
    border_colour_.g = v;
    return;
  }
  if (addr == BORDER_COLOR_R) {
    border_colour_.r = v;
    return;
  }
  LOG(INFO) << "Unknown Vicky register: " << addr;
}

void Vicky::RenderLine() {
  if (mode_ & Mstr_Ctrl_Disable_Vid)
    return;

  bool run_char_gen =
      mode_ & Mstr_Ctrl_Text_Mode_En || mode_ & Mstr_Ctrl_Text_Overlay;

  // Check cursor flash.
  if (run_char_gen && (cursor_reg_ & Vky_Cursor_Enable)) {
    uint16_t flash_interval_ms = 0;
    switch (cursor_reg_ >> 1) {
    case 0b00:
      flash_interval_ms = 1000;
      break;
    case 0b01:
      flash_interval_ms = 500;
      break;
    case 0b10:
      flash_interval_ms = 250;
      break;
    case 0b11:
      flash_interval_ms = 200;
      break;
    }
    auto time_since_flash =
        std::chrono::steady_clock::now() - last_cursor_flash_;
    if (time_since_flash > std::chrono::milliseconds(flash_interval_ms)) {
      cursor_state_ = !cursor_state_;
      is_dirty_ = !is_dirty_;
      last_cursor_flash_ = std::chrono::steady_clock::now();
    }
  }

  SDL_PixelFormat *pixel_format = pixel_format_.get();
  uint32_t *row_pixels = &frame_buffer_[kVickyBitmapWidth * raster_y_];

  // Render order:
  // Background bitmap
  // Sprite layers
  // Tile layer 3
  // Sprite layers
  // Tile Layer 2
  // Sprite layers
  // Tile Layer 1
  // Sprite layers
  // Tile layer 0
  // Sprites
  // Text
  for (uint16_t raster_x = 0; raster_x < kVickyBitmapWidth; raster_x++) {

    if (border_enabled_) {
      if (raster_x < kBorderWidth) {
        row_pixels[raster_x] =
            SDL_MapRGBA(pixel_format, border_colour_.r, border_colour_.g,
                        border_colour_.b, border_colour_.a);
        continue;
      }
      if (raster_x > kVickyBitmapWidth - kBorderWidth) {
        row_pixels[raster_x] =
            SDL_MapRGBA(pixel_format, border_colour_.r, border_colour_.g,
                        border_colour_.b, border_colour_.a);
        continue;
      }
      if (raster_y_ < kBorderHeight) {
        row_pixels[raster_x] =
            SDL_MapRGBA(pixel_format, border_colour_.r, border_colour_.g,
                        border_colour_.b, border_colour_.a);
        continue;
      }
      if (raster_y_ > kVickyBitmapHeight - kBorderHeight) {
        row_pixels[raster_x] =
            SDL_MapRGBA(pixel_format, border_colour_.r, border_colour_.g,
                        border_colour_.b, border_colour_.a);
        continue;
      }
    }

    // 8bpp indexed bitmap goes behind everything.
    if (mode_ & Mstr_Ctrl_Bitmap_En && bitmap_enabled_) {
      RenderBitmap(pixel_format, row_pixels, raster_x);
    }

    // Layers back to front, sprites first, tiles next
    for (uint8_t layer = kNumLayers; layer-- > 0;) {
      // Sprites
      if (mode_ & Mstr_Ctrl_Sprite_En) {
        RenderSprites(pixel_format, row_pixels, raster_x, layer);
      }

      if (mode_ & Mstr_Ctrl_TileMap_En) {
        // Tiles
        RenderTileMap(pixel_format, row_pixels, raster_x, layer);
      }
    }

    // Character set generator.
    if (run_char_gen) {
      RenderCharacterGenerator(row_pixels, raster_x);
    }

    // Mouse
    if (mouse_cursor_enable_) {
      RenderMouseCursor(row_pixels, raster_x);
    }
  }

  // TODO line interrupt
  raster_y_++;
  if (raster_y_ == kVickyBitmapHeight) {
    SDL_UpdateTexture(vicky_texture_.get(), nullptr, frame_buffer_,
                      kVickyBitmapWidth * sizeof(uint32_t));
    SDL_RenderCopy(renderer_, vicky_texture_.get(), nullptr, nullptr);
    SDL_RenderPresent(renderer_);
    raster_y_ = 0;
  }
}

void Vicky::RenderBitmap(const SDL_PixelFormat *pixel_format,
                         uint32_t *row_pixels, uint16_t raster_x) {
  uint8_t *indexed_row =
      video_ram_ + bitmap_addr_offset_ + (raster_y_ * kVickyBitmapWidth);
  uint8_t colour_index = indexed_row[raster_x];
  if (colour_index != 0) {
    SDL_Color color = lut_[bitmap_lut_][colour_index];
    row_pixels[raster_x] =
        SDL_MapRGBA(pixel_format, color.r, color.g, color.b, color.a);
  }
}

void Vicky::RenderSprites(const SDL_PixelFormat *pixel_format,
                          uint32_t *row_pixels, uint16_t raster_x,
                          uint8_t layer) {
  // TODO this sprite routine can't keep up to 14mhz emulation on my
  // PC if lots of sprites were enabled in many layers.
  uint32_t enabled_sprites = enabled_sprites_[layer];

  // No sprites in this layer?  Move on.
  if (!enabled_sprites)
    return;

  // Keep drawing until we run out of sprites in this layer.
  for (uint8_t sprite_num = 0; enabled_sprites != 0;
       sprite_num++, enabled_sprites <<= 1) {
    if (!(enabled_sprites & 1UL << sprite_num))
      return;

    Sprite &sprite = sprites_[sprite_num];

    if (raster_x < sprite.x || raster_x > sprite.x + kSpriteSize ||
        raster_y_ < sprite.y || raster_y_ > sprite.y + kSpriteSize)
      return;

    uint8_t *sprite_mem = video_ram_ + sprite.start_addr;
    uint8_t colour_index =
        sprite_mem[raster_x - sprite.x + (raster_y_ - sprite.y) * kSpriteSize];
    if (colour_index == 0)
      return;
    SDL_Color color1 = lut_[sprite.lut][colour_index];
    row_pixels[raster_x] =
        SDL_MapRGBA(pixel_format, color1.r, color1.g, color1.b, color1.a);
  }
}

void Vicky::RenderTileMap(const SDL_PixelFormat *pixel_format,
                          uint32_t *row_pixels, uint16_t raster_x,
                          uint8_t layer) {
  if (tile_sets_[layer].enabled) {

    // TODO support for linear tile sheets.  this assumes a 256x256 sheet
    // of 16x16 tiles for now.

    const auto &tile_set = tile_sets_[layer];
    uint8_t screen_tile_row = raster_y_ / kTileSize;
    uint8_t screen_tile_sub_row = raster_y_ % kTileSize;

    uint8_t screen_tile_col = raster_x / kTileSize;
    uint8_t screen_tile_sub_col = raster_x % kTileSize;

    uint8_t tile_num = tile_set.tile_map.map[screen_tile_row][screen_tile_col];
    uint8_t *tile_sheet_bitmap = &video_ram_[tile_set.start_addr];

    uint8_t tile_sheet_column =
        tile_num % kTileSize; // the column in the tile sheet
    uint8_t tile_sheet_row = tile_num / kTileSize; // the row in the tile sheet

    // the physical memory location of the row in the sheet our tile is
    // in
    uint8_t *tile_bitmap_row =
        &tile_sheet_bitmap[(tile_sheet_row * kTileSize + screen_tile_sub_row) *
                           tile_set.stride_x];

    // the physical memory location of the column in the sheet our tile
    // is in
    uint8_t *tile_bitmap_column =
        &tile_bitmap_row[tile_sheet_column * kTileSize];

    uint8_t colour_index = tile_bitmap_column[screen_tile_sub_col];
    if (colour_index != 0) {
      SDL_Color color = lut_[tile_set.lut][colour_index];
      row_pixels[raster_x] =
          SDL_MapRGBA(pixel_format, color.r, color.g, color.b, color.a);
    }
  }
}
void Vicky::RenderMouseCursor(uint32_t *row_pixels, uint16_t raster_x) {
  SDL_GetMouseState(&mouse_pos_x_, &mouse_pos_y_);
  if ((raster_x >= mouse_pos_x_ && raster_x <= mouse_pos_x_ + 16) &&
      (raster_y_ >= mouse_pos_y_ && raster_y_ <= mouse_pos_y_ + 16)) {
    uint8_t *mouse_mem =
        mouse_cursor_select_ ? mouse_cursor_0_ : mouse_cursor_1_;
    uint8_t mouse_sub_col = raster_x % 16;
    uint8_t mouse_sub_row = raster_y_ % 16;
    uint8_t pixel_val = mouse_mem[mouse_sub_col + (mouse_sub_row * 16)];
    if (pixel_val != 0) {
      row_pixels[raster_x] = pixel_val | (pixel_val << 8) | (pixel_val << 16);
    }
  }
}
void Vicky::RenderCharacterGenerator(uint32_t *row_pixels, uint16_t raster_x) {
  uint16_t bitmap_x = raster_x;
  uint16_t bitmap_y = raster_y_;

  // If the border is enabled, reduce the rendered area accordingly.
  if (border_enabled_) {
    bitmap_x -= kBorderWidth;
    bitmap_y -= kBorderHeight;
  }
  const uint16_t cursor_x = cursor_x_;
  const uint16_t cursor_y = cursor_y_;
  uint16_t row = bitmap_y / 8;
  uint16_t sub_row = bitmap_y % 8;

  uint8_t column = bitmap_x / 8;
  uint8_t sub_column = bitmap_x % 8;

  uint8_t character = text_mem_[column + (row * kColsPerLine)];
  uint8_t colour = text_colour_mem_[column + (row * kColsPerLine)];
  uint8_t fg_colour_num = (uint8_t)((colour & 0xf0) >> 4);
  uint8_t bg_colour_num = (uint8_t)(colour & 0x0f);
  uint8_t *character_font = &font_bank_0_[character * 8];
  uint8_t *cursor_font = &font_bank_0_[cursor_char_ * 8];

  uint32_t fg_colour = fg_colour_mem_[fg_colour_num];
  uint32_t bg_colour = bg_colour_mem_[bg_colour_num];

  // TODO: cursor colour?
  bool is_cursor_cell = cursor_state_ && cursor_reg_ & Vky_Cursor_Enable &&
                        (cursor_x == column && cursor_y == row);

  uint32_t *char_pixels = &row_pixels[raster_x];

  int pixel_pos = 1 << (7 - sub_column);
  if (is_cursor_cell && cursor_font[sub_row] & pixel_pos) {
    *char_pixels = fg_colour;
  } else if (character_font[sub_row] & pixel_pos) {
    *char_pixels = is_cursor_cell ? bg_colour : fg_colour;
  } else if (mode_ & Mstr_Ctrl_Text_Mode_En && !is_cursor_cell) {
    // note no bg color in overlay or when cursor on
    *char_pixels = bg_colour;
  }
}
