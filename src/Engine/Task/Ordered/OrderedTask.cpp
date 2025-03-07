// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#include "OrderedTask.hpp"
#include "Task/TaskEvents.hpp"
#include "Points/OrderedTaskPoint.hpp"
#include "Points/StartPoint.hpp"
#include "Points/FinishPoint.hpp"
#include "Task/Solvers/TaskMacCreadyTravelled.hpp"
#include "Task/Solvers/TaskMacCreadyRemaining.hpp"
#include "Task/Solvers/TaskMacCreadyTotal.hpp"
#include "Task/Solvers/TaskCruiseEfficiency.hpp"
#include "Task/Solvers/TaskEffectiveMacCready.hpp"
#include "Task/Solvers/TaskBestMc.hpp"
#include "Task/Solvers/TaskMinTarget.hpp"
#include "Task/Solvers/TaskGlideRequired.hpp"
#include "Task/Solvers/TaskOptTarget.hpp"
#include "Task/Visitors/TaskPointVisitor.hpp"
#include "Task/Factory/Create.hpp"
#include "Task/Factory/AbstractTaskFactory.hpp"
#include "Task/Factory/Constraints.hpp"
#include "Waypoint/Waypoints.hpp"
#include "Geo/Flat/FlatBoundingBox.hpp"
#include "Geo/GeoBounds.hpp"
#include "Task/Stats/TaskSummary.hpp"
#include "Task/PathSolvers/TaskDijkstraMin.hpp"
#include "Task/PathSolvers/TaskDijkstraMax.hpp"
#include "Task/ObservationZones/ObservationZoneClient.hpp"
#include "Task/ObservationZones/CylinderZone.hpp"
#include "time/BrokenTime.hpp"

/**
 * According to "FAI Sporting Code / Annex A to Section 3 - Gliding",
 * 6.3.1c and 6.3.2dii, the radius of the "start/finish ring" must be
 * subtracted from the task distance.  This flag controls whether this
 * behaviour is enabled.
 *
 * Currently, it is always enabled, but at some point, we may want to
 * make it optional.
 */
constexpr bool subtract_start_finish_cylinder_radius = true;

/**
 * Determine the cylinder radius if this is a CylinderZone.  If not,
 * return -1.
 */
[[gnu::pure]]
static double
GetCylinderRadiusOrMinusOne(const ObservationZone &oz) noexcept
{
  return oz.GetShape() == ObservationZone::Shape::CYLINDER
    ? ((const CylinderZone &)oz).GetRadius()
    : -1;
}

/**
 * Determine the cylinder radius if this is a CylinderZone.  If not,
 * return -1.
 */
[[gnu::pure]]
static double
GetCylinderRadiusOrMinusOne(const ObservationZoneClient &p) noexcept
{
  return GetCylinderRadiusOrMinusOne(p.GetObservationZone());
}

OrderedTask::OrderedTask(const TaskBehaviour &tb) noexcept
  :AbstractTask(TaskType::ORDERED, tb),
   factory_mode(tb.task_type_default),
   ordered_settings(tb.ordered_defaults)
{
  ClearName();
  active_factory = CreateTaskFactory(factory_mode, *this, task_behaviour);
  active_factory->UpdateOrderedTaskSettings(ordered_settings);
}

OrderedTask::~OrderedTask() noexcept
{
  RemoveAllPoints();
}

const TaskFactoryConstraints &
OrderedTask::GetFactoryConstraints() const noexcept
{
  return GetFactory().GetConstraints();
}

static void
SetTaskBehaviour(OrderedTask::OrderedTaskPointVector &vector,
                 const TaskBehaviour &tb) noexcept
{
  for (const auto &i : vector)
    i->SetTaskBehaviour(tb);
}

void
OrderedTask::SetTaskBehaviour(const TaskBehaviour &tb) noexcept
{
  AbstractTask::SetTaskBehaviour(tb);

  ::SetTaskBehaviour(task_points, tb);
  ::SetTaskBehaviour(optional_start_points, tb);
}

static void
UpdateObservationZones(OrderedTask::OrderedTaskPointVector &points,
                       const FlatProjection &projection) noexcept
{
  for (const auto &i : points)
    i->UpdateOZ(projection);
}

void
OrderedTask::UpdateStatsGeometry() noexcept
{
  ScanStartFinish();

  if (task_points.empty())
    stats.bounds.SetInvalid();
  else {
    // scan location of task points
    auto &first = *task_points.front();
    stats.bounds = first.GetLocation();

    for (const auto &tp : task_points)
      tp->ScanBounds(stats.bounds);

    // ... and optional start points
    for (const auto &tp : optional_start_points)
      tp->ScanBounds(stats.bounds);
  }

  stats.task_valid = !IsError(CheckTask());
  stats.has_targets = stats.task_valid && HasTargets();
  stats.is_mat = GetFactoryType() == TaskFactoryType::MAT;
  stats.has_optional_starts = stats.task_valid && HasOptionalStarts();
}

void
OrderedTask::UpdateGeometry() noexcept
{
  UpdateStatsGeometry();

  if (task_points.empty())
    return;

  auto &first = *task_points.front();

  first.ScanActive(*task_points[active_task_point]);

  task_projection = TaskProjection(stats.bounds);

  // update OZ's for items that depend on next-point geometry
  UpdateObservationZones(task_points, task_projection);
  UpdateObservationZones(optional_start_points, task_projection);

  // now that the task projection is stable, and oz is stable,
  // calculate the bounding box in projected coordinates
  for (const auto &tp : task_points)
    tp->UpdateBoundingBox(task_projection);

  for (const auto &tp : optional_start_points)
    tp->UpdateBoundingBox(task_projection);

  // update stats so data can be used during task construction
  /// @todo this should only be done if not flying! (currently done with has_entered)
  if (!task_points.front()->HasEntered()) {
    UpdateStatsDistances(GeoPoint::Invalid(), true);
    if (HasFinish()) {
      /// @todo: call AbstractTask::update stats methods with fake state
      /// so stats are updated
    }
  }

  force_full_update = true;
}

