////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "EngineInfoContainerDBServer.h"

#include "Aql/AqlItemBlock.h"
#include "Aql/ClusterNodes.h"
#include "Aql/Collection.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionNode.h"
#include "Aql/GraphNode.h"
#include "Aql/IResearchViewNode.h"
#include "Aql/IndexNode.h"
#include "Aql/ModificationNodes.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Basics/Result.h"
#include "Cluster/ClusterComm.h"
#include "Cluster/ClusterTrxMethods.h"
#include "Cluster/ServerState.h"
#include "Cluster/TraverserEngineRegistry.h"
#include "Graph/BaseOptions.h"
#include "RestServer/QueryRegistryFeature.h"
#include "StorageEngine/TransactionState.h"
#include "Utils/CollectionNameResolver.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ticks.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace {

const double SETUP_TIMEOUT = 90.0;

Result ExtractRemoteAndShard(VPackSlice keySlice, size_t& remoteId, std::string& shardId) {
  TRI_ASSERT(keySlice.isString());  // used as  a key in Json
  arangodb::velocypack::StringRef key(keySlice);
  size_t p = key.find(':');
  if (p == std::string::npos) {
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unexpected response from DBServer during setup"};
  }
  arangodb::velocypack::StringRef remId = key.substr(0, p);
  remoteId = basics::StringUtils::uint64(remId.data(), remId.length());
  if (remoteId == 0) {
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unexpected response from DBServer during setup"};
  }
  shardId = key.substr(p + 1).toString();
  if (shardId.empty()) {
    return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
            "Unexpected response from DBServer during setup"};
  }
  return {TRI_ERROR_NO_ERROR};
}

GatherNode* findFirstGather(ExecutionNode const& root) {
  ExecutionNode* node = root.getFirstParent();

  // moving down from a given node
  // towards a return node
  while (node) {
    switch (node->getType()) {
      case ExecutionNode::REMOTE:
        node = node->getFirstParent();

        if (!node || node->getType() != ExecutionNode::GATHER) {
          return nullptr;
        }

        return ExecutionNode::castTo<GatherNode*>(node);
      default:
        node = node->getFirstParent();
        break;
    }
  }

  return nullptr;
}

ScatterNode* findFirstScatter(ExecutionNode const& root) {
  ExecutionNode* node = root.getFirstDependency();

  // moving up from a given node
  // towards a singleton node
  while (node) {
    switch (node->getType()) {
      case ExecutionNode::REMOTE:
        node = node->getFirstDependency();

        if (!node) {
          return nullptr;
        }

        if (node->getType() != ExecutionNode::SCATTER &&
            node->getType() != ExecutionNode::DISTRIBUTE) {
          return nullptr;
        }

        return ExecutionNode::castTo<ScatterNode*>(node);
      default:
        node = node->getFirstDependency();
        break;
    }
  }

  return nullptr;
}

}  // namespace

EngineInfoContainerDBServer::EngineInfo::EngineInfo(size_t idOfRemoteNode) noexcept
    : _idOfRemoteNode(idOfRemoteNode), _otherId(0), _source(CollectionSource(nullptr)) {}

EngineInfoContainerDBServer::EngineInfo::~EngineInfo() {
  // This container is not responsible for nodes
  // they are managed by the AST somewhere else
  // We are also not responsible for the collection.
  TRI_ASSERT(!_nodes.empty());
}

EngineInfoContainerDBServer::EngineInfo::EngineInfo(EngineInfo&& other) noexcept
    : _nodes(std::move(other._nodes)),
      _idOfRemoteNode(other._idOfRemoteNode),
      _otherId(other._otherId),
      _source(std::move(other._source)) {
  TRI_ASSERT(!_nodes.empty());

#ifdef ARANGODB_ENABLE_MAINTAINER_MODE
  struct {
    void operator()(CollectionSource const& source) {
      TRI_ASSERT(source.collection);
    }

    void operator()(ViewSource const& source) { TRI_ASSERT(source.view); }
  } visitor;

  boost::apply_visitor(visitor, _source);
#endif
}

void EngineInfoContainerDBServer::EngineInfo::addNode(ExecutionNode* node) {
  TRI_ASSERT(node);

  auto setRestrictedShard = [](auto* node, auto& source) {
    TRI_ASSERT(node);

    auto* sourceImpl = boost::get<CollectionSource>(&source);
    TRI_ASSERT(sourceImpl);

    if (node->isRestricted()) {
      sourceImpl->restrictedShards.emplace(node->restrictedShard());
    }
  };

  switch (node->getType()) {
    case ExecutionNode::ENUMERATE_COLLECTION: {
      TRI_ASSERT(EngineType::Collection == type());
      setRestrictedShard(ExecutionNode::castTo<EnumerateCollectionNode*>(node), _source);
      break;
    }
    case ExecutionNode::INDEX: {
      TRI_ASSERT(EngineType::Collection == type());
      setRestrictedShard(ExecutionNode::castTo<IndexNode*>(node), _source);
      break;
    }
    case ExecutionNode::ENUMERATE_IRESEARCH_VIEW: {
      TRI_ASSERT(EngineType::Collection == type());
      auto& viewNode = *ExecutionNode::castTo<iresearch::IResearchViewNode*>(node);

      // evaluate node volatility before the distribution
      // can't do it on DB servers since only parts of the plan will be sent
      viewNode.volatility(true);

      _source = ViewSource(*viewNode.view().get(), findFirstGather(viewNode),
                           findFirstScatter(viewNode));
      break;
    }
    case ExecutionNode::INSERT:
    case ExecutionNode::UPDATE:
    case ExecutionNode::REMOVE:
    case ExecutionNode::REPLACE:
    case ExecutionNode::UPSERT: {
      TRI_ASSERT(EngineType::Collection == type());
      setRestrictedShard(ExecutionNode::castTo<ModificationNode*>(node), _source);
      break;
    }
    default:
      // do nothing
      break;
  }
  _nodes.emplace_back(node);
}

