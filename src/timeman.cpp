/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "search.h"
#include "timeman.h"
#include "uci.h"

TimeManagement Time; // Our global time management object

namespace {

  enum TimeType { OptimumTime, MaxTime };

  const double MaxRatio   = 5.25;  // When in trouble, we can step over reserved time with this ratio
  const double StealRatio   = 0.076;  // However, we must be careful to not get low on time


  template<TimeType T>
  int remaining(int myTime, int myInc, int moveOverhead, int movesToGo)
  {
    const double TMaxRatio   = (T == OptimumTime ? 1 : MaxRatio);
    const double TStealRatio   = (T == OptimumTime ? 0.018 : StealRatio);

    double ratio1 = std::min(1.0, TMaxRatio * (myTime + myInc * (movesToGo - 1)) / (double)movesToGo);
    double ratio2 = std::min(1.0, TStealRatio * (1.0 + 93 * myInc / (double)myTime));
    int hypMyTime = std::max(0, myTime - moveOverhead);

    return int(hypMyTime * std::min(ratio1, ratio2)); // Intel C++ asks for an explicit cast
  }

} // namespace


/// init() is called at the beginning of the search and calculates the allowed
/// thinking time out of the time control and current game ply. We support four
/// different kinds of time controls, passed in 'limits':
///
///  inc == 0 && movestogo == 0 means: x basetime  [sudden death!]
///  inc == 0 && movestogo != 0 means: x moves in y minutes
///  inc >  0 && movestogo == 0 means: x basetime + z increment
///  inc >  0 && movestogo != 0 means: x moves in y minutes + z increment

void TimeManagement::init(Search::LimitsType& limits, Color us, Value pScore)
{
  int moveOverhead    = Options["Move Overhead"];
  int npmsec          = Options["nodestime"];

  // If we have to play in 'nodes as time' mode, then convert from time
  // to nodes, and use resulting values in time management formulas.
  // WARNING: Given npms (nodes per millisecond) must be much lower then
  // the real engine speed to avoid time losses.
  if (npmsec)
  {
      if (!availableNodes) // Only once at game start
          availableNodes = npmsec * limits.time[us]; // Time is in msec

      // Convert from millisecs to nodes
      limits.time[us] = (int)availableNodes;
      limits.inc[us] *= npmsec;
      limits.npmsec = npmsec;
  }

  startTime = limits.startTime;

  int MoveHorizon = (pScore > 0) ? std::max(1, 49 - int(16.4 * log(1.0 + abs(pScore) / 87.5))) : 49;
  int MaxMTG = limits.movestogo ? std::min(limits.movestogo, MoveHorizon) : MoveHorizon;

      optimumTime = remaining<OptimumTime>(limits.time[us], limits.inc[us], moveOverhead, MaxMTG);
      maximumTime = remaining<MaxTime    >(limits.time[us], limits.inc[us], moveOverhead, MaxMTG);

  if (Options["Ponder"])
      optimumTime += optimumTime / 4;
}

