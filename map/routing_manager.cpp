#include "routing_manager.hpp"

#include "map/chart_generator.hpp"
#include "map/routing_mark.hpp"
#include "map/mwm_tree.hpp"

#include "private.h"

#include "tracking/reporter.hpp"

#include "routing/index_router.hpp"
#include "routing/num_mwm_id.hpp"
#include "routing/online_absent_fetcher.hpp"
#include "routing/road_graph_router.hpp"
#include "routing/route.hpp"
#include "routing/routing_algorithm.hpp"
#include "routing/routing_helpers.hpp"

#include "drape_frontend/drape_engine.hpp"

#include "indexer/map_style_reader.hpp"
#include "indexer/scales.hpp"

#include "storage/country_info_getter.hpp"
#include "storage/storage.hpp"

#include "coding/file_reader.hpp"
#include "coding/file_writer.hpp"
#include "coding/multilang_utf8_string.hpp"

#include "platform/country_file.hpp"
#include "platform/mwm_traits.hpp"
#include "platform/platform.hpp"
#include "platform/socket.hpp"

#include "base/scope_guard.hpp"

#include "3party/Alohalytics/src/alohalytics.h"
#include "3party/jansson/myjansson.hpp"

#include <map>

using namespace routing;

namespace
{
//#define SHOW_ROUTE_DEBUG_MARKS

char const kRouterTypeKey[] = "router";

double const kRouteScaleMultiplier = 1.5;

std::string const kRoutePointsFile = "route_points.dat";

uint32_t constexpr kInvalidTransactionId = 0;

void FillTurnsDistancesForRendering(std::vector<RouteSegment> const & segments,
                                    std::vector<double> & turns)
{
  using namespace routing::turns;
  turns.clear();
  turns.reserve(segments.size());
  for (auto const & s : segments)
  {
    auto const & t = s.GetTurn();
    CHECK_NOT_EQUAL(t.m_turn, CarDirection::Count, ());
    // We do not render some of turn directions.
    if (t.m_turn == CarDirection::None || t.m_turn == CarDirection::StartAtEndOfStreet ||
        t.m_turn == CarDirection::StayOnRoundAbout || t.m_turn == CarDirection::TakeTheExit ||
        t.m_turn == CarDirection::ReachedYourDestination)
    {
      continue;
    }
    turns.push_back(s.GetDistFromBeginningMerc());
  }
}

void FillTrafficForRendering(std::vector<RouteSegment> const & segments,
                             std::vector<traffic::SpeedGroup> & traffic)
{
  traffic.clear();
  traffic.reserve(segments.size());
  for (auto const & s : segments)
    traffic.push_back(s.GetTraffic());
}

RouteMarkData GetLastPassedPoint(std::vector<RouteMarkData> const & points)
{
  ASSERT_GREATER_OR_EQUAL(points.size(), 2, ());
  ASSERT(points[0].m_pointType == RouteMarkType::Start, ());
  RouteMarkData data = points[0];

  for (int i = static_cast<int>(points.size()) - 1; i >= 0; i--)
  {
    if (points[i].m_isPassed)
    {
      data = points[i];
      break;
    }
  }

  // Last passed point will be considered as start point.
  data.m_pointType = RouteMarkType::Start;
  data.m_intermediateIndex = 0;
  if (data.m_isMyPosition)
  {
    MyPositionMarkPoint * myPosMark = UserMarkContainer::UserMarkForMyPostion();
    data.m_position = myPosMark->GetPivot();
    data.m_isMyPosition = false;
  }

  return data;
}

void SerializeRoutePoint(json_t * node, RouteMarkData const & data)
{
  ASSERT(node != nullptr, ());
  ToJSONObject(*node, "type", static_cast<int>(data.m_pointType));
  ToJSONObject(*node, "title", data.m_title);
  ToJSONObject(*node, "subtitle", data.m_subTitle);
  ToJSONObject(*node, "x", data.m_position.x);
  ToJSONObject(*node, "y", data.m_position.y);
}

RouteMarkData DeserializeRoutePoint(json_t * node)
{
  ASSERT(node != nullptr, ());
  RouteMarkData data;

  int type = 0;
  FromJSONObject(node, "type", type);
  data.m_pointType = static_cast<RouteMarkType>(type);

  FromJSONObject(node, "title", data.m_title);
  FromJSONObject(node, "subtitle", data.m_subTitle);

  FromJSONObject(node, "x", data.m_position.x);
  FromJSONObject(node, "y", data.m_position.y);

  return data;
}

std::string SerializeRoutePoints(std::vector<RouteMarkData> const & points)
{
  ASSERT_GREATER_OR_EQUAL(points.size(), 2, ());
  auto pointsNode = my::NewJSONArray();

  // Save last passed point. It will be used on points loading if my position
  // isn't determined.
  auto lastPassedPoint = GetLastPassedPoint(points);
  auto lastPassedNode = my::NewJSONObject();
  SerializeRoutePoint(lastPassedNode.get(), lastPassedPoint);
  json_array_append_new(pointsNode.get(), lastPassedNode.release());

  for (auto const & p : points)
  {
    // Here we skip passed points and the start point.
    if (p.m_isPassed || p.m_pointType == RouteMarkType::Start)
      continue;

    auto pointNode = my::NewJSONObject();
    SerializeRoutePoint(pointNode.get(), p);
    json_array_append_new(pointsNode.get(), pointNode.release());
  }
  std::unique_ptr<char, JSONFreeDeleter> buffer(
    json_dumps(pointsNode.get(), JSON_COMPACT | JSON_ENSURE_ASCII));
  return std::string(buffer.get());
}

std::vector<RouteMarkData> DeserializeRoutePoints(std::string const & data)
{
  my::Json root(data.c_str());

  if (root.get() == nullptr || !json_is_array(root.get()))
    return {};

  size_t const sz = json_array_size(root.get());
  if (sz == 0)
    return {};

  std::vector<RouteMarkData> result;
  result.reserve(sz);
  for (size_t i = 0; i < sz; ++i)
  {
    auto pointNode = json_array_get(root.get(), i);
    if (pointNode == nullptr)
      continue;

    auto point = DeserializeRoutePoint(pointNode);
    if (point.m_position.EqualDxDy(m2::PointD::Zero(), 1e-7))
      continue;

    result.push_back(std::move(point));
  }

  if (result.size() < 2)
    return {};

  return result;
}

VehicleType GetVehicleType(RouterType routerType)
{
  switch (routerType)
  {
  case RouterType::Pedestrian: return VehicleType::Pedestrian;
  case RouterType::Bicycle: return VehicleType::Bicycle;
  case RouterType::Vehicle:
  case RouterType::Taxi: return VehicleType::Car;
  case RouterType::Count: CHECK(false, ("Invalid type", routerType)); return VehicleType::Count;
  }
}
}  // namespace

