/*
 * RestoreLoader.actor.cpp
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

// This file implements the functions and actors used by the RestoreLoader role.
// The RestoreLoader role starts with the restoreLoaderCore actor

#include "fdbclient/BackupContainer.h"
#include "fdbserver/RestoreLoader.actor.h"

#include "flow/actorcompiler.h"  // This must be the last #include.

typedef std::map<Version, Standalone<VectorRef<MutationRef>>> VersionedMutationsMap;

ACTOR Future<Void> handleSetApplierKeyRangeVectorRequest(RestoreSetApplierKeyRangeVectorRequest req, Reference<RestoreLoaderData> self);
ACTOR Future<Void> handleLoadFileRequest(RestoreLoadFileRequest req, Reference<RestoreLoaderData> self, bool isSampling = false);
ACTOR static Future<Void> _parseLogFileToMutationsOnLoader(std::map<Standalone<StringRef>, Standalone<StringRef>> *mutationMap,
									std::map<Standalone<StringRef>, uint32_t> *mutationPartMap,
 									Reference<IBackupContainer> bc, Version version,
 									std::string fileName, int64_t readOffset, int64_t readLen,
 									KeyRange restoreRange, Key addPrefix, Key removePrefix,
 									Key mutationLogPrefix);									 
ACTOR static Future<Void> _parseRangeFileToMutationsOnLoader(std::map<Version, Standalone<VectorRef<MutationRef>>> *kvOps,
 									Reference<IBackupContainer> bc, Version version,
 									std::string fileName, int64_t readOffset_input, int64_t readLen_input,
 									KeyRange restoreRange, Key addPrefix, Key removePrefix);	
ACTOR Future<Void> registerMutationsToApplier(Reference<RestoreLoaderData> self,
									std::map<Version, Standalone<VectorRef<MutationRef>>> *kvOps,
									bool isRangeFile, Version startVersion, Version endVersion);
 void _parseSerializedMutation(std::map<Version, Standalone<VectorRef<MutationRef>>> *kvOps,
	 						 std::map<Standalone<StringRef>, Standalone<StringRef>> *mutationMap,
							 bool isSampling = false);
bool isRangeMutation(MutationRef m);
void splitMutation(Reference<RestoreLoaderData> self,  MutationRef m, Arena& mvector_arena, VectorRef<MutationRef>& mvector, Arena& nodeIDs_arena, VectorRef<UID>& nodeIDs) ;


ACTOR Future<Void> restoreLoaderCore(Reference<RestoreLoaderData> self, RestoreLoaderInterface loaderInterf, Database cx) {
	state ActorCollection actors(false);
	state Future<Void> exitRole = Never();
	state double lastLoopTopTime;
	loop {
		
		double loopTopTime = now();
		double elapsedTime = loopTopTime - lastLoopTopTime;
		if( elapsedTime > 0.050 ) {
			if (g_random->random01() < 0.01)
				TraceEvent(SevWarn, "SlowRestoreLoaderLoopx100").detail("NodeDesc", self->describeNode()).detail("Elapsed", elapsedTime);
		}
		lastLoopTopTime = loopTopTime;
		state std::string requestTypeStr = "[Init]";

		try {
			choose {
				when ( RestoreSimpleRequest req = waitNext(loaderInterf.heartbeat.getFuture()) ) {
					requestTypeStr = "heartbeat";
					actors.add(handleHeartbeat(req, loaderInterf.id()));
				}
				when ( RestoreSetApplierKeyRangeVectorRequest req = waitNext(loaderInterf.setApplierKeyRangeVectorRequest.getFuture()) ) {
					requestTypeStr = "setApplierKeyRangeVectorRequest";
					actors.add(handleSetApplierKeyRangeVectorRequest(req, self));
				}
				when ( RestoreLoadFileRequest req = waitNext(loaderInterf.loadFile.getFuture()) ) {
					requestTypeStr = "loadFile";
					self->initBackupContainer(req.param.url);
					actors.add( handleLoadFileRequest(req, self, false) );
				}
				when ( RestoreVersionBatchRequest req = waitNext(loaderInterf.initVersionBatch.getFuture()) ) {
					requestTypeStr = "initVersionBatch";
					actors.add( handleInitVersionBatchRequest(req, self) );
				}
				when ( RestoreSimpleRequest req = waitNext(loaderInterf.finishRestore.getFuture()) ) {
					requestTypeStr = "finishRestore";
					exitRole = handlerFinishRestoreRequest(req, self, cx);
				}
				when ( wait(exitRole) ) {
					break;
				}
			}
		} catch (Error &e) {
            fprintf(stdout, "[ERROR] Restore Loader handle received request:%s error. error code:%d, error message:%s\n",
                    requestTypeStr.c_str(), e.code(), e.what());

			if ( requestTypeStr.find("[Init]") != std::string::npos ) {
				printf("Exit due to error at requestType:%s", requestTypeStr.c_str());
				break;
			}
		}
	}
	return Void();
}

// Restore Loader
ACTOR Future<Void> handleSetApplierKeyRangeVectorRequest(RestoreSetApplierKeyRangeVectorRequest req, Reference<RestoreLoaderData> self) {
	// Idempodent operation. OK to re-execute the duplicate cmd
	// The applier should remember the key range it is responsible for
	//ASSERT(req.cmd == (RestoreCommandEnum) req.cmdID.phase);
	//self->applierStatus.keyRange = req.range;
	while (self->isInProgress(RestoreCommandEnum::Notify_Loader_ApplierKeyRange)) {
		printf("[DEBUG] NODE:%s handleSetApplierKeyRangeVectorRequest wait for 1s\n",  self->describeNode().c_str());
		wait(delay(1.0));
	}
	if ( self->isCmdProcessed(req.cmdID) ) {
		req.reply.send(RestoreCommonReply(self->id(),req.cmdID));
		return Void();
	}
	self->setInProgressFlag(RestoreCommandEnum::Notify_Loader_ApplierKeyRange);

	VectorRef<UID> appliers = req.applierIDs;
	VectorRef<KeyRange> ranges = req.ranges;
	for ( int i = 0; i < appliers.size(); i++ ) {
		self->range2Applier[ranges[i].begin] = appliers[i];
	}
	
	self->processedCmd[req.cmdID] = 1;
	self->clearInProgressFlag(RestoreCommandEnum::Notify_Loader_ApplierKeyRange);
	req.reply.send(RestoreCommonReply(self->id(), req.cmdID));

	return Void();
}

// TODO: MX: 
ACTOR Future<Void> _processLoadingParam(LoadingParam param, Reference<RestoreLoaderData> self) {
	// Temporary data structure for parsing range and log files into (version, <K, V, mutationType>)
	state std::map<Version, Standalone<VectorRef<MutationRef>>> kvOps;
	// Must use StandAlone to save mutations, otherwise, the mutationref memory will be corrupted
	state std::map<Standalone<StringRef>, Standalone<StringRef>> mutationMap; // Key is the unique identifier for a batch of mutation logs at the same version
	state std::map<Standalone<StringRef>, uint32_t> mutationPartMap; // Sanity check the data parsing is correct

	printf("[INFO][Loader] Node:%s, Execute: handleLoadFileRequest, loading param:%s\n",
			self->describeNode().c_str(), param.toString().c_str());

	ASSERT( param.blockSize > 0 );
	//state std::vector<Future<Void>> fileParserFutures;
	if (param.offset % param.blockSize != 0) {
		printf("[WARNING] Parse file not at block boundary! param.offset:%ld param.blocksize:%ld, remainder:%ld\n",
				param.offset, param.blockSize, param.offset % param.blockSize);
	}
	state int64_t j;
	state int64_t readOffset;
	state int64_t readLen;
	for (j = param.offset; j < param.length; j += param.blockSize) {
		readOffset = j;
		readLen = std::min<int64_t>(param.blockSize, param.length - j);
		printf("[DEBUG_TMP] _parseRangeFileToMutationsOnLoader starts\n");
		if ( param.isRangeFile ) {
			wait( _parseRangeFileToMutationsOnLoader(&kvOps, self->bc, param.version, param.filename, readOffset, readLen, param.restoreRange, param.addPrefix, param.removePrefix) );
		} else {
			wait( _parseLogFileToMutationsOnLoader(&mutationMap, &mutationPartMap, self->bc, param.version, param.filename, readOffset, readLen, param.restoreRange, param.addPrefix, param.removePrefix, param.mutationLogPrefix) );
		}
		printf("[DEBUG_TMP] _parseRangeFileToMutationsOnLoader ends\n");
	}

	printf("[INFO][Loader] Finishes process Range file:%s\n", param.filename.c_str());
	
	if ( !param.isRangeFile ) {
		_parseSerializedMutation(&kvOps, &mutationMap);
	}
	
	wait( registerMutationsToApplier(self, &kvOps, true, param.prevVersion, param.endVersion) ); // Send the parsed mutation to applier who will apply the mutation to DB
	
	return Void();
}

ACTOR Future<Void> handleLoadFileRequest(RestoreLoadFileRequest req, Reference<RestoreLoaderData> self, bool isSampling) {
	try {
		if (self->processedFileParams.find(req.param) ==  self->processedFileParams.end()) {
			// Deduplicate the same requests
			printf("self->processedFileParams.size:%d Process param:%s\n", self->processedFileParams.size(), req.param.toString().c_str());
			self->processedFileParams[req.param] = Never();
			self->processedFileParams[req.param] = _processLoadingParam(req.param,  self);
			printf("processedFileParam.size:%d\n", self->processedFileParams.size());
			printf("processedFileParam[req.param].ready:%d\n", self->processedFileParams[req.param].isReady());
			ASSERT(self->processedFileParams.find(req.param) !=  self->processedFileParams.end());
			wait(self->processedFileParams[req.param]);
		} else {
			ASSERT(self->processedFileParams.find(req.param) !=  self->processedFileParams.end());
			printf("Process param that is being processed:%s\n", req.param.toString().c_str());
			wait(self->processedFileParams[req.param]);	
		}
	} catch (Error &e) {
		fprintf(stdout, "[ERROR] handleLoadFileRequest Node:%s, error. error code:%d, error message:%s\n", self->describeNode().c_str(),
				 e.code(), e.what());
	}

	req.reply.send(RestoreCommonReply(self->id(), req.cmdID));
	return Void();
}

ACTOR Future<Void> registerMutationsToApplier(Reference<RestoreLoaderData> self,
									VersionedMutationsMap *pkvOps,
									bool isRangeFile, Version startVersion, Version endVersion) {
    state VersionedMutationsMap &kvOps = *pkvOps;
	printf("[INFO][Loader] Node:%s self->masterApplierInterf:%s, registerMutationsToApplier\n",
			self->describeNode().c_str(), self->masterApplierInterf.toString().c_str());

	state int packMutationNum = 0;
	state int packMutationThreshold = 10;
	state int kvCount = 0;
	state std::vector<Future<RestoreCommonReply>> cmdReplies;

	state int splitMutationIndex = 0;

	// Ensure there is a mutation request sent at endVersion, so that applier can advance its notifiedVersion
	if ( kvOps.find(endVersion)  == kvOps.end() ) {
		kvOps[endVersion] = VectorRef<MutationRef>();
	}

	self->printAppliersKeyRange();

	//state double mutationVectorThreshold = 1;//1024 * 10; // Bytes.
	state std::map<UID, Standalone<VectorRef<MutationRef>>> applierMutationsBuffer; // The mutation vector to be sent to each applier
	state std::map<UID, double> applierMutationsSize; // buffered mutation vector size for each applier
	state Standalone<VectorRef<MutationRef>> mvector;
	state Standalone<VectorRef<UID>> nodeIDs;
	// Initialize the above two maps
	state std::vector<UID> applierIDs = self->getWorkingApplierIDs();
	state std::vector<std::pair<UID, RestoreSendMutationVectorVersionedRequest>> requests;
	state Version prevVersion = startVersion;
	loop {
		try {
			packMutationNum = 0;
			splitMutationIndex = 0;
			kvCount = 0;
			state std::map<Version, Standalone<VectorRef<MutationRef>>>::iterator kvOp;
			// MX: NEED TO A WAY TO GENERATE NON_DUPLICATE CMDUID across loaders
			self->cmdID.setPhase(RestoreCommandEnum::Loader_Send_Mutations_To_Applier); //MX: THIS MAY BE WRONG! CMDID may duplicate across loaders 
			
			for ( kvOp = kvOps.begin(); kvOp != kvOps.end(); kvOp++) {
				// In case try-catch has error and loop back
				applierMutationsBuffer.clear();
				applierMutationsSize.clear();
				for (auto &applierID : applierIDs) {
					applierMutationsBuffer[applierID] = Standalone<VectorRef<MutationRef>>(VectorRef<MutationRef>());
					applierMutationsSize[applierID] = 0.0;
				}
				state Version commitVersion = kvOp->first;
				state int mIndex;
				state MutationRef kvm;
				for (mIndex = 0; mIndex < kvOp->second.size(); mIndex++) {
					kvm = kvOp->second[mIndex];
					if ( debug_verbose ) {
						printf("[VERBOSE_DEBUG] mutation to sent to applier, mutation:%s\n", kvm.toString().c_str());
					}
					// Send the mutation to applier
					if ( isRangeMutation(kvm) ) { // MX: Use false to skip the range mutation handling
						// Because using a vector of mutations causes overhead, and the range mutation should happen rarely;
						// We handle the range mutation and key mutation differently for the benefit of avoiding memory copy
						mvector.pop_front(mvector.size());
						nodeIDs.pop_front(nodeIDs.size());
						//state std::map<Standalone<MutationRef>, UID> m2appliers;
						// '' Bug may be here! The splitMutation() may be wrong!
						splitMutation(self, kvm, mvector.arena(), mvector.contents(), nodeIDs.arena(), nodeIDs.contents());
						// m2appliers = splitMutationv2(self, kvm);
						// // convert m2appliers to mvector and nodeIDs
						// for (auto& m2applier : m2appliers) {
						// 	mvector.push_back(m2applier.first);
						// 	nodeIDs.push_back(m2applier.second);
						// }
					
						printf("SPLITMUTATION: mvector.size:%d\n", mvector.size());
						ASSERT(mvector.size() == nodeIDs.size());

						for (splitMutationIndex = 0; splitMutationIndex < mvector.size(); splitMutationIndex++ ) {
							MutationRef mutation = mvector[splitMutationIndex];
							UID applierID = nodeIDs[splitMutationIndex];
							printf("SPLITTED MUTATION: %d: mutation:%s applierID:%s\n", splitMutationIndex, mutation.toString().c_str(), applierID.toString().c_str());
							applierMutationsBuffer[applierID].push_back_deep(applierMutationsBuffer[applierID].arena(), mutation); // Q: Maybe push_back_deep()?
							applierMutationsSize[applierID] += mutation.expectedSize();

							kvCount++;
						}
					} else { // mutation operates on a particular key
						std::map<Standalone<KeyRef>, UID>::iterator itlow = self->range2Applier.lower_bound(kvm.param1); // lower_bound returns the iterator that is >= m.param1
						// make sure itlow->first <= m.param1
						if ( itlow == self->range2Applier.end() || itlow->first > kvm.param1 ) {
							if ( itlow == self->range2Applier.begin() ) {
								printf("KV-Applier: SHOULD NOT HAPPEN. kvm.param1:%s\n", kvm.param1.toString().c_str());
							}
							--itlow;
						}
						ASSERT( itlow->first <= kvm.param1 );
						MutationRef mutation = kvm;
						UID applierID = itlow->second;
						printf("KV--Applier: K:%s ApplierID:%s\n", kvm.param1.toString().c_str(), applierID.toString().c_str());
						kvCount++;

						applierMutationsBuffer[applierID].push_back_deep(applierMutationsBuffer[applierID].arena(), mutation); // Q: Maybe push_back_deep()?
						applierMutationsSize[applierID] += mutation.expectedSize();
					}
				} // Mutations at the same version

				// In case the mutation vector is not larger than mutationVectorThreshold
				// We must send out the leftover mutations any way; otherwise, the mutations at different versions will be mixed together
				printf("[DEBUG][Loader] sendMutationVector send mutations at Version:%ld to appliers, applierIDs.size:%d\n", commitVersion, applierIDs.size());
				for (auto &applierID : applierIDs) {
					printf("[DEBUG][Loader] sendMutationVector size:%d for applierID:%s\n", applierMutationsBuffer[applierID].size(), applierID.toString().c_str());
					self->cmdID.nextCmd(); // no-use
					requests.push_back( std::make_pair(applierID, RestoreSendMutationVectorVersionedRequest(self->cmdID, prevVersion, commitVersion, isRangeFile, applierMutationsBuffer[applierID])) );
					applierMutationsBuffer[applierID].pop_front(applierMutationsBuffer[applierID].size());
					applierMutationsSize[applierID] = 0;
					//std::vector<RestoreCommonReply> reps = wait( timeoutError( getAll(cmdReplies), FastRestore_Failure_Timeout ) ); // Q: We need to wait for each reply, otherwise, correctness has error. Why?
					//cmdReplies.clear();
				}
				wait( sendBatchRequests(&RestoreApplierInterface::sendMutationVector, self->appliersInterf, requests) );
				requests.clear();
				ASSERT( prevVersion < commitVersion );
				prevVersion = commitVersion;
			} // all versions of mutations

			printf("[Summary][Loader] Node:%s Last CMDUID:%s produces %d mutation operations\n",
					self->describeNode().c_str(), self->cmdID.toString().c_str(), kvCount);

			//kvOps.clear();
			break;

		} catch (Error &e) {
			fprintf(stdout, "[ERROR] registerMutationsToApplier Node:%s, Commands before cmdID:%s error. error code:%d, error message:%s\n", self->describeNode().c_str(),
					self->cmdID.toString().c_str(), e.code(), e.what());
		}
	};

	return Void();
}


// TODO: Add a unit test for this function
void splitMutation(Reference<RestoreLoaderData> self,  MutationRef m, Arena& mvector_arena, VectorRef<MutationRef>& mvector, Arena& nodeIDs_arena, VectorRef<UID>& nodeIDs) {
	// mvector[i] should be mapped to nodeID[i]
	ASSERT(mvector.empty());
	ASSERT(nodeIDs.empty());
	// key range [m->param1, m->param2)
	//std::map<Standalone<KeyRef>, UID>;
	printf("SPLITMUTATION: orignal mutation:%s\n", m.toString().c_str());
	std::map<Standalone<KeyRef>, UID>::iterator itlow, itup; //we will return [itlow, itup)
	itlow = self->range2Applier.lower_bound(m.param1); // lower_bound returns the iterator that is >= m.param1
	if ( itlow->first > m.param1 ) {
		if ( itlow != self->range2Applier.begin() ) {
			--itlow;
		}
	}

	// if ( itlow != self->range2Applier.begin() && itlow->first > m.param1 ) { // m.param1 is not the smallest key \00
	// 	// (itlow-1) is the node whose key range includes m.param1
	// 	--itlow;
	// } else {
	// 	if ( m.param1 != LiteralStringRef("\00") || itlow->first != m.param1 ) { // MX: This is useless
	// 		printf("[ERROR] splitMutation has bug on range mutation:%s\n", m.toString().c_str());
	// 	}
	// }

	itup = self->range2Applier.upper_bound(m.param2); // upper_bound returns the iterator that is > m.param2; return rmap::end if no keys are considered to go after m.param2.
	printf("SPLITMUTATION: itlow_key:%s itup_key:%s\n", itlow->first.toString().c_str(), itup == self->range2Applier.end() ? "[end]" : itup->first.toString().c_str());
	ASSERT( itup == self->range2Applier.end() || itup->first >= m.param2 );
	// Now adjust for the case: example: mutation range is [a, d); we have applier's ranges' inclusive lower bound values are: a, b, c, d, e; upper_bound(d) returns itup to e, but we want itup to d.
	//--itup;
	//ASSERT( itup->first <= m.param2 );
	// if ( itup->first < m.param2 ) {
	// 	++itup; //make sure itup is >= m.param2, that is, itup is the next key range >= m.param2
	// }

	std::map<Standalone<KeyRef>, UID>::iterator itApplier;
	while (itlow != itup) {
		Standalone<MutationRef> curm; //current mutation
		curm.type = m.type;
		// the first split mutation should starts with m.first. The later onces should start with the range2Applier boundary
		if ( m.param1 > itlow->first ) {
			curm.param1 = m.param1;
		} else {
			curm.param1 = itlow->first;
		}
		itApplier = itlow;
		//curm.param1 = ((m.param1 > itlow->first) ? m.param1 : itlow->first); 
		itlow++;
		if (itlow == itup) {
			ASSERT( m.param2 <= normalKeys.end );
			curm.param2 = m.param2;
		} else if ( m.param2 < itlow->first ) {
			curm.param2 = m.param2;
		} else {
			curm.param2 = itlow->first;
		}
		printf("SPLITMUTATION: mvector.push_back:%s\n", curm.toString().c_str());
		ASSERT( curm.param1 <= curm.param2 );
		mvector.push_back_deep(mvector_arena, curm);
		nodeIDs.push_back(nodeIDs_arena, itApplier->second);
	}

	printf("SPLITMUTATION: mvector.size:%d\n", mvector.size());

	return;
}


//key_input format: [logRangeMutation.first][hash_value_of_commit_version:1B][bigEndian64(commitVersion)][bigEndian32(part)]
bool concatenateBackupMutationForLogFile(std::map<Standalone<StringRef>, Standalone<StringRef>> *pMutationMap,
									std::map<Standalone<StringRef>, uint32_t> *pMutationPartMap,
									Standalone<StringRef> val_input, Standalone<StringRef> key_input) {
    std::map<Standalone<StringRef>, Standalone<StringRef>> &mutationMap = *pMutationMap;
	std::map<Standalone<StringRef>, uint32_t> &mutationPartMap = *pMutationPartMap;
	std::string prefix = "||\t";
	std::stringstream ss;
	// const int version_size = 12;
	// const int header_size = 12;
	StringRef val = val_input.contents();
	StringRefReaderMX reader(val, restore_corrupted_data());
	StringRefReaderMX readerKey(key_input, restore_corrupted_data()); //read key_input!
	int logRangeMutationFirstLength = key_input.size() - 1 - 8 - 4;
	bool concatenated = false;

	if ( logRangeMutationFirstLength < 0 ) {
		printf("[ERROR]!!! logRangeMutationFirstLength:%ld < 0, key_input.size:%ld\n", logRangeMutationFirstLength, key_input.size());
	}

	if ( debug_verbose ) {
		printf("[DEBUG] Process key_input:%s\n", getHexKey(key_input, logRangeMutationFirstLength).c_str());
	}

	//PARSE key
	Standalone<StringRef> id_old = key_input.substr(0, key_input.size() - 4); //Used to sanity check the decoding of key is correct
	Standalone<StringRef> partStr = key_input.substr(key_input.size() - 4, 4); //part
	StringRefReaderMX readerPart(partStr, restore_corrupted_data());
	uint32_t part_direct = readerPart.consumeNetworkUInt32(); //Consume a bigEndian value
	if ( debug_verbose  ) {
		printf("[DEBUG] Process prefix:%s and partStr:%s part_direct:%08x fromm key_input:%s, size:%ld\n",
			   getHexKey(id_old, logRangeMutationFirstLength).c_str(),
			   getHexString(partStr).c_str(),
			   part_direct,
			   getHexKey(key_input, logRangeMutationFirstLength).c_str(),
			   key_input.size());
	}

	StringRef longRangeMutationFirst;

	if ( logRangeMutationFirstLength > 0 ) {
		printf("readerKey consumes %dB\n", logRangeMutationFirstLength);
		longRangeMutationFirst = StringRef(readerKey.consume(logRangeMutationFirstLength), logRangeMutationFirstLength);
	}

	uint8_t hashValue = readerKey.consume<uint8_t>();
	uint64_t commitVersion = readerKey.consumeNetworkUInt64(); // Consume big Endian value encoded in log file, commitVersion is in littleEndian
	uint64_t commitVersionBE = bigEndian64(commitVersion);
	uint32_t part = readerKey.consumeNetworkUInt32(); //Consume big Endian value encoded in log file
	uint32_t partBE = bigEndian32(part);
	Standalone<StringRef> id2 = longRangeMutationFirst.withSuffix(StringRef(&hashValue,1)).withSuffix(StringRef((uint8_t*) &commitVersion, 8));

	//Use commitVersion as id
	Standalone<StringRef> id = StringRef((uint8_t*) &commitVersion, 8);

	if ( debug_verbose ) {
		printf("[DEBUG] key_input_size:%d longRangeMutationFirst:%s hashValue:%02x commitVersion:%016lx (BigEndian:%016lx) part:%08x (BigEndian:%08x), part_direct:%08x mutationMap.size:%ld\n",
			   key_input.size(), longRangeMutationFirst.printable().c_str(), hashValue,
			   commitVersion, commitVersionBE,
			   part, partBE,
			   part_direct, mutationMap.size());
	}

	if ( mutationMap.find(id) == mutationMap.end() ) {
		mutationMap.insert(std::make_pair(id, val_input));
		if ( part_direct != 0 ) {
			printf("[ERROR]!!! part:%d != 0 for key_input:%s\n", part_direct, getHexString(key_input).c_str());
		}
		mutationPartMap.insert(std::make_pair(id, part_direct));
	} else { // concatenate the val string
//		printf("[INFO] Concatenate the log's val string at version:%ld\n", id.toString().c_str());
		mutationMap[id] = mutationMap[id].contents().withSuffix(val_input.contents()); //Assign the new Areana to the map's value
		if ( part_direct != (mutationPartMap[id] + 1) ) {
			fprintf(stderr, "[ERROR]!!! current part id:%d new part_direct:%d is not the next integer of key_input:%s\n", mutationPartMap[id], part_direct, getHexString(key_input).c_str());
			printf("[HINT] Check if the same range or log file has been processed more than once!\n");
		}
		if ( part_direct != part ) {
			printf("part_direct:%08x != part:%08x\n", part_direct, part);
		}
		mutationPartMap[id] = part_direct;
		concatenated = true;
	}

	return concatenated;
}

bool isRangeMutation(MutationRef m) {
	if (m.type == MutationRef::Type::ClearRange) {
		if (m.type == MutationRef::Type::DebugKeyRange) {
			printf("[ERROR] DebugKeyRange mutation is in backup data unexpectedly. We still handle it as a range mutation; the suspicious mutation:%s\n", m.toString().c_str());
		}
		return true;
	} else {
		if ( !(m.type == MutationRef::Type::SetValue ||
				isAtomicOp((MutationRef::Type) m.type)) ) {
			printf("[ERROR] %s mutation is in backup data unexpectedly. We still handle it as a key mutation; the suspicious mutation:%s\n", typeString[m.type], m.toString().c_str());

		}
		return false;
	}
}


 // Parse the kv pair (version, serialized_mutation), which are the results parsed from log file.
 void _parseSerializedMutation(VersionedMutationsMap *pkvOps,
	 						 std::map<Standalone<StringRef>, Standalone<StringRef>> *pmutationMap,
							 bool isSampling) {
	// Step: Parse the concatenated KV pairs into (version, <K, V, mutationType>) pair
	VersionedMutationsMap &kvOps = *pkvOps;
	std::map<Standalone<StringRef>, Standalone<StringRef>> &mutationMap = *pmutationMap;

 	printf("[INFO] Parse the concatenated log data\n");
 	std::string prefix = "||\t";
	std::stringstream ss;
	// const int version_size = 12;
	// const int header_size = 12;
	int kvCount = 0;

	for ( auto& m : mutationMap ) {
		StringRef k = m.first.contents();
		StringRefReaderMX readerVersion(k, restore_corrupted_data());
		uint64_t commitVersion = readerVersion.consume<uint64_t>(); // Consume little Endian data


		StringRef val = m.second.contents();
		StringRefReaderMX reader(val, restore_corrupted_data());

		int count_size = 0;
		// Get the include version in the batch commit, which is not the commitVersion.
		// commitVersion is in the key
		//uint64_t includeVersion = reader.consume<uint64_t>();
		reader.consume<uint64_t>();
		count_size += 8;
		uint32_t val_length_decode = reader.consume<uint32_t>(); //Parse little endian value, confirmed it is correct!
		count_size += 4;

		kvOps.insert(std::make_pair(commitVersion, VectorRef<MutationRef>()));

		if ( debug_verbose ) {
			printf("----------------------------------------------------------Register Backup Mutation into KVOPs version:0x%08lx (%08ld)\n", commitVersion, commitVersion);
			printf("To decode value:%s\n", getHexString(val).c_str());
		}
		// In sampling, the last mutation vector may be not complete, we do not concatenate for performance benefit
		if ( val_length_decode != (val.size() - 12) ) {
			//IF we see val.size() == 10000, It means val should be concatenated! The concatenation may fail to copy the data
			if (isSampling) {
				printf("[PARSE WARNING]!!! val_length_decode:%d != val.size:%d version:%ld(0x%lx)\n",  val_length_decode, val.size(),
					commitVersion, commitVersion);
				printf("[PARSE WARNING] Skipped the mutation! OK for sampling workload but WRONG for restoring the workload\n");
				continue;
			} else {
				fprintf(stderr, "[PARSE ERROR]!!! val_length_decode:%d != val.size:%d version:%ld(0x%lx)\n",  val_length_decode, val.size(),
					commitVersion, commitVersion);
			}
		} else {
			if ( debug_verbose ) {
				printf("[PARSE SUCCESS] val_length_decode:%d == (val.size:%d - 12)\n", val_length_decode, val.size());
			}
		}

		// Get the mutation header
		while (1) {
			// stop when reach the end of the string
			if(reader.eof() ) { //|| *reader.rptr == 0xFF
				//printf("Finish decode the value\n");
				break;
			}


			uint32_t type = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
			uint32_t kLen = reader.consume<uint32_t>();//reader.consumeNetworkUInkvOps[t32();
			uint32_t vLen = reader.consume<uint32_t>();//reader.consumeNetworkUInt32();
			const uint8_t *k = reader.consume(kLen);
			const uint8_t *v = reader.consume(vLen);
			count_size += 4 * 3 + kLen + vLen;

			MutationRef mutation((MutationRef::Type) type, KeyRef(k, kLen), KeyRef(v, vLen));
			kvOps[commitVersion].push_back_deep(kvOps[commitVersion].arena(), mutation);
			kvCount++;

			if ( kLen < 0 || kLen > val.size() || vLen < 0 || vLen > val.size() ) {
				printf("%s[PARSE ERROR]!!!! kLen:%d(0x%04x) vLen:%d(0x%04x)\n", prefix.c_str(), kLen, kLen, vLen, vLen);
			}

			if ( debug_verbose ) {
				printf("%s---LogFile parsed mutations. Prefix:[%d]: Version:%016lx Type:%d K:%s V:%s k_size:%d v_size:%d\n", prefix.c_str(),
					   kvCount,
					   commitVersion, type,  getHexString(KeyRef(k, kLen)).c_str(), getHexString(KeyRef(v, vLen)).c_str(), kLen, vLen);
				printf("%s[PrintAgain]---LogFile parsed mutations. Prefix:[%d]: Version:%016lx (%016ld) Type:%d K:%s V:%s k_size:%d v_size:%d\n", prefix.c_str(),
					   kvCount,
					   commitVersion, commitVersion, type,  KeyRef(k, kLen).toString().c_str(), KeyRef(v, vLen).toString().c_str(), kLen, vLen);
			}

		}
		//	printf("----------------------------------------------------------\n");
	}

	printf("[INFO] Produces %d mutation operations from concatenated kv pairs that are parsed from log\n",  kvCount);

}

// Parsing log file, which is the same for sampling and loading phases
ACTOR static Future<Void> _parseRangeFileToMutationsOnLoader(VersionedMutationsMap *pkvOps,
 									Reference<IBackupContainer> bc, Version version,
 									std::string fileName, int64_t readOffset_input, int64_t readLen_input,
 									KeyRange restoreRange, Key addPrefix, Key removePrefix) {
    state VersionedMutationsMap &kvOps = *pkvOps;
 	state int64_t readOffset = readOffset_input;
 	state int64_t readLen = readLen_input;

	// if ( debug_verbose ) {
	 	printf("[VERBOSE_DEBUG] Parse range file and get mutations 1, bc:%lx\n", bc.getPtr());
	// }

 	// The set of key value version is rangeFile.version. the key-value set in the same range file has the same version
 	Reference<IAsyncFile> inFile = wait(bc->readFile(fileName));

	// if ( debug_verbose ) {
	// 	printf("[VERBOSE_DEBUG] Parse range file and get mutations 2\n");
	// }
 	state Standalone<VectorRef<KeyValueRef>> blockData = wait(parallelFileRestore::decodeRangeFileBlock(inFile, readOffset, readLen));

	// if ( debug_verbose ) {
	// 	printf("[VERBOSE_DEBUG] Parse range file and get mutations 3\n");
	// 	int tmpi = 0;
	// 	for (tmpi = 0; tmpi < blockData.size(); tmpi++) {
	// 		printf("\t[VERBOSE_DEBUG] mutation: key:%s value:%s\n", blockData[tmpi].key.toString().c_str(), blockData[tmpi].value.toString().c_str());
	// 	}
	// }

 	// First and last key are the range for this file
 	state KeyRange fileRange = KeyRangeRef(blockData.front().key, blockData.back().key);
 	printf("[INFO] RangeFile:%s KeyRange:%s, restoreRange:%s\n",
 			fileName.c_str(), fileRange.toString().c_str(), restoreRange.toString().c_str());

 	// If fileRange doesn't intersect restore range then we're done.
 	if(!fileRange.intersects(restoreRange)) {
 		TraceEvent("ExtractApplyRangeFileToDB_MX").detail("NoIntersectRestoreRange", "FinishAndReturn");
 		return Void();
 	}

 	// We know the file range intersects the restore range but there could still be keys outside the restore range.
 	// Find the subvector of kv pairs that intersect the restore range.  Note that the first and last keys are just the range endpoints for this file
	 // The blockData's first and last entries are metadata, not the real data
 	int rangeStart = 1; //1
 	int rangeEnd = blockData.size() -1; //blockData.size() - 1 // Q: the rangeStart and rangeEnd is [,)?
	// if ( debug_verbose ) {
	// 	printf("[VERBOSE_DEBUG] Range file decoded blockData\n");
	// 	for (auto& data : blockData ) {
	// 		printf("\t[VERBOSE_DEBUG] data key:%s val:%s\n", data.key.toString().c_str(), data.value.toString().c_str());
	// 	}
	// }

 	// Slide start from begining, stop if something in range is found
	// Move rangeStart and rangeEnd until they is within restoreRange
 	while(rangeStart < rangeEnd && !restoreRange.contains(blockData[rangeStart].key)) {
		// if ( debug_verbose ) {
		// 	printf("[VERBOSE_DEBUG] rangeStart:%d key:%s is not in the range:%s\n", rangeStart, blockData[rangeStart].key.toString().c_str(), restoreRange.toString().c_str());
		// }
		++rangeStart;
	 }
 	// Side end backwaself, stop if something in range is found
 	while(rangeEnd > rangeStart && !restoreRange.contains(blockData[rangeEnd - 1].key)) {
		// if ( debug_verbose ) {
		// 	printf("[VERBOSE_DEBUG] (rangeEnd:%d - 1) key:%s is not in the range:%s\n", rangeEnd, blockData[rangeStart].key.toString().c_str(), restoreRange.toString().c_str());
		// }
		--rangeEnd;
	 }

 	// MX: now data only contains the kv mutation within restoreRange
 	state VectorRef<KeyValueRef> data = blockData.slice(rangeStart, rangeEnd);
 	printf("[INFO] RangeFile:%s blockData entry size:%d recovered data size:%d\n", fileName.c_str(), blockData.size(), data.size());

 	// Shrink file range to be entirely within restoreRange and translate it to the new prefix
 	// First, use the untranslated file range to create the shrunk original file range which must be used in the kv range version map for applying mutations
 	state KeyRange originalFileRange = KeyRangeRef(std::max(fileRange.begin, restoreRange.begin), std::min(fileRange.end,   restoreRange.end));

 	// Now shrink and translate fileRange
 	Key fileEnd = std::min(fileRange.end,   restoreRange.end);
 	if(fileEnd == (removePrefix == StringRef() ? normalKeys.end : strinc(removePrefix)) ) {
 		fileEnd = addPrefix == StringRef() ? normalKeys.end : strinc(addPrefix);
 	} else {
 		fileEnd = fileEnd.removePrefix(removePrefix).withPrefix(addPrefix);
 	}
 	fileRange = KeyRangeRef(std::max(fileRange.begin, restoreRange.begin).removePrefix(removePrefix).withPrefix(addPrefix),fileEnd);

 	state int start = 0;
 	state int end = data.size();
 	//state int dataSizeLimit = BUGGIFY ? g_random->randomInt(256 * 1024, 10e6) : CLIENT_KNOBS->RESTORE_WRITE_TX_SIZE;
	state int dataSizeLimit = CLIENT_KNOBS->RESTORE_WRITE_TX_SIZE;
 	state int kvCount = 0;

 	//MX: This is where the key-value pair in range file is applied into DB
	loop {

		state int i = start;
		state int txBytes = 0;
		state int iend = start;

		// find iend that results in the desired transaction size
		for(; iend < end && txBytes < dataSizeLimit; ++iend) {
			txBytes += data[iend].key.expectedSize();
			txBytes += data[iend].value.expectedSize();
		}


		for(; i < iend; ++i) {
			//MXX: print out the key value version, and operations.
			if ( debug_verbose ) {
				printf("RangeFile [key:%s, value:%s, version:%ld, op:set]\n", data[i].key.printable().c_str(), data[i].value.printable().c_str(), version);
			}
// 				TraceEvent("PrintRangeFile_MX").detail("Key", data[i].key.printable()).detail("Value", data[i].value.printable())
// 					.detail("Version", rangeFile.version).detail("Op", "set");
////				printf("PrintRangeFile_MX: mType:set param1:%s param2:%s param1_size:%d, param2_size:%d\n",
////						getHexString(data[i].key.c_str(), getHexString(data[i].value).c_str(), data[i].key.size(), data[i].value.size());

			//NOTE: Should NOT removePrefix and addPrefix for the backup data!
			// In other words, the following operation is wrong:  data[i].key.removePrefix(removePrefix).withPrefix(addPrefix)
			MutationRef m(MutationRef::Type::SetValue, data[i].key, data[i].value); //ASSUME: all operation in range file is set.
			++kvCount;

			// We cache all kv operations into kvOps, and apply all kv operations later in one place
			kvOps.insert(std::make_pair(version, VectorRef<MutationRef>()));

			ASSERT(kvOps.find(version) != kvOps.end());
			kvOps[version].push_back_deep(kvOps[version].arena(), m);
		}

		// Commit succeeded, so advance starting point
		start = i;

		if(start == end) {
			//TraceEvent("ExtraApplyRangeFileToDB_MX").detail("Progress", "DoneApplyKVToDB");
			printf("[INFO][Loader]  Parse RangeFile:%s: the number of kv operations = %d\n", fileName.c_str(), kvCount);
			return Void();
		}
 	}
 }

 ACTOR static Future<Void> _parseLogFileToMutationsOnLoader(std::map<Standalone<StringRef>, Standalone<StringRef>> *pMutationMap,
									std::map<Standalone<StringRef>, uint32_t> *pMutationPartMap,
 									Reference<IBackupContainer> bc, Version version,
 									std::string fileName, int64_t readOffset, int64_t readLen,
 									KeyRange restoreRange, Key addPrefix, Key removePrefix,
 									Key mutationLogPrefix) {

	// Step: concatenate the backuped param1 and param2 (KV) at the same version.
 	//state Key mutationLogPrefix = mutationLogPrefix;
 	//TraceEvent("ReadLogFileStart").detail("LogFileName", fileName);
 	state Reference<IAsyncFile> inFile = wait(bc->readFile(fileName));
 	//TraceEvent("ReadLogFileFinish").detail("LogFileName", fileName);

 	printf("Parse log file:%s readOffset:%d readLen:%ld\n", fileName.c_str(), readOffset, readLen);
 	//TODO: NOTE: decodeLogFileBlock() should read block by block! based on my serial version. This applies to decode range file as well
 	state Standalone<VectorRef<KeyValueRef>> data = wait(parallelFileRestore::decodeLogFileBlock(inFile, readOffset, readLen));
 	//state Standalone<VectorRef<MutationRef>> data = wait(fileBackup::decodeLogFileBlock_MX(inFile, readOffset, readLen)); //Decode log file
 	TraceEvent("ReadLogFileFinish").detail("LogFileName", fileName).detail("DecodedDataSize", data.contents().size());
 	printf("ReadLogFile, raw data size:%d\n", data.size());

 	state int start = 0;
 	state int end = data.size();
 	//state int dataSizeLimit = BUGGIFY ? g_random->randomInt(256 * 1024, 10e6) : CLIENT_KNOBS->RESTORE_WRITE_TX_SIZE;
	state int dataSizeLimit = CLIENT_KNOBS->RESTORE_WRITE_TX_SIZE;
	state int kvCount = 0;
	state int numConcatenated = 0;
	loop {
 		try {
// 			printf("Process start:%d where end=%d\n", start, end);
 			if(start == end) {
 				printf("ReadLogFile: finish reading the raw data and concatenating the mutation at the same version\n");
 				break;
 			}

 			state int i = start;
 			state int txBytes = 0;
 			for(; i < end && txBytes < dataSizeLimit; ++i) {
 				Key k = data[i].key.withPrefix(mutationLogPrefix);
 				ValueRef v = data[i].value;
 				txBytes += k.expectedSize();
 				txBytes += v.expectedSize();
 				//MXX: print out the key value version, and operations.
 				//printf("LogFile [key:%s, value:%s, version:%ld, op:NoOp]\n", k.printable().c_str(), v.printable().c_str(), logFile.version);
 //				printf("LogFile [KEY:%s, VALUE:%s, VERSION:%ld, op:NoOp]\n", getHexString(k).c_str(), getHexString(v).c_str(), logFile.version);
 //				printBackupMutationRefValueHex(v, " |\t");
 //				printf("[DEBUG]||Concatenate backup mutation:fileInfo:%s, data:%d\n", logFile.toString().c_str(), i);
 				bool concatenated = concatenateBackupMutationForLogFile(pMutationMap, pMutationPartMap, data[i].value, data[i].key);
 				numConcatenated += ( concatenated ? 1 : 0);
 //				//TODO: Decode the value to get the mutation type. Use NoOp to distinguish from range kv for now.
 //				MutationRef m(MutationRef::Type::NoOp, data[i].key, data[i].value); //ASSUME: all operation in log file is NoOp.
 //				if ( self->kvOps.find(logFile.version) == self->kvOps.end() ) {
 //					self->kvOps.insert(std::make_pair(logFile.version, std::vector<MutationRef>()));
 //				} else {
 //					self->kvOps[logFile.version].push_back(m);
 //				}
 			}

 			start = i;

 		} catch(Error &e) {
 			if(e.code() == error_code_transaction_too_large)
 				dataSizeLimit /= 2;
 		}
 	}

 	printf("[INFO] raw kv number:%d parsed from log file, concatenated:%d kv, num_log_versions:%d\n", data.size(), numConcatenated, pMutationMap->size());

	return Void();
 }
