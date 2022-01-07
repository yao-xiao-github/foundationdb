/*
 * storageserver.actor.cpp
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

#include <cinttypes>
#include <functional>
#include <type_traits>
#include <unordered_map>

#include "fdbrpc/fdbrpc.h"
#include "fdbrpc/LoadBalance.h"
#include "flow/ActorCollection.h"
#include "flow/Arena.h"
#include "flow/Hash3.h"
#include "flow/Histogram.h"
#include "flow/IRandom.h"
#include "flow/IndexedSet.h"
#include "flow/SystemMonitor.h"
#include "flow/Tracing.h"
#include "flow/Util.h"
#include "fdbclient/Atomic.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/CommitProxyInterface.h"
#include "fdbclient/KeyBackedTypes.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/Notified.h"
#include "fdbclient/StatusClient.h"
#include "fdbclient/Tuple.h"
#include "fdbclient/SystemData.h"
#include "fdbclient/TransactionLineage.h"
#include "fdbclient/VersionedMap.h"
#include "fdbserver/FDBExecHelper.actor.h"
#include "fdbserver/IKeyValueStore.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/LatencyBandConfig.h"
#include "fdbserver/LogProtocolMessage.h"
#include "fdbserver/SpanContextMessage.h"
#include "fdbserver/LogSystem.h"
#include "fdbserver/MoveKeys.actor.h"
#include "fdbserver/MutationTracking.h"
#include "fdbserver/RecoveryState.h"
#include "fdbserver/StorageMetrics.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/TLogInterface.h"
#include "fdbserver/WaitFailure.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbrpc/sim_validation.h"
#include "fdbrpc/Smoother.h"
#include "fdbrpc/Stats.h"
#include "flow/TDMetric.actor.h"
#include "flow/genericactors.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

#ifndef __INTEL_COMPILER
#pragma region Data Structures
#endif

#define SHORT_CIRCUT_ACTUAL_STORAGE 0

namespace {
bool canReplyWith(Error e) {
	switch (e.code()) {
	case error_code_transaction_too_old:
	case error_code_future_version:
	case error_code_wrong_shard_server:
	case error_code_process_behind:
	case error_code_watch_cancelled:
	case error_code_unknown_change_feed:
	case error_code_server_overloaded:
	// getRangeAndMap related exceptions that are not retriable:
	case error_code_mapper_bad_index:
	case error_code_mapper_no_such_key:
	case error_code_mapper_bad_range_decriptor:
	case error_code_quick_get_key_values_has_more:
	case error_code_quick_get_value_miss:
	case error_code_quick_get_key_values_miss:
	case error_code_get_key_values_and_map_has_more:
		// case error_code_all_alternatives_failed:
		return true;
	default:
		return false;
	};
}
} // namespace

struct AddingShard : NonCopyable {
	KeyRange keys;
	Future<Void> fetchClient; // holds FetchKeys() actor
	Promise<Void> fetchComplete;
	Promise<Void> readWrite;
	PromiseStream<Key> changeFeedRemovals;

	// During the Fetching phase, it saves newer mutations whose version is greater or equal to fetchClient's
	// fetchVersion, while the shard is still busy catching up with fetchClient. It applies these updates after fetching
	// completes.
	std::deque<Standalone<VerUpdateRef>> updates;

	struct StorageServer* server;
	Version transferredVersion;

	// To learn more details of the phase transitions, see function fetchKeys(). The phases below are sorted in
	// chronological order and do not go back.
	enum Phase {
		WaitPrevious,
		// During Fetching phase, it fetches data before fetchVersion and write it to storage, then let updater know it
		// is ready to update the deferred updates` (see the comment of member variable `updates` above).
		Fetching,
		// During Waiting phase, it sends updater the deferred updates, and wait until they are durable.
		Waiting
		// The shard's state is changed from adding to readWrite then.
	};

	Phase phase;

	AddingShard(StorageServer* server, KeyRangeRef const& keys);

	// When fetchKeys "partially completes" (splits an adding shard in two), this is used to construct the left half
	AddingShard(AddingShard* prev, KeyRange const& keys)
	  : keys(keys), fetchClient(prev->fetchClient), server(prev->server), transferredVersion(prev->transferredVersion),
	    phase(prev->phase) {}
	~AddingShard() {
		if (!fetchComplete.isSet())
			fetchComplete.send(Void());
		if (!readWrite.isSet())
			readWrite.send(Void());
	}

	void addMutation(Version version, bool fromFetch, MutationRef const& mutation);

	bool isTransferred() const { return phase == Waiting; }
};

class ShardInfo : public ReferenceCounted<ShardInfo>, NonCopyable {
	ShardInfo(KeyRange keys, std::unique_ptr<AddingShard>&& adding, StorageServer* readWrite)
	  : adding(std::move(adding)), readWrite(readWrite), keys(keys) {}

public:
	// A shard has 3 mutual exclusive states: adding, readWrite and notAssigned.
	std::unique_ptr<AddingShard> adding;
	struct StorageServer* readWrite;
	KeyRange keys;
	uint64_t changeCounter;

	static ShardInfo* newNotAssigned(KeyRange keys) { return new ShardInfo(keys, nullptr, nullptr); }
	static ShardInfo* newReadWrite(KeyRange keys, StorageServer* data) { return new ShardInfo(keys, nullptr, data); }
	static ShardInfo* newAdding(StorageServer* data, KeyRange keys) {
		return new ShardInfo(keys, std::make_unique<AddingShard>(data, keys), nullptr);
	}
	static ShardInfo* addingSplitLeft(KeyRange keys, AddingShard* oldShard) {
		return new ShardInfo(keys, std::make_unique<AddingShard>(oldShard, keys), nullptr);
	}

	bool isReadable() const { return readWrite != nullptr; }
	bool notAssigned() const { return !readWrite && !adding; }
	bool assigned() const { return readWrite || adding; }
	bool isInVersionedData() const { return readWrite || (adding && adding->isTransferred()); }
	void addMutation(Version version, bool fromFetch, MutationRef const& mutation);
	bool isFetched() const { return readWrite || (adding && adding->fetchComplete.isSet()); }

	const char* debugDescribeState() const {
		if (notAssigned())
			return "NotAssigned";
		else if (adding && !adding->isTransferred())
			return "AddingFetching";
		else if (adding)
			return "AddingTransferred";
		else
			return "ReadWrite";
	}
};

struct StorageServerDisk {
	explicit StorageServerDisk(struct StorageServer* data, IKeyValueStore* storage) : data(data), storage(storage) {}

	void makeNewStorageServerDurable();
	bool makeVersionMutationsDurable(Version& prevStorageVersion, Version newStorageVersion, int64_t& bytesLeft);
	void makeVersionDurable(Version version);
	void makeTssQuarantineDurable();
	Future<bool> restoreDurableState();

	void changeLogProtocol(Version version, ProtocolVersion protocol);

	void writeMutation(MutationRef mutation);
	void writeKeyValue(KeyValueRef kv);
	void clearRange(KeyRangeRef keys);

	Future<Void> getError() { return storage->getError(); }
	Future<Void> init() { return storage->init(); }
	Future<Void> commit() { return storage->commit(); }

	// SOMEDAY: Put readNextKeyInclusive in IKeyValueStore
	Future<Key> readNextKeyInclusive(KeyRef key, IKeyValueStore::ReadType type = IKeyValueStore::ReadType::NORMAL) {
		return readFirstKey(storage, KeyRangeRef(key, allKeys.end), type);
	}
	Future<Optional<Value>> readValue(KeyRef key,
	                                  IKeyValueStore::ReadType type = IKeyValueStore::ReadType::NORMAL,
	                                  Optional<UID> debugID = Optional<UID>()) {
		return storage->readValue(key, type, debugID);
	}
	Future<Optional<Value>> readValuePrefix(KeyRef key,
	                                        int maxLength,
	                                        IKeyValueStore::ReadType type = IKeyValueStore::ReadType::NORMAL,
	                                        Optional<UID> debugID = Optional<UID>()) {
		return storage->readValuePrefix(key, maxLength, type, debugID);
	}
	Future<RangeResult> readRange(KeyRangeRef keys,
	                              int rowLimit = 1 << 30,
	                              int byteLimit = 1 << 30,
	                              IKeyValueStore::ReadType type = IKeyValueStore::ReadType::NORMAL) {
		return storage->readRange(keys, rowLimit, byteLimit, type);
	}

	KeyValueStoreType getKeyValueStoreType() const { return storage->getType(); }
	StorageBytes getStorageBytes() const { return storage->getStorageBytes(); }
	std::tuple<size_t, size_t, size_t> getSize() const { return storage->getSize(); }

private:
	struct StorageServer* data;
	IKeyValueStore* storage;

	void writeMutations(const VectorRef<MutationRef>& mutations, Version debugVersion, const char* debugContext);

	ACTOR static Future<Key> readFirstKey(IKeyValueStore* storage, KeyRangeRef range, IKeyValueStore::ReadType type) {
		RangeResult r = wait(storage->readRange(range, 1, 1 << 30, type));
		if (r.size())
			return r[0].key;
		else
			return range.end;
	}
};

struct UpdateEagerReadInfo {
	std::vector<KeyRef> keyBegin;
	std::vector<Key> keyEnd; // these are for ClearRange

	std::vector<std::pair<KeyRef, int>> keys;
	std::vector<Optional<Value>> value;

	Arena arena;

	void addMutations(VectorRef<MutationRef> const& mutations) {
		for (auto& m : mutations)
			addMutation(m);
	}

	void addMutation(MutationRef const& m) {
		// SOMEDAY: Theoretically we can avoid a read if there is an earlier overlapping ClearRange
		if (m.type == MutationRef::ClearRange && !m.param2.startsWith(systemKeys.end) &&
		    SERVER_KNOBS->ENABLE_CLEAR_RANGE_EAGER_READS)
			keyBegin.push_back(m.param2);
		else if (m.type == MutationRef::CompareAndClear) {
			if (SERVER_KNOBS->ENABLE_CLEAR_RANGE_EAGER_READS)
				keyBegin.push_back(keyAfter(m.param1, arena));
			if (keys.size() > 0 && keys.back().first == m.param1) {
				// Don't issue a second read, if the last read was equal to the current key.
				// CompareAndClear is likely to be used after another atomic operation on same key.
				keys.back().second = std::max(keys.back().second, m.param2.size() + 1);
			} else {
				keys.emplace_back(m.param1, m.param2.size() + 1);
			}
		} else if ((m.type == MutationRef::AppendIfFits) || (m.type == MutationRef::ByteMin) ||
		           (m.type == MutationRef::ByteMax))
			keys.emplace_back(m.param1, CLIENT_KNOBS->VALUE_SIZE_LIMIT);
		else if (isAtomicOp((MutationRef::Type)m.type))
			keys.emplace_back(m.param1, m.param2.size());
	}

	void finishKeyBegin() {
		if (SERVER_KNOBS->ENABLE_CLEAR_RANGE_EAGER_READS) {
			std::sort(keyBegin.begin(), keyBegin.end());
			keyBegin.resize(std::unique(keyBegin.begin(), keyBegin.end()) - keyBegin.begin());
		}
		std::sort(keys.begin(), keys.end(), [](const std::pair<KeyRef, int>& lhs, const std::pair<KeyRef, int>& rhs) {
			return (lhs.first < rhs.first) || (lhs.first == rhs.first && lhs.second > rhs.second);
		});
		keys.resize(std::unique(keys.begin(),
		                        keys.end(),
		                        [](const std::pair<KeyRef, int>& lhs, const std::pair<KeyRef, int>& rhs) {
			                        return lhs.first == rhs.first;
		                        }) -
		            keys.begin());
		// value gets populated in doEagerReads
	}

	Optional<Value>& getValue(KeyRef key) {
		int i = std::lower_bound(keys.begin(),
		                         keys.end(),
		                         std::pair<KeyRef, int>(key, 0),
		                         [](const std::pair<KeyRef, int>& lhs, const std::pair<KeyRef, int>& rhs) {
			                         return lhs.first < rhs.first;
		                         }) -
		        keys.begin();
		ASSERT(i < keys.size() && keys[i].first == key);
		return value[i];
	}

	KeyRef getKeyEnd(KeyRef key) {
		int i = std::lower_bound(keyBegin.begin(), keyBegin.end(), key) - keyBegin.begin();
		ASSERT(i < keyBegin.size() && keyBegin[i] == key);
		return keyEnd[i];
	}
};

const int VERSION_OVERHEAD =
    64 + sizeof(Version) + sizeof(Standalone<VerUpdateRef>) + // mutationLog, 64b overhead for map
    2 * (64 + sizeof(Version) +
         sizeof(Reference<VersionedMap<KeyRef, ValueOrClearToRef>::PTreeT>)); // versioned map [ x2 for
                                                                              // createNewVersion(version+1) ], 64b
                                                                              // overhead for map
// For both the mutation log and the versioned map.
static int mvccStorageBytes(MutationRef const& m) {
	return VersionedMap<KeyRef, ValueOrClearToRef>::overheadPerItem * 2 +
	       (MutationRef::OVERHEAD_BYTES + m.param1.size() + m.param2.size()) * 2;
}

struct FetchInjectionInfo {
	Arena arena;
	std::vector<VerUpdateRef> changes;
};

struct ChangeFeedInfo : ReferenceCounted<ChangeFeedInfo> {
	std::deque<Standalone<MutationsAndVersionRef>> mutations;
	Version storageVersion = invalidVersion; // The version between the storage version and the durable version are
	                                         // currently being written to disk
	Version durableVersion = invalidVersion; // All versions before the durable version are durable on disk
	Version emptyVersion = 0; // The change feed does not have any mutations before emptyVersion
	KeyRange range;
	Key id;
	AsyncTrigger newMutations;
	bool stopped = false; // A stopped change feed no longer adds new mutations, but is still queriable
	bool removing = false;
};

class ServerWatchMetadata : public ReferenceCounted<ServerWatchMetadata> {
public:
	Key key;
	Optional<Value> value;
	Version version;
	Future<Version> watch_impl;
	Promise<Version> versionPromise;
	Optional<TagSet> tags;
	Optional<UID> debugID;

	ServerWatchMetadata(Key key, Optional<Value> value, Version version, Optional<TagSet> tags, Optional<UID> debugID)
	  : key(key), value(value), version(version), tags(tags), debugID(debugID) {}
};

struct StorageServer {
	typedef VersionedMap<KeyRef, ValueOrClearToRef> VersionedData;

private:
	// versionedData contains sets and clears.

	// * Nonoverlapping: No clear overlaps a set or another clear, or adjoins another clear.
	// ~ Clears are maximal: If versionedData.at(v) contains a clear [b,e) then
	//      there is a key data[e]@v, or e==allKeys.end, or a shard boundary or former boundary at e

	// * Reads are possible: When k is in a readable shard, for any v in [storageVersion, version.get()],
	//      storage[k] + versionedData.at(v)[k] = database[k] @ v    (storage[k] might be @ any version in
	//      [durableVersion, storageVersion])

	// * Transferred shards are partially readable: When k is in an adding, transferred shard, for any v in
	// [transferredVersion, version.get()],
	//      storage[k] + versionedData.at(v)[k] = database[k] @ v

	// * versionedData contains versions [storageVersion(), version.get()].  It might also contain version
	// (version.get()+1), in which changeDurableVersion may be deleting ghosts, and/or it might
	//      contain later versions if applyUpdate is on the stack.

	// * Old shards are erased: versionedData.atLatest() has entries (sets or intersecting clears) only for keys in
	// readable or adding,transferred shards.
	//   Earlier versions may have extra entries for shards that *were* readable or adding,transferred when those
	//   versions were the latest, but they eventually are forgotten.

	// * Old mutations are erased: All items in versionedData.atLatest() have insertVersion() > durableVersion(), but
	// views
	//   at older versions may contain older items which are also in storage (this is OK because of idempotency)

	VersionedData versionedData;
	std::map<Version, Standalone<VerUpdateRef>> mutationLog; // versions (durableVersion, version]
	std::unordered_map<KeyRef, Reference<ServerWatchMetadata>> watchMap; // keep track of server watches

public:
public:
	// Histograms
	struct FetchKeysHistograms {
		const Reference<Histogram> latency;
		const Reference<Histogram> bytes;
		const Reference<Histogram> bandwidth;

		FetchKeysHistograms()
		  : latency(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
		                                    FETCH_KEYS_LATENCY_HISTOGRAM,
		                                    Histogram::Unit::microseconds)),
		    bytes(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
		                                  FETCH_KEYS_BYTES_HISTOGRAM,
		                                  Histogram::Unit::bytes)),
		    bandwidth(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
		                                      FETCH_KEYS_BYTES_PER_SECOND_HISTOGRAM,
		                                      Histogram::Unit::bytes_per_second)) {}
	} fetchKeysHistograms;

	Reference<Histogram> tlogCursorReadsLatencyHistogram;
	Reference<Histogram> ssVersionLockLatencyHistogram;
	Reference<Histogram> eagerReadsLatencyHistogram;
	Reference<Histogram> fetchKeysPTreeUpdatesLatencyHistogram;
	Reference<Histogram> tLogMsgsPTreeUpdatesLatencyHistogram;
	Reference<Histogram> storageUpdatesDurableLatencyHistogram;
	Reference<Histogram> storageCommitLatencyHistogram;
	Reference<Histogram> ssDurableVersionUpdateLatencyHistogram;

	// watch map operations
	Reference<ServerWatchMetadata> getWatchMetadata(KeyRef key) const;
	KeyRef setWatchMetadata(Reference<ServerWatchMetadata> metadata);
	void deleteWatchMetadata(KeyRef key);
	void clearWatchMetadata();

	class CurrentRunningFetchKeys {
		std::unordered_map<UID, double> startTimeMap;
		std::unordered_map<UID, KeyRange> keyRangeMap;

		static const StringRef emptyString;
		static const KeyRangeRef emptyKeyRange;

	public:
		void recordStart(const UID id, const KeyRange& keyRange) {
			startTimeMap[id] = now();
			keyRangeMap[id] = keyRange;
		}

		void recordFinish(const UID id) {
			startTimeMap.erase(id);
			keyRangeMap.erase(id);
		}

		std::pair<double, KeyRange> longestTime() const {
			if (numRunning() == 0) {
				return { -1, emptyKeyRange };
			}

			const double currentTime = now();
			double longest = 0;
			UID UIDofLongest;
			for (const auto& kv : startTimeMap) {
				const double currentRunningTime = currentTime - kv.second;
				if (longest <= currentRunningTime) {
					longest = currentRunningTime;
					UIDofLongest = kv.first;
				}
			}
			if (BUGGIFY) {
				UIDofLongest = deterministicRandom()->randomUniqueID();
			}
			auto it = keyRangeMap.find(UIDofLongest);
			if (it != keyRangeMap.end()) {
				return { longest, it->second };
			}
			return { -1, emptyKeyRange };
		}

		int numRunning() const { return startTimeMap.size(); }
	} currentRunningFetchKeys;

	Tag tag;
	std::vector<std::pair<Version, Tag>> history;
	std::vector<std::pair<Version, Tag>> allHistory;
	Version poppedAllAfter;
	std::map<Version, Arena>
	    freeable; // for each version, an Arena that must be held until that version is < oldestVersion
	Arena lastArena;
	double cpuUsage;
	double diskUsage;

	std::map<Version, Standalone<VerUpdateRef>> const& getMutationLog() const { return mutationLog; }
	std::map<Version, Standalone<VerUpdateRef>>& getMutableMutationLog() { return mutationLog; }
	VersionedData const& data() const { return versionedData; }
	VersionedData& mutableData() { return versionedData; }

	double old_rate = 1.0;
	double currentRate() {
		auto versionLag = version.get() - durableVersion.get();
		double res;
		if (versionLag >= SERVER_KNOBS->STORAGE_DURABILITY_LAG_HARD_MAX) {
			res = 0.0;
		} else if (versionLag > SERVER_KNOBS->STORAGE_DURABILITY_LAG_SOFT_MAX) {
			res =
			    1.0 -
			    (double(versionLag - SERVER_KNOBS->STORAGE_DURABILITY_LAG_SOFT_MAX) /
			     double(SERVER_KNOBS->STORAGE_DURABILITY_LAG_HARD_MAX - SERVER_KNOBS->STORAGE_DURABILITY_LAG_SOFT_MAX));
		} else {
			res = 1.0;
		}
		if (res != old_rate) {
			TraceEvent(SevDebug, "LocalRatekeeperChange", thisServerID)
			    .detail("Old", old_rate)
			    .detail("New", res)
			    .detail("NonDurableVersions", versionLag);
			old_rate = res;
		}
		return res;
	}

	void addMutationToMutationLogOrStorage(
	    Version ver,
	    MutationRef m); // Appends m to mutationLog@ver, or to storage if ver==invalidVersion

	// Update the byteSample, and write the updates to the mutation log@ver, or to storage if ver==invalidVersion
	void byteSampleApplyMutation(MutationRef const& m, Version ver);
	void byteSampleApplySet(KeyValueRef kv, Version ver);
	void byteSampleApplyClear(KeyRangeRef range, Version ver);

	void popVersion(Version v, bool popAllTags = false) {
		if (logSystem && !isTss()) {
			if (v > poppedAllAfter) {
				popAllTags = true;
				poppedAllAfter = std::numeric_limits<Version>::max();
			}

			std::vector<std::pair<Version, Tag>>* hist = &history;
			std::vector<std::pair<Version, Tag>> allHistoryCopy;
			if (popAllTags) {
				allHistoryCopy = allHistory;
				hist = &allHistoryCopy;
			}

			while (hist->size() && v > hist->back().first) {
				logSystem->pop(v, hist->back().second);
				hist->pop_back();
			}
			if (hist->size()) {
				logSystem->pop(v, hist->back().second);
			} else {
				logSystem->pop(v, tag);
			}
		}
	}

	Standalone<VerUpdateRef>& addVersionToMutationLog(Version v) {
		// return existing version...
		auto m = mutationLog.find(v);
		if (m != mutationLog.end())
			return m->second;

		// ...or create a new one
		auto& u = mutationLog[v];
		u.version = v;
		if (lastArena.getSize() >= 65536)
			lastArena = Arena(4096);
		u.arena() = lastArena;
		counters.bytesInput += VERSION_OVERHEAD;
		return u;
	}

	MutationRef addMutationToMutationLog(Standalone<VerUpdateRef>& mLV, MutationRef const& m) {
		byteSampleApplyMutation(m, mLV.version);
		counters.bytesInput += mvccStorageBytes(m);
		return mLV.push_back_deep(mLV.arena(), m);
	}

	void setTssPair(UID pairId) {
		tssPairID = Optional<UID>(pairId);

		// Set up tss fault injection here, only if we are in simulated mode and with fault injection.
		// With fault injection enabled, the tss will start acting normal for a bit, then after the specified delay
		// start behaving incorrectly.
		if (g_network->isSimulated() && !g_simulator.speedUpSimulation &&
		    g_simulator.tssMode >= ISimulator::TSSMode::EnabledAddDelay) {
			tssFaultInjectTime = now() + deterministicRandom()->randomInt(60, 300);
			TraceEvent(SevWarnAlways, "TSSInjectFaultEnabled", thisServerID)
			    .detail("Mode", g_simulator.tssMode)
			    .detail("At", tssFaultInjectTime.get());
		}
	}

	// If a TSS is "in quarantine", it means it has incorrect data. It is effectively in a "zombie" state where it
	// rejects all read requests and ignores all non-private mutations and data movements, but otherwise is still part
	// of the cluster. The purpose of this state is to "freeze" the TSS state after a mismatch so a human operator can
	// investigate, but preventing a new storage process from replacing the TSS on the worker. It will still get removed
	// from the cluster if it falls behind on the mutation stream, or if its tss pair gets removed and its tag is no
	// longer valid.
	bool isTSSInQuarantine() { return tssPairID.present() && tssInQuarantine; }

	void startTssQuarantine() {
		if (!tssInQuarantine) {
			// persist quarantine so it's still quarantined if rebooted
			storage.makeTssQuarantineDurable();
		}
		tssInQuarantine = true;
	}

	StorageServerDisk storage;

	KeyRangeMap<Reference<ShardInfo>> shards;
	uint64_t shardChangeCounter; // max( shards->changecounter )

	KeyRangeMap<bool> cachedRangeMap; // indicates if a key-range is being cached

	KeyRangeMap<std::vector<Reference<ChangeFeedInfo>>> keyChangeFeed;
	std::map<Key, Reference<ChangeFeedInfo>> uidChangeFeed;
	Deque<std::pair<std::vector<Key>, Version>> changeFeedVersions;
	std::map<UID, PromiseStream<Key>> changeFeedRemovals;
	std::set<Key> currentChangeFeeds;
	std::unordered_map<NetworkAddress, std::map<UID, Version>> changeFeedClientVersions;

	// newestAvailableVersion[k]
	//   == invalidVersion -> k is unavailable at all versions
	//   <= storageVersion -> k is unavailable at all versions (but might be read anyway from storage if we are in the
	//   process of committing makeShardDurable)
	//   == v              -> k is readable (from storage+versionedData) @ [storageVersion,v], and not being updated
	//   when version increases
	//   == latestVersion  -> k is readable (from storage+versionedData) @ [storageVersion,version.get()], and thus
	//   stays available when version increases
	CoalescedKeyRangeMap<Version> newestAvailableVersion;

	CoalescedKeyRangeMap<Version> newestDirtyVersion; // Similar to newestAvailableVersion, but includes (only) keys
	                                                  // that were only partly available (due to cancelled fetchKeys)

	// The following are in rough order from newest to oldest
	Version lastTLogVersion, lastVersionWithData, restoredVersion, prevVersion;
	NotifiedVersion version;
	NotifiedVersion desiredOldestVersion; // We can increase oldestVersion (and then durableVersion) to this version
	                                      // when the disk permits
	NotifiedVersion oldestVersion; // See also storageVersion()
	NotifiedVersion durableVersion; // At least this version will be readable from storage after a power failure
	Version rebootAfterDurableVersion;
	int8_t primaryLocality;
	Version knownCommittedVersion;

	Deque<std::pair<Version, Version>> recoveryVersionSkips;
	int64_t versionLag; // An estimate for how many versions it takes for the data to move from the logs to this storage
	                    // server

	Optional<UID> sourceTLogID; // the tLog from which the latest batch of versions were fetched

	ProtocolVersion logProtocol;

	Reference<ILogSystem> logSystem;
	Reference<ILogSystem::IPeekCursor> logCursor;

	Promise<UID> clusterId;
	UID thisServerID;
	Optional<UID> tssPairID; // if this server is a tss, this is the id of its (ss) pair
	Optional<UID> ssPairID; // if this server is an ss, this is the id of its (tss) pair
	Optional<double> tssFaultInjectTime;
	bool tssInQuarantine;

	Key sk;
	Reference<AsyncVar<ServerDBInfo> const> db;
	Database cx;
	ActorCollection actors;

	StorageServerMetrics metrics;
	CoalescedKeyRangeMap<bool, int64_t, KeyBytesMetric<int64_t>> byteSampleClears;
	AsyncVar<bool> byteSampleClearsTooLarge;
	Future<Void> byteSampleRecovery;
	Future<Void> durableInProgress;

	AsyncMap<Key, bool> watches;
	int64_t watchBytes;
	int64_t numWatches;
	AsyncVar<bool> noRecentUpdates;
	double lastUpdate;

	Int64MetricHandle readQueueSizeMetric;

	std::string folder;

	// defined only during splitMutations()/addMutation()
	UpdateEagerReadInfo* updateEagerReads;

	FlowLock durableVersionLock;
	FlowLock fetchKeysParallelismLock;
	int64_t fetchKeysBytesBudget;
	AsyncVar<bool> fetchKeysBudgetUsed;
	std::vector<Promise<FetchInjectionInfo*>> readyFetchKeys;

	int64_t instanceID;

	Promise<Void> otherError;
	Promise<Void> coreStarted;
	bool shuttingDown;

	bool behind;
	bool versionBehind;

	bool debug_inApplyUpdate;
	double debug_lastValidateTime;

	int64_t lastBytesInputEBrake;
	Version lastDurableVersionEBrake;

	int maxQueryQueue;
	int getAndResetMaxQueryQueueSize() {
		int val = maxQueryQueue;
		maxQueryQueue = 0;
		return val;
	}

	struct TransactionTagCounter {
		struct TagInfo {
			TransactionTag tag;
			double rate;
			double fractionalBusyness;

			TagInfo(TransactionTag const& tag, double rate, double fractionalBusyness)
			  : tag(tag), rate(rate), fractionalBusyness(fractionalBusyness) {}
		};

		TransactionTagMap<int64_t> intervalCounts;
		int64_t intervalTotalSampledCount = 0;
		TransactionTag busiestTag;
		int64_t busiestTagCount = 0;
		double intervalStart = 0;

		Optional<TagInfo> previousBusiestTag;

		UID thisServerID;

		Reference<EventCacheHolder> busiestReadTagEventHolder;

		TransactionTagCounter(UID thisServerID)
		  : thisServerID(thisServerID),
		    busiestReadTagEventHolder(makeReference<EventCacheHolder>(thisServerID.toString() + "/BusiestReadTag")) {}

		int64_t costFunction(int64_t bytes) { return bytes / SERVER_KNOBS->READ_COST_BYTE_FACTOR + 1; }

		void addRequest(Optional<TagSet> const& tags, int64_t bytes) {
			if (tags.present()) {
				TEST(true); // Tracking tag on storage server
				double cost = costFunction(bytes);
				for (auto& tag : tags.get()) {
					int64_t& count = intervalCounts[TransactionTag(tag, tags.get().getArena())];
					count += cost;
					if (count > busiestTagCount) {
						busiestTagCount = count;
						busiestTag = tag;
					}
				}

				intervalTotalSampledCount += cost;
			}
		}

		void startNewInterval() {
			double elapsed = now() - intervalStart;
			previousBusiestTag.reset();
			if (intervalStart > 0 && CLIENT_KNOBS->READ_TAG_SAMPLE_RATE > 0 && elapsed > 0) {
				double rate = busiestTagCount / CLIENT_KNOBS->READ_TAG_SAMPLE_RATE / elapsed;
				if (rate > SERVER_KNOBS->MIN_TAG_READ_PAGES_RATE) {
					previousBusiestTag = TagInfo(busiestTag, rate, (double)busiestTagCount / intervalTotalSampledCount);
				}

				TraceEvent("BusiestReadTag", thisServerID)
				    .detail("Elapsed", elapsed)
				    .detail("Tag", printable(busiestTag))
				    .detail("TagCost", busiestTagCount)
				    .detail("TotalSampledCost", intervalTotalSampledCount)
				    .detail("Reported", previousBusiestTag.present())
				    .trackLatest(busiestReadTagEventHolder->trackingKey);
			}

			intervalCounts.clear();
			intervalTotalSampledCount = 0;
			busiestTagCount = 0;
			intervalStart = now();
		}

		Optional<TagInfo> getBusiestTag() const { return previousBusiestTag; }
	};

	TransactionTagCounter transactionTagCounter;

	Optional<LatencyBandConfig> latencyBandConfig;

	struct Counters {
		CounterCollection cc;
		Counter allQueries, getKeyQueries, getValueQueries, getRangeQueries, getRangeAndFlatMapQueries,
		    getRangeStreamQueries, finishedQueries, lowPriorityQueries, rowsQueried, bytesQueried, watchQueries,
		    emptyQueries;

		// Bytes of the mutations that have been added to the memory of the storage server. When the data is durable
		// and cleared from the memory, we do not subtract it but add it to bytesDurable.
		Counter bytesInput;
		// Bytes of the mutations that have been removed from memory because they durable. The counting is same as
		// bytesInput, instead of the actual bytes taken in the storages, so that (bytesInput - bytesDurable) can
		// reflect the current memory footprint of MVCC.
		Counter bytesDurable;
		// Bytes fetched by fetchKeys() for data movements. The size is counted as a collection of KeyValueRef.
		Counter bytesFetched;
		// Like bytesInput but without MVCC accounting. The size is counted as how much it takes when serialized. It
		// is basically the size of both parameters of the mutation and a 12 bytes overhead that keeps mutation type
		// and the lengths of both parameters.
		Counter mutationBytes;

		Counter sampledBytesCleared;
		// The number of key-value pairs fetched by fetchKeys()
		Counter kvFetched;
		Counter mutations, setMutations, clearRangeMutations, atomicMutations;
		Counter updateBatches, updateVersions;
		Counter loops;
		Counter fetchWaitingMS, fetchWaitingCount, fetchExecutingMS, fetchExecutingCount;
		Counter readsRejected;
		Counter wrongShardServer;
		Counter fetchedVersions;
		Counter fetchesFromLogs;
		// The following counters measure how many of lookups in the getRangeAndFlatMapQueries are effective. "Miss"
		// means fallback if fallback is enabled, otherwise means failure (so that another layer could implement
		// fallback).
		Counter quickGetValueHit, quickGetValueMiss, quickGetKeyValuesHit, quickGetKeyValuesMiss;

		LatencySample readLatencySample;
		LatencyBands readLatencyBands;

		Counters(StorageServer* self)
		  : cc("StorageServer", self->thisServerID.toString()), allQueries("QueryQueue", cc),
		    getKeyQueries("GetKeyQueries", cc), getValueQueries("GetValueQueries", cc),
		    getRangeQueries("GetRangeQueries", cc), getRangeAndFlatMapQueries("GetRangeAndFlatMapQueries", cc),
		    getRangeStreamQueries("GetRangeStreamQueries", cc), finishedQueries("FinishedQueries", cc),
		    lowPriorityQueries("LowPriorityQueries", cc), rowsQueried("RowsQueried", cc),
		    bytesQueried("BytesQueried", cc), watchQueries("WatchQueries", cc), emptyQueries("EmptyQueries", cc),
		    bytesInput("BytesInput", cc), bytesDurable("BytesDurable", cc), bytesFetched("BytesFetched", cc),
		    mutationBytes("MutationBytes", cc), sampledBytesCleared("SampledBytesCleared", cc),
		    kvFetched("KVFetched", cc), mutations("Mutations", cc), setMutations("SetMutations", cc),
		    clearRangeMutations("ClearRangeMutations", cc), atomicMutations("AtomicMutations", cc),
		    updateBatches("UpdateBatches", cc), updateVersions("UpdateVersions", cc), loops("Loops", cc),
		    fetchWaitingMS("FetchWaitingMS", cc), fetchWaitingCount("FetchWaitingCount", cc),
		    fetchExecutingMS("FetchExecutingMS", cc), fetchExecutingCount("FetchExecutingCount", cc),
		    readsRejected("ReadsRejected", cc), wrongShardServer("WrongShardServer", cc),
		    fetchedVersions("FetchedVersions", cc), fetchesFromLogs("FetchesFromLogs", cc),
		    quickGetValueHit("QuickGetValueHit", cc), quickGetValueMiss("QuickGetValueMiss", cc),
		    quickGetKeyValuesHit("QuickGetKeyValuesHit", cc), quickGetKeyValuesMiss("QuickGetKeyValuesMiss", cc),
		    readLatencySample("ReadLatencyMetrics",
		                      self->thisServerID,
		                      SERVER_KNOBS->LATENCY_METRICS_LOGGING_INTERVAL,
		                      SERVER_KNOBS->LATENCY_SAMPLE_SIZE),
		    readLatencyBands("ReadLatencyBands", self->thisServerID, SERVER_KNOBS->STORAGE_LOGGING_DELAY) {
			specialCounter(cc, "LastTLogVersion", [self]() { return self->lastTLogVersion; });
			specialCounter(cc, "Version", [self]() { return self->version.get(); });
			specialCounter(cc, "StorageVersion", [self]() { return self->storageVersion(); });
			specialCounter(cc, "DurableVersion", [self]() { return self->durableVersion.get(); });
			specialCounter(cc, "DesiredOldestVersion", [self]() { return self->desiredOldestVersion.get(); });
			specialCounter(cc, "VersionLag", [self]() { return self->versionLag; });
			specialCounter(cc, "LocalRate", [self] { return int64_t(self->currentRate() * 100); });

			specialCounter(cc, "BytesReadSampleCount", [self]() { return self->metrics.bytesReadSample.queue.size(); });
			specialCounter(
			    cc, "FetchKeysFetchActive", [self]() { return self->fetchKeysParallelismLock.activePermits(); });
			specialCounter(cc, "FetchKeysWaiting", [self]() { return self->fetchKeysParallelismLock.waiters(); });
			specialCounter(cc, "QueryQueueMax", [self]() { return self->getAndResetMaxQueryQueueSize(); });
			specialCounter(cc, "BytesStored", [self]() { return self->metrics.byteSample.getEstimate(allKeys); });
			specialCounter(cc, "ActiveWatches", [self]() { return self->numWatches; });
			specialCounter(cc, "WatchBytes", [self]() { return self->watchBytes; });
			specialCounter(cc, "KvstoreSizeTotal", [self]() { return std::get<0>(self->storage.getSize()); });
			specialCounter(cc, "KvstoreNodeTotal", [self]() { return std::get<1>(self->storage.getSize()); });
			specialCounter(cc, "KvstoreInlineKey", [self]() { return std::get<2>(self->storage.getSize()); });
		}
	} counters;

	Reference<EventCacheHolder> storageServerSourceTLogIDEventHolder;

	StorageServer(IKeyValueStore* storage,
	              Reference<AsyncVar<ServerDBInfo> const> const& db,
	              StorageServerInterface const& ssi)
	  : tlogCursorReadsLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                            TLOG_CURSOR_READS_LATENCY_HISTOGRAM,
	                                                            Histogram::Unit::microseconds)),
	    ssVersionLockLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                          SS_VERSION_LOCK_LATENCY_HISTOGRAM,
	                                                          Histogram::Unit::microseconds)),
	    eagerReadsLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                       EAGER_READS_LATENCY_HISTOGRAM,
	                                                       Histogram::Unit::microseconds)),
	    fetchKeysPTreeUpdatesLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                                  FETCH_KEYS_PTREE_UPDATES_LATENCY_HISTOGRAM,
	                                                                  Histogram::Unit::microseconds)),
	    tLogMsgsPTreeUpdatesLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                                 TLOG_MSGS_PTREE_UPDATES_LATENCY_HISTOGRAM,
	                                                                 Histogram::Unit::microseconds)),
	    storageUpdatesDurableLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                                  STORAGE_UPDATES_DURABLE_LATENCY_HISTOGRAM,
	                                                                  Histogram::Unit::microseconds)),
	    storageCommitLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                          STORAGE_COMMIT_LATENCY_HISTOGRAM,
	                                                          Histogram::Unit::microseconds)),
	    ssDurableVersionUpdateLatencyHistogram(Histogram::getHistogram(STORAGESERVER_HISTOGRAM_GROUP,
	                                                                   SS_DURABLE_VERSION_UPDATE_LATENCY_HISTOGRAM,
	                                                                   Histogram::Unit::microseconds)),
	    tag(invalidTag), poppedAllAfter(std::numeric_limits<Version>::max()), cpuUsage(0.0), diskUsage(0.0),
	    storage(this, storage), shardChangeCounter(0), lastTLogVersion(0), lastVersionWithData(0), restoredVersion(0),
	    prevVersion(0), rebootAfterDurableVersion(std::numeric_limits<Version>::max()),
	    primaryLocality(tagLocalityInvalid), knownCommittedVersion(0), versionLag(0), logProtocol(0),
	    thisServerID(ssi.id()), tssInQuarantine(false), db(db), actors(false),
	    byteSampleClears(false, LiteralStringRef("\xff\xff\xff")), durableInProgress(Void()), watchBytes(0),
	    numWatches(0), noRecentUpdates(false), lastUpdate(now()),
	    readQueueSizeMetric(LiteralStringRef("StorageServer.ReadQueueSize")), updateEagerReads(nullptr),
	    fetchKeysParallelismLock(SERVER_KNOBS->FETCH_KEYS_PARALLELISM),
	    fetchKeysBytesBudget(SERVER_KNOBS->STORAGE_FETCH_BYTES), fetchKeysBudgetUsed(false),
	    instanceID(deterministicRandom()->randomUniqueID().first()), shuttingDown(false), behind(false),
	    versionBehind(false), debug_inApplyUpdate(false), debug_lastValidateTime(0), lastBytesInputEBrake(0),
	    lastDurableVersionEBrake(0), maxQueryQueue(0), transactionTagCounter(ssi.id()), counters(this),
	    storageServerSourceTLogIDEventHolder(
	        makeReference<EventCacheHolder>(ssi.id().toString() + "/StorageServerSourceTLogID")) {
		version.initMetric(LiteralStringRef("StorageServer.Version"), counters.cc.id);
		oldestVersion.initMetric(LiteralStringRef("StorageServer.OldestVersion"), counters.cc.id);
		durableVersion.initMetric(LiteralStringRef("StorageServer.DurableVersion"), counters.cc.id);
		desiredOldestVersion.initMetric(LiteralStringRef("StorageServer.DesiredOldestVersion"), counters.cc.id);

		newestAvailableVersion.insert(allKeys, invalidVersion);
		newestDirtyVersion.insert(allKeys, invalidVersion);
		addShard(ShardInfo::newNotAssigned(allKeys));

		cx = openDBOnServer(db, TaskPriority::DefaultEndpoint, LockAware::True);
	}

	//~StorageServer() { fclose(log); }

	// Puts the given shard into shards.  The caller is responsible for adding shards
	//   for all ranges in shards.getAffectedRangesAfterInsertion(newShard->keys)), because these
	//   shards are invalidated by the call.
	void addShard(ShardInfo* newShard) {
		ASSERT(!newShard->keys.empty());
		newShard->changeCounter = ++shardChangeCounter;
		//TraceEvent("AddShard", this->thisServerID).detail("KeyBegin", newShard->keys.begin).detail("KeyEnd", newShard->keys.end).detail("State", newShard->isReadable() ? "Readable" : newShard->notAssigned() ? "NotAssigned" : "Adding").detail("Version", this->version.get());
		/*auto affected = shards.getAffectedRangesAfterInsertion( newShard->keys, Reference<ShardInfo>() );
		for(auto i = affected.begin(); i != affected.end(); ++i)
		    shards.insert( *i, Reference<ShardInfo>() );*/
		shards.insert(newShard->keys, Reference<ShardInfo>(newShard));
	}
	void addMutation(Version version,
	                 bool fromFetch,
	                 MutationRef const& mutation,
	                 KeyRangeRef const& shard,
	                 UpdateEagerReadInfo* eagerReads);
	void setInitialVersion(Version ver) {
		version = ver;
		desiredOldestVersion = ver;
		oldestVersion = ver;
		durableVersion = ver;
		lastVersionWithData = ver;
		restoredVersion = ver;

		mutableData().createNewVersion(ver);
		mutableData().forgetVersionsBefore(ver);
	}

	bool isTss() const { return tssPairID.present(); }

	bool isSSWithTSSPair() const { return ssPairID.present(); }

	void setSSWithTssPair(UID idOfTSS) { ssPairID = Optional<UID>(idOfTSS); }

	void clearSSWithTssPair() { ssPairID = Optional<UID>(); }

	// This is the maximum version that might be read from storage (the minimum version is durableVersion)
	Version storageVersion() const { return oldestVersion.get(); }

	bool isReadable(KeyRangeRef const& keys) {
		auto sh = shards.intersectingRanges(keys);
		for (auto i = sh.begin(); i != sh.end(); ++i)
			if (!i->value()->isReadable())
				return false;
		return true;
	}

	void checkChangeCounter(uint64_t oldShardChangeCounter, KeyRef const& key) {
		if (oldShardChangeCounter != shardChangeCounter && shards[key]->changeCounter > oldShardChangeCounter) {
			TEST(true); // shard change during getValueQ
			throw wrong_shard_server();
		}
	}

	void checkChangeCounter(uint64_t oldShardChangeCounter, KeyRangeRef const& keys) {
		if (oldShardChangeCounter != shardChangeCounter) {
			auto sh = shards.intersectingRanges(keys);
			for (auto i = sh.begin(); i != sh.end(); ++i)
				if (i->value()->changeCounter > oldShardChangeCounter) {
					TEST(true); // shard change during range operation
					throw wrong_shard_server();
				}
		}
	}

	Counter::Value queueSize() { return counters.bytesInput.getValue() - counters.bytesDurable.getValue(); }

	// penalty used by loadBalance() to balance requests among SSes. We prefer SS with less write queue size.
	double getPenalty() {
		return std::max(std::max(1.0,
		                         (queueSize() - (SERVER_KNOBS->TARGET_BYTES_PER_STORAGE_SERVER -
		                                         2.0 * SERVER_KNOBS->SPRING_BYTES_STORAGE_SERVER)) /
		                             SERVER_KNOBS->SPRING_BYTES_STORAGE_SERVER),
		                (currentRate() < 1e-6 ? 1e6 : 1.0 / currentRate()));
	}

	// Normally the storage server prefers to serve read requests over making mutations
	// durable to disk. However, when the storage server falls to far behind on
	// making mutations durable, this function will change the priority to prefer writes.
	Future<Void> getQueryDelay() {
		if ((version.get() - durableVersion.get() > SERVER_KNOBS->LOW_PRIORITY_DURABILITY_LAG) ||
		    (queueSize() > SERVER_KNOBS->LOW_PRIORITY_STORAGE_QUEUE_BYTES)) {
			++counters.lowPriorityQueries;
			return delay(0, TaskPriority::LowPriorityRead);
		}
		return delay(0, TaskPriority::DefaultEndpoint);
	}

	template <class Reply>
	using isLoadBalancedReply = std::is_base_of<LoadBalancedReply, Reply>;

	template <class Reply>
	typename std::enable_if<isLoadBalancedReply<Reply>::value, void>::type
	sendErrorWithPenalty(const ReplyPromise<Reply>& promise, const Error& err, double penalty) {
		if (err.code() == error_code_wrong_shard_server) {
			++counters.wrongShardServer;
		}
		Reply reply;
		reply.error = err;
		reply.penalty = penalty;
		promise.send(reply);
	}

	template <class Reply>
	typename std::enable_if<!isLoadBalancedReply<Reply>::value, void>::type
	sendErrorWithPenalty(const ReplyPromise<Reply>& promise, const Error& err, double) {
		if (err.code() == error_code_wrong_shard_server) {
			++counters.wrongShardServer;
		}
		promise.sendError(err);
	}

	template <class Request>
	bool shouldRead(const Request& request) {
		auto rate = currentRate();
		if (isTSSInQuarantine() || (rate < SERVER_KNOBS->STORAGE_DURABILITY_LAG_REJECT_THRESHOLD &&
		                            deterministicRandom()->random01() >
		                                std::max(SERVER_KNOBS->STORAGE_DURABILITY_LAG_MIN_RATE,
		                                         rate / SERVER_KNOBS->STORAGE_DURABILITY_LAG_REJECT_THRESHOLD))) {
			sendErrorWithPenalty(request.reply, server_overloaded(), getPenalty());
			++counters.readsRejected;
			return false;
		}
		return true;
	}

	template <class Request, class HandleFunction>
	Future<Void> readGuard(const Request& request, const HandleFunction& fun) {
		bool read = shouldRead(request);
		if (!read) {
			return Void();
		}
		return fun(this, request);
	}
};

