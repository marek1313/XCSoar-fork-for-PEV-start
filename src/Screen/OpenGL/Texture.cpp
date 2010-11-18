/*
Copyright_License {

  XCSoar Glide Computer - http://www.xcsoar.org/
  Copyright (C) 2000-2010 The XCSoar Project
  A detailed list of copyright holders can be found in the file "AUTHORS".

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
}
*/

#include "Screen/OpenGL/Texture.hpp"
#include "Screen/SDL/Format.hpp"
#include "Asset.hpp"
#include "Compiler.h"

#ifdef ANDROID
#include <GLES/glext.h>
#endif

gcc_const gcc_unused
static unsigned
next_power_of_two(unsigned i)
{
  unsigned p = 1;
  while (p < i)
    p <<= 1;
  return p;
}

void
GLTexture::update(SDL_Surface *surface)
{
  unsigned pitch = surface->pitch / surface->format->BytesPerPixel;

#ifdef ANDROID
  /* 16 bit 5/6/5 on Android */
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pitch, surface->h,
                  GL_RGB, GL_UNSIGNED_SHORT_5_6_5, surface->pixels);
#else
  /* 32 bit R/G/B/A on full OpenGL */
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pitch, surface->h,
                  GL_BGRA, GL_UNSIGNED_BYTE, surface->pixels);
#endif
}

void
GLTexture::load(SDL_Surface *src)
{
  width = src->w;
  height = src->h;

  SDL_Surface *surface = ConvertToDisplayFormatPreserve(src);

  unsigned pitch = surface->pitch / surface->format->BytesPerPixel;

#ifdef ANDROID
  glPixelStorei(GL_UNPACK_ALIGNMENT, 2);

  unsigned width2 = next_power_of_two(pitch);
  unsigned height2 = next_power_of_two(height);

  if (width2 == pitch && height2 == height)
    /* 16 bit 5/6/5 on Android */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pitch, height, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, surface->pixels);
  else {
    /* dimensions are not a power of two: create an "undefined"
       expanded texture first, then copy the SDL_Surface as a sub
       texture */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width2, height2, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    update(surface);
  }

#else
  /* 32 bit R/G/B/A on full OpenGL */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pitch, surface->h, 0,
               GL_BGRA, GL_UNSIGNED_BYTE, surface->pixels);
#endif

  if (surface != src)
    SDL_FreeSurface(surface);
}

void
GLTexture::draw(int x_offset, int y_offset,
                int dest_x, int dest_y,
                unsigned dest_width, unsigned dest_height,
                int src_x, int src_y,
                unsigned src_width, unsigned src_height) const
{
#ifdef ANDROID
  const GLint rect[4] = { src_x, src_y, src_width, src_height };
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, rect);

  /* glDrawTexiOES() circumvents the projection settings, thus we must
     roll our own translation */
  unsigned screen_height = SDL_GetVideoSurface()->h;
  glDrawTexiOES(x_offset + dest_x,
                screen_height - y_offset - dest_y,
                0, dest_width, -dest_height);
#else
  GLfloat x0 = (GLfloat)src_x / width;
  GLfloat y0 = (GLfloat)src_y / height;
  GLfloat x1 = (GLfloat)(src_x + src_width) / width;
  GLfloat y1 = (GLfloat)(src_y + src_height) / height;

  glBegin(GL_QUADS);
  glTexCoord2f(x0, y0);
  glVertex3f(dest_x, dest_y, 0);
  glTexCoord2f(x1, y0);
  glVertex3f(dest_x + dest_width, dest_y, 0);
  glTexCoord2f(x1, y1);
  glVertex3f(dest_x + dest_width, dest_y + dest_height, 0);
  glTexCoord2f(x0, y1);
  glVertex3f(dest_x, dest_y + dest_height, 0);
  glEnd();
#endif
}
