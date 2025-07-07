# ClickHouse Query Pipeline: Deep Technical Architecture Analysis

## Executive Summary

This comprehensive technical report provides an in-depth analysis of ClickHouse's query pipeline architecture, covering the complete journey from SQL parsing to distributed execution with implementation-level details. ClickHouse employs a sophisticated multi-stage pipeline leveraging columnar storage, vectorized processing, and distributed computing. This analysis includes specific code structures, internal APIs, memory layouts, and performance characteristics based on the actual implementation.

## 1. Parser Architecture and AST Construction

### 1.1 Parser Implementation Details

ClickHouse uses a hand-written recursive descent parser implemented in C++. The parser architecture consists of several key components:

**Core Parser Classes:**
- `IParser` - Base interface for all parsers
- `IParserBase` - Base implementation with common functionality
- `TokenIterator` - Iterator for processing tokens
- `ParserSelectQuery` - Main SELECT query parser
- `ParserExpression` - Expression parser
- `ParserCreateQuery` - DDL statement parser

**Token Processing:**
```cpp
class TokenIterator {
    const Token * tokens;
    const Token * end;
    size_t max_depth = 0;
    size_t depth = 0;
};
```

The parser processes tokens using a recursive descent approach where each parser component handles specific SQL constructs:

**Parser Hierarchy:**
- `ParserSelectQuery` recursively calls underlying parsers
- `ParserExpressionWithOptionalAlias` handles expressions with aliases
- `ParserTablesInSelectQuery` processes FROM/JOIN clauses
- `ParserOrderByExpressionList` handles ORDER BY clauses

### 1.2 AST Node Structure

The Abstract Syntax Tree is represented by nodes implementing the `IAST` interface:

**Core AST Classes:**
- `IAST` - Base AST node interface
- `ASTSelectQuery` - Represents SELECT statements
- `ASTExpressionList` - List of expressions
- `ASTFunction` - Function calls
- `ASTIdentifier` - Column/table identifiers
- `ASTLiteral` - Literal values

**AST Memory Management:**
AST nodes use shared ownership through `ASTPtr` (shared_ptr<IAST>). Each node contains:
- Children nodes as `AST*` pointers
- Position information for error reporting
- Type information for semantic analysis

### 1.3 Parsing Process Flow

```
SQL Text → Lexer → Tokens → TokenIterator → Parser → AST
```

**Detailed Steps:**
1. **Lexical Analysis**: SQL text tokenized into keywords, identifiers, literals
2. **Syntax Analysis**: Recursive descent parsing builds AST
3. **Error Handling**: Position-aware error reporting with suggestions
4. **Memory Management**: Automatic cleanup through shared_ptr

## 2. Query Analysis and Semantic Processing

### 2.1 Query Analyzer Architecture

ClickHouse has two query analyzers:

**Legacy Analyzer (`ExpressionAnalyzer`):**
- Used for mutations and older query processing
- Implements `ExpressionAnalyzer` and `ExpressionActions`
- Handles semantic analysis and type checking

**New Analyzer (`InterpreterSelectQueryAnalyzer`):**
- Enabled by default in 24.3+
- Uses `QueryTree` abstraction layer between AST and QueryPipeline
- Implements advanced optimizations

### 2.2 Query Tree Structure

The new analyzer introduces the `QueryTree` representation:

**Query Tree Components:**
- `QueryNode` - Root query node
- `TableNode` - Table references
- `ColumnNode` - Column references
- `FunctionNode` - Function calls
- `JoinNode` - Join operations

**Analyzer Pipeline:**
```
AST → QueryTree → Optimization Passes → QueryPlan → Pipeline
```

### 2.3 Type System and Inference

**Type Resolution Process:**
1. **Name Resolution**: Resolve table/column names to storage references
2. **Type Checking**: Validate data types and implicit conversions
3. **Function Resolution**: Resolve function overloads based on argument types
4. **Aggregate Analysis**: Identify and validate aggregate functions

**Key Classes:**
- `IDataType` - Base type interface
- `DataTypeFactory` - Type creation and resolution
- `FunctionFactory` - Function resolution and instantiation

## 3. Query Plan Generation

### 3.1 QueryPlan Architecture

The `QueryPlan` represents the logical execution plan:

**Core Components:**
- `QueryPlan` - Main plan container
- `IQueryPlanStep` - Base step interface
- `ReadFromMergeTree` - Table scan step
- `FilterStep` - WHERE clause processing
- `AggregatingStep` - GROUP BY operations

**Step Types:**
```cpp
class IQueryPlanStep {
    virtual void transformPipeline(QueryPipelineBuilder & pipeline) = 0;
    virtual String getName() const = 0;
};
```