const StringRef StorageServer::CurrentRunningFetchKeys::emptyString = LiteralStringRef("");
const KeyRangeRef StorageServer::CurrentRunningFetchKeys::emptyKeyRange =
    KeyRangeRef(StorageServer::CurrentRunningFetchKeys::emptyString,
                StorageServer::CurrentRunningFetchKeys::emptyString);

// If and only if key:=value is in (storage+versionedData),    // NOT ACTUALLY: and key < allKeys.end,
//   and H(key) < |key+value|/bytesPerSample,
//     let sampledSize = max(|key+value|,bytesPerSample)
//     persistByteSampleKeys.begin()+key := sampledSize is in storage
//     (key,sampledSize) is in byteSample

// So P(key is sampled) * sampledSize == |key+value|

void StorageServer::byteSampleApplyMutation(MutationRef const& m, Version ver) {
	if (m.type == MutationRef::ClearRange)
		byteSampleApplyClear(KeyRangeRef(m.param1, m.param2), ver);
	else if (m.type == MutationRef::SetValue)
		byteSampleApplySet(KeyValueRef(m.param1, m.param2), ver);
	else
		ASSERT(false); // Mutation of unknown type modfying byte sample
}

// watchMap Operations
Reference<ServerWatchMetadata> StorageServer::getWatchMetadata(KeyRef key) const {
	const auto it = watchMap.find(key);
	if (it == watchMap.end())
		return Reference<ServerWatchMetadata>();
	return it->second;
}

KeyRef StorageServer::setWatchMetadata(Reference<ServerWatchMetadata> metadata) {
	KeyRef keyRef = metadata->key.contents();

	watchMap[keyRef] = metadata;
	return keyRef;
}

void StorageServer::deleteWatchMetadata(KeyRef key) {
	watchMap.erase(key);
}