namespace marketing
{
char const * const kRoutingCalculatingRoute = "Routing_CalculatingRoute";
}  // namespace marketing

RoutingManager::RoutingManager(Callbacks && callbacks, Delegate & delegate)
  : m_callbacks(std::move(callbacks))
  , m_delegate(delegate)
  , m_trackingReporter(platform::CreateSocket(), TRACKING_REALTIME_HOST, TRACKING_REALTIME_PORT,
                       tracking::Reporter::kPushDelayMs)
{
  auto const routingStatisticsFn = [](std::map<std::string, std::string> const & statistics) {
    alohalytics::LogEvent("Routing_CalculatingRoute", statistics);
    GetPlatform().GetMarketingService().SendMarketingEvent(marketing::kRoutingCalculatingRoute, {});
  };

  m_routingSession.Init(routingStatisticsFn, [this](m2::PointD const & pt)
  {
#ifdef SHOW_ROUTE_DEBUG_MARKS
    if (m_bmManager == nullptr)
      return;
    auto & controller = m_bmManager->GetUserMarksController(UserMarkType::DEBUG_MARK);
    controller.SetIsVisible(true);
    controller.SetIsDrawable(true);
    controller.CreateUserMark(pt);
    controller.NotifyChanges();
#endif
  });

  m_routingSession.SetReadyCallbacks(
      [this](Route const & route, IRouter::ResultCode code) { OnBuildRouteReady(route, code); },
      [this](Route const & route, IRouter::ResultCode code) { OnRebuildRouteReady(route, code); });

  m_routingSession.SetCheckpointCallback([this](size_t passedCheckpointIdx)
  {
    GetPlatform().RunOnGuiThread([this, passedCheckpointIdx]()
    {
      size_t const pointsCount = GetRoutePointsCount();

      // TODO(@bykoianko). Since routing system may invoke callbacks from different threads and here
      // we have to use gui thread, ASSERT is not correct. Uncomment it and delete condition after
      // refactoring of threads usage in routing system.
      //ASSERT_LESS(passedCheckpointIdx, pointsCount, ());
      if (passedCheckpointIdx >= pointsCount)
        return;

      if (passedCheckpointIdx == 0)
        OnRoutePointPassed(RouteMarkType::Start, 0);
      else if (passedCheckpointIdx + 1 == pointsCount)
        OnRoutePointPassed(RouteMarkType::Finish, 0);
      else
        OnRoutePointPassed(RouteMarkType::Intermediate, static_cast<int8_t>(passedCheckpointIdx - 1));
    });
  });
}

