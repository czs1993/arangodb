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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <chrono>
#include <exception>
#include <iostream>
#include <new>
#include <unordered_set>
#include <utility>

#include <velocypack/Options.h>
#include <velocypack/Slice.h>
#include <velocypack/velocypack-aliases.h>

#include "ApplicationServer.h"

#include "ApplicationFeatures/ApplicationFeature.h"
#include "ApplicationFeatures/PrivilegeFeature.h"
#include "Basics/ConditionLocker.h"
#include "Basics/Exceptions.h"
#include "Basics/Result.h"
#include "Basics/ScopeGuard.h"
#include "Basics/StringUtils.h"
#include "Basics/application-exit.h"
#include "Basics/debugging.h"
#include "Basics/voc-errors.h"
#include "Logger/LogMacros.h"
#include "Logger/Logger.h"
#include "Logger/LoggerStream.h"
#include "ProgramOptions/ArgumentParser.h"
#include "ProgramOptions/Option.h"
#include "ProgramOptions/Parameters.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"

using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

namespace {
// fail and abort with the specified message
static void failCallback(std::string const& message) {
  LOG_TOPIC("85b08", FATAL, arangodb::Logger::FIXME) << "error. cannot proceed. reason: " << message;
  FATAL_ERROR_EXIT();
}
}  // namespace

ApplicationServer* ApplicationServer::server = nullptr;

ApplicationServer::ApplicationServer(std::shared_ptr<ProgramOptions> options,
                                     const char* binaryPath)
    : _state(State::UNINITIALIZED),
      _options(options),
      _binaryPath(binaryPath) {
  // register callback function for failures
  fail = failCallback;

  if (ApplicationServer::server != nullptr) {
    LOG_TOPIC("fbe91", ERR, arangodb::Logger::FIXME)
        << "ApplicationServer initialized twice";
  }

  ApplicationServer::server = this;
}

ApplicationServer::~ApplicationServer() {
  for (auto& it : _features) {
    try {
      delete it.second;
    } catch (...) {
      // we must skip over errors here as we're in the destructor.
      // we cannot rely on the LoggerFeature being present either, so
      // we have to suppress errors here
    }
  }

  ApplicationServer::server = nullptr;
}

bool ApplicationServer::isPrepared() {
  if (server == nullptr) {
    return false;
  }

  auto tmp = server->state();
  return tmp == State::IN_START || 
         tmp == State::IN_WAIT ||
         tmp == State::IN_SHUTDOWN ||
         tmp == State::IN_STOP;
}

bool ApplicationServer::isStopping() {
  if (server == nullptr) {
    return false;
  }

  auto tmp = server->state();
  return isStoppingState(tmp);
}

bool ApplicationServer::isStoppingState(State state) {
  return state == State::IN_SHUTDOWN ||
         state == State::IN_STOP ||
         state == State::IN_UNPREPARE ||
         state == State::STOPPED ||
         state == State::ABORTED; 
}

void ApplicationServer::throwFeatureNotFoundException(std::string const& name) {
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "unknown feature '" + name + "'");
}

void ApplicationServer::throwFeatureNotEnabledException(std::string const& name) {
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "feature '" + name + "' is not enabled");
}

ApplicationFeature* ApplicationServer::lookupFeature(std::string const& name) {
  if (ApplicationServer::server == nullptr) {
    return nullptr;
  }

  try {
    return ApplicationServer::server->feature(name);
  } catch (...) {
    return nullptr;
  }
}

void ApplicationServer::disableFeatures(std::vector<std::string> const& names) {
  disableFeatures(names, false);
}

void ApplicationServer::forceDisableFeatures(std::vector<std::string> const& names) {
  disableFeatures(names, true);
}

void ApplicationServer::disableFeatures(std::vector<std::string> const& names, bool force) {
  for (auto const& name : names) {
    auto feature = ApplicationServer::lookupFeature(name);

    if (feature != nullptr) {
      if (force) {
        feature->forceDisable();
      } else {
        feature->disable();
      }
    }
  }
}

