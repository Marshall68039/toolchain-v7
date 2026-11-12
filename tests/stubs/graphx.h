#pragma once
#include <stdint.h>
typedef struct gfx_sprite_t { uint8_t width; uint8_t height; uint8_t data[1]; } gfx_sprite_t;
void gfx_Begin(void); void gfx_End(void);
void gfx_SetDrawBuffer(void); void gfx_SwapDraw(void);
void gfx_SetMonospaceFont(uint8_t spacing);
void gfx_SetPalette(const void *palette, unsigned size, uint8_t offset);
void gfx_FillScreen(uint8_t color); void gfx_SetColor(uint8_t color);
void gfx_FillRectangle(int x, int y, int w, int h);
void gfx_Rectangle(int x, int y, int w, int h);
void gfx_HorizLine(int x, int y, int length);
void gfx_Sprite(const gfx_sprite_t *sprite, int x, int y);
void gfx_SetTextFGColor(uint8_t c); void gfx_SetTextBGColor(uint8_t c);
void gfx_SetTextTransparentColor(uint8_t c);
void gfx_SetTextScale(uint8_t width_scale, uint8_t height_scale);
void gfx_SetTextXY(int x, int y);
void gfx_PrintChar(const char c);
void gfx_PrintString(const char *s);
void gfx_PrintStringXY(const char *s, int x, int y);
void gfx_ScaledSprite_NoClip(const gfx_sprite_t *sprite, int x, int y, uint8_t w_scale, uint8_t h_scale);
