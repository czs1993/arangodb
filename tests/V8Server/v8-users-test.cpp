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

#include "../IResearch/common.h"
#include "../Mocks/StorageEngineMock.h"
#include "Aql/QueryRegistry.h"
#include "Basics/StaticStrings.h"
#include "gtest/gtest.h"

#if USE_ENTERPRISE
#include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "GeneralServer/AuthenticationFeature.h"
#include "Replication/ReplicationFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/SystemDatabaseFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "Sharding/ShardingFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "Utils/ExecContext.h"
#include "V8/v8-vpack.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-users.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/vocbase.h"
#include "velocypack/Builder.h"

// The following v8 headers must be included late, or MSVC fails to compile
// (error in mswsockdef.h), because V8's win32-headers.h #undef some macros like
// CONST and VOID. If, e.g., "Cluster/ClusterInfo.h" (which is in turn included
// here by "Sharding/ShardingFeature.h") is included after these, compilation
// fails. Another option than to include the following headers late is to
// include ClusterInfo.h before them.
// I have not dug into which header included by ClusterInfo.h will finally
// include mwsockdef.h. Nor did I check whether all of the following headers
// will include V8's "src/base/win32-headers.h".
#include "src/api.h"
#include "src/objects-inl.h"
#include "src/objects/scope-info.h"

namespace {

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
 public:
  virtual void* Allocate(size_t length) override {
    void* data = AllocateUninitialized(length);
    return data == nullptr ? data : memset(data, 0, length);
  }
  virtual void* AllocateUninitialized(size_t length) override {
    return malloc(length);
  }
  virtual void Free(void* data, size_t) override { free(data); }
};

struct TestView : public arangodb::LogicalView {
  arangodb::Result _appendVelocyPackResult;
  arangodb::velocypack::Builder _properties;

  TestView(TRI_vocbase_t& vocbase, arangodb::velocypack::Slice const& definition, uint64_t planVersion)
      : arangodb::LogicalView(vocbase, definition, planVersion) {}
  virtual arangodb::Result appendVelocyPackImpl(
      arangodb::velocypack::Builder& builder,
      std::underlying_type<arangodb::LogicalDataSource::Serialize>::type) const override {
    builder.add("properties", _properties.slice());
    return _appendVelocyPackResult;
  }
  virtual arangodb::Result dropImpl() override { return arangodb::Result(); }
  virtual void open() override {}
  virtual arangodb::Result renameImpl(std::string const&) override {
    return arangodb::Result();
  }
  virtual arangodb::Result properties(arangodb::velocypack::Slice const& properties,
                                      bool partialUpdate) override {
    _properties = arangodb::velocypack::Builder(properties);
    return arangodb::Result();
  }
  virtual bool visitCollections(CollectionVisitor const& visitor) const override {
    return true;
  }
};

struct ViewFactory : public arangodb::ViewFactory {
  virtual arangodb::Result create(arangodb::LogicalView::ptr& view, TRI_vocbase_t& vocbase,
                                  arangodb::velocypack::Slice const& definition) const override {
    view = vocbase.createView(definition);

    return arangodb::Result();
  }

  virtual arangodb::Result instantiate(arangodb::LogicalView::ptr& view,
                                       TRI_vocbase_t& vocbase,
                                       arangodb::velocypack::Slice const& definition,
                                       uint64_t planVersion) const override {
    view = std::make_shared<TestView>(vocbase, definition, planVersion);

    return arangodb::Result();
  }
};

}  // namespace

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class V8UsersTest : public ::testing::Test {
 protected:
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;
  ViewFactory viewFactory;

  V8UsersTest() : engine(server), server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::v8Init();  // on-time initialize V8

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    // suppress WARNING {authentication} --server.jwt-secret is insecure. Use --server.jwt-secret-keyfile instead
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(),
                                    arangodb::LogLevel::ERR);

    features.emplace_back(new arangodb::AuthenticationFeature(server), false);  // required for VocbaseContext
    features.emplace_back(new arangodb::DatabaseFeature(server),
                          false);  // required for UserManager::updateUser(...)
    features.emplace_back(new arangodb::QueryRegistryFeature(server), false);  // required for TRI_vocbase_t
    arangodb::application_features::ApplicationServer::server->addFeature(
        features.back().first);  // need QueryRegistryFeature feature to be added now in order to create the system database
    system = std::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL,
                                             0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::ReplicationFeature(server), false);  // required for DatabaseFeature::createDatabase(...)
    features.emplace_back(new arangodb::ShardingFeature(server),
                          false);  // required for LogicalCollection::LogicalCollection(...)
    features.emplace_back(new arangodb::SystemDatabaseFeature(server, system.get()),
                          false);  // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::ViewTypesFeature(server),
                          false);  // required for LogicalView::create(...)

