////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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

#include "common.h"
#include "gtest/gtest.h"

#include "../Mocks/StorageEngineMock.h"

#if USE_ENTERPRISE
#include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "3rdParty/iresearch/tests/tests_config.hpp"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/Ast.h"
#include "Aql/OptimizerRulesFeature.h"
#include "Aql/Query.h"
#include "Basics/VelocyPackHelper.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchView.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/FlushFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "Sharding/ShardingFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "V8/v8-globals.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"

#include "IResearch/VelocyPackHelper.h"
#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "utils/utf8_path.hpp"

#include <velocypack/Iterator.h>

extern const char* ARGV0;  // defined in main.cpp

namespace {

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class IResearchQueryNumericTermTest : public ::testing::Test {
 protected:
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;

  IResearchQueryNumericTermTest() : engine(server), server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init(true);

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    // suppress WARNING {authentication} --server.jwt-secret is insecure. Use --server.jwt-secret-keyfile instead
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(),
                                    arangodb::LogLevel::ERR);

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AQL.name(), arangodb::LogLevel::ERR);  // suppress WARNING {aql} Suboptimal AqlItemMatrix index lookup:
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::ERR);  // suppress WARNING DefaultCustomTypeHandler called
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(),
                                    arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);

    // setup required application features
    features.emplace_back(new arangodb::FlushFeature(server), false);
    features.emplace_back(new arangodb::ViewTypesFeature(server), true);
    features.emplace_back(new arangodb::AuthenticationFeature(server), true);
    features.emplace_back(new arangodb::DatabasePathFeature(server), false);
    features.emplace_back(new arangodb::DatabaseFeature(server), false);
    features.emplace_back(new arangodb::QueryRegistryFeature(server), false);  // must be first
    arangodb::application_features::ApplicationServer::server->addFeature(
        features.back().first);  // need QueryRegistryFeature feature to be added now in order to create the system database
    system = irs::memory::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL,
                                                     0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::SystemDatabaseFeature(server, system.get()),
                          false);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(server), false);  // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(server), true);
    features.emplace_back(new arangodb::aql::OptimizerRulesFeature(server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(server), true);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::ShardingFeature(server), false);
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(server), true);

#if USE_ENTERPRISE
    features.emplace_back(new arangodb::LdapFeature(server),
                          false);  // required for AuthenticationFeature with USE_ENTERPRISE
