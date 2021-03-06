/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <set>
#include <string>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/logging.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include "master/detector.hpp"

#include "zookeeper/detector.hpp"
#include "zookeeper/group.hpp"
#include "zookeeper/url.hpp"

using namespace process;
using namespace zookeeper;

using std::set;
using std::string;

namespace mesos {
namespace internal {

const Duration MASTER_DETECTOR_ZK_SESSION_TIMEOUT = Seconds(10);


class StandaloneMasterDetectorProcess
  : public Process<StandaloneMasterDetectorProcess>
{
public:
  StandaloneMasterDetectorProcess() {}
  StandaloneMasterDetectorProcess(const UPID& _leader)
    : leader(_leader) {}
  ~StandaloneMasterDetectorProcess();

  void appoint(const Option<UPID>& leader);
  Future<Option<UPID> > detect(const Option<UPID>& previous = None());

private:
  Option<UPID> leader; // The appointed master.
  set<Promise<Option<UPID> >*> promises;
};


class ZooKeeperMasterDetectorProcess
  : public Process<ZooKeeperMasterDetectorProcess>
{
public:
  ZooKeeperMasterDetectorProcess(const URL& url);
  ZooKeeperMasterDetectorProcess(Owned<Group> group);
  ~ZooKeeperMasterDetectorProcess();

  virtual void initialize();
  Future<Option<UPID> > detect(const Option<UPID>& previous);

private:
  // Invoked when the group leadership has changed.
  void detected(const Future<Option<Group::Membership> >& leader);

  // Invoked when we have fetched the data associated with the leader.
  void fetched(const Future<string>& data);

  Owned<Group> group;
  LeaderDetector detector;

  // The leading Master.
  Option<UPID> leader;
  set<Promise<Option<UPID> >*> promises;

  // Potential non-retryable error.
  Option<Error> error;
};


Try<MasterDetector*> MasterDetector::create(const string& master)
{
  if (master == "") {
    return new StandaloneMasterDetector();
  } else if (master.find("zk://") == 0) {
    Try<URL> url = URL::parse(master);
    if (url.isError()) {
      return Try<MasterDetector*>::error(url.error());
    }
    if (url.get().path == "/") {
      return Try<MasterDetector*>::error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterDetector(url.get());
  } else if (master.find("file://") == 0) {
    const string& path = master.substr(7);
    const Try<string> read = os::read(path);
    if (read.isError()) {
      return Error("Failed to read from file at '" + path + "'");
    }

    return create(strings::trim(read.get()));
  }

  // Okay, try and parse what we got as a PID.
  UPID pid = master.find("master@") == 0
    ? UPID(master)
    : UPID("master@" + master);

  if (!pid) {
    return Try<MasterDetector*>::error(
        "Failed to parse '" + master + "'");
  }

  return new StandaloneMasterDetector(pid);
}


MasterDetector::~MasterDetector() {}


StandaloneMasterDetectorProcess::~StandaloneMasterDetectorProcess()
{
  foreach (Promise<Option<UPID> >* promise, promises) {
    promise->future().discard();
    delete promise;
  }
  promises.clear();
}


void StandaloneMasterDetectorProcess::appoint(
    const Option<process::UPID>& _leader)
{
  leader = _leader;

  foreach (Promise<Option<UPID> >* promise, promises) {
    promise->set(leader);
    delete promise;
  }
  promises.clear();
}


Future<Option<UPID> > StandaloneMasterDetectorProcess::detect(
    const Option<UPID>& previous)
{
  if (leader != previous) {
    return leader;
  }

  Promise<Option<UPID> >* promise = new Promise<Option<UPID> >();
  promises.insert(promise);
  return promise->future();
}


StandaloneMasterDetector::StandaloneMasterDetector()
{
  process = new StandaloneMasterDetectorProcess();
  spawn(process);
}


StandaloneMasterDetector::StandaloneMasterDetector(const UPID& leader)
{
  process = new StandaloneMasterDetectorProcess(leader);
  spawn(process);
}


StandaloneMasterDetector::~StandaloneMasterDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


void StandaloneMasterDetector::appoint(const Option<process::UPID>& leader)
{
  return dispatch(process, &StandaloneMasterDetectorProcess::appoint, leader);
}


Future<Option<UPID> > StandaloneMasterDetector::detect(
    const Option<UPID>& previous)
{
  return dispatch(process, &StandaloneMasterDetectorProcess::detect, previous);
}


// TODO(benh): Get ZooKeeper timeout from configuration.
// TODO(xujyan): Use peer constructor after switching to C++ 11.
ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    const URL& url)
  : group(new Group(url.servers,
                    MASTER_DETECTOR_ZK_SESSION_TIMEOUT,
                    url.path,
                    url.authentication)),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::ZooKeeperMasterDetectorProcess(
    Owned<Group> _group)
  : group(_group),
    detector(group.get()),
    leader(None()) {}


ZooKeeperMasterDetectorProcess::~ZooKeeperMasterDetectorProcess()
{
  foreach (Promise<Option<UPID> >* promise, promises) {
    promise->future().discard();
    delete promise;
  }
  promises.clear();
}


void ZooKeeperMasterDetectorProcess::initialize()
{
  detector.detect()
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


Future<Option<UPID> > ZooKeeperMasterDetectorProcess::detect(
    const Option<UPID>& previous)
{
  // Return immediately if the detector is no longer operational due
  // to a non-retryable error.
  if (error.isSome()) {
    return Failure(error.get().message);
  }

  if (leader != previous) {
    return leader;
  }

  Promise<Option<UPID> >* promise = new Promise<Option<UPID> >();
  promises.insert(promise);
  return promise->future();
}


void ZooKeeperMasterDetectorProcess::detected(
    const Future<Option<Group::Membership> >& _leader)
{
  CHECK(!_leader.isDiscarded());

  if (_leader.isFailed()) {
    LOG(ERROR) << "Failed to detect the leader: " << _leader.failure();

    // Setting this error stops the detection loop and the detector
    // transitions to an erroneous state. Further calls to detect()
    // will directly fail as a result.
    error = Error(_leader.failure());
    leader = None();
    foreach (Promise<Option<UPID> >* promise, promises) {
      promise->fail(_leader.failure());
      delete promise;
    }
    promises.clear();
    return;
  }

  if (_leader.get().isNone()) {
    leader = None();

    foreach (Promise<Option<UPID> >* promise, promises) {
      promise->set(leader);
      delete promise;
    }
    promises.clear();
  } else {
    // Fetch the data associated with the leader.
    group->data(_leader.get().get())
      .onAny(defer(self(), &Self::fetched, lambda::_1));
  }

  // Keep trying to detect leadership changes.
  detector.detect(_leader.get())
    .onAny(defer(self(), &Self::detected, lambda::_1));
}


void ZooKeeperMasterDetectorProcess::fetched(const Future<string>& data)
{
  CHECK(!data.isDiscarded());

  if (data.isFailed()) {
    leader = None();
    foreach (Promise<Option<UPID> >* promise, promises) {
      promise->fail(data.failure());
      delete promise;
    }
    promises.clear();
    return;
  }

  // Cache the master for subsequent requests.
  leader = UPID(data.get());
  LOG(INFO) << "A new leading master (UPID=" << leader.get() << ") is detected";

  foreach (Promise<Option<UPID> >* promise, promises) {
    promise->set(leader);
    delete promise;
  }
  promises.clear();
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(const URL& url)
{
  process = new ZooKeeperMasterDetectorProcess(url);
  spawn(process);
}


ZooKeeperMasterDetector::ZooKeeperMasterDetector(Owned<Group> group)
{
  process = new ZooKeeperMasterDetectorProcess(group);
  spawn(process);
}


ZooKeeperMasterDetector::~ZooKeeperMasterDetector()
{
  terminate(process);
  process::wait(process);
  delete process;
}


Future<Option<UPID> > ZooKeeperMasterDetector::detect(
    const Option<UPID>& previous)
{
  return dispatch(process, &ZooKeeperMasterDetectorProcess::detect, previous);
}

} // namespace internal {
} // namespace mesos {
