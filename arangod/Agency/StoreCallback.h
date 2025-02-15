////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Kaveh Vahedipour
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CONSENSUS_STORE_CALLBACK_H
#define ARANGOD_CONSENSUS_STORE_CALLBACK_H 1

#include "Cluster/ClusterComm.h"

namespace arangodb {
namespace consensus {

class Agent;

class StoreCallback : public arangodb::ClusterCommCallback {
public:
  StoreCallback(std::string const&, query_t const&, Agent* agent);

  bool operator()(arangodb::ClusterCommResult*) override final;

private:
  std::string _url;
  query_t _body;
  Agent* _agent;
};
}  // namespace consensus
}  // namespace arangodb

#endif
