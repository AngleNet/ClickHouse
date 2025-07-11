#pragma once

#include <Core/Names.h>
#include <Storages/AlterCommands.h>
#include <Storages/IStorage.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <Storages/MergeTree/MergeTreeDataWriter.h>
#include <Storages/MergeTree/MergeTreeDataMergerMutator.h>
#include <Storages/MergeTree/MergeTreePartsMover.h>
#include <Storages/MergeTree/MergeTreeMutationEntry.h>
#include <Storages/MergeTree/MergeTreeMutationStatus.h>
#include <Storages/MergeTree/MergeTreeDeduplicationLog.h>
#include <Storages/MergeTree/FutureMergedMutatedPart.h>
#include <Storages/MergeTree/MergePlainMergeTreeTask.h>
#include <Storages/MergeTree/MutatePlainMergeTreeTask.h>
#include <Storages/MergeTree/MergeTreeCommittingBlock.h>
#include <Storages/MergeTree/PatchParts/PatchPartsLock.h>

#include <Disks/StoragePolicy.h>
#include <Common/SimpleIncrement.h>


namespace DB
{

class PreparedSetsCache;
using PreparedSetsCachePtr = std::shared_ptr<PreparedSetsCache>;

/** See the description of the data structure in MergeTreeData.
  */
class StorageMergeTree final : public MergeTreeData
{
public:
    /** Attach the table with the appropriate name, along the appropriate path (with / at the end),
      *  (correctness of names and paths are not checked)
      *  consisting of the specified columns.
      *
      * See MergeTreeData constructor for comments on parameters.
      */
    StorageMergeTree(
        const StorageID & table_id_,
        const String & relative_data_path_,
        const StorageInMemoryMetadata & metadata,
        LoadingStrictnessLevel mode,
        ContextMutablePtr context_,
        const String & date_column_name,
        const MergingParams & merging_params_,
        std::unique_ptr<MergeTreeSettings> settings_);

    void startup() override;
    void shutdown(bool is_drop) override;

    ~StorageMergeTree() override;

    std::string getName() const override { return merging_params.getModeName() + "MergeTree"; }

    bool supportsParallelInsert() const override { return true; }

    bool supportsTransactions() const override { return true; }

    void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    std::optional<UInt64> totalRows(ContextPtr) const override;
    std::optional<UInt64> totalRowsByPartitionPredicate(const ActionsDAG & filter_actions_dag, ContextPtr) const override;
    std::optional<UInt64> totalBytes(ContextPtr) const override;
    std::optional<UInt64> totalBytesUncompressed(const Settings &) const override;
    MutationCounters getMutationCounters() const override;

    SinkToStoragePtr write(const ASTPtr & query, const StorageMetadataPtr & /*metadata_snapshot*/, ContextPtr context, bool async_insert) override;

    /** Perform the next step in combining the parts.
      */
    bool optimize(
        const ASTPtr & query,
        const StorageMetadataPtr & /*metadata_snapshot*/,
        const ASTPtr & partition,
        bool final,
        bool deduplicate,
        const Names & deduplicate_by_columns,
        bool cleanup,
        ContextPtr context) override;

    void mutate(const MutationCommands & commands, ContextPtr context) override;
    QueryPipeline updateLightweight(const MutationCommands & commands, ContextPtr query_context) override;

    bool hasLightweightDeletedMask() const override;

    /// Return introspection information about currently processing or recently processed mutations.
    std::vector<MergeTreeMutationStatus> getMutationsStatus() const override;

    CancellationCode killMutation(const String & mutation_id) override;

    /// Makes backup entries to backup the data of the storage.
    void backupData(BackupEntriesCollector & backup_entries_collector, const String & data_path_in_backup, const std::optional<ASTs> & partitions) override;

    void drop() override;
    void truncate(const ASTPtr &, const StorageMetadataPtr &, ContextPtr, TableExclusiveLockHolder &) override;

    void alter(const AlterCommands & commands, ContextPtr context, AlterLockHolder & table_lock_holder) override;

    void checkTableCanBeDropped([[ maybe_unused ]] ContextPtr query_context) const override;

    ActionLock getActionLock(StorageActionBlockType action_type) override;

    void onActionLockRemove(StorageActionBlockType action_type) override;

    DataValidationTasksPtr getCheckTaskList(const CheckTaskFilter & check_task_filter, ContextPtr context) override;
    std::optional<CheckResult> checkDataNext(DataValidationTasksPtr & check_task_list) override;

    bool scheduleDataProcessingJob(BackgroundJobsAssignee & assignee) override;

    std::map<std::string, MutationCommands> getUnfinishedMutationCommands() const override;

    MergeTreeDeduplicationLog * getDeduplicationLog() { return deduplication_log.get(); }

private:

    /// Mutex and condvar for synchronous mutations wait
    std::mutex mutation_wait_mutex;
    std::condition_variable mutation_wait_event;

    MergeTreeDataSelectExecutor reader;
    MergeTreeDataWriter writer;
    MergeTreeDataMergerMutator merger_mutator;

    std::unique_ptr<MergeTreeDeduplicationLog> deduplication_log;

    /// For block numbers.
    SimpleIncrement increment;

    /// For clearOldParts
    AtomicStopwatch time_after_previous_cleanup_parts;
    /// For clearOldTemporaryDirectories.
    AtomicStopwatch time_after_previous_cleanup_temporary_directories;
    /// For clearOldBrokenDetachedParts
    AtomicStopwatch time_after_previous_cleanup_broken_detached_parts;

    /// Mutex for parts currently processing in background
    /// merging (also with TTL), mutating or moving.
    mutable std::mutex currently_processing_in_background_mutex;
    mutable std::condition_variable currently_processing_in_background_condition;

    /// Parts that currently participate in merge or mutation.
    /// This set have to be used with `currently_processing_in_background_mutex`.
    DataParts currently_merging_mutating_parts;

    std::map<UInt64, MergeTreeMutationEntry> current_mutations_by_version;

    /// Unfinished mutations that are required for AlterConversions.
    MutationCounters mutation_counters;

    CommittingBlocksSet committing_blocks;
    mutable std::mutex committing_blocks_mutex;
    mutable std::condition_variable committing_blocks_cv;

    void removeCommittingBlock(CommittingBlock block);
    CommittingBlock allocateBlockNumber(CommittingBlock::Op op);
    void waitForCommittingInsertsAndMutations(Int64 max_block_number, size_t timeout_ms) const;
    CommittingBlocksSet getCommittingBlocks() const;

    std::atomic<bool> shutdown_called {false};
    std::atomic<bool> flush_called {false};

    /// PreparedSets cache for one executing mutation.
    /// NOTE: we only store weak_ptr to PreparedSetsCache, so that the cache is shared between mutation tasks that are executed in parallel.
    /// The goal is to avoiding consuming a lot of memory when the same big sets are used by multiple tasks at the same time.
    /// If the tasks are executed without time overlap, we will destroy the cache to free memory, and the next task might rebuild the same sets.
    std::mutex mutation_prepared_sets_cache_mutex;
    std::map<Int64, PreparedSetsCachePtr::weak_type> mutation_prepared_sets_cache;
    PlainLightweightUpdatesSync lightweight_updates_sync;

    void loadMutations();

    /// Load and initialize deduplication logs. Even if deduplication setting
    /// equals zero creates object with deduplication window equals zero.
    void loadDeduplicationLog();

    /** Determines what parts should be merged and merges it.
      * If aggressive - when selects parts don't takes into account their ratio size and novelty (used for OPTIMIZE query).
      * Returns true if merge is finished successfully.
      */
    bool merge(
            bool aggressive,
            const String & partition_id,
            bool final, bool deduplicate,
            const Names & deduplicate_by_columns,
            bool cleanup,
            const MergeTreeTransactionPtr & txn,
            PreformattedMessage & out_disable_reason,
            bool optimize_skip_merged_partitions = false);

    void renameAndCommitEmptyParts(MutableDataPartsVector & new_parts, Transaction & transaction);

    /// Make part state outdated and queue it to remove without timeout
    /// If force, then stop merges and block them until part state became outdated. Throw exception if part doesn't exists
    /// If not force, then take merges selector and check that part is not participating in background operations.
    MergeTreeDataPartPtr outdatePart(MergeTreeTransaction * txn, const String & part_name, bool force, bool clear_without_timeout = true);
    ActionLock stopMergesAndWait();
    ActionLock stopMergesAndWaitForPartition(String partition_id);

    /// Allocate block number for new mutation, write mutation to disk
    /// and into in-memory structures. Wake up merge-mutation task.
    Int64 startMutation(const MutationCommands & commands, ContextPtr query_context);
    /// Wait until mutation with version will finish mutation for all parts
    void waitForMutation(Int64 version, bool wait_for_another_mutation);
    void waitForMutation(const String & mutation_id, bool wait_for_another_mutation) override;
    void waitForMutation(Int64 version, const String & mutation_id, bool wait_for_another_mutation = false);
    void setMutationCSN(const String & mutation_id, CSN csn) override;

    friend struct CurrentlyMergingPartsTagger;
    friend class MergeTreeMergePredicate;
    friend struct PlainCommittingBlockHolder;

    std::expected<MergeMutateSelectedEntryPtr, SelectMergeFailure> selectPartsToMerge(
        const StorageMetadataPtr & metadata_snapshot,
        bool aggressive,
        const String & partition_id,
        bool final,
        TableLockHolder & table_lock_holder,
        std::unique_lock<std::mutex> & lock,
        const MergeTreeTransactionPtr & txn,
        bool optimize_skip_merged_partitions = false);

