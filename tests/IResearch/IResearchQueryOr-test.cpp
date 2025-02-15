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
#include "Cluster/ClusterFeature.h"
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
#include "V8Server/V8DealerFeature.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"
#include "VocBase/Methods/Collections.h"

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

class IResearchQueryOrTest : public ::testing::Test {
 protected:
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;

  IResearchQueryOrTest() : engine(server), server(nullptr, nullptr) {
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
    features.emplace_back(new arangodb::V8DealerFeature(server),
                          false);  // required for DatabaseFeature::createDatabase(...)
    features.emplace_back(new arangodb::ViewTypesFeature(server), true);
    features.emplace_back(new arangodb::AuthenticationFeature(server), true);
    features.emplace_back(new arangodb::DatabasePathFeature(server), false);
    features.emplace_back(new arangodb::DatabaseFeature(server), false);
    features.emplace_back(new arangodb::ShardingFeature(server), false);
    features.emplace_back(new arangodb::QueryRegistryFeature(server), false);  // must be first
    arangodb::application_features::ApplicationServer::server->addFeature(
        features.back().first);  // need QueryRegistryFeature feature to be added now in order to create the system database
    features.emplace_back(new arangodb::SystemDatabaseFeature(server), true);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(server), false);  // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(server), true);
    features.emplace_back(new arangodb::aql::OptimizerRulesFeature(server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(server), true);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(server), true);

#if USE_ENTERPRISE
    features.emplace_back(new arangodb::LdapFeature(server),
                          false);  // required for AuthenticationFeature with USE_ENTERPRISE
#endif

    // required for V8DealerFeature::prepare(), ClusterFeature::prepare() not required
    arangodb::application_features::ApplicationServer::server->addFeature(
        new arangodb::ClusterFeature(server));

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    auto const databases = VPackParser::fromJson(
        std::string("[ { \"name\": \"") +
        arangodb::StaticStrings::SystemDatabase + "\" } ]");
    auto* dbFeature =
        arangodb::application_features::ApplicationServer::lookupFeature<arangodb::DatabaseFeature>(
            "Database");
    dbFeature->loadDatabases(databases->slice());

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    auto* analyzers =
        arangodb::application_features::ApplicationServer::lookupFeature<arangodb::iresearch::IResearchAnalyzerFeature>();
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    TRI_vocbase_t* vocbase;

    dbFeature->createDatabase(1, "testVocbase", vocbase);  // required for IResearchAnalyzerFeature::emplace(...)
    arangodb::methods::Collections::createSystem(
        *vocbase,
        arangodb::tests::AnalyzerCollectionName, false);
    analyzers->emplace(result, "testVocbase::test_analyzer", "TestAnalyzer",
                       VPackParser::fromJson("\"abc\"")->slice(),
                       irs::flags{irs::frequency::type(), irs::position::type()}  // required for PHRASE
    );  // cache analyzer

    analyzers->emplace(result, "testVocbase::test_csv_analyzer",
                       "TestDelimAnalyzer",
                       VPackParser::fromJson("\",\"")->slice());  // cache analyzer