void RoutingManager::SetBookmarkManager(BookmarkManager * bmManager)
{
  m_bmManager = bmManager;
}

void RoutingManager::OnBuildRouteReady(Route const & route, IRouter::ResultCode code)
{
  // Hide preview.
  if (m_drapeEngine != nullptr)
    m_drapeEngine->RemoveAllRoutePreviewSegments();

  storage::TCountriesVec absentCountries;
  if (code == IRouter::NoError)
  {
    InsertRoute(route);
    if (m_drapeEngine != nullptr)
      m_drapeEngine->StopLocationFollow();

    // Validate route (in case of bicycle routing it can be invalid).
    ASSERT(route.IsValid(), ());
    if (route.IsValid())
    {
      m2::RectD routeRect = route.GetPoly().GetLimitRect();
      routeRect.Scale(kRouteScaleMultiplier);
      if (m_drapeEngine != nullptr)
      {
        m_drapeEngine->SetModelViewRect(routeRect, true /* applyRotation */, -1 /* zoom */,
                                        true /* isAnim */);
      }
    }
  }
  else
  {
    absentCountries.assign(route.GetAbsentCountries().begin(), route.GetAbsentCountries().end());

    if (code != IRouter::NeedMoreMaps)
      RemoveRoute(true /* deactivateFollowing */);
  }
  CallRouteBuilded(code, absentCountries);
}

void RoutingManager::OnRebuildRouteReady(Route const & route, IRouter::ResultCode code)
{
  // Hide preview.
  if (m_drapeEngine != nullptr)
    m_drapeEngine->RemoveAllRoutePreviewSegments();

  if (code != IRouter::NoError)
    return;

  InsertRoute(route);
  CallRouteBuilded(code, storage::TCountriesVec());
}

void RoutingManager::OnRoutePointPassed(RouteMarkType type, int8_t intermediateIndex)
{
  // Remove route point.
  ASSERT(m_bmManager != nullptr, ());
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  routePoints.PassRoutePoint(type, intermediateIndex);
  routePoints.NotifyChanges();

  if (type == RouteMarkType::Finish)
    RemoveRoute(false /* deactivateFollowing */);
}

RouterType RoutingManager::GetBestRouter(m2::PointD const & startPoint,
                                         m2::PointD const & finalPoint) const
{
  // todo Implement something more sophisticated here (or delete the method).
  return GetLastUsedRouter();
}

RouterType RoutingManager::GetLastUsedRouter() const
{
  std::string routerTypeStr;
  if (!settings::Get(kRouterTypeKey, routerTypeStr))
    return RouterType::Vehicle;

  auto const routerType = FromString(routerTypeStr);

  switch (routerType)
  {
  case RouterType::Pedestrian:
  case RouterType::Bicycle: return routerType;
  default: return RouterType::Vehicle;
  }
}

void RoutingManager::SetRouterImpl(RouterType type)
{
  auto const indexGetterFn = m_callbacks.m_indexGetter;
  CHECK(indexGetterFn, ("Type:", type));

  VehicleType const vehicleType = GetVehicleType(type);

  auto const countryFileGetter = [this](m2::PointD const & p) -> std::string {
    // TODO (@gorshenin): fix CountryInfoGetter to return CountryFile
    // instances instead of plain strings.
    return m_callbacks.m_countryInfoGetter().GetRegionCountryId(p);
  };

  auto numMwmIds = make_shared<NumMwmIds>();
  m_delegate.RegisterCountryFilesOnRoute(numMwmIds);

  auto & index = m_callbacks.m_indexGetter();

  auto localFileChecker = [this](std::string const & countryFile) -> bool {
    MwmSet::MwmId const mwmId = m_callbacks.m_indexGetter().GetMwmIdByCountryFile(
      platform::CountryFile(countryFile));
    if (!mwmId.IsAlive())
      return false;

    return version::MwmTraits(mwmId.GetInfo()->m_version).HasRoutingIndex();
  };

  auto const getMwmRectByName = [this](std::string const & countryId) -> m2::RectD {
    return m_callbacks.m_countryInfoGetter().GetLimitRectForLeaf(countryId);
  };

  auto fetcher = make_unique<OnlineAbsentCountriesFetcher>(countryFileGetter, localFileChecker);
  auto router = IndexRouter::Create(vehicleType, countryFileGetter, getMwmRectByName, numMwmIds,
                                    MakeNumMwmTree(*numMwmIds, m_callbacks.m_countryInfoGetter()),
                                    m_routingSession, index);

  m_routingSession.SetRoutingSettings(GetRoutingSettings(vehicleType));
  m_routingSession.SetRouter(std::move(router), std::move(fetcher));
  m_currentRouterType = type;
}