### 3.2 Optimization Phases

**Query Plan Optimizations:**
1. **Predicate Pushdown**: Move filters closer to data sources
2. **Projection Pushdown**: Eliminate unnecessary columns early
3. **Join Reordering**: Optimize join execution order
4. **Index Selection**: Choose optimal indexes for execution

**Implementation Details:**
- `QueryPlanOptimizationSettings` controls optimization behavior
- `buildQueryPlan()` constructs the initial plan
- `optimizeTree()` applies optimization passes

### 3.3 MergeTree Query Planning

**ReadFromMergeTree Step:**
```cpp
class ReadFromMergeTree : public ISourceStep {
    MergeTreeData & storage;
    SelectQueryInfo & query_info;
    Names required_columns;
    size_t max_block_size;
    size_t max_streams;
};
```

**Key Optimizations:**
- **Index Usage**: Primary key and secondary indexes
- **Part Pruning**: Skip irrelevant data parts
- **Granule Selection**: Process only relevant granules
- **Projection Usage**: Utilize materialized projections

## 4. Pipeline Construction and Execution

### 4.1 Processor-Based Pipeline

ClickHouse uses a processor-based execution model:

**Core Interfaces:**
```cpp
class IProcessor {
    virtual Status prepare() = 0;
    virtual void work() = 0;
    virtual InputPorts & getInputs() = 0;
    virtual OutputPorts & getOutputs() = 0;
};
```

**Processor Types:**
- `ISource` - Data sources (table scans)
- `ITransform` - Data transformations (filters, expressions)
- `ISink` - Data sinks (output formatters)
- `ISimpleTransform` - Single input/output transformations

### 4.2 Data Flow Architecture

**Port and Chunk System:**
```cpp
class Port {
    Header header;
    bool is_finished = false;
    State state = State::NotNeeded;
};

class Chunk {
    Columns columns;
    UInt64 num_rows = 0;
    ChunkInfoPtr chunk_info;
};
```

**Data Processing Flow:**
1. **Source Processors**: Read data from storage engines
2. **Transform Processors**: Apply filters, expressions, aggregations
3. **Sink Processors**: Format and output results
4. **Pipeline Execution**: Coordinate processor execution

### 4.3 Execution Engine Details

**PipelineExecutor Implementation:**
- **Thread Pool Management**: Uses `ThreadFromGlobalPool`
- **Work Stealing**: Balances load across threads
- **Memory Management**: Controls memory usage per pipeline
- **Exception Handling**: Propagates exceptions across processors

**Execution Characteristics:**
- **Vectorized Processing**: Operates on data chunks (8192 rows default)
- **SIMD Operations**: Leverages CPU vector instructions
- **Cache Efficiency**: Optimized memory access patterns
- **Parallel Execution**: Multi-threaded pipeline execution

## 5. Storage Engine Integration (MergeTree Deep Dive)

### 5.1 MergeTree Data Organization

**Physical Storage Structure:**
```
/var/lib/clickhouse/data/database/table/
├── partition_id_min_block_max_block_level_mutation/
│   ├── columns.txt          # Column metadata
│   ├── count.txt           # Row count
│   ├── primary.idx         # Primary index
│   ├── column_name.bin     # Column data (compressed)
│   ├── column_name.mrk2    # Mark files (granule pointers)
│   └── checksums.txt       # Data integrity checksums
```

**Part Naming Convention:**
```
<partition_id>_<min_block>_<max_block>_<level>_<mutation_version>
```

### 5.2 Index and Data Access

**Granule-Based Access:**
- **Index Granularity**: 8192 rows per granule (configurable)
- **Sparse Primary Index**: Stores every Nth row value
- **Mark Files**: Point to granule locations in compressed files
- **Range Reading**: Reads relevant granules based on index

**Data Reading Process:**
1. **Index Lookup**: Find relevant granules using primary.idx
2. **Mark Resolution**: Locate granule positions using .mrk2 files
3. **Compressed Reading**: Read and decompress relevant blocks
4. **Filtering**: Apply additional filters on decompressed data

### 5.3 MergeTreeDataSelectExecutor

**Core Selection Logic:**
```cpp
class MergeTreeDataSelectExecutor {
    QueryPlan readFromParts(
        MergeTreeData::DataPartsVector parts,
        const Names & column_names,
        const SelectQueryInfo & query_info,
        ContextPtr context,
        UInt64 max_block_size,
        size_t num_streams
    );
};
```