Collection const* EngineInfoContainerDBServer::EngineInfo::collection() const noexcept {
  TRI_ASSERT(EngineType::Collection == type());
  auto* source = boost::get<CollectionSource>(&_source);
  TRI_ASSERT(source);
  return source->collection;
}

void EngineInfoContainerDBServer::EngineInfo::collection(Collection* col) noexcept {
  TRI_ASSERT(EngineType::Collection == type());
  auto* source = boost::get<CollectionSource>(&_source);
  TRI_ASSERT(source);
  source->collection = col;
}

LogicalView const* EngineInfoContainerDBServer::EngineInfo::view() const noexcept {
  TRI_ASSERT(EngineType::View == type());
  auto* source = boost::get<ViewSource>(&_source);
  TRI_ASSERT(source);
  return source->view;
}

void EngineInfoContainerDBServer::EngineInfo::addClient(ServerID const& server) {
  TRI_ASSERT(EngineType::View == type());

  auto* source = boost::get<ViewSource>(&_source);
  TRI_ASSERT(source);

  if (source->scatter) {
    auto& clients = source->scatter->clients();
    TRI_ASSERT(clients.end() == std::find(clients.begin(), clients.end(), server));
    clients.emplace_back(server);
  }

  if (source->gather) {
    // FIXME introduce a separate step if sort mode detection will become heavy
    source->gather->sortMode(GatherNode::evaluateSortMode(++source->numClients));
  }
}

void EngineInfoContainerDBServer::EngineInfo::serializeSnippet(
    ServerID const& serverId, Query& query, std::vector<ShardID> const& shards,
    VPackBuilder& infoBuilder, bool isResponsibleForInitializeCursor) const {
  // The Key is required to build up the queryId mapping later
  // We're using serverId as queryId for the snippet since currently
  // it's impossible to have more than one view per engine
  std::string id = arangodb::basics::StringUtils::itoa(_idOfRemoteNode);
  id += ':';
  id += serverId;
  infoBuilder.add(VPackValue(id));

  TRI_ASSERT(!_nodes.empty());
  // copy the relevant fragment of the plan for the list of shards
  // Note that in these parts of the query there are no SubqueryNodes,
  // since they are all on the coordinator!

  ExecutionPlan plan(query.ast());
  ExecutionNode* previous = nullptr;

  for (auto enIt = _nodes.rbegin(), end = _nodes.rend(); enIt != end; ++enIt) {
    ExecutionNode const* current = *enIt;
    auto* clone = current->clone(&plan, false, false);
    auto const nodeType = clone->getType();

    // we need to count nodes by type ourselves, as we will set the
    // "varUsageComputed" flag below (which will handle the counting)
    plan.increaseCounter(nodeType);

    if (ExecutionNode::ENUMERATE_IRESEARCH_VIEW == nodeType) {
      auto* viewNode = ExecutionNode::castTo<iresearch::IResearchViewNode*>(clone);
      viewNode->shards() = shards;
    } else if (ExecutionNode::REMOTE == nodeType) {
      auto rem = ExecutionNode::castTo<RemoteNode*>(clone);
      // update the remote node with the information about the query
      rem->server("server:" + arangodb::ServerState::instance()->getId());
      rem->ownName(serverId);
      rem->queryId(_otherId);

      rem->isResponsibleForInitializeCursor(isResponsibleForInitializeCursor);
    }

    if (previous != nullptr) {
      clone->addDependency(previous);
    }

    previous = clone;
  }
  TRI_ASSERT(previous != nullptr);

  plan.root(previous);
  plan.setVarUsageComputed();
  // Always verbose
  const unsigned flags = ExecutionNode::SERIALIZE_DETAILS;
  plan.root()->toVelocyPack(infoBuilder, flags, /*keepTopLevelOpen*/ false);
}

