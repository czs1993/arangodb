////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "IResearchViewCoordinator.h"
#include "IResearchCommon.h"
#include "IResearchLinkHelper.h"

#include <velocypack/Builder.h>
#include <velocypack/Collection.h>
#include <velocypack/Parser.h>
#include "Basics/StaticStrings.h"
#include "Basics/StringUtils.h"
#include "Cluster/ClusterFeature.h"
#include "Cluster/ClusterInfo.h"
#include "Cluster/ServerState.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchLink.h"
#include "IResearch/VelocyPackHelper.h"
#include "RestServer/ViewTypesFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/ExecContext.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/Methods/Indexes.h"

namespace {

typedef irs::async_utils::read_write_mutex::read_mutex ReadMutex;
typedef irs::async_utils::read_write_mutex::write_mutex WriteMutex;

void ensureImmutableProperties(
    arangodb::iresearch::IResearchViewMeta& dst,
    arangodb::iresearch::IResearchViewMeta const& src) {
  dst._locale = src._locale;
  dst._version = src._version;
  dst._writebufferActive = src._writebufferActive;
  dst._writebufferIdle = src._writebufferIdle;
  dst._writebufferSizeMax = src._writebufferSizeMax;
  dst._primarySort = src._primarySort;
}

}  // namespace

namespace arangodb {
namespace iresearch {

////////////////////////////////////////////////////////////////////////////////
/// @brief IResearchView-specific implementation of a ViewFactory
////////////////////////////////////////////////////////////////////////////////
struct IResearchViewCoordinator::ViewFactory : public arangodb::ViewFactory {
  virtual arangodb::Result create(arangodb::LogicalView::ptr& view, TRI_vocbase_t& vocbase,
                                  arangodb::velocypack::Slice const& definition) const override {
    auto* ci = ClusterInfo::instance();

    if (!ci) {
      return arangodb::Result(
          TRI_ERROR_INTERNAL,
          std::string("failure to find 'ClusterInfo' instance while creating "
                      "arangosearch View in database '") +
              vocbase.name() + "'");
    }

    auto& properties = definition.isObject()
                           ? definition
                           : arangodb::velocypack::Slice::emptyObjectSlice();  // if no 'info' then assume defaults
    auto links = properties.hasKey(StaticStrings::LinksField)
                     ? properties.get(StaticStrings::LinksField)
                     : arangodb::velocypack::Slice::emptyObjectSlice();
    auto res = IResearchLinkHelper::validateLinks(vocbase, links);

    if (!res.ok()) {
      return res;
    }

    arangodb::LogicalView::ptr impl;

    res = arangodb::LogicalViewHelperClusterInfo::construct(impl, vocbase, definition);

    if (!res.ok()) {
      return res;
    }

    // create links on a best-effor basis
    // link creation failure does not cause view creation failure
    try {
      std::unordered_set<TRI_voc_cid_t> collections;

      res = IResearchLinkHelper::updateLinks(collections, *impl, links);

      if (!res.ok()) {
        LOG_TOPIC("39d88", WARN, arangodb::iresearch::TOPIC)
            << "failed to create links while creating arangosearch view '"
            << impl->name() << "': " << res.errorNumber() << " " << res.errorMessage();
      }
    } catch (arangodb::basics::Exception const& e) {
      IR_LOG_EXCEPTION();
      LOG_TOPIC("09bb9", WARN, arangodb::iresearch::TOPIC)
          << "caught exception while creating links while creating "
             "arangosearch view '"
          << impl->name() << "': " << e.code() << " " << e.what();
    } catch (std::exception const& e) {
      IR_LOG_EXCEPTION();
      LOG_TOPIC("6b99b", WARN, arangodb::iresearch::TOPIC)
          << "caught exception while creating links while creating "
             "arangosearch view '"
          << impl->name() << "': " << e.what();
    } catch (...) {
      IR_LOG_EXCEPTION();
      LOG_TOPIC("61ae6", WARN, arangodb::iresearch::TOPIC)
          << "caught exception while creating links while creating "
             "arangosearch view '"
          << impl->name() << "'";
    }

    view = ci->getView(vocbase.name(),
                       std::to_string(impl->id()));  // refresh view from Agency

    if (view) {
      view->open();  // open view to match the behavior in
                     // StorageEngine::openExistingDatabase(...) and original
                     // behavior of TRI_vocbase_t::createView(...)
    }

    return arangodb::Result();
  }

