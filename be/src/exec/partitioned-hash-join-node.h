// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef IMPALA_EXEC_PARTITIONED_HASH_JOIN_NODE_H
#define IMPALA_EXEC_PARTITIONED_HASH_JOIN_NODE_H

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>
#include <list>
#include <memory>
#include <string>

#include "exec/blocking-join-node.h"
#include "exec/exec-node.h"
#include "exec/partitioned-hash-join-builder.h"
#include "runtime/buffered-block-mgr.h"

#include "gen-cpp/Types_types.h"

namespace impala {

class BloomFilter;
class BufferedBlockMgr;
class BufferedTupleStream;
class MemPool;
class RowBatch;
class RuntimeFilter;
class TupleRow;

/// Operator to perform partitioned hash join, spilling to disk as necessary. This
/// operator implements multiple join modes with the same code algorithm.
///
/// The high-level algorithm is as follows:
///  1. Consume all build input and partition it. No hash tables are maintained.
///  2. Construct hash tables for as many unspilled partitions as possible.
///  3. Consume the probe input. Each probe row is hashed to find the corresponding build
///     partition. If the build partition is in-memory (i.e. not spilled), then the
///     partition's hash table is probed and any matching rows can be outputted. If the
///     build partition is spilled, the probe row must also be spilled for later
///     processing.
///  4. Any spilled partitions are processed. If the build rows and hash table for a
///     spilled partition fit in memory, the spilled partition is brought into memory
///     and its spilled probe rows are processed. Otherwise the spilled partition must be
///     repartitioned into smaller partitions. Repartitioning repeats steps 1-3 above,
///     except with the partition's spilled build and probe rows as input.
///
/// IMPLEMENTATION DETAILS:
/// -----------------------
/// The partitioned hash join algorithm is implemented with the PartitionedHashJoinNode
/// and PhjBuilder classes. Each join node has a builder (see PhjBuilder) that
/// partitions, stores and builds hash tables over the build rows.
///
/// The above algorithm is implemented as a state machine with the following phases:
///
///   1. [PARTITIONING_BUILD or REPARTITIONING_BUILD] Read build rows from child(1) OR
///      from the spilled build rows of a partition and partition them into the builder's
///      hash partitions. If there is sufficient memory, all build partitions are kept
///      in memory. Otherwise, build partitions are spilled as needed to free up memory.
///      Finally, build a hash table for each in-memory partition and create a probe
///      partition with a write buffer for each spilled partition.
///
///      After the phase, the algorithm advances from PARTITIONING_BUILD to
///      PARTITIONING_PROBE or from REPARTITIONING_BUILD to REPARTITIONING_PROBE.
///
///   2. [PARTITIONING_PROBE or REPARTITIONING_PROBE] Read the probe rows from child(0) or
///      a the spilled probe rows of a partition and partition them. If a probe row's
///      partition is in memory, probe the partition's hash table, otherwise spill the
///      probe row. Finally, output unmatched build rows for join modes that require it.
///
///      After the phase, the algorithm terminates if no spilled partitions remain or
///      continues to process one of the remaining spilled partitions by advancing to
///      either PROBING_SPILLED_PARTITION or REPARTITIONING_BUILD, depending on whether
///      the spilled partition's hash table fits in memory or not.
///
///   3. [PROBING_SPILLED_PARTITION] Read the probe rows from a spilled partition that
///      was brought back into memory and probe the partition's hash table. Finally,
///      output unmatched build rows for join modes that require it.
///
///      After the phase, the algorithm terminates if no spilled partitions remain or
///      continues to process one of the remaining spilled partitions by advancing to
///      either PROBING_SPILLED_PARTITION or REPARTITIONING_BUILD, depending on whether
///      the spilled partition's hash table fits in memory or not.
///
/// Null aware anti-join (NAAJ) extends the above algorithm by accumulating rows with
/// NULLs into several different streams, which are processed in a separate step to
/// produce additional output rows. The NAAJ algorithm is documented in more detail in
/// header comments for the null aware functions and data structures.
///
/// TODO: don't copy tuple rows so often.
class PartitionedHashJoinNode : public BlockingJoinNode {
 public:
  PartitionedHashJoinNode(ObjectPool* pool, const TPlanNode& tnode,
      const DescriptorTbl& descs);
  virtual ~PartitionedHashJoinNode();

