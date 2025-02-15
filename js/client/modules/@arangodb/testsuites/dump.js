
/* jshint strict: false, sub: true */
/* global print, arango */
'use strict';

// /////////////////////////////////////////////////////////////////////////////
// DISCLAIMER
// 
// Copyright 2016-2019 ArangoDB GmbH, Cologne, Germany
// Copyright 2014 triagens GmbH, Cologne, Germany
// 
// Licensed under the Apache License, Version 2.0 (the "License")
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//      http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// 
// Copyright holder is ArangoDB GmbH, Cologne, Germany
// 
// @author Max Neunhoeffer
// //////////////////////////////////////////////////////////////////////////////

const functionsDocumentation = {
  'dump': 'dump tests',
  'dump_authentication': 'dump tests with authentication',
  'dump_encrypted': 'encrypted dump tests',
  'dump_maskings': 'masked dump tests',
  'dump_multiple': 'restore multiple DBs at once',
  'hot_backup': 'hotbackup tests'
};

const optionsDocumentation = [
  '   - `skipEncrypted` : if set to true the encryption tests are skipped'
];

const pu = require('@arangodb/process-utils');
const tu = require('@arangodb/test-utils');
const fs = require('fs');
const _ = require('lodash');
const hb = require("@arangodb/hotbackup");
const sleep = require("internal").sleep;

// const BLUE = require('internal').COLORS.COLOR_BLUE;
const CYAN = require('internal').COLORS.COLOR_CYAN;
// const GREEN = require('internal').COLORS.COLOR_GREEN;
// const RED = require('internal').COLORS.COLOR_RED;
const RESET = require('internal').COLORS.COLOR_RESET;
// const YELLOW = require('internal').COLORS.COLOR_YELLOW;

const testPaths = {
  'dump': [tu.pathForTesting('server/dump')],
  'dump_authentication': [tu.pathForTesting('server/dump')],
  'dump_encrypted': [tu.pathForTesting('server/dump')],
  'dump_maskings': [tu.pathForTesting('server/dump')],
  'dump_multiple': [tu.pathForTesting('server/dump')],
  'hot_backup': [tu.pathForTesting('server/dump')]
};

class DumpRestoreHelper {
  constructor(instanceInfo, options, clientAuth, dumpOptions, restoreOptions, which, afterServerStart) {
    this.instanceInfo = instanceInfo;
    this.options = options;
    this.clientAuth = clientAuth;
    this.dumpOptions = dumpOptions;
    this.restoreOptions = restoreOptions;
    this.which = which;
    this.fn = afterServerStart(instanceInfo);
    this.results = {failed: 1};
    this.arangosh = tu.runInArangosh.bind(this, this.options, this.instanceInfo);
    this.dumpConfig = pu.createBaseConfig('dump', this.dumpOptions, this.instanceInfo);
    this.dumpConfig.setOutputDirectory('dump');
    this.dumpConfig.setIncludeSystem(true);

    if (dumpOptions.hasOwnProperty("maskings")) {
       this.dumpConfig.setMaskings(dumpOptions.maskings);
    }
    if (dumpOptions.allDatabases) {
      this.dumpConfig.setAllDatabases();
    }

    this.restoreConfig = pu.createBaseConfig('restore', this.restoreOptions, this.instanceInfo);
    this.restoreConfig.setInputDirectory('dump', true);
    this.restoreConfig.setIncludeSystem(true);

    this.restoreOldConfig = pu.createBaseConfig('restore', this.restoreOptions, this.instanceInfo);
    this.restoreOldConfig.setInputDirectory('dump', true);
    this.restoreOldConfig.setIncludeSystem(true);
    this.restoreOldConfig.setRootDir(pu.TOP_DIR);

    if (options.encrypted) {
      this.dumpConfig.activateEncryption();
      this.restoreOldConfig.activateEncryption();
    }
    if (restoreOptions.allDatabases) {
      this.restoreConfig.setAllDatabases();
      this.restoreOldConfig.setAllDatabases();
    } else {
      this.restoreOldConfig.setDatabase('_system');
    }

    this.arangorestore = pu.run.arangoDumpRestoreWithConfig.bind(this, this.restoreConfig, this.restoreOptions, this.instanceInfo.rootDir, this.options.coreCheck);
    this.arangorestoreOld = pu.run.arangoDumpRestoreWithConfig.bind(this, this.restoreOldConfig, this.restoreOptions, this.instanceInfo.rootDir, this.options.coreCheck);
    this.arangodump = pu.run.arangoDumpRestoreWithConfig.bind(this, this.dumpConfig, this.dumpOptions, this.instanceInfo.rootDir, this.options.coreCheck);
  }