    MergeMutateSelectedEntryPtr selectPartsToMutate(
        const StorageMetadataPtr & metadata_snapshot, PreformattedMessage & disable_reason,
        TableLockHolder & table_lock_holder, std::unique_lock<std::mutex> & currently_processing_in_background_mutex_lock);

    /// Returns a lock for lightweight update according to the update_parallel_mode setting
    std::unique_ptr<PlainLightweightUpdateLock> getLockForLightweightUpdate(const MutationCommands & commands, const ContextPtr & local_context);

    /// For current mutations queue, returns maximum version of mutation for a part,
    /// with respect of mutations which would not change it.
    /// Returns 0 if there is no such mutation in active status.
    UInt64 getCurrentMutationVersion(UInt64 data_version, std::unique_lock<std::mutex> & /* currently_processing_in_background_mutex_lock */) const;
    UInt64 getNextMutationVersion(UInt64 data_version, std::unique_lock<std::mutex> & /* currently_processing_in_background_mutex_lock */) const;

    /// Returns the maximum level of all outdated parts in a range (left; right), or 0 in case if empty range.
    /// Merges have to be aware of the outdated part's levels inside designated merge range.
    /// When two parts all_1_1_0, all_3_3_0 are merged into all_1_3_1, the gap between those parts have to be verified.
    /// There should not be an unactive part all_1_1_1. Otherwise it is impossible to load parts after restart, they intersects.
    /// Therefore this function is used in merge predicate in order to prevent merges over the gaps with high level outdated parts.
    UInt32 getMaxLevelInBetween(const PartProperties & left, const PartProperties & right) const;

    size_t clearOldMutations(bool truncate = false);

    /// Delete irrelevant parts from memory and disk.
    /// If 'force' - don't wait for old_parts_lifetime.
    size_t clearOldPartsFromFilesystem(bool force = false, bool with_pause_fail_point = false);

    // Partition helpers
    void dropPartNoWaitNoThrow(const String & part_name) override;
    void dropPart(const String & part_name, bool detach, ContextPtr context) override;
    void dropPartition(const ASTPtr & partition, bool detach, ContextPtr context) override;
    void dropPartsImpl(DataPartsVector && parts_to_remove, bool detach);
    PartitionCommandsResultInfo attachPartition(const ASTPtr & partition, const StorageMetadataPtr & metadata_snapshot, bool part, ContextPtr context) override;

    void replacePartitionFrom(const StoragePtr & source_table, const ASTPtr & partition, bool replace, ContextPtr context) override;
    void movePartitionToTable(const StoragePtr & dest_table, const ASTPtr & partition, ContextPtr context) override;
    bool partIsAssignedToBackgroundOperation(const DataPartPtr & part) const override;
    /// Update mutation entries after part mutation execution. May reset old
    /// errors if mutation was successful. Otherwise update last_failed* fields
    /// in mutation entries.
    void updateMutationEntriesErrors(FutureMergedMutatedPartPtr result_part, bool is_successful, const String & exception_message, const String & error_code_name);

    /// Return empty optional if mutation was killed. Otherwise return partially
    /// filled mutation status with information about error (latest_fail*) and
    /// is_done. mutation_ids filled with mutations with the same errors,
    /// because we can execute several mutations at once. Order is important for
    /// better readability of exception message. If mutation was killed doesn't
    /// return any ids.
    std::optional<MergeTreeMutationStatus> getIncompleteMutationsStatus(Int64 mutation_version, std::set<String> * mutation_ids = nullptr,
                                                                        bool from_another_mutation = false) const;

    std::optional<MergeTreeMutationStatus> getIncompleteMutationsStatusUnlocked(Int64 mutation_version, std::unique_lock<std::mutex> & lock,
                                                                        std::set<String> * mutation_ids = nullptr, bool from_another_mutation = false) const;

    std::unique_ptr<PlainCommittingBlockHolder> fillNewPartName(MutableDataPartPtr & part, DataPartsLock & lock);
    std::unique_ptr<PlainCommittingBlockHolder> fillNewPartNameAndResetLevel(MutableDataPartPtr & part, DataPartsLock & lock);

    void startBackgroundMovesIfNeeded() override;

    BackupEntries backupMutations(UInt64 version, const String & data_path_in_backup) const;

    /// Attaches restored parts to the storage.
    void attachRestoredParts(MutableDataPartsVector && parts) override;

    std::unique_ptr<MergeTreeSettings> getDefaultSettings() const override;

    PreparedSetsCachePtr getPreparedSetsCache(Int64 mutation_id);