// TIMES

TimeStamp
OrderedTask::ScanTotalStartTime() noexcept
{
  if (task_points.empty())
    return TimeStamp::Undefined();

  return task_points.front()->GetScoredState().time;
}

TimeStamp
OrderedTask::ScanLegStartTime() noexcept
{
  if (active_task_point > 0)
    return task_points[active_task_point-1]->GetScoredState().time;

  return TimeStamp::Undefined();
}

// DISTANCES

inline bool
OrderedTask::RunDijsktraMin(const GeoPoint &location) noexcept
{
  const unsigned task_size = TaskSize();
  if (task_size < 2)
    return false;

  if (dijkstra_min == nullptr)
    dijkstra_min = std::make_unique<TaskDijkstraMin>();
  TaskDijkstraMin &dijkstra = *dijkstra_min;

  const unsigned active_index = GetActiveIndex();
  dijkstra.SetTaskSize(task_size - active_index);
  for (unsigned i = active_index; i != task_size; ++i) {
    const SearchPointVector &boundary = task_points[i]->GetSearchPoints();
    dijkstra.SetBoundary(i - active_index, boundary);
  }

  SearchPoint ac(location, task_projection);
  if (!dijkstra.DistanceMin(ac))
    return false;

  for (unsigned i = active_index; i != task_size; ++i)
    SetPointSearchMin(i, dijkstra.GetSolution(i - active_index));

  return true;
}

inline double
OrderedTask::ScanDistanceMin(const GeoPoint &location, bool full) noexcept
{
  if (!full && location.IsValid() && last_min_location.IsValid() &&
      DistanceIsSignificant(location, last_min_location)) {
    const TaskWaypoint *active = GetActiveTaskPoint();
    if (active != nullptr) {
      const GeoPoint &target = active->GetWaypoint().location;
      const unsigned last_distance =
        (unsigned)last_min_location.Distance(target);
      const unsigned cur_distance =
        (unsigned)location.Distance(target);

      /* do the full scan only if the distance to the active task
         point has changed by more than 5%, otherwise we don't expect
         any relevant changes */
      if (last_distance < 2000 || cur_distance < 2000 ||
          last_distance * 20 >= cur_distance * 21 ||
          cur_distance * 20 >= last_distance * 21)
        full = true;
    }
  }

  if (full) {
    RunDijsktraMin(location);
    last_min_location = location;
  }

  return task_points.front()->ScanDistanceMin();
}

inline bool
OrderedTask::RunDijsktraMax(TaskDijkstraMax &dijkstra, 
                            SearchPointVector &results, 
                            bool ignoreSampledPoints) const noexcept
{
  const unsigned task_size = TaskSize();
  if (task_size < 2)
    return false;
  dijkstra.SetTaskSize(task_size);

  const unsigned active_index = GetActiveIndex();
  for (unsigned i = 0; i != task_size; ++i) {
    const SearchPointVector &boundary = (i == active_index || ignoreSampledPoints)
      /* since one can still travel further in the current sector, use
         the full boundary here */
      ? task_points[i]->GetBoundaryPoints()
      : task_points[i]->GetSearchPoints();

    dijkstra.SetBoundary(i, boundary);
  }

  double start_radius(-1), finish_radius(-1);
  if (subtract_start_finish_cylinder_radius) {
    /* to subtract the start/finish cylinder radius, we use only the
       nominal points (i.e. the cylinder's center), and later replace
       it with a point on the cylinder boundary */

    const auto &start = *task_points.front();
    start_radius = GetCylinderRadiusOrMinusOne(start);
    if (start_radius > 0)
      dijkstra.SetBoundary(0, start.GetNominalPoints());

    const auto &finish = *task_points.back();
    finish_radius = GetCylinderRadiusOrMinusOne(finish);
    if (finish_radius > 0)
      dijkstra.SetBoundary(task_size - 1, finish.GetNominalPoints());
  }

  if (!dijkstra.DistanceMax())
    return false;

  for (unsigned i = 0; i != results.size(); ++i) {
    results[i] = dijkstra.GetSolution(i);

    if (i == 0 && start_radius > 0) {
      /* subtract start cylinder radius by finding the intersection
         with the cylinder boundary */
      const GeoPoint &current = task_points.front()->GetLocation();
      const GeoPoint &neighbour = dijkstra.GetSolution(i + 1).GetLocation();
      GeoPoint gp = current.IntermediatePoint(neighbour, start_radius);
      results[i] = SearchPoint(gp, task_projection);
    }

    if (i == task_size - 1 && finish_radius > 0) {
      /* subtract finish cylinder radius by finding the intersection
         with the cylinder boundary */
      const GeoPoint &current = task_points.back()->GetLocation();
      const GeoPoint &neighbour = dijkstra.GetSolution(i - 1).GetLocation();
      GeoPoint gp = current.IntermediatePoint(neighbour, finish_radius);
      results[i] = SearchPoint(gp, task_projection);
    }
  }

  return true;
}

inline double
OrderedTask::ScanDistanceMax() noexcept
{
  if (task_points.empty()) // nothing to do!
    return 0;

  const unsigned task_size = TaskSize();
  assert(active_task_point < task_size);

  if (dijkstra_max == nullptr)
    dijkstra_max = std::make_unique<TaskDijkstraMax>();

  SearchPointVector maxDistancePoints(task_size); 
  bool updated = RunDijsktraMax(*dijkstra_max, maxDistancePoints, false);

  if (updated) {
    for (unsigned i = 0; i < maxDistancePoints.size(); ++i) {
      SetPointSearchMax(i, maxDistancePoints[i]);
      if (i <= GetActiveIndex() )
        set_tp_search_achieved(i, maxDistancePoints[i]);
    }
  }

  return task_points.front()->ScanDistanceMax();
}