#if USE_ENTERPRISE
    features.emplace_back(new arangodb::LdapFeature(server),
                          false);  // required for AuthenticationFeature with USE_ENTERPRISE
#endif

    arangodb::application_features::ApplicationServer::server->addFeature(
        new arangodb::V8DealerFeature(server));  // add without calling prepare(), required for DatabaseFeature::createDatabase(...)

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

    auto* viewTypesFeature =
        arangodb::application_features::ApplicationServer::lookupFeature<arangodb::ViewTypesFeature>();

    viewTypesFeature->emplace(arangodb::LogicalDataSource::Type::emplace(arangodb::velocypack::StringRef(
                                  "testViewType")),
                              viewFactory);
  }

  ~V8UsersTest() {
    system.reset();  // destroy before reseting the 'ENGINE'
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

    arangodb::EngineSelectorFeature::ENGINE =
        nullptr;  // nullify only after DatabaseFeature::unprepare()
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(),
                                    arangodb::LogLevel::DEFAULT);
  }
};

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_F(V8UsersTest, test_collection_auth) {
  auto usersJson = arangodb::velocypack::Parser::fromJson(
      "{ \"name\": \"_users\", \"isSystem\": true }");
  static const std::string userName("testUser");
  auto* databaseFeature =
      arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabaseFeature>(
          "Database");
  TRI_vocbase_t* vocbase;  // will be owned by DatabaseFeature
  ASSERT_TRUE(databaseFeature->createDatabase(1, "testDatabase", vocbase).ok());
  v8::Isolate::CreateParams isolateParams;
  ArrayBufferAllocator arrayBufferAllocator;
  isolateParams.array_buffer_allocator = &arrayBufferAllocator;
  auto isolate =
      std::shared_ptr<v8::Isolate>(v8::Isolate::New(isolateParams),
                                   [](v8::Isolate* p) -> void { p->Dispose(); });
  ASSERT_TRUE((nullptr != isolate));
  v8::Isolate::Scope isolateScope(isolate.get());  // otherwise v8::Isolate::Logger() will fail (called from v8::Exception::Error)
  v8::internal::Isolate::Current()->InitializeLoggingAndCounters();  // otherwise v8::Isolate::Logger() will fail (called from v8::Exception::Error)
  v8::HandleScope handleScope(isolate.get());  // required for v8::Context::New(...), v8::ObjectTemplate::New(...) and TRI_AddMethodVocbase(...)
  auto context = v8::Context::New(isolate.get());
  v8::Context::Scope contextScope(context);  // required for TRI_AddMethodVocbase(...)
  std::unique_ptr<TRI_v8_global_t> v8g(TRI_CreateV8Globals(isolate.get(), 0));  // create and set inside 'isolate' for use with 'TRI_GET_GLOBALS()'
  v8g->ArangoErrorTempl.Reset(isolate.get(), v8::ObjectTemplate::New(isolate.get()));  // otherwise v8:-utils::CreateErrorObject(...) will fail
  v8g->_vocbase = vocbase;
  TRI_InitV8Users(context, vocbase, v8g.get(), isolate.get());

  auto arangoUsers =
      v8::Local<v8::ObjectTemplate>::New(isolate.get(), v8g->UsersTempl)->NewInstance();
  auto fn_grantCollection =
      arangoUsers->Get(TRI_V8_ASCII_STRING(isolate.get(), "grantCollection"));
  EXPECT_TRUE((fn_grantCollection->IsFunction()));
  auto fn_revokeCollection =
      arangoUsers->Get(TRI_V8_ASCII_STRING(isolate.get(), "revokeCollection"));
  EXPECT_TRUE((fn_revokeCollection->IsFunction()));
  std::vector<v8::Local<v8::Value>> grantArgs = {
      TRI_V8_STD_STRING(isolate.get(), userName),
      TRI_V8_STD_STRING(isolate.get(), vocbase->name()),
      TRI_V8_ASCII_STRING(isolate.get(), "testDataSource"),
      TRI_V8_STD_STRING(isolate.get(),
                        arangodb::auth::convertFromAuthLevel(arangodb::auth::Level::RW)),
  };
  std::vector<v8::Local<v8::Value>> grantWildcardArgs = {
      TRI_V8_STD_STRING(isolate.get(), userName),
      TRI_V8_STD_STRING(isolate.get(), vocbase->name()),
      TRI_V8_ASCII_STRING(isolate.get(), "*"),
      TRI_V8_STD_STRING(isolate.get(),
                        arangodb::auth::convertFromAuthLevel(arangodb::auth::Level::RW)),
  };
  std::vector<v8::Local<v8::Value>> revokeArgs = {
      TRI_V8_STD_STRING(isolate.get(), userName),
      TRI_V8_STD_STRING(isolate.get(), vocbase->name()),
      TRI_V8_ASCII_STRING(isolate.get(), "testDataSource"),
  };
  std::vector<v8::Local<v8::Value>> revokeWildcardArgs = {
      TRI_V8_STD_STRING(isolate.get(), userName),
      TRI_V8_STD_STRING(isolate.get(), vocbase->name()),
      TRI_V8_ASCII_STRING(isolate.get(), "*"),
  };

  struct ExecContext : public arangodb::ExecContext {
    ExecContext()
        : arangodb::ExecContext(arangodb::ExecContext::Type::Default, userName,
                                "", arangodb::auth::Level::RW,
                                arangodb::auth::Level::NONE) {
    }  // ExecContext::isAdminUser() == true
  } execContext;
  arangodb::ExecContextScope execContextScope(&execContext);
  auto* authFeature = arangodb::AuthenticationFeature::instance();
  auto* userManager = authFeature->userManager();
  arangodb::aql::QueryRegistry queryRegistry(0);  // required for UserManager::loadFromDB()
  userManager->setGlobalVersion(0);  // required for UserManager::loadFromDB()
  userManager->setQueryRegistry(&queryRegistry);

  // test auth missing (grant)
  {
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));

    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_grantCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(grantArgs.size()),
                                       grantArgs.data());
    EXPECT_TRUE((result.IsEmpty()));
    EXPECT_TRUE((tryCatch.HasCaught()));
    EXPECT_TRUE((TRI_ERROR_NO_ERROR == TRI_V8ToVPack(isolate.get(), responce,
                                                     tryCatch.Exception(), false)));
    auto slice = responce.slice();
    EXPECT_TRUE((slice.isObject()));
    EXPECT_TRUE((slice.hasKey(arangodb::StaticStrings::ErrorNum) &&
                 slice.get(arangodb::StaticStrings::ErrorNum).isNumber<int>() &&
                 TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND ==
                     slice.get(arangodb::StaticStrings::ErrorNum).getNumber<int>()));
    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
  }

  // test auth missing (revoke)
  {
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    userPtr->grantCollection(vocbase->name(),
                             "testDataSource", arangodb::auth::Level::RO);  // for missing collections User::collectionAuthLevel(...) returns database auth::Level

    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_revokeCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(revokeArgs.size()),
                                       revokeArgs.data());
    EXPECT_TRUE((result.IsEmpty()));
    EXPECT_TRUE((tryCatch.HasCaught()));
    EXPECT_TRUE((TRI_ERROR_NO_ERROR == TRI_V8ToVPack(isolate.get(), responce,
                                                     tryCatch.Exception(), false)));
    auto slice = responce.slice();
    EXPECT_TRUE((slice.isObject()));
    EXPECT_TRUE((slice.hasKey(arangodb::StaticStrings::ErrorNum) &&
                 slice.get(arangodb::StaticStrings::ErrorNum).isNumber<int>() &&
                 TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND ==
                     slice.get(arangodb::StaticStrings::ErrorNum).getNumber<int>()));
    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));  // not modified from above
  }

  // test auth collection (grant)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testDataSource\" }");
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    auto logicalCollection = std::shared_ptr<arangodb::LogicalCollection>(
        vocbase->createCollection(collectionJson->slice()).get(),
        [vocbase](arangodb::LogicalCollection* ptr) -> void {
          vocbase->dropCollection(ptr->id(), false, 0);
        });
    ASSERT_TRUE((false == !logicalCollection));

    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_grantCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(grantArgs.size()),
                                       grantArgs.data());
    EXPECT_TRUE((!result.IsEmpty()));
    EXPECT_TRUE((result.ToLocalChecked()->IsUndefined()));
    EXPECT_TRUE((!tryCatch.HasCaught()));
    EXPECT_TRUE(
        (arangodb::auth::Level::RW ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
  }

  // test auth collection (revoke)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testDataSource\" }");
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    userPtr->grantCollection(vocbase->name(),
                             "testDataSource", arangodb::auth::Level::RO);  // for missing collections User::collectionAuthLevel(...) returns database auth::Level
    auto logicalCollection = std::shared_ptr<arangodb::LogicalCollection>(
        vocbase->createCollection(collectionJson->slice()).get(),
        [vocbase](arangodb::LogicalCollection* ptr) -> void {
          vocbase->dropCollection(ptr->id(), false, 0);
        });
    ASSERT_TRUE((false == !logicalCollection));

    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_revokeCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(revokeArgs.size()),
                                       revokeArgs.data());
    EXPECT_TRUE((!result.IsEmpty()));
    EXPECT_TRUE((result.ToLocalChecked()->IsUndefined()));
    EXPECT_TRUE((!tryCatch.HasCaught()));
    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
  }

  // test auth view (grant)
  {
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testDataSource\", \"type\": \"testViewType\" }");
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    auto logicalView = std::shared_ptr<arangodb::LogicalView>(
        vocbase->createView(viewJson->slice()).get(),
        [vocbase](arangodb::LogicalView* ptr) -> void {
          vocbase->dropView(ptr->id(), false);
        });
    ASSERT_TRUE((false == !logicalView));

    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_grantCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(grantArgs.size()),
                                       grantArgs.data());
    EXPECT_TRUE((result.IsEmpty()));
    EXPECT_TRUE((tryCatch.HasCaught()));
    EXPECT_TRUE((TRI_ERROR_NO_ERROR == TRI_V8ToVPack(isolate.get(), responce,
                                                     tryCatch.Exception(), false)));
    auto slice = responce.slice();
    EXPECT_TRUE((slice.isObject()));
    EXPECT_TRUE((slice.hasKey(arangodb::StaticStrings::ErrorNum) &&
                 slice.get(arangodb::StaticStrings::ErrorNum).isNumber<int>() &&
                 TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND ==
                     slice.get(arangodb::StaticStrings::ErrorNum).getNumber<int>()));
    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
  }

  // test auth view (revoke)
  {
    auto viewJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testDataSource\", \"type\": \"testViewType\" }");
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    userPtr->grantCollection(vocbase->name(),
                             "testDataSource", arangodb::auth::Level::RO);  // for missing collections User::collectionAuthLevel(...) returns database auth::Level
    auto logicalView = std::shared_ptr<arangodb::LogicalView>(
        vocbase->createView(viewJson->slice()).get(),
        [vocbase](arangodb::LogicalView* ptr) -> void {
          vocbase->dropView(ptr->id(), false);
        });
    ASSERT_TRUE((false == !logicalView));

    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_revokeCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(revokeArgs.size()),
                                       revokeArgs.data());
    EXPECT_TRUE((result.IsEmpty()));
    EXPECT_TRUE((tryCatch.HasCaught()));
    EXPECT_TRUE((TRI_ERROR_NO_ERROR == TRI_V8ToVPack(isolate.get(), responce,
                                                     tryCatch.Exception(), false)));
    auto slice = responce.slice();
    EXPECT_TRUE((slice.isObject()));
    EXPECT_TRUE((slice.hasKey(arangodb::StaticStrings::ErrorNum) &&
                 slice.get(arangodb::StaticStrings::ErrorNum).isNumber<int>() &&
                 TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND ==
                     slice.get(arangodb::StaticStrings::ErrorNum).getNumber<int>()));
    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));  // not modified from above
  }

  // test auth wildcard (grant)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testDataSource\" }");
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    auto logicalCollection = std::shared_ptr<arangodb::LogicalCollection>(
        vocbase->createCollection(collectionJson->slice()).get(),
        [vocbase](arangodb::LogicalCollection* ptr) -> void {
          vocbase->dropCollection(ptr->id(), false, 0);
        });
    ASSERT_TRUE((false == !logicalCollection));

    EXPECT_TRUE(
        (arangodb::auth::Level::NONE ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_grantCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(grantWildcardArgs.size()),
                                       grantWildcardArgs.data());
    EXPECT_TRUE((!result.IsEmpty()));
    EXPECT_TRUE((result.ToLocalChecked()->IsUndefined()));
    EXPECT_TRUE((!tryCatch.HasCaught()));
    EXPECT_TRUE(
        (arangodb::auth::Level::RW ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
  }

  // test auth wildcard (revoke)
  {
    auto collectionJson = arangodb::velocypack::Parser::fromJson(
        "{ \"name\": \"testDataSource\" }");
    auto scopedUsers = std::shared_ptr<arangodb::LogicalCollection>(
        system->createCollection(usersJson->slice()).get(),
        [this](arangodb::LogicalCollection* ptr) -> void {
          system->dropCollection(ptr->id(), true, 0.0);
        });
    arangodb::auth::UserMap userMap;
    arangodb::auth::User* userPtr = nullptr;
    userManager->setAuthInfo(userMap);  // insure an empty map is set before UserManager::storeUser(...)
    userManager->storeUser(false, userName, arangodb::StaticStrings::Empty,
                           true, arangodb::velocypack::Slice());
    userManager->accessUser(userName, [&userPtr](arangodb::auth::User const& user) -> arangodb::Result {
      userPtr = const_cast<arangodb::auth::User*>(&user);
      return arangodb::Result();
    });
    ASSERT_TRUE((nullptr != userPtr));
    userPtr->grantCollection(vocbase->name(),
                             "testDataSource", arangodb::auth::Level::RO);  // for missing collections User::collectionAuthLevel(...) returns database auth::Level
    auto logicalCollection = std::shared_ptr<arangodb::LogicalCollection>(
        vocbase->createCollection(collectionJson->slice()).get(),
        [vocbase](arangodb::LogicalCollection* ptr) -> void {
          vocbase->dropCollection(ptr->id(), false, 0);
        });
    ASSERT_TRUE((false == !logicalCollection));

    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));
    arangodb::velocypack::Builder responce;
    v8::TryCatch tryCatch(isolate.get());
    auto result = v8::Function::Cast(*fn_revokeCollection)
                      ->CallAsFunction(context, arangoUsers,
                                       static_cast<int>(revokeWildcardArgs.size()),
                                       revokeWildcardArgs.data());
    EXPECT_TRUE((!result.IsEmpty()));
    EXPECT_TRUE((result.ToLocalChecked()->IsUndefined()));
    EXPECT_TRUE((!tryCatch.HasCaught()));
    EXPECT_TRUE(
        (arangodb::auth::Level::RO ==
         execContext.collectionAuthLevel(vocbase->name(), "testDataSource")));  // unchanged since revocation is only for exactly matching collection names
  }
}