  virtual arangodb::Result instantiate(arangodb::LogicalView::ptr& view,
                                       TRI_vocbase_t& vocbase,
                                       arangodb::velocypack::Slice const& definition,
                                       uint64_t planVersion) const override {
    std::string error;
    auto impl = std::shared_ptr<IResearchViewCoordinator>(
        new IResearchViewCoordinator(vocbase, definition, planVersion));

    if (!impl->_meta.init(definition, error)) {
      return arangodb::Result(
          TRI_ERROR_BAD_PARAMETER,
          error.empty()
              ? (std::string("failed to initialize arangosearch View '") +
                 impl->name() + "' from definition: " + definition.toString())
              : (std::string("failed to initialize arangosearch View '") +
                 impl->name() + "' from definition, error in attribute '" +
                 error + "': " + definition.toString()));
    }

    view = impl;

    return arangodb::Result();
  }
};

IResearchViewCoordinator::~IResearchViewCoordinator() {
  arangodb::LogicalViewHelperClusterInfo::destruct(*this);  // cleanup of the storage engine
}

arangodb::Result IResearchViewCoordinator::appendVelocyPackImpl(
    arangodb::velocypack::Builder& builder, std::underlying_type<Serialize>::type flags) const {
  if (hasFlag(flags, Serialize::ForPersistence)) {
    auto res = arangodb::LogicalViewHelperClusterInfo::properties(builder, *this);

    if (!res.ok()) {
      return res;
    }
  }

  if (!hasFlag(flags, Serialize::Detailed)) {
    return arangodb::Result();  // nothing more to output
  }

  if (!builder.isOpenObject()) {
    return arangodb::Result(TRI_ERROR_BAD_PARAMETER,
                            std::string("invalid builder provided for "
                                        "IResearchViewCoordinator definition"));
  }

  static const std::function<bool(irs::string_ref const& key)> acceptor =
      [](irs::string_ref const& key) -> bool {
    return key != StaticStrings::VersionField;  // ignored fields
  };
  static const std::function<bool(irs::string_ref const& key)> persistenceAcceptor =
      [](irs::string_ref const&) -> bool { return true; };
  arangodb::velocypack::Builder sanitizedBuilder;

  sanitizedBuilder.openObject();

  if (!_meta.json(sanitizedBuilder) ||
      !mergeSliceSkipKeys(builder, sanitizedBuilder.close().slice(),
                          hasFlag(flags, Serialize::ForPersistence) ? persistenceAcceptor
                                                                    : acceptor)) {
    return arangodb::Result(
        TRI_ERROR_INTERNAL,
        std::string("failure to generate definition while generating "
                    "properties jSON for IResearch View in database '") +
            vocbase().name() + "'");
  }

  arangodb::velocypack::Builder links;

  // links are not persisted, their definitions are part of the corresponding
  // collections
  if (!hasFlag(flags, Serialize::ForPersistence)) {
    // verify that the current user has access on all linked collections
    ExecContext const& exec = ExecContext::current();
    if (!exec.isSuperuser()) {
      for (auto& entry : _collections) {
        if (!exec.canUseCollection(vocbase().name(), entry.second.first, auth::Level::RO)) {
          return Result(TRI_ERROR_FORBIDDEN);
        }
      }
    }

    ReadMutex mutex(_mutex);
    SCOPED_LOCK(mutex);  // '_collections' can be asynchronously modified

    links.openObject();

    for (auto& entry : _collections) {
      links.add(entry.second.first, entry.second.second.slice());
    }

    links.close();
    builder.add(StaticStrings::LinksField, links.slice());
  }

  return arangodb::Result();
}

/*static*/ arangodb::ViewFactory const& IResearchViewCoordinator::factory() {
  static const ViewFactory factory;

  return factory;
}

arangodb::Result IResearchViewCoordinator::link(IResearchLink const& link) {
  static const std::function<bool(irs::string_ref const& key)> acceptor = []( // acceptor
    irs::string_ref const& key // key
  ) -> bool {
    return key != arangodb::StaticStrings::IndexId // ignore index id
           && key != arangodb::StaticStrings::IndexType // ignore index type
           && key != StaticStrings::ViewIdField; // ignore view id
  };
  arangodb::velocypack::Builder builder;

  builder.openObject();

  auto res = link.properties(builder, false); // generate user-visible definition, agency will not see links

  if (!res.ok()) {
    return res;
  }

  builder.close();

  auto cid = link.collection().id();
  arangodb::velocypack::Builder sanitizedBuilder;

  sanitizedBuilder.openObject();

  // strip internal keys (added in IResearchLink::properties(...)) from externally visible link definition
  if (!mergeSliceSkipKeys(sanitizedBuilder, builder.slice(), acceptor)) {
    return arangodb::Result( // result
      TRI_ERROR_INTERNAL, // code
      std::string("failed to generate externally visible link definition while emplacing collection '") + std::to_string(cid) + "' into arangosearch View '" + name() + "'"
    );
  }

  sanitizedBuilder.close();

  WriteMutex mutex(_mutex); // '_collections' can be asynchronously read
  SCOPED_LOCK(mutex);
  auto entry = _collections.emplace( // emplace definition
    std::piecewise_construct, // piecewise construct
    std::forward_as_tuple(cid), // key
    std::forward_as_tuple( // value
      link.collection().name(), std::move(sanitizedBuilder) // args
    )
  );

  if (!entry.second) {
    return arangodb::Result( // result
      TRI_ERROR_ARANGO_DUPLICATE_IDENTIFIER, // code
      std::string("duplicate entry while emplacing collection '") + std::to_string(cid) + "' into arangosearch View '" + name() + "'"
    );
  }

  return arangodb::Result();
}

arangodb::Result IResearchViewCoordinator::renameImpl(std::string const& oldName) {
  return arangodb::LogicalViewHelperClusterInfo::rename(*this, oldName);
}

arangodb::Result IResearchViewCoordinator::unlink(TRI_voc_cid_t cid) noexcept {
  return arangodb::Result();  // NOOP since no internal store
}

IResearchViewCoordinator::IResearchViewCoordinator(TRI_vocbase_t& vocbase,
                                                   velocypack::Slice info, uint64_t planVersion)
    : LogicalView(vocbase, info, planVersion) {
  TRI_ASSERT(ServerState::instance()->isCoordinator());
}

bool IResearchViewCoordinator::visitCollections(CollectionVisitor const& visitor) const {
  ReadMutex mutex(_mutex);
  SCOPED_LOCK(mutex);  // '_collections' can be asynchronously modified

  for (auto& entry : _collections) {
    if (!visitor(entry.first)) {
      return false;
    }
  }

  return true;
}

arangodb::Result IResearchViewCoordinator::properties(velocypack::Slice const& slice,
                                                      bool partialUpdate) {
  auto* engine = arangodb::ClusterInfo::instance();

  if (!engine) {
    return arangodb::Result(TRI_ERROR_INTERNAL,
                            std::string("failure to get storage engine while "
                                        "updating arangosearch view '") +
                                name() + "'");
  }

  try {
    auto links = slice.hasKey(StaticStrings::LinksField)
                     ? slice.get(StaticStrings::LinksField)
                     : arangodb::velocypack::Slice::emptyObjectSlice();
    auto res = IResearchLinkHelper::validateLinks(vocbase(), links);

    if (!res.ok()) {
      return res;
    }

    // check link auth as per https://github.com/arangodb/backlog/issues/459
    ExecContext const& exe = ExecContext::current();
    if (!exe.isSuperuser()) {
      // check existing links
      for (auto& entry : _collections) {
        auto collection =
            engine->getCollection(vocbase().name(), std::to_string(entry.first));

        if (collection &&
            !exe.canUseCollection(vocbase().name(), collection->name(), arangodb::auth::Level::RO)) {
          return arangodb::Result(
              TRI_ERROR_FORBIDDEN,
              std::string("while updating arangosearch definition, error: "
                          "collection '") +
                  collection->name() + "' not authorized for read access");
        }
      }
    }

    std::string error;
    IResearchViewMeta meta;

    auto const& defaults = partialUpdate ? _meta : IResearchViewMeta::DEFAULT();

    if (!meta.init(slice, error, defaults)) {
      return arangodb::Result(
          TRI_ERROR_BAD_PARAMETER,
          error.empty() ? (std::string("failed to update arangosearch view '") +
                           name() + "' from definition: " + slice.toString())
                        : (std::string("failed to update arangosearch view '") +
                           name() + "' from definition, error in attribute '" +
                           error + "': " + slice.toString()));
    }

    // reset non-updatable values to match current meta
    ensureImmutableProperties(meta, _meta);

    // only trigger persisting of properties if they have changed
    if (_meta != meta) {
      auto oldMeta = std::move(_meta);

      _meta = std::move(meta);  // update meta for persistence

      auto result = arangodb::LogicalViewHelperClusterInfo::properties(*this);

      _meta = std::move(oldMeta);  // restore meta

      if (!result.ok()) {
        return result;
      }
    }

    if (links.isEmptyObject() && partialUpdate) {
      return arangodb::Result();  // nothing more to do
    }

    // ...........................................................................
    // update links if requested (on a best-effort basis)
    // indexing of collections is done in different threads so no locks can be
    // held and rollback is not possible as a result it's also possible for
    // links to be simultaneously modified via a different callflow (e.g. from
    // collections)
    // ...........................................................................

    arangodb::velocypack::Builder currentLinks;
    std::unordered_set<TRI_voc_cid_t> currentCids;

    {
      ReadMutex mutex(_mutex);
      SCOPED_LOCK(mutex);  // '_collections' can be asynchronously modified

      currentLinks.openObject();

      for (auto& entry : _collections) {
        currentCids.emplace(entry.first);
        currentLinks.add(entry.second.first, entry.second.second.slice());
      }

      currentLinks.close();
    }

    std::unordered_set<TRI_voc_cid_t> collections;

    if (partialUpdate) {
      return IResearchLinkHelper::updateLinks(collections, *this, links);
    }

    return IResearchLinkHelper::updateLinks( // update links
      collections, *this, links, currentCids // args
    );
  } catch (arangodb::basics::Exception& e) {
    LOG_TOPIC("714b3", WARN, iresearch::TOPIC)
        << "caught exception while updating properties for arangosearch view '"
        << name() << "': " << e.code() << " " << e.what();
    IR_LOG_EXCEPTION();

    return arangodb::Result(
        e.code(),
        std::string("error updating properties for arangosearch view '") +
            name() + "'");
  } catch (std::exception const& e) {
    LOG_TOPIC("86a5c", WARN, iresearch::TOPIC)
        << "caught exception while updating properties for arangosearch view '"
        << name() << "': " << e.what();
    IR_LOG_EXCEPTION();

    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        std::string("error updating properties for arangosearch view '") +
            name() + "'");
  } catch (...) {
    LOG_TOPIC("17b66", WARN, iresearch::TOPIC)
        << "caught exception while updating properties for arangosearch view '"
        << name() << "'";
    IR_LOG_EXCEPTION();

    return arangodb::Result(
        TRI_ERROR_BAD_PARAMETER,
        std::string("error updating properties for arangosearch view '") +
            name() + "'");
  }
}

Result IResearchViewCoordinator::dropImpl() {
  auto* engine = arangodb::ClusterInfo::instance();

  if (!engine) {
    return arangodb::Result(TRI_ERROR_INTERNAL,
                            std::string("failure to get storage engine while "
                                        "dropping arangosearch view '") +
                                name() + "'");
  }

  // drop links first
  {
    std::unordered_set<TRI_voc_cid_t> currentCids;

    visitCollections([&currentCids](TRI_voc_cid_t cid) -> bool {
      currentCids.emplace(cid);
      return true;
    });

    // check link auth as per https://github.com/arangodb/backlog/issues/459
    ExecContext const& exe = ExecContext::current();
    if (!exe.isSuperuser()) {
      for (auto& entry : currentCids) {
        auto collection = engine->getCollection(vocbase().name(), std::to_string(entry));

        if (collection &&
            !exe.canUseCollection(vocbase().name(), collection->name(), arangodb::auth::Level::RO)) {
          return arangodb::Result(TRI_ERROR_FORBIDDEN);
        }
      }
    }

    std::unordered_set<TRI_voc_cid_t> collections;
    auto res = IResearchLinkHelper::updateLinks( // update links
      collections, // modified collections
      *this, // view
                                                arangodb::velocypack::Slice::emptyObjectSlice(),
                                                currentCids);

    if (!res.ok()) {
      return arangodb::Result(
          res.errorNumber(),
          std::string(
              "failed to remove links while removing arangosearch view '") +
              name() + "': " + res.errorMessage());
    }
  }

  return arangodb::LogicalViewHelperClusterInfo::drop(*this);
}

}  // namespace iresearch
}  // namespace arangodb

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------