// adds a feature to the application server. the application server
// will take ownership of the feature object and destroy it in its
// destructor
void ApplicationServer::addFeature(ApplicationFeature* feature) {
  TRI_ASSERT(feature->state() == ApplicationFeature::State::UNINITIALIZED);
  _features.emplace(feature->name(), feature);
}

// checks for the existence of a named feature. will not throw when used for
// a non-existing feature
bool ApplicationServer::exists(std::string const& name) const {
  return (_features.find(name) != _features.end());
}

// returns a pointer to a named feature. will throw when used for
// a non-existing feature
ApplicationFeature* ApplicationServer::feature(std::string const& name) const {
  auto it = _features.find(name);

  if (it == _features.end()) {
    throwFeatureNotFoundException(name);
  }
  return (*it).second;
}

// return whether or not a feature is enabled
// will throw when called for a non-existing feature
bool ApplicationServer::isEnabled(std::string const& name) const {
  return feature(name)->isEnabled();
}

// return whether or not a feature is optional
// will throw when called for a non-existing feature
bool ApplicationServer::isOptional(std::string const& name) const {
  return feature(name)->isOptional();
}

// return whether or not a feature is required
// will throw when called for a non-existing feature
bool ApplicationServer::isRequired(std::string const& name) const {
  return feature(name)->isRequired();
}

// this method will initialize and validate options
// of all feature, start them and wait for a shutdown
// signal. after that, it will shutdown all features
void ApplicationServer::run(int argc, char* argv[]) {
  LOG_TOPIC("cc34f", TRACE, Logger::STARTUP) << "ApplicationServer::run";

  // collect options from all features
  // in this phase, all features are order-independent
  _state.store(State::IN_COLLECT_OPTIONS, std::memory_order_relaxed);
  reportServerProgress(State::IN_COLLECT_OPTIONS);
  collectOptions();

  // setup dependency, but ignore any failure for now
  setupDependencies(false);

  // parse the command line parameters and load any configuration
  // file(s)
  parseOptions(argc, argv);

  if (!_helpSection.empty()) {
    // help shown. we can exit early
    return;
  }

  // seal the options
  _options->seal();

  // validate options of all features
  _state.store(State::IN_VALIDATE_OPTIONS, std::memory_order_relaxed);
  reportServerProgress(State::IN_VALIDATE_OPTIONS);
  validateOptions();

  // setup and validate all feature dependencies
  setupDependencies(true);

  // turn off all features that depend on other features that have been
  // turned off
  disableDependentFeatures();

  // allows process control
  daemonize();

  // now the features will actually do some preparation work
  // in the preparation phase, the features must not start any threads
  // furthermore, they must not write any files under elevated privileges
  // if they want other features to access them, or if they want to access
  // these files with dropped privileges
  _state.store(State::IN_PREPARE, std::memory_order_relaxed);
  reportServerProgress(State::IN_PREPARE);
  prepare();

  // turn off all features that depend on other features that have been
  // turned off. we repeat this to allow features to turn other features
  // off even in the prepare phase
  disableDependentFeatures();

  // permanently drop the privileges
  dropPrivilegesPermanently();

  // start features. now features are allowed to start threads, write files etc.
  _state.store(State::IN_START, std::memory_order_relaxed);
  reportServerProgress(State::IN_START);
  start();

  // wait until we get signaled the shutdown request
  _state.store(State::IN_WAIT, std::memory_order_relaxed);
  reportServerProgress(State::IN_WAIT);
  wait();

  // beginShutdown is called asynchronously ----------

  // stop all features
  _state.store(State::IN_STOP, std::memory_order_relaxed);
  reportServerProgress(State::IN_STOP);
  stop();

  // unprepare all features
  _state.store(State::IN_UNPREPARE, std::memory_order_relaxed);
  reportServerProgress(State::IN_UNPREPARE);
  unprepare();

  // stopped
  _state.store(State::STOPPED, std::memory_order_relaxed);
  reportServerProgress(State::STOPPED);
}