  print (s) {
    print(CYAN + Date() + ': ' + this.which + ' and Restore - ' + s + RESET);
  }

  isAlive() {
    return pu.arangod.check.instanceAlive(this.instanceInfo, this.options);
  }

  getUptime() {
    try {
      return pu.arangod.check.uptime(this.instanceInfo, this.options);
    }
    catch (x) {
      print(x); // TODO
      print("uptime continuing anyways");
      return {};
    }
  }

  validate(phaseInfo) {
    phaseInfo.failed = (phaseInfo.status !== true || !this.isAlive() ? 1 : 0);
    return phaseInfo.failed === 0;
  };
 
  extractResults() {
    if (this.fn !== undefined) {
      fs.remove(this.fn);
    }
    print(CYAN + 'Shutting down...' + RESET);
    this.results['shutdown'] = pu.shutdownInstance(this.instanceInfo, this.options);
    print(CYAN + 'done.' + RESET);

    print();
    return this.results;
  }

  runSetupSuite(path) {
    this.print('Setting up');
    this.results.setup = this.arangosh(path, this.clientAuth);
    return this.validate(this.results.setup);
  }

  runCleanupSuite(path) {
    this.print('Cleaning up');
    this.results.cleanup = this.arangosh(path, this.clientAuth);
    return this.validate(this.results.setup);
  }

  dumpFrom(database) {
    this.print('dump');
    if (!this.dumpConfig.haveSetAllDatabases()) {
      this.dumpConfig.setDatabase(database);
    }
    this.results.dump = this.arangodump();
    return this.validate(this.results.dump);
  }

  restoreTo(database) {
    this.print('restore');
    if (!this.restoreConfig.haveSetAllDatabases()) {
      this.restoreConfig.setDatabase(database);
    }
    this.results.restore = this.arangorestore();
    return this.validate(this.results.restore);
  }

  runTests(file, database) {
    this.print('dump after restore');
    if (this.restoreConfig.haveSetAllDatabases()) {
      // if we dump with multiple databases, it remains with the original name.
      database = 'UnitTestsDumpSrc';
    }
    this.results.test = this.arangosh(file, {'server.database': database});
    return this.validate(this.results.test);
  }

  runReTests(file, database) {
    this.print('revalidating modifications');
    this.results.test = this.arangosh(file, {'server.database': database});
    return this.validate(this.results.test);
  }

  tearDown(file) {
    this.print('teardown');
    this.results.tearDown = this.arangosh(file);
    return this.validate(this.results.tearDown);
  }

  restoreOld(directory) {
    this.print('restoreOld');
    this.restoreOldConfig.setInputDirectory(directory, true);
    this.results.restoreOld = this.arangorestoreOld();
    return this.validate(this.results.restoreOld);
  }

  testRestoreOld(file) {
    this.print('test restoreOld');
    this.results.testRestoreOld = this.arangosh(file);
    return this.validate(this.results.testRestoreOld);
  }

  restoreFoxxComplete(database) {
    this.print('Foxx Apps with full restore');
    this.restoreConfig.setDatabase(database);
    this.results.restoreFoxxComplete = this.arangorestore();
    return this.validate(this.results.restoreFoxxComplete);
  }