double
OrderedTask::ScanDistanceMaxTotal() noexcept
{
  if (task_points.empty()) // nothing to do!
    return 0;

  const unsigned task_size = TaskSize();
  assert(active_task_point < task_size);

  if (dijkstra_max_total == nullptr)
    dijkstra_max_total = std::make_unique<TaskDijkstraMax>();

  SearchPointVector maxDistancePoints(task_size); 
  bool updated = RunDijsktraMax(*dijkstra_max_total, maxDistancePoints, true);
  
  if (updated) {
    for (unsigned i = 0; i < maxDistancePoints.size(); ++i)
      SetPointSearchMaxTotal(i, maxDistancePoints[i]);
  }

  return task_points.front()->ScanDistanceMaxTotal();
}

void
OrderedTask::ScanDistanceMinMax(const GeoPoint &location, bool force,
                                double *dmin, double *dmax) noexcept
{
  if (force)
    *dmax = ScanDistanceMax();

  *dmin = ScanDistanceMin(location, force);
}

double
OrderedTask::ScanDistanceNominal() const noexcept
{
  if (task_points.empty())
    return 0;

  const auto &start = *task_points.front();
  auto d = start.ScanDistanceNominal();

  auto radius = GetCylinderRadiusOrMinusOne(start);
  if (radius > 0 && radius < d)
    d -= radius;

  const auto &finish = *task_points.back();
  radius = GetCylinderRadiusOrMinusOne(finish);
  if (radius > 0 && radius < d)
    d -= radius;

  return d;
}

double
OrderedTask::ScanDistanceScored(const GeoPoint &location) noexcept
{
  return task_points.empty()
    ? 0
    : task_points.front()->ScanDistanceScored(location);
}

double
OrderedTask::ScanDistanceRemaining(const GeoPoint &location) noexcept
{
  return task_points.empty()
    ? 0
    : task_points.front()->ScanDistanceRemaining(location);
}

double
OrderedTask::ScanDistanceTravelled(const GeoPoint &location) noexcept
{
  return task_points.empty()
    ? 0
    : task_points.front()->ScanDistanceTravelled(location);
}

double
OrderedTask::ScanDistancePlanned() noexcept
{
  return task_points.empty()
    ? 0
    : task_points.front()->ScanDistancePlanned();
}

unsigned
OrderedTask::GetLastIntermediateAchieved() const noexcept
{
  if (TaskSize() < 2)
    return 0;

  for (unsigned i = 1; i < TaskSize() - 1; i++)
    if (!task_points[i]->HasEntered())
      return i - 1;
  return TaskSize() - 2;
}

// TRANSITIONS

bool
OrderedTask::CheckTransitions(const AircraftState &state,
                              const AircraftState &state_last) noexcept
{
  if (!taskpoint_start)
    return false;

  taskpoint_start->ScanActive(*task_points[active_task_point]);

  if (!state.flying)
    return false;

  const int n_task = task_points.size();

  if (!n_task)
    return false;

  FlatBoundingBox bb_last(task_projection.ProjectInteger(state_last.location),
                          1);
  FlatBoundingBox bb_now(task_projection.ProjectInteger(state.location),
                         1);
  const auto last_started_time = stats.start.GetStartedTime();
  const bool last_finished = stats.task_finished;

  const int t_min = std::max(0, (int)active_task_point - 1);
  const int t_max = std::min(n_task - 1, (int)active_task_point);
  bool full_update = false;

  for (int i = t_min; i <= t_max; i++) {

    bool transition_enter = false;
    bool transition_exit = false;

    if (i==0) {
      full_update |= CheckTransitionOptionalStart(state, state_last,
                                                  bb_now, bb_last,
                                                  transition_enter,
                                                  transition_exit,
                                                  stats.pev_based_advance_ready
                                                  );
    }

    full_update |= CheckTransitionPoint(*task_points[i],
                                        state, state_last, bb_now, bb_last,
                                        transition_enter, transition_exit,
                                        stats.pev_based_advance_ready, 
                                        i == 0);

    if (i == (int)active_task_point) {
      const bool last_request_armed = task_advance.NeedToArm();

      if (task_advance.CheckReadyToAdvance(*task_points[i], state,
                                           transition_enter,
                                           transition_exit)) {
        task_advance.SetArmed(false);

        if (i + 1 < n_task) {
          i++;
          SetActiveTaskPoint(i);
          taskpoint_start->ScanActive(*task_points[active_task_point]);

          if (task_events != nullptr)
            task_events->ActiveAdvanced(*task_points[i], i);

          // on sector exit, must update samples since start sector
          // exit transition clears samples
          full_update = true;
        }
      } else if (!last_request_armed && task_advance.NeedToArm()) {
        if (task_events != nullptr)
          task_events->RequestArm(*task_points[i]);
      }
    }
  }

  stats.need_to_arm = task_advance.NeedToArm();

  taskpoint_start->ScanActive(*task_points[active_task_point]);

  stats.task_finished = taskpoint_finish != nullptr &&
    taskpoint_finish->HasEntered();

  if (TaskStarted()) {
    const AircraftState &start_state = taskpoint_start->GetExitedState();
    assert(start_state.HasTime());
    stats.start.SetStarted(start_state);
    stats.pev_based_advance_ready=false;
    if (taskpoint_finish != nullptr){
      // Calculation based on FAI finish or max_height_loss
      taskpoint_finish->SetFaiFinishHeight(taskpoint_finish->CalculateFinishHeightFromStart(stats.start.altitude));
    }

  }
  if (task_events != nullptr) {
    if (stats.start.GetStartedTime() > last_started_time)
      task_events->TaskStart();

    if (stats.task_finished && !last_finished)
      task_events->TaskFinish();
  }

  return full_update;
}