#endif

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    auto* dbPathFeature =
        arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>(
            "DatabasePath");
    arangodb::tests::setDatabasePath(*dbPathFeature);  // ensure test data is stored in a unique directory
  }

  ~IResearchQueryNumericTermTest() {
    system.reset();  // destroy before reseting the 'ENGINE'
    arangodb::AqlFeature(server).stop();  // unset singleton instance
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AQL.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(),
                                    arangodb::LogLevel::DEFAULT);
  }
};  // IResearchQuerySetup

}  // namespace

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchQueryNumericTermTest, test) {
  // ArangoDB specific string comparer
  struct StringComparer {
    bool operator()(irs::string_ref const& lhs, irs::string_ref const& rhs) const {
      return arangodb::basics::VelocyPackHelper::compareStringValues(
                 lhs.c_str(), lhs.size(), rhs.c_str(), rhs.size(), true) < 0;
    }
  };  // StringComparer

  static std::vector<std::string> const EMPTY;

  auto createJson = arangodb::velocypack::Parser::fromJson(
      "{ \
    \"name\": \"testView\", \
    \"type\": \"arangosearch\" \
  }");

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1,
                        "testVocbase");
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection1;
  std::shared_ptr<arangodb::LogicalCollection> logicalCollection2;

  // add collection_1
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"collection_1\" }");
    logicalCollection1 = vocbase.createCollection(collectionJson->slice());
    ASSERT_TRUE((nullptr != logicalCollection1));
  }

  // add collection_2
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"collection_2\" }");
    logicalCollection2 = vocbase.createCollection(collectionJson->slice());
    ASSERT_TRUE((nullptr != logicalCollection2));
  }

  // add view
  auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
      vocbase.createView(createJson->slice()));
  ASSERT_TRUE((false == !view));

  // add link to collection
  {
    auto updateJson = arangodb::velocypack::Parser::fromJson(
        "{ \"links\" : {"
        "\"collection_1\" : { \"includeAllFields\" : true },"
        "\"collection_2\" : { \"includeAllFields\" : true }"
        "}}");
    EXPECT_TRUE((view->properties(updateJson->slice(), true).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->properties(builder, arangodb::LogicalDataSource::makeFlags(
                                  arangodb::LogicalDataSource::Serialize::Detailed));
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_TRUE(slice.get("name").copyString() == "testView");
    EXPECT_TRUE(slice.get("type").copyString() ==
                arangodb::iresearch::DATA_SOURCE_TYPE.name());
    EXPECT_TRUE(slice.get("deleted").isNone());  // no system properties
    auto tmpSlice = slice.get("links");
    EXPECT_TRUE((true == tmpSlice.isObject() && 2 == tmpSlice.length()));
  }

  std::deque<arangodb::ManagedDocumentResult> insertedDocs;

  // populate view with the data
  {
    arangodb::OperationOptions opt;

    arangodb::transaction::Methods trx(arangodb::transaction::StandaloneContext::Create(vocbase),
                                       EMPTY, EMPTY, EMPTY,
                                       arangodb::transaction::Options());
    EXPECT_TRUE((trx.begin().ok()));

    // insert into collections
    {
      irs::utf8_path resource;
      resource /= irs::string_ref(arangodb::tests::testResourceDir);
      resource /= irs::string_ref("simple_sequential.json");

      auto builder =
          arangodb::basics::VelocyPackHelper::velocyPackFromFile(resource.utf8());
      auto root = builder.slice();
      ASSERT_TRUE(root.isArray());

      size_t i = 0;

      std::shared_ptr<arangodb::LogicalCollection> collections[]{logicalCollection1,
                                                                 logicalCollection2};

      for (auto doc : arangodb::velocypack::ArrayIterator(root)) {
        insertedDocs.emplace_back();
        auto const res =
            collections[i % 2]->insert(&trx, doc, insertedDocs.back(), opt, false);
        EXPECT_TRUE(res.ok());
        ++i;
      }
    }

    EXPECT_TRUE((trx.commit().ok()));
    EXPECT_TRUE(
        (arangodb::tests::executeQuery(vocbase,
                                       "FOR d IN testView SEARCH 1 ==1 OPTIONS "
                                       "{ waitForSync: true } RETURN d")
             .result.ok()));  // commit
  }

  // -----------------------------------------------------------------------------
  // --SECTION-- ==
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == '0' RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == true RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == false RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == null RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // missing term
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == -1 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.value == 90.564, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs{
        {12, &insertedDocs[12]}};

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.value == 90.564 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.value == -32.5, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs{
        {16, &insertedDocs[16]}};

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.value == -32.5 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq == 2, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs{
        {2, &insertedDocs[2]}};

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == 2 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq == 2.0, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs{
        {2, &insertedDocs[2]}};

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq == 2.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.value == 100.0, TFIDF() ASC, BM25() ASC, d.seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<ptrdiff_t>();
      if (value != 100) {
        continue;
      }
      expectedDocs.emplace(keySlice.getNumber<size_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH 100.0 == d.value SORT BM25(d) ASC, TFIDF(d) "
        "ASC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION-- !=
  // -----------------------------------------------------------------------------

  // invalid type, unordered
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      expectedDocs.emplace(arangodb::iresearch::getStringRef(keySlice), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq != '0' RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("name");
      auto const key = arangodb::iresearch::getStringRef(keySlice);

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // invalid type, unordered
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      expectedDocs.emplace(arangodb::iresearch::getStringRef(keySlice), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq != false RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("name");
      auto const key = arangodb::iresearch::getStringRef(keySlice);

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // invalid type, d.seq DESC
  {
    std::map<ptrdiff_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      expectedDocs.emplace(keySlice.getNumber<ptrdiff_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq != null SORT d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // missing term, unordered
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      expectedDocs.emplace(arangodb::iresearch::getStringRef(keySlice), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq != -1 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("name");
      auto const key = arangodb::iresearch::getStringRef(keySlice);

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // existing duplicated term, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      auto const valueSlice = docSlice.get("value");

      if (!valueSlice.isNone() && valueSlice.getNumber<ptrdiff_t>() == 100) {
        continue;
      }

      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.value != 100 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // existing unique term, unordered
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      expectedDocs.emplace(arangodb::iresearch::getStringRef(keySlice), &doc);
    }
    expectedDocs.erase("C");

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq != 2.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("name");
      auto const key = arangodb::iresearch::getStringRef(keySlice);

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // missing term, seq DESC
  {
    std::vector<arangodb::ManagedDocumentResult const*> expectedDocs;

    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const fieldSlice = docSlice.get("value");

      if (!fieldSlice.isNone() &&
          (fieldSlice.isNumber() && -1. == fieldSlice.getNumber<double>())) {
        continue;
      }

      expectedDocs.emplace_back(&doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value != -1 SORT d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice((*expectedDoc)->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // existing duplicated term, TFIDF() ASC, BM25() ASC, seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");

      if (!valueSlice.isNone() && 123 == valueSlice.getNumber<ptrdiff_t>()) {
        continue;
      }

      auto const keySlice = docSlice.get("seq");
      expectedDocs.emplace(keySlice.getNumber<size_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH 123 != d.value SORT TFIDF(d) ASC, BM25(d) "
        "ASC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(resultIt.size() == expectedDocs.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION-- <
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq < '0' RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq < true RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq < false RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq < null RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq < 7, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key >= 7) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq < 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq < 0 (less than min term), unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq < 0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq < 31 (less than max term), BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key >= 31) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq < 31 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value < 0, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() >= 0) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value < 0 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value < 95, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() >= 95) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value < 95 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION-- <=
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq <= '0' RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq <= true RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq <= false RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq <= null RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq <= 7, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key > 7) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq <= 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq <= 0 (less or equal than min term), unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq <= 0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(1 == resultIt.size());
    EXPECT_TRUE(resultIt.valid());

    auto const resolved = resultIt.value().resolveExternals();
    EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                         arangodb::velocypack::Slice(insertedDocs[0].vpack()), resolved, true));

    resultIt.next();
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq <= 31 (less or equal than max term), BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key > 31) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq <= 31 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value <= 0, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() > 0) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value <= 0 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value <= 95, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() > 95) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value <= 95 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION-- >
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > '0' RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > true RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > false RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > null RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 7, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq > 31 (greater than max term), unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 31 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 0 (less or equal than min term), BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 0) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult =
        arangodb::tests::executeQuery(vocbase,
                                      "FOR d IN testView SEARCH d.seq > 0 SORT "
                                      "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > 0, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() <= 0) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value > 0 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > 95, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() <= 95) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value > 95 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION-- >=
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= '0' RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= true RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= false RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= null RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 7, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key < 7) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq >= 31 (greater than max term), unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= 31 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(1 == resultIt.size());
    EXPECT_TRUE(resultIt.valid());

    auto const resolved = resultIt.value().resolveExternals();
    EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                         arangodb::velocypack::Slice(insertedDocs[31].vpack()),
                         resolved, true));

    resultIt.next();
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 0 (less or equal than min term), BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 0 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= 0, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() < 0) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value >= 0 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > 95, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone() || valueSlice.getNumber<ptrdiff_t>() < 95) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value >= 95 SORT BM25(d), TFIDF(d), d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                      Range (>, <)
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > '0' AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > true AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > false AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > null AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 7 AND d.name < 18, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 7 AND d.seq < 18 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq > 7 AND d.seq < 18, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > 7.1 AND d.seq < 17.9 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq > 18 AND d.seq < 7 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 18 AND d.seq < 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 7 AND d.seq < 7.0 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 7 AND d.seq < 7.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 0 AND d.seq < 31 , TFIDF() ASC, BM25() ASC, d.name DESC
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*, StringComparer> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 0 || key >= 31) {
        continue;
      }
      expectedDocs.emplace(arangodb::iresearch::getStringRef(
                               docSlice.get("name")),
                           &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > 0 AND d.seq < 31 SORT tfidf(d), "
        "BM25(d), d.name DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > 90.564 AND d.value < 300, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value <= 90.564 || value >= 300) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value > 90.564 AND d.value < 300 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > -32.5 AND d.value < 50, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value <= -32.5 || value >= 50) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value > -32.5 AND d.value < 50 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                     Range (>=, <)
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= '0' AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= true AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= false AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= null AND d.seq < 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 7 AND d.seq < 18, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key < 7 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= 7 AND d.seq < 18 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq > 7.1 AND d.seq <= 17.9, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 7.1 AND d.seq <= 17.9 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq >= 18 AND d.seq < 7 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq >= 18 AND d.seq < 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 7 AND d.seq < 7.0 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 7 AND d.seq < 7.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 0 AND d.seq < 31 , TFIDF() ASC, BM25() ASC, d.name DESC
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*, StringComparer> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key >= 31) {
        continue;
      }
      expectedDocs.emplace(arangodb::iresearch::getStringRef(
                               docSlice.get("name")),
                           &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 0 AND d.seq < 31 SORT tfidf(d), "
        "BM25(d), d.name DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= 90.564 AND d.value < 300, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value < 90.564 || value >= 300) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value >= 90.564 AND d.value < 300 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= -32.5 AND d.value < 50, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value < -32.5 || value >= 50) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value >= -32.5 AND d.value < 50 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                     Range (>, <=)
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > '0' AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > true AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > false AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > null AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 7 AND d.seq <= 18, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7 || key > 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 7 AND d.seq <= 18 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq > 7 AND d.seq <= 17.9, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > 7.1 AND d.seq <= 17.9 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq > 18 AND d.seq <= 7 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq > 18 AND d.seq <= 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 7 AND d.seq <= 7.0 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > 7 AND d.seq <= 7.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 0 AND d.seq <= 31 , TFIDF() ASC, BM25() ASC, d.name DESC
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*, StringComparer> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 0 || key > 31) {
        continue;
      }
      expectedDocs.emplace(arangodb::iresearch::getStringRef(
                               docSlice.get("name")),
                           &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq > 0 AND d.seq <= 31 SORT tfidf(d), "
        "BM25(d), d.name DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > 90.564 AND d.value <= 300, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value <= 90.564 || value > 300) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value > 90.564 AND d.value <= 300 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value > -32.5 AND d.value <= 50, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value <= -32.5 || value > 50) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value > -32.5 AND d.value <= 50 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                    Range (>=, <=)
  // -----------------------------------------------------------------------------

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= '0' AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= true AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= false AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // invalid type
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= null AND d.seq <= 15 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 7 AND d.seq <= 18, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key < 7 || key > 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 7 AND d.seq <= 18 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq >= 7.1 AND d.seq <= 17.9, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 7 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 7.1 AND d.seq <= 17.9 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq >= 18 AND d.seq <= 7 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 18 AND d.seq <= 7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 7.0 AND d.seq <= 7.0 , unordered
  // will be optimized to d.seq == 7.0
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 7.0 AND d.seq <= 7.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(1 == resultIt.size());

    auto const resolved = resultIt.value().resolveExternals();
    EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                         arangodb::velocypack::Slice(insertedDocs[7].vpack()), resolved, true));

    resultIt.next();
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq > 7 AND d.seq <= 7.0 , unordered
  // behavior same as above
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 7 AND d.seq <= 7.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(1 == resultIt.size());

    auto const resolved = resultIt.value().resolveExternals();
    EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                         arangodb::velocypack::Slice(insertedDocs[7].vpack()), resolved, true));

    resultIt.next();
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 0 AND d.seq <= 31 , TFIDF() ASC, BM25() ASC, d.name DESC
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*, StringComparer> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key > 31) {
        continue;
      }
      expectedDocs.emplace(arangodb::iresearch::getStringRef(
                               docSlice.get("name")),
                           &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq >= 0 AND d.seq <= 31 SORT tfidf(d), "
        "BM25(d), d.name DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= 90.564 AND d.value <= 300, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value < 90.564 || value > 300) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value >= 90.564 AND d.value <= 300 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= -32.5 AND d.value <= 50, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value < -32.5 || value > 50) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value >= -32.5 AND d.value <= 50 SORT "
        "BM25(d), TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // -----------------------------------------------------------------------------
  // --SECTION--                                                    Range (>=, <=)
  // -----------------------------------------------------------------------------

  // d.seq >= 7 AND d.seq <= 18, unordered
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key < 7 || key > 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq IN 7..18 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq >= 7.1 AND d.seq <= 17.9, unordered
  // (will be converted to d.seq >= 7 AND d.seq <= 17)
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key <= 6 || key >= 18) {
        continue;
      }
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq IN 7.1..17.9 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      auto const keySlice = resolved.get("seq");
      auto const key = keySlice.getNumber<size_t>();

      auto expectedDoc = expectedDocs.find(key);
      ASSERT_TRUE(expectedDoc != expectedDocs.end());
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      expectedDocs.erase(expectedDoc);
    }
    EXPECT_TRUE(expectedDocs.empty());
  }

  // d.seq >= 18 AND d.seq <= 7 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq IN 18..7 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(0 == resultIt.size());
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 7 AND d.seq <= 7.0 , unordered
  {
    auto queryResult = arangodb::tests::executeQuery(
        vocbase, "FOR d IN testView SEARCH d.seq IN 7..7.0 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(1 == resultIt.size());

    auto const resolved = resultIt.value().resolveExternals();
    EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                         arangodb::velocypack::Slice(insertedDocs[7].vpack()), resolved, true));

    resultIt.next();
    EXPECT_TRUE(!resultIt.valid());
  }

  // d.seq >= 0 AND d.seq <= 31 , TFIDF() ASC, BM25() ASC, d.name DESC
  {
    std::map<irs::string_ref, arangodb::ManagedDocumentResult const*, StringComparer> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      if (key > 31) {
        continue;
      }
      expectedDocs.emplace(arangodb::iresearch::getStringRef(
                               docSlice.get("name")),
                           &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.seq IN 0..31 SORT tfidf(d), BM25(d), "
        "d.name DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= 90.564 AND d.value <= 300, BM25() ASC, TFIDF() ASC seq DESC
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value < 90.564 || value > 300) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value IN 90.564..300 SORT BM25(d), "
        "TFIDF(d), d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.value >= -32.5 AND d.value <= 50, BM25() ASC, TFIDF() ASC seq DESC
  // (will be converted to d.value >= -32 AND d.value <= 50)
  {
    std::map<size_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const valueSlice = docSlice.get("value");
      if (valueSlice.isNone()) {
        continue;
      }
      auto const value = valueSlice.getNumber<double>();
      if (value < -32 || value > 50) {
        continue;
      }
      auto const keySlice = docSlice.get("seq");
      auto const key = keySlice.getNumber<size_t>();
      expectedDocs.emplace(key, &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.value IN -32.5..50 SORT BM25(d), TFIDF(d), "
        "d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE(0 == arangodb::basics::VelocyPackHelper::compare(
                           arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                           resolved, true));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }
}