// signal the server to shut down
void ApplicationServer::beginShutdown() {
  // fetch the old state, check if somebody already called shutdown, and only
  // proceed if not.
  State old = State::UNINITIALIZED;
  do {
    old = state();
    if (isStoppingState(old)) {
      // beginShutdown already called, nothing to do now
      return;
    }
    // try to enter the new state, but make sure nobody changed it in between
  } while (!_state.compare_exchange_weak(old, State::IN_SHUTDOWN, std::memory_order_relaxed));
  
  LOG_TOPIC("c7911", TRACE, Logger::STARTUP) << "ApplicationServer::beginShutdown";

  // make sure that we advance the state when we get out of here
  auto waitAborter = scopeGuard([this]() { 
    CONDITION_LOCKER(guard, _shutdownCondition);

    _abortWaiting = true;
    guard.signal();
  });
  
  // now we can execute the actual shutdown sequence

  // fowards the begin shutdown signal to all features
  for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend(); ++it) {
    if ((*it)->isEnabled()) {
      LOG_TOPIC("e181f", TRACE, Logger::STARTUP) << (*it)->name() << "::beginShutdown";
      try {
        (*it)->beginShutdown();
      } catch (std::exception const& ex) {
        LOG_TOPIC("b2cf4", ERR, Logger::STARTUP)
            << "caught exception during beginShutdown of feature '"
            << (*it)->name() << "': " << ex.what();
      } catch (...) {
        LOG_TOPIC("3f708", ERR, Logger::STARTUP)
            << "caught unknown exception during beginShutdown of feature '"
            << (*it)->name() << "'";
      }
    }
  }
}

void ApplicationServer::shutdownFatalError() {
  reportServerProgress(State::ABORTED);
}

// return VPack options, with optional filters applied to filter
// out specific options. the filter function is expected to return true
// for any options that should become part of the result
VPackBuilder ApplicationServer::options(std::function<bool(std::string const&)> const& filter) const {
  return _options->toVPack(false, false, filter);
}

// walks over all features and runs a callback function for them
// the order in which features are visited is unspecified
void ApplicationServer::apply(std::function<void(ApplicationFeature*)> callback,
                              bool enabledOnly) {
  for (auto& it : _features) {
    if (!enabledOnly || it.second->isEnabled()) {
      callback(it.second);
    }
  }
}

void ApplicationServer::collectOptions() {
  LOG_TOPIC("0eac7", TRACE, Logger::STARTUP) << "ApplicationServer::collectOptions";

  _options->addSection(Section("", "Global configuration", "global options", false, false));

  _options->addOption("--dump-dependencies", "dump dependency graph",
                      new BooleanParameter(&_dumpDependencies),
                      arangodb::options::makeFlags(arangodb::options::Flags::Hidden,
                                                   arangodb::options::Flags::Command));

  _options->addOption("--dump-options",
                      "dump configuration options in JSON format",
                      new BooleanParameter(&_dumpOptions),
                      arangodb::options::makeFlags(arangodb::options::Flags::Hidden,
                                                   arangodb::options::Flags::Command));

  apply(
      [this](ApplicationFeature* feature) {
        LOG_TOPIC("b2731", TRACE, Logger::STARTUP) << feature->name() << "::loadOptions";
        feature->collectOptions(_options);
        reportFeatureProgress(_state.load(std::memory_order_relaxed), feature->name());
      },
      true);
}