inline bool
OrderedTask::CheckTransitionOptionalStart(const AircraftState &state,
                                          const AircraftState &state_last,
                                          const FlatBoundingBox& bb_now,
                                          const FlatBoundingBox& bb_last,
                                          bool &transition_enter,
                                          bool &transition_exit,
                                          bool pev_based_advance_ready)
                                           noexcept
{
  bool full_update = false;

  for (auto begin = optional_start_points.cbegin(),
         end = optional_start_points.cend(), i = begin; i != end; ++i) {
    full_update |= CheckTransitionPoint(**i,
                                        state, state_last, bb_now, bb_last,
                                        transition_enter, transition_exit,
                                        pev_based_advance_ready, true);

    if (transition_enter || transition_exit) {
      // we have entered or exited this optional start point, so select it.
      // user has no choice in this: rules for multiple start points are that
      // the last start OZ flown through is used for scoring

      SelectOptionalStart(std::distance(begin, i));

      return full_update;
    }
  }
  return full_update;
}

bool
OrderedTask::CheckTransitionPoint(OrderedTaskPoint &point,
                                  const AircraftState &state,
                                  const AircraftState &state_last,
                                  const FlatBoundingBox &bb_now,
                                  const FlatBoundingBox &bb_last,
                                  bool &transition_enter,
                                  bool &transition_exit,
                                  const bool pev_ready_to_advance,
                                  const bool is_start) noexcept
{
  const bool nearby = point.BoundingBoxOverlaps(bb_now) ||
    point.BoundingBoxOverlaps(bb_last);

  if (nearby && point.TransitionEnter(state, state_last)) {
    transition_enter = true;

    if (task_events != nullptr)
      task_events->EnterTransition(point);
  }

  if (nearby && point.TransitionExit(state, state_last, pev_ready_to_advance, task_projection)) {
    transition_exit = true;

    if (task_events != nullptr)
      task_events->ExitTransition(point);
  }

  if (is_start)
    UpdateStartTransition(state, point);

  return nearby
    ? point.UpdateSampleNear(state, task_projection)
    : point.UpdateSampleFar(state, task_projection);
}

// ADDITIONAL FUNCTIONS

bool
OrderedTask::UpdateIdle(const AircraftState &state,
                        const GlidePolar &glide_polar) noexcept
{
  bool retval = AbstractTask::UpdateIdle(state, glide_polar);

  if (HasStart() && task_behaviour.optimise_targets_range &&
      GetOrderedTaskSettings().aat_min_time.count() > 0) {

    CalcMinTarget(state, glide_polar,
                  GetOrderedTaskSettings().aat_min_time + task_behaviour.optimise_targets_margin);

    if (task_behaviour.optimise_targets_bearing &&
        task_points[active_task_point]->GetType() == TaskPointType::AAT) {
      TaskPointList tps(task_points);
      AATPoint *ap = (AATPoint *)task_points[active_task_point].get();
      // very nasty hack
      TaskOptTarget tot(tps, active_task_point, state,
                        task_behaviour.glide, glide_polar,
                        *ap, task_projection, *taskpoint_start);
      tot.search(0.5);
    }
    retval = true;
  }

  return retval;
}


void OrderedTask::UpdateAfterPEV(const AircraftState &state, BrokenTime bt) noexcept {

	 pev_received=false;
	 if (state.time.ToDuration().count()<0) {
		 return;
	 }

      //BrokenTime bt = BrokenTime::FromSecondOfDay(state.time);
      RoughTime new_start = RoughTime::FromSinceMidnight(state.time.ToDuration());
      RoughTime new_end = RoughTime::Invalid();
      OrderedTaskSettings &ots =
          ordered_settings;
      const StartConstraints &start = ots.start_constraints;

      if (start.score_pev){

          // to be added confirmation dialog in case PEV events more often than configured time window
          //ots.start_constraints.closed_substart_time_span = state.time+std::chrono::seconds(RoughTimeDelta::FromDuration(start.pev_start_window).AsSeconds());

          stats.pev_based_advance_ready=true;

          if (start.pev_start_wait_time.count() > 0) {
                    auto t = std::chrono::duration_cast<std::chrono::minutes>(start.pev_start_wait_time);
                    // Set start time to the next full minute after wait time.
                    // This way we make sure wait time is passed before xcsoar opens the start.
                    if (bt.second > 0)
                      t += std::chrono::minutes{1};
                    new_start = new_start + RoughTimeDelta::FromDuration(t);
                  }
          // in this case we use only wait time to force waiting for next window
          // start window end is not limited as the start should occure at PEV when reaching start zone after PEV
          const RoughTimeSpan ts = RoughTimeSpan(new_start, new_end);

          ots.start_constraints.open_time_span=ts;




      }else
      {
        
        if (start.pev_start_wait_time.count() > 0) {
          auto t = std::chrono::duration_cast<std::chrono::minutes>(start.pev_start_wait_time);
          // Set start time to the next full minute after wait time.
          // This way we make sure wait time is passed before xcsoar opens the start.
          if (bt.second > 0)
            t += std::chrono::minutes{1};
          new_start = new_start + RoughTimeDelta::FromDuration(t);
        }

        if (start.pev_start_window.count() > 0) {
          new_end = new_start + RoughTimeDelta::FromDuration(start.pev_start_window);
        }
        const RoughTimeSpan ts = RoughTimeSpan(new_start, new_end);

        ots.start_constraints.open_time_span=ts;

      
      }
       
}

bool OrderedTask::SetPEV(const BrokenTime bt) {
   //Use state time instead of system time in updating information related to PEV inside Task
   if (!last_state_time.IsDefined())return false;

 
 	if (taskpoint_start){

 		  if ((taskpoint_start->GetScorePEV())&&!(ordered_settings.start_constraints.open_time_span.HasBegun(RoughTime{last_state_time})))
 		    // the start gate is not yet open when we left the OZ
 		    return false;
 	}
  
  pev_received=true;
  pev_receive_time=bt;
  return true;

 };

bool
OrderedTask::UpdateSample(const AircraftState &state,
                          [[maybe_unused]] const GlidePolar &glide_polar,
                          [[maybe_unused]] const bool full_update) noexcept
{
  assert(state.location.IsValid());

  stats.inside_oz = active_task_point < task_points.size() &&
    task_points[active_task_point]->IsInSector(state);

  return true;
}

