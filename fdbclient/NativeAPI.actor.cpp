/*
 * NativeAPI.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
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

#include <algorithm>
#include <iterator>
#include <regex>
#include <unordered_set>
#include <tuple>
#include <utility>
#include <vector>

#include "fdbclient/FDBTypes.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/MultiInterface.h"

#include "fdbclient/ActorLineageProfiler.h"
#include "fdbclient/AnnotateActor.h"
#include "fdbclient/Atomic.h"
#include "fdbclient/ClusterInterface.h"
#include "fdbclient/CoordinationInterface.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/GlobalConfig.actor.h"
#include "fdbclient/IKnobCollection.h"
#include "fdbclient/JsonBuilder.h"
#include "fdbclient/KeyBackedTypes.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "fdbclient/NameLineage.h"
#include "fdbclient/CommitProxyInterface.h"
#include "fdbclient/MonitorLeader.h"
#include "fdbclient/MutationList.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbclient/ParallelStream.actor.h"
#include "fdbclient/SpecialKeySpace.actor.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/TransactionLineage.h"
#include "fdbclient/versions.h"
#include "fdbclient/WellKnownEndpoints.h"
#include "fdbrpc/LoadBalance.h"
#include "fdbrpc/Net2FileSystem.h"
#include "fdbrpc/simulator.h"
#include "flow/Arena.h"
#include "flow/ActorCollection.h"
#include "flow/DeterministicRandom.h"
#include "flow/Error.h"
#include "flow/IRandom.h"
#include "flow/flow.h"
#include "flow/genericactors.actor.h"
#include "flow/Knobs.h"
#include "flow/Platform.h"
#include "flow/SystemMonitor.h"
#include "flow/TLSConfig.actor.h"
#include "flow/Tracing.h"
#include "flow/UnitTest.h"
#include "flow/serialize.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#undef min
#undef max
#else
#include <time.h>
#endif
#include "flow/actorcompiler.h" // This must be the last #include.

extern const char* getSourceVersion();

namespace {

TransactionLineageCollector transactionLineageCollector;
NameLineageCollector nameLineageCollector;

template <class Interface, class Request>
Future<REPLY_TYPE(Request)> loadBalance(
    DatabaseContext* ctx,
    const Reference<LocationInfo> alternatives,
    RequestStream<Request> Interface::*channel,
    const Request& request = Request(),
    TaskPriority taskID = TaskPriority::DefaultPromiseEndpoint,
    AtMostOnce atMostOnce =
        AtMostOnce::False, // if true, throws request_maybe_delivered() instead of retrying automatically
    QueueModel* model = nullptr) {
	if (alternatives->hasCaches) {
		return loadBalance(alternatives->locations(), channel, request, taskID, atMostOnce, model);
	}
	return fmap(
	    [ctx](auto const& res) {
		    if (res.cached) {
			    ctx->updateCache.trigger();
		    }
		    return res;
	    },
	    loadBalance(alternatives->locations(), channel, request, taskID, atMostOnce, model));
}
} // namespace

NetworkOptions networkOptions;
TLSConfig tlsConfig(TLSEndpointType::CLIENT);

// The default values, TRACE_DEFAULT_ROLL_SIZE and TRACE_DEFAULT_MAX_LOGS_SIZE are located in Trace.h.
NetworkOptions::NetworkOptions()
  : traceRollSize(TRACE_DEFAULT_ROLL_SIZE), traceMaxLogsSize(TRACE_DEFAULT_MAX_LOGS_SIZE), traceLogGroup("default"),
    traceFormat("xml"), traceClockSource("now"),
    supportedVersions(new ReferencedObject<Standalone<VectorRef<ClientVersionRef>>>()), runLoopProfilingEnabled(false) {
}

static const Key CLIENT_LATENCY_INFO_PREFIX = LiteralStringRef("client_latency/");
static const Key CLIENT_LATENCY_INFO_CTR_PREFIX = LiteralStringRef("client_latency_counter/");

void DatabaseContext::addTssMapping(StorageServerInterface const& ssi, StorageServerInterface const& tssi) {
	auto result = tssMapping.find(ssi.id());
	// Update tss endpoint mapping if ss isn't in mapping, or the interface it mapped to changed
	if (result == tssMapping.end() ||
	    result->second.getValue.getEndpoint().token.first() != tssi.getValue.getEndpoint().token.first()) {
		Reference<TSSMetrics> metrics;
		if (result == tssMapping.end()) {
			// new TSS pairing
			metrics = makeReference<TSSMetrics>();
			tssMetrics[tssi.id()] = metrics;
			tssMapping[ssi.id()] = tssi;
		} else {
			if (result->second.id() == tssi.id()) {
				metrics = tssMetrics[tssi.id()];
			} else {
				TEST(true); // SS now maps to new TSS! This will probably never happen in practice
				tssMetrics.erase(result->second.id());
				metrics = makeReference<TSSMetrics>();
				tssMetrics[tssi.id()] = metrics;
			}
			result->second = tssi;
		}

		// data requests duplicated for load and data comparison
		queueModel.updateTssEndpoint(ssi.getValue.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.getValue.getEndpoint(), metrics));
		queueModel.updateTssEndpoint(ssi.getKey.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.getKey.getEndpoint(), metrics));
		queueModel.updateTssEndpoint(ssi.getKeyValues.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.getKeyValues.getEndpoint(), metrics));
		queueModel.updateTssEndpoint(ssi.getKeyValuesStream.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.getKeyValuesStream.getEndpoint(), metrics));

		// non-data requests duplicated for load
		queueModel.updateTssEndpoint(ssi.watchValue.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.watchValue.getEndpoint(), metrics));
		queueModel.updateTssEndpoint(ssi.splitMetrics.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.splitMetrics.getEndpoint(), metrics));
		queueModel.updateTssEndpoint(ssi.getReadHotRanges.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.getReadHotRanges.getEndpoint(), metrics));
		queueModel.updateTssEndpoint(ssi.getRangeSplitPoints.getEndpoint().token.first(),
		                             TSSEndpointData(tssi.id(), tssi.getRangeSplitPoints.getEndpoint(), metrics));
	}
}

void DatabaseContext::removeTssMapping(StorageServerInterface const& ssi) {
	auto result = tssMapping.find(ssi.id());
	if (result != tssMapping.end()) {
		tssMetrics.erase(ssi.id());
		tssMapping.erase(result);
		queueModel.removeTssEndpoint(ssi.getValue.getEndpoint().token.first());
		queueModel.removeTssEndpoint(ssi.getKey.getEndpoint().token.first());
		queueModel.removeTssEndpoint(ssi.getKeyValues.getEndpoint().token.first());
		queueModel.removeTssEndpoint(ssi.getKeyValuesStream.getEndpoint().token.first());

		queueModel.removeTssEndpoint(ssi.watchValue.getEndpoint().token.first());
		queueModel.removeTssEndpoint(ssi.splitMetrics.getEndpoint().token.first());
		queueModel.removeTssEndpoint(ssi.getReadHotRanges.getEndpoint().token.first());
		queueModel.removeTssEndpoint(ssi.getRangeSplitPoints.getEndpoint().token.first());
	}
}

Reference<StorageServerInfo> StorageServerInfo::getInterface(DatabaseContext* cx,
                                                             StorageServerInterface const& ssi,
                                                             LocalityData const& locality) {
	auto it = cx->server_interf.find(ssi.id());
	if (it != cx->server_interf.end()) {
		if (it->second->interf.getValue.getEndpoint().token != ssi.getValue.getEndpoint().token) {
			if (it->second->interf.locality == ssi.locality) {
				// FIXME: load balance holds pointers to individual members of the interface, and this assignment will
				// swap out the object they are
				//       pointing to. This is technically correct, but is very unnatural. We may want to refactor load
				//       balance to take an AsyncVar<Reference<Interface>> so that it is notified when the interface
				//       changes.

				it->second->interf = ssi;
			} else {
				it->second->notifyContextDestroyed();
				Reference<StorageServerInfo> loc(new StorageServerInfo(cx, ssi, locality));
				cx->server_interf[ssi.id()] = loc.getPtr();
				return loc;
			}
		}

		return Reference<StorageServerInfo>::addRef(it->second);
	}

	Reference<StorageServerInfo> loc(new StorageServerInfo(cx, ssi, locality));
	cx->server_interf[ssi.id()] = loc.getPtr();
	return loc;
}

void StorageServerInfo::notifyContextDestroyed() {
	cx = nullptr;
}

StorageServerInfo::~StorageServerInfo() {
	if (cx) {
		auto it = cx->server_interf.find(interf.id());
		if (it != cx->server_interf.end())
			cx->server_interf.erase(it);
		cx = nullptr;
	}
}

std::string printable(const VectorRef<KeyValueRef>& val) {
	std::string s;
	for (int i = 0; i < val.size(); i++)
		s = s + printable(val[i].key) + format(":%d ", val[i].value.size());
	return s;
}

std::string printable(const KeyValueRef& val) {
	return printable(val.key) + format(":%d ", val.value.size());
}

std::string printable(const VectorRef<StringRef>& val) {
	std::string s;
	for (int i = 0; i < val.size(); i++)
		s = s + printable(val[i]) + " ";
	return s;
}

std::string printable(const StringRef& val) {
	return val.printable();
}

std::string printable(const std::string& str) {
	return StringRef(str).printable();
}

std::string printable(const KeyRangeRef& range) {
	return printable(range.begin) + " - " + printable(range.end);
}

std::string printable(const VectorRef<KeyRangeRef>& val) {
	std::string s;
	for (int i = 0; i < val.size(); i++)
		s = s + printable(val[i]) + " ";
	return s;
}

int unhex(char c) {
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	UNREACHABLE();
}

std::string unprintable(std::string const& val) {
	std::string s;
	for (int i = 0; i < val.size(); i++) {
		char c = val[i];
		if (c == '\\') {
			if (++i == val.size())
				ASSERT(false);
			if (val[i] == '\\') {
				s += '\\';
			} else if (val[i] == 'x') {
				if (i + 2 >= val.size())
					ASSERT(false);
				s += char((unhex(val[i + 1]) << 4) + unhex(val[i + 2]));
				i += 2;
			} else
				ASSERT(false);
		} else
			s += c;
	}
	return s;
}

void DatabaseContext::validateVersion(Version version) const {
	// Version could be 0 if the INITIALIZE_NEW_DATABASE option is set. In that case, it is illegal to perform any
	// reads. We throw client_invalid_operation because the caller didn't directly set the version, so the
	// version_invalid error might be confusing.
	if (version == 0) {
		throw client_invalid_operation();
	}
	if (switchable && version < minAcceptableReadVersion) {
		TEST(true); // Attempted to read a version lower than any this client has seen from the current cluster
		throw transaction_too_old();
	}

	ASSERT(version > 0 || version == latestVersion);
}

void validateOptionValuePresent(Optional<StringRef> value) {
	if (!value.present()) {
		throw invalid_option_value();
	}
}

void validateOptionValueNotPresent(Optional<StringRef> value) {
	if (value.present() && value.get().size() > 0) {
		throw invalid_option_value();
	}
}

void dumpMutations(const MutationListRef& mutations) {
	for (auto m = mutations.begin(); m; ++m) {
		switch (m->type) {
		case MutationRef::SetValue:
			printf("  '%s' := '%s'\n", printable(m->param1).c_str(), printable(m->param2).c_str());
			break;
		case MutationRef::AddValue:
			printf("  '%s' += '%s'", printable(m->param1).c_str(), printable(m->param2).c_str());
			break;
		case MutationRef::ClearRange:
			printf("  Clear ['%s','%s')\n", printable(m->param1).c_str(), printable(m->param2).c_str());
			break;
		default:
			printf("  Unknown mutation %d('%s','%s')\n",
			       m->type,
			       printable(m->param1).c_str(),
			       printable(m->param2).c_str());
			break;
		}
	}
}

template <>
void addref(DatabaseContext* ptr) {
	ptr->addref();
}
template <>
void delref(DatabaseContext* ptr) {
	ptr->delref();
}

void traceTSSErrors(const char* name, UID tssId, const std::unordered_map<int, uint64_t>& errorsByCode) {
	TraceEvent ev(name, tssId);
	for (auto& it : errorsByCode) {
		ev.detail("E" + std::to_string(it.first), it.second);
	}
}

ACTOR Future<Void> databaseLogger(DatabaseContext* cx) {
	state double lastLogged = 0;
	loop {
		wait(delay(CLIENT_KNOBS->SYSTEM_MONITOR_INTERVAL, TaskPriority::FlushTrace));

		TraceEvent ev("TransactionMetrics", cx->dbId);

		ev.detail("Elapsed", (lastLogged == 0) ? 0 : now() - lastLogged)
		    .detail("Cluster",
		            cx->getConnectionFile() ? cx->getConnectionFile()->getConnectionString().clusterKeyName().toString()
		                                    : "")
		    .detail("Internal", cx->internal);

		cx->cc.logToTraceEvent(ev);

		ev.detail("LocationCacheEntryCount", cx->locationCache.size());
		ev.detail("MeanLatency", cx->latencies.mean())
		    .detail("MedianLatency", cx->latencies.median())
		    .detail("Latency90", cx->latencies.percentile(0.90))
		    .detail("Latency98", cx->latencies.percentile(0.98))
		    .detail("MaxLatency", cx->latencies.max())
		    .detail("MeanRowReadLatency", cx->readLatencies.mean())
		    .detail("MedianRowReadLatency", cx->readLatencies.median())
		    .detail("MaxRowReadLatency", cx->readLatencies.max())
		    .detail("MeanGRVLatency", cx->GRVLatencies.mean())
		    .detail("MedianGRVLatency", cx->GRVLatencies.median())
		    .detail("MaxGRVLatency", cx->GRVLatencies.max())
		    .detail("MeanCommitLatency", cx->commitLatencies.mean())
		    .detail("MedianCommitLatency", cx->commitLatencies.median())
		    .detail("MaxCommitLatency", cx->commitLatencies.max())
		    .detail("MeanMutationsPerCommit", cx->mutationsPerCommit.mean())
		    .detail("MedianMutationsPerCommit", cx->mutationsPerCommit.median())
		    .detail("MaxMutationsPerCommit", cx->mutationsPerCommit.max())
		    .detail("MeanBytesPerCommit", cx->bytesPerCommit.mean())
		    .detail("MedianBytesPerCommit", cx->bytesPerCommit.median())
		    .detail("MaxBytesPerCommit", cx->bytesPerCommit.max());

		cx->latencies.clear();
		cx->readLatencies.clear();
		cx->GRVLatencies.clear();
		cx->commitLatencies.clear();
		cx->mutationsPerCommit.clear();
		cx->bytesPerCommit.clear();

		for (const auto& it : cx->tssMetrics) {
			// TODO could skip this whole thing if tss if request counter is zero?
			// That would potentially complicate elapsed calculation though
			if (it.second->mismatches.getIntervalDelta()) {
				cx->tssMismatchStream.send(
				    std::pair<UID, std::vector<DetailedTSSMismatch>>(it.first, it.second->detailedMismatches));
			}

			// do error histograms as separate event
			if (it.second->ssErrorsByCode.size()) {
				traceTSSErrors("TSS_SSErrors", it.first, it.second->ssErrorsByCode);
			}

			if (it.second->tssErrorsByCode.size()) {
				traceTSSErrors("TSS_TSSErrors", it.first, it.second->tssErrorsByCode);
			}

			TraceEvent tssEv("TSSClientMetrics", cx->dbId);
			tssEv.detail("TSSID", it.first)
			    .detail("Elapsed", (lastLogged == 0) ? 0 : now() - lastLogged)
			    .detail("Internal", cx->internal);

			it.second->cc.logToTraceEvent(tssEv);

			tssEv.detail("MeanSSGetValueLatency", it.second->SSgetValueLatency.mean())
			    .detail("MedianSSGetValueLatency", it.second->SSgetValueLatency.median())
			    .detail("SSGetValueLatency90", it.second->SSgetValueLatency.percentile(0.90))
			    .detail("SSGetValueLatency99", it.second->SSgetValueLatency.percentile(0.99));

			tssEv.detail("MeanTSSGetValueLatency", it.second->TSSgetValueLatency.mean())
			    .detail("MedianTSSGetValueLatency", it.second->TSSgetValueLatency.median())
			    .detail("TSSGetValueLatency90", it.second->TSSgetValueLatency.percentile(0.90))
			    .detail("TSSGetValueLatency99", it.second->TSSgetValueLatency.percentile(0.99));

			tssEv.detail("MeanSSGetKeyLatency", it.second->SSgetKeyLatency.mean())
			    .detail("MedianSSGetKeyLatency", it.second->SSgetKeyLatency.median())
			    .detail("SSGetKeyLatency90", it.second->SSgetKeyLatency.percentile(0.90))
			    .detail("SSGetKeyLatency99", it.second->SSgetKeyLatency.percentile(0.99));

			tssEv.detail("MeanTSSGetKeyLatency", it.second->TSSgetKeyLatency.mean())
			    .detail("MedianTSSGetKeyLatency", it.second->TSSgetKeyLatency.median())
			    .detail("TSSGetKeyLatency90", it.second->TSSgetKeyLatency.percentile(0.90))
			    .detail("TSSGetKeyLatency99", it.second->TSSgetKeyLatency.percentile(0.99));

			tssEv.detail("MeanSSGetKeyValuesLatency", it.second->SSgetKeyValuesLatency.mean())
			    .detail("MedianSSGetKeyValuesLatency", it.second->SSgetKeyValuesLatency.median())
			    .detail("SSGetKeyValuesLatency90", it.second->SSgetKeyValuesLatency.percentile(0.90))
			    .detail("SSGetKeyValuesLatency99", it.second->SSgetKeyValuesLatency.percentile(0.99));

			tssEv.detail("MeanTSSGetKeyValuesLatency", it.second->TSSgetKeyValuesLatency.mean())
			    .detail("MedianTSSGetKeyValuesLatency", it.second->TSSgetKeyValuesLatency.median())
			    .detail("TSSGetKeyValuesLatency90", it.second->TSSgetKeyValuesLatency.percentile(0.90))
			    .detail("TSSGetKeyValuesLatency99", it.second->TSSgetKeyValuesLatency.percentile(0.99));

			it.second->clear();
		}

		lastLogged = now();
	}
}

struct TrInfoChunk {
	ValueRef value;
	Key key;
};

ACTOR static Future<Void> transactionInfoCommitActor(Transaction* tr, std::vector<TrInfoChunk>* chunks) {
	state const Key clientLatencyAtomicCtr = CLIENT_LATENCY_INFO_CTR_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin);
	state int retryCount = 0;
	loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			state Future<Standalone<StringRef>> vstamp = tr->getVersionstamp();
			int64_t numCommitBytes = 0;
			for (auto& chunk : *chunks) {
				tr->atomicOp(chunk.key, chunk.value, MutationRef::SetVersionstampedKey);
				numCommitBytes += chunk.key.size() + chunk.value.size() -
				                  4; // subtract number of bytes of key that denotes verstion stamp index
			}
			tr->atomicOp(clientLatencyAtomicCtr, StringRef((uint8_t*)&numCommitBytes, 8), MutationRef::AddValue);
			wait(tr->commit());
			return Void();
		} catch (Error& e) {
			retryCount++;
			if (retryCount == 10)
				throw;
			wait(tr->onError(e));
		}
	}
}

ACTOR static Future<Void> delExcessClntTxnEntriesActor(Transaction* tr, int64_t clientTxInfoSizeLimit) {
	state const Key clientLatencyName = CLIENT_LATENCY_INFO_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin);
	state const Key clientLatencyAtomicCtr = CLIENT_LATENCY_INFO_CTR_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin);
	TraceEvent(SevInfo, "DelExcessClntTxnEntriesCalled").log();
	loop {
		try {
			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::LOCK_AWARE);
			Optional<Value> ctrValue = wait(tr->get(KeyRef(clientLatencyAtomicCtr), Snapshot::True));
			if (!ctrValue.present()) {
				TraceEvent(SevInfo, "NumClntTxnEntriesNotFound").log();
				return Void();
			}
			state int64_t txInfoSize = 0;
			ASSERT(ctrValue.get().size() == sizeof(int64_t));
			memcpy(&txInfoSize, ctrValue.get().begin(), ctrValue.get().size());
			if (txInfoSize < clientTxInfoSizeLimit)
				return Void();
			int getRangeByteLimit = (txInfoSize - clientTxInfoSizeLimit) < CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT
			                            ? (txInfoSize - clientTxInfoSizeLimit)
			                            : CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT;
			GetRangeLimits limit(GetRangeLimits::ROW_LIMIT_UNLIMITED, getRangeByteLimit);
			RangeResult txEntries =
			    wait(tr->getRange(KeyRangeRef(clientLatencyName, strinc(clientLatencyName)), limit));
			state int64_t numBytesToDel = 0;
			KeyRef endKey;
			for (auto& kv : txEntries) {
				endKey = kv.key;
				numBytesToDel += kv.key.size() + kv.value.size();
				if (txInfoSize - numBytesToDel <= clientTxInfoSizeLimit)
					break;
			}
			if (numBytesToDel) {
				tr->clear(KeyRangeRef(txEntries[0].key, strinc(endKey)));
				TraceEvent(SevInfo, "DeletingExcessCntTxnEntries").detail("BytesToBeDeleted", numBytesToDel);
				int64_t bytesDel = -numBytesToDel;
				tr->atomicOp(clientLatencyAtomicCtr, StringRef((uint8_t*)&bytesDel, 8), MutationRef::AddValue);
				wait(tr->commit());
			}
			if (txInfoSize - numBytesToDel <= clientTxInfoSizeLimit)
				return Void();
		} catch (Error& e) {
			wait(tr->onError(e));
		}
	}
}

// Delref and addref self to give self a chance to get destroyed.
ACTOR static Future<Void> refreshTransaction(DatabaseContext* self, Transaction* tr) {
	*tr = Transaction();
	wait(delay(0)); // Give ourselves the chance to get cancelled if self was destroyed
	*tr = Transaction(Database(Reference<DatabaseContext>::addRef(self)));
	return Void();
}

// The reason for getting a pointer to DatabaseContext instead of a reference counted object is because reference
// counting will increment reference count for DatabaseContext which holds the future of this actor. This creates a
// cyclic reference and hence this actor and Database object will not be destroyed at all.
ACTOR static Future<Void> clientStatusUpdateActor(DatabaseContext* cx) {
	state const std::string clientLatencyName =
	    CLIENT_LATENCY_INFO_PREFIX.withPrefix(fdbClientInfoPrefixRange.begin).toString();
	state Transaction tr;
	state std::vector<TrInfoChunk> commitQ;
	state int txBytes = 0;

	loop {
		// Need to make sure that we eventually destroy tr. We can't rely on getting cancelled to do this because of
		// the cyclic reference to self.
		wait(refreshTransaction(cx, &tr));
		try {
			ASSERT(cx->clientStatusUpdater.outStatusQ.empty());
			cx->clientStatusUpdater.inStatusQ.swap(cx->clientStatusUpdater.outStatusQ);
			// Split Transaction Info into chunks
			state std::vector<TrInfoChunk> trChunksQ;
			for (auto& entry : cx->clientStatusUpdater.outStatusQ) {
				auto& bw = entry.second;
				int64_t value_size_limit = BUGGIFY
				                               ? deterministicRandom()->randomInt(1e3, CLIENT_KNOBS->VALUE_SIZE_LIMIT)
				                               : CLIENT_KNOBS->VALUE_SIZE_LIMIT;
				int num_chunks = (bw.getLength() + value_size_limit - 1) / value_size_limit;
				std::string random_id = deterministicRandom()->randomAlphaNumeric(16);
				std::string user_provided_id = entry.first.size() ? entry.first + "/" : "";
				for (int i = 0; i < num_chunks; i++) {
					TrInfoChunk chunk;
					BinaryWriter chunkBW(Unversioned());
					chunkBW << bigEndian32(i + 1) << bigEndian32(num_chunks);
					chunk.key = KeyRef(clientLatencyName + std::string(10, '\x00') + "/" + random_id + "/" +
					                   chunkBW.toValue().toString() + "/" + user_provided_id + std::string(4, '\x00'));
					int32_t pos = littleEndian32(clientLatencyName.size());
					memcpy(mutateString(chunk.key) + chunk.key.size() - sizeof(int32_t), &pos, sizeof(int32_t));
					if (i == num_chunks - 1) {
						chunk.value = ValueRef(static_cast<uint8_t*>(bw.getData()) + (i * value_size_limit),
						                       bw.getLength() - (i * value_size_limit));
					} else {
						chunk.value =
						    ValueRef(static_cast<uint8_t*>(bw.getData()) + (i * value_size_limit), value_size_limit);
					}
					trChunksQ.push_back(std::move(chunk));
				}
			}

			// Commit the chunks splitting into different transactions if needed
			state int64_t dataSizeLimit =
			    BUGGIFY ? deterministicRandom()->randomInt(200e3, 1.5 * CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT)
			            : 0.8 * CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT;
			state std::vector<TrInfoChunk>::iterator tracking_iter = trChunksQ.begin();
			ASSERT(commitQ.empty() && (txBytes == 0));
			loop {
				state std::vector<TrInfoChunk>::iterator iter = tracking_iter;
				txBytes = 0;
				commitQ.clear();
				try {
					while (iter != trChunksQ.end()) {
						if (iter->value.size() + iter->key.size() + txBytes > dataSizeLimit) {
							wait(transactionInfoCommitActor(&tr, &commitQ));
							tracking_iter = iter;
							commitQ.clear();
							txBytes = 0;
						}
						commitQ.push_back(*iter);
						txBytes += iter->value.size() + iter->key.size();
						++iter;
					}
					if (!commitQ.empty()) {
						wait(transactionInfoCommitActor(&tr, &commitQ));
						commitQ.clear();
						txBytes = 0;
					}
					break;
				} catch (Error& e) {
					if (e.code() == error_code_transaction_too_large) {
						dataSizeLimit /= 2;
						ASSERT(dataSizeLimit >= CLIENT_KNOBS->VALUE_SIZE_LIMIT + CLIENT_KNOBS->KEY_SIZE_LIMIT);
					} else {
						TraceEvent(SevWarnAlways, "ClientTrInfoErrorCommit").error(e).detail("TxBytes", txBytes);
						commitQ.clear();
						txBytes = 0;
						throw;
					}
				}
			}
			cx->clientStatusUpdater.outStatusQ.clear();
			wait(GlobalConfig::globalConfig().onInitialized());
			double sampleRate = GlobalConfig::globalConfig().get<double>(fdbClientInfoTxnSampleRate,
			                                                             std::numeric_limits<double>::infinity());
			double clientSamplingProbability =
			    std::isinf(sampleRate) ? CLIENT_KNOBS->CSI_SAMPLING_PROBABILITY : sampleRate;
			int64_t sizeLimit = GlobalConfig::globalConfig().get<int64_t>(fdbClientInfoTxnSizeLimit, -1);
			int64_t clientTxnInfoSizeLimit = sizeLimit == -1 ? CLIENT_KNOBS->CSI_SIZE_LIMIT : sizeLimit;
			if (!trChunksQ.empty() && deterministicRandom()->random01() < clientSamplingProbability)
				wait(delExcessClntTxnEntriesActor(&tr, clientTxnInfoSizeLimit));

			wait(delay(CLIENT_KNOBS->CSI_STATUS_DELAY));
		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				throw;
			}
			cx->clientStatusUpdater.outStatusQ.clear();
			TraceEvent(SevWarnAlways, "UnableToWriteClientStatus").error(e);
			wait(delay(10.0));
		}
	}
}

ACTOR Future<Void> assertFailure(GrvProxyInterface remote, Future<ErrorOr<GetReadVersionReply>> reply) {
	try {
		ErrorOr<GetReadVersionReply> res = wait(reply);
		if (!res.isError()) {
			TraceEvent(SevError, "GotStaleReadVersion")
			    .detail("Remote", remote.getConsistentReadVersion.getEndpoint().addresses.address.toString())
			    .detail("Provisional", remote.provisional)
			    .detail("ReadVersion", res.get().version);
			ASSERT_WE_THINK(false);
		}
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			throw;
		}
		// we want this to fail -- so getting here is good, we'll just ignore the error.
	}
	return Void();
}

Future<Void> attemptGRVFromOldProxies(std::vector<GrvProxyInterface> oldProxies,
                                      std::vector<GrvProxyInterface> newProxies) {
	Span span(deterministicRandom()->randomUniqueID(), "VerifyCausalReadRisky"_loc);
	std::vector<Future<Void>> replies;
	replies.reserve(oldProxies.size());
	GetReadVersionRequest req(
	    span.context, 1, TransactionPriority::IMMEDIATE, GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY);
	TraceEvent evt("AttemptGRVFromOldProxies");
	evt.detail("NumOldProxies", oldProxies.size()).detail("NumNewProxies", newProxies.size());
	auto traceProxies = [&](std::vector<GrvProxyInterface>& proxies, std::string const& key) {
		for (int i = 0; i < proxies.size(); ++i) {
			auto k = key + std::to_string(i);
			evt.detail(k.c_str(), proxies[i].id());
		}
	};
	traceProxies(oldProxies, "OldProxy"s);
	traceProxies(newProxies, "NewProxy"s);
	evt.log();
	for (auto& i : oldProxies) {
		req.reply = ReplyPromise<GetReadVersionReply>();
		replies.push_back(assertFailure(i, i.getConsistentReadVersion.tryGetReply(req)));
	}
	return waitForAll(replies);
}

ACTOR static Future<Void> monitorProxiesChange(DatabaseContext* cx,
                                               Reference<AsyncVar<ClientDBInfo> const> clientDBInfo,
                                               AsyncTrigger* triggerVar) {
	state std::vector<CommitProxyInterface> curCommitProxies;
	state std::vector<GrvProxyInterface> curGrvProxies;
	state ActorCollection actors(false);
	curCommitProxies = clientDBInfo->get().commitProxies;
	curGrvProxies = clientDBInfo->get().grvProxies;

	loop {
		choose {
			when(wait(clientDBInfo->onChange())) {
				if (clientDBInfo->get().commitProxies != curCommitProxies ||
				    clientDBInfo->get().grvProxies != curGrvProxies) {
					// This condition is a bit complicated. Here we want to verify that we're unable to receive a read
					// version from a proxy of an old generation after a successful recovery. The conditions are:
					// 1. We only do this with a configured probability.
					// 2. If the old set of Grv proxies is empty, there's nothing to do
					// 3. If the new set of Grv proxies is empty, it means the recovery is not complete. So if an old
					//    Grv proxy still gives out read versions, this would be correct behavior.
					// 4. If we see a provisional proxy, it means the recovery didn't complete yet, so the same as (3)
					//    applies.
					if (deterministicRandom()->random01() < cx->verifyCausalReadsProp && !curGrvProxies.empty() &&
					    !clientDBInfo->get().grvProxies.empty() && !clientDBInfo->get().grvProxies[0].provisional) {
						actors.add(attemptGRVFromOldProxies(curGrvProxies, clientDBInfo->get().grvProxies));
					}
					curCommitProxies = clientDBInfo->get().commitProxies;
					curGrvProxies = clientDBInfo->get().grvProxies;
					triggerVar->trigger();
				}
			}
			when(wait(actors.getResult())) { UNSTOPPABLE_ASSERT(false); }
		}
	}
}

void updateLocationCacheWithCaches(DatabaseContext* self,
                                   const std::map<UID, StorageServerInterface>& removed,
                                   const std::map<UID, StorageServerInterface>& added) {
	// TODO: this needs to be more clever in the future
	auto ranges = self->locationCache.ranges();
	for (auto iter = ranges.begin(); iter != ranges.end(); ++iter) {
		if (iter->value() && iter->value()->hasCaches) {
			auto& val = iter->value();
			std::vector<Reference<ReferencedInterface<StorageServerInterface>>> interfaces;
			interfaces.reserve(val->size() - removed.size() + added.size());
			for (int i = 0; i < val->size(); ++i) {
				const auto& interf = (*val)[i];
				if (removed.count(interf->interf.id()) == 0) {
					interfaces.emplace_back(interf);
				}
			}
			for (const auto& p : added) {
				interfaces.push_back(makeReference<ReferencedInterface<StorageServerInterface>>(p.second));
			}
			iter->value() = makeReference<LocationInfo>(interfaces, true);
		}
	}
}

Reference<LocationInfo> addCaches(const Reference<LocationInfo>& loc,
                                  const std::vector<Reference<ReferencedInterface<StorageServerInterface>>>& other) {
	std::vector<Reference<ReferencedInterface<StorageServerInterface>>> interfaces;
	interfaces.reserve(loc->size() + other.size());
	for (int i = 0; i < loc->size(); ++i) {
		interfaces.emplace_back((*loc)[i]);
	}
	interfaces.insert(interfaces.end(), other.begin(), other.end());
	return makeReference<LocationInfo>(interfaces, true);
}

ACTOR Future<Void> updateCachedRanges(DatabaseContext* self, std::map<UID, StorageServerInterface>* cacheServers) {
	state Transaction tr;
	state Value trueValue = storageCacheValue(std::vector<uint16_t>{ 0 });
	state Value falseValue = storageCacheValue(std::vector<uint16_t>{});
	try {
		loop {
			// Need to make sure that we eventually destroy tr. We can't rely on getting cancelled to do this because of
			// the cyclic reference to self.
			tr = Transaction();
			wait(delay(0)); // Give ourselves the chance to get cancelled if self was destroyed
			wait(brokenPromiseToNever(self->updateCache.onTrigger())); // brokenPromiseToNever because self might get
			                                                           // destroyed elsewhere while we're waiting here.
			tr = Transaction(Database(Reference<DatabaseContext>::addRef(self)));
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::READ_LOCK_AWARE);
			try {
				RangeResult range = wait(tr.getRange(storageCacheKeys, CLIENT_KNOBS->TOO_MANY));
				ASSERT(!range.more);
				std::vector<Reference<ReferencedInterface<StorageServerInterface>>> cacheInterfaces;
				cacheInterfaces.reserve(cacheServers->size());
				for (const auto& p : *cacheServers) {
					cacheInterfaces.push_back(makeReference<ReferencedInterface<StorageServerInterface>>(p.second));
				}
				bool currCached = false;
				KeyRef begin, end;
				for (const auto& kv : range) {
					// These booleans have to flip consistently
					ASSERT(currCached == (kv.value == falseValue));
					if (kv.value == trueValue) {
						begin = kv.key.substr(storageCacheKeys.begin.size());
						currCached = true;
					} else {
						currCached = false;
						end = kv.key.substr(storageCacheKeys.begin.size());
						KeyRangeRef cachedRange{ begin, end };
						auto ranges = self->locationCache.containedRanges(cachedRange);
						KeyRef containedRangesBegin, containedRangesEnd, prevKey;
						if (!ranges.empty()) {
							containedRangesBegin = ranges.begin().range().begin;
						}
						for (auto iter = ranges.begin(); iter != ranges.end(); ++iter) {
							containedRangesEnd = iter->range().end;
							if (iter->value() && !iter->value()->hasCaches) {
								iter->value() = addCaches(iter->value(), cacheInterfaces);
							}
						}
						auto iter = self->locationCache.rangeContaining(begin);
						if (iter->value() && !iter->value()->hasCaches) {
							if (end >= iter->range().end) {
								Key endCopy = iter->range().end; // Copy because insertion invalidates iterator
								self->locationCache.insert(KeyRangeRef{ begin, endCopy },
								                           addCaches(iter->value(), cacheInterfaces));
							} else {
								self->locationCache.insert(KeyRangeRef{ begin, end },
								                           addCaches(iter->value(), cacheInterfaces));
							}
						}
						iter = self->locationCache.rangeContainingKeyBefore(end);
						if (iter->value() && !iter->value()->hasCaches) {
							Key beginCopy = iter->range().begin; // Copy because insertion invalidates iterator
							self->locationCache.insert(KeyRangeRef{ beginCopy, end },
							                           addCaches(iter->value(), cacheInterfaces));
						}
					}
				}
				wait(delay(2.0)); // we want to wait at least some small amount of time before
				// updating this list again
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	} catch (Error& e) {
		TraceEvent(SevError, "UpdateCachedRangesFailed").error(e);
		throw;
	}
}

// The reason for getting a pointer to DatabaseContext instead of a reference counted object is because reference
// counting will increment reference count for DatabaseContext which holds the future of this actor. This creates a
// cyclic reference and hence this actor and Database object will not be destroyed at all.
ACTOR Future<Void> monitorCacheList(DatabaseContext* self) {
	state Transaction tr;
	state std::map<UID, StorageServerInterface> cacheServerMap;
	state Future<Void> updateRanges = updateCachedRanges(self, &cacheServerMap);
	// if no caches are configured, we don't want to run this actor at all
	// so we just wait for the first trigger from a storage server
	wait(self->updateCache.onTrigger());
	try {
		loop {
			// Need to make sure that we eventually destroy tr. We can't rely on getting cancelled to do this because of
			// the cyclic reference to self.
			wait(refreshTransaction(self, &tr));
			try {
				RangeResult cacheList = wait(tr.getRange(storageCacheServerKeys, CLIENT_KNOBS->TOO_MANY));
				ASSERT(!cacheList.more);
				bool hasChanges = false;
				std::map<UID, StorageServerInterface> allCacheServers;
				for (auto kv : cacheList) {
					auto ssi = BinaryReader::fromStringRef<StorageServerInterface>(kv.value, IncludeVersion());
					allCacheServers.emplace(ssi.id(), ssi);
				}
				std::map<UID, StorageServerInterface> newCacheServers;
				std::map<UID, StorageServerInterface> deletedCacheServers;
				std::set_difference(allCacheServers.begin(),
				                    allCacheServers.end(),
				                    cacheServerMap.begin(),
				                    cacheServerMap.end(),
				                    std::insert_iterator<std::map<UID, StorageServerInterface>>(
				                        newCacheServers, newCacheServers.begin()));
				std::set_difference(cacheServerMap.begin(),
				                    cacheServerMap.end(),
				                    allCacheServers.begin(),
				                    allCacheServers.end(),
				                    std::insert_iterator<std::map<UID, StorageServerInterface>>(
				                        deletedCacheServers, deletedCacheServers.begin()));
				hasChanges = !(newCacheServers.empty() && deletedCacheServers.empty());
				if (hasChanges) {
					updateLocationCacheWithCaches(self, deletedCacheServers, newCacheServers);
				}
				cacheServerMap = std::move(allCacheServers);
				wait(delay(5.0));
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	} catch (Error& e) {
		TraceEvent(SevError, "MonitorCacheListFailed").error(e);
		throw;
	}
}

ACTOR static Future<Void> handleTssMismatches(DatabaseContext* cx) {
	state Reference<ReadYourWritesTransaction> tr;
	state KeyBackedMap<UID, UID> tssMapDB = KeyBackedMap<UID, UID>(tssMappingKeys.begin);
	state KeyBackedMap<Tuple, std::string> tssMismatchDB = KeyBackedMap<Tuple, std::string>(tssMismatchKeys.begin);
	loop {
		// <tssid, list of detailed mismatch data>
		state std::pair<UID, std::vector<DetailedTSSMismatch>> data = waitNext(cx->tssMismatchStream.getFuture());
		// find ss pair id so we can remove it from the mapping
		state UID tssPairID;
		bool found = false;
		for (const auto& it : cx->tssMapping) {
			if (it.second.id() == data.first) {
				tssPairID = it.first;
				found = true;
				break;
			}
		}
		if (found) {
			state bool quarantine = CLIENT_KNOBS->QUARANTINE_TSS_ON_MISMATCH;
			TraceEvent(SevWarnAlways, quarantine ? "TSS_QuarantineMismatch" : "TSS_KillMismatch")
			    .detail("TSSID", data.first.toString());
			TEST(quarantine); // Quarantining TSS because it got mismatch
			TEST(!quarantine); // Killing TSS because it got mismatch

			tr = makeReference<ReadYourWritesTransaction>(Database(Reference<DatabaseContext>::addRef(cx)));
			state int tries = 0;
			loop {
				try {
					tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
					tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
					if (quarantine) {
						tr->set(tssQuarantineKeyFor(data.first), LiteralStringRef(""));
					} else {
						tr->clear(serverTagKeyFor(data.first));
					}
					tssMapDB.erase(tr, tssPairID);

					for (const DetailedTSSMismatch& d : data.second) {
						// <tssid, time, mismatchid> -> mismatch data
						tssMismatchDB.set(
						    tr,
						    Tuple().append(data.first.toString()).append(d.timestamp).append(d.mismatchId.toString()),
						    d.traceString);
					}

					wait(tr->commit());

					break;
				} catch (Error& e) {
					wait(tr->onError(e));
				}
				tries++;
				if (tries > 10) {
					// Give up, it'll get another mismatch or a human will investigate eventually
					TraceEvent("TSS_MismatchGaveUp").detail("TSSID", data.first.toString());
					break;
				}
			}
			// clear out txn so that the extra DatabaseContext ref gets decref'd and we can free cx
			tr = makeReference<ReadYourWritesTransaction>();
		} else {
			TEST(true); // Not handling TSS with mismatch because it's already gone
		}
	}
}

ACTOR static Future<HealthMetrics> getHealthMetricsActor(DatabaseContext* cx, bool detailed) {
	if (now() - cx->healthMetricsLastUpdated < CLIENT_KNOBS->AGGREGATE_HEALTH_METRICS_MAX_STALENESS) {
		if (detailed) {
			return cx->healthMetrics;
		} else {
			HealthMetrics result;
			result.update(cx->healthMetrics, false, false);
			return result;
		}
	}
	state bool sendDetailedRequest =
	    detailed && now() - cx->detailedHealthMetricsLastUpdated > CLIENT_KNOBS->DETAILED_HEALTH_METRICS_MAX_STALENESS;
	loop {
		choose {
			when(wait(cx->onProxiesChanged())) {}
			when(GetHealthMetricsReply rep = wait(basicLoadBalance(cx->getGrvProxies(false),
			                                                       &GrvProxyInterface::getHealthMetrics,
			                                                       GetHealthMetricsRequest(sendDetailedRequest)))) {
				cx->healthMetrics.update(rep.healthMetrics, detailed, true);
				if (detailed) {
					cx->healthMetricsLastUpdated = now();
					cx->detailedHealthMetricsLastUpdated = now();
					return cx->healthMetrics;
				} else {
					cx->healthMetricsLastUpdated = now();
					HealthMetrics result;
					result.update(cx->healthMetrics, false, false);
					return result;
				}
			}
		}
	}
}

Future<HealthMetrics> DatabaseContext::getHealthMetrics(bool detailed = false) {
	return getHealthMetricsActor(this, detailed);
}

void DatabaseContext::registerSpecialKeySpaceModule(SpecialKeySpace::MODULE module,
                                                    SpecialKeySpace::IMPLTYPE type,
                                                    std::unique_ptr<SpecialKeyRangeReadImpl>&& impl) {
	specialKeySpace->registerKeyRange(module, type, impl->getKeyRange(), impl.get());
	specialKeySpaceModules.push_back(std::move(impl));
}

ACTOR Future<RangeResult> getWorkerInterfaces(Reference<ClusterConnectionFile> clusterFile);
ACTOR Future<Optional<Value>> getJSON(Database db);

struct WorkerInterfacesSpecialKeyImpl : SpecialKeyRangeReadImpl {
	Future<RangeResult> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override {
		if (ryw->getDatabase().getPtr() && ryw->getDatabase()->getConnectionFile()) {
			Key prefix = Key(getKeyRange().begin);
			return map(getWorkerInterfaces(ryw->getDatabase()->getConnectionFile()),
			           [prefix = prefix, kr = KeyRange(kr)](const RangeResult& in) {
				           RangeResult result;
				           for (const auto& [k_, v] : in) {
					           auto k = k_.withPrefix(prefix);
					           if (kr.contains(k))
						           result.push_back_deep(result.arena(), KeyValueRef(k, v));
				           }

				           std::sort(result.begin(), result.end(), KeyValueRef::OrderByKey{});
				           return result;
			           });
		} else {
			return RangeResult();
		}
	}

	explicit WorkerInterfacesSpecialKeyImpl(KeyRangeRef kr) : SpecialKeyRangeReadImpl(kr) {}
};

struct SingleSpecialKeyImpl : SpecialKeyRangeReadImpl {
	Future<RangeResult> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override {
		ASSERT(kr.contains(k));
		return map(f(ryw), [k = k](Optional<Value> v) {
			RangeResult result;
			if (v.present()) {
				result.push_back_deep(result.arena(), KeyValueRef(k, v.get()));
			}
			return result;
		});
	}

	SingleSpecialKeyImpl(KeyRef k, const std::function<Future<Optional<Value>>(ReadYourWritesTransaction*)>& f)
	  : SpecialKeyRangeReadImpl(singleKeyRange(k)), k(k), f(f) {}

private:
	Key k;
	std::function<Future<Optional<Value>>(ReadYourWritesTransaction*)> f;
};

class HealthMetricsRangeImpl : public SpecialKeyRangeAsyncImpl {
public:
	explicit HealthMetricsRangeImpl(KeyRangeRef kr);
	Future<RangeResult> getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const override;
};

static RangeResult healthMetricsToKVPairs(const HealthMetrics& metrics, KeyRangeRef kr) {
	RangeResult result;
	if (CLIENT_BUGGIFY)
		return result;
	if (kr.contains(LiteralStringRef("\xff\xff/metrics/health/aggregate")) && metrics.worstStorageDurabilityLag != 0) {
		json_spirit::mObject statsObj;
		statsObj["batch_limited"] = metrics.batchLimited;
		statsObj["tps_limit"] = metrics.tpsLimit;
		statsObj["worst_storage_durability_lag"] = metrics.worstStorageDurabilityLag;
		statsObj["limiting_storage_durability_lag"] = metrics.limitingStorageDurabilityLag;
		statsObj["worst_storage_queue"] = metrics.worstStorageQueue;
		statsObj["limiting_storage_queue"] = metrics.limitingStorageQueue;
		statsObj["worst_log_queue"] = metrics.worstTLogQueue;
		std::string statsString =
		    json_spirit::write_string(json_spirit::mValue(statsObj), json_spirit::Output_options::raw_utf8);
		ValueRef bytes(result.arena(), statsString);
		result.push_back(result.arena(), KeyValueRef(LiteralStringRef("\xff\xff/metrics/health/aggregate"), bytes));
	}
	// tlog stats
	{
		int phase = 0; // Avoid comparing twice per loop iteration
		for (const auto& [uid, logStats] : metrics.tLogQueue) {
			StringRef k{
				StringRef(uid.toString()).withPrefix(LiteralStringRef("\xff\xff/metrics/health/log/"), result.arena())
			};
			if (phase == 0 && k >= kr.begin) {
				phase = 1;
			}
			if (phase == 1) {
				if (k < kr.end) {
					json_spirit::mObject statsObj;
					statsObj["log_queue"] = logStats;
					std::string statsString =
					    json_spirit::write_string(json_spirit::mValue(statsObj), json_spirit::Output_options::raw_utf8);
					ValueRef bytes(result.arena(), statsString);
					result.push_back(result.arena(), KeyValueRef(k, bytes));
				} else {
					break;
				}
			}
		}
	}
	// Storage stats
	{
		int phase = 0; // Avoid comparing twice per loop iteration
		for (const auto& [uid, storageStats] : metrics.storageStats) {
			StringRef k{ StringRef(uid.toString())
				             .withPrefix(LiteralStringRef("\xff\xff/metrics/health/storage/"), result.arena()) };
			if (phase == 0 && k >= kr.begin) {
				phase = 1;
			}
			if (phase == 1) {
				if (k < kr.end) {
					json_spirit::mObject statsObj;
					statsObj["storage_durability_lag"] = storageStats.storageDurabilityLag;
					statsObj["storage_queue"] = storageStats.storageQueue;
					statsObj["cpu_usage"] = storageStats.cpuUsage;
					statsObj["disk_usage"] = storageStats.diskUsage;
					std::string statsString =
					    json_spirit::write_string(json_spirit::mValue(statsObj), json_spirit::Output_options::raw_utf8);
					ValueRef bytes(result.arena(), statsString);
					result.push_back(result.arena(), KeyValueRef(k, bytes));
				} else {
					break;
				}
			}
		}
	}
	return result;
}

ACTOR static Future<RangeResult> healthMetricsGetRangeActor(ReadYourWritesTransaction* ryw, KeyRangeRef kr) {
	HealthMetrics metrics = wait(ryw->getDatabase()->getHealthMetrics(
	    /*detailed ("per process")*/ kr.intersects(KeyRangeRef(LiteralStringRef("\xff\xff/metrics/health/storage/"),
	                                                           LiteralStringRef("\xff\xff/metrics/health/storage0"))) ||
	    kr.intersects(KeyRangeRef(LiteralStringRef("\xff\xff/metrics/health/log/"),
	                              LiteralStringRef("\xff\xff/metrics/health/log0")))));
	return healthMetricsToKVPairs(metrics, kr);
}

