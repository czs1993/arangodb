////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Andreas Streichardt <andreas@arangodb.com>
////////////////////////////////////////////////////////////////////////////////

#include "AuthenticationFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Auth/Common.h"
#include "Auth/Handler.h"
#include "Basics/FileUtils.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Cluster/ServerState.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "ProgramOptions/ProgramOptions.h"
#include "Random/RandomGenerator.h"
#include "RestServer/QueryRegistryFeature.h"

#if USE_ENTERPRISE
#include "Enterprise/Ldap/LdapAuthenticationHandler.h"
#include "Enterprise/Ldap/LdapFeature.h"
#endif

using namespace arangodb::options;

namespace arangodb {

AuthenticationFeature* AuthenticationFeature::INSTANCE = nullptr;

AuthenticationFeature::AuthenticationFeature(application_features::ApplicationServer& server)
    : ApplicationFeature(server, "Authentication"),
      _userManager(nullptr),
      _authCache(nullptr),
      _authenticationUnixSockets(true),
      _authenticationSystemOnly(true),
      _localAuthentication(true),
      _active(true),
      _authenticationTimeout(0.0),
      _jwtSecretProgramOption("") {
  setOptional(false);
  startsAfter("BasicsPhase");

#ifdef USE_ENTERPRISE
  startsAfter("Ldap");
#endif
}

void AuthenticationFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("server", "Server features");

  options->addOldOption("server.disable-authentication",
                        "server.authentication");
  options->addOldOption("server.disable-authentication-unix-sockets",
                        "server.authentication-unix-sockets");
  options->addOldOption("server.authenticate-system-only",
                        "server.authentication-system-only");
  options->addOldOption("server.allow-method-override",
                        "http.allow-method-override");
  options->addOldOption("server.hide-product-header",
                        "http.hide-product-header");
  options->addOldOption("server.keep-alive-timeout", "http.keep-alive-timeout");
  options->addOldOption("server.default-api-compatibility", "");
  options->addOldOption("no-server", "server.rest-server");

  options->addOption("--server.authentication",
                     "enable authentication for ALL client requests",
                     new BooleanParameter(&_active));

  options->addOption(
      "--server.authentication-timeout",
      "timeout for the authentication cache in seconds (0 = indefinitely)",
      new DoubleParameter(&_authenticationTimeout));

  options->addOption("--server.local-authentication",
                     "enable authentication using the local user database",
                     new BooleanParameter(&_localAuthentication));

  options->addOption(
      "--server.authentication-system-only",
      "use HTTP authentication only for requests to /_api and /_admin",
      new BooleanParameter(&_authenticationSystemOnly));

#ifdef ARANGODB_HAVE_DOMAIN_SOCKETS
  options->addOption("--server.authentication-unix-sockets",
                     "authentication for requests via UNIX domain sockets",
                     new BooleanParameter(&_authenticationUnixSockets));
#endif

  // Maybe deprecate this option in devel
  options->addOption("--server.jwt-secret",
                     "secret to use when doing jwt authentication",
                     new StringParameter(&_jwtSecretProgramOption))
                     .setDeprecatedIn(30322).setDeprecatedIn(30402);

  options->addOption(
      "--server.jwt-secret-keyfile",
      "file containing jwt secret to use when doing jwt authentication.",
      new StringParameter(&_jwtSecretKeyfileProgramOption));
}

void AuthenticationFeature::validateOptions(std::shared_ptr<ProgramOptions>) {
  if (!_jwtSecretKeyfileProgramOption.empty()) {
    try {
      // Note that the secret is trimmed for whitespace, because whitespace
      // at the end of a file can easily happen. We do not base64-encode,
      // though, so the bytes count as given. Zero bytes might be a problem
      // here.
      _jwtSecretProgramOption =
          basics::StringUtils::trim(basics::FileUtils::slurp(_jwtSecretKeyfileProgramOption),
                                    " \t\n\r");
    } catch (std::exception const& ex) {
      LOG_TOPIC("d3617", FATAL, Logger::STARTUP)
          << "unable to read content of jwt-secret file '"
          << _jwtSecretKeyfileProgramOption << "': " << ex.what()
          << ". please make sure the file/directory is readable for the "
             "arangod process and user";
      FATAL_ERROR_EXIT();
    }

  } else if (!_jwtSecretProgramOption.empty()) {
    if (_jwtSecretProgramOption.length() > _maxSecretLength) {
      LOG_TOPIC("9abfc", FATAL, arangodb::Logger::STARTUP)
          << "Given JWT secret too long. Max length is " << _maxSecretLength;
      FATAL_ERROR_EXIT();
    }
  }
}

void AuthenticationFeature::prepare() {
  TRI_ASSERT(isEnabled());
  TRI_ASSERT(_userManager == nullptr);

  ServerState::RoleEnum role = ServerState::instance()->getRole();
  TRI_ASSERT(role != ServerState::RoleEnum::ROLE_UNDEFINED);
  if (ServerState::isSingleServer(role) || ServerState::isCoordinator(role)) {
#if USE_ENTERPRISE
    if (application_features::ApplicationServer::getFeature<LdapFeature>("Ldap")->isEnabled()) {
      _userManager.reset(
          new auth::UserManager(std::make_unique<LdapAuthenticationHandler>()));
    } else {
      _userManager.reset(new auth::UserManager());
    }
#else
    _userManager.reset(new auth::UserManager());
#endif
  } else {
    LOG_TOPIC("713c0", DEBUG, Logger::AUTHENTICATION) << "Not creating user manager";
  }

  TRI_ASSERT(_authCache == nullptr);
  _authCache.reset(new auth::TokenCache(_userManager.get(), _authenticationTimeout));

  std::string jwtSecret = _jwtSecretProgramOption;
  if (jwtSecret.empty()) {
    LOG_TOPIC("43396", INFO, Logger::AUTHENTICATION)
        << "Jwt secret not specified, generating...";
    uint16_t m = 254;
    for (size_t i = 0; i < _maxSecretLength; i++) {
      jwtSecret += (1 + RandomGenerator::interval(m));
    }
  }
  _authCache->setJwtSecret(jwtSecret);

  INSTANCE = this;
}

void AuthenticationFeature::start() {
  TRI_ASSERT(isEnabled());

  // If this is empty here, --server.jwt-secret was used
  if (!_jwtSecretProgramOption.empty() &&
      _jwtSecretKeyfileProgramOption.empty()) {
    LOG_TOPIC("1aaae", WARN, arangodb::Logger::AUTHENTICATION)
        << "--server.jwt-secret is insecure. Use --server.jwt-secret-keyfile "
           "instead.";
  }

  std::ostringstream out;

  out << "Authentication is turned " << (_active ? "on" : "off");

  if (_userManager != nullptr) {
    auto queryRegistryFeature =
        application_features::ApplicationServer::getFeature<QueryRegistryFeature>(
            "QueryRegistry");
    _userManager->setQueryRegistry(queryRegistryFeature->queryRegistry());
  }

  if (_active && _authenticationSystemOnly) {
    out << " (system only)";
  }

#ifdef ARANGODB_HAVE_DOMAIN_SOCKETS
  out << ", authentication for unix sockets is turned "
      << (_authenticationUnixSockets ? "on" : "off");
#endif

  LOG_TOPIC("3844e", INFO, arangodb::Logger::AUTHENTICATION) << out.str();
}

void AuthenticationFeature::unprepare() { INSTANCE = nullptr; }

}  // namespace arangodb