// TASK

void
OrderedTask::SetNeighbours(unsigned position) noexcept
{
  OrderedTaskPoint* prev = nullptr;
  OrderedTaskPoint* next = nullptr;

  if (position >= task_points.size())
    // nothing to do
    return;

  if (position > 0)
    prev = task_points[position - 1].get();

  if (position + 1 < task_points.size())
    next = task_points[position + 1].get();

  task_points[position]->SetNeighbours(prev, next);

  if (position==0) {
    for (const auto &tp : optional_start_points)
      tp->SetNeighbours(prev, next);
  }
}

TaskValidationErrorSet
OrderedTask::CheckTask() const noexcept
{
  return this->GetFactory().Validate();
}

AATPoint*
OrderedTask::GetAATTaskPoint(unsigned TPindex) const noexcept
{
 if (TPindex > task_points.size() - 1) {
   return nullptr;
 }

 if (task_points[TPindex]->GetType() == TaskPointType::AAT)
   return (AATPoint *)task_points[TPindex].get();
 else
   return (AATPoint *)nullptr;
}

inline bool
OrderedTask::ScanStartFinish() noexcept
{
  /// @todo also check there are not more than one start/finish point
  if (task_points.empty()) {
    taskpoint_start = nullptr;
    taskpoint_finish = nullptr;
    return false;
  }

  taskpoint_start = task_points.front()->GetType() == TaskPointType::START
    ? (StartPoint *)task_points.front().get()
    : nullptr;

  taskpoint_finish = task_points.size() > 1 &&
    task_points.back()->GetType() == TaskPointType::FINISH
    ? (FinishPoint *)task_points.back().get()
    : nullptr;

  return HasStart() && HasFinish();
}

inline void
OrderedTask::ErasePoint(const unsigned index) noexcept
{
  task_points.erase(task_points.begin() + index);
}

inline void
OrderedTask::EraseOptionalStartPoint(const unsigned index) noexcept
{
  optional_start_points.erase(optional_start_points.begin() + index);
}

bool
OrderedTask::Remove(const unsigned position) noexcept
{
  if (position >= task_points.size())
    return false;

  if (active_task_point > position ||
      (active_task_point > 0 && active_task_point == task_points.size() - 1))
    active_task_point--;

  ErasePoint(position);

  if (position < task_points.size())
    SetNeighbours(position);

  if (position)
    SetNeighbours(position - 1);

  return true;
}

bool
OrderedTask::RemoveOptionalStart(const unsigned position) noexcept
{
  if (position >= optional_start_points.size())
    return false;

  EraseOptionalStartPoint(position);

  if (task_points.size()>1)
    SetNeighbours(0);

  return true;
}

bool
OrderedTask::Append(const OrderedTaskPoint &new_tp) noexcept
{
  if (!task_points.empty() &&
      (/* is the new_tp allowed in this context? */
       !new_tp.IsPredecessorAllowed() ||
       /* can a tp be appended after the last one? */
       !task_points.back()->IsSuccessorAllowed()))
    return false;

  const unsigned i = task_points.size();
  task_points.emplace_back(new_tp.Clone(task_behaviour, ordered_settings));
  if (i > 0)
    SetNeighbours(i - 1);
  else {
    // give it a value when we have one tp so it is not uninitialised
    last_min_location = new_tp.GetLocation();
  }

  SetNeighbours(i);
  return true;
}

bool
OrderedTask::AppendOptionalStart(const OrderedTaskPoint &new_tp) noexcept
{
  optional_start_points.emplace_back(new_tp.Clone(task_behaviour,
                                                  ordered_settings));
  if (task_points.size() > 1)
    SetNeighbours(0);
  return true;
}

bool
OrderedTask::Insert(const OrderedTaskPoint &new_tp,
                    const unsigned position) noexcept
{
  if (position >= task_points.size())
    return Append(new_tp);

  if (/* is the new_tp allowed in this context? */
      (position > 0 && !new_tp.IsPredecessorAllowed()) ||
      !new_tp.IsSuccessorAllowed() ||
      /* can a tp be inserted at this position? */
      (position > 0 && !task_points[position - 1]->IsSuccessorAllowed()) ||
      !task_points[position]->IsPredecessorAllowed())
    return false;

  if (active_task_point >= position)
    active_task_point++;

  task_points.insert(task_points.begin() + position,
                     new_tp.Clone(task_behaviour, ordered_settings));

  if (position)
    SetNeighbours(position - 1);

  SetNeighbours(position);
  SetNeighbours(position + 1);

  return true;
}

bool
OrderedTask::Replace(const OrderedTaskPoint &new_tp,
                     const unsigned position) noexcept
{
  if (position >= task_points.size())
    return false;

  if (task_points[position]->Equals(new_tp))
    // nothing to do
    return true;

  /* is the new_tp allowed in this context? */
  if ((position > 0 && !new_tp.IsPredecessorAllowed()) ||
      (position + 1 < task_points.size() && !new_tp.IsSuccessorAllowed()))
    return false;

  task_points[position] = new_tp.Clone(task_behaviour, ordered_settings);

  if (position)
    SetNeighbours(position - 1);

  SetNeighbours(position);
  if (position + 1 < task_points.size())
    SetNeighbours(position + 1);

  return true;
}


bool
OrderedTask::ReplaceOptionalStart(const OrderedTaskPoint &new_tp,
                                  const unsigned position) noexcept
{
  if (position >= optional_start_points.size())
    return false;

  if (optional_start_points[position]->Equals(new_tp))
    // nothing to do
    return true;

  optional_start_points[position] = new_tp.Clone(task_behaviour,
                                                 ordered_settings);

  SetNeighbours(0);
  return true;
}


void
OrderedTask::SetActiveTaskPoint(unsigned index) noexcept
{
  if (index >= task_points.size() || index == active_task_point)
    return;

  task_advance.SetArmed(false);
  active_task_point = index;
  force_full_update = true;
}

