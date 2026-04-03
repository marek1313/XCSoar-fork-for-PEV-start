// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "ThermalLook.hpp"
#include "FontDescription.hpp"
#include "Screen/Layout.hpp"

void
ThermalLook::Initialise(bool small)
{
  weak_color = Color(0x99, 0x99, 0x99);
  mid_color = Color(0x00,0x99,0x00);
  strong_color = Color(0x00,0x00,0x99);
  
  weak_brush.Create(weak_color);
  mid_brush.Create(mid_color);
  strong_brush.Create(strong_color);

  unsigned width = Layout::FastScale(small ? 1u : 2u);
  
}
