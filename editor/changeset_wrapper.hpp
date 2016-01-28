#pragma once

#include "editor/server_api.hpp"
#include "editor/xml_feature.hpp"

#include "base/exception.hpp"

#include "std/set.hpp"

class FeatureType;

namespace osm
{

struct ClientToken;

class ChangesetWrapper
{
public:
  DECLARE_EXCEPTION(ChangesetWrapperException, RootException);
  DECLARE_EXCEPTION(NetworkErrorException, ChangesetWrapperException);
  DECLARE_EXCEPTION(HttpErrorException, ChangesetWrapperException);
  DECLARE_EXCEPTION(OsmXmlParseException, ChangesetWrapperException);
  DECLARE_EXCEPTION(OsmObjectWasDeletedException, ChangesetWrapperException);
  DECLARE_EXCEPTION(CreateChangeSetFailedException, ChangesetWrapperException);
  DECLARE_EXCEPTION(ModifyNodeFailedException, ChangesetWrapperException);
  DECLARE_EXCEPTION(LinearFeaturesAreNotSupportedException, ChangesetWrapperException);

  ChangesetWrapper(TKeySecret const & keySecret, ServerApi06::TKeyValueTags const & comments);
  ~ChangesetWrapper();

  /// Throws many exceptions from above list, plus including XMLNode's parsing ones.
  /// OsmObjectWasDeletedException means that node was deleted from OSM server by someone else.
  editor::XMLFeature GetMatchingNodeFeatureFromOSM(m2::PointD const & center);
  editor::XMLFeature GetMatchingAreaFeatureFromOSM(set<m2::PointD> const & geomerty);

  /// Throws exceptions from above list.
  void ModifyNode(editor::XMLFeature node);

private:
  /// Unfortunately, pugi can't return xml_documents from methods.
  /// Throws exceptions from above list.
  void LoadXmlFromOSM(ms::LatLon const & ll, pugi::xml_document & doc);

  ServerApi06::TKeyValueTags m_changesetComments;
  ServerApi06 m_api;
  static constexpr uint64_t kInvalidChangesetId = 0;
  uint64_t m_changesetId = kInvalidChangesetId;
};

} // namespace osm