  virtual Status Init(const TPlanNode& tnode, RuntimeState* state);
  virtual Status Prepare(RuntimeState* state);
  virtual Status Open(RuntimeState* state);
  virtual Status GetNext(RuntimeState* state, RowBatch* row_batch, bool* eos);
  virtual Status Reset(RuntimeState* state);
  virtual void Close(RuntimeState* state);

 protected:
  virtual void AddToDebugString(int indentation_level, std::stringstream* out) const;
  virtual Status ProcessBuildInput(RuntimeState* state);

 private:
  class ProbePartition;

  enum HashJoinState {
    /// Partitioning the build (right) child's input into the builder's hash partitions.
    PARTITIONING_BUILD,

    /// Processing the probe (left) child's input, probing hash tables and
    /// spilling probe rows into 'probe_hash_partitions_' if necessary.
    PARTITIONING_PROBE,

    /// Processing the spilled probe rows of a single spilled partition
    /// ('input_partition_') that fits in memory.
    PROBING_SPILLED_PARTITION,

    /// Repartitioning the build rows of a single spilled partition ('input_partition_')
    /// into the builder's hash partitions.
    /// Corresponds to PARTITIONING_BUILD but reading from a spilled partition.
    REPARTITIONING_BUILD,

    /// Probing the repartitioned hash partitions of a single spilled partition
    /// ('input_partition_') with the probe rows of that partition.
    /// Corresponds to PARTITIONING_PROBE but reading from a spilled partition.
    REPARTITIONING_PROBE,
  };

  /// Constants from PhjBuilder, added to this node for convenience.
  static const int PARTITION_FANOUT = PhjBuilder::PARTITION_FANOUT;
  static const int NUM_PARTITIONING_BITS = PhjBuilder::NUM_PARTITIONING_BITS;
  static const int MAX_PARTITION_DEPTH = PhjBuilder::MAX_PARTITION_DEPTH;

  /// Initialize 'probe_hash_partitions_' and 'hash_tbls_' before probing. One probe
  /// partition is created per spilled build partition, and 'hash_tbls_' is initialized
  /// with pointers to the hash tables of in-memory partitions and NULL pointers for
  /// spilled or closed partitions.
  /// Called after the builder has partitioned the build rows and built hash tables,
  /// either in the initial build step, or after repartitioning a spilled partition.
  /// After this function returns, all partitions are ready to process probe rows.
  Status PrepareForProbe();

  /// Creates an initialized probe partition at 'partition_idx' in
  /// 'probe_hash_partitions_'.
  void CreateProbePartition(
      int partition_idx, std::unique_ptr<BufferedTupleStream> probe_rows);

  /// Append the probe row 'row' to 'stream'. The stream must be unpinned and must have
  /// a write buffer allocated, so this will succeed unless an error is encountered.
  /// Returns false and sets 'status' to an error if an error is encountered. This odd
  /// return convention is used to avoid emitting unnecessary code for ~Status in perf-
  /// critical code.
  bool AppendProbeRow(BufferedTupleStream* stream, TupleRow* row, Status* status);

  /// Probes the hash table for rows matching the current probe row and appends
  /// all the matching build rows (with probe row) to output batch. Returns true
  /// if probing is done for the current probe row and should continue to next row.
  ///
  /// 'out_batch_iterator' is the iterator for the output batch.
  /// 'remaining_capacity' tracks the number of additional rows that can be added to
  /// the output batch. It's updated as rows are added to the output batch.
  /// Using a separate variable is probably faster than calling
  /// 'out_batch_iterator->parent()->AtCapacity()' as it avoids unnecessary memory load.
  bool inline ProcessProbeRowInnerJoin(
      ExprContext* const* other_join_conjunct_ctxs, int num_other_join_conjuncts,
      ExprContext* const* conjunct_ctxs, int num_conjuncts,
      RowBatch::Iterator* out_batch_iterator, int* remaining_capacity);