  testFoxxComplete(file, database) {
    this.print('Test Foxx Apps after full restore');
    this.results.testFoxxComplete = this.arangosh(file, {'server.database': database});
    return this.validate(this.results.testFoxxComplete);
  }

  restoreFoxxAppsBundle(database) {
    this.print('Foxx Apps restore _apps then _appbundles');
    this.restoreConfig.setDatabase(database);
    this.restoreConfig.restrictToCollection('_apps');
    this.results.restoreFoxxAppBundlesStep1 = this.arangorestore();
    if (!this.validate(this.results.restoreFoxxAppBundlesStep1)) {
      return false;
    }
    this.restoreConfig.restrictToCollection('_appbundles');
    // TODO if cluster, switch coordinator!
    this.results.restoreFoxxAppBundlesStep2 = this.arangorestore();
    return this.validate(this.results.restoreFoxxAppBundlesStep2);
  }

  testFoxxAppsBundle(file, database) {
    this.print('Test Foxx Apps after _apps then _appbundles restore');
    this.results.testFoxxAppBundles = this.arangosh(file, {'server.database': database});
    return this.validate(this.results.testFoxxAppBundles);
  }

  restoreFoxxBundleApps(database) {
    this.print('Foxx Apps restore _appbundles then _apps');
    this.restoreConfig.setDatabase(database);
    this.restoreConfig.restrictToCollection('_appbundles');
    this.results.restoreFoxxAppBundlesStep1 = this.arangorestore();
    if (!this.validate(this.results.restoreFoxxAppBundlesStep1)) {
      return false;
    }
    this.restoreConfig.restrictToCollection('_apps');
    // TODO if cluster, switch coordinator!
    this.results.restoreFoxxAppBundlesStep2 = this.arangorestore();
    return this.validate(this.results.restoreFoxxAppBundlesStep2);
  }

  testFoxxBundleApps(file, database) {
    this.print('Test Foxx Apps after _appbundles then _apps restore');
    this.results.testFoxxFoxxAppBundles = this.arangosh(file, {'server.database': database});
    return this.validate(this.results.testFoxxAppBundles);
  }
  
  createHotBackup() {
    this.print("creating backup");
    let cmds = {
      "label": "testHotBackup"
    };
    this.results.createHotBackup = pu.run.arangoBackup(this.options, this.instanceInfo, "create", cmds, this.instanceInfo.rootDir, true);
    this.print("done creating backup");
    return this.results.createHotBackup.status;
  }

  restoreHotBackup() {
    this.print("restoring backup - start");
    let list = this.listHotBackup();
    let backupName;
    Object.keys(list).forEach(function (name, i) {
      if (name.search("testHotBackup") !== -1) {
        backupName = name;
      }
    });
    if (backupName === undefined) {
      this.print("didn't find a backup matching our pattern!");
      this.results.restoreHotBackup = { status: false };
      return false;
    }
    this.print("restoring backup");
    let cmds = {
      "identifier": backupName,
      "max-wait-for-restart": 100.0
    };
    this.results.restoreHotBackup = pu.run.arangoBackup(this.options, this.instanceInfo, "restore", cmds, this.instanceInfo.rootDir, true);
    this.print("done restoring backup");
    return true;
  }

  listHotBackup() {
    return hb.get();
  }
};

function getClusterStrings(options) {
  if (options.hasOwnProperty('allDatabases') && options.allDatabases) {
    return {
      cluster: '-multiple',
      notCluster: '-multiple'
    };
  }
  if (options.cluster) {
    return {
      cluster: '-cluster',
      notCluster: '-singleserver'
    };
  } else {
    return {
      cluster: '',
      notCluster: '-cluster'
    };
  }
}

