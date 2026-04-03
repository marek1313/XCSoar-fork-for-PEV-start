// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "ThermalRenderer.hpp"
#include "ui/canvas/Canvas.hpp"
#include "Screen/Layout.hpp"
#include "Look/ThermalLook.hpp"

#include "Math/Screen.hpp"
#include "util/Macros.hpp"
#include "Asset.hpp"

#ifdef ENABLE_OPENGL
#include "ui/canvas/opengl/Scope.hpp"
#endif

void
ThermalRenderer::Draw(Canvas &canvas, const ThermalLook &thermal_look,
                      bool fading,
                      const Thermal &thermal, 
                      const ThermalColor color, const PixelPoint pt) noexcept
{


 
    // Select brush depending on Thermal
    switch (int(thermal.average)) {
    case Thermal::AVERAGE::WEAK:

      canvas.Select(thermal_look.weak_brush);
      break;
    case Thermal::AVERAGE::MID:

      canvas.Select(thermal_look.mid_brush);
      break;
    case Thermal::AVERAGE::STRONG:

      canvas.Select(thermal_look.strong_brush);
      break;
    }

    // Select black pen
    canvas.SelectBlackPen();



  



  

  canvas.SelectHollowBrush();
  canvas.DrawCircle(pt, Layout::FastScale(8u));
}