  /// Probes and updates the hash table for the current probe row for either
  /// RIGHT_SEMI_JOIN or RIGHT_ANTI_JOIN. For RIGHT_SEMI_JOIN, all matching build
  /// rows will be appended to the output batch; For RIGHT_ANTI_JOIN, update the
  /// hash table only if matches are found. The actual output happens in
  /// OutputUnmatchedBuild(). Returns true if probing is done for the current
  /// probe row and should continue to next row.
  ///
  /// 'out_batch_iterator' is the iterator for the output batch.
  /// 'remaining_capacity' tracks the number of additional rows that can be added to
  /// the output batch. It's updated as rows are added to the output batch.
  /// Using a separate variable is probably faster than calling
  /// 'out_batch_iterator->parent()->AtCapacity()' as it avoids unnecessary memory load.
  template<int const JoinOp>
  bool inline ProcessProbeRowRightSemiJoins(
      ExprContext* const* other_join_conjunct_ctxs, int num_other_join_conjuncts,
      ExprContext* const* conjunct_ctxs, int num_conjuncts,
      RowBatch::Iterator* out_batch_iterator, int* remaining_capacity);

  /// Probes the hash table for the current probe row for LEFT_SEMI_JOIN,
  /// LEFT_ANTI_JOIN or NULL_AWARE_LEFT_ANTI_JOIN. The probe row will be appended
  /// to output batch if there is a match (for LEFT_SEMI_JOIN) or if there is no
  /// match (for LEFT_ANTI_JOIN). Returns true if probing is done for the current
  /// probe row and should continue to next row.
  ///
  /// 'out_batch_iterator' is the iterator for the output batch.
  /// 'remaining_capacity' tracks the number of additional rows that can be added to
  /// the output batch. It's updated as rows are added to the output batch.
  /// Using a separate variable is probably faster than calling
  /// 'out_batch_iterator->parent()->AtCapacity()' as it avoids unnecessary memory load.
  template<int const JoinOp>
  bool inline ProcessProbeRowLeftSemiJoins(
      ExprContext* const* other_join_conjunct_ctxs, int num_other_join_conjuncts,
      ExprContext* const* conjunct_ctxs, int num_conjuncts,
      RowBatch::Iterator* out_batch_iterator, int* remaining_capacity, Status* status);

  /// Probes the hash table for the current probe row for LEFT_OUTER_JOIN,
  /// RIGHT_OUTER_JOIN or FULL_OUTER_JOIN. The matching build and/or probe row
  /// will be appended to output batch. For RIGHT/FULL_OUTER_JOIN, some of the outputs
  /// are added in OutputUnmatchedBuild(). Returns true if probing is done for the
  /// current probe row and should continue to next row.
  ///
  /// 'out_batch_iterator' is the iterator for the output batch.
  /// 'remaining_capacity' tracks the number of additional rows that can be added to
  /// the output batch. It's updated as rows are added to the output batch.
  /// Using a separate variable is probably faster than calling
  /// 'out_batch_iterator->parent()->AtCapacity()' as it avoids unnecessary memory load.
  /// 'status' may be updated if appending to null aware BTS fails.
  template<int const JoinOp>
  bool inline ProcessProbeRowOuterJoins(
      ExprContext* const* other_join_conjunct_ctxs, int num_other_join_conjuncts,
      ExprContext* const* conjunct_ctxs, int num_conjuncts,
      RowBatch::Iterator* out_batch_iterator, int* remaining_capacity);

  /// Probes 'current_probe_row_' against the the hash tables and append outputs
  /// to output batch. Wrapper around the join-type specific probe row functions
  /// declared above.
  template<int const JoinOp>
  bool inline ProcessProbeRow(
      ExprContext* const* other_join_conjunct_ctxs, int num_other_join_conjuncts,
      ExprContext* const* conjunct_ctxs, int num_conjuncts,
      RowBatch::Iterator* out_batch_iterator, int* remaining_capacity, Status* status);

  /// Evaluates some number of rows in 'probe_batch_' against the probe expressions
  /// and hashes the results to 32-bit hash values. The evaluation results and the hash
  /// values are stored in the expression values cache in 'ht_ctx'. The number of rows
  /// processed depends on the capacity available in 'ht_ctx->expr_values_cache_'.
  /// 'prefetch_mode' specifies the prefetching mode in use. If it's not PREFETCH_NONE,
  /// hash table buckets will be prefetched based on the hash values computed. Note
  /// that 'prefetch_mode' will be substituted with constants during codegen time.
  void EvalAndHashProbePrefetchGroup(TPrefetchMode::type prefetch_mode,
      HashTableCtx* ctx);