TaskWaypoint*
OrderedTask::GetActiveTaskPoint() const noexcept
{
  if (active_task_point < task_points.size())
    return task_points[active_task_point].get();

  return nullptr;
}

bool
OrderedTask::IsValidTaskPoint(const int index_offset) const noexcept
{
  unsigned index = active_task_point + index_offset;
  return (index < task_points.size());
}

void
OrderedTask::GlideSolutionRemaining(const AircraftState &aircraft,
                                    const GlidePolar &polar,
                                    GlideResult &total,
                                    GlideResult &leg) noexcept
{
  if (!aircraft.location.IsValid() || task_points.empty()) {
    total.Reset();
    leg.Reset();
    return;
  }

  TaskPointList tps(task_points);
  TaskMacCreadyRemaining tm(tps.begin(), tps.end(),
                            active_task_point,
                            task_behaviour.glide, polar);
  total = tm.glide_solution(aircraft);
  leg = tm.get_active_solution();
}

void
OrderedTask::GlideSolutionTravelled(const AircraftState &aircraft,
                                    const GlidePolar &glide_polar,
                                    GlideResult &total,
                                    GlideResult &leg) noexcept
{
  if (!aircraft.location.IsValid() || task_points.empty()) {
    total.Reset();
    leg.Reset();
    return;
  }

  TaskPointList tps(task_points);
  TaskMacCreadyTravelled tm(tps.begin(), active_task_point,
                            task_behaviour.glide, glide_polar);
  total = tm.glide_solution(aircraft);
  leg = tm.get_active_solution();
}

void
OrderedTask::GlideSolutionPlanned(const AircraftState &aircraft,
                                  const GlidePolar &glide_polar,
                                  GlideResult &total,
                                  GlideResult &leg,
                                  DistanceStat &total_remaining_effective,
                                  DistanceStat &leg_remaining_effective,
                                  const GlideResult &solution_remaining_total,
                                  const GlideResult &solution_remaining_leg) noexcept
{
  if (task_points.empty()) {
    total.Reset();
    leg.Reset();
    total_remaining_effective.Reset();
    leg_remaining_effective.Reset();
    return;
  }

  TaskPointList tps(task_points);
  TaskMacCreadyTotal tm(tps.begin(), tps.end(),
                        active_task_point,
                        task_behaviour.glide, glide_polar);
  total = tm.glide_solution(aircraft);
  leg = tm.get_active_solution();

  if (solution_remaining_total.IsOk())
    total_remaining_effective.SetDistance(tm.effective_distance(solution_remaining_total.time_elapsed));
  else
    total_remaining_effective.Reset();

  if (solution_remaining_leg.IsOk())
    leg_remaining_effective.SetDistance(tm.effective_leg_distance(solution_remaining_leg.time_elapsed));
  else
    leg_remaining_effective.Reset();
}

// Auxiliary glide functions

double
OrderedTask::CalcRequiredGlide(const AircraftState &aircraft,
                               const GlidePolar &glide_polar) const noexcept
{
  TaskPointList tps(task_points);
  TaskGlideRequired bgr(tps, active_task_point, aircraft,
                        task_behaviour.glide, glide_polar);
  return bgr.search(0);
}

bool
OrderedTask::CalcBestMC(const AircraftState &aircraft,
                        const GlidePolar &glide_polar,
                        double &best) const noexcept
{
  // note setting of lower limit on mc
  TaskPointList tps(task_points);
  TaskBestMc bmc(tps, active_task_point, aircraft,
                 task_behaviour.glide, glide_polar);
  return bmc.search(glide_polar.GetMC(), best);
}


bool
OrderedTask::AllowIncrementalBoundaryStats(const AircraftState &aircraft) const noexcept
{
  if (active_task_point == 0)
    /* disabled for the start point */
    return false;

  if (task_points[active_task_point]->IsBoundaryScored())
    return true;

  bool in_sector = task_points[active_task_point]->IsInSector(aircraft) ||
    task_points[active_task_point-1]->IsInSector(aircraft);

  return !in_sector;
}

bool
OrderedTask::CalcCruiseEfficiency(const AircraftState &aircraft,
                                  const GlidePolar &glide_polar,
                                  double &val) const noexcept
{
  if (AllowIncrementalBoundaryStats(aircraft)) {
    TaskPointList tps(task_points);
    TaskCruiseEfficiency bce(tps, active_task_point, aircraft,
                             task_behaviour.glide, glide_polar);
    val = bce.search(1);
    return true;
  } else {
    val = 1;
    return false;
  }
}

bool
OrderedTask::CalcEffectiveMC(const AircraftState &aircraft,
                             const GlidePolar &glide_polar,
                             double &val) const noexcept
{
  if (AllowIncrementalBoundaryStats(aircraft)) {
    TaskPointList tps(task_points);
    TaskEffectiveMacCready bce(tps, active_task_point, aircraft,
                               task_behaviour.glide, glide_polar);
    val = bce.search(glide_polar.GetMC());
    return true;
  } else {
    val = glide_polar.GetMC();
    return false;
  }
}


inline double
OrderedTask::CalcMinTarget(const AircraftState &aircraft,
                           const GlidePolar &glide_polar,
                           const FloatDuration t_target) noexcept
{
  if (stats.has_targets) {
    // only perform scan if modification is possible
    const auto t_rem = fdim(t_target, stats.total.time_elapsed);

    TaskPointList tps(task_points);
    TaskMinTarget bmt(tps, active_task_point, aircraft,
                      task_behaviour.glide, glide_polar,
                      t_rem, *taskpoint_start);
    auto p = bmt.search(0);
    return p;
  }

  return 0;
}