function dump_backend (options, serverAuthInfo, clientAuth, dumpOptions, restoreOptions, which, tstFiles, afterServerStart) {
  print(CYAN + which + ' tests...' + RESET);

  let instanceInfo = pu.startInstance('tcp', options, serverAuthInfo, which);

  if (instanceInfo === false) {
    let rc =  {
      failed: 1,
    };
    rc[which] = {
      status: false,
      message: 'failed to start server!'
    };
    return rc;
  }
  const helper = new DumpRestoreHelper(instanceInfo, options, clientAuth, dumpOptions, restoreOptions, which, afterServerStart);
 
  const setupFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpSetup));
  const cleanupFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpCleanup));
  const testFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpAgain));
  const tearDownFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpTearDown));
  if (
    !helper.runSetupSuite(setupFile) ||
    !helper.dumpFrom('UnitTestsDumpSrc') ||
    !helper.runCleanupSuite(cleanupFile) ||  
    !helper.restoreTo('UnitTestsDumpDst') ||
    !helper.runTests(testFile,'UnitTestsDumpDst') ||
    !helper.tearDown(tearDownFile)) {
    return helper.extractResults();
  }

  if (tstFiles.hasOwnProperty("dumpCheckGraph")) {
    const notCluster = getClusterStrings(options).notCluster;
    const restoreDir = tu.makePathUnix(tu.pathForTesting('server/dump/dump' + notCluster));
    const oldTestFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpCheckGraph));
    if (!helper.restoreOld(restoreDir) ||
        !helper.testRestoreOld(oldTestFile)) {
      return helper.extractResults();
    }
  }

  if (tstFiles.hasOwnProperty("foxxTest")) {
    const foxxTestFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.foxxTest));
    if (!helper.restoreFoxxComplete('UnitTestsDumpFoxxComplete') ||
        !helper.testFoxxComplete(foxxTestFile, 'UnitTestsDumpFoxxComplete') ||
        !helper.restoreFoxxAppsBundle('UnitTestsDumpFoxxAppsBundle') ||
        !helper.testFoxxAppsBundle(foxxTestFile, 'UnitTestsDumpFoxxAppsBundle') ||
        !helper.restoreFoxxAppsBundle('UnitTestsDumpFoxxBundleApps') ||
        !helper.testFoxxAppsBundle(foxxTestFile, 'UnitTestsDumpFoxxBundleApps')) {
      return helper.extractResults();
    }
  }

  return helper.extractResults();
}

function dump (options) {
  let c = getClusterStrings(options);
  let tstFiles = {
    dumpSetup: 'dump-setup' + c.cluster + '.js',
    dumpCleanup: 'cleanup-nothing.js',
    dumpAgain: 'dump-' + options.storageEngine + c.cluster + '.js',
    dumpTearDown: 'dump-teardown' + c.cluster + '.js',
    dumpCheckGraph: 'check-graph.js',
    foxxTest: 'check-foxx.js'
  };

  return dump_backend(options, {}, {}, options, options, 'dump', tstFiles, function(){});
}

function dumpMultiple (options) {
  let c = getClusterStrings(options);
  let tstFiles = {
    dumpSetup: 'dump-setup' + c.cluster + '.js',
    dumpCleanup: 'cleanup-multiple.js',
    dumpAgain: 'dump-' + options.storageEngine + c.cluster + '.js',
    dumpTearDown: 'dump-teardown' + c.cluster + '.js',
    dumpCheckGraph: 'check-graph-multiple.js'
  };
  
  let dumpOptions = {
    allDatabases: true
  };
  _.defaults(dumpOptions, options);
  return dump_backend(dumpOptions, {}, {}, dumpOptions, dumpOptions, 'dump_multiple', tstFiles, function(){});
}