  /// Find the next probe row. Returns true if a probe row is found. In which case,
  /// 'current_probe_row_' and 'hash_tbl_iterator_' have been set up to point to the
  /// next probe row and its corresponding partition. 'status' may be updated if
  /// append to the spilled partitions' BTS or null probe rows' BTS fail.
  template<int const JoinOp>
  bool inline NextProbeRow(
      HashTableCtx* ht_ctx, RowBatch::Iterator* probe_batch_iterator,
      int* remaining_capacity, Status* status);

  /// Process probe rows from probe_batch_. Returns either if out_batch is full or
  /// probe_batch_ is entirely consumed.
  /// For RIGHT_ANTI_JOIN, all this function does is to mark whether each build row
  /// had a match.
  /// Returns the number of rows added to out_batch; -1 on error (and *status will
  /// be set). This function doesn't commit rows to the output batch so it's the caller's
  /// responsibility to do so.
  template<int const JoinOp>
  int ProcessProbeBatch(TPrefetchMode::type, RowBatch* out_batch, HashTableCtx* ht_ctx,
      Status* status);

  /// Wrapper that calls the templated version of ProcessProbeBatch() based on 'join_op'.
  int ProcessProbeBatch(const TJoinOp::type join_op, TPrefetchMode::type,
      RowBatch* out_batch, HashTableCtx* ht_ctx, Status* status);

  /// Sweep the hash_tbl_ of the partition that is at the front of
  /// output_build_partitions_, using hash_tbl_iterator_ and output any unmatched build
  /// rows. If reaches the end of the hash table it closes that partition, removes it from
  /// output_build_partitions_ and moves hash_tbl_iterator_ to the beginning of the
  /// new partition at the front of output_build_partitions_.
  void OutputUnmatchedBuild(RowBatch* out_batch);

  /// Initializes 'null_aware_probe_partition_' and prepares its probe stream for writing.
  Status InitNullAwareProbePartition();

  /// Initializes 'null_probe_rows_' and prepares that stream for writing.
  Status InitNullProbeRows();

  /// Initializes null_aware_partition_ and nulls_build_batch_ to output rows.
  Status PrepareNullAwarePartition();

  /// Continues processing from null_aware_partition_. Called after we have finished
  /// processing all build and probe input (including repartitioning them).
  Status OutputNullAwareProbeRows(RuntimeState* state, RowBatch* out_batch);

  /// Evaluates all other_join_conjuncts against null_probe_rows_ with all the
  /// rows in build. This updates matched_null_probe_, short-circuiting if one of the
  /// conjuncts pass (i.e. there is a match).
  /// This is used for NAAJ, when there are NULL probe rows.
  Status EvaluateNullProbe(BufferedTupleStream* build);

  /// Prepares to output NULLs on the probe side for NAAJ. Before calling this,
  /// matched_null_probe_ should have been fully evaluated.
  Status PrepareNullAwareNullProbe();

  /// Outputs NULLs on the probe side, returning rows where matched_null_probe_[i] is
  /// false. Used for NAAJ.
  Status OutputNullAwareNullProbe(RuntimeState* state, RowBatch* out_batch);

  /// Call at the end of consuming the probe rows. Cleans up the build and probe hash
  /// partitions and:
  ///  - If the build partition had a hash table, close it. The build and probe
  ///    partitions are fully processed. The streams are transferred to 'batch'.
  ///    In the case of right-outer and full-outer joins, instead of closing this
  ///    partition we put it on a list of partitions for which we need to flush their
  ///    unmatched rows.
  ///  - If the build partition did not have a hash table, meaning both build and probe
  ///    rows were spilled, move the partition to 'spilled_partitions_'.
  Status CleanUpHashPartitions(RowBatch* batch);

  /// Get the next row batch from the probe (left) side (child(0)). If we are done
  /// consuming the input, sets 'probe_batch_pos_' to -1, otherwise, sets it to 0.
  Status NextProbeRowBatch(RuntimeState* state, RowBatch* out_batch);

