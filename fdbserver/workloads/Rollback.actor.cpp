/*
 * Rollback.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbrpc/simulator.h"
#include "fdbserver/MasterInterface.h"
#include "fdbclient/SystemData.h"
#include "fdbserver/ServerDBInfo.h"
#include "flow/actorcompiler.h" // This must be the last #include.

// Choose a random proxy and a random tLog, represented as unclogTlog.
// The workload first clogs network link between the chosen proxy and all tLogs but the unclogTlog;
// While the network is still clogged, the workload kills the proxy and clogs the unclogged tlog's interface.
// Note: The clogged network link's latency will become "clogDuration".
struct RollbackWorkload : TestWorkload {
	bool enableFailures, multiple, enabled;
	double meanDelay, clogDuration, testDuration;

	RollbackWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		enabled = !clientId; // only do this on the "first" client
		meanDelay = getOption(options, LiteralStringRef("meanDelay"), 20.0); // Only matters if multiple==true
		clogDuration = getOption(options, LiteralStringRef("clogDuration"), 3.0);
		testDuration = getOption(options, LiteralStringRef("testDuration"), 10.0);
		enableFailures = getOption(options, LiteralStringRef("enableFailures"), false);
		multiple = getOption(options, LiteralStringRef("multiple"), true);
	}

	std::string description() const override { return "RollbackWorkload"; }
	Future<Void> setup(Database const& cx) override { return Void(); }
	Future<Void> start(Database const& cx) override {
		if (g_simulator == g_network && enabled)
			return timeout(reportErrors(rollbackFailureWorker(cx, this, meanDelay), "RollbackFailureWorkerError"),
			               testDuration,
			               Void());
		return Void();
	}
	Future<bool> check(Database const& cx) override { return true; }
	void getMetrics(std::vector<PerfMetric>& m) override {}

	ACTOR Future<Void> simulateFailure(Database cx, RollbackWorkload* self) {
		state ServerDBInfo system = self->dbInfo->get();
		auto tlogs = system.logSystemConfig.allPresentLogs();

		if (tlogs.empty() || system.client.commitProxies.empty()) {
			TraceEvent(SevInfo, "UnableToTriggerRollback").detail("Reason", "No tlogs in System Map");
			return Void();
		}

		state CommitProxyInterface proxy = deterministicRandom()->randomChoice(system.client.commitProxies);

		int utIndex = deterministicRandom()->randomInt(0, tlogs.size());
		state NetworkAddress uncloggedTLog = tlogs[utIndex].address();

		for (int t = 0; t < tlogs.size(); t++)
			if (t != utIndex)
				if (tlogs[t].address().ip == proxy.address().ip) {
					TraceEvent(SevInfo, "UnableToTriggerRollback").detail("Reason", "proxy-clogged tLog shared IPs");
					return Void();
				}

		TraceEvent("AttemptingToTriggerRollback")
		    .detail("CommitProxy", proxy.address())
		    .detail("UncloggedTLog", uncloggedTLog);

		for (int t = 0; t < tlogs.size(); t++) {
			if (t != utIndex) {
				g_simulator->clogPair(proxy.address().ip, tlogs[t].address().ip, self->clogDuration);
				// g_simulator->clogInterface( g_simulator->getProcess( system.tlogs[t].commit.getEndpoint() ),
				// self->clogDuration, ClogAll );
			}
		}

		// While the clogged machines are still clogged...
		wait(delay(self->clogDuration / 3));
		system = self->dbInfo->get();

		// Kill the proxy and clog the unclogged tlog
		if (self->enableFailures) {
			g_simulator->killProcess(g_simulator->getProcessByAddress(proxy.address()), ISimulator::KillInstantly);
			g_simulator->clogInterface(uncloggedTLog.ip, self->clogDuration, ClogAll);
		} else {
			g_simulator->clogInterface(proxy.address().ip, self->clogDuration, ClogAll);
			g_simulator->clogInterface(uncloggedTLog.ip, self->clogDuration, ClogAll);
		}
		return Void();
	}

	ACTOR Future<Void> rollbackFailureWorker(Database cx, RollbackWorkload* self, double delay) {
		state PromiseStream<Void> events;
		if (self->multiple) {
			state double lastTime = now();
			loop {
				wait(poisson(&lastTime, delay));
				wait(self->simulateFailure(cx, self));
			}
		} else {
			wait(::delay(deterministicRandom()->random01() *
			             std::max(0.0, self->testDuration - self->clogDuration * 13.0)));
			wait(self->simulateFailure(cx, self));
		}
		return Void();
	}
};

WorkloadFactory<RollbackWorkload> RollbackWorkloadFactory("Rollback");