void ApplicationServer::parseOptions(int argc, char* argv[]) {
  ArgumentParser parser(_options.get());

  _helpSection = parser.helpSection(argc, argv);

  if (!_helpSection.empty()) {
    // user asked for "--help"

    // translate "all" to ".", because section "all" does not exist
    if (_helpSection == "all" || _helpSection == "hidden") {
      _helpSection = ".";
    }
    _options->printHelp(_helpSection);
    return;
  }

  if (!parser.parse(argc, argv)) {
    // command-line option parsing failed. an error was already printed
    // by now, so we can exit
    exit(EXIT_FAILURE);
  }

  if (_dumpDependencies) {
    std::cout << "digraph dependencies\n"
              << "{\n"
              << "  overlap = false;\n";
    for (auto feature : _features) {
      for (auto before : feature.second->startsAfter()) {
        std::cout << "  " << feature.first << " -> " << before << ";\n";
      }
    }
    std::cout << "}\n";
    exit(EXIT_SUCCESS);
  }

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    if ((*it)->isEnabled()) {
      LOG_TOPIC("5c642", TRACE, Logger::STARTUP) << (*it)->name() << "::loadOptions";
      (*it)->loadOptions(_options, _binaryPath);
    }
  }

  if (_dumpOptions) {
    auto builder = _options->toVPack(false, true, [](std::string const&) { return true; });
    arangodb::velocypack::Options options;
    options.prettyPrint = true;
    std::cout << builder.slice().toJson(&options) << std::endl;
    exit(EXIT_SUCCESS);
  }
}

void ApplicationServer::validateOptions() {
  LOG_TOPIC("1ed27", TRACE, Logger::STARTUP) << "ApplicationServer::validateOptions";

  for (auto feature : _orderedFeatures) {
    if (feature->isEnabled()) {
      LOG_TOPIC("fa73c", TRACE, Logger::STARTUP) << feature->name() << "::validateOptions";
      feature->validateOptions(_options);
      feature->state(ApplicationFeature::State::VALIDATED);
      reportFeatureProgress(_state.load(std::memory_order_relaxed), feature->name());
    }
  }

  // inform about obsolete options
  _options->walk(
      [](Section const&, Option const& option) {
        if (option.hasFlag(arangodb::options::Flags::Obsolete)) {
          LOG_TOPIC("6843e", WARN, Logger::STARTUP)
              << "obsolete option '" << option.displayName() << "' used in configuration. "
              << "setting this option will not have any effect.";
        }
      },
      true, true);
}

// setup and validate all feature dependencies, determine feature order
void ApplicationServer::setupDependencies(bool failOnMissing) {
  LOG_TOPIC("15559", TRACE, Logger::STARTUP)
      << "ApplicationServer::validateDependencies";

  // apply all "startsBefore" values
  for (auto& it : _features) {
    for (auto const& other : it.second->startsBefore()) {
      if (!this->exists(other)) {
        if (failOnMissing) {
          fail("feature '" + it.second->name() +
               "' depends on unknown feature '" + other + "'");
        }
        continue;
      }
      this->feature(other)->startsAfter(it.second->name());
    }
  }

  // calculate ancestors for all features
  for (auto& it : _features) {
    it.second->determineAncestors();
  }

  // first check if a feature references an unknown other feature
  if (failOnMissing) {
    apply(
        [this](ApplicationFeature* feature) {
          for (auto& other : feature->requires()) {
            if (!this->exists(other)) {
              fail("feature '" + feature->name() +
                   "' depends on unknown feature '" + other + "'");
            }
            if (!this->feature(other)->isEnabled()) {
              fail("enabled feature '" + feature->name() +
                   "' depends on other feature '" + other +
                   "', which is disabled");
            }
          }
        },
        true);
  }

  // first insert all features, even the inactive ones
  std::vector<ApplicationFeature*> features;
  for (auto& it : _features) {
    auto const& us = it.second;
    auto insertPosition = features.end();

    for (size_t i = features.size(); i > 0; --i) {
      auto const& other = features[i - 1];
      if (us->doesStartBefore(other->name())) {
        // we start before the other feature. so move ourselves up
        insertPosition = features.begin() + (i - 1);
      } else if (other->doesStartBefore(us->name())) {
        // the other feature starts before us. so stop moving up
        break;
      } else {
        // no dependencies between the two features
        if (us->name() < other->name()) {
          insertPosition = features.begin() + (i - 1);
        }
      }
    }
    features.insert(insertPosition, it.second);
  }

  LOG_TOPIC("0fafb", TRACE, Logger::STARTUP) << "ordered features:";

  int position = 0;
  for (auto feature : features) {
    auto const& startsAfter = feature->startsAfter();

    std::string dependencies;
    if (!startsAfter.empty()) {
      dependencies = " - depends on: " + StringUtils::join(startsAfter, ", ");
    }
    LOG_TOPIC("b2ad5", TRACE, Logger::STARTUP)
        << "feature #" << ++position << ": " << feature->name()
        << (feature->isEnabled() ? "" : " (disabled)") << dependencies;
  }

  // remove all inactive features
  for (auto it = features.begin(); it != features.end(); /* no hoisting */) {
    if ((*it)->isEnabled()) {
      // keep feature
      (*it)->state(ApplicationFeature::State::INITIALIZED);
      ++it;
    } else {
      // remove feature
      it = features.erase(it);
    }
  }

  _orderedFeatures = features;
}