  /// Get the next probe row batch from 'input_partition_'. If we are done consuming the
  /// input, sets 'probe_batch_pos_' to -1, otherwise, sets it to 0.
  Status NextSpilledProbeRowBatch(RuntimeState* state, RowBatch* out_batch);

  /// Moves onto the next spilled partition and initializes 'input_partition_'. This
  /// function processes the entire build side of 'input_partition_' and when this
  /// function returns, we are ready to consume the probe side of 'input_partition_'.
  /// If the build side's hash table fits in memory, we will construct input_partition_'s
  /// hash table. If it does not, meaning we need to repartition, this function will
  /// repartition the build rows into 'builder->hash_partitions_' and prepare for
  /// repartitioning the partition's probe rows.
  Status PrepareSpilledPartitionForProbe(RuntimeState* state, bool* got_partition);

  /// Calls Close() on every probe partition, destroys the partitions and cleans up any
  /// references to the partitions. Also closes and destroys 'null_probe_rows_'.
  void CloseAndDeletePartitions();

  /// Prepares for probing the next batch.
  void ResetForProbe();

  /// Codegen function to create output row. Assumes that the probe row is non-NULL.
  Status CodegenCreateOutputRow(LlvmCodeGen* codegen, llvm::Function** fn);

  /// Codegen processing probe batches.  Identical signature to ProcessProbeBatch.
  /// Returns non-OK if codegen was not possible.
  Status CodegenProcessProbeBatch(RuntimeState* state);

  /// Returns the current state of the partition as a string.
  std::string PrintState() const;

  /// Updates 'state_' to 'next_state', logging the transition.
  void UpdateState(HashJoinState next_state);

  std::string NodeDebugString() const;

  RuntimeState* runtime_state_;

  /// Our equi-join predicates "<lhs> = <rhs>" are separated into
  /// build_expr_ctxs_ (over child(1)) and probe_expr_ctxs_ (over child(0))
  std::vector<ExprContext*> build_expr_ctxs_;
  std::vector<ExprContext*> probe_expr_ctxs_;

  /// Non-equi-join conjuncts from the ON clause.
  std::vector<ExprContext*> other_join_conjunct_ctxs_;

  /// Used for hash-related functionality, such as evaluating rows and calculating hashes.
  boost::scoped_ptr<HashTableCtx> ht_ctx_;

  /// The iterator that corresponds to the look up of current_probe_row_.
  HashTable::Iterator hash_tbl_iterator_;

  /// Number of probe rows that have been partitioned.
  RuntimeProfile::Counter* num_probe_rows_partitioned_;

  /// Time spent evaluating other_join_conjuncts for NAAJ.
  RuntimeProfile::Counter* null_aware_eval_timer_;

  /////////////////////////////////////////
  /// BEGIN: Members that must be Reset()

  /// State of the partitioned hash join algorithm. Used just for debugging.
  HashJoinState state_;

  /// The build-side of the join. Initialized in Init().
  boost::scoped_ptr<PhjBuilder> builder_;

  /// Cache of the per partition hash table to speed up ProcessProbeBatch.
  /// In the case where we need to partition the probe:
  ///  hash_tbls_[i] = builder_->hash_partitions_[i]->hash_tbl();
  /// In the case where we don't need to partition the probe:
  ///  hash_tbls_[i] = input_partition_->hash_tbl();
  HashTable* hash_tbls_[PARTITION_FANOUT];

  /// Probe partitions, with indices corresponding to the build partitions in
  /// builder_->hash_partitions(). This is non-empty only in the PARTITIONING_PROBE or
  /// REPARTITIONING_PROBE states, in which case it has NULL entries for in-memory
  /// build partitions and non-NULL entries for spilled build partitions (so that we
  /// have somewhere to spill the probe rows for the spilled partition).
  std::vector<std::unique_ptr<ProbePartition>> probe_hash_partitions_;

  /// The list of probe partitions that have been spilled and still need more
  /// processing. These partitions could need repartitioning, in which case more
  /// partitions will be added to this list after repartitioning.
  /// This list is populated at CleanUpHashPartitions().
  std::list<std::unique_ptr<ProbePartition>> spilled_partitions_;