**Optimization Features:**
- **Part Selection**: Choose relevant data parts
- **Index Usage**: Primary key and skip indexes
- **Parallel Reading**: Multiple streams for large datasets
- **Memory Management**: Control memory usage during reads

## 6. Advanced Features and Optimizations

### 6.1 Data Skipping Indexes

**Skip Index Types:**
- `minmax` - Min/max values per granule
- `set` - Set of unique values
- `bloom_filter` - Bloom filter for membership testing
- `tokenbf_v1` - Token-based bloom filter for text search

**Implementation:**
```cpp
class IMergeTreeIndex {
    virtual void update(const Block & block, size_t * pos, size_t limit) = 0;
    virtual bool mayBeTrueInRange(const Range & range) const = 0;
};
```

### 6.2 Materialized Views and Projections

**Projection Architecture:**
- **Automatic Selection**: Query optimizer chooses optimal projection
- **Incremental Updates**: Maintained automatically during inserts
- **Cost-Based Selection**: Compares projection costs
- **Fallback Mechanism**: Falls back to base table if needed

### 6.3 Memory Management and Compression

**Memory Allocation:**
- **Arena Allocator**: Pool-based allocation for performance
- **Memory Tracking**: Per-query memory usage tracking
- **Spill-to-Disk**: Handles datasets larger than RAM
- **Compression**: Multiple codecs (LZ4, ZSTD, Delta, DoubleDelta)

**Compression Implementation:**
```cpp
class ICompressionCodec {
    virtual UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const = 0;
    virtual UInt32 compress(const char * source, UInt32 source_size, char * dest) const = 0;
    virtual void decompress(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const = 0;
};
```

## 7. Distributed Query Execution

### 7.1 Cluster Architecture

**Distributed Table Engine:**
```cpp
class StorageDistributed : public IStorage {
    Cluster cluster;
    String remote_database;
    String remote_table;
    ShardingKeyExpr sharding_key;
};
```

**Query Distribution Process:**
1. **Shard Selection**: Determine target shards
2. **Query Rewriting**: Modify query for remote execution
3. **Parallel Execution**: Execute on multiple shards
4. **Result Merging**: Combine results from all shards

### 7.2 Network Protocol

**Native Protocol Implementation:**
- **Binary Format**: Efficient serialization of blocks
- **Compression**: Network traffic compression
- **Connection Pooling**: Reuse connections across queries
- **Error Propagation**: Detailed error information across network

### 7.3 Distributed Aggregation

**Two-Stage Aggregation:**
1. **Local Aggregation**: Partial aggregation on each shard
2. **Global Aggregation**: Final aggregation on coordinator
3. **State Transfer**: Serialize/deserialize aggregate states
4. **Memory Management**: Control memory usage across nodes

## 8. Performance Analysis and Tuning

### 8.1 Query Performance Metrics

**Key Performance Indicators:**
- **Rows/Second**: Processing throughput
- **Bytes/Second**: I/O throughput  
- **Memory Usage**: Peak and average consumption
- **CPU Utilization**: Per-core usage statistics
- **Cache Hit Rates**: Index and data cache efficiency

**Profiling Tools:**
- `EXPLAIN PIPELINE` - Show execution pipeline
- `EXPLAIN PLAN` - Display query plan
- `system.query_log` - Query execution statistics
- `system.trace_log` - Sampling profiler data

### 8.2 Configuration Parameters

**Critical Settings:**
```sql
-- Memory Management
max_memory_usage = 10000000000
max_bytes_before_external_group_by = 20000000000
max_bytes_before_external_sort = 20000000000

-- Parallel Execution  
max_threads = 16
max_insert_threads = 16
max_alter_threads = 16

-- I/O Configuration
merge_tree_max_rows_to_use_cache = 128000000
merge_tree_max_bytes_to_use_cache = 2147483648
uncompressed_cache_size = 8589934592

-- Network Settings
max_connections = 1024
keep_alive_timeout = 3
tcp_keep_alive_timeout = 30
```

### 8.3 Optimization Strategies

**Query Optimization:**
1. **Index Design**: Proper ORDER BY and PARTITION BY
2. **Data Types**: Use appropriate types for storage efficiency
3. **Compression**: Select optimal compression codecs
4. **Partitioning**: Design effective partition schemes

**Hardware Optimization:**
1. **Storage**: NVMe SSDs for hot data, HDD for cold data
2. **Memory**: Sufficient RAM for indexes and caches
3. **CPU**: Modern processors with SIMD support
4. **Network**: High-bandwidth for distributed setups

## 9. Error Handling and Debugging

### 9.1 Exception Hierarchy