void ApplicationServer::daemonize() {
  LOG_TOPIC("ca0b1", TRACE, Logger::STARTUP) << "ApplicationServer::daemonize";

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    if ((*it)->isEnabled()) {
      (*it)->daemonize();
    }
  }
}

void ApplicationServer::disableDependentFeatures() {
  LOG_TOPIC("3e03b", TRACE, Logger::STARTUP)
      << "ApplicationServer::disableDependentFeatures";

  for (auto feature : _orderedFeatures) {
    auto const& onlyEnabledWith = feature->onlyEnabledWith();

    if (!feature->isEnabled() || onlyEnabledWith.empty()) {
      continue;
    }

    for (auto const& other : onlyEnabledWith) {
      ApplicationFeature* f = lookupFeature(other);
      if (f == nullptr) {
        LOG_TOPIC("f70cc", TRACE, Logger::STARTUP)
            << "turning off feature '" << feature->name()
            << "' because it is enabled only in conjunction with non-existing "
               "feature '"
            << other << "'";
        feature->disable();
        break;
      } else if (!f->isEnabled()) {
        LOG_TOPIC("58e0e", TRACE, Logger::STARTUP)
            << "turning off feature '" << feature->name()
            << "' because it is enabled only in conjunction with disabled "
               "feature '"
            << f->name() << "'";
        feature->disable();
        break;
      }
    }
  }
}

void ApplicationServer::prepare() {
  LOG_TOPIC("04e8f", TRACE, Logger::STARTUP) << "ApplicationServer::prepare";

  // we start with elevated privileges
  bool privilegesElevated = true;

  for (auto feature : _orderedFeatures) {
    if (feature->isEnabled()) {
      bool const requiresElevated = feature->requiresElevatedPrivileges();

      if (requiresElevated != privilegesElevated) {
        // must change privileges for the feature
        if (requiresElevated) {
          raisePrivilegesTemporarily();
          privilegesElevated = true;
        } else {
          dropPrivilegesTemporarily();
          privilegesElevated = false;
        }
      }

      try {
        LOG_TOPIC("d4e57", TRACE, Logger::STARTUP) << feature->name() << "::prepare";
        feature->prepare();
        feature->state(ApplicationFeature::State::PREPARED);
      } catch (std::exception const& ex) {
        LOG_TOPIC("37921", ERR, Logger::STARTUP)
            << "caught exception during prepare of feature '" << feature->name()
            << "': " << ex.what();
        // restore original privileges
        if (!privilegesElevated) {
          raisePrivilegesTemporarily();
        }
        throw;
      } catch (...) {
        LOG_TOPIC("a1b9f", ERR, Logger::STARTUP)
            << "caught unknown exception during preparation of feature '"
            << feature->name() << "'";
        // restore original privileges
        if (!privilegesElevated) {
          raisePrivilegesTemporarily();
        }
        throw;
      }

      reportFeatureProgress(_state.load(std::memory_order_relaxed), feature->name());
    }
  }
}