void RoutingManager::RemoveRoute(bool deactivateFollowing)
{
  if (m_drapeEngine == nullptr)
    return;

  if (deactivateFollowing)
    SetPointsFollowingMode(false /* enabled */);

  std::lock_guard<std::mutex> lock(m_drapeSubroutesMutex);
  if (deactivateFollowing)
  {
    // Remove all subroutes.
    m_drapeEngine->RemoveSubroute(dp::DrapeID(), true /* deactivateFollowing */);
  }
  else
  {
    for (auto const & subrouteId : m_drapeSubroutes)
      m_drapeEngine->RemoveSubroute(subrouteId, false /* deactivateFollowing */);
  }
  m_drapeSubroutes.clear();
}

void RoutingManager::InsertRoute(Route const & route)
{
  if (m_drapeEngine == nullptr)
    return;

  // TODO: Now we always update whole route, so we need to remove previous one.
  RemoveRoute(false /* deactivateFollowing */);

  std::lock_guard<std::mutex> lock(m_drapeSubroutesMutex);
  std::vector<RouteSegment> segments;
  std::vector<m2::PointD> points;
  double distance = 0.0;
  for (size_t subrouteIndex = route.GetCurrentSubrouteIdx(); subrouteIndex < route.GetSubrouteCount(); ++subrouteIndex)
  {
    route.GetSubrouteInfo(subrouteIndex, segments);

    // Fill points.
    double const currentBaseDistance = distance;
    points.clear();
    points.reserve(segments.size() + 1);
    points.push_back(route.GetSubrouteAttrs(subrouteIndex).GetStart().GetPoint());
    for (auto const & s : segments)
      points.push_back(s.GetJunction().GetPoint());
    if (points.size() < 2)
    {
      LOG(LWARNING, ("Invalid subroute. Points number =", points.size()));
      continue;
    }
    distance = segments.back().GetDistFromBeginningMerc();

    auto subroute = make_unique_dp<df::Subroute>();
    subroute->m_polyline = m2::PolylineD(points);
    subroute->m_baseDistance = currentBaseDistance;
    switch (m_currentRouterType)
    {
      case RouterType::Vehicle:
        subroute->m_routeType = df::RouteType::Car;
        subroute->m_color = df::kRouteColor;
        FillTrafficForRendering(segments, subroute->m_traffic);
        FillTurnsDistancesForRendering(segments, subroute->m_turns);
        break;
      case RouterType::Pedestrian:
        subroute->m_routeType = df::RouteType::Pedestrian;
        subroute->m_color = df::kRoutePedestrian;
        subroute->m_pattern = df::RoutePattern(4.0, 2.0);
        break;
      case RouterType::Bicycle:
        subroute->m_routeType = df::RouteType::Bicycle;
        subroute->m_color = df::kRouteBicycle;
        subroute->m_pattern = df::RoutePattern(8.0, 2.0);
        FillTurnsDistancesForRendering(segments, subroute->m_turns);
        break;
      case RouterType::Taxi:
        subroute->m_routeType = df::RouteType::Taxi;
        subroute->m_color = df::kRouteColor;
        FillTrafficForRendering(segments, subroute->m_traffic);
        FillTurnsDistancesForRendering(segments, subroute->m_turns);
        break;
      default: ASSERT(false, ("Unknown router type"));
    }

    auto const subrouteId = m_drapeEngine->AddSubroute(std::move(subroute));
    m_drapeSubroutes.push_back(subrouteId);

    // TODO: we will send subrouteId to routing subsystem when we can partly update route.
    //route.SetSubrouteUid(subrouteIndex, static_cast<SubrouteUid>(subrouteId));
  }
}

void RoutingManager::FollowRoute()
{
  ASSERT(m_drapeEngine != nullptr, ());

  if (!m_routingSession.EnableFollowMode())
    return;

  m_delegate.OnRouteFollow(m_currentRouterType);

  HideRoutePoint(RouteMarkType::Start);
  SetPointsFollowingMode(true /* enabled */);
}

void RoutingManager::CloseRouting(bool removeRoutePoints)
{
  // Hide preview.
  if (m_drapeEngine != nullptr)
    m_drapeEngine->RemoveAllRoutePreviewSegments();
  
  if (m_routingSession.IsBuilt())
    m_routingSession.EmitCloseRoutingEvent();
  m_routingSession.Reset();
  RemoveRoute(true /* deactivateFollowing */);

  if (removeRoutePoints)
  {
    auto & controller = m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK);
    controller.Clear();
    controller.NotifyChanges();
  }
}

