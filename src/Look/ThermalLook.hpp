// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once

#include "ui/canvas/Color.hpp"
#include "ui/canvas/Pen.hpp"
#include "ui/canvas/Brush.hpp"
#include "ui/canvas/Font.hpp"

struct ThermalLook;

struct ThermalLook {
  Color weak_color;
  Color mid_color;
  Color strong_color;
  

  Brush weak_brush;
  Brush mid_brush;
  Brush strong_brush;

  void Initialise(bool small);
};