  /// The current spilled probe partition being processed as input to repartitioning,
  /// or the source of the probe rows if the hash table fits in memory.
  std::unique_ptr<ProbePartition> input_partition_;

  /// In the case of right-outer and full-outer joins, this is the list of the partitions
  /// for which we need to output their unmatched build rows.
  /// This list is populated at CleanUpHashPartitions().
  std::list<PhjBuilder::Partition*> output_build_partitions_;

  /// Used while processing null_aware_partition_. It contains all the build tuple rows
  /// with a NULL when evaluating the hash table expr.
  boost::scoped_ptr<RowBatch> nulls_build_batch_;

  /// Partition used if 'null_aware_' is set. During probing, rows from the probe
  /// side that did not have a match in the hash table are appended to this partition.
  /// At the very end, we then iterate over the partition's probe rows. For each probe
  /// row, we return the rows that did not match any of the partition's build rows. This
  /// is NULL if this join is not null aware or we are done processing this partition.
  boost::scoped_ptr<ProbePartition> null_aware_probe_partition_;

  /// For NAAJ, this stream contains all probe rows that had NULL on the hash table
  /// conjuncts. Must be unique_ptr so we can release it and transfer to output batches.
  std::unique_ptr<BufferedTupleStream> null_probe_rows_;

  /// For each row in null_probe_rows_, true if this row has matched any build row
  /// (i.e. the resulting joined row passes other_join_conjuncts).
  /// TODO: remove this. We need to be able to put these bits inside the tuple itself.
  std::vector<bool> matched_null_probe_;

  /// The current index into null_probe_rows_/matched_null_probe_ that we are
  /// outputting.
  int64_t null_probe_output_idx_;

  /// END: Members that must be Reset()
  /////////////////////////////////////////

  /// The probe-side partition corresponding to a build partition. The probe partition
  /// is created when a build partition is spilled so that probe rows can be spilled to
  /// disk for later processing.
  class ProbePartition {
   public:
    /// Create a new probe partition. 'probe_rows' should be an empty unpinned stream
    /// that has been prepared for writing with an I/O-sized write buffer.
    ProbePartition(RuntimeState* state, PartitionedHashJoinNode* parent,
        PhjBuilder::Partition* build_partition,
        std::unique_ptr<BufferedTupleStream> probe_rows);
    ~ProbePartition();

    /// Prepare to read the probe rows. Allocates the first read block, so reads will
    /// not fail with out of memory if this succeeds. Returns an error if the first read
    /// block cannot be acquired. "delete_on_read" mode is used, so the blocks backing
    /// the buffered tuple stream will be destroyed after reading.
    Status PrepareForRead();

    /// Close the partition and attach resources to 'batch' if non-NULL or free the
    /// resources if 'batch' is NULL. Idempotent.
    void Close(RowBatch* batch);

    BufferedTupleStream* ALWAYS_INLINE probe_rows() { return probe_rows_.get(); }
    PhjBuilder::Partition* build_partition() { return build_partition_; }

    inline bool IsClosed() const { return probe_rows_ == NULL; }

   private:
    PartitionedHashJoinNode* parent_;

    /// The corresponding build partition. Not NULL. Owned by PhjBuilder.
    PhjBuilder::Partition* build_partition_;

    /// Stream of probe tuples in this partition. Initially owned by this object but
    /// transferred to the parent exec node (via the row batch) when the partition
    /// is complete. If NULL, ownership was transferred and the partition is closed.
    std::unique_ptr<BufferedTupleStream> probe_rows_;
  };

  /// For the below codegen'd functions, xxx_fn_level0_ uses CRC hashing when available
  /// and is used when the partition level is 0, otherwise xxx_fn_ uses murmur hash and is
  /// used for subsequent levels.

  typedef int (*ProcessProbeBatchFn)(PartitionedHashJoinNode*,
      TPrefetchMode::type, RowBatch*, HashTableCtx*, Status*);
  /// Jitted ProcessProbeBatch function pointers.  NULL if codegen is disabled.
  ProcessProbeBatchFn process_probe_batch_fn_;
  ProcessProbeBatchFn process_probe_batch_fn_level0_;

};

}

#endif