void ApplicationServer::start() {
  LOG_TOPIC("8ef64", TRACE, Logger::STARTUP) << "ApplicationServer::start";

  Result res;

  for (auto feature : _orderedFeatures) {
    if (!feature->isEnabled()) {
      continue;
    }

    LOG_TOPIC("27b63", TRACE, Logger::STARTUP) << feature->name() << "::start";

    try {
      feature->start();
      feature->state(ApplicationFeature::State::STARTED);
      reportFeatureProgress(_state.load(std::memory_order_relaxed), feature->name());
    } catch (basics::Exception const& ex) {
      res.reset(
          ex.code(),
          std::string(
              "startup aborted: caught exception during start of feature '") +
              feature->name() + "': " + ex.what());
    } catch (std::bad_alloc const& ex) {
      res.reset(
          TRI_ERROR_OUT_OF_MEMORY,
          std::string(
              "startup aborted: caught exception during start of feature '") +
              feature->name() + "': " + ex.what());
    } catch (std::exception const& ex) {
      res.reset(
          TRI_ERROR_INTERNAL,
          std::string(
              "startup aborted: caught exception during start of feature '") +
              feature->name() + "': " + ex.what());
    } catch (...) {
      res.reset(TRI_ERROR_INTERNAL,
                std::string("startup aborted: caught unknown exception during "
                            "start of feature '") +
                    feature->name() + "'");
    }

    if (res.fail()) {
      LOG_TOPIC("4ec19", ERR, Logger::STARTUP) << res.errorMessage() << ". shutting down";
      LOG_TOPIC("51732", TRACE, Logger::STARTUP)
          << "aborting startup, now stopping and unpreparing all features";
      // try to stop all feature that we just started
      for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend(); ++it) {
        auto feature = *it;
        if (!feature->isEnabled()) {
          continue;
        }
        if (feature->state() == ApplicationFeature::State::STARTED) {
          LOG_TOPIC("e5cfd", TRACE, Logger::STARTUP)
              << "forcefully stopping feature '" << feature->name() << "'";
          try {
            feature->beginShutdown();
            feature->stop();
            feature->state(ApplicationFeature::State::STOPPED);
          } catch (...) {
            // ignore errors on shutdown
            LOG_TOPIC("13223", TRACE, Logger::STARTUP)
                << "caught exception while stopping feature '" << feature->name() << "'";
          }
        }
      }

      // try to unprepare all feature that we just started
      for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend(); ++it) {
        auto feature = *it;
        if (feature->state() == ApplicationFeature::State::STOPPED) {
          LOG_TOPIC("6ba4f", TRACE, Logger::STARTUP)
              << "forcefully unpreparing feature '" << feature->name() << "'";
          try {
            feature->unprepare();
            feature->state(ApplicationFeature::State::UNPREPARED);
          } catch (...) {
            // ignore errors on shutdown
            LOG_TOPIC("7d68f", TRACE, Logger::STARTUP)
                << "caught exception while unpreparing feature '"
                << feature->name() << "'";
          }
        }
      }
      shutdownFatalError();
      // throw exception so the startup aborts
      THROW_ARANGO_EXCEPTION(res);
    }
  }

  for (auto const& callback : _startupCallbacks) {
    callback();
  }
}

void ApplicationServer::stop() {
  LOG_TOPIC("3e53e", TRACE, Logger::STARTUP) << "ApplicationServer::stop";

  for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend(); ++it) {
    auto feature = *it;
    if (!feature->isEnabled()) {
      continue;
    }

    LOG_TOPIC("4cd18", TRACE, Logger::STARTUP) << feature->name() << "::stop";
    try {
      feature->stop();
    } catch (std::exception const& ex) {
      LOG_TOPIC("f07eb", ERR, Logger::STARTUP)
          << "caught exception during stop of feature '" << feature->name()
          << "': " << ex.what();
    } catch (...) {
      LOG_TOPIC("6d496", ERR, Logger::STARTUP)
          << "caught unknown exception during stop of feature '"
          << feature->name() << "'";
    }
    feature->state(ApplicationFeature::State::STOPPED);
    reportFeatureProgress(_state.load(std::memory_order_relaxed), feature->name());
  }
}