    auto* dbPathFeature =
        arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabasePathFeature>(
            "DatabasePath");
    arangodb::tests::setDatabasePath(*dbPathFeature);  // ensure test data is stored in a unique directory
  }

  ~IResearchQueryOrTest() {
    arangodb::AqlFeature(server).stop();  // unset singleton instance
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(),
                                    arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AQL.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;

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
    arangodb::EngineSelectorFeature::ENGINE = nullptr;
  }
};  // IResearchQuerySetup

}  // namespace

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(IResearchQueryOrTest, test) {
  static std::vector<std::string> const EMPTY;

  auto createJson = VPackParser::fromJson(
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
    auto collectionJson = VPackParser::fromJson(
        "{ \"name\": \"collection_1\" }");
    logicalCollection1 = vocbase.createCollection(collectionJson->slice());
    ASSERT_TRUE((nullptr != logicalCollection1));
  }

  // add collection_2
  {
    auto collectionJson = VPackParser::fromJson(
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
    auto updateJson = VPackParser::fromJson(
        "{ \"links\": {"
        "\"collection_1\": { \"analyzers\": [ \"test_analyzer\", \"identity\" "
        "], \"includeAllFields\": true, \"trackListPositions\": true, "
        "\"storeValues\":\"id\" },"
        "\"collection_2\": { \"analyzers\": [ \"test_analyzer\", \"identity\" "
        "], \"includeAllFields\": true, \"storeValues\":\"id\" }"
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

  // d.name == 'A' OR d.name == 'Q', d.seq DESC
  {
    std::map<ptrdiff_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      if (keySlice.isNone()) {
        continue;
      }
      auto const key = arangodb::iresearch::getStringRef(keySlice);
      if (key != "A" && key != "Q") {
        continue;
      }
      expectedDocs.emplace(docSlice.get("seq").getNumber<ptrdiff_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'A' OR d.name == 'Q' SORT d.seq "
        "DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_EQUAL_SLICES(
          arangodb::velocypack::Slice(expectedDoc->second->vpack()),
          resolved);
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.name == 'X' OR d.same == 'xyz', BM25(d) DESC, TFIDF(d) DESC, d.seq DESC
  {
    std::map<ptrdiff_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      expectedDocs.emplace(docSlice.get("seq").getNumber<ptrdiff_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'X' OR d.same == 'xyz' SORT "
        "BM25(d) DESC, TFIDF(d) DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    // Check 1st (the most relevant doc)
    // {"name":"X","seq":23,"same":"xyz", "duplicated":"vczc", "prefix":"bateradsfsfasdf" }
    {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(expectedDocs[23]->vpack()),
                            resolved, true)));
      expectedDocs.erase(23);
    }

    // Check the rest of documents
    auto expectedDoc = expectedDocs.rbegin();
    for (resultIt.next(); resultIt.valid(); resultIt.next()) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                            resolved, true)));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.name == 'K' OR d.value <= 100 OR d.duplicated == abcd, TFIDF(d) DESC, d.seq DESC
  {
    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[10].vpack()),  // {"name":"K","seq":10,"same":"xyz","value":12,"duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[30].vpack()),  // {"name":"$","seq":30,"same":"xyz","duplicated":"abcd","prefix":"abcy" }
        arangodb::velocypack::Slice(insertedDocs[26].vpack()),  // {"name":"~","seq":26,"same":"xyz", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[20].vpack()),  // {"name":"U","seq":20,"same":"xyz", "prefix":"abc", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[4].vpack()),  // {"name":"E","seq":4,"same":"xyz","value":100,"duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),  // {"name":"A","seq":0,"same":"xyz","value":100,"duplicated":"abcd","prefix":"abcd" }
        arangodb::velocypack::Slice(insertedDocs[16].vpack()),  // {"name":"Q","seq":16,"same":"xyz", "value":-32.5, "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[15].vpack()),  // {"name":"P","seq":15,"same":"xyz","value":50,"prefix":"abde"}
        arangodb::velocypack::Slice(insertedDocs[14].vpack()),  // {"name":"O","seq":14,"same":"xyz","value":0 }
        arangodb::velocypack::Slice(insertedDocs[13].vpack()),  // {"name":"N","seq":13,"same":"xyz","value":1,"duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[12].vpack()),  // {"name":"M","seq":12,"same":"xyz","value":90.564 }
        arangodb::velocypack::Slice(insertedDocs[11].vpack()),  // {"name":"L","seq":11,"same":"xyz","value":95 }
        arangodb::velocypack::Slice(insertedDocs[9].vpack()),  // {"name":"J","seq":9,"same":"xyz","value":100 }
        arangodb::velocypack::Slice(insertedDocs[8].vpack()),  // {"name":"I","seq":8,"same":"xyz","value":100,"prefix":"bcd" }
        arangodb::velocypack::Slice(insertedDocs[6].vpack()),  // {"name":"G","seq":6,"same":"xyz","value":100 }
        arangodb::velocypack::Slice(insertedDocs[3].vpack()),  // {"name":"D","seq":3,"same":"xyz","value":12,"prefix":"abcde"}
    };

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'K' OR d.value <= 100 OR "
        "d.duplicated == 'abcd' SORT TFIDF(d) DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    // Check the documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();
      resultIt.next();
      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(*expectedDoc, resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // d.name == 'A' OR d.name == 'Q' OR d.same != 'xyz', d.seq DESC
  {
    std::map<ptrdiff_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      if (keySlice.isNone()) {
        continue;
      }
      auto const key = arangodb::iresearch::getStringRef(keySlice);
      if (key != "A" && key != "Q") {
        continue;
      }
      expectedDocs.emplace(docSlice.get("seq").getNumber<ptrdiff_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'A' OR d.name == 'Q' OR d.same != "
        "'xyz' SORT d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    auto expectedDoc = expectedDocs.rbegin();
    for (auto const actualDoc : resultIt) {
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_EQUAL_SLICES(arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                                        resolved);
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.name == 'F' OR EXISTS(d.duplicated), BM25(d) DESC, d.seq DESC
  {
    std::map<ptrdiff_t, arangodb::ManagedDocumentResult const*> expectedDocs;
    for (auto const& doc : insertedDocs) {
      arangodb::velocypack::Slice docSlice(doc.vpack());
      auto const keySlice = docSlice.get("name");
      if (keySlice.isNone()) {
        continue;
      }
      auto const key = arangodb::iresearch::getStringRef(keySlice);
      if (key != "F" && docSlice.get("duplicated").isNone()) {
        continue;
      }
      expectedDocs.emplace(docSlice.get("seq").getNumber<ptrdiff_t>(), &doc);
    }

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'F' OR EXISTS(d.duplicated) SORT "
        "BM25(d) DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    // Check 1st (the most relevant doc)
    // {"name":"F","seq":5,"same":"xyz", "value":1234 }
    {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(expectedDocs[5]->vpack()),
                            resolved, true)));
      expectedDocs.erase(5);
    }

    // Check the rest of documents
    auto expectedDoc = expectedDocs.rbegin();
    for (resultIt.next(); resultIt.valid(); resultIt.next()) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();
      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(expectedDoc->second->vpack()),
                            resolved, true)));
      ++expectedDoc;
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.rend());
  }

  // d.name == 'D' OR STARTS_WITH(d.prefix, 'abc'), TFIDF(d) DESC, d.seq DESC
  {
    std::vector<arangodb::velocypack::Slice> expectedDocs{
        // The most relevant document (satisfied both search conditions)
        arangodb::velocypack::Slice(insertedDocs[3].vpack()),  // {"name":"D","seq":3,"same":"xyz", "value":12, "prefix":"abcde"}

        // Less relevant documents (satisfied STARTS_WITH condition only, has unqiue term in 'prefix' field)
        arangodb::velocypack::Slice(insertedDocs[25].vpack()),  // {"name":"Z","seq":25,"same":"xyz", "prefix":"abcdrer" }
        arangodb::velocypack::Slice(insertedDocs[20].vpack()),  // {"name":"U","seq":20,"same":"xyz", "prefix":"abc", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),  // {"name":"A","seq":0,"same":"xyz", "value":100, "duplicated":"abcd", "prefix":"abcd" }

        // The least relevant documents (contain non-unique term 'abcy' in 'prefix' field)
        arangodb::velocypack::Slice(insertedDocs[31].vpack()),  // {"name":"%","seq":31,"same":"xyz", "prefix":"abcy"}
        arangodb::velocypack::Slice(insertedDocs[30].vpack()),  // {"name":"$","seq":30,"same":"xyz", "duplicated":"abcd", "prefix":"abcy" }
    };

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'D' OR STARTS_WITH(d.prefix, "
        "'abc') SORT TFIDF(d) DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // d.name == 'D' OR STARTS_WITH(d.prefix, 'abc'), BM25(d) DESC, d.seq DESC
  {
    std::vector<arangodb::velocypack::Slice> expectedDocs{
        // The most relevant document (satisfied both search conditions)
        arangodb::velocypack::Slice(insertedDocs[3].vpack()),  // {"name":"D","seq":3,"same":"xyz", "value":12, "prefix":"abcde"}

        // Less relevant documents (satisfied STARTS_WITH condition only, has unqiue term in 'prefix' field)
        arangodb::velocypack::Slice(insertedDocs[25].vpack()),  // {"name":"Z","seq":25,"same":"xyz", "prefix":"abcdrer" }
        arangodb::velocypack::Slice(insertedDocs[20].vpack()),  // {"name":"U","seq":20,"same":"xyz", "prefix":"abc", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),  // {"name":"A","seq":0,"same":"xyz", "value":100, "duplicated":"abcd", "prefix":"abcd" }

        // The least relevant documents (contain non-unique term 'abcy' in 'prefix' field)
        arangodb::velocypack::Slice(insertedDocs[31].vpack()),  // {"name":"%","seq":31,"same":"xyz", "prefix":"abcy"}
        arangodb::velocypack::Slice(insertedDocs[30].vpack()),  // {"name":"$","seq":30,"same":"xyz", "duplicated":"abcd", "prefix":"abcy" }
    };

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'D' OR STARTS_WITH(d.prefix, "
        "'abc') SORT BM25(d) DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // d.name == 'D' OR STARTS_WITH(d.prefix, 'abc'), BM25(d) DESC, d.seq DESC, LIMIT 3
  {
    std::vector<arangodb::velocypack::Slice> expectedDocs{
        // The most relevant document (satisfied both search conditions)
        arangodb::velocypack::Slice(insertedDocs[3].vpack()),  // {"name":"D","seq":3,"same":"xyz", "value":12, "prefix":"abcde"}

        // Less relevant documents (satisfied STARTS_WITH condition only, has unqiue term in 'prefix' field)
        arangodb::velocypack::Slice(insertedDocs[25].vpack()),  // {"name":"Z","seq":25,"same":"xyz", "prefix":"abcdrer" }
        arangodb::velocypack::Slice(insertedDocs[20].vpack()),  // {"name":"U","seq":20,"same":"xyz", "prefix":"abc", "duplicated":"abcd"}
    };

    auto queryResult = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH d.name == 'D' OR STARTS_WITH(d.prefix, "
        "'abc') SORT BM25(d) DESC, d.seq DESC LIMIT 3 RETURN d");
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    EXPECT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // STARTS_WITH(d['prefix'], 'abc') OR EXISTS(d.duplicated) OR d.value < 100 OR d.name >= 'Z', BM25(d) DESC, TFIDF(d) DESC, d.seq DESC
  {
    std::vector<arangodb::velocypack::Slice> expected = {
        arangodb::velocypack::Slice(insertedDocs[25].vpack()),  // {"name":"Z","seq":25,"same":"xyz", "prefix":"abcdrer" ,
        arangodb::velocypack::Slice(insertedDocs[26].vpack()),  // {"name":"~","seq":26,"same":"xyz", "duplicated":"abcd"}

        arangodb::velocypack::Slice(insertedDocs[20].vpack()),  // {"name":"U","seq":20,"same":"xyz", "prefix":"abc", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[3].vpack()),  // {"name":"D","seq":3,"same":"xyz", "value":12, "prefix":"abcde"}
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),  // {"name":"A","seq":0,"same":"xyz", "value":100, "duplicated":"abcd", "prefix":"abcd" }
        arangodb::velocypack::Slice(insertedDocs[31].vpack()),  // {"name":"%","seq":31,"same":"xyz", "prefix":"abcy"}
        arangodb::velocypack::Slice(insertedDocs[30].vpack()),  // {"name":"$","seq":30,"same":"xyz", "duplicated":"abcd", "prefix":"abcy" }

        arangodb::velocypack::Slice(insertedDocs[23].vpack()),  // {"name":"X","seq":23,"same":"xyz", "duplicated":"vczc", "prefix":"bateradsfsfasdf" }
        arangodb::velocypack::Slice(insertedDocs[18].vpack()),  // {"name":"S","seq":18,"same":"xyz", "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[16].vpack()),  // {"name":"Q","seq":16,"same":"xyz", "value":-32.5, "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[15].vpack()),  // {"name":"P","seq":15,"same":"xyz","value":50, "prefix":"abde"},
        arangodb::velocypack::Slice(insertedDocs[14].vpack()),  // {"name":"O","seq":14,"same":"xyz","value":0 },
        arangodb::velocypack::Slice(insertedDocs[13].vpack()),  // {"name":"N","seq":13,"same":"xyz","value":1, "duplicated":"vczc"},
        arangodb::velocypack::Slice(insertedDocs[12].vpack()),  // {"name":"M","seq":12,"same":"xyz","value":90.564 },
        arangodb::velocypack::Slice(insertedDocs[11].vpack()),  // {"name":"L","seq":11,"same":"xyz","value":95 }
        arangodb::velocypack::Slice(insertedDocs[10].vpack()),  // {"name":"K","seq":10,"same":"xyz","value":12, "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[7].vpack()),  // {"name":"H","seq":7,"same":"xyz", "value":123, "duplicated":"vczc"},
        arangodb::velocypack::Slice(insertedDocs[4].vpack()),  // {"name":"E","seq":4,"same":"xyz", "value":100, "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[2].vpack()),  // {"name":"C","seq":2,"same":"xyz", "value":123, "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[1].vpack()),  // {"name":"B","seq":1,"same":"xyz", "value":101, "duplicated":"vczc"}
    };

    auto result = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH STARTS_WITH(d['prefix'], 'abc') OR "
        "EXISTS(d.duplicated) OR d.value < 100 OR d.name >= 'Z' SORT TFIDF(d) "
        "DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(result.result.ok());
    auto slice = result.data->slice();
    EXPECT_TRUE(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      EXPECT_TRUE((i < expected.size()));

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++],
                                                                    resolved, true)));
    }

    EXPECT_TRUE((i == expected.size()));
  }

  // PHRASE(d['duplicated'], 'v', 2, 'z', 'test_analyzer') OR STARTS_WITH(d['prefix'], 'abc') OR d.value > 100 OR d.name >= 'Z', BM25(d) DESC, TFIDF(d) DESC, d.seq DESC
  {
    std::vector<arangodb::velocypack::Slice> expected = {
        arangodb::velocypack::Slice(insertedDocs[25].vpack()),  // {"name":"Z","seq":25,"same":"xyz", "prefix":"abcdrer" ,
        arangodb::velocypack::Slice(insertedDocs[26].vpack()),  // {"name":"~","seq":26,"same":"xyz", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[23].vpack()),  // {"name":"X","seq":23,"same":"xyz", "duplicated":"vczc", "prefix":"bateradsfsfasdf" }
        arangodb::velocypack::Slice(insertedDocs[18].vpack()),  // {"name":"S","seq":18,"same":"xyz", "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[16].vpack()),  // {"name":"Q","seq":16,"same":"xyz", "value":-32.5, "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[13].vpack()),  // {"name":"N","seq":13,"same":"xyz","value":1, "duplicated":"vczc"},
        arangodb::velocypack::Slice(insertedDocs[7].vpack()),  // {"name":"H","seq":7,"same":"xyz", "value":123, "duplicated":"vczc"},
        arangodb::velocypack::Slice(insertedDocs[2].vpack()),  // {"name":"C","seq":2,"same":"xyz", "value":123, "duplicated":"vczc"}
        arangodb::velocypack::Slice(insertedDocs[1].vpack()),  // {"name":"B","seq":1,"same":"xyz", "value":101, "duplicated":"vczc"}

        arangodb::velocypack::Slice(insertedDocs[20].vpack()),  // {"name":"U","seq":20,"same":"xyz", "prefix":"abc", "duplicated":"abcd"}
        arangodb::velocypack::Slice(insertedDocs[3].vpack()),  // {"name":"D","seq":3,"same":"xyz", "value":12, "prefix":"abcde"}
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),  // {"name":"A","seq":0,"same":"xyz", "value":100, "duplicated":"abcd", "prefix":"abcd" }
        arangodb::velocypack::Slice(insertedDocs[31].vpack()),  // {"name":"%","seq":31,"same":"xyz", "prefix":"abcy"}
        arangodb::velocypack::Slice(insertedDocs[30].vpack()),  // {"name":"$","seq":30,"same":"xyz", "duplicated":"abcd", "prefix":"abcy" }

        arangodb::velocypack::Slice(insertedDocs[15].vpack()),  // {"name":"P","seq":15,"same":"xyz","value":50, "prefix":"abde"},
        arangodb::velocypack::Slice(insertedDocs[14].vpack()),  // {"name":"O","seq":14,"same":"xyz","value":0 },
        arangodb::velocypack::Slice(insertedDocs[12].vpack()),  // {"name":"M","seq":12,"same":"xyz","value":90.564 },
        arangodb::velocypack::Slice(insertedDocs[11].vpack()),  // {"name":"L","seq":11,"same":"xyz","value":95 }
        arangodb::velocypack::Slice(insertedDocs[10].vpack()),  // {"name":"K","seq":10,"same":"xyz","value":12, "duplicated":"abcd"}
    };

    auto result = arangodb::tests::executeQuery(
        vocbase,
        "FOR d IN testView SEARCH ANALYZER(PHRASE(d.duplicated, 'v', 1, 'z'), "
        "'test_analyzer') OR STARTS_WITH(d['prefix'], 'abc') OR d.value < 100 "
        "OR d.name >= 'Z' SORT TFIDF(d) DESC, d.seq DESC RETURN d");
    ASSERT_TRUE(result.result.ok());
    auto slice = result.data->slice();
    EXPECT_TRUE(slice.isArray());
    size_t i = 0;

    for (arangodb::velocypack::ArrayIterator itr(slice); itr.valid(); ++itr) {
      auto const resolved = itr.value().resolveExternals();
      EXPECT_TRUE((i < expected.size()));

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(expected[i++],
                                                                    resolved, true)));
    }
    EXPECT_TRUE((i == expected.size()));
  }
}