void EngineInfoContainerDBServer::EngineInfo::serializeSnippet(
    Query& query, const ShardID& id, VPackBuilder& infoBuilder,
    bool isResponsibleForInitializeCursor) const {
  auto* collection = boost::get<CollectionSource>(&_source);
  TRI_ASSERT(collection);
  auto const& restrictedShards = collection->restrictedShards;

  if (!restrictedShards.empty()) {
    if (restrictedShards.find(id) == restrictedShards.end()) {
      return;
    }
    // We only have one shard it has to be responsible!
    isResponsibleForInitializeCursor = true;
  }
  // The Key is required to build up the queryId mapping later
  infoBuilder.add(VPackValue(arangodb::basics::StringUtils::itoa(_idOfRemoteNode) + ":" + id));

  TRI_ASSERT(!_nodes.empty());

  // build up a map of prototypes, e.g. c1 => c2, c2 => c3, c3 => c4,
  // so we can determine a common prototype ancestor in case we have 3- or 4-way joins
  std::unordered_map<aql::Collection const*, aql::Collection const*> prototypes;
  for (auto enIt = _nodes.rbegin(), end = _nodes.rend(); enIt != end; ++enIt) {
    auto const nodeType = (*enIt)->getType();
    if (nodeType == ExecutionNode::INDEX || nodeType == ExecutionNode::ENUMERATE_COLLECTION) {
      auto x = dynamic_cast<CollectionAccessingNode*>(*enIt);
      auto const* prototype = x->prototypeCollection();
      if (prototype == nullptr) {
        continue;
      }
      prototypes[x->collection()] = prototype;
    }
  }

  // copy the relevant fragment of the plan for each shard
  // Note that in these parts of the query there are no SubqueryNodes,
  // since they are all on the coordinator!
  // Also note: As _collection is set to the correct current shard
  // this clone does the translation collection => shardId implicitly
  // at the relevant parts of the query.

  std::unordered_set<aql::Collection*> cleanup;
  cleanup.emplace(collection->collection);

  collection->collection->setCurrentShard(id);

  ExecutionPlan plan(query.ast());
  ExecutionNode* previous = nullptr;

  for (auto enIt = _nodes.rbegin(), end = _nodes.rend(); enIt != end; ++enIt) {
    ExecutionNode const* current = *enIt;
    auto clone = current->clone(&plan, false, false);
    auto const nodeType = clone->getType();

    // we need to count nodes by type ourselves, as we will set the
    // "varUsageComputed" flag below (which will handle the counting)
    plan.increaseCounter(nodeType);
    
    if (nodeType == ExecutionNode::INDEX || nodeType == ExecutionNode::ENUMERATE_COLLECTION) {
      auto x = dynamic_cast<CollectionAccessingNode*>(clone);
      auto const* prototype = x->prototypeCollection();
      // find prototypes of prototypes
      while (prototype != nullptr) {
        auto it = prototypes.find(prototype);
        if (it == prototypes.end()) {
          break;
        }
        prototype = (*it).second;
      }

      if (prototype != nullptr) {
        auto s1 = prototype->shardIds();
        auto s2 = x->collection()->shardIds();
        if (s1->size() == s2->size()) {
          for (size_t i = 0; i < s1->size(); ++i) {
            if ((*s1)[i] == id) {
              // inject shard id into collection
              auto collection = const_cast<arangodb::aql::Collection*>(x->collection());
              collection->setCurrentShard((*s2)[i]);
              cleanup.emplace(collection);
              break;
            }
          }
        }
      }
    }

    if (ExecutionNode::REMOTE == nodeType) {
      auto rem = ExecutionNode::castTo<RemoteNode*>(clone);
      // update the remote node with the information about the query
      rem->server("server:" + arangodb::ServerState::instance()->getId());
      rem->ownName(id);
      rem->queryId(_otherId);

      rem->isResponsibleForInitializeCursor(isResponsibleForInitializeCursor);
    }

    if (previous != nullptr) {
      clone->addDependency(previous);
    }

    previous = clone;
  }
  TRI_ASSERT(previous != nullptr);

  plan.root(previous);
  plan.setVarUsageComputed();
  const unsigned flags = ExecutionNode::SERIALIZE_DETAILS;
  plan.root()->toVelocyPack(infoBuilder, flags, /*keepTopLevelOpen*/ false);

  // remove shard id hack for all participating collections 
  for (auto& it : cleanup) {
    it->resetCurrentShard();
  }
}

void EngineInfoContainerDBServer::CollectionInfo::mergeShards(
    std::shared_ptr<std::vector<ShardID>> const& shards) {
  for (auto const& s : *shards) {
    usedShards.emplace(s);
  }
}

EngineInfoContainerDBServer::EngineInfoContainerDBServer(Query* query) noexcept
    : _query(query) {}

void EngineInfoContainerDBServer::addNode(ExecutionNode* node) {
  TRI_ASSERT(node);
  TRI_ASSERT(!_engineStack.empty());
  _engineStack.top()->addNode(node);
  switch (node->getType()) {
    case ExecutionNode::ENUMERATE_COLLECTION: 
    case ExecutionNode::INDEX: {
      auto* scatter = findFirstScatter(*node);
      auto const* colNode = dynamic_cast<CollectionAccessingNode const*>(node);
      if (colNode == nullptr) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL, "unable to cast node to CollectionAccessingNode");
      }
      std::unordered_set<std::string> restrictedShard;
      if (colNode->isRestricted()) {
        restrictedShard.emplace(colNode->restrictedShard());
      }

      auto const* col = colNode->collection();
      handleCollection(col, AccessMode::Type::READ, scatter, restrictedShard);
      updateCollection(col);
      break;
    }
    case ExecutionNode::ENUMERATE_IRESEARCH_VIEW: {
      auto& viewNode = *ExecutionNode::castTo<iresearch::IResearchViewNode*>(node);
      auto* view = viewNode.view().get();

      for (aql::Collection const& col : viewNode.collections()) {
        auto& info = handleCollection(&col, AccessMode::Type::READ);
        info.views.push_back(view);
      }

      break;
    }
    case ExecutionNode::INSERT:
    case ExecutionNode::UPDATE:
    case ExecutionNode::REMOVE:
    case ExecutionNode::REPLACE:
    case ExecutionNode::UPSERT: {
      auto* scatter = findFirstScatter(*node);
      auto const& modNode = *ExecutionNode::castTo<ModificationNode const*>(node);
      auto const* col = modNode.collection();

      std::unordered_set<std::string> restrictedShard;
      if (modNode.isRestricted()) {
        restrictedShard.emplace(modNode.restrictedShard());
      }

      handleCollection(col,
                       modNode.getOptions().exclusive ? AccessMode::Type::EXCLUSIVE
                                                      : AccessMode::Type::WRITE,
                       scatter, restrictedShard);
      updateCollection(col);
      break;
    }
    default:
      // Do nothing
      break;
  };
}