HealthMetricsRangeImpl::HealthMetricsRangeImpl(KeyRangeRef kr) : SpecialKeyRangeAsyncImpl(kr) {}

Future<RangeResult> HealthMetricsRangeImpl::getRange(ReadYourWritesTransaction* ryw, KeyRangeRef kr) const {
	return healthMetricsGetRangeActor(ryw, kr);
}

DatabaseContext::DatabaseContext(Reference<AsyncVar<Reference<ClusterConnectionFile>>> connectionFile,
                                 Reference<AsyncVar<ClientDBInfo>> clientInfo,
                                 Reference<AsyncVar<Optional<ClientLeaderRegInterface>> const> coordinator,
                                 Future<Void> clientInfoMonitor,
                                 TaskPriority taskID,
                                 LocalityData const& clientLocality,
                                 EnableLocalityLoadBalance enableLocalityLoadBalance,
                                 LockAware lockAware,
                                 IsInternal internal,
                                 int apiVersion,
                                 IsSwitchable switchable)
  : lockAware(lockAware), switchable(switchable), connectionFile(connectionFile), proxyProvisional(false),
    clientLocality(clientLocality), enableLocalityLoadBalance(enableLocalityLoadBalance), internal(internal),
    cc("TransactionMetrics"), transactionReadVersions("ReadVersions", cc),
    transactionReadVersionsThrottled("ReadVersionsThrottled", cc),
    transactionReadVersionsCompleted("ReadVersionsCompleted", cc),
    transactionReadVersionBatches("ReadVersionBatches", cc),
    transactionBatchReadVersions("BatchPriorityReadVersions", cc),
    transactionDefaultReadVersions("DefaultPriorityReadVersions", cc),
    transactionImmediateReadVersions("ImmediatePriorityReadVersions", cc),
    transactionBatchReadVersionsCompleted("BatchPriorityReadVersionsCompleted", cc),
    transactionDefaultReadVersionsCompleted("DefaultPriorityReadVersionsCompleted", cc),
    transactionImmediateReadVersionsCompleted("ImmediatePriorityReadVersionsCompleted", cc),
    transactionLogicalReads("LogicalUncachedReads", cc), transactionPhysicalReads("PhysicalReadRequests", cc),
    transactionPhysicalReadsCompleted("PhysicalReadRequestsCompleted", cc),
    transactionGetKeyRequests("GetKeyRequests", cc), transactionGetValueRequests("GetValueRequests", cc),
    transactionGetRangeRequests("GetRangeRequests", cc),
    transactionGetRangeStreamRequests("GetRangeStreamRequests", cc), transactionWatchRequests("WatchRequests", cc),
    transactionGetAddressesForKeyRequests("GetAddressesForKeyRequests", cc), transactionBytesRead("BytesRead", cc),
    transactionKeysRead("KeysRead", cc), transactionMetadataVersionReads("MetadataVersionReads", cc),
    transactionCommittedMutations("CommittedMutations", cc),
    transactionCommittedMutationBytes("CommittedMutationBytes", cc), transactionSetMutations("SetMutations", cc),
    transactionClearMutations("ClearMutations", cc), transactionAtomicMutations("AtomicMutations", cc),
    transactionsCommitStarted("CommitStarted", cc), transactionsCommitCompleted("CommitCompleted", cc),
    transactionKeyServerLocationRequests("KeyServerLocationRequests", cc),
    transactionKeyServerLocationRequestsCompleted("KeyServerLocationRequestsCompleted", cc),
    transactionStatusRequests("StatusRequests", cc), transactionsTooOld("TooOld", cc),
    transactionsFutureVersions("FutureVersions", cc), transactionsNotCommitted("NotCommitted", cc),
    transactionsMaybeCommitted("MaybeCommitted", cc), transactionsResourceConstrained("ResourceConstrained", cc),
    transactionsProcessBehind("ProcessBehind", cc), transactionsThrottled("Throttled", cc),
    transactionsExpensiveClearCostEstCount("ExpensiveClearCostEstCount", cc),
    transactionGrvFullBatches("NumGrvFullBatches", cc), transactionGrvTimedOutBatches("NumGrvTimedOutBatches", cc),
    latencies(1000), readLatencies(1000), commitLatencies(1000), GRVLatencies(1000), mutationsPerCommit(1000),
    bytesPerCommit(1000), outstandingWatches(0), transactionTracingEnabled(true), taskID(taskID),
    clientInfo(clientInfo), clientInfoMonitor(clientInfoMonitor), coordinator(coordinator), apiVersion(apiVersion),
    mvCacheInsertLocation(0), healthMetricsLastUpdated(0), detailedHealthMetricsLastUpdated(0),
    smoothMidShardSize(CLIENT_KNOBS->SHARD_STAT_SMOOTH_AMOUNT),
    specialKeySpace(std::make_unique<SpecialKeySpace>(specialKeys.begin, specialKeys.end, /* test */ false)) {
	dbId = deterministicRandom()->randomUniqueID();
	connected = (clientInfo->get().commitProxies.size() && clientInfo->get().grvProxies.size())
	                ? Void()
	                : clientInfo->onChange();

	metadataVersionCache.resize(CLIENT_KNOBS->METADATA_VERSION_CACHE_SIZE);
	maxOutstandingWatches = CLIENT_KNOBS->DEFAULT_MAX_OUTSTANDING_WATCHES;

	snapshotRywEnabled = apiVersionAtLeast(300) ? 1 : 0;

	logger = databaseLogger(this);
	locationCacheSize = g_network->isSimulated() ? CLIENT_KNOBS->LOCATION_CACHE_EVICTION_SIZE_SIM
	                                             : CLIENT_KNOBS->LOCATION_CACHE_EVICTION_SIZE;

	getValueSubmitted.init(LiteralStringRef("NativeAPI.GetValueSubmitted"));
	getValueCompleted.init(LiteralStringRef("NativeAPI.GetValueCompleted"));

	monitorProxiesInfoChange = monitorProxiesChange(this, clientInfo, &proxiesChangeTrigger);
	tssMismatchHandler = handleTssMismatches(this);
	clientStatusUpdater.actor = clientStatusUpdateActor(this);
	cacheListMonitor = monitorCacheList(this);

	smoothMidShardSize.reset(CLIENT_KNOBS->INIT_MID_SHARD_BYTES);

	if (apiVersionAtLeast(700)) {
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::ERRORMSG,
		                              SpecialKeySpace::IMPLTYPE::READONLY,
		                              std::make_unique<SingleSpecialKeyImpl>(
		                                  SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::ERRORMSG).begin,
		                                  [](ReadYourWritesTransaction* ryw) -> Future<Optional<Value>> {
			                                  if (ryw->getSpecialKeySpaceErrorMsg().present())
				                                  return Optional<Value>(ryw->getSpecialKeySpaceErrorMsg().get());
			                                  else
				                                  return Optional<Value>();
		                                  }));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<ManagementCommandsOptionsImpl>(
		        KeyRangeRef(LiteralStringRef("options/"), LiteralStringRef("options0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<ExcludeServersRangeImpl>(SpecialKeySpace::getManamentApiCommandRange("exclude")));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<FailedServersRangeImpl>(SpecialKeySpace::getManamentApiCommandRange("failed")));
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::MANAGEMENT,
		                              SpecialKeySpace::IMPLTYPE::READWRITE,
		                              std::make_unique<ExcludedLocalitiesRangeImpl>(
		                                  SpecialKeySpace::getManamentApiCommandRange("excludedlocality")));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<FailedLocalitiesRangeImpl>(SpecialKeySpace::getManamentApiCommandRange("failedlocality")));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<ExclusionInProgressRangeImpl>(
		        KeyRangeRef(LiteralStringRef("in_progress_exclusion/"), LiteralStringRef("in_progress_exclusion0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::CONFIGURATION,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<ProcessClassRangeImpl>(
		        KeyRangeRef(LiteralStringRef("process/class_type/"), LiteralStringRef("process/class_type0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::CONFIGURATION).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::CONFIGURATION,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<ProcessClassSourceRangeImpl>(
		        KeyRangeRef(LiteralStringRef("process/class_source/"), LiteralStringRef("process/class_source0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::CONFIGURATION).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<LockDatabaseImpl>(
		        singleKeyRange(LiteralStringRef("db_locked"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<ConsistencyCheckImpl>(
		        singleKeyRange(LiteralStringRef("consistency_check_suspended"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::GLOBALCONFIG,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<GlobalConfigImpl>(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::GLOBALCONFIG)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::TRACING,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<TracingOptionsImpl>(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::TRACING)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::CONFIGURATION,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<CoordinatorsImpl>(
		        KeyRangeRef(LiteralStringRef("coordinators/"), LiteralStringRef("coordinators0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::CONFIGURATION).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<CoordinatorsAutoImpl>(
		        singleKeyRange(LiteralStringRef("auto_coordinators"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<AdvanceVersionImpl>(
		        singleKeyRange(LiteralStringRef("min_required_commit_version"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<ClientProfilingImpl>(
		        KeyRangeRef(LiteralStringRef("profiling/"), LiteralStringRef("profiling0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<MaintenanceImpl>(
		        KeyRangeRef(LiteralStringRef("maintenance/"), LiteralStringRef("maintenance0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::MANAGEMENT,
		    SpecialKeySpace::IMPLTYPE::READWRITE,
		    std::make_unique<DataDistributionImpl>(
		        KeyRangeRef(LiteralStringRef("data_distribution/"), LiteralStringRef("data_distribution0"))
		            .withPrefix(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::MANAGEMENT).begin)));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::ACTORLINEAGE,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<ActorLineageImpl>(SpecialKeySpace::getModuleRange(SpecialKeySpace::MODULE::ACTORLINEAGE)));
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::ACTOR_PROFILER_CONF,
		                              SpecialKeySpace::IMPLTYPE::READWRITE,
		                              std::make_unique<ActorProfilerConf>(SpecialKeySpace::getModuleRange(
		                                  SpecialKeySpace::MODULE::ACTOR_PROFILER_CONF)));
	}
	if (apiVersionAtLeast(630)) {
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::TRANSACTION,
		                              SpecialKeySpace::IMPLTYPE::READONLY,
		                              std::make_unique<ConflictingKeysImpl>(conflictingKeysRange));
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::TRANSACTION,
		                              SpecialKeySpace::IMPLTYPE::READONLY,
		                              std::make_unique<ReadConflictRangeImpl>(readConflictRangeKeysRange));
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::TRANSACTION,
		                              SpecialKeySpace::IMPLTYPE::READONLY,
		                              std::make_unique<WriteConflictRangeImpl>(writeConflictRangeKeysRange));
		registerSpecialKeySpaceModule(SpecialKeySpace::MODULE::METRICS,
		                              SpecialKeySpace::IMPLTYPE::READONLY,
		                              std::make_unique<DDStatsRangeImpl>(ddStatsRange));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::METRICS,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<HealthMetricsRangeImpl>(KeyRangeRef(LiteralStringRef("\xff\xff/metrics/health/"),
		                                                         LiteralStringRef("\xff\xff/metrics/health0"))));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::WORKERINTERFACE,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<WorkerInterfacesSpecialKeyImpl>(KeyRangeRef(
		        LiteralStringRef("\xff\xff/worker_interfaces/"), LiteralStringRef("\xff\xff/worker_interfaces0"))));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::STATUSJSON,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<SingleSpecialKeyImpl>(LiteralStringRef("\xff\xff/status/json"),
		                                           [](ReadYourWritesTransaction* ryw) -> Future<Optional<Value>> {
			                                           if (ryw->getDatabase().getPtr() &&
			                                               ryw->getDatabase()->getConnectionFile()) {
				                                           ++ryw->getDatabase()->transactionStatusRequests;
				                                           return getJSON(ryw->getDatabase());
			                                           } else {
				                                           return Optional<Value>();
			                                           }
		                                           }));
		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::CLUSTERFILEPATH,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<SingleSpecialKeyImpl>(
		        LiteralStringRef("\xff\xff/cluster_file_path"),
		        [](ReadYourWritesTransaction* ryw) -> Future<Optional<Value>> {
			        try {
				        if (ryw->getDatabase().getPtr() && ryw->getDatabase()->getConnectionFile()) {
					        Optional<Value> output = StringRef(ryw->getDatabase()->getConnectionFile()->getFilename());
					        return output;
				        }
			        } catch (Error& e) {
				        return e;
			        }
			        return Optional<Value>();
		        }));

		registerSpecialKeySpaceModule(
		    SpecialKeySpace::MODULE::CONNECTIONSTRING,
		    SpecialKeySpace::IMPLTYPE::READONLY,
		    std::make_unique<SingleSpecialKeyImpl>(
		        LiteralStringRef("\xff\xff/connection_string"),
		        [](ReadYourWritesTransaction* ryw) -> Future<Optional<Value>> {
			        try {
				        if (ryw->getDatabase().getPtr() && ryw->getDatabase()->getConnectionFile()) {
					        Reference<ClusterConnectionFile> f = ryw->getDatabase()->getConnectionFile();
					        Optional<Value> output = StringRef(f->getConnectionString().toString());
					        return output;
				        }
			        } catch (Error& e) {
				        return e;
			        }
			        return Optional<Value>();
		        }));
	}
	throttleExpirer = recurring([this]() { expireThrottles(); }, CLIENT_KNOBS->TAG_THROTTLE_EXPIRATION_INTERVAL);

	if (BUGGIFY) {
		DatabaseContext::debugUseTags = true;
	}
}

DatabaseContext::DatabaseContext(const Error& err)
  : deferredError(err), internal(IsInternal::False), cc("TransactionMetrics"),
    transactionReadVersions("ReadVersions", cc), transactionReadVersionsThrottled("ReadVersionsThrottled", cc),
    transactionReadVersionsCompleted("ReadVersionsCompleted", cc),
    transactionReadVersionBatches("ReadVersionBatches", cc),
    transactionBatchReadVersions("BatchPriorityReadVersions", cc),
    transactionDefaultReadVersions("DefaultPriorityReadVersions", cc),
    transactionImmediateReadVersions("ImmediatePriorityReadVersions", cc),
    transactionBatchReadVersionsCompleted("BatchPriorityReadVersionsCompleted", cc),
    transactionDefaultReadVersionsCompleted("DefaultPriorityReadVersionsCompleted", cc),
    transactionImmediateReadVersionsCompleted("ImmediatePriorityReadVersionsCompleted", cc),
    transactionLogicalReads("LogicalUncachedReads", cc), transactionPhysicalReads("PhysicalReadRequests", cc),
    transactionPhysicalReadsCompleted("PhysicalReadRequestsCompleted", cc),
    transactionGetKeyRequests("GetKeyRequests", cc), transactionGetValueRequests("GetValueRequests", cc),
    transactionGetRangeRequests("GetRangeRequests", cc),
    transactionGetRangeStreamRequests("GetRangeStreamRequests", cc), transactionWatchRequests("WatchRequests", cc),
    transactionGetAddressesForKeyRequests("GetAddressesForKeyRequests", cc), transactionBytesRead("BytesRead", cc),
    transactionKeysRead("KeysRead", cc), transactionMetadataVersionReads("MetadataVersionReads", cc),
    transactionCommittedMutations("CommittedMutations", cc),
    transactionCommittedMutationBytes("CommittedMutationBytes", cc), transactionSetMutations("SetMutations", cc),
    transactionClearMutations("ClearMutations", cc), transactionAtomicMutations("AtomicMutations", cc),
    transactionsCommitStarted("CommitStarted", cc), transactionsCommitCompleted("CommitCompleted", cc),
    transactionKeyServerLocationRequests("KeyServerLocationRequests", cc),
    transactionKeyServerLocationRequestsCompleted("KeyServerLocationRequestsCompleted", cc),
    transactionStatusRequests("StatusRequests", cc), transactionsTooOld("TooOld", cc),
    transactionsFutureVersions("FutureVersions", cc), transactionsNotCommitted("NotCommitted", cc),
    transactionsMaybeCommitted("MaybeCommitted", cc), transactionsResourceConstrained("ResourceConstrained", cc),
    transactionsProcessBehind("ProcessBehind", cc), transactionsThrottled("Throttled", cc),
    transactionsExpensiveClearCostEstCount("ExpensiveClearCostEstCount", cc),
    transactionGrvFullBatches("NumGrvFullBatches", cc), transactionGrvTimedOutBatches("NumGrvTimedOutBatches", cc),
    latencies(1000), readLatencies(1000), commitLatencies(1000), GRVLatencies(1000), mutationsPerCommit(1000),
    bytesPerCommit(1000), transactionTracingEnabled(true), smoothMidShardSize(CLIENT_KNOBS->SHARD_STAT_SMOOTH_AMOUNT) {}

// Static constructor used by server processes to create a DatabaseContext
// For internal (fdbserver) use only
Database DatabaseContext::create(Reference<AsyncVar<ClientDBInfo>> clientInfo,
                                 Future<Void> clientInfoMonitor,
                                 LocalityData clientLocality,
                                 EnableLocalityLoadBalance enableLocalityLoadBalance,
                                 TaskPriority taskID,
                                 LockAware lockAware,
                                 int apiVersion,
                                 IsSwitchable switchable) {
	return Database(new DatabaseContext(Reference<AsyncVar<Reference<ClusterConnectionFile>>>(),
	                                    clientInfo,
	                                    makeReference<AsyncVar<Optional<ClientLeaderRegInterface>>>(),
	                                    clientInfoMonitor,
	                                    taskID,
	                                    clientLocality,
	                                    enableLocalityLoadBalance,
	                                    lockAware,
	                                    IsInternal::True,
	                                    apiVersion,
	                                    switchable));
}

DatabaseContext::~DatabaseContext() {
	cacheListMonitor.cancel();
	monitorProxiesInfoChange.cancel();
	monitorTssInfoChange.cancel();
	tssMismatchHandler.cancel();
	for (auto it = server_interf.begin(); it != server_interf.end(); it = server_interf.erase(it))
		it->second->notifyContextDestroyed();
	ASSERT_ABORT(server_interf.empty());
	locationCache.insert(allKeys, Reference<LocationInfo>());
}

std::pair<KeyRange, Reference<LocationInfo>> DatabaseContext::getCachedLocation(const KeyRef& key, Reverse isBackward) {
	if (isBackward) {
		auto range = locationCache.rangeContainingKeyBefore(key);
		return std::make_pair(range->range(), range->value());
	} else {
		auto range = locationCache.rangeContaining(key);
		return std::make_pair(range->range(), range->value());
	}
}

bool DatabaseContext::getCachedLocations(const KeyRangeRef& range,
                                         std::vector<std::pair<KeyRange, Reference<LocationInfo>>>& result,
                                         int limit,
                                         Reverse reverse) {
	result.clear();

	auto begin = locationCache.rangeContaining(range.begin);
	auto end = locationCache.rangeContainingKeyBefore(range.end);

	loop {
		auto r = reverse ? end : begin;
		if (!r->value()) {
			TEST(result.size()); // had some but not all cached locations
			result.clear();
			return false;
		}
		result.emplace_back(r->range() & range, r->value());
		if (result.size() == limit || begin == end) {
			break;
		}

		if (reverse)
			--end;
		else
			++begin;
	}

	return true;
}

Reference<LocationInfo> DatabaseContext::setCachedLocation(const KeyRangeRef& keys,
                                                           const std::vector<StorageServerInterface>& servers) {
	std::vector<Reference<ReferencedInterface<StorageServerInterface>>> serverRefs;
	serverRefs.reserve(servers.size());
	for (const auto& interf : servers) {
		serverRefs.push_back(StorageServerInfo::getInterface(this, interf, clientLocality));
	}

	int maxEvictionAttempts = 100, attempts = 0;
	auto loc = makeReference<LocationInfo>(serverRefs);
	while (locationCache.size() > locationCacheSize && attempts < maxEvictionAttempts) {
		TEST(true); // NativeAPI storage server locationCache entry evicted
		attempts++;
		auto r = locationCache.randomRange();
		Key begin = r.begin(), end = r.end(); // insert invalidates r, so can't be passed a mere reference into it
		locationCache.insert(KeyRangeRef(begin, end), Reference<LocationInfo>());
	}
	locationCache.insert(keys, loc);
	return loc;
}

void DatabaseContext::invalidateCache(const KeyRef& key, Reverse isBackward) {
	if (isBackward) {
		locationCache.rangeContainingKeyBefore(key)->value() = Reference<LocationInfo>();
	} else {
		locationCache.rangeContaining(key)->value() = Reference<LocationInfo>();
	}
}

void DatabaseContext::invalidateCache(const KeyRangeRef& keys) {
	auto rs = locationCache.intersectingRanges(keys);
	Key begin = rs.begin().begin(),
	    end = rs.end().begin(); // insert invalidates rs, so can't be passed a mere reference into it
	locationCache.insert(KeyRangeRef(begin, end), Reference<LocationInfo>());
}

Future<Void> DatabaseContext::onProxiesChanged() const {
	return this->proxiesChangeTrigger.onTrigger();
}

bool DatabaseContext::sampleReadTags() const {
	double sampleRate = GlobalConfig::globalConfig().get(transactionTagSampleRate, CLIENT_KNOBS->READ_TAG_SAMPLE_RATE);
	return sampleRate > 0 && deterministicRandom()->random01() <= sampleRate;
}

bool DatabaseContext::sampleOnCost(uint64_t cost) const {
	double sampleCost =
	    GlobalConfig::globalConfig().get<double>(transactionTagSampleCost, CLIENT_KNOBS->COMMIT_SAMPLE_COST);
	if (sampleCost <= 0)
		return false;
	return deterministicRandom()->random01() <= (double)cost / sampleCost;
}

int64_t extractIntOption(Optional<StringRef> value, int64_t minValue, int64_t maxValue) {
	validateOptionValuePresent(value);
	if (value.get().size() != 8) {
		throw invalid_option_value();
	}

	int64_t passed = *((int64_t*)(value.get().begin()));
	if (passed > maxValue || passed < minValue) {
		throw invalid_option_value();
	}

	return passed;
}

uint64_t extractHexOption(StringRef value) {
	char* end;
	uint64_t id = strtoull(value.toString().c_str(), &end, 16);
	if (*end)
		throw invalid_option_value();
	return id;
}

void DatabaseContext::setOption(FDBDatabaseOptions::Option option, Optional<StringRef> value) {
	int defaultFor = FDBDatabaseOptions::optionInfo.getMustExist(option).defaultFor;
	if (defaultFor >= 0) {
		ASSERT(FDBTransactionOptions::optionInfo.find((FDBTransactionOptions::Option)defaultFor) !=
		       FDBTransactionOptions::optionInfo.end());
		transactionDefaults.addOption((FDBTransactionOptions::Option)defaultFor, value.castTo<Standalone<StringRef>>());
	} else {
		switch (option) {
		case FDBDatabaseOptions::LOCATION_CACHE_SIZE:
			locationCacheSize = (int)extractIntOption(value, 0, std::numeric_limits<int>::max());
			break;
		case FDBDatabaseOptions::MACHINE_ID:
			clientLocality =
			    LocalityData(clientLocality.processId(),
			                 value.present() ? Standalone<StringRef>(value.get()) : Optional<Standalone<StringRef>>(),
			                 clientLocality.machineId(),
			                 clientLocality.dcId());
			if (clientInfo->get().commitProxies.size())
				commitProxies = makeReference<CommitProxyInfo>(clientInfo->get().commitProxies, false);
			if (clientInfo->get().grvProxies.size())
				grvProxies = makeReference<GrvProxyInfo>(clientInfo->get().grvProxies, true);
			server_interf.clear();
			locationCache.insert(allKeys, Reference<LocationInfo>());
			break;
		case FDBDatabaseOptions::MAX_WATCHES:
			maxOutstandingWatches = (int)extractIntOption(value, 0, CLIENT_KNOBS->ABSOLUTE_MAX_WATCHES);
			break;
		case FDBDatabaseOptions::DATACENTER_ID:
			clientLocality =
			    LocalityData(clientLocality.processId(),
			                 clientLocality.zoneId(),
			                 clientLocality.machineId(),
			                 value.present() ? Standalone<StringRef>(value.get()) : Optional<Standalone<StringRef>>());
			if (clientInfo->get().commitProxies.size())
				commitProxies = makeReference<CommitProxyInfo>(clientInfo->get().commitProxies, false);
			if (clientInfo->get().grvProxies.size())
				grvProxies = makeReference<GrvProxyInfo>(clientInfo->get().grvProxies, true);
			server_interf.clear();
			locationCache.insert(allKeys, Reference<LocationInfo>());
			break;
		case FDBDatabaseOptions::SNAPSHOT_RYW_ENABLE:
			validateOptionValueNotPresent(value);
			snapshotRywEnabled++;
			break;
		case FDBDatabaseOptions::SNAPSHOT_RYW_DISABLE:
			validateOptionValueNotPresent(value);
			snapshotRywEnabled--;
			break;
		case FDBDatabaseOptions::DISTRIBUTED_TRANSACTION_TRACE_ENABLE:
			validateOptionValueNotPresent(value);
			transactionTracingEnabled++;
			break;
		case FDBDatabaseOptions::DISTRIBUTED_TRANSACTION_TRACE_DISABLE:
			validateOptionValueNotPresent(value);
			transactionTracingEnabled--;
			break;
		case FDBDatabaseOptions::USE_CONFIG_DATABASE:
			validateOptionValueNotPresent(value);
			useConfigDatabase = true;
			break;
		case FDBDatabaseOptions::TEST_CAUSAL_READ_RISKY:
			verifyCausalReadsProp = double(extractIntOption(value, 0, 100)) / 100.0;
			break;
		default:
			break;
		}
	}
}

void DatabaseContext::addWatch() {
	if (outstandingWatches >= maxOutstandingWatches)
		throw too_many_watches();

	++outstandingWatches;
}

void DatabaseContext::removeWatch() {
	--outstandingWatches;
	ASSERT(outstandingWatches >= 0);
}

Future<Void> DatabaseContext::onConnected() {
	return connected;
}

ACTOR static Future<Void> switchConnectionFileImpl(Reference<ClusterConnectionFile> connFile, DatabaseContext* self) {
	TEST(true); // Switch connection file
	TraceEvent("SwitchConnectionFile")
	    .detail("ConnectionFile", connFile->canGetFilename() ? connFile->getFilename() : "")
	    .detail("ConnectionString", connFile->getConnectionString().toString());

	// Reset state from former cluster.
	self->commitProxies.clear();
	self->grvProxies.clear();
	self->minAcceptableReadVersion = std::numeric_limits<Version>::max();
	self->invalidateCache(allKeys);

	auto clearedClientInfo = self->clientInfo->get();
	clearedClientInfo.commitProxies.clear();
	clearedClientInfo.grvProxies.clear();
	clearedClientInfo.id = deterministicRandom()->randomUniqueID();
	self->clientInfo->set(clearedClientInfo);
	self->connectionFile->set(connFile);

	state Database db(Reference<DatabaseContext>::addRef(self));
	state Transaction tr(db);
	loop {
		tr.setOption(FDBTransactionOptions::READ_LOCK_AWARE);
		try {
			TraceEvent("SwitchConnectionFileAttemptingGRV").log();
			Version v = wait(tr.getReadVersion());
			TraceEvent("SwitchConnectionFileGotRV")
			    .detail("ReadVersion", v)
			    .detail("MinAcceptableReadVersion", self->minAcceptableReadVersion);
			ASSERT(self->minAcceptableReadVersion != std::numeric_limits<Version>::max());
			self->connectionFileChangedTrigger.trigger();
			return Void();
		} catch (Error& e) {
			TraceEvent("SwitchConnectionFileError").detail("Error", e.what());
			wait(tr.onError(e));
		}
	}
}

Reference<ClusterConnectionFile> DatabaseContext::getConnectionFile() {
	if (connectionFile) {
		return connectionFile->get();
	}
	return Reference<ClusterConnectionFile>();
}

Future<Void> DatabaseContext::switchConnectionFile(Reference<ClusterConnectionFile> standby) {
	ASSERT(switchable);
	return switchConnectionFileImpl(standby, this);
}

Future<Void> DatabaseContext::connectionFileChanged() {
	return connectionFileChangedTrigger.onTrigger();
}

void DatabaseContext::expireThrottles() {
	for (auto& priorityItr : throttledTags) {
		for (auto tagItr = priorityItr.second.begin(); tagItr != priorityItr.second.end();) {
			if (tagItr->second.expired()) {
				TEST(true); // Expiring client throttle
				tagItr = priorityItr.second.erase(tagItr);
			} else {
				++tagItr;
			}
		}
	}
}

extern IPAddress determinePublicIPAutomatically(ClusterConnectionString const& ccs);

// Creates a database object that represents a connection to a cluster
// This constructor uses a preallocated DatabaseContext that may have been created
// on another thread
Database Database::createDatabase(Reference<ClusterConnectionFile> connFile,
                                  int apiVersion,
                                  IsInternal internal,
                                  LocalityData const& clientLocality,
                                  DatabaseContext* preallocatedDb) {
	if (!g_network)
		throw network_not_setup();

	if (connFile) {
		if (networkOptions.traceDirectory.present() && !traceFileIsOpen()) {
			g_network->initMetrics();
			FlowTransport::transport().initMetrics();
			initTraceEventMetrics();

			auto publicIP = determinePublicIPAutomatically(connFile->getConnectionString());
			selectTraceFormatter(networkOptions.traceFormat);
			selectTraceClockSource(networkOptions.traceClockSource);
			openTraceFile(NetworkAddress(publicIP, ::getpid()),
			              networkOptions.traceRollSize,
			              networkOptions.traceMaxLogsSize,
			              networkOptions.traceDirectory.get(),
			              "trace",
			              networkOptions.traceLogGroup,
			              networkOptions.traceFileIdentifier,
			              networkOptions.tracePartialFileSuffix);

			TraceEvent("ClientStart")
			    .detail("SourceVersion", getSourceVersion())
			    .detail("Version", FDB_VT_VERSION)
			    .detail("PackageName", FDB_VT_PACKAGE_NAME)
			    .detail("ClusterFile", connFile->getFilename().c_str())
			    .detail("ConnectionString", connFile->getConnectionString().toString())
			    .detailf("ActualTime", "%lld", DEBUG_DETERMINISM ? 0 : time(nullptr))
			    .detail("ApiVersion", apiVersion)
			    .detailf("ImageOffset", "%p", platform::getImageOffset())
			    .trackLatest("ClientStart");

			initializeSystemMonitorMachineState(SystemMonitorMachineState(IPAddress(publicIP)));

			systemMonitor();
			uncancellable(recurring(&systemMonitor, CLIENT_KNOBS->SYSTEM_MONITOR_INTERVAL, TaskPriority::FlushTrace));
		}
	}

	g_network->initTLS();

	auto clientInfo = makeReference<AsyncVar<ClientDBInfo>>();
	auto coordinator = makeReference<AsyncVar<Optional<ClientLeaderRegInterface>>>();
	auto connectionFile = makeReference<AsyncVar<Reference<ClusterConnectionFile>>>();
	connectionFile->set(connFile);
	Future<Void> clientInfoMonitor = monitorProxies(connectionFile,
	                                                clientInfo,
	                                                coordinator,
	                                                networkOptions.supportedVersions,
	                                                StringRef(networkOptions.traceLogGroup));

	DatabaseContext* db;
	if (preallocatedDb) {
		db = new (preallocatedDb) DatabaseContext(connectionFile,
		                                          clientInfo,
		                                          coordinator,
		                                          clientInfoMonitor,
		                                          TaskPriority::DefaultEndpoint,
		                                          clientLocality,
		                                          EnableLocalityLoadBalance::True,
		                                          LockAware::False,
		                                          internal,
		                                          apiVersion,
		                                          IsSwitchable::True);
	} else {
		db = new DatabaseContext(connectionFile,
		                         clientInfo,
		                         coordinator,
		                         clientInfoMonitor,
		                         TaskPriority::DefaultEndpoint,
		                         clientLocality,
		                         EnableLocalityLoadBalance::True,
		                         LockAware::False,
		                         internal,
		                         apiVersion,
		                         IsSwitchable::True);
	}

	auto database = Database(db);
	GlobalConfig::create(
	    database, Reference<AsyncVar<ClientDBInfo> const>(clientInfo), std::addressof(clientInfo->get()));
	GlobalConfig::globalConfig().trigger(samplingFrequency, samplingProfilerUpdateFrequency);
	GlobalConfig::globalConfig().trigger(samplingWindow, samplingProfilerUpdateWindow);
	return database;
}

Database Database::createDatabase(std::string connFileName,
                                  int apiVersion,
                                  IsInternal internal,
                                  LocalityData const& clientLocality) {
	Reference<ClusterConnectionFile> rccf = Reference<ClusterConnectionFile>(
	    new ClusterConnectionFile(ClusterConnectionFile::lookupClusterFileName(connFileName).first));
	return Database::createDatabase(rccf, apiVersion, internal, clientLocality);
}

Reference<WatchMetadata> DatabaseContext::getWatchMetadata(KeyRef key) const {
	const auto it = watchMap.find(key);
	if (it == watchMap.end())
		return Reference<WatchMetadata>();
	return it->second;
}

KeyRef DatabaseContext::setWatchMetadata(Reference<WatchMetadata> metadata) {
	KeyRef keyRef = metadata->key.contents();
	watchMap[keyRef] = metadata;
	return keyRef;
}

void DatabaseContext::deleteWatchMetadata(KeyRef key) {
	watchMap.erase(key);
}

void DatabaseContext::clearWatchMetadata() {
	watchMap.clear();
}

WatchMetadata::WatchMetadata(Key key, Optional<Value> value, Version version, TransactionInfo info, TagSet tags)
  : key(key), value(value), version(version), info(info), tags(tags) {
	// create dummy future
	watchFuture = watchPromise.getFuture();
}

const UniqueOrderedOptionList<FDBTransactionOptions>& Database::getTransactionDefaults() const {
	ASSERT(db);
	return db->transactionDefaults;
}

void setNetworkOption(FDBNetworkOptions::Option option, Optional<StringRef> value) {
	std::regex identifierRegex("^[a-zA-Z0-9_]*$");
	switch (option) {
	// SOMEDAY: If the network is already started, should these five throw an error?
	case FDBNetworkOptions::TRACE_ENABLE:
		networkOptions.traceDirectory = value.present() ? value.get().toString() : "";
		break;
	case FDBNetworkOptions::TRACE_ROLL_SIZE:
		validateOptionValuePresent(value);
		networkOptions.traceRollSize = extractIntOption(value, 0, std::numeric_limits<int64_t>::max());
		break;
	case FDBNetworkOptions::TRACE_MAX_LOGS_SIZE:
		validateOptionValuePresent(value);
		networkOptions.traceMaxLogsSize = extractIntOption(value, 0, std::numeric_limits<int64_t>::max());
		break;
	case FDBNetworkOptions::TRACE_FORMAT:
		validateOptionValuePresent(value);
		networkOptions.traceFormat = value.get().toString();
		if (!validateTraceFormat(networkOptions.traceFormat)) {
			fprintf(stderr, "Unrecognized trace format: `%s'\n", networkOptions.traceFormat.c_str());
			throw invalid_option_value();
		}
		break;
	case FDBNetworkOptions::TRACE_FILE_IDENTIFIER:
		validateOptionValuePresent(value);
		networkOptions.traceFileIdentifier = value.get().toString();
		if (networkOptions.traceFileIdentifier.length() > CLIENT_KNOBS->TRACE_LOG_FILE_IDENTIFIER_MAX_LENGTH) {
			fprintf(stderr, "Trace file identifier provided is too long.\n");
			throw invalid_option_value();
		} else if (!std::regex_match(networkOptions.traceFileIdentifier, identifierRegex)) {
			fprintf(stderr, "Trace file identifier should only contain alphanumerics and underscores.\n");
			throw invalid_option_value();
		}
		break;

	case FDBNetworkOptions::TRACE_LOG_GROUP:
		if (value.present()) {
			if (traceFileIsOpen()) {
				setTraceLogGroup(value.get().toString());
			} else {
				networkOptions.traceLogGroup = value.get().toString();
			}
		}
		break;
	case FDBNetworkOptions::TRACE_CLOCK_SOURCE:
		validateOptionValuePresent(value);
		networkOptions.traceClockSource = value.get().toString();
		if (!validateTraceClockSource(networkOptions.traceClockSource)) {
			fprintf(stderr, "Unrecognized trace clock source: `%s'\n", networkOptions.traceClockSource.c_str());
			throw invalid_option_value();
		}
		break;
	case FDBNetworkOptions::TRACE_PARTIAL_FILE_SUFFIX:
		validateOptionValuePresent(value);
		networkOptions.tracePartialFileSuffix = value.get().toString();
		break;
	case FDBNetworkOptions::KNOB: {
		validateOptionValuePresent(value);

		std::string optionValue = value.get().toString();
		TraceEvent("SetKnob").detail("KnobString", optionValue);

		size_t eq = optionValue.find_first_of('=');
		if (eq == optionValue.npos) {
			TraceEvent(SevWarnAlways, "InvalidKnobString").detail("KnobString", optionValue);
			throw invalid_option_value();
		}

		std::string knobName = optionValue.substr(0, eq);
		std::string knobValueString = optionValue.substr(eq + 1);

		try {
			auto knobValue = IKnobCollection::parseKnobValue(knobName, knobValueString, IKnobCollection::Type::CLIENT);
			IKnobCollection::getMutableGlobalKnobCollection().setKnob(knobName, knobValue);
		} catch (Error& e) {
			TraceEvent(SevWarnAlways, "UnrecognizedKnob").detail("Knob", knobName.c_str());
			fprintf(stderr, "FoundationDB client ignoring unrecognized knob option '%s'\n", knobName.c_str());
		}
		break;
	}
	case FDBNetworkOptions::TLS_PLUGIN:
		validateOptionValuePresent(value);
		break;
	case FDBNetworkOptions::TLS_CERT_PATH:
		validateOptionValuePresent(value);
		tlsConfig.setCertificatePath(value.get().toString());
		break;
	case FDBNetworkOptions::TLS_CERT_BYTES: {
		validateOptionValuePresent(value);
		tlsConfig.setCertificateBytes(value.get().toString());
		break;
	}
	case FDBNetworkOptions::TLS_CA_PATH: {
		validateOptionValuePresent(value);
		tlsConfig.setCAPath(value.get().toString());
		break;
	}
	case FDBNetworkOptions::TLS_CA_BYTES: {
		validateOptionValuePresent(value);
		tlsConfig.setCABytes(value.get().toString());
		break;
	}
	case FDBNetworkOptions::TLS_PASSWORD:
		validateOptionValuePresent(value);
		tlsConfig.setPassword(value.get().toString());
		break;
	case FDBNetworkOptions::TLS_KEY_PATH:
		validateOptionValuePresent(value);
		tlsConfig.setKeyPath(value.get().toString());
		break;
	case FDBNetworkOptions::TLS_KEY_BYTES: {
		validateOptionValuePresent(value);
		tlsConfig.setKeyBytes(value.get().toString());
		break;
	}
	case FDBNetworkOptions::TLS_VERIFY_PEERS:
		validateOptionValuePresent(value);
		tlsConfig.clearVerifyPeers();
		tlsConfig.addVerifyPeers(value.get().toString());
		break;
	case FDBNetworkOptions::CLIENT_BUGGIFY_ENABLE:
		enableBuggify(true, BuggifyType::Client);
		break;
	case FDBNetworkOptions::CLIENT_BUGGIFY_DISABLE:
		enableBuggify(false, BuggifyType::Client);
		break;
	case FDBNetworkOptions::CLIENT_BUGGIFY_SECTION_ACTIVATED_PROBABILITY:
		validateOptionValuePresent(value);
		clearBuggifySections(BuggifyType::Client);
		P_BUGGIFIED_SECTION_ACTIVATED[int(BuggifyType::Client)] = double(extractIntOption(value, 0, 100)) / 100.0;
		break;
	case FDBNetworkOptions::CLIENT_BUGGIFY_SECTION_FIRED_PROBABILITY:
		validateOptionValuePresent(value);
		P_BUGGIFIED_SECTION_FIRES[int(BuggifyType::Client)] = double(extractIntOption(value, 0, 100)) / 100.0;
		break;
	case FDBNetworkOptions::DISABLE_CLIENT_STATISTICS_LOGGING:
		validateOptionValueNotPresent(value);
		networkOptions.logClientInfo = false;
		break;
	case FDBNetworkOptions::SUPPORTED_CLIENT_VERSIONS: {
		// The multi-version API should be providing us these guarantees
		ASSERT(g_network);
		ASSERT(value.present());

		Standalone<VectorRef<ClientVersionRef>> supportedVersions;
		std::vector<StringRef> supportedVersionsStrings = value.get().splitAny(LiteralStringRef(";"));
		for (StringRef versionString : supportedVersionsStrings) {
			supportedVersions.push_back_deep(supportedVersions.arena(), ClientVersionRef(versionString));
		}

		ASSERT(supportedVersions.size() > 0);
		networkOptions.supportedVersions->set(supportedVersions);

		break;
	}
	case FDBNetworkOptions::ENABLE_RUN_LOOP_PROFILING: // Same as ENABLE_SLOW_TASK_PROFILING
		validateOptionValueNotPresent(value);
		networkOptions.runLoopProfilingEnabled = true;
		break;
	case FDBNetworkOptions::DISTRIBUTED_CLIENT_TRACER: {
		validateOptionValuePresent(value);
		std::string tracer = value.get().toString();
		if (tracer == "none" || tracer == "disabled") {
			openTracer(TracerType::DISABLED);
		} else if (tracer == "logfile" || tracer == "file" || tracer == "log_file") {
			openTracer(TracerType::LOG_FILE);
		} else if (tracer == "network_lossy") {
			openTracer(TracerType::NETWORK_LOSSY);
		} else {
			fprintf(stderr, "ERROR: Unknown or unsupported tracer: `%s'", tracer.c_str());
			throw invalid_option_value();
		}
		break;
	}
	default:
		break;
	}
}

// update the network busyness on a 1s cadence
ACTOR Future<Void> monitorNetworkBusyness() {
	state double prevTime = now();
	loop {
		wait(delay(CLIENT_KNOBS->NETWORK_BUSYNESS_MONITOR_INTERVAL, TaskPriority::FlushTrace));
		double elapsed = now() - prevTime; // get elapsed time from last execution
		prevTime = now();
		struct NetworkMetrics::PriorityStats& tracker = g_network->networkInfo.metrics.starvationTrackerNetworkBusyness;

		if (tracker.active) { // update metrics
			tracker.duration += now() - tracker.windowedTimer;
			tracker.maxDuration = std::max(tracker.maxDuration, now() - tracker.timer);
			tracker.windowedTimer = now();
		}

		double busyFraction = std::min(elapsed, tracker.duration) / elapsed;

		// The burstiness score is an indicator of the maximum busyness spike over the measurement interval.
		// It scales linearly from 0 to 1 as the largest burst goes from the start to the saturation threshold.
		// This allows us to account for saturation that happens in smaller bursts than the measurement interval.
		//
		// Burstiness will not be calculated if the saturation threshold is smaller than the start threshold or
		// if either value is negative.
		double burstiness = 0;
		if (CLIENT_KNOBS->BUSYNESS_SPIKE_START_THRESHOLD >= 0 &&
		    CLIENT_KNOBS->BUSYNESS_SPIKE_SATURATED_THRESHOLD >= CLIENT_KNOBS->BUSYNESS_SPIKE_START_THRESHOLD) {
			burstiness = std::min(1.0,
			                      std::max(0.0, tracker.maxDuration - CLIENT_KNOBS->BUSYNESS_SPIKE_START_THRESHOLD) /
			                          std::max(1e-6,
			                                   CLIENT_KNOBS->BUSYNESS_SPIKE_SATURATED_THRESHOLD -
			                                       CLIENT_KNOBS->BUSYNESS_SPIKE_START_THRESHOLD));
		}

		g_network->networkInfo.metrics.networkBusyness = std::max(busyFraction, burstiness);

		tracker.duration = 0;
		tracker.maxDuration = 0;
	}
}

// Setup g_network and start monitoring for network busyness
void setupNetwork(uint64_t transportId, UseMetrics useMetrics) {
	if (g_network)
		throw network_already_setup();

	if (!networkOptions.logClientInfo.present())
		networkOptions.logClientInfo = true;

	TLS::DisableOpenSSLAtExitHandler();
	g_network = newNet2(tlsConfig, false, useMetrics || networkOptions.traceDirectory.present());
	g_network->addStopCallback(Net2FileSystem::stop);
	g_network->addStopCallback(TLS::DestroyOpenSSLGlobalState);
	FlowTransport::createInstance(true, transportId, WLTOKEN_RESERVED_COUNT);
	Net2FileSystem::newFileSystem();

	uncancellable(monitorNetworkBusyness());
}

void runNetwork() {
	if (!g_network) {
		throw network_not_setup();
	}

	if (!g_network->checkRunnable()) {
		throw network_cannot_be_restarted();
	}

	if (networkOptions.traceDirectory.present() && networkOptions.runLoopProfilingEnabled) {
		setupRunLoopProfiler();
	}

	g_network->run();

	if (networkOptions.traceDirectory.present())
		systemMonitor();
}

void stopNetwork() {
	if (!g_network)
		throw network_not_setup();

	g_network->stop();
	closeTraceFile();
}

void DatabaseContext::updateProxies() {
	if (proxiesLastChange == clientInfo->get().id)
		return;
	proxiesLastChange = clientInfo->get().id;
	commitProxies.clear();
	grvProxies.clear();
	bool commitProxyProvisional = false, grvProxyProvisional = false;
	if (clientInfo->get().commitProxies.size()) {
		commitProxies = makeReference<CommitProxyInfo>(clientInfo->get().commitProxies, false);
		commitProxyProvisional = clientInfo->get().commitProxies[0].provisional;
	}
	if (clientInfo->get().grvProxies.size()) {
		grvProxies = makeReference<GrvProxyInfo>(clientInfo->get().grvProxies, true);
		grvProxyProvisional = clientInfo->get().grvProxies[0].provisional;
	}
	if (clientInfo->get().commitProxies.size() && clientInfo->get().grvProxies.size()) {
		ASSERT(commitProxyProvisional == grvProxyProvisional);
		proxyProvisional = commitProxyProvisional;
	}
}

Reference<CommitProxyInfo> DatabaseContext::getCommitProxies(bool useProvisionalProxies) {
	updateProxies();
	if (proxyProvisional && !useProvisionalProxies) {
		return Reference<CommitProxyInfo>();
	}
	return commitProxies;
}

Reference<GrvProxyInfo> DatabaseContext::getGrvProxies(bool useProvisionalProxies) {
	updateProxies();
	if (proxyProvisional && !useProvisionalProxies) {
		return Reference<GrvProxyInfo>();
	}
	return grvProxies;
}

// Actor which will wait until the MultiInterface<CommitProxyInterface> returned by the DatabaseContext cx is not
// nullptr
ACTOR Future<Reference<CommitProxyInfo>> getCommitProxiesFuture(DatabaseContext* cx, bool useProvisionalProxies) {
	loop {
		Reference<CommitProxyInfo> commitProxies = cx->getCommitProxies(useProvisionalProxies);
		if (commitProxies)
			return commitProxies;
		wait(cx->onProxiesChanged());
	}
}

// Returns a future which will not be set until the CommitProxyInfo of this DatabaseContext is not nullptr
Future<Reference<CommitProxyInfo>> DatabaseContext::getCommitProxiesFuture(bool useProvisionalProxies) {
	return ::getCommitProxiesFuture(this, useProvisionalProxies);
}

void GetRangeLimits::decrement(VectorRef<KeyValueRef> const& data) {
	if (rows != GetRangeLimits::ROW_LIMIT_UNLIMITED) {
		ASSERT(data.size() <= rows);
		rows -= data.size();
	}

	minRows = std::max(0, minRows - data.size());

	if (bytes != GetRangeLimits::BYTE_LIMIT_UNLIMITED)
		bytes = std::max(0, bytes - (int)data.expectedSize() - (8 - (int)sizeof(KeyValueRef)) * data.size());
}

void GetRangeLimits::decrement(KeyValueRef const& data) {
	minRows = std::max(0, minRows - 1);
	if (rows != GetRangeLimits::ROW_LIMIT_UNLIMITED)
		rows--;
	if (bytes != GetRangeLimits::BYTE_LIMIT_UNLIMITED)
		bytes = std::max(0, bytes - (int)8 - (int)data.expectedSize());
}

// True if either the row or byte limit has been reached
bool GetRangeLimits::isReached() {
	return rows == 0 || (bytes == 0 && minRows == 0);
}

// True if data would cause the row or byte limit to be reached
bool GetRangeLimits::reachedBy(VectorRef<KeyValueRef> const& data) {
	return (rows != GetRangeLimits::ROW_LIMIT_UNLIMITED && data.size() >= rows) ||
	       (bytes != GetRangeLimits::BYTE_LIMIT_UNLIMITED &&
	        (int)data.expectedSize() + (8 - (int)sizeof(KeyValueRef)) * data.size() >= bytes && data.size() >= minRows);
}

bool GetRangeLimits::hasByteLimit() {
	return bytes != GetRangeLimits::BYTE_LIMIT_UNLIMITED;
}

bool GetRangeLimits::hasRowLimit() {
	return rows != GetRangeLimits::ROW_LIMIT_UNLIMITED;
}

bool GetRangeLimits::hasSatisfiedMinRows() {
	return hasByteLimit() && minRows == 0;
}

AddressExclusion AddressExclusion::parse(StringRef const& key) {
	// Must not change: serialized to the database!
	auto parsedIp = IPAddress::parse(key.toString());
	if (parsedIp.present()) {
		return AddressExclusion(parsedIp.get());
	}

	// Not a whole machine, includes `port'.
	try {
		auto addr = NetworkAddress::parse(key.toString());
		if (addr.isTLS()) {
			TraceEvent(SevWarnAlways, "AddressExclusionParseError")
			    .detail("String", key)
			    .detail("Description", "Address inclusion string should not include `:tls' suffix.");
			return AddressExclusion();
		}
		return AddressExclusion(addr.ip, addr.port);
	} catch (Error&) {
		TraceEvent(SevWarnAlways, "AddressExclusionParseError").detail("String", key);
		return AddressExclusion();
	}
}

Future<RangeResult> getRange(Database const& cx,
                             Future<Version> const& fVersion,
                             KeySelector const& begin,
                             KeySelector const& end,
                             GetRangeLimits const& limits,
                             Reverse const& reverse,
                             TransactionInfo const& info,
                             TagSet const& tags);

ACTOR Future<Optional<Value>> getValue(Future<Version> version,
                                       Key key,
                                       Database cx,
                                       TransactionInfo info,
                                       Reference<TransactionLogInfo> trLogInfo,
                                       TagSet tags);

ACTOR Future<Optional<StorageServerInterface>> fetchServerInterface(Database cx,
                                                                    TransactionInfo info,
                                                                    UID id,
                                                                    TagSet tags,
                                                                    Future<Version> ver = latestVersion) {
	Optional<Value> val = wait(getValue(ver, serverListKeyFor(id), cx, info, Reference<TransactionLogInfo>(), tags));
	if (!val.present()) {
		// A storage server has been removed from serverList since we read keyServers
		return Optional<StorageServerInterface>();
	}

	return decodeServerListValue(val.get());
}

ACTOR Future<Optional<std::vector<StorageServerInterface>>> transactionalGetServerInterfaces(Future<Version> ver,
                                                                                             Database cx,
                                                                                             TransactionInfo info,
                                                                                             std::vector<UID> ids,
                                                                                             TagSet tags) {
	state std::vector<Future<Optional<StorageServerInterface>>> serverListEntries;
	serverListEntries.reserve(ids.size());
	for (int s = 0; s < ids.size(); s++) {
		serverListEntries.push_back(fetchServerInterface(cx, info, ids[s], tags, ver));
	}

	std::vector<Optional<StorageServerInterface>> serverListValues = wait(getAll(serverListEntries));
	std::vector<StorageServerInterface> serverInterfaces;
	for (int s = 0; s < serverListValues.size(); s++) {
		if (!serverListValues[s].present()) {
			// A storage server has been removed from ServerList since we read keyServers
			return Optional<std::vector<StorageServerInterface>>();
		}
		serverInterfaces.push_back(serverListValues[s].get());
	}
	return serverInterfaces;
}

void updateTssMappings(Database cx, const GetKeyServerLocationsReply& reply) {
	// Since a ss -> tss mapping is included in resultsTssMapping iff that SS is in results and has a tss pair,
	// all SS in results that do not have a mapping present must not have a tss pair.
	std::unordered_map<UID, const StorageServerInterface*> ssiById;
	for (const auto& [_, shard] : reply.results) {
		for (auto& ssi : shard) {
			ssiById[ssi.id()] = &ssi;
		}
	}

	for (const auto& mapping : reply.resultsTssMapping) {
		auto ssi = ssiById.find(mapping.first);
		ASSERT(ssi != ssiById.end());
		cx->addTssMapping(*ssi->second, mapping.second);
		ssiById.erase(mapping.first);
	}

	// if SS didn't have a mapping above, it's still in the ssiById map, so remove its tss mapping
	for (const auto& it : ssiById) {
		cx->removeTssMapping(*it.second);
	}
}

// If isBackward == true, returns the shard containing the key before 'key' (an infinitely long, inexpressible key).
// Otherwise returns the shard containing key
ACTOR Future<std::pair<KeyRange, Reference<LocationInfo>>>
getKeyLocation_internal(Database cx, Key key, TransactionInfo info, Reverse isBackward = Reverse::False) {
	state Span span("NAPI:getKeyLocation"_loc, info.spanID);
	if (isBackward) {
		ASSERT(key != allKeys.begin && key <= allKeys.end);
	} else {
		ASSERT(key < allKeys.end);
	}

	if (info.debugID.present())
		g_traceBatch.addEvent("TransactionDebug", info.debugID.get().first(), "NativeAPI.getKeyLocation.Before");

	loop {
		++cx->transactionKeyServerLocationRequests;
		choose {
			when(wait(cx->onProxiesChanged())) {}
			when(GetKeyServerLocationsReply rep = wait(basicLoadBalance(
			         cx->getCommitProxies(info.useProvisionalProxies),
			         &CommitProxyInterface::getKeyServersLocations,
			         GetKeyServerLocationsRequest(span.context, key, Optional<KeyRef>(), 100, isBackward, key.arena()),
			         TaskPriority::DefaultPromiseEndpoint))) {
				++cx->transactionKeyServerLocationRequestsCompleted;
				if (info.debugID.present())
					g_traceBatch.addEvent(
					    "TransactionDebug", info.debugID.get().first(), "NativeAPI.getKeyLocation.After");
				ASSERT(rep.results.size() == 1);

				auto locationInfo = cx->setCachedLocation(rep.results[0].first, rep.results[0].second);
				updateTssMappings(cx, rep);
				return std::make_pair(KeyRange(rep.results[0].first, rep.arena), locationInfo);
			}
		}
	}
}

template <class F>
Future<std::pair<KeyRange, Reference<LocationInfo>>> getKeyLocation(Database const& cx,
                                                                    Key const& key,
                                                                    F StorageServerInterface::*member,
                                                                    TransactionInfo const& info,
                                                                    Reverse isBackward = Reverse::False) {
	// we first check whether this range is cached
	auto ssi = cx->getCachedLocation(key, isBackward);
	if (!ssi.second) {
		return getKeyLocation_internal(cx, key, info, isBackward);
	}

	for (int i = 0; i < ssi.second->size(); i++) {
		if (IFailureMonitor::failureMonitor().onlyEndpointFailed(ssi.second->get(i, member).getEndpoint())) {
			cx->invalidateCache(key);
			ssi.second.clear();
			return getKeyLocation_internal(cx, key, info, isBackward);
		}
	}

	return ssi;
}

ACTOR Future<std::vector<std::pair<KeyRange, Reference<LocationInfo>>>>
getKeyRangeLocations_internal(Database cx, KeyRange keys, int limit, Reverse reverse, TransactionInfo info) {
	state Span span("NAPI:getKeyRangeLocations"_loc, info.spanID);
	if (info.debugID.present())
		g_traceBatch.addEvent("TransactionDebug", info.debugID.get().first(), "NativeAPI.getKeyLocations.Before");

	loop {
		++cx->transactionKeyServerLocationRequests;
		choose {
			when(wait(cx->onProxiesChanged())) {}
			when(GetKeyServerLocationsReply _rep = wait(basicLoadBalance(
			         cx->getCommitProxies(info.useProvisionalProxies),
			         &CommitProxyInterface::getKeyServersLocations,
			         GetKeyServerLocationsRequest(span.context, keys.begin, keys.end, limit, reverse, keys.arena()),
			         TaskPriority::DefaultPromiseEndpoint))) {
				++cx->transactionKeyServerLocationRequestsCompleted;
				state GetKeyServerLocationsReply rep = _rep;
				if (info.debugID.present())
					g_traceBatch.addEvent(
					    "TransactionDebug", info.debugID.get().first(), "NativeAPI.getKeyLocations.After");
				ASSERT(rep.results.size());

				state std::vector<std::pair<KeyRange, Reference<LocationInfo>>> results;
				state int shard = 0;
				for (; shard < rep.results.size(); shard++) {
					// FIXME: these shards are being inserted into the map sequentially, it would be much more CPU
					// efficient to save the map pairs and insert them all at once.
					results.emplace_back(rep.results[shard].first & keys,
					                     cx->setCachedLocation(rep.results[shard].first, rep.results[shard].second));
					wait(yield());
				}
				updateTssMappings(cx, rep);

				return results;
			}
		}
	}
}

// Get the SS locations for each shard in the 'keys' key-range;
// Returned vector size is the number of shards in the input keys key-range.
// Returned vector element is <ShardRange, storage server location info> pairs, where
// ShardRange is the whole shard key-range, not a part of the given key range.
// Example: If query the function with  key range (b, d), the returned list of pairs could be something like:
// [([a, b1), locationInfo), ([b1, c), locationInfo), ([c, d1), locationInfo)].
template <class F>
Future<std::vector<std::pair<KeyRange, Reference<LocationInfo>>>> getKeyRangeLocations(
    Database const& cx,
    KeyRange const& keys,
    int limit,
    Reverse reverse,
    F StorageServerInterface::*member,
    TransactionInfo const& info) {
	ASSERT(!keys.empty());

	std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations;
	if (!cx->getCachedLocations(keys, locations, limit, reverse)) {
		return getKeyRangeLocations_internal(cx, keys, limit, reverse, info);
	}

	bool foundFailed = false;
	for (const auto& [range, locInfo] : locations) {
		bool onlyEndpointFailed = false;
		for (int i = 0; i < locInfo->size(); i++) {
			if (IFailureMonitor::failureMonitor().onlyEndpointFailed(locInfo->get(i, member).getEndpoint())) {
				onlyEndpointFailed = true;
				break;
			}
		}

		if (onlyEndpointFailed) {
			cx->invalidateCache(range.begin);
			foundFailed = true;
		}
	}

	if (foundFailed) {
		return getKeyRangeLocations_internal(cx, keys, limit, reverse, info);
	}

	return locations;
}

ACTOR Future<Void> warmRange_impl(Transaction* self, Database cx, KeyRange keys) {
	state int totalRanges = 0;
	state int totalRequests = 0;
	loop {
		std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations = wait(
		    getKeyRangeLocations_internal(cx, keys, CLIENT_KNOBS->WARM_RANGE_SHARD_LIMIT, Reverse::False, self->info));
		totalRanges += CLIENT_KNOBS->WARM_RANGE_SHARD_LIMIT;
		totalRequests++;
		if (locations.size() == 0 || totalRanges >= cx->locationCacheSize ||
		    locations[locations.size() - 1].first.end >= keys.end)
			break;

		keys = KeyRangeRef(locations[locations.size() - 1].first.end, keys.end);

		if (totalRequests % 20 == 0) {
			// To avoid blocking the proxies from starting other transactions, occasionally get a read version.
			state Transaction tr(cx);
			loop {
				try {
					tr.setOption(FDBTransactionOptions::LOCK_AWARE);
					tr.setOption(FDBTransactionOptions::CAUSAL_READ_RISKY);
					wait(success(tr.getReadVersion()));
					break;
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
		}
	}

	return Void();
}

Future<Void> Transaction::warmRange(Database cx, KeyRange keys) {
	return warmRange_impl(this, cx, keys);
}

ACTOR Future<Optional<Value>> getValue(Future<Version> version,
                                       Key key,
                                       Database cx,
                                       TransactionInfo info,
                                       Reference<TransactionLogInfo> trLogInfo,
                                       TagSet tags) {
	state Version ver = wait(version);
	state Span span("NAPI:getValue"_loc, info.spanID);
	span.addTag("key"_sr, key);
	cx->validateVersion(ver);

	loop {
		state std::pair<KeyRange, Reference<LocationInfo>> ssi =
		    wait(getKeyLocation(cx, key, &StorageServerInterface::getValue, info));
		state Optional<UID> getValueID = Optional<UID>();
		state uint64_t startTime;
		state double startTimeD;
		try {
			if (info.debugID.present()) {
				getValueID = nondeterministicRandom()->randomUniqueID();

				g_traceBatch.addAttach("GetValueAttachID", info.debugID.get().first(), getValueID.get().first());
				g_traceBatch.addEvent("GetValueDebug",
				                      getValueID.get().first(),
				                      "NativeAPI.getValue.Before"); //.detail("TaskID", g_network->getCurrentTask());
				/*TraceEvent("TransactionDebugGetValueInfo", getValueID.get())
				    .detail("Key", key)
				    .detail("ReqVersion", ver)
				    .detail("Servers", describe(ssi.second->get()));*/
			}

			++cx->getValueSubmitted;
			startTime = timer_int();
			startTimeD = now();
			++cx->transactionPhysicalReads;

			state GetValueReply reply;
			try {
				if (CLIENT_BUGGIFY_WITH_PROB(.01)) {
					throw deterministicRandom()->randomChoice(
					    std::vector<Error>{ transaction_too_old(), future_version() });
				}
				choose {
					when(wait(cx->connectionFileChanged())) { throw transaction_too_old(); }
					when(GetValueReply _reply = wait(loadBalance(
					         cx.getPtr(),
					         ssi.second,
					         &StorageServerInterface::getValue,
					         GetValueRequest(
					             span.context, key, ver, cx->sampleReadTags() ? tags : Optional<TagSet>(), getValueID),
					         TaskPriority::DefaultPromiseEndpoint,
					         AtMostOnce::False,
					         cx->enableLocalityLoadBalance ? &cx->queueModel : nullptr))) {
						reply = _reply;
					}
				}
				++cx->transactionPhysicalReadsCompleted;
			} catch (Error&) {
				++cx->transactionPhysicalReadsCompleted;
				throw;
			}

			double latency = now() - startTimeD;
			cx->readLatencies.addSample(latency);
			if (trLogInfo) {
				int valueSize = reply.value.present() ? reply.value.get().size() : 0;
				trLogInfo->addLog(
				    FdbClientLogEvents::EventGet(startTimeD, cx->clientLocality.dcId(), latency, valueSize, key));
			}
			cx->getValueCompleted->latency = timer_int() - startTime;
			cx->getValueCompleted->log();

			if (info.debugID.present()) {
				g_traceBatch.addEvent("GetValueDebug",
				                      getValueID.get().first(),
				                      "NativeAPI.getValue.After"); //.detail("TaskID", g_network->getCurrentTask());
				/*TraceEvent("TransactionDebugGetValueDone", getValueID.get())
				    .detail("Key", key)
				    .detail("ReqVersion", ver)
				    .detail("ReplySize", reply.value.present() ? reply.value.get().size() : -1);*/
			}

			cx->transactionBytesRead += reply.value.present() ? reply.value.get().size() : 0;
			++cx->transactionKeysRead;
			return reply.value;
		} catch (Error& e) {
			cx->getValueCompleted->latency = timer_int() - startTime;
			cx->getValueCompleted->log();
			if (info.debugID.present()) {
				g_traceBatch.addEvent("GetValueDebug",
				                      getValueID.get().first(),
				                      "NativeAPI.getValue.Error"); //.detail("TaskID", g_network->getCurrentTask());
				/*TraceEvent("TransactionDebugGetValueDone", getValueID.get())
				    .detail("Key", key)
				    .detail("ReqVersion", ver)
				    .detail("ReplySize", reply.value.present() ? reply.value.get().size() : -1);*/
			}
			if (e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed ||
			    (e.code() == error_code_transaction_too_old && ver == latestVersion)) {
				cx->invalidateCache(key);
				wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, info.taskID));
			} else {
				if (trLogInfo)
					trLogInfo->addLog(FdbClientLogEvents::EventGetError(
					    startTimeD, cx->clientLocality.dcId(), static_cast<int>(e.code()), key));
				throw e;
			}
		}
	}
}

ACTOR Future<Key> getKey(Database cx, KeySelector k, Future<Version> version, TransactionInfo info, TagSet tags) {
	wait(success(version));

	state Optional<UID> getKeyID = Optional<UID>();
	state Span span("NAPI:getKey"_loc, info.spanID);
	if (info.debugID.present()) {
		getKeyID = nondeterministicRandom()->randomUniqueID();

		g_traceBatch.addAttach("GetKeyAttachID", info.debugID.get().first(), getKeyID.get().first());
		g_traceBatch.addEvent(
		    "GetKeyDebug",
		    getKeyID.get().first(),
		    "NativeAPI.getKey.AfterVersion"); //.detail("StartKey",
		                                      // k.getKey()).detail("Offset",k.offset).detail("OrEqual",k.orEqual);
	}

	loop {
		if (k.getKey() == allKeys.end) {
			if (k.offset > 0)
				return allKeys.end;
			k.orEqual = false;
		} else if (k.getKey() == allKeys.begin && k.offset <= 0) {
			return Key();
		}

		Key locationKey(k.getKey(), k.arena());
		state std::pair<KeyRange, Reference<LocationInfo>> ssi =
		    wait(getKeyLocation(cx, locationKey, &StorageServerInterface::getKey, info, Reverse{ k.isBackward() }));

		try {
			if (info.debugID.present())
				g_traceBatch.addEvent(
				    "GetKeyDebug",
				    getKeyID.get().first(),
				    "NativeAPI.getKey.Before"); //.detail("StartKey",
				                                // k.getKey()).detail("Offset",k.offset).detail("OrEqual",k.orEqual);
			++cx->transactionPhysicalReads;

			GetKeyRequest req(
			    span.context, k, version.get(), cx->sampleReadTags() ? tags : Optional<TagSet>(), getKeyID);
			req.arena.dependsOn(k.arena());

			state GetKeyReply reply;
			try {
				choose {
					when(wait(cx->connectionFileChanged())) { throw transaction_too_old(); }
					when(GetKeyReply _reply =
					         wait(loadBalance(cx.getPtr(),
					                          ssi.second,
					                          &StorageServerInterface::getKey,
					                          req,
					                          TaskPriority::DefaultPromiseEndpoint,
					                          AtMostOnce::False,
					                          cx->enableLocalityLoadBalance ? &cx->queueModel : nullptr))) {
						reply = _reply;
					}
				}
				++cx->transactionPhysicalReadsCompleted;
			} catch (Error&) {
				++cx->transactionPhysicalReadsCompleted;
				throw;
			}
			if (info.debugID.present())
				g_traceBatch.addEvent("GetKeyDebug",
				                      getKeyID.get().first(),
				                      "NativeAPI.getKey.After"); //.detail("NextKey",reply.sel.key).detail("Offset",
				                                                 // reply.sel.offset).detail("OrEqual", k.orEqual);
			k = reply.sel;
			if (!k.offset && k.orEqual) {
				return k.getKey();
			}
		} catch (Error& e) {
			if (info.debugID.present())
				g_traceBatch.addEvent("GetKeyDebug", getKeyID.get().first(), "NativeAPI.getKey.Error");
			if (e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed) {
				cx->invalidateCache(k.getKey(), Reverse{ k.isBackward() });

				wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, info.taskID));
			} else {
				TraceEvent(SevInfo, "GetKeyError").error(e).detail("AtKey", k.getKey()).detail("Offset", k.offset);
				throw e;
			}
		}
	}
}

ACTOR Future<Version> waitForCommittedVersion(Database cx, Version version, SpanID spanContext) {
	state Span span("NAPI:waitForCommittedVersion"_loc, { spanContext });
	try {
		loop {
			choose {
				when(wait(cx->onProxiesChanged())) {}
				when(GetReadVersionReply v =
				         wait(basicLoadBalance(cx->getGrvProxies(false),
				                               &GrvProxyInterface::getConsistentReadVersion,
				                               GetReadVersionRequest(span.context, 0, TransactionPriority::IMMEDIATE),
				                               cx->taskID))) {
					cx->minAcceptableReadVersion = std::min(cx->minAcceptableReadVersion, v.version);
					if (v.midShardSize > 0)
						cx->smoothMidShardSize.setTotal(v.midShardSize);
					if (v.version >= version)
						return v.version;
					// SOMEDAY: Do the wait on the server side, possibly use less expensive source of committed version
					// (causal consistency is not needed for this purpose)
					wait(delay(CLIENT_KNOBS->FUTURE_VERSION_RETRY_DELAY, cx->taskID));
				}
			}
		}
	} catch (Error& e) {
		TraceEvent(SevError, "WaitForCommittedVersionError").error(e);
		throw;
	}
}

ACTOR Future<Version> getRawVersion(Database cx, SpanID spanContext) {
	state Span span("NAPI:getRawVersion"_loc, { spanContext });
	loop {
		choose {
			when(wait(cx->onProxiesChanged())) {}
			when(GetReadVersionReply v =
			         wait(basicLoadBalance(cx->getGrvProxies(false),
			                               &GrvProxyInterface::getConsistentReadVersion,
			                               GetReadVersionRequest(spanContext, 0, TransactionPriority::IMMEDIATE),
			                               cx->taskID))) {
				return v.version;
			}
		}
	}
}

ACTOR Future<Void> readVersionBatcher(
    DatabaseContext* cx,
    FutureStream<std::pair<Promise<GetReadVersionReply>, Optional<UID>>> versionStream,
    uint32_t flags);

ACTOR Future<Version> watchValue(Future<Version> version,
                                 Key key,
                                 Optional<Value> value,
                                 Database cx,
                                 TransactionInfo info,
                                 TagSet tags) {
	state Version ver = wait(version);
	state Span span("NAPI:watchValue"_loc, info.spanID);
	cx->validateVersion(ver);
	ASSERT(ver != latestVersion);

	loop {
		state std::pair<KeyRange, Reference<LocationInfo>> ssi =
		    wait(getKeyLocation(cx, key, &StorageServerInterface::watchValue, info));

		try {
			state Optional<UID> watchValueID = Optional<UID>();
			if (info.debugID.present()) {
				watchValueID = nondeterministicRandom()->randomUniqueID();

				g_traceBatch.addAttach("WatchValueAttachID", info.debugID.get().first(), watchValueID.get().first());
				g_traceBatch.addEvent("WatchValueDebug",
				                      watchValueID.get().first(),
				                      "NativeAPI.watchValue.Before"); //.detail("TaskID", g_network->getCurrentTask());
			}
			state WatchValueReply resp;
			choose {
				when(WatchValueReply r =
				         wait(loadBalance(cx.getPtr(),
				                          ssi.second,
				                          &StorageServerInterface::watchValue,
				                          WatchValueRequest(span.context,
				                                            key,
				                                            value,
				                                            ver,
				                                            cx->sampleReadTags() ? tags : Optional<TagSet>(),
				                                            watchValueID),
				                          TaskPriority::DefaultPromiseEndpoint))) {
					resp = r;
				}
				when(wait(cx->connectionFile ? cx->connectionFile->onChange() : Never())) { wait(Never()); }
			}
			if (info.debugID.present()) {
				g_traceBatch.addEvent("WatchValueDebug",
				                      watchValueID.get().first(),
				                      "NativeAPI.watchValue.After"); //.detail("TaskID", g_network->getCurrentTask());
			}

			// FIXME: wait for known committed version on the storage server before replying,
			// cannot do this until the storage server is notified on knownCommittedVersion changes from tlog (faster
			// than the current update loop)
			Version v = wait(waitForCommittedVersion(cx, resp.version, span.context));

			//TraceEvent("WatcherCommitted").detail("CommittedVersion", v).detail("WatchVersion", resp.version).detail("Key",  key ).detail("Value", value);

			// False if there is a master failure between getting the response and getting the committed version,
			// Dependent on SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT
			if (v - resp.version < 50000000) {
				return resp.version;
			}
			ver = v;
		} catch (Error& e) {
			if (e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed) {
				cx->invalidateCache(key);
				wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, info.taskID));
			} else if (e.code() == error_code_watch_cancelled || e.code() == error_code_process_behind) {
				// clang-format off
				TEST(e.code() == error_code_watch_cancelled); // Too many watches on the storage server, poll for changes instead
				TEST(e.code() == error_code_process_behind); // The storage servers are all behind
				// clang-format on
				wait(delay(CLIENT_KNOBS->WATCH_POLLING_TIME, info.taskID));
			} else if (e.code() == error_code_timed_out) { // The storage server occasionally times out watches in case
				                                           // it was cancelled
				TEST(true); // A watch timed out
				wait(delay(CLIENT_KNOBS->FUTURE_VERSION_RETRY_DELAY, info.taskID));
			} else {
				state Error err = e;
				wait(delay(CLIENT_KNOBS->FUTURE_VERSION_RETRY_DELAY, info.taskID));
				throw err;
			}
		}
	}
}

ACTOR Future<Void> watchStorageServerResp(KeyRef key, Database cx) {
	loop {
		try {
			state Reference<WatchMetadata> metadata = cx->getWatchMetadata(key);
			if (!metadata.isValid())
				return Void();

			Version watchVersion = wait(watchValue(Future<Version>(metadata->version),
			                                       metadata->key,
			                                       metadata->value,
			                                       cx,
			                                       metadata->info,
			                                       metadata->tags));

			metadata = cx->getWatchMetadata(key);
			if (!metadata.isValid())
				return Void();

			if (watchVersion >= metadata->version) { // case 1: version_1 (SS) >= version_2 (map)
				cx->deleteWatchMetadata(key);
				if (metadata->watchPromise.canBeSet())
					metadata->watchPromise.send(watchVersion);
			} else { // ABA happens
				TEST(true); // ABA issue where the version returned from the server is less than the version in the map
				if (metadata->watchPromise.getFutureReferenceCount() ==
				    1) { // case 2: version_1 < version_2 and future_count == 1
					cx->deleteWatchMetadata(key);
				}
			}
		} catch (Error& e) {
			if (e.code() == error_code_operation_cancelled) {
				throw e;
			}

			Reference<WatchMetadata> metadata = cx->getWatchMetadata(key);
			if (!metadata.isValid()) {
				return Void();
			} else if (metadata->watchPromise.getFutureReferenceCount() == 1) {
				cx->deleteWatchMetadata(key);
				return Void();
			} else if (e.code() == error_code_future_version) {
				continue;
			}
			cx->deleteWatchMetadata(key);
			metadata->watchPromise.sendError(e);
			throw e;
		}
	}
}

ACTOR Future<Void> sameVersionDiffValue(Version ver,
                                        Key key,
                                        Optional<Value> value,
                                        Database cx,
                                        TransactionInfo info,
                                        TagSet tags) {
	state ReadYourWritesTransaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			state Optional<Value> valSS = wait(tr.get(key));
			Reference<WatchMetadata> metadata = cx->getWatchMetadata(key.contents());

			if (metadata.isValid() &&
			    valSS != metadata->value) { // val_3 != val_1 (storage server value doesnt match value in map)
				cx->deleteWatchMetadata(key.contents());

				metadata->watchPromise.send(ver);
				metadata->watchFutureSS.cancel();
			}

			if (valSS ==
			    value) { // val_3 == val_2 (storage server value matches value passed into the function -> new watch)
				metadata = makeReference<WatchMetadata>(key, value, ver, info, tags);
				KeyRef keyRef = cx->setWatchMetadata(metadata);

				metadata->watchFutureSS = watchStorageServerResp(keyRef, cx);
			}

			if (valSS != value)
				return Void(); // if val_3 != val_2

			wait(success(metadata->watchPromise.getFuture())); // val_3 == val_2

			return Void();
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

Future<Void> getWatchFuture(Version ver,
                            Key key,
                            Optional<Value> value,
                            Database cx,
                            TransactionInfo info,
                            TagSet tags) {
	Reference<WatchMetadata> metadata = cx->getWatchMetadata(key.contents());

	if (!metadata.isValid()) { // case 1: key not in map
		metadata = makeReference<WatchMetadata>(key, value, ver, info, tags);
		KeyRef keyRef = cx->setWatchMetadata(metadata);

		metadata->watchFutureSS = watchStorageServerResp(keyRef, cx);
		return success(metadata->watchPromise.getFuture());
	} else if (metadata->value == value) { // case 2: val_1 == val_2 (received watch with same value as key already in
		                                   // the map so just update)
		if (ver > metadata->version) {
			metadata->version = ver;
			metadata->info = info;
			metadata->tags = tags;
		}

		return success(metadata->watchPromise.getFuture());
	} else if (ver > metadata->version) { // case 3: val_1 != val_2 && version_2 > version_1 (recived watch with
		                                  // different value and a higher version so recreate in SS)
		TEST(true); // Setting a watch that has a different value than the one in the map but a higher version (newer)
		cx->deleteWatchMetadata(key.contents());

		metadata->watchPromise.send(ver);
		metadata->watchFutureSS.cancel();

		metadata = makeReference<WatchMetadata>(key, value, ver, info, tags);
		KeyRef keyRef = cx->setWatchMetadata(metadata);

		metadata->watchFutureSS = watchStorageServerResp(keyRef, cx);

		return success(metadata->watchPromise.getFuture());
	} else if (metadata->version == ver) { // case 5: val_1 != val_2 && version_1 == version_2 (recived watch with
		                                   // different value but same version)
		TEST(true); // Setting a watch which has a different value than the one in the map but the same version
		return sameVersionDiffValue(ver, key, value, cx, info, tags);
	}
	TEST(true); // Setting a watch which has a different value than the one in the map but a lower version (older)
	// case 4: val_1 != val_2 && version_2 < version_1
	return Void();
}

ACTOR Future<Void> watchValueMap(Future<Version> version,
                                 Key key,
                                 Optional<Value> value,
                                 Database cx,
                                 TransactionInfo info,
                                 TagSet tags) {
	state Version ver = wait(version);
	wait(getWatchFuture(ver, key, value, cx, info, tags));
	return Void();
}

void transformRangeLimits(GetRangeLimits limits, Reverse reverse, GetKeyValuesRequest& req) {
	if (limits.bytes != 0) {
		if (!limits.hasRowLimit())
			req.limit = CLIENT_KNOBS->REPLY_BYTE_LIMIT; // Can't get more than this many rows anyway
		else
			req.limit = std::min(CLIENT_KNOBS->REPLY_BYTE_LIMIT, limits.rows);

		if (reverse)
			req.limit *= -1;

		if (!limits.hasByteLimit())
			req.limitBytes = CLIENT_KNOBS->REPLY_BYTE_LIMIT;
		else
			req.limitBytes = std::min(CLIENT_KNOBS->REPLY_BYTE_LIMIT, limits.bytes);
	} else {
		req.limitBytes = CLIENT_KNOBS->REPLY_BYTE_LIMIT;
		req.limit = reverse ? -limits.minRows : limits.minRows;
	}
}

ACTOR Future<RangeResult> getExactRange(Database cx,
                                        Version version,
                                        KeyRange keys,
                                        GetRangeLimits limits,
                                        Reverse reverse,
                                        TransactionInfo info,
                                        TagSet tags) {
	state RangeResult output;
	state Span span("NAPI:getExactRange"_loc, info.spanID);

	// printf("getExactRange( '%s', '%s' )\n", keys.begin.toString().c_str(), keys.end.toString().c_str());
	loop {
		state std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations = wait(getKeyRangeLocations(
		    cx, keys, CLIENT_KNOBS->GET_RANGE_SHARD_LIMIT, reverse, &StorageServerInterface::getKeyValues, info));
		ASSERT(locations.size());
		state int shard = 0;
		loop {
			const KeyRangeRef& range = locations[shard].first;

			GetKeyValuesRequest req;
			req.version = version;
			req.begin = firstGreaterOrEqual(range.begin);
			req.end = firstGreaterOrEqual(range.end);
			req.spanContext = span.context;

			// keep shard's arena around in case of async tss comparison
			req.arena.dependsOn(locations[shard].first.arena());

			transformRangeLimits(limits, reverse, req);
			ASSERT(req.limitBytes > 0 && req.limit != 0 && req.limit < 0 == reverse);

			// FIXME: buggify byte limits on internal functions that use them, instead of globally
			req.tags = cx->sampleReadTags() ? tags : Optional<TagSet>();
			req.debugID = info.debugID;

			try {
				if (info.debugID.present()) {
					g_traceBatch.addEvent(
					    "TransactionDebug", info.debugID.get().first(), "NativeAPI.getExactRange.Before");
					/*TraceEvent("TransactionDebugGetExactRangeInfo", info.debugID.get())
					    .detail("ReqBeginKey", req.begin.getKey())
					    .detail("ReqEndKey", req.end.getKey())
					    .detail("ReqLimit", req.limit)
					    .detail("ReqLimitBytes", req.limitBytes)
					    .detail("ReqVersion", req.version)
					    .detail("Reverse", reverse)
					    .detail("Servers", locations[shard].second->description());*/
				}
				++cx->transactionPhysicalReads;
				state GetKeyValuesReply rep;
				try {
					choose {
						when(wait(cx->connectionFileChanged())) { throw transaction_too_old(); }
						when(GetKeyValuesReply _rep =
						         wait(loadBalance(cx.getPtr(),
						                          locations[shard].second,
						                          &StorageServerInterface::getKeyValues,
						                          req,
						                          TaskPriority::DefaultPromiseEndpoint,
						                          AtMostOnce::False,
						                          cx->enableLocalityLoadBalance ? &cx->queueModel : nullptr))) {
							rep = _rep;
						}
					}
					++cx->transactionPhysicalReadsCompleted;
				} catch (Error&) {
					++cx->transactionPhysicalReadsCompleted;
					throw;
				}
				if (info.debugID.present())
					g_traceBatch.addEvent(
					    "TransactionDebug", info.debugID.get().first(), "NativeAPI.getExactRange.After");
				output.arena().dependsOn(rep.arena);
				output.append(output.arena(), rep.data.begin(), rep.data.size());

				if (limits.hasRowLimit() && rep.data.size() > limits.rows) {
					TraceEvent(SevError, "GetExactRangeTooManyRows")
					    .detail("RowLimit", limits.rows)
					    .detail("DeliveredRows", output.size());
					ASSERT(false);
				}
				limits.decrement(rep.data);

				if (limits.isReached()) {
					output.more = true;
					return output;
				}

				bool more = rep.more;
				// If the reply says there is more but we know that we finished the shard, then fix rep.more
				if (reverse && more && rep.data.size() > 0 &&
				    output[output.size() - 1].key == locations[shard].first.begin)
					more = false;

				if (more) {
					if (!rep.data.size()) {
						TraceEvent(SevError, "GetExactRangeError")
						    .detail("Reason", "More data indicated but no rows present")
						    .detail("LimitBytes", limits.bytes)
						    .detail("LimitRows", limits.rows)
						    .detail("OutputSize", output.size())
						    .detail("OutputBytes", output.expectedSize())
						    .detail("BlockSize", rep.data.size())
						    .detail("BlockBytes", rep.data.expectedSize());
						ASSERT(false);
					}
					TEST(true); // GetKeyValuesReply.more in getExactRange
					// Make next request to the same shard with a beginning key just after the last key returned
					if (reverse)
						locations[shard].first =
						    KeyRangeRef(locations[shard].first.begin, output[output.size() - 1].key);
					else
						locations[shard].first =
						    KeyRangeRef(keyAfter(output[output.size() - 1].key), locations[shard].first.end);
				}

				if (!more || locations[shard].first.empty()) {
					TEST(true); // getExactrange (!more || locations[shard].first.empty())
					if (shard == locations.size() - 1) {
						const KeyRangeRef& range = locations[shard].first;
						KeyRef begin = reverse ? keys.begin : range.end;
						KeyRef end = reverse ? range.begin : keys.end;

						if (begin >= end) {
							output.more = false;
							return output;
						}
						TEST(true); // Multiple requests of key locations

						keys = KeyRangeRef(begin, end);
						break;
					}

					++shard;
				}

				// Soft byte limit - return results early if the user specified a byte limit and we got results
				// This can prevent problems where the desired range spans many shards and would be too slow to
				// fetch entirely.
				if (limits.hasSatisfiedMinRows() && output.size() > 0) {
					output.more = true;
					return output;
				}

			} catch (Error& e) {
				if (e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed) {
					const KeyRangeRef& range = locations[shard].first;

					if (reverse)
						keys = KeyRangeRef(keys.begin, range.end);
					else
						keys = KeyRangeRef(range.begin, keys.end);

					cx->invalidateCache(keys);
					wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, info.taskID));
					break;
				} else {
					TraceEvent(SevInfo, "GetExactRangeError")
					    .error(e)
					    .detail("ShardBegin", locations[shard].first.begin)
					    .detail("ShardEnd", locations[shard].first.end);
					throw;
				}
			}
		}
	}
}

Future<Key> resolveKey(Database const& cx,
                       KeySelector const& key,
                       Version const& version,
                       TransactionInfo const& info,
                       TagSet const& tags) {
	if (key.isFirstGreaterOrEqual())
		return Future<Key>(key.getKey());

	if (key.isFirstGreaterThan())
		return Future<Key>(keyAfter(key.getKey()));

	return getKey(cx, key, version, info, tags);
}

ACTOR Future<RangeResult> getRangeFallback(Database cx,
                                           Version version,
                                           KeySelector begin,
                                           KeySelector end,
                                           GetRangeLimits limits,
                                           Reverse reverse,
                                           TransactionInfo info,
                                           TagSet tags) {
	if (version == latestVersion) {
		state Transaction transaction(cx);
		transaction.setOption(FDBTransactionOptions::CAUSAL_READ_RISKY);
		transaction.setOption(FDBTransactionOptions::LOCK_AWARE);
		transaction.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
		Version ver = wait(transaction.getReadVersion());
		version = ver;
	}

	Future<Key> fb = resolveKey(cx, begin, version, info, tags);
	state Future<Key> fe = resolveKey(cx, end, version, info, tags);

	state Key b = wait(fb);
	state Key e = wait(fe);
	if (b >= e) {
		return RangeResult();
	}

	// if e is allKeys.end, we have read through the end of the database
	// if b is allKeys.begin, we have either read through the beginning of the database,
	// or allKeys.begin exists in the database and will be part of the conflict range anyways

	RangeResult _r = wait(getExactRange(cx, version, KeyRangeRef(b, e), limits, reverse, info, tags));
	RangeResult r = _r;

	if (b == allKeys.begin && ((reverse && !r.more) || !reverse))
		r.readToBegin = true;
	if (e == allKeys.end && ((!reverse && !r.more) || reverse))
		r.readThroughEnd = true;

	ASSERT(!limits.hasRowLimit() || r.size() <= limits.rows);

	// If we were limiting bytes and the returned range is twice the request (plus 10K) log a warning
	if (limits.hasByteLimit() &&
	    r.expectedSize() >
	        size_t(limits.bytes + CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT + CLIENT_KNOBS->VALUE_SIZE_LIMIT + 1) &&
	    limits.minRows == 0) {
		TraceEvent(SevWarnAlways, "GetRangeFallbackTooMuchData")
		    .detail("LimitBytes", limits.bytes)
		    .detail("DeliveredBytes", r.expectedSize())
		    .detail("LimitRows", limits.rows)
		    .detail("DeliveredRows", r.size());
	}

	return r;
}

void getRangeFinished(Database cx,
                      Reference<TransactionLogInfo> trLogInfo,
                      double startTime,
                      KeySelector begin,
                      KeySelector end,
                      Snapshot snapshot,
                      Promise<std::pair<Key, Key>> conflictRange,
                      Reverse reverse,
                      RangeResult result) {
	int64_t bytes = 0;
	for (const KeyValueRef& kv : result) {
		bytes += kv.key.size() + kv.value.size();
	}

	cx->transactionBytesRead += bytes;
	cx->transactionKeysRead += result.size();

	if (trLogInfo) {
		trLogInfo->addLog(FdbClientLogEvents::EventGetRange(
		    startTime, cx->clientLocality.dcId(), now() - startTime, bytes, begin.getKey(), end.getKey()));
	}

	if (!snapshot) {
		Key rangeBegin;
		Key rangeEnd;

		if (result.readToBegin) {
			rangeBegin = allKeys.begin;
		} else if (((!reverse || !result.more || begin.offset > 1) && begin.offset > 0) || result.size() == 0) {
			rangeBegin = Key(begin.getKey(), begin.arena());
		} else {
			rangeBegin = reverse ? result.end()[-1].key : result[0].key;
		}

		if (end.offset > begin.offset && end.getKey() < rangeBegin) {
			rangeBegin = Key(end.getKey(), end.arena());
		}

		if (result.readThroughEnd) {
			rangeEnd = allKeys.end;
		} else if (((reverse || !result.more || end.offset <= 0) && end.offset <= 1) || result.size() == 0) {
			rangeEnd = Key(end.getKey(), end.arena());
		} else {
			rangeEnd = keyAfter(reverse ? result[0].key : result.end()[-1].key);
		}

		if (begin.offset < end.offset && begin.getKey() > rangeEnd) {
			rangeEnd = Key(begin.getKey(), begin.arena());
		}

		conflictRange.send(std::make_pair(rangeBegin, rangeEnd));
	}
}

ACTOR Future<RangeResult> getRange(Database cx,
                                   Reference<TransactionLogInfo> trLogInfo,
                                   Future<Version> fVersion,
                                   KeySelector begin,
                                   KeySelector end,
                                   GetRangeLimits limits,
                                   Promise<std::pair<Key, Key>> conflictRange,
                                   Snapshot snapshot,
                                   Reverse reverse,
                                   TransactionInfo info,
                                   TagSet tags) {
	state GetRangeLimits originalLimits(limits);
	state KeySelector originalBegin = begin;
	state KeySelector originalEnd = end;
	state RangeResult output;
	state Span span("NAPI:getRange"_loc, info.spanID);

	try {
		state Version version = wait(fVersion);
		cx->validateVersion(version);

		state double startTime = now();
		state Version readVersion = version; // Needed for latestVersion requests; if more, make future requests at the
		                                     // version that the first one completed
		                                     // FIXME: Is this really right?  Weaken this and see if there is a problem;
		                                     // if so maybe there is a much subtler problem even with this.

		if (begin.getKey() == allKeys.begin && begin.offset < 1) {
			output.readToBegin = true;
			begin = KeySelector(firstGreaterOrEqual(begin.getKey()), begin.arena());
		}

		ASSERT(!limits.isReached());
		ASSERT((!limits.hasRowLimit() || limits.rows >= limits.minRows) && limits.minRows >= 0);

		loop {
			if (end.getKey() == allKeys.begin && (end.offset < 1 || end.isFirstGreaterOrEqual())) {
				getRangeFinished(
				    cx, trLogInfo, startTime, originalBegin, originalEnd, snapshot, conflictRange, reverse, output);
				return output;
			}

			Key locationKey = reverse ? Key(end.getKey(), end.arena()) : Key(begin.getKey(), begin.arena());
			Reverse locationBackward{ reverse ? (end - 1).isBackward() : begin.isBackward() };
			state std::pair<KeyRange, Reference<LocationInfo>> beginServer =
			    wait(getKeyLocation(cx, locationKey, &StorageServerInterface::getKeyValues, info, locationBackward));
			state KeyRange shard = beginServer.first;
			state bool modifiedSelectors = false;
			state GetKeyValuesRequest req;

			req.isFetchKeys = (info.taskID == TaskPriority::FetchKeys);
			req.version = readVersion;

			// In case of async tss comparison, also make req arena depend on begin, end, and/or shard's arena depending
			// on which  is used
			bool dependOnShard = false;
			if (reverse && (begin - 1).isDefinitelyLess(shard.begin) &&
			    (!begin.isFirstGreaterOrEqual() ||
			     begin.getKey() != shard.begin)) { // In this case we would be setting modifiedSelectors to true, but
				                                   // not modifying anything

				req.begin = firstGreaterOrEqual(shard.begin);
				modifiedSelectors = true;
				req.arena.dependsOn(shard.arena());
				dependOnShard = true;
			} else {
				req.begin = begin;
				req.arena.dependsOn(begin.arena());
			}

			if (!reverse && end.isDefinitelyGreater(shard.end)) {
				req.end = firstGreaterOrEqual(shard.end);
				modifiedSelectors = true;
				if (!dependOnShard) {
					req.arena.dependsOn(shard.arena());
				}
			} else {
				req.end = end;
				req.arena.dependsOn(end.arena());
			}

			transformRangeLimits(limits, reverse, req);
			ASSERT(req.limitBytes > 0 && req.limit != 0 && req.limit < 0 == reverse);

			req.tags = cx->sampleReadTags() ? tags : Optional<TagSet>();
			req.debugID = info.debugID;
			req.spanContext = span.context;
			try {
				if (info.debugID.present()) {
					g_traceBatch.addEvent("TransactionDebug", info.debugID.get().first(), "NativeAPI.getRange.Before");
					/*TraceEvent("TransactionDebugGetRangeInfo", info.debugID.get())
					    .detail("ReqBeginKey", req.begin.getKey())
					    .detail("ReqEndKey", req.end.getKey())
					    .detail("OriginalBegin", originalBegin.toString())
					    .detail("OriginalEnd", originalEnd.toString())
					    .detail("Begin", begin.toString())
					    .detail("End", end.toString())
					    .detail("Shard", shard)
					    .detail("ReqLimit", req.limit)
					    .detail("ReqLimitBytes", req.limitBytes)
					    .detail("ReqVersion", req.version)
					    .detail("Reverse", reverse)
					    .detail("ModifiedSelectors", modifiedSelectors)
					    .detail("Servers", beginServer.second->description());*/
				}

				++cx->transactionPhysicalReads;
				state GetKeyValuesReply rep;
				try {
					if (CLIENT_BUGGIFY_WITH_PROB(.01)) {
						throw deterministicRandom()->randomChoice(
						    std::vector<Error>{ transaction_too_old(), future_version() });
					}
					// state AnnotateActor annotation(currentLineage);
					GetKeyValuesReply _rep =
					    wait(loadBalance(cx.getPtr(),
					                     beginServer.second,
					                     &StorageServerInterface::getKeyValues,
					                     req,
					                     TaskPriority::DefaultPromiseEndpoint,
					                     AtMostOnce::False,
					                     cx->enableLocalityLoadBalance ? &cx->queueModel : nullptr));
					rep = _rep;
					++cx->transactionPhysicalReadsCompleted;
				} catch (Error&) {
					++cx->transactionPhysicalReadsCompleted;
					throw;
				}

				if (info.debugID.present()) {
					g_traceBatch.addEvent("TransactionDebug",
					                      info.debugID.get().first(),
					                      "NativeAPI.getRange.After"); //.detail("SizeOf", rep.data.size());
					/*TraceEvent("TransactionDebugGetRangeDone", info.debugID.get())
					    .detail("ReqBeginKey", req.begin.getKey())
					    .detail("ReqEndKey", req.end.getKey())
					    .detail("RepIsMore", rep.more)
					    .detail("VersionReturned", rep.version)
					    .detail("RowsReturned", rep.data.size());*/
				}

				ASSERT(!rep.more || rep.data.size());
				ASSERT(!limits.hasRowLimit() || rep.data.size() <= limits.rows);

				limits.decrement(rep.data);

				if (reverse && begin.isLastLessOrEqual() && rep.data.size() &&
				    rep.data.end()[-1].key == begin.getKey()) {
					modifiedSelectors = false;
				}

				bool finished = limits.isReached() || (!modifiedSelectors && !rep.more) || limits.hasSatisfiedMinRows();
				bool readThrough = modifiedSelectors && !rep.more;

				// optimization: first request got all data--just return it
				if (finished && !output.size()) {
					bool readToBegin = output.readToBegin;
					bool readThroughEnd = output.readThroughEnd;

					output = RangeResult(RangeResultRef(rep.data, modifiedSelectors || limits.isReached() || rep.more),
					                     rep.arena);
					output.readToBegin = readToBegin;
					output.readThroughEnd = readThroughEnd;

					if (BUGGIFY && limits.hasByteLimit() && output.size() > std::max(1, originalLimits.minRows)) {
						// Copy instead of resizing because TSS maybe be using output's arena for comparison. This only
						// happens in simulation so it's fine
						RangeResult copy;
						int newSize =
						    deterministicRandom()->randomInt(std::max(1, originalLimits.minRows), output.size());
						for (int i = 0; i < newSize; i++) {
							copy.push_back_deep(copy.arena(), output[i]);
						}
						output = copy;
						output.more = true;

						getRangeFinished(cx,
						                 trLogInfo,
						                 startTime,
						                 originalBegin,
						                 originalEnd,
						                 snapshot,
						                 conflictRange,
						                 reverse,
						                 output);
						return output;
					}

					if (readThrough) {
						output.arena().dependsOn(shard.arena());
						output.readThrough = reverse ? shard.begin : shard.end;
					}

					getRangeFinished(
					    cx, trLogInfo, startTime, originalBegin, originalEnd, snapshot, conflictRange, reverse, output);
					return output;
				}

				output.arena().dependsOn(rep.arena);
				output.append(output.arena(), rep.data.begin(), rep.data.size());

				if (finished) {
					if (readThrough) {
						output.arena().dependsOn(shard.arena());
						output.readThrough = reverse ? shard.begin : shard.end;
					}
					output.more = modifiedSelectors || limits.isReached() || rep.more;

					getRangeFinished(
					    cx, trLogInfo, startTime, originalBegin, originalEnd, snapshot, conflictRange, reverse, output);
					return output;
				}

				readVersion = rep.version; // see above comment

				if (!rep.more) {
					ASSERT(modifiedSelectors);
					TEST(true); // !GetKeyValuesReply.more and modifiedSelectors in getRange

					if (!rep.data.size()) {
						RangeResult result = wait(getRangeFallback(
						    cx, version, originalBegin, originalEnd, originalLimits, reverse, info, tags));
						getRangeFinished(cx,
						                 trLogInfo,
						                 startTime,
						                 originalBegin,
						                 originalEnd,
						                 snapshot,
						                 conflictRange,
						                 reverse,
						                 result);
						return result;
					}

					if (reverse)
						end = firstGreaterOrEqual(shard.begin);
					else
						begin = firstGreaterOrEqual(shard.end);
				} else {
					TEST(true); // GetKeyValuesReply.more in getRange
					if (reverse)
						end = firstGreaterOrEqual(output[output.size() - 1].key);
					else
						begin = firstGreaterThan(output[output.size() - 1].key);
				}

			} catch (Error& e) {
				if (info.debugID.present()) {
					g_traceBatch.addEvent("TransactionDebug", info.debugID.get().first(), "NativeAPI.getRange.Error");
					TraceEvent("TransactionDebugError", info.debugID.get()).error(e);
				}
				if (e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed ||
				    (e.code() == error_code_transaction_too_old && readVersion == latestVersion)) {
					cx->invalidateCache(reverse ? end.getKey() : begin.getKey(),
					                    Reverse{ reverse ? (end - 1).isBackward() : begin.isBackward() });

					if (e.code() == error_code_wrong_shard_server) {
						RangeResult result = wait(getRangeFallback(
						    cx, version, originalBegin, originalEnd, originalLimits, reverse, info, tags));
						getRangeFinished(cx,
						                 trLogInfo,
						                 startTime,
						                 originalBegin,
						                 originalEnd,
						                 snapshot,
						                 conflictRange,
						                 reverse,
						                 result);
						return result;
					}

					wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, info.taskID));
				} else {
					if (trLogInfo)
						trLogInfo->addLog(FdbClientLogEvents::EventGetRangeError(startTime,
						                                                         cx->clientLocality.dcId(),
						                                                         static_cast<int>(e.code()),
						                                                         begin.getKey(),
						                                                         end.getKey()));

					throw e;
				}
			}
		}
	} catch (Error& e) {
		if (conflictRange.canBeSet()) {
			conflictRange.send(std::make_pair(Key(), Key()));
		}

		throw;
	}
}

template <class StreamReply>
struct TSSDuplicateStreamData {
	PromiseStream<StreamReply> stream;
	Promise<Void> tssComparisonDone;

	// empty constructor for optional?
	TSSDuplicateStreamData() {}

	TSSDuplicateStreamData(PromiseStream<StreamReply> stream) : stream(stream) {}

	bool done() { return tssComparisonDone.getFuture().isReady(); }

	void setDone() {
		if (tssComparisonDone.canBeSet()) {
			tssComparisonDone.send(Void());
		}
	}

	~TSSDuplicateStreamData() {}
};

// Error tracking here is weird, and latency doesn't really mean the same thing here as it does with normal tss
// comparisons, so this is pretty much just counting mismatches
ACTOR template <class Request>
static Future<Void> tssStreamComparison(Request request,
                                        TSSDuplicateStreamData<REPLYSTREAM_TYPE(Request)> streamData,
                                        ReplyPromiseStream<REPLYSTREAM_TYPE(Request)> tssReplyStream,
                                        TSSEndpointData tssData) {
	state bool ssEndOfStream = false;
	state bool tssEndOfStream = false;
	state Optional<REPLYSTREAM_TYPE(Request)> ssReply = Optional<REPLYSTREAM_TYPE(Request)>();
	state Optional<REPLYSTREAM_TYPE(Request)> tssReply = Optional<REPLYSTREAM_TYPE(Request)>();

	loop {
		// reset replies
		ssReply = Optional<REPLYSTREAM_TYPE(Request)>();
		tssReply = Optional<REPLYSTREAM_TYPE(Request)>();

		state double startTime = now();
		// wait for ss response
		try {
			REPLYSTREAM_TYPE(Request) _ssReply = waitNext(streamData.stream.getFuture());
			ssReply = _ssReply;
		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				streamData.setDone();
				throw;
			}
			if (e.code() == error_code_end_of_stream) {
				// ss response will be set to empty, to compare to the SS response if it wasn't empty and cause a
				// mismatch
				ssEndOfStream = true;
			} else {
				tssData.metrics->ssError(e.code());
			}
			TEST(e.code() != error_code_end_of_stream); // SS got error in TSS stream comparison
		}

		state double sleepTime = std::max(startTime + FLOW_KNOBS->LOAD_BALANCE_TSS_TIMEOUT - now(), 0.0);
		// wait for tss response
		try {
			choose {
				when(REPLYSTREAM_TYPE(Request) _tssReply = waitNext(tssReplyStream.getFuture())) {
					tssReply = _tssReply;
				}
				when(wait(delay(sleepTime))) {
					++tssData.metrics->tssTimeouts;
					TEST(true); // Got TSS timeout in stream comparison
				}
			}
		} catch (Error& e) {
			if (e.code() == error_code_actor_cancelled) {
				streamData.setDone();
				throw;
			}
			if (e.code() == error_code_end_of_stream) {
				// tss response will be set to empty, to compare to the SS response if it wasn't empty and cause a
				// mismatch
				tssEndOfStream = true;
			} else {
				tssData.metrics->tssError(e.code());
			}
			TEST(e.code() != error_code_end_of_stream); // TSS got error in TSS stream comparison
		}

		if (!ssEndOfStream || !tssEndOfStream) {
			++tssData.metrics->streamComparisons;
		}

		// if both are successful, compare
		if (ssReply.present() && tssReply.present()) {
			// compare results
			// FIXME: this code is pretty much identical to LoadBalance.h
			// TODO could add team check logic in if we added synchronous way to turn this into a fixed getRange request
			// and send it to the whole team and compare? I think it's fine to skip that for streaming though
			TEST(ssEndOfStream != tssEndOfStream); // SS or TSS stream finished early!

			// skip tss comparison if both are end of stream
			if ((!ssEndOfStream || !tssEndOfStream) && !TSS_doCompare(ssReply.get(), tssReply.get())) {
				TEST(true); // TSS mismatch in stream comparison
				TraceEvent mismatchEvent(
				    (g_network->isSimulated() && g_simulator.tssMode == ISimulator::TSSMode::EnabledDropMutations)
				        ? SevWarnAlways
				        : SevError,
				    TSS_mismatchTraceName(request));
				mismatchEvent.setMaxEventLength(FLOW_KNOBS->TSS_LARGE_TRACE_SIZE);
				mismatchEvent.detail("TSSID", tssData.tssId);

				if (tssData.metrics->shouldRecordDetailedMismatch()) {
					TSS_traceMismatch(mismatchEvent, request, ssReply.get(), tssReply.get());

					TEST(FLOW_KNOBS
					         ->LOAD_BALANCE_TSS_MISMATCH_TRACE_FULL); // Tracing Full TSS Mismatch in stream comparison
					TEST(!FLOW_KNOBS->LOAD_BALANCE_TSS_MISMATCH_TRACE_FULL); // Tracing Partial TSS Mismatch in stream
					                                                         // comparison and storing the rest in FDB

					if (!FLOW_KNOBS->LOAD_BALANCE_TSS_MISMATCH_TRACE_FULL) {
						mismatchEvent.disable();
						UID mismatchUID = deterministicRandom()->randomUniqueID();
						tssData.metrics->recordDetailedMismatchData(mismatchUID, mismatchEvent.getFields().toString());

						// record a summarized trace event instead
						TraceEvent summaryEvent((g_network->isSimulated() &&
						                         g_simulator.tssMode == ISimulator::TSSMode::EnabledDropMutations)
						                            ? SevWarnAlways
						                            : SevError,
						                        TSS_mismatchTraceName(request));
						summaryEvent.detail("TSSID", tssData.tssId).detail("MismatchId", mismatchUID);
					}
				} else {
					// don't record trace event
					mismatchEvent.disable();
				}
				streamData.setDone();
				return Void();
			}
		}
		if (!ssReply.present() || !tssReply.present() || ssEndOfStream || tssEndOfStream) {
			// if both streams don't still have more data, stop comparison
			streamData.setDone();
			return Void();
		}
	}
}

// Currently only used for GetKeyValuesStream but could easily be plugged for other stream types
// User of the stream has to forward the SS's responses to the returned promise stream, if it is set
template <class Request>
Optional<TSSDuplicateStreamData<REPLYSTREAM_TYPE(Request)>>
maybeDuplicateTSSStreamFragment(Request& req, QueueModel* model, RequestStream<Request> const* ssStream) {
	if (model) {
		Optional<TSSEndpointData> tssData = model->getTssData(ssStream->getEndpoint().token.first());

		if (tssData.present()) {
			TEST(true); // duplicating stream to TSS
			resetReply(req);
			// FIXME: optimize to avoid creating new netNotifiedQueueWithAcknowledgements for each stream duplication
			RequestStream<Request> tssRequestStream(tssData.get().endpoint);
			ReplyPromiseStream<REPLYSTREAM_TYPE(Request)> tssReplyStream = tssRequestStream.getReplyStream(req);
			PromiseStream<REPLYSTREAM_TYPE(Request)> ssDuplicateReplyStream;
			TSSDuplicateStreamData<REPLYSTREAM_TYPE(Request)> streamData(ssDuplicateReplyStream);
			model->addActor.send(tssStreamComparison(req, streamData, tssReplyStream, tssData.get()));
			return Optional<TSSDuplicateStreamData<REPLYSTREAM_TYPE(Request)>>(streamData);
		}
	}
	return Optional<TSSDuplicateStreamData<REPLYSTREAM_TYPE(Request)>>();
}

// Streams all of the KV pairs in a target key range into a ParallelStream fragment
ACTOR Future<Void> getRangeStreamFragment(ParallelStream<RangeResult>::Fragment* results,
                                          Database cx,
                                          Reference<TransactionLogInfo> trLogInfo,
                                          Version version,
                                          KeyRange keys,
                                          GetRangeLimits limits,
                                          Snapshot snapshot,
                                          Reverse reverse,
                                          TransactionInfo info,
                                          TagSet tags,
                                          SpanID spanContext) {
	loop {
		state std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations = wait(getKeyRangeLocations(
		    cx, keys, CLIENT_KNOBS->GET_RANGE_SHARD_LIMIT, reverse, &StorageServerInterface::getKeyValuesStream, info));
		ASSERT(locations.size());
		state int shard = 0;
		loop {
			const KeyRange& range = locations[shard].first;

			state Optional<TSSDuplicateStreamData<GetKeyValuesStreamReply>> tssDuplicateStream;
			state GetKeyValuesStreamRequest req;
			req.version = version;
			req.begin = firstGreaterOrEqual(range.begin);
			req.end = firstGreaterOrEqual(range.end);
			req.spanContext = spanContext;
			req.limit = reverse ? -CLIENT_KNOBS->REPLY_BYTE_LIMIT : CLIENT_KNOBS->REPLY_BYTE_LIMIT;
			req.limitBytes = std::numeric_limits<int>::max();

			// keep shard's arena around in case of async tss comparison
			req.arena.dependsOn(range.arena());

			ASSERT(req.limitBytes > 0 && req.limit != 0 && req.limit < 0 == reverse);

			// FIXME: buggify byte limits on internal functions that use them, instead of globally
			req.tags = cx->sampleReadTags() ? tags : Optional<TagSet>();
			req.debugID = info.debugID;

			try {
				if (info.debugID.present()) {
					g_traceBatch.addEvent(
					    "TransactionDebug", info.debugID.get().first(), "NativeAPI.RangeStream.Before");
				}
				++cx->transactionPhysicalReads;
				state GetKeyValuesStreamReply rep;

				if (locations[shard].second->size() == 0) {
					wait(cx->connectionFileChanged());
					results->sendError(transaction_too_old());
					return Void();
				}

				state int useIdx = -1;

				loop {
					// FIXME: create a load balance function for this code so future users of reply streams do not have
					// to duplicate this code
					int count = 0;
					for (int i = 0; i < locations[shard].second->size(); i++) {
						if (!IFailureMonitor::failureMonitor()
						         .getState(locations[shard]
						                       .second->get(i, &StorageServerInterface::getKeyValuesStream)
						                       .getEndpoint())
						         .failed) {
							if (deterministicRandom()->random01() <= 1.0 / ++count) {
								useIdx = i;
							}
						}
					}

					if (useIdx >= 0) {
						break;
					}

					std::vector<Future<Void>> ok(locations[shard].second->size());
					for (int i = 0; i < ok.size(); i++) {
						ok[i] = IFailureMonitor::failureMonitor().onStateEqual(
						    locations[shard].second->get(i, &StorageServerInterface::getKeyValuesStream).getEndpoint(),
						    FailureStatus(false));
					}

					// Making this SevWarn means a lot of clutter
					if (now() - g_network->networkInfo.newestAlternativesFailure > 1 ||
					    deterministicRandom()->random01() < 0.01) {
						TraceEvent("AllAlternativesFailed")
						    .detail("Alternatives", locations[shard].second->description());
					}

					wait(allAlternativesFailedDelay(quorum(ok, 1)));
				}

				state ReplyPromiseStream<GetKeyValuesStreamReply> replyStream =
				    locations[shard]
				        .second->get(useIdx, &StorageServerInterface::getKeyValuesStream)
				        .getReplyStream(req);

				tssDuplicateStream = maybeDuplicateTSSStreamFragment(
				    req,
				    cx->enableLocalityLoadBalance ? &cx->queueModel : nullptr,
				    &locations[shard].second->get(useIdx, &StorageServerInterface::getKeyValuesStream));

				state bool breakAgain = false;
				loop {
					wait(results->onEmpty());
					try {
						choose {
							when(wait(cx->connectionFileChanged())) {
								results->sendError(transaction_too_old());
								if (tssDuplicateStream.present() && !tssDuplicateStream.get().done()) {
									tssDuplicateStream.get().stream.sendError(transaction_too_old());
								}
								return Void();
							}

							when(GetKeyValuesStreamReply _rep = waitNext(replyStream.getFuture())) { rep = _rep; }
						}
						++cx->transactionPhysicalReadsCompleted;
					} catch (Error& e) {
						++cx->transactionPhysicalReadsCompleted;
						if (e.code() == error_code_broken_promise) {
							if (tssDuplicateStream.present() && !tssDuplicateStream.get().done()) {
								tssDuplicateStream.get().stream.sendError(connection_failed());
							}
							throw connection_failed();
						}
						if (e.code() != error_code_end_of_stream) {
							if (tssDuplicateStream.present() && !tssDuplicateStream.get().done()) {
								tssDuplicateStream.get().stream.sendError(e);
							}
							throw;
						}
						rep = GetKeyValuesStreamReply();
					}
					if (info.debugID.present())
						g_traceBatch.addEvent(
						    "TransactionDebug", info.debugID.get().first(), "NativeAPI.getExactRange.After");
					RangeResult output(RangeResultRef(rep.data, rep.more), rep.arena);

					if (tssDuplicateStream.present() && !tssDuplicateStream.get().done()) {
						// shallow copy the reply with an arena depends, and send it to the duplicate stream for TSS
						GetKeyValuesStreamReply replyCopy;
						replyCopy.version = rep.version;
						replyCopy.more = rep.more;
						replyCopy.cached = rep.cached;
						replyCopy.arena.dependsOn(rep.arena);
						replyCopy.data.append(replyCopy.arena, rep.data.begin(), rep.data.size());
						tssDuplicateStream.get().stream.send(replyCopy);
					}

					int64_t bytes = 0;
					for (const KeyValueRef& kv : output) {
						bytes += kv.key.size() + kv.value.size();
					}

					cx->transactionBytesRead += bytes;
					cx->transactionKeysRead += output.size();

					// If the reply says there is more but we know that we finished the shard, then fix rep.more
					if (reverse && output.more && rep.data.size() > 0 &&
					    output[output.size() - 1].key == locations[shard].first.begin) {
						output.more = false;
					}

					if (output.more) {
						if (!rep.data.size()) {
							TraceEvent(SevError, "GetRangeStreamError")
							    .detail("Reason", "More data indicated but no rows present")
							    .detail("LimitBytes", limits.bytes)
							    .detail("LimitRows", limits.rows)
							    .detail("OutputSize", output.size())
							    .detail("OutputBytes", output.expectedSize())
							    .detail("BlockSize", rep.data.size())
							    .detail("BlockBytes", rep.data.expectedSize());
							ASSERT(false);
						}
						TEST(true); // GetKeyValuesStreamReply.more in getRangeStream
						// Make next request to the same shard with a beginning key just after the last key returned
						if (reverse)
							locations[shard].first =
							    KeyRangeRef(locations[shard].first.begin, output[output.size() - 1].key);
						else
							locations[shard].first =
							    KeyRangeRef(keyAfter(output[output.size() - 1].key), locations[shard].first.end);
					}

					if (locations[shard].first.empty()) {
						output.more = false;
					}

					if (!output.more) {
						const KeyRange& range = locations[shard].first;
						if (shard == locations.size() - 1) {
							KeyRef begin = reverse ? keys.begin : range.end;
							KeyRef end = reverse ? range.begin : keys.end;

							if (begin >= end) {
								if (range.begin == allKeys.begin) {
									output.readToBegin = true;
								}
								if (range.end == allKeys.end) {
									output.readThroughEnd = true;
								}
								output.arena().dependsOn(keys.arena());
								output.readThrough = reverse ? keys.begin : keys.end;
								results->send(std::move(output));
								results->finish();
								if (tssDuplicateStream.present() && !tssDuplicateStream.get().done()) {
									tssDuplicateStream.get().stream.sendError(end_of_stream());
								}
								return Void();
							}
							keys = KeyRangeRef(begin, end);
							breakAgain = true;
						} else {
							++shard;
						}
						output.arena().dependsOn(range.arena());
						output.readThrough = reverse ? range.begin : range.end;
						results->send(std::move(output));
						break;
					}

					ASSERT(output.size());
					if (keys.begin == allKeys.begin && !reverse) {
						output.readToBegin = true;
					}
					if (keys.end == allKeys.end && reverse) {
						output.readThroughEnd = true;
					}
					results->send(std::move(output));
				}
				if (breakAgain) {
					break;
				}
			} catch (Error& e) {
				// send errors to tss duplicate stream, including actor_cancelled
				if (tssDuplicateStream.present() && !tssDuplicateStream.get().done()) {
					tssDuplicateStream.get().stream.sendError(e);
				}
				if (e.code() == error_code_actor_cancelled) {
					throw;
				}
				if (e.code() == error_code_wrong_shard_server || e.code() == error_code_all_alternatives_failed ||
				    e.code() == error_code_connection_failed) {
					const KeyRangeRef& range = locations[shard].first;

					if (reverse)
						keys = KeyRangeRef(keys.begin, range.end);
					else
						keys = KeyRangeRef(range.begin, keys.end);

					cx->invalidateCache(keys);
					wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, info.taskID));
					break;
				} else {
					results->sendError(e);
					return Void();
				}
			}
		}
	}
}

ACTOR Future<Standalone<VectorRef<KeyRef>>> getRangeSplitPoints(Database cx, KeyRange keys, int64_t chunkSize);

static KeyRange intersect(KeyRangeRef lhs, KeyRangeRef rhs) {
	return KeyRange(KeyRangeRef(std::max(lhs.begin, rhs.begin), std::min(lhs.end, rhs.end)));
}

// Divides the requested key range into 1MB fragments, create range streams for each fragment, and merges the results so
// the client get them in order
ACTOR Future<Void> getRangeStream(PromiseStream<RangeResult> _results,
                                  Database cx,
                                  Reference<TransactionLogInfo> trLogInfo,
                                  Future<Version> fVersion,
                                  KeySelector begin,
                                  KeySelector end,
                                  GetRangeLimits limits,
                                  Promise<std::pair<Key, Key>> conflictRange,
                                  Snapshot snapshot,
                                  Reverse reverse,
                                  TransactionInfo info,
                                  TagSet tags) {

	state ParallelStream<RangeResult> results(_results, CLIENT_KNOBS->RANGESTREAM_BUFFERED_FRAGMENTS_LIMIT);

	// FIXME: better handling to disable row limits
	ASSERT(!limits.hasRowLimit());
	state Span span("NAPI:getRangeStream"_loc, info.spanID);

	state Version version = wait(fVersion);
	cx->validateVersion(version);

	Future<Key> fb = resolveKey(cx, begin, version, info, tags);
	state Future<Key> fe = resolveKey(cx, end, version, info, tags);

	state Key b = wait(fb);
	state Key e = wait(fe);

	if (!snapshot) {
		// FIXME: this conflict range is too large, and should be updated continously as results are returned
		conflictRange.send(std::make_pair(std::min(b, Key(begin.getKey(), begin.arena())),
		                                  std::max(e, Key(end.getKey(), end.arena()))));
	}

	if (b >= e) {
		wait(results.finish());
		return Void();
	}

	// if e is allKeys.end, we have read through the end of the database
	// if b is allKeys.begin, we have either read through the beginning of the database,
	// or allKeys.begin exists in the database and will be part of the conflict range anyways

	state std::vector<Future<Void>> outstandingRequests;
	while (b < e) {
		state std::pair<KeyRange, Reference<LocationInfo>> ssi =
		    wait(getKeyLocation(cx, reverse ? e : b, &StorageServerInterface::getKeyValuesStream, info, reverse));
		state KeyRange shardIntersection = intersect(ssi.first, KeyRangeRef(b, e));
		state Standalone<VectorRef<KeyRef>> splitPoints =
		    wait(getRangeSplitPoints(cx, shardIntersection, CLIENT_KNOBS->RANGESTREAM_FRAGMENT_SIZE));
		state std::vector<KeyRange> toSend;
		// state std::vector<Future<std::list<KeyRangeRef>::iterator>> outstandingRequests;

		if (!splitPoints.empty()) {
			toSend.push_back(KeyRange(KeyRangeRef(shardIntersection.begin, splitPoints.front()), splitPoints.arena()));
			for (int i = 0; i < splitPoints.size() - 1; ++i) {
				toSend.push_back(KeyRange(KeyRangeRef(splitPoints[i], splitPoints[i + 1]), splitPoints.arena()));
			}
			toSend.push_back(KeyRange(KeyRangeRef(splitPoints.back(), shardIntersection.end), splitPoints.arena()));
		} else {
			toSend.push_back(KeyRange(KeyRangeRef(shardIntersection.begin, shardIntersection.end)));
		}

		state int idx = 0;
		state int useIdx = 0;
		for (; idx < toSend.size(); ++idx) {
			useIdx = reverse ? toSend.size() - idx - 1 : idx;
			if (toSend[useIdx].empty()) {
				continue;
			}
			ParallelStream<RangeResult>::Fragment* fragment = wait(results.createFragment());
			outstandingRequests.push_back(getRangeStreamFragment(
			    fragment, cx, trLogInfo, version, toSend[useIdx], limits, snapshot, reverse, info, tags, span.context));
		}
		if (reverse) {
			e = shardIntersection.begin;
		} else {
			b = shardIntersection.end;
		}
	}
	wait(waitForAll(outstandingRequests) && results.finish());
	return Void();
}

Future<RangeResult> getRange(Database const& cx,
                             Future<Version> const& fVersion,
                             KeySelector const& begin,
                             KeySelector const& end,
                             GetRangeLimits const& limits,
                             Reverse const& reverse,
                             TransactionInfo const& info,
                             TagSet const& tags) {
	return getRange(cx,
	                Reference<TransactionLogInfo>(),
	                fVersion,
	                begin,
	                end,
	                limits,
	                Promise<std::pair<Key, Key>>(),
	                Snapshot::True,
	                reverse,
	                info,
	                tags);
}

bool DatabaseContext::debugUseTags = false;
const std::vector<std::string> DatabaseContext::debugTransactionTagChoices = { "a", "b", "c", "d", "e", "f", "g",
	                                                                           "h", "i", "j", "k", "l", "m", "n",
	                                                                           "o", "p", "q", "r", "s", "t" };

void debugAddTags(Transaction* tr) {
	int numTags = deterministicRandom()->randomInt(0, CLIENT_KNOBS->MAX_TAGS_PER_TRANSACTION + 1);
	for (int i = 0; i < numTags; ++i) {
		TransactionTag tag;
		if (deterministicRandom()->random01() < 0.7) {
			tag = TransactionTagRef(deterministicRandom()->randomChoice(DatabaseContext::debugTransactionTagChoices));
		} else {
			int length = deterministicRandom()->randomInt(1, CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH + 1);
			uint8_t* s = new (tag.arena()) uint8_t[length];
			for (int j = 0; j < length; ++j) {
				s[j] = (uint8_t)deterministicRandom()->randomInt(0, 256);
			}

			tag.contents() = TransactionTagRef(s, length);
		}

		if (deterministicRandom()->coinflip()) {
			tr->options.readTags.addTag(tag);
		}
		tr->options.tags.addTag(tag);
	}
}

SpanID generateSpanID(int transactionTracingEnabled) {
	uint64_t txnId = deterministicRandom()->randomUInt64();
	if (transactionTracingEnabled > 0) {
		uint64_t tokenId = deterministicRandom()->random01() <= FLOW_KNOBS->TRACING_SAMPLE_RATE
		                       ? deterministicRandom()->randomUInt64()
		                       : 0;
		return SpanID(txnId, tokenId);
	} else {
		return SpanID(txnId, 0);
	}
}

Transaction::Transaction() : info(TaskPriority::DefaultEndpoint, generateSpanID(true)) {}

Transaction::Transaction(Database const& cx)
  : info(cx->taskID, generateSpanID(cx->transactionTracingEnabled)), numErrors(0), options(cx),
    span(info.spanID, "Transaction"_loc), trLogInfo(createTrLogInfoProbabilistically(cx)), cx(cx),
    backoff(CLIENT_KNOBS->DEFAULT_BACKOFF), committedVersion(invalidVersion), tr(info.spanID) {
	if (DatabaseContext::debugUseTags) {
		debugAddTags(this);
	}
}

Transaction::~Transaction() {
	flushTrLogsIfEnabled();
	cancelWatches();
}

void Transaction::operator=(Transaction&& r) noexcept {
	flushTrLogsIfEnabled();
	cx = std::move(r.cx);
	tr = std::move(r.tr);
	readVersion = std::move(r.readVersion);
	metadataVersion = std::move(r.metadataVersion);
	extraConflictRanges = std::move(r.extraConflictRanges);
	commitResult = std::move(r.commitResult);
	committing = std::move(r.committing);
	options = std::move(r.options);
	info = r.info;
	backoff = r.backoff;
	numErrors = r.numErrors;
	committedVersion = r.committedVersion;
	versionstampPromise = std::move(r.versionstampPromise);
	watches = r.watches;
	trLogInfo = std::move(r.trLogInfo);
}

void Transaction::flushTrLogsIfEnabled() {
	if (trLogInfo && trLogInfo->logsAdded && trLogInfo->trLogWriter.getData()) {
		ASSERT(trLogInfo->flushed == false);
		cx->clientStatusUpdater.inStatusQ.push_back({ trLogInfo->identifier, std::move(trLogInfo->trLogWriter) });
		trLogInfo->flushed = true;
	}
}

void Transaction::setVersion(Version v) {
	startTime = now();
	if (readVersion.isValid())
		throw read_version_already_set();
	if (v <= 0)
		throw version_invalid();
	readVersion = v;
}

Future<Optional<Value>> Transaction::get(const Key& key, Snapshot snapshot) {
	++cx->transactionLogicalReads;
	++cx->transactionGetValueRequests;
	// ASSERT (key < allKeys.end);

	// There are no keys in the database with size greater than KEY_SIZE_LIMIT
	if (key.size() >
	    (key.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		return Optional<Value>();

	auto ver = getReadVersion();

	/*	if (!systemKeys.contains(key))
	        return Optional<Value>(Value()); */

	if (!snapshot)
		tr.transaction.read_conflict_ranges.push_back(tr.arena, singleKeyRange(key, tr.arena));

	if (key == metadataVersionKey) {
		++cx->transactionMetadataVersionReads;
		if (!ver.isReady() || metadataVersion.isSet()) {
			return metadataVersion.getFuture();
		} else {
			if (ver.isError())
				return ver.getError();
			if (ver.get() == cx->metadataVersionCache[cx->mvCacheInsertLocation].first) {
				return cx->metadataVersionCache[cx->mvCacheInsertLocation].second;
			}

			Version v = ver.get();
			int hi = cx->mvCacheInsertLocation;
			int lo = (cx->mvCacheInsertLocation + 1) % cx->metadataVersionCache.size();

			while (hi != lo) {
				int cu = hi > lo ? (hi + lo) / 2
				                 : ((hi + cx->metadataVersionCache.size() + lo) / 2) % cx->metadataVersionCache.size();
				if (v == cx->metadataVersionCache[cu].first) {
					return cx->metadataVersionCache[cu].second;
				}
				if (cu == lo) {
					break;
				}
				if (v < cx->metadataVersionCache[cu].first) {
					hi = cu;
				} else {
					lo = (cu + 1) % cx->metadataVersionCache.size();
				}
			}
		}
	}

	return getValue(ver, key, cx, info, trLogInfo, options.readTags);
}

void Watch::setWatch(Future<Void> watchFuture) {
	this->watchFuture = watchFuture;

	// Cause the watch loop to go around and start waiting on watchFuture
	onSetWatchTrigger.send(Void());
}

// FIXME: This seems pretty horrible. Now a Database can't die until all of its watches do...
ACTOR Future<Void> watch(Reference<Watch> watch, Database cx, TagSet tags, TransactionInfo info) {
	try {
		choose {
			// RYOW write to value that is being watched (if applicable)
			// Errors
			when(wait(watch->onChangeTrigger.getFuture())) {}

			// NativeAPI finished commit and updated watchFuture
			when(wait(watch->onSetWatchTrigger.getFuture())) {

				loop {
					choose {
						// NativeAPI watchValue future finishes or errors
						when(wait(watch->watchFuture)) { break; }

						when(wait(cx->connectionFileChanged())) {
							TEST(true); // Recreated a watch after switch
							cx->clearWatchMetadata();
							watch->watchFuture =
							    watchValueMap(cx->minAcceptableReadVersion, watch->key, watch->value, cx, info, tags);
						}
					}
				}
			}
		}
	} catch (Error& e) {
		cx->removeWatch();
		throw;
	}

	cx->removeWatch();
	return Void();
}

Future<Version> Transaction::getRawReadVersion() {
	return ::getRawVersion(cx, info.spanID);
}

Future<Void> Transaction::watch(Reference<Watch> watch) {
	++cx->transactionWatchRequests;
	cx->addWatch();
	watches.push_back(watch);
	return ::watch(watch, cx, options.readTags, info);
}

ACTOR Future<Standalone<VectorRef<const char*>>> getAddressesForKeyActor(Key key,
                                                                         Future<Version> ver,
                                                                         Database cx,
                                                                         TransactionInfo info,
                                                                         TransactionOptions options) {
	state std::vector<StorageServerInterface> ssi;

	// If key >= allKeys.end, then getRange will return a kv-pair with an empty value. This will result in our
	// serverInterfaces vector being empty, which will cause us to return an empty addresses list.

	state Key ksKey = keyServersKey(key);
	state RangeResult serverTagResult = wait(getRange(cx,
	                                                  ver,
	                                                  lastLessOrEqual(serverTagKeys.begin),
	                                                  firstGreaterThan(serverTagKeys.end),
	                                                  GetRangeLimits(CLIENT_KNOBS->TOO_MANY),
	                                                  Reverse::False,
	                                                  info,
	                                                  options.readTags));
	ASSERT(!serverTagResult.more && serverTagResult.size() < CLIENT_KNOBS->TOO_MANY);
	Future<RangeResult> futureServerUids = getRange(cx,
	                                                ver,
	                                                lastLessOrEqual(ksKey),
	                                                firstGreaterThan(ksKey),
	                                                GetRangeLimits(1),
	                                                Reverse::False,
	                                                info,
	                                                options.readTags);
	RangeResult serverUids = wait(futureServerUids);

	ASSERT(serverUids.size()); // every shard needs to have a team

	std::vector<UID> src;
	std::vector<UID> ignore; // 'ignore' is so named because it is the vector into which we decode the 'dest' servers in
	                         // the case where this key is being relocated. But 'src' is the canonical location until
	                         // the move is finished, because it could be cancelled at any time.
	decodeKeyServersValue(serverTagResult, serverUids[0].value, src, ignore);
	Optional<std::vector<StorageServerInterface>> serverInterfaces =
	    wait(transactionalGetServerInterfaces(ver, cx, info, src, options.readTags));

	ASSERT(serverInterfaces.present()); // since this is happening transactionally, /FF/keyServers and /FF/serverList
	                                    // need to be consistent with one another
	ssi = serverInterfaces.get();

	Standalone<VectorRef<const char*>> addresses;
	for (auto i : ssi) {
		std::string ipString = options.includePort ? i.address().toString() : i.address().ip.toString();
		char* c_string = new (addresses.arena()) char[ipString.length() + 1];
		strcpy(c_string, ipString.c_str());
		addresses.push_back(addresses.arena(), c_string);
	}
	return addresses;
}

Future<Standalone<VectorRef<const char*>>> Transaction::getAddressesForKey(const Key& key) {
	++cx->transactionLogicalReads;
	++cx->transactionGetAddressesForKeyRequests;
	auto ver = getReadVersion();

	return getAddressesForKeyActor(key, ver, cx, info, options);
}

ACTOR Future<Key> getKeyAndConflictRange(Database cx,
                                         KeySelector k,
                                         Future<Version> version,
                                         Promise<std::pair<Key, Key>> conflictRange,
                                         TransactionInfo info,
                                         TagSet tags) {
	try {
		Key rep = wait(getKey(cx, k, version, info, tags));
		if (k.offset <= 0)
			conflictRange.send(std::make_pair(rep, k.orEqual ? keyAfter(k.getKey()) : Key(k.getKey(), k.arena())));
		else
			conflictRange.send(
			    std::make_pair(k.orEqual ? keyAfter(k.getKey()) : Key(k.getKey(), k.arena()), keyAfter(rep)));
		return rep;
	} catch (Error& e) {
		conflictRange.send(std::make_pair(Key(), Key()));
		throw;
	}
}

Future<Key> Transaction::getKey(const KeySelector& key, Snapshot snapshot) {
	++cx->transactionLogicalReads;
	++cx->transactionGetKeyRequests;
	if (snapshot)
		return ::getKey(cx, key, getReadVersion(), info, options.readTags);

	Promise<std::pair<Key, Key>> conflictRange;
	extraConflictRanges.push_back(conflictRange.getFuture());
	return getKeyAndConflictRange(cx, key, getReadVersion(), conflictRange, info, options.readTags);
}

Future<RangeResult> Transaction::getRange(const KeySelector& begin,
                                          const KeySelector& end,
                                          GetRangeLimits limits,
                                          Snapshot snapshot,
                                          Reverse reverse) {
	++cx->transactionLogicalReads;
	++cx->transactionGetRangeRequests;

	if (limits.isReached())
		return RangeResult();

	if (!limits.isValid())
		return range_limits_invalid();

	ASSERT(limits.rows != 0);

	KeySelector b = begin;
	if (b.orEqual) {
		TEST(true); // Native begin orEqual==true
		b.removeOrEqual(b.arena());
	}

	KeySelector e = end;
	if (e.orEqual) {
		TEST(true); // Native end orEqual==true
		e.removeOrEqual(e.arena());
	}

	if (b.offset >= e.offset && b.getKey() >= e.getKey()) {
		TEST(true); // Native range inverted
		return RangeResult();
	}

	Promise<std::pair<Key, Key>> conflictRange;
	if (!snapshot) {
		extraConflictRanges.push_back(conflictRange.getFuture());
	}

	return ::getRange(
	    cx, trLogInfo, getReadVersion(), b, e, limits, conflictRange, snapshot, reverse, info, options.readTags);
}

Future<RangeResult> Transaction::getRange(const KeySelector& begin,
                                          const KeySelector& end,
                                          int limit,
                                          Snapshot snapshot,
                                          Reverse reverse) {
	return getRange(begin, end, GetRangeLimits(limit), snapshot, reverse);
}

// A method for streaming data from the storage server that is more efficient than getRange when reading large amounts
// of data
Future<Void> Transaction::getRangeStream(const PromiseStream<RangeResult>& results,
                                         const KeySelector& begin,
                                         const KeySelector& end,
                                         GetRangeLimits limits,
                                         Snapshot snapshot,
                                         Reverse reverse) {
	++cx->transactionLogicalReads;
	++cx->transactionGetRangeStreamRequests;

	// FIXME: limits are not implemented yet, and this code has not be tested with reverse=true
	ASSERT(!limits.hasByteLimit() && !limits.hasRowLimit() && !reverse);

	KeySelector b = begin;
	if (b.orEqual) {
		TEST(true); // Native stream begin orEqual==true
		b.removeOrEqual(b.arena());
	}

	KeySelector e = end;
	if (e.orEqual) {
		TEST(true); // Native stream end orEqual==true
		e.removeOrEqual(e.arena());
	}

	if (b.offset >= e.offset && b.getKey() >= e.getKey()) {
		TEST(true); // Native stream range inverted
		results.sendError(end_of_stream());
		return Void();
	}

	Promise<std::pair<Key, Key>> conflictRange;
	if (!snapshot) {
		extraConflictRanges.push_back(conflictRange.getFuture());
	}

	return forwardErrors(::getRangeStream(results,
	                                      cx,
	                                      trLogInfo,
	                                      getReadVersion(),
	                                      b,
	                                      e,
	                                      limits,
	                                      conflictRange,
	                                      snapshot,
	                                      reverse,
	                                      info,
	                                      options.readTags),
	                     results);
}

Future<Void> Transaction::getRangeStream(const PromiseStream<RangeResult>& results,
                                         const KeySelector& begin,
                                         const KeySelector& end,
                                         int limit,
                                         Snapshot snapshot,
                                         Reverse reverse) {
	return getRangeStream(results, begin, end, GetRangeLimits(limit), snapshot, reverse);
}

void Transaction::addReadConflictRange(KeyRangeRef const& keys) {
	ASSERT(!keys.empty());

	// There aren't any keys in the database with size larger than KEY_SIZE_LIMIT, so if range contains large keys
	// we can translate it to an equivalent one with smaller keys
	KeyRef begin = keys.begin;
	KeyRef end = keys.end;

	if (begin.size() >
	    (begin.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		begin = begin.substr(
		    0,
		    (begin.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT) +
		        1);
	if (end.size() >
	    (end.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		end = end.substr(
		    0,
		    (end.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT) +
		        1);

	KeyRangeRef r = KeyRangeRef(begin, end);

	if (r.empty()) {
		return;
	}

	tr.transaction.read_conflict_ranges.push_back_deep(tr.arena, r);
}

void Transaction::makeSelfConflicting() {
	BinaryWriter wr(Unversioned());
	wr.serializeBytes(LiteralStringRef("\xFF/SC/"));
	wr << deterministicRandom()->randomUniqueID();
	auto r = singleKeyRange(wr.toValue(), tr.arena);
	tr.transaction.read_conflict_ranges.push_back(tr.arena, r);
	tr.transaction.write_conflict_ranges.push_back(tr.arena, r);
}

void Transaction::set(const KeyRef& key, const ValueRef& value, AddConflictRange addConflictRange) {
	++cx->transactionSetMutations;
	if (key.size() >
	    (key.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		throw key_too_large();
	if (value.size() > CLIENT_KNOBS->VALUE_SIZE_LIMIT)
		throw value_too_large();

	auto& req = tr;
	auto& t = req.transaction;
	auto r = singleKeyRange(key, req.arena);
	auto v = ValueRef(req.arena, value);
	t.mutations.emplace_back(req.arena, MutationRef::SetValue, r.begin, v);

	if (addConflictRange) {
		t.write_conflict_ranges.push_back(req.arena, r);
	}
}

void Transaction::atomicOp(const KeyRef& key,
                           const ValueRef& operand,
                           MutationRef::Type operationType,
                           AddConflictRange addConflictRange) {
	++cx->transactionAtomicMutations;
	if (key.size() >
	    (key.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		throw key_too_large();
	if (operand.size() > CLIENT_KNOBS->VALUE_SIZE_LIMIT)
		throw value_too_large();

	if (apiVersionAtLeast(510)) {
		if (operationType == MutationRef::Min)
			operationType = MutationRef::MinV2;
		else if (operationType == MutationRef::And)
			operationType = MutationRef::AndV2;
	}

	auto& req = tr;
	auto& t = req.transaction;
	auto r = singleKeyRange(key, req.arena);
	auto v = ValueRef(req.arena, operand);

	t.mutations.emplace_back(req.arena, operationType, r.begin, v);

	if (addConflictRange && operationType != MutationRef::SetVersionstampedKey)
		t.write_conflict_ranges.push_back(req.arena, r);

	TEST(true); // NativeAPI atomic operation
}

void Transaction::clear(const KeyRangeRef& range, AddConflictRange addConflictRange) {
	++cx->transactionClearMutations;
	auto& req = tr;
	auto& t = req.transaction;

	KeyRef begin = range.begin;
	KeyRef end = range.end;

	// There aren't any keys in the database with size larger than KEY_SIZE_LIMIT, so if range contains large keys
	// we can translate it to an equivalent one with smaller keys
	if (begin.size() >
	    (begin.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		begin = begin.substr(
		    0,
		    (begin.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT) +
		        1);
	if (end.size() >
	    (end.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		end = end.substr(
		    0,
		    (end.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT) +
		        1);

	auto r = KeyRangeRef(req.arena, KeyRangeRef(begin, end));
	if (r.empty())
		return;

	t.mutations.emplace_back(req.arena, MutationRef::ClearRange, r.begin, r.end);

	if (addConflictRange)
		t.write_conflict_ranges.push_back(req.arena, r);
}
void Transaction::clear(const KeyRef& key, AddConflictRange addConflictRange) {
	++cx->transactionClearMutations;
	// There aren't any keys in the database with size larger than KEY_SIZE_LIMIT
	if (key.size() >
	    (key.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		return;

	auto& req = tr;
	auto& t = req.transaction;

	// efficient single key range clear range mutation, see singleKeyRange
	uint8_t* data = new (req.arena) uint8_t[key.size() + 1];
	memcpy(data, key.begin(), key.size());
	data[key.size()] = 0;
	t.mutations.emplace_back(
	    req.arena, MutationRef::ClearRange, KeyRef(data, key.size()), KeyRef(data, key.size() + 1));
	if (addConflictRange)
		t.write_conflict_ranges.emplace_back(req.arena, KeyRef(data, key.size()), KeyRef(data, key.size() + 1));
}
void Transaction::addWriteConflictRange(const KeyRangeRef& keys) {
	ASSERT(!keys.empty());
	auto& req = tr;
	auto& t = req.transaction;

	// There aren't any keys in the database with size larger than KEY_SIZE_LIMIT, so if range contains large keys
	// we can translate it to an equivalent one with smaller keys
	KeyRef begin = keys.begin;
	KeyRef end = keys.end;

	if (begin.size() >
	    (begin.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		begin = begin.substr(
		    0,
		    (begin.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT) +
		        1);
	if (end.size() >
	    (end.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
		end = end.substr(
		    0,
		    (end.startsWith(systemKeys.begin) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT) +
		        1);

	KeyRangeRef r = KeyRangeRef(begin, end);

	if (r.empty()) {
		return;
	}

	t.write_conflict_ranges.push_back_deep(req.arena, r);
}

double Transaction::getBackoff(int errCode) {
	double returnedBackoff = backoff;

	if (errCode == error_code_tag_throttled) {
		auto priorityItr = cx->throttledTags.find(options.priority);
		for (auto& tag : options.tags) {
			if (priorityItr != cx->throttledTags.end()) {
				auto tagItr = priorityItr->second.find(tag);
				if (tagItr != priorityItr->second.end()) {
					TEST(true); // Returning throttle backoff
					returnedBackoff = std::max(
					    returnedBackoff,
					    std::min(CLIENT_KNOBS->TAG_THROTTLE_RECHECK_INTERVAL, tagItr->second.throttleDuration()));
					if (returnedBackoff == CLIENT_KNOBS->TAG_THROTTLE_RECHECK_INTERVAL) {
						break;
					}
				}
			}
		}
	}

	returnedBackoff *= deterministicRandom()->random01();

	// Set backoff for next time
	if (errCode == error_code_proxy_memory_limit_exceeded) {
		backoff = std::min(backoff * CLIENT_KNOBS->BACKOFF_GROWTH_RATE, CLIENT_KNOBS->RESOURCE_CONSTRAINED_MAX_BACKOFF);
	} else {
		backoff = std::min(backoff * CLIENT_KNOBS->BACKOFF_GROWTH_RATE, options.maxBackoff);
	}

	return returnedBackoff;
}

TransactionOptions::TransactionOptions(Database const& cx) {
	reset(cx);
	if (BUGGIFY) {
		commitOnFirstProxy = true;
	}
}

void TransactionOptions::clear() {
	maxBackoff = CLIENT_KNOBS->DEFAULT_MAX_BACKOFF;
	getReadVersionFlags = 0;
	sizeLimit = CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT;
	maxTransactionLoggingFieldLength = 0;
	checkWritesEnabled = false;
	causalWriteRisky = false;
	commitOnFirstProxy = false;
	debugDump = false;
	lockAware = false;
	readOnly = false;
	firstInBatch = false;
	includePort = false;
	reportConflictingKeys = false;
	tags = TagSet{};
	readTags = TagSet{};
	priority = TransactionPriority::DEFAULT;
	expensiveClearCostEstimation = false;
}

TransactionOptions::TransactionOptions() {
	clear();
}

void TransactionOptions::reset(Database const& cx) {
	clear();
	lockAware = cx->lockAware;
	if (cx->apiVersionAtLeast(630)) {
		includePort = true;
	}
}

void Transaction::reset() {
	tr = CommitTransactionRequest();
	readVersion = Future<Version>();
	metadataVersion = Promise<Optional<Key>>();
	extraConflictRanges.clear();
	versionstampPromise = Promise<Standalone<StringRef>>();
	commitResult = Promise<Void>();
	committing = Future<Void>();
	info.taskID = cx->taskID;
	info.debugID = Optional<UID>();
	flushTrLogsIfEnabled();
	trLogInfo = Reference<TransactionLogInfo>(createTrLogInfoProbabilistically(cx));
	cancelWatches();

	if (apiVersionAtLeast(16)) {
		options.reset(cx);
	}
}

void Transaction::fullReset() {
	reset();
	span = Span(span.location);
	info.spanID = span.context;
	backoff = CLIENT_KNOBS->DEFAULT_BACKOFF;
}

int Transaction::apiVersionAtLeast(int minVersion) const {
	return cx->apiVersionAtLeast(minVersion);
}

class MutationBlock {
public:
	bool mutated;
	bool cleared;
	ValueRef setValue;

	MutationBlock() : mutated(false) {}
	MutationBlock(bool _cleared) : mutated(true), cleared(_cleared) {}
	MutationBlock(ValueRef value) : mutated(true), cleared(false), setValue(value) {}
};

bool compareBegin(KeyRangeRef lhs, KeyRangeRef rhs) {
	return lhs.begin < rhs.begin;
}

// If there is any intersection between the two given sets of ranges, returns a range that
//   falls within the intersection
Optional<KeyRangeRef> intersects(VectorRef<KeyRangeRef> lhs, VectorRef<KeyRangeRef> rhs) {
	if (lhs.size() && rhs.size()) {
		std::sort(lhs.begin(), lhs.end(), compareBegin);
		std::sort(rhs.begin(), rhs.end(), compareBegin);

		int l = 0, r = 0;
		while (l < lhs.size() && r < rhs.size()) {
			if (lhs[l].end <= rhs[r].begin)
				l++;
			else if (rhs[r].end <= lhs[l].begin)
				r++;
			else
				return lhs[l] & rhs[r];
		}
	}

	return Optional<KeyRangeRef>();
}

ACTOR void checkWrites(Database cx,
                       Future<Void> committed,
                       Promise<Void> outCommitted,
                       CommitTransactionRequest req,
                       Transaction* checkTr) {
	state Version version;
	try {
		wait(committed);
		// If the commit is successful, by definition the transaction still exists for now.  Grab the version, and don't
		// use it again.
		version = checkTr->getCommittedVersion();
		outCommitted.send(Void());
	} catch (Error& e) {
		outCommitted.sendError(e);
		return;
	}

	wait(delay(deterministicRandom()->random01())); // delay between 0 and 1 seconds

	// Future<Optional<Version>> version, Database cx, CommitTransactionRequest req ) {
	state KeyRangeMap<MutationBlock> expectedValues;

	auto& mutations = req.transaction.mutations;
	state int mCount = mutations.size(); // debugging info for traceEvent

	for (int idx = 0; idx < mutations.size(); idx++) {
		if (mutations[idx].type == MutationRef::SetValue)
			expectedValues.insert(singleKeyRange(mutations[idx].param1), MutationBlock(mutations[idx].param2));
		else if (mutations[idx].type == MutationRef::ClearRange)
			expectedValues.insert(KeyRangeRef(mutations[idx].param1, mutations[idx].param2), MutationBlock(true));
	}

	try {
		state Transaction tr(cx);
		tr.setVersion(version);
		state int checkedRanges = 0;
		state KeyRangeMap<MutationBlock>::Ranges ranges = expectedValues.ranges();
		state KeyRangeMap<MutationBlock>::iterator it = ranges.begin();
		for (; it != ranges.end(); ++it) {
			state MutationBlock m = it->value();
			if (m.mutated) {
				checkedRanges++;
				if (m.cleared) {
					RangeResult shouldBeEmpty = wait(tr.getRange(it->range(), 1));
					if (shouldBeEmpty.size()) {
						TraceEvent(SevError, "CheckWritesFailed")
						    .detail("Class", "Clear")
						    .detail("KeyBegin", it->range().begin)
						    .detail("KeyEnd", it->range().end);
						return;
					}
				} else {
					Optional<Value> val = wait(tr.get(it->range().begin));
					if (!val.present() || val.get() != m.setValue) {
						TraceEvent evt(SevError, "CheckWritesFailed");
						evt.detail("Class", "Set").detail("Key", it->range().begin).detail("Expected", m.setValue);
						if (!val.present())
							evt.detail("Actual", "_Value Missing_");
						else
							evt.detail("Actual", val.get());
						return;
					}
				}
			}
		}
		TraceEvent("CheckWritesSuccess")
		    .detail("Version", version)
		    .detail("MutationCount", mCount)
		    .detail("CheckedRanges", checkedRanges);
	} catch (Error& e) {
		bool ok = e.code() == error_code_transaction_too_old || e.code() == error_code_future_version;
		TraceEvent(ok ? SevWarn : SevError, "CheckWritesFailed").error(e);
		throw;
	}
}

ACTOR static Future<Void> commitDummyTransaction(Database cx,
                                                 KeyRange range,
                                                 TransactionInfo info,
                                                 TransactionOptions options) {
	state Transaction tr(cx);
	state int retries = 0;
	state Span span("NAPI:dummyTransaction"_loc, info.spanID);
	tr.span.addParent(span.context);
	loop {
		try {
			TraceEvent("CommitDummyTransaction").detail("Key", range.begin).detail("Retries", retries);
			tr.options = options;
			tr.info.taskID = info.taskID;
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::CAUSAL_WRITE_RISKY);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			tr.addReadConflictRange(range);
			tr.addWriteConflictRange(range);
			wait(tr.commit());
			return Void();
		} catch (Error& e) {
			TraceEvent("CommitDummyTransactionError")
			    .error(e, true)
			    .detail("Key", range.begin)
			    .detail("Retries", retries);
			wait(tr.onError(e));
		}
		++retries;
	}
}

void Transaction::cancelWatches(Error const& e) {
	for (int i = 0; i < watches.size(); ++i)
		if (!watches[i]->onChangeTrigger.isSet())
			watches[i]->onChangeTrigger.sendError(e);

	watches.clear();
}

void Transaction::setupWatches() {
	try {
		Future<Version> watchVersion = getCommittedVersion() > 0 ? getCommittedVersion() : getReadVersion();

		for (int i = 0; i < watches.size(); ++i)
			watches[i]->setWatch(
			    watchValueMap(watchVersion, watches[i]->key, watches[i]->value, cx, info, options.readTags));

		watches.clear();
	} catch (Error&) {
		ASSERT(false); // The above code must NOT throw because commit has already occured.
		throw internal_error();
	}
}

ACTOR Future<Optional<ClientTrCommitCostEstimation>> estimateCommitCosts(Transaction* self,
                                                                         CommitTransactionRef const* transaction) {
	state ClientTrCommitCostEstimation trCommitCosts;
	state KeyRangeRef keyRange;
	state int i = 0;

	for (; i < transaction->mutations.size(); ++i) {
		auto* it = &transaction->mutations[i];

		if (it->type == MutationRef::Type::SetValue || it->isAtomicOp()) {
			trCommitCosts.opsCount++;
			trCommitCosts.writeCosts += getWriteOperationCost(it->expectedSize());
		} else if (it->type == MutationRef::Type::ClearRange) {
			trCommitCosts.opsCount++;
			keyRange = KeyRangeRef(it->param1, it->param2);
			if (self->options.expensiveClearCostEstimation) {
				StorageMetrics m = wait(self->getStorageMetrics(keyRange, CLIENT_KNOBS->TOO_MANY));
				trCommitCosts.clearIdxCosts.emplace_back(i, getWriteOperationCost(m.bytes));
				trCommitCosts.writeCosts += getWriteOperationCost(m.bytes);
				++trCommitCosts.expensiveCostEstCount;
				++self->getDatabase()->transactionsExpensiveClearCostEstCount;
			} else {
				std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations =
				    wait(getKeyRangeLocations(self->getDatabase(),
				                              keyRange,
				                              CLIENT_KNOBS->TOO_MANY,
				                              Reverse::False,
				                              &StorageServerInterface::getShardState,
				                              self->info));
				if (locations.empty()) {
					continue;
				}

				uint64_t bytes = 0;
				if (locations.size() == 1) {
					bytes = CLIENT_KNOBS->INCOMPLETE_SHARD_PLUS;
				} else { // small clear on the boundary will hit two shards but be much smaller than the shard size
					bytes = CLIENT_KNOBS->INCOMPLETE_SHARD_PLUS * 2 +
					        (locations.size() - 2) * (int64_t)self->getDatabase()->smoothMidShardSize.smoothTotal();
				}

				trCommitCosts.clearIdxCosts.emplace_back(i, getWriteOperationCost(bytes));
				trCommitCosts.writeCosts += getWriteOperationCost(bytes);
			}
		}
	}

	// sample on written bytes
	if (!self->getDatabase()->sampleOnCost(trCommitCosts.writeCosts))
		return Optional<ClientTrCommitCostEstimation>();

	// sample clear op: the expectation of #sampledOp is every COMMIT_SAMPLE_COST sample once
	// we also scale the cost of mutations whose cost is less than COMMIT_SAMPLE_COST as scaledCost =
	// min(COMMIT_SAMPLE_COST, cost) If we have 4 transactions: A - 100 1-cost mutations: E[sampled ops] = 1, E[sampled
	// cost] = 100 B - 1 100-cost mutation: E[sampled ops] = 1, E[sampled cost] = 100 C - 50 2-cost mutations: E[sampled
	// ops] = 1, E[sampled cost] = 100 D - 1 150-cost mutation and 150 1-cost mutations: E[sampled ops] = 3, E[sampled
	// cost] = 150cost * 1 + 150 * 100cost * 0.01 = 300
	ASSERT(trCommitCosts.writeCosts > 0);
	std::deque<std::pair<int, uint64_t>> newClearIdxCosts;
	for (const auto& [idx, cost] : trCommitCosts.clearIdxCosts) {
		if (trCommitCosts.writeCosts >= CLIENT_KNOBS->COMMIT_SAMPLE_COST) {
			double mul = trCommitCosts.writeCosts / std::max(1.0, (double)CLIENT_KNOBS->COMMIT_SAMPLE_COST);
			if (deterministicRandom()->random01() < cost * mul / trCommitCosts.writeCosts) {
				newClearIdxCosts.emplace_back(
				    idx, cost < CLIENT_KNOBS->COMMIT_SAMPLE_COST ? CLIENT_KNOBS->COMMIT_SAMPLE_COST : cost);
			}
		} else if (deterministicRandom()->random01() < (double)cost / trCommitCosts.writeCosts) {
			newClearIdxCosts.emplace_back(
			    idx, cost < CLIENT_KNOBS->COMMIT_SAMPLE_COST ? CLIENT_KNOBS->COMMIT_SAMPLE_COST : cost);
		}
	}

	trCommitCosts.clearIdxCosts.swap(newClearIdxCosts);
	return trCommitCosts;
}

ACTOR static Future<Void> tryCommit(Database cx,
                                    Reference<TransactionLogInfo> trLogInfo,
                                    CommitTransactionRequest req,
                                    Future<Version> readVersion,
                                    TransactionInfo info,
                                    Version* pCommittedVersion,
                                    Transaction* tr,
                                    TransactionOptions options) {
	state TraceInterval interval("TransactionCommit");
	state double startTime = now();
	state Span span("NAPI:tryCommit"_loc, info.spanID);
	req.spanContext = span.context;
	if (info.debugID.present())
		TraceEvent(interval.begin()).detail("Parent", info.debugID.get());
	try {
		if (CLIENT_BUGGIFY) {
			throw deterministicRandom()->randomChoice(std::vector<Error>{
			    not_committed(), transaction_too_old(), proxy_memory_limit_exceeded(), commit_unknown_result() });
		}

		if (req.tagSet.present() && tr->options.priority < TransactionPriority::IMMEDIATE) {
			wait(store(req.transaction.read_snapshot, readVersion) &&
			     store(req.commitCostEstimation, estimateCommitCosts(tr, &req.transaction)));
		} else {
			wait(store(req.transaction.read_snapshot, readVersion));
		}

		startTime = now();
		state Optional<UID> commitID = Optional<UID>();
		if (info.debugID.present()) {
			commitID = nondeterministicRandom()->randomUniqueID();
			g_traceBatch.addAttach("CommitAttachID", info.debugID.get().first(), commitID.get().first());
			g_traceBatch.addEvent("CommitDebug", commitID.get().first(), "NativeAPI.commit.Before");
		}

		req.debugID = commitID;
		state Future<CommitID> reply;
		if (options.commitOnFirstProxy) {
			if (cx->clientInfo->get().firstCommitProxy.present()) {
				reply = throwErrorOr(brokenPromiseToMaybeDelivered(
				    cx->clientInfo->get().firstCommitProxy.get().commit.tryGetReply(req)));
			} else {
				const std::vector<CommitProxyInterface>& proxies = cx->clientInfo->get().commitProxies;
				reply = proxies.size() ? throwErrorOr(brokenPromiseToMaybeDelivered(proxies[0].commit.tryGetReply(req)))
				                       : Never();
			}
		} else {
			reply = basicLoadBalance(cx->getCommitProxies(info.useProvisionalProxies),
			                         &CommitProxyInterface::commit,
			                         req,
			                         TaskPriority::DefaultPromiseEndpoint,
			                         AtMostOnce::True);
		}

		choose {
			when(wait(cx->onProxiesChanged())) {
				reply.cancel();
				throw request_maybe_delivered();
			}
			when(CommitID ci = wait(reply)) {
				Version v = ci.version;
				if (v != invalidVersion) {
					if (CLIENT_BUGGIFY) {
						throw commit_unknown_result();
					}
					if (info.debugID.present())
						TraceEvent(interval.end()).detail("CommittedVersion", v);
					*pCommittedVersion = v;
					if (v > cx->metadataVersionCache[cx->mvCacheInsertLocation].first) {
						cx->mvCacheInsertLocation = (cx->mvCacheInsertLocation + 1) % cx->metadataVersionCache.size();
						cx->metadataVersionCache[cx->mvCacheInsertLocation] = std::make_pair(v, ci.metadataVersion);
					}

					Standalone<StringRef> ret = makeString(10);
					placeVersionstamp(mutateString(ret), v, ci.txnBatchId);
					tr->versionstampPromise.send(ret);

					tr->numErrors = 0;
					++cx->transactionsCommitCompleted;
					cx->transactionCommittedMutations += req.transaction.mutations.size();
					cx->transactionCommittedMutationBytes += req.transaction.mutations.expectedSize();

					if (info.debugID.present())
						g_traceBatch.addEvent("CommitDebug", commitID.get().first(), "NativeAPI.commit.After");

					double latency = now() - startTime;
					cx->commitLatencies.addSample(latency);
					cx->latencies.addSample(now() - tr->startTime);
					if (trLogInfo)
						trLogInfo->addLog(FdbClientLogEvents::EventCommit_V2(startTime,
						                                                     cx->clientLocality.dcId(),
						                                                     latency,
						                                                     req.transaction.mutations.size(),
						                                                     req.transaction.mutations.expectedSize(),
						                                                     ci.version,
						                                                     req));
					return Void();
				} else {
					// clear the RYW transaction which contains previous conflicting keys
					tr->info.conflictingKeys.reset();
					if (ci.conflictingKRIndices.present()) {
						tr->info.conflictingKeys =
						    std::make_shared<CoalescedKeyRangeMap<Value>>(conflictingKeysFalse, specialKeys.end);
						state Standalone<VectorRef<int>> conflictingKRIndices = ci.conflictingKRIndices.get();
						// drop duplicate indices and merge overlapped ranges
						// Note: addReadConflictRange in native transaction object does not merge overlapped ranges
						state std::unordered_set<int> mergedIds(conflictingKRIndices.begin(),
						                                        conflictingKRIndices.end());
						for (auto const& rCRIndex : mergedIds) {
							const KeyRangeRef kr = req.transaction.read_conflict_ranges[rCRIndex];
							const KeyRange krWithPrefix = KeyRangeRef(kr.begin.withPrefix(conflictingKeysRange.begin),
							                                          kr.end.withPrefix(conflictingKeysRange.begin));
							tr->info.conflictingKeys->insert(krWithPrefix, conflictingKeysTrue);
						}
					}

					if (info.debugID.present())
						TraceEvent(interval.end()).detail("Conflict", 1);

					if (info.debugID.present())
						g_traceBatch.addEvent("CommitDebug", commitID.get().first(), "NativeAPI.commit.After");

					throw not_committed();
				}
			}
		}
	} catch (Error& e) {
		if (e.code() == error_code_request_maybe_delivered || e.code() == error_code_commit_unknown_result) {
			// We don't know if the commit happened, and it might even still be in flight.

			if (!options.causalWriteRisky) {
				// Make sure it's not still in flight, either by ensuring the master we submitted to is dead, or the
				// version we submitted with is dead, or by committing a conflicting transaction successfully
				// if ( cx->getCommitProxies()->masterGeneration <= originalMasterGeneration )

				// To ensure the original request is not in flight, we need a key range which intersects its read
				// conflict ranges We pick a key range which also intersects its write conflict ranges, since that
				// avoids potentially creating conflicts where there otherwise would be none We make the range as small
				// as possible (a single key range) to minimize conflicts The intersection will never be empty, because
				// if it were (since !causalWriteRisky) makeSelfConflicting would have been applied automatically to req
				KeyRangeRef selfConflictingRange =
				    intersects(req.transaction.write_conflict_ranges, req.transaction.read_conflict_ranges).get();

				TEST(true); // Waiting for dummy transaction to report commit_unknown_result

				wait(commitDummyTransaction(cx, singleKeyRange(selfConflictingRange.begin), info, tr->options));
			}

			// The user needs to be informed that we aren't sure whether the commit happened.  Standard retry loops
			// retry it anyway (relying on transaction idempotence) but a client might do something else.
			throw commit_unknown_result();
		} else {
			if (e.code() != error_code_transaction_too_old && e.code() != error_code_not_committed &&
			    e.code() != error_code_database_locked && e.code() != error_code_proxy_memory_limit_exceeded &&
			    e.code() != error_code_batch_transaction_throttled && e.code() != error_code_tag_throttled) {
				TraceEvent(SevError, "TryCommitError").error(e);
			}
			if (trLogInfo)
				trLogInfo->addLog(FdbClientLogEvents::EventCommitError(
				    startTime, cx->clientLocality.dcId(), static_cast<int>(e.code()), req));
			throw;
		}
	}
}

Future<Void> Transaction::commitMutations() {
	try {
		// if this is a read-only transaction return immediately
		if (!tr.transaction.write_conflict_ranges.size() && !tr.transaction.mutations.size()) {
			numErrors = 0;

			committedVersion = invalidVersion;
			versionstampPromise.sendError(no_commit_version());
			return Void();
		}

		++cx->transactionsCommitStarted;

		if (options.readOnly)
			return transaction_read_only();

		cx->mutationsPerCommit.addSample(tr.transaction.mutations.size());
		cx->bytesPerCommit.addSample(tr.transaction.mutations.expectedSize());
		if (options.tags.size())
			tr.tagSet = options.tags;

		size_t transactionSize = getSize();
		if (transactionSize > (uint64_t)FLOW_KNOBS->PACKET_WARNING) {
			TraceEvent(!g_network->isSimulated() ? SevWarnAlways : SevWarn, "LargeTransaction")
			    .suppressFor(1.0)
			    .detail("Size", transactionSize)
			    .detail("NumMutations", tr.transaction.mutations.size())
			    .detail("ReadConflictSize", tr.transaction.read_conflict_ranges.expectedSize())
			    .detail("WriteConflictSize", tr.transaction.write_conflict_ranges.expectedSize())
			    .detail("DebugIdentifier", trLogInfo ? trLogInfo->identifier : "");
		}

		if (!apiVersionAtLeast(300)) {
			transactionSize =
			    tr.transaction.mutations.expectedSize(); // Old API versions didn't account for conflict ranges when
			                                             // determining whether to throw transaction_too_large
		}

		if (transactionSize > options.sizeLimit) {
			return transaction_too_large();
		}

		if (!readVersion.isValid())
			getReadVersion(
			    GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY); // sets up readVersion field.  We had no reads, so no
			                                                    // need for (expensive) full causal consistency.

		bool isCheckingWrites = options.checkWritesEnabled && deterministicRandom()->random01() < 0.01;
		for (int i = 0; i < extraConflictRanges.size(); i++)
			if (extraConflictRanges[i].isReady() &&
			    extraConflictRanges[i].get().first < extraConflictRanges[i].get().second)
				tr.transaction.read_conflict_ranges.emplace_back(
				    tr.arena, extraConflictRanges[i].get().first, extraConflictRanges[i].get().second);

		if (!options.causalWriteRisky &&
		    !intersects(tr.transaction.write_conflict_ranges, tr.transaction.read_conflict_ranges).present())
			makeSelfConflicting();

		if (isCheckingWrites) {
			// add all writes into the read conflict range...
			tr.transaction.read_conflict_ranges.append(
			    tr.arena, tr.transaction.write_conflict_ranges.begin(), tr.transaction.write_conflict_ranges.size());
		}

		if (options.debugDump) {
			UID u = nondeterministicRandom()->randomUniqueID();
			TraceEvent("TransactionDump", u).log();
			for (auto i = tr.transaction.mutations.begin(); i != tr.transaction.mutations.end(); ++i)
				TraceEvent("TransactionMutation", u)
				    .detail("T", i->type)
				    .detail("P1", i->param1)
				    .detail("P2", i->param2);
		}

		if (options.lockAware) {
			tr.flags = tr.flags | CommitTransactionRequest::FLAG_IS_LOCK_AWARE;
		}
		if (options.firstInBatch) {
			tr.flags = tr.flags | CommitTransactionRequest::FLAG_FIRST_IN_BATCH;
		}
		if (options.reportConflictingKeys) {
			tr.transaction.report_conflicting_keys = true;
		}

		Future<Void> commitResult =
		    tryCommit(cx, trLogInfo, tr, readVersion, info, &this->committedVersion, this, options);

		if (isCheckingWrites) {
			Promise<Void> committed;
			checkWrites(cx, commitResult, committed, tr, this);
			return committed.getFuture();
		}
		return commitResult;
	} catch (Error& e) {
		TraceEvent("ClientCommitError").error(e);
		return Future<Void>(e);
	} catch (...) {
		Error e(error_code_unknown_error);
		TraceEvent("ClientCommitError").error(e);
		return Future<Void>(e);
	}
}

ACTOR Future<Void> commitAndWatch(Transaction* self) {
	try {
		wait(self->commitMutations());

		if (!self->watches.empty()) {
			self->setupWatches();
		}

		if (!self->apiVersionAtLeast(700)) {
			self->reset();
		}

		return Void();
	} catch (Error& e) {
		if (e.code() != error_code_actor_cancelled) {
			if (!self->watches.empty()) {
				self->cancelWatches(e);
			}

			self->versionstampPromise.sendError(transaction_invalid_version());

			if (!self->apiVersionAtLeast(700)) {
				self->reset();
			}
		}

		throw;
	}
}

Future<Void> Transaction::commit() {
	ASSERT(!committing.isValid());
	committing = commitAndWatch(this);
	return committing;
}

void Transaction::setOption(FDBTransactionOptions::Option option, Optional<StringRef> value) {
	switch (option) {
	case FDBTransactionOptions::INITIALIZE_NEW_DATABASE:
		validateOptionValueNotPresent(value);
		if (readVersion.isValid())
			throw read_version_already_set();
		readVersion = Version(0);
		options.causalWriteRisky = true;
		break;

	case FDBTransactionOptions::CAUSAL_READ_RISKY:
		validateOptionValueNotPresent(value);
		options.getReadVersionFlags |= GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY;
		break;

	case FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE:
		validateOptionValueNotPresent(value);
		options.priority = TransactionPriority::IMMEDIATE;
		break;

	case FDBTransactionOptions::PRIORITY_BATCH:
		validateOptionValueNotPresent(value);
		options.priority = TransactionPriority::BATCH;
		break;

	case FDBTransactionOptions::CAUSAL_WRITE_RISKY:
		validateOptionValueNotPresent(value);
		options.causalWriteRisky = true;
		break;

	case FDBTransactionOptions::COMMIT_ON_FIRST_PROXY:
		validateOptionValueNotPresent(value);
		options.commitOnFirstProxy = true;
		break;

	case FDBTransactionOptions::CHECK_WRITES_ENABLE:
		validateOptionValueNotPresent(value);
		options.checkWritesEnabled = true;
		break;

	case FDBTransactionOptions::DEBUG_DUMP:
		validateOptionValueNotPresent(value);
		options.debugDump = true;
		break;

	case FDBTransactionOptions::TRANSACTION_LOGGING_ENABLE:
		setOption(FDBTransactionOptions::DEBUG_TRANSACTION_IDENTIFIER, value);
		setOption(FDBTransactionOptions::LOG_TRANSACTION);
		break;

	case FDBTransactionOptions::DEBUG_TRANSACTION_IDENTIFIER:
		validateOptionValuePresent(value);

		if (value.get().size() > 100 || value.get().size() == 0) {
			throw invalid_option_value();
		}

		if (trLogInfo) {
			if (trLogInfo->identifier.empty()) {
				trLogInfo->identifier = value.get().printable();
			} else if (trLogInfo->identifier != value.get().printable()) {
				TraceEvent(SevWarn, "CannotChangeDebugTransactionIdentifier")
				    .detail("PreviousIdentifier", trLogInfo->identifier)
				    .detail("NewIdentifier", value.get());
				throw client_invalid_operation();
			}
		} else {
			trLogInfo = makeReference<TransactionLogInfo>(value.get().printable(), TransactionLogInfo::DONT_LOG);
			trLogInfo->maxFieldLength = options.maxTransactionLoggingFieldLength;
		}
		if (info.debugID.present()) {
			TraceEvent(SevInfo, "TransactionBeingTraced")
			    .detail("DebugTransactionID", trLogInfo->identifier)
			    .detail("ServerTraceID", info.debugID.get());
		}
		break;

	case FDBTransactionOptions::LOG_TRANSACTION:
		validateOptionValueNotPresent(value);
		if (trLogInfo && !trLogInfo->identifier.empty()) {
			trLogInfo->logTo(TransactionLogInfo::TRACE_LOG);
		} else {
			TraceEvent(SevWarn, "DebugTransactionIdentifierNotSet")
			    .detail("Error", "Debug Transaction Identifier option must be set before logging the transaction");
			throw client_invalid_operation();
		}
		break;

	case FDBTransactionOptions::TRANSACTION_LOGGING_MAX_FIELD_LENGTH:
		validateOptionValuePresent(value);
		{
			int maxFieldLength = extractIntOption(value, -1, std::numeric_limits<int32_t>::max());
			if (maxFieldLength == 0) {
				throw invalid_option_value();
			}
			options.maxTransactionLoggingFieldLength = maxFieldLength;
		}
		if (trLogInfo) {
			trLogInfo->maxFieldLength = options.maxTransactionLoggingFieldLength;
		}
		break;

	case FDBTransactionOptions::SERVER_REQUEST_TRACING:
		validateOptionValueNotPresent(value);
		debugTransaction(deterministicRandom()->randomUniqueID());
		if (trLogInfo && !trLogInfo->identifier.empty()) {
			TraceEvent(SevInfo, "TransactionBeingTraced")
			    .detail("DebugTransactionID", trLogInfo->identifier)
			    .detail("ServerTraceID", info.debugID.get());
		}
		break;

	case FDBTransactionOptions::MAX_RETRY_DELAY:
		validateOptionValuePresent(value);
		options.maxBackoff = extractIntOption(value, 0, std::numeric_limits<int32_t>::max()) / 1000.0;
		break;

	case FDBTransactionOptions::SIZE_LIMIT:
		validateOptionValuePresent(value);
		options.sizeLimit = extractIntOption(value, 32, CLIENT_KNOBS->TRANSACTION_SIZE_LIMIT);
		break;

	case FDBTransactionOptions::LOCK_AWARE:
		validateOptionValueNotPresent(value);
		options.lockAware = true;
		options.readOnly = false;
		break;

	case FDBTransactionOptions::READ_LOCK_AWARE:
		validateOptionValueNotPresent(value);
		if (!options.lockAware) {
			options.lockAware = true;
			options.readOnly = true;
		}
		break;

	case FDBTransactionOptions::FIRST_IN_BATCH:
		validateOptionValueNotPresent(value);
		options.firstInBatch = true;
		break;

	case FDBTransactionOptions::USE_PROVISIONAL_PROXIES:
		validateOptionValueNotPresent(value);
		options.getReadVersionFlags |= GetReadVersionRequest::FLAG_USE_PROVISIONAL_PROXIES;
		info.useProvisionalProxies = true;
		break;

	case FDBTransactionOptions::INCLUDE_PORT_IN_ADDRESS:
		validateOptionValueNotPresent(value);
		options.includePort = true;
		break;

	case FDBTransactionOptions::TAG:
		validateOptionValuePresent(value);
		options.tags.addTag(value.get());
		break;

	case FDBTransactionOptions::AUTO_THROTTLE_TAG:
		validateOptionValuePresent(value);
		options.tags.addTag(value.get());
		options.readTags.addTag(value.get());
		break;

	case FDBTransactionOptions::SPAN_PARENT:
		validateOptionValuePresent(value);
		if (value.get().size() != 16) {
			throw invalid_option_value();
		}
		span.addParent(BinaryReader::fromStringRef<UID>(value.get(), Unversioned()));
		break;

	case FDBTransactionOptions::REPORT_CONFLICTING_KEYS:
		validateOptionValueNotPresent(value);
		options.reportConflictingKeys = true;
		break;

	case FDBTransactionOptions::EXPENSIVE_CLEAR_COST_ESTIMATION_ENABLE:
		validateOptionValueNotPresent(value);
		options.expensiveClearCostEstimation = true;
		break;

	default:
		break;
	}
}

ACTOR Future<GetReadVersionReply> getConsistentReadVersion(SpanID parentSpan,
                                                           DatabaseContext* cx,
                                                           uint32_t transactionCount,
                                                           TransactionPriority priority,
                                                           uint32_t flags,
                                                           TransactionTagMap<uint32_t> tags,
                                                           Optional<UID> debugID) {
	state Span span("NAPI:getConsistentReadVersion"_loc, parentSpan);

	++cx->transactionReadVersionBatches;
	if (debugID.present())
		g_traceBatch.addEvent("TransactionDebug", debugID.get().first(), "NativeAPI.getConsistentReadVersion.Before");
	loop {
		try {
			state GetReadVersionRequest req(span.context, transactionCount, priority, flags, tags, debugID);

			choose {
				when(wait(cx->onProxiesChanged())) {}
				when(GetReadVersionReply v = wait(basicLoadBalance(
				         cx->getGrvProxies(flags & GetReadVersionRequest::FLAG_USE_PROVISIONAL_PROXIES),
				         &GrvProxyInterface::getConsistentReadVersion,
				         req,
				         cx->taskID))) {
					if (tags.size() != 0) {
						auto& priorityThrottledTags = cx->throttledTags[priority];
						for (auto& tag : tags) {
							auto itr = v.tagThrottleInfo.find(tag.first);
							if (itr == v.tagThrottleInfo.end()) {
								TEST(true); // Removing client throttle
								priorityThrottledTags.erase(tag.first);
							} else {
								TEST(true); // Setting client throttle
								auto result = priorityThrottledTags.try_emplace(tag.first, itr->second);
								if (!result.second) {
									result.first->second.update(itr->second);
								}
							}
						}
					}

					if (debugID.present())
						g_traceBatch.addEvent(
						    "TransactionDebug", debugID.get().first(), "NativeAPI.getConsistentReadVersion.After");
					ASSERT(v.version > 0);
					cx->minAcceptableReadVersion = std::min(cx->minAcceptableReadVersion, v.version);
					return v;
				}
			}
		} catch (Error& e) {
			if (e.code() != error_code_broken_promise && e.code() != error_code_batch_transaction_throttled)
				TraceEvent(SevError, "GetConsistentReadVersionError").error(e);
			if (e.code() == error_code_batch_transaction_throttled && !cx->apiVersionAtLeast(630)) {
				wait(delayJittered(5.0));
			} else {
				throw;
			}
		}
	}
}

ACTOR Future<Void> readVersionBatcher(DatabaseContext* cx,
                                      FutureStream<DatabaseContext::VersionRequest> versionStream,
                                      TransactionPriority priority,
                                      uint32_t flags) {
	state std::vector<Promise<GetReadVersionReply>> requests;
	state PromiseStream<Future<Void>> addActor;
	state Future<Void> collection = actorCollection(addActor.getFuture());
	state Future<Void> timeout;
	state Optional<UID> debugID;
	state bool send_batch;
	state Reference<Histogram> batchSizeDist = Histogram::getHistogram(LiteralStringRef("GrvBatcher"),
	                                                                   LiteralStringRef("ClientGrvBatchSize"),
	                                                                   Histogram::Unit::countLinear,
	                                                                   0,
	                                                                   CLIENT_KNOBS->MAX_BATCH_SIZE * 2);
	state Reference<Histogram> batchIntervalDist =
	    Histogram::getHistogram(LiteralStringRef("GrvBatcher"),
	                            LiteralStringRef("ClientGrvBatchInterval"),
	                            Histogram::Unit::microseconds,
	                            0,
	                            CLIENT_KNOBS->GRV_BATCH_TIMEOUT * 1000000 * 2);
	state Reference<Histogram> grvReplyLatencyDist = Histogram::getHistogram(
	    LiteralStringRef("GrvBatcher"), LiteralStringRef("ClientGrvReplyLatency"), Histogram::Unit::microseconds);
	state double lastRequestTime = now();

	state TransactionTagMap<uint32_t> tags;

	// dynamic batching
	state PromiseStream<double> replyTimes;
	state PromiseStream<Error> _errorStream;
	state double batchTime = 0;
	state Span span("NAPI:readVersionBatcher"_loc);
	loop {
		send_batch = false;
		choose {
			when(DatabaseContext::VersionRequest req = waitNext(versionStream)) {
				if (req.debugID.present()) {
					if (!debugID.present()) {
						debugID = nondeterministicRandom()->randomUniqueID();
					}
					g_traceBatch.addAttach("TransactionAttachID", req.debugID.get().first(), debugID.get().first());
				}
				span.addParent(req.spanContext);
				requests.push_back(req.reply);
				for (auto tag : req.tags) {
					++tags[tag];
				}

				if (requests.size() == CLIENT_KNOBS->MAX_BATCH_SIZE) {
					send_batch = true;
					++cx->transactionGrvFullBatches;
				} else if (!timeout.isValid()) {
					timeout = delay(batchTime, TaskPriority::GetConsistentReadVersion);
				}
			}
			when(wait(timeout.isValid() ? timeout : Never())) {
				send_batch = true;
				++cx->transactionGrvTimedOutBatches;
			}
			// dynamic batching monitors reply latencies
			when(double reply_latency = waitNext(replyTimes.getFuture())) {
				double target_latency = reply_latency * 0.5;
				batchTime = std::min(0.1 * target_latency + 0.9 * batchTime, CLIENT_KNOBS->GRV_BATCH_TIMEOUT);
				grvReplyLatencyDist->sampleSeconds(reply_latency);
			}
			when(wait(collection)) {} // for errors
		}
		if (send_batch) {
			int count = requests.size();
			ASSERT(count);

			batchSizeDist->sampleRecordCounter(count);
			auto requestTime = now();
			batchIntervalDist->sampleSeconds(requestTime - lastRequestTime);
			lastRequestTime = requestTime;

			// dynamic batching
			Promise<GetReadVersionReply> GRVReply;
			requests.push_back(GRVReply);
			addActor.send(ready(timeReply(GRVReply.getFuture(), replyTimes)));

			Future<Void> batch = incrementalBroadcastWithError(
			    getConsistentReadVersion(span.context, cx, count, priority, flags, std::move(tags), std::move(debugID)),
			    std::move(requests),
			    CLIENT_KNOBS->BROADCAST_BATCH_SIZE);

			span = Span("NAPI:readVersionBatcher"_loc);
			tags.clear();
			debugID = Optional<UID>();
			requests.clear();
			addActor.send(batch);
			timeout = Future<Void>();
		}
	}
}

ACTOR Future<Version> extractReadVersion(Location location,
                                         SpanID spanContext,
                                         SpanID parent,
                                         DatabaseContext* cx,
                                         TransactionPriority priority,
                                         Reference<TransactionLogInfo> trLogInfo,
                                         Future<GetReadVersionReply> f,
                                         LockAware lockAware,
                                         double startTime,
                                         Promise<Optional<Value>> metadataVersion,
                                         TagSet tags) {
	state Span span(spanContext, location, { parent });
	GetReadVersionReply rep = wait(f);
	double latency = now() - startTime;
	cx->GRVLatencies.addSample(latency);
	if (trLogInfo)
		trLogInfo->addLog(FdbClientLogEvents::EventGetVersion_V3(
		    startTime, cx->clientLocality.dcId(), latency, priority, rep.version));
	if (rep.locked && !lockAware)
		throw database_locked();

	++cx->transactionReadVersionsCompleted;
	switch (priority) {
	case TransactionPriority::IMMEDIATE:
		++cx->transactionImmediateReadVersionsCompleted;
		break;
	case TransactionPriority::DEFAULT:
		++cx->transactionDefaultReadVersionsCompleted;
		break;
	case TransactionPriority::BATCH:
		++cx->transactionBatchReadVersionsCompleted;
		break;
	default:
		ASSERT(false);
	}

	if (tags.size() != 0) {
		auto& priorityThrottledTags = cx->throttledTags[priority];
		for (auto& tag : tags) {
			auto itr = priorityThrottledTags.find(tag);
			if (itr != priorityThrottledTags.end()) {
				if (itr->second.expired()) {
					priorityThrottledTags.erase(itr);
				} else if (itr->second.throttleDuration() > 0) {
					TEST(true); // throttling transaction after getting read version
					++cx->transactionReadVersionsThrottled;
					throw tag_throttled();
				}
			}
		}

		for (auto& tag : tags) {
			auto itr = priorityThrottledTags.find(tag);
			if (itr != priorityThrottledTags.end()) {
				itr->second.addReleased(1);
			}
		}
	}

	if (rep.version > cx->metadataVersionCache[cx->mvCacheInsertLocation].first) {
		cx->mvCacheInsertLocation = (cx->mvCacheInsertLocation + 1) % cx->metadataVersionCache.size();
		cx->metadataVersionCache[cx->mvCacheInsertLocation] = std::make_pair(rep.version, rep.metadataVersion);
	}

	metadataVersion.send(rep.metadataVersion);
	return rep.version;
}

Future<Version> Transaction::getReadVersion(uint32_t flags) {
	if (!readVersion.isValid()) {
		++cx->transactionReadVersions;
		flags |= options.getReadVersionFlags;
		switch (options.priority) {
		case TransactionPriority::IMMEDIATE:
			flags |= GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE;
			++cx->transactionImmediateReadVersions;
			break;
		case TransactionPriority::DEFAULT:
			flags |= GetReadVersionRequest::PRIORITY_DEFAULT;
			++cx->transactionDefaultReadVersions;
			break;
		case TransactionPriority::BATCH:
			flags |= GetReadVersionRequest::PRIORITY_BATCH;
			++cx->transactionBatchReadVersions;
			break;
		default:
			ASSERT(false);
		}

		if (options.tags.size() != 0) {
			double maxThrottleDelay = 0.0;
			bool canRecheck = false;

			auto& priorityThrottledTags = cx->throttledTags[options.priority];
			for (auto& tag : options.tags) {
				auto itr = priorityThrottledTags.find(tag);
				if (itr != priorityThrottledTags.end()) {
					if (!itr->second.expired()) {
						maxThrottleDelay = std::max(maxThrottleDelay, itr->second.throttleDuration());
						canRecheck = itr->second.canRecheck();
					} else {
						priorityThrottledTags.erase(itr);
					}
				}
			}

			if (maxThrottleDelay > 0.0 && !canRecheck) { // TODO: allow delaying?
				TEST(true); // Throttling tag before GRV request
				++cx->transactionReadVersionsThrottled;
				readVersion = tag_throttled();
				return readVersion;
			} else {
				TEST(maxThrottleDelay > 0.0); // Rechecking throttle
			}

			for (auto& tag : options.tags) {
				auto itr = priorityThrottledTags.find(tag);
				if (itr != priorityThrottledTags.end()) {
					itr->second.updateChecked();
				}
			}
		}

		auto& batcher = cx->versionBatcher[flags];
		if (!batcher.actor.isValid()) {
			batcher.actor = readVersionBatcher(cx.getPtr(), batcher.stream.getFuture(), options.priority, flags);
		}

		Location location = "NAPI:getReadVersion"_loc;
		UID spanContext = generateSpanID(cx->transactionTracingEnabled);
		auto const req = DatabaseContext::VersionRequest(spanContext, options.tags, info.debugID);
		batcher.stream.send(req);
		startTime = now();
		readVersion = extractReadVersion(location,
		                                 spanContext,
		                                 info.spanID,
		                                 cx.getPtr(),
		                                 options.priority,
		                                 trLogInfo,
		                                 req.reply.getFuture(),
		                                 LockAware{ options.lockAware },
		                                 startTime,
		                                 metadataVersion,
		                                 options.tags);
	}
	return readVersion;
}

Optional<Version> Transaction::getCachedReadVersion() const {
	if (readVersion.isValid() && readVersion.isReady() && !readVersion.isError()) {
		return readVersion.get();
	} else {
		return Optional<Version>();
	}
}

Future<Standalone<StringRef>> Transaction::getVersionstamp() {
	if (committing.isValid()) {
		return transaction_invalid_version();
	}
	return versionstampPromise.getFuture();
}

// Gets the protocol version reported by a coordinator via the protocol info interface
ACTOR Future<ProtocolVersion> getCoordinatorProtocol(NetworkAddressList coordinatorAddresses) {
	RequestStream<ProtocolInfoRequest> requestStream{ Endpoint::wellKnown({ coordinatorAddresses },
		                                                                  WLTOKEN_PROTOCOL_INFO) };
	ProtocolInfoReply reply = wait(retryBrokenPromise(requestStream, ProtocolInfoRequest{}));

	return reply.version;
}

// Gets the protocol version reported by a coordinator in its connect packet
// If we are unable to get a version from the connect packet (e.g. because we lost connection with the peer), then this
// function will return with an unset result.
// If an expected version is given, this future won't return if the actual protocol version matches the expected version
ACTOR Future<Optional<ProtocolVersion>> getCoordinatorProtocolFromConnectPacket(
    NetworkAddress coordinatorAddress,
    Optional<ProtocolVersion> expectedVersion) {

	state Reference<AsyncVar<Optional<ProtocolVersion>> const> protocolVersion =
	    FlowTransport::transport().getPeerProtocolAsyncVar(coordinatorAddress);

	loop {
		if (protocolVersion->get().present() && protocolVersion->get() != expectedVersion) {
			return protocolVersion->get();
		}

		Future<Void> change = protocolVersion->onChange();
		if (!protocolVersion->get().present()) {
			// If we still don't have any connection info after a timeout, retry sending the protocol version request
			change = timeout(change, FLOW_KNOBS->CONNECTION_MONITOR_TIMEOUT, Void());
		}

		wait(change);

		if (!protocolVersion->get().present()) {
			return protocolVersion->get();
		}
	}
}

// Returns the protocol version reported by the given coordinator
// If an expected version is given, the future won't return until the protocol version is different than expected
ACTOR Future<ProtocolVersion> getClusterProtocolImpl(
    Reference<AsyncVar<Optional<ClientLeaderRegInterface>> const> coordinator,
    Optional<ProtocolVersion> expectedVersion) {

	state bool needToConnect = true;
	state Future<ProtocolVersion> protocolVersion = Never();

	loop {
		if (!coordinator->get().present()) {
			wait(coordinator->onChange());
		} else {
			Endpoint coordinatorEndpoint = coordinator->get().get().getLeader.getEndpoint();
			if (needToConnect) {
				// Even though we typically rely on the connect packet to get the protocol version, we need to send some
				// request in order to start a connection. This protocol version request serves that purpose.
				protocolVersion = getCoordinatorProtocol(coordinatorEndpoint.addresses);
				needToConnect = false;
			}
			choose {
				when(wait(coordinator->onChange())) { needToConnect = true; }

				when(ProtocolVersion pv = wait(protocolVersion)) {
					if (!expectedVersion.present() || expectedVersion.get() != pv) {
						return pv;
					}

					protocolVersion = Never();
				}

				// Older versions of FDB don't have an endpoint to return the protocol version, so we get this info from
				// the connect packet
				when(Optional<ProtocolVersion> pv = wait(getCoordinatorProtocolFromConnectPacket(
				         coordinatorEndpoint.getPrimaryAddress(), expectedVersion))) {
					if (pv.present()) {
						return pv.get();
					} else {
						needToConnect = true;
					}
				}
			}
		}
	}
}

// Returns the protocol version reported by the coordinator this client is currently connected to
// If an expected version is given, the future won't return until the protocol version is different than expected
// Note: this will never return if the server is running a protocol from FDB 5.0 or older
Future<ProtocolVersion> DatabaseContext::getClusterProtocol(Optional<ProtocolVersion> expectedVersion) {
	return getClusterProtocolImpl(coordinator, expectedVersion);
}

uint32_t Transaction::getSize() {
	auto s = tr.transaction.mutations.expectedSize() + tr.transaction.read_conflict_ranges.expectedSize() +
	         tr.transaction.write_conflict_ranges.expectedSize();
	return s;
}

Future<Void> Transaction::onError(Error const& e) {
	if (e.code() == error_code_success) {
		return client_invalid_operation();
	}
	if (e.code() == error_code_not_committed || e.code() == error_code_commit_unknown_result ||
	    e.code() == error_code_database_locked || e.code() == error_code_proxy_memory_limit_exceeded ||
	    e.code() == error_code_process_behind || e.code() == error_code_batch_transaction_throttled ||
	    e.code() == error_code_tag_throttled) {
		if (e.code() == error_code_not_committed)
			++cx->transactionsNotCommitted;
		else if (e.code() == error_code_commit_unknown_result)
			++cx->transactionsMaybeCommitted;
		else if (e.code() == error_code_proxy_memory_limit_exceeded)
			++cx->transactionsResourceConstrained;
		else if (e.code() == error_code_process_behind)
			++cx->transactionsProcessBehind;
		else if (e.code() == error_code_batch_transaction_throttled || e.code() == error_code_tag_throttled) {
			++cx->transactionsThrottled;
		}

		double backoff = getBackoff(e.code());
		reset();
		return delay(backoff, info.taskID);
	}
	if (e.code() == error_code_transaction_too_old || e.code() == error_code_future_version) {
		if (e.code() == error_code_transaction_too_old)
			++cx->transactionsTooOld;
		else if (e.code() == error_code_future_version)
			++cx->transactionsFutureVersions;

		double maxBackoff = options.maxBackoff;
		reset();
		return delay(std::min(CLIENT_KNOBS->FUTURE_VERSION_RETRY_DELAY, maxBackoff), info.taskID);
	}

	if (g_network->isSimulated() && ++numErrors % 10 == 0)
		TraceEvent(SevWarnAlways, "TransactionTooManyRetries").detail("NumRetries", numErrors);

	return e;
}
ACTOR Future<StorageMetrics> getStorageMetricsLargeKeyRange(Database cx, KeyRange keys);

ACTOR Future<StorageMetrics> doGetStorageMetrics(Database cx, KeyRange keys, Reference<LocationInfo> locationInfo) {
	loop {
		try {
			WaitMetricsRequest req(keys, StorageMetrics(), StorageMetrics());
			req.min.bytes = 0;
			req.max.bytes = -1;
			StorageMetrics m = wait(loadBalance(
			    locationInfo->locations(), &StorageServerInterface::waitMetrics, req, TaskPriority::DataDistribution));
			return m;
		} catch (Error& e) {
			if (e.code() != error_code_wrong_shard_server && e.code() != error_code_all_alternatives_failed) {
				TraceEvent(SevError, "WaitStorageMetricsError").error(e);
				throw;
			}
			wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, TaskPriority::DataDistribution));
			cx->invalidateCache(keys);
			StorageMetrics m = wait(getStorageMetricsLargeKeyRange(cx, keys));
			return m;
		}
	}
}

ACTOR Future<StorageMetrics> getStorageMetricsLargeKeyRange(Database cx, KeyRange keys) {
	state Span span("NAPI:GetStorageMetricsLargeKeyRange"_loc);
	std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations =
	    wait(getKeyRangeLocations(cx,
	                              keys,
	                              std::numeric_limits<int>::max(),
	                              Reverse::False,
	                              &StorageServerInterface::waitMetrics,
	                              TransactionInfo(TaskPriority::DataDistribution, span.context)));
	state int nLocs = locations.size();
	state std::vector<Future<StorageMetrics>> fx(nLocs);
	state StorageMetrics total;
	KeyRef partBegin, partEnd;
	for (int i = 0; i < nLocs; i++) {
		partBegin = (i == 0) ? keys.begin : locations[i].first.begin;
		partEnd = (i == nLocs - 1) ? keys.end : locations[i].first.end;
		fx[i] = doGetStorageMetrics(cx, KeyRangeRef(partBegin, partEnd), locations[i].second);
	}
	wait(waitForAll(fx));
	for (int i = 0; i < nLocs; i++) {
		total += fx[i].get();
	}
	return total;
}

ACTOR Future<Void> trackBoundedStorageMetrics(KeyRange keys,
                                              Reference<LocationInfo> location,
                                              StorageMetrics x,
                                              StorageMetrics halfError,
                                              PromiseStream<StorageMetrics> deltaStream) {
	try {
		loop {
			WaitMetricsRequest req(keys, x - halfError, x + halfError);
			StorageMetrics nextX = wait(loadBalance(location->locations(), &StorageServerInterface::waitMetrics, req));
			deltaStream.send(nextX - x);
			x = nextX;
		}
	} catch (Error& e) {
		deltaStream.sendError(e);
		throw e;
	}
}

ACTOR Future<StorageMetrics> waitStorageMetricsMultipleLocations(
    std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations,
    StorageMetrics min,
    StorageMetrics max,
    StorageMetrics permittedError) {
	state int nLocs = locations.size();
	state std::vector<Future<StorageMetrics>> fx(nLocs);
	state StorageMetrics total;
	state PromiseStream<StorageMetrics> deltas;
	state std::vector<Future<Void>> wx(fx.size());
	state StorageMetrics halfErrorPerMachine = permittedError * (0.5 / nLocs);
	state StorageMetrics maxPlus = max + halfErrorPerMachine * (nLocs - 1);
	state StorageMetrics minMinus = min - halfErrorPerMachine * (nLocs - 1);

	for (int i = 0; i < nLocs; i++) {
		WaitMetricsRequest req(locations[i].first, StorageMetrics(), StorageMetrics());
		req.min.bytes = 0;
		req.max.bytes = -1;
		fx[i] = loadBalance(locations[i].second->locations(),
		                    &StorageServerInterface::waitMetrics,
		                    req,
		                    TaskPriority::DataDistribution);
	}
	wait(waitForAll(fx));

	// invariant: true total is between (total-permittedError/2, total+permittedError/2)
	for (int i = 0; i < nLocs; i++)
		total += fx[i].get();

	if (!total.allLessOrEqual(maxPlus))
		return total;
	if (!minMinus.allLessOrEqual(total))
		return total;

	for (int i = 0; i < nLocs; i++)
		wx[i] = trackBoundedStorageMetrics(
		    locations[i].first, locations[i].second, fx[i].get(), halfErrorPerMachine, deltas);

	loop {
		StorageMetrics delta = waitNext(deltas.getFuture());
		total += delta;
		if (!total.allLessOrEqual(maxPlus))
			return total;
		if (!minMinus.allLessOrEqual(total))
			return total;
	}
}

ACTOR Future<StorageMetrics> extractMetrics(Future<std::pair<Optional<StorageMetrics>, int>> fMetrics) {
	std::pair<Optional<StorageMetrics>, int> x = wait(fMetrics);
	return x.first.get();
}

ACTOR Future<Standalone<VectorRef<ReadHotRangeWithMetrics>>> getReadHotRanges(Database cx, KeyRange keys) {
	state Span span("NAPI:GetReadHotRanges"_loc);
	loop {
		int64_t shardLimit = 100; // Shard limit here does not really matter since this function is currently only used
		                          // to find the read-hot sub ranges within a read-hot shard.
		std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations =
		    wait(getKeyRangeLocations(cx,
		                              keys,
		                              shardLimit,
		                              Reverse::False,
		                              &StorageServerInterface::getReadHotRanges,
		                              TransactionInfo(TaskPriority::DataDistribution, span.context)));
		try {
			// TODO: how to handle this?
			// This function is called whenever a shard becomes read-hot. But somehow the shard was splitted across more
			// than one storage server after become read-hot and before this function is called, i.e. a race condition.
			// Should we abort and wait the newly splitted shards to be hot again?
			state int nLocs = locations.size();
			// if (nLocs > 1) {
			// 	TraceEvent("RHDDebug")
			// 	    .detail("NumSSIs", nLocs)
			// 	    .detail("KeysBegin", keys.begin.printable().c_str())
			// 	    .detail("KeysEnd", keys.end.printable().c_str());
			// }
			state std::vector<Future<ReadHotSubRangeReply>> fReplies(nLocs);
			KeyRef partBegin, partEnd;
			for (int i = 0; i < nLocs; i++) {
				partBegin = (i == 0) ? keys.begin : locations[i].first.begin;
				partEnd = (i == nLocs - 1) ? keys.end : locations[i].first.end;
				ReadHotSubRangeRequest req(KeyRangeRef(partBegin, partEnd));
				fReplies[i] = loadBalance(locations[i].second->locations(),
				                          &StorageServerInterface::getReadHotRanges,
				                          req,
				                          TaskPriority::DataDistribution);
			}

			wait(waitForAll(fReplies));

			if (nLocs == 1) {
				TEST(true); // Single-shard read hot range request
				return fReplies[0].get().readHotRanges;
			} else {
				TEST(true); // Multi-shard read hot range request
				Standalone<VectorRef<ReadHotRangeWithMetrics>> results;
				for (int i = 0; i < nLocs; i++) {
					results.append(results.arena(),
					               fReplies[i].get().readHotRanges.begin(),
					               fReplies[i].get().readHotRanges.size());
					results.arena().dependsOn(fReplies[i].get().readHotRanges.arena());
				}

				return results;
			}
		} catch (Error& e) {
			if (e.code() != error_code_wrong_shard_server && e.code() != error_code_all_alternatives_failed) {
				TraceEvent(SevError, "GetReadHotSubRangesError").error(e);
				throw;
			}
			cx->invalidateCache(keys);
			wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, TaskPriority::DataDistribution));
		}
	}
}

ACTOR Future<std::pair<Optional<StorageMetrics>, int>> waitStorageMetrics(Database cx,
                                                                          KeyRange keys,
                                                                          StorageMetrics min,
                                                                          StorageMetrics max,
                                                                          StorageMetrics permittedError,
                                                                          int shardLimit,
                                                                          int expectedShardCount) {
	state Span span("NAPI:WaitStorageMetrics"_loc, generateSpanID(cx->transactionTracingEnabled));
	loop {
		std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations =
		    wait(getKeyRangeLocations(cx,
		                              keys,
		                              shardLimit,
		                              Reverse::False,
		                              &StorageServerInterface::waitMetrics,
		                              TransactionInfo(TaskPriority::DataDistribution, span.context)));
		if (expectedShardCount >= 0 && locations.size() != expectedShardCount) {
			return std::make_pair(Optional<StorageMetrics>(), locations.size());
		}

		// SOMEDAY: Right now, if there are too many shards we delay and check again later. There may be a better
		// solution to this.
		if (locations.size() < shardLimit) {
			try {
				Future<StorageMetrics> fx;
				if (locations.size() > 1) {
					fx = waitStorageMetricsMultipleLocations(locations, min, max, permittedError);
				} else {
					WaitMetricsRequest req(keys, min, max);
					fx = loadBalance(locations[0].second->locations(),
					                 &StorageServerInterface::waitMetrics,
					                 req,
					                 TaskPriority::DataDistribution);
				}
				StorageMetrics x = wait(fx);
				return std::make_pair(x, -1);
			} catch (Error& e) {
				if (e.code() != error_code_wrong_shard_server && e.code() != error_code_all_alternatives_failed) {
					TraceEvent(SevError, "WaitStorageMetricsError").error(e);
					throw;
				}
				cx->invalidateCache(keys);
				wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, TaskPriority::DataDistribution));
			}
		} else {
			TraceEvent(SevWarn, "WaitStorageMetricsPenalty")
			    .detail("Keys", keys)
			    .detail("Limit", CLIENT_KNOBS->STORAGE_METRICS_SHARD_LIMIT)
			    .detail("JitteredSecondsOfPenitence", CLIENT_KNOBS->STORAGE_METRICS_TOO_MANY_SHARDS_DELAY);
			wait(delayJittered(CLIENT_KNOBS->STORAGE_METRICS_TOO_MANY_SHARDS_DELAY, TaskPriority::DataDistribution));
			// make sure that the next getKeyRangeLocations() call will actually re-fetch the range
			cx->invalidateCache(keys);
		}
	}
}

Future<std::pair<Optional<StorageMetrics>, int>> Transaction::waitStorageMetrics(KeyRange const& keys,
                                                                                 StorageMetrics const& min,
                                                                                 StorageMetrics const& max,
                                                                                 StorageMetrics const& permittedError,
                                                                                 int shardLimit,
                                                                                 int expectedShardCount) {
	return ::waitStorageMetrics(cx, keys, min, max, permittedError, shardLimit, expectedShardCount);
}

Future<StorageMetrics> Transaction::getStorageMetrics(KeyRange const& keys, int shardLimit) {
	if (shardLimit > 0) {
		StorageMetrics m;
		m.bytes = -1;
		return extractMetrics(::waitStorageMetrics(cx, keys, StorageMetrics(), m, StorageMetrics(), shardLimit, -1));
	} else {
		return ::getStorageMetricsLargeKeyRange(cx, keys);
	}
}

ACTOR Future<Standalone<VectorRef<DDMetricsRef>>> waitDataDistributionMetricsList(Database cx,
                                                                                  KeyRange keys,
                                                                                  int shardLimit) {
	loop {
		choose {
			when(wait(cx->onProxiesChanged())) {}
			when(ErrorOr<GetDDMetricsReply> rep =
			         wait(errorOr(basicLoadBalance(cx->getCommitProxies(false),
			                                       &CommitProxyInterface::getDDMetrics,
			                                       GetDDMetricsRequest(keys, shardLimit))))) {
				if (rep.isError()) {
					throw rep.getError();
				}
				return rep.get().storageMetricsList;
			}
		}
	}
}

Future<Standalone<VectorRef<ReadHotRangeWithMetrics>>> Transaction::getReadHotRanges(KeyRange const& keys) {
	return ::getReadHotRanges(cx, keys);
}

ACTOR Future<Standalone<VectorRef<KeyRef>>> getRangeSplitPoints(Database cx, KeyRange keys, int64_t chunkSize) {
	state Span span("NAPI:GetRangeSplitPoints"_loc);
	loop {
		state std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations =
		    wait(getKeyRangeLocations(cx,
		                              keys,
		                              CLIENT_KNOBS->TOO_MANY,
		                              Reverse::False,
		                              &StorageServerInterface::getRangeSplitPoints,
		                              TransactionInfo(TaskPriority::DataDistribution, span.context)));
		try {
			state int nLocs = locations.size();
			state std::vector<Future<SplitRangeReply>> fReplies(nLocs);
			KeyRef partBegin, partEnd;
			for (int i = 0; i < nLocs; i++) {
				partBegin = (i == 0) ? keys.begin : locations[i].first.begin;
				partEnd = (i == nLocs - 1) ? keys.end : locations[i].first.end;
				SplitRangeRequest req(KeyRangeRef(partBegin, partEnd), chunkSize);
				fReplies[i] = loadBalance(locations[i].second->locations(),
				                          &StorageServerInterface::getRangeSplitPoints,
				                          req,
				                          TaskPriority::DataDistribution);
			}

			wait(waitForAll(fReplies));
			Standalone<VectorRef<KeyRef>> results;

			results.push_back_deep(results.arena(), keys.begin);
			for (int i = 0; i < nLocs; i++) {
				if (i > 0) {
					results.push_back_deep(results.arena(), locations[i].first.begin); // Need this shard boundary
				}
				if (fReplies[i].get().splitPoints.size() > 0) {
					results.append(
					    results.arena(), fReplies[i].get().splitPoints.begin(), fReplies[i].get().splitPoints.size());
					results.arena().dependsOn(fReplies[i].get().splitPoints.arena());
				}
			}
			if (results.back() != keys.end) {
				results.push_back_deep(results.arena(), keys.end);
			}

			return results;
		} catch (Error& e) {
			if (e.code() != error_code_wrong_shard_server && e.code() != error_code_all_alternatives_failed) {
				TraceEvent(SevError, "GetRangeSplitPoints").error(e);
				throw;
			}
			cx->invalidateCache(keys);
			wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, TaskPriority::DataDistribution));
		}
	}
}

Future<Standalone<VectorRef<KeyRef>>> Transaction::getRangeSplitPoints(KeyRange const& keys, int64_t chunkSize) {
	return ::getRangeSplitPoints(cx, keys, chunkSize);
}

ACTOR Future<Standalone<VectorRef<KeyRef>>> splitStorageMetrics(Database cx,
                                                                KeyRange keys,
                                                                StorageMetrics limit,
                                                                StorageMetrics estimated) {
	state Span span("NAPI:SplitStorageMetrics"_loc);
	loop {
		state std::vector<std::pair<KeyRange, Reference<LocationInfo>>> locations =
		    wait(getKeyRangeLocations(cx,
		                              keys,
		                              CLIENT_KNOBS->STORAGE_METRICS_SHARD_LIMIT,
		                              Reverse::False,
		                              &StorageServerInterface::splitMetrics,
		                              TransactionInfo(TaskPriority::DataDistribution, span.context)));
		state StorageMetrics used;
		state Standalone<VectorRef<KeyRef>> results;

		// SOMEDAY: Right now, if there are too many shards we delay and check again later. There may be a better
		// solution to this.
		if (locations.size() == CLIENT_KNOBS->STORAGE_METRICS_SHARD_LIMIT) {
			wait(delay(CLIENT_KNOBS->STORAGE_METRICS_TOO_MANY_SHARDS_DELAY, TaskPriority::DataDistribution));
			cx->invalidateCache(keys);
		} else {
			results.push_back_deep(results.arena(), keys.begin);
			try {
				//TraceEvent("SplitStorageMetrics").detail("Locations", locations.size());

				state int i = 0;
				for (; i < locations.size(); i++) {
					SplitMetricsRequest req(locations[i].first, limit, used, estimated, i == locations.size() - 1);
					SplitMetricsReply res = wait(loadBalance(locations[i].second->locations(),
					                                         &StorageServerInterface::splitMetrics,
					                                         req,
					                                         TaskPriority::DataDistribution));
					if (res.splits.size() &&
					    res.splits[0] <= results.back()) { // split points are out of order, possibly because of moving
						                                   // data, throw error to retry
						ASSERT_WE_THINK(
						    false); // FIXME: This seems impossible and doesn't seem to be covered by testing
						throw all_alternatives_failed();
					}
					if (res.splits.size()) {
						results.append(results.arena(), res.splits.begin(), res.splits.size());
						results.arena().dependsOn(res.splits.arena());
					}
					used = res.used;

					//TraceEvent("SplitStorageMetricsResult").detail("Used", used.bytes).detail("Location", i).detail("Size", res.splits.size());
				}

				if (used.allLessOrEqual(limit * CLIENT_KNOBS->STORAGE_METRICS_UNFAIR_SPLIT_LIMIT)) {
					results.resize(results.arena(), results.size() - 1);
				}

				results.push_back_deep(results.arena(), keys.end);
				return results;
			} catch (Error& e) {
				if (e.code() != error_code_wrong_shard_server && e.code() != error_code_all_alternatives_failed) {
					TraceEvent(SevError, "SplitStorageMetricsError").error(e);
					throw;
				}
				cx->invalidateCache(keys);
				wait(delay(CLIENT_KNOBS->WRONG_SHARD_SERVER_DELAY, TaskPriority::DataDistribution));
			}
		}
	}
}

Future<Standalone<VectorRef<KeyRef>>> Transaction::splitStorageMetrics(KeyRange const& keys,
                                                                       StorageMetrics const& limit,
                                                                       StorageMetrics const& estimated) {
	return ::splitStorageMetrics(cx, keys, limit, estimated);
}

void Transaction::checkDeferredError() const {
	cx->checkDeferredError();
}

Reference<TransactionLogInfo> Transaction::createTrLogInfoProbabilistically(const Database& cx) {
	if (!cx->isError()) {
		double clientSamplingProbability = GlobalConfig::globalConfig().get<double>(
		    fdbClientInfoTxnSampleRate, CLIENT_KNOBS->CSI_SAMPLING_PROBABILITY);
		if (((networkOptions.logClientInfo.present() && networkOptions.logClientInfo.get()) || BUGGIFY) &&
		    deterministicRandom()->random01() < clientSamplingProbability &&
		    (!g_network->isSimulated() || !g_simulator.speedUpSimulation)) {
			return makeReference<TransactionLogInfo>(TransactionLogInfo::DATABASE);
		}
	}

	return Reference<TransactionLogInfo>();
}

void Transaction::setTransactionID(uint64_t id) {
	ASSERT(getSize() == 0);
	info.spanID = SpanID(id, info.spanID.second());
}

void Transaction::setToken(uint64_t token) {
	ASSERT(getSize() == 0);
	info.spanID = SpanID(info.spanID.first(), token);
}

void enableClientInfoLogging() {
	ASSERT(networkOptions.logClientInfo.present() == false);
	networkOptions.logClientInfo = true;
	TraceEvent(SevInfo, "ClientInfoLoggingEnabled").log();
}

ACTOR Future<Void> snapCreate(Database cx, Standalone<StringRef> snapCmd, UID snapUID) {
	TraceEvent("SnapCreateEnter").detail("SnapCmd", snapCmd).detail("UID", snapUID);
	try {
		loop {
			choose {
				when(wait(cx->onProxiesChanged())) {}
				when(wait(basicLoadBalance(cx->getCommitProxies(false),
				                           &CommitProxyInterface::proxySnapReq,
				                           ProxySnapRequest(snapCmd, snapUID, snapUID),
				                           cx->taskID,
				                           AtMostOnce::True))) {
					TraceEvent("SnapCreateExit").detail("SnapCmd", snapCmd).detail("UID", snapUID);
					return Void();
				}
			}
		}
	} catch (Error& e) {
		TraceEvent("SnapCreateError").detail("SnapCmd", snapCmd.toString()).detail("UID", snapUID).error(e);
		throw;
	}
}

ACTOR Future<bool> checkSafeExclusions(Database cx, std::vector<AddressExclusion> exclusions) {
	TraceEvent("ExclusionSafetyCheckBegin")
	    .detail("NumExclusion", exclusions.size())
	    .detail("Exclusions", describe(exclusions));
	state ExclusionSafetyCheckRequest req(exclusions);
	state bool ddCheck;
	try {
		loop {
			choose {
				when(wait(cx->onProxiesChanged())) {}
				when(ExclusionSafetyCheckReply _ddCheck =
				         wait(basicLoadBalance(cx->getCommitProxies(false),
				                               &CommitProxyInterface::exclusionSafetyCheckReq,
				                               req,
				                               cx->taskID))) {
					ddCheck = _ddCheck.safe;
					break;
				}
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_actor_cancelled) {
			TraceEvent("ExclusionSafetyCheckError")
			    .detail("NumExclusion", exclusions.size())
			    .detail("Exclusions", describe(exclusions))
			    .error(e);
		}
		throw;
	}
	TraceEvent("ExclusionSafetyCheckCoordinators").log();
	state ClientCoordinators coordinatorList(cx->getConnectionFile());
	state std::vector<Future<Optional<LeaderInfo>>> leaderServers;
	leaderServers.reserve(coordinatorList.clientLeaderServers.size());
	for (int i = 0; i < coordinatorList.clientLeaderServers.size(); i++) {
		leaderServers.push_back(retryBrokenPromise(coordinatorList.clientLeaderServers[i].getLeader,
		                                           GetLeaderRequest(coordinatorList.clusterKey, UID()),
		                                           TaskPriority::CoordinationReply));
	}
	// Wait for quorum so we don't dismiss live coordinators as unreachable by acting too fast
	choose {
		when(wait(smartQuorum(leaderServers, leaderServers.size() / 2 + 1, 1.0))) {}
		when(wait(delay(3.0))) {
			TraceEvent("ExclusionSafetyCheckNoCoordinatorQuorum").log();
			return false;
		}
	}
	int attemptCoordinatorExclude = 0;
	int coordinatorsUnavailable = 0;
	for (int i = 0; i < leaderServers.size(); i++) {
		NetworkAddress leaderAddress =
		    coordinatorList.clientLeaderServers[i].getLeader.getEndpoint().getPrimaryAddress();
		if (leaderServers[i].isReady()) {
			if ((std::count(
			         exclusions.begin(), exclusions.end(), AddressExclusion(leaderAddress.ip, leaderAddress.port)) ||
			     std::count(exclusions.begin(), exclusions.end(), AddressExclusion(leaderAddress.ip)))) {
				attemptCoordinatorExclude++;
			}
		} else {
			coordinatorsUnavailable++;
		}
	}
	int faultTolerance = (leaderServers.size() - 1) / 2 - coordinatorsUnavailable;
	bool coordinatorCheck = (attemptCoordinatorExclude <= faultTolerance);
	TraceEvent("ExclusionSafetyCheckFinish")
	    .detail("CoordinatorListSize", leaderServers.size())
	    .detail("NumExclusions", exclusions.size())
	    .detail("FaultTolerance", faultTolerance)
	    .detail("AttemptCoordinatorExclude", attemptCoordinatorExclude)
	    .detail("CoordinatorCheck", coordinatorCheck)
	    .detail("DataDistributorCheck", ddCheck);

	return (ddCheck && coordinatorCheck);
}

ACTOR Future<Void> addInterfaceActor(std::map<Key, std::pair<Value, ClientLeaderRegInterface>>* address_interface,
                                     Reference<FlowLock> connectLock,
                                     KeyValue kv) {
	wait(connectLock->take());
	state FlowLock::Releaser releaser(*connectLock);
	state ClientWorkerInterface workerInterf =
	    BinaryReader::fromStringRef<ClientWorkerInterface>(kv.value, IncludeVersion());
	state ClientLeaderRegInterface leaderInterf(workerInterf.address());
	choose {
		when(Optional<LeaderInfo> rep =
		         wait(brokenPromiseToNever(leaderInterf.getLeader.getReply(GetLeaderRequest())))) {
			StringRef ip_port =
			    kv.key.endsWith(LiteralStringRef(":tls")) ? kv.key.removeSuffix(LiteralStringRef(":tls")) : kv.key;
			(*address_interface)[ip_port] = std::make_pair(kv.value, leaderInterf);

			if (workerInterf.reboot.getEndpoint().addresses.secondaryAddress.present()) {
				Key full_ip_port2 =
				    StringRef(workerInterf.reboot.getEndpoint().addresses.secondaryAddress.get().toString());
				StringRef ip_port2 = full_ip_port2.endsWith(LiteralStringRef(":tls"))
				                         ? full_ip_port2.removeSuffix(LiteralStringRef(":tls"))
				                         : full_ip_port2;
				(*address_interface)[ip_port2] = std::make_pair(kv.value, leaderInterf);
			}
		}
		when(wait(delay(CLIENT_KNOBS->CLI_CONNECT_TIMEOUT))) {} // NOTE : change timeout time here if necessary
	}
	return Void();
}

ACTOR static Future<int64_t> rebootWorkerActor(DatabaseContext* cx, ValueRef addr, bool check, int duration) {
	// ignore negative value
	if (duration < 0)
		duration = 0;
	// fetch the addresses of all workers
	state std::map<Key, std::pair<Value, ClientLeaderRegInterface>> address_interface;
	if (!cx->getConnectionFile())
		return 0;
	RangeResult kvs = wait(getWorkerInterfaces(cx->getConnectionFile()));
	ASSERT(!kvs.more);
	// Note: reuse this knob from fdbcli, change it if necessary
	Reference<FlowLock> connectLock(new FlowLock(CLIENT_KNOBS->CLI_CONNECT_PARALLELISM));
	std::vector<Future<Void>> addInterfs;
	for (const auto& it : kvs) {
		addInterfs.push_back(addInterfaceActor(&address_interface, connectLock, it));
	}
	wait(waitForAll(addInterfs));
	if (!address_interface.count(addr))
		return 0;

	BinaryReader::fromStringRef<ClientWorkerInterface>(address_interface[addr].first, IncludeVersion())
	    .reboot.send(RebootRequest(false, check, duration));
	return 1;
}

Future<int64_t> DatabaseContext::rebootWorker(StringRef addr, bool check, int duration) {
	return rebootWorkerActor(this, addr, check, duration);
}

Future<Void> DatabaseContext::forceRecoveryWithDataLoss(StringRef dcId) {
	return forceRecovery(getConnectionFile(), dcId);
}

ACTOR static Future<Void> createSnapshotActor(DatabaseContext* cx, UID snapUID, StringRef snapCmd) {
	wait(mgmtSnapCreate(cx->clone(), snapCmd, snapUID));
	return Void();
}

Future<Void> DatabaseContext::createSnapshot(StringRef uid, StringRef snapshot_command) {
	std::string uid_str = uid.toString();
	if (!std::all_of(uid_str.begin(), uid_str.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
	    uid_str.size() != 32) {
		// only 32-length hex string is considered as a valid UID
		throw snap_invalid_uid_string();
	}
	return createSnapshotActor(this, UID::fromString(uid_str), snapshot_command);
}

ACTOR Future<Void> setPerpetualStorageWiggle(Database cx, bool enable, LockAware lockAware) {
	state ReadYourWritesTransaction tr(cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			if (lockAware) {
				tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			}

			tr.set(perpetualStorageWiggleKey, enable ? "1"_sr : "0"_sr);
			wait(tr.commit());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	return Void();
}

Reference<DatabaseContext::TransactionT> DatabaseContext::createTransaction() {
	return makeReference<ReadYourWritesTransaction>(Database(Reference<DatabaseContext>::addRef(this)));
}