**Exception Classes:**
```cpp
class Exception : public std::exception {
    int code;
    String message;
    String stack_trace;
};

class NetException : public Exception {};
class ParsingException : public Exception {};
class ExecutionException : public Exception {};
```

### 9.2 Debugging Tools

**System Tables for Debugging:**
- `system.processes` - Current queries
- `system.query_log` - Query history
- `system.part_log` - Part operations
- `system.merge_log` - Merge operations
- `system.crash_log` - Crash information

**Logging Configuration:**
```xml
<logger>
    <level>trace</level>
    <log>/var/log/clickhouse-server/clickhouse-server.log</log>
    <errorlog>/var/log/clickhouse-server/clickhouse-server.err.log</errorlog>
    <size>1000M</size>
    <count>10</count>
</logger>
```

## 10. Future Developments and Roadmap

### 10.1 Analyzer Improvements

**Query Tree Enhancements:**
- Complete migration from legacy ExpressionAnalyzer
- Advanced cost-based optimizations
- Better join algorithms and optimizations
- Improved subquery handling

### 10.2 Semi-Structured Data Support

**JSON Type Evolution:**
- Production-ready JSON type with Variant foundation
- Better handling of nested and dynamic data
- Improved query performance on JSON columns
- Schema inference and evolution

### 10.3 Performance Enhancements

**Vectorization Improvements:**
- Enhanced SIMD utilization
- Better memory access patterns
- Improved cache locality
- Runtime code generation for hot paths

## 11. Implementation Case Studies

### 11.1 ClickHouse Cloud LogHouse

**Scale Statistics:**
- **Data Volume**: 19 PiB uncompressed (37 trillion rows)
- **Compression Ratio**: 17x (19 PiB → 1.13 PiB compressed)
- **Processing Rate**: 1.1 million log lines/second (largest region)
- **Cost Efficiency**: 200x less expensive than Datadog

**Technical Architecture:**
- **Data Collection**: OpenTelemetry agents and gateways
- **Processing Pipeline**: Custom OTel processors in Go
- **Storage**: SharedMergeTree with S3 backend
- **Query Interface**: Enhanced Grafana with custom plugins

### 11.2 Performance Benchmarks

**Query Performance Examples:**
```sql
-- Analytical query on sensor data
SELECT toStartOfDay(timestamp), event, sum(metric_value)
FROM sensor_values 
WHERE site_id = 233 AND timestamp > '2010-01-01'
GROUP BY toStartOfDay(timestamp), event
ORDER BY sum(metric_value) DESC LIMIT 20;

-- Performance: 0.042 seconds, 90K rows processed
-- Index usage: 11/24415 granules read (0.05%)
```

**Optimization Impact:**
- **Proper Index Design**: 2000x reduction in data scanned
- **Compression**: 17-40x storage reduction
- **Vectorization**: Millions of rows/second processing
- **Parallel Execution**: Linear scaling with CPU cores

## 12. Conclusion

ClickHouse's query pipeline represents a sophisticated and highly optimized architecture for analytical query processing. The combination of:

- **Advanced Parsing**: Hand-optimized recursive descent parser
- **Intelligent Optimization**: Two-tier analyzer with advanced optimizations  
- **Efficient Execution**: Processor-based vectorized pipeline
- **Optimal Storage**: Columnar MergeTree with intelligent indexing
- **Distributed Computing**: Cluster-aware query distribution

Creates a system capable of handling petabyte-scale analytical workloads with exceptional performance. The architecture's modular design, extensive optimization capabilities, and robust error handling make it suitable for enterprise-scale deployments while maintaining the flexibility needed for diverse analytical use cases.

Understanding these implementation details enables database administrators, developers, and architects to make informed decisions about schema design, query optimization, and system configuration, ultimately maximizing the performance and efficiency of ClickHouse deployments.

## References and Technical Resources

### Documentation and Specifications
- ClickHouse Official Architecture Documentation
- Query Pipeline Implementation (src/Processors/)
- MergeTree Storage Engine Specification
- Native Protocol Documentation
- Performance Analysis Guidelines

### Source Code Analysis
- Parser Implementation (src/Parsers/)
- Query Analyzer (src/Analyzer/)  
- Execution Pipeline (src/Processors/)
- Storage Engines (src/Storages/)
- Distributed Execution (src/Storages/Distributed/)

### Performance Studies
- ClickHouse vs. Traditional DBMS Benchmarks
- Compression Algorithm Analysis
- Vectorization Performance Studies
- Distributed Query Execution Analysis
- Real-world Deployment Case Studies

---

*This technical report provides comprehensive implementation-level details of ClickHouse's query pipeline architecture based on current source code analysis and production deployments as of 2024.*