void EngineInfoContainerDBServer::openSnippet(size_t idOfRemoteNode) {
  _engineStack.emplace(std::make_shared<EngineInfo>(idOfRemoteNode));
}

// Closing a snippet means:
// 1. pop it off the stack.
// 2. Wire it up with the given coordinator ID
// 3. Move it in the Collection => Engine map

void EngineInfoContainerDBServer::closeSnippet(QueryId coordinatorEngineId) {
  TRI_ASSERT(!_engineStack.empty());
  auto e = _engineStack.top();
  TRI_ASSERT(e);
  _engineStack.pop();

  e->connectQueryId(coordinatorEngineId);

  if (EngineInfo::EngineType::View == e->type()) {
    _viewInfos[e->view()].engines.emplace_back(std::move(e));
  } else {
    TRI_ASSERT(e->collection() != nullptr);
    auto it = _collectionInfos.find(e->collection());
    // This is not possible we have a snippet where no collection is involved
    TRI_ASSERT(it != _collectionInfos.end());
    if (it == _collectionInfos.end()) {
      THROW_ARANGO_EXCEPTION_MESSAGE(
          TRI_ERROR_INTERNAL,
          "created a DBServer QuerySnippet without a collection. This should "
          "not happen");
    }
    it->second.engines.emplace_back(std::move(e));
  }
}

/**
 * @brief Take care of this collection, set the lock state accordingly
 *        and maintain the list of used shards for this collection.
 *
 * @param col The collection that should be used
 * @param accessType The lock-type of this collection
 * @param restrictedShards The list of shards that can be relevant in this query
 * (a subset of the collection shards). Empty set means no restriction
 */
EngineInfoContainerDBServer::CollectionInfo& EngineInfoContainerDBServer::handleCollection(
    Collection const* col, AccessMode::Type const& accessType,
    ScatterNode* scatter /* = nullptr */,
    std::unordered_set<std::string> const& restrictedShards /*= {}*/
) {
  auto const shards = col->shardIds(
      restrictedShards.empty() ? _query->queryOptions().shardIds : restrictedShards);

  // What if we have an empty shard list here?
  if (shards->empty()) {
    // TODO FIXME
    LOG_TOPIC("0997e", WARN, arangodb::Logger::AQL)
        << "TEMPORARY: A collection access of a query has no result in any "
           "shard";
  }

  auto& info = _collectionInfos[col];

  // We need to upgrade the lock
  info.lockType = std::max(info.lockType, accessType);
  info.mergeShards(shards);

  if (scatter) {
    auto& clients = scatter->clients();
    clients.reserve(clients.size() + shards->size());
    std::copy(shards->begin(), shards->end(), std::back_inserter(clients));
  }

  return info;
}

// Then we update the collection pointer of the last engine.
#ifndef USE_ENTERPRISE
void EngineInfoContainerDBServer::updateCollection(Collection const* col) {
  TRI_ASSERT(!_engineStack.empty());
  auto e = _engineStack.top();
  // ... const_cast
  e->collection(const_cast<Collection*>(col));
}
#endif

void EngineInfoContainerDBServer::DBServerInfo::addShardLock(AccessMode::Type const& lock,
                                                             ShardID const& id) {
  _shardLocking[lock].emplace_back(id);
}

void EngineInfoContainerDBServer::DBServerInfo::addEngine(
    std::shared_ptr<EngineInfoContainerDBServer::EngineInfo> const& info, ShardID const& id) {
  _engineInfos[info].emplace_back(id);
}

void EngineInfoContainerDBServer::DBServerInfo::setShardAsResponsibleForInitializeCursor(
    ShardID const& id) {
  _shardsResponsibleForInitializeCursor.emplace(id);
}

void EngineInfoContainerDBServer::DBServerInfo::buildMessage(
    ServerID const& serverId, EngineInfoContainerDBServer const& context,
    Query& query, VPackBuilder& infoBuilder) const {
  TRI_ASSERT(infoBuilder.isEmpty());

  infoBuilder.openObject();
  infoBuilder.add(VPackValue("lockInfo"));
  infoBuilder.openObject();
  for (auto const& shardLocks : _shardLocking) {
    infoBuilder.add(VPackValue(AccessMode::typeString(shardLocks.first)));
    infoBuilder.openArray();
    for (auto const& s : shardLocks.second) {
      infoBuilder.add(VPackValue(s));
    }
    infoBuilder.close();  // The array
  }
  infoBuilder.close();  // lockInfo
  infoBuilder.add(VPackValue("options"));

  // toVelocyPack will open & close the "options" object
#ifdef USE_ENTERPRISE
  if (query.trx()->state()->options().skipInaccessibleCollections) {
    aql::QueryOptions opts = query.queryOptions();
    TRI_ASSERT(opts.transactionOptions.skipInaccessibleCollections);
    for (auto const& it : _engineInfos) {
      TRI_ASSERT(it.first);
      EngineInfo const& engine = *it.first;
      std::vector<ShardID> const& shards = it.second;

      if (engine.type() != EngineInfo::EngineType::View &&
          query.trx()->isInaccessibleCollectionId(engine.collection()->getPlanId())) {
        for (ShardID const& sid : shards) {
          opts.inaccessibleCollections.insert(sid);
        }
        opts.inaccessibleCollections.insert(std::to_string(engine.collection()->getPlanId()));
      }
    }
    opts.toVelocyPack(infoBuilder, true);
  } else {
    query.queryOptions().toVelocyPack(infoBuilder, true);
  }
#else
  query.queryOptions().toVelocyPack(infoBuilder, true);
#endif

  infoBuilder.add(VPackValue("variables"));
  // This will open and close an Object.
  query.ast()->variables()->toVelocyPack(infoBuilder);
  infoBuilder.add(VPackValue("snippets"));
  infoBuilder.openObject();

  auto isResponsibleForInitializeCursor = [this](ShardID const& id) {
    return _shardsResponsibleForInitializeCursor.find(id) !=
           _shardsResponsibleForInitializeCursor.end();
  };

  auto isAnyResponsibleForInitializeCursor =
      [&isResponsibleForInitializeCursor](std::vector<ShardID> const& ids) {
        for (auto const& id : ids) {
          if (isResponsibleForInitializeCursor(id)) {
            return true;
          }
        }
        return false;
      };

  for (auto const& it : _engineInfos) {
    TRI_ASSERT(it.first);
    EngineInfo& engine = *it.first;
    std::vector<ShardID> const& shards = it.second;

    // serialize for the list of shards
    switch (engine.type()) {
      case EngineInfo::EngineType::View: {
        engine.serializeSnippet(serverId, query, shards, infoBuilder,
                                isAnyResponsibleForInitializeCursor(shards));
        engine.addClient(serverId);
      } break;
      case EngineInfo::EngineType::Collection: {
        for (auto const& shard : shards) {
          engine.serializeSnippet(query, shard, infoBuilder,
                                  isResponsibleForInitializeCursor(shard));
        }
      } break;
    }
  }
  infoBuilder.close();  // snippets
  injectTraverserEngines(infoBuilder);
  infoBuilder.close();  // Object
}