void RoutingManager::SetLastUsedRouter(RouterType type)
{
  settings::Set(kRouterTypeKey, ToString(type));
}

void RoutingManager::HideRoutePoint(RouteMarkType type, int8_t intermediateIndex)
{
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  RouteMarkPoint * mark = routePoints.GetRoutePoint(type, intermediateIndex);
  if (mark != nullptr)
  {
    mark->SetIsVisible(false);
    routePoints.NotifyChanges();
  }
}

bool RoutingManager::IsMyPosition(RouteMarkType type, int8_t intermediateIndex)
{
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  RouteMarkPoint * mark = routePoints.GetRoutePoint(type, intermediateIndex);
  return mark != nullptr ? mark->IsMyPosition() : false;
}

std::vector<RouteMarkData> RoutingManager::GetRoutePoints() const
{
  std::vector<RouteMarkData> result;
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  for (auto const & p : routePoints.GetRoutePoints())
    result.push_back(p->GetMarkData());
  return result;
}

size_t RoutingManager::GetRoutePointsCount() const
{
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  return routePoints.GetRoutePointsCount();
}

bool RoutingManager::CouldAddIntermediatePoint() const
{
  if (!IsRoutingActive())
    return false;

  // Now only car routing supports intermediate points.
  if (m_currentRouterType != RouterType::Vehicle)
    return false;

  auto const & controller = m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK);
  return controller.GetUserMarkCount() <
         static_cast<size_t>(RoutePointsLayout::kMaxIntermediatePointsCount + 2);
}

void RoutingManager::AddRoutePoint(RouteMarkData && markData)
{
  ASSERT(m_bmManager != nullptr, ());
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));

  // Always replace start and finish points.
  if (markData.m_pointType == RouteMarkType::Start || markData.m_pointType == RouteMarkType::Finish)
    routePoints.RemoveRoutePoint(markData.m_pointType);

  markData.m_isVisible = !markData.m_isMyPosition;
  routePoints.AddRoutePoint(std::move(markData));
  ReorderIntermediatePoints();
  routePoints.NotifyChanges();
}

void RoutingManager::RemoveRoutePoint(RouteMarkType type, int8_t intermediateIndex)
{
  ASSERT(m_bmManager != nullptr, ());
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  routePoints.RemoveRoutePoint(type, intermediateIndex);
  ReorderIntermediatePoints();
  routePoints.NotifyChanges();
}

void RoutingManager::RemoveIntermediateRoutePoints()
{
  ASSERT(m_bmManager != nullptr, ());
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  routePoints.RemoveIntermediateRoutePoints();
  routePoints.NotifyChanges();
}

void RoutingManager::MoveRoutePoint(RouteMarkType currentType, int8_t currentIntermediateIndex,
                                    RouteMarkType targetType, int8_t targetIntermediateIndex)
{
  ASSERT(m_bmManager != nullptr, ());
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  routePoints.MoveRoutePoint(currentType, currentIntermediateIndex,
                             targetType, targetIntermediateIndex);
  routePoints.NotifyChanges();
}

void RoutingManager::SetPointsFollowingMode(bool enabled)
{
  ASSERT(m_bmManager != nullptr, ());
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  routePoints.SetFollowingMode(enabled);
  routePoints.NotifyChanges();
}

std::vector<int8_t> RoutingManager::PredictIntermediatePointsOrder(std::vector<m2::PointD> const & points)
{
  //TODO(@bykoianko) Implement it
  std::vector<int8_t> result;
  result.reserve(points.size());
  for (size_t i = 0; i < points.size(); ++i)
    result.push_back(static_cast<int8_t>(i));
  return result;
}

void RoutingManager::ReorderIntermediatePoints()
{
  std::vector<RouteMarkPoint *> points;
  std::vector<m2::PointD> positions;
  points.reserve(RoutePointsLayout::kMaxIntermediatePointsCount);
  positions.reserve(RoutePointsLayout::kMaxIntermediatePointsCount);
  RoutePointsLayout routePoints(m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK));
  for (auto const & p : routePoints.GetRoutePoints())
  {
    if (p->GetRoutePointType() == RouteMarkType::Intermediate)
    {
      points.push_back(p);
      positions.push_back(p->GetPivot());
    }
  }
  if (points.empty())
    return;

  std::vector<int8_t> const order = PredictIntermediatePointsOrder(positions);
  ASSERT_EQUAL(order.size(), points.size(), ());
  for (size_t i = 0; i < order.size(); ++i)
    points[i]->SetIntermediateIndex(order[i]);
  routePoints.NotifyChanges();
}