double
OrderedTask::CalcGradient(const AircraftState &state) const noexcept
{
  if (task_points.empty())
    return 0;

  // Iterate through remaining turnpoints
  double distance = 0;
  for (const auto &tp : task_points)
    // Sum up the leg distances
    distance += tp->GetVectorRemaining(state.location).distance;

  if (distance <= 0)
    return 0;

  // Calculate gradient to the last turnpoint of the remaining task
  return (state.altitude - task_points.back()->GetElevation()) / distance;
}

static void
Visit(const OrderedTask::OrderedTaskPointVector &points,
      TaskPointConstVisitor &visitor)
{
  for (const auto &tp : points)
    visitor.Visit(*tp);
}

void
OrderedTask::AcceptTaskPointVisitor(TaskPointConstVisitor& visitor) const
{
  Visit(task_points, visitor);
}

static void
ResetPoints(OrderedTask::OrderedTaskPointVector &points) noexcept
{
  for (auto &i : points)
    i->Reset();
}

void
OrderedTask::Reset() noexcept
{
  /// @todo also reset data in this class e.g. stats?
  ResetPoints(task_points);
  ResetPoints(optional_start_points);

  AbstractTask::Reset();
  stats.task_finished = false;
  stats.start.Reset();
  task_advance.Reset();
  SetActiveTaskPoint(0);
  UpdateStatsGeometry();
}

bool
OrderedTask::TaskStarted(bool soft) const noexcept
{
  if (taskpoint_start) {
    // have we really started?
    if (taskpoint_start->HasExited())
      return true;

    // if soft starts allowed, consider started if we progressed to next tp
    if (soft && (active_task_point>0))
      return true;
  }

  return false;
}

/**
 * Test whether two points (as previous search locations) are significantly
 * different to warrant a new search
 *
 * @param a1 First point to compare
 * @param a2 Second point to compare
 * @param dist_threshold Threshold distance for significance
 *
 * @return True if distance is significant
 */
[[gnu::pure]]
static bool
DistanceIsSignificant(const SearchPoint &a1, const SearchPoint &a2,
                      const unsigned dist_threshold = 1) noexcept
{
  return a1.FlatSquareDistanceTo(a2) > (dist_threshold * dist_threshold);
}

inline bool
OrderedTask::DistanceIsSignificant(const GeoPoint &location,
                                   const GeoPoint &location_last) const noexcept
{
  SearchPoint a1(location, task_projection);
  SearchPoint a2(location_last, task_projection);
  return ::DistanceIsSignificant(a1, a2);
}


const SearchPointVector &
OrderedTask::GetPointSearchPoints(unsigned tp) const noexcept
{
  return task_points[tp]->GetSearchPoints();
}

void
OrderedTask::SetPointSearchMin(unsigned tp, const SearchPoint &sol) noexcept
{
  task_points[tp]->SetSearchMin(sol);
}

void
OrderedTask::set_tp_search_achieved(unsigned tp, const SearchPoint &sol) noexcept
{
  if (task_points[tp]->HasSampled())
    SetPointSearchMin(tp, sol);
}

void
OrderedTask::SetPointSearchMax(unsigned tp, const SearchPoint &sol) noexcept
{
  task_points[tp]->SetSearchMax(sol);
}

void
OrderedTask::SetPointSearchMaxTotal(unsigned tp, const SearchPoint &sol) noexcept
{
  task_points[tp]->SetSearchMaxTotal(sol);
}

bool
OrderedTask::IsFull() const noexcept
{
  return TaskSize() >= GetFactory().GetConstraints().max_points;
}

inline void
OrderedTask::UpdateStartTransition(const AircraftState &state,
                                   OrderedTaskPoint &start) noexcept
{
  if (active_task_point == 0) {
    // find boundary point that produces shortest
    // distance from state to that point to next tp point
    taskpoint_start->find_best_start(state, *task_points[1], task_projection);
  } else if (!start.HasExited() && !start.IsInSector(state)) {
    start.Reset();
    // reset on invalid transition to outside
    // point to nominal start point
  }
  // @todo: modify this for optional start?
}

bool
OrderedTask::HasTargets() const noexcept
{
  for (const auto &tp : task_points)
    if (tp->HasTarget())
      return true;

  return false;
}

std::unique_ptr<OrderedTask>
OrderedTask::Clone(const TaskBehaviour &tb) const noexcept
{
  auto new_task = std::make_unique<OrderedTask>(tb);

  new_task->SetFactory(factory_mode);

  new_task->ordered_settings = ordered_settings;

  for (const auto &tp : task_points)
    new_task->Append(*tp);

  for (const auto &tp : optional_start_points)
    new_task->AppendOptionalStart(*tp);

  new_task->active_task_point = active_task_point;
  new_task->UpdateGeometry();

  new_task->SetName(GetName());

  return new_task;
}

void
OrderedTask::CheckDuplicateWaypoints(Waypoints& waypoints,
                                     OrderedTaskPointVector& points,
                                     const bool is_task) noexcept
{
  for (auto begin = points.cbegin(), end = points.cend(), i = begin;
       i != end; ++i) {
    auto wp = waypoints.CheckExistsOrAppend((*i)->GetWaypointPtr());

    const auto new_tp =
      (*i)->Clone(task_behaviour, ordered_settings, std::move(wp));
    if (is_task)
      Replace(*new_tp, std::distance(begin, i));
    else
      ReplaceOptionalStart(*new_tp, std::distance(begin, i));
  }
}

void
OrderedTask::CheckDuplicateWaypoints(Waypoints &waypoints) noexcept
{
  CheckDuplicateWaypoints(waypoints, task_points, true);
  CheckDuplicateWaypoints(waypoints, optional_start_points, false);
}