void EngineInfoContainerDBServer::DBServerInfo::injectTraverserEngines(VPackBuilder& infoBuilder) const {
  if (_traverserEngineInfos.empty()) {
    return;
  }
  TRI_ASSERT(infoBuilder.isOpenObject());
  infoBuilder.add(VPackValue("traverserEngines"));
  infoBuilder.openArray();
  for (auto const& it : _traverserEngineInfos) {
    GraphNode* en = it.first;
    TraverserEngineShardLists const& list = it.second;
    infoBuilder.openObject();
    {
      // Options
      infoBuilder.add(VPackValue("options"));
      graph::BaseOptions* opts = en->options();
      opts->buildEngineInfo(infoBuilder);
    }
    {
      // Variables
      std::vector<aql::Variable const*> vars;
      en->getConditionVariables(vars);
      if (!vars.empty()) {
        infoBuilder.add(VPackValue("variables"));
        infoBuilder.openArray();
        for (auto v : vars) {
          v->toVelocyPack(infoBuilder);
        }
        infoBuilder.close();
      }
    }

    infoBuilder.add(VPackValue("shards"));
    infoBuilder.openObject();
    infoBuilder.add(VPackValue("vertices"));
    infoBuilder.openObject();
    for (auto const& col : list.vertexCollections) {
      infoBuilder.add(VPackValue(col.first));
      infoBuilder.openArray();
      for (auto const& v : col.second) {
        infoBuilder.add(VPackValue(v));
      }
      infoBuilder.close();  // this collection
    }
    infoBuilder.close();  // vertices

    infoBuilder.add(VPackValue("edges"));
    infoBuilder.openArray();
    for (auto const& edgeShards : list.edgeCollections) {
      infoBuilder.openArray();
      for (auto const& e : edgeShards) {
        infoBuilder.add(VPackValue(e));
      }
      infoBuilder.close();
    }
    infoBuilder.close();  // edges

#ifdef USE_ENTERPRISE
    if (!list.inaccessibleShards.empty()) {
      infoBuilder.add(VPackValue("inaccessible"));
      infoBuilder.openArray();
      for (ShardID const& shard : list.inaccessibleShards) {
        infoBuilder.add(VPackValue(shard));
      }
      infoBuilder.close();  // inaccessible
    }
#endif
    infoBuilder.close();  // shards

    en->enhanceEngineInfo(infoBuilder);

    infoBuilder.close();  // base
  }

  infoBuilder.close();  // traverserEngines
}

void EngineInfoContainerDBServer::DBServerInfo::combineTraverserEngines(ServerID const& serverID,
                                                                        VPackSlice const ids) {
  if (ids.length() != _traverserEngineInfos.size()) {
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
                                   "The DBServer was not able to create enough "
                                   "traversal engines. This can happen during "
                                   "failover. Please check; " +
                                       serverID);
  }
  auto idIter = VPackArrayIterator(ids);
  // We need to use the same order of iterating over
  // the traverserEngineInfos to wire the correct GraphNodes
  // to the correct engine ids
  for (auto const& it : _traverserEngineInfos) {
    it.first->addEngine(idIter.value().getNumber<traverser::TraverserEngineID>(), serverID);
    idIter.next();
  }
}

void EngineInfoContainerDBServer::DBServerInfo::addTraverserEngine(GraphNode* node,
                                                                   TraverserEngineShardLists&& shards) {
  _traverserEngineInfos.emplace_back(std::make_pair(node, std::move(shards)));
}