void RoutingManager::GenerateTurnNotifications(std::vector<std::string> & turnNotifications)
{
  if (m_currentRouterType == RouterType::Taxi)
    return;

  return m_routingSession.GenerateTurnNotifications(turnNotifications);
}

void RoutingManager::BuildRoute(uint32_t timeoutSec)
{
  ASSERT_THREAD_CHECKER(m_threadChecker, ("BuildRoute"));
  ASSERT(m_drapeEngine != nullptr, ());

  auto routePoints = GetRoutePoints();
  if (routePoints.size() < 2)
  {
    CallRouteBuilded(IRouter::Cancelled, storage::TCountriesVec());
    CloseRouting(false /* remove route points */);
    return;
  }

  // Update my position.
  for (auto & p : routePoints)
  {
    if (!p.m_isMyPosition)
      continue;

    MyPositionMarkPoint * myPosition = UserMarkContainer::UserMarkForMyPostion();
    if (!myPosition->HasPosition())
    {
      CallRouteBuilded(IRouter::NoCurrentPosition, storage::TCountriesVec());
      return;
    }
    p.m_position = myPosition->GetPivot();
  }

  // Check for equal points.
  double const kEps = 1e-7;
  for (size_t i = 0; i < routePoints.size(); i++)
  {
    for (size_t j = i + 1; j < routePoints.size(); j++)
    {
      if (routePoints[i].m_position.EqualDxDy(routePoints[j].m_position, kEps))
      {
        CallRouteBuilded(IRouter::Cancelled, storage::TCountriesVec());
        CloseRouting(false /* remove route points */);
        return;
      }
    }
  }

  bool const isP2P = !routePoints.front().m_isMyPosition && !routePoints.back().m_isMyPosition;

  // Send tag to Push Woosh.
  {
    std::string tag;
    switch (m_currentRouterType)
    {
    case RouterType::Vehicle:
      tag = isP2P ? marketing::kRoutingP2PVehicleDiscovered : marketing::kRoutingVehicleDiscovered;
      break;
    case RouterType::Pedestrian:
      tag = isP2P ? marketing::kRoutingP2PPedestrianDiscovered
                  : marketing::kRoutingPedestrianDiscovered;
      break;
    case RouterType::Bicycle:
      tag = isP2P ? marketing::kRoutingP2PBicycleDiscovered : marketing::kRoutingBicycleDiscovered;
      break;
    case RouterType::Taxi:
      tag = isP2P ? marketing::kRoutingP2PTaxiDiscovered : marketing::kRoutingTaxiDiscovered;
      break;
    case RouterType::Count: CHECK(false, ("Bad router type", m_currentRouterType));
    }
    GetPlatform().GetMarketingService().SendPushWooshTag(tag);
  }

  if (IsRoutingActive())
    CloseRouting(false /* remove route points */);

  // Show preview.
  if (m_drapeEngine != nullptr)
  {
    m2::RectD rect;
    for (size_t pointIndex = 0; pointIndex + 1 < routePoints.size(); pointIndex++)
    {
      rect.Add(routePoints[pointIndex].m_position);
      rect.Add(routePoints[pointIndex + 1].m_position);
      m_drapeEngine->AddRoutePreviewSegment(routePoints[pointIndex].m_position,
                                            routePoints[pointIndex + 1].m_position);
    }
    rect.Scale(kRouteScaleMultiplier);
    m_drapeEngine->SetModelViewRect(rect, true /* applyRotation */, -1 /* zoom */,
                                    true /* isAnim */);
  }

  m_routingSession.SetUserCurrentPosition(routePoints.front().m_position);

  std::vector<m2::PointD> points;
  points.reserve(routePoints.size());
  for (auto const & point : routePoints)
    points.push_back(point.m_position);

  m_routingSession.BuildRoute(Checkpoints(std::move(points)), timeoutSec);
}

void RoutingManager::SetUserCurrentPosition(m2::PointD const & position)
{
  if (IsRoutingActive())
    m_routingSession.SetUserCurrentPosition(position);
}

bool RoutingManager::DisableFollowMode()
{
  bool const disabled = m_routingSession.DisableFollowMode();
  if (disabled && m_drapeEngine != nullptr)
    m_drapeEngine->DeactivateRouteFollowing();

  return disabled;
}

void RoutingManager::CheckLocationForRouting(location::GpsInfo const & info)
{
  if (!IsRoutingActive())
    return;

  auto const featureIndexGetterFn = m_callbacks.m_indexGetter;
  ASSERT(featureIndexGetterFn, ());
  RoutingSession::State const state =
      m_routingSession.OnLocationPositionChanged(info, featureIndexGetterFn());
  if (state == RoutingSession::RouteNeedRebuild)
  {
    m_routingSession.RebuildRoute(
        MercatorBounds::FromLatLon(info.m_latitude, info.m_longitude),
        [this](Route const & route, IRouter::ResultCode code) { OnRebuildRouteReady(route, code); },
        0 /* timeoutSec */, RoutingSession::State::RouteRebuilding,
        true /* adjustToPrevRoute */);
  }
}