function dumpAuthentication (options) {
  if (options.cluster) {
    if (options.extremeVerbosity) {
      print(CYAN + 'Skipped because of cluster.' + RESET);
    }

    return {
      'dump_authentication': {
        'status': true,
        'message': 'skipped because of cluster',
        'skipped': true
      }
    };
  }

  const clientAuth = {
    'server.authentication': 'true'
  };

  const serverAuthInfo = {
    'server.authentication': 'true',
    'server.jwt-secret': 'haxxmann'
  };

  let dumpAuthOpts = {
    username: 'foobaruser',
    password: 'foobarpasswd'
  };

  _.defaults(dumpAuthOpts, options);
  let tstFiles = {
    dumpSetup: 'dump-authentication-setup.js',
    dumpCleanup: 'cleanup-nothing.js',
    dumpAgain: 'dump-authentication.js',
    dumpTearDown: 'dump-teardown.js',
    foxxTest: 'check-foxx.js'
  };

  return dump_backend(options, serverAuthInfo, clientAuth, dumpAuthOpts, dumpAuthOpts, 'dump_authentication', tstFiles, function(){});
}

function dumpEncrypted (options) {
  // test is only meaningful in the enterprise version
  let skip = true;
  if (global.ARANGODB_CLIENT_VERSION) {
    let version = global.ARANGODB_CLIENT_VERSION(true);
    if (version.hasOwnProperty('enterprise-version')) {
      skip = false;
    }
  }

  if (skip) {
    print('skipping dump_encrypted test');
    return {
      dump_encrypted: {
        status: true,
        skipped: true
      }
    };
  }

  let c = getClusterStrings(options);

  let afterServerStart = function(instanceInfo) {
    let keyFile = fs.join(instanceInfo.rootDir, 'secret-key');
    fs.write(keyFile, 'DER-HUND-der-hund-der-hund-der-h'); // must be exactly 32 chars long
    return keyFile;
  };

  let dumpOptions = _.clone(options);
  dumpOptions.encrypted = true;
  
  let tstFiles = {
    dumpSetup: 'dump-setup' + c.cluster + '.js',
    dumpCleanup: 'cleanup-nothing.js',
    dumpAgain: 'dump-' + options.storageEngine + c.cluster + '.js',
    dumpTearDown: 'dump-teardown' + c.cluster + '.js',
    foxxTest: 'check-foxx.js'
  };

  return dump_backend(options, {}, {}, dumpOptions, dumpOptions, 'dump_encrypted', tstFiles, afterServerStart);
}

function dumpMaskings (options) {
  // test is only meaningful in the enterprise version
  let skip = true;
  if (global.ARANGODB_CLIENT_VERSION) {
    let version = global.ARANGODB_CLIENT_VERSION(true);
    if (version.hasOwnProperty('enterprise-version')) {
      skip = false;
    }
  }

  if (skip) {
    print('skipping dump_maskings test');
    return {
      dump_maskings: {
        status: true,
        skipped: true
      }
    };
  }

  let tstFiles = {
    dumpSetup: 'dump-maskings-setup.js',
    dumpCleanup: 'cleanup-nothing.js',
    dumpAgain: 'dump-maskings.js',
    dumpTearDown: 'dump-teardown.js'
  };

  let dumpMaskingsOpts = {
    maskings: 'maskings1.json'
  };

  _.defaults(dumpMaskingsOpts, options);

  return dump_backend(options, {}, {}, dumpMaskingsOpts, options, 'dump_maskings', tstFiles, function(){});
}