std::map<ServerID, EngineInfoContainerDBServer::DBServerInfo> EngineInfoContainerDBServer::createDBServerMapping() const {
  auto* ci = ClusterInfo::instance();
  TRI_ASSERT(ci);

  std::map<ServerID, DBServerInfo> dbServerMapping;

  // Only one remote block of each remote node is responsible for forwarding the
  // initializeCursor and shutDown requests.
  // For simplicity, we always use the first remote block if we have more
  // than one.
  using ExecutionNodeId = size_t;
  std::unordered_map<ExecutionNodeId, std::pair<ServerID, ShardID>> responsibleForShutdown{};
  auto const chooseResponsibleSnippet =
      [&responsibleForShutdown](EngineInfo const& engineInfo,
                                ServerID const& dbServerId, ShardID const& shardId) {
        for (auto const& node : engineInfo.nodes()) {
          if (node->getType() == ExecutionNode::NodeType::REMOTE) {
            // Try to emplace. We explicitly want to ignore duplicate inserts
            // here, as we want one snippet per query!
            std::ignore =
                responsibleForShutdown.emplace(node->id(),
                                               std::make_pair(dbServerId, shardId));
          }
        }
      };

  for (auto const& it : _collectionInfos) {
    // it.first => Collection const*
    // it.second.lockType => Lock Type
    // it.second.engines => All Engines using this collection
    // it.second.usedShards => All shards of this collection releveant for this
    // query
    auto const& colInfo = it.second;

    for (auto const& shardId : colInfo.usedShards) {
      auto const servers = ci->getResponsibleServer(shardId);

      if (!servers || servers->empty()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE,
            "Could not find responsible server for shard " + shardId);
      }

      auto const& dbServerId = (*servers)[0];

      auto& mapping = dbServerMapping[dbServerId];

      mapping.addShardLock(colInfo.lockType, shardId);

      for (auto const& e : colInfo.engines) {
        mapping.addEngine(e, shardId);
        chooseResponsibleSnippet(*e, dbServerId, shardId);
      }

      for (auto const* view : colInfo.views) {
        auto const viewInfo = _viewInfos.find(view);

        if (viewInfo == _viewInfos.end()) {
          continue;
        }

        for (auto const& viewEngine : viewInfo->second.engines) {
          mapping.addEngine(viewEngine, shardId);
          chooseResponsibleSnippet(*viewEngine, dbServerId, shardId);
        }
      }
    }
  }

  // Set one DBServer snippet to be responsible for each coordinator query snippet
  for (auto const& it : responsibleForShutdown) {
    ServerID const& serverId = it.second.first;
    ShardID const& shardId = it.second.second;
    dbServerMapping[serverId].setShardAsResponsibleForInitializeCursor(shardId);
  }

#ifdef USE_ENTERPRISE
  prepareSatellites(dbServerMapping);
#endif

  return dbServerMapping;
}

void EngineInfoContainerDBServer::injectGraphNodesToMapping(
    std::map<ServerID, EngineInfoContainerDBServer::DBServerInfo>& dbServerMapping) const {
  if (_graphNodes.empty()) {
    return;
  }

#ifdef USE_ENTERPRISE
  transaction::Methods* trx = _query->trx();
  transaction::Options& trxOps = _query->trx()->state()->options();
#endif

  auto clusterInfo = arangodb::ClusterInfo::instance();

  /// Typedef for a complicated mapping used in TraverserEngines.
  typedef std::unordered_map<ServerID, EngineInfoContainerDBServer::TraverserEngineShardLists> Serv2ColMap;

  for (GraphNode* en : _graphNodes) {
    // Every node needs it's own Serv2ColMap
    Serv2ColMap mappingServerToCollections;
    en->prepareOptions();

    std::vector<std::unique_ptr<arangodb::aql::Collection>> const& edges =
        en->edgeColls();

    // Here we create a mapping
    // ServerID => ResponsibleShards
    // Where Responsible shards is divided in edgeCollections and
    // vertexCollections
    // For edgeCollections the Ordering is important for the index access.
    // Also the same edgeCollection can be included twice (iff direction is ANY)
    size_t length = edges.size();

    auto findServerLists = [&](ShardID const& shard) -> Serv2ColMap::iterator {
      auto serverList = clusterInfo->getResponsibleServer(shard);
      if (serverList->empty()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(
            TRI_ERROR_CLUSTER_BACKEND_UNAVAILABLE,
            "Could not find responsible server for shard " + shard);
      }
      TRI_ASSERT(!serverList->empty());
      auto& leader = (*serverList)[0];
      auto pair = mappingServerToCollections.find(leader);
      if (pair == mappingServerToCollections.end()) {
        mappingServerToCollections.emplace(leader, TraverserEngineShardLists{length});
        pair = mappingServerToCollections.find(leader);
      }
      return pair;
    };

    std::unordered_set<std::string> const& restrictToShards =
        _query->queryOptions().shardIds;
    for (size_t i = 0; i < length; ++i) {
      auto shardIds = edges[i]->shardIds(restrictToShards);
      for (auto const& shard : *shardIds) {
        auto pair = findServerLists(shard);
        pair->second.edgeCollections[i].emplace_back(shard);
      }
    }

    std::vector<std::unique_ptr<arangodb::aql::Collection>> const& vertices =
        en->vertexColls();
    if (vertices.empty()) {
      std::unordered_set<std::string> knownEdges;
      for (auto const& it : edges) {
        knownEdges.emplace(it->name());
      }

      TRI_ASSERT(_query);
      auto& resolver = _query->resolver();

      // This case indicates we do not have a named graph. We simply use
      // ALL collections known to this query.
      std::map<std::string, Collection*> const* cs = _query->collections()->collections();
      for (auto const& collection : (*cs)) {
        if (!resolver.getCollection(collection.first)) {
          // not a collection, filter out
          continue;
        }

        if (knownEdges.find(collection.second->name()) == knownEdges.end()) {
          // This collection is not one of the edge collections used in this
          // graph.
          auto shardIds = collection.second->shardIds(restrictToShards);
          for (ShardID const& shard : *shardIds) {
            auto pair = findServerLists(shard);
            pair->second.vertexCollections[collection.second->name()].emplace_back(shard);
#ifdef USE_ENTERPRISE
            if (trx->isInaccessibleCollectionId(collection.second->getPlanId())) {
              TRI_ASSERT(ServerState::instance()->isSingleServerOrCoordinator());
              TRI_ASSERT(trxOps.skipInaccessibleCollections);
              pair->second.inaccessibleShards.insert(shard);
              pair->second.inaccessibleShards.insert(
                  std::to_string(collection.second->getCollection()->id()));
            }
#endif
          }
        }
      }
      // We have to make sure that all engines at least know all vertex
      // collections.
      // Thanks to fanout...
      for (auto const& collection : (*cs)) {
        for (auto& entry : mappingServerToCollections) {
          // implicity creates the map entry in case it does not exist
          entry.second.vertexCollections[collection.second->name()];
        }
      }
    } else {
      // This Traversal is started with a GRAPH. It knows all relevant
      // collections.
      for (auto const& it : vertices) {
        auto shardIds = it->shardIds(restrictToShards);
        for (ShardID const& shard : *shardIds) {
          auto pair = findServerLists(shard);
          pair->second.vertexCollections[it->name()].emplace_back(shard);
#ifdef USE_ENTERPRISE
          if (trx->isInaccessibleCollectionId(it->getPlanId())) {
            TRI_ASSERT(trxOps.skipInaccessibleCollections);
            pair->second.inaccessibleShards.insert(shard);
            pair->second.inaccessibleShards.insert(
                std::to_string(it->getCollection()->id()));
          }
#endif
        }
      }
      // We have to make sure that all engines at least know all vertex
      // collections.
      // Thanks to fanout...
      for (auto const& it : vertices) {
        for (auto& entry : mappingServerToCollections) {
          // implicitly creates the map entry in case it does not exist.
          entry.second.vertexCollections[it->name()];
        }
      }
    }

    // Now we have sorted all collections on the db servers. Hand the engines
    // over
    // to the server builder.

    // NOTE: This is only valid because it is single threaded and we do not
    // have concurrent access. We move out stuff from this Map => memory will
    // get corrupted if we would read it again.
    for (auto it : mappingServerToCollections) {
      // This condition is guaranteed because all shards have been prepared
      // for locking in the EngineInfos
      TRI_ASSERT(dbServerMapping.find(it.first) != dbServerMapping.end());
      dbServerMapping.find(it.first)->second.addTraverserEngine(en, std::move(it.second));
    }
  }
}