void StorageServer::clearWatchMetadata() {
	watchMap.clear();
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

/////////////////////////////////// Validation ///////////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region Validation
#endif
bool validateRange(StorageServer::VersionedData::ViewAtVersion const& view,
                   KeyRangeRef range,
                   Version version,
                   UID id,
                   Version minInsertVersion) {
	// * Nonoverlapping: No clear overlaps a set or another clear, or adjoins another clear.
	// * Old mutations are erased: All items in versionedData.atLatest() have insertVersion() > durableVersion()

	//TraceEvent("ValidateRange", id).detail("KeyBegin", range.begin).detail("KeyEnd", range.end).detail("Version", version);
	KeyRef k;
	bool ok = true;
	bool kIsClear = false;
	auto i = view.lower_bound(range.begin);
	if (i != view.begin())
		--i;
	for (; i != view.end() && i.key() < range.end; ++i) {
		ASSERT(i.insertVersion() > minInsertVersion);
		if (kIsClear && i->isClearTo() ? i.key() <= k : i.key() < k) {
			TraceEvent(SevError, "InvalidRange", id)
			    .detail("Key1", k)
			    .detail("Key2", i.key())
			    .detail("Version", version);
			ok = false;
		}
		// ASSERT( i.key() >= k );
		kIsClear = i->isClearTo();
		k = kIsClear ? i->getEndKey() : i.key();
	}
	return ok;
}

void validate(StorageServer* data, bool force = false) {
	try {
		if (force || (EXPENSIVE_VALIDATION)) {
			data->newestAvailableVersion.validateCoalesced();
			data->newestDirtyVersion.validateCoalesced();

			for (auto s = data->shards.ranges().begin(); s != data->shards.ranges().end(); ++s) {
				ASSERT(s->value()->keys == s->range());
				ASSERT(!s->value()->keys.empty());
			}

			for (auto s = data->shards.ranges().begin(); s != data->shards.ranges().end(); ++s)
				if (s->value()->isReadable()) {
					auto ar = data->newestAvailableVersion.intersectingRanges(s->range());
					for (auto a = ar.begin(); a != ar.end(); ++a)
						ASSERT(a->value() == latestVersion);
				}

			// * versionedData contains versions [storageVersion(), version.get()].  It might also contain version
			// (version.get()+1), in which changeDurableVersion may be deleting ghosts, and/or it might
			//      contain later versions if applyUpdate is on the stack.
			ASSERT(data->data().getOldestVersion() == data->storageVersion());
			ASSERT(data->data().getLatestVersion() == data->version.get() ||
			       data->data().getLatestVersion() == data->version.get() + 1 ||
			       (data->debug_inApplyUpdate && data->data().getLatestVersion() > data->version.get()));

			auto latest = data->data().atLatest();

			// * Old shards are erased: versionedData.atLatest() has entries (sets or clear *begins*) only for keys in
			// readable or adding,transferred shards.
			for (auto s = data->shards.ranges().begin(); s != data->shards.ranges().end(); ++s) {
				ShardInfo* shard = s->value().getPtr();
				if (!shard->isInVersionedData()) {
					if (latest.lower_bound(s->begin()) != latest.lower_bound(s->end())) {
						TraceEvent(SevError, "VF", data->thisServerID)
						    .detail("LastValidTime", data->debug_lastValidateTime)
						    .detail("KeyBegin", s->begin())
						    .detail("KeyEnd", s->end())
						    .detail("FirstKey", latest.lower_bound(s->begin()).key())
						    .detail("FirstInsertV", latest.lower_bound(s->begin()).insertVersion());
					}
					ASSERT(latest.lower_bound(s->begin()) == latest.lower_bound(s->end()));
				}
			}

			latest.validate();
			validateRange(latest, allKeys, data->version.get(), data->thisServerID, data->durableVersion.get());

			data->debug_lastValidateTime = now();
		}
	} catch (...) {
		TraceEvent(SevError, "ValidationFailure", data->thisServerID)
		    .detail("LastValidTime", data->debug_lastValidateTime);
		throw;
	}
}
#ifndef __INTEL_COMPILER
#pragma endregion
#endif

void updateProcessStats(StorageServer* self) {
	if (g_network->isSimulated()) {
		// diskUsage and cpuUsage are not relevant in the simulator,
		// and relying on the actual values could break seed determinism
		self->cpuUsage = 100.0;
		self->diskUsage = 100.0;
		return;
	}

	SystemStatistics sysStats = getSystemStatistics();
	if (sysStats.initialized) {
		self->cpuUsage = 100 * sysStats.processCPUSeconds / sysStats.elapsed;
		self->diskUsage = 100 * std::max(0.0, (sysStats.elapsed - sysStats.processDiskIdleSeconds) / sysStats.elapsed);
	}
}

///////////////////////////////////// Queries /////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region Queries
#endif

ACTOR Future<Version> waitForVersionActor(StorageServer* data, Version version, SpanID spanContext) {
	state Span span("SS.WaitForVersion"_loc, { spanContext });
	choose {
		when(wait(data->version.whenAtLeast(version))) {
			// FIXME: A bunch of these can block with or without the following delay 0.
			// wait( delay(0) );  // don't do a whole bunch of these at once
			if (version < data->oldestVersion.get())
				throw transaction_too_old(); // just in case
			return version;
		}
		when(wait(delay(SERVER_KNOBS->FUTURE_VERSION_DELAY))) {
			if (deterministicRandom()->random01() < 0.001)
				TraceEvent(SevWarn, "ShardServerFutureVersion1000x", data->thisServerID)
				    .detail("Version", version)
				    .detail("MyVersion", data->version.get())
				    .detail("ServerID", data->thisServerID);
			throw future_version();
		}
	}
}

Future<Version> waitForVersion(StorageServer* data, Version version, SpanID spanContext) {
	if (version == latestVersion) {
		version = std::max(Version(1), data->version.get());
	}

	if (version < data->oldestVersion.get() || version <= 0) {
		return transaction_too_old();
	} else if (version <= data->version.get()) {
		return version;
	}

	if ((data->behind || data->versionBehind) && version > data->version.get()) {
		return process_behind();
	}

	if (deterministicRandom()->random01() < 0.001) {
		TraceEvent("WaitForVersion1000x").log();
	}
	return waitForVersionActor(data, version, spanContext);
}

ACTOR Future<Version> waitForVersionNoTooOld(StorageServer* data, Version version) {
	// This could become an Actor transparently, but for now it just does the lookup
	if (version == latestVersion)
		version = std::max(Version(1), data->version.get());
	if (version <= data->version.get())
		return version;
	choose {
		when(wait(data->version.whenAtLeast(version))) { return version; }
		when(wait(delay(SERVER_KNOBS->FUTURE_VERSION_DELAY))) {
			if (deterministicRandom()->random01() < 0.001)
				TraceEvent(SevWarn, "ShardServerFutureVersion1000x", data->thisServerID)
				    .detail("Version", version)
				    .detail("MyVersion", data->version.get())
				    .detail("ServerID", data->thisServerID);
			throw future_version();
		}
	}
}

ACTOR Future<Void> getValueQ(StorageServer* data, GetValueRequest req) {
	state int64_t resultSize = 0;
	Span span("SS:getValue"_loc, { req.spanContext });
	span.addTag("key"_sr, req.key);
	// Temporarily disabled -- this path is hit a lot
	// getCurrentLineage()->modify(&TransactionLineage::txID) = req.spanContext.first();

	try {
		++data->counters.getValueQueries;
		++data->counters.allQueries;
		++data->readQueueSizeMetric;
		data->maxQueryQueue = std::max<int>(
		    data->maxQueryQueue, data->counters.allQueries.getValue() - data->counters.finishedQueries.getValue());

		// Active load balancing runs at a very high priority (to obtain accurate queue lengths)
		// so we need to downgrade here
		wait(data->getQueryDelay());

		if (req.debugID.present())
			g_traceBatch.addEvent("GetValueDebug",
			                      req.debugID.get().first(),
			                      "getValueQ.DoRead"); //.detail("TaskID", g_network->getCurrentTask());

		state Optional<Value> v;
		state Version version = wait(waitForVersion(data, req.version, req.spanContext));
		if (req.debugID.present())
			g_traceBatch.addEvent("GetValueDebug",
			                      req.debugID.get().first(),
			                      "getValueQ.AfterVersion"); //.detail("TaskID", g_network->getCurrentTask());

		state uint64_t changeCounter = data->shardChangeCounter;

		if (!data->shards[req.key]->isReadable()) {
			//TraceEvent("WrongShardServer", data->thisServerID).detail("Key", req.key).detail("Version", version).detail("In", "getValueQ");
			throw wrong_shard_server();
		}

		state int path = 0;
		auto i = data->data().at(version).lastLessOrEqual(req.key);
		if (i && i->isValue() && i.key() == req.key) {
			v = (Value)i->getValue();
			path = 1;
		} else if (!i || !i->isClearTo() || i->getEndKey() <= req.key) {
			path = 2;
			Optional<Value> vv = wait(data->storage.readValue(req.key, IKeyValueStore::ReadType::NORMAL, req.debugID));
			// Validate that while we were reading the data we didn't lose the version or shard
			if (version < data->storageVersion()) {
				TEST(true); // transaction_too_old after readValue
				throw transaction_too_old();
			}
			data->checkChangeCounter(changeCounter, req.key);
			v = vv;
		}

		DEBUG_MUTATION("ShardGetValue",
		               version,
		               MutationRef(MutationRef::DebugKey, req.key, v.present() ? v.get() : LiteralStringRef("<null>")),
		               data->thisServerID);
		DEBUG_MUTATION("ShardGetPath",
		               version,
		               MutationRef(MutationRef::DebugKey,
		                           req.key,
		                           path == 0   ? LiteralStringRef("0")
		                           : path == 1 ? LiteralStringRef("1")
		                                       : LiteralStringRef("2")),
		               data->thisServerID);

		/*
		StorageMetrics m;
		m.bytesPerKSecond = req.key.size() + (v.present() ? v.get().size() : 0);
		m.iosPerKSecond = 1;
		data->metrics.notify(req.key, m);
		*/

		if (v.present()) {
			++data->counters.rowsQueried;
			resultSize = v.get().size();
			data->counters.bytesQueried += resultSize;
		} else {
			++data->counters.emptyQueries;
		}

		if (SERVER_KNOBS->READ_SAMPLING_ENABLED) {
			// If the read yields no value, randomly sample the empty read.
			int64_t bytesReadPerKSecond =
			    v.present() ? std::max((int64_t)(req.key.size() + v.get().size()), SERVER_KNOBS->EMPTY_READ_PENALTY)
			                : SERVER_KNOBS->EMPTY_READ_PENALTY;
			data->metrics.notifyBytesReadPerKSecond(req.key, bytesReadPerKSecond);
		}

		if (req.debugID.present())
			g_traceBatch.addEvent("GetValueDebug",
			                      req.debugID.get().first(),
			                      "getValueQ.AfterRead"); //.detail("TaskID", g_network->getCurrentTask());

		// Check if the desired key might be cached
		auto cached = data->cachedRangeMap[req.key];
		// if (cached)
		//	TraceEvent(SevDebug, "SSGetValueCached").detail("Key", req.key);

		GetValueReply reply(v, cached);
		reply.penalty = data->getPenalty();
		req.reply.send(reply);
	} catch (Error& e) {
		if (!canReplyWith(e))
			throw;
		data->sendErrorWithPenalty(req.reply, e, data->getPenalty());
	}

	data->transactionTagCounter.addRequest(req.tags, resultSize);

	++data->counters.finishedQueries;
	--data->readQueueSizeMetric;

	double duration = g_network->timer() - req.requestTime();
	data->counters.readLatencySample.addMeasurement(duration);
	if (data->latencyBandConfig.present()) {
		int maxReadBytes =
		    data->latencyBandConfig.get().readConfig.maxReadBytes.orDefault(std::numeric_limits<int>::max());
		data->counters.readLatencyBands.addMeasurement(duration, resultSize > maxReadBytes);
	}

	return Void();
};

// Pessimistic estimate the number of overhead bytes used by each
// watch. Watch key references are stored in an AsyncMap<Key,bool>, and actors
// must be kept alive until the watch is finished.
extern size_t WATCH_OVERHEAD_WATCHQ, WATCH_OVERHEAD_WATCHIMPL;

ACTOR Future<Version> watchWaitForValueChange(StorageServer* data, SpanID parent, KeyRef key) {
	state Location spanLocation = "SS:watchWaitForValueChange"_loc;
	state Span span(spanLocation, { parent });
	state Reference<ServerWatchMetadata> metadata = data->getWatchMetadata(key);

	if (metadata->debugID.present())
		g_traceBatch.addEvent("WatchValueDebug",
		                      metadata->debugID.get().first(),
		                      "watchValueSendReply.Before"); //.detail("TaskID", g_network->getCurrentTask());

	wait(success(waitForVersionNoTooOld(data, metadata->version)));
	if (metadata->debugID.present())
		g_traceBatch.addEvent("WatchValueDebug",
		                      metadata->debugID.get().first(),
		                      "watchValueSendReply.AfterVersion"); //.detail("TaskID", g_network->getCurrentTask());

	state Version minVersion = data->data().latestVersion;
	state Future<Void> watchFuture = data->watches.onChange(metadata->key);
	loop {
		try {
			metadata = data->getWatchMetadata(key);
			state Version latest = data->version.get();
			TEST(latest >= minVersion &&
			     latest < data->data().latestVersion); // Starting watch loop with latestVersion > data->version
			GetValueRequest getReq(span.context, metadata->key, latest, metadata->tags, metadata->debugID);
			state Future<Void> getValue = getValueQ(
			    data, getReq); // we are relying on the delay zero at the top of getValueQ, if removed we need one here
			GetValueReply reply = wait(getReq.reply.getFuture());
			span = Span(spanLocation, parent);

			if (reply.error.present()) {
				ASSERT(reply.error.get().code() != error_code_future_version);
				throw reply.error.get();
			}
			if (BUGGIFY) {
				throw transaction_too_old();
			}

			DEBUG_MUTATION(
			    "ShardWatchValue",
			    latest,
			    MutationRef(MutationRef::DebugKey,
			                metadata->key,
			                reply.value.present() ? StringRef(reply.value.get()) : LiteralStringRef("<null>")),
			    data->thisServerID);

			if (metadata->debugID.present())
				g_traceBatch.addEvent(
				    "WatchValueDebug",
				    metadata->debugID.get().first(),
				    "watchValueSendReply.AfterRead"); //.detail("TaskID", g_network->getCurrentTask());

			if (reply.value != metadata->value && latest >= metadata->version) {
				return latest; // fire watch
			}

			if (data->watchBytes > SERVER_KNOBS->MAX_STORAGE_SERVER_WATCH_BYTES) {
				TEST(true); // Too many watches, reverting to polling
				throw watch_cancelled();
			}

			state int64_t watchBytes =
			    (metadata->key.expectedSize() + metadata->value.expectedSize() + key.expectedSize() +
			     sizeof(Reference<ServerWatchMetadata>) + sizeof(ServerWatchMetadata) + WATCH_OVERHEAD_WATCHIMPL);

			data->watchBytes += watchBytes;
			try {
				if (latest < minVersion) {
					// If the version we read is less than minVersion, then we may fail to be notified of any changes
					// that occur up to or including minVersion To prevent that, we'll check the key again once the
					// version reaches our minVersion
					watchFuture = watchFuture || data->version.whenAtLeast(minVersion);
				}
				if (BUGGIFY) {
					// Simulate a trigger on the watch that results in the loop going around without the value changing
					watchFuture = watchFuture || delay(deterministicRandom()->random01());
				}
				wait(watchFuture);
				data->watchBytes -= watchBytes;
			} catch (Error& e) {
				data->watchBytes -= watchBytes;
				throw;
			}
		} catch (Error& e) {
			if (e.code() != error_code_transaction_too_old) {
				throw e;
			}

			TEST(true); // Reading a watched key failed with transaction_too_old
		}

		watchFuture = data->watches.onChange(metadata->key);
		wait(data->version.whenAtLeast(data->data().latestVersion));
	}
}

void checkCancelWatchImpl(StorageServer* data, WatchValueRequest req) {
	Reference<ServerWatchMetadata> metadata = data->getWatchMetadata(req.key.contents());
	if (metadata.isValid() && metadata->versionPromise.getFutureReferenceCount() == 1) {
		// last watch timed out so cancel watch_impl and delete key from the map
		data->deleteWatchMetadata(req.key.contents());
		metadata->watch_impl.cancel();
	}
}

ACTOR Future<Void> watchValueSendReply(StorageServer* data,
                                       WatchValueRequest req,
                                       Future<Version> resp,
                                       SpanID spanContext) {
	state Span span("SS:watchValue"_loc, { spanContext });
	state double startTime = now();
	++data->counters.watchQueries;
	++data->numWatches;
	data->watchBytes += WATCH_OVERHEAD_WATCHQ;

	loop {
		double timeoutDelay = -1;
		if (data->noRecentUpdates.get()) {
			timeoutDelay = std::max(CLIENT_KNOBS->FAST_WATCH_TIMEOUT - (now() - startTime), 0.0);
		} else if (!BUGGIFY) {
			timeoutDelay = std::max(CLIENT_KNOBS->WATCH_TIMEOUT - (now() - startTime), 0.0);
		}

		try {
			choose {
				when(Version ver = wait(resp)) {
					// fire watch
					req.reply.send(WatchValueReply{ ver });
					checkCancelWatchImpl(data, req);
					--data->numWatches;
					data->watchBytes -= WATCH_OVERHEAD_WATCHQ;
					return Void();
				}
				when(wait(timeoutDelay < 0 ? Never() : delay(timeoutDelay))) {
					// watch timed out
					data->sendErrorWithPenalty(req.reply, timed_out(), data->getPenalty());
					checkCancelWatchImpl(data, req);
					--data->numWatches;
					data->watchBytes -= WATCH_OVERHEAD_WATCHQ;
					return Void();
				}
				when(wait(data->noRecentUpdates.onChange())) {}
			}
		} catch (Error& e) {
			data->watchBytes -= WATCH_OVERHEAD_WATCHQ;
			checkCancelWatchImpl(data, req);
			--data->numWatches;

			if (!canReplyWith(e))
				throw e;
			data->sendErrorWithPenalty(req.reply, e, data->getPenalty());
			return Void();
		}
	}
}

ACTOR Future<Void> changeFeedPopQ(StorageServer* self, ChangeFeedPopRequest req) {
	wait(delay(0));

	TraceEvent(SevDebug, "ChangeFeedPopQuery", self->thisServerID)
	    .detail("RangeID", req.rangeID.printable())
	    .detail("Version", req.version)
	    .detail("Range", req.range.toString());

	if (!self->isReadable(req.range)) {
		req.reply.sendError(wrong_shard_server());
		return Void();
	}
	auto feed = self->uidChangeFeed.find(req.rangeID);
	if (feed == self->uidChangeFeed.end()) {
		req.reply.sendError(unknown_change_feed());
		return Void();
	}
	if (req.version - 1 > feed->second->emptyVersion) {
		feed->second->emptyVersion = req.version - 1;
		while (!feed->second->mutations.empty() && feed->second->mutations.front().version < req.version) {
			feed->second->mutations.pop_front();
		}
		if (feed->second->storageVersion != invalidVersion) {
			self->storage.clearRange(KeyRangeRef(changeFeedDurableKey(feed->second->id, 0),
			                                     changeFeedDurableKey(feed->second->id, req.version)));
			if (req.version > feed->second->storageVersion) {
				feed->second->storageVersion = invalidVersion;
				feed->second->durableVersion = invalidVersion;
			}
			wait(self->durableVersion.whenAtLeast(self->storageVersion() + 1));
		}
	}
	req.reply.send(Void());
	return Void();
}

ACTOR Future<Void> overlappingChangeFeedsQ(StorageServer* data, OverlappingChangeFeedsRequest req) {
	wait(delay(0));
	wait(data->version.whenAtLeast(req.minVersion));

	if (!data->isReadable(req.range)) {
		req.reply.sendError(wrong_shard_server());
		return Void();
	}

	auto ranges = data->keyChangeFeed.intersectingRanges(req.range);
	std::map<Key, std::pair<KeyRange, bool>> rangeIds;
	for (auto r : ranges) {
		for (auto& it : r.value()) {
			rangeIds[it->id] = std::make_pair(it->range, it->stopped);
		}
	}
	OverlappingChangeFeedsReply reply;
	for (auto& it : rangeIds) {
		reply.rangeIds.push_back(OverlappingChangeFeedEntry(it.first, it.second.first, it.second.second));
	}
	req.reply.send(reply);
	return Void();
}

MutationsAndVersionRef filterMutationsInverted(Arena& arena, MutationsAndVersionRef const& m, KeyRange const& range) {
	Optional<VectorRef<MutationRef>> modifiedMutations;
	for (int i = 0; i < m.mutations.size(); i++) {
		if (m.mutations[i].type == MutationRef::SetValue) {
			if (modifiedMutations.present() && !range.contains(m.mutations[i].param1)) {
				modifiedMutations.get().push_back(arena, m.mutations[i]);
			}
			if (!modifiedMutations.present() && range.contains(m.mutations[i].param1)) {
				modifiedMutations = m.mutations.slice(0, i);
				arena.dependsOn(range.arena());
			}
		} else {
			ASSERT(m.mutations[i].type == MutationRef::ClearRange);
			if (!modifiedMutations.present() &&
			    ((m.mutations[i].param1 < range.begin && m.mutations[i].param2 > range.begin) ||
			     (m.mutations[i].param2 > range.end && m.mutations[i].param1 < range.end))) {
				modifiedMutations = m.mutations.slice(0, i);
				arena.dependsOn(range.arena());
			}
			if (modifiedMutations.present()) {
				if (m.mutations[i].param1 < range.begin) {
					modifiedMutations.get().push_back(arena,
					                                  MutationRef(MutationRef::ClearRange,
					                                              m.mutations[i].param1,
					                                              std::min(range.begin, m.mutations[i].param2)));
				}
				if (m.mutations[i].param2 > range.end) {
					modifiedMutations.get().push_back(arena,
					                                  MutationRef(MutationRef::ClearRange,
					                                              std::max(range.end, m.mutations[i].param1),
					                                              m.mutations[i].param2));
				}
			}
		}
	}
	if (modifiedMutations.present()) {
		return MutationsAndVersionRef(modifiedMutations.get(), m.version, m.knownCommittedVersion);
	}
	return m;
}

MutationsAndVersionRef filterMutations(Arena& arena,
                                       MutationsAndVersionRef const& m,
                                       KeyRange const& range,
                                       bool inverted) {
	if (m.mutations.size() == 1 && m.mutations.back().param1 == lastEpochEndPrivateKey) {
		return m;
	}

	if (inverted) {
		return filterMutationsInverted(arena, m, range);
	}

	Optional<VectorRef<MutationRef>> modifiedMutations;
	for (int i = 0; i < m.mutations.size(); i++) {
		if (m.mutations[i].type == MutationRef::SetValue) {
			if (modifiedMutations.present() && range.contains(m.mutations[i].param1)) {
				modifiedMutations.get().push_back(arena, m.mutations[i]);
			}
			if (!modifiedMutations.present() && !range.contains(m.mutations[i].param1)) {
				modifiedMutations = m.mutations.slice(0, i);
				arena.dependsOn(range.arena());
			}
		} else {
			ASSERT(m.mutations[i].type == MutationRef::ClearRange);
			if (!modifiedMutations.present() &&
			    (m.mutations[i].param1 < range.begin || m.mutations[i].param2 > range.end)) {
				modifiedMutations = m.mutations.slice(0, i);
				arena.dependsOn(range.arena());
			}
			if (modifiedMutations.present()) {
				if (m.mutations[i].param1 < range.end && range.begin < m.mutations[i].param2) {
					modifiedMutations.get().push_back(arena,
					                                  MutationRef(MutationRef::ClearRange,
					                                              std::max(range.begin, m.mutations[i].param1),
					                                              std::min(range.end, m.mutations[i].param2)));
				}
			}
		}
	}
	if (modifiedMutations.present()) {
		return MutationsAndVersionRef(modifiedMutations.get(), m.version, m.knownCommittedVersion);
	}
	return m;
}

ACTOR Future<std::pair<ChangeFeedStreamReply, bool>> getChangeFeedMutations(StorageServer* data,
                                                                            ChangeFeedStreamRequest req,
                                                                            bool inverted) {
	state ChangeFeedStreamReply reply;
	state ChangeFeedStreamReply memoryReply;
	state int remainingLimitBytes = CLIENT_KNOBS->REPLY_BYTE_LIMIT;
	state int remainingDurableBytes = CLIENT_KNOBS->REPLY_BYTE_LIMIT;

	if (data->version.get() < req.begin) {
		wait(data->version.whenAtLeast(req.begin));
	}
	state uint64_t changeCounter = data->shardChangeCounter;
	if (!inverted && !data->isReadable(req.range)) {
		throw wrong_shard_server();
	}

	auto feed = data->uidChangeFeed.find(req.rangeID);
	if (feed == data->uidChangeFeed.end()) {
		throw unknown_change_feed();
	}

	// We must copy the mutationDeque when fetching the durable bytes in case mutations are popped from memory while
	// waiting for the results
	state Version dequeVersion = data->version.get();
	state Version dequeKnownCommit = data->knownCommittedVersion;

	if (req.end > feed->second->emptyVersion + 1) {
		for (auto& it : feed->second->mutations) {
			if (it.version >= req.end || it.version > dequeVersion || remainingLimitBytes <= 0) {
				break;
			}
			if (it.version >= req.begin) {
				memoryReply.arena.dependsOn(it.arena());
				auto m = filterMutations(memoryReply.arena, it, req.range, inverted);
				memoryReply.mutations.push_back(memoryReply.arena, m);
				remainingLimitBytes -= sizeof(MutationsAndVersionRef) + m.expectedSize();
			}
		}
	}

	if (req.end > feed->second->emptyVersion + 1 && feed->second->durableVersion != invalidVersion &&
	    req.begin <= feed->second->durableVersion) {
		RangeResult res = wait(data->storage.readRange(
		    KeyRangeRef(changeFeedDurableKey(req.rangeID, std::max(req.begin, feed->second->emptyVersion)),
		                changeFeedDurableKey(req.rangeID, req.end)),
		    1 << 30,
		    remainingDurableBytes));

		if (!req.range.empty()) {
			data->checkChangeCounter(changeCounter, req.range);
		}

		Version lastVersion = req.begin - 1;
		for (auto& kv : res) {
			Key id;
			Version version, knownCommittedVersion;
			Standalone<VectorRef<MutationRef>> mutations;
			std::tie(id, version) = decodeChangeFeedDurableKey(kv.key);
			std::tie(mutations, knownCommittedVersion) = decodeChangeFeedDurableValue(kv.value);
			reply.arena.dependsOn(mutations.arena());
			auto m = filterMutations(
			    reply.arena, MutationsAndVersionRef(mutations, version, knownCommittedVersion), req.range, inverted);
			reply.mutations.push_back(reply.arena, m);
			remainingDurableBytes -=
			    sizeof(KeyValueRef) +
			    kv.expectedSize(); // This is tracking the size on disk rather than the reply size
			                       // because we cannot add mutations from memory if there are potentially more on disk
			lastVersion = version;
		}
		if (remainingDurableBytes > 0) {
			reply.arena.dependsOn(memoryReply.arena);
			auto it = memoryReply.mutations.begin();
			int totalCount = memoryReply.mutations.size();
			while (it != memoryReply.mutations.end() && it->version <= lastVersion) {
				++it;
				--totalCount;
			}
			reply.mutations.append(reply.arena, it, totalCount);
		}
	} else {
		reply = memoryReply;
	}

	Version finalVersion = std::min(req.end - 1, dequeVersion);
	if ((reply.mutations.empty() || reply.mutations.back().version < finalVersion) && remainingLimitBytes > 0 &&
	    remainingDurableBytes > 0) {
		reply.mutations.push_back(
		    reply.arena, MutationsAndVersionRef(finalVersion, finalVersion == dequeVersion ? dequeKnownCommit : 0));
	}

	if (MUTATION_TRACKING_ENABLED) {
		for (auto& mutations : reply.mutations) {
			for (auto& m : mutations.mutations) {
				DEBUG_MUTATION("ChangeFeedRead", mutations.version, m, data->thisServerID)
				    .detail("ChangeFeedID", req.rangeID)
				    .detail("ReqBegin", req.begin)
				    .detail("ReqEnd", req.end)
				    .detail("ReqRange", req.range);
			}
		}
	}

	return std::make_pair(reply, remainingLimitBytes > 0 && remainingDurableBytes > 0);
}

ACTOR Future<Void> localChangeFeedStream(StorageServer* data,
                                         PromiseStream<Standalone<MutationsAndVersionRef>> results,
                                         Key rangeID,
                                         Version begin,
                                         Version end,
                                         KeyRange range) {
	try {
		loop {
			state ChangeFeedStreamRequest feedRequest;
			feedRequest.rangeID = rangeID;
			feedRequest.begin = begin;
			feedRequest.end = end;
			feedRequest.range = range;
			state std::pair<ChangeFeedStreamReply, bool> feedReply =
			    wait(getChangeFeedMutations(data, feedRequest, true));
			begin = feedReply.first.mutations.back().version + 1;
			state int resultLoc = 0;
			while (resultLoc < feedReply.first.mutations.size()) {
				if (feedReply.first.mutations[resultLoc].mutations.size() ||
				    feedReply.first.mutations[resultLoc].version == end - 1) {
					wait(results.onEmpty());
					results.send(feedReply.first.mutations[resultLoc]);
				}
				resultLoc++;
			}

			if (begin == end) {
				return Void();
			}
		}
	} catch (Error& e) {
		TraceEvent(SevError, "LocalChangeFeedError", data->thisServerID).error(e);
		throw;
	}
}

ACTOR Future<Void> changeFeedStreamQ(StorageServer* data, ChangeFeedStreamRequest req) {
	state Span span("SS:getChangeFeedStream"_loc, { req.spanContext });
	state bool atLatest = false;
	state UID streamUID = deterministicRandom()->randomUniqueID();
	state bool removeUID = false;
	state Optional<Version> blockedVersion;
	req.reply.setByteLimit(SERVER_KNOBS->RANGESTREAM_LIMIT_BYTES);

	wait(delay(0, TaskPriority::DefaultEndpoint));

	try {
		loop {
			Future<Void> onReady = req.reply.onReady();
			if (atLatest && !onReady.isReady()) {
				data->changeFeedClientVersions[req.reply.getEndpoint().getPrimaryAddress()][streamUID] =
				    blockedVersion.present() ? blockedVersion.get() : data->prevVersion;
				removeUID = true;
			}
			wait(onReady);
			state Future<std::pair<ChangeFeedStreamReply, bool>> feedReplyFuture =
			    getChangeFeedMutations(data, req, false);
			if (atLatest && !removeUID && !feedReplyFuture.isReady()) {
				data->changeFeedClientVersions[req.reply.getEndpoint().getPrimaryAddress()][streamUID] =
				    blockedVersion.present() ? blockedVersion.get() : data->prevVersion;
				removeUID = true;
			}
			std::pair<ChangeFeedStreamReply, bool> _feedReply = wait(feedReplyFuture);
			ChangeFeedStreamReply feedReply = _feedReply.first;
			bool gotAll = _feedReply.second;

			req.begin = feedReply.mutations.back().version + 1;
			if (!atLatest && gotAll) {
				atLatest = true;
			}
			auto& clientVersions = data->changeFeedClientVersions[req.reply.getEndpoint().getPrimaryAddress()];
			Version minVersion = removeUID ? data->version.get() : data->prevVersion;
			if (removeUID) {
				data->changeFeedClientVersions[req.reply.getEndpoint().getPrimaryAddress()].erase(streamUID);
				removeUID = false;
			}

			for (auto& it : clientVersions) {
				minVersion = std::min(minVersion, it.second);
			}
			feedReply.atLatestVersion = atLatest;
			feedReply.minStreamVersion = gotAll ? minVersion : feedReply.mutations.back().version;
			req.reply.send(feedReply);
			if (feedReply.mutations.back().version == req.end - 1) {
				req.reply.sendError(end_of_stream());
				return Void();
			}
			if (gotAll) {
				blockedVersion = Optional<Version>();
				auto feed = data->uidChangeFeed.find(req.rangeID);
				if (feed == data->uidChangeFeed.end() || feed->second->removing) {
					req.reply.sendError(unknown_change_feed());
					return Void();
				}
				choose {
					when(wait(feed->second->newMutations.onTrigger())) {
					} // FIXME: check that this is triggered when the range is moved to a different
					  // server, also check that the stream is closed
					when(wait(req.end == std::numeric_limits<Version>::max() ? Future<Void>(Never())
					                                                         : data->version.whenAtLeast(req.end))) {}
					when(wait(delay(5.0))) {} // TODO REMOVE this once empty version logic is fully implemented
				}
				auto feed = data->uidChangeFeed.find(req.rangeID);
				if (feed == data->uidChangeFeed.end() || feed->second->removing) {
					req.reply.sendError(unknown_change_feed());
					return Void();
				}
			} else {
				blockedVersion = feedReply.mutations.back().version;
			}
		}
	} catch (Error& e) {
		auto it = data->changeFeedClientVersions.find(req.reply.getEndpoint().getPrimaryAddress());
		if (it != data->changeFeedClientVersions.end()) {
			if (removeUID) {
				it->second.erase(streamUID);
			}
			if (it->second.empty()) {
				data->changeFeedClientVersions.erase(it);
			}
		}
		if (e.code() != error_code_operation_obsolete) {
			if (!canReplyWith(e))
				throw;
			req.reply.sendError(e);
		}
	}
	return Void();
}

ACTOR Future<Void> changeFeedVersionUpdateQ(StorageServer* data, ChangeFeedVersionUpdateRequest req) {
	wait(data->version.whenAtLeast(req.minVersion));
	wait(delay(0));
	auto& clientVersions = data->changeFeedClientVersions[req.reply.getEndpoint().getPrimaryAddress()];
	Version minVersion = data->version.get();
	for (auto& it : clientVersions) {
		minVersion = std::min(minVersion, it.second);
	}
	req.reply.send(ChangeFeedVersionUpdateReply(minVersion));
	return Void();
}

#ifdef NO_INTELLISENSE
size_t WATCH_OVERHEAD_WATCHQ =
    sizeof(WatchValueSendReplyActorState<WatchValueSendReplyActor>) + sizeof(WatchValueSendReplyActor);
size_t WATCH_OVERHEAD_WATCHIMPL =
    sizeof(WatchWaitForValueChangeActorState<WatchWaitForValueChangeActor>) + sizeof(WatchWaitForValueChangeActor);
#else
size_t WATCH_OVERHEAD_WATCHQ = 0; // only used in IDE so value is irrelevant
size_t WATCH_OVERHEAD_WATCHIMPL = 0;
#endif

ACTOR Future<Void> getShardState_impl(StorageServer* data, GetShardStateRequest req) {
	ASSERT(req.mode != GetShardStateRequest::NO_WAIT);

	loop {
		std::vector<Future<Void>> onChange;

		for (auto t : data->shards.intersectingRanges(req.keys)) {
			if (!t.value()->assigned()) {
				onChange.push_back(delay(SERVER_KNOBS->SHARD_READY_DELAY));
				break;
			}

			if (req.mode == GetShardStateRequest::READABLE && !t.value()->isReadable())
				onChange.push_back(t.value()->adding->readWrite.getFuture());

			if (req.mode == GetShardStateRequest::FETCHING && !t.value()->isFetched())
				onChange.push_back(t.value()->adding->fetchComplete.getFuture());
		}

		if (!onChange.size()) {
			req.reply.send(GetShardStateReply{ data->version.get(), data->durableVersion.get() });
			return Void();
		}

		wait(waitForAll(onChange));
		wait(delay(0)); // onChange could have been triggered by cancellation, let things settle before rechecking
	}
}

ACTOR Future<Void> getShardStateQ(StorageServer* data, GetShardStateRequest req) {
	choose {
		when(wait(getShardState_impl(data, req))) {}
		when(wait(delay(g_network->isSimulated() ? 10 : 60))) {
			data->sendErrorWithPenalty(req.reply, timed_out(), data->getPenalty());
		}
	}
	return Void();
}

void merge(Arena& arena,
           VectorRef<KeyValueRef, VecSerStrategy::String>& output,
           VectorRef<KeyValueRef> const& vm_output,
           RangeResult const& base,
           int& vCount,
           int limit,
           bool stopAtEndOfBase,
           int& pos,
           int limitBytes = 1 << 30)
// Combines data from base (at an older version) with sets from newer versions in [start, end) and appends the first (up
// to) |limit| rows to output If limit<0, base and output are in descending order, and start->key()>end->key(), but
// start is still inclusive and end is exclusive
{
	ASSERT(limit != 0);
	// Add a dependency of the new arena on the result from the KVS so that we don't have to copy any of the KVS
	// results.
	arena.dependsOn(base.arena());

	bool forward = limit > 0;
	if (!forward)
		limit = -limit;
	int adjustedLimit = limit + output.size();
	int accumulatedBytes = 0;
	KeyValueRef const* baseStart = base.begin();
	KeyValueRef const* baseEnd = base.end();
	while (baseStart != baseEnd && vCount > 0 && output.size() < adjustedLimit && accumulatedBytes < limitBytes) {
		if (forward ? baseStart->key < vm_output[pos].key : baseStart->key > vm_output[pos].key) {
			output.push_back(arena, *baseStart++);
		} else {
			output.push_back_deep(arena, vm_output[pos]);
			if (baseStart->key == vm_output[pos].key)
				++baseStart;
			++pos;
			vCount--;
		}
		accumulatedBytes += sizeof(KeyValueRef) + output.end()[-1].expectedSize();
	}
	while (baseStart != baseEnd && output.size() < adjustedLimit && accumulatedBytes < limitBytes) {
		output.push_back(arena, *baseStart++);
		accumulatedBytes += sizeof(KeyValueRef) + output.end()[-1].expectedSize();
	}
	if (!stopAtEndOfBase) {
		while (vCount > 0 && output.size() < adjustedLimit && accumulatedBytes < limitBytes) {
			output.push_back_deep(arena, vm_output[pos]);
			accumulatedBytes += sizeof(KeyValueRef) + output.end()[-1].expectedSize();
			++pos;
			vCount--;
		}
	}
}

ACTOR Future<Optional<Value>> quickGetValue(StorageServer* data,
                                            StringRef key,
                                            Version version,
                                            // To provide span context, tags, debug ID to underlying lookups.
                                            GetKeyValuesAndFlatMapRequest* pOriginalReq) {
	if (data->shards[key]->isReadable()) {
		try {
			// TODO: Use a lower level API may be better? Or tweak priorities?
			GetValueRequest req(pOriginalReq->spanContext, key, version, pOriginalReq->tags, pOriginalReq->debugID);
			// Note that it does not use readGuard to avoid server being overloaded here. Throttling is enforced at the
			// original request level, rather than individual underlying lookups. The reason is that throttle any
			// individual underlying lookup will fail the original request, which is not productive.
			data->actors.add(getValueQ(data, req));
			GetValueReply reply = wait(req.reply.getFuture());
			if (!reply.error.present()) {
				++data->counters.quickGetValueHit;
				return reply.value;
			}
			// Otherwise fallback.
		} catch (Error& e) {
			// Fallback.
		}
	}
	// Otherwise fallback.

	++data->counters.quickGetValueMiss;
	if (SERVER_KNOBS->QUICK_GET_VALUE_FALLBACK) {
		state Transaction tr(data->cx);
		tr.setVersion(version);
		// TODO: is DefaultPromiseEndpoint the best priority for this?
		tr.info.taskID = TaskPriority::DefaultPromiseEndpoint;
		Future<Optional<Value>> valueFuture = tr.get(key, Snapshot::True);
		// TODO: async in case it needs to read from other servers.
		state Optional<Value> valueOption = wait(valueFuture);
		return valueOption;
	} else {
		throw quick_get_value_miss();
	}
};

// If limit>=0, it returns the first rows in the range (sorted ascending), otherwise the last rows (sorted descending).
// readRange has O(|result|) + O(log |data|) cost
ACTOR Future<GetKeyValuesReply> readRange(StorageServer* data,
                                          Version version,
                                          KeyRange range,
                                          int limit,
                                          int* pLimitBytes,
                                          SpanID parentSpan,
                                          IKeyValueStore::ReadType type) {
	state GetKeyValuesReply result;
	state StorageServer::VersionedData::ViewAtVersion view = data->data().at(version);
	state StorageServer::VersionedData::iterator vCurrent = view.end();
	state KeyRef readBegin;
	state KeyRef readEnd;
	state Key readBeginTemp;
	state int vCount = 0;
	state Span span("SS:readRange"_loc, parentSpan);

	// for caching the storage queue results during the first PTree traversal
	state VectorRef<KeyValueRef> resultCache;

	// for remembering the position in the resultCache
	state int pos = 0;

	// Check if the desired key-range is cached
	auto containingRange = data->cachedRangeMap.rangeContaining(range.begin);
	if (containingRange.value() && containingRange->range().end >= range.end) {
		//TraceEvent(SevDebug, "SSReadRangeCached").detail("Size",data->cachedRangeMap.size()).detail("ContainingRangeBegin",containingRange->range().begin).detail("ContainingRangeEnd",containingRange->range().end).
		//	detail("Begin", range.begin).detail("End",range.end);
		result.cached = true;
	} else
		result.cached = false;

	// if (limit >= 0) we are reading forward, else backward
	if (limit >= 0) {
		// We might care about a clear beginning before start that
		//  runs into range
		vCurrent = view.lastLessOrEqual(range.begin);
		if (vCurrent && vCurrent->isClearTo() && vCurrent->getEndKey() > range.begin)
			readBegin = vCurrent->getEndKey();
		else
			readBegin = range.begin;

		vCurrent = view.lower_bound(readBegin);

		while (limit > 0 && *pLimitBytes > 0 && readBegin < range.end) {
			ASSERT(!vCurrent || vCurrent.key() >= readBegin);
			ASSERT(data->storageVersion() <= version);

			/* Traverse the PTree further, if thare are no unconsumed resultCache items */
			if (pos == resultCache.size()) {
				if (vCurrent) {
					auto b = vCurrent;
					--b;
					ASSERT(!b || b.key() < readBegin);
				}

				// Read up to limit items from the view, stopping at the next clear (or the end of the range)
				int vSize = 0;
				while (vCurrent && vCurrent.key() < range.end && !vCurrent->isClearTo() && vCount < limit &&
				       vSize < *pLimitBytes) {
					// Store the versionedData results in resultCache
					resultCache.emplace_back(result.arena, vCurrent.key(), vCurrent->getValue());
					vSize += sizeof(KeyValueRef) + resultCache.cback().expectedSize();
					++vCount;
					++vCurrent;
				}
			}

			// Read the data on disk up to vCurrent (or the end of the range)
			readEnd = vCurrent ? std::min(vCurrent.key(), range.end) : range.end;
			RangeResult atStorageVersion =
			    wait(data->storage.readRange(KeyRangeRef(readBegin, readEnd), limit, *pLimitBytes, type));

			ASSERT(atStorageVersion.size() <= limit);
			if (data->storageVersion() > version)
				throw transaction_too_old();

			// merge the sets in resultCache with the sets on disk, stopping at the last key from disk if there is
			// 'more'
			int prevSize = result.data.size();
			merge(result.arena,
			      result.data,
			      resultCache,
			      atStorageVersion,
			      vCount,
			      limit,
			      atStorageVersion.more,
			      pos,
			      *pLimitBytes);
			limit -= result.data.size() - prevSize;

			for (auto i = result.data.begin() + prevSize; i != result.data.end(); i++) {
				*pLimitBytes -= sizeof(KeyValueRef) + i->expectedSize();
			}

			if (limit <= 0 || *pLimitBytes <= 0) {
				break;
			}

			// Setup for the next iteration
			// If we hit our limits reading from disk but then combining with MVCC gave us back more room
			if (atStorageVersion
			        .more) { // if there might be more data, begin reading right after what we already found to find out
				ASSERT(result.data.end()[-1].key == atStorageVersion.end()[-1].key);
				readBegin = readBeginTemp = keyAfter(result.data.end()[-1].key);
			} else if (vCurrent && vCurrent->isClearTo()) { // if vCurrent is a clear, skip it.
				ASSERT(vCurrent->getEndKey() > readBegin);
				readBegin = vCurrent->getEndKey(); // next disk read should start at the end of the clear
				++vCurrent;
			} else {
				ASSERT(readEnd == range.end);
				break;
			}
		}
	} else {
		vCurrent = view.lastLess(range.end);

		// A clear might extend all the way to range.end
		if (vCurrent && vCurrent->isClearTo() && vCurrent->getEndKey() >= range.end) {
			readEnd = vCurrent.key();
			--vCurrent;
		} else {
			readEnd = range.end;
		}

		while (limit < 0 && *pLimitBytes > 0 && readEnd > range.begin) {
			ASSERT(!vCurrent || vCurrent.key() < readEnd);
			ASSERT(data->storageVersion() <= version);

			/* Traverse the PTree further, if thare are no unconsumed resultCache items */
			if (pos == resultCache.size()) {
				if (vCurrent) {
					auto b = vCurrent;
					++b;
					ASSERT(!b || b.key() >= readEnd);
				}

				vCount = 0;
				int vSize = 0;
				while (vCurrent && vCurrent.key() >= range.begin && !vCurrent->isClearTo() && vCount < -limit &&
				       vSize < *pLimitBytes) {
					// Store the versionedData results in resultCache
					resultCache.emplace_back(result.arena, vCurrent.key(), vCurrent->getValue());
					vSize += sizeof(KeyValueRef) + resultCache.cback().expectedSize();
					++vCount;
					--vCurrent;
				}
			}

			readBegin = vCurrent ? std::max(vCurrent->isClearTo() ? vCurrent->getEndKey() : vCurrent.key(), range.begin)
			                     : range.begin;
			RangeResult atStorageVersion =
			    wait(data->storage.readRange(KeyRangeRef(readBegin, readEnd), limit, *pLimitBytes, type));

			ASSERT(atStorageVersion.size() <= -limit);
			if (data->storageVersion() > version)
				throw transaction_too_old();

			int prevSize = result.data.size();
			merge(result.arena,
			      result.data,
			      resultCache,
			      atStorageVersion,
			      vCount,
			      limit,
			      atStorageVersion.more,
			      pos,
			      *pLimitBytes);
			limit += result.data.size() - prevSize;

			for (auto i = result.data.begin() + prevSize; i != result.data.end(); i++) {
				*pLimitBytes -= sizeof(KeyValueRef) + i->expectedSize();
			}

			if (limit >= 0 || *pLimitBytes <= 0) {
				break;
			}

			if (atStorageVersion.more) {
				ASSERT(result.data.end()[-1].key == atStorageVersion.end()[-1].key);
				readEnd = result.data.end()[-1].key;
			} else if (vCurrent && vCurrent->isClearTo()) {
				ASSERT(vCurrent.key() < readEnd);
				readEnd = vCurrent.key();
				--vCurrent;
			} else {
				ASSERT(readBegin == range.begin);
				break;
			}
		}
	}

	// all but the last item are less than *pLimitBytes
	ASSERT(result.data.size() == 0 || *pLimitBytes + result.data.end()[-1].expectedSize() + sizeof(KeyValueRef) > 0);
	result.more = limit == 0 || *pLimitBytes <= 0; // FIXME: Does this have to be exact?
	result.version = version;
	return result;
}

// bool selectorInRange( KeySelectorRef const& sel, KeyRangeRef const& range ) {
// Returns true if the given range suffices to at least begin to resolve the given KeySelectorRef
//	return sel.getKey() >= range.begin && (sel.isBackward() ? sel.getKey() <= range.end : sel.getKey() < range.end);
//}

ACTOR Future<Key> findKey(StorageServer* data,
                          KeySelectorRef sel,
                          Version version,
                          KeyRange range,
                          int* pOffset,
                          SpanID parentSpan,
                          IKeyValueStore::ReadType type)
// Attempts to find the key indicated by sel in the data at version, within range.
// Precondition: selectorInRange(sel, range)
// If it is found, offset is set to 0 and a key is returned which falls inside range.
// If the search would depend on any key outside range OR if the key selector offset is too large (range read returns
// too many bytes), it returns either
//   a negative offset and a key in [range.begin, sel.getKey()], indicating the key is (the first key <= returned key) +
//   offset, or a positive offset and a key in (sel.getKey(), range.end], indicating the key is (the first key >=
//   returned key) + offset-1
// The range passed in to this function should specify a shard.  If range.begin is repeatedly not the beginning of a
// shard, then it is possible to get stuck looping here
{
	ASSERT(version != latestVersion);
	ASSERT(selectorInRange(sel, range) && version >= data->oldestVersion.get());

	// Count forward or backward distance items, skipping the first one if it == key and skipEqualKey
	state bool forward = sel.offset > 0; // If forward, result >= sel.getKey(); else result <= sel.getKey()
	state int sign = forward ? +1 : -1;
	state bool skipEqualKey = sel.orEqual == forward;
	state int distance = forward ? sel.offset : 1 - sel.offset;
	state Span span("SS.findKey"_loc, { parentSpan });

	// Don't limit the number of bytes if this is a trivial key selector (there will be at most two items returned from
	// the read range in this case)
	state int maxBytes;
	if (sel.offset <= 1 && sel.offset >= 0)
		maxBytes = std::numeric_limits<int>::max();
	else
		maxBytes = (g_network->isSimulated() && g_simulator.tssMode == ISimulator::TSSMode::Disabled && BUGGIFY)
		               ? SERVER_KNOBS->BUGGIFY_LIMIT_BYTES
		               : SERVER_KNOBS->STORAGE_LIMIT_BYTES;

	state GetKeyValuesReply rep = wait(
	    readRange(data,
	              version,
	              forward ? KeyRangeRef(sel.getKey(), range.end) : KeyRangeRef(range.begin, keyAfter(sel.getKey())),
	              (distance + skipEqualKey) * sign,
	              &maxBytes,
	              span.context,
	              type));
	state bool more = rep.more && rep.data.size() != distance + skipEqualKey;

	// If we get only one result in the reverse direction as a result of the data being too large, we could get stuck in
	// a loop
	if (more && !forward && rep.data.size() == 1) {
		TEST(true); // Reverse key selector returned only one result in range read
		maxBytes = std::numeric_limits<int>::max();
		GetKeyValuesReply rep2 = wait(readRange(
		    data, version, KeyRangeRef(range.begin, keyAfter(sel.getKey())), -2, &maxBytes, span.context, type));
		rep = rep2;
		more = rep.more && rep.data.size() != distance + skipEqualKey;
		ASSERT(rep.data.size() == 2 || !more);
	}

	int index = distance - 1;
	if (skipEqualKey && rep.data.size() && rep.data[0].key == sel.getKey())
		++index;

	if (index < rep.data.size()) {
		*pOffset = 0;

		if (SERVER_KNOBS->READ_SAMPLING_ENABLED) {
			int64_t bytesReadPerKSecond =
			    std::max((int64_t)rep.data[index].key.size(), SERVER_KNOBS->EMPTY_READ_PENALTY);
			data->metrics.notifyBytesReadPerKSecond(sel.getKey(), bytesReadPerKSecond);
		}

		return rep.data[index].key;
	} else {
		if (SERVER_KNOBS->READ_SAMPLING_ENABLED) {
			int64_t bytesReadPerKSecond = SERVER_KNOBS->EMPTY_READ_PENALTY;
			data->metrics.notifyBytesReadPerKSecond(sel.getKey(), bytesReadPerKSecond);
		}

		// FIXME: If range.begin=="" && !forward, return success?
		*pOffset = index - rep.data.size() + 1;
		if (!forward)
			*pOffset = -*pOffset;

		if (more) {
			TEST(true); // Key selector read range had more results

			ASSERT(rep.data.size());
			Key returnKey = forward ? keyAfter(rep.data.back().key) : rep.data.back().key;

			// This is possible if key/value pairs are very large and only one result is returned on a last less than
			// query SOMEDAY: graceful handling of exceptionally sized values
			ASSERT(returnKey != sel.getKey());
			return returnKey;
		} else {
			return forward ? range.end : range.begin;
		}
	}
}

KeyRange getShardKeyRange(StorageServer* data, const KeySelectorRef& sel)
// Returns largest range such that the shard state isReadable and selectorInRange(sel, range) or wrong_shard_server if
// no such range exists
{
	auto i = sel.isBackward() ? data->shards.rangeContainingKeyBefore(sel.getKey())
	                          : data->shards.rangeContaining(sel.getKey());
	if (!i->value()->isReadable())
		throw wrong_shard_server();
	ASSERT(selectorInRange(sel, i->range()));
	return i->range();
}

ACTOR Future<Void> getKeyValuesQ(StorageServer* data, GetKeyValuesRequest req)
// Throws a wrong_shard_server if the keys in the request or result depend on data outside this server OR if a large
// selector offset prevents all data from being read in one range read
{
	state Span span("SS:getKeyValues"_loc, { req.spanContext });
	state int64_t resultSize = 0;
	state IKeyValueStore::ReadType type =
	    req.isFetchKeys ? IKeyValueStore::ReadType::FETCH : IKeyValueStore::ReadType::NORMAL;
	getCurrentLineage()->modify(&TransactionLineage::txID) = req.spanContext.first();

	++data->counters.getRangeQueries;
	++data->counters.allQueries;
	++data->readQueueSizeMetric;
	data->maxQueryQueue = std::max<int>(
	    data->maxQueryQueue, data->counters.allQueries.getValue() - data->counters.finishedQueries.getValue());

	// Active load balancing runs at a very high priority (to obtain accurate queue lengths)
	// so we need to downgrade here
	if (SERVER_KNOBS->FETCH_KEYS_LOWER_PRIORITY && req.isFetchKeys) {
		wait(delay(0, TaskPriority::FetchKeys));
	} else {
		wait(data->getQueryDelay());
	}

	try {
		if (req.debugID.present())
			g_traceBatch.addEvent("TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValues.Before");
		state Version version = wait(waitForVersion(data, req.version, span.context));

		state uint64_t changeCounter = data->shardChangeCounter;
		//		try {
		state KeyRange shard = getShardKeyRange(data, req.begin);

		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValues.AfterVersion");
		//.detail("ShardBegin", shard.begin).detail("ShardEnd", shard.end);
		//} catch (Error& e) { TraceEvent("WrongShardServer", data->thisServerID).detail("Begin",
		// req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("Shard",
		//"None").detail("In", "getKeyValues>getShardKeyRange"); throw e; }

		if (!selectorInRange(req.end, shard) && !(req.end.isFirstGreaterOrEqual() && req.end.getKey() == shard.end)) {
			//			TraceEvent("WrongShardServer1", data->thisServerID).detail("Begin",
			// req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("ShardBegin",
			// shard.begin).detail("ShardEnd", shard.end).detail("In", "getKeyValues>checkShardExtents");
			throw wrong_shard_server();
		}

		state int offset1 = 0;
		state int offset2;
		state Future<Key> fBegin = req.begin.isFirstGreaterOrEqual()
		                               ? Future<Key>(req.begin.getKey())
		                               : findKey(data, req.begin, version, shard, &offset1, span.context, type);
		state Future<Key> fEnd = req.end.isFirstGreaterOrEqual()
		                             ? Future<Key>(req.end.getKey())
		                             : findKey(data, req.end, version, shard, &offset2, span.context, type);
		state Key begin = wait(fBegin);
		state Key end = wait(fEnd);

		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValues.AfterKeys");
		//.detail("Off1",offset1).detail("Off2",offset2).detail("ReqBegin",req.begin.getKey()).detail("ReqEnd",req.end.getKey());

		// Offsets of zero indicate begin/end keys in this shard, which obviously means we can answer the query
		// An end offset of 1 is also OK because the end key is exclusive, so if the first key of the next shard is the
		// end the last actual key returned must be from this shard. A begin offset of 1 is also OK because then either
		// begin is past end or equal to end (so the result is definitely empty)
		if ((offset1 && offset1 != 1) || (offset2 && offset2 != 1)) {
			TEST(true); // wrong_shard_server due to offset
			// We could detect when offset1 takes us off the beginning of the database or offset2 takes us off the end,
			// and return a clipped range rather than an error (since that is what the NativeAPI.getRange will do anyway
			// via its "slow path"), but we would have to add some flags to the response to encode whether we went off
			// the beginning and the end, since it needs that information.
			//TraceEvent("WrongShardServer2", data->thisServerID).detail("Begin", req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("ShardBegin", shard.begin).detail("ShardEnd", shard.end).detail("In", "getKeyValues>checkOffsets").detail("BeginKey", begin).detail("EndKey", end).detail("BeginOffset", offset1).detail("EndOffset", offset2);
			throw wrong_shard_server();
		}

		if (begin >= end) {
			if (req.debugID.present())
				g_traceBatch.addEvent("TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValues.Send");
			//.detail("Begin",begin).detail("End",end);

			GetKeyValuesReply none;
			none.version = version;
			none.more = false;
			none.penalty = data->getPenalty();

			data->checkChangeCounter(changeCounter,
			                         KeyRangeRef(std::min<KeyRef>(req.begin.getKey(), req.end.getKey()),
			                                     std::max<KeyRef>(req.begin.getKey(), req.end.getKey())));
			req.reply.send(none);
		} else {
			state int remainingLimitBytes = req.limitBytes;

			GetKeyValuesReply _r = wait(
			    readRange(data, version, KeyRangeRef(begin, end), req.limit, &remainingLimitBytes, span.context, type));
			GetKeyValuesReply r = _r;

			if (req.debugID.present())
				g_traceBatch.addEvent(
				    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValues.AfterReadRange");
			//.detail("Begin",begin).detail("End",end).detail("SizeOf",r.data.size());
			data->checkChangeCounter(
			    changeCounter,
			    KeyRangeRef(std::min<KeyRef>(begin, std::min<KeyRef>(req.begin.getKey(), req.end.getKey())),
			                std::max<KeyRef>(end, std::max<KeyRef>(req.begin.getKey(), req.end.getKey()))));
			if (EXPENSIVE_VALIDATION) {
				for (int i = 0; i < r.data.size(); i++)
					ASSERT(r.data[i].key >= begin && r.data[i].key < end);
				ASSERT(r.data.size() <= std::abs(req.limit));
			}

			/*for( int i = 0; i < r.data.size(); i++ ) {
			    StorageMetrics m;
			    m.bytesPerKSecond = r.data[i].expectedSize();
			    m.iosPerKSecond = 1; //FIXME: this should be 1/r.data.size(), but we cannot do that because it is an int
			    data->metrics.notify(r.data[i].key, m);
			}*/

			// For performance concerns, the cost of a range read is billed to the start key and end key of the range.
			int64_t totalByteSize = 0;
			for (int i = 0; i < r.data.size(); i++) {
				totalByteSize += r.data[i].expectedSize();
			}
			if (totalByteSize > 0 && SERVER_KNOBS->READ_SAMPLING_ENABLED) {
				int64_t bytesReadPerKSecond = std::max(totalByteSize, SERVER_KNOBS->EMPTY_READ_PENALTY) / 2;
				data->metrics.notifyBytesReadPerKSecond(r.data[0].key, bytesReadPerKSecond);
				data->metrics.notifyBytesReadPerKSecond(r.data[r.data.size() - 1].key, bytesReadPerKSecond);
			}

			r.penalty = data->getPenalty();
			req.reply.send(r);

			resultSize = req.limitBytes - remainingLimitBytes;
			data->counters.bytesQueried += resultSize;
			data->counters.rowsQueried += r.data.size();
			if (r.data.size() == 0) {
				++data->counters.emptyQueries;
			}
		}
	} catch (Error& e) {
		if (!canReplyWith(e))
			throw;
		data->sendErrorWithPenalty(req.reply, e, data->getPenalty());
	}

	data->transactionTagCounter.addRequest(req.tags, resultSize);
	++data->counters.finishedQueries;
	--data->readQueueSizeMetric;

	double duration = g_network->timer() - req.requestTime();
	data->counters.readLatencySample.addMeasurement(duration);
	if (data->latencyBandConfig.present()) {
		int maxReadBytes =
		    data->latencyBandConfig.get().readConfig.maxReadBytes.orDefault(std::numeric_limits<int>::max());
		int maxSelectorOffset =
		    data->latencyBandConfig.get().readConfig.maxKeySelectorOffset.orDefault(std::numeric_limits<int>::max());
		data->counters.readLatencyBands.addMeasurement(duration,
		                                               resultSize > maxReadBytes ||
		                                                   abs(req.begin.offset) > maxSelectorOffset ||
		                                                   abs(req.end.offset) > maxSelectorOffset);
	}

	return Void();
}

ACTOR Future<RangeResult> quickGetKeyValues(StorageServer* data,
                                            StringRef prefix,
                                            Version version,
                                            // To provide span context, tags, debug ID to underlying lookups.
                                            GetKeyValuesAndFlatMapRequest* pOriginalReq) {
	try {
		// TODO: Use a lower level API may be better? Or tweak priorities?
		GetKeyValuesRequest req;
		req.spanContext = pOriginalReq->spanContext;
		req.arena = Arena();
		req.begin = firstGreaterOrEqual(KeyRef(req.arena, prefix));
		req.end = firstGreaterOrEqual(strinc(prefix, req.arena));
		req.version = version;
		req.tags = pOriginalReq->tags;
		req.debugID = pOriginalReq->debugID;

		// Note that it does not use readGuard to avoid server being overloaded here. Throttling is enforced at the
		// original request level, rather than individual underlying lookups. The reason is that throttle any individual
		// underlying lookup will fail the original request, which is not productive.
		data->actors.add(getKeyValuesQ(data, req));
		GetKeyValuesReply reply = wait(req.reply.getFuture());
		if (!reply.error.present()) {
			++data->counters.quickGetKeyValuesHit;
			// Convert GetKeyValuesReply to RangeResult.
			return RangeResult(RangeResultRef(reply.data, reply.more), reply.arena);
		}
		// Otherwise fallback.
	} catch (Error& e) {
		// Fallback.
	}

	++data->counters.quickGetKeyValuesMiss;
	if (SERVER_KNOBS->QUICK_GET_KEY_VALUES_FALLBACK) {
		state Transaction tr(data->cx);
		tr.setVersion(version);
		// TODO: is DefaultPromiseEndpoint the best priority for this?
		tr.info.taskID = TaskPriority::DefaultPromiseEndpoint;
		Future<RangeResult> rangeResultFuture = tr.getRange(prefixRange(prefix), Snapshot::True);
		// TODO: async in case it needs to read from other servers.
		RangeResult rangeResult = wait(rangeResultFuture);
		return rangeResult;
	} else {
		throw quick_get_key_values_miss();
	}
};

Key constructMappedKey(KeyValueRef* keyValue, Tuple& mappedKeyFormatTuple, bool& isRangeQuery) {
	// Lazily parse key and/or value to tuple because they may not need to be a tuple if not used.
	Optional<Tuple> keyTuple;
	Optional<Tuple> valueTuple;

	Tuple mappedKeyTuple;
	for (int i = 0; i < mappedKeyFormatTuple.size(); i++) {
		Tuple::ElementType type = mappedKeyFormatTuple.getType(i);
		if (type == Tuple::BYTES || type == Tuple::UTF8) {
			std::string s = mappedKeyFormatTuple.getString(i).toString();
			auto sz = s.size();

			// Handle escape.
			bool escaped = false;
			size_t p = 0;
			while (true) {
				size_t found = s.find("{{", p);
				if (found == std::string::npos) {
					break;
				}
				s.replace(found, 2, "{");
				p += 1;
				escaped = true;
			}
			p = 0;
			while (true) {
				size_t found = s.find("}}", p);
				if (found == std::string::npos) {
					break;
				}
				s.replace(found, 2, "}");
				p += 1;
				escaped = true;
			}
			if (escaped) {
				// If the element uses escape, cope the escaped version.
				mappedKeyTuple.append(s);
			}
			// {K[??]} or {V[??]}
			else if (sz > 5 && s[0] == '{' && (s[1] == 'K' || s[1] == 'V') && s[2] == '[' && s[sz - 2] == ']' &&
			         s[sz - 1] == '}') {
				int idx;
				try {
					idx = std::stoi(s.substr(3, sz - 5));
				} catch (std::exception& e) {
					throw mapper_bad_index();
				}
				Tuple* referenceTuple;
				if (s[1] == 'K') {
					// Use keyTuple as reference.
					if (!keyTuple.present()) {
						// May throw exception if the key is not parsable as a tuple.
						keyTuple = Tuple::unpack(keyValue->key);
					}
					referenceTuple = &keyTuple.get();
				} else if (s[1] == 'V') {
					// Use valueTuple as reference.
					if (!valueTuple.present()) {
						// May throw exception if the value is not parsable as a tuple.
						valueTuple = Tuple::unpack(keyValue->value);
					}
					referenceTuple = &valueTuple.get();
				} else {
					ASSERT(false);
					throw internal_error();
				}

				if (idx < 0 || idx >= referenceTuple->size()) {
					throw mapper_bad_index();
				}
				mappedKeyTuple.append(referenceTuple->subTuple(idx, idx + 1));
			} else if (s == "{...}") {
				// Range query.
				if (i != mappedKeyFormatTuple.size() - 1) {
					// It must be the last element of the mapper tuple
					throw mapper_bad_range_decriptor();
				}
				// Every record will try to set it. It's ugly, but not wrong.
				isRangeQuery = true;
				// Do not add it to the mapped key.
			} else {
				// If the element is a string but neither escaped nor descriptors, just copy it.
				mappedKeyTuple.append(mappedKeyFormatTuple.subTuple(i, i + 1));
			}
		} else {
			// If the element not a string, just copy it.
			mappedKeyTuple.append(mappedKeyFormatTuple.subTuple(i, i + 1));
		}
	}
	return mappedKeyTuple.getDataAsStandalone();
}

TEST_CASE("/fdbserver/storageserver/constructMappedKey") {
	Key key = Tuple().append("key-0"_sr).append("key-1"_sr).append("key-2"_sr).getDataAsStandalone();
	Value value = Tuple().append("value-0"_sr).append("value-1"_sr).append("value-2"_sr).getDataAsStandalone();
	state KeyValueRef kvr(key, value);
	{
		Tuple mapperTuple = Tuple()
		                        .append("normal"_sr)
		                        .append("{{escaped}}"_sr)
		                        .append("{K[2]}"_sr)
		                        .append("{V[0]}"_sr)
		                        .append("{...}"_sr);

		bool isRangeQuery = false;
		Key mappedKey = constructMappedKey(&kvr, mapperTuple, isRangeQuery);

		Key expectedMappedKey = Tuple()
		                            .append("normal"_sr)
		                            .append("{escaped}"_sr)
		                            .append("key-2"_sr)
		                            .append("value-0"_sr)
		                            .getDataAsStandalone();
		//		std::cout << printable(mappedKey) << " == " << printable(expectedMappedKey) << std::endl;
		ASSERT(mappedKey.compare(expectedMappedKey) == 0);
		ASSERT(isRangeQuery == true);
	}
	{
		Tuple mapperTuple = Tuple().append("{{{{}}"_sr).append("}}"_sr);

		bool isRangeQuery = false;
		Key mappedKey = constructMappedKey(&kvr, mapperTuple, isRangeQuery);

		Key expectedMappedKey = Tuple().append("{{}"_sr).append("}"_sr).getDataAsStandalone();
		//		std::cout << printable(mappedKey) << " == " << printable(expectedMappedKey) << std::endl;
		ASSERT(mappedKey.compare(expectedMappedKey) == 0);
		ASSERT(isRangeQuery == false);
	}
	{
		Tuple mapperTuple = Tuple().append("{{{{}}"_sr).append("}}"_sr);

		bool isRangeQuery = false;
		Key mappedKey = constructMappedKey(&kvr, mapperTuple, isRangeQuery);

		Key expectedMappedKey = Tuple().append("{{}"_sr).append("}"_sr).getDataAsStandalone();
		//		std::cout << printable(mappedKey) << " == " << printable(expectedMappedKey) << std::endl;
		ASSERT(mappedKey.compare(expectedMappedKey) == 0);
		ASSERT(isRangeQuery == false);
	}
	{
		Tuple mapperTuple = Tuple().append("{K[100]}"_sr);
		bool isRangeQuery = false;
		state bool throwException = false;
		try {
			Key mappedKey = constructMappedKey(&kvr, mapperTuple, isRangeQuery);
		} catch (Error& e) {
			ASSERT(e.code() == error_code_mapper_bad_index);
			throwException = true;
		}
		ASSERT(throwException);
	}
	{
		Tuple mapperTuple = Tuple().append("{...}"_sr).append("last-element"_sr);
		bool isRangeQuery = false;
		state bool throwException2 = false;
		try {
			Key mappedKey = constructMappedKey(&kvr, mapperTuple, isRangeQuery);
		} catch (Error& e) {
			ASSERT(e.code() == error_code_mapper_bad_range_decriptor);
			throwException2 = true;
		}
		ASSERT(throwException2);
	}
	{
		Tuple mapperTuple = Tuple().append("{K[not-a-number]}"_sr);
		bool isRangeQuery = false;
		state bool throwException3 = false;
		try {
			Key mappedKey = constructMappedKey(&kvr, mapperTuple, isRangeQuery);
		} catch (Error& e) {
			ASSERT(e.code() == error_code_mapper_bad_index);
			throwException3 = true;
		}
		ASSERT(throwException3);
	}
	return Void();
}

ACTOR Future<GetKeyValuesAndFlatMapReply> flatMap(StorageServer* data,
                                                  GetKeyValuesReply input,
                                                  StringRef mapper,
                                                  // To provide span context, tags, debug ID to underlying lookups.
                                                  GetKeyValuesAndFlatMapRequest* pOriginalReq) {
	state GetKeyValuesAndFlatMapReply result;
	result.version = input.version;
	if (input.more) {
		throw get_key_values_and_map_has_more();
	}
	result.more = input.more;
	result.cached = input.cached;
	result.arena.dependsOn(input.arena);

	result.data.reserve(result.arena, input.data.size());
	state bool isRangeQuery = false;
	state Tuple mappedKeyFormatTuple = Tuple::unpack(mapper);
	state KeyValueRef* it = input.data.begin();
	for (; it != input.data.end(); it++) {
		state StringRef key = it->key;

		state Key mappedKey = constructMappedKey(it, mappedKeyFormatTuple, isRangeQuery);
		// Make sure the mappedKey is always available, so that it's good even we want to get key asynchronously.
		result.arena.dependsOn(mappedKey.arena());

		if (isRangeQuery) {
			// Use the mappedKey as the prefix of the range query.
			RangeResult rangeResult = wait(quickGetKeyValues(data, mappedKey, input.version, pOriginalReq));

			if (rangeResult.more) {
				// Probably the fan out is too large. The user should use the old way to query.
				throw quick_get_key_values_has_more();
			}
			result.arena.dependsOn(rangeResult.arena());
			for (int i = 0; i < rangeResult.size(); i++) {
				result.data.emplace_back(result.arena, rangeResult[i].key, rangeResult[i].value);
			}
		} else {
			Optional<Value> valueOption = wait(quickGetValue(data, mappedKey, input.version, pOriginalReq));

			if (valueOption.present()) {
				Value value = valueOption.get();
				result.arena.dependsOn(value.arena());
				result.data.emplace_back(result.arena, mappedKey, value);
			} else {
				// TODO: Shall we throw exception if the key doesn't exist or the range is empty?
			}
		}
	}
	return result;
}

// Most of the actor is copied from getKeyValuesQ. I tried to use templates but things become nearly impossible after
// combining actor shenanigans with template shenanigans.
ACTOR Future<Void> getKeyValuesAndFlatMapQ(StorageServer* data, GetKeyValuesAndFlatMapRequest req)
// Throws a wrong_shard_server if the keys in the request or result depend on data outside this server OR if a large
// selector offset prevents all data from being read in one range read
{
	state Span span("SS:getKeyValuesAndFlatMap"_loc, { req.spanContext });
	state int64_t resultSize = 0;
	state IKeyValueStore::ReadType type =
	    req.isFetchKeys ? IKeyValueStore::ReadType::FETCH : IKeyValueStore::ReadType::NORMAL;
	getCurrentLineage()->modify(&TransactionLineage::txID) = req.spanContext.first();

	++data->counters.getRangeAndFlatMapQueries;
	++data->counters.allQueries;
	++data->readQueueSizeMetric;
	data->maxQueryQueue = std::max<int>(
	    data->maxQueryQueue, data->counters.allQueries.getValue() - data->counters.finishedQueries.getValue());

	// Active load balancing runs at a very high priority (to obtain accurate queue lengths)
	// so we need to downgrade here
	if (SERVER_KNOBS->FETCH_KEYS_LOWER_PRIORITY && req.isFetchKeys) {
		wait(delay(0, TaskPriority::FetchKeys));
	} else {
		wait(data->getQueryDelay());
	}

	try {
		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesAndFlatMap.Before");
		state Version version = wait(waitForVersion(data, req.version, span.context));

		state uint64_t changeCounter = data->shardChangeCounter;
		//		try {
		state KeyRange shard = getShardKeyRange(data, req.begin);

		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesAndFlatMap.AfterVersion");
		//.detail("ShardBegin", shard.begin).detail("ShardEnd", shard.end);
		//} catch (Error& e) { TraceEvent("WrongShardServer", data->thisServerID).detail("Begin",
		// req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("Shard",
		//"None").detail("In", "getKeyValuesAndFlatMap>getShardKeyRange"); throw e; }

		if (!selectorInRange(req.end, shard) && !(req.end.isFirstGreaterOrEqual() && req.end.getKey() == shard.end)) {
			//			TraceEvent("WrongShardServer1", data->thisServerID).detail("Begin",
			// req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("ShardBegin",
			// shard.begin).detail("ShardEnd", shard.end).detail("In", "getKeyValuesAndFlatMap>checkShardExtents");
			throw wrong_shard_server();
		}

		state int offset1 = 0;
		state int offset2;
		state Future<Key> fBegin = req.begin.isFirstGreaterOrEqual()
		                               ? Future<Key>(req.begin.getKey())
		                               : findKey(data, req.begin, version, shard, &offset1, span.context, type);
		state Future<Key> fEnd = req.end.isFirstGreaterOrEqual()
		                             ? Future<Key>(req.end.getKey())
		                             : findKey(data, req.end, version, shard, &offset2, span.context, type);
		state Key begin = wait(fBegin);
		state Key end = wait(fEnd);

		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesAndFlatMap.AfterKeys");
		//.detail("Off1",offset1).detail("Off2",offset2).detail("ReqBegin",req.begin.getKey()).detail("ReqEnd",req.end.getKey());

		// Offsets of zero indicate begin/end keys in this shard, which obviously means we can answer the query
		// An end offset of 1 is also OK because the end key is exclusive, so if the first key of the next shard is the
		// end the last actual key returned must be from this shard. A begin offset of 1 is also OK because then either
		// begin is past end or equal to end (so the result is definitely empty)
		if ((offset1 && offset1 != 1) || (offset2 && offset2 != 1)) {
			TEST(true); // wrong_shard_server due to offset in getKeyValuesAndFlatMapQ
			// We could detect when offset1 takes us off the beginning of the database or offset2 takes us off the end,
			// and return a clipped range rather than an error (since that is what the NativeAPI.getRange will do anyway
			// via its "slow path"), but we would have to add some flags to the response to encode whether we went off
			// the beginning and the end, since it needs that information.
			//TraceEvent("WrongShardServer2", data->thisServerID).detail("Begin", req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("ShardBegin", shard.begin).detail("ShardEnd", shard.end).detail("In", "getKeyValuesAndFlatMap>checkOffsets").detail("BeginKey", begin).detail("EndKey", end).detail("BeginOffset", offset1).detail("EndOffset", offset2);
			throw wrong_shard_server();
		}

		if (begin >= end) {
			if (req.debugID.present())
				g_traceBatch.addEvent(
				    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesAndFlatMap.Send");
			//.detail("Begin",begin).detail("End",end);

			GetKeyValuesAndFlatMapReply none;
			none.version = version;
			none.more = false;
			none.penalty = data->getPenalty();

			data->checkChangeCounter(changeCounter,
			                         KeyRangeRef(std::min<KeyRef>(req.begin.getKey(), req.end.getKey()),
			                                     std::max<KeyRef>(req.begin.getKey(), req.end.getKey())));
			req.reply.send(none);
		} else {
			state int remainingLimitBytes = req.limitBytes;

			GetKeyValuesReply getKeyValuesReply = wait(
			    readRange(data, version, KeyRangeRef(begin, end), req.limit, &remainingLimitBytes, span.context, type));

			state GetKeyValuesAndFlatMapReply r;
			try {
				// Map the scanned range to another list of keys and look up.
				GetKeyValuesAndFlatMapReply _r = wait(flatMap(data, getKeyValuesReply, req.mapper, &req));
				r = _r;
			} catch (Error& e) {
				TraceEvent("FlatMapError").error(e);
				throw;
			}

			if (req.debugID.present())
				g_traceBatch.addEvent("TransactionDebug",
				                      req.debugID.get().first(),
				                      "storageserver.getKeyValuesAndFlatMap.AfterReadRange");
			//.detail("Begin",begin).detail("End",end).detail("SizeOf",r.data.size());
			data->checkChangeCounter(
			    changeCounter,
			    KeyRangeRef(std::min<KeyRef>(begin, std::min<KeyRef>(req.begin.getKey(), req.end.getKey())),
			                std::max<KeyRef>(end, std::max<KeyRef>(req.begin.getKey(), req.end.getKey()))));
			if (EXPENSIVE_VALIDATION) {
				// TODO: Only mapped keys are returned, which are not supposed to be in the range.
				//				for (int i = 0; i < r.data.size(); i++)
				//					ASSERT(r.data[i].key >= begin && r.data[i].key < end);
				// TODO: GetKeyValuesWithFlatMapRequest doesn't respect limit yet.
				//                ASSERT(r.data.size() <= std::abs(req.limit));
			}

			/*for( int i = 0; i < r.data.size(); i++ ) {
			    StorageMetrics m;
			    m.bytesPerKSecond = r.data[i].expectedSize();
			    m.iosPerKSecond = 1; //FIXME: this should be 1/r.data.size(), but we cannot do that because it is an int
			    data->metrics.notify(r.data[i].key, m);
			}*/

			// For performance concerns, the cost of a range read is billed to the start key and end key of the range.
			int64_t totalByteSize = 0;
			for (int i = 0; i < r.data.size(); i++) {
				totalByteSize += r.data[i].expectedSize();
			}
			if (totalByteSize > 0 && SERVER_KNOBS->READ_SAMPLING_ENABLED) {
				int64_t bytesReadPerKSecond = std::max(totalByteSize, SERVER_KNOBS->EMPTY_READ_PENALTY) / 2;
				data->metrics.notifyBytesReadPerKSecond(r.data[0].key, bytesReadPerKSecond);
				data->metrics.notifyBytesReadPerKSecond(r.data[r.data.size() - 1].key, bytesReadPerKSecond);
			}

			r.penalty = data->getPenalty();
			req.reply.send(r);

			resultSize = req.limitBytes - remainingLimitBytes;
			data->counters.bytesQueried += resultSize;
			data->counters.rowsQueried += r.data.size();
			if (r.data.size() == 0) {
				++data->counters.emptyQueries;
			}
		}
	} catch (Error& e) {
		if (!canReplyWith(e))
			throw;
		data->sendErrorWithPenalty(req.reply, e, data->getPenalty());
	}

	data->transactionTagCounter.addRequest(req.tags, resultSize);
	++data->counters.finishedQueries;
	--data->readQueueSizeMetric;

	double duration = g_network->timer() - req.requestTime();
	data->counters.readLatencySample.addMeasurement(duration);
	if (data->latencyBandConfig.present()) {
		int maxReadBytes =
		    data->latencyBandConfig.get().readConfig.maxReadBytes.orDefault(std::numeric_limits<int>::max());
		int maxSelectorOffset =
		    data->latencyBandConfig.get().readConfig.maxKeySelectorOffset.orDefault(std::numeric_limits<int>::max());
		data->counters.readLatencyBands.addMeasurement(duration,
		                                               resultSize > maxReadBytes ||
		                                                   abs(req.begin.offset) > maxSelectorOffset ||
		                                                   abs(req.end.offset) > maxSelectorOffset);
	}

	return Void();
}

ACTOR Future<Void> getKeyValuesStreamQ(StorageServer* data, GetKeyValuesStreamRequest req)
// Throws a wrong_shard_server if the keys in the request or result depend on data outside this server OR if a large
// selector offset prevents all data from being read in one range read
{
	state Span span("SS:getKeyValuesStream"_loc, { req.spanContext });
	state int64_t resultSize = 0;
	state IKeyValueStore::ReadType type =
	    req.isFetchKeys ? IKeyValueStore::ReadType::FETCH : IKeyValueStore::ReadType::NORMAL;

	req.reply.setByteLimit(SERVER_KNOBS->RANGESTREAM_LIMIT_BYTES);
	++data->counters.getRangeStreamQueries;
	++data->counters.allQueries;
	++data->readQueueSizeMetric;
	data->maxQueryQueue = std::max<int>(
	    data->maxQueryQueue, data->counters.allQueries.getValue() - data->counters.finishedQueries.getValue());

	// Active load balancing runs at a very high priority (to obtain accurate queue lengths)
	// so we need to downgrade here
	if (SERVER_KNOBS->FETCH_KEYS_LOWER_PRIORITY && req.isFetchKeys) {
		wait(delay(0, TaskPriority::FetchKeys));
	} else {
		wait(delay(0, TaskPriority::DefaultEndpoint));
	}

	try {
		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesStream.Before");
		state Version version = wait(waitForVersion(data, req.version, span.context));

		state uint64_t changeCounter = data->shardChangeCounter;
		//		try {
		state KeyRange shard = getShardKeyRange(data, req.begin);

		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesStream.AfterVersion");
		//.detail("ShardBegin", shard.begin).detail("ShardEnd", shard.end);
		//} catch (Error& e) { TraceEvent("WrongShardServer", data->thisServerID).detail("Begin",
		// req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("Shard",
		//"None").detail("In", "getKeyValues>getShardKeyRange"); throw e; }

		if (!selectorInRange(req.end, shard) && !(req.end.isFirstGreaterOrEqual() && req.end.getKey() == shard.end)) {
			//			TraceEvent("WrongShardServer1", data->thisServerID).detail("Begin",
			// req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("ShardBegin",
			// shard.begin).detail("ShardEnd", shard.end).detail("In", "getKeyValues>checkShardExtents");
			throw wrong_shard_server();
		}

		state int offset1 = 0;
		state int offset2;
		state Future<Key> fBegin = req.begin.isFirstGreaterOrEqual()
		                               ? Future<Key>(req.begin.getKey())
		                               : findKey(data, req.begin, version, shard, &offset1, span.context, type);
		state Future<Key> fEnd = req.end.isFirstGreaterOrEqual()
		                             ? Future<Key>(req.end.getKey())
		                             : findKey(data, req.end, version, shard, &offset2, span.context, type);
		state Key begin = wait(fBegin);
		state Key end = wait(fEnd);
		if (req.debugID.present())
			g_traceBatch.addEvent(
			    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesStream.AfterKeys");
		//.detail("Off1",offset1).detail("Off2",offset2).detail("ReqBegin",req.begin.getKey()).detail("ReqEnd",req.end.getKey());

		// Offsets of zero indicate begin/end keys in this shard, which obviously means we can answer the query
		// An end offset of 1 is also OK because the end key is exclusive, so if the first key of the next shard is the
		// end the last actual key returned must be from this shard. A begin offset of 1 is also OK because then either
		// begin is past end or equal to end (so the result is definitely empty)
		if ((offset1 && offset1 != 1) || (offset2 && offset2 != 1)) {
			TEST(true); // wrong_shard_server due to offset in rangeStream
			// We could detect when offset1 takes us off the beginning of the database or offset2 takes us off the end,
			// and return a clipped range rather than an error (since that is what the NativeAPI.getRange will do anyway
			// via its "slow path"), but we would have to add some flags to the response to encode whether we went off
			// the beginning and the end, since it needs that information.
			//TraceEvent("WrongShardServer2", data->thisServerID).detail("Begin", req.begin.toString()).detail("End", req.end.toString()).detail("Version", version).detail("ShardBegin", shard.begin).detail("ShardEnd", shard.end).detail("In", "getKeyValues>checkOffsets").detail("BeginKey", begin).detail("EndKey", end).detail("BeginOffset", offset1).detail("EndOffset", offset2);
			throw wrong_shard_server();
		}

		if (begin >= end) {
			if (req.debugID.present())
				g_traceBatch.addEvent(
				    "TransactionDebug", req.debugID.get().first(), "storageserver.getKeyValuesStream.Send");
			//.detail("Begin",begin).detail("End",end);

			GetKeyValuesStreamReply none;
			none.version = version;
			none.more = false;

			data->checkChangeCounter(changeCounter,
			                         KeyRangeRef(std::min<KeyRef>(req.begin.getKey(), req.end.getKey()),
			                                     std::max<KeyRef>(req.begin.getKey(), req.end.getKey())));
			req.reply.send(none);
			req.reply.sendError(end_of_stream());
		} else {
			loop {
				wait(req.reply.onReady());

				if (version < data->oldestVersion.get()) {
					throw transaction_too_old();
				}

				state int byteLimit = (BUGGIFY && g_simulator.tssMode == ISimulator::TSSMode::Disabled)
				                          ? 1
				                          : CLIENT_KNOBS->REPLY_BYTE_LIMIT;
				GetKeyValuesReply _r =
				    wait(readRange(data, version, KeyRangeRef(begin, end), req.limit, &byteLimit, span.context, type));
				GetKeyValuesStreamReply r(_r);

				if (req.debugID.present())
					g_traceBatch.addEvent("TransactionDebug",
					                      req.debugID.get().first(),
					                      "storageserver.getKeyValuesStream.AfterReadRange");
				//.detail("Begin",begin).detail("End",end).detail("SizeOf",r.data.size());
				data->checkChangeCounter(
				    changeCounter,
				    KeyRangeRef(std::min<KeyRef>(begin, std::min<KeyRef>(req.begin.getKey(), req.end.getKey())),
				                std::max<KeyRef>(end, std::max<KeyRef>(req.begin.getKey(), req.end.getKey()))));
				if (EXPENSIVE_VALIDATION) {
					for (int i = 0; i < r.data.size(); i++)
						ASSERT(r.data[i].key >= begin && r.data[i].key < end);
					ASSERT(r.data.size() <= std::abs(req.limit));
				}

				/*for( int i = 0; i < r.data.size(); i++ ) {
				    StorageMetrics m;
				    m.bytesPerKSecond = r.data[i].expectedSize();
				    m.iosPerKSecond = 1; //FIXME: this should be 1/r.data.size(), but we cannot do that because it is an
				int data->metrics.notify(r.data[i].key, m);
				}*/

				// For performance concerns, the cost of a range read is billed to the start key and end key of the
				// range.
				int64_t totalByteSize = 0;
				for (int i = 0; i < r.data.size(); i++) {
					totalByteSize += r.data[i].expectedSize();
				}
				if (totalByteSize > 0 && SERVER_KNOBS->READ_SAMPLING_ENABLED) {
					int64_t bytesReadPerKSecond = std::max(totalByteSize, SERVER_KNOBS->EMPTY_READ_PENALTY) / 2;
					data->metrics.notifyBytesReadPerKSecond(r.data[0].key, bytesReadPerKSecond);
					data->metrics.notifyBytesReadPerKSecond(r.data[r.data.size() - 1].key, bytesReadPerKSecond);
				}

				req.reply.send(r);

				data->counters.rowsQueried += r.data.size();
				if (r.data.size() == 0) {
					++data->counters.emptyQueries;
				}
				if (!r.more) {
					req.reply.sendError(end_of_stream());
					break;
				}
				ASSERT(r.data.size());

				if (req.limit >= 0) {
					begin = keyAfter(r.data.back().key);
				} else {
					end = r.data.back().key;
				}

				if (SERVER_KNOBS->FETCH_KEYS_LOWER_PRIORITY && req.isFetchKeys) {
					wait(delay(0, TaskPriority::FetchKeys));
				} else {
					wait(delay(0, TaskPriority::DefaultEndpoint));
				}

				data->transactionTagCounter.addRequest(req.tags, resultSize);
			}
		}
	} catch (Error& e) {
		if (e.code() != error_code_operation_obsolete) {
			if (!canReplyWith(e))
				throw;
			req.reply.sendError(e);
		}
	}

	data->transactionTagCounter.addRequest(req.tags, resultSize);
	++data->counters.finishedQueries;
	--data->readQueueSizeMetric;

	return Void();
}

ACTOR Future<Void> getKeyQ(StorageServer* data, GetKeyRequest req) {
	state Span span("SS:getKey"_loc, { req.spanContext });
	state int64_t resultSize = 0;
	getCurrentLineage()->modify(&TransactionLineage::txID) = req.spanContext.first();

	++data->counters.getKeyQueries;
	++data->counters.allQueries;
	++data->readQueueSizeMetric;
	data->maxQueryQueue = std::max<int>(
	    data->maxQueryQueue, data->counters.allQueries.getValue() - data->counters.finishedQueries.getValue());

	// Active load balancing runs at a very high priority (to obtain accurate queue lengths)
	// so we need to downgrade here
	wait(data->getQueryDelay());

	try {
		state Version version = wait(waitForVersion(data, req.version, req.spanContext));

		state uint64_t changeCounter = data->shardChangeCounter;
		state KeyRange shard = getShardKeyRange(data, req.sel);

		state int offset;
		Key k =
		    wait(findKey(data, req.sel, version, shard, &offset, req.spanContext, IKeyValueStore::ReadType::NORMAL));

		data->checkChangeCounter(
		    changeCounter, KeyRangeRef(std::min<KeyRef>(req.sel.getKey(), k), std::max<KeyRef>(req.sel.getKey(), k)));

		KeySelector updated;
		if (offset < 0)
			updated = firstGreaterOrEqual(k) +
			          offset; // first thing on this shard OR (large offset case) smallest key retrieved in range read
		else if (offset > 0)
			updated =
			    firstGreaterOrEqual(k) + offset -
			    1; // first thing on next shard OR (large offset case) keyAfter largest key retrieved in range read
		else
			updated = KeySelectorRef(k, true, 0); // found

		resultSize = k.size();
		data->counters.bytesQueried += resultSize;
		++data->counters.rowsQueried;

		// Check if the desired key might be cached
		auto cached = data->cachedRangeMap[k];
		// if (cached)
		//	TraceEvent(SevDebug, "SSGetKeyCached").detail("Key", k).detail("Begin",
		// shard.begin.printable()).detail("End", shard.end.printable());

		GetKeyReply reply(updated, cached);
		reply.penalty = data->getPenalty();

		req.reply.send(reply);
	} catch (Error& e) {
		// if (e.code() == error_code_wrong_shard_server) TraceEvent("WrongShardServer").detail("In","getKey");
		if (!canReplyWith(e))
			throw;
		data->sendErrorWithPenalty(req.reply, e, data->getPenalty());
	}

	// SOMEDAY: The size reported here is an undercount of the bytes read due to the fact that we have to scan for the
	// key It would be more accurate to count all the read bytes, but it's not critical because this function is only
	// used if read-your-writes is disabled
	data->transactionTagCounter.addRequest(req.tags, resultSize);

	++data->counters.finishedQueries;
	--data->readQueueSizeMetric;

	double duration = g_network->timer() - req.requestTime();
	data->counters.readLatencySample.addMeasurement(duration);
	if (data->latencyBandConfig.present()) {
		int maxReadBytes =
		    data->latencyBandConfig.get().readConfig.maxReadBytes.orDefault(std::numeric_limits<int>::max());
		int maxSelectorOffset =
		    data->latencyBandConfig.get().readConfig.maxKeySelectorOffset.orDefault(std::numeric_limits<int>::max());
		data->counters.readLatencyBands.addMeasurement(
		    duration, resultSize > maxReadBytes || abs(req.sel.offset) > maxSelectorOffset);
	}

	return Void();
}

void getQueuingMetrics(StorageServer* self, StorageQueuingMetricsRequest const& req) {
	StorageQueuingMetricsReply reply;
	reply.localTime = now();
	reply.instanceID = self->instanceID;
	reply.bytesInput = self->counters.bytesInput.getValue();
	reply.bytesDurable = self->counters.bytesDurable.getValue();

	reply.storageBytes = self->storage.getStorageBytes();
	reply.localRateLimit = self->currentRate();

	reply.version = self->version.get();
	reply.cpuUsage = self->cpuUsage;
	reply.diskUsage = self->diskUsage;
	reply.durableVersion = self->durableVersion.get();

	Optional<StorageServer::TransactionTagCounter::TagInfo> busiestTag = self->transactionTagCounter.getBusiestTag();
	reply.busiestTag = busiestTag.map<TransactionTag>(
	    [](StorageServer::TransactionTagCounter::TagInfo tagInfo) { return tagInfo.tag; });
	reply.busiestTagFractionalBusyness = busiestTag.present() ? busiestTag.get().fractionalBusyness : 0.0;
	reply.busiestTagRate = busiestTag.present() ? busiestTag.get().rate : 0.0;

	req.reply.send(reply);
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

/////////////////////////// Updates ////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region Updates
#endif

ACTOR Future<Void> doEagerReads(StorageServer* data, UpdateEagerReadInfo* eager) {
	eager->finishKeyBegin();

	if (SERVER_KNOBS->ENABLE_CLEAR_RANGE_EAGER_READS) {
		std::vector<Future<Key>> keyEnd(eager->keyBegin.size());
		for (int i = 0; i < keyEnd.size(); i++)
			keyEnd[i] = data->storage.readNextKeyInclusive(eager->keyBegin[i], IKeyValueStore::ReadType::EAGER);

		state Future<std::vector<Key>> futureKeyEnds = getAll(keyEnd);
		state std::vector<Key> keyEndVal = wait(futureKeyEnds);
		eager->keyEnd = keyEndVal;
	}

	std::vector<Future<Optional<Value>>> value(eager->keys.size());
	for (int i = 0; i < value.size(); i++)
		value[i] =
		    data->storage.readValuePrefix(eager->keys[i].first, eager->keys[i].second, IKeyValueStore::ReadType::EAGER);

	state Future<std::vector<Optional<Value>>> futureValues = getAll(value);
	std::vector<Optional<Value>> optionalValues = wait(futureValues);
	eager->value = optionalValues;

	return Void();
}

bool changeDurableVersion(StorageServer* data, Version desiredDurableVersion) {
	// Remove entries from the latest version of data->versionedData that haven't changed since they were inserted
	//   before or at desiredDurableVersion, to maintain the invariants for versionedData.
	// Such entries remain in older versions of versionedData until they are forgotten, because it is expensive to dig
	// them out. We also remove everything up to and including newDurableVersion from mutationLog, and everything
	//   up to but excluding desiredDurableVersion from freeable
	// May return false if only part of the work has been done, in which case the caller must call again with the same
	// parameters

	auto& verData = data->mutableData();
	ASSERT(verData.getLatestVersion() == data->version.get() || verData.getLatestVersion() == data->version.get() + 1);

	Version nextDurableVersion = desiredDurableVersion;

	auto mlv = data->getMutationLog().begin();
	if (mlv != data->getMutationLog().end() && mlv->second.version <= desiredDurableVersion) {
		auto& v = mlv->second;
		nextDurableVersion = v.version;
		data->freeable[data->version.get()].dependsOn(v.arena());

		if (verData.getLatestVersion() <= data->version.get())
			verData.createNewVersion(data->version.get() + 1);

		int64_t bytesDurable = VERSION_OVERHEAD;
		for (const auto& m : v.mutations) {
			bytesDurable += mvccStorageBytes(m);
			auto i = verData.atLatest().find(m.param1);
			if (i) {
				ASSERT(i.key() == m.param1);
				ASSERT(i.insertVersion() >= nextDurableVersion);
				if (i.insertVersion() == nextDurableVersion)
					verData.erase(i);
			}
			if (m.type == MutationRef::SetValue) {
				// A set can split a clear, so there might be another entry immediately after this one that should also
				// be cleaned up
				i = verData.atLatest().upper_bound(m.param1);
				if (i) {
					ASSERT(i.insertVersion() >= nextDurableVersion);
					if (i.insertVersion() == nextDurableVersion)
						verData.erase(i);
				}
			}
		}
		data->counters.bytesDurable += bytesDurable;
	}

	if (EXPENSIVE_VALIDATION) {
		// Check that the above loop did its job
		auto view = data->data().atLatest();
		for (auto i = view.begin(); i != view.end(); ++i)
			ASSERT(i.insertVersion() > nextDurableVersion);
	}
	data->getMutableMutationLog().erase(data->getMutationLog().begin(),
	                                    data->getMutationLog().upper_bound(nextDurableVersion));
	data->freeable.erase(data->freeable.begin(), data->freeable.lower_bound(nextDurableVersion));

	Future<Void> checkFatalError = data->otherError.getFuture();
	data->durableVersion.set(nextDurableVersion);
	setDataDurableVersion(data->thisServerID, data->durableVersion.get());
	if (checkFatalError.isReady())
		checkFatalError.get();

	// TraceEvent("ForgotVersionsBefore", data->thisServerID).detail("Version", nextDurableVersion);
	validate(data);

	return nextDurableVersion == desiredDurableVersion;
}

Optional<MutationRef> clipMutation(MutationRef const& m, KeyRangeRef range) {
	if (isSingleKeyMutation((MutationRef::Type)m.type)) {
		if (range.contains(m.param1))
			return m;
	} else if (m.type == MutationRef::ClearRange) {
		KeyRangeRef i = range & KeyRangeRef(m.param1, m.param2);
		if (!i.empty())
			return MutationRef((MutationRef::Type)m.type, i.begin, i.end);
	} else
		ASSERT(false);
	return Optional<MutationRef>();
}

// Return true if the mutation need to be applied, otherwise (it's a CompareAndClear mutation and failed the comparison)
// false.
bool expandMutation(MutationRef& m,
                    StorageServer::VersionedData const& data,
                    UpdateEagerReadInfo* eager,
                    KeyRef eagerTrustedEnd,
                    Arena& ar) {
	// After this function call, m should be copied into an arena immediately (before modifying data, shards, or eager)
	if (m.type == MutationRef::ClearRange) {
		// Expand the clear
		const auto& d = data.atLatest();

		// If another clear overlaps the beginning of this one, engulf it
		auto i = d.lastLess(m.param1);
		if (i && i->isClearTo() && i->getEndKey() >= m.param1)
			m.param1 = i.key();

		// If another clear overlaps the end of this one, engulf it; otherwise expand
		i = d.lastLessOrEqual(m.param2);
		if (i && i->isClearTo() && i->getEndKey() >= m.param2) {
			m.param2 = i->getEndKey();
		} else if (SERVER_KNOBS->ENABLE_CLEAR_RANGE_EAGER_READS) {
			// Expand to the next set or clear (from storage or latestVersion), and if it
			// is a clear, engulf it as well
			i = d.lower_bound(m.param2);
			KeyRef endKeyAtStorageVersion =
			    m.param2 == eagerTrustedEnd ? eagerTrustedEnd : std::min(eager->getKeyEnd(m.param2), eagerTrustedEnd);
			if (!i || endKeyAtStorageVersion < i.key())
				m.param2 = endKeyAtStorageVersion;
			else if (i->isClearTo())
				m.param2 = i->getEndKey();
			else
				m.param2 = i.key();
		}
	} else if (m.type != MutationRef::SetValue && (m.type)) {

		Optional<StringRef> oldVal;
		auto it = data.atLatest().lastLessOrEqual(m.param1);
		if (it != data.atLatest().end() && it->isValue() && it.key() == m.param1)
			oldVal = it->getValue();
		else if (it != data.atLatest().end() && it->isClearTo() && it->getEndKey() > m.param1) {
			TEST(true); // Atomic op right after a clear.
		} else {
			Optional<Value>& oldThing = eager->getValue(m.param1);
			if (oldThing.present())
				oldVal = oldThing.get();
		}

		switch (m.type) {
		case MutationRef::AddValue:
			m.param2 = doLittleEndianAdd(oldVal, m.param2, ar);
			break;
		case MutationRef::And:
			m.param2 = doAnd(oldVal, m.param2, ar);
			break;
		case MutationRef::Or:
			m.param2 = doOr(oldVal, m.param2, ar);
			break;
		case MutationRef::Xor:
			m.param2 = doXor(oldVal, m.param2, ar);
			break;
		case MutationRef::AppendIfFits:
			m.param2 = doAppendIfFits(oldVal, m.param2, ar);
			break;
		case MutationRef::Max:
			m.param2 = doMax(oldVal, m.param2, ar);
			break;
		case MutationRef::Min:
			m.param2 = doMin(oldVal, m.param2, ar);
			break;
		case MutationRef::ByteMin:
			m.param2 = doByteMin(oldVal, m.param2, ar);
			break;
		case MutationRef::ByteMax:
			m.param2 = doByteMax(oldVal, m.param2, ar);
			break;
		case MutationRef::MinV2:
			m.param2 = doMinV2(oldVal, m.param2, ar);
			break;
		case MutationRef::AndV2:
			m.param2 = doAndV2(oldVal, m.param2, ar);
			break;
		case MutationRef::CompareAndClear:
			if (oldVal.present() && m.param2 == oldVal.get()) {
				m.type = MutationRef::ClearRange;
				m.param2 = keyAfter(m.param1, ar);
				return expandMutation(m, data, eager, eagerTrustedEnd, ar);
			}
			return false;
		}
		m.type = MutationRef::SetValue;
	}

	return true;
}

void applyMutation(StorageServer* self,
                   MutationRef const& m,
                   Arena& arena,
                   StorageServer::VersionedData& data,
                   Version version,
                   bool fromFetch) {
	// m is expected to be in arena already
	// Clear split keys are added to arena
	StorageMetrics metrics;
	metrics.bytesPerKSecond = mvccStorageBytes(m) / 2;
	metrics.iosPerKSecond = 1;
	self->metrics.notify(m.param1, metrics);

	if (m.type == MutationRef::SetValue) {
		// VersionedMap (data) is bookkeeping all empty ranges. If the key to be set is new, it is supposed to be in a
		// range what was empty. Break the empty range into halves.
		auto prev = data.atLatest().lastLessOrEqual(m.param1);
		if (prev && prev->isClearTo() && prev->getEndKey() > m.param1) {
			ASSERT(prev.key() <= m.param1);
			KeyRef end = prev->getEndKey();
			// the insert version of the previous clear is preserved for the "left half", because in
			// changeDurableVersion() the previous clear is still responsible for removing it insert() invalidates prev,
			// so prev.key() is not safe to pass to it by reference
			data.insert(KeyRef(prev.key()),
			            ValueOrClearToRef::clearTo(m.param1),
			            prev.insertVersion()); // overwritten by below insert if empty
			KeyRef nextKey = keyAfter(m.param1, arena);
			if (end != nextKey) {
				ASSERT(end > nextKey);
				// the insert version of the "right half" is not preserved, because in changeDurableVersion() this set
				// is responsible for removing it
				// FIXME: This copy is technically an asymptotic problem, definitely a waste of memory (copy of keyAfter
				// is a waste, but not asymptotic)
				data.insert(nextKey, ValueOrClearToRef::clearTo(KeyRef(arena, end)));
			}
		}
		data.insert(m.param1, ValueOrClearToRef::value(m.param2));
		self->watches.trigger(m.param1);

		if (!fromFetch) {
			for (auto& it : self->keyChangeFeed[m.param1]) {
				if (!it->stopped) {
					if (it->mutations.empty() || it->mutations.back().version != version) {
						it->mutations.push_back(MutationsAndVersionRef(version, self->knownCommittedVersion));
					}
					it->mutations.back().mutations.push_back_deep(it->mutations.back().arena(), m);
					self->currentChangeFeeds.insert(it->id);

					DEBUG_MUTATION("ChangeFeedWriteSet", version, m, self->thisServerID)
					    .detail("Range", it->range)
					    .detail("ChangeFeedID", it->id);
				}
			}
		}
	} else if (m.type == MutationRef::ClearRange) {
		data.erase(m.param1, m.param2);
		ASSERT(m.param2 > m.param1);
		ASSERT(!data.isClearContaining(data.atLatest(), m.param1));
		data.insert(m.param1, ValueOrClearToRef::clearTo(m.param2));
		self->watches.triggerRange(m.param1, m.param2);

		if (!fromFetch) {
			auto ranges = self->keyChangeFeed.intersectingRanges(KeyRangeRef(m.param1, m.param2));
			for (auto& r : ranges) {
				for (auto& it : r.value()) {
					if (!it->stopped) {
						if (it->mutations.empty() || it->mutations.back().version != version) {
							it->mutations.push_back(MutationsAndVersionRef(version, self->knownCommittedVersion));
						}
						it->mutations.back().mutations.push_back_deep(it->mutations.back().arena(), m);
						self->currentChangeFeeds.insert(it->id);
						DEBUG_MUTATION("ChangeFeedWriteClear", version, m, self->thisServerID)
						    .detail("Range", it->range)
						    .detail("ChangeFeedID", it->id);
					}
				}
			}
		}
	}
}

void removeDataRange(StorageServer* ss,
                     Standalone<VerUpdateRef>& mLV,
                     KeyRangeMap<Reference<ShardInfo>>& shards,
                     KeyRangeRef range) {
	// modify the latest version of data to remove all sets and trim all clears to exclude range.
	// Add a clear to mLV (mutationLog[data.getLatestVersion()]) that ensures all keys in range are removed from the
	// disk when this latest version becomes durable mLV is also modified if necessary to ensure that split clears can
	// be forgotten

	MutationRef clearRange(MutationRef::ClearRange, range.begin, range.end);
	clearRange = ss->addMutationToMutationLog(mLV, clearRange);

	auto& data = ss->mutableData();

	// Expand the range to the right to include other shards not in versionedData
	for (auto r = shards.rangeContaining(range.end); r != shards.ranges().end() && !r->value()->isInVersionedData();
	     ++r)
		range = KeyRangeRef(range.begin, r->end());

	auto endClear = data.atLatest().lastLess(range.end);
	if (endClear && endClear->isClearTo() && endClear->getEndKey() > range.end) {
		// This clear has been bumped up to insertVersion==data.getLatestVersion and needs a corresponding mutation log
		// entry to forget
		MutationRef m(MutationRef::ClearRange, range.end, endClear->getEndKey());
		m = ss->addMutationToMutationLog(mLV, m);
		data.insert(m.param1, ValueOrClearToRef::clearTo(m.param2));
	}

	auto beginClear = data.atLatest().lastLess(range.begin);
	if (beginClear && beginClear->isClearTo() && beginClear->getEndKey() > range.begin) {
		// We don't need any special mutationLog entry - because the begin key and insert version are unchanged the
		// original clear
		//   mutation works to forget this one - but we need range.begin in the right arena
		KeyRef rb(mLV.arena(), range.begin);
		// insert() invalidates beginClear, so beginClear.key() is not safe to pass to it by reference
		data.insert(KeyRef(beginClear.key()), ValueOrClearToRef::clearTo(rb), beginClear.insertVersion());
	}

	data.erase(range.begin, range.end);
}

void setAvailableStatus(StorageServer* self, KeyRangeRef keys, bool available);
void setAssignedStatus(StorageServer* self, KeyRangeRef keys, bool nowAssigned);

void coalesceShards(StorageServer* data, KeyRangeRef keys) {
	auto shardRanges = data->shards.intersectingRanges(keys);
	auto fullRange = data->shards.ranges();

	auto iter = shardRanges.begin();
	if (iter != fullRange.begin())
		--iter;
	auto iterEnd = shardRanges.end();
	if (iterEnd != fullRange.end())
		++iterEnd;

	bool lastReadable = false;
	bool lastNotAssigned = false;
	KeyRangeMap<Reference<ShardInfo>>::iterator lastRange;

	for (; iter != iterEnd; ++iter) {
		if (lastReadable && iter->value()->isReadable()) {
			KeyRange range = KeyRangeRef(lastRange->begin(), iter->end());
			data->addShard(ShardInfo::newReadWrite(range, data));
			iter = data->shards.rangeContaining(range.begin);
		} else if (lastNotAssigned && iter->value()->notAssigned()) {
			KeyRange range = KeyRangeRef(lastRange->begin(), iter->end());
			data->addShard(ShardInfo::newNotAssigned(range));
			iter = data->shards.rangeContaining(range.begin);
		}

		lastReadable = iter->value()->isReadable();
		lastNotAssigned = iter->value()->notAssigned();
		lastRange = iter;
	}
}

template <class T>
void addMutation(T& target, Version version, bool fromFetch, MutationRef const& mutation) {
	target.addMutation(version, fromFetch, mutation);
}

template <class T>
void addMutation(Reference<T>& target, Version version, bool fromFetch, MutationRef const& mutation) {
	addMutation(*target, version, fromFetch, mutation);
}

template <class T>
void splitMutations(StorageServer* data, KeyRangeMap<T>& map, VerUpdateRef const& update) {
	for (int i = 0; i < update.mutations.size(); i++) {
		splitMutation(data, map, update.mutations[i], update.version, update.version);
	}
}

template <class T>
void splitMutation(StorageServer* data, KeyRangeMap<T>& map, MutationRef const& m, Version ver, bool fromFetch) {
	if (isSingleKeyMutation((MutationRef::Type)m.type)) {
		if (!SHORT_CIRCUT_ACTUAL_STORAGE || !normalKeys.contains(m.param1))
			addMutation(map.rangeContaining(m.param1)->value(), ver, fromFetch, m);
	} else if (m.type == MutationRef::ClearRange) {
		KeyRangeRef mKeys(m.param1, m.param2);
		if (!SHORT_CIRCUT_ACTUAL_STORAGE || !normalKeys.contains(mKeys)) {
			auto r = map.intersectingRanges(mKeys);
			for (auto i = r.begin(); i != r.end(); ++i) {
				KeyRangeRef k = mKeys & i->range();
				addMutation(i->value(), ver, fromFetch, MutationRef((MutationRef::Type)m.type, k.begin, k.end));
			}
		}
	} else
		ASSERT(false); // Unknown mutation type in splitMutations
}

ACTOR Future<Void> logFetchKeysWarning(AddingShard* shard) {
	state double startTime = now();
	loop {
		state double waitSeconds = BUGGIFY ? 5.0 : 600.0;
		wait(delay(waitSeconds));

		const auto traceEventLevel =
		    waitSeconds > SERVER_KNOBS->FETCH_KEYS_TOO_LONG_TIME_CRITERIA ? SevWarnAlways : SevInfo;
		TraceEvent(traceEventLevel, "FetchKeysTooLong")
		    .detail("Duration", now() - startTime)
		    .detail("Phase", shard->phase)
		    .detail("Begin", shard->keys.begin.printable())
		    .detail("End", shard->keys.end.printable());
	}
}

class FetchKeysMetricReporter {
	const UID uid;
	const double startTime;
	int fetchedBytes;
	StorageServer::FetchKeysHistograms& histograms;
	StorageServer::CurrentRunningFetchKeys& currentRunning;
	Counter& bytesFetchedCounter;
	Counter& kvFetchedCounter;

public:
	FetchKeysMetricReporter(const UID& uid_,
	                        const double startTime_,
	                        const KeyRange& keyRange,
	                        StorageServer::FetchKeysHistograms& histograms_,
	                        StorageServer::CurrentRunningFetchKeys& currentRunning_,
	                        Counter& bytesFetchedCounter,
	                        Counter& kvFetchedCounter)
	  : uid(uid_), startTime(startTime_), fetchedBytes(0), histograms(histograms_), currentRunning(currentRunning_),
	    bytesFetchedCounter(bytesFetchedCounter), kvFetchedCounter(kvFetchedCounter) {

		currentRunning.recordStart(uid, keyRange);
	}

	void addFetchedBytes(const int bytes, const int kvCount) {
		fetchedBytes += bytes;
		bytesFetchedCounter += bytes;
		kvFetchedCounter += kvCount;
	}

	~FetchKeysMetricReporter() {
		double latency = now() - startTime;

		// If fetchKeys is *NOT* run, i.e. returning immediately, still report a record.
		if (latency == 0)
			latency = 1e6;

		const uint32_t bandwidth = fetchedBytes / latency;

		histograms.latency->sampleSeconds(latency);
		histograms.bytes->sample(fetchedBytes);
		histograms.bandwidth->sample(bandwidth);

		currentRunning.recordFinish(uid);
	}
};

ACTOR Future<Void> tryGetRange(PromiseStream<RangeResult> results, Transaction* tr, KeyRange keys) {
	state KeySelectorRef begin = firstGreaterOrEqual(keys.begin);
	state KeySelectorRef end = firstGreaterOrEqual(keys.end);

	try {
		loop {
			GetRangeLimits limits(GetRangeLimits::ROW_LIMIT_UNLIMITED, SERVER_KNOBS->FETCH_BLOCK_BYTES);
			limits.minRows = 0;
			state RangeResult rep = wait(tr->getRange(begin, end, limits, Snapshot::True));
			if (!rep.more) {
				rep.readThrough = keys.end;
			}
			results.send(rep);

			if (!rep.more) {
				results.sendError(end_of_stream());
				return Void();
			}

			if (rep.readThrough.present()) {
				begin = firstGreaterOrEqual(rep.readThrough.get());
			} else {
				begin = firstGreaterThan(rep.end()[-1].key);
			}
		}
	} catch (Error& e) {
		if (e.code() == error_code_actor_cancelled) {
			throw;
		}
		results.sendError(e);
		throw;
	}
}

#define PERSIST_PREFIX "\xff\xff"

// Immutable
static const KeyValueRef persistFormat(LiteralStringRef(PERSIST_PREFIX "Format"),
                                       LiteralStringRef("FoundationDB/StorageServer/1/4"));
static const KeyRangeRef persistFormatReadableRange(LiteralStringRef("FoundationDB/StorageServer/1/2"),
                                                    LiteralStringRef("FoundationDB/StorageServer/1/5"));
static const KeyRef persistID = LiteralStringRef(PERSIST_PREFIX "ID");
static const KeyRef persistTssPairID = LiteralStringRef(PERSIST_PREFIX "tssPairID");
static const KeyRef persistTssQuarantine = LiteralStringRef(PERSIST_PREFIX "tssQ");
static const KeyRef persistClusterIdKey = LiteralStringRef(PERSIST_PREFIX "clusterId");

// (Potentially) change with the durable version or when fetchKeys completes
static const KeyRef persistVersion = LiteralStringRef(PERSIST_PREFIX "Version");
static const KeyRangeRef persistShardAssignedKeys =
    KeyRangeRef(LiteralStringRef(PERSIST_PREFIX "ShardAssigned/"), LiteralStringRef(PERSIST_PREFIX "ShardAssigned0"));
static const KeyRangeRef persistShardAvailableKeys =
    KeyRangeRef(LiteralStringRef(PERSIST_PREFIX "ShardAvailable/"), LiteralStringRef(PERSIST_PREFIX "ShardAvailable0"));
static const KeyRangeRef persistByteSampleKeys =
    KeyRangeRef(LiteralStringRef(PERSIST_PREFIX "BS/"), LiteralStringRef(PERSIST_PREFIX "BS0"));
static const KeyRangeRef persistByteSampleSampleKeys =
    KeyRangeRef(LiteralStringRef(PERSIST_PREFIX "BS/" PERSIST_PREFIX "BS/"),
                LiteralStringRef(PERSIST_PREFIX "BS/" PERSIST_PREFIX "BS0"));
static const KeyRef persistLogProtocol = LiteralStringRef(PERSIST_PREFIX "LogProtocol");
static const KeyRef persistPrimaryLocality = LiteralStringRef(PERSIST_PREFIX "PrimaryLocality");
static const KeyRangeRef persistChangeFeedKeys =
    KeyRangeRef(LiteralStringRef(PERSIST_PREFIX "RF/"), LiteralStringRef(PERSIST_PREFIX "RF0"));
// data keys are unmangled (but never start with PERSIST_PREFIX because they are always in allKeys)

ACTOR Future<Void> fetchChangeFeedApplier(StorageServer* data,
                                          Reference<ChangeFeedInfo> changeFeedInfo,
                                          Key rangeId,
                                          KeyRange range,
                                          Version fetchVersion,
                                          bool existing) {
	state Reference<ChangeFeedData> feedResults = makeReference<ChangeFeedData>();
	state Future<Void> feed = data->cx->getChangeFeedStream(
	    feedResults, rangeId, 0, existing ? fetchVersion + 1 : data->version.get() + 1, range);

	if (!existing) {
		try {
			loop {
				Standalone<VectorRef<MutationsAndVersionRef>> res = waitNext(feedResults->mutations.getFuture());
				for (auto& it : res) {
					if (it.mutations.size()) {
						data->storage.writeKeyValue(
						    KeyValueRef(changeFeedDurableKey(rangeId, it.version),
						                changeFeedDurableValue(it.mutations, it.knownCommittedVersion)));
						changeFeedInfo->storageVersion = std::max(changeFeedInfo->durableVersion, it.version);
						changeFeedInfo->durableVersion = changeFeedInfo->storageVersion;
					}
				}
				wait(yield());
			}
		} catch (Error& e) {
			if (e.code() != error_code_end_of_stream) {
				throw;
			}
			return Void();
		}
	}

	state PromiseStream<Standalone<MutationsAndVersionRef>> localResults;

	// Add 2 to fetch version to make sure the local stream will have more versions in the stream than the remote stream
	// to avoid edge cases in the merge logic
	state Future<Void> localStream = localChangeFeedStream(data, localResults, rangeId, 0, fetchVersion + 2, range);
	state Standalone<MutationsAndVersionRef> localResult;

	Standalone<MutationsAndVersionRef> _localResult = waitNext(localResults.getFuture());
	localResult = _localResult;
	try {
		loop {
			state Standalone<VectorRef<MutationsAndVersionRef>> remoteResult =
			    waitNext(feedResults->mutations.getFuture());
			state int remoteLoc = 0;
			while (remoteLoc < remoteResult.size()) {
				if (remoteResult[remoteLoc].version < localResult.version) {
					if (remoteResult[remoteLoc].mutations.size()) {
						data->storage.writeKeyValue(
						    KeyValueRef(changeFeedDurableKey(rangeId, remoteResult[remoteLoc].version),
						                changeFeedDurableValue(remoteResult[remoteLoc].mutations,
						                                       remoteResult[remoteLoc].knownCommittedVersion)));
						changeFeedInfo->storageVersion =
						    std::max(changeFeedInfo->durableVersion, remoteResult[remoteLoc].version);
						changeFeedInfo->durableVersion = changeFeedInfo->storageVersion;
					}
					remoteLoc++;
				} else if (remoteResult[remoteLoc].version == localResult.version) {
					if (remoteResult[remoteLoc].mutations.size()) {
						ASSERT(localResult.mutations.size());
						remoteResult[remoteLoc].mutations.append(
						    remoteResult.arena(), localResult.mutations.begin(), localResult.mutations.size());
						data->storage.writeKeyValue(
						    KeyValueRef(changeFeedDurableKey(rangeId, remoteResult[remoteLoc].version),
						                changeFeedDurableValue(remoteResult[remoteLoc].mutations,
						                                       remoteResult[remoteLoc].knownCommittedVersion)));
						changeFeedInfo->storageVersion =
						    std::max(changeFeedInfo->durableVersion, remoteResult[remoteLoc].version);
						changeFeedInfo->durableVersion = changeFeedInfo->storageVersion;
					}
					remoteLoc++;
					Standalone<MutationsAndVersionRef> _localResult = waitNext(localResults.getFuture());
					localResult = _localResult;
				} else {
					Standalone<MutationsAndVersionRef> _localResult = waitNext(localResults.getFuture());
					localResult = _localResult;
				}
			}
			wait(yield());
		}
	} catch (Error& e) {
		if (e.code() != error_code_end_of_stream) {
			throw;
		}
	}
	return Void();
}

ACTOR Future<Void> fetchChangeFeed(StorageServer* data,
                                   Key rangeId,
                                   KeyRange range,
                                   bool stopped,
                                   Version fetchVersion) {
	state Reference<ChangeFeedInfo> changeFeedInfo;
	wait(delay(0)); // allow this actor to be cancelled by removals
	state bool existing = data->uidChangeFeed.count(rangeId);

	TraceEvent(SevDebug, "FetchChangeFeed", data->thisServerID)
	    .detail("RangeID", rangeId.printable())
	    .detail("Range", range.toString())
	    .detail("Existing", existing);

	if (!existing) {
		changeFeedInfo = Reference<ChangeFeedInfo>(new ChangeFeedInfo());
		changeFeedInfo->range = range;
		changeFeedInfo->id = rangeId;
		changeFeedInfo->stopped = stopped;
		data->uidChangeFeed[rangeId] = changeFeedInfo;
		auto rs = data->keyChangeFeed.modify(range);
		for (auto r = rs.begin(); r != rs.end(); ++r) {
			r->value().push_back(changeFeedInfo);
		}
		data->keyChangeFeed.coalesce(range.contents());
		auto& mLV = data->addVersionToMutationLog(data->data().getLatestVersion());
		data->addMutationToMutationLog(
		    mLV,
		    MutationRef(MutationRef::SetValue,
		                persistChangeFeedKeys.begin.toString() + rangeId.toString(),
		                changeFeedValue(range, invalidVersion, ChangeFeedStatus::CHANGE_FEED_CREATE)));
	} else {
		changeFeedInfo = data->uidChangeFeed[rangeId];
	}

	loop {
		try {
			wait(fetchChangeFeedApplier(data, changeFeedInfo, rangeId, range, fetchVersion, existing));
			return Void();
		} catch (Error& e) {
			if (e.code() != error_code_change_feed_not_registered) {
				throw;
			}
		}
		wait(delay(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY));
	}
}

ACTOR Future<Void> dispatchChangeFeeds(StorageServer* data, UID fetchKeysID, KeyRange keys, Version fetchVersion) {
	// find overlapping range feeds
	state std::map<Key, Future<Void>> feedFetches;
	state PromiseStream<Key> removals;
	data->changeFeedRemovals[fetchKeysID] = removals;
	try {
		state std::vector<OverlappingChangeFeedEntry> feeds =
		    wait(data->cx->getOverlappingChangeFeeds(keys, fetchVersion + 1));
		for (auto& feed : feeds) {
			feedFetches[feed.rangeId] = fetchChangeFeed(data, feed.rangeId, feed.range, feed.stopped, fetchVersion);
		}

		loop {
			Future<Void> nextFeed = Never();
			if (!removals.getFuture().isReady()) {
				bool done = true;
				while (!feedFetches.empty()) {
					if (feedFetches.begin()->second.isReady()) {
						feedFetches.erase(feedFetches.begin());
					} else {
						nextFeed = feedFetches.begin()->second;
						done = false;
						break;
					}
				}
				if (done) {
					data->changeFeedRemovals.erase(fetchKeysID);
					return Void();
				}
			}
			choose {
				when(Key remove = waitNext(removals.getFuture())) { feedFetches.erase(remove); }
				when(wait(nextFeed)) {}
			}
		}

	} catch (Error& e) {
		if (!data->shuttingDown) {
			data->changeFeedRemovals.erase(fetchKeysID);
		}
		throw;
	}
}

ACTOR Future<Void> fetchKeys(StorageServer* data, AddingShard* shard) {
	state const UID fetchKeysID = deterministicRandom()->randomUniqueID();
	state TraceInterval interval("FetchKeys");
	state KeyRange keys = shard->keys;
	state Future<Void> warningLogger = logFetchKeysWarning(shard);
	state const double startTime = now();
	state Version fetchVersion = invalidVersion;
	state FetchKeysMetricReporter metricReporter(fetchKeysID,
	                                             startTime,
	                                             keys,
	                                             data->fetchKeysHistograms,
	                                             data->currentRunningFetchKeys,
	                                             data->counters.bytesFetched,
	                                             data->counters.kvFetched);

	// delay(0) to force a return to the run loop before the work of fetchKeys is started.
	//  This allows adding->start() to be called inline with CSK.
	wait(data->coreStarted.getFuture() && delay(0));

	try {
		DEBUG_KEY_RANGE("fetchKeysBegin", data->version.get(), shard->keys, data->thisServerID);

		TraceEvent(SevDebug, interval.begin(), data->thisServerID)
		    .detail("KeyBegin", shard->keys.begin)
		    .detail("KeyEnd", shard->keys.end);

		validate(data);

		// Wait (if necessary) for the latest version at which any key in keys was previously available (+1) to be
		// durable
		auto navr = data->newestAvailableVersion.intersectingRanges(keys);
		Version lastAvailable = invalidVersion;
		for (auto r = navr.begin(); r != navr.end(); ++r) {
			ASSERT(r->value() != latestVersion);
			lastAvailable = std::max(lastAvailable, r->value());
		}
		auto ndvr = data->newestDirtyVersion.intersectingRanges(keys);
		for (auto r = ndvr.begin(); r != ndvr.end(); ++r)
			lastAvailable = std::max(lastAvailable, r->value());

		if (lastAvailable != invalidVersion && lastAvailable >= data->durableVersion.get()) {
			TEST(true); // FetchKeys waits for previous available version to be durable
			wait(data->durableVersion.whenAtLeast(lastAvailable + 1));
		}

		TraceEvent(SevDebug, "FetchKeysVersionSatisfied", data->thisServerID).detail("FKID", interval.pairID);

		wait(data->fetchKeysParallelismLock.take(TaskPriority::DefaultYield));
		state FlowLock::Releaser holdingFKPL(data->fetchKeysParallelismLock);

		state double executeStart = now();
		++data->counters.fetchWaitingCount;
		data->counters.fetchWaitingMS += 1000 * (executeStart - startTime);

		// Fetch keys gets called while the update actor is processing mutations. data->version will not be updated
		// until all mutations for a version have been processed. We need to take the durableVersionLock to ensure
		// data->version is greater than the version of the mutation which caused the fetch to be initiated.
		wait(data->durableVersionLock.take());

		shard->phase = AddingShard::Fetching;

		data->durableVersionLock.release();

		wait(delay(0));

		// Get the history
		state int debug_getRangeRetries = 0;
		state int debug_nextRetryToLog = 1;

		// FIXME: The client cache does not notice when servers are added to a team. To read from a local storage server
		// we must refresh the cache manually.
		data->cx->invalidateCache(keys);

		loop {
			state Transaction tr(data->cx);
			fetchVersion = data->version.get();

			TraceEvent(SevDebug, "FetchKeysUnblocked", data->thisServerID)
			    .detail("FKID", interval.pairID)
			    .detail("Version", fetchVersion);

			while (!shard->updates.empty() && shard->updates[0].version <= fetchVersion)
				shard->updates.pop_front();
			tr.setVersion(fetchVersion);
			tr.info.taskID = TaskPriority::FetchKeys;
			state PromiseStream<RangeResult> results;
			state Future<Void> hold = SERVER_KNOBS->FETCH_USING_STREAMING
			                              ? tr.getRangeStream(results, keys, GetRangeLimits(), Snapshot::True)
			                              : tryGetRange(results, &tr, keys);
			state Key nfk = keys.begin;

			try {
				loop {
					TEST(true); // Fetching keys for transferred shard
					while (data->fetchKeysBudgetUsed.get()) {
						wait(data->fetchKeysBudgetUsed.onChange());
					}
					state RangeResult this_block = waitNext(results.getFuture());

					state int expectedBlockSize =
					    (int)this_block.expectedSize() + (8 - (int)sizeof(KeyValueRef)) * this_block.size();

					TraceEvent(SevDebug, "FetchKeysBlock", data->thisServerID)
					    .detail("FKID", interval.pairID)
					    .detail("BlockRows", this_block.size())
					    .detail("BlockBytes", expectedBlockSize)
					    .detail("KeyBegin", keys.begin)
					    .detail("KeyEnd", keys.end)
					    .detail("Last", this_block.size() ? this_block.end()[-1].key : std::string())
					    .detail("Version", fetchVersion)
					    .detail("More", this_block.more);

					DEBUG_KEY_RANGE("fetchRange", fetchVersion, keys, data->thisServerID);
					if (MUTATION_TRACKING_ENABLED) {
						for (auto k = this_block.begin(); k != this_block.end(); ++k) {
							DEBUG_MUTATION("fetch",
							               fetchVersion,
							               MutationRef(MutationRef::SetValue, k->key, k->value),
							               data->thisServerID);
						}
					}

					metricReporter.addFetchedBytes(expectedBlockSize, this_block.size());

					// Write this_block to storage
					state KeyValueRef* kvItr = this_block.begin();
					for (; kvItr != this_block.end(); ++kvItr) {
						data->storage.writeKeyValue(*kvItr);
						wait(yield());
					}

					kvItr = this_block.begin();
					for (; kvItr != this_block.end(); ++kvItr) {
						data->byteSampleApplySet(*kvItr, invalidVersion);
						wait(yield());
					}

					ASSERT(this_block.readThrough.present() || this_block.size());
					nfk = this_block.readThrough.present() ? this_block.readThrough.get()
					                                       : keyAfter(this_block.end()[-1].key);
					this_block = RangeResult();

					data->fetchKeysBytesBudget -= expectedBlockSize;
					if (data->fetchKeysBytesBudget <= 0) {
						data->fetchKeysBudgetUsed.set(true);
					}
				}
			} catch (Error& e) {
				if (e.code() != error_code_end_of_stream && e.code() != error_code_connection_failed &&
				    e.code() != error_code_transaction_too_old && e.code() != error_code_future_version &&
				    e.code() != error_code_process_behind && e.code() != error_code_server_overloaded) {
					throw;
				}
				if (nfk == keys.begin) {
					TraceEvent("FKBlockFail", data->thisServerID)
					    .error(e, true)
					    .suppressFor(1.0)
					    .detail("FKID", interval.pairID);

					// FIXME: remove when we no longer support upgrades from 5.X
					if (debug_getRangeRetries >= 100) {
						data->cx->enableLocalityLoadBalance = EnableLocalityLoadBalance::False;
						TraceEvent(SevWarnAlways, "FKDisableLB").detail("FKID", fetchKeysID);
					}

					debug_getRangeRetries++;
					if (debug_nextRetryToLog == debug_getRangeRetries) {
						debug_nextRetryToLog += std::min(debug_nextRetryToLog, 1024);
						TraceEvent(SevWarn, "FetchPast", data->thisServerID)
						    .detail("TotalAttempts", debug_getRangeRetries)
						    .detail("FKID", interval.pairID)
						    .detail("N", fetchVersion)
						    .detail("E", data->version.get());
					}
					wait(delayJittered(FLOW_KNOBS->PREVENT_FAST_SPIN_DELAY));
					continue;
				}
				if (nfk < keys.end) {
					std::deque<Standalone<VerUpdateRef>> updatesToSplit = std::move(shard->updates);

					// This actor finishes committing the keys [keys.begin,nfk) that we already fetched.
					// The remaining unfetched keys [nfk,keys.end) will become a separate AddingShard with its own
					// fetchKeys.
					shard->server->addShard(ShardInfo::addingSplitLeft(KeyRangeRef(keys.begin, nfk), shard));
					shard->server->addShard(ShardInfo::newAdding(data, KeyRangeRef(nfk, keys.end)));
					shard = data->shards.rangeContaining(keys.begin).value()->adding.get();
					warningLogger = logFetchKeysWarning(shard);
					AddingShard* otherShard = data->shards.rangeContaining(nfk).value()->adding.get();
					keys = shard->keys;

					// Split our prior updates.  The ones that apply to our new, restricted key range will go back into
					// shard->updates, and the ones delivered to the new shard will be discarded because it is in
					// WaitPrevious phase (hasn't chosen a fetchVersion yet). What we are doing here is expensive and
					// could get more expensive if we started having many more blocks per shard. May need optimization
					// in the future.
					std::deque<Standalone<VerUpdateRef>>::iterator u = updatesToSplit.begin();
					for (; u != updatesToSplit.end(); ++u) {
						splitMutations(data, data->shards, *u);
					}

					TEST(true); // fetchkeys has more
					TEST(shard->updates.size()); // Shard has updates
					ASSERT(otherShard->updates.empty());
				}
				break;
			}
		}

		// FIXME: remove when we no longer support upgrades from 5.X
		data->cx->enableLocalityLoadBalance = EnableLocalityLoadBalance::True;
		TraceEvent(SevWarnAlways, "FKReenableLB").detail("FKID", fetchKeysID);

		// We have completed the fetch and write of the data, now we wait for MVCC window to pass.
		//  As we have finished this work, we will allow more work to start...
		shard->fetchComplete.send(Void());

		TraceEvent(SevDebug, "FKBeforeFinalCommit", data->thisServerID)
		    .detail("FKID", interval.pairID)
		    .detail("SV", data->storageVersion())
		    .detail("DV", data->durableVersion.get());
		// Directly commit()ing the IKVS would interfere with updateStorage, possibly resulting in an incomplete version
		// being recovered. Instead we wait for the updateStorage loop to commit something (and consequently also what
		// we have written)

		state Future<Void> fetchDurable = data->durableVersion.whenAtLeast(data->storageVersion() + 1);
		wait(dispatchChangeFeeds(data, fetchKeysID, keys, fetchVersion));

		holdingFKPL.release();
		wait(fetchDurable);

		TraceEvent(SevDebug, "FKAfterFinalCommit", data->thisServerID)
		    .detail("FKID", interval.pairID)
		    .detail("SV", data->storageVersion())
		    .detail("DV", data->durableVersion.get());

		// Wait to run during update(), after a new batch of versions is received from the tlog but before eager reads
		// take place.
		Promise<FetchInjectionInfo*> p;
		data->readyFetchKeys.push_back(p);

		// After we add to the promise readyFetchKeys, update() would provide a pointer to FetchInjectionInfo that we
		// can put mutation in.
		FetchInjectionInfo* batch = wait(p.getFuture());
		TraceEvent(SevDebug, "FKUpdateBatch", data->thisServerID).detail("FKID", interval.pairID);

		shard->phase = AddingShard::Waiting;

		// Choose a transferredVersion.  This choice and timing ensure that
		//   * The transferredVersion can be mutated in versionedData
		//   * The transferredVersion isn't yet committed to storage (so we can write the availability status change)
		//   * The transferredVersion is <= the version of any of the updates in batch, and if there is an equal version
		//     its mutations haven't been processed yet
		shard->transferredVersion = data->version.get() + 1;
		// shard->transferredVersion = batch->changes[0].version;  //< FIXME: This obeys the documented properties, and
		// seems "safer" because it never introduces extra versions into the data structure, but violates some ASSERTs
		// currently
		data->mutableData().createNewVersion(shard->transferredVersion);
		ASSERT(shard->transferredVersion > data->storageVersion());
		ASSERT(shard->transferredVersion == data->data().getLatestVersion());

		TraceEvent(SevDebug, "FetchKeysHaveData", data->thisServerID)
		    .detail("FKID", interval.pairID)
		    .detail("Version", shard->transferredVersion)
		    .detail("StorageVersion", data->storageVersion());
		validate(data);

		// Put the updates that were collected during the FinalCommit phase into the batch at the transferredVersion.
		// Eager reads will be done for them by update(), and the mutations will come back through
		// AddingShard::addMutations and be applied to versionedMap and mutationLog as normal. The lie about their
		// version is acceptable because this shard will never be read at versions < transferredVersion

		for (auto i = shard->updates.begin(); i != shard->updates.end(); ++i) {
			i->version = shard->transferredVersion;
			batch->arena.dependsOn(i->arena());
		}

		int startSize = batch->changes.size();
		TEST(startSize); // Adding fetch data to a batch which already has changes
		batch->changes.resize(batch->changes.size() + shard->updates.size());

		// FIXME: pass the deque back rather than copy the data
		std::copy(shard->updates.begin(), shard->updates.end(), batch->changes.begin() + startSize);
		Version checkv = shard->transferredVersion;

		for (auto b = batch->changes.begin() + startSize; b != batch->changes.end(); ++b) {
			ASSERT(b->version >= checkv);
			checkv = b->version;
			if (MUTATION_TRACKING_ENABLED) {
				for (auto& m : b->mutations) {
					DEBUG_MUTATION("fetchKeysFinalCommitInject", batch->changes[0].version, m, data->thisServerID);
				}
			}
		}

		shard->updates.clear();

		setAvailableStatus(data,
		                   keys,
		                   true); // keys will be available when getLatestVersion()==transferredVersion is durable
		// Persist shard here.

		// Note that since it receives a pointer to FetchInjectionInfo, the thread does not leave this actor until this
		// point.

		// Wait for the transferredVersion (and therefore the shard data) to be committed and durable.
		wait(data->durableVersion.whenAtLeast(shard->transferredVersion));

		ASSERT(data->shards[shard->keys.begin]->assigned() &&
		       data->shards[shard->keys.begin]->keys ==
		           shard->keys); // We aren't changing whether the shard is assigned
		data->newestAvailableVersion.insert(shard->keys, latestVersion);
		shard->readWrite.send(Void());
		data->addShard(ShardInfo::newReadWrite(shard->keys, data)); // invalidates shard!
		coalesceShards(data, keys);

		validate(data);

		++data->counters.fetchExecutingCount;
		data->counters.fetchExecutingMS += 1000 * (now() - executeStart);

		TraceEvent(SevDebug, interval.end(), data->thisServerID);
	} catch (Error& e) {
		TraceEvent(SevDebug, interval.end(), data->thisServerID).error(e, true).detail("Version", data->version.get());

		if (e.code() == error_code_actor_cancelled && !data->shuttingDown && shard->phase >= AddingShard::Fetching) {
			if (shard->phase < AddingShard::Waiting) {
				data->storage.clearRange(keys); // Should be replaced by drop shard.
				data->byteSampleApplyClear(keys, invalidVersion);
			} else {
				ASSERT(data->data().getLatestVersion() > data->version.get());
				removeDataRange(
				    data, data->addVersionToMutationLog(data->data().getLatestVersion()), data->shards, keys);
				setAvailableStatus(data, keys, false);
				// Drop shard metadata here.

				// Prevent another, overlapping fetchKeys from entering the Fetching phase until
				// data->data().getLatestVersion() is durable
				data->newestDirtyVersion.insert(keys, data->data().getLatestVersion());
			}
		}

		TraceEvent(SevError, "FetchKeysError", data->thisServerID)
		    .error(e)
		    .detail("Elapsed", now() - startTime)
		    .detail("KeyBegin", keys.begin)
		    .detail("KeyEnd", keys.end);
		if (e.code() != error_code_actor_cancelled)
			data->otherError.sendError(e); // Kill the storage server.  Are there any recoverable errors?
		throw; // goes nowhere
	}

	return Void();
};

AddingShard::AddingShard(StorageServer* server, KeyRangeRef const& keys)
  : keys(keys), server(server), transferredVersion(invalidVersion), phase(WaitPrevious) {
	fetchClient = fetchKeys(server, this);
}

void AddingShard::addMutation(Version version, bool fromFetch, MutationRef const& mutation) {
	if (mutation.type == mutation.ClearRange) {
		ASSERT(keys.begin <= mutation.param1 && mutation.param2 <= keys.end);
	} else if (isSingleKeyMutation((MutationRef::Type)mutation.type)) {
		ASSERT(keys.contains(mutation.param1));
	}

	if (phase == WaitPrevious) {
		// Updates can be discarded
	} else if (phase == Fetching) {
		// Save incoming mutations (See the comments of member variable `updates`).

		// Create a new VerUpdateRef in updates queue if it is a new version.
		if (!updates.size() || version > updates.end()[-1].version) {
			VerUpdateRef v;
			v.version = version;
			v.isPrivateData = false;
			updates.push_back(v);
		} else {
			ASSERT(version == updates.end()[-1].version);
		}
		// Add the mutation to the version.
		updates.back().mutations.push_back_deep(updates.back().arena(), mutation);
		if (!fromFetch) {
			if (mutation.type == MutationRef::SetValue) {
				for (auto& it : server->keyChangeFeed[mutation.param1]) {
					if (!it->stopped) {
						if (it->mutations.empty() || it->mutations.back().version != version) {
							it->mutations.push_back(MutationsAndVersionRef(version, server->knownCommittedVersion));
						}
						it->mutations.back().mutations.push_back_deep(it->mutations.back().arena(), mutation);
						server->currentChangeFeeds.insert(it->id);
					}
				}
			} else if (mutation.type == MutationRef::ClearRange) {
				auto ranges = server->keyChangeFeed.intersectingRanges(KeyRangeRef(mutation.param1, mutation.param2));
				for (auto& r : ranges) {
					for (auto& it : r.value()) {
						if (!it->stopped) {
							if (it->mutations.empty() || it->mutations.back().version != version) {
								it->mutations.push_back(MutationsAndVersionRef(version, server->knownCommittedVersion));
							}
							it->mutations.back().mutations.push_back_deep(it->mutations.back().arena(), mutation);
							server->currentChangeFeeds.insert(it->id);
						}
					}
				}
			}
		}
	} else if (phase == Waiting) {
		server->addMutation(version, fromFetch, mutation, keys, server->updateEagerReads);
	} else
		ASSERT(false);
}

void ShardInfo::addMutation(Version version, bool fromFetch, MutationRef const& mutation) {
	ASSERT((void*)this);
	ASSERT(keys.contains(mutation.param1));
	if (adding)
		adding->addMutation(version, fromFetch, mutation);
	else if (readWrite)
		readWrite->addMutation(version, fromFetch, mutation, this->keys, readWrite->updateEagerReads);
	else if (mutation.type != MutationRef::ClearRange) {
		TraceEvent(SevError, "DeliveredToNotAssigned").detail("Version", version).detail("Mutation", mutation);
		ASSERT(false); // Mutation delivered to notAssigned shard!
	}
}

enum ChangeServerKeysContext { CSK_UPDATE, CSK_RESTORE, CSK_ASSIGN_EMPTY };
const char* changeServerKeysContextName[] = { "Update", "Restore" };

void changeServerKeys(StorageServer* data,
                      const KeyRangeRef& keys,
                      bool nowAssigned,
                      Version version,
                      ChangeServerKeysContext context) {
	ASSERT(!keys.empty());

	// TraceEvent("ChangeServerKeys", data->thisServerID)
	//     .detail("KeyBegin", keys.begin)
	//     .detail("KeyEnd", keys.end)
	//     .detail("NowAssigned", nowAssigned)
	//     .detail("Version", version)
	//     .detail("Context", changeServerKeysContextName[(int)context]);
	validate(data);

	// TODO(alexmiller): Figure out how to selectively enable spammy data distribution events.
	DEBUG_KEY_RANGE(nowAssigned ? "KeysAssigned" : "KeysUnassigned", version, keys, data->thisServerID);

	bool isDifferent = false;
	auto existingShards = data->shards.intersectingRanges(keys);
	for (auto it = existingShards.begin(); it != existingShards.end(); ++it) {
		if (nowAssigned != it->value()->assigned()) {
			isDifferent = true;
			TraceEvent("CSKRangeDifferent", data->thisServerID)
			    .detail("KeyBegin", it->range().begin)
			    .detail("KeyEnd", it->range().end);
			break;
		}
	}
	if (!isDifferent) {
		// TraceEvent("CSKShortCircuit", data->thisServerID).detail("KeyBegin", keys.begin).detail("KeyEnd", keys.end);
		return;
	}

	// Save a backup of the ShardInfo references before we start messing with shards, in order to defer fetchKeys
	// cancellation (and its potential call to removeDataRange()) until shards is again valid
	std::vector<Reference<ShardInfo>> oldShards;
	auto os = data->shards.intersectingRanges(keys);
	for (auto r = os.begin(); r != os.end(); ++r)
		oldShards.push_back(r->value());

	// As addShard (called below)'s documentation requires, reinitialize any overlapping range(s)
	auto ranges = data->shards.getAffectedRangesAfterInsertion(
	    keys, Reference<ShardInfo>()); // null reference indicates the range being changed
	for (int i = 0; i < ranges.size(); i++) {
		if (!ranges[i].value) {
			ASSERT((KeyRangeRef&)ranges[i] == keys); // there shouldn't be any nulls except for the range being inserted
		} else if (ranges[i].value->notAssigned())
			data->addShard(ShardInfo::newNotAssigned(ranges[i]));
		else if (ranges[i].value->isReadable())
			data->addShard(ShardInfo::newReadWrite(ranges[i], data));
		else {
			ASSERT(ranges[i].value->adding);
			data->addShard(ShardInfo::newAdding(data, ranges[i]));
			TEST(true); // ChangeServerKeys reFetchKeys
		}
	}

	// Shard state depends on nowAssigned and whether the data is available (actually assigned in memory or on the disk)
	// up to the given version.  The latter depends on data->newestAvailableVersion, so loop over the ranges of that.
	// SOMEDAY: Could this just use shards?  Then we could explicitly do the removeDataRange here when an
	// adding/transferred shard is cancelled
	auto vr = data->newestAvailableVersion.intersectingRanges(keys);
	std::vector<std::pair<KeyRange, Version>> changeNewestAvailable;
	std::vector<KeyRange> removeRanges;
	std::vector<KeyRange> newEmptyRanges;
	for (auto r = vr.begin(); r != vr.end(); ++r) {
		KeyRangeRef range = keys & r->range();
		bool dataAvailable = r->value() == latestVersion || r->value() >= version;
		// TraceEvent("CSKRange", data->thisServerID)
		//     .detail("KeyBegin", range.begin)
		//     .detail("KeyEnd", range.end)
		//     .detail("Available", dataAvailable)
		//     .detail("NowAssigned", nowAssigned)
		//     .detail("NewestAvailable", r->value())
		//     .detail("ShardState0", data->shards[range.begin]->debugDescribeState());
		if (context == CSK_ASSIGN_EMPTY && !dataAvailable) {
			ASSERT(nowAssigned);
			TraceEvent("ChangeServerKeysAddEmptyRange", data->thisServerID)
			    .detail("Begin", range.begin)
			    .detail("End", range.end);
			newEmptyRanges.push_back(range);
			data->addShard(ShardInfo::newReadWrite(range, data));
		} else if (!nowAssigned) {
			if (dataAvailable) {
				ASSERT(r->value() ==
				       latestVersion); // Not that we care, but this used to be checked instead of dataAvailable
				ASSERT(data->mutableData().getLatestVersion() > version || context == CSK_RESTORE);
				changeNewestAvailable.emplace_back(range, version);
				removeRanges.push_back(range);
			}
			data->addShard(ShardInfo::newNotAssigned(range));
			data->watches.triggerRange(range.begin, range.end);
		} else if (!dataAvailable) {
			// SOMEDAY: Avoid restarting adding/transferred shards
			if (version == 0) { // bypass fetchkeys; shard is known empty at version 0
				TraceEvent("ChangeServerKeysInitialRange", data->thisServerID)
				    .detail("Begin", range.begin)
				    .detail("End", range.end);
				changeNewestAvailable.emplace_back(range, latestVersion);
				data->addShard(ShardInfo::newReadWrite(range, data));
				setAvailableStatus(data, range, true);
				// Create and persist a new shard??? May already exist???
			} else {
				auto& shard = data->shards[range.begin];
				if (!shard->assigned() || shard->keys != range)
					data->addShard(ShardInfo::newAdding(data, range));
			}
		} else {
			changeNewestAvailable.emplace_back(range, latestVersion);
			data->addShard(ShardInfo::newReadWrite(range, data));
		}
	}
	// Update newestAvailableVersion when a shard becomes (un)available (in a separate loop to avoid invalidating vr
	// above)
	for (auto r = changeNewestAvailable.begin(); r != changeNewestAvailable.end(); ++r)
		data->newestAvailableVersion.insert(r->first, r->second);

	if (!nowAssigned)
		data->metrics.notifyNotReadable(keys);

	coalesceShards(data, KeyRangeRef(ranges[0].begin, ranges[ranges.size() - 1].end));

	// Now it is OK to do removeDataRanges, directly and through fetchKeys cancellation (and we have to do so before
	// validate())
	oldShards.clear();
	ranges.clear();
	for (auto r = removeRanges.begin(); r != removeRanges.end(); ++r) {
		removeDataRange(data, data->addVersionToMutationLog(data->data().getLatestVersion()), data->shards, *r);
		setAvailableStatus(data, *r, false);
		// drop and delete shard metadata
	}

	// Clear the moving-in empty range, and set it available at the latestVersion.
	for (const auto& range : newEmptyRanges) {
		MutationRef clearRange(MutationRef::ClearRange, range.begin, range.end);
		data->addMutation(data->data().getLatestVersion(), true, clearRange, range, data->updateEagerReads);
		data->newestAvailableVersion.insert(range, latestVersion);
		setAvailableStatus(data, range, true);
		// create and persist empty shard.
	}
	validate(data);

	if (!nowAssigned) {
		std::map<Key, KeyRange> candidateFeeds;
		auto ranges = data->keyChangeFeed.intersectingRanges(keys);
		for (auto r : ranges) {
			for (auto feed : r.value()) {
				candidateFeeds[feed->id] = feed->range;
			}
		}
		for (auto f : candidateFeeds) {
			bool foundAssigned = false;
			auto shards = data->shards.intersectingRanges(f.second);
			for (auto shard : shards) {
				if (shard->value()->assigned()) {
					foundAssigned = true;
					break;
				}
			}
			if (!foundAssigned) {
				Key beginClearKey = f.first.withPrefix(persistChangeFeedKeys.begin);
				auto& mLV = data->addVersionToMutationLog(data->data().getLatestVersion());
				data->addMutationToMutationLog(
				    mLV, MutationRef(MutationRef::ClearRange, beginClearKey, keyAfter(beginClearKey)));
				data->addMutationToMutationLog(mLV,
				                               MutationRef(MutationRef::ClearRange,
				                                           changeFeedDurableKey(f.first, 0),
				                                           changeFeedDurableKey(f.first, version)));
				auto rs = data->keyChangeFeed.modify(f.second);
				for (auto r = rs.begin(); r != rs.end(); ++r) {
					auto& feedList = r->value();
					for (int i = 0; i < feedList.size(); i++) {
						if (feedList[i]->id == f.first) {
							swapAndPop(&feedList, i--);
						}
					}
				}
				auto feed = data->uidChangeFeed.find(f.first);
				if (feed != data->uidChangeFeed.end()) {
					feed->second->removing = true;
					feed->second->newMutations.trigger();
					data->uidChangeFeed.erase(feed);
				}
			}
		}
	}
}

void rollback(StorageServer* data, Version rollbackVersion, Version nextVersion) {
	TEST(true); // call to shard rollback
	DEBUG_KEY_RANGE("Rollback", rollbackVersion, allKeys, data->thisServerID);

	// We used to do a complicated dance to roll back in MVCC history.  It's much simpler, and more testable,
	// to simply restart the storage server actor and restore from the persistent disk state, and then roll
	// forward from the TLog's history.  It's not quite as efficient, but we rarely have to do this in practice.

	// FIXME: This code is relying for liveness on an undocumented property of the log system implementation: that after
	// a rollback the rolled back versions will eventually be missing from the peeked log.  A more sophisticated
	// approach would be to make the rollback range durable and, after reboot, skip over those versions if they appear
	// in peek results.

	throw please_reboot();
}

void StorageServer::addMutation(Version version,
                                bool fromFetch,
                                MutationRef const& mutation,
                                KeyRangeRef const& shard,
                                UpdateEagerReadInfo* eagerReads) {
	MutationRef expanded = mutation;
	auto& mLog = addVersionToMutationLog(version);

	if (!expandMutation(expanded, data(), eagerReads, shard.end, mLog.arena())) {
		return;
	}
	expanded = addMutationToMutationLog(mLog, expanded);
	DEBUG_MUTATION("applyMutation", version, expanded, thisServerID)
	    .detail("ShardBegin", shard.begin)
	    .detail("ShardEnd", shard.end);
	applyMutation(this, expanded, mLog.arena(), mutableData(), version, fromFetch);
	// printf("\nSSUpdate: Printing versioned tree after applying mutation\n");
	// mutableData().printTree(version);
}

struct OrderByVersion {
	bool operator()(const VerUpdateRef& a, const VerUpdateRef& b) {
		if (a.version != b.version)
			return a.version < b.version;
		if (a.isPrivateData != b.isPrivateData)
			return a.isPrivateData;
		return false;
	}
};

class StorageUpdater {
public:
	StorageUpdater()
	  : currentVersion(invalidVersion), fromVersion(invalidVersion), restoredVersion(invalidVersion),
	    processedStartKey(false), processedCacheStartKey(false) {}
	StorageUpdater(Version fromVersion, Version restoredVersion)
	  : currentVersion(fromVersion), fromVersion(fromVersion), restoredVersion(restoredVersion),
	    processedStartKey(false), processedCacheStartKey(false) {}

	void applyMutation(StorageServer* data, MutationRef const& m, Version ver, bool fromFetch) {
		//TraceEvent("SSNewVersion", data->thisServerID).detail("VerWas", data->mutableData().latestVersion).detail("ChVer", ver);

		if (currentVersion != ver) {
			fromVersion = currentVersion;
			currentVersion = ver;
			data->mutableData().createNewVersion(ver);
		}

		if (m.param1.startsWith(systemKeys.end)) {
			if ((m.type == MutationRef::SetValue) && m.param1.substr(1).startsWith(storageCachePrefix))
				applyPrivateCacheData(data, m);
			else {
				applyPrivateData(data, m);
			}
		} else {
			// FIXME: enable when DEBUG_MUTATION is active
			// for(auto m = changes[c].mutations.begin(); m; ++m) {
			//	DEBUG_MUTATION("SSUpdateMutation", changes[c].version, *m, data->thisServerID);
			//}

			splitMutation(data, data->shards, m, ver, fromFetch);
		}

		if (data->otherError.getFuture().isReady())
			data->otherError.getFuture().get();
	}

	Version currentVersion;

private:
	Version fromVersion;
	Version restoredVersion;

	KeyRef startKey;
	bool nowAssigned;
	bool emptyRange;
	bool processedStartKey;

	KeyRef cacheStartKey;
	bool processedCacheStartKey;

	void applyPrivateData(StorageServer* data, MutationRef const& m) {
		TraceEvent(SevDebug, "SSPrivateMutation", data->thisServerID).detail("Mutation", m);

		if (processedStartKey) {
			// Because of the implementation of the krm* functions, we expect changes in pairs, [begin,end)
			// We can also ignore clearRanges, because they are always accompanied by such a pair of sets with the same
			// keys
			ASSERT(m.type == MutationRef::SetValue && m.param1.startsWith(data->sk));
			KeyRangeRef keys(startKey.removePrefix(data->sk), m.param1.removePrefix(data->sk));

			// ignore data movements for tss in quarantine
			if (!data->isTSSInQuarantine()) {
				// add changes in shard assignment to the mutation log
				setAssignedStatus(data, keys, nowAssigned);

				// The changes for version have already been received (and are being processed now).  We need to fetch
				// the data for change.version-1 (changes from versions < change.version)
				// If emptyRange, treat the shard as empty, see removeKeysFromFailedServer() for more details about this
				// scenario.
				const ChangeServerKeysContext context = emptyRange ? CSK_ASSIGN_EMPTY : CSK_UPDATE;
				changeServerKeys(data, keys, nowAssigned, currentVersion - 1, context);
			}

			processedStartKey = false;
		} else if (m.type == MutationRef::SetValue && m.param1.startsWith(data->sk)) {
			// Because of the implementation of the krm* functions, we expect changes in pairs, [begin,end)
			// We can also ignore clearRanges, because they are always accompanied by such a pair of sets with the same
			// keys
			startKey = m.param1;
			nowAssigned = m.param2 != serverKeysFalse;
			emptyRange = m.param2 == serverKeysTrueEmptyRange;
			processedStartKey = true;
		} else if (m.type == MutationRef::SetValue && m.param1 == lastEpochEndPrivateKey) {
			// lastEpochEnd transactions are guaranteed by the master to be alone in their own batch (version)
			// That means we don't have to worry about the impact on changeServerKeys
			// ASSERT( /*isFirstVersionUpdateFromTLog && */!std::next(it) );

			Version rollbackVersion;
			BinaryReader br(m.param2, Unversioned());
			br >> rollbackVersion;

			if (rollbackVersion < fromVersion && rollbackVersion > restoredVersion) {
				TEST(true); // ShardApplyPrivateData shard rollback
				TraceEvent(SevWarn, "Rollback", data->thisServerID)
				    .detail("FromVersion", fromVersion)
				    .detail("ToVersion", rollbackVersion)
				    .detail("AtVersion", currentVersion)
				    .detail("StorageVersion", data->storageVersion());
				ASSERT(rollbackVersion >= data->storageVersion());
				rollback(data, rollbackVersion, currentVersion);
			}
			for (auto& it : data->uidChangeFeed) {
				it.second->mutations.push_back(MutationsAndVersionRef(currentVersion, rollbackVersion));
				it.second->mutations.back().mutations.push_back_deep(it.second->mutations.back().arena(), m);
				data->currentChangeFeeds.insert(it.first);
			}

			data->recoveryVersionSkips.emplace_back(rollbackVersion, currentVersion - rollbackVersion);
		} else if (m.type == MutationRef::SetValue && m.param1 == killStoragePrivateKey) {
			TraceEvent("StorageServerWorkerRemoved", data->thisServerID).detail("Reason", "KillStorage");
			throw worker_removed();
		} else if ((m.type == MutationRef::SetValue || m.type == MutationRef::ClearRange) &&
		           m.param1.substr(1).startsWith(serverTagPrefix)) {
			UID serverTagKey = decodeServerTagKey(m.param1.substr(1));
			bool matchesThisServer = serverTagKey == data->thisServerID;
			bool matchesTssPair = data->isTss() ? serverTagKey == data->tssPairID.get() : false;
			// Remove SS if another SS is now assigned our tag, or this server was removed by deleting our tag entry
			// Since TSS don't have tags, they check for their pair's tag. If a TSS is in quarantine, it will stick
			// around until its pair is removed or it is finished quarantine.
			if ((m.type == MutationRef::SetValue &&
			     ((!data->isTss() && !matchesThisServer) || (data->isTss() && !matchesTssPair))) ||
			    (m.type == MutationRef::ClearRange &&
			     ((!data->isTSSInQuarantine() && matchesThisServer) || (data->isTss() && matchesTssPair)))) {
				TraceEvent("StorageServerWorkerRemoved", data->thisServerID)
				    .detail("Reason", "ServerTag")
				    .detail("MutationType", getTypeString(m.type))
				    .detail("TagMatches", matchesThisServer)
				    .detail("IsTSS", data->isTss());
				throw worker_removed();
			}
			if (!data->isTss() && m.type == MutationRef::ClearRange && data->ssPairID.present() &&
			    serverTagKey == data->ssPairID.get()) {
				data->clearSSWithTssPair();
			}
		} else if (m.type == MutationRef::SetValue && m.param1 == rebootWhenDurablePrivateKey) {
			data->rebootAfterDurableVersion = currentVersion;
			TraceEvent("RebootWhenDurableSet", data->thisServerID)
			    .detail("DurableVersion", data->durableVersion.get())
			    .detail("RebootAfterDurableVersion", data->rebootAfterDurableVersion);
		} else if (m.type == MutationRef::SetValue && m.param1 == primaryLocalityPrivateKey) {
			data->primaryLocality = BinaryReader::fromStringRef<int8_t>(m.param2, Unversioned());
			auto& mLV = data->addVersionToMutationLog(data->data().getLatestVersion());
			data->addMutationToMutationLog(mLV, MutationRef(MutationRef::SetValue, persistPrimaryLocality, m.param2));
		} else if (m.type == MutationRef::SetValue && m.param1.startsWith(changeFeedPrivatePrefix)) {
			Key changeFeedId = m.param1.removePrefix(changeFeedPrivatePrefix);
			KeyRange changeFeedRange;
			Version popVersion;
			ChangeFeedStatus status;
			std::tie(changeFeedRange, popVersion, status) = decodeChangeFeedValue(m.param2);
			auto feed = data->uidChangeFeed.find(changeFeedId);
			if (feed == data->uidChangeFeed.end()) {
				if (status == ChangeFeedStatus::CHANGE_FEED_CREATE) {
					TraceEvent(SevDebug, "AddingChangeFeed", data->thisServerID)
					    .detail("RangeID", changeFeedId.printable())
					    .detail("Range", changeFeedRange.toString())
					    .detail("Version", currentVersion);
					Reference<ChangeFeedInfo> changeFeedInfo(new ChangeFeedInfo());
					changeFeedInfo->range = changeFeedRange;
					changeFeedInfo->id = changeFeedId;
					changeFeedInfo->emptyVersion = currentVersion - 1;
					data->uidChangeFeed[changeFeedId] = changeFeedInfo;

					auto rs = data->keyChangeFeed.modify(changeFeedRange);
					for (auto r = rs.begin(); r != rs.end(); ++r) {
						r->value().push_back(changeFeedInfo);
					}
					data->keyChangeFeed.coalesce(changeFeedRange.contents());
					auto& mLV = data->addVersionToMutationLog(data->data().getLatestVersion());
					data->addMutationToMutationLog(
					    mLV,
					    MutationRef(MutationRef::SetValue,
					                persistChangeFeedKeys.begin.toString() + changeFeedId.toString(),
					                m.param2));
				}
			} else {
				if (status == ChangeFeedStatus::CHANGE_FEED_DESTROY) {
					Key beginClearKey = changeFeedId.withPrefix(persistChangeFeedKeys.begin);
					auto& mLV = data->addVersionToMutationLog(data->data().getLatestVersion());
					data->addMutationToMutationLog(
					    mLV, MutationRef(MutationRef::ClearRange, beginClearKey, keyAfter(beginClearKey)));
					data->addMutationToMutationLog(mLV,
					                               MutationRef(MutationRef::ClearRange,
					                                           changeFeedDurableKey(feed->second->id, 0),
					                                           changeFeedDurableKey(feed->second->id, currentVersion)));
					auto rs = data->keyChangeFeed.modify(feed->second->range);
					for (auto r = rs.begin(); r != rs.end(); ++r) {
						auto& feedList = r->value();
						for (int i = 0; i < feedList.size(); i++) {
							if (feedList[i] == feed->second) {
								swapAndPop(&feedList, i--);
							}
						}
					}
					data->uidChangeFeed.erase(feed);
				} else {
					if (popVersion != invalidVersion && popVersion - 1 > feed->second->emptyVersion) {
						feed->second->emptyVersion = popVersion - 1;
						while (!feed->second->mutations.empty() &&
						       feed->second->mutations.front().version < popVersion) {
							feed->second->mutations.pop_front();
						}
						if (feed->second->storageVersion != invalidVersion) {
							data->storage.clearRange(KeyRangeRef(changeFeedDurableKey(feed->second->id, 0),
							                                     changeFeedDurableKey(feed->second->id, popVersion)));
							if (popVersion > feed->second->storageVersion) {
								feed->second->storageVersion = invalidVersion;
								feed->second->durableVersion = invalidVersion;
							}
						}
					}
					feed->second->stopped = (status == ChangeFeedStatus::CHANGE_FEED_STOP);
					auto& mLV = data->addVersionToMutationLog(data->data().getLatestVersion());
					data->addMutationToMutationLog(
					    mLV,
					    MutationRef(MutationRef::SetValue,
					                persistChangeFeedKeys.begin.toString() + changeFeedId.toString(),
					                m.param2));
				}
			}
		} else if (m.param1.substr(1).startsWith(tssMappingKeys.begin) &&
		           (m.type == MutationRef::SetValue || m.type == MutationRef::ClearRange)) {
			if (!data->isTss()) {
				UID ssId = Codec<UID>::unpack(Tuple::unpack(m.param1.substr(1).removePrefix(tssMappingKeys.begin)));
				ASSERT(ssId == data->thisServerID);
				if (m.type == MutationRef::SetValue) {
					UID tssId = Codec<UID>::unpack(Tuple::unpack(m.param2));
					data->setSSWithTssPair(tssId);
				} else {
					data->clearSSWithTssPair();
				}
			}
		} else if (m.param1.substr(1).startsWith(tssQuarantineKeys.begin) &&
		           (m.type == MutationRef::SetValue || m.type == MutationRef::ClearRange)) {
			if (data->isTss()) {
				UID ssId = decodeTssQuarantineKey(m.param1.substr(1));
				ASSERT(ssId == data->thisServerID);
				if (m.type == MutationRef::SetValue) {
					TEST(true); // Putting TSS in quarantine
					TraceEvent(SevWarn, "TSSQuarantineStart", data->thisServerID).log();
					data->startTssQuarantine();
				} else {
					TraceEvent(SevWarn, "TSSQuarantineStop", data->thisServerID).log();
					TraceEvent("StorageServerWorkerRemoved", data->thisServerID).detail("Reason", "TSSQuarantineStop");
					// dipose of this TSS
					throw worker_removed();
				}
			}
		} else {
			ASSERT(false); // Unknown private mutation
		}
	}

	void applyPrivateCacheData(StorageServer* data, MutationRef const& m) {
		//TraceEvent(SevDebug, "SSPrivateCacheMutation", data->thisServerID).detail("Mutation", m);

		if (processedCacheStartKey) {
			// Because of the implementation of the krm* functions, we expect changes in pairs, [begin,end)
			ASSERT((m.type == MutationRef::SetValue) && m.param1.substr(1).startsWith(storageCachePrefix));
			KeyRangeRef keys(cacheStartKey.removePrefix(systemKeys.begin).removePrefix(storageCachePrefix),
			                 m.param1.removePrefix(systemKeys.begin).removePrefix(storageCachePrefix));
			data->cachedRangeMap.insert(keys, true);

			// Figure out the affected shard ranges and maintain the cached key-range information in the in-memory map
			// TODO revisit- we are not splitting the cached ranges based on shards as of now.
			if (0) {
				auto cachedRanges = data->shards.intersectingRanges(keys);
				for (auto shard = cachedRanges.begin(); shard != cachedRanges.end(); ++shard) {
					KeyRangeRef intersectingRange = shard.range() & keys;
					TraceEvent(SevDebug, "SSPrivateCacheMutationInsertUnexpected", data->thisServerID)
					    .detail("Begin", intersectingRange.begin)
					    .detail("End", intersectingRange.end);
					data->cachedRangeMap.insert(intersectingRange, true);
				}
			}
			processedStartKey = false;
		} else if ((m.type == MutationRef::SetValue) && m.param1.substr(1).startsWith(storageCachePrefix)) {
			// Because of the implementation of the krm* functions, we expect changes in pairs, [begin,end)
			cacheStartKey = m.param1;
			processedCacheStartKey = true;
		} else {
			ASSERT(false); // Unknown private mutation
		}
	}
};

ACTOR Future<Void> tssDelayForever() {
	loop {
		wait(delay(5.0));
		if (g_simulator.speedUpSimulation) {
			return Void();
		}
	}
}

ACTOR Future<Void> update(StorageServer* data, bool* pReceivedUpdate) {
	state double start;
	try {
		// If we are disk bound and durableVersion is very old, we need to block updates or we could run out of memory
		// This is often referred to as the storage server e-brake (emergency brake)

		// We allow the storage server to make some progress between e-brake periods, referreed to as "overage", in
		// order to ensure that it advances desiredOldestVersion enough for updateStorage to make enough progress on
		// freeing up queue size.
		state double waitStartT = 0;
		if (data->queueSize() >= SERVER_KNOBS->STORAGE_HARD_LIMIT_BYTES &&
		    data->durableVersion.get() < data->desiredOldestVersion.get() &&
		    ((data->desiredOldestVersion.get() - SERVER_KNOBS->STORAGE_HARD_LIMIT_VERSION_OVERAGE >
		      data->lastDurableVersionEBrake) ||
		     (data->counters.bytesInput.getValue() - SERVER_KNOBS->STORAGE_HARD_LIMIT_BYTES_OVERAGE >
		      data->lastBytesInputEBrake))) {

			while (data->queueSize() >= SERVER_KNOBS->STORAGE_HARD_LIMIT_BYTES &&
			       data->durableVersion.get() < data->desiredOldestVersion.get()) {
				if (now() - waitStartT >= 1) {
					TraceEvent(SevWarn, "StorageServerUpdateLag", data->thisServerID)
					    .detail("Version", data->version.get())
					    .detail("DurableVersion", data->durableVersion.get())
					    .detail("DesiredOldestVersion", data->desiredOldestVersion.get())
					    .detail("QueueSize", data->queueSize())
					    .detail("LastBytesInputEBrake", data->lastBytesInputEBrake)
					    .detail("LastDurableVersionEBrake", data->lastDurableVersionEBrake);
					waitStartT = now();
				}

				data->behind = true;
				wait(delayJittered(.005, TaskPriority::TLogPeekReply));
			}
			data->lastBytesInputEBrake = data->counters.bytesInput.getValue();
			data->lastDurableVersionEBrake = data->durableVersion.get();
		}

		if (g_network->isSimulated() && data->isTss() && g_simulator.tssMode == ISimulator::TSSMode::EnabledAddDelay &&
		    !g_simulator.speedUpSimulation && data->tssFaultInjectTime.present() &&
		    data->tssFaultInjectTime.get() < now()) {
			if (deterministicRandom()->random01() < 0.01) {
				TraceEvent(SevWarnAlways, "TSSInjectDelayForever", data->thisServerID).log();
				// small random chance to just completely get stuck here, each tss should eventually hit this in this
				// mode
				wait(tssDelayForever());
			} else {
				// otherwise pause for part of a second
				double delayTime = deterministicRandom()->random01();
				TraceEvent(SevWarnAlways, "TSSInjectDelay", data->thisServerID).detail("Delay", delayTime);
				wait(delay(delayTime));
			}
		}

		while (data->byteSampleClearsTooLarge.get()) {
			wait(data->byteSampleClearsTooLarge.onChange());
		}

		state Reference<ILogSystem::IPeekCursor> cursor = data->logCursor;

		state double beforeTLogCursorReads = now();
		loop {
			wait(cursor->getMore());
			if (!cursor->isExhausted()) {
				break;
			}
		}
		data->tlogCursorReadsLatencyHistogram->sampleSeconds(now() - beforeTLogCursorReads);
		if (cursor->popped() > 0) {
			TraceEvent("StorageServerWorkerRemoved", data->thisServerID).detail("Reason", "PeekPoppedTLogData");
			throw worker_removed();
		}

		++data->counters.updateBatches;
		data->lastTLogVersion = cursor->getMaxKnownVersion();
		data->knownCommittedVersion = cursor->getMinKnownCommittedVersion();
		data->versionLag = std::max<int64_t>(0, data->lastTLogVersion - data->version.get());

		ASSERT(*pReceivedUpdate == false);
		*pReceivedUpdate = true;

		start = now();
		wait(data->durableVersionLock.take(TaskPriority::TLogPeekReply, 1));
		state FlowLock::Releaser holdingDVL(data->durableVersionLock);
		if (now() - start > 0.1)
			TraceEvent("SSSlowTakeLock1", data->thisServerID)
			    .detailf("From", "%016llx", debug_lastLoadBalanceResultEndpointToken)
			    .detail("Duration", now() - start)
			    .detail("Version", data->version.get());
		data->ssVersionLockLatencyHistogram->sampleSeconds(now() - start);

		start = now();
		state UpdateEagerReadInfo eager;
		state FetchInjectionInfo fii;
		state Reference<ILogSystem::IPeekCursor> cloneCursor2;

		loop {
			state uint64_t changeCounter = data->shardChangeCounter;
			bool epochEnd = false;
			bool hasPrivateData = false;
			bool firstMutation = true;
			bool dbgLastMessageWasProtocol = false;

			Reference<ILogSystem::IPeekCursor> cloneCursor1 = cursor->cloneNoMore();
			cloneCursor2 = cursor->cloneNoMore();

			cloneCursor1->setProtocolVersion(data->logProtocol);

			for (; cloneCursor1->hasMessage(); cloneCursor1->nextMessage()) {
				ArenaReader& cloneReader = *cloneCursor1->reader();

				if (LogProtocolMessage::isNextIn(cloneReader)) {
					LogProtocolMessage lpm;
					cloneReader >> lpm;
					//TraceEvent(SevDebug, "SSReadingLPM", data->thisServerID).detail("Mutation", lpm);
					dbgLastMessageWasProtocol = true;
					cloneCursor1->setProtocolVersion(cloneReader.protocolVersion());
				} else if (cloneReader.protocolVersion().hasSpanContext() &&
				           SpanContextMessage::isNextIn(cloneReader)) {
					SpanContextMessage scm;
					cloneReader >> scm;
				} else {
					MutationRef msg;
					cloneReader >> msg;
					// TraceEvent(SevDebug, "SSReadingLog", data->thisServerID).detail("Mutation", msg);

					if (firstMutation && msg.param1.startsWith(systemKeys.end))
						hasPrivateData = true;
					firstMutation = false;

					if (msg.param1 == lastEpochEndPrivateKey) {
						epochEnd = true;
						ASSERT(dbgLastMessageWasProtocol);
					}

					eager.addMutation(msg);
					dbgLastMessageWasProtocol = false;
				}
			}

			// Any fetchKeys which are ready to transition their shards to the adding,transferred state do so now.
			// If there is an epoch end we skip this step, to increase testability and to prevent inserting a version in
			// the middle of a rolled back version range.
			while (!hasPrivateData && !epochEnd && !data->readyFetchKeys.empty()) {
				auto fk = data->readyFetchKeys.back();
				data->readyFetchKeys.pop_back();
				fk.send(&fii);
				// fetchKeys() would put the data it fetched into the fii. The thread will not return back to this actor
				// until it was completed.
			}

			for (auto& c : fii.changes)
				eager.addMutations(c.mutations);

			wait(doEagerReads(data, &eager));
			if (data->shardChangeCounter == changeCounter)
				break;
			TEST(true); // A fetchKeys completed while we were doing this, so eager might be outdated.  Read it again.
			// SOMEDAY: Theoretically we could check the change counters of individual shards and retry the reads only
			// selectively
			eager = UpdateEagerReadInfo();
		}
		data->eagerReadsLatencyHistogram->sampleSeconds(now() - start);

		if (now() - start > 0.1)
			TraceEvent("SSSlowTakeLock2", data->thisServerID)
			    .detailf("From", "%016llx", debug_lastLoadBalanceResultEndpointToken)
			    .detail("Duration", now() - start)
			    .detail("Version", data->version.get());

		data->updateEagerReads = &eager;
		data->debug_inApplyUpdate = true;

		state StorageUpdater updater(data->lastVersionWithData, data->restoredVersion);

		if (EXPENSIVE_VALIDATION)
			data->data().atLatest().validate();
		validate(data);

		state bool injectedChanges = false;
		state int changeNum = 0;
		state int mutationBytes = 0;
		state double beforeFetchKeysUpdates = now();
		for (; changeNum < fii.changes.size(); changeNum++) {
			state int mutationNum = 0;
			state VerUpdateRef* pUpdate = &fii.changes[changeNum];
			for (; mutationNum < pUpdate->mutations.size(); mutationNum++) {
				updater.applyMutation(data, pUpdate->mutations[mutationNum], pUpdate->version, true);
				mutationBytes += pUpdate->mutations[mutationNum].totalSize();
				// data->counters.mutationBytes or data->counters.mutations should not be updated because they should
				// have counted when the mutations arrive from cursor initially.
				injectedChanges = true;
				if (mutationBytes > SERVER_KNOBS->DESIRED_UPDATE_BYTES) {
					mutationBytes = 0;
					wait(delay(SERVER_KNOBS->UPDATE_DELAY));
				}
			}
		}
		data->fetchKeysPTreeUpdatesLatencyHistogram->sampleSeconds(now() - beforeFetchKeysUpdates);

		state Version ver = invalidVersion;
		cloneCursor2->setProtocolVersion(data->logProtocol);
		state SpanID spanContext = SpanID();
		state double beforeTLogMsgsUpdates = now();
		state std::set<Key> updatedChangeFeeds;
		for (; cloneCursor2->hasMessage(); cloneCursor2->nextMessage()) {
			if (mutationBytes > SERVER_KNOBS->DESIRED_UPDATE_BYTES) {
				mutationBytes = 0;
				// Instead of just yielding, leave time for the storage server to respond to reads
				wait(delay(SERVER_KNOBS->UPDATE_DELAY));
			}

			if (cloneCursor2->version().version > ver) {
				ASSERT(cloneCursor2->version().version > data->version.get());
			}

			auto& rd = *cloneCursor2->reader();

			if (cloneCursor2->version().version > ver && cloneCursor2->version().version > data->version.get()) {
				++data->counters.updateVersions;
				if (data->currentChangeFeeds.size()) {
					data->changeFeedVersions.emplace_back(
					    std::vector<Key>(data->currentChangeFeeds.begin(), data->currentChangeFeeds.end()), ver);
					updatedChangeFeeds.insert(data->currentChangeFeeds.begin(), data->currentChangeFeeds.end());
					data->currentChangeFeeds.clear();
				}
				ver = cloneCursor2->version().version;
			}

			if (LogProtocolMessage::isNextIn(rd)) {
				LogProtocolMessage lpm;
				rd >> lpm;

				data->logProtocol = rd.protocolVersion();
				data->storage.changeLogProtocol(ver, data->logProtocol);
				cloneCursor2->setProtocolVersion(rd.protocolVersion());
				spanContext = UID();
			} else if (rd.protocolVersion().hasSpanContext() && SpanContextMessage::isNextIn(rd)) {
				SpanContextMessage scm;
				rd >> scm;
				spanContext = scm.spanContext;
			} else {
				MutationRef msg;
				rd >> msg;

				Span span("SS:update"_loc, { spanContext });
				span.addTag("key"_sr, msg.param1);

				// Drop non-private mutations if TSS fault injection is enabled in simulation, or if this is a TSS in
				// quarantine.
				if (g_network->isSimulated() && data->isTss() && !g_simulator.speedUpSimulation &&
				    g_simulator.tssMode == ISimulator::TSSMode::EnabledDropMutations &&
				    data->tssFaultInjectTime.present() && data->tssFaultInjectTime.get() < now() &&
				    (msg.type == MutationRef::SetValue || msg.type == MutationRef::ClearRange) &&
				    (msg.param1.size() < 2 || msg.param1[0] != 0xff || msg.param1[1] != 0xff) &&
				    deterministicRandom()->random01() < 0.05) {
					TraceEvent(SevWarnAlways, "TSSInjectDropMutation", data->thisServerID)
					    .detail("Mutation", msg)
					    .detail("Version", cloneCursor2->version().toString());
				} else if (data->isTSSInQuarantine() &&
				           (msg.param1.size() < 2 || msg.param1[0] != 0xff || msg.param1[1] != 0xff)) {
					TraceEvent("TSSQuarantineDropMutation", data->thisServerID)
					    .suppressFor(10.0)
					    .detail("Version", cloneCursor2->version().toString());
				} else if (ver != invalidVersion) { // This change belongs to a version < minVersion
					DEBUG_MUTATION("SSPeek", ver, msg, data->thisServerID);
					if (ver == 1) {
						//TraceEvent("SSPeekMutation", data->thisServerID).log();
						// The following trace event may produce a value with special characters
						TraceEvent("SSPeekMutation", data->thisServerID)
						    .detail("Mutation", msg)
						    .detail("Version", cloneCursor2->version().toString());
					}

					updater.applyMutation(data, msg, ver, false);
					mutationBytes += msg.totalSize();
					data->counters.mutationBytes += msg.totalSize();
					++data->counters.mutations;
					switch (msg.type) {
					case MutationRef::SetValue:
						++data->counters.setMutations;
						break;
					case MutationRef::ClearRange:
						++data->counters.clearRangeMutations;
						break;
					case MutationRef::AddValue:
					case MutationRef::And:
					case MutationRef::AndV2:
					case MutationRef::AppendIfFits:
					case MutationRef::ByteMax:
					case MutationRef::ByteMin:
					case MutationRef::Max:
					case MutationRef::Min:
					case MutationRef::MinV2:
					case MutationRef::Or:
					case MutationRef::Xor:
					case MutationRef::CompareAndClear:
						++data->counters.atomicMutations;
						break;
					}
				} else
					TraceEvent(SevError, "DiscardingPeekedData", data->thisServerID)
					    .detail("Mutation", msg)
					    .detail("Version", cloneCursor2->version().toString());
			}
		}
		data->tLogMsgsPTreeUpdatesLatencyHistogram->sampleSeconds(now() - beforeTLogMsgsUpdates);
		if (data->currentChangeFeeds.size()) {
			data->changeFeedVersions.emplace_back(
			    std::vector<Key>(data->currentChangeFeeds.begin(), data->currentChangeFeeds.end()), ver);
			updatedChangeFeeds.insert(data->currentChangeFeeds.begin(), data->currentChangeFeeds.end());
			data->currentChangeFeeds.clear();
		}

		if (ver != invalidVersion) {
			data->lastVersionWithData = ver;
		}
		ver = cloneCursor2->version().version - 1;

		if (injectedChanges)
			data->lastVersionWithData = ver;

		data->updateEagerReads = nullptr;
		data->debug_inApplyUpdate = false;

		if (ver == invalidVersion && !fii.changes.empty()) {
			ver = updater.currentVersion;
		}

		if (ver != invalidVersion && ver > data->version.get()) {
			// TODO(alexmiller): Update to version tracking.
			// DEBUG_KEY_RANGE("SSUpdate", ver, KeyRangeRef());

			data->mutableData().createNewVersion(ver);
			if (data->otherError.getFuture().isReady())
				data->otherError.getFuture().get();

			data->counters.fetchedVersions += (ver - data->version.get());
			++data->counters.fetchesFromLogs;
			Optional<UID> curSourceTLogID = cursor->getCurrentPeekLocation();

			if (curSourceTLogID != data->sourceTLogID) {
				data->sourceTLogID = curSourceTLogID;

				TraceEvent("StorageServerSourceTLogID", data->thisServerID)
				    .detail("SourceTLogID",
				            data->sourceTLogID.present() ? data->sourceTLogID.get().toString() : "unknown")
				    .trackLatest(data->storageServerSourceTLogIDEventHolder->trackingKey);
			}

			data->noRecentUpdates.set(false);
			data->lastUpdate = now();

			data->prevVersion = data->version.get();
			data->version.set(ver); // Triggers replies to waiting gets for new version(s)

			for (auto& it : updatedChangeFeeds) {
				auto feed = data->uidChangeFeed.find(it);
				if (feed != data->uidChangeFeed.end()) {
					feed->second->newMutations.trigger();
				}
			}

			setDataVersion(data->thisServerID, data->version.get());
			if (data->otherError.getFuture().isReady())
				data->otherError.getFuture().get();

			Version maxVersionsInMemory = SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS;
			for (int i = 0; i < data->recoveryVersionSkips.size(); i++) {
				maxVersionsInMemory += data->recoveryVersionSkips[i].second;
			}

			// Trigger updateStorage if necessary
			Version proposedOldestVersion =
			    std::max(data->version.get(), cursor->getMinKnownCommittedVersion()) - maxVersionsInMemory;
			if (data->primaryLocality == tagLocalitySpecial || data->tag.locality == data->primaryLocality) {
				proposedOldestVersion = std::max(proposedOldestVersion, data->lastTLogVersion - maxVersionsInMemory);
			}
			proposedOldestVersion = std::min(proposedOldestVersion, data->version.get() - 1);
			proposedOldestVersion = std::max(proposedOldestVersion, data->oldestVersion.get());
			proposedOldestVersion = std::max(proposedOldestVersion, data->desiredOldestVersion.get());

			//TraceEvent("StorageServerUpdated", data->thisServerID).detail("Ver", ver).detail("DataVersion", data->version.get())
			//	.detail("LastTLogVersion", data->lastTLogVersion).detail("NewOldest",
			// data->oldestVersion.get()).detail("DesiredOldest",data->desiredOldestVersion.get())
			//	.detail("MaxVersionInMemory", maxVersionsInMemory).detail("Proposed",
			// proposedOldestVersion).detail("PrimaryLocality", data->primaryLocality).detail("Tag",
			// data->tag.toString());

			while (!data->recoveryVersionSkips.empty() &&
			       proposedOldestVersion > data->recoveryVersionSkips.front().first) {
				data->recoveryVersionSkips.pop_front();
			}
			data->desiredOldestVersion.set(proposedOldestVersion);
		}

		validate(data);

		data->logCursor->advanceTo(cloneCursor2->version());
		if (cursor->version().version >= data->lastTLogVersion) {
			if (data->behind) {
				TraceEvent("StorageServerNoLongerBehind", data->thisServerID)
				    .detail("CursorVersion", cursor->version().version)
				    .detail("TLogVersion", data->lastTLogVersion);
			}
			data->behind = false;
		}

		return Void(); // update will get called again ASAP
	} catch (Error& err) {
		state Error e = err;
		if (e.code() != error_code_worker_removed && e.code() != error_code_please_reboot) {
			TraceEvent(SevError, "SSUpdateError", data->thisServerID).error(e).backtrace();
		} else if (e.code() == error_code_please_reboot) {
			wait(data->durableInProgress);
		}
		throw e;
	}
}

ACTOR Future<Void> updateStorage(StorageServer* data) {
	loop {
		ASSERT(data->durableVersion.get() == data->storageVersion());
		if (g_network->isSimulated()) {
			double endTime =
			    g_simulator.checkDisabled(format("%s/updateStorage", data->thisServerID.toString().c_str()));
			if (endTime > now()) {
				wait(delay(endTime - now(), TaskPriority::UpdateStorage));
			}
		}
		wait(data->desiredOldestVersion.whenAtLeast(data->storageVersion() + 1));
		wait(delay(0, TaskPriority::UpdateStorage));

		state Promise<Void> durableInProgress;
		data->durableInProgress = durableInProgress.getFuture();

		state Version startOldestVersion = data->storageVersion();
		state Version newOldestVersion = data->storageVersion();
		state Version desiredVersion = data->desiredOldestVersion.get();
		state int64_t bytesLeft = SERVER_KNOBS->STORAGE_COMMIT_BYTES;

		// Write mutations to storage until we reach the desiredVersion or have written too much (bytesleft)
		state double beforeStorageUpdates = now();
		loop {
			state bool done = data->storage.makeVersionMutationsDurable(newOldestVersion, desiredVersion, bytesLeft);
			// We want to forget things from these data structures atomically with changing oldestVersion (and "before",
			// since oldestVersion.set() may trigger waiting actors) forgetVersionsBeforeAsync visibly forgets
			// immediately (without waiting) but asynchronously frees memory.
			Future<Void> finishedForgetting =
			    data->mutableData().forgetVersionsBeforeAsync(newOldestVersion, TaskPriority::UpdateStorage);
			data->oldestVersion.set(newOldestVersion);
			wait(finishedForgetting);
			wait(yield(TaskPriority::UpdateStorage));
			if (done)
				break;
		}

		std::set<Key> modifiedChangeFeeds;
		while (!data->changeFeedVersions.empty() && data->changeFeedVersions.front().second <= newOldestVersion) {
			modifiedChangeFeeds.insert(data->changeFeedVersions.front().first.begin(),
			                           data->changeFeedVersions.front().first.end());
			data->changeFeedVersions.pop_front();
		}

		state std::vector<Key> updatedChangeFeeds(modifiedChangeFeeds.begin(), modifiedChangeFeeds.end());
		state int curFeed = 0;
		while (curFeed < updatedChangeFeeds.size()) {
			auto info = data->uidChangeFeed.find(updatedChangeFeeds[curFeed]);
			if (info != data->uidChangeFeed.end()) {
				for (auto& it : info->second->mutations) {
					if (it.version > newOldestVersion) {
						break;
					}
					data->storage.writeKeyValue(
					    KeyValueRef(changeFeedDurableKey(info->second->id, it.version),
					                changeFeedDurableValue(it.mutations, it.knownCommittedVersion)));
					info->second->storageVersion = it.version;
				}
				wait(yield(TaskPriority::UpdateStorage));
			}
			curFeed++;
		}

		// Set the new durable version as part of the outstanding change set, before commit
		if (startOldestVersion != newOldestVersion)
			data->storage.makeVersionDurable(newOldestVersion);
		data->storageUpdatesDurableLatencyHistogram->sampleSeconds(now() - beforeStorageUpdates);

		debug_advanceMaxCommittedVersion(data->thisServerID, newOldestVersion);
		state double beforeStorageCommit = now();
		state Future<Void> durable = data->storage.commit();
		state Future<Void> durableDelay = Void();

		if (bytesLeft > 0) {
			durableDelay = delay(SERVER_KNOBS->STORAGE_COMMIT_INTERVAL, TaskPriority::UpdateStorage);
		}

		wait(ioTimeoutError(durable, SERVER_KNOBS->MAX_STORAGE_COMMIT_TIME));
		data->storageCommitLatencyHistogram->sampleSeconds(now() - beforeStorageCommit);

		debug_advanceMinCommittedVersion(data->thisServerID, newOldestVersion);

		if (newOldestVersion > data->rebootAfterDurableVersion) {
			TraceEvent("RebootWhenDurableTriggered", data->thisServerID)
			    .detail("NewOldestVersion", newOldestVersion)
			    .detail("RebootAfterDurableVersion", data->rebootAfterDurableVersion);
			// To avoid brokenPromise error, which is caused by the sender of the durableInProgress (i.e., this process)
			// never sets durableInProgress, we should set durableInProgress before send the please_reboot() error.
			// Otherwise, in the race situation when storage server receives both reboot and
			// brokenPromise of durableInProgress, the worker of the storage server will die.
			// We will eventually end up with no worker for storage server role.
			// The data distributor's buildTeam() will get stuck in building a team
			durableInProgress.sendError(please_reboot());
			throw please_reboot();
		}

		curFeed = 0;
		while (curFeed < updatedChangeFeeds.size()) {
			auto info = data->uidChangeFeed.find(updatedChangeFeeds[curFeed]);
			if (info != data->uidChangeFeed.end()) {
				while (!info->second->mutations.empty() && info->second->mutations.front().version < newOldestVersion) {
					info->second->mutations.pop_front();
				}
				info->second->durableVersion = info->second->storageVersion;
				wait(yield(TaskPriority::UpdateStorage));
			}
			curFeed++;
		}

		durableInProgress.send(Void());
		wait(delay(0, TaskPriority::UpdateStorage)); // Setting durableInProgess could cause the storage server to shut
		                                             // down, so delay to check for cancellation

		// Taking and releasing the durableVersionLock ensures that no eager reads both begin before the commit was
		// effective and are applied after we change the durable version. Also ensure that we have to lock while calling
		// changeDurableVersion, because otherwise the latest version of mutableData might be partially loaded.
		state double beforeSSDurableVersionUpdate = now();
		wait(data->durableVersionLock.take());
		data->popVersion(data->durableVersion.get() + 1);

		while (!changeDurableVersion(data, newOldestVersion)) {
			if (g_network->check_yield(TaskPriority::UpdateStorage)) {
				data->durableVersionLock.release();
				wait(delay(0, TaskPriority::UpdateStorage));
				wait(data->durableVersionLock.take());
			}
		}

		data->durableVersionLock.release();
		data->ssDurableVersionUpdateLatencyHistogram->sampleSeconds(now() - beforeSSDurableVersionUpdate);

		//TraceEvent("StorageServerDurable", data->thisServerID).detail("Version", newOldestVersion);
		data->fetchKeysBytesBudget = SERVER_KNOBS->STORAGE_FETCH_BYTES;
		data->fetchKeysBudgetUsed.set(false);
		if (!data->fetchKeysBudgetUsed.get()) {
			wait(durableDelay || data->fetchKeysBudgetUsed.onChange());
		}
	}
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

////////////////////////////////// StorageServerDisk ///////////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region StorageServerDisk
#endif

void StorageServerDisk::makeNewStorageServerDurable() {
	storage->set(persistFormat);
	storage->set(KeyValueRef(persistID, BinaryWriter::toValue(data->thisServerID, Unversioned())));
	if (data->tssPairID.present()) {
		storage->set(KeyValueRef(persistTssPairID, BinaryWriter::toValue(data->tssPairID.get(), Unversioned())));
	}
	ASSERT(data->clusterId.getFuture().isReady() && data->clusterId.getFuture().get().isValid());
	storage->set(
	    KeyValueRef(persistClusterIdKey, BinaryWriter::toValue(data->clusterId.getFuture().get(), Unversioned())));
	storage->set(KeyValueRef(persistVersion, BinaryWriter::toValue(data->version.get(), Unversioned())));
	storage->set(KeyValueRef(persistShardAssignedKeys.begin.toString(), LiteralStringRef("0")));
	storage->set(KeyValueRef(persistShardAvailableKeys.begin.toString(), LiteralStringRef("0")));
}

void setAvailableStatus(StorageServer* self, KeyRangeRef keys, bool available) {
	// ASSERT( self->debug_inApplyUpdate );
	ASSERT(!keys.empty());

	auto& mLV = self->addVersionToMutationLog(self->data().getLatestVersion());

	KeyRange availableKeys = KeyRangeRef(persistShardAvailableKeys.begin.toString() + keys.begin.toString(),
	                                     persistShardAvailableKeys.begin.toString() + keys.end.toString());
	//TraceEvent("SetAvailableStatus", self->thisServerID).detail("Version", mLV.version).detail("RangeBegin", availableKeys.begin).detail("RangeEnd", availableKeys.end);

	self->addMutationToMutationLog(mLV, MutationRef(MutationRef::ClearRange, availableKeys.begin, availableKeys.end));
	self->addMutationToMutationLog(mLV,
	                               MutationRef(MutationRef::SetValue,
	                                           availableKeys.begin,
	                                           available ? LiteralStringRef("1") : LiteralStringRef("0")));
	if (keys.end != allKeys.end) {
		bool endAvailable = self->shards.rangeContaining(keys.end)->value()->isInVersionedData();
		self->addMutationToMutationLog(mLV,
		                               MutationRef(MutationRef::SetValue,
		                                           availableKeys.end,
		                                           endAvailable ? LiteralStringRef("1") : LiteralStringRef("0")));
	}
}

void setAssignedStatus(StorageServer* self, KeyRangeRef keys, bool nowAssigned) {
	ASSERT(!keys.empty());
	auto& mLV = self->addVersionToMutationLog(self->data().getLatestVersion());
	KeyRange assignedKeys = KeyRangeRef(persistShardAssignedKeys.begin.toString() + keys.begin.toString(),
	                                    persistShardAssignedKeys.begin.toString() + keys.end.toString());
	//TraceEvent("SetAssignedStatus", self->thisServerID).detail("Version", mLV.version).detail("RangeBegin", assignedKeys.begin).detail("RangeEnd", assignedKeys.end);
	self->addMutationToMutationLog(mLV, MutationRef(MutationRef::ClearRange, assignedKeys.begin, assignedKeys.end));
	self->addMutationToMutationLog(mLV,
	                               MutationRef(MutationRef::SetValue,
	                                           assignedKeys.begin,
	                                           nowAssigned ? LiteralStringRef("1") : LiteralStringRef("0")));
	if (keys.end != allKeys.end) {
		bool endAssigned = self->shards.rangeContaining(keys.end)->value()->assigned();
		self->addMutationToMutationLog(mLV,
		                               MutationRef(MutationRef::SetValue,
		                                           assignedKeys.end,
		                                           endAssigned ? LiteralStringRef("1") : LiteralStringRef("0")));
	}
}

void StorageServerDisk::clearRange(KeyRangeRef keys) {
	storage->clear(keys);
}

void StorageServerDisk::writeKeyValue(KeyValueRef kv) {
	storage->set(kv);
}

void StorageServerDisk::writeMutation(MutationRef mutation) {
	if (mutation.type == MutationRef::SetValue) {
		storage->set(KeyValueRef(mutation.param1, mutation.param2));
	} else if (mutation.type == MutationRef::ClearRange) {
		storage->clear(KeyRangeRef(mutation.param1, mutation.param2));
	} else
		ASSERT(false);
}

void StorageServerDisk::writeMutations(const VectorRef<MutationRef>& mutations,
                                       Version debugVersion,
                                       const char* debugContext) {
	for (const auto& m : mutations) {
		DEBUG_MUTATION(debugContext, debugVersion, m, data->thisServerID);
		if (m.type == MutationRef::SetValue) {
			storage->set(KeyValueRef(m.param1, m.param2));
		} else if (m.type == MutationRef::ClearRange) {
			storage->clear(KeyRangeRef(m.param1, m.param2));
		}
	}
}

bool StorageServerDisk::makeVersionMutationsDurable(Version& prevStorageVersion,
                                                    Version newStorageVersion,
                                                    int64_t& bytesLeft) {
	if (bytesLeft <= 0)
		return true;

	// Apply mutations from the mutationLog
	auto u = data->getMutationLog().upper_bound(prevStorageVersion);
	if (u != data->getMutationLog().end() && u->first <= newStorageVersion) {
		VerUpdateRef const& v = u->second;
		ASSERT(v.version > prevStorageVersion && v.version <= newStorageVersion);
		// TODO(alexmiller): Update to version tracking.
		// DEBUG_KEY_RANGE("makeVersionMutationsDurable", v.version, KeyRangeRef());
		writeMutations(v.mutations, v.version, "makeVersionDurable");
		for (const auto& m : v.mutations)
			bytesLeft -= mvccStorageBytes(m);
		prevStorageVersion = v.version;
		return false;
	} else {
		prevStorageVersion = newStorageVersion;
		return true;
	}
}

// Update data->storage to persist the changes from (data->storageVersion(),version]
void StorageServerDisk::makeVersionDurable(Version version) {
	storage->set(KeyValueRef(persistVersion, BinaryWriter::toValue(version, Unversioned())));

	// TraceEvent("MakeDurable", data->thisServerID)
	//     .detail("FromVersion", prevStorageVersion)
	//     .detail("ToVersion", version);
}

// Update data->storage to persist tss quarantine state
void StorageServerDisk::makeTssQuarantineDurable() {
	storage->set(KeyValueRef(persistTssQuarantine, LiteralStringRef("1")));
}

void StorageServerDisk::changeLogProtocol(Version version, ProtocolVersion protocol) {
	data->addMutationToMutationLogOrStorage(
	    version,
	    MutationRef(MutationRef::SetValue, persistLogProtocol, BinaryWriter::toValue(protocol, Unversioned())));
}

ACTOR Future<Void> applyByteSampleResult(StorageServer* data,
                                         IKeyValueStore* storage,
                                         Key begin,
                                         Key end,
                                         std::vector<Standalone<VectorRef<KeyValueRef>>>* results = nullptr) {
	state int totalFetches = 0;
	state int totalKeys = 0;
	state int totalBytes = 0;
	loop {
		RangeResult bs = wait(storage->readRange(
		    KeyRangeRef(begin, end), SERVER_KNOBS->STORAGE_LIMIT_BYTES, SERVER_KNOBS->STORAGE_LIMIT_BYTES));
		if (results)
			results->push_back(bs.castTo<VectorRef<KeyValueRef>>());
		int rangeSize = bs.expectedSize();
		totalFetches++;
		totalKeys += bs.size();
		totalBytes += rangeSize;
		for (int j = 0; j < bs.size(); j++) {
			KeyRef key = bs[j].key.removePrefix(persistByteSampleKeys.begin);
			if (!data->byteSampleClears.rangeContaining(key).value()) {
				data->metrics.byteSample.sample.insert(
				    key, BinaryReader::fromStringRef<int32_t>(bs[j].value, Unversioned()), false);
			}
		}
		if (rangeSize >= SERVER_KNOBS->STORAGE_LIMIT_BYTES) {
			Key nextBegin = keyAfter(bs.back().key);
			data->byteSampleClears.insert(KeyRangeRef(begin, nextBegin).removePrefix(persistByteSampleKeys.begin),
			                              true);
			data->byteSampleClearsTooLarge.set(data->byteSampleClears.size() >
			                                   SERVER_KNOBS->MAX_BYTE_SAMPLE_CLEAR_MAP_SIZE);
			begin = nextBegin;
			if (begin == end) {
				break;
			}
		} else {
			data->byteSampleClears.insert(KeyRangeRef(begin.removePrefix(persistByteSampleKeys.begin),
			                                          end == persistByteSampleKeys.end
			                                              ? LiteralStringRef("\xff\xff\xff")
			                                              : end.removePrefix(persistByteSampleKeys.begin)),
			                              true);
			data->byteSampleClearsTooLarge.set(data->byteSampleClears.size() >
			                                   SERVER_KNOBS->MAX_BYTE_SAMPLE_CLEAR_MAP_SIZE);
			break;
		}

		if (!results) {
			wait(delay(SERVER_KNOBS->BYTE_SAMPLE_LOAD_DELAY));
		}
	}
	TraceEvent("RecoveredByteSampleRange", data->thisServerID)
	    .detail("Begin", begin)
	    .detail("End", end)
	    .detail("Fetches", totalFetches)
	    .detail("Keys", totalKeys)
	    .detail("ReadBytes", totalBytes);
	return Void();
}

ACTOR Future<Void> restoreByteSample(StorageServer* data,
                                     IKeyValueStore* storage,
                                     Promise<Void> byteSampleSampleRecovered,
                                     Future<Void> startRestore) {
	state std::vector<Standalone<VectorRef<KeyValueRef>>> byteSampleSample;
	wait(applyByteSampleResult(
	    data, storage, persistByteSampleSampleKeys.begin, persistByteSampleSampleKeys.end, &byteSampleSample));
	byteSampleSampleRecovered.send(Void());
	wait(startRestore);
	wait(delay(SERVER_KNOBS->BYTE_SAMPLE_START_DELAY));

	size_t bytes_per_fetch = 0;
	// Since the expected size also includes (as of now) the space overhead of the container, we calculate our own
	// number here
	for (auto& it : byteSampleSample) {
		for (auto& kv : it) {
			bytes_per_fetch += BinaryReader::fromStringRef<int32_t>(kv.value, Unversioned());
		}
	}
	bytes_per_fetch = (bytes_per_fetch / SERVER_KNOBS->BYTE_SAMPLE_LOAD_PARALLELISM) + 1;

	state std::vector<Future<Void>> sampleRanges;
	int accumulatedSize = 0;
	Key lastStart =
	    persistByteSampleKeys.begin; // make sure the first range starts at the absolute beginning of the byte sample
	for (auto& it : byteSampleSample) {
		for (auto& kv : it) {
			if (accumulatedSize >= bytes_per_fetch) {
				accumulatedSize = 0;
				Key realKey = kv.key.removePrefix(persistByteSampleKeys.begin);
				sampleRanges.push_back(applyByteSampleResult(data, storage, lastStart, realKey));
				lastStart = realKey;
			}
			accumulatedSize += BinaryReader::fromStringRef<int32_t>(kv.value, Unversioned());
		}
	}
	// make sure that the last range goes all the way to the end of the byte sample
	sampleRanges.push_back(applyByteSampleResult(data, storage, lastStart, persistByteSampleKeys.end));

	wait(waitForAll(sampleRanges));
	TraceEvent("RecoveredByteSampleChunkedRead", data->thisServerID).detail("Ranges", sampleRanges.size());

	if (BUGGIFY)
		wait(delay(deterministicRandom()->random01() * 10.0));

	return Void();
}

// Reads the cluster ID from the transaction state store.
ACTOR Future<UID> getClusterId(StorageServer* self) {
	state ReadYourWritesTransaction tr(self->cx);
	loop {
		try {
			tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr.setOption(FDBTransactionOptions::LOCK_AWARE);
			Optional<Value> clusterId = wait(tr.get(clusterIdKey));
			ASSERT(clusterId.present());
			return BinaryReader::fromStringRef<UID>(clusterId.get(), Unversioned());
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
}

// Read the cluster ID from the transaction state store and persist it to local
// storage. This function should only be necessary during an upgrade when the
// prior FDB version did not support cluster IDs. The normal path for storage
// server recruitment will include the cluster ID in the initial recruitment
// message.
ACTOR Future<Void> persistClusterId(StorageServer* self) {
	state Transaction tr(self->cx);
	loop {
		try {
			Optional<Value> clusterId = wait(tr.get(clusterIdKey));
			if (clusterId.present()) {
				auto uid = BinaryReader::fromStringRef<UID>(clusterId.get(), Unversioned());
				self->storage.writeKeyValue(
				    KeyValueRef(persistClusterIdKey, BinaryWriter::toValue(uid, Unversioned())));
				// Purposely not calling commit here, and letting the recurring
				// commit handle save this value to disk
				self->clusterId.send(uid);
			}
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}
	return Void();
}

ACTOR Future<bool> restoreDurableState(StorageServer* data, IKeyValueStore* storage) {
	state Future<Optional<Value>> fFormat = storage->readValue(persistFormat.key);
	state Future<Optional<Value>> fID = storage->readValue(persistID);
	state Future<Optional<Value>> fClusterID = storage->readValue(persistClusterIdKey);
	state Future<Optional<Value>> ftssPairID = storage->readValue(persistTssPairID);
	state Future<Optional<Value>> fTssQuarantine = storage->readValue(persistTssQuarantine);
	state Future<Optional<Value>> fVersion = storage->readValue(persistVersion);
	state Future<Optional<Value>> fLogProtocol = storage->readValue(persistLogProtocol);
	state Future<Optional<Value>> fPrimaryLocality = storage->readValue(persistPrimaryLocality);
	state Future<RangeResult> fShardAssigned = storage->readRange(persistShardAssignedKeys);
	state Future<RangeResult> fShardAvailable = storage->readRange(persistShardAvailableKeys);
	state Future<RangeResult> fChangeFeeds = storage->readRange(persistChangeFeedKeys);

	state Promise<Void> byteSampleSampleRecovered;
	state Promise<Void> startByteSampleRestore;
	data->byteSampleRecovery =
	    restoreByteSample(data, storage, byteSampleSampleRecovered, startByteSampleRestore.getFuture());

	TraceEvent("ReadingDurableState", data->thisServerID).log();
	wait(waitForAll(
	    std::vector{ fFormat, fID, fClusterID, ftssPairID, fTssQuarantine, fVersion, fLogProtocol, fPrimaryLocality }));
	wait(waitForAll(std::vector{ fShardAssigned, fShardAvailable, fChangeFeeds }));
	wait(byteSampleSampleRecovered.getFuture());
	TraceEvent("RestoringDurableState", data->thisServerID).log();

	if (!fFormat.get().present()) {
		// The DB was never initialized
		TraceEvent("DBNeverInitialized", data->thisServerID).log();
		storage->dispose();
		data->thisServerID = UID();
		data->sk = Key();
		return false;
	}
	if (!persistFormatReadableRange.contains(fFormat.get().get())) {
		TraceEvent(SevError, "UnsupportedDBFormat")
		    .detail("Format", fFormat.get().get().toString())
		    .detail("Expected", persistFormat.value.toString());
		throw worker_recovery_failed();
	}
	data->thisServerID = BinaryReader::fromStringRef<UID>(fID.get().get(), Unversioned());
	if (ftssPairID.get().present()) {
		data->setTssPair(BinaryReader::fromStringRef<UID>(ftssPairID.get().get(), Unversioned()));
	}

	if (fClusterID.get().present()) {
		data->clusterId.send(BinaryReader::fromStringRef<UID>(fClusterID.get().get(), Unversioned()));
	} else {
		TEST(true); // storage server upgraded to version supporting cluster IDs
		data->actors.add(persistClusterId(data));
	}

	// It's a bit sketchy to rely on an untrusted storage engine to persist its quarantine state when the quarantine
	// state means the storage engine already had a durability or correctness error, but it should get re-quarantined
	// very quickly because of a mismatch if it starts trying to do things again
	if (fTssQuarantine.get().present()) {
		TEST(true); // TSS restarted while quarantined
		data->tssInQuarantine = true;
	}

	data->sk = serverKeysPrefixFor((data->tssPairID.present()) ? data->tssPairID.get() : data->thisServerID)
	               .withPrefix(systemKeys.begin); // FFFF/serverKeys/[this server]/

	if (fLogProtocol.get().present())
		data->logProtocol = BinaryReader::fromStringRef<ProtocolVersion>(fLogProtocol.get().get(), Unversioned());

	if (fPrimaryLocality.get().present())
		data->primaryLocality = BinaryReader::fromStringRef<int8_t>(fPrimaryLocality.get().get(), Unversioned());

	state Version version = BinaryReader::fromStringRef<Version>(fVersion.get().get(), Unversioned());
	debug_checkRestoredVersion(data->thisServerID, version, "StorageServer");
	data->setInitialVersion(version);

	state RangeResult available = fShardAvailable.get();
	state int availableLoc;
	for (availableLoc = 0; availableLoc < available.size(); availableLoc++) {
		KeyRangeRef keys(available[availableLoc].key.removePrefix(persistShardAvailableKeys.begin),
		                 availableLoc + 1 == available.size()
		                     ? allKeys.end
		                     : available[availableLoc + 1].key.removePrefix(persistShardAvailableKeys.begin));
		ASSERT(!keys.empty());

		bool nowAvailable = available[availableLoc].value != LiteralStringRef("0");
		/*if(nowAvailable)
		  TraceEvent("AvailableShard", data->thisServerID).detail("RangeBegin", keys.begin).detail("RangeEnd", keys.end);*/
		data->newestAvailableVersion.insert(keys, nowAvailable ? latestVersion : invalidVersion);
		wait(yield());
	}

	state RangeResult assigned = fShardAssigned.get();
	state int assignedLoc;
	for (assignedLoc = 0; assignedLoc < assigned.size(); assignedLoc++) {
		KeyRangeRef keys(assigned[assignedLoc].key.removePrefix(persistShardAssignedKeys.begin),
		                 assignedLoc + 1 == assigned.size()
		                     ? allKeys.end
		                     : assigned[assignedLoc + 1].key.removePrefix(persistShardAssignedKeys.begin));
		ASSERT(!keys.empty());
		bool nowAssigned = assigned[assignedLoc].value != LiteralStringRef("0");
		/*if(nowAssigned)
		  TraceEvent("AssignedShard", data->thisServerID).detail("RangeBegin", keys.begin).detail("RangeEnd", keys.end);*/
		changeServerKeys(data, keys, nowAssigned, version, CSK_RESTORE);

		if (!nowAssigned)
			ASSERT(data->newestAvailableVersion.allEqual(keys, invalidVersion));
		wait(yield());
	}

	state RangeResult changeFeeds = fChangeFeeds.get();
	state int feedLoc;
	for (feedLoc = 0; feedLoc < changeFeeds.size(); feedLoc++) {
		Key changeFeedId = changeFeeds[feedLoc].key.removePrefix(persistChangeFeedKeys.begin);
		KeyRange changeFeedRange;
		Version popVersion;
		ChangeFeedStatus status;
		std::tie(changeFeedRange, popVersion, status) = decodeChangeFeedValue(changeFeeds[feedLoc].value);
		TraceEvent(SevDebug, "RestoringChangeFeed", data->thisServerID)
		    .detail("RangeID", changeFeedId.printable())
		    .detail("Range", changeFeedRange.toString())
		    .detail("Status", status)
		    .detail("PopVer", popVersion);
		Reference<ChangeFeedInfo> changeFeedInfo(new ChangeFeedInfo());
		changeFeedInfo->range = changeFeedRange;
		changeFeedInfo->id = changeFeedId;
		changeFeedInfo->durableVersion = version;
		changeFeedInfo->storageVersion = version;
		changeFeedInfo->emptyVersion = popVersion - 1;
		changeFeedInfo->stopped = status == ChangeFeedStatus::CHANGE_FEED_STOP;
		data->uidChangeFeed[changeFeedId] = changeFeedInfo;
		auto rs = data->keyChangeFeed.modify(changeFeedRange);
		for (auto r = rs.begin(); r != rs.end(); ++r) {
			r->value().push_back(changeFeedInfo);
		}
		wait(yield());
	}
	data->keyChangeFeed.coalesce(allKeys);
	// TODO: why is this seemingly random delay here?
	wait(delay(0.0001));

	{
		// Erase data which isn't available (it is from some fetch at a later version)
		// SOMEDAY: Keep track of keys that might be fetching, make sure we don't have any data elsewhere?
		for (auto it = data->newestAvailableVersion.ranges().begin(); it != data->newestAvailableVersion.ranges().end();
		     ++it) {
			if (it->value() == invalidVersion) {
				KeyRangeRef clearRange(it->begin(), it->end());
				// TODO(alexmiller): Figure out how to selectively enable spammy data distribution events.
				// DEBUG_KEY_RANGE("clearInvalidVersion", invalidVersion, clearRange);
				storage->clear(clearRange);
				data->byteSampleApplyClear(clearRange, invalidVersion);
			}
		}
	}

	validate(data, true);
	startByteSampleRestore.send(Void());

	return true;
}

Future<bool> StorageServerDisk::restoreDurableState() {
	return ::restoreDurableState(data, storage);
}

// Determines whether a key-value pair should be included in a byte sample
// Also returns size information about the sample
ByteSampleInfo isKeyValueInSample(KeyValueRef keyValue) {
	ByteSampleInfo info;

	const KeyRef key = keyValue.key;
	info.size = key.size() + keyValue.value.size();

	uint32_t a = 0;
	uint32_t b = 0;
	hashlittle2(key.begin(), key.size(), &a, &b);

	double probability =
	    (double)info.size / (key.size() + SERVER_KNOBS->BYTE_SAMPLING_OVERHEAD) / SERVER_KNOBS->BYTE_SAMPLING_FACTOR;
	info.inSample = a / ((1 << 30) * 4.0) < probability;
	info.sampledSize = info.size / std::min(1.0, probability);

	return info;
}

void StorageServer::addMutationToMutationLogOrStorage(Version ver, MutationRef m) {
	if (ver != invalidVersion) {
		addMutationToMutationLog(addVersionToMutationLog(ver), m);
	} else {
		storage.writeMutation(m);
		byteSampleApplyMutation(m, ver);
	}
}

void StorageServer::byteSampleApplySet(KeyValueRef kv, Version ver) {
	// Update byteSample in memory and (eventually) on disk and notify waiting metrics

	ByteSampleInfo sampleInfo = isKeyValueInSample(kv);
	auto& byteSample = metrics.byteSample.sample;

	int64_t delta = 0;
	const KeyRef key = kv.key;

	auto old = byteSample.find(key);
	if (old != byteSample.end())
		delta = -byteSample.getMetric(old);
	if (sampleInfo.inSample) {
		delta += sampleInfo.sampledSize;
		byteSample.insert(key, sampleInfo.sampledSize);
		addMutationToMutationLogOrStorage(ver,
		                                  MutationRef(MutationRef::SetValue,
		                                              key.withPrefix(persistByteSampleKeys.begin),
		                                              BinaryWriter::toValue(sampleInfo.sampledSize, Unversioned())));
	} else {
		bool any = old != byteSample.end();
		if (!byteSampleRecovery.isReady()) {
			if (!byteSampleClears.rangeContaining(key).value()) {
				byteSampleClears.insert(key, true);
				byteSampleClearsTooLarge.set(byteSampleClears.size() > SERVER_KNOBS->MAX_BYTE_SAMPLE_CLEAR_MAP_SIZE);
				any = true;
			}
		}
		if (any) {
			byteSample.erase(old);
			auto diskRange = singleKeyRange(key.withPrefix(persistByteSampleKeys.begin));
			addMutationToMutationLogOrStorage(ver,
			                                  MutationRef(MutationRef::ClearRange, diskRange.begin, diskRange.end));
		}
	}

	if (delta)
		metrics.notifyBytes(key, delta);
}

void StorageServer::byteSampleApplyClear(KeyRangeRef range, Version ver) {
	// Update byteSample in memory and (eventually) on disk via the mutationLog and notify waiting metrics

	auto& byteSample = metrics.byteSample.sample;
	bool any = false;

	if (range.begin < allKeys.end) {
		// NotifyBytes should not be called for keys past allKeys.end
		KeyRangeRef searchRange = KeyRangeRef(range.begin, std::min(range.end, allKeys.end));
		counters.sampledBytesCleared += byteSample.sumRange(searchRange.begin, searchRange.end);

		auto r = metrics.waitMetricsMap.intersectingRanges(searchRange);
		for (auto shard = r.begin(); shard != r.end(); ++shard) {
			KeyRangeRef intersectingRange = shard.range() & range;
			int64_t bytes = byteSample.sumRange(intersectingRange.begin, intersectingRange.end);
			metrics.notifyBytes(shard, -bytes);
			any = any || bytes > 0;
		}
	}

	if (range.end > allKeys.end && byteSample.sumRange(std::max(allKeys.end, range.begin), range.end) > 0)
		any = true;

	if (!byteSampleRecovery.isReady()) {
		auto clearRanges = byteSampleClears.intersectingRanges(range);
		for (auto it : clearRanges) {
			if (!it.value()) {
				byteSampleClears.insert(range, true);
				byteSampleClearsTooLarge.set(byteSampleClears.size() > SERVER_KNOBS->MAX_BYTE_SAMPLE_CLEAR_MAP_SIZE);
				any = true;
				break;
			}
		}
	}

	if (any) {
		byteSample.eraseAsync(range.begin, range.end);
		auto diskRange = range.withPrefix(persistByteSampleKeys.begin);
		addMutationToMutationLogOrStorage(ver, MutationRef(MutationRef::ClearRange, diskRange.begin, diskRange.end));
	}
}

ACTOR Future<Void> waitMetrics(StorageServerMetrics* self, WaitMetricsRequest req, Future<Void> timeout) {
	state PromiseStream<StorageMetrics> change;
	state StorageMetrics metrics = self->getMetrics(req.keys);
	state Error error = success();
	state bool timedout = false;

	if (!req.min.allLessOrEqual(metrics) || !metrics.allLessOrEqual(req.max)) {
		TEST(true); // ShardWaitMetrics return case 1 (quickly)
		req.reply.send(metrics);
		return Void();
	}

	{
		auto rs = self->waitMetricsMap.modify(req.keys);
		for (auto r = rs.begin(); r != rs.end(); ++r)
			r->value().push_back(change);
		loop {
			try {
				choose {
					when(StorageMetrics c = waitNext(change.getFuture())) {
						metrics += c;

						// SOMEDAY: validation! The changes here are possibly partial changes (we receive multiple
						// messages per
						//  update to our requested range). This means that the validation would have to occur after all
						//  the messages for one clear or set have been dispatched.

						/*StorageMetrics m = getMetrics( data, req.keys );
						  bool b = ( m.bytes != metrics.bytes || m.bytesPerKSecond != metrics.bytesPerKSecond ||
						  m.iosPerKSecond != metrics.iosPerKSecond ); if (b) { printf("keys: '%s' - '%s' @%p\n",
						  printable(req.keys.begin).c_str(), printable(req.keys.end).c_str(), this);
						  printf("waitMetrics: desync %d (%lld %lld %lld) != (%lld %lld %lld); +(%lld %lld %lld)\n", b,
						  m.bytes, m.bytesPerKSecond, m.iosPerKSecond, metrics.bytes, metrics.bytesPerKSecond,
						  metrics.iosPerKSecond, c.bytes, c.bytesPerKSecond, c.iosPerKSecond);

						  }*/
					}
					when(wait(timeout)) { timedout = true; }
				}
			} catch (Error& e) {
				if (e.code() == error_code_actor_cancelled)
					throw; // This is only cancelled when the main loop had exited...no need in this case to clean up
					       // self
				error = e;
				break;
			}

			if (timedout) {
				TEST(true); // ShardWaitMetrics return on timeout
				// FIXME: instead of using random chance, send wrong_shard_server when the call in from
				// waitMetricsMultiple (requires additional information in the request)
				if (deterministicRandom()->random01() < SERVER_KNOBS->WAIT_METRICS_WRONG_SHARD_CHANCE) {
					req.reply.sendError(wrong_shard_server());
				} else {
					req.reply.send(metrics);
				}
				break;
			}

			if (!req.min.allLessOrEqual(metrics) || !metrics.allLessOrEqual(req.max)) {
				TEST(true); // ShardWaitMetrics return case 2 (delayed)
				req.reply.send(metrics);
				break;
			}
		}

		wait(delay(0)); // prevent iterator invalidation of functions sending changes
	}

	auto rs = self->waitMetricsMap.modify(req.keys);
	for (auto i = rs.begin(); i != rs.end(); ++i) {
		auto& x = i->value();
		for (int j = 0; j < x.size(); j++) {
			if (x[j] == change) {
				swapAndPop(&x, j);
				break;
			}
		}
	}
	self->waitMetricsMap.coalesce(req.keys);

	if (error.code() != error_code_success) {
		if (error.code() != error_code_wrong_shard_server)
			throw error;
		TEST(true); // ShardWaitMetrics delayed wrong_shard_server()
		req.reply.sendError(error);
	}

	return Void();
}

Future<Void> StorageServerMetrics::waitMetrics(WaitMetricsRequest req, Future<Void> delay) {
	return ::waitMetrics(this, req, delay);
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

/////////////////////////////// Core //////////////////////////////////////
#ifndef __INTEL_COMPILER
#pragma region Core
#endif

ACTOR Future<Void> metricsCore(StorageServer* self, StorageServerInterface ssi) {
	state Future<Void> doPollMetrics = Void();

	wait(self->byteSampleRecovery);

	// Logs all counters in `counters.cc` and reset the interval.
	self->actors.add(traceCounters("StorageMetrics",
	                               self->thisServerID,
	                               SERVER_KNOBS->STORAGE_LOGGING_DELAY,
	                               &self->counters.cc,
	                               self->thisServerID.toString() + "/StorageMetrics",
	                               [self = self](TraceEvent& te) {
		                               te.detail("Tag", self->tag.toString());
		                               StorageBytes sb = self->storage.getStorageBytes();
		                               te.detail("KvstoreBytesUsed", sb.used);
		                               te.detail("KvstoreBytesFree", sb.free);
		                               te.detail("KvstoreBytesAvailable", sb.available);
		                               te.detail("KvstoreBytesTotal", sb.total);
		                               te.detail("KvstoreBytesTemp", sb.temp);
		                               if (self->isTss()) {
			                               te.detail("TSSPairID", self->tssPairID);
			                               te.detail("TSSJointID",
			                                         UID(self->thisServerID.first() ^ self->tssPairID.get().first(),
			                                             self->thisServerID.second() ^ self->tssPairID.get().second()));
		                               } else if (self->isSSWithTSSPair()) {
			                               te.detail("SSPairID", self->ssPairID);
			                               te.detail("TSSJointID",
			                                         UID(self->thisServerID.first() ^ self->ssPairID.get().first(),
			                                             self->thisServerID.second() ^ self->ssPairID.get().second()));
		                               }
	                               }));

	loop {
		choose {
			when(WaitMetricsRequest req = waitNext(ssi.waitMetrics.getFuture())) {
				if (!self->isReadable(req.keys)) {
					TEST(true); // waitMetrics immediate wrong_shard_server()
					self->sendErrorWithPenalty(req.reply, wrong_shard_server(), self->getPenalty());
				} else {
					self->actors.add(
					    self->metrics.waitMetrics(req, delayJittered(SERVER_KNOBS->STORAGE_METRIC_TIMEOUT)));
				}
			}
			when(SplitMetricsRequest req = waitNext(ssi.splitMetrics.getFuture())) {
				if (!self->isReadable(req.keys)) {
					TEST(true); // splitMetrics immediate wrong_shard_server()
					self->sendErrorWithPenalty(req.reply, wrong_shard_server(), self->getPenalty());
				} else {
					self->metrics.splitMetrics(req);
				}
			}
			when(GetStorageMetricsRequest req = waitNext(ssi.getStorageMetrics.getFuture())) {
				StorageBytes sb = self->storage.getStorageBytes();
				self->metrics.getStorageMetrics(
				    req, sb, self->counters.bytesInput.getRate(), self->versionLag, self->lastUpdate);
			}
			when(ReadHotSubRangeRequest req = waitNext(ssi.getReadHotRanges.getFuture())) {
				if (!self->isReadable(req.keys)) {
					TEST(true); // readHotSubRanges immediate wrong_shard_server()
					self->sendErrorWithPenalty(req.reply, wrong_shard_server(), self->getPenalty());
				} else {
					self->metrics.getReadHotRanges(req);
				}
			}
			when(SplitRangeRequest req = waitNext(ssi.getRangeSplitPoints.getFuture())) {
				if (!self->isReadable(req.keys)) {
					TEST(true); // getSplitPoints immediate wrong_shard_server()
					self->sendErrorWithPenalty(req.reply, wrong_shard_server(), self->getPenalty());
				} else {
					self->metrics.getSplitPoints(req);
				}
			}
			when(wait(doPollMetrics)) {
				self->metrics.poll();
				doPollMetrics = delay(SERVER_KNOBS->STORAGE_SERVER_POLL_METRICS_DELAY);
			}
		}
	}
}

ACTOR Future<Void> logLongByteSampleRecovery(Future<Void> recovery) {
	choose {
		when(wait(recovery)) {}
		when(wait(delay(SERVER_KNOBS->LONG_BYTE_SAMPLE_RECOVERY_DELAY))) {
			TraceEvent(g_network->isSimulated() ? SevWarn : SevWarnAlways, "LongByteSampleRecovery");
		}
	}

	return Void();
}

ACTOR Future<Void> checkBehind(StorageServer* self) {
	state int behindCount = 0;
	loop {
		wait(delay(SERVER_KNOBS->BEHIND_CHECK_DELAY));
		state Transaction tr(self->cx);
		loop {
			try {
				Version readVersion = wait(tr.getRawReadVersion());
				if (readVersion > self->version.get() + SERVER_KNOBS->BEHIND_CHECK_VERSIONS) {
					behindCount++;
				} else {
					behindCount = 0;
				}
				self->versionBehind = behindCount >= SERVER_KNOBS->BEHIND_CHECK_COUNT;
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}
	}
}

ACTOR Future<Void> serveGetValueRequests(StorageServer* self, FutureStream<GetValueRequest> getValue) {
	getCurrentLineage()->modify(&TransactionLineage::operation) = TransactionLineage::Operation::GetValue;
	loop {
		GetValueRequest req = waitNext(getValue);
		// Warning: This code is executed at extremely high priority (TaskPriority::LoadBalancedEndpoint), so downgrade
		// before doing real work
		if (req.debugID.present())
			g_traceBatch.addEvent("GetValueDebug",
			                      req.debugID.get().first(),
			                      "storageServer.received"); //.detail("TaskID", g_network->getCurrentTask());

		if (SHORT_CIRCUT_ACTUAL_STORAGE && normalKeys.contains(req.key))
			req.reply.send(GetValueReply());
		else
			self->actors.add(self->readGuard(req, getValueQ));
	}
}

ACTOR Future<Void> serveGetKeyValuesRequests(StorageServer* self, FutureStream<GetKeyValuesRequest> getKeyValues) {
	getCurrentLineage()->modify(&TransactionLineage::operation) = TransactionLineage::Operation::GetKeyValues;
	loop {
		GetKeyValuesRequest req = waitNext(getKeyValues);

		// Warning: This code is executed at extremely high priority (TaskPriority::LoadBalancedEndpoint), so downgrade
		// before doing real work
		self->actors.add(self->readGuard(req, getKeyValuesQ));
	}
}

ACTOR Future<Void> serveGetKeyValuesAndFlatMapRequests(
    StorageServer* self,
    FutureStream<GetKeyValuesAndFlatMapRequest> getKeyValuesAndFlatMap) {
	// TODO: Is it fine to keep TransactionLineage::Operation::GetKeyValues here?
	getCurrentLineage()->modify(&TransactionLineage::operation) = TransactionLineage::Operation::GetKeyValues;
	loop {
		GetKeyValuesAndFlatMapRequest req = waitNext(getKeyValuesAndFlatMap);

		// Warning: This code is executed at extremely high priority (TaskPriority::LoadBalancedEndpoint), so downgrade
		// before doing real work
		self->actors.add(self->readGuard(req, getKeyValuesAndFlatMapQ));
	}
}

ACTOR Future<Void> serveGetKeyValuesStreamRequests(StorageServer* self,
                                                   FutureStream<GetKeyValuesStreamRequest> getKeyValuesStream) {
	loop {
		GetKeyValuesStreamRequest req = waitNext(getKeyValuesStream);
		// Warning: This code is executed at extremely high priority (TaskPriority::LoadBalancedEndpoint), so downgrade
		// before doing real work
		// FIXME: add readGuard again
		self->actors.add(getKeyValuesStreamQ(self, req));
	}
}

ACTOR Future<Void> serveGetKeyRequests(StorageServer* self, FutureStream<GetKeyRequest> getKey) {
	getCurrentLineage()->modify(&TransactionLineage::operation) = TransactionLineage::Operation::GetKey;
	loop {
		GetKeyRequest req = waitNext(getKey);
		// Warning: This code is executed at extremely high priority (TaskPriority::LoadBalancedEndpoint), so downgrade
		// before doing real work
		self->actors.add(self->readGuard(req, getKeyQ));
	}
}

ACTOR Future<Void> watchValueWaitForVersion(StorageServer* self,
                                            WatchValueRequest req,
                                            PromiseStream<WatchValueRequest> stream) {
	state Span span("SS:watchValueWaitForVersion"_loc, { req.spanContext });
	getCurrentLineage()->modify(&TransactionLineage::txID) = req.spanContext.first();
	try {
		wait(success(waitForVersionNoTooOld(self, req.version)));
		stream.send(req);
	} catch (Error& e) {
		if (!canReplyWith(e))
			throw e;
		self->sendErrorWithPenalty(req.reply, e, self->getPenalty());
	}
	return Void();
}

ACTOR Future<Void> serveWatchValueRequestsImpl(StorageServer* self, FutureStream<WatchValueRequest> stream) {
	loop {
		getCurrentLineage()->modify(&TransactionLineage::txID) = 0;
		state WatchValueRequest req = waitNext(stream);
		state Reference<ServerWatchMetadata> metadata = self->getWatchMetadata(req.key.contents());
		state Span span("SS:serveWatchValueRequestsImpl"_loc, { req.spanContext });
		getCurrentLineage()->modify(&TransactionLineage::txID) = req.spanContext.first();

		if (!metadata.isValid()) { // case 1: no watch set for the current key
			metadata = makeReference<ServerWatchMetadata>(req.key, req.value, req.version, req.tags, req.debugID);
			KeyRef key = self->setWatchMetadata(metadata);
			metadata->watch_impl = forward(watchWaitForValueChange(self, span.context, key), metadata->versionPromise);
			self->actors.add(watchValueSendReply(self, req, metadata->versionPromise.getFuture(), span.context));
		} else if (metadata->value ==
		           req.value) { // case 2: there is a watch in the map and it has the same value so just update version
			if (req.version > metadata->version) {
				metadata->version = req.version;
				metadata->tags = req.tags;
				metadata->debugID = req.debugID;
			}
			self->actors.add(watchValueSendReply(self, req, metadata->versionPromise.getFuture(), span.context));
		} else if (req.version > metadata->version) { // case 3: version in map has a lower version so trigger watch and
			                                          // create a new entry in map
			self->deleteWatchMetadata(req.key.contents());
			metadata->versionPromise.send(req.version);
			metadata->watch_impl.cancel();

			metadata = makeReference<ServerWatchMetadata>(req.key, req.value, req.version, req.tags, req.debugID);
			KeyRef key = self->setWatchMetadata(metadata);
			metadata->watch_impl = forward(watchWaitForValueChange(self, span.context, key), metadata->versionPromise);

			self->actors.add(watchValueSendReply(self, req, metadata->versionPromise.getFuture(), span.context));
		} else if (req.version <
		           metadata->version) { // case 4: version in the map is higher so immediately trigger watch
			TEST(true); // watch version in map is higher so trigger watch (case 4)
			req.reply.send(WatchValueReply{ metadata->version });
		} else { // case 5: watch value differs but their versions are the same (rare case) so check with the SS
			TEST(true); // watch version in the map is the same but value is different (case 5)
			loop {
				try {
					state Version latest = self->version.get();
					GetValueRequest getReq(span.context, metadata->key, latest, metadata->tags, metadata->debugID);
					state Future<Void> getValue = getValueQ(self, getReq);
					GetValueReply reply = wait(getReq.reply.getFuture());
					metadata = self->getWatchMetadata(req.key.contents());

					if (metadata.isValid() && reply.value != metadata->value) { // valSS != valMap
						self->deleteWatchMetadata(req.key.contents());
						metadata->versionPromise.send(req.version);
						metadata->watch_impl.cancel();
					}

					if (reply.value == req.value) { // valSS == valreq
						metadata =
						    makeReference<ServerWatchMetadata>(req.key, req.value, req.version, req.tags, req.debugID);
						KeyRef key = self->setWatchMetadata(metadata);
						metadata->watch_impl =
						    forward(watchWaitForValueChange(self, span.context, key), metadata->versionPromise);
						self->actors.add(
						    watchValueSendReply(self, req, metadata->versionPromise.getFuture(), span.context));
					} else {
						req.reply.send(WatchValueReply{ latest });
					}
					break;
				} catch (Error& e) {
					if (e.code() != error_code_transaction_too_old) {
						if (!canReplyWith(e))
							throw e;
						self->sendErrorWithPenalty(req.reply, e, self->getPenalty());
						break;
					}
					TEST(true); // Reading a watched key failed with transaction_too_old case 5
				}
			}
		}
	}
}

ACTOR Future<Void> serveWatchValueRequests(StorageServer* self, FutureStream<WatchValueRequest> watchValue) {
	state PromiseStream<WatchValueRequest> stream;
	getCurrentLineage()->modify(&TransactionLineage::operation) = TransactionLineage::Operation::WatchValue;
	self->actors.add(serveWatchValueRequestsImpl(self, stream.getFuture()));

	loop {
		WatchValueRequest req = waitNext(watchValue);
		// TODO: fast load balancing?
		if (self->shouldRead(req)) {
			self->actors.add(watchValueWaitForVersion(self, req, stream));
		}
	}
}

ACTOR Future<Void> serveChangeFeedStreamRequests(StorageServer* self,
                                                 FutureStream<ChangeFeedStreamRequest> changeFeedStream) {
	loop {
		ChangeFeedStreamRequest req = waitNext(changeFeedStream);
		self->actors.add(changeFeedStreamQ(self, req));
	}
}

ACTOR Future<Void> serveOverlappingChangeFeedsRequests(
    StorageServer* self,
    FutureStream<OverlappingChangeFeedsRequest> overlappingChangeFeeds) {
	loop {
		OverlappingChangeFeedsRequest req = waitNext(overlappingChangeFeeds);
		self->actors.add(self->readGuard(req, overlappingChangeFeedsQ));
	}
}

ACTOR Future<Void> serveChangeFeedPopRequests(StorageServer* self, FutureStream<ChangeFeedPopRequest> changeFeedPops) {
	loop {
		ChangeFeedPopRequest req = waitNext(changeFeedPops);
		self->actors.add(self->readGuard(req, changeFeedPopQ));
	}
}

ACTOR Future<Void> serveChangeFeedVersionUpdateRequests(
    StorageServer* self,
    FutureStream<ChangeFeedVersionUpdateRequest> changeFeedVersionUpdate) {
	loop {
		ChangeFeedVersionUpdateRequest req = waitNext(changeFeedVersionUpdate);
		self->actors.add(self->readGuard(req, changeFeedVersionUpdateQ));
	}
}

ACTOR Future<Void> reportStorageServerState(StorageServer* self) {
	if (!SERVER_KNOBS->REPORT_DD_METRICS) {
		return Void();
	}

	loop {
		wait(delay(SERVER_KNOBS->DD_METRICS_REPORT_INTERVAL));

		const auto numRunningFetchKeys = self->currentRunningFetchKeys.numRunning();
		if (numRunningFetchKeys == 0) {
			continue;
		}

		const auto longestRunningFetchKeys = self->currentRunningFetchKeys.longestTime();

		auto level = SevInfo;
		if (longestRunningFetchKeys.first >= SERVER_KNOBS->FETCH_KEYS_TOO_LONG_TIME_CRITERIA) {
			level = SevWarnAlways;
		}

		TraceEvent(level, "FetchKeysCurrentStatus", self->thisServerID)
		    .detail("Timestamp", now())
		    .detail("LongestRunningTime", longestRunningFetchKeys.first)
		    .detail("StartKey", longestRunningFetchKeys.second.begin)
		    .detail("EndKey", longestRunningFetchKeys.second.end)
		    .detail("NumRunning", numRunningFetchKeys);
	}
}

ACTOR Future<Void> storageServerCore(StorageServer* self, StorageServerInterface ssi) {
	state Future<Void> doUpdate = Void();
	state bool updateReceived =
	    false; // true iff the current update() actor assigned to doUpdate has already received an update from the tlog
	state double lastLoopTopTime = now();
	state Future<Void> dbInfoChange = Void();
	state Future<Void> checkLastUpdate = Void();
	state Future<Void> updateProcessStatsTimer = delay(SERVER_KNOBS->FASTRESTORE_UPDATE_PROCESS_STATS_INTERVAL);

	self->actors.add(updateStorage(self));
	self->actors.add(waitFailureServer(ssi.waitFailure.getFuture()));
	self->actors.add(self->otherError.getFuture());
	self->actors.add(metricsCore(self, ssi));
	self->actors.add(logLongByteSampleRecovery(self->byteSampleRecovery));
	self->actors.add(checkBehind(self));
	self->actors.add(serveGetValueRequests(self, ssi.getValue.getFuture()));
	self->actors.add(serveGetKeyValuesRequests(self, ssi.getKeyValues.getFuture()));
	self->actors.add(serveGetKeyValuesAndFlatMapRequests(self, ssi.getKeyValuesAndFlatMap.getFuture()));
	self->actors.add(serveGetKeyValuesStreamRequests(self, ssi.getKeyValuesStream.getFuture()));
	self->actors.add(serveGetKeyRequests(self, ssi.getKey.getFuture()));
	self->actors.add(serveWatchValueRequests(self, ssi.watchValue.getFuture()));
	self->actors.add(serveChangeFeedStreamRequests(self, ssi.changeFeedStream.getFuture()));
	self->actors.add(serveOverlappingChangeFeedsRequests(self, ssi.overlappingChangeFeeds.getFuture()));
	self->actors.add(serveChangeFeedPopRequests(self, ssi.changeFeedPop.getFuture()));
	self->actors.add(serveChangeFeedVersionUpdateRequests(self, ssi.changeFeedVersionUpdate.getFuture()));
	self->actors.add(traceRole(Role::STORAGE_SERVER, ssi.id()));
	self->actors.add(reportStorageServerState(self));

	self->transactionTagCounter.startNewInterval();
	self->actors.add(
	    recurring([&]() { self->transactionTagCounter.startNewInterval(); }, SERVER_KNOBS->TAG_MEASUREMENT_INTERVAL));

	self->coreStarted.send(Void());

	loop {
		++self->counters.loops;

		double loopTopTime = now();
		double elapsedTime = loopTopTime - lastLoopTopTime;
		if (elapsedTime > 0.050) {
			if (deterministicRandom()->random01() < 0.01)
				TraceEvent(SevWarn, "SlowSSLoopx100", self->thisServerID).detail("Elapsed", elapsedTime);
		}
		lastLoopTopTime = loopTopTime;

		choose {
			when(wait(checkLastUpdate)) {
				if (now() - self->lastUpdate >= CLIENT_KNOBS->NO_RECENT_UPDATES_DURATION) {
					self->noRecentUpdates.set(true);
					checkLastUpdate = delay(CLIENT_KNOBS->NO_RECENT_UPDATES_DURATION);
				} else {
					checkLastUpdate =
					    delay(std::max(CLIENT_KNOBS->NO_RECENT_UPDATES_DURATION - (now() - self->lastUpdate), 0.1));
				}
			}
			when(wait(dbInfoChange)) {
				TEST(self->logSystem); // shardServer dbInfo changed
				dbInfoChange = self->db->onChange();
				if (self->db->get().recoveryState >= RecoveryState::ACCEPTING_COMMITS) {
					self->logSystem = ILogSystem::fromServerDBInfo(self->thisServerID, self->db->get());
					if (self->logSystem) {
						if (self->db->get().logSystemConfig.recoveredAt.present()) {
							self->poppedAllAfter = self->db->get().logSystemConfig.recoveredAt.get();
						}
						self->logCursor = self->logSystem->peekSingle(
						    self->thisServerID, self->version.get() + 1, self->tag, self->history);
						self->popVersion(self->durableVersion.get() + 1, true);
					}
					// If update() is waiting for results from the tlog, it might never get them, so needs to be
					// cancelled.  But if it is waiting later, cancelling it could cause problems (e.g. fetchKeys that
					// already committed to transitioning to waiting state)
					if (!updateReceived) {
						doUpdate = Void();
					}
				}

				Optional<LatencyBandConfig> newLatencyBandConfig = self->db->get().latencyBandConfig;
				if (newLatencyBandConfig.present() != self->latencyBandConfig.present() ||
				    (newLatencyBandConfig.present() &&
				     newLatencyBandConfig.get().readConfig != self->latencyBandConfig.get().readConfig)) {
					self->latencyBandConfig = newLatencyBandConfig;
					self->counters.readLatencyBands.clearBands();
					TraceEvent("LatencyBandReadUpdatingConfig").detail("Present", newLatencyBandConfig.present());
					if (self->latencyBandConfig.present()) {
						for (auto band : self->latencyBandConfig.get().readConfig.bands) {
							self->counters.readLatencyBands.addThreshold(band);
						}
					}
				}
			}
			when(GetShardStateRequest req = waitNext(ssi.getShardState.getFuture())) {
				if (req.mode == GetShardStateRequest::NO_WAIT) {
					if (self->isReadable(req.keys))
						req.reply.send(GetShardStateReply{ self->version.get(), self->durableVersion.get() });
					else
						req.reply.sendError(wrong_shard_server());
				} else {
					self->actors.add(getShardStateQ(self, req));
				}
			}
			when(StorageQueuingMetricsRequest req = waitNext(ssi.getQueuingMetrics.getFuture())) {
				getQueuingMetrics(self, req);
			}
			when(ReplyPromise<KeyValueStoreType> reply = waitNext(ssi.getKeyValueStoreType.getFuture())) {
				reply.send(self->storage.getKeyValueStoreType());
			}
			when(wait(doUpdate)) {
				updateReceived = false;
				if (!self->logSystem)
					doUpdate = Never();
				else
					doUpdate = update(self, &updateReceived);
			}
			when(wait(updateProcessStatsTimer)) {
				updateProcessStats(self);
				updateProcessStatsTimer = delay(SERVER_KNOBS->FASTRESTORE_UPDATE_PROCESS_STATS_INTERVAL);
			}
			when(wait(self->actors.getResult())) {}
		}
	}
}

bool storageServerTerminated(StorageServer& self, IKeyValueStore* persistentData, Error const& e) {
	self.shuttingDown = true;

	// Clearing shards shuts down any fetchKeys actors; these may do things on cancellation that are best done with self
	// still valid
	self.shards.insert(allKeys, Reference<ShardInfo>());

	// Dispose the IKVS (destroying its data permanently) only if this shutdown is definitely permanent.  Otherwise just
	// close it.
	if (e.code() == error_code_please_reboot) {
		// do nothing.
	} else if (e.code() == error_code_worker_removed || e.code() == error_code_recruitment_failed) {
		// SOMEDAY: could close instead of dispose if tss in quarantine gets removed so it could still be investigated?
		persistentData->dispose();
	} else {
		persistentData->close();
	}

	if (e.code() == error_code_worker_removed || e.code() == error_code_recruitment_failed ||
	    e.code() == error_code_file_not_found || e.code() == error_code_actor_cancelled) {
		TraceEvent("StorageServerTerminated", self.thisServerID).error(e, true);
		return true;
	} else
		return false;
}

ACTOR Future<Void> memoryStoreRecover(IKeyValueStore* store, Reference<IClusterConnectionRecord> connRecord, UID id) {
	if (store->getType() != KeyValueStoreType::MEMORY || connRecord.getPtr() == nullptr) {
		return Never();
	}

	// create a temp client connect to DB
	Database cx = Database::createDatabase(connRecord, Database::API_VERSION_LATEST);

	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(cx);
	state int noCanRemoveCount = 0;
	loop {
		try {
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);

			state bool canRemove = wait(canRemoveStorageServer(tr, id));
			if (!canRemove) {
				TEST(true); // it's possible that the caller had a transaction in flight that assigned keys to the
				            // server. Wait for it to reverse its mistake.
				wait(delayJittered(SERVER_KNOBS->REMOVE_RETRY_DELAY, TaskPriority::UpdateStorage));
				tr->reset();
				TraceEvent("RemoveStorageServerRetrying")
				    .detail("Count", noCanRemoveCount++)
				    .detail("ServerID", id)
				    .detail("CanRemove", canRemove);
			} else {
				return Void();
			}
		} catch (Error& e) {
			state Error err = e;
			wait(tr->onError(e));
			TraceEvent("RemoveStorageServerRetrying").error(err);
		}
	}
}

// for creating a new storage server
ACTOR Future<Void> storageServer(IKeyValueStore* persistentData,
                                 StorageServerInterface ssi,
                                 Tag seedTag,
                                 UID clusterId,
                                 Version tssSeedVersion,
                                 ReplyPromise<InitializeStorageReply> recruitReply,
                                 Reference<AsyncVar<ServerDBInfo> const> db,
                                 std::string folder) {
	state StorageServer self(persistentData, db, ssi);
	state Future<Void> ssCore;
	self.clusterId.send(clusterId);
	if (ssi.isTss()) {
		self.setTssPair(ssi.tssPairID.get());
		ASSERT(self.isTss());
	}

	self.sk = serverKeysPrefixFor(self.tssPairID.present() ? self.tssPairID.get() : self.thisServerID)
	              .withPrefix(systemKeys.begin); // FFFF/serverKeys/[this server]/
	self.folder = folder;

	try {
		wait(self.storage.init());
		wait(self.storage.commit());

		if (seedTag == invalidTag) {
			std::pair<Version, Tag> verAndTag = wait(addStorageServer(
			    self.cx, ssi)); // Might throw recruitment_failed in case of simultaneous master failure
			self.tag = verAndTag.second;
			if (ssi.isTss()) {
				self.setInitialVersion(tssSeedVersion);
			} else {
				self.setInitialVersion(verAndTag.first - 1);
			}
		} else {
			self.tag = seedTag;
		}

		self.storage.makeNewStorageServerDurable();
		wait(self.storage.commit());

		TraceEvent("StorageServerInit", ssi.id())
		    .detail("Version", self.version.get())
		    .detail("SeedTag", seedTag.toString())
		    .detail("TssPair", ssi.isTss() ? ssi.tssPairID.get().toString() : "");
		InitializeStorageReply rep;
		rep.interf = ssi;
		rep.addedVersion = self.version.get();
		recruitReply.send(rep);
		self.byteSampleRecovery = Void();

		ssCore = storageServerCore(&self, ssi);
		wait(ssCore);

		throw internal_error();
	} catch (Error& e) {
		// If we die with an error before replying to the recruitment request, send the error to the recruiter
		// (ClusterController, and from there to the DataDistributionTeamCollection)
		if (!recruitReply.isSet())
			recruitReply.sendError(recruitment_failed());

		// If the storage server dies while something that uses self is still on the stack,
		// we want that actor to complete before we terminate and that memory goes out of scope
		state Error err = e;
		if (storageServerTerminated(self, persistentData, err)) {
			ssCore.cancel();
			self.actors.clear(true);
			wait(delay(0));
			return Void();
		}
		ssCore.cancel();
		self.actors.clear(true);
		wait(delay(0));
		throw err;
	}
}

ACTOR Future<Void> replaceInterface(StorageServer* self, StorageServerInterface ssi) {
	ASSERT(!ssi.isTss());
	state Transaction tr(self->cx);

	loop {
		state Future<Void> infoChanged = self->db->onChange();
		state Reference<CommitProxyInfo> commitProxies(
		    new CommitProxyInfo(self->db->get().client.commitProxies, false));
		choose {
			when(GetStorageServerRejoinInfoReply _rep =
			         wait(commitProxies->size()
			                  ? basicLoadBalance(commitProxies,
			                                     &CommitProxyInterface::getStorageServerRejoinInfo,
			                                     GetStorageServerRejoinInfoRequest(ssi.id(), ssi.locality.dcId()))
			                  : Never())) {
				state GetStorageServerRejoinInfoReply rep = _rep;

				try {
					tr.reset();
					tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
					tr.setVersion(rep.version);

					tr.addReadConflictRange(singleKeyRange(serverListKeyFor(ssi.id())));
					tr.addReadConflictRange(singleKeyRange(serverTagKeyFor(ssi.id())));
					tr.addReadConflictRange(serverTagHistoryRangeFor(ssi.id()));
					tr.addReadConflictRange(singleKeyRange(tagLocalityListKeyFor(ssi.locality.dcId())));

					tr.set(serverListKeyFor(ssi.id()), serverListValue(ssi));

					if (rep.newLocality) {
						tr.addReadConflictRange(tagLocalityListKeys);
						tr.set(tagLocalityListKeyFor(ssi.locality.dcId()),
						       tagLocalityListValue(rep.newTag.get().locality));
					}

					// this only should happen if SS moved datacenters
					if (rep.newTag.present()) {
						KeyRange conflictRange = singleKeyRange(serverTagConflictKeyFor(rep.newTag.get()));
						tr.addReadConflictRange(conflictRange);
						tr.addWriteConflictRange(conflictRange);
						tr.setOption(FDBTransactionOptions::FIRST_IN_BATCH);
						tr.set(serverTagKeyFor(ssi.id()), serverTagValue(rep.newTag.get()));
						tr.atomicOp(serverTagHistoryKeyFor(ssi.id()),
						            serverTagValue(rep.tag),
						            MutationRef::SetVersionstampedKey);
					}

					if (rep.history.size() && rep.history.back().first < self->version.get()) {
						tr.clear(serverTagHistoryRangeBefore(ssi.id(), self->version.get()));
					}

					choose {
						when(wait(tr.commit())) {
							self->history = rep.history;

							if (rep.newTag.present()) {
								self->tag = rep.newTag.get();
								self->history.insert(self->history.begin(),
								                     std::make_pair(tr.getCommittedVersion(), rep.tag));
							} else {
								self->tag = rep.tag;
							}
							self->allHistory = self->history;

							TraceEvent("SSTag", self->thisServerID).detail("MyTag", self->tag.toString());
							for (auto it : self->history) {
								TraceEvent("SSHistory", self->thisServerID)
								    .detail("Ver", it.first)
								    .detail("Tag", it.second.toString());
							}

							if (self->history.size() && BUGGIFY) {
								TraceEvent("SSHistoryReboot", self->thisServerID).log();
								throw please_reboot();
							}

							break;
						}
						when(wait(infoChanged)) {}
					}
				} catch (Error& e) {
					wait(tr.onError(e));
				}
			}
			when(wait(infoChanged)) {}
		}
	}

	return Void();
}

ACTOR Future<Void> replaceTSSInterface(StorageServer* self, StorageServerInterface ssi) {
	// RYW for KeyBackedMap
	state Reference<ReadYourWritesTransaction> tr = makeReference<ReadYourWritesTransaction>(self->cx);
	state KeyBackedMap<UID, UID> tssMapDB = KeyBackedMap<UID, UID>(tssMappingKeys.begin);

	ASSERT(ssi.isTss());

	loop {
		try {
			state Tag myTag;

			tr->reset();
			tr->setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
			tr->setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);

			Optional<Value> pairTagValue = wait(tr->get(serverTagKeyFor(self->tssPairID.get())));

			if (!pairTagValue.present()) {
				TEST(true); // Race where tss was down, pair was removed, tss starts back up
				TraceEvent("StorageServerWorkerRemoved", self->thisServerID).detail("Reason", "TssPairMissing");
				throw worker_removed();
			}

			myTag = decodeServerTagValue(pairTagValue.get());

			tr->addReadConflictRange(singleKeyRange(serverListKeyFor(ssi.id())));
			tr->set(serverListKeyFor(ssi.id()), serverListValue(ssi));

			// add itself back to tss mapping
			if (!self->isTSSInQuarantine()) {
				tssMapDB.set(tr, self->tssPairID.get(), ssi.id());
			}

			wait(tr->commit());
			self->tag = myTag;

			break;
		} catch (Error& e) {
			wait(tr->onError(e));
		}
	}

	return Void();
}

// for recovering an existing storage server
ACTOR Future<Void> storageServer(IKeyValueStore* persistentData,
                                 StorageServerInterface ssi,
                                 Reference<AsyncVar<ServerDBInfo> const> db,
                                 std::string folder,
                                 Promise<Void> recovered,
                                 Reference<IClusterConnectionRecord> connRecord) {
	state StorageServer self(persistentData, db, ssi);
	state Future<Void> ssCore;
	self.folder = folder;

	try {
		state double start = now();
		TraceEvent("StorageServerRebootStart", self.thisServerID).log();

		wait(self.storage.init());
		choose {
			// after a rollback there might be uncommitted changes.
			// for memory storage engine type, wait until recovery is done before commit
			when(wait(self.storage.commit())) {}

			when(wait(memoryStoreRecover(persistentData, connRecord, self.thisServerID))) {
				TraceEvent("DisposeStorageServer", self.thisServerID).log();
				throw worker_removed();
			}
		}

		bool ok = wait(self.storage.restoreDurableState());
		if (!ok) {
			if (recovered.canBeSet())
				recovered.send(Void());
			return Void();
		}
		TraceEvent("SSTimeRestoreDurableState", self.thisServerID).detail("TimeTaken", now() - start);

		// if this is a tss storage file, use that as source of truth for this server being a tss instead of the
		// presence of the tss pair key in the storage engine
		if (ssi.isTss()) {
			ASSERT(self.isTss());
			ssi.tssPairID = self.tssPairID.get();
		} else {
			ASSERT(!self.isTss());
		}

		ASSERT(self.thisServerID == ssi.id());

		self.sk = serverKeysPrefixFor(self.tssPairID.present() ? self.tssPairID.get() : self.thisServerID)
		              .withPrefix(systemKeys.begin); // FFFF/serverKeys/[this server]/

		TraceEvent("StorageServerReboot", self.thisServerID).detail("Version", self.version.get());

		if (recovered.canBeSet())
			recovered.send(Void());

		try {
			if (self.isTss()) {
				wait(replaceTSSInterface(&self, ssi));
			} else {
				wait(replaceInterface(&self, ssi));
			}
		} catch (Error& e) {
			if (e.code() != error_code_worker_removed) {
				throw;
			}
			state UID clusterId = wait(getClusterId(&self));
			ASSERT(self.clusterId.isValid());
			UID durableClusterId = wait(self.clusterId.getFuture());
			ASSERT(durableClusterId.isValid());
			if (clusterId == durableClusterId) {
				throw worker_removed();
			}
			// When a storage server connects to a new cluster, it deletes its
			// old data and creates a new, empty data file for the new cluster.
			// We want to avoid this and force a manual removal of the storage
			// servers' old data when being assigned to a new cluster to avoid
			// accidental data loss.
			TraceEvent(SevError, "StorageServerBelongsToExistingCluster")
			    .detail("ClusterID", durableClusterId)
			    .detail("NewClusterID", clusterId);
			wait(Future<Void>(Never()));
		}

		TraceEvent("StorageServerStartingCore", self.thisServerID).detail("TimeTaken", now() - start);

		// wait( delay(0) );  // To make sure self->zkMasterInfo.onChanged is available to wait on
		ssCore = storageServerCore(&self, ssi);
		wait(ssCore);

		throw internal_error();
	} catch (Error& e) {
		if (recovered.canBeSet())
			recovered.send(Void());

		// If the storage server dies while something that uses self is still on the stack,
		// we want that actor to complete before we terminate and that memory goes out of scope
		state Error err = e;
		if (storageServerTerminated(self, persistentData, err)) {
			ssCore.cancel();
			self.actors.clear(true);
			wait(delay(0));
			return Void();
		}
		ssCore.cancel();
		self.actors.clear(true);
		wait(delay(0));
		throw err;
	}
}

#ifndef __INTEL_COMPILER
#pragma endregion
#endif

/*
4 Reference count
4 priority
24 pointers
8 lastUpdateVersion
2 updated, replacedPointer
--
42 PTree overhead

8 Version insertVersion
--
50 VersionedMap overhead

12 KeyRef
12 ValueRef
1  isClear
--
25 payload


50 overhead
25 payload
21 structure padding
32 allocator rounds up
---
128 allocated

To reach 64, need to save: 11 bytes + all padding

Possibilities:
  -8 Combine lastUpdateVersion, insertVersion?
  -2 Fold together updated, replacedPointer, isClear bits
  -3 Fold away updated, replacedPointer, isClear
  -8 Move value lengths into arena
  -4 Replace priority with H(pointer)
  -12 Compress pointers (using special allocator)
  -4 Modular lastUpdateVersion (make sure no node survives 4 billion updates)
*/

void versionedMapTest() {
	VersionedMap<int, int> vm;

	printf("SS Ptree node is %zu bytes\n", sizeof(StorageServer::VersionedData::PTreeT));

	const int NSIZE = sizeof(VersionedMap<int, int>::PTreeT);
	const int ASIZE = NSIZE <= 64 ? 64 : nextFastAllocatedSize(NSIZE);

	auto before = FastAllocator<ASIZE>::getTotalMemory();

	for (int v = 1; v <= 1000; ++v) {
		vm.createNewVersion(v);
		for (int i = 0; i < 1000; i++) {
			int k = deterministicRandom()->randomInt(0, 2000000);
			/*for(int k2=k-5; k2<k+5; k2++)
			    if (vm.atLatest().find(k2) != vm.atLatest().end())
			        vm.erase(k2);*/
			vm.erase(k - 5, k + 5);
			vm.insert(k, v);
		}
	}

	auto after = FastAllocator<ASIZE>::getTotalMemory();

	int count = 0;
	for (auto i = vm.atLatest().begin(); i != vm.atLatest().end(); ++i)
		++count;

	printf("PTree node is %d bytes, allocated as %d bytes\n", NSIZE, ASIZE);
	printf("%d distinct after %d insertions\n", count, 1000 * 1000);
	printf("Memory used: %f MB\n", (after - before) / 1e6);
}