function hotBackup (options) {
  let c = getClusterStrings(options);
  options.extraArgs["backup.api-enabled"] = true;
  if (options.storageEngine === "mmfiles") {
    return {
      'hotbackup for mmfiles not yet implemented': {
        status: true,
      }
    };
  }
  if (!require("internal").isEnterprise()) {
    return {
      'hotbackup is only enterprise': {
        status: true,
      }
    };
  }
  let tstFiles = {
    dumpSetup: 'dump-setup' + c.cluster + '.js',
    dumpCheck: 'dump-' + options.storageEngine + c.cluster + '.js',
    dumpModify: 'dump-' + options.storageEngine + '-modify.js',
    dumpRecheck: 'dump-' + options.storageEngine + '-modified.js',
    dumpTearDown: 'dump-teardown' + c.cluster + '.js',
    // do we need this? dumpCheckGraph: 'check-graph.js',
    // todo foxxTest: 'check-foxx.js'
  };

  let which = "dump";
  // /return dump_backend(options, {}, {}, dumpMaskingsOpts, options, 'dump_maskings', tstFiles, function(){});
  print(CYAN + which + ' tests...' + RESET);

  let instanceInfo = pu.startInstance('tcp', options, {}, which);

  if (instanceInfo === false) {
    let rc =  {
      failed: 1,
    };
    rc[which] = {
      status: false,
      message: 'failed to start server!'
    };
    return rc;
  }
  const helper = new DumpRestoreHelper(instanceInfo, options, {}, options, options, which, function(){});
  const setupFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpSetup));
  const dumpCheck = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpCheck));
  const dumpModify = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpModify));
  const dumpRecheck  = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpRecheck));
  const tearDownFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpTearDown));
  if (!helper.runSetupSuite(setupFile) ||
      !helper.dumpFrom('UnitTestsDumpSrc') ||
      !helper.restoreTo('UnitTestsDumpDst') ||
      !helper.isAlive() ||
      !helper.createHotBackup() ||
      !helper.isAlive() ||
      !helper.runTests(dumpModify,'UnitTestsDumpDst') ||
      !helper.isAlive() ||
      !helper.runReTests(dumpRecheck,'UnitTestsDumpDst') ||
      !helper.isAlive() ||
      !helper.restoreHotBackup() ||
      !helper.runTests(dumpCheck, 'UnitTestsDumpDst')||
      !helper.tearDown(tearDownFile)) {
    return helper.extractResults();
  }

  if (tstFiles.hasOwnProperty("dumpCheckGraph")) {
    const notCluster = getClusterStrings(options).notCluster;
    const restoreDir = tu.makePathUnix(tu.pathForTesting('server/dump/dump' + notCluster));
    const oldTestFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.dumpCheckGraph));
    if (!helper.restoreOld(restoreDir) ||
        !helper.testRestoreOld(oldTestFile)) {
      return helper.extractResults();
    }
  }

  if (tstFiles.hasOwnProperty("foxxTest")) {
    const foxxTestFile = tu.makePathUnix(fs.join(testPaths[which][0], tstFiles.foxxTest));
    if (!helper.restoreFoxxComplete('UnitTestsDumpFoxxComplete') ||
        !helper.testFoxxComplete(foxxTestFile, 'UnitTestsDumpFoxxComplete') ||
        !helper.restoreFoxxAppsBundle('UnitTestsDumpFoxxAppsBundle') ||
        !helper.testFoxxAppsBundle(foxxTestFile, 'UnitTestsDumpFoxxAppsBundle') ||
        !helper.restoreFoxxAppsBundle('UnitTestsDumpFoxxBundleApps') ||
        !helper.testFoxxAppsBundle(foxxTestFile, 'UnitTestsDumpFoxxBundleApps')) {
      return helper.extractResults();
    }
  }

  return helper.extractResults();
}

exports.setup = function (testFns, defaultFns, opts, fnDocs, optionsDoc, allTestPaths) {
  Object.assign(allTestPaths, testPaths);
  testFns['dump'] = dump;
  defaultFns.push('dump');

  testFns['dump_authentication'] = dumpAuthentication;
  defaultFns.push('dump_authentication');

  testFns['dump_encrypted'] = dumpEncrypted;
  defaultFns.push('dump_encrypted');

  testFns['dump_maskings'] = dumpMaskings;
  defaultFns.push('dump_maskings');

  testFns['dump_multiple'] = dumpMultiple;
  defaultFns.push('dump_multiple');

  testFns['hot_backup'] = hotBackup;
  defaultFns.push('hot_backup');

  for (var attrname in functionsDocumentation) { fnDocs[attrname] = functionsDocumentation[attrname]; }
  for (var i = 0; i < optionsDocumentation.length; i++) { optionsDoc.push(optionsDocumentation[i]); }
};