Result EngineInfoContainerDBServer::buildEngines(MapRemoteToSnippet& queryIds) const {
  TRI_ASSERT(_engineStack.empty());

  // We create a map for DBServer => All Query snippets executed there
  auto dbServerMapping = createDBServerMapping();
  // This Mapping does not contain Traversal Engines
  //
  // We add traversal engines if necessary
  injectGraphNodesToMapping(dbServerMapping);

  auto cc = ClusterComm::instance();
  if (cc == nullptr) {
    // nullptr only happens on controlled shutdown
    return {TRI_ERROR_SHUTTING_DOWN};
  }

  double ttl = _query->queryOptions().ttl;

  std::string const url(
      "/_db/" + arangodb::basics::StringUtils::urlEncode(_query->vocbase().name()) +
      "/_api/aql/setup?ttl=" + std::to_string(ttl));

  auto cleanupGuard = scopeGuard([this, &cc, &queryIds]() {
    cleanupEngines(cc, TRI_ERROR_INTERNAL, _query->vocbase().name(), queryIds);
  });

  // Build Lookup Infos
  VPackBuilder infoBuilder;  
  transaction::Methods* trx = _query->trx();

  // we need to lock per server in a deterministic order to avoid deadlocks
  for (auto& it : dbServerMapping) {
    std::string const serverDest = "server:" + it.first;

    LOG_TOPIC("4bbe6", DEBUG, arangodb::Logger::AQL) << "Building Engine Info for " << it.first;
    infoBuilder.clear();
    it.second.buildMessage(it.first, *this, *_query, infoBuilder);
    LOG_TOPIC("2f1fd", DEBUG, arangodb::Logger::AQL)
        << "Sending the Engine info: " << infoBuilder.toJson();

    // Now we send to DBServers.
    // We expect a body with {snippets: {id => engineId}, traverserEngines:
    // [engineId]}}
    
    // add the transaction ID header
    std::unordered_map<std::string, std::string> headers;
    ClusterTrxMethods::addAQLTransactionHeader(*trx, /*server*/ it.first, headers);

    CoordTransactionID coordTransactionID = TRI_NewTickServer();

    _query->incHttpRequests(1);
    auto res = cc->syncRequest(coordTransactionID, serverDest, RequestType::POST,
                               url, infoBuilder.toJson(), headers, SETUP_TIMEOUT);

    if (res->getErrorCode() != TRI_ERROR_NO_ERROR) {
      LOG_TOPIC("f9a77", DEBUG, Logger::AQL)
          << it.first << " responded with " << res->getErrorCode() << " -> "
          << res->stringifyErrorMessage();
      LOG_TOPIC("41082", TRACE, Logger::AQL) << infoBuilder.toJson();
      return {res->getErrorCode(), res->stringifyErrorMessage()};
    }

    std::shared_ptr<VPackBuilder> builder = res->result->getBodyVelocyPack();
    VPackSlice response = builder->slice();

    if (!response.isObject() || !response.get("result").isObject()) {
      LOG_TOPIC("0c3f2", ERR, Logger::AQL) << "Received error information from "
                                  << it.first << " : " << response.toJson();
      return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
              "Unable to deploy query on all required "
              "servers. This can happen during "
              "failover. Please check: " +
                  it.first};
    }

    VPackSlice result = response.get("result");
    VPackSlice snippets = result.get("snippets");

    for (auto const& resEntry : VPackObjectIterator(snippets)) {
      if (!resEntry.value.isString()) {
        return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
                "Unable to deploy query on all required "
                "servers. This can happen during "
                "failover. Please check: " +
                    it.first};
      }
      size_t remoteId = 0;
      std::string shardId = "";
      auto res = ExtractRemoteAndShard(resEntry.key, remoteId, shardId);
      if (!res.ok()) {
        // FIXME in case if that fails, there will
        // be a segfault somewhere in 'cleanup'
        return res;
      }
      TRI_ASSERT(remoteId != 0);
      TRI_ASSERT(!shardId.empty());
      auto& remote = queryIds[remoteId];
      auto& thisServer = remote[serverDest];
      thisServer.emplace_back(resEntry.value.copyString());
    }

    VPackSlice travEngines = result.get("traverserEngines");
    if (!travEngines.isNone()) {
      if (!travEngines.isArray()) {
        return {TRI_ERROR_CLUSTER_AQL_COMMUNICATION,
                "Unable to deploy query on all required "
                "servers. This can happen during "
                "failover. Please check: " +
                    it.first};
      }

      it.second.combineTraverserEngines(it.first, travEngines);
    }
  }