    void assertNotReadonly() const;

    friend class MergeTreeSink;
    friend class MergeTreeSinkPatch;
    friend class MergeTreeData;
    friend class MergePlainMergeTreeTask;
    friend class MutatePlainMergeTreeTask;

    struct DataValidationTasks : public IStorage::DataValidationTasksBase
    {
        DataValidationTasks(DataPartsVector && parts_, ContextPtr context_)
            : parts(std::move(parts_)), it(parts.begin()), context(std::move(context_))
        {}

        DataPartPtr next()
        {
            std::lock_guard lock(mutex);
            if (it == parts.end())
                return nullptr;
            return *(it++);
        }

        size_t size() const override
        {
            std::lock_guard lock(mutex);
            return std::distance(it, parts.end());
        }

        mutable std::mutex mutex;
        DataPartsVector parts;
        DataPartsVector::const_iterator it;

        ContextPtr context;
    };

    struct MutationsSnapshot final : public MutationsSnapshotBase
    {
        using MutationsByVersion = std::map<UInt64, std::shared_ptr<const MutationCommands>>;
        MutationsByVersion mutations_by_version;

        MutationsSnapshot() = default;
        MutationsSnapshot(Params params_, MutationCounters counters_, MutationsByVersion mutations_snapshot, DataPartsVector patches_);

        MutationCommands getOnFlyMutationCommandsForPart(const MergeTreeData::DataPartPtr & part) const override;
        std::shared_ptr<MergeTreeData::IMutationsSnapshot> cloneEmpty() const override { return std::make_shared<MutationsSnapshot>(); }
        NameSet getAllUpdatedColumns() const override;
    };

    class PartMutationBackoffPolicy
    {
        struct PartMutationInfo
        {
            size_t retry_count;
            size_t latest_fail_time_us;
            size_t max_postpone_time_ms;
            size_t max_postpone_power;

            explicit PartMutationInfo(size_t max_postpone_time_ms_)
                            : retry_count(0ull)
                            , latest_fail_time_us(static_cast<size_t>(Poco::Timestamp().epochMicroseconds()))
                            , max_postpone_time_ms(max_postpone_time_ms_)
                            , max_postpone_power((max_postpone_time_ms_) ? (static_cast<size_t>(std::log2(max_postpone_time_ms_))) : (0ull))
            {}


            size_t getNextMinExecutionTimeUsResolution() const
            {
                if (max_postpone_time_ms == 0)
                    return static_cast<size_t>(Poco::Timestamp().epochMicroseconds());
                size_t current_backoff_interval_us = (1 << retry_count) * 1000ul;
                return latest_fail_time_us + current_backoff_interval_us;
            }

            void addPartFailure()
            {
                if (max_postpone_time_ms == 0)
                    return;
                retry_count = std::min(max_postpone_power, retry_count + 1);
                latest_fail_time_us = static_cast<size_t>(Poco::Timestamp().epochMicroseconds());
            }

            bool partCanBeMutated() const
            {
                if (max_postpone_time_ms == 0)
                    return true;

                auto current_time_us = static_cast<size_t>(Poco::Timestamp().epochMicroseconds());
                return current_time_us >= getNextMinExecutionTimeUsResolution();
            }
        };

        using DataPartsWithRetryInfo = std::unordered_map<String, PartMutationInfo>;
        DataPartsWithRetryInfo failed_mutation_parts;
        mutable std::mutex parts_info_lock;

    public:

        void resetMutationFailures()
        {
            std::unique_lock _lock(parts_info_lock);
            failed_mutation_parts.clear();
        }

        void removePartFromFailed(const String & part_name)
        {
            std::unique_lock _lock(parts_info_lock);
            failed_mutation_parts.erase(part_name);
        }

        void addPartMutationFailure (const String& part_name, size_t max_postpone_time_ms_)
        {
            std::unique_lock _lock(parts_info_lock);
            auto part_info_it = failed_mutation_parts.find(part_name);
            if (part_info_it == failed_mutation_parts.end())
            {
                auto [it, success] = failed_mutation_parts.emplace(part_name, PartMutationInfo(max_postpone_time_ms_));
                std::swap(it, part_info_it);
            }
            auto& part_info = part_info_it->second;
            part_info.addPartFailure();
        }

        bool partCanBeMutated(const String& part_name)
        {

            std::unique_lock _lock(parts_info_lock);
            auto iter = failed_mutation_parts.find(part_name);
            if (iter == failed_mutation_parts.end())
                return true;
            return iter->second.partCanBeMutated();
        }
    };
    /// Controls postponing logic for failed mutations.
    PartMutationBackoffPolicy mutation_backoff_policy;

    MutationsSnapshotPtr getMutationsSnapshot(const IMutationsSnapshot::Params & params) const override;
};

}
