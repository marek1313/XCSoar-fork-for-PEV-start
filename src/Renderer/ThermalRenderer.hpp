// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#pragma once



struct PixelPoint;
class Canvas;
struct ThermalLook;
struct Thermal;



namespace ThermalRenderer
{
void
Draw(Canvas &canvas, const ThermalLook &thermal_look,

     const Thermal &thermal, 
     PixelPoint pt) noexcept;


}