#ifdef USE_ENTERPRISE
  resetSatellites();
#endif
  cleanupGuard.cancel();
  return TRI_ERROR_NO_ERROR;
}

void EngineInfoContainerDBServer::addGraphNode(GraphNode* node) {
  // Add all Edge Collections to the Transactions, Traversals do never write
  for (auto const& col : node->edgeColls()) {
    handleCollection(col.get(), AccessMode::Type::READ);
  }

  // Add all Vertex Collections to the Transactions, Traversals do never write
  auto& vCols = node->vertexColls();
  if (vCols.empty()) {
    TRI_ASSERT(_query);
    auto& resolver = _query->resolver();

    // This case indicates we do not have a named graph. We simply use
    // ALL collections known to this query.
    std::map<std::string, Collection*> const* cs = _query->collections()->collections();
    for (auto const& col : *cs) {
      if (!resolver.getCollection(col.first)) {
        // not a collection, filter out
        continue;
      }

      handleCollection(col.second, AccessMode::Type::READ);
    }
  } else {
    for (auto const& col : node->vertexColls()) {
      handleCollection(col.get(), AccessMode::Type::READ);
    }
  }

  _graphNodes.emplace_back(node);
}


namespace {
struct NoopCb final : public arangodb::ClusterCommCallback {
  bool operator()(ClusterCommResult*) override{
    return true;
  }
};
}

/**
 * @brief Will send a shutdown to all engines registered in the list of
 * queryIds.
 * NOTE: This function will ignore all queryids where the key is not of
 * the expected format
 * they may be leftovers from Coordinator.
 * Will also clear the list of queryIds after return.
 *
 * @param cc The ClusterComm
 * @param errorCode error Code to be send to DBServers for logging.
 * @param dbname Name of the database this query is executed in.
 * @param queryIds A map of QueryIds of the format: (remoteNodeId:shardId) ->
 * queryid.
 */
void EngineInfoContainerDBServer::cleanupEngines(std::shared_ptr<ClusterComm> cc,
                                                 int errorCode, std::string const& dbname,
                                                 MapRemoteToSnippet& queryIds) const {
  // Shutdown query snippets
  std::string url("/_db/" + arangodb::basics::StringUtils::urlEncode(dbname) +
                  "/_api/aql/shutdown/");
  std::vector<ClusterCommRequest> requests;
  auto body = std::make_shared<std::string>(
      "{\"code\":" + std::to_string(errorCode) + "}");
  for (auto const& it : queryIds) {
    // it.first == RemoteNodeId, we don't need this
    // it.second server -> [snippets]
    for (auto const& serToSnippets : it.second) {
      auto server = serToSnippets.first;
      for (auto const& shardId : serToSnippets.second) {
        requests.emplace_back(server, rest::RequestType::PUT, url + shardId, body);
      }
    }
  }
  
  // Shutdown traverser engines
  url = "/_db/" + arangodb::basics::StringUtils::urlEncode(dbname) +
        "/_internal/traverser/";
  std::unordered_map<std::string, std::string> headers;
  std::shared_ptr<std::string> noBody;
  
  CoordTransactionID coordinatorTransactionID = TRI_NewTickServer();
  auto cb = std::make_shared<::NoopCb>();
  
  constexpr double shortTimeout = 10.0;  // Picked arbitrarily
  for (auto const& gn : _graphNodes) {
    auto allEngines = gn->engines();
    for (auto const& engine : *allEngines) {
      cc->asyncRequest(coordinatorTransactionID, engine.first, rest::RequestType::DELETE_REQ,
                       url + basics::StringUtils::itoa(engine.second), noBody, headers, cb,
                       shortTimeout, false, 2.0);
    }
    _query->incHttpRequests(allEngines->size());
  }
  
  queryIds.clear();
}