bool
OrderedTask::Commit(const OrderedTask &that) noexcept
{
  bool modified = false;

  SetName(that.GetName());

  // change mode to that one
  SetFactory(that.factory_mode);

  // copy across behaviour
  SetOrderedTaskSettings(that.ordered_settings);

  // remove if that task is smaller than this one
  while (TaskSize() > that.TaskSize()) {
    Remove(TaskSize() - 1);
    modified = true;
  }

  // ensure each task point made identical
  for (unsigned i = 0; i < that.TaskSize(); ++i) {
    if (i >= TaskSize()) {
      // that task is larger than this
      Append(*that.task_points[i]);
      modified = true;
    } else if (!task_points[i]->Equals(*that.task_points[i])) {
      // that task point is changed
      Replace(*that.task_points[i], i);
      modified = true;
    }
  }

  // remove if that optional start list is smaller than this one
  while (optional_start_points.size() > that.optional_start_points.size()) {
    RemoveOptionalStart(optional_start_points.size() - 1);
    modified = true;
  }

  // ensure each task point made identical
  for (unsigned i = 0; i < that.optional_start_points.size(); ++i) {
    if (i >= optional_start_points.size()) {
      // that task is larger than this
      AppendOptionalStart(*that.optional_start_points[i]);
      modified = true;
    } else if (!optional_start_points[i]->Equals(*that.optional_start_points[i])) {
      // that task point is changed
      ReplaceOptionalStart(*that.optional_start_points[i], i);
      modified = true;
    }
  }

  if (modified)
    UpdateGeometry();
    // @todo also re-scan task sample state,
    // potentially resetting task

  return modified;
}

bool
OrderedTask::RelocateOptionalStart(const unsigned position,
                                   WaypointPtr &&waypoint) noexcept
{
  if (position >= optional_start_points.size())
    return false;

  optional_start_points[position] =
    optional_start_points[position]->Clone(task_behaviour, ordered_settings,
                                           std::move(waypoint));
  return true;
}

bool
OrderedTask::Relocate(const unsigned position, WaypointPtr &&waypoint) noexcept
{
  if (position >= TaskSize())
    return false;

  auto new_tp = task_points[position]->Clone(task_behaviour,
                                             ordered_settings,
                                             std::move(waypoint));
  bool success = Replace(*new_tp, position);
  return success;
}

void
OrderedTask::SetFactory(const TaskFactoryType the_factory) noexcept
{
  // detect no change
  if (factory_mode == the_factory)
    return;

  if (the_factory != TaskFactoryType::MIXED) {
    // can switch from anything to mixed, otherwise need reset
    Reset();

    /// @todo call into task_events to ask if reset is desired on
    /// factory change
  }
  factory_mode = the_factory;

  active_factory = CreateTaskFactory(factory_mode, *this, task_behaviour);
  active_factory->UpdateOrderedTaskSettings(ordered_settings);

  PropagateOrderedTaskSettings();
}

void
OrderedTask::SetOrderedTaskSettings(const OrderedTaskSettings &ob) noexcept
{
  ordered_settings = ob;

  PropagateOrderedTaskSettings();
}

void
OrderedTask::PropagateOrderedTaskSettings() noexcept
{
  for (auto &tp : task_points)
    tp->SetOrderedTaskSettings(ordered_settings);

  for (auto &tp : optional_start_points)
    tp->SetOrderedTaskSettings(ordered_settings);

  //Update finish height in case it is based on started altitude
  if (taskpoint_start!=nullptr && taskpoint_finish!=nullptr)
    	  if (taskpoint_start->GetActiveState()==OrderedTaskPoint::BEFORE_ACTIVE){
    		  taskpoint_finish->SetFaiFinishHeight(taskpoint_finish->CalculateFinishHeightFromStart(stats.start.altitude));
  }
}

bool
OrderedTask::IsScored() const noexcept
{
  return GetFactoryConstraints().task_scored;
}

std::vector<TaskFactoryType>
OrderedTask::GetFactoryTypes([[maybe_unused]] bool all) const noexcept
{
  /// @todo: check transform types if all=false
  std::vector<TaskFactoryType> f_list;
  f_list.push_back(TaskFactoryType::RACING);
  f_list.push_back(TaskFactoryType::AAT);
  f_list.push_back(TaskFactoryType::MAT);
  f_list.push_back(TaskFactoryType::FAI_GENERAL);
  return f_list;
}

void
OrderedTask::RemoveAllPoints() noexcept
{
  task_points.clear();
  optional_start_points.clear();

  active_task_point = 0;
  taskpoint_start = nullptr;
  taskpoint_finish = nullptr;
  force_full_update = true;
}

void
OrderedTask::Clear() noexcept
{
  RemoveAllPoints();

  ClearName();

  Reset();
  ordered_settings = task_behaviour.ordered_defaults;
  active_factory->UpdateOrderedTaskSettings(ordered_settings);
}

void
OrderedTask::RotateOptionalStarts() noexcept
{
  if (IsEmpty() || optional_start_points.empty())
    return;

  SelectOptionalStart(0);
}

void
OrderedTask::SelectOptionalStart(unsigned pos) noexcept
{
  assert(pos< optional_start_points.size());

  // put task start onto end
  optional_start_points.push_back(std::move(task_points.front()));
  // set task start from top optional item
  task_points.front() = std::move(optional_start_points[pos]);
  // remove top optional item from list
  optional_start_points.erase(optional_start_points.begin()+pos);

  // update neighbour links
  SetNeighbours(0);
  if (task_points.size()>1)
    SetNeighbours(1);

  // we've changed the task, so update geometry
  UpdateGeometry();
}

void
OrderedTask::UpdateSummary(TaskSummary& ordered_summary) const noexcept
{
  ordered_summary.clear();

  ordered_summary.active = active_task_point;

  bool first = true;
  for (const auto &tpp : task_points) {
    const OrderedTaskPoint &tp = *tpp;

    TaskSummaryPoint tsp;
    tsp.d_planned = tp.GetVectorPlanned().distance;
    if (first) {
      first = false;
      tsp.achieved = tp.HasExited();
    } else {
      tsp.achieved = tp.HasSampled();
    }
    ordered_summary.append(tsp);
  }

  if (stats.total.remaining.IsDefined() && stats.total.planned.IsDefined())
    ordered_summary.update(stats.total.remaining.GetDistance(),
                           stats.total.planned.GetDistance());
}