void RoutingManager::CallRouteBuilded(IRouter::ResultCode code,
                                      storage::TCountriesVec const & absentCountries)
{
  m_routingCallback(code, absentCountries);
}

void RoutingManager::MatchLocationToRoute(location::GpsInfo & location,
                                          location::RouteMatchingInfo & routeMatchingInfo) const
{
  if (!IsRoutingActive())
    return;

  m_routingSession.MatchLocationToRoute(location, routeMatchingInfo);
}

void RoutingManager::OnLocationUpdate(location::GpsInfo & info)
{
  if (m_drapeEngine == nullptr)
    m_gpsInfoCache = my::make_unique<location::GpsInfo>(info);

  auto routeMatchingInfo = GetRouteMatchingInfo(info);

  if (m_drapeEngine != nullptr)
    m_drapeEngine->SetGpsInfo(info, m_routingSession.IsNavigable(), routeMatchingInfo);

  if (IsTrackingReporterEnabled())
    m_trackingReporter.AddLocation(info, m_routingSession.MatchTraffic(routeMatchingInfo));
}

location::RouteMatchingInfo RoutingManager::GetRouteMatchingInfo(location::GpsInfo & info)
{
  location::RouteMatchingInfo routeMatchingInfo;
  CheckLocationForRouting(info);
  MatchLocationToRoute(info, routeMatchingInfo);
  return routeMatchingInfo;
}

void RoutingManager::SetDrapeEngine(ref_ptr<df::DrapeEngine> engine, bool is3dAllowed)
{
  m_drapeEngine = engine;

  // Apply gps info which was set before drape engine creation.
  if (m_gpsInfoCache != nullptr)
  {
    auto routeMatchingInfo = GetRouteMatchingInfo(*m_gpsInfoCache);
    m_drapeEngine->SetGpsInfo(*m_gpsInfoCache, m_routingSession.IsNavigable(), routeMatchingInfo);
    m_gpsInfoCache.reset();
  }

  // In case of the engine reinitialization recover route.
  if (IsRoutingActive())
  {
    InsertRoute(*m_routingSession.GetRoute());
    if (is3dAllowed && m_routingSession.IsFollowing())
      m_drapeEngine->EnablePerspective();
  }
}

bool RoutingManager::HasRouteAltitude() const { return m_routingSession.HasRouteAltitude(); }

bool RoutingManager::GenerateRouteAltitudeChart(uint32_t width, uint32_t height,
                                                std::vector<uint8_t> & imageRGBAData,
                                                int32_t & minRouteAltitude,
                                                int32_t & maxRouteAltitude,
                                                measurement_utils::Units & altitudeUnits) const
{
  feature::TAltitudes altitudes;
  std::vector<double> segDistance;

  if (!m_routingSession.GetRouteAltitudesAndDistancesM(segDistance, altitudes))
    return false;
  segDistance.insert(segDistance.begin(), 0.0);

  if (altitudes.empty())
    return false;

  if (!maps::GenerateChart(width, height, segDistance, altitudes,
                           GetStyleReader().GetCurrentStyle(), imageRGBAData))
    return false;

  auto const minMaxIt = minmax_element(altitudes.cbegin(), altitudes.cend());
  feature::TAltitude const minRouteAltitudeM = *minMaxIt.first;
  feature::TAltitude const maxRouteAltitudeM = *minMaxIt.second;

  if (!settings::Get(settings::kMeasurementUnits, altitudeUnits))
    altitudeUnits = measurement_utils::Units::Metric;

  switch (altitudeUnits)
  {
  case measurement_utils::Units::Imperial:
    minRouteAltitude = measurement_utils::MetersToFeet(minRouteAltitudeM);
    maxRouteAltitude = measurement_utils::MetersToFeet(maxRouteAltitudeM);
    break;
  case measurement_utils::Units::Metric:
    minRouteAltitude = minRouteAltitudeM;
    maxRouteAltitude = maxRouteAltitudeM;
    break;
  }
  return true;
}

bool RoutingManager::IsTrackingReporterEnabled() const
{
  if (m_currentRouterType != RouterType::Vehicle)
    return false;

  if (!m_routingSession.IsFollowing())
    return false;

  bool enableTracking = false;
  UNUSED_VALUE(settings::Get(tracking::Reporter::kEnableTrackingKey, enableTracking));
  return enableTracking;
}

