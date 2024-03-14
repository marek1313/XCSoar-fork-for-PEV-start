// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "StartStats.hpp"
#include "Navigation/Aircraft.hpp"

#include <cassert>

void
StartStats::SetStarted(const AircraftState &aircraft,bool pev)
{

  advanced_by_pev = pev;
  time = aircraft.time;
  altitude = aircraft.altitude;
  ground_speed = aircraft.ground_speed;

}