void ApplicationServer::unprepare() {
  LOG_TOPIC("d6764", TRACE, Logger::STARTUP) << "ApplicationServer::unprepare";

  for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend(); ++it) {
    auto feature = *it;
    if (!feature->isEnabled()) {
      continue;
    }

    LOG_TOPIC("98be4", TRACE, Logger::STARTUP) << feature->name() << "::unprepare";
    try {
      feature->unprepare();
    } catch (std::exception const& ex) {
      LOG_TOPIC("dc019", ERR, Logger::STARTUP)
          << "caught exception during unprepare of feature '" << feature->name()
          << "': " << ex.what();
    } catch (...) {
      LOG_TOPIC("859a3", ERR, Logger::STARTUP)
          << "caught unknown exception during unprepare of feature '"
          << feature->name() << "'";
    }
    feature->state(ApplicationFeature::State::UNPREPARED);
    reportFeatureProgress(_state.load(std::memory_order_relaxed), feature->name());
  }
}

void ApplicationServer::wait() {
  LOG_TOPIC("f86df", TRACE, Logger::STARTUP) << "ApplicationServer::wait";

  // wait here until beginShutdown has been called and finished
  while (true) {
    // wait until somebody calls beginShutdown and it finishes
    CONDITION_LOCKER(guard, _shutdownCondition);
    
    if (_abortWaiting) {
      // yippieh!
      break;
    }

    using namespace std::chrono_literals;
    guard.wait(100ms);
  }
}

// temporarily raise privileges
void ApplicationServer::raisePrivilegesTemporarily() {
  if (_privilegesDropped) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "must not raise privileges after dropping them");
  }

  LOG_TOPIC("34163", TRACE, Logger::STARTUP) << "raising privileges";
  // TODO: raising privileges not implemented
}

// temporarily drop privileges
void ApplicationServer::dropPrivilegesTemporarily() {
  if (_privilegesDropped) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "must not try to drop privileges after dropping them");
  }

  LOG_TOPIC("8d23d", TRACE, Logger::STARTUP) << "dropping privileges";
  // TODO: dropping privileges not implemented
}

// permanently dropped privileges
void ApplicationServer::dropPrivilegesPermanently() {
  if (_privilegesDropped) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "must not try to drop privileges after having dropped them");
  }

  if (exists("Privilege")) {
    auto privilege =
        dynamic_cast<PrivilegeFeature*>(lookupFeature("Privilege"));
    if (privilege != nullptr) {
      privilege->dropPrivilegesPermanently();
    }
  }

  _privilegesDropped = true;
}

void ApplicationServer::reportServerProgress(State state) {
  for (auto reporter : _progressReports) {
    reporter._state(state);
  }
}

void ApplicationServer::reportFeatureProgress(State state, std::string const& name) {
  for (auto reporter : _progressReports) {
    reporter._feature(state, name);
  }
}

char const* ApplicationServer::stringifyState() const { 
  switch (_state.load()) {
    case State::UNINITIALIZED: 
      return "uninitialized";
    case State::IN_COLLECT_OPTIONS:
      return "in collect options";
    case State::IN_VALIDATE_OPTIONS:
      return "in validate options";
    case State::IN_PREPARE:
      return "in prepare";
    case State::IN_START:
      return "in start";
    case State::IN_WAIT:
      return "in wait";
    case State::IN_SHUTDOWN:
      return "in beginShutdown";
    case State::IN_STOP:
      return "in stop";
    case State::IN_UNPREPARE:
      return "in unprepare";
    case State::STOPPED:
      return "in stopped";
    case State::ABORTED:
      return "in aborted";
  }
  // we should never get here
  TRI_ASSERT(false);
  return "unknown";
}