void RoutingManager::SetRouter(RouterType type)
{
  ASSERT_THREAD_CHECKER(m_threadChecker, ("SetRouter"));

  if (m_currentRouterType == type)
    return;

  // Hide preview.
  if (m_drapeEngine != nullptr)
    m_drapeEngine->RemoveAllRoutePreviewSegments();

  SetLastUsedRouter(type);
  SetRouterImpl(type);
}

// static
uint32_t RoutingManager::InvalidRoutePointsTransactionId()
{
  return kInvalidTransactionId;
}

uint32_t RoutingManager::GenerateRoutePointsTransactionId() const
{
  static uint32_t id = kInvalidTransactionId + 1;
  return id++;
}

uint32_t RoutingManager::OpenRoutePointsTransaction()
{
  auto const id = GenerateRoutePointsTransactionId();
  m_routePointsTransactions[id].m_routeMarks = GetRoutePoints();
  return id;
}

void RoutingManager::ApplyRoutePointsTransaction(uint32_t transactionId)
{
  if (m_routePointsTransactions.find(transactionId) == m_routePointsTransactions.end())
    return;

  // If we apply a transaction we can remove all earlier transactions.
  // All older transactions must be kept since they can be applied or cancelled later.
  for (auto it = m_routePointsTransactions.begin(); it != m_routePointsTransactions.end();)
  {
    if (it->first <= transactionId)
      it = m_routePointsTransactions.erase(it);
    else
      ++it;
  }
}

void RoutingManager::CancelRoutePointsTransaction(uint32_t transactionId)
{
  auto const it = m_routePointsTransactions.find(transactionId);
  if (it == m_routePointsTransactions.end())
    return;
  auto routeMarks = it->second.m_routeMarks;

  // If we cancel a transaction we must remove all later transactions.
  for (auto it = m_routePointsTransactions.begin(); it != m_routePointsTransactions.end();)
  {
    if (it->first >= transactionId)
      it = m_routePointsTransactions.erase(it);
    else
      ++it;
  }

  // Revert route points.
  ASSERT(m_bmManager != nullptr, ());
  auto & controller = m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK);
  controller.Clear();
  RoutePointsLayout routePoints(controller);
  for (auto & markData : routeMarks)
    routePoints.AddRoutePoint(std::move(markData));
  routePoints.NotifyChanges();
}

bool RoutingManager::HasSavedRoutePoints() const
{
  auto const fileName = GetPlatform().SettingsPathForFile(kRoutePointsFile);
  return GetPlatform().IsFileExistsByFullPath(fileName);
}

bool RoutingManager::LoadRoutePoints()
{
  if (!HasSavedRoutePoints())
    return false;

  // Delete file after loading.
  auto const fileName = GetPlatform().SettingsPathForFile(kRoutePointsFile);
  MY_SCOPE_GUARD(routePointsFileGuard, std::bind(&FileWriter::DeleteFileX,
                                                 std::cref(fileName)));

  std::string data;
  try
  {
    ReaderPtr<Reader>(GetPlatform().GetReader(fileName)).ReadAsString(data);
  }
  catch (RootException const & ex)
  {
    LOG(LWARNING, ("Loading road points failed:", ex.Msg()));
    return false;
  }

  auto points = DeserializeRoutePoints(data);
  if (points.empty())
    return false;

  // If we have found my position, we use my position as start point.
  MyPositionMarkPoint * myPosMark = UserMarkContainer::UserMarkForMyPostion();
  ASSERT(m_bmManager != nullptr, ());
  m_bmManager->GetUserMarksController(UserMarkType::ROUTING_MARK).Clear();
  for (auto & p : points)
  {
    if (p.m_pointType == RouteMarkType::Start && myPosMark->HasPosition())
    {
      RouteMarkData startPt;
      startPt.m_pointType = RouteMarkType::Start;
      startPt.m_isMyPosition = true;
      startPt.m_position = myPosMark->GetPivot();
      AddRoutePoint(std::move(startPt));
    }
    else
    {
      AddRoutePoint(std::move(p));
    }
  }
  return true;
}

void RoutingManager::SaveRoutePoints() const
{
  auto points = GetRoutePoints();
  if (points.size() < 2)
    return;

  if (points.back().m_isPassed)
    return;

  try
  {
    auto const fileName = GetPlatform().SettingsPathForFile(kRoutePointsFile);
    FileWriter writer(fileName);
    std::string const pointsData = SerializeRoutePoints(points);
    writer.Write(pointsData.c_str(), pointsData.length());
  }
  catch (RootException const & ex)
  {
    LOG(LWARNING, ("Saving road points failed:", ex.Msg()));
  }
}
