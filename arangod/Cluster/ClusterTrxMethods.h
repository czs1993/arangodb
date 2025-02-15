////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_CLUSTER_CLUSTER_TRX_METHODS_H
#define ARANGOD_CLUSTER_CLUSTER_TRX_METHODS_H 1

#include "Basics/Common.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/voc-types.h"

#include <velocypack/Slice.h>

namespace arangodb {

struct OperationOptions;
class TransactionState;

namespace ClusterTrxMethods {

/// @brief begin a transaction on all followers
arangodb::Result beginTransactionOnLeaders(TransactionState&,
                                           std::vector<ServerID> const& leaders);

/// @brief begin a transaction on all followers
arangodb::Result beginTransactionOnFollowers(transaction::Methods& trx,
                                             arangodb::FollowerInfo& info,
                                             std::vector<ServerID> const& followers);

/// @brief commit a transaction on a subordinate
arangodb::Result commitTransaction(transaction::Methods& trx);

/// @brief commit a transaction on a subordinate
arangodb::Result abortTransaction(transaction::Methods& trx);

/// @brief add the transaction ID header for servers
template <typename MapT>
void addTransactionHeader(transaction::Methods const& trx,
                          ServerID const& server, MapT& headers);

/// @brief add transaction ID header for setting up AQL snippets
void addAQLTransactionHeader(transaction::Methods const& trx, ServerID const& server,
                             std::unordered_map<std::string, std::string>& headers);

/// @brief check whether this is any kind el cheapo transaction
bool isElCheapo(transaction::Methods const& trx);
bool isElCheapo(TransactionState const& state);
}  // namespace ClusterTrxMethods
}  // namespace arangodb

#endif
