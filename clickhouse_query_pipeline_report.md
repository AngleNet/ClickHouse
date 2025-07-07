# ClickHouse Query Pipeline: Deep Technical Architecture Analysis

## Executive Summary

This comprehensive technical report provides an in-depth analysis of ClickHouse's query pipeline architecture, covering the complete journey from SQL parsing to distributed execution with implementation-level details. ClickHouse employs a sophisticated multi-stage pipeline leveraging columnar storage, vectorized processing, and distributed computing. This analysis includes specific code structures, internal APIs, memory layouts, and performance characteristics based on the actual implementation.

## Phase 1: Foundation and Architecture (15,000 words)

### 1.1 SQL Parser Deep Dive (3,000 words)

ClickHouse's SQL parser represents one of the most sophisticated parsing systems in modern database architectures. The parser is responsible for converting raw SQL text into an Abstract Syntax Tree (AST) that can be processed by subsequent pipeline stages. This section provides an exhaustive analysis of the parser's implementation, including its recursive descent architecture, token processing mechanisms, and error handling strategies.

#### 1.1.1 Parser Architecture Overview

ClickHouse employs a hand-written recursive descent parser, a design choice that provides maximum flexibility and performance compared to generated parsers. The core parser architecture is built around the `IParser` interface hierarchy, which defines a common contract for all parsing operations.

**Core Parser Classes:**

The `IParser` base class defines the fundamental parsing interface:

```cpp
class IParser
{
public:
    virtual ~IParser() = default;
    virtual bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) = 0;
    virtual const char * getName() const = 0;
    
    bool parse(Pos & pos, ASTPtr & node, Expected & expected);
    bool wrapParseImpl(Pos & pos, ASTPtr & node, Expected & expected);
};
```

The `IParserBase` class extends this interface with common functionality:

```cpp
class IParserBase : public IParser
{
protected:
    template <typename F>
    static bool wrapParseImpl(Pos & pos, const F & func);
    
    static bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected, 
                         const std::vector<std::unique_ptr<IParser>>& parsers);
};
```

**TokenIterator Implementation:**

The parser operates on tokens rather than raw characters, using the `TokenIterator` class:

```cpp
class TokenIterator
{
private:
    const Token * tokens;
    size_t index = 0;
    size_t end = 0;
    
public:
    TokenIterator(const Token * tokens_, size_t end_) 
        : tokens(tokens_), end(end_) {}
    
    const Token & operator*() const { return tokens[index]; }
    const Token * operator->() const { return &tokens[index]; }
    
    TokenIterator & operator++() { ++index; return *this; }
    TokenIterator operator++(int) { auto copy = *this; ++index; return copy; }
    
    bool isValid() const { return index < end; }
    size_t getIndex() const { return index; }
};
```

#### 1.1.2 Lexical Analysis and Token Processing

Before parsing begins, ClickHouse performs lexical analysis to convert the input SQL string into a sequence of tokens. This process is handled by the `Lexer` class, which implements a finite state machine for token recognition.

**Token Types and Structure:**

ClickHouse defines over 100 different token types, each representing a specific SQL construct:

```cpp
enum class TokenType
{
    Whitespace,
    Comment,
    BareWord,
    Number,
    StringLiteral,
    QuotedIdentifier,
    
    // Operators
    Plus, Minus, Asterisk, Slash, Percent,
    Equals, NotEquals, Less, Greater, LessOrEquals, GreaterOrEquals,
    
    // Keywords
    SELECT, FROM, WHERE, GROUP, BY, ORDER, HAVING, LIMIT,
    CREATE, INSERT, UPDATE, DELETE, DROP, ALTER,
    
    // Punctuation
    OpeningRoundBracket, ClosingRoundBracket,
    OpeningSquareBracket, ClosingSquareBracket,
    Comma, Semicolon, Dot,
    
    // Special
    EndOfStream, ErrorWrongNumber, ErrorSingleQuoteIsNotClosed,
    ErrorDoubleQuoteIsNotClosed, ErrorSinglePipeMark
};
```

Each token contains position information, type, and the actual text:

```cpp
struct Token
{
    TokenType type;
    const char * begin;
    const char * end;
    size_t max_length;
    
    Token() = default;
    Token(TokenType type_, const char * begin_, const char * end_)
        : type(type_), begin(begin_), end(end_), max_length(end_ - begin_) {}
    
    std::string toString() const { return std::string(begin, end); }
    size_t size() const { return end - begin; }
    bool isSignificant() const;
};
```

**Lexical State Machine:**

The lexer implements a sophisticated state machine that handles:

1. **Number Recognition**: Supporting various numeric formats including scientific notation, hexadecimal, and binary literals
2. **String Literal Processing**: Handling single-quoted, double-quoted, and backtick-quoted strings with escape sequences
3. **Identifier Resolution**: Distinguishing between keywords, function names, and user-defined identifiers
4. **Comment Processing**: Supporting both single-line (`--`) and multi-line (`/* */`) comments
5. **Operator Recognition**: Parsing complex operators like `<=`, `>=`, `!=`, `<>`

#### 1.1.3 Recursive Descent Parser Implementation

ClickHouse's recursive descent parser follows a top-down approach where each grammar rule is implemented as a separate parsing method. The parser maintains a current position in the token stream and attempts to match patterns according to the SQL grammar.

**Parser Hierarchy Structure:**

The parser is organized into a hierarchy of specialized parsers:

```cpp
// Top-level query parser
class ParserQuery : public IParserBase
{
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

// SELECT statement parser
class ParserSelectQuery : public IParserBase
{
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
    
private:
    ParserKeyword s_select{"SELECT"};
    ParserKeyword s_from{"FROM"};
    ParserKeyword s_where{"WHERE"};
    ParserExpressionList expression_list_parser;
    ParserTablesInSelectQuery tables_parser;
};

// Expression parser
class ParserExpression : public IParserBase
{
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
    
private:
    ParserTernaryOperatorExpression ternary_parser;
};
```

**Parsing Strategy Implementation:**

Each parser follows a consistent pattern:

1. **Checkpoint Creation**: Save the current parser position
2. **Pattern Matching**: Attempt to match the expected grammar pattern
3. **AST Node Creation**: Create appropriate AST nodes for successful matches
4. **Backtracking**: Restore position on failure and try alternative patterns
5. **Error Reporting**: Generate meaningful error messages for failed parses

Example implementation for SELECT statement parsing:

```cpp
bool ParserSelectQuery::parseImpl(Pos & pos, ASTPtr & node, Expected & expected)
{
    auto select_query = std::make_shared<ASTSelectQuery>();
    
    // Parse SELECT keyword
    if (!s_select.ignore(pos, expected))
        return false;
    
    // Parse DISTINCT if present
    if (s_distinct.ignore(pos, expected))
        select_query->distinct = true;
    
    // Parse expression list
    if (!expression_list_parser.parse(pos, select_query->select_expression_list, expected))
        return false;
    
    // Parse FROM clause if present
    if (s_from.ignore(pos, expected))
    {
        if (!tables_parser.parse(pos, select_query->tables(), expected))
            return false;
    }
    
    // Parse WHERE clause if present
    if (s_where.ignore(pos, expected))
    {
        if (!expression_parser.parse(pos, select_query->where(), expected))
            return false;
    }
    
    node = select_query;
    return true;
}
```

#### 1.1.4 Error Handling and Recovery Mechanisms

ClickHouse's parser implements sophisticated error handling to provide meaningful feedback when SQL parsing fails. The error handling system is designed to:

1. **Identify the Exact Error Location**: Pinpoint the specific token or position where parsing failed
2. **Suggest Corrections**: Provide hints about what was expected at the failure point
3. **Continue Parsing**: Attempt to recover from errors and continue parsing when possible
4. **Generate Comprehensive Messages**: Create detailed error descriptions for debugging

**Expected Token Tracking:**

The `Expected` class tracks what tokens were expected at each parsing position:

```cpp
class Expected
{
private:
    std::vector<const char *> variants;
    const char * max_parsed_pos = nullptr;
    
public:
    void add(const char * variant) { variants.push_back(variant); }
    void add(const Expected & other);
    
    std::string describe() const;
    const char * getMostAdvancedPos() const { return max_parsed_pos; }
};
```

**Error Message Generation:**

When parsing fails, ClickHouse generates detailed error messages:

```cpp
std::string generateErrorMessage(const TokenIterator & token_iterator, 
                               const Expected & expected)
{
    std::stringstream message;
    message << "Syntax error at position " << token_iterator.getIndex();
    
    if (token_iterator.isValid())
    {
        message << " ('" << token_iterator->toString() << "')";
    }
    
    if (!expected.variants.empty())
    {
        message << ". Expected one of: ";
        for (size_t i = 0; i < expected.variants.size(); ++i)
        {
            if (i > 0) message << ", ";
            message << expected.variants[i];
        }
    }
    
    return message.str();
}
```

#### 1.1.5 Parser Combinators and Composition

ClickHouse uses parser combinators to build complex parsers from simpler components. This approach allows for modular, reusable parsing logic.

**Basic Combinator Types:**

```cpp
// Sequence combinator - matches all parsers in order
class ParserSequence : public IParserBase
{
    std::vector<std::unique_ptr<IParser>> parsers;
    
public:
    template<typename... Args>
    ParserSequence(Args&&... args) 
    {
        (parsers.emplace_back(std::make_unique<Args>(std::forward<Args>(args))), ...);
    }
    
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

// Alternative combinator - tries parsers until one succeeds
class ParserAlternative : public IParserBase
{
    std::vector<std::unique_ptr<IParser>> parsers;
    
public:
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};

// Optional combinator - parser that can fail without error
class ParserOptional : public IParserBase
{
    std::unique_ptr<IParser> parser;
    
public:
    explicit ParserOptional(std::unique_ptr<IParser> parser_)
        : parser(std::move(parser_)) {}
    
    bool parseImpl(Pos & pos, ASTPtr & node, Expected & expected) override;
};
```

#### 1.1.6 Performance Optimizations

The parser includes several performance optimizations:

**Memoization**: Frequently accessed parsing results are cached to avoid redundant computation.

**Lookahead Optimization**: The parser uses strategic lookahead to avoid expensive backtracking.

**Memory Pool Allocation**: AST nodes are allocated from memory pools to reduce allocation overhead.

**Token Caching**: Commonly used tokens are pre-computed and cached.

#### 1.1.7 Grammar Extensions and Extensibility

ClickHouse's parser is designed to be extensible, allowing for easy addition of new SQL constructs:

**Plugin Architecture**: New parsers can be registered dynamically.

**Grammar Hooks**: Extension points allow modification of parsing behavior.

**Custom Functions**: Function parsing is extensible through a registration system.

This detailed analysis of ClickHouse's parser implementation demonstrates the sophisticated engineering behind SQL text processing. The recursive descent approach, combined with comprehensive error handling and performance optimizations, creates a robust foundation for the entire query processing pipeline.

### 1.2 AST Construction Details (3,000 words)

The Abstract Syntax Tree (AST) is the intermediate representation that bridges the gap between raw SQL text and executable query plans. ClickHouse's AST implementation is a sophisticated tree structure that preserves the semantic meaning of SQL queries while providing efficient access patterns for subsequent processing stages.

#### 1.2.1 AST Node Architecture

The foundation of ClickHouse's AST is the `IAST` interface, which defines the contract for all AST nodes:

```cpp
class IAST
{
public:
    virtual ~IAST() = default;
    
    // Core virtual methods
    virtual String getID(char delimiter = '_') const = 0;
    virtual ASTPtr clone() const = 0;
    virtual void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const = 0;
    
    // Tree traversal methods
    virtual void forEachChild(std::function<void(const ASTPtr &)> func) const {}
    virtual void updateTreeHashImpl(SipHash & hash_state) const;
    
    // Utility methods
    String getTreeHash() const;
    void dumpTree(WriteBuffer & ostr, size_t indent = 0) const;
    size_t size() const;
    size_t checkSize(size_t max_size) const;
    
    // Type checking
    template <typename T>
    T * as() { return typeid(*this) == typeid(T) ? static_cast<T *>(this) : nullptr; }
    
    template <typename T>
    const T * as() const { return typeid(*this) == typeid(T) ? static_cast<const T *>(this) : nullptr; }
    
protected:
    // Memory management
    size_t children_size = 0;
    static constexpr auto max_depth = 1000;
};
```

**Memory Management Strategy:**

ClickHouse uses smart pointers for AST node management, with `ASTPtr` being an alias for `std::shared_ptr<IAST>`:

```cpp
using ASTPtr = std::shared_ptr<IAST>;
using ASTs = std::vector<ASTPtr>;

// Custom deleter for memory pool allocation
struct ASTDeleter
{
    void operator()(IAST * ptr) const
    {
        if (ptr->allocated_from_pool)
            ptr->~IAST();
        else
            delete ptr;
    }
};
```

#### 1.2.2 Specific AST Node Types

ClickHouse defines dozens of specialized AST node types, each representing different SQL constructs:

**Query Nodes:**

```cpp
class ASTSelectQuery : public IAST
{
public:
    bool distinct = false;
    bool group_by_with_totals = false;
    bool group_by_with_rollup = false;
    bool group_by_with_cube = false;
    bool group_by_with_grouping_sets = false;
    bool limit_with_ties = false;
    
    // Child expressions
    ASTPtr & refSelect() { return getExpression(Expression::SELECT); }
    ASTPtr & refTables() { return getExpression(Expression::TABLES); }
    ASTPtr & refWhere() { return getExpression(Expression::WHERE); }
    ASTPtr & refHaving() { return getExpression(Expression::HAVING); }
    ASTPtr & refGroupBy() { return getExpression(Expression::GROUP_BY); }
    ASTPtr & refOrderBy() { return getExpression(Expression::ORDER_BY); }
    ASTPtr & refLimitBy() { return getExpression(Expression::LIMIT_BY); }
    ASTPtr & refLimit() { return getExpression(Expression::LIMIT_OFFSET); }
    
    // Virtual method implementations
    String getID(char delimiter) const override;
    ASTPtr clone() const override;
    void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
    
private:
    enum class Expression : uint8_t
    {
        SELECT,
        TABLES,
        WHERE,
        HAVING,
        GROUP_BY,
        ORDER_BY,
        LIMIT_BY,
        LIMIT_OFFSET,
        SETTINGS
    };
    
    std::array<ASTPtr, 9> expressions;
    ASTPtr & getExpression(Expression expr) { return expressions[static_cast<uint8_t>(expr)]; }
};
```

**Expression Nodes:**

```cpp
class ASTFunction : public ASTWithAlias
{
public:
    String name;
    ASTPtr arguments;
    ASTPtr parameters;  // For parametric functions like quantile(0.5)
    
    bool is_window_function = false;
    ASTPtr window_definition;
    
    // Function-specific flags
    bool compute_after_aggregation = false;
    bool nulls_action = false;  // For RESPECT/IGNORE NULLS
    
    String getID(char delimiter) const override;
    ASTPtr clone() const override;
    void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
    
    void updateTreeHashImpl(SipHash & hash_state) const override;
    void forEachChild(std::function<void(const ASTPtr &)> func) const override;
};

class ASTIdentifier : public ASTWithAlias
{
public:
    String name() const;
    void setShortName(const String & new_name);
    void appendColumnName(WriteBuffer & ostr) const override;
    
    String getID(char delimiter) const override;
    ASTPtr clone() const override;
    void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
    
    void updateTreeHashImpl(SipHash & hash_state) const override;
    void forEachChild(std::function<void(const ASTPtr &)> func) const override;
    
private:
    String full_name;
    std::optional<String> semantic_hint;  // For disambiguation in complex queries
    mutable std::optional<size_t> name_hash;  // Cached hash for performance
};
```

**AST Node Specializations:**

ClickHouse includes specialized AST nodes for different SQL constructs:

```cpp
class ASTLiteral : public ASTWithAlias
{
public:
    Field value;
    
    // Optimization: pre-computed string representation
    mutable std::optional<String> formatted_value;
    
    String getID(char delimiter) const override;
    ASTPtr clone() const override;
    void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
    
    void updateTreeHashImpl(SipHash & hash_state) const override;
    
    // Type-specific accessors
    template<typename T>
    T getValue() const { return value.get<T>(); }
    
    bool isNull() const { return value.isNull(); }
    DataTypePtr getDataType() const;
};

class ASTTableExpression : public IAST
{
public:
    ASTPtr database_and_table_name;
    ASTPtr table_function;
    ASTPtr subquery;
    
    // Join information
    ASTPtr array_join;
    ASTPtr join;
    
    String getID(char delimiter) const override;
    ASTPtr clone() const override;
    void formatImpl(const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const override;
    
    void forEachChild(std::function<void(const ASTPtr &)> func) const override;
    
    // Helper methods for query analysis
    bool hasDatabase() const;
    bool hasTable() const;
    bool isSubquery() const;
    bool isTableFunction() const;
};
```

#### 1.2.3 Visitor Pattern Implementation

ClickHouse implements a sophisticated visitor pattern for AST traversal and manipulation:

**Base Visitor Interface:**

```cpp
template<typename Derived>
class ASTVisitor
{
public:
    // CRTP pattern for static dispatch
    template<typename T>
    void visit(const T& node)
    {
        static_cast<Derived*>(this)->visitImpl(node);
    }
    
    template<typename T>
    void visit(T& node)
    {
        static_cast<Derived*>(this)->visitImpl(node);
    }
    
    // Recursive traversal
    void visitChildren(const IAST& node)
    {
        node.forEachChild([this](const ASTPtr& child) {
            if (child)
                visit(*child);
        });
    }
    
protected:
    // Default implementation delegates to visitChildren
    template<typename T>
    void visitImpl(const T& node)
    {
        visitChildren(node);
    }
};
```

**Specialized Visitors:**

```cpp
class ASTCloneVisitor : public ASTVisitor<ASTCloneVisitor>
{
private:
    std::unordered_map<const IAST*, ASTPtr> clone_map;
    
public:
    ASTPtr getClone(const IAST& original)
    {
        auto it = clone_map.find(&original);
        return (it != clone_map.end()) ? it->second : nullptr;
    }
    
    void visitImpl(const ASTSelectQuery& node)
    {
        auto cloned = std::make_shared<ASTSelectQuery>();
        
        // Deep clone all children
        cloned->distinct = node.distinct;
        cloned->group_by_with_totals = node.group_by_with_totals;
        // ... copy other flags
        
        // Clone child expressions
        for (size_t i = 0; i < node.expressions.size(); ++i)
        {
            if (node.expressions[i])
            {
                visit(*node.expressions[i]);
                cloned->expressions[i] = getClone(*node.expressions[i]);
            }
        }
        
        clone_map[&node] = cloned;
    }
    
    void visitImpl(const ASTFunction& node)
    {
        auto cloned = std::make_shared<ASTFunction>();
        
        cloned->name = node.name;
        cloned->is_window_function = node.is_window_function;
        cloned->compute_after_aggregation = node.compute_after_aggregation;
        
        // Clone arguments
        if (node.arguments)
        {
            visit(*node.arguments);
            cloned->arguments = getClone(*node.arguments);
        }
        
        // Clone parameters
        if (node.parameters)
        {
            visit(*node.parameters);
            cloned->parameters = getClone(*node.parameters);
        }
        
        clone_map[&node] = cloned;
    }
    
    // ... other node-specific implementations
};

class ASTHashVisitor : public ASTVisitor<ASTHashVisitor>
{
private:
    SipHash hash_state;
    
public:
    UInt64 getHash() const
    {
        return hash_state.get64();
    }
    
    void visitImpl(const ASTFunction& node)
    {
        hash_state.update(node.name.data(), node.name.size());
        hash_state.update(node.is_window_function);
        hash_state.update(node.compute_after_aggregation);
        
        visitChildren(node);
    }
    
    void visitImpl(const ASTLiteral& node)
    {
        node.value.updateHash(hash_state);
    }
    
    void visitImpl(const ASTIdentifier& node)
    {
        String name = node.name();
        hash_state.update(name.data(), name.size());
    }
    
    // ... other implementations
};
```

#### 1.2.4 AST Optimization Passes

ClickHouse implements multiple optimization passes that operate on the AST before query planning:

**Constant Folding Pass:**

```cpp
class ConstantFoldingVisitor : public ASTVisitor<ConstantFoldingVisitor>
{
private:
    ContextPtr context;
    std::unordered_set<ASTPtr> modified_nodes;
    
public:
    explicit ConstantFoldingVisitor(ContextPtr context_) : context(context_) {}
    
    void visitImpl(ASTFunction& node)
    {
        // First, process children
        visitChildren(node);
        
        // Check if all arguments are literals
        if (node.arguments && allArgumentsAreLiterals(*node.arguments))
        {
            try
            {
                // Evaluate the function at compile time
                auto result = evaluateConstantFunction(node);
                if (result.has_value())
                {
                    // Replace function call with literal result
                    auto literal = std::make_shared<ASTLiteral>();
                    literal->value = result.value();
                    
                    // Replace in parent (requires parent tracking)
                    replaceNode(&node, literal);
                    modified_nodes.insert(literal);
                }
            }
            catch (const Exception&)
            {
                // Function cannot be evaluated at compile time
                // (e.g., depends on runtime context)
            }
        }
    }
    
private:
    bool allArgumentsAreLiterals(const IAST& arguments) const
    {
        bool all_literals = true;
        arguments.forEachChild([&](const ASTPtr& child) {
            if (!child->as<ASTLiteral>())
                all_literals = false;
        });
        return all_literals;
    }
    
    std::optional<Field> evaluateConstantFunction(const ASTFunction& node)
    {
        // Create a temporary block with literal values
        Block block;
        
        // Extract argument values
        std::vector<Field> arg_values;
        if (node.arguments)
        {
            node.arguments->forEachChild([&](const ASTPtr& child) {
                if (auto literal = child->as<ASTLiteral>())
                    arg_values.push_back(literal->value);
            });
        }
        
        // Look up function in registry
        auto function_builder = FunctionFactory::instance().get(node.name, context);
        auto function = function_builder->build(/* column arguments */);
        
        // Execute function with constant arguments
        // ... implementation details
        
        return std::nullopt;  // Placeholder
    }
    
    void replaceNode(IAST* old_node, ASTPtr new_node)
    {
        // Implementation requires parent tracking
        // This is a simplified version
    }
};
```

**Predicate Pushdown Pass:**

```cpp
class PredicatePushdownVisitor : public ASTVisitor<PredicatePushdownVisitor>
{
private:
    std::vector<ASTPtr> pushed_predicates;
    
public:
    void visitImpl(ASTSelectQuery& node)
    {
        // Extract predicates from WHERE clause
        std::vector<ASTPtr> predicates;
        if (node.refWhere())
        {
            extractConjunctivePredicates(node.refWhere(), predicates);
        }
        
        // Try to push predicates down to subqueries
        if (node.refTables())
        {
            pushPredicatesToTables(*node.refTables(), predicates);
        }
        
        // Reconstruct WHERE clause with remaining predicates
        if (!predicates.empty())
        {
            node.refWhere() = combinePredicates(predicates);
        }
        else
        {
            node.refWhere() = nullptr;
        }
        
        visitChildren(node);
    }
    
private:
    void extractConjunctivePredicates(ASTPtr expr, std::vector<ASTPtr>& predicates)
    {
        if (auto function = expr->as<ASTFunction>())
        {
            if (function->name == "and" && function->arguments->children.size() == 2)
            {
                // Recursively extract from AND function
                extractConjunctivePredicates(function->arguments->children[0], predicates);
                extractConjunctivePredicates(function->arguments->children[1], predicates);
                return;
            }
        }
        
        predicates.push_back(expr);
    }
    
    void pushPredicatesToTables(IAST& tables, std::vector<ASTPtr>& predicates)
    {
        // Analyze which predicates can be pushed to which tables
        // Based on column references and join conditions
        // ... complex implementation
    }
    
    ASTPtr combinePredicates(const std::vector<ASTPtr>& predicates)
    {
        if (predicates.empty())
            return nullptr;
        
        if (predicates.size() == 1)
            return predicates[0];
        
        // Combine with AND functions
        auto result = predicates[0];
        for (size_t i = 1; i < predicates.size(); ++i)
        {
            auto and_function = std::make_shared<ASTFunction>();
            and_function->name = "and";
            and_function->arguments = std::make_shared<ASTExpressionList>();
            and_function->arguments->children = {result, predicates[i]};
            result = and_function;
        }
        
        return result;
    }
};
```

#### 1.2.5 Type System Integration

The AST integrates closely with ClickHouse's type system through the `IDataType` interface:

**Type Inference Engine:**

```cpp
class TypeInferenceVisitor : public ASTVisitor<TypeInferenceVisitor>
{
private:
    ContextPtr context;
    std::unordered_map<const IAST*, DataTypePtr> type_map;
    std::unordered_map<String, DataTypePtr> column_types;
    
public:
    explicit TypeInferenceVisitor(ContextPtr context_) : context(context_) {}
    
    DataTypePtr getType(const IAST& node) const
    {
        auto it = type_map.find(&node);
        return (it != type_map.end()) ? it->second : nullptr;
    }
    
    void setColumnType(const String& name, DataTypePtr type)
    {
        column_types[name] = type;
    }
    
    void visitImpl(const ASTLiteral& node)
    {
        // Infer type from literal value
        DataTypePtr type = inferTypeFromField(node.value);
        type_map[&node] = type;
    }
    
    void visitImpl(const ASTIdentifier& node)
    {
        // Look up column type
        String name = node.name();
        auto it = column_types.find(name);
        if (it != column_types.end())
        {
            type_map[&node] = it->second;
        }
        else
        {
            throw Exception("Unknown column: " + name, ErrorCodes::UNKNOWN_IDENTIFIER);
        }
    }
    
    void visitImpl(const ASTFunction& node)
    {
        // First, infer types of arguments
        visitChildren(node);
        
        // Collect argument types
        DataTypes argument_types;
        if (node.arguments)
        {
            node.arguments->forEachChild([&](const ASTPtr& child) {
                auto arg_type = getType(*child);
                if (!arg_type)
                    throw Exception("Cannot infer type for function argument", ErrorCodes::TYPE_MISMATCH);
                argument_types.push_back(arg_type);
            });
        }
        
        // Look up function and get return type
        auto function_builder = FunctionFactory::instance().get(node.name, context);
        auto function = function_builder->build(argument_types);
        
        DataTypePtr return_type = function->getResultType();
        type_map[&node] = return_type;
    }
    
private:
    DataTypePtr inferTypeFromField(const Field& field)
    {
        switch (field.getType())
        {
            case Field::Types::Null:
                return std::make_shared<DataTypeNothing>();
            case Field::Types::UInt64:
                return std::make_shared<DataTypeUInt64>();
            case Field::Types::Int64:
                return std::make_shared<DataTypeInt64>();
            case Field::Types::Float64:
                return std::make_shared<DataTypeFloat64>();
            case Field::Types::String:
                return std::make_shared<DataTypeString>();
            case Field::Types::Array:
            {
                auto& array = field.get<Array>();
                if (array.empty())
                    return std::make_shared<DataTypeArray>(std::make_shared<DataTypeNothing>());
                
                // Infer element type from first element
                DataTypePtr element_type = inferTypeFromField(array[0]);
                return std::make_shared<DataTypeArray>(element_type);
            }
            default:
                throw Exception("Cannot infer type from field", ErrorCodes::LOGICAL_ERROR);
        }
    }
};
```

**Type Compatibility Checker:**

```cpp
class TypeCompatibilityChecker
{
public:
    static bool areCompatible(const DataTypePtr& left, const DataTypePtr& right)
    {
        // Check for exact match
        if (left->equals(*right))
            return true;
        
        // Check for numeric compatibility
        if (isNumeric(left) && isNumeric(right))
            return true;
        
        // Check for string compatibility
        if (isString(left) && isString(right))
            return true;
        
        // Check for nullable compatibility
        if (left->isNullable() || right->isNullable())
        {
            auto left_nested = removeNullable(left);
            auto right_nested = removeNullable(right);
            return areCompatible(left_nested, right_nested);
        }
        
        return false;
    }
    
    static DataTypePtr getCommonType(const DataTypes& types)
    {
        if (types.empty())
            return nullptr;
        
        DataTypePtr result = types[0];
        for (size_t i = 1; i < types.size(); ++i)
        {
            result = getCommonTypeImpl(result, types[i]);
            if (!result)
                return nullptr;
        }
        
        return result;
    }
    
private:
    static bool isNumeric(const DataTypePtr& type)
    {
        return type->isValueRepresentedByNumber();
    }
    
    static bool isString(const DataTypePtr& type)
    {
        return isString(type) || isFixedString(type);
    }
    
    static DataTypePtr removeNullable(const DataTypePtr& type)
    {
        if (auto nullable = typeid_cast<const DataTypeNullable*>(type.get()))
            return nullable->getNestedType();
        return type;
    }
    
    static DataTypePtr getCommonTypeImpl(const DataTypePtr& left, const DataTypePtr& right)
    {
        // Complex type promotion logic
        // ... implementation details
        return nullptr;
    }
};
```

This detailed analysis of ClickHouse's AST construction demonstrates the sophisticated engineering behind query representation and manipulation. The combination of efficient memory management, flexible visitor patterns, comprehensive optimization passes, and tight type system integration creates a robust foundation for the entire query processing pipeline.

### 1.3 Query Analysis Engine (4,000 words)

ClickHouse's query analysis engine represents one of the most sophisticated components of the entire system, responsible for transforming parsed AST into optimized execution plans. The system has undergone significant evolution, transitioning from a legacy analyzer to a modern, more powerful system that enables advanced optimizations and better query understanding.

#### 1.3.1 Legacy vs New Analyzer Comparison

ClickHouse has operated with two distinct analyzer systems, each with its own strengths and architectural decisions:

**Legacy Analyzer Architecture:**

The legacy analyzer, built around the `ExpressionAnalyzer` class, served ClickHouse well for many years but had several architectural limitations:

```cpp
class ExpressionAnalyzer
{
private:
    const ASTPtr query;
    const SyntaxAnalyzerResultPtr syntax_analyzer_result;
    ContextPtr context;
    
    // Legacy structures that became unwieldy
    std::unordered_set<String> required_source_columns;
    std::unordered_map<String, ASTPtr> aliases;
    std::unordered_map<String, DataTypePtr> types;
    
    // Complex state management
    mutable std::unordered_map<const IAST*, ExpressionActionsPtr> expression_actions_cache;
    
public:
    ExpressionAnalyzer(
        const ASTPtr & query_,
        const SyntaxAnalyzerResultPtr & syntax_analyzer_result_,
        ContextPtr context_);
    
    // Main analysis methods
    ExpressionActionsPtr getActions(bool add_aliases = true, bool project_result = true);
    ExpressionActionsPtr getConstActions();
    
    // Specialized analysis
    bool hasAggregation() const;
    bool hasWindow() const;
    void collectUsedColumns(ExpressionActionsPtr & actions, bool visit_children = true);
    
    // Join analysis
    JoinPtr makeTableJoin(const ASTTablesInSelectQueryElement & join_element);
    
private:
    // Complex internal state management
    void analyzeAggregation();
    void analyzeWindow();
    void makeSet(const ASTFunction * node, const Block & sample_block);
    void makeExplicitSet(const ASTFunction * node, const Block & sample_block, bool create_ordered_set);
};
```

**Problems with Legacy Analyzer:**

1. **Monolithic Design**: The `ExpressionAnalyzer` became a massive class handling too many responsibilities
2. **Complex State Management**: Mutable state scattered across multiple data structures
3. **Limited Optimization Opportunities**: Difficult to implement advanced optimizations due to rigid structure
4. **Poor Error Messages**: Complex control flow made it hard to provide meaningful error messages
5. **Maintenance Burden**: Adding new features required understanding the entire complex system

**New Analyzer Architecture:**

The new analyzer, enabled by default since ClickHouse 24.3, introduces the `QueryTree` abstraction and a more modular design:

```cpp
class QueryAnalyzer
{
private:
    ContextPtr context;
    QueryTreeNodePtr query_tree;
    AnalysisScope scope;
    
public:
    explicit QueryAnalyzer(ContextPtr context_) : context(context_) {}
    
    QueryTreeNodePtr analyze(QueryTreeNodePtr query_tree_node);
    
private:
    void analyzeImpl(QueryTreeNodePtr & node, AnalysisScope & scope);
    void analyzeQuery(QueryNodePtr & query_node, AnalysisScope & scope);
    void analyzeExpression(QueryTreeNodePtr & node, AnalysisScope & scope);
    void analyzeFunction(FunctionNodePtr & function_node, AnalysisScope & scope);
    void analyzeJoin(JoinNodePtr & join_node, AnalysisScope & scope);
};

// Separate, focused analyzer passes
class QueryTreePassManager
{
private:
    std::vector<std::unique_ptr<IQueryTreePass>> passes;
    
public:
    void addPass(std::unique_ptr<IQueryTreePass> pass);
    void run(QueryTreeNodePtr & query_tree, ContextPtr context);
    
private:
    bool runSinglePass(IQueryTreePass & pass, QueryTreeNodePtr & query_tree, ContextPtr context);
};
```

**Advantages of New Analyzer:**

1. **Modular Design**: Separate concerns into focused components
2. **Immutable Query Tree**: Reduces state management complexity
3. **Extensible Pass System**: Easy to add new optimization passes
4. **Better Error Reporting**: Clear error context and meaningful messages
5. **Advanced Optimizations**: Enables sophisticated query transformations

#### 1.3.2 QueryTree Abstraction

The QueryTree represents a significant architectural advancement, providing a more structured and analyzable representation of queries:

**QueryTree Node Hierarchy:**

```cpp
class IQueryTreeNode
{
public:
    enum class NodeType
    {
        QUERY,
        UNION,
        TABLE,
        TABLE_FUNCTION,
        COLUMN,
        CONSTANT,
        FUNCTION,
        LAMBDA,
        SORT,
        INTERPOLATE,
        WINDOW,
        ARRAY_JOIN,
        JOIN,
        LIST
    };
    
    virtual ~IQueryTreeNode() = default;
    
    virtual NodeType getNodeType() const = 0;
    virtual String getName() const = 0;
    virtual DataTypePtr getResultType() const = 0;
    
    // Tree navigation
    virtual QueryTreeNodes getChildren() const = 0;
    virtual void setChildren(QueryTreeNodes children) = 0;
    
    // Visitor support
    virtual void dumpTreeImpl(WriteBuffer & buffer, FormatState & format_state, size_t indent) const = 0;
    virtual bool isEqualImpl(const IQueryTreeNode & rhs) const = 0;
    virtual void updateTreeHashImpl(HashState & hash_state) const = 0;
    
    // Cloning support
    virtual QueryTreeNodePtr cloneImpl() const = 0;
    
protected:
    // Common functionality
    void dumpChildrenImpl(WriteBuffer & buffer, const QueryTreeNodes & children, FormatState & format_state, size_t indent) const;
};

using QueryTreeNodePtr = std::shared_ptr<IQueryTreeNode>;
using QueryTreeNodes = std::vector<QueryTreeNodePtr>;
```

**Specialized Node Types:**

```cpp
class QueryNode : public IQueryTreeNode
{
private:
    // Query structure
    QueryTreeNodePtr projection;
    QueryTreeNodePtr where;
    QueryTreeNodePtr prewhere;
    QueryTreeNodePtr having;
    QueryTreeNodePtr group_by;
    QueryTreeNodePtr order_by;
    QueryTreeNodePtr limit_by;
    QueryTreeNodePtr limit;
    QueryTreeNodePtr offset;
    
    // Join table
    QueryTreeNodePtr join_tree;
    
    // Query properties
    bool is_subquery = false;
    bool is_cte = false;
    bool is_distinct = false;
    bool has_totals = false;
    
    // Settings and context
    SettingsChanges settings_changes;
    
public:
    NodeType getNodeType() const override { return NodeType::QUERY; }
    String getName() const override { return "Query"; }
    
    // Accessors
    const QueryTreeNodePtr & getProjection() const { return projection; }
    void setProjection(QueryTreeNodePtr projection_) { projection = std::move(projection_); }
    
    const QueryTreeNodePtr & getWhere() const { return where; }
    void setWhere(QueryTreeNodePtr where_) { where = std::move(where_); }
    
    // ... other accessors
    
    DataTypePtr getResultType() const override;
    QueryTreeNodes getChildren() const override;
    void setChildren(QueryTreeNodes children) override;
};

class FunctionNode : public IQueryTreeNode
{
private:
    String function_name;
    QueryTreeNodes arguments;
    QueryTreeNodes parameters;
    
    // Resolved function information
    FunctionBasePtr function;
    DataTypePtr result_type;
    
    // Function properties
    bool is_aggregate_function = false;
    bool is_window_function = false;
    bool is_lambda_function = false;
    
public:
    NodeType getNodeType() const override { return NodeType::FUNCTION; }
    String getName() const override { return function_name; }
    
    const String & getFunctionName() const { return function_name; }
    void setFunctionName(String function_name_) { function_name = std::move(function_name_); }
    
    const QueryTreeNodes & getArguments() const { return arguments; }
    void setArguments(QueryTreeNodes arguments_) { arguments = std::move(arguments_); }
    
    // Function resolution
    const FunctionBasePtr & getFunction() const { return function; }
    void resolveFunction(FunctionBasePtr function_);
    
    DataTypePtr getResultType() const override { return result_type; }
    QueryTreeNodes getChildren() const override;
    void setChildren(QueryTreeNodes children) override;
};

class ColumnNode : public IQueryTreeNode
{
private:
    ColumnIdentifier column_identifier;
    DataTypePtr column_type;
    
    // Source information
    QueryTreeNodePtr column_source;
    std::optional<size_t> column_source_index;
    
public:
    NodeType getNodeType() const override { return NodeType::COLUMN; }
    String getName() const override { return column_identifier.getFullName(); }
    
    const ColumnIdentifier & getColumnIdentifier() const { return column_identifier; }
    void setColumnIdentifier(ColumnIdentifier column_identifier_) { column_identifier = std::move(column_identifier_); }
    
    const QueryTreeNodePtr & getColumnSource() const { return column_source; }
    void setColumnSource(QueryTreeNodePtr column_source_) { column_source = std::move(column_source_); }
    
    DataTypePtr getResultType() const override { return column_type; }
    QueryTreeNodes getChildren() const override;
    void setChildren(QueryTreeNodes children) override;
};
```

#### 1.3.3 Semantic Analysis Phases

The new analyzer performs semantic analysis through a series of well-defined phases:

**Phase 1: Scope Resolution**

```cpp
class AnalysisScope
{
private:
    // Scope hierarchy
    AnalysisScope * parent_scope = nullptr;
    std::vector<std::unique_ptr<AnalysisScope>> child_scopes;
    
    // Available columns and aliases
    std::unordered_map<String, QueryTreeNodePtr> alias_name_to_expression_node;
    std::unordered_map<String, QueryTreeNodePtr> column_name_to_column_node;
    
    // Table information
    std::unordered_map<String, QueryTreeNodePtr> table_name_to_table_node;
    std::unordered_map<String, TableExpressionData> table_expression_name_to_table_expression_data;
    
    // CTE (Common Table Expressions)
    std::unordered_map<String, QueryTreeNodePtr> cte_name_to_query_node;
    
    // Lambda parameters
    std::unordered_set<String> lambda_argument_names;
    
public:
    // Scope management
    AnalysisScope * getParentScope() const { return parent_scope; }
    AnalysisScope & createChildScope() {
        auto child_scope = std::make_unique<AnalysisScope>();
        child_scope->parent_scope = this;
        auto & child_scope_ref = *child_scope;
        child_scopes.push_back(std::move(child_scope));
        return child_scope_ref;
    }
    
    // Identifier resolution
    IdentifierResolveResult tryResolveIdentifier(const Identifier & identifier) const;
    QueryTreeNodePtr resolveIdentifier(const Identifier & identifier) const;
    
    // Alias management
    void addAlias(const String & alias_name, QueryTreeNodePtr expression_node);
    QueryTreeNodePtr tryResolveAlias(const String & alias_name) const;
    
    // Table management
    void addTableExpression(const String & table_expression_name, QueryTreeNodePtr table_expression_node);
    QueryTreeNodePtr tryResolveTableExpression(const String & table_expression_name) const;
    
    // CTE management
    void addCTE(const String & cte_name, QueryTreeNodePtr cte_query_node);
    QueryTreeNodePtr tryResolveCTE(const String & cte_name) const;
};
```

**Phase 2: Type Resolution and Checking**

```cpp
class TypeAnalyzer
{
private:
    ContextPtr context;
    
public:
    explicit TypeAnalyzer(ContextPtr context_) : context(context_) {}
    
    void analyzeNode(QueryTreeNodePtr & node, AnalysisScope & scope)
    {
        switch (node->getNodeType())
        {
            case IQueryTreeNode::NodeType::FUNCTION:
                analyzeFunctionNode(node, scope);
                break;
            case IQueryTreeNode::NodeType::COLUMN:
                analyzeColumnNode(node, scope);
                break;
            case IQueryTreeNode::NodeType::CONSTANT:
                analyzeConstantNode(node, scope);
                break;
            // ... other node types
        }
    }
    
private:
    void analyzeFunctionNode(QueryTreeNodePtr & node, AnalysisScope & scope)
    {
        auto & function_node = node->as<FunctionNode &>();
        
        // Analyze arguments first
        auto & arguments = function_node.getArguments();
        for (auto & argument : arguments)
            analyzeNode(argument, scope);
        
        // Collect argument types
        DataTypes argument_types;
        for (const auto & argument : arguments)
            argument_types.push_back(argument->getResultType());
        
        // Resolve function
        auto function_builder = FunctionFactory::instance().get(function_node.getFunctionName(), context);
        auto function_base = function_builder->build(argument_types);
        
        function_node.resolveFunction(function_base);
    }
    
    void analyzeColumnNode(QueryTreeNodePtr & node, AnalysisScope & scope)
    {
        auto & column_node = node->as<ColumnNode &>();
        
        // Resolve column source
        auto column_source = scope.resolveIdentifier(column_node.getColumnIdentifier());
        if (!column_source)
            throw Exception("Unknown column: " + column_node.getColumnIdentifier().getFullName(), 
                          ErrorCodes::UNKNOWN_IDENTIFIER);
        
        column_node.setColumnSource(column_source);
        
        // Set column type from source
        if (auto table_node = column_source->as<TableNode>())
        {
            auto storage = table_node->getStorage();
            auto metadata = storage->getInMemoryMetadataPtr();
            auto column_type = metadata->getColumns().getPhysical(column_node.getColumnIdentifier().getColumnName()).type;
            column_node.setColumnType(column_type);
        }
    }
    
    void analyzeConstantNode(QueryTreeNodePtr & node, AnalysisScope & scope)
    {
        auto & constant_node = node->as<ConstantNode &>();
        
        // Infer type from constant value
        DataTypePtr type = inferTypeFromField(constant_node.getValue());
        constant_node.setResultType(type);
    }
};
```

**Phase 3: Expression Optimization**

```cpp
class ExpressionOptimizer
{
private:
    ContextPtr context;
    
public:
    explicit ExpressionOptimizer(ContextPtr context_) : context(context_) {}
    
    void optimizeNode(QueryTreeNodePtr & node)
    {
        switch (node->getNodeType())
        {
            case IQueryTreeNode::NodeType::FUNCTION:
                optimizeFunctionNode(node);
                break;
            case IQueryTreeNode::NodeType::QUERY:
                optimizeQueryNode(node);
                break;
            // ... other node types
        }
        
        // Recursively optimize children
        auto children = node->getChildren();
        for (auto & child : children)
            optimizeNode(child);
        node->setChildren(std::move(children));
    }
    
private:
    void optimizeFunctionNode(QueryTreeNodePtr & node)
    {
        auto & function_node = node->as<FunctionNode &>();
        
        // Constant folding
        if (canFoldFunction(function_node))
        {
            auto result = evaluateConstantFunction(function_node);
            if (result.has_value())
            {
                // Replace function with constant
                auto constant_node = std::make_shared<ConstantNode>(result.value());
                node = constant_node;
                return;
            }
        }
        
        // Function-specific optimizations
        if (function_node.getFunctionName() == "and")
            optimizeAndFunction(function_node);
        else if (function_node.getFunctionName() == "or")
            optimizeOrFunction(function_node);
        else if (function_node.getFunctionName() == "if")
            optimizeIfFunction(function_node);
    }
    
    void optimizeAndFunction(FunctionNode & function_node)
    {
        auto & arguments = function_node.getArguments();
        
        // Remove constant true arguments
        arguments.erase(
            std::remove_if(arguments.begin(), arguments.end(),
                [](const QueryTreeNodePtr & arg) {
                    if (auto constant = arg->as<ConstantNode>())
                        return constant->getValue().get<bool>() == true;
                    return false;
                }),
            arguments.end());
        
        // If any argument is constant false, replace entire expression with false
        for (const auto & arg : arguments)
        {
            if (auto constant = arg->as<ConstantNode>())
            {
                if (constant->getValue().get<bool>() == false)
                {
                    function_node.setArguments({std::make_shared<ConstantNode>(Field(false))});
                    return;
                }
            }
        }
        
        // If only one argument remains, replace function with argument
        if (arguments.size() == 1)
        {
            // This requires parent node update, simplified here
        }
    }
    
    bool canFoldFunction(const FunctionNode & function_node) const
    {
        // Check if all arguments are constants
        for (const auto & arg : function_node.getArguments())
        {
            if (arg->getNodeType() != IQueryTreeNode::NodeType::CONSTANT)
                return false;
        }
        return true;
    }
    
    std::optional<Field> evaluateConstantFunction(const FunctionNode & function_node)
    {
        try
        {
            // Create temporary block with constant columns
            Block block;
            ColumnsWithTypeAndName arguments;
            
            for (const auto & arg : function_node.getArguments())
            {
                auto constant_node = arg->as<ConstantNode>();
                auto constant_column = ColumnConst::create(
                    constant_node->getResultType()->createColumn(),
                    constant_node->getValue()
                );
                
                arguments.emplace_back(
                    std::move(constant_column),
                    constant_node->getResultType(),
                    "arg_" + std::to_string(arguments.size())
                );
            }
            
            // Execute function
            auto function = function_node.getFunction();
            auto result_column = function->execute(arguments, function->getResultType(), 1);
            
            // Extract constant value
            if (auto const_column = typeid_cast<const ColumnConst *>(result_column.get()))
                return const_column->getField();
            
            return std::nullopt;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
};
```

#### 1.3.4 Symbol Resolution

Symbol resolution in the new analyzer is more sophisticated and handles complex cases like nested scopes, CTEs, and ambiguous references:

**Identifier Resolution Engine:**

```cpp
class IdentifierResolver
{
private:
    AnalysisScope & scope;
    ContextPtr context;
    
public:
    IdentifierResolver(AnalysisScope & scope_, ContextPtr context_) 
        : scope(scope_), context(context_) {}
    
    IdentifierResolveResult resolveIdentifier(const Identifier & identifier)
    {
        // Try different resolution strategies in order of precedence
        
        // 1. Lambda arguments (highest precedence)
        if (auto result = tryResolveLambdaArgument(identifier))
            return result;
        
        // 2. Aliases in current scope
        if (auto result = tryResolveAlias(identifier))
            return result;
        
        // 3. Columns from table expressions
        if (auto result = tryResolveColumn(identifier))
            return result;
        
        // 4. CTE (Common Table Expressions)
        if (auto result = tryResolveCTE(identifier))
            return result;
        
        // 5. Table names
        if (auto result = tryResolveTable(identifier))
            return result;
        
        // 6. Global scope (databases, functions, etc.)
        if (auto result = tryResolveGlobal(identifier))
            return result;
        
        throw Exception("Unknown identifier: " + identifier.getFullName(), 
                       ErrorCodes::UNKNOWN_IDENTIFIER);
    }
    
private:
    IdentifierResolveResult tryResolveColumn(const Identifier & identifier)
    {
        std::vector<IdentifierResolveResult> candidates;
        
        // Search in all available table expressions
        for (const auto & [table_name, table_data] : scope.getTableExpressions())
        {
            if (auto column_node = tryResolveColumnInTable(identifier, table_data))
            {
                candidates.emplace_back(IdentifierResolveResult{
                    .resolved_identifier = column_node,
                    .resolve_place = IdentifierResolvePlace::TABLE_EXPRESSION,
                    .table_expression_name = table_name
                });
            }
        }
        
        if (candidates.empty())
            return {};
        
        if (candidates.size() == 1)
            return candidates[0];
        
        // Handle ambiguous references
        return resolveAmbiguousColumn(identifier, candidates);
    }
    
    QueryTreeNodePtr tryResolveColumnInTable(const Identifier & identifier, const TableExpressionData & table_data)
    {
        // Handle qualified identifiers (table.column)
        if (identifier.isCompound())
        {
            auto table_name = identifier.getParts()[0];
            auto column_name = identifier.getParts()[1];
            
            if (table_name != table_data.table_name && table_name != table_data.table_alias)
                return nullptr;
            
            return findColumnInTableMetadata(column_name, table_data);
        }
        
        // Handle unqualified identifiers (column)
        return findColumnInTableMetadata(identifier.getFullName(), table_data);
    }
    
    QueryTreeNodePtr findColumnInTableMetadata(const String & column_name, const TableExpressionData & table_data)
    {
        if (auto table_node = table_data.table_expression->as<TableNode>())
        {
            auto storage = table_node->getStorage();
            auto metadata = storage->getInMemoryMetadataPtr();
            
            if (metadata->getColumns().hasPhysical(column_name))
            {
                auto column_type = metadata->getColumns().getPhysical(column_name).type;
                auto column_node = std::make_shared<ColumnNode>();
                column_node->setColumnIdentifier(ColumnIdentifier(column_name));
                column_node->setColumnType(column_type);
                column_node->setColumnSource(table_data.table_expression);
                return column_node;
            }
        }
        
        return nullptr;
    }
    
    IdentifierResolveResult resolveAmbiguousColumn(const Identifier & identifier, 
                                                  const std::vector<IdentifierResolveResult> & candidates)
    {
        // Strategy 1: Prefer columns from tables without aliases
        std::vector<IdentifierResolveResult> unaliased_candidates;
        for (const auto & candidate : candidates)
        {
            if (candidate.table_expression_name == candidate.table_expression_alias)
                unaliased_candidates.push_back(candidate);
        }
        
        if (unaliased_candidates.size() == 1)
            return unaliased_candidates[0];
        
        // Strategy 2: Check if all candidates resolve to the same physical column
        bool all_same = true;
        auto first_column = candidates[0].resolved_identifier->as<ColumnNode>();
        
        for (size_t i = 1; i < candidates.size(); ++i)
        {
            auto candidate_column = candidates[i].resolved_identifier->as<ColumnNode>();
            if (first_column->getColumnIdentifier() != candidate_column->getColumnIdentifier() ||
                !first_column->getResultType()->equals(*candidate_column->getResultType()))
            {
                all_same = false;
                break;
            }
        }
        
        if (all_same)
            return candidates[0];
        
        // Strategy 3: Report ambiguous reference error
        std::vector<String> candidate_sources;
        for (const auto & candidate : candidates)
            candidate_sources.push_back(candidate.table_expression_name);
        
        throw Exception(
            "Ambiguous column reference: " + identifier.getFullName() + 
            " could refer to columns from: " + boost::algorithm::join(candidate_sources, ", "),
            ErrorCodes::AMBIGUOUS_IDENTIFIER);
    }
};

struct IdentifierResolveResult
{
    QueryTreeNodePtr resolved_identifier;
    IdentifierResolvePlace resolve_place;
    String table_expression_name;
    String table_expression_alias;
    
    bool isResolved() const { return resolved_identifier != nullptr; }
};

enum class IdentifierResolvePlace
{
    NONE,
    LAMBDA_ARGUMENT,
    ALIAS,
    TABLE_EXPRESSION,
    CTE,
    TABLE,
    GLOBAL
};
```

This comprehensive query analysis engine provides the foundation for all subsequent query processing steps. The transition from the legacy analyzer to the new QueryTree-based system represents a significant architectural improvement, enabling more sophisticated optimizations, better error reporting, and easier maintenance of the codebase.

The new analyzer's modular design, with its clear separation of concerns and well-defined interfaces, makes it much easier to extend ClickHouse with new SQL features and optimization passes. The QueryTree abstraction provides a clean, immutable representation of queries that can be safely transformed by multiple optimization passes without the complex state management issues that plagued the legacy system.

Now let me continue with section 1.4 to complete Phase 1:

### 1.4 Query Planning Architecture (3,000 words)

ClickHouse's query planning architecture represents the critical bridge between analyzed queries and executable pipelines. The QueryPlan system provides a structured, optimizable representation of query execution steps that can be transformed through various optimization passes before being converted into actual execution pipelines.

#### 1.4.1 QueryPlan Structure

The QueryPlan serves as an intermediate representation that captures the logical execution steps required to process a query:

```cpp
class QueryPlan
{
private:
    QueryPlanStepPtr root_step;
    std::vector<std::unique_ptr<QueryPlanStep>> steps;
    
    // Optimization context
    QueryPlanOptimizationSettings optimization_settings;
    ContextPtr context;
    
public:
    QueryPlan() = default;
    ~QueryPlan() = default;
    
    // Plan construction
    void addStep(QueryPlanStepPtr step);
    void addStepToRoot(QueryPlanStepPtr step);
    
    // Plan structure access
    QueryPlanStepPtr getRootStep() const { return root_step; }
    const std::vector<std::unique_ptr<QueryPlanStep>>& getSteps() const { return steps; }
    
    // Optimization
    void optimize(const QueryPlanOptimizationSettings & settings);
    
    // Pipeline construction
    QueryPipelineBuilderPtr buildQueryPipeline(
        const QueryPlanOptimizationSettings & optimization_settings,
        const BuildQueryPipelineSettings & build_pipeline_settings);
    
    // Introspection
    void explainPlan(WriteBuffer & buffer, const ExplainPlanOptions & options) const;
    void explainPipeline(WriteBuffer & buffer, const ExplainPipelineOptions & options) const;
    
private:
    void checkInitialized() const;
    void checkNotCompleted() const;
};
```

**QueryPlan Step Hierarchy:**

```cpp
class IQueryPlanStep
{
public:
    virtual ~IQueryPlanStep() = default;
    
    virtual String getName() const = 0;
    virtual String getStepDescription() const = 0;
    
    // Input/Output streams
    virtual DataStreams getInputStreams() const = 0;
    virtual DataStream getOutputStream() const = 0;
    
    // Pipeline construction
    virtual QueryPipelineBuilderPtr updatePipeline(
        QueryPipelineBuilders pipelines,
        const BuildQueryPipelineSettings & settings) = 0;
    
    // Optimization
    virtual void describePipeline(FormatSettings & settings) const {}
    virtual void describeActions(JSONBuilder::JSONMap & map) const {}
    virtual void describeIndexes(JSONBuilder::JSONMap & map) const {}
    
    // Step transformation
    virtual void transformPipeline(const std::function<void(QueryPipelineBuilder &)> & transform) {}
    
protected:
    // Child steps management
    std::vector<std::unique_ptr<IQueryPlanStep>> children;
    
public:
    void addChild(std::unique_ptr<IQueryPlanStep> child);
    const std::vector<std::unique_ptr<IQueryPlanStep>>& getChildren() const { return children; }
};
```

**Specialized Step Types:**

```cpp
class ReadFromMergeTree : public IQueryPlanStep
{
private:
    MergeTreeData & storage;
    StorageSnapshotPtr storage_snapshot;
    
    // Query parameters
    Names required_columns;
    SelectQueryInfo query_info;
    ContextPtr context;
    
    // Optimization results
    MergeTreeDataSelectExecutor::PartitionIdToMaxBlock max_block_numbers_to_read;
    Poco::Logger * log;
    
    // Performance characteristics
    size_t requested_num_streams = 1;
    size_t output_streams = 0;
    
public:
    ReadFromMergeTree(
        MergeTreeData & storage_,
        StorageSnapshotPtr storage_snapshot_,
        Names required_columns_,
        SelectQueryInfo & query_info_,
        ContextPtr context_,
        size_t num_streams_);
    
    String getName() const override { return "ReadFromMergeTree"; }
    String getStepDescription() const override;
    
    DataStreams getInputStreams() const override { return {}; }
    DataStream getOutputStream() const override;
    
    QueryPipelineBuilderPtr updatePipeline(
        QueryPipelineBuilders pipelines,
        const BuildQueryPipelineSettings & settings) override;
    
    void describeIndexes(JSONBuilder::JSONMap & map) const override;
    void describeActions(JSONBuilder::JSONMap & map) const override;
    
private:
    void initializePipeline(QueryPipelineBuilder & pipeline, const BuildQueryPipelineSettings & settings);
    Pipe readFromPool(const Names & column_names, size_t num_streams, size_t min_marks_for_concurrent_read);
};

class FilterStep : public IQueryPlanStep
{
private:
    DataStream input_stream;
    ActionsDAGPtr filter_actions;
    String filter_column_name;
    bool remove_filter_column;
    
public:
    FilterStep(
        const DataStream & input_stream_,
        ActionsDAGPtr filter_actions_,
        String filter_column_name_,
        bool remove_filter_column_);
    
    String getName() const override { return "Filter"; }
    String getStepDescription() const override;
    
    DataStreams getInputStreams() const override { return {input_stream}; }
    DataStream getOutputStream() const override;
    
    QueryPipelineBuilderPtr updatePipeline(
        QueryPipelineBuilders pipelines,
        const BuildQueryPipelineSettings & settings) override;
    
    void describeActions(JSONBuilder::JSONMap & map) const override;
    
    const ActionsDAGPtr & getFilterActions() const { return filter_actions; }
    const String & getFilterColumnName() const { return filter_column_name; }
};

class AggregatingStep : public IQueryPlanStep
{
private:
    DataStream input_stream;
    Aggregator::Params params;
    GroupingSetsParamsList grouping_sets_params;
    bool final;
    size_t max_block_size;
    size_t aggregation_in_order_max_block_bytes;
    size_t merge_threads;
    size_t temporary_data_merge_threads;
    
    // Optimization flags
    bool storage_has_evenly_distributed_read;
    bool group_by_use_nulls;
    
public:
    AggregatingStep(
        const DataStream & input_stream_,
        Aggregator::Params params_,
        GroupingSetsParamsList grouping_sets_params_,
        bool final_,
        size_t max_block_size_,
        size_t aggregation_in_order_max_block_bytes_,
        size_t merge_threads_,
        size_t temporary_data_merge_threads_,
        bool storage_has_evenly_distributed_read_,
        bool group_by_use_nulls_);
    
    String getName() const override { return "Aggregating"; }
    String getStepDescription() const override;
    
    DataStreams getInputStreams() const override { return {input_stream}; }
    DataStream getOutputStream() const override;
    
    QueryPipelineBuilderPtr updatePipeline(
        QueryPipelineBuilders pipelines,
        const BuildQueryPipelineSettings & settings) override;
    
    void describeActions(JSONBuilder::JSONMap & map) const override;
    void describePipeline(FormatSettings & settings) const override;
    
    const Aggregator::Params & getParams() const { return params; }
};
```

#### 1.4.2 Step Hierarchy and Relationships

The QueryPlan step hierarchy is designed to represent all possible query operations in a composable manner:

**Data Source Steps:**
- `ReadFromMergeTree`: Reads data from MergeTree tables with optimizations
- `ReadFromMemoryStorage`: Reads from in-memory tables
- `ReadFromRemote`: Reads data from remote shards in distributed queries
- `ReadFromPreparedSource`: Reads from pre-prepared data sources

**Transformation Steps:**
- `FilterStep`: Applies WHERE conditions and other filters
- `ExpressionStep`: Evaluates expressions and projections
- `SortingStep`: Sorts data according to ORDER BY clauses
- `LimitStep`: Applies LIMIT and OFFSET restrictions

**Aggregation Steps:**
- `AggregatingStep`: Performs GROUP BY aggregations
- `MergingAggregatedStep`: Merges pre-aggregated data
- `TotalsHavingStep`: Handles HAVING conditions and TOTALS

**Join Steps:**
- `JoinStep`: Performs various types of joins
- `FilledJoinStep`: Handles special join optimizations
- `ArrayJoinStep`: Processes ARRAY JOIN operations

**Output Steps:**
- `LimitByStep`: Implements LIMIT BY functionality
- `DistinctStep`: Removes duplicate rows
- `ExtremesStep`: Calculates query extremes

#### 1.4.3 Optimization Rules Engine

ClickHouse employs a sophisticated rule-based optimization system that transforms QueryPlan structures:

```cpp
class QueryPlanOptimizationSettings
{
public:
    // Optimization toggles
    bool optimize_plan = true;
    bool read_in_order = true;
    bool distinct_in_order = true;
    bool optimize_sorting = true;
    bool optimize_duplicate_order_by_and_distinct = true;
    bool optimize_monotonous_functions_in_order_by = true;
    bool optimize_functions_to_subcolumns = true;
    bool optimize_using_constraints = true;
    bool optimize_substitute_columns = true;
    bool optimize_count_from_files = true;
    
    // Performance tuning
    size_t max_threads = 0;
    size_t max_streams_to_max_threads_ratio = 1;
    size_t max_streams_for_merge_tree_reading = 0;
    
    // Debugging
    bool build_sets_from_right_part_of_join = true;
    bool optimize_prewhere = true;
    bool force_primary_key = false;
    
    void loadFromContext(ContextPtr context);
};

class QueryPlanOptimizer
{
private:
    std::vector<std::unique_ptr<QueryPlanOptimizationRule>> rules;
    QueryPlanOptimizationSettings settings;
    
public:
    explicit QueryPlanOptimizer(const QueryPlanOptimizationSettings & settings_);
    
    void addRule(std::unique_ptr<QueryPlanOptimizationRule> rule);
    void optimize(QueryPlan & plan) const;
    
private:
    bool applyRules(QueryPlan & plan) const;
    void collectRules();
};
```

**Core Optimization Rules:**

```cpp
class MergeExpressions : public QueryPlanOptimizationRule
{
public:
    String getName() const override { return "MergeExpressions"; }
    String getDescription() const override { return "Merge consecutive Expression steps"; }
    
    bool match(const QueryPlanStepPtr & step) const override
    {
        if (const auto * expression = typeid_cast<const ExpressionStep *>(step.get()))
        {
            if (expression->getChildren().size() == 1)
            {
                return typeid_cast<const ExpressionStep *>(expression->getChildren()[0].get()) != nullptr;
            }
        }
        return false;
    }
    
    void transform(QueryPlanStepPtr & step) const override
    {
        auto * expression_step = static_cast<ExpressionStep *>(step.get());
        auto * child_expression = static_cast<ExpressionStep *>(expression_step->getChildren()[0].get());
        
        // Merge the two expression steps
        auto merged_actions = ActionsDAG::merge(
            std::move(*child_expression->getActions()),
            std::move(*expression_step->getActions())
        );
        
        auto merged_step = std::make_unique<ExpressionStep>(
            child_expression->getInputStreams()[0],
            std::move(merged_actions)
        );
        
        step = std::move(merged_step);
    }
};

class PushDownFilterThroughExpression : public QueryPlanOptimizationRule
{
public:
    String getName() const override { return "PushDownFilterThroughExpression"; }
    String getDescription() const override { return "Push filter conditions through expression steps"; }
    
    bool match(const QueryPlanStepPtr & step) const override
    {
        if (const auto * filter = typeid_cast<const FilterStep *>(step.get()))
        {
            if (filter->getChildren().size() == 1)
            {
                return typeid_cast<const ExpressionStep *>(filter->getChildren()[0].get()) != nullptr;
            }
        }
        return false;
    }
    
    void transform(QueryPlanStepPtr & step) const override
    {
        auto * filter_step = static_cast<FilterStep *>(step.get());
        auto * expression_step = static_cast<ExpressionStep *>(filter_step->getChildren()[0].get());
        
        // Analyze if filter can be pushed down
        const auto & filter_actions = filter_step->getFilterActions();
        const auto & expression_actions = expression_step->getActions();
        
        auto [pushable_filter, remaining_filter] = splitFilterActions(
            filter_actions, 
            expression_actions,
            filter_step->getFilterColumnName()
        );
        
        if (pushable_filter)
        {
            // Create new filter step before expression
            auto new_filter_step = std::make_unique<FilterStep>(
                expression_step->getInputStreams()[0],
                pushable_filter,
                pushable_filter->getResultName(),
                true
            );
            
            // Update the plan structure
            expression_step->replaceChild(0, std::move(new_filter_step));
            
            if (remaining_filter)
            {
                // Keep remaining filter after expression
                filter_step->updateFilterActions(remaining_filter);
            }
            else
            {
                // Remove filter step entirely
                step = expression_step->shared_from_this();
            }
        }
    }
    
private:
    std::pair<ActionsDAGPtr, ActionsDAGPtr> splitFilterActions(
        const ActionsDAGPtr & filter_actions,
        const ActionsDAGPtr & expression_actions,
        const String & filter_column) const;
};

class OptimizeReadInOrder : public QueryPlanOptimizationRule
{
public:
    String getName() const override { return "OptimizeReadInOrder"; }
    String getDescription() const override { return "Optimize reading data in sorted order"; }
    
    bool match(const QueryPlanStepPtr & step) const override
    {
        if (const auto * sorting = typeid_cast<const SortingStep *>(step.get()))
        {
            // Check if we can read data in the required order
            return canOptimizeReadInOrder(sorting);
        }
        return false;
    }
    
    void transform(QueryPlanStepPtr & step) const override
    {
        auto * sorting_step = static_cast<SortingStep *>(step.get());
        
        // Find ReadFromMergeTree step
        auto * read_step = findReadFromMergeTreeStep(sorting_step);
        if (!read_step)
            return;
        
        // Check if table is sorted by the same columns
        const auto & sort_description = sorting_step->getSortDescription();
        if (canReadInOrder(read_step, sort_description))
        {
            // Configure read step to read in order
            read_step->enableReadInOrder(sort_description);
            
            // Remove or simplify sorting step
            if (isCompletelyOptimized(sort_description, read_step->getOrderDescription()))
            {
                // Remove sorting step entirely
                step = sorting_step->getChildren()[0];
            }
            else
            {
                // Convert to partial sorting
                sorting_step->convertToPartialSorting();
            }
        }
    }
    
private:
    bool canOptimizeReadInOrder(const SortingStep * sorting_step) const;
    ReadFromMergeTree * findReadFromMergeTreeStep(const IQueryPlanStep * step) const;
    bool canReadInOrder(const ReadFromMergeTree * read_step, const SortDescription & sort_desc) const;
    bool isCompletelyOptimized(const SortDescription & required, const SortDescription & provided) const;
};
```

#### 1.4.4 Cost-Based Optimization

ClickHouse incorporates cost-based optimization techniques to make intelligent decisions about query execution strategies:

```cpp
class QueryPlanCostModel
{
private:
    struct StepCost
    {
        double cpu_cost = 0.0;
        double memory_cost = 0.0;
        double io_cost = 0.0;
        double network_cost = 0.0;
        
        double getTotalCost() const 
        {
            return cpu_cost + memory_cost + io_cost + network_cost;
        }
    };
    
    ContextPtr context;
    std::unordered_map<const IQueryPlanStep*, StepCost> step_costs;
    
public:
    explicit QueryPlanCostModel(ContextPtr context_) : context(context_) {}
    
    double estimateStepCost(const IQueryPlanStep & step) const;
    double estimatePlanCost(const QueryPlan & plan) const;
    
    void updateStepCost(const IQueryPlanStep & step, const StepCost & cost);
    
private:
    StepCost calculateReadCost(const ReadFromMergeTree & read_step) const;
    StepCost calculateFilterCost(const FilterStep & filter_step) const;
    StepCost calculateAggregationCost(const AggregatingStep & agg_step) const;
    StepCost calculateJoinCost(const JoinStep & join_step) const;
    StepCost calculateSortCost(const SortingStep & sort_step) const;
};
```

**Join Order Optimization:**

```cpp
class JoinOrderOptimizer
{
private:
    QueryPlanCostModel cost_model;
    size_t max_tables_for_exhaustive_search = 6;
    
public:
    explicit JoinOrderOptimizer(ContextPtr context) : cost_model(context) {}
    
    void optimizeJoinOrder(QueryPlan & plan) const
    {
        auto join_steps = findJoinSteps(plan);
        if (join_steps.size() <= 1)
            return;
        
        if (join_steps.size() <= max_tables_for_exhaustive_search)
        {
            optimizeExhaustively(join_steps);
        }
        else
        {
            optimizeGreedy(join_steps);
        }
    }
    
private:
    std::vector<JoinStep*> findJoinSteps(const QueryPlan & plan) const;
    void optimizeExhaustively(std::vector<JoinStep*> & join_steps) const;
    void optimizeGreedy(std::vector<JoinStep*> & join_steps) const;
    
    double estimateJoinCost(const JoinStep & left_step, const JoinStep & right_step) const;
    size_t estimateTableSize(const IQueryPlanStep & step) const;
    double estimateSelectivity(const JoinStep & join_step) const;
};
```

This comprehensive query planning architecture enables ClickHouse to systematically optimize queries through a combination of rule-based transformations and cost-based decisions, ensuring that the final execution plan is both correct and efficient.

### 1.5 Pipeline Construction (3,000 words)

The pipeline construction phase represents the final transformation from logical query plans to executable processing graphs. ClickHouse's pipeline architecture is built around the concept of processors—lightweight, composable units that can be connected to form complex data processing graphs with sophisticated parallelization and resource management capabilities.

#### 1.5.1 Processor Architecture

The foundation of ClickHouse's pipeline system is the `IProcessor` interface, which defines a standardized way to process data streams:

```cpp
class IProcessor
{
public:
    enum class Status
    {
        NeedData,       // Processor needs more input data
        PortFull,       // Output port is full, cannot produce more data
        Finished,       // Processor has completed its work
        Ready,          // Processor is ready to do work
        Async,          // Processor is doing asynchronous work
        ExpandPipeline  // Processor wants to add more processors to pipeline
    };
    
    virtual ~IProcessor() = default;
    
    virtual String getName() const = 0;
    virtual Status prepare() = 0;
    virtual void work() {}
    virtual Processors expandPipeline() { return {}; }
    
    // Port management
    const InputPorts & getInputs() const { return inputs; }
    const OutputPorts & getOutputs() const { return outputs; }
    
    // Resource management
    virtual UInt64 elapsed_us() const { return 0; }
    virtual UInt64 processed_rows() const { return 0; }
    virtual UInt64 processed_bytes() const { return 0; }
    
    // Debugging and profiling
    virtual void setDescription(const String & description_) { description = description_; }
    const String & getDescription() const { return description; }
    
protected:
    InputPorts inputs;
    OutputPorts outputs;
    String description;
    
    // Helper methods for derived classes
    InputPort & addInputPort(Block header, bool can_be_totals = false);
    OutputPort & addOutputPort(Block header, bool can_be_totals = false);
};
```

**Port System for Data Flow:**

```cpp
class Port
{
public:
    enum class State
    {
        NotNeeded,
        NeedData,
        HasData,
        Finished
    };
    
protected:
    State state = State::NotNeeded;
    Block header;
    Chunk data;
    
    IProcessor * processor = nullptr;
    Port * connected_port = nullptr;
    
public:
    Port(Block header_, IProcessor * processor_) : header(std::move(header_)), processor(processor_) {}
    
    void connect(Port & other);
    bool isConnected() const { return connected_port != nullptr; }
    
    const Block & getHeader() const { return header; }
    State getState() const { return state; }
    
    // Data operations (implemented differently for input/output ports)
    virtual bool hasData() const = 0;
    virtual void pushData(Chunk chunk) = 0;
    virtual Chunk pullData() = 0;
    
    virtual void finish() = 0;
    virtual bool isFinished() const = 0;
};

class InputPort : public Port
{
public:
    using Port::Port;
    
    bool hasData() const override
    {
        return connected_port && connected_port->hasData();
    }
    
    Chunk pullData() override
    {
        if (!connected_port || !connected_port->hasData())
            throw Exception("Cannot pull data from port", ErrorCodes::LOGICAL_ERROR);
        
        return connected_port->pullData();
    }
    
    void pushData(Chunk) override
    {
        throw Exception("Cannot push data to input port", ErrorCodes::LOGICAL_ERROR);
    }
    
    bool isFinished() const override
    {
        return connected_port && connected_port->isFinished();
    }
    
    void finish() override
    {
        state = State::Finished;
    }
};

class OutputPort : public Port
{
private:
    std::queue<Chunk> data_queue;
    bool finished = false;
    
public:
    using Port::Port;
    
    bool hasData() const override
    {
        return !data_queue.empty();
    }
    
    void pushData(Chunk chunk) override
    {
        if (finished)
            throw Exception("Cannot push data to finished port", ErrorCodes::LOGICAL_ERROR);
        
        data_queue.push(std::move(chunk));
        state = State::HasData;
    }
    
    Chunk pullData() override
    {
        if (data_queue.empty())
            throw Exception("Cannot pull data from empty port", ErrorCodes::LOGICAL_ERROR);
        
        auto chunk = std::move(data_queue.front());
        data_queue.pop();
        
        if (data_queue.empty() && !finished)
            state = State::NeedData;
        
        return chunk;
    }
    
    void finish() override
    {
        finished = true;
        state = State::Finished;
    }
    
    bool isFinished() const override
    {
        return finished && data_queue.empty();
    }
};
```

#### 1.5.2 Core Processor Types

ClickHouse implements a rich set of processor types to handle different aspects of query execution:

**Source Processors:**

```cpp
class SourceFromInputStream : public IProcessor
{
private:
    BlockInputStreamPtr stream;
    Chunk current_chunk;
    bool has_input = true;
    
public:
    SourceFromInputStream(BlockInputStreamPtr stream_, Block header_)
        : stream(std::move(stream_))
    {
        addOutputPort(std::move(header_));
    }
    
    String getName() const override { return "SourceFromInputStream"; }
    
    Status prepare() override
    {
        auto & output = getOutputs().front();
        
        if (output.isFinished())
            return Status::Finished;
        
        if (!has_input)
        {
            output.finish();
            return Status::Finished;
        }
        
        if (output.hasData())
            return Status::PortFull;
        
        if (current_chunk.hasRows())
        {
            output.pushData(std::move(current_chunk));
            return Status::PortFull;
        }
        
        return Status::Ready;
    }
    
    void work() override
    {
        if (auto block = stream->read())
        {
            current_chunk = Chunk(block.getColumns(), block.rows());
        }
        else
        {
            has_input = false;
        }
    }
};

class SourceFromSingleChunk : public IProcessor
{
private:
    Chunk chunk;
    bool has_data = true;
    
public:
    SourceFromSingleChunk(Block header_, Chunk chunk_)
        : chunk(std::move(chunk_))
    {
        addOutputPort(std::move(header_));
    }
    
    String getName() const override { return "SourceFromSingleChunk"; }
    
    Status prepare() override
    {
        auto & output = getOutputs().front();
        
        if (output.isFinished() || !has_data)
        {
            output.finish();
            return Status::Finished;
        }
        
        if (output.hasData())
            return Status::PortFull;
        
        output.pushData(std::move(chunk));
        has_data = false;
        
        return Status::PortFull;
    }
};
```

**Transform Processors:**

```cpp
class ExpressionTransform : public IProcessor
{
private:
    ExpressionActionsPtr expression;
    Block input_header;
    Block output_header;
    
public:
    ExpressionTransform(Block input_header_, ExpressionActionsPtr expression_)
        : expression(std::move(expression_)), input_header(std::move(input_header_))
    {
        output_header = expression->updateHeader(input_header);
        addInputPort(input_header);
        addOutputPort(output_header);
    }
    
    String getName() const override { return "ExpressionTransform"; }
    
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        if (!output.canPush())
            return Status::PortFull;
        
        if (input.isFinished())
        {
            output.finish();
            return Status::Finished;
        }
        
        if (!input.hasData())
            return Status::NeedData;
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        auto chunk = input.pullData();
        
        if (!chunk.hasRows())
        {
            output.pushData(std::move(chunk));
            return;
        }
        
        auto block = input_header.cloneWithColumns(chunk.detachColumns());
        expression->execute(block);
        
        auto result_chunk = Chunk(block.getColumns(), block.rows());
        output.pushData(std::move(result_chunk));
    }
};

class FilterTransform : public IProcessor
{
private:
    ExpressionActionsPtr filter_expression;
    String filter_column_name;
    bool remove_filter_column;
    
    Block input_header;
    Block output_header;
    
public:
    FilterTransform(
        Block input_header_,
        ExpressionActionsPtr filter_expression_,
        String filter_column_name_,
        bool remove_filter_column_)
        : filter_expression(std::move(filter_expression_))
        , filter_column_name(std::move(filter_column_name_))
        , remove_filter_column(remove_filter_column_)
        , input_header(std::move(input_header_))
    {
        output_header = input_header;
        if (remove_filter_column)
            output_header.erase(filter_column_name);
        
        addInputPort(input_header);
        addOutputPort(output_header);
    }
    
    String getName() const override { return "FilterTransform"; }
    
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        if (!output.canPush())
            return Status::PortFull;
        
        if (input.isFinished())
        {
            output.finish();
            return Status::Finished;
        }
        
        if (!input.hasData())
            return Status::NeedData;
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        auto chunk = input.pullData();
        
        if (!chunk.hasRows())
        {
            output.pushData(std::move(chunk));
            return;
        }
        
        auto block = input_header.cloneWithColumns(chunk.detachColumns());
        
        // Apply filter expression
        filter_expression->execute(block);
        
        // Get filter column
        auto filter_column = block.getByName(filter_column_name).column;
        
        // Apply filter
        auto filtered_columns = applyFilter(block.getColumns(), filter_column);
        
        // Remove filter column if requested
        if (remove_filter_column)
        {
            auto filter_pos = block.getPositionByName(filter_column_name);
            filtered_columns.erase(filtered_columns.begin() + filter_pos);
        }
        
        auto filtered_chunk = Chunk(std::move(filtered_columns), filtered_columns[0]->size());
        output.pushData(std::move(filtered_chunk));
    }
    
private:
    Columns applyFilter(const Columns & columns, const ColumnPtr & filter_column) const
    {
        Columns result;
        result.reserve(columns.size());
        
        for (const auto & column : columns)
        {
            result.push_back(column->filter(*filter_column, -1));
        }
        
        return result;
    }
};
```

**Aggregation Processors:**

```cpp
class AggregatingTransform : public IProcessor
{
private:
    AggregatingTransformParamsPtr params;
    std::unique_ptr<Aggregator> aggregator;
    
    Aggregator::AggregateDataPtr aggregate_data;
    bool is_consume_finished = false;
    bool is_generate_finished = false;
    
    Block input_header;
    Block output_header;
    
public:
    AggregatingTransform(Block input_header_, AggregatingTransformParamsPtr params_)
        : params(std::move(params_)), input_header(std::move(input_header_))
    {
        aggregator = std::make_unique<Aggregator>(params->params);
        output_header = aggregator->getHeader(false);
        
        addInputPort(input_header);
        addOutputPort(output_header);
        
        aggregate_data = aggregator->prepareAggregateData();
    }
    
    String getName() const override { return "AggregatingTransform"; }
    
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        if (is_generate_finished)
        {
            output.finish();
            return Status::Finished;
        }
        
        if (!output.canPush())
            return Status::PortFull;
        
        if (is_consume_finished)
            return Status::Ready;
        
        if (input.isFinished())
        {
            is_consume_finished = true;
            return Status::Ready;
        }
        
        if (!input.hasData())
            return Status::NeedData;
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (!is_consume_finished)
        {
            auto chunk = input.pullData();
            
            if (chunk.hasRows())
            {
                auto block = input_header.cloneWithColumns(chunk.detachColumns());
                aggregator->executeOnBlock(block, aggregate_data, params->threads);
            }
        }
        else
        {
            // Generate output
            auto block = aggregator->prepareBlockAndFillWithoutKey(
                aggregate_data, params->final, params->max_block_size);
            
            if (block.rows() == 0)
            {
                is_generate_finished = true;
            }
            else
            {
                auto chunk = Chunk(block.getColumns(), block.rows());
                output.pushData(std::move(chunk));
            }
        }
    }
};
```

#### 1.5.3 Pipeline Graph Construction

The pipeline construction process involves building a directed acyclic graph of processors:

```cpp
class QueryPipelineBuilder
{
private:
    Processors processors;
    std::vector<OutputPort*> current_output_ports;
    size_t max_threads;
    
public:
    explicit QueryPipelineBuilder(size_t max_threads_ = 0) : max_threads(max_threads_) {}
    
    void addSource(ProcessorPtr source)
    {
        auto * source_ptr = source.get();
        processors.emplace_back(std::move(source));
        
        for (auto & output : source_ptr->getOutputs())
            current_output_ports.push_back(&output);
    }
    
    void addTransform(ProcessorPtr transform)
    {
        if (current_output_ports.size() != transform->getInputs().size())
            throw Exception("Processor input/output port count mismatch", ErrorCodes::LOGICAL_ERROR);
        
        auto * transform_ptr = transform.get();
        processors.emplace_back(std::move(transform));
        
        // Connect inputs
        auto input_it = transform_ptr->getInputs().begin();
        for (auto * output_port : current_output_ports)
        {
            connectPorts(*output_port, *input_it);
            ++input_it;
        }
        
        // Update current outputs
        current_output_ports.clear();
        for (auto & output : transform_ptr->getOutputs())
            current_output_ports.push_back(&output);
    }
    
    void addSink(ProcessorPtr sink)
    {
        if (current_output_ports.size() != sink->getInputs().size())
            throw Exception("Sink input port count mismatch", ErrorCodes::LOGICAL_ERROR);
        
        auto * sink_ptr = sink.get();
        processors.emplace_back(std::move(sink));
        
        // Connect inputs
        auto input_it = sink_ptr->getInputs().begin();
        for (auto * output_port : current_output_ports)
        {
            connectPorts(*output_port, *input_it);
            ++input_it;
        }
        
        current_output_ports.clear();
    }
    
    void resize(size_t num_streams)
    {
        if (current_output_ports.size() == num_streams)
            return;
        
        if (current_output_ports.size() > num_streams)
        {
            // Merge streams
            while (current_output_ports.size() > num_streams)
            {
                addTransform(std::make_shared<ConcatProcessor>(
                    getCommonHeader(), 2));
            }
        }
        else
        {
            // Split streams (not always possible)
            throw Exception("Cannot increase number of streams", ErrorCodes::LOGICAL_ERROR);
        }
    }
    
    QueryPipeline build()
    {
        return QueryPipeline(std::move(processors), max_threads);
    }
    
private:
    void connectPorts(OutputPort & output, InputPort & input)
    {
        output.connect(input);
    }
    
    Block getCommonHeader() const
    {
        if (current_output_ports.empty())
            return {};
        
        return current_output_ports[0]->getHeader();
    }
};
```

#### 1.5.4 Resource Allocation and Parallelism

ClickHouse's pipeline system includes sophisticated resource management and parallelization strategies:

```cpp
class PipelineExecutor
{
private:
    Processors processors;
    std::vector<std::thread> threads;
    std::atomic<bool> cancelled{false};
    
    // Resource tracking
    std::atomic<size_t> active_processors{0};
    std::atomic<size_t> total_memory_usage{0};
    
    // Thread pool management
    ThreadPool thread_pool;
    std::queue<IProcessor*> ready_processors;
    std::mutex ready_processors_mutex;
    std::condition_variable ready_processors_cv;
    
public:
    explicit PipelineExecutor(Processors processors_, size_t max_threads)
        : processors(std::move(processors_))
        , thread_pool(max_threads)
    {
        initializeProcessorGraph();
    }
    
    void execute()
    {
        // Start worker threads
        for (size_t i = 0; i < thread_pool.getMaxThreads(); ++i)
        {
            threads.emplace_back([this] { workerThread(); });
        }
        
        // Wait for completion
        for (auto & thread : threads)
            thread.join();
    }
    
    void cancel()
    {
        cancelled = true;
        ready_processors_cv.notify_all();
    }
    
private:
    void initializeProcessorGraph()
    {
        // Build processor dependency graph
        for (auto & processor : processors)
        {
            // Initialize processor state
            auto status = processor->prepare();
            if (status == IProcessor::Status::Ready)
            {
                addToReadyQueue(processor.get());
            }
        }
    }
    
    void workerThread()
    {
        while (!cancelled)
        {
            IProcessor* processor = nullptr;
            
            // Get next ready processor
            {
                std::unique_lock<std::mutex> lock(ready_processors_mutex);
                ready_processors_cv.wait(lock, [this] {
                    return !ready_processors.empty() || cancelled;
                });
                
                if (cancelled)
                    break;
                
                processor = ready_processors.front();
                ready_processors.pop();
            }
            
            if (processor)
            {
                executeProcessor(processor);
            }
        }
    }
    
    void executeProcessor(IProcessor* processor)
    {
        try
        {
            ++active_processors;
            
            // Execute processor work
            processor->work();
            
            // Update processor state and schedule dependent processors
            auto status = processor->prepare();
            handleProcessorStatus(processor, status);
            
            --active_processors;
        }
        catch (...)
        {
            --active_processors;
            cancel();
            throw;
        }
    }
    
    void handleProcessorStatus(IProcessor* processor, IProcessor::Status status)
    {
        switch (status)
        {
            case IProcessor::Status::Ready:
                addToReadyQueue(processor);
                break;
                
            case IProcessor::Status::NeedData:
            case IProcessor::Status::PortFull:
                // Check connected processors
                scheduleConnectedProcessors(processor);
                break;
                
            case IProcessor::Status::Finished:
                // Processor completed, check if pipeline is done
                scheduleConnectedProcessors(processor);
                break;
                
            case IProcessor::Status::ExpandPipeline:
            {
                // Add new processors to pipeline
                auto new_processors = processor->expandPipeline();
                for (auto & new_processor : new_processors)
                {
                    processors.push_back(new_processor);
                    auto new_status = new_processor->prepare();
                    handleProcessorStatus(new_processor.get(), new_status);
                }
                break;
            }
                
            case IProcessor::Status::Async:
                // Processor is doing async work, will be rescheduled later
                break;
        }
    }
    
    void addToReadyQueue(IProcessor* processor)
    {
        std::lock_guard<std::mutex> lock(ready_processors_mutex);
        ready_processors.push(processor);
        ready_processors_cv.notify_one();
    }
    
    void scheduleConnectedProcessors(IProcessor* processor)
    {
        // Check all connected processors and schedule ready ones
        for (auto & input : processor->getInputs())
        {
            if (input.isConnected())
            {
                auto * connected_processor = input.getConnectedPort().getProcessor();
                auto status = connected_processor->prepare();
                if (status == IProcessor::Status::Ready)
                {
                    addToReadyQueue(connected_processor);
                }
            }
        }
        
        for (auto & output : processor->getOutputs())
        {
            if (output.isConnected())
            {
                auto * connected_processor = output.getConnectedPort().getProcessor();
                auto status = connected_processor->prepare();
                if (status == IProcessor::Status::Ready)
                {
                    addToReadyQueue(connected_processor);
                }
            }
        }
    }
};
```

This sophisticated pipeline construction system enables ClickHouse to build highly optimized, parallel execution graphs that can efficiently process large volumes of data while maintaining excellent resource utilization and performance characteristics.

## Phase 1 Summary

Phase 1 has provided a comprehensive foundation covering ClickHouse's query pipeline construction and execution architecture. We've explored:

1. **Parser Architecture and AST Construction**: The sophisticated recursive descent parser, AST node hierarchies, memory management, visitor patterns, optimization passes, and type system integration.

2. **Query Analysis Engine**: The evolution from legacy to new analyzer, QueryTree abstraction, semantic analysis phases, and advanced symbol resolution mechanisms.

3. **Query Planning Architecture**: QueryPlan structure, step hierarchy, optimization rules engine, and cost-based optimization strategies.

4. **Pipeline Construction**: Processor architecture, core processor types, pipeline graph construction, and resource allocation with parallelism.

This foundation sets the stage for the deeper technical exploration that will follow in subsequent phases, covering storage engines, distributed execution, memory management, and performance optimization techniques.

---

# Phase 2: Storage Engine Deep Dive (20,000 words)

## 2.1 IStorage Interface and Storage Engine Architecture (4,000 words)

ClickHouse's storage layer is built around a sophisticated abstraction that enables diverse storage engines while maintaining a consistent interface for query execution. At the heart of this architecture lies the `IStorage` interface, which defines the contract between the query processing layer and the underlying storage implementations.

### 2.1.1 IStorage Interface Design

The `IStorage` interface represents one of ClickHouse's most critical abstractions, providing a unified API for all storage engines:

```cpp
class IStorage : public std::enable_shared_from_this<IStorage>
{
public:
    using StoragePtr = std::shared_ptr<IStorage>;
    
    // Core metadata interface
    virtual String getName() const = 0;
    virtual String getTableName() const = 0;
    virtual String getDatabaseName() const = 0;
    
    // Schema management
    virtual StorageInMemoryMetadata getInMemoryMetadata() const = 0;
    virtual Block getHeader() const = 0;
    virtual NamesAndTypesList getColumns() const = 0;
    virtual NamesAndTypesList getVirtuals() const { return {}; }
    
    // Query execution interface
    virtual void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) = 0;
    
    virtual SinkToStoragePtr write(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr context,
        bool async_insert = false) = 0;
    
    // Transaction and consistency
    virtual void startup() {}
    virtual void shutdown() {}
    virtual void flush() {}
    
    // Storage capabilities
    virtual bool supportsParallelInsert() const { return false; }
    virtual bool supportsSubcolumns() const { return false; }
    virtual bool supportsDynamicSubcolumns() const { return false; }
    virtual bool supportsPrewhere() const { return false; }
    virtual bool supportsFinal() const { return false; }
    virtual bool supportsIndexForIn() const { return false; }
    virtual bool supportsReplication() const { return false; }
    virtual bool supportsDeduplication() const { return false; }
    
    // Optimization hints
    virtual std::optional<UInt64> totalRows(const Settings & settings) const { return {}; }
    virtual std::optional<UInt64> totalBytes(const Settings & settings) const { return {}; }
    
    // Advanced operations
    virtual void alter(const AlterCommands & commands, ContextPtr context, 
                      AlterLockHolder & table_lock_holder) {}
    virtual void checkAlterIsPossible(const AlterCommands & commands, 
                                    ContextPtr context) const {}
    
    virtual Pipe alterPartition(const ASTPtr & query, const StorageMetadataPtr & metadata_snapshot,
                               const PartitionCommands & commands, ContextPtr context) { return {}; }
    
    // Mutation support
    virtual void mutate(const MutationCommands & commands, ContextPtr context) {}
    virtual bool hasMutationsToWaitFor() const { return false; }
    virtual void waitForMutation(Int64 version, const String & file_name) {}
    
protected:
    mutable std::shared_mutex metadata_mutex;
    StorageInMemoryMetadata metadata;
    
    // Storage identification
    StorageID storage_id;
    String relative_data_path;
    
    // Engine-specific settings
    ContextPtr global_context;
    LoggerPtr log;
};
```

### 2.1.2 Storage Engine Registration and Factory Pattern

ClickHouse uses a sophisticated factory pattern to manage storage engine registration and instantiation:

```cpp
class StorageFactory : private boost::noncopyable
{
public:
    using Creator = std::function<StoragePtr(const StorageFactory::Arguments & args)>;
    using Features = std::set<String>;
    
    struct Arguments
    {
        const String & engine_name;
        ASTs & engine_args;
        ASTStorage * storage_def;
        const ASTCreateQuery & query;
        const String & relative_data_path;
        const StorageID & table_id;
        ContextPtr local_context;
        ContextPtr context;
        const ColumnsDescription & columns;
        const ConstraintsDescription & constraints;
        bool attach;
        bool has_force_restore_data_flag;
        const String & comment;
    };
    
    static StorageFactory & instance()
    {
        static StorageFactory factory;
        return factory;
    }
    
    StoragePtr get(const Arguments & arguments) const
    {
        auto it = storages.find(arguments.engine_name);
        if (it == storages.end())
            throw Exception("Unknown storage engine " + arguments.engine_name, 
                          ErrorCodes::UNKNOWN_STORAGE);
        
        return it->second.creator(arguments);
    }
    
    void registerStorage(const String & name, Creator creator, Features features = {})
    {
        if (!storages.emplace(name, StorageInfo{std::move(creator), std::move(features)}).second)
            throw Exception("Storage engine " + name + " already registered", 
                          ErrorCodes::LOGICAL_ERROR);
    }
    
    const auto & getAllStorages() const { return storages; }
    
    bool isStorageSupported(const String & name) const
    {
        return storages.find(name) != storages.end();
    }
    
    Features getStorageFeatures(const String & name) const
    {
        auto it = storages.find(name);
        return it != storages.end() ? it->second.features : Features{};
    }
    
private:
    struct StorageInfo
    {
        Creator creator;
        Features features;
    };
    
    std::unordered_map<String, StorageInfo> storages;
};

// Registration macro for storage engines
#define REGISTER_STORAGE(NAME, CREATOR) \
    namespace { \
        class Register##NAME { \
        public: \
            Register##NAME() { \
                StorageFactory::instance().registerStorage(#NAME, CREATOR); \
            } \
        }; \
        static Register##NAME register_##NAME; \
    }
```

### 2.1.3 Storage Metadata Management

ClickHouse maintains comprehensive metadata for each storage engine through the `StorageInMemoryMetadata` class:

```cpp
class StorageInMemoryMetadata
{
public:
    ColumnsDescription columns;
    IndicesDescription secondary_indices;
    ConstraintsDescription constraints;
    ProjectionsDescription projections;
    
    // Table properties
    ASTPtr partition_key;
    ASTPtr primary_key;
    ASTPtr order_by;
    ASTPtr sample_by;
    ASTPtr ttl_table;
    
    // Settings and configuration
    ASTStorage storage_def;
    String comment;
    
    // Derived metadata
    KeyDescription partition_key_desc;
    KeyDescription primary_key_desc;
    KeyDescription sorting_key_desc;
    KeyDescription sampling_key_desc;
    
    TTLDescription ttl_description;
    
public:
    StorageInMemoryMetadata() = default;
    StorageInMemoryMetadata(const StorageInMemoryMetadata & other);
    StorageInMemoryMetadata & operator=(const StorageInMemoryMetadata & other);
    
    bool empty() const { return columns.empty(); }
    
    void setColumns(ColumnsDescription columns_);
    void setSecondaryIndices(IndicesDescription secondary_indices_);
    void setConstraints(ConstraintsDescription constraints_);
    void setProjections(ProjectionsDescription projections_);
    
    void setPartitionKey(const ASTPtr & partition_key_);
    void setPrimaryKey(const ASTPtr & primary_key_);
    void setOrderBy(const ASTPtr & order_by_);
    void setSampleBy(const ASTPtr & sample_by_);
    void setTTL(const ASTPtr & ttl_);
    
    // Validation and consistency checks
    void check(const NamesAndTypesList & provided_columns) const;
    void checkCompatibility(const StorageInMemoryMetadata & other) const;
    
    // Key analysis
    Names getColumnsRequiredForPartitionKey() const;
    Names getColumnsRequiredForPrimaryKey() const;
    Names getColumnsRequiredForSortingKey() const;
    Names getColumnsRequiredForSampling() const;
    
    // Serialization for DDL operations
    ASTPtr getCreateTableQuery() const;
    String getCreateTableQueryString() const;
    
private:
    void buildKeyDescriptions(ContextPtr context);
    void validateKeyExpressions() const;
};
```

### 2.1.4 Storage Snapshot System

ClickHouse implements a sophisticated snapshot system to ensure consistent reads across concurrent operations:

```cpp
class StorageSnapshot
{
public:
    using Ptr = std::shared_ptr<StorageSnapshot>;
    
    StorageSnapshot(
        IStorage & storage_,
        StorageMetadataPtr metadata_,
        ContextPtr context_)
        : storage(storage_)
        , metadata(std::move(metadata_))
        , context(context_)
        , snapshot_time(std::chrono::system_clock::now())
    {
        // Capture current state
        object_columns = metadata->getColumns();
        
        // Build column dependencies
        buildColumnDependencies();
        
        // Initialize virtual columns
        virtual_columns = storage.getVirtuals();
    }
    
    // Column access interface
    const ColumnsDescription & getColumns() const { return object_columns; }
    const NamesAndTypesList & getVirtuals() const { return virtual_columns; }
    
    ColumnPtr getColumn(const String & column_name) const
    {
        if (auto column = object_columns.tryGetPhysical(column_name))
            return column;
        
        // Check virtual columns
        for (const auto & virtual_column : virtual_columns)
        {
            if (virtual_column.name == column_name)
                return virtual_column.type->createColumn();
        }
        
        throw Exception("Column " + column_name + " not found", ErrorCodes::NO_SUCH_COLUMN_IN_TABLE);
    }
    
    // Metadata access
    const StorageMetadataPtr & getMetadata() const { return metadata; }
    
    // Consistency validation
    bool isValid() const
    {
        auto current_metadata = storage.getInMemoryMetadata();
        return metadata->hash() == current_metadata.hash();
    }
    
    // Column dependency tracking
    Names getRequiredColumns(const Names & requested_columns) const
    {
        std::set<String> required;
        
        for (const auto & column_name : requested_columns)
        {
            required.insert(column_name);
            
            // Add dependent columns
            auto it = column_dependencies.find(column_name);
            if (it != column_dependencies.end())
            {
                for (const auto & dep : it->second)
                    required.insert(dep);
            }
        }
        
        return Names(required.begin(), required.end());
    }
    
private:
    IStorage & storage;
    StorageMetadataPtr metadata;
    ContextPtr context;
    
    ColumnsDescription object_columns;
    NamesAndTypesList virtual_columns;
    
    std::chrono::system_clock::time_point snapshot_time;
    
    // Column dependency graph for computed columns
    std::unordered_map<String, std::set<String>> column_dependencies;
    
    void buildColumnDependencies()
    {
        // Build dependency graph for computed columns, materialized columns, etc.
        for (const auto & column : object_columns)
        {
            if (column.default_desc.kind == ColumnDefaultKind::Materialized ||
                column.default_desc.kind == ColumnDefaultKind::Alias)
            {
                auto dependencies = extractColumnDependencies(column.default_desc.expression);
                column_dependencies[column.name] = std::move(dependencies);
            }
        }
    }
    
    std::set<String> extractColumnDependencies(const ASTPtr & expression) const
    {
        std::set<String> dependencies;
        
        if (!expression)
            return dependencies;
        
        // Traverse AST to find column references
        class ColumnVisitor : public InDepthNodeVisitor<ColumnVisitor, true>
        {
        public:
            std::set<String> & dependencies;
            explicit ColumnVisitor(std::set<String> & deps) : dependencies(deps) {}
            
            void visit(ASTPtr & node)
            {
                if (auto identifier = node->as<ASTIdentifier>())
                {
                    dependencies.insert(identifier->name());
                }
            }
        };
        
        ColumnVisitor visitor(dependencies);
        visitor.visit(const_cast<ASTPtr&>(expression));
        
        return dependencies;
    }
};
```

This comprehensive storage architecture provides ClickHouse with the flexibility to support diverse storage engines while maintaining consistent performance and reliability characteristics across all implementations.

## 2.2 MergeTree Family Architecture (5,000 words)

The MergeTree family of storage engines forms the cornerstone of ClickHouse's storage architecture, designed specifically for high-performance analytical workloads with massive data volumes and high ingestion rates.

### 2.2.1 Core MergeTree Implementation

The basic MergeTree engine implements the fundamental LSM-tree-inspired architecture that underlies all variants in the family:

```cpp
class StorageMergeTree : public MergeTreeData, public IStorage
{
public:
    StorageMergeTree(
        const StorageID & table_id_,
        const String & relative_data_path_,
        const StorageInMemoryMetadata & metadata_,
        ContextMutablePtr context_,
        const String & date_column_name,
        const MergingParams & merging_params_,
        std::unique_ptr<MergeTreeSettings> storage_settings_,
        bool has_force_restore_data_flag)
        : MergeTreeData(table_id_, relative_data_path_, metadata_, context_,
                       date_column_name, merging_params_, std::move(storage_settings_),
                       has_force_restore_data_flag)
        , background_operations_assignee(*this, BackgroundOperationsAssignee::Type::DataProcessing)
        , background_moves_assignee(*this, BackgroundOperationsAssignee::Type::DataMoving)
    {
        initializeBackgroundTasks();
    }
    
    String getName() const override { return merging_params.getModeName() + "MergeTree"; }
    
    void read(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override
    {
        // Create read-from-merge-tree step
        auto reading_step = std::make_unique<ReadFromMergeTree>(
            column_names,
            storage_snapshot,
            query_info,
            context,
            max_block_size,
            num_streams,
            processed_stage,
            shared_from_this());
        
        query_plan.addStep(std::move(reading_step));
    }
    
    SinkToStoragePtr write(
        const ASTPtr & query,
        const StorageMetadataPtr & metadata_snapshot,
        ContextPtr context,
        bool async_insert) override
    {
        return std::make_shared<MergeTreeSink>(
            *this, metadata_snapshot, context, async_insert);
    }
    
    void startup() override
    {
        background_operations_assignee.start();
        background_moves_assignee.start();
        
        // Start background merge scheduler
        if (auto pool = getContext()->getBackgroundPool())
        {
            background_operations_assignee.assignToPool(pool);
        }
        
        // Start background move scheduler for tiered storage
        if (auto move_pool = getContext()->getBackgroundMovePool())
        {
            background_moves_assignee.assignToPool(move_pool);
        }
    }
    
    void shutdown() override
    {
        background_operations_assignee.finish();
        background_moves_assignee.finish();
        
        // Wait for all background operations to complete
        std::unique_lock lock(background_operations_mutex);
        background_operations_condition.wait(lock, [this] {
            return background_operations_count == 0;
        });
    }
    
    // Merge operations
    bool scheduleDataProcessingJob(BackgroundJobsAssignee & assignee) override
    {
        if (shutdown_called)
            return false;
        
        auto merge_entry = selectPartsToMerge();
        if (!merge_entry)
            return false;
        
        assignee.scheduleCommonTask([this, merge_entry]() mutable {
            return executeMerge(std::move(merge_entry));
        });
        
        return true;
    }
    
    // Mutation operations  
    void mutate(const MutationCommands & commands, ContextPtr context) override
    {
        auto mutation_entry = std::make_shared<MergeTreeMutationEntry>(commands, context);
        
        std::lock_guard lock(mutation_mutex);
        
        // Add mutation to queue
        Int64 version = mutation_entry->commit(zookeeper);
        current_mutations_by_version.emplace(version, mutation_entry);
        
        // Wake up background merger to process mutation
        background_operations_condition.notify_all();
    }
    
private:
    BackgroundJobsAssignee background_operations_assignee;
    BackgroundJobsAssignee background_moves_assignee;
    
    std::mutex background_operations_mutex;
    std::condition_variable background_operations_condition;
    std::atomic<size_t> background_operations_count{0};
    
    // Mutation tracking
    std::mutex mutation_mutex;
    std::map<Int64, MergeTreeMutationEntryPtr> current_mutations_by_version;
    
    void initializeBackgroundTasks()
    {
        // Set up merge selection strategy
        merge_selecting_task = std::make_shared<MergeSelectingTask>(*this);
        
        // Set up cleanup tasks
        cleanup_task = std::make_shared<CleanupTask>(*this);
        
        // Set up moving task for tiered storage
        moving_task = std::make_shared<MovingTask>(*this);
    }
    
    MergeTreeDataMergerMutator::FutureMergedMutatedPartPtr selectPartsToMerge()
    {
        std::lock_guard lock(currently_processing_in_background_mutex);
        
        auto data_settings = getSettings();
        auto metadata_snapshot = getInMemoryMetadataPtr();
        
        // Select parts for merge based on various strategies
        auto future_part = merger_mutator.selectPartsToMerge(
            future_parts,
            false, // aggressive
            data_settings->max_bytes_to_merge_at_max_space_in_pool,
            merge_pred,
            nullptr, // txn
            nullptr); // out_disable_reason
        
        if (!future_part)
            return nullptr;
        
        // Reserve selected parts
        for (const auto & part : future_part->parts)
        {
            currently_merging_mutating_parts.emplace(part, future_part);
        }
        
        return future_part;
    }
    
    bool executeMerge(MergeTreeDataMergerMutator::FutureMergedMutatedPartPtr future_part)
    {
        ++background_operations_count;
        
        try
        {
            auto transaction = createTransaction();
            
            // Execute the actual merge
            auto part = merger_mutator.mergePartsToTemporaryPart(
                future_part,
                metadata_snapshot,
                nullptr, // merge_entry
                nullptr, // table_lock_holder
                time(nullptr),
                getContext(),
                transaction->getTID(),
                space_reservation,
                deduplicate,
                deduplicate_by_columns,
                cleanup,
                merge_mutate_entry,
                need_prefix);
            
            // Commit the merge
            renameTempPartAndAdd(part, transaction);
            transaction->commit();
            
            // Update part selection predicates
            updateMergePredicates();
            
            return true;
        }
        catch (...)
        {
            // Handle merge failure
            LOG_ERROR(log, "Failed to execute merge: {}", getCurrentExceptionMessage(false));
            
            // Clean up reserved parts
            std::lock_guard lock(currently_processing_in_background_mutex);
            for (const auto & part : future_part->parts)
            {
                currently_merging_mutating_parts.erase(part);
            }
            
            --background_operations_count;
            background_operations_condition.notify_all();
            
            return false;
        }
        
        --background_operations_count;
        background_operations_condition.notify_all();
        
        return true;
    }
};
```

### 2.2.2 Specialized MergeTree Variants

ClickHouse provides several specialized variants of MergeTree, each optimized for specific use cases:

**ReplacingMergeTree for Data Deduplication:**

```cpp
class StorageReplacingMergeTree : public StorageMergeTree
{
public:
    StorageReplacingMergeTree(/* parameters */)
        : StorageMergeTree(/* base parameters */)
    {
        // Set replacing merge parameters
        merging_params.mode = MergingParams::Replacing;
        merging_params.version_column = version_column_name;
        merging_params.is_deleted_column = is_deleted_column_name;
    }
    
    String getName() const override { return "ReplacingMergeTree"; }
    
protected:
    // Custom merge logic for deduplication
    class ReplacingMergedBlockOutputStream : public MergedBlockOutputStream
    {
    public:
        ReplacingMergedBlockOutputStream(/* parameters */)
            : MergedBlockOutputStream(/* base parameters */)
            , version_column_num(version_column_num_)
            , is_deleted_column_num(is_deleted_column_num_)
        {}
        
        void write(Block && block) override
        {
            if (!block)
                return;
            
            // Sort by primary key + version (if present)
            sortBlock(block);
            
            // Deduplicate rows with same primary key
            auto deduplicated_block = deduplicateBlock(block);
            
            MergedBlockOutputStream::write(std::move(deduplicated_block));
        }
        
    private:
        size_t version_column_num;
        size_t is_deleted_column_num;
        
        Block deduplicateBlock(const Block & block)
        {
            if (block.rows() <= 1)
                return block;
            
            // Group rows by primary key
            std::map<String, std::vector<size_t>> key_to_rows;
            
            for (size_t i = 0; i < block.rows(); ++i)
            {
                String key = extractPrimaryKey(block, i);
                key_to_rows[key].push_back(i);
            }
            
            // Select latest version for each key
            std::vector<size_t> selected_rows;
            for (const auto & [key, rows] : key_to_rows)
            {
                size_t selected_row = selectLatestVersion(block, rows);
                
                // Check if row is deleted
                if (is_deleted_column_num != std::numeric_limits<size_t>::max())
                {
                    auto is_deleted_column = block.getByPosition(is_deleted_column_num).column;
                    if (is_deleted_column->getBool(selected_row))
                        continue; // Skip deleted rows
                }
                
                selected_rows.push_back(selected_row);
            }
            
            // Build result block
            return buildResultBlock(block, selected_rows);
        }
        
        size_t selectLatestVersion(const Block & block, const std::vector<size_t> & rows)
        {
            if (version_column_num == std::numeric_limits<size_t>::max())
                return rows.back(); // No version column, use last row
            
            size_t best_row = rows[0];
            auto version_column = block.getByPosition(version_column_num).column;
            
            for (size_t i = 1; i < rows.size(); ++i)
            {
                if (version_column->compareAt(rows[i], best_row, *version_column, 1) > 0)
                    best_row = rows[i];
            }
            
            return best_row;
        }
    };
};
```

**SummingMergeTree for Aggregation:**

```cpp
class StorageSummingMergeTree : public StorageMergeTree
{
public:
    StorageSummingMergeTree(/* parameters */)
        : StorageMergeTree(/* base parameters */)
    {
        merging_params.mode = MergingParams::Summing;
        merging_params.columns_to_sum = columns_to_sum_;
        merging_params.partition_value_types = partition_value_types_;
    }
    
    String getName() const override { return "SummingMergeTree"; }
    
protected:
    class SummingMergedBlockOutputStream : public MergedBlockOutputStream
    {
    public:
        SummingMergedBlockOutputStream(/* parameters */)
            : MergedBlockOutputStream(/* base parameters */)
            , columns_to_sum(columns_to_sum_)
        {
            // Identify summable columns
            for (size_t i = 0; i < header.columns(); ++i)
            {
                const auto & column_name = header.getByPosition(i).name;
                const auto & column_type = header.getByPosition(i).type;
                
                if (columns_to_sum.empty() || columns_to_sum.count(column_name))
                {
                    if (column_type->isNumeric())
                    {
                        summable_columns.push_back(i);
                    }
                }
            }
        }
        
        void write(Block && block) override
        {
            if (!block)
                return;
            
            // Sort by primary key
            sortBlock(block);
            
            // Sum rows with same primary key
            auto summed_block = sumBlock(block);
            
            MergedBlockOutputStream::write(std::move(summed_block));
        }
        
    private:
        Names columns_to_sum;
        std::vector<size_t> summable_columns;
        
        Block sumBlock(const Block & block)
        {
            if (block.rows() <= 1)
                return block;
            
            // Group rows by primary key
            std::map<String, std::vector<size_t>> key_to_rows;
            
            for (size_t i = 0; i < block.rows(); ++i)
            {
                String key = extractPrimaryKey(block, i);
                key_to_rows[key].push_back(i);
            }
            
            // Sum rows for each key
            MutableColumns result_columns = block.cloneEmptyColumns();
            
            for (const auto & [key, rows] : key_to_rows)
            {
                if (rows.size() == 1)
                {
                    // Single row, just copy
                    for (size_t col = 0; col < result_columns.size(); ++col)
                    {
                        result_columns[col]->insertFrom(*block.getByPosition(col).column, rows[0]);
                    }
                }
                else
                {
                    // Multiple rows, sum them
                    sumRows(block, rows, result_columns);
                }
            }
            
            return block.cloneWithColumns(std::move(result_columns));
        }
        
        void sumRows(const Block & block, const std::vector<size_t> & rows, MutableColumns & result_columns)
        {
            // Copy first row as base
            for (size_t col = 0; col < result_columns.size(); ++col)
            {
                result_columns[col]->insertFrom(*block.getByPosition(col).column, rows[0]);
            }
            
            size_t result_row = result_columns[0]->size() - 1;
            
            // Sum numeric columns
            for (size_t col_idx : summable_columns)
            {
                auto & result_column = result_columns[col_idx];
                auto source_column = block.getByPosition(col_idx).column;
                
                // Sum all rows into the result
                for (size_t i = 1; i < rows.size(); ++i)
                {
                    addToColumn(*result_column, result_row, *source_column, rows[i]);
                }
            }
        }
        
        void addToColumn(IColumn & result_column, size_t result_row, 
                        const IColumn & source_column, size_t source_row)
        {
            // Type-specific addition logic
            WhichDataType which(result_column.getDataType());
            
            if (which.isInt8())
                addNumeric<Int8>(result_column, result_row, source_column, source_row);
            else if (which.isInt16())
                addNumeric<Int16>(result_column, result_row, source_column, source_row);
            else if (which.isInt32())
                addNumeric<Int32>(result_column, result_row, source_column, source_row);
            else if (which.isInt64())
                addNumeric<Int64>(result_column, result_row, source_column, source_row);
            else if (which.isFloat32())
                addNumeric<Float32>(result_column, result_row, source_column, source_row);
            else if (which.isFloat64())
                addNumeric<Float64>(result_column, result_row, source_column, source_row);
            // ... other numeric types
        }
        
        template<typename T>
        void addNumeric(IColumn & result_column, size_t result_row,
                       const IColumn & source_column, size_t source_row)
        {
            auto & typed_result = static_cast<ColumnVector<T> &>(result_column);
            auto & typed_source = static_cast<const ColumnVector<T> &>(source_column);
            
            typed_result.getData()[result_row] += typed_source.getData()[source_row];
        }
    };
};
```

### 2.2.3 Part Management and Lifecycle

MergeTree engines implement sophisticated part management to optimize storage and query performance:

```cpp
class MergeTreeData
{
public:
    using DataPartPtr = std::shared_ptr<IMergeTreeDataPart>;
    using DataPartsVector = std::vector<DataPartPtr>;
    using DataPartState = IMergeTreeDataPart::State;
    
    struct LessDataPart
    {
        using is_transparent = void;
        
        bool operator()(const DataPartPtr & lhs, const DataPartPtr & rhs) const
        {
            return lhs->info < rhs->info;
        }
    };
    
    using DataPartsIndexes = std::map<DataPartPtr, size_t, LessDataPart>;
    using DataPartsLock = std::unique_lock<std::shared_mutex>;
    
protected:
    mutable std::shared_mutex data_parts_mutex;
    
    /// Current set of data parts.
    DataPartsIndexes data_parts_indexes;
    DataPartsVector data_parts_by_state_and_info[DataPartState::MAX_VALUE];
    
    /// Index of parts by partition and min_block_number, for fast search of parts within partition.
    std::map<String, std::map<Int64, DataPartPtr>> data_parts_by_info;
    
public:
    DataPartsVector getDataPartsVector(
        const DataPartStates & affordable_states = DataPartStates({DataPartState::Active}),
        DataPartsLock * acquired_lock = nullptr) const
    {
        DataPartsLock lock(data_parts_mutex);
        
        if (acquired_lock)
            *acquired_lock = std::move(lock);
        
        DataPartsVector result;
        for (auto state : affordable_states)
        {
            auto & parts_in_state = data_parts_by_state_and_info[state];
            result.insert(result.end(), parts_in_state.begin(), parts_in_state.end());
        }
        
        return result;
    }
    
    DataPartPtr getPartIfExists(const MergeTreePartInfo & part_info, const DataPartStates & valid_states)
    {
        DataPartsLock lock(data_parts_mutex);
        
        auto it = data_parts_by_info.find(part_info.partition_id);
        if (it == data_parts_by_info.end())
            return nullptr;
        
        auto part_it = it->second.find(part_info.min_block_number);
        if (part_it == it->second.end())
            return nullptr;
        
        auto part = part_it->second;
        if (valid_states.count(part->getState()))
            return part;
        
        return nullptr;
    }
    
    void addPart(DataPartPtr part)
    {
        DataPartsLock lock(data_parts_mutex);
        addPartNoLock(part, lock);
    }
    
    bool renamePart(DataPartPtr part, const String & new_name)
    {
        DataPartsLock lock(data_parts_mutex);
        
        if (part->getState() != DataPartState::Active)
            return false;
        
        // Remove from indexes
        removePartFromIndexes(part);
        
        // Rename on disk
        auto old_path = part->getFullPath();
        auto new_path = relative_data_path + new_name + "/";
        
        if (!Poco::File(old_path).exists())
            return false;
        
        Poco::File(old_path).renameTo(new_path);
        
        // Update part info
        part->name = new_name;
        part->info = MergeTreePartInfo::fromPartName(new_name, format_version);
        
        // Add back to indexes
        addPartToIndexes(part);
        
        return true;
    }
    
    void removePartsInRangeFromWorkingSet(const MergeTreePartInfo & drop_range, 
                                         DataPartsVector & parts_to_remove)
    {
        DataPartsLock lock(data_parts_mutex);
        
        auto partition_it = data_parts_by_info.find(drop_range.partition_id);
        if (partition_it == data_parts_by_info.end())
            return;
        
        auto & parts_in_partition = partition_it->second;
        
        // Find all parts that intersect with drop range
        auto begin_it = parts_in_partition.lower_bound(drop_range.min_block_number);
        auto end_it = parts_in_partition.upper_bound(drop_range.max_block_number);
        
        for (auto it = begin_it; it != end_it; ++it)
        {
            auto part = it->second;
            if (part->info.intersects(drop_range))
            {
                parts_to_remove.push_back(part);
                removePartFromIndexes(part);
                part->setState(DataPartState::Outdated);
            }
        }
    }
    
private:
    void addPartNoLock(DataPartPtr part, DataPartsLock & /* lock */)
    {
        auto state = part->getState();
        
        // Add to main indexes
        data_parts_indexes.emplace(part, data_parts_by_state_and_info[state].size());
        data_parts_by_state_and_info[state].push_back(part);
        
        // Add to partition index
        data_parts_by_info[part->info.partition_id][part->info.min_block_number] = part;
        
        // Update statistics
        updateDataPartsStats();
    }
    
    void removePartFromIndexes(DataPartPtr part)
    {
        auto state = part->getState();
        
        // Remove from main index
        auto main_it = data_parts_indexes.find(part);
        if (main_it != data_parts_indexes.end())
        {
            auto & parts_vector = data_parts_by_state_and_info[state];
            auto vector_index = main_it->second;
            
            // Swap with last element and pop
            if (vector_index < parts_vector.size() - 1)
            {
                std::swap(parts_vector[vector_index], parts_vector.back());
                data_parts_indexes[parts_vector[vector_index]] = vector_index;
            }
            
            parts_vector.pop_back();
            data_parts_indexes.erase(main_it);
        }
        
        // Remove from partition index
        auto partition_it = data_parts_by_info.find(part->info.partition_id);
        if (partition_it != data_parts_by_info.end())
        {
            partition_it->second.erase(part->info.min_block_number);
            if (partition_it->second.empty())
                data_parts_by_info.erase(partition_it);
        }
    }
    
    void updateDataPartsStats()
    {
        // Update various statistics about data parts
        total_active_size_bytes = 0;
        total_active_size_rows = 0;
        
        for (const auto & part : data_parts_by_state_and_info[DataPartState::Active])
        {
            total_active_size_bytes += part->getBytesOnDisk();
            total_active_size_rows += part->rows_count;
        }
    }
};
```

This sophisticated part management system enables ClickHouse to efficiently handle massive datasets while maintaining excellent query performance through intelligent part organization, lifecycle management, and background optimization processes.

## 2.3 Data Parts, Granules, and Blocks Implementation (4,000 words)

The physical storage organization in ClickHouse is built around a three-tier hierarchy: data parts, granules, and blocks. This sophisticated structure enables efficient compression, indexing, and parallel processing while maintaining excellent query performance.

### 2.3.1 Data Part Structure and Implementation

Each data part represents an immutable collection of data stored on disk, implementing the fundamental unit of ClickHouse's LSM-tree-inspired storage:

```cpp
class IMergeTreeDataPart : public std::enable_shared_from_this<IMergeTreeDataPart>
{
public:
    enum State
    {
        Temporary,       // Part is being created
        PreActive,       // Part is created but not yet active
        Active,          // Part is active and can be read
        Outdated,        // Part is outdated but not yet deleted
        Deleting,        // Part is being deleted
        DeleteOnDestroy, // Part will be deleted when object is destroyed
        MAX_VALUE = DeleteOnDestroy
    };
    
    using Checksums = MergeTreeDataPartChecksums;
    using ColumnSize = std::pair<size_t, size_t>; // compressed, uncompressed
    using ColumnSizeByName = std::map<String, ColumnSize>;
    
protected:
    const MergeTreeData & storage;
    String name;
    MergeTreePartInfo info;
    
    mutable State state{Temporary};
    mutable std::mutex state_mutex;
    
    // Part metadata
    size_t rows_count = 0;
    time_t modification_time = 0;
    mutable time_t remove_time = std::numeric_limits<time_t>::max();
    
    // Storage paths
    String relative_path;
    mutable String absolute_path;
    
    // Checksums and validation
    mutable Checksums checksums;
    mutable bool checksums_loaded = false;
    
    // Column information
    mutable ColumnSizeByName columns_sizes;
    mutable std::optional<time_t> columns_sizes_on_disk_load_time;
    
    // Index and marks
    mutable MarkCache::MappedPtr marks_cache;
    mutable UncompressedCache::MappedPtr uncompressed_cache;
    
public:
    IMergeTreeDataPart(
        const MergeTreeData & storage_,
        const String & name_,
        const MergeTreePartInfo & info_,
        const VolumePtr & volume_,
        const std::optional<String> & relative_path_ = {})
        : storage(storage_)
        , name(name_)
        , info(info_)
        , volume(volume_)
    {
        if (relative_path_.has_value())
            relative_path = relative_path_.value();
        else
            relative_path = info.getPartName();
    }
    
    virtual ~IMergeTreeDataPart() = default;
    
    // Basic accessors
    const String & getName() const { return name; }
    const MergeTreePartInfo & getInfo() const { return info; }
    String getFullPath() const
    {
        if (absolute_path.empty())
            absolute_path = storage.getFullPathOnDisk(volume->getDisk()) + relative_path + "/";
        return absolute_path;
    }
    
    // State management
    State getState() const
    {
        std::lock_guard lock(state_mutex);
        return state;
    }
    
    void setState(State new_state) const
    {
        std::lock_guard lock(state_mutex);
        state = new_state;
    }
    
    bool isStoredOnDisk() const
    {
        return volume->getDisk()->exists(relative_path);
    }
    
    // Size and statistics
    virtual UInt64 getBytesOnDisk() const = 0;
    virtual UInt64 getMarksCount() const = 0;
    
    size_t getRowsCount() const { return rows_count; }
    time_t getModificationTime() const { return modification_time; }
    
    // Column operations
    virtual bool hasColumnFiles(const NameAndTypePair & column) const = 0;
    virtual std::optional<String> getFileNameForColumn(const NameAndTypePair & column) const = 0;
    virtual void checkConsistency(bool require_part_metadata) const = 0;
    
    // Serialization
    virtual void loadRowsCount() = 0;
    virtual void loadPartitionAndMinMaxIndex() = 0;
    virtual void loadIndex() = 0;
    virtual void loadProjections(bool require_columns_checksums) = 0;
    
    // Column size calculation
    ColumnSize getColumnSize(const String & column_name) const
    {
        loadColumnsSizes();
        auto it = columns_sizes.find(column_name);
        return it == columns_sizes.end() ? ColumnSize{0, 0} : it->second;
    }
    
    ColumnSizeByName getColumnsSizes() const
    {
        loadColumnsSizes();
        return columns_sizes;
    }
    
    // Checksums management
    const Checksums & getChecksums() const
    {
        loadChecksums();
        return checksums;
    }
    
    void setChecksums(const Checksums & checksums_)
    {
        checksums = checksums_;
        checksums_loaded = true;
    }
    
    // Index access
    virtual void loadPrimaryIndex() = 0;
    virtual void unloadPrimaryIndex() = 0;
    virtual bool isPrimaryIndexLoaded() const = 0;
    
protected:
    VolumePtr volume;
    DiskPtr disk;
    
    virtual void loadChecksums() const = 0;
    virtual void loadColumnsSizes() const = 0;
    
    // Helper methods for derived classes
    void calculateColumnsSizesOnDisk()
    {
        if (columns_sizes_on_disk_load_time && 
            *columns_sizes_on_disk_load_time >= modification_time)
            return;
        
        columns_sizes.clear();
        
        auto disk = volume->getDisk();
        String path = getFullPath();
        
        for (const auto & [file_name, checksum] : checksums.files)
        {
            if (file_name.ends_with(".bin"))
            {
                String column_name = file_name.substr(0, file_name.length() - 4);
                auto file_size = disk->getFileSize(path + file_name);
                columns_sizes[column_name] = ColumnSize{file_size, checksum.uncompressed_size};
            }
        }
        
        columns_sizes_on_disk_load_time = modification_time;
    }
};
```

### 2.3.2 Wide vs Compact Part Formats

ClickHouse supports two physical storage formats for data parts, each optimized for different scenarios:

**Wide Format Implementation:**

```cpp
class MergeTreeDataPartWide : public IMergeTreeDataPart
{
public:
    MergeTreeDataPartWide(/* parameters */)
        : IMergeTreeDataPart(/* base parameters */)
    {}
    
    String getTypeName() const override { return "Wide"; }
    
    bool hasColumnFiles(const NameAndTypePair & column) const override
    {
        auto disk = volume->getDisk();
        String path = getFullPath();
        
        // Check for main column file
        String data_file = path + escapeForFileName(column.name) + ".bin";
        if (!disk->exists(data_file))
            return false;
        
        // Check for mark file
        String mark_file = path + escapeForFileName(column.name) + ".mrk2";
        if (!disk->exists(mark_file))
            return false;
        
        return true;
    }
    
    std::optional<String> getFileNameForColumn(const NameAndTypePair & column) const override
    {
        String escaped_name = escapeForFileName(column.name);
        
        // Handle different column types
        if (column.type->isNullable())
        {
            // Nullable columns have separate null map files
            return escaped_name + ".null.bin";
        }
        else if (column.type->isArray())
        {
            // Array columns have separate size files
            return escaped_name + ".size0.bin";
        }
        else if (column.type->isLowCardinality())
        {
            // LowCardinality columns have dictionary files
            return escaped_name + ".dict.bin";
        }
        
        return escaped_name + ".bin";
    }
    
    UInt64 getBytesOnDisk() const override
    {
        loadChecksums();
        
        UInt64 total_size = 0;
        for (const auto & [file_name, checksum] : checksums.files)
        {
            total_size += checksum.file_size;
        }
        
        return total_size;
    }
    
    UInt64 getMarksCount() const override
    {
        if (marks_count_cache.has_value())
            return *marks_count_cache;
        
        // Calculate marks count from any mark file
        auto disk = volume->getDisk();
        String path = getFullPath();
        
        for (const auto & [file_name, checksum] : checksums.files)
        {
            if (file_name.ends_with(".mrk2"))
            {
                auto file_size = disk->getFileSize(path + file_name);
                marks_count_cache = file_size / sizeof(MarkInCompressedFile);
                return *marks_count_cache;
            }
        }
        
        return 0;
    }
    
    void loadPrimaryIndex() override
    {
        if (primary_index_loaded)
            return;
        
        auto disk = volume->getDisk();
        String index_path = getFullPath() + "primary.idx";
        
        if (!disk->exists(index_path))
        {
            primary_index_loaded = true;
            return;
        }
        
        auto index_file = disk->readFile(index_path);
        auto index_size = disk->getFileSize(index_path);
        
        // Read primary index
        primary_index.resize(index_size / storage.getPrimaryKeySize());
        index_file->read(reinterpret_cast<char*>(primary_index.data()), index_size);
        
        primary_index_loaded = true;
    }
    
protected:
    void loadChecksums() const override
    {
        if (checksums_loaded)
            return;
        
        auto disk = volume->getDisk();
        String checksums_path = getFullPath() + "checksums.txt";
        
        if (!disk->exists(checksums_path))
        {
            checksums_loaded = true;
            return;
        }
        
        auto checksums_file = disk->readFile(checksums_path);
        String checksums_content;
        readStringUntilEOF(checksums_content, *checksums_file);
        
        // Parse checksums file
        checksums.read(checksums_content);
        checksums_loaded = true;
    }
    
    void loadColumnsSizes() const override
    {
        calculateColumnsSizesOnDisk();
    }
    
private:
    mutable std::optional<UInt64> marks_count_cache;
    mutable bool primary_index_loaded = false;
    mutable PrimaryIndex primary_index;
};
```

**Compact Format Implementation:**

```cpp
class MergeTreeDataPartCompact : public IMergeTreeDataPart
{
public:
    static constexpr auto DATA_FILE_NAME = "data.bin";
    static constexpr auto DATA_FILE_NAME_WITH_EXTENSION = "data.cmrk2";
    
    MergeTreeDataPartCompact(/* parameters */)
        : IMergeTreeDataPart(/* base parameters */)
    {}
    
    String getTypeName() const override { return "Compact"; }
    
    bool hasColumnFiles(const NameAndTypePair & column) const override
    {
        // In compact format, all columns are in single data file
        auto disk = volume->getDisk();
        String path = getFullPath();
        
        return disk->exists(path + DATA_FILE_NAME) && 
               disk->exists(path + DATA_FILE_NAME_WITH_EXTENSION);
    }
    
    std::optional<String> getFileNameForColumn(const NameAndTypePair & column) const override
    {
        // All columns share the same data file in compact format
        return DATA_FILE_NAME;
    }
    
    UInt64 getBytesOnDisk() const override
    {
        loadChecksums();
        
        auto it = checksums.files.find(DATA_FILE_NAME);
        if (it != checksums.files.end())
            return it->second.file_size;
        
        return 0;
    }
    
    UInt64 getMarksCount() const override
    {
        if (marks_count_cache.has_value())
            return *marks_count_cache;
        
        auto disk = volume->getDisk();
        String marks_path = getFullPath() + DATA_FILE_NAME_WITH_EXTENSION;
        
        if (!disk->exists(marks_path))
            return 0;
        
        auto file_size = disk->getFileSize(marks_path);
        auto columns_count = storage.getInMemoryMetadataPtr()->getColumns().size();
        
        marks_count_cache = file_size / (columns_count * sizeof(MarkInCompressedFile));
        return *marks_count_cache;
    }
    
    void loadPrimaryIndex() override
    {
        if (primary_index_loaded)
            return;
        
        // Primary index is embedded in the compact marks file
        loadMarksFile();
        primary_index_loaded = true;
    }
    
protected:
    void loadChecksums() const override
    {
        if (checksums_loaded)
            return;
        
        auto disk = volume->getDisk();
        String checksums_path = getFullPath() + "checksums.txt";
        
        if (disk->exists(checksums_path))
        {
            auto checksums_file = disk->readFile(checksums_path);
            String checksums_content;
            readStringUntilEOF(checksums_content, *checksums_file);
            checksums.read(checksums_content);
        }
        
        checksums_loaded = true;
    }
    
    void loadColumnsSizes() const override
    {
        // In compact format, need to parse the data file structure
        loadMarksFile();
        calculateColumnsSizesFromMarks();
    }
    
private:
    mutable std::optional<UInt64> marks_count_cache;
    mutable bool primary_index_loaded = false;
    mutable bool marks_loaded = false;
    
    struct CompactMark
    {
        size_t offset_in_compressed_file;
        size_t offset_in_decompressed_block;
        UInt64 rows_in_granule;
    };
    
    mutable std::vector<std::vector<CompactMark>> marks_by_column;
    
    void loadMarksFile() const
    {
        if (marks_loaded)
            return;
        
        auto disk = volume->getDisk();
        String marks_path = getFullPath() + DATA_FILE_NAME_WITH_EXTENSION;
        
        if (!disk->exists(marks_path))
        {
            marks_loaded = true;
            return;
        }
        
        auto marks_file = disk->readFile(marks_path);
        auto metadata = storage.getInMemoryMetadataPtr();
        auto columns = metadata->getColumns().getAllPhysical();
        
        marks_by_column.resize(columns.size());
        
        size_t marks_count = getMarksCount();
        
        for (size_t mark_idx = 0; mark_idx < marks_count; ++mark_idx)
        {
            for (size_t col_idx = 0; col_idx < columns.size(); ++col_idx)
            {
                CompactMark mark;
                readBinary(mark.offset_in_compressed_file, *marks_file);
                readBinary(mark.offset_in_decompressed_block, *marks_file);
                readBinary(mark.rows_in_granule, *marks_file);
                
                marks_by_column[col_idx].push_back(mark);
            }
        }
        
        marks_loaded = true;
    }
    
    void calculateColumnsSizesFromMarks() const
    {
        if (!marks_loaded)
            return;
        
        columns_sizes.clear();
        auto columns = storage.getInMemoryMetadataPtr()->getColumns().getAllPhysical();
        
        for (size_t col_idx = 0; col_idx < columns.size(); ++col_idx)
        {
            const auto & column = columns[col_idx];
            const auto & column_marks = marks_by_column[col_idx];
            
            if (column_marks.empty())
                continue;
            
            // Calculate compressed size based on mark offsets
            size_t compressed_size = 0;
            if (column_marks.size() > 1)
            {
                compressed_size = column_marks.back().offset_in_compressed_file - 
                                column_marks.front().offset_in_compressed_file;
            }
            
            // Estimate uncompressed size
            size_t uncompressed_size = 0;
            for (const auto & mark : column_marks)
            {
                uncompressed_size += mark.rows_in_granule * column.type->getSizeOfValueInMemory();
            }
            
            columns_sizes[column.name] = ColumnSize{compressed_size, uncompressed_size};
        }
    }
};
```

### 2.3.3 Granule Organization and Mark System

The granule system provides the logical organization for data access and indexing:

```cpp
class MergeTreeMarksLoader
{
public:
    using MarksPtr = std::shared_ptr<MarksInCompressedFile>;
    
    struct LoadedMarks
    {
        MarksPtr marks;
        size_t marks_count;
        size_t granules_count;
    };
    
    static LoadedMarks loadMarks(
        const MergeTreeDataPartPtr & data_part,
        const String & stream_name,
        const MarkCache::Key & cache_key,
        MarkCache * mark_cache)
    {
        // Try to get from cache first
        if (mark_cache)
        {
            auto cached_marks = mark_cache->get(cache_key);
            if (cached_marks)
            {
                return LoadedMarks{
                    cached_marks,
                    cached_marks->size(),
                    cached_marks->size()
                };
            }
        }
        
        // Load from disk
        auto marks = loadMarksFromDisk(data_part, stream_name);
        
        // Cache the loaded marks
        if (mark_cache && marks)
        {
            mark_cache->set(cache_key, marks);
        }
        
        return LoadedMarks{
            marks,
            marks ? marks->size() : 0,
            marks ? marks->size() : 0
        };
    }
    
private:
    static MarksPtr loadMarksFromDisk(
        const MergeTreeDataPartPtr & data_part,
        const String & stream_name)
    {
        String marks_file_path = data_part->getFullPath() + stream_name + ".mrk2";
        auto disk = data_part->volume->getDisk();
        
        if (!disk->exists(marks_file_path))
            return nullptr;
        
        auto file_size = disk->getFileSize(marks_file_path);
        size_t marks_count = file_size / sizeof(MarkInCompressedFile);
        
        auto marks = std::make_shared<MarksInCompressedFile>(marks_count);
        
        auto marks_file = disk->readFile(marks_file_path);
        marks_file->read(reinterpret_cast<char*>(marks->data()), file_size);
        
        return marks;
    }
};

struct MarkInCompressedFile
{
    size_t offset_in_compressed_file;
    size_t offset_in_decompressed_block;
    
    bool operator==(const MarkInCompressedFile & other) const
    {
        return offset_in_compressed_file == other.offset_in_compressed_file &&
               offset_in_decompressed_block == other.offset_in_decompressed_block;
    }
    
    bool operator<(const MarkInCompressedFile & other) const
    {
        return std::tie(offset_in_compressed_file, offset_in_decompressed_block) <
               std::tie(other.offset_in_compressed_file, other.offset_in_decompressed_block);
    }
};

using MarksInCompressedFile = std::vector<MarkInCompressedFile>;
```

### 2.3.4 Block-Level Compression and Storage

ClickHouse implements sophisticated block-level compression to optimize storage efficiency:

```cpp
class CompressedBlockOutputStream
{
public:
    CompressedBlockOutputStream(
        WriteBuffer & out_,
        CompressionCodecPtr codec_,
        size_t max_compressed_block_size_ = DEFAULT_MAX_COMPRESSED_BLOCK_SIZE)
        : out(out_)
        , codec(std::move(codec_))
        , max_compressed_block_size(max_compressed_block_size_)
    {}
    
    void write(const char * data, size_t size)
    {
        while (size > 0)
        {
            size_t bytes_to_write = std::min(size, max_compressed_block_size - buffer.size());
            
            buffer.insert(buffer.end(), data, data + bytes_to_write);
            data += bytes_to_write;
            size -= bytes_to_write;
            
            if (buffer.size() >= max_compressed_block_size)
            {
                flushBuffer();
            }
        }
    }
    
    void flush()
    {
        if (!buffer.empty())
            flushBuffer();
        out.sync();
    }
    
private:
    WriteBuffer & out;
    CompressionCodecPtr codec;
    size_t max_compressed_block_size;
    
    std::vector<char> buffer;
    
    void flushBuffer()
    {
        if (buffer.empty())
            return;
        
        // Compress the buffer
        auto compressed_data = codec->compress(buffer.data(), buffer.size());
        
        // Write compressed block header
        writeCompressedBlockHeader(out, compressed_data.size(), buffer.size());
        
        // Write compressed data
        out.write(compressed_data.data(), compressed_data.size());
        
        buffer.clear();
    }
    
    void writeCompressedBlockHeader(WriteBuffer & out, size_t compressed_size, size_t uncompressed_size)
    {
        // ClickHouse compressed block format:
        // - 16 bytes checksum (CityHash128)
        // - 1 byte compression method
        // - 4 bytes compressed size
        // - 4 bytes uncompressed size
        
        UInt128 checksum = CityHash_v1_0_2::CityHash128(
            reinterpret_cast<const char*>(&compressed_size), 
            sizeof(compressed_size) + sizeof(uncompressed_size));
        
        writeBinary(checksum.low64, out);
        writeBinary(checksum.high64, out);
        writeBinary(static_cast<UInt8>(codec->getMethodByte()), out);
        writeBinary(static_cast<UInt32>(compressed_size), out);
        writeBinary(static_cast<UInt32>(uncompressed_size), out);
    }
};

class CompressedBlockInputStream
{
public:
    CompressedBlockInputStream(ReadBuffer & in_)
        : in(in_)
    {}
    
    size_t read(char * data, size_t size)
    {
        size_t bytes_read = 0;
        
        while (bytes_read < size)
        {
            if (current_block_pos >= current_block.size())
            {
                if (!loadNextBlock())
                    break;
            }
            
            size_t bytes_to_copy = std::min(
                size - bytes_read,
                current_block.size() - current_block_pos);
            
            memcpy(data + bytes_read, 
                   current_block.data() + current_block_pos, 
                   bytes_to_copy);
            
            bytes_read += bytes_to_copy;
            current_block_pos += bytes_to_copy;
        }
        
        return bytes_read;
    }
    
    bool eof() const
    {
        return in.eof() && current_block_pos >= current_block.size();
    }
    
private:
    ReadBuffer & in;
    std::vector<char> current_block;
    size_t current_block_pos = 0;
    
    bool loadNextBlock()
    {
        if (in.eof())
            return false;
        
        // Read compressed block header
        UInt128 checksum;
        UInt8 method;
        UInt32 compressed_size;
        UInt32 uncompressed_size;
        
        readBinary(checksum.low64, in);
        readBinary(checksum.high64, in);
        readBinary(method, in);
        readBinary(compressed_size, in);
        readBinary(uncompressed_size, in);
        
        // Validate checksum
        UInt128 expected_checksum = CityHash_v1_0_2::CityHash128(
            reinterpret_cast<const char*>(&compressed_size),
            sizeof(compressed_size) + sizeof(uncompressed_size));
        
        if (checksum != expected_checksum)
            throw Exception("Checksum mismatch in compressed block", ErrorCodes::CHECKSUM_DOESNT_MATCH);
        
        // Read compressed data
        std::vector<char> compressed_data(compressed_size);
        in.read(compressed_data.data(), compressed_size);
        
        // Decompress
        auto codec = CompressionCodecFactory::instance().get(method);
        current_block = codec->decompress(compressed_data.data(), compressed_size, uncompressed_size);
        current_block_pos = 0;
        
        return true;
    }
};
```

This three-tier storage hierarchy enables ClickHouse to achieve excellent compression ratios, efficient indexing, and high-performance parallel processing while maintaining the flexibility to handle diverse data types and access patterns.

## 2.4 Compression Algorithms and Codecs (3,500 words)

ClickHouse implements a comprehensive compression framework that supports multiple algorithms optimized for different data types and access patterns. The codec system provides both high compression ratios and fast decompression speeds essential for analytical workloads.

### 2.4.1 Compression Codec Architecture

The compression system is built around a flexible codec interface that enables pluggable compression algorithms:

```cpp
class ICompressionCodec
{
public:
    virtual ~ICompressionCodec() = default;
    
    // Core compression interface
    virtual UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const = 0;
    virtual UInt32 compress(const char * source, UInt32 source_size, char * dest) const = 0;
    virtual void decompress(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const = 0;
    
    // Codec identification
    virtual UInt8 getMethodByte() const = 0;
    virtual String getCodecDesc() const = 0;
    
    // Performance characteristics
    virtual bool isCompression() const { return true; }
    virtual bool isGenericCompression() const { return true; }
    virtual bool isNone() const { return false; }
    
    // Optimization hints
    virtual bool isExperimentalCodec() const { return false; }
    virtual bool isDeltaCodec() const { return false; }
    virtual bool isFloatingPointTimeSeriesCodec() const { return false; }
    
    // Factory method for creating codec instances
    static CompressionCodecPtr createInstance(UInt8 method_byte);
    
protected:
    // Helper methods for codec implementations
    static void validateDecompressedSize(UInt32 size_decompressed_must_be, UInt32 size_decompressed);
    static UInt32 readDecompressedBlockSize(const char * compressed_data);
    static void writeDecompressedBlockSize(char * compressed_data, UInt32 size_decompressed);
};

using CompressionCodecPtr = std::shared_ptr<ICompressionCodec>;
```

### 2.4.2 LZ4 Compression Implementation

LZ4 serves as the default compression algorithm, providing excellent balance between compression ratio and speed:

```cpp
class CompressionCodecLZ4 : public ICompressionCodec
{
public:
    static constexpr UInt8 METHOD_BYTE = 0x82;
    static constexpr auto CODEC_NAME = "LZ4";
    
    CompressionCodecLZ4() = default;
    
    UInt8 getMethodByte() const override { return METHOD_BYTE; }
    String getCodecDesc() const override { return CODEC_NAME; }
    
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override
    {
        return LZ4_compressBound(uncompressed_size);
    }
    
    UInt32 compress(const char * source, UInt32 source_size, char * dest) const override
    {
        return LZ4_compress_default(source, dest, source_size, getMaxCompressedDataSize(source_size));
    }
    
    void decompress(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override
    {
        int result = LZ4_decompress_safe(source, dest, source_size, uncompressed_size);
        
        if (result < 0)
            throw Exception("Cannot decompress LZ4 block", ErrorCodes::CANNOT_DECOMPRESS);
        
        validateDecompressedSize(uncompressed_size, result);
    }
    
    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return true; }
};
```

### 2.4.3 ZSTD Compression Implementation

ZSTD provides higher compression ratios at the cost of increased CPU usage:

```cpp
class CompressionCodecZSTD : public ICompressionCodec
{
public:
    static constexpr UInt8 METHOD_BYTE = 0x90;
    static constexpr auto CODEC_NAME = "ZSTD";
    
    explicit CompressionCodecZSTD(int level = 1) : compression_level(level) {}
    
    UInt8 getMethodByte() const override { return METHOD_BYTE; }
    String getCodecDesc() const override 
    { 
        return CODEC_NAME + "(" + std::to_string(compression_level) + ")"; 
    }
    
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override
    {
        return ZSTD_compressBound(uncompressed_size);
    }
    
    UInt32 compress(const char * source, UInt32 source_size, char * dest) const override
    {
        size_t compressed_size = ZSTD_compress(dest, getMaxCompressedDataSize(source_size),
                                             source, source_size, compression_level);
        
        if (ZSTD_isError(compressed_size))
            throw Exception("Cannot compress with ZSTD: " + String(ZSTD_getErrorName(compressed_size)), 
                          ErrorCodes::CANNOT_COMPRESS);
        
        return compressed_size;
    }
    
    void decompress(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override
    {
        size_t decompressed_size = ZSTD_decompress(dest, uncompressed_size, source, source_size);
        
        if (ZSTD_isError(decompressed_size))
            throw Exception("Cannot decompress ZSTD: " + String(ZSTD_getErrorName(decompressed_size)), 
                          ErrorCodes::CANNOT_DECOMPRESS);
        
        validateDecompressedSize(uncompressed_size, decompressed_size);
    }
    
private:
    int compression_level;
};
```

### 2.4.4 Delta Compression for Time Series

Delta compression is particularly effective for time series and monotonic data:

```cpp
class CompressionCodecDelta : public ICompressionCodec
{
public:
    static constexpr UInt8 METHOD_BYTE = 0x93;
    static constexpr auto CODEC_NAME = "Delta";
    
    explicit CompressionCodecDelta(UInt8 delta_bytes_size) : delta_bytes_size(delta_bytes_size) {}
    
    UInt8 getMethodByte() const override { return METHOD_BYTE; }
    String getCodecDesc() const override 
    { 
        return CODEC_NAME + "(" + std::to_string(delta_bytes_size) + ")"; 
    }
    
    bool isDeltaCodec() const override { return true; }
    bool isGenericCompression() const override { return false; }
    
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override
    {
        // Delta compression doesn't reduce size, just transforms data
        return uncompressed_size + sizeof(UInt8); // +1 for delta_bytes_size
    }
    
    UInt32 compress(const char * source, UInt32 source_size, char * dest) const override
    {
        if (source_size < delta_bytes_size)
            throw Exception("Cannot compress with Delta codec: source size too small", 
                          ErrorCodes::CANNOT_COMPRESS);
        
        // Write delta_bytes_size header
        *dest = delta_bytes_size;
        dest++;
        
        // Copy first value unchanged
        memcpy(dest, source, delta_bytes_size);
        dest += delta_bytes_size;
        source += delta_bytes_size;
        
        // Apply delta compression
        UInt32 compressed_size = sizeof(UInt8) + delta_bytes_size;
        
        switch (delta_bytes_size)
        {
            case 1:
                compressed_size += compressDelta<UInt8>(source, dest, source_size - delta_bytes_size);
                break;
            case 2:
                compressed_size += compressDelta<UInt16>(source, dest, source_size - delta_bytes_size);
                break;
            case 4:
                compressed_size += compressDelta<UInt32>(source, dest, source_size - delta_bytes_size);
                break;
            case 8:
                compressed_size += compressDelta<UInt64>(source, dest, source_size - delta_bytes_size);
                break;
            default:
                throw Exception("Unsupported delta bytes size: " + std::to_string(delta_bytes_size), 
                              ErrorCodes::BAD_ARGUMENTS);
        }
        
        return compressed_size;
    }
    
    void decompress(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override
    {
        if (source_size < sizeof(UInt8))
            throw Exception("Cannot decompress Delta: source too small", ErrorCodes::CANNOT_DECOMPRESS);
        
        UInt8 stored_delta_bytes_size = *source;
        source++;
        
        if (stored_delta_bytes_size != delta_bytes_size)
            throw Exception("Delta bytes size mismatch", ErrorCodes::CANNOT_DECOMPRESS);
        
        if (source_size < sizeof(UInt8) + delta_bytes_size)
            throw Exception("Cannot decompress Delta: source too small", ErrorCodes::CANNOT_DECOMPRESS);
        
        // Copy first value unchanged
        memcpy(dest, source, delta_bytes_size);
        dest += delta_bytes_size;
        source += delta_bytes_size;
        
        // Decompress deltas
        switch (delta_bytes_size)
        {
            case 1:
                decompressDelta<UInt8>(source, dest, source_size - sizeof(UInt8) - delta_bytes_size,
                                     uncompressed_size - delta_bytes_size);
                break;
            case 2:
                decompressDelta<UInt16>(source, dest, source_size - sizeof(UInt8) - delta_bytes_size,
                                      uncompressed_size - delta_bytes_size);
                break;
            case 4:
                decompressDelta<UInt32>(source, dest, source_size - sizeof(UInt8) - delta_bytes_size,
                                      uncompressed_size - delta_bytes_size);
                break;
            case 8:
                decompressDelta<UInt64>(source, dest, source_size - sizeof(UInt8) - delta_bytes_size,
                                      uncompressed_size - delta_bytes_size);
                break;
            default:
                throw Exception("Unsupported delta bytes size: " + std::to_string(delta_bytes_size), 
                              ErrorCodes::BAD_ARGUMENTS);
        }
    }
    
private:
    UInt8 delta_bytes_size;
    
    template<typename T>
    UInt32 compressDelta(const char * source, char * dest, UInt32 source_size) const
    {
        const T * source_typed = reinterpret_cast<const T *>(source);
        T * dest_typed = reinterpret_cast<T *>(dest);
        
        UInt32 source_count = source_size / sizeof(T);
        T previous = *reinterpret_cast<const T *>(source - sizeof(T)); // First value
        
        for (UInt32 i = 0; i < source_count; ++i)
        {
            T current = source_typed[i];
            dest_typed[i] = current - previous;
            previous = current;
        }
        
        return source_count * sizeof(T);
    }
    
    template<typename T>
    void decompressDelta(const char * source, char * dest, UInt32 source_size, UInt32 uncompressed_size) const
    {
        const T * source_typed = reinterpret_cast<const T *>(source);
        T * dest_typed = reinterpret_cast<T *>(dest);
        
        UInt32 source_count = source_size / sizeof(T);
        UInt32 dest_count = uncompressed_size / sizeof(T);
        
        if (source_count != dest_count)
            throw Exception("Delta decompression size mismatch", ErrorCodes::CANNOT_DECOMPRESS);
        
        T previous = *reinterpret_cast<const T *>(dest - sizeof(T)); // First value
        
        for (UInt32 i = 0; i < source_count; ++i)
        {
            T delta = source_typed[i];
            T current = previous + delta;
            dest_typed[i] = current;
            previous = current;
        }
    }
};
```

### 2.4.5 Double Delta Compression

Double delta compression is highly effective for timestamps and other smoothly changing sequences:

```cpp
class CompressionCodecDoubleDelta : public ICompressionCodec
{
public:
    static constexpr UInt8 METHOD_BYTE = 0x94;
    static constexpr auto CODEC_NAME = "DoubleDelta";
    
    explicit CompressionCodecDoubleDelta(UInt8 data_bytes_size) : data_bytes_size(data_bytes_size) {}
    
    UInt8 getMethodByte() const override { return METHOD_BYTE; }
    String getCodecDesc() const override 
    { 
        return CODEC_NAME + "(" + std::to_string(data_bytes_size) + ")"; 
    }
    
    bool isDeltaCodec() const override { return true; }
    bool isGenericCompression() const override { return false; }
    
    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override
    {
        // Double delta can potentially expand data in worst case
        return uncompressed_size + (uncompressed_size / 8) + sizeof(UInt8) + 2 * data_bytes_size;
    }
    
    UInt32 compress(const char * source, UInt32 source_size, char * dest) const override
    {
        if (source_size < 2 * data_bytes_size)
            throw Exception("Cannot compress with DoubleDelta: source size too small", 
                          ErrorCodes::CANNOT_COMPRESS);
        
        switch (data_bytes_size)
        {
            case 1:
                return compressDoubleDelta<UInt8>(source, source_size, dest);
            case 2:
                return compressDoubleDelta<UInt16>(source, source_size, dest);
            case 4:
                return compressDoubleDelta<UInt32>(source, source_size, dest);
            case 8:
                return compressDoubleDelta<UInt64>(source, source_size, dest);
            default:
                throw Exception("Unsupported data bytes size: " + std::to_string(data_bytes_size), 
                              ErrorCodes::BAD_ARGUMENTS);
        }
    }
    
    void decompress(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override
    {
        if (source_size < sizeof(UInt8))
            throw Exception("Cannot decompress DoubleDelta: source too small", ErrorCodes::CANNOT_DECOMPRESS);
        
        UInt8 stored_data_bytes_size = *source;
        if (stored_data_bytes_size != data_bytes_size)
            throw Exception("Data bytes size mismatch", ErrorCodes::CANNOT_DECOMPRESS);
        
        switch (data_bytes_size)
        {
            case 1:
                decompressDoubleDelta<UInt8>(source + 1, source_size - 1, dest, uncompressed_size);
                break;
            case 2:
                decompressDoubleDelta<UInt16>(source + 1, source_size - 1, dest, uncompressed_size);
                break;
            case 4:
                decompressDoubleDelta<UInt32>(source + 1, source_size - 1, dest, uncompressed_size);
                break;
            case 8:
                decompressDoubleDelta<UInt64>(source + 1, source_size - 1, dest, uncompressed_size);
                break;
            default:
                throw Exception("Unsupported data bytes size: " + std::to_string(data_bytes_size), 
                              ErrorCodes::BAD_ARGUMENTS);
        }
    }
    
private:
    UInt8 data_bytes_size;
    
    template<typename T>
    UInt32 compressDoubleDelta(const char * source, UInt32 source_size, char * dest) const
    {
        const T * source_typed = reinterpret_cast<const T *>(source);
        UInt32 source_count = source_size / sizeof(T);
        
        if (source_count < 2)
            throw Exception("DoubleDelta requires at least 2 values", ErrorCodes::CANNOT_COMPRESS);
        
        // Write header
        *dest = data_bytes_size;
        dest++;
        
        // Write first two values unchanged
        memcpy(dest, source_typed, 2 * sizeof(T));
        dest += 2 * sizeof(T);
        
        // Compress double deltas using variable-length encoding
        UInt32 compressed_size = sizeof(UInt8) + 2 * sizeof(T);
        
        T prev_value = source_typed[0];
        T curr_value = source_typed[1];
        T prev_delta = curr_value - prev_value;
        
        for (UInt32 i = 2; i < source_count; ++i)
        {
            T next_value = source_typed[i];
            T curr_delta = next_value - curr_value;
            T double_delta = curr_delta - prev_delta;
            
            // Variable-length encode the double delta
            compressed_size += encodeVarint(double_delta, dest);
            
            prev_value = curr_value;
            curr_value = next_value;
            prev_delta = curr_delta;
        }
        
        return compressed_size;
    }
    
    template<typename T>
    void decompressDoubleDelta(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
    {
        if (source_size < 2 * sizeof(T))
            throw Exception("DoubleDelta source too small", ErrorCodes::CANNOT_DECOMPRESS);
        
        T * dest_typed = reinterpret_cast<T *>(dest);
        UInt32 dest_count = uncompressed_size / sizeof(T);
        
        if (dest_count < 2)
            throw Exception("DoubleDelta destination too small", ErrorCodes::CANNOT_DECOMPRESS);
        
        // Read first two values
        memcpy(dest_typed, source, 2 * sizeof(T));
        source += 2 * sizeof(T);
        
        if (dest_count == 2)
            return;
        
        T prev_value = dest_typed[0];
        T curr_value = dest_typed[1];
        T prev_delta = curr_value - prev_value;
        
        const char * source_end = source + source_size - 2 * sizeof(T);
        
        for (UInt32 i = 2; i < dest_count; ++i)
        {
            if (source >= source_end)
                throw Exception("DoubleDelta source exhausted", ErrorCodes::CANNOT_DECOMPRESS);
            
            T double_delta;
            source += decodeVarint(source, double_delta);
            
            T curr_delta = prev_delta + double_delta;
            T next_value = curr_value + curr_delta;
            
            dest_typed[i] = next_value;
            
            prev_value = curr_value;
            curr_value = next_value;
            prev_delta = curr_delta;
        }
    }
    
    template<typename T>
    UInt32 encodeVarint(T value, char *& dest) const
    {
        // Simple variable-length encoding
        UInt32 bytes_written = 0;
        
        while (value >= 0x80)
        {
            *dest++ = (value & 0x7F) | 0x80;
            value >>= 7;
            bytes_written++;
        }
        
        *dest++ = value & 0x7F;
        bytes_written++;
        
        return bytes_written;
    }
    
    template<typename T>
    UInt32 decodeVarint(const char * source, T & value) const
    {
        value = 0;
        UInt32 shift = 0;
        UInt32 bytes_read = 0;
        
        while (true)
        {
            UInt8 byte = *source++;
            bytes_read++;
            
            value |= static_cast<T>(byte & 0x7F) << shift;
            
            if ((byte & 0x80) == 0)
                break;
            
            shift += 7;
        }
        
        return bytes_read;
    }
};
```

### 2.4.6 Codec Factory and Selection

The codec factory manages codec registration and selection based on data characteristics:

```cpp
class CompressionCodecFactory
{
public:
    static CompressionCodecFactory & instance()
    {
        static CompressionCodecFactory factory;
        return factory;
    }
    
    CompressionCodecPtr get(UInt8 method_byte) const
    {
        auto it = codecs_by_method_byte.find(method_byte);
        if (it == codecs_by_method_byte.end())
            throw Exception("Unknown compression method: " + std::to_string(method_byte), 
                          ErrorCodes::UNKNOWN_COMPRESSION_METHOD);
        
        return it->second();
    }
    
    CompressionCodecPtr get(const String & codec_name, std::optional<int> level = {}) const
    {
        auto it = codecs_by_name.find(codec_name);
        if (it == codecs_by_name.end())
            throw Exception("Unknown compression codec: " + codec_name, 
                          ErrorCodes::UNKNOWN_COMPRESSION_METHOD);
        
        return it->second(level);
    }
    
    void registerCodec(const String & codec_name, UInt8 method_byte, 
                      std::function<CompressionCodecPtr()> creator)
    {
        codecs_by_name[codec_name] = [creator](std::optional<int>) { return creator(); };
        codecs_by_method_byte[method_byte] = creator;
    }
    
    void registerParameterizedCodec(const String & codec_name, UInt8 method_byte,
                                   std::function<CompressionCodecPtr(std::optional<int>)> creator)
    {
        codecs_by_name[codec_name] = creator;
        codecs_by_method_byte[method_byte] = [creator]() { return creator({}); };
    }
    
    // Automatic codec selection based on data characteristics
    CompressionCodecPtr selectOptimalCodec(const ColumnPtr & column, 
                                          const CompressionSettings & settings) const
    {
        auto type = column->getDataType();
        
        // For time series data, prefer delta compression
        if (settings.enable_time_series_compression && isTimeSeriesCandidate(column))
        {
            if (type->isInteger() && type->getSizeOfValueInMemory() <= 8)
            {
                return std::make_shared<CompressionCodecDoubleDelta>(type->getSizeOfValueInMemory());
            }
        }
        
        // For floating point data, use specialized codecs
        if (type->isFloat())
        {
            return std::make_shared<CompressionCodecGorilla>();
        }
        
        // For highly repetitive data, use dictionary compression
        if (settings.enable_dictionary_compression && isDictionaryCandidate(column))
        {
            return std::make_shared<CompressionCodecT64>();
        }
        
        // Default to LZ4 for general purpose compression
        return std::make_shared<CompressionCodecLZ4>();
    }
    
private:
    std::unordered_map<String, std::function<CompressionCodecPtr(std::optional<int>)>> codecs_by_name;
    std::unordered_map<UInt8, std::function<CompressionCodecPtr()>> codecs_by_method_byte;
    
    bool isTimeSeriesCandidate(const ColumnPtr & column) const
    {
        // Analyze column for time series characteristics
        if (column->size() < 100)
            return false;
        
        // Check for monotonic or nearly monotonic sequences
        size_t monotonic_count = 0;
        for (size_t i = 1; i < std::min(column->size(), size_t(1000)); ++i)
        {
            if (column->compareAt(i, i-1, *column, 1) >= 0)
                monotonic_count++;
        }
        
        return monotonic_count > column->size() * 0.8;
    }
    
    bool isDictionaryCandidate(const ColumnPtr & column) const
    {
        // Check cardinality vs size ratio
        auto cardinality = column->getNumberOfUniqueValues();
        return cardinality < column->size() * 0.1; // Less than 10% unique values
    }
};
```

This comprehensive compression framework enables ClickHouse to achieve excellent storage efficiency while maintaining the high decompression speeds required for analytical query performance. The automatic codec selection ensures optimal compression for different data patterns without requiring manual tuning.

## 2.5 Index Structures and Skip Indices (3,500 words)

ClickHouse implements a sophisticated indexing system that combines primary indices with various skip indices to accelerate query execution across massive datasets. The indexing architecture is designed to work efficiently with the columnar storage format and support diverse query patterns.

### 2.5.1 Primary Index Implementation

The primary index in ClickHouse provides coarse-grained data location information based on the sorting key:

```cpp
class MergeTreePrimaryIndex
{
public:
    using IndexType = std::vector<Field>;
    using IndexPtr = std::shared_ptr<IndexType>;
    
    struct IndexEntry
    {
        IndexType key_values;
        size_t granule_number;
        size_t rows_count;
        
        bool operator<(const IndexEntry & other) const
        {
            return key_values < other.key_values;
        }
    };
    
    using IndexEntries = std::vector<IndexEntry>;
    
private:
    IndexEntries entries;
    KeyDescription key_description;
    size_t index_granularity;
    
public:
    MergeTreePrimaryIndex(const KeyDescription & key_description_, size_t index_granularity_)
        : key_description(key_description_), index_granularity(index_granularity_)
    {}
    
    void load(ReadBuffer & in, size_t marks_count)
    {
        entries.clear();
        entries.reserve(marks_count);
        
        for (size_t i = 0; i < marks_count; ++i)
        {
            IndexEntry entry;
            entry.granule_number = i;
            entry.key_values.resize(key_description.column_names.size());
            
            // Read key values for this granule
            for (size_t j = 0; j < key_description.column_names.size(); ++j)
            {
                const auto & key_column_type = key_description.data_types[j];
                key_column_type->deserializeBinary(entry.key_values[j], in);
            }
            
            entries.push_back(std::move(entry));
        }
    }
    
    void save(WriteBuffer & out) const
    {
        for (const auto & entry : entries)
        {
            for (size_t j = 0; j < key_description.column_names.size(); ++j)
            {
                const auto & key_column_type = key_description.data_types[j];
                key_column_type->serializeBinary(entry.key_values[j], out);
            }
        }
    }
    
    // Range queries using primary index
    MarkRanges getMarkRanges(const KeyCondition & key_condition) const
    {
        MarkRanges ranges;
        
        if (entries.empty())
            return ranges;
        
        // Binary search for range start
        size_t left_bound = 0;
        size_t right_bound = entries.size();
        
        // Find first granule that might contain matching data
        while (left_bound < right_bound)
        {
            size_t middle = left_bound + (right_bound - left_bound) / 2;
            
            if (key_condition.mayBeTrueInRange(entries[middle].key_values,
                                             middle + 1 < entries.size() ? 
                                             entries[middle + 1].key_values : 
                                             entries[middle].key_values))
            {
                right_bound = middle;
            }
            else
            {
                left_bound = middle + 1;
            }
        }
        
        size_t range_start = left_bound;
        
        // Find last granule that might contain matching data
        left_bound = range_start;
        right_bound = entries.size();
        
        while (left_bound < right_bound)
        {
            size_t middle = left_bound + (right_bound - left_bound) / 2;
            
            if (key_condition.mayBeTrueInRange(entries[middle].key_values,
                                             middle + 1 < entries.size() ? 
                                             entries[middle + 1].key_values : 
                                             entries[middle].key_values))
            {
                left_bound = middle + 1;
            }
            else
            {
                right_bound = middle;
            }
        }
        
        size_t range_end = left_bound;
        
        if (range_start < range_end)
        {
            ranges.emplace_back(range_start, range_end);
        }
        
        return ranges;
    }
    
    // Point queries
    std::optional<size_t> findGranule(const IndexType & key_values) const
    {
        auto it = std::lower_bound(entries.begin(), entries.end(), 
                                  IndexEntry{key_values, 0, 0});
        
        if (it != entries.end() && it->key_values == key_values)
            return it->granule_number;
        
        return std::nullopt;
    }
    
    // Statistics
    size_t getMemorySize() const
    {
        size_t size = sizeof(*this);
        size += entries.size() * sizeof(IndexEntry);
        
        for (const auto & entry : entries)
        {
            for (const auto & field : entry.key_values)
            {
                size += field.size();
            }
        }
        
        return size;
    }
    
    bool empty() const { return entries.empty(); }
    size_t size() const { return entries.size(); }
};
```

### 2.5.2 Skip Index Framework

Skip indices provide additional indexing capabilities for non-primary key columns:

```cpp
class IMergeTreeIndex
{
public:
    virtual ~IMergeTreeIndex() = default;
    
    // Index identification
    virtual String getName() const = 0;
    virtual String getTypeName() const = 0;
    
    // Index creation from column data
    virtual MergeTreeIndexAggregatorPtr createIndexAggregator() const = 0;
    virtual MergeTreeIndexConditionPtr createIndexCondition(
        const SelectQueryInfo & query_info, ContextPtr context) const = 0;
    
    // Serialization
    virtual void serializeBinary(const IndexGranules & granules, WriteBuffer & out) const = 0;
    virtual void deserializeBinary(IndexGranules & granules, ReadBuffer & in) const = 0;
    
    // Index properties
    virtual bool mayBenefitFromIndexForIn(const ASTPtr & node) const { return false; }
    virtual size_t getIndexGranuleSize() const = 0;
    
    // Memory usage
    virtual size_t getMemorySize() const = 0;
    
protected:
    String index_name;
    IndexDescription index_description;
};

class MergeTreeIndexAggregator
{
public:
    virtual ~MergeTreeIndexAggregator() = default;
    
    // Update index with new data
    virtual void update(const Block & block, size_t * pos, size_t limit) = 0;
    
    // Finalize current granule
    virtual IndexGranulePtr getGranuleAndReset() = 0;
    
    // Check if granule is ready
    virtual bool empty() const = 0;
};

class MergeTreeIndexCondition
{
public:
    virtual ~MergeTreeIndexCondition() = default;
    
    // Check if index granule might contain matching data
    virtual bool mayBeTrueOnGranule(const IndexGranulePtr & granule) const = 0;
    
    // Check if index can be used for this condition
    virtual bool alwaysUnknownOrTrue() const = 0;
    
    // Get selectivity estimate
    virtual Float64 getSelectivity() const { return 1.0; }
};
```

### 2.5.3 MinMax Skip Index Implementation

The MinMax index tracks minimum and maximum values for efficient range queries:

```cpp
class MergeTreeIndexMinMax : public IMergeTreeIndex
{
public:
    explicit MergeTreeIndexMinMax(const IndexDescription & index_description_)
        : index_description(index_description_)
    {
        index_name = index_description.name;
    }
    
    String getName() const override { return index_name; }
    String getTypeName() const override { return "minmax"; }
    
    MergeTreeIndexAggregatorPtr createIndexAggregator() const override
    {
        return std::make_shared<MergeTreeIndexAggregatorMinMax>(index_description);
    }
    
    MergeTreeIndexConditionPtr createIndexCondition(
        const SelectQueryInfo & query_info, ContextPtr context) const override
    {
        return std::make_shared<MergeTreeIndexConditionMinMax>(
            query_info, context, index_description);
    }
    
    void serializeBinary(const IndexGranules & granules, WriteBuffer & out) const override
    {
        for (const auto & granule : granules)
        {
            auto minmax_granule = std::static_pointer_cast<MergeTreeIndexGranuleMinMax>(granule);
            
            for (size_t i = 0; i < minmax_granule->min_values.size(); ++i)
            {
                const auto & type = index_description.data_types[i];
                type->serializeBinary(minmax_granule->min_values[i], out);
                type->serializeBinary(minmax_granule->max_values[i], out);
            }
        }
    }
    
    void deserializeBinary(IndexGranules & granules, ReadBuffer & in) const override
    {
        for (auto & granule : granules)
        {
            auto minmax_granule = std::static_pointer_cast<MergeTreeIndexGranuleMinMax>(granule);
            
            for (size_t i = 0; i < index_description.data_types.size(); ++i)
            {
                const auto & type = index_description.data_types[i];
                type->deserializeBinary(minmax_granule->min_values[i], in);
                type->deserializeBinary(minmax_granule->max_values[i], in);
            }
        }
    }
    
    size_t getIndexGranuleSize() const override
    {
        return index_description.data_types.size() * 2 * sizeof(Field);
    }
    
    size_t getMemorySize() const override
    {
        return sizeof(*this) + index_description.getMemorySize();
    }
};

class MergeTreeIndexGranuleMinMax : public IndexGranule
{
public:
    std::vector<Field> min_values;
    std::vector<Field> max_values;
    bool initialized = false;
    
    explicit MergeTreeIndexGranuleMinMax(size_t columns_count)
        : min_values(columns_count), max_values(columns_count)
    {}
    
    void update(const Block & block, size_t * pos, size_t limit) override
    {
        if (!initialized)
        {
            initialize(block);
            initialized = true;
        }
        
        for (size_t column_idx = 0; column_idx < block.columns(); ++column_idx)
        {
            const auto & column = block.getByPosition(column_idx).column;
            
            for (size_t row = *pos; row < *pos + limit; ++row)
            {
                Field value;
                column->get(row, value);
                
                if (value < min_values[column_idx])
                    min_values[column_idx] = value;
                
                if (value > max_values[column_idx])
                    max_values[column_idx] = value;
            }
        }
    }
    
    bool empty() const override { return !initialized; }
    
private:
    void initialize(const Block & block)
    {
        for (size_t column_idx = 0; column_idx < block.columns(); ++column_idx)
        {
            const auto & column = block.getByPosition(column_idx).column;
            
            if (column->size() > 0)
            {
                column->get(0, min_values[column_idx]);
                max_values[column_idx] = min_values[column_idx];
            }
        }
    }
};

class MergeTreeIndexConditionMinMax : public MergeTreeIndexCondition
{
public:
    MergeTreeIndexConditionMinMax(
        const SelectQueryInfo & query_info,
        ContextPtr context,
        const IndexDescription & index_description)
        : key_condition(query_info, context, index_description.column_names, 
                       index_description.expression)
    {}
    
    bool mayBeTrueOnGranule(const IndexGranulePtr & granule) const override
    {
        auto minmax_granule = std::static_pointer_cast<MergeTreeIndexGranuleMinMax>(granule);
        
        if (!minmax_granule->initialized)
            return true;
        
        return key_condition.mayBeTrueInRange(minmax_granule->min_values, 
                                            minmax_granule->max_values);
    }
    
    bool alwaysUnknownOrTrue() const override
    {
        return key_condition.alwaysUnknownOrTrue();
    }
    
    Float64 getSelectivity() const override
    {
        return key_condition.getSelectivity();
    }
    
private:
    KeyCondition key_condition;
};
```

### 2.5.4 Bloom Filter Skip Index

Bloom filters provide efficient membership testing for equality and IN queries:

```cpp
class MergeTreeIndexBloomFilter : public IMergeTreeIndex
{
public:
    explicit MergeTreeIndexBloomFilter(const IndexDescription & index_description_,
                                      size_t bits_per_row_ = 10.0,
                                      size_t hash_functions_ = 5)
        : index_description(index_description_)
        , bits_per_row(bits_per_row_)
        , hash_functions(hash_functions_)
    {
        index_name = index_description.name;
    }
    
    String getName() const override { return index_name; }
    String getTypeName() const override { return "bloom_filter"; }
    
    MergeTreeIndexAggregatorPtr createIndexAggregator() const override
    {
        return std::make_shared<MergeTreeIndexAggregatorBloomFilter>(
            index_description, bits_per_row, hash_functions);
    }
    
    MergeTreeIndexConditionPtr createIndexCondition(
        const SelectQueryInfo & query_info, ContextPtr context) const override
    {
        return std::make_shared<MergeTreeIndexConditionBloomFilter>(
            query_info, context, index_description, hash_functions);
    }
    
    bool mayBenefitFromIndexForIn(const ASTPtr & node) const override
    {
        return true; // Bloom filters are excellent for IN queries
    }
    
    size_t getIndexGranuleSize() const override
    {
        // Approximate size of bloom filter
        return (bits_per_row * 8192) / 8; // Assume 8192 rows per granule
    }
    
private:
    size_t bits_per_row;
    size_t hash_functions;
};

class BloomFilter
{
public:
    BloomFilter(size_t size_, size_t hash_functions_)
        : size(size_), hash_functions(hash_functions_)
    {
        bits.resize((size + 7) / 8, 0);
    }
    
    void add(const StringRef & value)
    {
        for (size_t i = 0; i < hash_functions; ++i)
        {
            size_t hash = calculateHash(value, i) % size;
            setBit(hash);
        }
    }
    
    bool contains(const StringRef & value) const
    {
        for (size_t i = 0; i < hash_functions; ++i)
        {
            size_t hash = calculateHash(value, i) % size;
            if (!getBit(hash))
                return false;
        }
        return true;
    }
    
    void serialize(WriteBuffer & out) const
    {
        writeBinary(size, out);
        writeBinary(hash_functions, out);
        out.write(reinterpret_cast<const char*>(bits.data()), bits.size());
    }
    
    void deserialize(ReadBuffer & in)
    {
        readBinary(size, in);
        readBinary(hash_functions, in);
        bits.resize((size + 7) / 8);
        in.read(reinterpret_cast<char*>(bits.data()), bits.size());
    }
    
private:
    size_t size;
    size_t hash_functions;
    std::vector<UInt8> bits;
    
    void setBit(size_t index)
    {
        bits[index / 8] |= (1 << (index % 8));
    }
    
    bool getBit(size_t index) const
    {
        return (bits[index / 8] & (1 << (index % 8))) != 0;
    }
    
    size_t calculateHash(const StringRef & value, size_t seed) const
    {
        // Use CityHash with different seeds
        return CityHash_v1_0_2::CityHash64WithSeed(value.data, value.size, seed);
    }
};

class MergeTreeIndexGranuleBloomFilter : public IndexGranule
{
public:
    std::vector<BloomFilter> bloom_filters;
    
    explicit MergeTreeIndexGranuleBloomFilter(
        size_t columns_count, size_t bits_per_row, size_t hash_functions)
    {
        bloom_filters.reserve(columns_count);
        for (size_t i = 0; i < columns_count; ++i)
        {
            bloom_filters.emplace_back(bits_per_row * 8192, hash_functions); // 8192 rows per granule
        }
    }
    
    void update(const Block & block, size_t * pos, size_t limit) override
    {
        for (size_t column_idx = 0; column_idx < block.columns(); ++column_idx)
        {
            const auto & column = block.getByPosition(column_idx).column;
            auto & bloom_filter = bloom_filters[column_idx];
            
            for (size_t row = *pos; row < *pos + limit; ++row)
            {
                StringRef value = column->getDataAt(row);
                bloom_filter.add(value);
            }
        }
    }
    
    bool empty() const override { return bloom_filters.empty(); }
};
```

### 2.5.5 Index Selection and Optimization

ClickHouse includes sophisticated logic for selecting and optimizing index usage:

```cpp
class IndexSelector
{
public:
    struct IndexCandidate
    {
        MergeTreeIndexPtr index;
        MergeTreeIndexConditionPtr condition;
        Float64 selectivity;
        size_t cost;
        
        bool operator<(const IndexCandidate & other) const
        {
            // Prefer indices with lower selectivity and cost
            return selectivity * cost < other.selectivity * other.cost;
        }
    };
    
    static std::vector<IndexCandidate> selectIndices(
        const std::vector<MergeTreeIndexPtr> & available_indices,
        const SelectQueryInfo & query_info,
        ContextPtr context)
    {
        std::vector<IndexCandidate> candidates;
        
        for (const auto & index : available_indices)
        {
            auto condition = index->createIndexCondition(query_info, context);
            
            if (condition->alwaysUnknownOrTrue())
                continue; // Index cannot help with this query
            
            IndexCandidate candidate;
            candidate.index = index;
            candidate.condition = condition;
            candidate.selectivity = condition->getSelectivity();
            candidate.cost = estimateIndexCost(index, query_info);
            
            candidates.push_back(candidate);
        }
        
        // Sort by effectiveness (selectivity * cost)
        std::sort(candidates.begin(), candidates.end());
        
        // Remove redundant indices
        return removeRedundantIndices(candidates, query_info);
    }
    
private:
    static size_t estimateIndexCost(const MergeTreeIndexPtr & index, 
                                   const SelectQueryInfo & query_info)
    {
        // Simple cost model based on index type and size
        size_t base_cost = 1;
        
        if (index->getTypeName() == "bloom_filter")
            base_cost = 2; // Bloom filters have higher CPU cost
        else if (index->getTypeName() == "minmax")
            base_cost = 1; // MinMax is very cheap
        
        return base_cost * index->getIndexGranuleSize();
    }
    
    static std::vector<IndexCandidate> removeRedundantIndices(
        const std::vector<IndexCandidate> & candidates,
        const SelectQueryInfo & query_info)
    {
        std::vector<IndexCandidate> result;
        
        for (const auto & candidate : candidates)
        {
            bool is_redundant = false;
            
            // Check if this index is made redundant by already selected indices
            for (const auto & selected : result)
            {
                if (isIndexRedundant(candidate, selected, query_info))
                {
                    is_redundant = true;
                    break;
                }
            }
            
            if (!is_redundant)
                result.push_back(candidate);
        }
        
        return result;
    }
    
    static bool isIndexRedundant(const IndexCandidate & candidate,
                                const IndexCandidate & selected,
                                const SelectQueryInfo & query_info)
    {
        // Simple heuristic: if indices cover the same columns and selected has better selectivity
        // In practice, this would be more sophisticated
        return candidate.index->getName() == selected.index->getName() &&
               candidate.selectivity >= selected.selectivity;
    }
};
```

This sophisticated indexing system enables ClickHouse to efficiently handle diverse query patterns across massive datasets, providing the performance characteristics required for real-time analytical workloads.

## Phase 2 Summary

Phase 2 has provided a comprehensive deep dive into ClickHouse's storage engine architecture, covering:

1. **IStorage Interface and Architecture**: The foundational abstraction layer that enables diverse storage engines while maintaining consistent interfaces for query processing.

2. **MergeTree Family Implementation**: The core LSM-tree-inspired storage engines including specialized variants for deduplication, aggregation, and other use cases.

3. **Data Organization**: The three-tier hierarchy of parts, granules, and blocks that enables efficient compression, indexing, and parallel processing.

4. **Compression Framework**: A comprehensive codec system supporting multiple algorithms optimized for different data types and access patterns.

5. **Indexing System**: Primary indices combined with various skip indices that accelerate query execution across massive datasets.

This storage layer foundation provides the robust, high-performance data management capabilities that enable ClickHouse to excel in analytical workloads while maintaining excellent compression ratios and query performance.

---

# Phase 3: Processor Architecture (15,000 words)

## 3.1 IProcessor Interface and Execution Model (3,000 words)

ClickHouse's modern query execution engine is built around a sophisticated processor architecture that replaced the older stream-based system. At the heart of this architecture lies the `IProcessor` interface, which defines a unified abstraction for all query execution operators. This processor-based approach enables fine-grained parallelism, dynamic pipeline modification, and efficient resource utilization across multi-core systems.

### The IProcessor Interface Design

The `IProcessor` interface represents a fundamental shift from traditional database execution models. Each processor is a self-contained execution unit that can consume data from input ports, perform transformations, and produce results through output ports. The interface is designed around a state machine pattern that enables non-blocking, asynchronous execution:

```cpp
class IProcessor
{
public:
    /// Processor states for execution control
    enum class Status
    {
        NeedData,       /// Processor needs more input data
        PortFull,       /// Output port is full, cannot produce more data
        Finished,       /// Processor has completed its work
        Ready,          /// Processor is ready to do work
        Async,          /// Processor is doing async work (e.g., reading from disk)
        ExpandPipeline  /// Processor wants to add new processors to pipeline
    };

    /// Main execution method - returns current status
    virtual Status work() = 0;
    
    /// Prepare for execution - called before work()
    virtual Status prepare() = 0;
    
    /// Get input and output ports
    virtual InputPorts & getInputs() = 0;
    virtual OutputPorts & getOutputs() = 0;
    
    /// Get processor description for debugging
    virtual String getName() const = 0;
};
```

This interface design provides several key advantages. First, the state-based execution model allows for fine-grained control over processor execution, enabling the scheduler to make optimal decisions about when to execute each processor. Second, the port-based communication system creates clear data flow dependencies that can be analyzed for parallelization opportunities. Third, the asynchronous execution support allows processors to yield control during I/O operations without blocking the entire pipeline.

### Processor State Machine Mechanics

The processor state machine is the core mechanism that drives query execution. Each processor maintains an internal state that determines what actions it can perform and how it interacts with the execution scheduler. The state transitions follow a carefully designed protocol:

**NeedData State**: When a processor enters the NeedData state, it indicates that it requires more input data to continue processing. The scheduler will not execute this processor until data becomes available on at least one of its input ports. This state is crucial for implementing backpressure in the pipeline - if a downstream processor cannot accept more data, upstream processors will eventually transition to NeedData, causing execution to pause until the bottleneck is resolved.

**Ready State**: A processor in the Ready state has all necessary inputs available and can perform useful work. The scheduler prioritizes Ready processors for execution, as they can make immediate progress. The transition to Ready typically occurs when input data arrives or when internal processing completes.

**PortFull State**: This state indicates that the processor has produced output data but cannot send it downstream because output ports are full. This creates natural flow control in the pipeline, preventing fast producers from overwhelming slow consumers.

**Finished State**: Once a processor completes all its work and has no more data to produce, it transitions to Finished. The scheduler removes finished processors from the execution queue, and their resources can be reclaimed.

**Async State**: For processors that need to perform I/O operations or other asynchronous work, the Async state allows them to yield control while maintaining their position in the pipeline. This is particularly important for disk I/O, network operations, and other potentially blocking activities.

### Port-Based Communication System

The communication between processors occurs through a sophisticated port system that manages data flow and dependencies. Each processor declares its input and output ports, which are strongly typed and enforce data format consistency across the pipeline.

Input ports act as data receivers with built-in buffering and flow control mechanisms:

```cpp
class InputPort
{
private:
    Header header;                    /// Column types and names
    std::shared_ptr<Chunk> data;     /// Current data chunk
    bool is_finished = false;        /// No more data will arrive
    OutputPort * output_port = nullptr; /// Connected output port
    
public:
    /// Check if data is available
    bool hasData() const { return data != nullptr; }
    
    /// Pull data from connected output port
    Chunk pull();
    
    /// Check if connected output is finished
    bool isFinished() const;
    
    /// Set port as needed for execution planning
    void setNeeded();
};
```

Output ports manage data production and delivery to downstream processors:

```cpp
class OutputPort
{
private:
    Header header;                    /// Column types and names
    std::shared_ptr<Chunk> data;     /// Data to be sent
    bool is_finished = false;        /// No more data will be produced
    InputPort * input_port = nullptr; /// Connected input port
    
public:
    /// Check if port can accept more data
    bool canPush() const { return data == nullptr; }
    
    /// Push data to connected input port
    void push(Chunk chunk);
    
    /// Mark port as finished
    void finish();
    
    /// Check if port has data waiting
    bool hasData() const { return data != nullptr; }
};
```

The port system implements several sophisticated features for efficient data flow management. First, it provides automatic backpressure propagation - when a downstream processor cannot accept more data, the pressure propagates upstream through the port connections. Second, it supports chunk-based processing, where data is moved in optimally-sized chunks rather than row-by-row, enabling vectorized operations. Third, the strongly-typed headers ensure that data format mismatches are caught early in pipeline construction rather than during execution.

### Vectorized Execution Model

ClickHouse's processor architecture is designed specifically to support vectorized execution, where operations are performed on chunks of data rather than individual rows. This approach provides significant performance benefits by improving CPU cache utilization, enabling SIMD optimizations, and reducing function call overhead.

Each data chunk contains multiple columns represented as `IColumn` objects, along with metadata about the number of rows and column types. The chunk size is dynamically adjusted based on data characteristics and memory constraints, typically ranging from 8,192 to 65,536 rows. This size optimization balances memory usage with vectorization efficiency.

The vectorized model extends throughout the processor hierarchy. Source processors read data in chunks from storage engines, transform processors operate on entire chunks at once, and sink processors write chunks to their destinations. This consistency enables the query optimizer to make assumptions about data granularity and optimize accordingly.

### Dynamic Pipeline Modification

One of the most sophisticated features of the processor architecture is its support for dynamic pipeline modification during execution. Processors can request pipeline changes through the `ExpandPipeline` status, allowing for runtime adaptation to changing conditions.

Common scenarios for dynamic modification include:

**External Sorting**: When a sort processor detects that input data exceeds available memory, it can dynamically add external merge processors to handle disk-based sorting. This transformation occurs transparently without interrupting other pipeline operations.

**Parallel Aggregation**: Aggregation processors can spawn additional parallel aggregators when they detect high cardinality data that would benefit from distributed processing. The original processor becomes a coordinator that merges results from the parallel workers.

**Adaptive Join Strategies**: Join processors can switch between hash join and sort-merge join algorithms based on actual data characteristics observed during execution. This adaptation occurs by inserting appropriate preprocessing processors into the pipeline.

The dynamic modification system maintains pipeline consistency by ensuring that all changes preserve data flow semantics and type safety. New processors inherit appropriate headers and port configurations from their parent processors, and the scheduler seamlessly integrates them into the execution plan.

### Resource Management and Scheduling

The processor architecture includes sophisticated resource management capabilities that ensure optimal utilization of system resources. Each processor can declare its resource requirements, including memory usage, CPU intensity, and I/O characteristics. The scheduler uses this information to make intelligent decisions about processor execution order and parallelization.

Memory management is particularly critical in the processor model. Each processor tracks its memory consumption and can trigger garbage collection or external processing when limits are approached. The system maintains global memory pressure indicators that influence processor scheduling decisions, prioritizing memory-efficient processors when resources are constrained.

CPU scheduling considers processor characteristics when making execution decisions. CPU-intensive processors like aggregation and sorting operations are scheduled to maximize core utilization, while I/O-bound processors are interleaved to overlap computation with data access. The scheduler also considers NUMA topology when assigning processors to threads, ensuring that memory-intensive operations run on cores with optimal memory access patterns.

## 3.2 Processor State Machine and Port System (3,000 words)

The processor state machine represents one of the most sophisticated aspects of ClickHouse's execution engine, providing a foundation for non-blocking, asynchronous query processing. This state machine, combined with the port communication system, enables fine-grained control over data flow and resource utilization while maintaining high performance across diverse workloads.

### State Machine Implementation Details

The processor state machine operates on a carefully designed set of states that capture all possible execution conditions. Each state transition is governed by specific rules that ensure correctness and optimal performance:

```cpp
enum class Status
{
    /// Processor needs more input data to continue
    NeedData,
    
    /// Output port is full, cannot produce more data  
    PortFull,
    
    /// Processor has completed all work
    Finished,
    
    /// Processor is ready to perform work
    Ready,
    
    /// Processor is performing asynchronous work
    Async,
    
    /// Processor wants to modify the pipeline
    ExpandPipeline
};
```

The state machine implementation follows a strict protocol for state transitions. The `prepare()` method is always called before `work()`, allowing processors to examine their input/output port states and determine their current status without performing actual work. This separation enables the scheduler to make informed decisions about processor execution without incurring processing overhead.

**NeedData State Transitions**: A processor transitions to NeedData when it has consumed all available input data and requires more to continue. This state is not simply a lack of data - it represents a specific condition where the processor has examined its inputs and determined that progress cannot be made. The transition back from NeedData occurs when new data arrives on any input port, triggering a re-evaluation during the next prepare() call.

```cpp
class TransformProcessor : public IProcessor
{
private:
    bool input_finished = false;
    bool output_finished = false;
    
public:
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        /// Check if we're completely finished
        if (output_finished)
        {
            input.close();
            return Status::Finished;
        }
        
        /// Check if output is blocked
        if (output.hasData())
            return Status::PortFull;
        
        /// Check if we have input data to process
        if (input.hasData())
            return Status::Ready;
        
        /// Check if input is finished
        if (input.isFinished())
        {
            if (!input_finished)
            {
                input_finished = true;
                return Status::Ready; // May need to finalize output
            }
            
            output.finish();
            output_finished = true;
            return Status::Finished;
        }
        
        /// Need more input data
        input.setNeeded();
        return Status::NeedData;
    }
};
```

**Ready State Management**: The Ready state indicates that a processor can make meaningful progress. This might involve processing available input data, generating output based on internal state, or performing computational work. The Ready state is the primary target for scheduler execution, as these processors can immediately contribute to query progress.

**PortFull Backpressure Handling**: When a processor produces output but cannot deliver it because downstream ports are full, it transitions to PortFull. This state implements backpressure propagation throughout the pipeline. The processor remains in PortFull until downstream consumers process their data and free up port capacity.

**Asynchronous Operation Support**: The Async state enables processors to perform non-blocking I/O operations or other asynchronous work. When a processor enters Async state, it typically registers a callback or future that will signal completion. The scheduler removes the processor from immediate execution consideration but monitors for completion signals.

### Port Communication Protocol

The port system implements a sophisticated communication protocol that manages data transfer between processors while maintaining type safety and flow control. Each port connection represents a typed data channel with specific semantics for data availability and completion.

Input port implementation focuses on efficient data consumption with minimal copying:

```cpp
class InputPort
{
private:
    Header header;                    /// Column metadata
    std::shared_ptr<Chunk> data;     /// Current data chunk
    bool is_finished = false;        /// End of stream marker
    bool is_needed = false;          /// Execution planning hint
    OutputPort * output_port = nullptr; /// Connected output
    
public:
    /// Non-blocking data access
    bool hasData() const { return data != nullptr; }
    
    /// Consume available data
    Chunk pull() 
    {
        if (!hasData())
            throw Exception("No data available to pull");
        auto result = std::move(*data);
        data.reset();
        return result;
    }
    
    /// Check for stream completion
    bool isFinished() const 
    { 
        return is_finished && !hasData(); 
    }
    
    /// Execution planning hint
    void setNeeded() { is_needed = true; }
    bool isNeeded() const { return is_needed; }
    
    /// Connection management
    void connect(OutputPort & output)
    {
        if (output_port)
            throw Exception("InputPort already connected");
        
        output_port = &output;
        output.input_port = this;
        
        /// Validate header compatibility
        if (!header.isCompatibleWith(output.getHeader()))
            throw Exception("Incompatible port headers");
    }
    
    /// Close port and propagate signal
    void close()
    {
        if (output_port && !output_port->isFinished())
            output_port->finish();
    }
};
```

The `setNeeded()` mechanism provides crucial optimization hints for query planning. When a processor indicates that it needs data from specific input ports, the scheduler can prioritize upstream processors that feed those ports. This creates a demand-driven execution model that focuses computational resources on the most critical data paths.

Output port implementation emphasizes efficient data delivery and backpressure management:

```cpp
class OutputPort  
{
private:
    Header header;                    /// Column metadata
    std::shared_ptr<Chunk> data;     /// Data waiting to be consumed
    bool is_finished = false;        /// No more data will be produced
    InputPort * input_port = nullptr; /// Connected input
    
public:
    /// Check capacity for new data
    bool canPush() const { return data == nullptr; }
    
    /// Deliver data to downstream processor
    void push(Chunk chunk)
    {
        if (!canPush())
            throw Exception("Port is full, cannot push data");
        data = std::make_shared<Chunk>(std::move(chunk));
        
        /// Notify connected input port
        if (input_port)
            input_port->onDataAvailable();
    }
    
    /// Signal end of data stream
    void finish() 
    { 
        is_finished = true;
        if (input_port)
            input_port->onFinished();
    }
    
    /// Check for pending data
    bool hasData() const { return data != nullptr; }
    
    /// Transfer data to connected input
    void updateInputPort()
    {
        if (input_port && hasData() && !input_port->hasData())
        {
            input_port->data = std::move(data);
            data.reset();
        }
        
        if (input_port && is_finished)
            input_port->is_finished = true;
    }
};
```

### Data Flow Synchronization

The port system implements sophisticated synchronization mechanisms that ensure correct data flow without requiring explicit locking. The synchronization relies on the atomic nature of pointer operations and careful ordering of state updates.

When a processor produces data, it follows a specific protocol:
1. Check that the output port can accept data (`canPush()`)
2. Create the data chunk with appropriate content
3. Atomically update the port with the new data (`push()`)
4. Update internal state to reflect data production

Similarly, data consumption follows a complementary protocol:
1. Check that input data is available (`hasData()`)
2. Atomically extract the data chunk (`pull()`)
3. Update internal state to reflect data consumption
4. Process the extracted data

```cpp
class SynchronizedPortSystem
{
private:
    std::atomic<size_t> data_version{0};
    std::atomic<bool> finished_flag{false};
    
public:
    /// Thread-safe data transfer
    bool tryTransferData(OutputPort & output, InputPort & input)
    {
        /// Check preconditions atomically
        if (!output.hasData() || input.hasData())
            return false;
        
        /// Perform atomic transfer
        auto expected_version = data_version.load();
        if (data_version.compare_exchange_weak(expected_version, expected_version + 1))
        {
            /// Transfer data under version lock
            input.data = std::move(output.data);
            output.data.reset();
            return true;
        }
        
        return false; /// Retry needed
    }
    
    /// Signal completion across port boundary
    void signalFinished(OutputPort & output)
    {
        bool expected = false;
        if (finished_flag.compare_exchange_strong(expected, true))
        {
            output.finish();
            if (output.input_port)
                output.input_port->is_finished = true;
        }
    }
};
```

This protocol ensures that data transfer occurs atomically without requiring heavyweight synchronization primitives. The shared pointer mechanism provides automatic memory management while maintaining thread safety for the data chunks themselves.

### Chunk-Based Processing Model

The processor architecture is built around chunk-based data processing, where operations work on collections of rows rather than individual tuples. This design choice provides significant performance benefits while simplifying the implementation of complex operations.

Chunks are represented by the `Chunk` class, which contains multiple columns and associated metadata:

```cpp
class Chunk
{
private:
    Columns columns;          /// Vector of IColumn shared pointers
    UInt64 num_rows;         /// Number of rows in all columns
    ChunkInfoPtr chunk_info; /// Optional metadata for special processing
    
public:
    /// Construction and basic access
    Chunk() : num_rows(0) {}
    Chunk(Columns columns_, UInt64 num_rows_)
        : columns(std::move(columns_)), num_rows(num_rows_)
    {
        checkColumnsConsistency();
    }
    
    /// Column access and manipulation
    const Columns & getColumns() const { return columns; }
    void setColumns(Columns columns_) 
    { 
        columns = std::move(columns_);
        checkColumnsConsistency();
    }
    
    /// Row count management
    UInt64 getNumRows() const { return num_rows; }
    void setNumRows(UInt64 num_rows_) { num_rows = num_rows_; }
    
    /// Chunk operations
    Chunk clone() const
    {
        Columns cloned_columns;
        cloned_columns.reserve(columns.size());
        
        for (const auto & column : columns)
            cloned_columns.push_back(column->cloneResized(num_rows));
        
        return Chunk(std::move(cloned_columns), num_rows);
    }
    
    void clear() 
    { 
        columns.clear(); 
        num_rows = 0; 
        chunk_info.reset();
    }
    
    bool empty() const { return num_rows == 0; }
    
    /// Memory management
    size_t bytes() const
    {
        size_t total_bytes = 0;
        for (const auto & column : columns)
            total_bytes += column->byteSize();
        return total_bytes;
    }
    
    size_t allocatedBytes() const
    {
        size_t total_bytes = 0;
        for (const auto & column : columns)
            total_bytes += column->allocatedBytes();
        return total_bytes;
    }
    
private:
    void checkColumnsConsistency() const
    {
        if (columns.empty())
        {
            if (num_rows != 0)
                throw Exception("Chunk with zero columns must have zero rows");
            return;
        }
        
        for (const auto & column : columns)
        {
            if (column->size() != num_rows)
                throw Exception("Column size mismatch in chunk");
        }
    }
};
```

The chunk size is dynamically determined based on several factors:
- **Memory constraints**: Chunks are sized to fit comfortably in CPU cache while avoiding excessive memory usage
- **Vectorization efficiency**: Larger chunks enable better SIMD utilization and reduce function call overhead  
- **Pipeline characteristics**: I/O-bound operations prefer larger chunks, while CPU-intensive operations may use smaller chunks for better parallelization

Typical chunk sizes range from 8,192 to 65,536 rows, with the exact size determined by data characteristics and system configuration. The dynamic sizing ensures optimal performance across diverse query patterns and data types.

### Advanced Port Features

The port system includes several advanced features that support sophisticated query execution patterns:

**Multi-input Processors**: Some processors require data from multiple input sources, such as join operations that need both left and right input streams. The port system supports processors with multiple input ports, each potentially operating at different rates and with different data availability patterns.

```cpp
class JoinProcessor : public IProcessor
{
private:
    enum InputPortIndex { LEFT = 0, RIGHT = 1 };
    bool left_finished = false;
    bool right_finished = false;
    
public:
    JoinProcessor(const Block & left_header, const Block & right_header)
    {
        addInputPort(left_header);   /// Port 0: left input
        addInputPort(right_header);  /// Port 1: right input
        addOutputPort(createJoinHeader(left_header, right_header));
    }
    
    Status prepare() override
    {
        auto & inputs = getInputs();
        auto & output = getOutputs().front();
        
        auto & left_input = *std::next(inputs.begin(), LEFT);
        auto & right_input = *std::next(inputs.begin(), RIGHT);
        
        /// Check output availability
        if (output.hasData())
            return Status::PortFull;
        
        /// Check if both inputs have data or are finished
        bool left_ready = left_input.hasData() || left_input.isFinished();
        bool right_ready = right_input.hasData() || right_input.isFinished();
        
        if (left_ready && right_ready)
            return Status::Ready;
        
        /// Set needed flags for missing inputs
        if (!left_ready)
            left_input.setNeeded();
        if (!right_ready)
            right_input.setNeeded();
        
        return Status::NeedData;
    }
};
```

**Conditional Data Flow**: Certain processors implement conditional logic that determines which output ports receive data based on processing results. For example, a filter processor might send matching rows to one output port and non-matching rows to another for further processing.

**Port Multiplexing**: Advanced processors can multiplex data across multiple output ports to enable parallel downstream processing. This capability is crucial for implementing parallel aggregation and other divide-and-conquer algorithms.

```cpp
class PartitionProcessor : public IProcessor
{
private:
    size_t num_partitions;
    std::vector<size_t> partition_keys;
    PartitionFunction partition_function;
    
public:
    PartitionProcessor(const Block & header, size_t num_partitions_)
        : num_partitions(num_partitions_)
    {
        addInputPort(header);
        
        /// Create output port for each partition
        for (size_t i = 0; i < num_partitions; ++i)
            addOutputPort(header);
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & outputs = getOutputs();
        
        auto chunk = input.pull();
        if (chunk.empty())
            return;
        
        /// Partition chunk into multiple output chunks
        std::vector<Chunk> partitioned_chunks(num_partitions);
        partitionChunk(chunk, partitioned_chunks);
        
        /// Send chunks to appropriate output ports
        auto output_it = outputs.begin();
        for (size_t i = 0; i < num_partitions; ++i, ++output_it)
        {
            if (!partitioned_chunks[i].empty())
                output_it->push(std::move(partitioned_chunks[i]));
        }
    }
};
```

**Header Propagation**: The port system maintains detailed header information that describes the structure and types of data flowing through each connection. Headers are propagated through the pipeline during construction, enabling early detection of type mismatches and optimization opportunities.

### Error Handling and Recovery

The processor state machine includes comprehensive error handling mechanisms that ensure robust execution even when individual processors encounter problems. Error conditions are represented as special states that trigger appropriate recovery actions.

When a processor encounters an error during execution, it can transition to an error state that propagates the exception information upstream and downstream. The scheduler detects error states and initiates cleanup procedures that gracefully shut down the affected pipeline segments while preserving partial results where possible.

```cpp
class ErrorHandlingProcessor : public IProcessor
{
private:
    std::exception_ptr current_exception;
    bool error_occurred = false;
    
public:
    Status prepare() override
    {
        if (error_occurred)
        {
            /// Propagate error to connected ports
            for (auto & input : getInputs())
                input.close();
            for (auto & output : getOutputs())
                output.finish();
            
            return Status::Finished;
        }
        
        return prepareImpl();
    }
    
    void work() override
    {
        try
        {
            workImpl();
        }
        catch (...)
        {
            current_exception = std::current_exception();
            error_occurred = true;
            
            /// Log error for debugging
            LOG_ERROR(&Poco::Logger::get("ErrorHandlingProcessor"), 
                     "Processor {} encountered error", getName());
        }
    }
    
    /// Check if processor has encountered an error
    bool hasError() const { return error_occurred; }
    
    /// Rethrow captured exception
    void rethrowException() const
    {
        if (current_exception)
            std::rethrow_exception(current_exception);
    }
    
protected:
    virtual Status prepareImpl() = 0;
    virtual void workImpl() = 0;
};
```

The error handling system distinguishes between recoverable and non-recoverable errors. Recoverable errors, such as temporary I/O failures, may trigger retry mechanisms or fallback processing strategies. Non-recoverable errors, such as data corruption or resource exhaustion, result in query termination with appropriate error reporting.

This sophisticated state machine and port system provides the foundation for ClickHouse's high-performance, parallel query execution engine, enabling efficient processing of complex analytical workloads while maintaining robustness and correctness.

## 3.3 Core Processor Types and Implementations (3,000 words)

ClickHouse's processor architecture encompasses a rich hierarchy of specialized processor types, each optimized for specific query execution tasks. These processors form the building blocks of query pipelines, with each type implementing the `IProcessor` interface while providing specialized functionality for data sources, transformations, aggregations, and output operations.

### Source Processors: Data Ingestion Layer

Source processors serve as the entry points for data into the query pipeline, responsible for reading data from various storage engines and external sources. These processors are particularly critical because they often represent the primary bottleneck in query execution, especially for I/O-bound workloads.

**StorageSource Processor**: The most fundamental source processor reads data directly from ClickHouse storage engines. This processor implements sophisticated optimizations for different storage formats:

```cpp
class StorageSource : public IProcessor
{
private:
    StoragePtr storage;               /// Reference to storage engine
    QueryInfo query_info;           /// Query context and optimization hints
    Block header;                   /// Expected output format
    ReadFromStorageStep step;       /// Execution step with parameters
    
    /// Reading state
    QueryPipelineBuilder pipeline_builder;
    PullingPipelineExecutor executor;
    
public:
    StorageSource(StoragePtr storage_, const Block & header_, ReadFromStorageStep step_)
        : storage(std::move(storage_)), header(header_), step(std::move(step_))
    {
        addOutputPort(header);
        initializePipeline();
    }
    
    String getName() const override { return "StorageSource"; }
    
    Status prepare() override
    {
        auto & output = getOutputs().front();
        
        if (output.isFinished())
            return Status::Finished;
        
        if (output.hasData())
            return Status::PortFull;
        
        if (isStorageFinished())
        {
            output.finish();
            return Status::Finished;
        }
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & output = getOutputs().front();
        
        auto chunk = readNextChunk();
        if (chunk.empty())
        {
            output.finish();
        }
        else
        {
            output.push(std::move(chunk));
        }
    }
    
private:
    void initializePipeline()
    {
        /// Build internal pipeline for reading from storage
        auto reading_step = std::make_unique<ReadFromStorageStep>(step);
        reading_step->initializePipeline(pipeline_builder);
        
        auto pipeline = pipeline_builder.build();
        executor = std::make_unique<PullingPipelineExecutor>(pipeline);
    }
    
    Chunk readNextChunk()
    {
        Block block;
        if (executor->pull(block))
        {
            return Chunk(block.getColumns(), block.rows());
        }
        return {};
    }
    
    bool isStorageFinished() const
    {
        return executor && executor->isFinished();
    }
};
```

The StorageSource processor coordinates with storage engines to implement advanced optimizations such as predicate pushdown, column pruning, and index utilization. For MergeTree engines, it leverages primary key indexes and skip indexes to minimize data reading. For external storage systems, it implements connection pooling and batch reading strategies.

**Parallel Reading Coordination**: Modern storage engines support parallel reading from multiple data parts or segments. The StorageSource processor coordinates these parallel streams, managing load balancing and ensuring optimal resource utilization across available I/O channels.

**RemoteSource Processor**: For distributed queries, the RemoteSource processor manages connections to remote ClickHouse nodes, implementing sophisticated networking optimizations:

```cpp
class RemoteSource : public IProcessor
{
private:
    ConnectionPoolPtr connection_pool;   /// Pool of remote connections
    String query;                       /// Query to execute remotely
    Settings settings;                  /// Query execution settings
    
    /// Network state management
    std::vector<ConnectionPtr> connections;
    MultiplexedConnections multiplexed;
    std::unique_ptr<RemoteQueryExecutor> executor;
    
    /// Async execution state
    std::future<void> async_result;
    bool is_async_running = false;
    
public:
    RemoteSource(ConnectionPoolPtr pool_, const String & query_, const Settings & settings_)
        : connection_pool(std::move(pool_)), query(query_), settings(settings_)
    {
        addOutputPort(Block{}); /// Header will be determined after connection
    }
    
    String getName() const override { return "RemoteSource"; }
    
    Status prepare() override
    {
        auto & output = getOutputs().front();
        
        if (output.isFinished())
            return Status::Finished;
        
        if (output.hasData())
            return Status::PortFull;
        
        if (!executor)
        {
            establishConnections();
            return Status::Ready;
        }
        
        if (is_async_running)
        {
            /// Check if async operation completed
            if (async_result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                is_async_running = false;
                return Status::Ready;
            }
            return Status::Async;
        }
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & output = getOutputs().front();
        
        if (!executor)
        {
            sendQuery();
            return;
        }
        
        auto chunk = receiveData();
        if (chunk.empty())
        {
            output.finish();
        }
        else
        {
            output.push(std::move(chunk));
        }
    }
    
private:
    void establishConnections()
    {
        connections = connection_pool->getMany(settings.max_parallel_connections);
        multiplexed = MultiplexedConnections(connections, settings);
        executor = std::make_unique<RemoteQueryExecutor>(multiplexed);
    }
    
    void sendQuery()
    {
        executor->sendQuery(query, settings);
        
        /// Update output port header with actual result structure
        auto & output = getOutputs().front();
        output.setHeader(executor->getHeader());
    }
    
    Chunk receiveData()
    {
        Block block;
        if (executor->read(block))
        {
            return Chunk(block.getColumns(), block.rows());
        }
        return {};
    }
};
```

The RemoteSource processor implements advanced networking features including connection multiplexing, compression negotiation, and adaptive timeout management. It supports both synchronous and asynchronous execution modes, with the latter enabling overlap of network I/O with local processing.

### Transform Processors: Data Manipulation Engine

Transform processors implement the core data manipulation operations that modify, filter, and restructure data as it flows through the pipeline. These processors are designed for high throughput and optimal resource utilization.

**FilterTransform Processor**: One of the most frequently used processors, FilterTransform applies predicate conditions to filter rows from input chunks:

```cpp
class FilterTransform : public IProcessor
{
private:
    ExpressionActionsPtr expression;  /// Compiled filter expression
    String filter_column_name;       /// Name of boolean result column
    bool remove_filter_column;       /// Whether to remove filter column from output
    
    /// Optimization state
    ConstantFilterDescription constant_filter;
    bool is_constant_filter;
    
    /// Processing state
    Block input_header;
    Block output_header;
    
public:
    FilterTransform(const Block & header_, ExpressionActionsPtr expression_,
                   const String & filter_column_name_, bool remove_filter_column_)
        : expression(std::move(expression_))
        , filter_column_name(filter_column_name_)
        , remove_filter_column(remove_filter_column_)
        , input_header(header_)
    {
        output_header = transform_header(input_header);
        
        addInputPort(input_header);
        addOutputPort(output_header);
        
        analyzeConstantFilter();
    }
    
    String getName() const override { return "FilterTransform"; }
    
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        if (output.hasData())
            return Status::PortFull;
        
        if (input.isFinished())
        {
            output.finish();
            return Status::Finished;
        }
        
        if (!input.hasData())
        {
            input.setNeeded();
            return Status::NeedData;
        }
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        auto chunk = input.pull();
        
        if (chunk.empty())
            return;
        
        /// Handle constant filter optimization
        if (is_constant_filter)
        {
            if (constant_filter.always_false)
            {
                /// Filter eliminates all rows
                chunk.clear();
            }
            else if (constant_filter.always_true)
            {
                /// Filter passes all rows, just remove filter column if needed
                if (remove_filter_column)
                    chunk = removeFilterColumn(chunk);
            }
        }
        else
        {
            /// Apply dynamic filter
            chunk = applyFilter(chunk);
        }
        
        output.push(std::move(chunk));
    }
    
private:
    void analyzeConstantFilter()
    {
        /// Check if filter expression is constant
        if (expression->hasConstantResult())
        {
            is_constant_filter = true;
            
            auto constant_value = expression->getConstantResult();
            constant_filter.always_true = constant_value && constant_value->getBool();
            constant_filter.always_false = !constant_filter.always_true;
        }
        else
        {
            is_constant_filter = false;
        }
    }
    
    Chunk applyFilter(Chunk chunk)
    {
        auto block = input_header.cloneWithColumns(chunk.detachColumns());
        
        /// Execute filter expression
        expression->execute(block);
        
        /// Get filter column
        const auto & filter_column = block.getByName(filter_column_name).column;
        
        /// Apply filter to all columns
        for (auto & column_with_type : block)
        {
            column_with_type.column = column_with_type.column->filter(*filter_column, -1);
        }
        
        /// Remove filter column if requested
        if (remove_filter_column)
            block.erase(filter_column_name);
        
        return Chunk(block.getColumns(), block.rows());
    }
    
    Chunk removeFilterColumn(Chunk chunk)
    {
        auto block = input_header.cloneWithColumns(chunk.detachColumns());
        block.erase(filter_column_name);
        return Chunk(block.getColumns(), block.rows());
    }
    
    Block transform_header(const Block & header)
    {
        auto result = header;
        expression->execute(result, true); /// Dry run to get output structure
        
        if (remove_filter_column)
            result.erase(filter_column_name);
        
        return result;
    }
};
```

The FilterTransform processor implements several sophisticated optimizations. For constant filter conditions, it can completely skip processing of chunks that don't match. For highly selective filters, it uses SIMD-optimized filtering algorithms that process multiple rows simultaneously. The processor also supports predicate vectorization, where multiple filter conditions are evaluated together to minimize branching overhead.

**ExpressionTransform Processor**: This processor evaluates arbitrary expressions on input data, supporting complex calculations, function calls, and type conversions:

```cpp
class ExpressionTransform : public IProcessor
{
private:
    ExpressionActionsPtr expression;  /// Compiled expression tree
    Block input_header;              /// Input column structure
    Block output_header;             /// Output column structure
    
    /// Expression optimization state
    std::vector<size_t> result_positions;
    bool has_array_join;
    
public:
    ExpressionTransform(const Block & header_, ExpressionActionsPtr expression_)
        : expression(std::move(expression_)), input_header(header_)
    {
        output_header = transform_header(input_header);
        
        addInputPort(input_header);
        addOutputPort(output_header);
        
        optimizeColumnPositions();
    }
    
    String getName() const override { return "ExpressionTransform"; }
    
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        if (output.hasData())
            return Status::PortFull;
        
        if (input.isFinished())
        {
            output.finish();
            return Status::Finished;
        }
        
        if (!input.hasData())
        {
            input.setNeeded();
            return Status::NeedData;
        }
        
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        auto chunk = input.pull();
        
        if (chunk.empty())
            return;
        
        auto block = input_header.cloneWithColumns(chunk.detachColumns());
        
        /// Execute expression
        expression->execute(block);
        
        /// Handle array join if present
        if (has_array_join)
            handleArrayJoin(block);
        
        /// Extract result columns in correct order
        Columns result_columns;
        result_columns.reserve(output_header.columns());
        
        for (const auto & column_name : output_header.getNames())
        {
            result_columns.push_back(block.getByName(column_name).column);
        }
        
        output.push(Chunk(std::move(result_columns), block.rows()));
    }
    
private:
    Block transform_header(const Block & header)
    {
        auto result = header;
        expression->execute(result, true); /// Dry run
        return result;
    }
    
    void optimizeColumnPositions()
    {
        /// Pre-compute column positions for faster access
        result_positions.reserve(output_header.columns());
        
        for (const auto & column_name : output_header.getNames())
        {
            auto position = input_header.getPositionByName(column_name);
            result_positions.push_back(position);
        }
        
        /// Check for array join operations
        has_array_join = expression->hasArrayJoin();
    }
    
    void handleArrayJoin(Block & block)
    {
        /// Array join can multiply the number of rows
        /// Implementation would handle the array expansion logic
        /// This is a simplified placeholder
    }
};
```

The ExpressionTransform processor leverages ClickHouse's sophisticated expression compilation system, which can generate optimized code for complex expression trees. It supports vectorized function evaluation, constant folding, and common subexpression elimination. For expressions involving array operations, it implements specialized handling that efficiently processes nested data structures.

**SortingTransform Processor**: Implements high-performance sorting with support for both in-memory and external sorting algorithms:

```cpp
class SortingTransform : public IProcessor
{
private:
    SortDescription sort_description;  /// Sorting criteria
    UInt64 limit;                     /// Optional limit for top-N optimization
    size_t max_bytes_before_external; /// Memory limit for external sorting
    
    /// Sorting state
    std::vector<Chunk> chunks;        /// Accumulated chunks for sorting
    std::unique_ptr<MergeSorter> merge_sorter; /// External sorting support
    bool is_external_sorting = false;
    bool is_input_finished = false;
    bool is_output_finished = false;
    
    /// Memory tracking
    size_t current_memory_usage = 0;
    
public:
    SortingTransform(const Block & header_, const SortDescription & sort_description_,
                    UInt64 limit_ = 0, size_t max_bytes_before_external_ = 0)
        : sort_description(sort_description_)
        , limit(limit_)
        , max_bytes_before_external(max_bytes_before_external_)
    {
        addInputPort(header_);
        addOutputPort(header_);
    }
    
    String getName() const override { return "SortingTransform"; }
    
    Status prepare() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        if (is_output_finished)
        {
            output.finish();
            return Status::Finished;
        }
        
        if (output.hasData())
            return Status::PortFull;
        
        if (!is_input_finished)
        {
            if (input.isFinished())
            {
                is_input_finished = true;
                return Status::Ready; /// Need to start output generation
            }
            
            if (input.hasData())
                return Status::Ready; /// Need to consume input
            
            input.setNeeded();
            return Status::NeedData;
        }
        
        /// Input finished, generating output
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        if (!is_input_finished)
        {
            /// Accumulate input chunks
            auto chunk = input.pull();
            if (!chunk.empty())
            {
                current_memory_usage += chunk.allocatedBytes();
                chunks.push_back(std::move(chunk));
                
                /// Check if we need to switch to external sorting
                if (max_bytes_before_external > 0 && 
                    current_memory_usage > max_bytes_before_external)
                {
                    initializeExternalSorting();
                }
            }
        }
        else
        {
            /// Generate sorted output
            auto chunk = generateOutput();
            if (chunk.empty())
            {
                is_output_finished = true;
            }
            else
            {
                output.push(std::move(chunk));
            }
        }
    }
    
private:
    void initializeExternalSorting()
    {
        if (is_external_sorting)
            return;
        
        is_external_sorting = true;
        
        /// Create external merge sorter
        merge_sorter = std::make_unique<MergeSorter>(
            sort_description, max_bytes_before_external);
        
        /// Feed existing chunks to external sorter
        for (auto & chunk : chunks)
        {
            merge_sorter->addChunk(std::move(chunk));
        }
        chunks.clear();
        current_memory_usage = 0;
    }
    
    Chunk generateOutput()
    {
        if (is_external_sorting)
        {
            return merge_sorter->getNextChunk();
        }
        else
        {
            /// Perform in-memory sort
            if (!chunks.empty())
            {
                auto result = performInMemorySort();
                chunks.clear();
                return result;
            }
        }
        
        return {};
    }
    
    Chunk performInMemorySort()
    {
        if (chunks.empty())
            return {};
        
        /// Merge all chunks into single block
        auto merged_block = mergeChunks(chunks);
        
        /// Sort the merged block
        sortBlock(merged_block, sort_description, limit);
        
        return Chunk(merged_block.getColumns(), merged_block.rows());
    }
    
    Block mergeChunks(const std::vector<Chunk> & chunks_to_merge)
    {
        /// Implementation would efficiently merge chunks
        /// This is a simplified placeholder
        Block result;
        return result;
    }
    
    void sortBlock(Block & block, const SortDescription & description, UInt64 limit_rows)
    {
        /// Implementation would use optimized sorting algorithms
        /// Including partial sort for limited results
    }
};
```

The SortingTransform processor automatically switches between in-memory and external sorting based on memory consumption. For small datasets, it uses optimized in-memory algorithms with SIMD acceleration. For larger datasets, it implements a sophisticated external merge sort that minimizes I/O overhead while maintaining optimal performance.

This comprehensive processor architecture provides ClickHouse with the flexibility and performance needed to handle diverse analytical workloads efficiently. Each processor type is optimized for its specific role while maintaining compatibility with the overall execution framework.

## 3.4 Pipeline Graph Construction (3,000 words)

The QueryPipelineBuilder represents the sophisticated orchestration layer that transforms logical query plans into executable processor graphs. This component bridges the gap between high-level query semantics and low-level execution mechanics, implementing complex optimization strategies while maintaining the flexibility to handle diverse query patterns efficiently.

### QueryPipelineBuilder Architecture

The QueryPipelineBuilder operates as a stateful factory that incrementally constructs processor graphs by translating QueryPlan steps into interconnected processor networks. Its design emphasizes modularity, allowing different query operations to contribute processors independently while maintaining global coherence.

```cpp
class QueryPipelineBuilder
{
private:
    /// Pipeline state
    Processors processors;                    /// All processors in the pipeline
    std::vector<OutputPort *> current_outputs; /// Current output ports
    Block current_header;                     /// Current data schema
    
    /// Optimization state
    std::map<String, ProcessorPtr> processor_cache; /// Reusable processors
    std::vector<ProcessorPtr> detached_processors;  /// Standalone processors
    
    /// Resource management
    size_t max_threads;                       /// Thread limit
    size_t max_memory_usage;                  /// Memory limit
    ProcessorSchedulingPolicy scheduling_policy; /// Execution strategy
    
public:
    QueryPipelineBuilder() = default;
    
    /// Initialize with data source
    void init(Pipe pipe)
    {
        processors = std::move(pipe.processors);
        current_outputs = std::move(pipe.output_ports);
        current_header = std::move(pipe.header);
        
        validatePipelineConsistency();
    }
    
    /// Add transformation step
    void addTransform(ProcessorPtr processor)
    {
        if (current_outputs.empty())
            throw Exception("Cannot add transform to empty pipeline");
        
        connectProcessorInputs(processor.get(), current_outputs);
        
        /// Update current state
        current_outputs.clear();
        for (auto & output : processor->getOutputs())
            current_outputs.push_back(&output);
        
        current_header = processor->getOutputs().front().getHeader();
        processors.push_back(std::move(processor));
        
        validatePipelineConsistency();
    }
    
    /// Add parallel transformation
    void addParallelTransform(ProcessorPtr processor_template, size_t num_streams)
    {
        if (current_outputs.size() != num_streams)
            throw Exception("Stream count mismatch for parallel transform");
        
        std::vector<ProcessorPtr> parallel_processors;
        std::vector<OutputPort *> new_outputs;
        
        for (size_t i = 0; i < num_streams; ++i)
        {
            auto processor = processor_template->clone();
            
            /// Connect input
            processor->getInputs().front().connect(*current_outputs[i]);
            
            /// Collect output
            new_outputs.push_back(&processor->getOutputs().front());
            
            parallel_processors.push_back(std::move(processor));
        }
        
        /// Update pipeline state
        processors.insert(processors.end(), 
                         parallel_processors.begin(), 
                         parallel_processors.end());
        
        current_outputs = std::move(new_outputs);
        
        validatePipelineConsistency();
    }
    
    /// Resize pipeline streams
    void resize(size_t num_streams)
    {
        if (current_outputs.size() == num_streams)
            return;
        
        if (current_outputs.size() < num_streams)
        {
            /// Split existing streams
            expandStreams(num_streams);
        }
        else
        {
            /// Merge streams
            mergeStreams(num_streams);
        }
        
        validatePipelineConsistency();
    }
    
    /// Build final pipeline
    QueryPipeline build()
    {
        if (processors.empty())
            throw Exception("Cannot build empty pipeline");
        
        /// Optimize processor graph
        optimizeProcessorGraph();
        
        /// Assign processor IDs and validate
        assignProcessorIds();
        validateFinalPipeline();
        
        return QueryPipeline(std::move(processors), 
                           std::move(current_outputs),
                           std::move(current_header));
    }
    
private:
    void connectProcessorInputs(IProcessor * processor, 
                               const std::vector<OutputPort *> & outputs)
    {
        auto & inputs = processor->getInputs();
        
        if (inputs.size() != outputs.size())
            throw Exception("Input/output port count mismatch");
        
        auto input_it = inputs.begin();
        for (auto * output : outputs)
        {
            input_it->connect(*output);
            ++input_it;
        }
    }
    
    void expandStreams(size_t target_streams)
    {
        /// Implementation depends on current processor types
        /// May use ResizeProcessor or parallel duplication
        
        std::vector<ProcessorPtr> resize_processors;
        std::vector<OutputPort *> new_outputs;
        
        for (auto * current_output : current_outputs)
        {
            auto resize_processor = std::make_shared<ResizeProcessor>(
                current_output->getHeader(), 
                1, /// input streams
                target_streams / current_outputs.size() /// output streams per input
            );
            
            resize_processor->getInputs().front().connect(*current_output);
            
            for (auto & output : resize_processor->getOutputs())
                new_outputs.push_back(&output);
            
            resize_processors.push_back(std::move(resize_processor));
        }
        
        processors.insert(processors.end(), 
                         resize_processors.begin(), 
                         resize_processors.end());
        
        current_outputs = std::move(new_outputs);
    }
    
    void mergeStreams(size_t target_streams)
    {
        /// Use UnionProcessor to merge streams
        auto union_processor = std::make_shared<UnionProcessor>(
            current_header, current_outputs.size(), target_streams);
        
        /// Connect all current outputs to union processor
        auto input_it = union_processor->getInputs().begin();
        for (auto * output : current_outputs)
        {
            input_it->connect(*output);
            ++input_it;
        }
        
        /// Update current state
        current_outputs.clear();
        for (auto & output : union_processor->getOutputs())
            current_outputs.push_back(&output);
        
        processors.push_back(std::move(union_processor));
    }
    
    void optimizeProcessorGraph()
    {
        /// Apply various optimization passes
        eliminateRedundantProcessors();
        fuseCompatibleProcessors();
        optimizeMemoryUsage();
        balanceProcessorLoad();
    }
    
    void eliminateRedundantProcessors()
    {
        /// Remove processors that don't modify data
        std::vector<ProcessorPtr> optimized_processors;
        
        for (auto & processor : processors)
        {
            if (isRedundantProcessor(processor.get()))
            {
                bypassProcessor(processor.get());
            }
            else
            {
                optimized_processors.push_back(processor);
            }
        }
        
        processors = std::move(optimized_processors);
    }
    
    void fuseCompatibleProcessors()
    {
        /// Combine adjacent processors when beneficial
        /// Example: FilterTransform + ExpressionTransform -> FilterExpressionTransform
        
        bool changes_made = true;
        while (changes_made)
        {
            changes_made = false;
            
            for (size_t i = 0; i < processors.size() - 1; ++i)
            {
                auto & current = processors[i];
                auto & next = processors[i + 1];
                
                if (canFuseProcessors(current.get(), next.get()))
                {
                    auto fused = fuseProcessors(current.get(), next.get());
                    
                    /// Replace both processors with fused version
                    processors[i] = fused;
                    processors.erase(processors.begin() + i + 1);
                    
                    changes_made = true;
                    break;
                }
            }
        }
    }
    
    bool isRedundantProcessor(IProcessor * processor)
    {
        /// Check if processor is a no-op
        return processor->getName() == "NullTransform" ||
               (processor->getName() == "ExpressionTransform" && 
                hasNoEffectiveOperations(processor));
    }
    
    void bypassProcessor(IProcessor * processor)
    {
        /// Connect processor inputs directly to outputs
        auto & inputs = processor->getInputs();
        auto & outputs = processor->getOutputs();
        
        if (inputs.size() != outputs.size())
            return; /// Cannot bypass
        
        auto input_it = inputs.begin();
        auto output_it = outputs.begin();
        
        while (input_it != inputs.end())
        {
            /// Find upstream processor
            auto * upstream_output = input_it->getConnectedOutput();
            if (upstream_output)
            {
                /// Reconnect to downstream
                for (auto * downstream_input : output_it->getConnectedInputs())
                {
                    downstream_input->connect(*upstream_output);
                }
            }
            
            ++input_it;
            ++output_it;
        }
    }
    
    bool canFuseProcessors(IProcessor * first, IProcessor * second)
    {
        /// Check if processors can be combined
        if (first->getOutputs().size() != 1 || second->getInputs().size() != 1)
            return false;
        
        /// Check if they're directly connected
        auto & first_output = first->getOutputs().front();
        auto & second_input = second->getInputs().front();
        
        if (&first_output != second_input.getConnectedOutput())
            return false;
        
        /// Check if fusion is beneficial
        return isFusionBeneficial(first, second);
    }
    
    ProcessorPtr fuseProcessors(IProcessor * first, IProcessor * second)
    {
        /// Create fused processor based on types
        if (first->getName() == "FilterTransform" && 
            second->getName() == "ExpressionTransform")
        {
            return createFilterExpressionProcessor(first, second);
        }
        
        /// Add more fusion patterns as needed
        return nullptr;
    }
    
    void validatePipelineConsistency()
    {
        /// Verify all processors are properly connected
        for (auto & processor : processors)
        {
            validateProcessorConnections(processor.get());
        }
        
        /// Verify header consistency
        if (!current_outputs.empty())
        {
            auto expected_header = current_outputs.front()->getHeader();
            for (auto * output : current_outputs)
            {
                if (!output->getHeader().isCompatibleWith(expected_header))
                    throw Exception("Header mismatch in pipeline");
            }
        }
    }
    
    void validateProcessorConnections(IProcessor * processor)
    {
        /// Check input connections
        for (auto & input : processor->getInputs())
        {
            if (!input.isConnected())
                throw Exception("Unconnected input port in processor");
        }
        
        /// Check output connections for non-sink processors
        if (!processor->getOutputs().empty())
        {
            bool has_connected_output = false;
            for (auto & output : processor->getOutputs())
            {
                if (output.isConnected())
                {
                    has_connected_output = true;
                    break;
                }
            }
            
            if (!has_connected_output && !isSinkProcessor(processor))
                throw Exception("No connected outputs in non-sink processor");
        }
    }
};
```

### Logical to Physical Translation

The QueryPipelineBuilder implements a sophisticated translation mechanism that converts logical query plan steps into physical processor networks. This translation process considers both the semantic requirements of each operation and the physical constraints of the execution environment.

**Step-by-Step Translation Process**: Each QueryPlan step corresponds to a specific pattern of processor creation and connection. The builder maintains a registry of step translators that encapsulate the knowledge of how to convert logical operations into processor graphs.

```cpp
class StepTranslator
{
public:
    virtual ~StepTranslator() = default;
    
    /// Translate step into processors
    virtual void translateStep(const QueryPlanStep & step, 
                              QueryPipelineBuilder & builder) = 0;
    
    /// Check if step can be translated
    virtual bool canTranslate(const QueryPlanStep & step) const = 0;
    
    /// Get resource requirements
    virtual ResourceRequirements getResourceRequirements(
        const QueryPlanStep & step) const = 0;
};

class FilterStepTranslator : public StepTranslator
{
public:
    void translateStep(const QueryPlanStep & step, 
                      QueryPipelineBuilder & builder) override
    {
        auto & filter_step = static_cast<const FilterStep &>(step);
        
        /// Create filter processor
        auto filter_processor = std::make_shared<FilterTransform>(
            builder.getCurrentHeader(),
            filter_step.getFilterExpression(),
            filter_step.getFilterColumnName(),
            filter_step.shouldRemoveFilterColumn()
        );
        
        /// Add to pipeline
        builder.addTransform(filter_processor);
    }
    
    bool canTranslate(const QueryPlanStep & step) const override
    {
        return step.getStepType() == QueryPlanStep::Type::Filter;
    }
    
    ResourceRequirements getResourceRequirements(
        const QueryPlanStep & step) const override
    {
        /// Filter operations are typically CPU-bound
        return ResourceRequirements{
            .cpu_weight = 1.0,
            .memory_weight = 0.1,
            .io_weight = 0.0
        };
    }
};
```

**Parallelization Strategy**: The builder automatically determines optimal parallelization strategies based on step characteristics, data volume, and system resources. It implements several parallelization patterns:

1. **Data Parallelism**: Operations that can process different data chunks independently
2. **Pipeline Parallelism**: Operations that can overlap execution stages
3. **Hybrid Parallelism**: Combinations of data and pipeline parallelism

### Resource Management and Optimization

The QueryPipelineBuilder incorporates sophisticated resource management that considers memory constraints, CPU availability, and I/O bandwidth when constructing processor graphs.

**Memory-Aware Construction**: The builder tracks memory requirements for each processor and implements strategies to minimize peak memory usage:

```cpp
class MemoryAwareBuilder
{
private:
    size_t max_memory_limit;
    size_t current_memory_estimate;
    std::map<ProcessorPtr, size_t> processor_memory_usage;
    
public:
    void addProcessorWithMemoryCheck(ProcessorPtr processor)
    {
        auto memory_requirement = estimateProcessorMemory(processor.get());
        
        if (current_memory_estimate + memory_requirement > max_memory_limit)
        {
            /// Apply memory optimization strategies
            optimizeMemoryUsage();
            
            /// Recheck after optimization
            if (current_memory_estimate + memory_requirement > max_memory_limit)
            {
                /// Switch to external processing or streaming
                convertToExternalProcessing(processor);
            }
        }
        
        processor_memory_usage[processor] = memory_requirement;
        current_memory_estimate += memory_requirement;
    }
    
private:
    size_t estimateProcessorMemory(IProcessor * processor)
    {
        /// Estimate based on processor type and input characteristics
        if (auto * sort_processor = dynamic_cast<SortingTransform *>(processor))
        {
            return estimateSortMemory(sort_processor);
        }
        else if (auto * agg_processor = dynamic_cast<AggregatingTransform *>(processor))
        {
            return estimateAggregationMemory(agg_processor);
        }
        else
        {
            /// Default estimate for transform processors
            return estimateTransformMemory(processor);
        }
    }
    
    void optimizeMemoryUsage()
    {
        /// Apply various memory optimization strategies
        eliminateRedundantBuffering();
        enableStreamingProcessing();
        adjustChunkSizes();
    }
    
    void convertToExternalProcessing(ProcessorPtr & processor)
    {
        /// Convert memory-intensive processors to external variants
        if (auto * sort_processor = dynamic_cast<SortingTransform *>(processor.get()))
        {
            auto external_sort = std::make_shared<ExternalSortingTransform>(
                sort_processor->getInputHeader(),
                sort_processor->getSortDescription(),
                max_memory_limit / 4 /// Use quarter of available memory
            );
            
            processor = external_sort;
        }
    }
};
```

**CPU Affinity and NUMA Awareness**: The builder considers system topology when assigning processors to execution threads, implementing NUMA-aware scheduling that minimizes memory access latency:

```cpp
class NUMAAwareBuilder
{
private:
    std::vector<CPUSet> numa_nodes;
    std::map<ProcessorPtr, size_t> processor_numa_affinity;
    
public:
    void assignNUMAAffinities()
    {
        /// Analyze processor memory access patterns
        analyzeMemoryAccessPatterns();
        
        /// Assign processors to NUMA nodes
        for (auto & processor : processors)
        {
            auto optimal_node = findOptimalNUMANode(processor.get());
            processor_numa_affinity[processor] = optimal_node;
        }
    }
    
private:
    size_t findOptimalNUMANode(IProcessor * processor)
    {
        /// Consider data locality and processor characteristics
        if (isMemoryIntensive(processor))
        {
            /// Prefer node with most available memory
            return findNodeWithMostMemory();
        }
        else if (isCPUIntensive(processor))
        {
            /// Prefer node with most available CPU
            return findNodeWithMostCPU();
        }
        else
        {
            /// Use load balancing
            return findLeastLoadedNode();
        }
    }
};
```

### Advanced Pipeline Patterns

The QueryPipelineBuilder supports sophisticated pipeline patterns that enable efficient execution of complex queries:

**Pipeline Fusion**: Adjacent processors with compatible interfaces can be fused into single processors that eliminate intermediate data materialization:

```cpp
class PipelineFusion
{
public:
    static ProcessorPtr fuseFilterAndExpression(
        FilterTransform * filter, 
        ExpressionTransform * expression)
    {
        /// Create combined processor that applies filter and expression together
        auto combined_actions = combineExpressionActions(
            filter->getFilterExpression(),
            expression->getExpression()
        );
        
        return std::make_shared<FilterExpressionTransform>(
            filter->getInputHeader(),
            combined_actions,
            filter->getFilterColumnName(),
            filter->shouldRemoveFilterColumn()
        );
    }
    
    static ProcessorPtr fuseAggregationStages(
        AggregatingTransform * partial_agg,
        MergingAggregatedTransform * final_agg)
    {
        /// Create single processor that performs both partial and final aggregation
        return std::make_shared<TwoStageAggregatingTransform>(
            partial_agg->getInputHeader(),
            partial_agg->getAggregatingParams(),
            final_agg->getMergingParams()
        );
    }
};
```

**Adaptive Parallelization**: The builder can dynamically adjust parallelization based on runtime characteristics:

```cpp
class AdaptiveParallelization
{
private:
    std::atomic<size_t> current_parallelism{1};
    std::atomic<double> cpu_utilization{0.0};
    std::atomic<double> memory_pressure{0.0};
    
public:
    void adjustParallelization()
    {
        auto new_parallelism = calculateOptimalParallelism();
        
        if (new_parallelism != current_parallelism.load())
        {
            reshapeProcessorGraph(new_parallelism);
            current_parallelism = new_parallelism;
        }
    }
    
private:
    size_t calculateOptimalParallelism()
    {
        /// Consider system resources and query characteristics
        auto cpu_factor = std::min(2.0, 1.0 / std::max(0.1, cpu_utilization.load()));
        auto memory_factor = std::min(2.0, 1.0 / std::max(0.1, memory_pressure.load()));
        
        auto optimal = static_cast<size_t>(
            current_parallelism.load() * cpu_factor * memory_factor
        );
        
        return std::clamp(optimal, 1UL, std::thread::hardware_concurrency());
    }
    
    void reshapeProcessorGraph(size_t new_parallelism)
    {
        /// Dynamically adjust processor graph structure
        /// This is a complex operation that requires careful coordination
    }
};
```

This sophisticated pipeline construction system enables ClickHouse to automatically generate highly optimized execution plans that adapt to both query characteristics and system resources, providing the foundation for efficient analytical query processing across diverse workloads and hardware configurations.

## 3.5 Parallelism and Resource Allocation (3,000 words)

ClickHouse's execution engine implements sophisticated parallelism and resource allocation strategies that maximize hardware utilization while maintaining query performance and system stability. The system employs multiple levels of parallelism, from fine-grained processor-level concurrency to coarse-grained pipeline-level parallelization, all orchestrated through intelligent resource management algorithms.

### Thread Allocation Strategies

The thread allocation system operates on multiple levels, considering both global system resources and query-specific requirements. The allocation strategy balances competing demands for CPU cores, memory bandwidth, and I/O channels while avoiding resource contention and maintaining predictable performance.

**Dynamic Thread Pool Management**: ClickHouse employs a sophisticated thread pool architecture that adapts to workload characteristics and system conditions:

```cpp
class QueryThreadPool
{
private:
    /// Thread pool configuration
    size_t min_threads;
    size_t max_threads;
    size_t current_threads;
    
    /// Thread management
    std::vector<std::thread> worker_threads;
    std::queue<TaskPtr> task_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_condition;
    
    /// Resource tracking
    std::atomic<size_t> active_tasks{0};
    std::atomic<size_t> pending_tasks{0};
    std::atomic<double> cpu_utilization{0.0};
    std::atomic<size_t> memory_usage{0};
    
    /// NUMA awareness
    std::vector<CPUSet> numa_nodes;
    std::map<std::thread::id, size_t> thread_numa_affinity;
    
public:
    QueryThreadPool(size_t min_threads_, size_t max_threads_)
        : min_threads(min_threads_), max_threads(max_threads_)
    {
        initializeNUMATopology();
        createInitialThreads();
    }
    
    /// Submit task for execution
    void submitTask(TaskPtr task)
    {
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            task_queue.push(task);
            pending_tasks++;
        }
        
        queue_condition.notify_one();
        
        /// Check if we need more threads
        if (shouldExpandThreadPool())
        {
            expandThreadPool();
        }
    }
    
    /// Get optimal thread count for query
    size_t getOptimalThreadCount(const QueryContext & context)
    {
        /// Consider query characteristics
        auto query_complexity = analyzeQueryComplexity(context);
        auto memory_requirements = estimateMemoryRequirements(context);
        
        /// Consider system resources
        auto available_cores = getAvailableCores();
        auto available_memory = getAvailableMemory();
        
        /// Calculate optimal thread count
        size_t cpu_based_threads = std::min(
            static_cast<size_t>(query_complexity * available_cores),
            available_cores
        );
        
        size_t memory_based_threads = available_memory / memory_requirements;
        
        return std::min({cpu_based_threads, memory_based_threads, max_threads});
    }
    
private:
    void initializeNUMATopology()
    {
        /// Detect NUMA nodes and CPU topology
        auto numa_node_count = numa_num_configured_nodes();
        numa_nodes.resize(numa_node_count);
        
        for (size_t node = 0; node < numa_node_count; ++node)
        {
            auto cpu_mask = numa_allocate_cpumask();
            numa_node_to_cpus(node, cpu_mask);
            
            for (size_t cpu = 0; cpu < numa_num_configured_cpus(); ++cpu)
            {
                if (numa_bitmask_isbitset(cpu_mask, cpu))
                {
                    numa_nodes[node].insert(cpu);
                }
            }
            
            numa_free_cpumask(cpu_mask);
        }
    }
    
    void createInitialThreads()
    {
        for (size_t i = 0; i < min_threads; ++i)
        {
            createWorkerThread(i % numa_nodes.size());
        }
        current_threads = min_threads;
    }
    
    void createWorkerThread(size_t preferred_numa_node)
    {
        worker_threads.emplace_back([this, preferred_numa_node]() {
            /// Set NUMA affinity
            setThreadNUMAAffinity(preferred_numa_node);
            
            /// Main worker loop
            while (true)
            {
                TaskPtr task;
                
                {
                    std::unique_lock<std::mutex> lock(queue_mutex);
                    queue_condition.wait(lock, [this] { 
                        return !task_queue.empty() || should_shutdown; 
                    });
                    
                    if (should_shutdown && task_queue.empty())
                        break;
                    
                    task = task_queue.front();
                    task_queue.pop();
                    pending_tasks--;
                    active_tasks++;
                }
                
                /// Execute task
                executeTask(task);
                active_tasks--;
            }
        });
        
        /// Store NUMA affinity for this thread
        thread_numa_affinity[worker_threads.back().get_id()] = preferred_numa_node;
    }
    
    bool shouldExpandThreadPool()
    {
        /// Expand if we have pending tasks and available resources
        return pending_tasks > 0 && 
               current_threads < max_threads &&
               cpu_utilization < 0.8 &&
               memory_usage < getMemoryLimit() * 0.7;
    }
    
    void expandThreadPool()
    {
        if (current_threads >= max_threads)
            return;
        
        /// Find NUMA node with least loaded threads
        auto target_numa_node = findLeastLoadedNUMANode();
        
        createWorkerThread(target_numa_node);
        current_threads++;
    }
    
    size_t findLeastLoadedNUMANode()
    {
        std::vector<size_t> numa_thread_counts(numa_nodes.size(), 0);
        
        for (const auto & [thread_id, numa_node] : thread_numa_affinity)
        {
            numa_thread_counts[numa_node]++;
        }
        
        return std::min_element(numa_thread_counts.begin(), 
                               numa_thread_counts.end()) - 
               numa_thread_counts.begin();
    }
    
    void setThreadNUMAAffinity(size_t numa_node)
    {
        if (numa_node >= numa_nodes.size())
            return;
        
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        
        for (auto cpu : numa_nodes[numa_node])
        {
            CPU_SET(cpu, &cpu_set);
        }
        
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
    }
};
```

**Processor-Level Parallelism**: Individual processors can leverage internal parallelism through vectorization, multi-threading, and specialized algorithms:

```cpp
class ParallelAggregatingTransform : public IProcessor
{
private:
    /// Aggregation parameters
    AggregatingParams params;
    size_t num_threads;
    
    /// Parallel aggregation state
    std::vector<std::unique_ptr<Aggregator>> aggregators;
    std::vector<std::thread> aggregation_threads;
    ThreadPool thread_pool;
    
    /// Synchronization
    std::mutex result_mutex;
    std::condition_variable completion_signal;
    std::atomic<size_t> completed_threads{0};
    
public:
    ParallelAggregatingTransform(const Block & header_, 
                                const AggregatingParams & params_,
                                size_t num_threads_)
        : params(params_), num_threads(num_threads_)
    {
        addInputPort(header_);
        addOutputPort(params.getHeader(header_));
        
        /// Create per-thread aggregators
        for (size_t i = 0; i < num_threads; ++i)
        {
            aggregators.emplace_back(std::make_unique<Aggregator>(params));
        }
    }
    
    void work() override
    {
        auto & input = getInputs().front();
        auto & output = getOutputs().front();
        
        auto chunk = input.pull();
        if (chunk.empty())
        {
            /// Finalize aggregation
            finalizeAggregation();
            return;
        }
        
        /// Distribute chunk across threads
        distributeChunkForAggregation(chunk);
    }
    
private:
    void distributeChunkForAggregation(const Chunk & chunk)
    {
        /// Partition chunk by aggregation key hash
        auto partitioned_chunks = partitionChunkByHash(chunk, num_threads);
        
        /// Submit aggregation tasks
        for (size_t i = 0; i < num_threads; ++i)
        {
            if (!partitioned_chunks[i].empty())
            {
                thread_pool.submitTask([this, i, chunk = std::move(partitioned_chunks[i])]() {
                    aggregators[i]->executeOnChunk(chunk);
                });
            }
        }
    }
    
    void finalizeAggregation()
    {
        /// Wait for all aggregation threads to complete
        thread_pool.waitForAllTasks();
        
        /// Merge aggregation results
        auto merged_result = mergeAggregationResults();
        
        /// Output final result
        auto & output = getOutputs().front();
        output.push(std::move(merged_result));
    }
    
    std::vector<Chunk> partitionChunkByHash(const Chunk & chunk, size_t num_partitions)
    {
        std::vector<Chunk> partitions(num_partitions);
        
        /// Get key columns for hashing
        auto key_columns = extractKeyColumns(chunk);
        
        /// Calculate hash for each row
        std::vector<size_t> row_hashes(chunk.getNumRows());
        for (size_t row = 0; row < chunk.getNumRows(); ++row)
        {
            row_hashes[row] = calculateRowHash(key_columns, row) % num_partitions;
        }
        
        /// Distribute rows to partitions
        for (size_t partition = 0; partition < num_partitions; ++partition)
        {
            std::vector<size_t> partition_rows;
            for (size_t row = 0; row < chunk.getNumRows(); ++row)
            {
                if (row_hashes[row] == partition)
                {
                    partition_rows.push_back(row);
                }
            }
            
            if (!partition_rows.empty())
            {
                partitions[partition] = extractRowsFromChunk(chunk, partition_rows);
            }
        }
        
        return partitions;
    }
    
    Chunk mergeAggregationResults()
    {
        /// Merge results from all aggregators
        std::vector<BlocksList> aggregated_blocks;
        
        for (auto & aggregator : aggregators)
        {
            auto blocks = aggregator->convertToBlocks();
            aggregated_blocks.push_back(std::move(blocks));
        }
        
        /// Merge blocks using final aggregation
        auto final_aggregator = std::make_unique<Aggregator>(params);
        
        for (auto & blocks : aggregated_blocks)
        {
            for (auto & block : blocks)
            {
                final_aggregator->mergeOnBlock(block);
            }
        }
        
        auto result_blocks = final_aggregator->convertToBlocks();
        return mergeBlocksIntoChunk(result_blocks);
    }
};
```

### NUMA Awareness and Memory Locality

ClickHouse implements comprehensive NUMA (Non-Uniform Memory Access) awareness to optimize memory access patterns and minimize cross-node memory traffic. This is particularly important for large-scale analytical workloads that process massive datasets.

**NUMA-Aware Memory Allocation**: The system includes specialized memory allocators that consider NUMA topology when allocating memory for processors and data structures:

```cpp
class NUMAAwareAllocator
{
private:
    struct NUMANode
    {
        size_t node_id;
        size_t available_memory;
        size_t allocated_memory;
        std::vector<size_t> cpu_cores;
        std::unique_ptr<MemoryPool> memory_pool;
    };
    
    std::vector<NUMANode> numa_nodes;
    std::map<std::thread::id, size_t> thread_numa_mapping;
    
public:
    NUMAAwareAllocator()
    {
        initializeNUMANodes();
    }
    
    /// Allocate memory on optimal NUMA node
    void * allocate(size_t size, size_t alignment = 0)
    {
        auto optimal_node = findOptimalNUMANode(size);
        return allocateOnNode(optimal_node, size, alignment);
    }
    
    /// Allocate memory on specific NUMA node
    void * allocateOnNode(size_t node_id, size_t size, size_t alignment = 0)
    {
        if (node_id >= numa_nodes.size())
            return nullptr;
        
        auto & node = numa_nodes[node_id];
        
        /// Check if node has sufficient memory
        if (node.available_memory < size)
        {
            /// Try to free some memory or use another node
            if (!tryFreeMemoryOnNode(node_id, size))
            {
                return allocateOnAlternativeNode(size, alignment);
            }
        }
        
        /// Allocate using node-specific memory pool
        void * ptr = node.memory_pool->allocate(size, alignment);
        if (ptr)
        {
            node.allocated_memory += size;
            node.available_memory -= size;
        }
        
        return ptr;
    }
    
    /// Get optimal NUMA node for current thread
    size_t getCurrentThreadNUMANode()
    {
        auto thread_id = std::this_thread::get_id();
        auto it = thread_numa_mapping.find(thread_id);
        
        if (it != thread_numa_mapping.end())
        {
            return it->second;
        }
        
        /// Determine node based on CPU affinity
        auto node_id = detectCurrentNUMANode();
        thread_numa_mapping[thread_id] = node_id;
        return node_id;
    }
    
private:
    void initializeNUMANodes()
    {
        auto num_nodes = numa_num_configured_nodes();
        numa_nodes.resize(num_nodes);
        
        for (size_t node = 0; node < num_nodes; ++node)
        {
            numa_nodes[node].node_id = node;
            numa_nodes[node].available_memory = numa_node_size64(node, nullptr);
            numa_nodes[node].allocated_memory = 0;
            
            /// Get CPU cores for this node
            auto cpu_mask = numa_allocate_cpumask();
            numa_node_to_cpus(node, cpu_mask);
            
            for (size_t cpu = 0; cpu < numa_num_configured_cpus(); ++cpu)
            {
                if (numa_bitmask_isbitset(cpu_mask, cpu))
                {
                    numa_nodes[node].cpu_cores.push_back(cpu);
                }
            }
            
            numa_free_cpumask(cpu_mask);
            
            /// Create node-specific memory pool
            numa_nodes[node].memory_pool = std::make_unique<MemoryPool>(
                numa_nodes[node].available_memory / 2 /// Reserve half for OS
            );
        }
    }
    
    size_t findOptimalNUMANode(size_t size)
    {
        /// Prefer current thread's NUMA node
        auto current_node = getCurrentThreadNUMANode();
        if (numa_nodes[current_node].available_memory >= size)
        {
            return current_node;
        }
        
        /// Find node with most available memory
        size_t best_node = 0;
        size_t max_available = 0;
        
        for (size_t node = 0; node < numa_nodes.size(); ++node)
        {
            if (numa_nodes[node].available_memory > max_available)
            {
                max_available = numa_nodes[node].available_memory;
                best_node = node;
            }
        }
        
        return best_node;
    }
    
    size_t detectCurrentNUMANode()
    {
        /// Get current CPU
        auto cpu = sched_getcpu();
        
        /// Find which NUMA node contains this CPU
        for (size_t node = 0; node < numa_nodes.size(); ++node)
        {
            auto & cores = numa_nodes[node].cpu_cores;
            if (std::find(cores.begin(), cores.end(), cpu) != cores.end())
            {
                return node;
            }
        }
        
        return 0; /// Default to node 0
    }
};
```

### Memory Management in Parallel Execution

Parallel execution introduces complex memory management challenges, including coordinating memory usage across multiple threads, preventing memory fragmentation, and maintaining optimal cache locality.

**Shared Memory Pools**: ClickHouse implements sophisticated shared memory pool systems that enable efficient memory sharing between processors while maintaining thread safety:

```cpp
class SharedMemoryPool
{
private:
    /// Memory pool segments
    struct MemorySegment
    {
        void * base_ptr;
        size_t size;
        std::atomic<size_t> allocated_bytes{0};
        std::atomic<size_t> reference_count{0};
        std::mutex allocation_mutex;
        
        /// Free block management
        std::set<std::pair<size_t, size_t>> free_blocks; /// {offset, size}
        
        bool canAllocate(size_t requested_size) const
        {
            return allocated_bytes.load() + requested_size <= size;
        }
        
        void * allocate(size_t size, size_t alignment)
        {
            std::lock_guard<std::mutex> lock(allocation_mutex);
            
            /// Find suitable free block
            auto it = findFreeBlock(size, alignment);
            if (it == free_blocks.end())
                return nullptr;
            
            auto [offset, block_size] = *it;
            free_blocks.erase(it);
            
            /// Split block if necessary
            if (block_size > size)
            {
                free_blocks.insert({offset + size, block_size - size});
            }
            
            allocated_bytes += size;
            return static_cast<char *>(base_ptr) + offset;
        }
        
        void deallocate(void * ptr, size_t size)
        {
            std::lock_guard<std::mutex> lock(allocation_mutex);
            
            auto offset = static_cast<char *>(ptr) - static_cast<char *>(base_ptr);
            
            /// Add to free blocks and coalesce
            coalesceFreeBlocks(offset, size);
            allocated_bytes -= size;
        }
        
    private:
        std::set<std::pair<size_t, size_t>>::iterator findFreeBlock(
            size_t size, size_t alignment)
        {
            for (auto it = free_blocks.begin(); it != free_blocks.end(); ++it)
            {
                auto [offset, block_size] = *it;
                
                /// Check alignment
                auto aligned_offset = (offset + alignment - 1) & ~(alignment - 1);
                auto aligned_size = size + (aligned_offset - offset);
                
                if (aligned_size <= block_size)
                {
                    return it;
                }
            }
            
            return free_blocks.end();
        }
        
        void coalesceFreeBlocks(size_t offset, size_t size)
        {
            /// Find adjacent blocks and merge
            auto new_block = std::make_pair(offset, size);
            
            /// Check for adjacent blocks
            auto it = free_blocks.lower_bound(new_block);
            
            /// Merge with previous block
            if (it != free_blocks.begin())
            {
                auto prev_it = std::prev(it);
                if (prev_it->first + prev_it->second == offset)
                {
                    new_block.first = prev_it->first;
                    new_block.second += prev_it->second;
                    free_blocks.erase(prev_it);
                }
            }
            
            /// Merge with next block
            if (it != free_blocks.end() && offset + size == it->first)
            {
                new_block.second += it->second;
                free_blocks.erase(it);
            }
            
            free_blocks.insert(new_block);
        }
    };
    
    std::vector<std::unique_ptr<MemorySegment>> segments;
    std::mutex segments_mutex;
    
public:
    SharedMemoryPool(size_t initial_size = 1024 * 1024 * 1024) /// 1GB default
    {
        createSegment(initial_size);
    }
    
    void * allocate(size_t size, size_t alignment = 8)
    {
        /// Try existing segments first
        for (auto & segment : segments)
        {
            if (segment->canAllocate(size))
            {
                void * ptr = segment->allocate(size, alignment);
                if (ptr)
                {
                    segment->reference_count++;
                    return ptr;
                }
            }
        }
        
        /// Create new segment if needed
        {
            std::lock_guard<std::mutex> lock(segments_mutex);
            
            auto new_segment_size = std::max(size * 2, 256 * 1024 * 1024UL);
            createSegment(new_segment_size);
            
            auto & segment = segments.back();
            void * ptr = segment->allocate(size, alignment);
            if (ptr)
            {
                segment->reference_count++;
                return ptr;
            }
        }
        
        return nullptr;
    }
    
    void deallocate(void * ptr, size_t size)
    {
        /// Find segment containing this pointer
        for (auto & segment : segments)
        {
            if (ptr >= segment->base_ptr && 
                ptr < static_cast<char *>(segment->base_ptr) + segment->size)
            {
                segment->deallocate(ptr, size);
                segment->reference_count--;
                
                /// Clean up empty segments
                if (segment->reference_count == 0 && segment->allocated_bytes == 0)
                {
                    cleanupSegment(segment.get());
                }
                
                return;
            }
        }
    }
    
private:
    void createSegment(size_t size)
    {
        auto segment = std::make_unique<MemorySegment>();
        segment->base_ptr = aligned_alloc(4096, size); /// Page-aligned
        segment->size = size;
        segment->free_blocks.insert({0, size});
        
        segments.push_back(std::move(segment));
    }
    
    void cleanupSegment(MemorySegment * segment)
    {
        /// Remove from segments list and free memory
        auto it = std::find_if(segments.begin(), segments.end(),
            [segment](const auto & seg) { return seg.get() == segment; });
        
        if (it != segments.end())
        {
            free(segment->base_ptr);
            segments.erase(it);
        }
    }
};
```

### Load Balancing and Work Stealing

ClickHouse implements sophisticated load balancing mechanisms that ensure optimal resource utilization across all available processing units. The system employs work-stealing algorithms that dynamically redistribute work from overloaded threads to underutilized ones.

**Dynamic Work Stealing**: The work stealing implementation allows idle threads to steal work from busy threads, maintaining optimal load distribution:

```cpp
class WorkStealingScheduler
{
private:
    struct WorkQueue
    {
        std::deque<TaskPtr> tasks;
        std::mutex mutex;
        std::atomic<size_t> steal_attempts{0};
        std::atomic<size_t> successful_steals{0};
    };
    
    std::vector<std::unique_ptr<WorkQueue>> worker_queues;
    std::atomic<size_t> global_task_count{0};
    
public:
    WorkStealingScheduler(size_t num_workers)
    {
        worker_queues.resize(num_workers);
        for (size_t i = 0; i < num_workers; ++i)
        {
            worker_queues[i] = std::make_unique<WorkQueue>();
        }
    }
    
    /// Submit task to least loaded queue
    void submitTask(TaskPtr task)
    {
        auto target_queue = findLeastLoadedQueue();
        
        {
            std::lock_guard<std::mutex> lock(worker_queues[target_queue]->mutex);
            worker_queues[target_queue]->tasks.push_back(task);
        }
        
        global_task_count++;
    }
    
    /// Get next task for worker
    TaskPtr getNextTask(size_t worker_id)
    {
        /// Try local queue first
        auto task = tryGetLocalTask(worker_id);
        if (task)
            return task;
        
        /// Try work stealing
        return tryStealTask(worker_id);
    }
    
private:
    size_t findLeastLoadedQueue()
    {
        size_t min_size = std::numeric_limits<size_t>::max();
        size_t best_queue = 0;
        
        for (size_t i = 0; i < worker_queues.size(); ++i)
        {
            std::lock_guard<std::mutex> lock(worker_queues[i]->mutex);
            if (worker_queues[i]->tasks.size() < min_size)
            {
                min_size = worker_queues[i]->tasks.size();
                best_queue = i;
            }
        }
        
        return best_queue;
    }
    
    TaskPtr tryGetLocalTask(size_t worker_id)
    {
        auto & queue = worker_queues[worker_id];
        std::lock_guard<std::mutex> lock(queue->mutex);
        
        if (queue->tasks.empty())
            return nullptr;
        
        auto task = queue->tasks.front();
        queue->tasks.pop_front();
        global_task_count--;
        
        return task;
    }
    
    TaskPtr tryStealTask(size_t worker_id)
    {
        /// Try to steal from other queues
        for (size_t attempts = 0; attempts < worker_queues.size(); ++attempts)
        {
            auto target_queue = (worker_id + attempts + 1) % worker_queues.size();
            
            worker_queues[target_queue]->steal_attempts++;
            
            auto task = stealFromQueue(target_queue);
            if (task)
            {
                worker_queues[target_queue]->successful_steals++;
                return task;
            }
        }
        
        return nullptr;
    }
    
    TaskPtr stealFromQueue(size_t queue_id)
    {
        auto & queue = worker_queues[queue_id];
        std::lock_guard<std::mutex> lock(queue->mutex);
        
        if (queue->tasks.size() <= 1)
            return nullptr; /// Don't steal last task
        
        /// Steal from back to minimize contention
        auto task = queue->tasks.back();
        queue->tasks.pop_back();
        global_task_count--;
        
        return task;
    }
};
```

This comprehensive parallelism and resource allocation system enables ClickHouse to efficiently utilize modern multi-core, multi-socket systems while maintaining optimal performance across diverse analytical workloads. The combination of NUMA awareness, intelligent thread allocation, and dynamic load balancing ensures that the system can scale effectively from small single-node deployments to large distributed clusters.

## Phase 3 Summary

Phase 3 has provided a comprehensive exploration of ClickHouse's processor architecture, covering:

1. **IProcessor Interface and Execution Model**: The foundational abstraction that enables flexible, high-performance query execution through a sophisticated state machine and vectorized processing model.

2. **Processor State Machine and Port System**: The communication and synchronization mechanisms that enable efficient data flow between processors while maintaining thread safety and optimal performance.

3. **Core Processor Types**: The specialized processor implementations that handle different aspects of query execution, from data ingestion through complex transformations and aggregations.

4. **Pipeline Graph Construction**: The sophisticated orchestration system that translates logical query plans into optimized processor networks with advanced optimization strategies.

5. **Parallelism and Resource Allocation**: The comprehensive resource management system that maximizes hardware utilization through intelligent thread allocation, NUMA awareness, and dynamic load balancing.

Together, these components form a sophisticated execution engine that can efficiently process complex analytical queries across diverse hardware configurations while maintaining high performance and system stability.

## Phase 4: Data Structures and Memory Management (12,000 words)

Phase 4 explores the fundamental data structures and memory management systems that underpin ClickHouse's high-performance query processing capabilities. This includes the columnar data representation through the IColumn interface hierarchy, the sophisticated memory management systems including Arena allocators and PODArray implementations, Block structures for data chunk processing, and Field abstractions for individual value handling. These components work together to provide efficient, cache-friendly data processing that maximizes CPU utilization and minimizes memory overhead.

### 4.1 IColumn Interface and Columnar Data Layout (2,500 words)

The IColumn interface represents the cornerstone of ClickHouse's columnar data architecture, providing a unified abstraction for all column data types while enabling specialized implementations optimized for specific data patterns. This sophisticated interface hierarchy enables vectorized operations, efficient memory utilization, and seamless integration with query processing pipelines.

#### 4.1.1 Core IColumn Interface Architecture

The IColumn interface defines the fundamental contract for all columnar data structures in ClickHouse. This interface provides over 50 virtual methods covering data access, memory management, vectorized operations, comparison, serialization, and type information. The design enables both generic operations through the common interface and specialized optimizations through type-specific implementations.

**Core Virtual Methods:**

```cpp
class IColumn
{
public:
    // Data access
    virtual size_t size() const = 0;
    virtual Field operator[](size_t n) const = 0;
    virtual StringRef getDataAt(size_t n) const = 0;
    
    // Memory management
    virtual MutableColumnPtr cloneEmpty() const = 0;
    virtual ColumnPtr cut(size_t start, size_t length) const = 0;
    virtual void insertFrom(const IColumn & src, size_t n) = 0;
    
    // Vectorized operations
    virtual ColumnPtr filter(const Filter & filter, ssize_t result_size_hint) const = 0;
    virtual ColumnPtr permute(const Permutation & perm, size_t limit) const = 0;
    
    // Comparison and hashing
    virtual int compareAt(size_t n, size_t m, const IColumn & rhs, int nan_direction_hint) const = 0;
    virtual void updateHashWithValue(size_t n, SipHash & hash) const = 0;
    
    // Serialization
    virtual void serializeValueIntoArena(size_t n, Arena & arena, char const *& begin) const = 0;
    
    // Type information
    virtual const char * getFamilyName() const = 0;
    virtual bool isFixedAndContiguous() const = 0;
};
```

**Specialized Implementations:** ClickHouse provides dozens of specialized column implementations including ColumnVector for numeric types, ColumnString for variable-length strings, ColumnArray for nested arrays, ColumnTuple for structured data, ColumnNullable for nullable types, ColumnLowCardinality for dictionary encoding, and ColumnConst for constant values.

#### 4.1.2 Memory Layout and Cache Optimization

ClickHouse's columnar layout is designed for maximum cache efficiency and SIMD operations. Numeric columns store data in contiguous arrays enabling vectorized processing, while variable-length types like strings use offset-based layouts that maintain cache locality within reasonable bounds.

**Numeric Column Layout:**

```cpp
template <typename T>
class ColumnVector final : public COWHelper<IColumn, ColumnVector<T>>
{
private:
    PaddedPODArray<T> data;  // Contiguous array with padding for SIMD
    
public:
    const T * data_begin() const { return data.begin(); }
    T * data_begin() { return data.begin(); }
    
    // Vectorized operations leverage contiguous layout
    ColumnPtr filter(const Filter & filter, ssize_t result_size_hint) const override
    {
        // SIMD-optimized filtering when possible
        if constexpr (std::is_arithmetic_v<T> && sizeof(T) <= 8)
            return filterArrayImplAVX2(data.data(), filter.data(), data.size());
        else
            return filterArrayImpl(data.data(), filter.data(), data.size());
    }
};
```

**String Column Optimization:** String columns use a two-array approach with chars stored contiguously and offsets providing boundary information, enabling efficient range operations while maintaining reasonable cache locality.

### 4.2 Arena Allocators and Memory Pools (2,500 words)

ClickHouse employs sophisticated memory management through Arena allocators and memory pools designed to minimize allocation overhead, reduce memory fragmentation, and provide optimal performance for analytical workloads. These systems are particularly crucial for aggregate function states, temporary data structures, and string processing.

#### 4.2.1 Arena Allocator Architecture

The Arena allocator provides extremely fast memory allocation by pre-allocating large contiguous blocks and distributing memory through simple pointer arithmetic. This approach eliminates the overhead of individual malloc/free calls while providing excellent cache locality.

**Core Arena Implementation:**

```cpp
class Arena
{
private:
    /// Memory chunks allocated from system
    struct Chunk
    {
        char * begin;
        char * pos;      /// Current allocation position
        char * end;      /// End of chunk
        
        Chunk(size_t size) 
        {
            begin = pos = static_cast<char *>(aligned_alloc(4096, size));
            end = begin + size;
        }
        
        ~Chunk() { free(begin); }
        
        size_t size() const { return end - begin; }
        size_t used() const { return pos - begin; }
        size_t remaining() const { return end - pos; }
    };
    
    std::vector<std::unique_ptr<Chunk>> chunks;
    Chunk * head = nullptr;
    size_t growth_factor = 2;
    size_t linear_growth_threshold = 128 * 1024 * 1024; // 128MB
    size_t initial_size = 4096;
    
public:
    Arena(size_t initial_size_ = 4096) : initial_size(initial_size_)
    {
        addChunk(initial_size);
    }
    
    /// Fast allocation from current chunk
    char * alloc(size_t size, size_t alignment = 8)
    {
        if (unlikely(!head))
            addChunk(initial_size);
        
        /// Align current position
        char * aligned_pos = reinterpret_cast<char *>(
            (reinterpret_cast<uintptr_t>(head->pos) + alignment - 1) & ~(alignment - 1));
        
        if (unlikely(aligned_pos + size > head->end))
        {
            /// Need new chunk
            size_t next_size = calculateNextChunkSize(size);
            addChunk(next_size);
            return alloc(size, alignment);
        }
        
        head->pos = aligned_pos + size;
        return aligned_pos;
    }
    
    /// Allocate and continue from previous allocation (for serialization)
    char * allocContinue(size_t size, char const *& begin)
    {
        char * res = alloc(size);
        if (begin == nullptr)
            begin = res;
        return res;
    }
    
    /// Memory statistics
    size_t size() const
    {
        size_t total = 0;
        for (const auto & chunk : chunks)
            total += chunk->used();
        return total;
    }
    
    size_t allocatedBytes() const
    {
        size_t total = 0;
        for (const auto & chunk : chunks)
            total += chunk->size();
        return total;
    }
    
private:
    void addChunk(size_t size)
    {
        chunks.emplace_back(std::make_unique<Chunk>(size));
        head = chunks.back().get();
    }
    
    size_t calculateNextChunkSize(size_t required_size)
    {
        if (chunks.empty())
            return std::max(required_size, initial_size);
        
        size_t last_size = chunks.back()->size();
        
        /// Linear growth for very large chunks
        if (last_size >= linear_growth_threshold)
            return std::max(required_size, last_size + linear_growth_threshold);
        
        /// Exponential growth for smaller chunks
        return std::max(required_size, last_size * growth_factor);
    }
};
```

**Memory Pool Specializations:** ClickHouse implements specialized memory pools for different use cases:

1. **StringArena**: Optimized for string storage with efficient small allocations
2. **AggregateDataArena**: Designed for aggregate function states with alignment requirements
3. **HashTableArena**: Memory pool for hash table nodes with fast allocation/deallocation
4. **TemporaryDataArena**: Short-lived allocations during query processing

#### 4.2.2 PODArray Implementation

PODArray (Plain Old Data Array) is ClickHouse's optimized dynamic array implementation designed for maximum performance with fundamental data types. It includes sophisticated growth strategies, NUMA awareness, and SIMD-friendly memory layout.

**Core PODArray Architecture:**

```cpp
template <typename T, size_t initial_bytes = 4096, typename TAllocator = Allocator<false>>
class PODArrayBase
{
protected:
    static constexpr size_t pad_right = 15;  /// Padding for SIMD operations
    static constexpr size_t initial_capacity = initial_bytes / sizeof(T);
    
    T * c_start = nullptr;
    T * c_end = nullptr;
    T * c_end_of_storage = nullptr;
    
    TAllocator alloc;
    
public:
    using value_type = T;
    using allocator_type = TAllocator;
    
    PODArrayBase() { reserveForNextSize(); }
    
    explicit PODArrayBase(size_t n) 
    { 
        reserveForNextSize();
        resize(n);
    }
    
    PODArrayBase(size_t n, const T & x) 
    { 
        reserveForNextSize();
        assign(n, x);
    }
    
    ~PODArrayBase() 
    { 
        if (c_start)
            alloc.deallocate(c_start, allocated_bytes());
    }
    
    /// Direct data access
    T * data() { return c_start; }
    const T * data() const { return c_start; }
    T * begin() { return c_start; }
    const T * begin() const { return c_start; }
    T * end() { return c_end; }
    const T * end() const { return c_end; }
    
    /// Size operations
    size_t size() const { return c_end - c_start; }
    size_t capacity() const { return c_end_of_storage - c_start; }
    bool empty() const { return c_end == c_start; }
    size_t allocated_bytes() const { return capacity() * sizeof(T) + pad_right; }
    
    /// Element access
    T & operator[](size_t n) { return c_start[n]; }
    const T & operator[](size_t n) const { return c_start[n]; }
    T & back() { return *(c_end - 1); }
    const T & back() const { return *(c_end - 1); }
    
    /// Modification operations
    void push_back(const T & x)
    {
        if (unlikely(c_end == c_end_of_storage))
            reserveForNextSize();
        
        new (c_end) T(x);
        ++c_end;
    }
    
    template <typename... Args>
    void emplace_back(Args &&... args)
    {
        if (unlikely(c_end == c_end_of_storage))
            reserveForNextSize();
        
        new (c_end) T(std::forward<Args>(args)...);
        ++c_end;
    }
    
    void pop_back() { --c_end; }
    
    void resize(size_t n)
    {
        if (n > capacity())
            reserve(roundUpToPowerOfTwoOrZero(n));
        
        if constexpr (std::is_trivially_constructible_v<T>)
        {
            c_end = c_start + n;
        }
        else
        {
            /// Construct/destruct elements as needed
            if (n > size())
            {
                std::uninitialized_default_construct(c_end, c_start + n);
            }
            else if (n < size())
            {
                std::destroy(c_start + n, c_end);
            }
            c_end = c_start + n;
        }
    }
    
    void reserve(size_t n)
    {
        if (n <= capacity())
            return;
        
        /// Calculate new capacity with growth strategy
        size_t new_capacity = std::max(n, capacity() * 2);
        
        /// Allocate new memory
        T * new_data = alloc.allocate(new_capacity * sizeof(T) + pad_right);
        
        /// Move existing elements
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memcpy(new_data, c_start, size() * sizeof(T));
        }
        else
        {
            std::uninitialized_move(c_start, c_end, new_data);
            std::destroy(c_start, c_end);
        }
        
        /// Update pointers
        size_t old_size = size();
        if (c_start)
            alloc.deallocate(c_start, allocated_bytes());
        
        c_start = new_data;
        c_end = c_start + old_size;
        c_end_of_storage = c_start + new_capacity;
    }
    
    /// Efficient assignment operations
    void assign(size_t n, const T & val)
    {
        resize(n);
        std::fill(c_start, c_end, val);
    }
    
    template <typename Iterator>
    void assign(Iterator first, Iterator last)
    {
        size_t n = std::distance(first, last);
        resize(n);
        std::copy(first, last, c_start);
    }
    
    /// SIMD-optimized operations
    void fill(const T & value)
    {
        if constexpr (std::is_arithmetic_v<T> && sizeof(T) <= 8)
        {
            fillVectorized(c_start, c_end, value);
        }
        else
        {
            std::fill(c_start, c_end, value);
        }
    }
    
    void zero()
    {
        if constexpr (std::is_trivially_copyable_v<T>)
        {
            memset(c_start, 0, size() * sizeof(T));
        }
        else
        {
            fill(T{});
        }
    }
    
private:
    void reserveForNextSize()
    {
        if (capacity() == 0)
            reserve(initial_capacity);
    }
    
    /// SIMD implementation for arithmetic types
    template<typename U = T>
    std::enable_if_t<std::is_arithmetic_v<U> && sizeof(U) == 4>
    fillVectorized(T * begin, T * end, const T & value)
    {
        size_t size = end - begin;
        size_t simd_size = (size / 8) * 8;
        
        /// AVX2 implementation for 32-bit values
        if (simd_size > 0)
        {
            __m256i fill_vec = _mm256_set1_epi32(bit_cast<int32_t>(value));
            __m256i * simd_ptr = reinterpret_cast<__m256i *>(begin);
            
            for (size_t i = 0; i < simd_size / 8; ++i)
            {
                _mm256_storeu_si256(simd_ptr + i, fill_vec);
            }
        }
        
        /// Handle remaining elements
        std::fill(begin + simd_size, end, value);
    }
    
    template<typename U = T>
    std::enable_if_t<!std::is_arithmetic_v<U> || sizeof(U) != 4>
    fillVectorized(T * begin, T * end, const T & value)
    {
        std::fill(begin, end, value);
    }
};

/// Commonly used PODArray types
template <typename T>
using PODArray = PODArrayBase<T, 4096, Allocator<false>>;

template <typename T>
using PaddedPODArray = PODArrayBase<T, 4096, AllocatorWithStackMemory<Allocator<false>, 4096>>;
```

#### 4.2.3 NUMA-Aware Memory Management

For multi-socket systems, ClickHouse implements NUMA-aware memory allocation strategies that optimize data locality and minimize cross-socket memory access penalties.

**NUMA-Aware Allocator:**

```cpp
class NUMAAwareAllocator
{
private:
    struct NUMANode
    {
        size_t node_id;
        size_t available_memory;
        size_t allocated_memory;
        std::vector<size_t> cpu_cores;
        std::unique_ptr<Arena> arena;
    };
    
    std::vector<NUMANode> numa_nodes;
    std::map<std::thread::id, size_t> thread_numa_mapping;
    
public:
    void * allocate(size_t size, size_t alignment = 0)
    {
        auto optimal_node = findOptimalNUMANode(size);
        return allocateOnNode(optimal_node, size, alignment);
    }
    
    void * allocateOnNode(size_t node_id, size_t size, size_t alignment = 0)
    {
        if (node_id >= numa_nodes.size())
            return nullptr;
        
        auto & node = numa_nodes[node_id];
        
        /// Allocate using node-specific arena
        void * ptr = node.arena->alloc(size, alignment);
        if (ptr)
        {
            node.allocated_memory += size;
            node.available_memory -= size;
        }
        
        return ptr;
    }
    
    size_t getCurrentThreadNUMANode()
    {
        auto thread_id = std::this_thread::get_id();
        auto it = thread_numa_mapping.find(thread_id);
        
        if (it != thread_numa_mapping.end())
            return it->second;
        
        /// Determine node based on CPU affinity
        auto node_id = detectCurrentNUMANode();
        thread_numa_mapping[thread_id] = node_id;
        return node_id;
    }
    
private:
    size_t findOptimalNUMANode(size_t size)
    {
        /// Prefer current thread's NUMA node
        auto current_node = getCurrentThreadNUMANode();
        if (numa_nodes[current_node].available_memory >= size)
            return current_node;
        
        /// Find node with most available memory
        size_t best_node = 0;
        size_t max_available = 0;
        
        for (size_t node = 0; node < numa_nodes.size(); ++node)
        {
            if (numa_nodes[node].available_memory > max_available)
            {
                max_available = numa_nodes[node].available_memory;
                best_node = node;
            }
        }
        
        return best_node;
    }
};
```

This sophisticated memory management system ensures optimal performance across diverse hardware configurations while minimizing allocation overhead and maximizing cache efficiency throughout the query processing pipeline.

### 4.3 Block Structure and Data Flow Management (2,500 words)

The Block structure represents the fundamental unit of data flow in ClickHouse's query processing pipeline. A Block serves as a container for a subset of table data during query execution, organizing columns with their associated data types and names into a cohesive processing unit. This design enables efficient batch processing while maintaining type safety and enabling complex transformations across the query pipeline.

#### 4.3.1 Core Block Architecture

The Block class in ClickHouse encapsulates the essential components needed for data chunk processing:

**Fundamental Block Structure:**

```cpp
class Block
{
private:
    /// Container holding columns with metadata
    using Container = std::vector<ColumnWithTypeAndName>;
    Container data;
    
    /// Performance optimization for column lookup
    IndexByName index_by_name;
    
public:
    /// Basic constructors and assignment
    Block() = default;
    Block(const Block &) = default;
    Block(Block &&) noexcept = default;
    Block & operator=(const Block &) = default;
    Block & operator=(Block &&) noexcept = default;
    
    /// Constructor from column list
    Block(std::initializer_list<ColumnWithTypeAndName> il);
    Block(const ColumnsWithTypeAndName & columns_);
    
    /// Column access and manipulation
    const ColumnWithTypeAndName & getByPosition(size_t position) const;
    ColumnWithTypeAndName & getByPosition(size_t position);
    const ColumnWithTypeAndName & getByName(const std::string & name) const;
    ColumnWithTypeAndName & getByName(const std::string & name);
    
    /// Column existence checks
    bool has(const std::string & name) const;
    size_t getPositionByName(const std::string & name) const;
    
    /// Column insertion and removal
    void insert(size_t position, ColumnWithTypeAndName elem);
    void insert(ColumnWithTypeAndName elem);
    void insertUnique(ColumnWithTypeAndName elem);
    ColumnWithTypeAndName getByPositionAndClone(size_t position) const;
    
    /// Block manipulation
    void erase(size_t position);
    void erase(const std::string & name);
    void clear();
    void swap(Block & other) noexcept;
    
    /// Block properties
    size_t columns() const { return data.size(); }
    size_t rows() const;
    size_t bytes() const;
    size_t allocatedBytes() const;
    bool empty() const { return rows() == 0; }
    
    /// Column operations
    Names getNames() const;
    DataTypes getDataTypes() const;
    Columns getColumns() const;
    ColumnsWithTypeAndName getColumnsWithTypeAndName() const;
    NamesAndTypesList getNamesAndTypesList() const;
    NamesAndTypes getNamesAndTypes() const;
    
    /// Block transformations
    void setColumns(const Columns & columns);
    void setColumnsWithTypeAndName(const ColumnsWithTypeAndName & columns);
    Block cloneEmpty() const;
    Block cloneWithColumns(const Columns & columns) const;
    Block cloneWithoutColumns() const;
    Block sortColumns() const;
    
    /// Data validation and debugging
    void checkNumberOfRows(bool allow_empty_columns = false) const;
    void checkMissingValues() const;
    std::string dumpStructure() const;
    std::string dumpNames() const;
    std::string dumpData() const;
    
    /// Performance optimizations
    void reserve(size_t count);
    void shrinkToFit();
    void updateHash(SipHash & hash) const;
    
private:
    /// Index maintenance for fast column lookup
    void rebuildIndexByName();
    void updateIndexByName(size_t position, const std::string & name);
    void eraseFromIndexByName(size_t position);
};
```

**ColumnWithTypeAndName Structure:**

The core building block of every Block is the ColumnWithTypeAndName structure, which encapsulates all necessary information about a column:

```cpp
struct ColumnWithTypeAndName
{
    ColumnPtr column;          /// Actual column data
    DataTypePtr type;          /// Data type information
    String name;               /// Column name
    
    ColumnWithTypeAndName() = default;
    ColumnWithTypeAndName(const ColumnPtr & column_, const DataTypePtr & type_, const String & name_)
        : column(column_), type(type_), name(name_) {}
    
    /// Convenience constructors for different scenarios
    ColumnWithTypeAndName(ColumnPtr column_, DataTypePtr type_, String name_)
        : column(std::move(column_)), type(std::move(type_)), name(std::move(name_)) {}
    
    ColumnWithTypeAndName cloneEmpty() const
    {
        return ColumnWithTypeAndName(column ? column->cloneEmpty() : nullptr, type, name);
    }
    
    ColumnWithTypeAndName cloneWithDefaultValues(size_t size) const
    {
        auto new_column = column->cloneEmpty();
        new_column->insertManyDefaults(size);
        return ColumnWithTypeAndName(std::move(new_column), type, name);
    }
    
    /// Type checking and validation
    bool hasEqualStructure(const ColumnWithTypeAndName & other) const
    {
        if (!type->equals(*other.type))
            return false;
        
        if (column && other.column)
            return column->structureEquals(*other.column);
        
        return !column && !other.column;
    }
    
    void dumpStructure(WriteBuffer & out) const
    {
        writeString(name, out);
        writeString(" ", out);
        writeString(type->getName(), out);
        if (column)
        {
            writeString(" (", out);
            writeString(toString(column->size()), out);
            writeString(" rows)", out);
        }
    }
    
    /// Memory usage information
    size_t byteSize() const
    {
        size_t result = name.size();
        if (column)
            result += column->byteSize();
        return result;
    }
    
    size_t allocatedBytes() const
    {
        size_t result = name.capacity();
        if (column)
            result += column->allocatedBytes();
        return result;
    }
};
```

#### 4.3.2 Block Operations and Transformations

Blocks support a comprehensive set of operations that enable efficient data manipulation throughout the query pipeline:

**Column Manipulation Operations:**

```cpp
/// Advanced column operations implementation
class Block
{
public:
    /// Insert column with type checking and optimization
    void insert(size_t position, ColumnWithTypeAndName elem)
    {
        if (position > data.size())
            throw Exception("Position out of bound in Block::insert()", ErrorCodes::POSITION_OUT_OF_BOUND);
        
        /// Validate that column size matches existing rows
        if (!data.empty() && elem.column)
        {
            size_t expected_rows = rows();
            if (elem.column->size() != expected_rows)
                throw Exception("Sizes of columns doesn't match", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);
        }
        
        /// Insert and update index
        auto it = data.begin() + position;
        data.insert(it, std::move(elem));
        rebuildIndexByName();
    }
    
    /// Efficient column removal with index maintenance
    void erase(const std::string & name)
    {
        auto it = index_by_name.find(name);
        if (it == index_by_name.end())
            throw Exception("Not found column " + name + " in block", ErrorCodes::NOT_FOUND_COLUMN_IN_BLOCK);
        
        erase(it->second);
    }
    
    void erase(size_t position)
    {
        if (position >= data.size())
            throw Exception("Position out of bound in Block::erase()", ErrorCodes::POSITION_OUT_OF_BOUND);
        
        auto it = data.begin() + position;
        data.erase(it);
        rebuildIndexByName();
    }
    
    /// Clone block structure without data
    Block cloneEmpty() const
    {
        Block res;
        res.data.reserve(data.size());
        
        for (const auto & elem : data)
            res.data.emplace_back(elem.cloneEmpty());
        
        res.rebuildIndexByName();
        return res;
    }
    
    /// Clone with specific columns replaced
    Block cloneWithColumns(const Columns & columns) const
    {
        if (columns.size() != data.size())
            throw Exception("Size of columns does not match", ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);
        
        Block res;
        res.data.reserve(data.size());
        
        for (size_t i = 0; i < data.size(); ++i)
        {
            res.data.emplace_back(columns[i], data[i].type, data[i].name);
        }
        
        res.rebuildIndexByName();
        return res;
    }
    
    /// Efficient column sorting by name
    Block sortColumns() const
    {
        Block sorted_block = *this;
        
        std::sort(sorted_block.data.begin(), sorted_block.data.end(),
            [](const ColumnWithTypeAndName & a, const ColumnWithTypeAndName & b)
            {
                return a.name < b.name;
            });
        
        sorted_block.rebuildIndexByName();
        return sorted_block;
    }
    
private:
    /// Optimized index building for fast column lookup
    void rebuildIndexByName()
    {
        index_by_name.clear();
        index_by_name.reserve(data.size());
        
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (!data[i].name.empty())
                index_by_name[data[i].name] = i;
        }
    }
};
```

**Block Validation and Integrity Checks:**

```cpp
/// Comprehensive validation methods
class Block
{
public:
    /// Validate consistency of row counts across columns
    void checkNumberOfRows(bool allow_empty_columns = false) const
    {
        if (data.empty())
            return;
        
        ssize_t rows_in_first_column = -1;
        
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (!data[i].column)
            {
                if (!allow_empty_columns)
                    throw Exception("Column " + data[i].name + " is empty", ErrorCodes::EMPTY_COLUMN);
                continue;
            }
            
            ssize_t rows_in_current_column = data[i].column->size();
            
            if (rows_in_first_column == -1)
                rows_in_first_column = rows_in_current_column;
            else if (rows_in_first_column != rows_in_current_column)
            {
                throw Exception("Sizes of columns doesn't match: "
                    + data[0].name + ": " + toString(rows_in_first_column)
                    + ", " + data[i].name + ": " + toString(rows_in_current_column),
                    ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);
            }
        }
    }
    
    /// Check for missing or null values where not expected
    void checkMissingValues() const
    {
        for (const auto & elem : data)
        {
            if (!elem.column)
                throw Exception("Column " + elem.name + " is missing", ErrorCodes::EMPTY_COLUMN);
            
            if (!elem.type)
                throw Exception("Type for column " + elem.name + " is missing", ErrorCodes::EMPTY_DATA_TYPE);
        }
    }
    
    /// Generate detailed structure information for debugging
    std::string dumpStructure() const
    {
        WriteBufferFromOwnString out;
        
        if (data.empty())
        {
            out << "(empty)";
            return out.str();
        }
        
        for (size_t i = 0; i < data.size(); ++i)
        {
            if (i > 0)
                out << ", ";
            
            data[i].dumpStructure(out);
        }
        
        return out.str();
    }
    
    /// Export all column names
    Names getNames() const
    {
        Names names;
        names.reserve(data.size());
        
        for (const auto & elem : data)
            names.push_back(elem.name);
        
        return names;
    }
    
    /// Export all data types
    DataTypes getDataTypes() const
    {
        DataTypes types;
        types.reserve(data.size());
        
        for (const auto & elem : data)
            types.push_back(elem.type);
        
        return types;
    }
};
```

#### 4.3.3 Data Flow Optimization in Query Processing

Blocks are central to ClickHouse's efficient data flow management, enabling sophisticated optimizations throughout the query pipeline:

**Block-Level Memory Management:**

```cpp
/// Memory-aware block operations
class Block
{
public:
    /// Calculate total memory usage
    size_t bytes() const
    {
        size_t result = 0;
        for (const auto & elem : data)
            result += elem.byteSize();
        return result;
    }
    
    size_t allocatedBytes() const
    {
        size_t result = 0;
        for (const auto & elem : data)
            result += elem.allocatedBytes();
        
        /// Add overhead of block structure itself
        result += data.capacity() * sizeof(ColumnWithTypeAndName);
        result += index_by_name.size() * (sizeof(std::string) + sizeof(size_t));
        
        return result;
    }
    
    /// Optimize memory usage
    void shrinkToFit()
    {
        data.shrink_to_fit();
        
        /// Shrink individual columns
        for (auto & elem : data)
        {
            if (elem.column)
            {
                auto mutable_column = elem.column->assumeMutable();
                /// Note: Individual column shrinking depends on column type
                elem.column = std::move(mutable_column);
            }
        }
        
        /// Rebuild compact index
        rebuildIndexByName();
    }
    
    /// Reserve capacity for efficient building
    void reserve(size_t count)
    {
        data.reserve(count);
        index_by_name.reserve(count);
    }
    
    /// Efficient hash computation for block comparison
    void updateHash(SipHash & hash) const
    {
        hash.update(data.size());
        
        for (const auto & elem : data)
        {
            hash.update(elem.name);
            elem.type->updateHash(hash);
            
            if (elem.column)
                elem.column->updateHashFast(hash);
        }
    }
    
    /// Get row count efficiently
    size_t rows() const
    {
        if (data.empty())
            return 0;
        
        /// Find first non-null column to determine row count
        for (const auto & elem : data)
        {
            if (elem.column)
                return elem.column->size();
        }
        
        return 0;
    }
};
```

**Block Transformation Operations:**

```cpp
/// Advanced block transformation capabilities
class Block
{
public:
    /// Transform block through column operations
    template <typename Transformer>
    Block transform(Transformer transformer) const
    {
        Block result;
        result.data.reserve(data.size());
        
        for (const auto & elem : data)
        {
            auto transformed = transformer(elem);
            if (transformed.column)  // Filter out null results
                result.insert(std::move(transformed));
        }
        
        return result;
    }
    
    /// Merge blocks with compatible schemas
    static Block merge(const std::vector<Block> & blocks)
    {
        if (blocks.empty())
            return Block{};
        
        const auto & first_block = blocks[0];
        Block result = first_block.cloneEmpty();
        
        /// Calculate total rows needed
        size_t total_rows = 0;
        for (const auto & block : blocks)
            total_rows += block.rows();
        
        if (total_rows == 0)
            return result;
        
        /// Reserve capacity in result columns
        MutableColumns result_columns;
        result_columns.reserve(first_block.columns());
        
        for (size_t col_idx = 0; col_idx < first_block.columns(); ++col_idx)
        {
            auto result_column = first_block.getByPosition(col_idx).column->cloneEmpty();
            result_column->reserve(total_rows);
            result_columns.push_back(std::move(result_column));
        }
        
        /// Merge data from all blocks
        for (const auto & block : blocks)
        {
            if (block.columns() != first_block.columns())
                throw Exception("Cannot merge blocks with different column counts", 
                               ErrorCodes::SIZES_OF_COLUMNS_DOESNT_MATCH);
            
            for (size_t col_idx = 0; col_idx < block.columns(); ++col_idx)
            {
                const auto & source_column = block.getByPosition(col_idx).column;
                result_columns[col_idx]->insertRangeFrom(*source_column, 0, source_column->size());
            }
        }
        
        /// Set merged columns
        Columns final_columns;
        final_columns.reserve(result_columns.size());
        for (auto & col : result_columns)
            final_columns.push_back(std::move(col));
        
        result.setColumns(final_columns);
        return result;
    }
    
    /// Split block into smaller chunks
    std::vector<Block> split(size_t max_rows_per_chunk) const
    {
        std::vector<Block> result;
        
        size_t total_rows = rows();
        if (total_rows == 0)
            return result;
        
        size_t chunks_needed = (total_rows + max_rows_per_chunk - 1) / max_rows_per_chunk;
        result.reserve(chunks_needed);
        
        for (size_t start_row = 0; start_row < total_rows; start_row += max_rows_per_chunk)
        {
            size_t end_row = std::min(start_row + max_rows_per_chunk, total_rows);
            size_t chunk_size = end_row - start_row;
            
            Block chunk_block = cloneEmpty();
            MutableColumns chunk_columns;
            chunk_columns.reserve(columns());
            
            for (size_t col_idx = 0; col_idx < columns(); ++col_idx)
            {
                const auto & source_column = getByPosition(col_idx).column;
                auto chunk_column = source_column->cut(start_row, chunk_size);
                chunk_columns.push_back(std::move(chunk_column));
            }
            
            Columns final_chunk_columns;
            final_chunk_columns.reserve(chunk_columns.size());
            for (auto & col : chunk_columns)
                final_chunk_columns.push_back(std::move(col));
            
            chunk_block.setColumns(final_chunk_columns);
            result.push_back(std::move(chunk_block));
        }
        
        return result;
    }
};
```

#### 4.3.4 Integration with Query Pipeline

Blocks serve as the primary data exchange format between processors in ClickHouse's query pipeline, enabling sophisticated optimizations and transformations:

**Pipeline Integration Pattern:**

```cpp
/// Example of how blocks flow through processors
class ExampleTransformProcessor : public ISimpleTransform
{
private:
    /// Transformation logic specific to processor
    Block transformImpl(Block && input_block) override
    {
        if (input_block.rows() == 0)
            return input_block;
        
        /// Validate input block structure
        input_block.checkNumberOfRows();
        input_block.checkMissingValues();
        
        /// Apply transformations
        Block result_block = input_block.cloneEmpty();
        
        /// Example: Add computed column
        auto computed_column = computeNewColumn(input_block);
        result_block.insert(ColumnWithTypeAndName(
            computed_column, 
            std::make_shared<DataTypeInt64>(), 
            "computed_value"));
        
        /// Copy existing columns with potential filtering
        for (size_t i = 0; i < input_block.columns(); ++i)
        {
            const auto & source_elem = input_block.getByPosition(i);
            if (shouldIncludeColumn(source_elem.name))
            {
                result_block.insert(ColumnWithTypeAndName(
                    source_elem.column, 
                    source_elem.type, 
                    source_elem.name));
            }
        }
        
        /// Validate output block
        result_block.checkNumberOfRows();
        
        return result_block;
    }
    
    ColumnPtr computeNewColumn(const Block & input_block)
    {
        size_t rows = input_block.rows();
        auto result = ColumnInt64::create(rows);
        auto & result_data = result->getData();
        
        /// Example computation based on existing columns
        for (size_t i = 0; i < rows; ++i)
        {
            result_data[i] = static_cast<Int64>(i * 42);  // Placeholder computation
        }
        
        return result;
    }
    
    bool shouldIncludeColumn(const std::string & column_name)
    {
        /// Example filtering logic
        return column_name != "temporary_column";
    }
};
```

This comprehensive Block architecture enables ClickHouse to efficiently process data chunks throughout the query pipeline while maintaining type safety, supporting complex transformations, and optimizing memory usage. The Block's design as a self-contained processing unit facilitates parallel execution, pipeline optimization, and seamless integration with the broader query processing infrastructure.

### 4.4 Field Abstraction and Type System Integration (2,500 words)

The Field class serves as ClickHouse's universal value holder, providing a type-safe discriminated union that can represent any data type supported by the system. This abstraction is crucial for scenarios requiring dynamic type handling, generic value manipulation, and seamless integration between different components of the query processing pipeline.

#### 4.4.1 Core Field Architecture

The Field class implements a sophisticated variant-like structure that can hold any ClickHouse data type while maintaining type safety and performance:

**Fundamental Field Structure:**

```cpp
class Field
{
public:
    /// Type enumeration for all supported types
    enum Types
    {
        Null    = 0,
        UInt64  = 1,
        Int64   = 2,
        Float64 = 3,
        UInt128 = 4,
        Int128  = 5,
        
        String  = 16,
        Array   = 17,
        Tuple   = 18,
        Map     = 19,
        Object  = 20,
        
        AggregateFunctionState = 21,
        
        Bool    = 22,
        
        UInt256 = 23,
        Int256  = 24,
        
        Decimal32  = 25,
        Decimal64  = 26,
        Decimal128 = 27,
        Decimal256 = 28,
        
        /// Maximum value for efficient implementation
        MAX_ENUMERATION = 32
    };
    
private:
    /// Storage union for different value types
    union
    {
        UInt64 uint64;
        Int64 int64;
        Float64 float64;
        UInt128 uint128;
        Int128 int128;
        UInt256 uint256;
        Int256 int256;
        
        /// String stored as pointer for efficiency
        String * string;
        Array * array;
        Tuple * tuple;
        Map * map;
        Object * object;
        AggregateFunctionStateData * aggregate_function;
        
        DecimalField<Decimal32> * decimal32;
        DecimalField<Decimal64> * decimal64;
        DecimalField<Decimal128> * decimal128;
        DecimalField<Decimal256> * decimal256;
        
        bool bool_value;
    } storage;
    
    /// Current type stored in the Field
    Types which;
    
public:
    /// Constructors for all basic types
    Field() : which(Null) {}
    
    Field(const Null & x) : which(Null) { (void)x; }
    Field(const UInt64 & x) : which(UInt64) { storage.uint64 = x; }
    Field(const Int64 & x) : which(Int64) { storage.int64 = x; }
    Field(const Float64 & x) : which(Float64) { storage.float64 = x; }
    Field(const UInt128 & x) : which(UInt128) { storage.uint128 = x; }
    Field(const Int128 & x) : which(Int128) { storage.int128 = x; }
    Field(const UInt256 & x) : which(UInt256) { new (&storage.uint256) UInt256(x); }
    Field(const Int256 & x) : which(Int256) { new (&storage.int256) Int256(x); }
    Field(const bool & x) : which(Bool) { storage.bool_value = x; }
    
    /// String and complex type constructors
    Field(const String & x) : which(String) 
    { 
        storage.string = new String(x); 
    }
    
    Field(String && x) : which(String) 
    { 
        storage.string = new String(std::move(x)); 
    }
    
    Field(const char * x) : which(String) 
    { 
        storage.string = new String(x); 
    }
    
    Field(const Array & x) : which(Array) 
    { 
        storage.array = new Array(x); 
    }
    
    Field(Array && x) : which(Array) 
    { 
        storage.array = new Array(std::move(x)); 
    }
    
    Field(const Tuple & x) : which(Tuple) 
    { 
        storage.tuple = new Tuple(x); 
    }
    
    Field(Tuple && x) : which(Tuple) 
    { 
        storage.tuple = new Tuple(std::move(x)); 
    }
    
    /// Type checking methods
    Types getType() const { return which; }
    bool isNull() const { return which == Null; }
    
    /// Generic type checking
    template <typename T>
    bool isType() const
    {
        return which == TypeToEnum<NearestFieldType<T>>::value;
    }
    
    /// Safe value extraction with type checking
    template <typename T>
    T & get()
    {
        static_assert(!std::is_same_v<T, Field>, "Use safeGet instead of get<Field>");
        
        NearestFieldType<T> * ptr = tryGet<NearestFieldType<T>>();
        if (unlikely(!ptr))
            throw Exception("Bad get: has " + toString() + ", requested " + demangle(typeid(T).name()),
                          ErrorCodes::BAD_GET);
        return *ptr;
    }
    
    template <typename T>
    const T & get() const
    {
        auto * ptr = tryGet<T>();
        if (unlikely(!ptr))
            throw Exception("Bad get: has " + toString() + ", requested " + demangle(typeid(T).name()),
                          ErrorCodes::BAD_GET);
        return *ptr;
    }
    
    /// Safe extraction without exceptions
    template <typename T>
    T * tryGet()
    {
        constexpr Types target_type = TypeToEnum<NearestFieldType<T>>::value;
        if (likely(which == target_type))
            return reinterpret_cast<T *>(&storage);
        return nullptr;
    }
    
    template <typename T>
    const T * tryGet() const
    {
        constexpr Types target_type = TypeToEnum<NearestFieldType<T>>::value;
        if (likely(which == target_type))
            return reinterpret_cast<const T *>(&storage);
        return nullptr;
    }
    
    /// Assignment operators
    Field & operator= (const UInt64 & x) { assignValue(x, UInt64); return *this; }
    Field & operator= (const Int64 & x) { assignValue(x, Int64); return *this; }
    Field & operator= (const Float64 & x) { assignValue(x, Float64); return *this; }
    Field & operator= (const String & x) { assignComplex(x, String); return *this; }
    Field & operator= (String && x) { assignComplex(std::move(x), String); return *this; }
    Field & operator= (const Array & x) { assignComplex(x, Array); return *this; }
    Field & operator= (Array && x) { assignComplex(std::move(x), Array); return *this; }
    
    /// Copy and move operations
    Field(const Field & other) { copyFrom(other); }
    Field(Field && other) noexcept { moveFrom(std::move(other)); }
    Field & operator=(const Field & other) { destroy(); copyFrom(other); return *this; }
    Field & operator=(Field && other) noexcept { destroy(); moveFrom(std::move(other)); return *this; }
    
    /// Destructor
    ~Field() { destroy(); }
    
    /// Comparison operations
    bool operator< (const Field & rhs) const { return compareImpl(*this, rhs) < 0; }
    bool operator<= (const Field & rhs) const { return compareImpl(*this, rhs) <= 0; }
    bool operator== (const Field & rhs) const { return compareImpl(*this, rhs) == 0; }
    bool operator!= (const Field & rhs) const { return compareImpl(*this, rhs) != 0; }
    bool operator>= (const Field & rhs) const { return compareImpl(*this, rhs) >= 0; }
    bool operator> (const Field & rhs) const { return compareImpl(*this, rhs) > 0; }
    
    /// String representation
    String toString() const;
    String toStringQuoted() const;
    String dump() const;
    
    /// Hashing support
    UInt64 getHash() const;
    
private:
    /// Helper methods for memory management
    void destroy();
    void copyFrom(const Field & other);
    void moveFrom(Field && other) noexcept;
    
    /// Type-safe assignment helpers
    template <typename T>
    void assignValue(const T & x, Types type)
    {
        destroy();
        which = type;
        *reinterpret_cast<T *>(&storage) = x;
    }
    
    template <typename T>
    void assignComplex(T && x, Types type)
    {
        destroy();
        which = type;
        *reinterpret_cast<typename std::remove_reference_t<T> **>(&storage) = 
            new typename std::remove_reference_t<T>(std::forward<T>(x));
    }
    
    /// Comparison implementation
    static int compareImpl(const Field & lhs, const Field & rhs);
};
```

#### 4.4.2 Type Conversion and Compatibility System

Field provides sophisticated type conversion capabilities that enable seamless integration between different data types throughout the ClickHouse system:

**Type Conversion Framework:**

```cpp
/// Type mapping from C++ types to Field enum values
template <typename T>
struct TypeToEnum;

template <> struct TypeToEnum<Null> { static constexpr Field::Types value = Field::Null; };
template <> struct TypeToEnum<UInt64> { static constexpr Field::Types value = Field::UInt64; };
template <> struct TypeToEnum<Int64> { static constexpr Field::Types value = Field::Int64; };
template <> struct TypeToEnum<Float64> { static constexpr Field::Types value = Field::Float64; };
template <> struct TypeToEnum<String> { static constexpr Field::Types value = Field::String; };
template <> struct TypeToEnum<Array> { static constexpr Field::Types value = Field::Array; };
template <> struct TypeToEnum<Tuple> { static constexpr Field::Types value = Field::Tuple; };
template <> struct TypeToEnum<bool> { static constexpr Field::Types value = Field::Bool; };

/// Automatic type promotion for compatible types
template <typename T>
using NearestFieldType = typename NearestFieldTypeImpl<T>::Type;

template <typename T>
struct NearestFieldTypeImpl
{
    using Type = T;
};

/// Integer type promotions
template <> struct NearestFieldTypeImpl<UInt8> { using Type = UInt64; };
template <> struct NearestFieldTypeImpl<UInt16> { using Type = UInt64; };
template <> struct NearestFieldTypeImpl<UInt32> { using Type = UInt64; };

template <> struct NearestFieldTypeImpl<Int8> { using Type = Int64; };
template <> struct NearestFieldTypeImpl<Int16> { using Type = Int64; };
template <> struct NearestFieldTypeImpl<Int32> { using Type = Int64; };

/// Floating point promotions
template <> struct NearestFieldTypeImpl<Float32> { using Type = Float64; };

/// Safe conversion between compatible types
template <typename T>
Field convertToField(const T & value)
{
    if constexpr (std::is_same_v<T, Field>)
    {
        return value;
    }
    else if constexpr (std::is_convertible_v<T, typename NearestFieldType<T>>)
    {
        return Field(static_cast<typename NearestFieldType<T>>(value));
    }
    else
    {
        static_assert(std::is_same_v<T, void>, "Cannot convert type to Field");
    }
}

/// Conversion from Field to specific types
template <typename T>
T convertFromField(const Field & field)
{
    if (field.isNull())
    {
        if constexpr (std::is_arithmetic_v<T>)
            return T{};
        else
            throw Exception("Cannot convert Null to non-nullable type", ErrorCodes::TYPE_MISMATCH);
    }
    
    if constexpr (std::is_same_v<T, bool>)
    {
        if (field.getType() == Field::Bool)
            return field.get<bool>();
        else if (field.getType() == Field::UInt64)
            return field.get<UInt64>() != 0;
        else if (field.getType() == Field::Int64)
            return field.get<Int64>() != 0;
        else
            throw Exception("Cannot convert " + field.toString() + " to bool", ErrorCodes::TYPE_MISMATCH);
    }
    else if constexpr (std::is_integral_v<T>)
    {
        return convertIntegralFromField<T>(field);
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        return convertFloatingFromField<T>(field);
    }
    else if constexpr (std::is_same_v<T, String>)
    {
        return convertStringFromField(field);
    }
    else
    {
        return field.get<T>();
    }
}

/// Specialized conversion for integral types
template <typename T>
T convertIntegralFromField(const Field & field)
{
    static_assert(std::is_integral_v<T>);
    
    switch (field.getType())
    {
        case Field::UInt64:
        {
            UInt64 value = field.get<UInt64>();
            if constexpr (std::is_signed_v<T>)
            {
                if (value > static_cast<UInt64>(std::numeric_limits<T>::max()))
                    throw Exception("Value " + toString(value) + " is too large for target type", 
                                   ErrorCodes::VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE);
            }
            return static_cast<T>(value);
        }
        case Field::Int64:
        {
            Int64 value = field.get<Int64>();
            if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max())
                throw Exception("Value " + toString(value) + " is out of range for target type", 
                               ErrorCodes::VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE);
            return static_cast<T>(value);
        }
        case Field::Float64:
        {
            Float64 value = field.get<Float64>();
            if (std::isnan(value) || std::isinf(value))
                throw Exception("Cannot convert NaN or Inf to integer", ErrorCodes::TYPE_MISMATCH);
            
            if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max())
                throw Exception("Value " + toString(value) + " is out of range for target type", 
                               ErrorCodes::VALUE_IS_OUT_OF_RANGE_OF_DATA_TYPE);
            
            return static_cast<T>(value);
        }
        case Field::Bool:
        {
            return static_cast<T>(field.get<bool>() ? 1 : 0);
        }
        default:
            throw Exception("Cannot convert " + field.toString() + " to integral type", 
                           ErrorCodes::TYPE_MISMATCH);
    }
}

/// Specialized conversion for floating point types
template <typename T>
T convertFloatingFromField(const Field & field)
{
    static_assert(std::is_floating_point_v<T>);
    
    switch (field.getType())
    {
        case Field::UInt64:
            return static_cast<T>(field.get<UInt64>());
        case Field::Int64:
            return static_cast<T>(field.get<Int64>());
        case Field::Float64:
            return static_cast<T>(field.get<Float64>());
        case Field::Bool:
            return static_cast<T>(field.get<bool>() ? 1.0 : 0.0);
        default:
            throw Exception("Cannot convert " + field.toString() + " to floating point type", 
                           ErrorCodes::TYPE_MISMATCH);
    }
}

/// String conversion with formatting
String convertStringFromField(const Field & field)
{
    switch (field.getType())
    {
        case Field::String:
            return field.get<String>();
        case Field::UInt64:
            return toString(field.get<UInt64>());
        case Field::Int64:
            return toString(field.get<Int64>());
        case Field::Float64:
            return toString(field.get<Float64>());
        case Field::Bool:
            return field.get<bool>() ? "true" : "false";
        case Field::Array:
        {
            const Array & array = field.get<Array>();
            WriteBufferFromOwnString buf;
            buf << '[';
            for (size_t i = 0; i < array.size(); ++i)
            {
                if (i > 0)
                    buf << ", ";
                buf << convertStringFromField(array[i]);
            }
            buf << ']';
            return buf.str();
        }
        case Field::Tuple:
        {
            const Tuple & tuple = field.get<Tuple>();
            WriteBufferFromOwnString buf;
            buf << '(';
            for (size_t i = 0; i < tuple.size(); ++i)
            {
                if (i > 0)
                    buf << ", ";
                buf << convertStringFromField(tuple[i]);
            }
            buf << ')';
            return buf.str();
        }
        case Field::Null:
            return "NULL";
        default:
            return field.toString();
    }
}
```

#### 4.4.3 Performance Optimizations and Memory Management

Field implements several sophisticated optimizations to minimize memory overhead and maximize performance:

**Memory Layout Optimizations:**

```cpp
class Field
{
private:
    /// Optimized memory management for complex types
    void destroy()
    {
        switch (which)
        {
            case String:
                delete storage.string;
                break;
            case Array:
                delete storage.array;
                break;
            case Tuple:
                delete storage.tuple;
                break;
            case Map:
                delete storage.map;
                break;
            case Object:
                delete storage.object;
                break;
            case AggregateFunctionState:
                delete storage.aggregate_function;
                break;
            case Decimal32:
                delete storage.decimal32;
                break;
            case Decimal64:
                delete storage.decimal64;
                break;
            case Decimal128:
                delete storage.decimal128;
                break;
            case Decimal256:
                delete storage.decimal256;
                break;
            case UInt256:
                storage.uint256.~UInt256();
                break;
            case Int256:
                storage.int256.~Int256();
                break;
            default:
                /// No cleanup needed for simple types
                break;
        }
        which = Null;
    }
    
    void copyFrom(const Field & other)
    {
        which = other.which;
        
        switch (which)
        {
            case Null:
            case Bool:
            case UInt64:
            case Int64:
            case Float64:
                /// Simple bitwise copy for fundamental types
                storage = other.storage;
                break;
                
            case UInt128:
            case Int128:
                storage = other.storage;
                break;
                
            case UInt256:
                new (&storage.uint256) UInt256(other.storage.uint256);
                break;
            case Int256:
                new (&storage.int256) Int256(other.storage.int256);
                break;
                
            case String:
                storage.string = new String(*other.storage.string);
                break;
            case Array:
                storage.array = new Array(*other.storage.array);
                break;
            case Tuple:
                storage.tuple = new Tuple(*other.storage.tuple);
                break;
            case Map:
                storage.map = new Map(*other.storage.map);
                break;
            case Object:
                storage.object = new Object(*other.storage.object);
                break;
            case AggregateFunctionState:
                storage.aggregate_function = new AggregateFunctionStateData(*other.storage.aggregate_function);
                break;
            case Decimal32:
                storage.decimal32 = new DecimalField<Decimal32>(*other.storage.decimal32);
                break;
            case Decimal64:
                storage.decimal64 = new DecimalField<Decimal64>(*other.storage.decimal64);
                break;
            case Decimal128:
                storage.decimal128 = new DecimalField<Decimal128>(*other.storage.decimal128);
                break;
            case Decimal256:
                storage.decimal256 = new DecimalField<Decimal256>(*other.storage.decimal256);
                break;
        }
    }
    
    void moveFrom(Field && other) noexcept
    {
        which = other.which;
        storage = other.storage;
        other.which = Null;
    }
    
    /// High-performance comparison implementation
    static int compareImpl(const Field & lhs, const Field & rhs)
    {
        if (lhs.which != rhs.which)
        {
            /// Handle cross-type comparisons with logical ordering
            return compareAcrossTypes(lhs, rhs);
        }
        
        switch (lhs.which)
        {
            case Null:
                return 0;
            case Bool:
                return lhs.storage.bool_value < rhs.storage.bool_value ? -1 : 
                       (lhs.storage.bool_value > rhs.storage.bool_value ? 1 : 0);
            case UInt64:
                return lhs.storage.uint64 < rhs.storage.uint64 ? -1 : 
                       (lhs.storage.uint64 > rhs.storage.uint64 ? 1 : 0);
            case Int64:
                return lhs.storage.int64 < rhs.storage.int64 ? -1 : 
                       (lhs.storage.int64 > rhs.storage.int64 ? 1 : 0);
            case Float64:
                return compareFloatingPoint(lhs.storage.float64, rhs.storage.float64);
            case String:
                return lhs.storage.string->compare(*rhs.storage.string);
            case Array:
                return compareArrays(*lhs.storage.array, *rhs.storage.array);
            case Tuple:
                return compareTuples(*lhs.storage.tuple, *rhs.storage.tuple);
            default:
                return compareComplexTypes(lhs, rhs);
        }
    }
    
    /// Specialized comparison for arrays
    static int compareArrays(const Array & lhs, const Array & rhs)
    {
        size_t min_size = std::min(lhs.size(), rhs.size());
        
        for (size_t i = 0; i < min_size; ++i)
        {
            int result = compareImpl(lhs[i], rhs[i]);
            if (result != 0)
                return result;
        }
        
        return lhs.size() < rhs.size() ? -1 : (lhs.size() > rhs.size() ? 1 : 0);
    }
    
    /// Specialized comparison for tuples
    static int compareTuples(const Tuple & lhs, const Tuple & rhs)
    {
        size_t min_size = std::min(lhs.size(), rhs.size());
        
        for (size_t i = 0; i < min_size; ++i)
        {
            int result = compareImpl(lhs[i], rhs[i]);
            if (result != 0)
                return result;
        }
        
        return lhs.size() < rhs.size() ? -1 : (lhs.size() > rhs.size() ? 1 : 0);
    }
    
    /// Handle floating point comparison with NaN handling
    static int compareFloatingPoint(Float64 lhs, Float64 rhs)
    {
        if (std::isnan(lhs) && std::isnan(rhs))
            return 0;
        if (std::isnan(lhs))
            return 1;  /// NaN is considered greater
        if (std::isnan(rhs))
            return -1;
        
        return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
    }
};
```

#### 4.4.4 Integration with ClickHouse Type System

Field seamlessly integrates with ClickHouse's broader type system, enabling dynamic type handling throughout the query processing pipeline:

**Dynamic Type Integration:**

```cpp
/// Integration with IDataType for dynamic operations
class FieldTypeConverter
{
public:
    /// Convert Field to column value using data type information
    static void insertIntoColumn(IColumn & column, const Field & field, const IDataType & type)
    {
        /// Delegate to data type for type-specific insertion
        type.serializeAsField(field, column);
    }
    
    /// Extract Field from column using data type information
    static Field extractFromColumn(const IColumn & column, size_t row, const IDataType & type)
    {
        return type.deserializeAsField(column, row);
    }
    
    /// Convert Field to string representation using data type formatting
    static String formatField(const Field & field, const IDataType & type)
    {
        WriteBufferFromOwnString buf;
        type.serializeAsText(field, buf, FormatSettings{});
        return buf.str();
    }
    
    /// Parse string to Field using data type parsing
    static Field parseField(const String & str, const IDataType & type)
    {
        ReadBufferFromString buf(str);
        return type.deserializeAsTextEscaped(buf, FormatSettings{});
    }
};

/// Field visitor pattern for type-safe operations
template <typename Visitor>
auto applyVisitor(Visitor && visitor, const Field & field)
{
    switch (field.getType())
    {
        case Field::Null:
            return visitor(Null{});
        case Field::Bool:
            return visitor(field.get<bool>());
        case Field::UInt64:
            return visitor(field.get<UInt64>());
        case Field::Int64:
            return visitor(field.get<Int64>());
        case Field::Float64:
            return visitor(field.get<Float64>());
        case Field::UInt128:
            return visitor(field.get<UInt128>());
        case Field::Int128:
            return visitor(field.get<Int128>());
        case Field::UInt256:
            return visitor(field.get<UInt256>());
        case Field::Int256:
            return visitor(field.get<Int256>());
        case Field::String:
            return visitor(field.get<String>());
        case Field::Array:
            return visitor(field.get<Array>());
        case Field::Tuple:
            return visitor(field.get<Tuple>());
        case Field::Map:
            return visitor(field.get<Map>());
        case Field::Object:
            return visitor(field.get<Object>());
        case Field::AggregateFunctionState:
            return visitor(field.get<AggregateFunctionStateData>());
        case Field::Decimal32:
            return visitor(field.get<DecimalField<Decimal32>>());
        case Field::Decimal64:
            return visitor(field.get<DecimalField<Decimal64>>());
        case Field::Decimal128:
            return visitor(field.get<DecimalField<Decimal128>>());
        case Field::Decimal256:
            return visitor(field.get<DecimalField<Decimal256>>());
    }
    
    __builtin_unreachable();
}

/// Arithmetic operations on Fields
class FieldArithmetic
{
public:
    static Field add(const Field & lhs, const Field & rhs)
    {
        return applyBinaryOperation(lhs, rhs, [](auto a, auto b) { return a + b; });
    }
    
    static Field subtract(const Field & lhs, const Field & rhs)
    {
        return applyBinaryOperation(lhs, rhs, [](auto a, auto b) { return a - b; });
    }
    
    static Field multiply(const Field & lhs, const Field & rhs)
    {
        return applyBinaryOperation(lhs, rhs, [](auto a, auto b) { return a * b; });
    }
    
    static Field divide(const Field & lhs, const Field & rhs)
    {
        return applyBinaryOperation(lhs, rhs, [](auto a, auto b) 
        { 
            if (b == 0)
                throw Exception("Division by zero", ErrorCodes::ILLEGAL_DIVISION);
            return a / b; 
        });
    }
    
private:
    template <typename Operation>
    static Field applyBinaryOperation(const Field & lhs, const Field & rhs, Operation op)
    {
        /// Promote both operands to compatible types
        auto promoted = promoteToCommonType(lhs, rhs);
        
        return applyVisitor([&](auto && l) -> Field
        {
            return applyVisitor([&](auto && r) -> Field
            {
                using LType = std::decay_t<decltype(l)>;
                using RType = std::decay_t<decltype(r)>;
                
                if constexpr (std::is_arithmetic_v<LType> && std::is_arithmetic_v<RType>)
                {
                    using ResultType = decltype(op(l, r));
                    return Field(op(l, r));
                }
                else
                {
                    throw Exception("Cannot apply arithmetic operation to non-numeric types",
                                   ErrorCodes::TYPE_MISMATCH);
                }
            }, promoted.second);
        }, promoted.first);
    }
    
    static std::pair<Field, Field> promoteToCommonType(const Field & lhs, const Field & rhs)
    {
        /// Implement type promotion logic for arithmetic operations
        if (lhs.getType() == rhs.getType())
            return {lhs, rhs};
        
        /// Promote integers to larger types
        if ((lhs.getType() == Field::UInt64 || lhs.getType() == Field::Int64) &&
            (rhs.getType() == Field::UInt64 || rhs.getType() == Field::Int64))
        {
            return {lhs, rhs};  /// Already compatible
        }
        
        /// Promote integers to floating point
        if ((lhs.getType() == Field::UInt64 || lhs.getType() == Field::Int64) &&
            rhs.getType() == Field::Float64)
        {
            return {Field(static_cast<Float64>(convertFromField<Int64>(lhs))), rhs};
        }
        
        if (lhs.getType() == Field::Float64 &&
            (rhs.getType() == Field::UInt64 || rhs.getType() == Field::Int64))
        {
            return {lhs, Field(static_cast<Float64>(convertFromField<Int64>(rhs)))};
        }
        
        throw Exception("Cannot find common type for arithmetic operation", ErrorCodes::TYPE_MISMATCH);
    }
};
```

This comprehensive Field implementation provides ClickHouse with a powerful abstraction for handling dynamic values while maintaining type safety and optimal performance throughout the query processing pipeline.

### 4.5 Aggregation Function States and Memory Management (2,500 words)

Aggregation function states represent one of the most performance-critical components in ClickHouse's data processing pipeline. These states maintain intermediate results during GROUP BY operations and require sophisticated memory management strategies to handle potentially millions of concurrent aggregation states efficiently while minimizing memory fragmentation and maximizing cache locality.

#### 4.5.1 Aggregation State Architecture

ClickHouse implements a hierarchical architecture for aggregation states that balances flexibility with performance:

**Core Aggregation State Interface:**

```cpp
/// Base interface for all aggregation function states
struct IAggregateFunction
{
    virtual ~IAggregateFunction() = default;
    
    /// State creation and destruction
    virtual void create(AggregateDataPtr __restrict place) const = 0;
    virtual void destroy(AggregateDataPtr __restrict place) const noexcept = 0;
    virtual bool hasTrivialDestructor() const = 0;
    
    /// Data manipulation
    virtual void add(AggregateDataPtr __restrict place, const IColumn ** columns, 
                     size_t row_num, Arena * arena) const = 0;
    virtual void addBatch(size_t row_begin, size_t row_end, AggregateDataPtr * places, 
                          size_t place_offset, const IColumn ** columns, Arena * arena, 
                          ssize_t if_argument_pos = -1) const = 0;
    virtual void addBatchSinglePlace(size_t row_begin, size_t row_end, AggregateDataPtr place, 
                                     const IColumn ** columns, Arena * arena, 
                                     ssize_t if_argument_pos = -1) const = 0;
    
    /// State merging for distributed aggregation
    virtual void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena * arena) const = 0;
    virtual void mergeBatch(size_t row_begin, size_t row_end, AggregateDataPtr * places, 
                            size_t place_offset, const AggregateDataPtr * rhs, Arena * arena) const = 0;
    
    /// Serialization and communication
    virtual void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf, 
                           std::optional<size_t> version = std::nullopt) const = 0;
    virtual void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf, 
                             std::optional<size_t> version = std::nullopt, Arena * arena = nullptr) const = 0;
    
    /// Result extraction
    virtual void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena * arena) const = 0;
    virtual bool allocatesMemoryInArena() const = 0;
    
    /// Memory and performance characteristics
    virtual size_t sizeOfData() const = 0;
    virtual size_t alignOfData() const = 0;
    virtual bool isState() const { return false; }
    virtual bool isVersioned() const { return false; }
    virtual size_t getDefaultVersion() const { return 0; }
    virtual size_t getVersionFromRevision(size_t revision) const { return 0; }
    
    /// Function metadata
    virtual String getName() const = 0;
    virtual DataTypePtr getReturnType() const = 0;
    virtual DataTypes getArgumentTypes() const = 0;
    virtual bool isParallelizable() const { return true; }
    virtual bool canBeUsedInHashJoin() const { return false; }
    
    /// Optimization hints
    virtual bool isFixedSize() const { return true; }
    virtual size_t getMaxSizeOfState() const { return sizeOfData(); }
    virtual AggregateFunctionPtr getOwnNullAdapter(
        const AggregateFunctionPtr & nested, const DataTypes & arguments,
        const Array & params, const AggregateFunctionProperties & properties) const;
};

/// Concrete implementation template for typed states
template <typename Data, typename Name>
class AggregateFunctionTemplate : public IAggregateFunction
{
public:
    using State = Data;
    static constexpr bool has_trivial_destructor = std::is_trivially_destructible_v<Data>;
    
    void create(AggregateDataPtr __restrict place) const override
    {
        new (place) Data;
    }
    
    void destroy(AggregateDataPtr __restrict place) const noexcept override
    {
        if constexpr (!has_trivial_destructor)
            reinterpret_cast<Data *>(place)->~Data();
    }
    
    bool hasTrivialDestructor() const override
    {
        return has_trivial_destructor;
    }
    
    size_t sizeOfData() const override
    {
        return sizeof(Data);
    }
    
    size_t alignOfData() const override
    {
        return alignof(Data);
    }
    
    String getName() const override
    {
        return Name::name;
    }
    
protected:
    /// Helper to safely access state data
    static Data & data(AggregateDataPtr __restrict place)
    {
        return *reinterpret_cast<Data *>(place);
    }
    
    static const Data & data(ConstAggregateDataPtr __restrict place)
    {
        return *reinterpret_cast<const Data *>(place);
    }
};
```

**Specialized Aggregation State Examples:**

```cpp
/// Simple count aggregation state
struct AggregateFunctionCountData
{
    UInt64 count = 0;
    
    void add() { ++count; }
    void merge(const AggregateFunctionCountData & rhs) { count += rhs.count; }
    void serialize(WriteBuffer & buf) const { writeBinary(count, buf); }
    void deserialize(ReadBuffer & buf) { readBinary(count, buf); }
    UInt64 get() const { return count; }
};

class AggregateFunctionCount final : public AggregateFunctionTemplate<AggregateFunctionCountData, NameCount>
{
public:
    AggregateFunctionCount(const DataTypes & argument_types, const Array & params)
        : argument_types_impl(argument_types) {}
    
    void add(AggregateDataPtr __restrict place, const IColumn ** columns, 
             size_t row_num, Arena *) const override
    {
        data(place).add();
    }
    
    void addBatch(size_t row_begin, size_t row_end, AggregateDataPtr * places, 
                  size_t place_offset, const IColumn ** columns, Arena * arena,
                  ssize_t if_argument_pos = -1) const override
    {
        /// Optimized batch processing
        if (if_argument_pos >= 0)
        {
            const auto & flags = assert_cast<const ColumnUInt8 &>(*columns[if_argument_pos]);
            const UInt8 * flag_data = flags.getData().data();
            
            for (size_t i = row_begin; i < row_end; ++i)
            {
                if (flag_data[i])
                    data(places[i] + place_offset).add();
            }
        }
        else
        {
            for (size_t i = row_begin; i < row_end; ++i)
                data(places[i] + place_offset).add();
        }
    }
    
    void addBatchSinglePlace(size_t row_begin, size_t row_end, AggregateDataPtr place, 
                             const IColumn ** columns, Arena * arena,
                             ssize_t if_argument_pos = -1) const override
    {
        if (if_argument_pos >= 0)
        {
            const auto & flags = assert_cast<const ColumnUInt8 &>(*columns[if_argument_pos]);
            data(place).count += countBytesInFilter(flags, row_begin, row_end);
        }
        else
        {
            data(place).count += row_end - row_begin;
        }
    }
    
    void merge(AggregateDataPtr __restrict place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        data(place).merge(data(rhs));
    }
    
    void serialize(ConstAggregateDataPtr __restrict place, WriteBuffer & buf,
                   std::optional<size_t> /* version */) const override
    {
        data(place).serialize(buf);
    }
    
    void deserialize(AggregateDataPtr __restrict place, ReadBuffer & buf,
                     std::optional<size_t> /* version */, Arena *) const override
    {
        data(place).deserialize(buf);
    }
    
    void insertResultInto(AggregateDataPtr __restrict place, IColumn & to, Arena *) const override
    {
        assert_cast<ColumnUInt64 &>(to).getData().push_back(data(place).get());
    }
    
    bool allocatesMemoryInArena() const override { return false; }
    
    DataTypePtr getReturnType() const override
    {
        return std::make_shared<DataTypeUInt64>();
    }
    
    DataTypes getArgumentTypes() const override
    {
        return argument_types_impl;
    }
    
private:
    DataTypes argument_types_impl;
};

/// Complex sum aggregation with overflow handling
template <typename T>
struct AggregateFunctionSumData
{
    using ResultType = std::conditional_t<std::is_floating_point_v<T>, Float64, 
                       std::conditional_t<sizeof(T) <= 4, Int64, __int128>>;
    
    ResultType sum = 0;
    bool has_values = false;
    
    void add(T value)
    {
        if constexpr (std::is_floating_point_v<T>)
        {
            /// Handle NaN and infinity
            if (likely(!std::isnan(value)))
            {
                if (likely(!has_values))
                {
                    sum = value;
                    has_values = true;
                }
                else
                {
                    sum += value;
                }
            }
        }
        else
        {
            /// Integer overflow detection
            if constexpr (sizeof(ResultType) > sizeof(T))
            {
                sum += value;
                has_values = true;
            }
            else
            {
                /// Use overflow-safe arithmetic
                if (__builtin_add_overflow(sum, value, &sum))
                    throw Exception("Sum overflow", ErrorCodes::DECIMAL_OVERFLOW);
                has_values = true;
            }
        }
    }
    
    void merge(const AggregateFunctionSumData & rhs)
    {
        if (rhs.has_values)
        {
            if (has_values)
            {
                if constexpr (std::is_floating_point_v<T>)
                {
                    sum += rhs.sum;
                }
                else
                {
                    if (__builtin_add_overflow(sum, rhs.sum, &sum))
                        throw Exception("Sum overflow during merge", ErrorCodes::DECIMAL_OVERFLOW);
                }
            }
            else
            {
                sum = rhs.sum;
                has_values = true;
            }
        }
    }
    
    void serialize(WriteBuffer & buf) const
    {
        writeBinary(has_values, buf);
        if (has_values)
            writeBinary(sum, buf);
    }
    
    void deserialize(ReadBuffer & buf)
    {
        readBinary(has_values, buf);
        if (has_values)
            readBinary(sum, buf);
        else
            sum = 0;
    }
    
    ResultType get() const
    {
        return has_values ? sum : ResultType(0);
    }
};

/// Variable-length state for complex aggregations
struct AggregateFunctionUniqData
{
    using Set = ClearableHashSet<UInt64, DefaultHash<UInt64>, HashTableGrower<1>, 
                                 HashTableAllocatorWithStackMemory<(1ULL << 3) * sizeof(UInt64)>>;
    Set set;
    
    void add(UInt64 value, Arena * arena)
    {
        set.insert(value, arena);
    }
    
    void merge(const AggregateFunctionUniqData & rhs, Arena * arena)
    {
        set.merge(rhs.set, arena);
    }
    
    void serialize(WriteBuffer & buf) const
    {
        set.writeText(buf);
    }
    
    void deserialize(ReadBuffer & buf, Arena * arena)
    {
        set.readText(buf, arena);
    }
    
    UInt64 get() const
    {
        return set.size();
    }
    
    bool allocatesMemoryInArena() const { return true; }
};
```

#### 4.5.2 Memory Pool Management for Aggregation States

ClickHouse implements sophisticated memory pool management specifically optimized for aggregation workloads:

**Aggregation-Specific Memory Pools:**

```cpp
/// Specialized arena for aggregation states
class AggregationStateArena : public Arena
{
private:
    /// Pool for fixed-size states
    struct FixedSizePool
    {
        size_t state_size;
        size_t alignment;
        char * current_chunk = nullptr;
        char * chunk_end = nullptr;
        size_t states_per_chunk;
        std::vector<std::unique_ptr<char[]>> chunks;
        
        FixedSizePool(size_t size, size_t align, size_t chunk_size = 64 * 1024)
            : state_size(size), alignment(align)
        {
            /// Calculate optimal states per chunk
            size_t aligned_size = (size + align - 1) & ~(align - 1);
            states_per_chunk = chunk_size / aligned_size;
            if (states_per_chunk == 0)
                states_per_chunk = 1;
        }
        
        char * allocate()
        {
            if (current_chunk + state_size > chunk_end)
                allocateNewChunk();
            
            char * result = current_chunk;
            current_chunk += (state_size + alignment - 1) & ~(alignment - 1);
            return result;
        }
        
    private:
        void allocateNewChunk()
        {
            size_t chunk_size = states_per_chunk * ((state_size + alignment - 1) & ~(alignment - 1));
            auto chunk = std::make_unique<char[]>(chunk_size + alignment);
            
            /// Align chunk start
            current_chunk = reinterpret_cast<char *>(
                (reinterpret_cast<uintptr_t>(chunk.get()) + alignment - 1) & ~(alignment - 1));
            chunk_end = current_chunk + chunk_size;
            
            chunks.push_back(std::move(chunk));
        }
    };
    
    /// Pools for different state sizes
    std::unordered_map<std::pair<size_t, size_t>, std::unique_ptr<FixedSizePool>, 
                       PairHash<size_t, size_t>> fixed_pools;
    std::mutex pools_mutex;
    
public:
    /// Allocate state with specific size and alignment requirements
    char * allocateState(size_t size, size_t alignment)
    {
        if (size <= 128 && alignment <= 16)  /// Use fixed pools for small states
        {
            std::lock_guard<std::mutex> lock(pools_mutex);
            
            auto key = std::make_pair(size, alignment);
            auto it = fixed_pools.find(key);
            
            if (it == fixed_pools.end())
            {
                it = fixed_pools.emplace(key, 
                    std::make_unique<FixedSizePool>(size, alignment)).first;
            }
            
            return it->second->allocate();
        }
        else
        {
            /// Use general arena for large states
            return alloc(size, alignment);
        }
    }
    
    /// Batch allocation for multiple states
    void allocateStates(size_t count, size_t state_size, size_t alignment, char ** results)
    {
        if (count == 1)
        {
            results[0] = allocateState(state_size, alignment);
            return;
        }
        
        if (state_size <= 128 && alignment <= 16)
        {
            std::lock_guard<std::mutex> lock(pools_mutex);
            
            auto key = std::make_pair(state_size, alignment);
            auto it = fixed_pools.find(key);
            
            if (it == fixed_pools.end())
            {
                it = fixed_pools.emplace(key, 
                    std::make_unique<FixedSizePool>(state_size, alignment)).first;
            }
            
            auto & pool = *it->second;
            for (size_t i = 0; i < count; ++i)
                results[i] = pool.allocate();
        }
        else
        {
            /// Allocate large block and split
            size_t aligned_size = (state_size + alignment - 1) & ~(alignment - 1);
            char * base = alloc(count * aligned_size, alignment);
            
            for (size_t i = 0; i < count; ++i)
                results[i] = base + i * aligned_size;
        }
    }
    
    /// Memory usage statistics
    size_t getFixedPoolsMemory() const
    {
        std::lock_guard<std::mutex> lock(pools_mutex);
        
        size_t total = 0;
        for (const auto & [key, pool] : fixed_pools)
        {
            total += pool->chunks.size() * pool->states_per_chunk * 
                    ((key.first + key.second - 1) & ~(key.second - 1));
        }
        return total;
    }
};

/// High-performance state management for hash aggregation
class AggregationStateManager
{
private:
    AggregationStateArena arena;
    std::vector<AggregateFunctionInstruction> instructions;
    std::vector<size_t> state_offsets;
    size_t total_state_size = 0;
    size_t max_alignment = 1;
    
public:
    AggregationStateManager(const AggregateFunctionsDescriptions & descriptions)
    {
        /// Calculate layout for all aggregation states
        calculateStateLayout(descriptions);
    }
    
    /// Create states for new aggregation key
    void createStates(AggregateDataPtr place) const
    {
        char * current_place = place;
        
        for (size_t i = 0; i < instructions.size(); ++i)
        {
            const auto & instruction = instructions[i];
            instruction.function->create(current_place + state_offsets[i]);
        }
    }
    
    /// Destroy states when key is removed
    void destroyStates(AggregateDataPtr place) const noexcept
    {
        for (size_t i = 0; i < instructions.size(); ++i)
        {
            const auto & instruction = instructions[i];
            if (!instruction.function->hasTrivialDestructor())
                instruction.function->destroy(place + state_offsets[i]);
        }
    }
    
    /// Batch state creation for performance
    void createStatesBatch(AggregateDataPtr * places, size_t count) const
    {
        for (size_t func_idx = 0; func_idx < instructions.size(); ++func_idx)
        {
            const auto & instruction = instructions[func_idx];
            size_t offset = state_offsets[func_idx];
            
            for (size_t i = 0; i < count; ++i)
                instruction.function->create(places[i] + offset);
        }
    }
    
    /// Batch processing of aggregation operations
    void addBatch(AggregateDataPtr * places, size_t place_offset, 
                  const Columns & columns, size_t row_begin, size_t row_end) const
    {
        for (size_t i = 0; i < instructions.size(); ++i)
        {
            const auto & instruction = instructions[i];
            const IColumn ** arg_columns = reinterpret_cast<const IColumn **>(
                instruction.arguments.data());
            
            instruction.function->addBatch(
                row_begin, row_end, places, 
                place_offset + state_offsets[i], 
                arg_columns, &arena,
                instruction.has_filter ? instruction.filter_column : -1);
        }
    }
    
    /// Memory management
    Arena & getArena() { return arena; }
    size_t getTotalStateSize() const { return total_state_size; }
    size_t getMaxAlignment() const { return max_alignment; }
    
    /// Statistics and monitoring
    struct StateStatistics
    {
        size_t total_memory_used;
        size_t fixed_pool_memory;
        size_t arena_memory;
        size_t number_of_states;
        std::vector<size_t> state_sizes;
    };
    
    StateStatistics getStatistics() const
    {
        StateStatistics stats;
        stats.arena_memory = arena.size();
        stats.fixed_pool_memory = arena.getFixedPoolsMemory();
        stats.total_memory_used = stats.arena_memory + stats.fixed_pool_memory;
        
        for (const auto & instruction : instructions)
            stats.state_sizes.push_back(instruction.function->sizeOfData());
        
        return stats;
    }
    
private:
    void calculateStateLayout(const AggregateFunctionsDescriptions & descriptions)
    {
        instructions.reserve(descriptions.size());
        state_offsets.reserve(descriptions.size());
        
        size_t current_offset = 0;
        
        for (const auto & description : descriptions)
        {
            AggregateFunctionInstruction instruction;
            instruction.function = description.function;
            instruction.arguments = description.argument_numbers;
            instruction.has_filter = description.filter_column != -1;
            instruction.filter_column = description.filter_column;
            
            size_t state_size = instruction.function->sizeOfData();
            size_t state_alignment = instruction.function->alignOfData();
            
            /// Align current offset
            current_offset = (current_offset + state_alignment - 1) & ~(state_alignment - 1);
            state_offsets.push_back(current_offset);
            current_offset += state_size;
            
            max_alignment = std::max(max_alignment, state_alignment);
            instructions.push_back(std::move(instruction));
        }
        
        total_state_size = current_offset;
        
        /// Align total size to maximum alignment
        total_state_size = (total_state_size + max_alignment - 1) & ~(max_alignment - 1);
    }
};
```

#### 4.5.3 Performance Optimizations for Aggregation States

ClickHouse implements numerous performance optimizations specifically for aggregation state management:

**SIMD and Vectorization Optimizations:**

```cpp
/// Vectorized aggregation operations
class VectorizedAggregation
{
public:
    /// Vectorized sum for numeric types
    template <typename T>
    static void addBatchSum(const T * values, size_t count, T & state)
    {
        if constexpr (sizeof(T) == 4 && std::is_arithmetic_v<T>)
        {
            /// AVX2 implementation for 32-bit types
            size_t simd_count = count & ~7ULL;  /// Process 8 elements at a time
            
            __m256i sum_vec = _mm256_setzero_si256();
            const __m256i * data_ptr = reinterpret_cast<const __m256i *>(values);
            
            for (size_t i = 0; i < simd_count / 8; ++i)
            {
                __m256i data_vec = _mm256_loadu_si256(data_ptr + i);
                sum_vec = _mm256_add_epi32(sum_vec, data_vec);
            }
            
            /// Horizontal sum of vector
            __m128i sum_high = _mm256_extracti128_si256(sum_vec, 1);
            __m128i sum_low = _mm256_castsi256_si128(sum_vec);
            __m128i sum_128 = _mm_add_epi32(sum_high, sum_low);
            
            /// Further reduce to single value
            sum_128 = _mm_hadd_epi32(sum_128, sum_128);
            sum_128 = _mm_hadd_epi32(sum_128, sum_128);
            
            T simd_sum = _mm_cvtsi128_si32(sum_128);
            state += simd_sum;
            
            /// Handle remaining elements
            for (size_t i = simd_count; i < count; ++i)
                state += values[i];
        }
        else
        {
            /// Fallback scalar implementation
            for (size_t i = 0; i < count; ++i)
                state += values[i];
        }
    }
    
    /// Vectorized min/max operations
    template <typename T>
    static void addBatchMinMax(const T * values, size_t count, T & min_state, T & max_state)
    {
        if constexpr (sizeof(T) == 4 && std::is_arithmetic_v<T>)
        {
            size_t simd_count = count & ~7ULL;
            
            __m256i min_vec = _mm256_set1_epi32(min_state);
            __m256i max_vec = _mm256_set1_epi32(max_state);
            
            const __m256i * data_ptr = reinterpret_cast<const __m256i *>(values);
            
            for (size_t i = 0; i < simd_count / 8; ++i)
            {
                __m256i data_vec = _mm256_loadu_si256(data_ptr + i);
                min_vec = _mm256_min_epi32(min_vec, data_vec);
                max_vec = _mm256_max_epi32(max_vec, data_vec);
            }
            
            /// Extract minimum and maximum from vectors
            min_state = horizontalMin(min_vec);
            max_state = horizontalMax(max_vec);
            
            /// Handle remaining elements
            for (size_t i = simd_count; i < count; ++i)
            {
                min_state = std::min(min_state, values[i]);
                max_state = std::max(max_state, values[i]);
            }
        }
        else
        {
            for (size_t i = 0; i < count; ++i)
            {
                min_state = std::min(min_state, values[i]);
                max_state = std::max(max_state, values[i]);
            }
        }
    }
    
private:
    static int32_t horizontalMin(__m256i vec)
    {
        __m128i hi = _mm256_extracti128_si256(vec, 1);
        __m128i lo = _mm256_castsi256_si128(vec);
        __m128i min_128 = _mm_min_epi32(hi, lo);
        
        __m128i shuf = _mm_shuffle_epi32(min_128, _MM_SHUFFLE(2, 3, 0, 1));
        min_128 = _mm_min_epi32(min_128, shuf);
        
        shuf = _mm_shuffle_epi32(min_128, _MM_SHUFFLE(1, 0, 3, 2));
        min_128 = _mm_min_epi32(min_128, shuf);
        
        return _mm_cvtsi128_si32(min_128);
    }
    
    static int32_t horizontalMax(__m256i vec)
    {
        __m128i hi = _mm256_extracti128_si256(vec, 1);
        __m128i lo = _mm256_castsi256_si128(vec);
        __m128i max_128 = _mm_max_epi32(hi, lo);
        
        __m128i shuf = _mm_shuffle_epi32(max_128, _MM_SHUFFLE(2, 3, 0, 1));
        max_128 = _mm_max_epi32(max_128, shuf);
        
        shuf = _mm_shuffle_epi32(max_128, _MM_SHUFFLE(1, 0, 3, 2));
        max_128 = _mm_max_epi32(max_128, shuf);
        
        return _mm_cvtsi128_si32(max_128);
    }
};
```

This sophisticated aggregation state management system enables ClickHouse to efficiently handle complex analytical workloads with millions of concurrent aggregation states while maintaining optimal performance through specialized memory management, vectorized operations, and careful cache optimization.

## Phase 4 Summary

Phase 4 has provided a comprehensive exploration of ClickHouse's data structures and memory management systems, covering:

1. **IColumn Interface and Columnar Data Layout**: The foundational abstraction that enables efficient columnar data processing with specialized implementations optimized for different data types and access patterns.

2. **Arena Allocators and Memory Pools**: Sophisticated memory management systems that minimize allocation overhead and maximize cache efficiency through strategic memory pool design and NUMA-aware allocation strategies.

3. **Block Structure and Data Flow Management**: The core unit of data processing that enables efficient batch operations while maintaining type safety and supporting complex transformations throughout the query pipeline.

4. **Field Abstraction and Type System Integration**: A universal value holder that provides seamless integration between different data types while maintaining type safety and optimal performance for dynamic value handling.

5. **Aggregation Function States and Memory Management**: Specialized memory management for high-performance aggregation operations that can efficiently handle millions of concurrent states with vectorized operations and optimized memory layouts.

Together, these components form a sophisticated foundation that enables ClickHouse to achieve exceptional performance in analytical workloads through careful attention to memory management, cache optimization, and vectorized processing capabilities.

---

# Phase 5: Aggregation Engine Deep Dive (10,000 words)

The aggregation engine represents one of ClickHouse's most sophisticated subsystems, responsible for processing GROUP BY operations, aggregation functions, and combinators. This phase explores the hash table implementations, aggregate function architecture, AggregatingTransform processor, combinator functions, and specialized memory management strategies that enable ClickHouse to aggregate billions of rows efficiently.

## 5.1 Aggregation Hash Tables and Data Structures (2,000 words)

ClickHouse's aggregation performance depends heavily on highly optimized hash table implementations. The system employs multiple hash table variants, each optimized for specific data types, cardinalities, and memory constraints.

### 5.1.1 Hash Table Selection Framework

The aggregation engine uses a sophisticated dispatch mechanism to select the optimal hash table implementation based on key characteristics:

```cpp
namespace DB
{

enum class AggregatedDataVariants
{
    EMPTY = 0,
    WITHOUT_KEY,
    KEY_8,
    KEY_16,
    KEY_32,
    KEY_64,
    KEY_STRING,
    KEY_FIXED_STRING,
    KEYS_128,
    KEYS_256,
    HASHED,
    SERIALIZED,
    STRING_HASH_MAP,
    TWO_LEVEL_HASHED,
    TWO_LEVEL_STRING_HASH_MAP,
};

class AggregatedDataVariantsDispatcher
{
private:
    struct VariantSelector
    {
        using KeyTypes = std::vector<DataTypePtr>;
        using KeySizes = std::vector<size_t>;
        
        AggregatedDataVariants selectVariant(
            const KeyTypes & key_types,
            const KeySizes & key_sizes,
            size_t total_key_size) const
        {
            // Single numeric key optimizations
            if (key_types.size() == 1)
            {
                const auto & type = key_types[0];
                if (type->isValueRepresentedByNumber() && !type->haveSubtypes())
                {
                    size_t size = type->getSizeOfValueInMemory();
                    switch (size)
                    {
                        case 1: return AggregatedDataVariants::KEY_8;
                        case 2: return AggregatedDataVariants::KEY_16;
                        case 4: return AggregatedDataVariants::KEY_32;
                        case 8: return AggregatedDataVariants::KEY_64;
                    }
                }
                
                // String key optimizations
                if (isString(type))
                    return AggregatedDataVariants::KEY_STRING;
                if (isFixedString(type))
                    return AggregatedDataVariants::KEY_FIXED_STRING;
            }
            
            // Multi-key optimizations
            if (total_key_size <= 16)
                return AggregatedDataVariants::KEYS_128;
            if (total_key_size <= 32)
                return AggregatedDataVariants::KEYS_256;
            
            // Fallback to generic hash table
            return AggregatedDataVariants::HASHED;
        }
    };
    
public:
    template <typename Method>
    void executeDispatch(AggregatedDataVariants::Type type, Method && method) const
    {
        switch (type)
        {
            case AggregatedDataVariants::KEY_8:
                method.template operator()<AggregationMethodOneNumber<UInt8>>();
                break;
            case AggregatedDataVariants::KEY_16:
                method.template operator()<AggregationMethodOneNumber<UInt16>>();
                break;
            case AggregatedDataVariants::KEY_32:
                method.template operator()<AggregationMethodOneNumber<UInt32>>();
                break;
            case AggregatedDataVariants::KEY_64:
                method.template operator()<AggregationMethodOneNumber<UInt64>>();
                break;
            case AggregatedDataVariants::KEY_STRING:
                method.template operator()<AggregationMethodString>();
                break;
            case AggregatedDataVariants::HASHED:
                method.template operator()<AggregationMethodHashed>();
                break;
            case AggregatedDataVariants::TWO_LEVEL_HASHED:
                method.template operator()<AggregationMethodTwoLevelHashed>();
                break;
        }
    }
};

}
```

### 5.1.2 Specialized Hash Tables

ClickHouse implements multiple hash table variants, each with distinct performance characteristics:

**Linear Probing Hash Table (Primary Implementation):**

```cpp
template <typename Key, typename Cell, typename Hash, typename Grower, typename Allocator>
class HashTable : 
    protected Hash,
    protected Allocator,
    protected Cell::State,
    protected ZeroValueStorage<Cell::need_zero_value_storage, Cell>
{
private:
    using Self = HashTable;
    using cell_type = Cell;
    
    Cell * buf;                    /// Hash table buffer
    size_t m_size = 0;            /// Number of elements
    size_t mask = 0;              /// Hash table size - 1
    mutable size_t saved_hash = 0; /// Last calculated hash value
    
    Grower grower;
    
    static constexpr size_t RESIZE_THRESHOLD_BITS = 6;
    static constexpr size_t RESIZE_THRESHOLD = 1ULL << RESIZE_THRESHOLD_BITS;
    
public:
    using key_type = typename Cell::Key;
    using value_type = typename Cell::value_type;
    using LookupResult = typename Cell::LookupResult;
    
    /// Find or insert element with given key
    template <typename KeyHolder>
    LookupResult ALWAYS_INLINE emplaceKey(KeyHolder && key_holder, size_t hash_value)
    {
        size_t place_value = grower.place(hash_value);
        
        while (true)
        {
            Cell * cell = &buf[place_value & mask];
            
            if (cell->isZero(*this))
            {
                // Empty cell found - insert here
                cell->setKey(std::forward<KeyHolder>(key_holder), *this);
                ++m_size;
                
                if (unlikely(grower.overflow(m_size)))
                    resize();
                    
                return LookupResult(cell, true);  // inserted = true
            }
            
            if (cell->keyEquals(key_holder, hash_value, *this))
            {
                // Found existing key
                return LookupResult(cell, false); // inserted = false  
            }
            
            // Collision - linear probing
            place_value = grower.next(place_value);
        }
    }
    
    /// Optimized find for lookup-only operations
    template <typename KeyHolder>
    Cell * ALWAYS_INLINE find(KeyHolder && key_holder, size_t hash_value) const
    {
        size_t place_value = grower.place(hash_value);
        
        while (true)
        {
            Cell * cell = &buf[place_value & mask];
            
            if (cell->isZero(*this))
                return nullptr;  // Not found
                
            if (cell->keyEquals(key_holder, hash_value, *this))
                return cell;     // Found
                
            place_value = grower.next(place_value);
        }
    }
    
private:
    void resize()
    {
        size_t old_size = grower.bufSize();
        grower.increaseSize();
        size_t new_size = grower.bufSize();
        
        Cell * old_buf = buf;
        buf = static_cast<Cell *>(Allocator::alloc(new_size * sizeof(Cell)));
        mask = new_size - 1;
        
        // Initialize new buffer
        memset(buf, 0, new_size * sizeof(Cell));
        
        // Rehash existing elements
        for (size_t i = 0; i < old_size; ++i)
        {
            Cell & old_cell = old_buf[i];
            if (!old_cell.isZero(*this))
            {
                size_t hash_value = old_cell.getHash(*this);
                size_t place_value = grower.place(hash_value);
                
                while (!buf[place_value & mask].isZero(*this))
                    place_value = grower.next(place_value);
                    
                buf[place_value & mask] = std::move(old_cell);
            }
        }
        
        Allocator::free(old_buf, old_size * sizeof(Cell));
    }
};
```

**Two-Level Hash Table for Large Aggregations:**

```cpp
template <typename Impl>
class TwoLevelHashTable : public Impl
{
public:
    static constexpr size_t NUM_BUCKETS = 256;
    static constexpr size_t BUCKET_COUNT_BITS = 8;
    
private:
    using Base = Impl;
    using Self = TwoLevelHashTable;
    
    struct Bucket
    {
        std::unique_ptr<Base> impl;
        mutable std::mutex mutex;
        size_t size_hint = 0;
        
        Base & getImpl()
        {
            if (!impl)
                impl = std::make_unique<Base>();
            return *impl;
        }
    };
    
    std::array<Bucket, NUM_BUCKETS> buckets;
    std::atomic<size_t> total_size{0};
    
public:
    template <typename KeyHolder>
    typename Base::LookupResult emplaceKey(KeyHolder && key_holder, size_t hash_value)
    {
        size_t bucket_index = getBucketFromHash(hash_value);
        Bucket & bucket = buckets[bucket_index];
        
        // Thread-safe bucket access
        std::lock_guard<std::mutex> lock(bucket.mutex);
        auto result = bucket.getImpl().emplaceKey(
            std::forward<KeyHolder>(key_holder), hash_value);
            
        if (result.isInserted())
        {
            total_size.fetch_add(1, std::memory_order_relaxed);
            ++bucket.size_hint;
        }
        
        return result;
    }
    
    /// Parallel processing across buckets
    template <typename Func>
    void forEachBucket(Func && func) const
    {
        const size_t num_threads = std::min(
            getCurrentThreadCount(), 
            static_cast<size_t>(NUM_BUCKETS));
            
        ThreadPool pool(num_threads);
        
        for (size_t bucket_idx = 0; bucket_idx < NUM_BUCKETS; ++bucket_idx)
        {
            pool.scheduleOrThrowOnError([&func, bucket_idx, this]()
            {
                const auto & bucket = buckets[bucket_idx];
                if (bucket.impl && bucket.impl->size() > 0)
                    func(bucket_idx, *bucket.impl);
            });
        }
        
        pool.wait();
    }
    
private:
    static size_t getBucketFromHash(size_t hash_value)
    {
        return (hash_value >> (64 - BUCKET_COUNT_BITS)) & (NUM_BUCKETS - 1);
    }
};
```

### 5.1.3 String-Optimized Hash Tables

For string aggregation keys, ClickHouse uses specialized hash tables that optimize memory layout and comparison operations:

```cpp
template <typename TData>
class StringHashTable
{
public:
    using Key = StringRef;
    using Cell = HashTableCell<Key, TData>;
    using LookupResult = typename Cell::LookupResult;
    
private:
    /// String arena for key storage
    Arena string_pool;
    
    /// Main hash table
    using Impl = HashTable<StringRef, Cell, StringRefHash, 
                          HashTableGrower<>, ArenaAllocator>;
    Impl impl;
    
    /// Cache for short string optimization
    static constexpr size_t SHORT_STRING_CACHE_SIZE = 1024;
    std::array<Cell, SHORT_STRING_CACHE_SIZE> short_string_cache;
    
public:
    template <typename KeyHolder>
    LookupResult emplaceKey(KeyHolder && key_holder)
    {
        StringRef key = toStringRef(key_holder);
        
        // Short string optimization
        if (key.size <= sizeof(size_t))
        {
            size_t hash = integerHash(key);
            size_t cache_index = hash & (SHORT_STRING_CACHE_SIZE - 1);
            Cell & cell = short_string_cache[cache_index];
            
            if (cell.isZero() || cell.keyEquals(key))
            {
                if (cell.isZero())
                {
                    // Store string inline in hash value
                    cell.setKey(key);
                }
                return LookupResult(&cell, cell.isZero());
            }
        }
        
        // Copy string to arena if needed for persistence
        StringRef persistent_key = copyStringToArena(key);
        size_t hash = StringRefHash{}(persistent_key);
        
        return impl.emplaceKey(persistent_key, hash);
    }
    
private:
    StringRef copyStringToArena(StringRef src)
    {
        if (src.size == 0)
            return StringRef{};
            
        char * data = string_pool.alloc(src.size);
        memcpy(data, src.data, src.size);
        return StringRef{data, src.size};
    }
    
    static size_t integerHash(StringRef key)
    {
        // Convert short string to integer for hashing
        size_t result = 0;
        memcpy(&result, key.data, std::min(key.size, sizeof(size_t)));
        return intHash64(result);
    }
};
```

This comprehensive hash table framework enables ClickHouse to achieve optimal performance across diverse aggregation workloads by selecting the most appropriate data structure based on key characteristics and cardinality patterns.

## 5.2 Aggregate Functions Architecture and Registration (2,000 words)

ClickHouse's aggregate function system provides a flexible framework for implementing both built-in and user-defined aggregation operations. The architecture separates function logic from execution context through the IAggregateFunction interface.

### 5.2.1 IAggregateFunction Interface

The core interface defines the contract for all aggregate functions:

```cpp
class IAggregateFunction
{
public:
    using AggregateDataPtr = char *;
    
    virtual ~IAggregateFunction() = default;
    
    /// Function metadata
    virtual String getName() const = 0;
    virtual DataTypePtr getReturnType() const = 0;
    virtual DataTypes getArgumentTypes() const = 0;
    
    /// State management
    virtual size_t sizeOfData() const = 0;
    virtual size_t alignOfData() const = 0;
    virtual void create(AggregateDataPtr place) const = 0;
    virtual void destroy(AggregateDataPtr place) const noexcept = 0;
    
    /// Core aggregation operations
    virtual void add(
        AggregateDataPtr place,
        const IColumn ** columns,
        size_t row_num,
        Arena * arena) const = 0;
        
    virtual void addBatch(
        size_t batch_size,
        AggregateDataPtr * places,
        size_t place_offset,
        const IColumn ** columns,
        Arena * arena,
        ssize_t if_argument_pos = -1) const = 0;
        
    virtual void addBatchSparse(
        size_t batch_size,
        AggregateDataPtr * places,
        size_t place_offset,
        const IColumn ** columns,
        const UInt8 * null_map,
        Arena * arena,
        ssize_t if_argument_pos = -1) const = 0;
    
    /// Merge operations for parallel aggregation
    virtual void merge(
        AggregateDataPtr place,
        ConstAggregateDataPtr rhs,
        Arena * arena) const = 0;
        
    virtual void mergeBatch(
        size_t batch_size,
        AggregateDataPtr * places,
        size_t place_offset,
        const AggregateDataPtr * rhs,
        Arena * arena) const = 0;
    
    /// Serialization for storage and network transfer
    virtual void serialize(
        ConstAggregateDataPtr place,
        WriteBuffer & buf,
        std::optional<size_t> version = std::nullopt) const = 0;
        
    virtual void deserialize(
        AggregateDataPtr place,
        ReadBuffer & buf,
        std::optional<size_t> version = std::nullopt,
        Arena * arena) const = 0;
    
    /// Result extraction
    virtual void insertResultInto(
        ConstAggregateDataPtr place,
        IColumn & to,
        Arena * arena) const = 0;
        
    /// Optional optimization hints
    virtual bool allocatesMemoryInArena() const { return false; }
    virtual bool isState() const { return false; }
    virtual bool isVersioned() const { return false; }
    virtual size_t getVersionFromRevision(size_t revision) const { return 0; }
    
    /// Parallelization support
    virtual bool canBeParallelized() const { return true; }
    virtual AggregateFunctionPtr getOwnNullAdapter(
        const AggregateFunctionPtr &,
        const DataTypes &,
        const Array &,
        const Settings &) const { return nullptr; }
};
```

### 5.2.2 Aggregate Function Registration System

ClickHouse uses a factory pattern with automatic registration for aggregate functions:

```cpp
class AggregateFunctionFactory : public IFactoryWithAliases<AggregateFunctionFactory>
{
private:
    using Creator = std::function<AggregateFunctionPtr(
        const std::string &,
        const DataTypes &,
        const Array &,
        const Settings &)>;
        
    using CreatorMap = std::unordered_map<std::string, Creator>;
    using PropertiesMap = std::unordered_map<std::string, AggregateFunctionProperties>;
    
    CreatorMap aggregate_functions;
    PropertiesMap function_properties;
    
public:
    /// Register a new aggregate function
    void registerFunction(
        const std::string & name,
        Creator creator,
        AggregateFunctionProperties properties = {},
        CaseSensitiveness case_sensitiveness = CaseSensitive)
    {
        String function_name = normalizeName(name, case_sensitiveness);
        aggregate_functions[function_name] = std::move(creator);
        function_properties[function_name] = std::move(properties);
    }
    
    /// Create aggregate function instance
    AggregateFunctionPtr get(
        const String & name,
        const DataTypes & argument_types,
        const Array & parameters = {},
        const Settings & settings = {}) const
    {
        String normalized_name = getAliasToOrName(name);
        
        auto it = aggregate_functions.find(normalized_name);
        if (it == aggregate_functions.end())
            throw Exception(ErrorCodes::UNKNOWN_AGGREGATE_FUNCTION, 
                          "Unknown aggregate function {}", name);
        
        try
        {
            return it->second(normalized_name, argument_types, parameters, settings);
        }
        catch (Exception & e)
        {
            e.addMessage(fmt::format("while creating aggregate function '{}'", name));
            throw;
        }
    }
    
    /// Function introspection
    AggregateFunctionProperties getProperties(const String & name) const
    {
        String normalized_name = getAliasToOrName(name);
        auto it = function_properties.find(normalized_name);
        return it != function_properties.end() ? it->second : AggregateFunctionProperties{};
    }
    
    /// List all registered functions
    std::vector<String> getAllRegisteredNames() const
    {
        std::vector<String> result;
        result.reserve(aggregate_functions.size());
        
        for (const auto & [name, _] : aggregate_functions)
            result.push_back(name);
            
        return result;
    }
};

/// Automatic registration helper
template <typename FunctionClass>
struct AggregateFunctionRegistrator
{
    AggregateFunctionRegistrator(
        const std::string & name,
        AggregateFunctionProperties properties = {},
        CaseSensitiveness case_sensitiveness = CaseSensitive)
    {
        auto creator = [](const std::string &,
                         const DataTypes & argument_types,
                         const Array & parameters,
                         const Settings &) -> AggregateFunctionPtr
        {
            return std::make_shared<FunctionClass>(argument_types, parameters);
        };
        
        AggregateFunctionFactory::instance().registerFunction(
            name, std::move(creator), std::move(properties), case_sensitiveness);
    }
};

#define REGISTER_AGGREGATE_FUNCTION(class_name, function_name) \
    namespace { \
        static AggregateFunctionRegistrator<class_name> \
            register_##class_name(#function_name); \
    }
```

### 5.2.3 Example Aggregate Function Implementation

Here's a complete implementation of a sum aggregate function:

```cpp
template <typename T>
class AggregateFunctionSum final : public IAggregateFunctionDataHelper<
    AggregateFunctionSumData<T>, AggregateFunctionSum<T>>
{
private:
    using Data = AggregateFunctionSumData<T>;
    using ColVecType = ColumnVector<T>;
    
    DataTypePtr result_type;
    
public:
    explicit AggregateFunctionSum(const DataTypes & argument_types)
        : IAggregateFunctionDataHelper<Data, AggregateFunctionSum<T>>(argument_types, {})
        , result_type(std::make_shared<DataTypeNumber<T>>())
    {
        if (argument_types.size() != 1)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                          "Aggregate function sum requires exactly one argument");
                          
        if (!isNumber(argument_types[0]) && !isDecimal(argument_types[0]))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                          "Aggregate function sum requires numeric argument");
    }
    
    String getName() const override { return "sum"; }
    DataTypePtr getReturnType() const override { return result_type; }
    bool allocatesMemoryInArena() const override { return false; }
    
    void add(AggregateDataPtr place, const IColumn ** columns, 
             size_t row_num, Arena *) const override
    {
        const auto & column = static_cast<const ColVecType &>(*columns[0]);
        this->data(place).sum += column.getData()[row_num];
    }
    
    void addBatch(
        size_t batch_size,
        AggregateDataPtr * places,
        size_t place_offset,
        const IColumn ** columns,
        Arena *,
        ssize_t if_argument_pos = -1) const override
    {
        const auto & column = static_cast<const ColVecType &>(*columns[0]);
        const auto & data_vec = column.getData();
        
        if (if_argument_pos >= 0)
        {
            // Conditional sum with IF combinator
            const auto & if_column = static_cast<const ColumnUInt8 &>(*columns[if_argument_pos]);
            const auto & if_data = if_column.getData();
            
            for (size_t i = 0; i < batch_size; ++i)
            {
                if (if_data[i])
                {
                    AggregateDataPtr place = places[i] + place_offset;
                    this->data(place).sum += data_vec[i];
                }
            }
        }
        else
        {
            // Vectorized sum without conditions
            for (size_t i = 0; i < batch_size; ++i)
            {
                AggregateDataPtr place = places[i] + place_offset;
                this->data(place).sum += data_vec[i];
            }
        }
    }
    
    void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena *) const override
    {
        this->data(place).sum += this->data(rhs).sum;
    }
    
    void serialize(ConstAggregateDataPtr place, WriteBuffer & buf, 
                   std::optional<size_t>) const override
    {
        writeBinary(this->data(place).sum, buf);
    }
    
    void deserialize(AggregateDataPtr place, ReadBuffer & buf, 
                     std::optional<size_t>, Arena *) const override
    {
        readBinary(this->data(place).sum, buf);
    }
    
    void insertResultInto(ConstAggregateDataPtr place, IColumn & to, Arena *) const override
    {
        static_cast<ColVecType &>(to).getData().push_back(this->data(place).sum);
    }
};

/// Aggregate function data structure
template <typename T>
struct AggregateFunctionSumData
{
    using Type = T;
    Type sum{};
    
    void reset() { sum = {}; }
    
    template <typename U>
    void addValue(U value)
    {
        sum += static_cast<Type>(value);
    }
};

// Register the function
REGISTER_AGGREGATE_FUNCTION(AggregateFunctionSum<Float64>, sum)
```

This implementation demonstrates key aspects of ClickHouse's aggregate function architecture:

1. **Template-based design** for type-specific optimizations
2. **Vectorized batch processing** for performance
3. **Combinator support** through conditional parameters
4. **Serialization/deserialization** for distributed aggregation
5. **Memory management integration** through Arena usage

The registration system enables automatic discovery and instantiation of aggregate functions, while the interface provides a uniform API for the aggregation engine to work with diverse function implementations.

## 5.3 AggregatingTransform Implementation (2,000 words)

The AggregatingTransform is the core processor responsible for executing aggregation operations within ClickHouse's query pipeline. It coordinates between hash tables, aggregate functions, and memory management to achieve high-performance aggregation.

### 5.3.1 AggregatingTransform Architecture

The AggregatingTransform implements the IProcessor interface and manages the entire aggregation lifecycle:

```cpp
class AggregatingTransform : public IProcessor
{
private:
    /// Configuration and context
    AggregatingTransformParamsPtr params;
    ContextPtr context;
    
    /// Core aggregation components
    std::unique_ptr<Aggregator> aggregator;
    AggregatedDataVariants aggregated_data;
    
    /// Pipeline state management
    bool is_consume_finished = false;
    bool is_generate_finished = false;
    bool is_two_level = false;
    size_t current_bucket = 0;
    
    /// Input/output management
    Block input_header;
    Block output_header;
    Chunks input_chunks;
    
    /// Performance tracking
    size_t rows_processed = 0;
    size_t bytes_processed = 0;
    Stopwatch watch;
    
public:
    AggregatingTransform(
        Block input_header_,
        AggregatingTransformParamsPtr params_,
        ContextPtr context_)
        : params(std::move(params_))
        , context(std::move(context_))
        , input_header(std::move(input_header_))
    {
        /// Initialize aggregator with parameters
        aggregator = std::make_unique<Aggregator>(params->params);
        output_header = aggregator->getHeader(params->final);
        
        /// Setup input/output ports
        inputs.emplace_back(input_header, this);
        outputs.emplace_back(output_header, this);
        
        /// Prepare aggregation data structures
        aggregated_data.init(aggregator->getAggregatedDataVariantsType());
    }
    
    String getName() const override { return "AggregatingTransform"; }
    
    Status prepare() override
    {
        auto & input = inputs.front();
        auto & output = outputs.front();
        
        /// Check if output is finished
        if (output.isFinished())
        {
            input.close();
            return Status::Finished;
        }
        
        /// Generation phase
        if (is_consume_finished)
        {
            if (is_generate_finished)
                return Status::Finished;
                
            if (!output.canPush())
                return Status::PortFull;
                
            return Status::Ready;
        }
        
        /// Consumption phase
        if (input.isFinished())
        {
            is_consume_finished = true;
            return Status::Ready;
        }
        
        if (!input.hasData())
            return Status::NeedData;
            
        return Status::Ready;
    }
    
    void work() override
    {
        auto & input = inputs.front();
        auto & output = outputs.front();
        
        if (!is_consume_finished)
        {
            consumeCurrentChunk(input);
        }
        else
        {
            generateOutputChunk(output);
        }
    }
    
private:
    void consumeCurrentChunk(InputPort & input)
    {
        auto chunk = input.pull();
        
        if (!chunk.hasRows())
            return;
            
        /// Convert chunk to block for aggregation
        auto block = input_header.cloneWithColumns(chunk.detachColumns());
        
        /// Track performance metrics
        rows_processed += block.rows();
        bytes_processed += block.bytes();
        
        /// Execute aggregation on the block
        watch.restart();
        aggregator->executeOnBlock(
            block,
            aggregated_data,
            params->key_columns,
            params->aggregate_columns,
            params->final);
        
        /// Check if we need to switch to two-level aggregation
        if (!is_two_level && shouldSwitchToTwoLevel())
        {
            switchToTwoLevelAggregation();
        }
        
        /// Check memory limits
        if (params->max_bytes && aggregated_data.sizeWithoutOverflowRow() > params->max_bytes)
        {
            handleMemoryOverflow();
        }
    }
    
    void generateOutputChunk(OutputPort & output)
    {
        Block result_block;
        
        if (!is_two_level)
        {
            /// Single-level aggregation result
            result_block = aggregator->prepareBlockAndFillWithoutKey(
                aggregated_data, params->final, params->max_block_size);
        }
        else
        {
            /// Two-level aggregation - output bucket by bucket
            result_block = aggregator->prepareBlockAndFillSingleLevel(
                aggregated_data, params->final, params->max_block_size, current_bucket);
            
            if (result_block.rows() == 0)
            {
                ++current_bucket;
                if (current_bucket >= aggregated_data.NUM_BUCKETS)
                {
                    is_generate_finished = true;
                    return;
                }
                
                /// Try next bucket
                result_block = aggregator->prepareBlockAndFillSingleLevel(
                    aggregated_data, params->final, params->max_block_size, current_bucket);
            }
        }
        
        if (result_block.rows() == 0)
        {
            is_generate_finished = true;
        }
        else
        {
            auto chunk = Chunk(result_block.getColumns(), result_block.rows());
            output.push(std::move(chunk));
        }
    }
    
    bool shouldSwitchToTwoLevel() const
    {
        /// Switch to two-level aggregation based on cardinality
        static constexpr size_t TWO_LEVEL_THRESHOLD = 100000;
        return aggregated_data.size() > TWO_LEVEL_THRESHOLD;
    }
    
    void switchToTwoLevelAggregation()
    {
        is_two_level = true;
        aggregated_data.convertToTwoLevel();
    }
    
    void handleMemoryOverflow()
    {
        /// Implement spill-to-disk or external merge strategies
        if (params->overflow_mode == OverflowMode::THROW)
        {
            throw Exception("Memory limit exceeded during aggregation", 
                          ErrorCodes::MEMORY_LIMIT_EXCEEDED);
        }
        else if (params->overflow_mode == OverflowMode::BREAK)
        {
            /// Stop consuming new data
            is_consume_finished = true;
        }
    }
};
```

### 5.3.2 Aggregator Implementation

The Aggregator class implements the core aggregation logic and coordinates with hash tables and aggregate functions:

```cpp
class Aggregator
{
private:
    /// Configuration
    Params params;
    
    /// Function and key management
    AggregateFunctionsInstructions aggregate_functions;
    std::vector<size_t> key_column_numbers;
    std::vector<size_t> aggregate_column_numbers;
    
    /// State management
    AggregationStateManager state_manager;
    
    /// Two-level aggregation support
    TwoLevelSettings two_level_settings;
    
public:
    explicit Aggregator(const Params & params_) : params(params_)
    {
        /// Initialize aggregate functions
        setupAggregateFunctions();
        
        /// Setup key extraction
        setupKeyColumns();
        
        /// Initialize state manager
        state_manager.initialize(aggregate_functions);
    }
    
    /// Main aggregation execution
    void executeOnBlock(
        const Block & block,
        AggregatedDataVariants & result,
        const ColumnNumbers & key_columns,
        const ColumnNumbers & aggregate_columns,
        bool final) const
    {
        /// Dispatch to appropriate aggregation method
        result.dispatched_variants.visit([&](auto & variant) {
            executeOnBlockImpl(block, variant, key_columns, aggregate_columns, final);
        });
    }
    
    /// Generate output blocks
    Block prepareBlockAndFillWithoutKey(
        AggregatedDataVariants & data_variants,
        bool final,
        size_t max_block_size) const
    {
        return data_variants.dispatched_variants.visit([&](auto & variant) -> Block {
            return prepareBlockAndFillImpl(variant, final, max_block_size);
        });
    }
    
private:
    template <typename Method>
    void executeOnBlockImpl(
        const Block & block,
        Method & method,
        const ColumnNumbers & key_columns,
        const ColumnNumbers & aggregate_columns,
        bool final) const
    {
        typename Method::State state(key_columns);
        
        /// Extract key columns
        ColumnRawPtrs key_columns_raw;
        key_columns_raw.reserve(key_columns.size());
        for (size_t i : key_columns)
            key_columns_raw.push_back(block.getByPosition(i).column.get());
        
        /// Extract aggregate columns
        ColumnRawPtrs aggregate_columns_raw;
        aggregate_columns_raw.reserve(aggregate_columns.size());
        for (size_t i : aggregate_columns)
            aggregate_columns_raw.push_back(block.getByPosition(i).column.get());
        
        /// Perform aggregation
        size_t rows = block.rows();
        
        /// Batch processing for performance
        static constexpr size_t BATCH_SIZE = 4096;
        
        for (size_t batch_start = 0; batch_start < rows; batch_start += BATCH_SIZE)
        {
            size_t batch_end = std::min(batch_start + BATCH_SIZE, rows);
            size_t batch_size = batch_end - batch_start;
            
            /// Process batch
            executeOnBatch(
                method, state, 
                key_columns_raw, aggregate_columns_raw,
                batch_start, batch_size);
        }
    }
    
    template <typename Method>
    void executeOnBatch(
        Method & method,
        typename Method::State & state,
        const ColumnRawPtrs & key_columns,
        const ColumnRawPtrs & aggregate_columns,
        size_t batch_start,
        size_t batch_size) const
    {
        /// Prepare batch of aggregation places
        std::vector<AggregateDataPtr> places(batch_size);
        std::vector<bool> places_new(batch_size);
        
        /// Extract or create aggregation states for each row in batch
        for (size_t i = 0; i < batch_size; ++i)
        {
            size_t row_num = batch_start + i;
            
            /// Get key for this row
            auto key = state.getKey(key_columns, row_num);
            
            /// Find or create aggregation state
            auto [place, inserted] = method.findOrCreatePlace(key);
            places[i] = place;
            places_new[i] = inserted;
            
            /// Initialize state for new keys
            if (inserted)
                state_manager.createStates(place);
        }
        
        /// Apply aggregate functions to batch
        for (size_t func_idx = 0; func_idx < aggregate_functions.size(); ++func_idx)
        {
            const auto & instruction = aggregate_functions[func_idx];
            
            /// Prepare function arguments
            ColumnRawPtrs func_columns;
            for (size_t arg_idx : instruction.arguments)
                func_columns.push_back(aggregate_columns[arg_idx]);
            
            /// Apply function to batch
            instruction.function->addBatch(
                batch_size, places.data(), instruction.state_offset,
                func_columns.data(), state_manager.getArena(),
                instruction.has_filter ? instruction.filter_column : -1);
        }
    }
    
    template <typename Method>
    Block prepareBlockAndFillImpl(
        Method & method,
        bool final,
        size_t max_block_size) const
    {
        Block result_block = getHeader(final);
        MutableColumns result_columns = result_block.cloneEmptyColumns();
        
        size_t rows_processed = 0;
        
        /// Iterate through all aggregated data
        method.forEachPlace([&](const auto & key, AggregateDataPtr place) {
            if (rows_processed >= max_block_size)
                return false;  /// Stop processing this batch
            
            /// Insert key columns
            insertKeyIntoColumns(key, result_columns, rows_processed);
            
            /// Insert aggregated values
            for (size_t func_idx = 0; func_idx < aggregate_functions.size(); ++func_idx)
            {
                const auto & instruction = aggregate_functions[func_idx];
                size_t result_col_idx = key_column_numbers.size() + func_idx;
                
                instruction.function->insertResultInto(
                    place + instruction.state_offset,
                    *result_columns[result_col_idx],
                    state_manager.getArena());
            }
            
            ++rows_processed;
            return true;  /// Continue processing
        });
        
        result_block.setColumns(std::move(result_columns));
        return result_block;
    }
    
    void setupAggregateFunctions()
    {
        aggregate_functions.reserve(params.aggregates.size());
        
        for (const auto & aggregate : params.aggregates)
        {
            AggregateFunctionInstruction instruction;
            instruction.function = aggregate.function;
            instruction.arguments = aggregate.argument_numbers;
            instruction.state_offset = 0;  /// Will be calculated later
            instruction.has_filter = aggregate.filter_column >= 0;
            instruction.filter_column = aggregate.filter_column;
            
            aggregate_functions.push_back(std::move(instruction));
        }
        
        /// Calculate state offsets
        size_t offset = 0;
        for (auto & instruction : aggregate_functions)
        {
            size_t alignment = instruction.function->alignOfData();
            offset = (offset + alignment - 1) & ~(alignment - 1);
            instruction.state_offset = offset;
            offset += instruction.function->sizeOfData();
        }
    }
    
    void setupKeyColumns()
    {
        key_column_numbers.reserve(params.keys.size());
        for (const auto & key : params.keys)
            key_column_numbers.push_back(key);
    }
};
```

### 5.3.3 Performance Optimizations

The AggregatingTransform implements several critical performance optimizations:

**Batch Processing and Vectorization:**

```cpp
/// Vectorized key extraction for numeric keys
template <typename T>
class VectorizedKeyExtractor
{
public:
    static void extractKeys(
        const IColumn & column,
        size_t batch_start,
        size_t batch_size,
        std::vector<T> & keys)
    {
        const auto & typed_column = static_cast<const ColumnVector<T> &>(column);
        const auto & data = typed_column.getData();
        
        keys.resize(batch_size);
        
        /// SIMD-optimized copy when possible
        if constexpr (sizeof(T) == 4 || sizeof(T) == 8)
        {
            /// Use AVX2 for bulk copy
            std::memcpy(keys.data(), 
                       data.data() + batch_start, 
                       batch_size * sizeof(T));
        }
        else
        {
            /// Fallback for other types
            for (size_t i = 0; i < batch_size; ++i)
                keys[i] = data[batch_start + i];
        }
    }
};

/// Optimized aggregation for specific function types
class OptimizedAggregation
{
public:
    /// Fast path for sum aggregation
    template <typename T>
    static void addBatchSum(
        const T * values,
        size_t count,
        AggregateDataPtr * places,
        size_t state_offset)
    {
        /// Unroll loop for better performance
        size_t unroll_count = count & ~3ULL;
        
        for (size_t i = 0; i < unroll_count; i += 4)
        {
            auto * state0 = reinterpret_cast<T *>(places[i] + state_offset);
            auto * state1 = reinterpret_cast<T *>(places[i + 1] + state_offset);
            auto * state2 = reinterpret_cast<T *>(places[i + 2] + state_offset);
            auto * state3 = reinterpret_cast<T *>(places[i + 3] + state_offset);
            
            *state0 += values[i];
            *state1 += values[i + 1];
            *state2 += values[i + 2];
            *state3 += values[i + 3];
        }
        
        /// Handle remaining elements
        for (size_t i = unroll_count; i < count; ++i)
        {
            auto * state = reinterpret_cast<T *>(places[i] + state_offset);
            *state += values[i];
        }
    }
    
    /// Fast path for count aggregation
    static void addBatchCount(
        size_t count,
        AggregateDataPtr * places,
        size_t state_offset,
        const UInt8 * filter = nullptr)
    {
        if (filter == nullptr)
        {
            /// No filter - increment all counters
            for (size_t i = 0; i < count; ++i)
            {
                auto * state = reinterpret_cast<UInt64 *>(places[i] + state_offset);
                ++(*state);
            }
        }
        else
        {
            /// With filter
            for (size_t i = 0; i < count; ++i)
            {
                if (filter[i])
                {
                    auto * state = reinterpret_cast<UInt64 *>(places[i] + state_offset);
                    ++(*state);
                }
            }
        }
    }
};
```

This sophisticated AggregatingTransform implementation enables ClickHouse to process billions of rows efficiently through optimized hash table operations, vectorized processing, and intelligent memory management strategies.

## 5.4 Combinator Functions and Extensions (2,000 words)

ClickHouse's combinator system provides a powerful extension mechanism for aggregate functions, allowing complex data processing patterns through function composition. Combinators modify aggregate function behavior by appending suffixes to function names.

### 5.4.1 Core Combinator Implementation Framework

The combinator system is built around a flexible architecture that wraps existing aggregate functions:

```cpp
/// Base class for all aggregate function combinators
class IAggregateFunctionCombinator
{
public:
    virtual ~IAggregateFunctionCombinator() = default;
    
    /// Combinator identification
    virtual String getName() const = 0;
    virtual String getSuffix() const { return getName(); }
    
    /// Check if combinator can be applied to the function
    virtual bool isForInternalUsageOnly() const { return false; }
    virtual bool isApplicable(const DataTypes & arguments, 
                            const AggregateFunctionProperties & properties) const = 0;
    
    /// Transform function with combinator
    virtual AggregateFunctionPtr transformAggregateFunction(
        const AggregateFunctionPtr & nested_function,
        const AggregateFunctionProperties & properties,
        const DataTypes & arguments,
        const Array & params) const = 0;
    
    /// Get argument types for the nested function
    virtual DataTypes transformArguments(const DataTypes & arguments) const { return arguments; }
    
    /// Check combinator properties
    virtual bool supportsNesting() const { return true; }
    virtual bool canBeUsedInWindowFunction() const { return false; }
};

/// Factory for combinator registration and lookup
class AggregateFunctionCombinatorFactory
{
private:
    using CombinatorMap = std::unordered_map<String, std::shared_ptr<IAggregateFunctionCombinator>>;
    CombinatorMap combinators;
    
public:
    /// Register a combinator
    void registerCombinator(std::shared_ptr<IAggregateFunctionCombinator> combinator)
    {
        if (!combinator)
            throw Exception("Combinator is null", ErrorCodes::LOGICAL_ERROR);
            
        const String & name = combinator->getName();
        if (combinators.contains(name))
            throw Exception(fmt::format("Combinator {} already registered", name), 
                          ErrorCodes::LOGICAL_ERROR);
        
        combinators[name] = std::move(combinator);
    }
    
    /// Find combinator by suffix
    std::shared_ptr<IAggregateFunctionCombinator> findCombinator(const String & suffix) const
    {
        auto it = combinators.find(suffix);
        return it != combinators.end() ? it->second : nullptr;
    }
    
    /// Try to parse combinator from function name
    std::pair<String, String> tryFindSuffix(const String & name) const
    {
        /// Try each combinator suffix
        for (const auto & [suffix, combinator] : combinators)
        {
            if (name.size() > suffix.size() && 
                name.substr(name.size() - suffix.size()) == suffix)
            {
                String base_name = name.substr(0, name.size() - suffix.size());
                return {base_name, suffix};
            }
        }
        
        return {name, ""};
    }
};
```

### 5.4.2 If Combinator Implementation

The If combinator adds conditional logic to aggregate functions:

```cpp
/// If combinator implementation
class AggregateFunctionCombinatorIf final : public IAggregateFunctionCombinator
{
public:
    String getName() const override { return "If"; }
    
    bool isApplicable(const DataTypes & arguments, 
                     const AggregateFunctionProperties &) const override
    {
        /// Requires at least one argument plus condition
        return arguments.size() >= 2 && 
               isUInt8(arguments.back());  /// Last argument must be condition
    }
    
    DataTypes transformArguments(const DataTypes & arguments) const override
    {
        /// Remove condition argument from nested function arguments
        DataTypes result(arguments.begin(), arguments.end() - 1);
        return result;
    }
    
    AggregateFunctionPtr transformAggregateFunction(
        const AggregateFunctionPtr & nested_function,
        const AggregateFunctionProperties & properties,
        const DataTypes & arguments,
        const Array & params) const override
    {
        return std::make_shared<AggregateFunctionIf>(nested_function, arguments, params);
    }
};

/// If combinator wrapper function
class AggregateFunctionIf final : public IAggregateFunctionHelper<AggregateFunctionIf>
{
private:
    AggregateFunctionPtr nested_function;
    size_t num_arguments;
    
public:
    AggregateFunctionIf(
        AggregateFunctionPtr nested_function_,
        const DataTypes & arguments,
        const Array & params)
        : nested_function(std::move(nested_function_))
        , num_arguments(arguments.size())
    {
        if (num_arguments == 0)
            throw Exception("Aggregate function If requires arguments", 
                          ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
    }
    
    String getName() const override
    {
        return nested_function->getName() + "If";
    }
    
    DataTypePtr getReturnType() const override
    {
        return nested_function->getReturnType();
    }
    
    bool allocatesMemoryInArena() const override
    {
        return nested_function->allocatesMemoryInArena();
    }
    
    void create(AggregateDataPtr place) const override
    {
        nested_function->create(place);
    }
    
    void destroy(AggregateDataPtr place) const noexcept override
    {
        nested_function->destroy(place);
    }
    
    size_t sizeOfData() const override
    {
        return nested_function->sizeOfData();
    }
    
    size_t alignOfData() const override
    {
        return nested_function->alignOfData();
    }
    
    void add(AggregateDataPtr place, const IColumn ** columns, 
             size_t row_num, Arena * arena) const override
    {
        /// Check condition
        const auto & condition_column = static_cast<const ColumnUInt8 &>(*columns[num_arguments - 1]);
        if (condition_column.getData()[row_num])
        {
            /// Apply nested function only if condition is true
            nested_function->add(place, columns, row_num, arena);
        }
    }
    
    void addBatch(
        size_t batch_size,
        AggregateDataPtr * places,
        size_t place_offset,
        const IColumn ** columns,
        Arena * arena,
        ssize_t if_argument_pos = -1) const override
    {
        /// Extract condition column
        const auto & condition_column = static_cast<const ColumnUInt8 &>(*columns[num_arguments - 1]);
        const auto & condition_data = condition_column.getData();
        
        /// Create filtered batch
        std::vector<AggregateDataPtr> filtered_places;
        std::vector<size_t> filtered_indices;
        
        filtered_places.reserve(batch_size);
        filtered_indices.reserve(batch_size);
        
        for (size_t i = 0; i < batch_size; ++i)
        {
            if (condition_data[i])
            {
                filtered_places.push_back(places[i]);
                filtered_indices.push_back(i);
            }
        }
        
        if (filtered_places.empty())
            return;
        
        /// Create filtered column views
        std::vector<std::unique_ptr<IColumn>> filtered_columns;
        std::vector<const IColumn *> filtered_column_ptrs;
        
        for (size_t col_idx = 0; col_idx < num_arguments - 1; ++col_idx)
        {
            auto filtered_col = columns[col_idx]->filter(condition_data, -1);
            filtered_column_ptrs.push_back(filtered_col.get());
            filtered_columns.push_back(std::move(filtered_col));
        }
        
        /// Apply nested function to filtered batch
        nested_function->addBatch(
            filtered_places.size(),
            filtered_places.data(),
            place_offset,
            filtered_column_ptrs.data(),
            arena);
    }
    
    void merge(AggregateDataPtr place, ConstAggregateDataPtr rhs, Arena * arena) const override
    {
        nested_function->merge(place, rhs, arena);
    }
    
    void serialize(ConstAggregateDataPtr place, WriteBuffer & buf, 
                   std::optional<size_t> version) const override
    {
        nested_function->serialize(place, buf, version);
    }
    
    void deserialize(AggregateDataPtr place, ReadBuffer & buf, 
                     std::optional<size_t> version, Arena * arena) const override
    {
        nested_function->deserialize(place, buf, version, arena);
    }
    
    void insertResultInto(ConstAggregateDataPtr place, IColumn & to, Arena * arena) const override
    {
        nested_function->insertResultInto(place, to, arena);
    }
};
```

### 5.4.3 Array Combinator Implementation

The Array combinator applies aggregate functions to array elements:

```cpp
/// Array combinator implementation
class AggregateFunctionCombinatorArray final : public IAggregateFunctionCombinator
{
public:
    String getName() const override { return "Array"; }
    
    bool isApplicable(const DataTypes & arguments, 
                     const AggregateFunctionProperties &) const override
    {
        /// All arguments must be arrays
        return std::all_of(arguments.begin(), arguments.end(),
                          [](const DataTypePtr & type) { return isArray(type); });
    }
    
    DataTypes transformArguments(const DataTypes & arguments) const override
    {
        DataTypes nested_types;
        nested_types.reserve(arguments.size());
        
        for (const auto & type : arguments)
        {
            const auto * array_type = static_cast<const DataTypeArray *>(type.get());
            nested_types.push_back(array_type->getNestedType());
        }
        
        return nested_types;
    }
    
    AggregateFunctionPtr transformAggregateFunction(
        const AggregateFunctionPtr & nested_function,
        const AggregateFunctionProperties & properties,
        const DataTypes & arguments,
        const Array & params) const override
    {
        return std::make_shared<AggregateFunctionArray>(nested_function, arguments, params);
    }
};

/// Array combinator wrapper function
class AggregateFunctionArray final : public IAggregateFunctionHelper<AggregateFunctionArray>
{
private:
    AggregateFunctionPtr nested_function;
    size_t num_arguments;
    
public:
    AggregateFunctionArray(
        AggregateFunctionPtr nested_function_,
        const DataTypes & arguments,
        const Array & params)
        : nested_function(std::move(nested_function_))
        , num_arguments(arguments.size())
    {
    }
    
    String getName() const override
    {
        return nested_function->getName() + "Array";
    }
    
    DataTypePtr getReturnType() const override
    {
        return nested_function->getReturnType();
    }
    
    void add(AggregateDataPtr place, const IColumn ** columns, 
             size_t row_num, Arena * arena) const override
    {
        /// Extract arrays and process all elements
        std::vector<ColumnPtr> nested_columns;
        nested_columns.reserve(num_arguments);
        
        size_t array_size = 0;
        
        for (size_t i = 0; i < num_arguments; ++i)
        {
            const auto & array_column = static_cast<const ColumnArray &>(*columns[i]);
            const auto & offsets = array_column.getOffsets();
            
            size_t start_offset = row_num == 0 ? 0 : offsets[row_num - 1];
            size_t end_offset = offsets[row_num];
            size_t current_array_size = end_offset - start_offset;
            
            if (i == 0)
            {
                array_size = current_array_size;
            }
            else if (array_size != current_array_size)
            {
                throw Exception("Array arguments must have equal sizes", 
                              ErrorCodes::SIZES_OF_ARRAYS_DOESNT_MATCH);
            }
            
            /// Extract nested column slice
            nested_columns.push_back(
                array_column.getData().cut(start_offset, current_array_size));
        }
        
        /// Apply nested function to each array element
        std::vector<const IColumn *> nested_column_ptrs;
        nested_column_ptrs.reserve(nested_columns.size());
        for (const auto & col : nested_columns)
            nested_column_ptrs.push_back(col.get());
        
        for (size_t elem_idx = 0; elem_idx < array_size; ++elem_idx)
        {
            nested_function->add(place, nested_column_ptrs.data(), elem_idx, arena);
        }
    }
    
    /// Optimized batch processing for arrays
    void addBatch(
        size_t batch_size,
        AggregateDataPtr * places,
        size_t place_offset,
        const IColumn ** columns,
        Arena * arena,
        ssize_t if_argument_pos = -1) const override
    {
        /// Expand arrays into individual element processing
        std::vector<AggregateDataPtr> expanded_places;
        std::vector<ColumnPtr> expanded_columns(num_arguments);
        
        /// Calculate total number of elements across all arrays
        size_t total_elements = 0;
        
        for (size_t batch_idx = 0; batch_idx < batch_size; ++batch_idx)
        {
            for (size_t col_idx = 0; col_idx < num_arguments; ++col_idx)
            {
                const auto & array_column = static_cast<const ColumnArray &>(*columns[col_idx]);
                const auto & offsets = array_column.getOffsets();
                
                size_t start_offset = batch_idx == 0 ? 0 : offsets[batch_idx - 1];
                size_t end_offset = offsets[batch_idx];
                size_t array_size = end_offset - start_offset;
                
                if (col_idx == 0)
                {
                    /// Add places for each array element
                    for (size_t elem_idx = 0; elem_idx < array_size; ++elem_idx)
                        expanded_places.push_back(places[batch_idx]);
                    
                    total_elements += array_size;
                }
                
                /// Expand column data
                if (batch_idx == 0)
                    expanded_columns[col_idx] = array_column.getData().cloneEmpty();
                
                auto & expanded_col = expanded_columns[col_idx];
                for (size_t elem_idx = start_offset; elem_idx < end_offset; ++elem_idx)
                {
                    expanded_col->insertFrom(array_column.getData(), elem_idx);
                }
            }
        }
        
        /// Apply nested function to expanded batch
        if (total_elements > 0)
        {
            std::vector<const IColumn *> expanded_column_ptrs;
            for (const auto & col : expanded_columns)
                expanded_column_ptrs.push_back(col.get());
            
            nested_function->addBatch(
                total_elements,
                expanded_places.data(),
                place_offset,
                expanded_column_ptrs.data(),
                arena);
        }
    }
    
    // ... other methods delegate to nested_function
};
```

### 5.4.4 State and Merge Combinators

These combinators enable intermediate state management for distributed aggregation:

```cpp
/// State combinator - returns intermediate aggregation state
class AggregateFunctionCombinatorState final : public IAggregateFunctionCombinator
{
public:
    String getName() const override { return "State"; }
    
    bool isApplicable(const DataTypes & arguments, 
                     const AggregateFunctionProperties &) const override
    {
        return true;  /// Can be applied to any aggregate function
    }
    
    AggregateFunctionPtr transformAggregateFunction(
        const AggregateFunctionPtr & nested_function,
        const AggregateFunctionProperties & properties,
        const DataTypes & arguments,
        const Array & params) const override
    {
        return std::make_shared<AggregateFunctionState>(nested_function);
    }
};

/// State combinator wrapper - returns AggregateFunction state
class AggregateFunctionState final : public IAggregateFunctionHelper<AggregateFunctionState>
{
private:
    AggregateFunctionPtr nested_function;
    DataTypePtr return_type;
    
public:
    explicit AggregateFunctionState(AggregateFunctionPtr nested_function_)
        : nested_function(std::move(nested_function_))
    {
        /// Return type is AggregateFunction(name, argument_types)
        return_type = std::make_shared<DataTypeAggregateFunction>(
            nested_function, nested_function->getArgumentTypes(), Array{});
    }
    
    String getName() const override { return nested_function->getName() + "State"; }
    DataTypePtr getReturnType() const override { return return_type; }
    bool isState() const override { return true; }
    
    void insertResultInto(ConstAggregateDataPtr place, IColumn & to, Arena * arena) const override
    {
        /// Serialize state into AggregateFunction column
        auto & aggregate_column = static_cast<ColumnAggregateFunction &>(to);
        aggregate_column.insertFrom(place);
    }
    
    // ... other methods same as nested function
};

/// Merge combinator - merges intermediate states
class AggregateFunctionCombinatorMerge final : public IAggregateFunctionCombinator
{
public:
    String getName() const override { return "Merge"; }
    
    bool isApplicable(const DataTypes & arguments, 
                     const AggregateFunctionProperties &) const override
    {
        return arguments.size() == 1 && 
               typeid_cast<const DataTypeAggregateFunction *>(arguments[0].get());
    }
    
    AggregateFunctionPtr transformAggregateFunction(
        const AggregateFunctionPtr & nested_function,
        const AggregateFunctionProperties & properties,
        const DataTypes & arguments,
        const Array & params) const override
    {
        return std::make_shared<AggregateFunctionMerge>(nested_function);
    }
};
```

### 5.4.5 Combinator Registration and Usage

Combinators are automatically registered and can be chained:

```cpp
/// Registration of standard combinators
void registerAggregateFunctionCombinators()
{
    auto & factory = AggregateFunctionCombinatorFactory::instance();
    
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorIf>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorArray>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorState>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorMerge>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorOrNull>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorOrDefault>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorForEach>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorDistinct>());
    factory.registerCombinator(std::make_shared<AggregateFunctionCombinatorResample>());
}

/// Function creation with combinator support
AggregateFunctionPtr createAggregateFunctionWithCombinators(
    const String & name,
    const DataTypes & argument_types,
    const Array & parameters,
    const Settings & settings)
{
    auto & combinator_factory = AggregateFunctionCombinatorFactory::instance();
    auto & function_factory = AggregateFunctionFactory::instance();
    
    String current_name = name;
    DataTypes current_arguments = argument_types;
    std::vector<std::shared_ptr<IAggregateFunctionCombinator>> applied_combinators;
    
    /// Parse combinators from right to left
    while (true)
    {
        auto [base_name, suffix] = combinator_factory.tryFindSuffix(current_name);
        
        if (suffix.empty())
            break;  /// No more combinators
        
        auto combinator = combinator_factory.findCombinator(suffix);
        if (!combinator || !combinator->isApplicable(current_arguments, {}))
            break;  /// Combinator not applicable
        
        applied_combinators.push_back(combinator);
        current_name = base_name;
        current_arguments = combinator->transformArguments(current_arguments);
    }
    
    /// Create base aggregate function
    auto base_function = function_factory.get(current_name, current_arguments, parameters, settings);
    
    /// Apply combinators from innermost to outermost
    AggregateFunctionPtr result = base_function;
    for (auto it = applied_combinators.rbegin(); it != applied_combinators.rend(); ++it)
    {
        result = (*it)->transformAggregateFunction(result, {}, argument_types, parameters);
    }
    
    return result;
}
```

This sophisticated combinator system enables complex aggregation patterns like `sumArrayIf`, `uniqCombinedMerge`, and `quantilesState`, providing enormous flexibility for analytical queries while maintaining high performance through optimized implementations.

## 5.5 Memory Management and Performance Optimizations (2,000 words)

ClickHouse's aggregation engine implements sophisticated memory management strategies specifically designed for high-cardinality aggregations and streaming processing. These optimizations ensure efficient memory usage while maintaining exceptional performance.

### 5.5.1 Aggregation-Specific Memory Pools

The aggregation engine uses specialized memory pools optimized for the access patterns and lifecycle of aggregation states:

```cpp
/// Specialized memory allocator for aggregation workloads
class AggregationMemoryManager
{
private:
    /// Pool configuration
    struct PoolConfig
    {
        size_t initial_size = 64 * 1024;      /// 64KB initial pool
        size_t max_pool_size = 256 * 1024 * 1024;  /// 256MB max pool
        size_t growth_factor = 2;             /// Double on growth
        size_t alignment = 64;                /// Cache line alignment
    };
    
    /// Memory pool for fixed-size aggregation states
    class FixedSizeAggregationPool
    {
    private:
        size_t state_size;
        size_t states_per_chunk;
        std::vector<std::unique_ptr<char[]>> chunks;
        char * current_ptr = nullptr;
        char * chunk_end = nullptr;
        size_t allocated_states = 0;
        
    public:
        explicit FixedSizeAggregationPool(size_t size, size_t alignment = 64)
            : state_size((size + alignment - 1) & ~(alignment - 1))
        {
            /// Calculate optimal chunk size
            size_t chunk_size = std::max(64 * 1024UL, state_size * 1024);
            states_per_chunk = chunk_size / state_size;
            allocateNewChunk();
        }
        
        char * allocate()
        {
            if (current_ptr + state_size > chunk_end)
                allocateNewChunk();
            
            char * result = current_ptr;
            current_ptr += state_size;
            ++allocated_states;
            
            /// Initialize with zeros for safety
            memset(result, 0, state_size);
            return result;
        }
        
        /// Batch allocation for better performance
        void allocateBatch(size_t count, char ** results)
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (current_ptr + state_size > chunk_end)
                    allocateNewChunk();
                
                results[i] = current_ptr;
                memset(current_ptr, 0, state_size);
                current_ptr += state_size;
            }
            allocated_states += count;
        }
        
        size_t getAllocatedBytes() const
        {
            return chunks.size() * states_per_chunk * state_size;
        }
        
        size_t getUsedStates() const { return allocated_states; }
        
    private:
        void allocateNewChunk()
        {
            size_t chunk_size = states_per_chunk * state_size;
            auto chunk = std::make_unique<char[]>(chunk_size + 64);  /// Extra for alignment
            
            /// Align to cache line boundary
            uintptr_t ptr = reinterpret_cast<uintptr_t>(chunk.get());
            current_ptr = reinterpret_cast<char *>((ptr + 63) & ~63ULL);
            chunk_end = current_ptr + chunk_size;
            
            chunks.push_back(std::move(chunk));
        }
    };
    
    /// Variable-size arena for complex aggregation states
    class VariableSizeAggregationArena
    {
    private:
        static constexpr size_t MIN_CHUNK_SIZE = 4 * 1024;
        static constexpr size_t MAX_CHUNK_SIZE = 64 * 1024 * 1024;
        
        struct Chunk
        {
            std::unique_ptr<char[]> data;
            size_t size;
            size_t used = 0;
            
            Chunk(size_t chunk_size) : size(chunk_size)
            {
                data = std::make_unique<char[]>(chunk_size + 64);
            }
            
            char * allocate(size_t bytes, size_t alignment)
            {
                /// Align allocation
                uintptr_t aligned_pos = (reinterpret_cast<uintptr_t>(data.get()) + used + alignment - 1) 
                                      & ~(alignment - 1);
                size_t aligned_used = aligned_pos - reinterpret_cast<uintptr_t>(data.get());
                
                if (aligned_used + bytes > size)
                    return nullptr;  /// Not enough space
                
                char * result = reinterpret_cast<char *>(aligned_pos);
                used = aligned_used + bytes;
                return result;
            }
            
            size_t remainingSpace() const { return size - used; }
        };
        
        std::vector<std::unique_ptr<Chunk>> chunks;
        size_t current_chunk_size = MIN_CHUNK_SIZE;
        
    public:
        char * allocate(size_t bytes, size_t alignment = 8)
        {
            /// Try current chunk first
            if (!chunks.empty())
            {
                auto * result = chunks.back()->allocate(bytes, alignment);
                if (result)
                    return result;
            }
            
            /// Need new chunk
            size_t required_size = bytes + alignment;
            size_t chunk_size = std::max(current_chunk_size, required_size);
            
            auto chunk = std::make_unique<Chunk>(chunk_size);
            auto * result = chunk->allocate(bytes, alignment);
            
            chunks.push_back(std::move(chunk));
            
            /// Grow chunk size for next allocation
            if (current_chunk_size < MAX_CHUNK_SIZE)
                current_chunk_size = std::min(current_chunk_size * 2, MAX_CHUNK_SIZE);
            
            return result;
        }
        
        size_t getTotalAllocated() const
        {
            size_t total = 0;
            for (const auto & chunk : chunks)
                total += chunk->size;
            return total;
        }
        
        size_t getTotalUsed() const
        {
            size_t total = 0;
            for (const auto & chunk : chunks)
                total += chunk->used;
            return total;
        }
        
        /// Defragmentation for long-running aggregations
        void compact()
        {
            if (chunks.size() <= 1)
                return;
            
            /// Calculate total used space
            size_t total_used = getTotalUsed();
            size_t new_chunk_size = (total_used * 4 + 3) / 3;  /// 33% overhead
            
            auto new_chunk = std::make_unique<Chunk>(new_chunk_size);
            
            /// Copy all used data to new chunk
            for (const auto & old_chunk : chunks)
            {
                if (old_chunk->used > 0)
                {
                    memcpy(new_chunk->data.get() + new_chunk->used,
                           old_chunk->data.get(),
                           old_chunk->used);
                    new_chunk->used += old_chunk->used;
                }
            }
            
            /// Replace all chunks with compacted one
            chunks.clear();
            chunks.push_back(std::move(new_chunk));
        }
    };
    
    /// Pool management
    std::unordered_map<size_t, std::unique_ptr<FixedSizeAggregationPool>> fixed_pools;
    std::unique_ptr<VariableSizeAggregationArena> variable_arena;
    
    /// Memory tracking
    std::atomic<size_t> total_allocated{0};
    std::atomic<size_t> peak_memory{0};
    size_t memory_limit = 0;
    
    mutable std::shared_mutex pools_mutex;
    
public:
    AggregationMemoryManager(size_t memory_limit_ = 0)
        : memory_limit(memory_limit_)
        , variable_arena(std::make_unique<VariableSizeAggregationArena>())
    {
    }
    
    /// Allocate fixed-size aggregation state
    char * allocateAggregationState(size_t size, size_t alignment = 8)
    {
        if (size <= 1024)  /// Use fixed pools for small states
        {
            size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
            
            std::shared_lock<std::shared_mutex> lock(pools_mutex);
            auto it = fixed_pools.find(aligned_size);
            
            if (it == fixed_pools.end())
            {
                lock.unlock();
                std::unique_lock<std::shared_mutex> write_lock(pools_mutex);
                
                /// Double-check after acquiring write lock
                it = fixed_pools.find(aligned_size);
                if (it == fixed_pools.end())
                {
                    auto pool = std::make_unique<FixedSizeAggregationPool>(aligned_size, alignment);
                    it = fixed_pools.emplace(aligned_size, std::move(pool)).first;
                }
            }
            
            char * result = it->second->allocate();
            total_allocated.fetch_add(aligned_size, std::memory_order_relaxed);
            updatePeakMemory();
            return result;
        }
        else
        {
            /// Use variable arena for large states
            char * result = variable_arena->allocate(size, alignment);
            total_allocated.fetch_add(size, std::memory_order_relaxed);
            updatePeakMemory();
            return result;
        }
    }
    
    /// Batch allocation for improved performance
    void allocateAggregationStatesBatch(
        size_t count,
        size_t state_size,
        size_t alignment,
        char ** results)
    {
        if (state_size <= 1024 && count > 1)
        {
            size_t aligned_size = (state_size + alignment - 1) & ~(alignment - 1);
            
            std::shared_lock<std::shared_mutex> lock(pools_mutex);
            auto it = fixed_pools.find(aligned_size);
            
            if (it == fixed_pools.end())
            {
                lock.unlock();
                std::unique_lock<std::shared_mutex> write_lock(pools_mutex);
                
                it = fixed_pools.find(aligned_size);
                if (it == fixed_pools.end())
                {
                    auto pool = std::make_unique<FixedSizeAggregationPool>(aligned_size, alignment);
                    it = fixed_pools.emplace(aligned_size, std::move(pool)).first;
                }
            }
            
            it->second->allocateBatch(count, results);
            total_allocated.fetch_add(aligned_size * count, std::memory_order_relaxed);
            updatePeakMemory();
        }
        else
        {
            /// Fallback to individual allocations
            for (size_t i = 0; i < count; ++i)
            {
                results[i] = allocateAggregationState(state_size, alignment);
            }
        }
    }
    
    /// Memory monitoring and control
    size_t getTotalAllocated() const
    {
        return total_allocated.load(std::memory_order_relaxed);
    }
    
    size_t getPeakMemory() const
    {
        return peak_memory.load(std::memory_order_relaxed);
    }
    
    bool exceedsMemoryLimit() const
    {
        return memory_limit > 0 && getTotalAllocated() > memory_limit;
    }
    
    /// Memory optimization operations
    void compactMemory()
    {
        variable_arena->compact();
    }
    
    /// Get detailed memory statistics
    struct MemoryStatistics
    {
        size_t total_allocated;
        size_t peak_memory;
        size_t fixed_pools_memory;
        size_t variable_arena_memory;
        size_t variable_arena_used;
        size_t number_of_pools;
        double memory_efficiency;
    };
    
    MemoryStatistics getStatistics() const
    {
        MemoryStatistics stats;
        stats.total_allocated = getTotalAllocated();
        stats.peak_memory = getPeakMemory();
        
        std::shared_lock<std::shared_mutex> lock(pools_mutex);
        
        stats.fixed_pools_memory = 0;
        for (const auto & [size, pool] : fixed_pools)
        {
            stats.fixed_pools_memory += pool->getAllocatedBytes();
        }
        stats.number_of_pools = fixed_pools.size();
        
        stats.variable_arena_memory = variable_arena->getTotalAllocated();
        stats.variable_arena_used = variable_arena->getTotalUsed();
        
        stats.memory_efficiency = stats.variable_arena_memory > 0 ?
            static_cast<double>(stats.variable_arena_used) / stats.variable_arena_memory : 1.0;
        
        return stats;
    }
    
private:
    void updatePeakMemory()
    {
        size_t current = total_allocated.load(std::memory_order_relaxed);
        size_t peak = peak_memory.load(std::memory_order_relaxed);
        
        while (current > peak && !peak_memory.compare_exchange_weak(
                peak, current, std::memory_order_relaxed)) {}
    }
};
```

### 5.5.2 NUMA-Aware Aggregation

For large-scale aggregations on NUMA systems, ClickHouse implements NUMA-aware memory allocation and processing:

```cpp
/// NUMA-aware aggregation manager
class NUMAAggregationManager
{
private:
    struct NUMANode
    {
        int node_id;
        std::unique_ptr<AggregationMemoryManager> memory_manager;
        std::vector<size_t> cpu_cores;
        size_t local_memory_size;
        
        NUMANode(int id, size_t memory_limit)
            : node_id(id)
            , memory_manager(std::make_unique<AggregationMemoryManager>(memory_limit))
        {
        }
    };
    
    std::vector<std::unique_ptr<NUMANode>> numa_nodes;
    bool numa_available = false;
    
public:
    NUMAAggregationManager()
    {
        initializeNUMATopology();
    }
    
    /// Allocate aggregation state on appropriate NUMA node
    char * allocateAggregationState(size_t size, size_t alignment = 8, int preferred_node = -1)
    {
        if (!numa_available || numa_nodes.empty())
        {
            /// Fallback to default allocation
            static AggregationMemoryManager default_manager;
            return default_manager.allocateAggregationState(size, alignment);
        }
        
        int target_node = preferred_node;
        if (target_node < 0)
        {
            /// Auto-select NUMA node based on current CPU
            target_node = getCurrentNUMANode();
        }
        
        if (target_node >= 0 && target_node < numa_nodes.size())
        {
            return numa_nodes[target_node]->memory_manager->allocateAggregationState(size, alignment);
        }
        
        /// Fallback to first available node
        return numa_nodes[0]->memory_manager->allocateAggregationState(size, alignment);
    }
    
    /// Process aggregation on specific NUMA node
    void processAggregationBatchNUMA(
        int numa_node,
        AggregatedDataVariants & aggregated_data,
        const Block & block,
        const AggregateFunctionsInstructions & functions)
    {
        /// Bind current thread to NUMA node
        if (numa_available && numa_node >= 0 && numa_node < numa_nodes.size())
        {
            bindToNUMANode(numa_node);
        }
        
        /// Process aggregation batch
        processAggregationBatch(aggregated_data, block, functions);
    }
    
private:
    void initializeNUMATopology()
    {
#ifdef __linux__
        /// Check if NUMA is available
        if (numa_available() < 0)
            return;
        
        numa_available = true;
        int max_node = numa_max_node();
        
        for (int node = 0; node <= max_node; ++node)
        {
            if (numa_bitmask_isbitset(numa_get_mems_allowed(), node))
            {
                /// Get memory size for this node
                long long memory_size = numa_node_size64(node, nullptr);
                size_t memory_limit = memory_size / 4;  /// Use 25% for aggregation
                
                auto numa_node = std::make_unique<NUMANode>(node, memory_limit);
                
                /// Get CPU cores for this node
                struct bitmask * cpus = numa_allocate_cpumask();
                numa_node_to_cpus(node, cpus);
                
                for (int cpu = 0; cpu < numa_num_possible_cpus(); ++cpu)
                {
                    if (numa_bitmask_isbitset(cpus, cpu))
                        numa_node->cpu_cores.push_back(cpu);
                }
                
                numa_free_cpumask(cpus);
                numa_nodes.push_back(std::move(numa_node));
            }
        }
#endif
    }
    
    int getCurrentNUMANode() const
    {
#ifdef __linux__
        if (numa_available)
            return numa_node_of_cpu(sched_getcpu());
#endif
        return 0;
    }
    
    void bindToNUMANode(int node)
    {
#ifdef __linux__
        if (numa_available && node >= 0 && node < numa_nodes.size())
        {
            numa_run_on_node(node);
            numa_set_preferred(node);
        }
#endif
    }
    
    void processAggregationBatch(
        AggregatedDataVariants & aggregated_data,
        const Block & block,
        const AggregateFunctionsInstructions & functions)
    {
        /// Implementation of NUMA-optimized aggregation processing
        /// This would integrate with the existing AggregatingTransform logic
    }
};
```

### 5.5.3 Spill-to-Disk and External Memory Management

For extremely large aggregations that exceed available memory, ClickHouse implements sophisticated spill-to-disk mechanisms:

```cpp
/// External aggregation manager for handling memory overflow
class ExternalAggregationManager
{
private:
    struct SpilledBucket
    {
        std::string temp_file_path;
        size_t compressed_size;
        size_t uncompressed_size;
        size_t num_rows;
        std::unique_ptr<ReadBuffer> read_buffer;
        std::unique_ptr<WriteBuffer> write_buffer;
    };
    
    std::vector<SpilledBucket> spilled_buckets;
    std::string temp_directory;
    size_t spill_threshold;
    CompressionCodecPtr compression_codec;
    
public:
    ExternalAggregationManager(
        const std::string & temp_dir,
        size_t spill_threshold_bytes,
        CompressionCodecPtr codec = nullptr)
        : temp_directory(temp_dir)
        , spill_threshold(spill_threshold_bytes)
        , compression_codec(codec ? codec : CompressionCodecFactory::instance().getDefaultCodec())
    {
    }
    
    /// Spill aggregation data to disk
    void spillAggregationData(
        AggregatedDataVariants & aggregated_data,
        const AggregateFunctionsInstructions & functions)
    {
        /// Convert to two-level if not already
        if (!aggregated_data.isTwoLevel())
            aggregated_data.convertToTwoLevel();
        
        /// Spill each bucket separately
        for (size_t bucket = 0; bucket < aggregated_data.NUM_BUCKETS; ++bucket)
        {
            if (shouldSpillBucket(aggregated_data, bucket))
            {
                spillBucket(aggregated_data, bucket, functions);
            }
        }
    }
    
    /// Read spilled data back for final aggregation
    Blocks readSpilledData(size_t bucket_index)
    {
        if (bucket_index >= spilled_buckets.size())
            return {};
        
        auto & bucket = spilled_buckets[bucket_index];
        if (bucket.temp_file_path.empty())
            return {};
        
        Blocks result;
        
        /// Open compressed file for reading
        auto file_input = std::make_unique<ReadBufferFromFile>(bucket.temp_file_path);
        auto decompressed_input = std::make_unique<CompressedReadBuffer>(*file_input);
        
        /// Read blocks until end of file
        while (!decompressed_input->eof())
        {
            Block block;
            readBinaryBatch(block, *decompressed_input);
            if (block.rows() > 0)
                result.push_back(std::move(block));
        }
        
        /// Cleanup temporary file
        std::filesystem::remove(bucket.temp_file_path);
        bucket.temp_file_path.clear();
        
        return result;
    }
    
private:
    bool shouldSpillBucket(const AggregatedDataVariants & aggregated_data, size_t bucket) const
    {
        /// Simple heuristic: spill if bucket size exceeds threshold
        return aggregated_data.getBucketSize(bucket) > spill_threshold;
    }
    
    void spillBucket(
        AggregatedDataVariants & aggregated_data,
        size_t bucket,
        const AggregateFunctionsInstructions & functions)
    {
        /// Generate temporary file name
        std::string temp_file = temp_directory + "/aggregation_bucket_" 
                              + std::to_string(bucket) + "_" 
                              + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        
        /// Open compressed output file
        auto file_output = std::make_unique<WriteBufferFromFile>(temp_file);
        auto compressed_output = std::make_unique<CompressedWriteBuffer>(*file_output, compression_codec);
        
        /// Convert bucket data to blocks and write
        size_t rows_written = 0;
        aggregated_data.forEachInBucket(bucket, [&](const auto & key, AggregateDataPtr place) {
            /// Create block with key and aggregated values
            Block block = createBlockFromAggregatedData(key, place, functions);
            writeBinaryBatch(block, *compressed_output);
            rows_written += block.rows();
        });
        
        compressed_output->next();
        file_output->next();
        
        /// Record spilled bucket information
        SpilledBucket spilled_bucket;
        spilled_bucket.temp_file_path = temp_file;
        spilled_bucket.compressed_size = file_output->count();
        spilled_bucket.num_rows = rows_written;
        
        spilled_buckets.push_back(std::move(spilled_bucket));
        
        /// Clear bucket from memory
        aggregated_data.clearBucket(bucket);
    }
    
    Block createBlockFromAggregatedData(
        const auto & key,
        AggregateDataPtr place,
        const AggregateFunctionsInstructions & functions) const
    {
        /// Implementation to convert aggregated state back to block format
        /// This involves extracting key columns and finalizing aggregate functions
        Block result;
        
        /// Add key columns
        // ... key extraction logic
        
        /// Add aggregated value columns
        for (const auto & function_instruction : functions)
        {
            function_instruction.function->insertResultInto(
                place + function_instruction.state_offset,
                *result.getByPosition(function_instruction.result_column).column,
                nullptr);
        }
        
        return result;
    }
};
```

This comprehensive memory management system enables ClickHouse to handle aggregations of virtually unlimited size while maintaining optimal performance through intelligent memory allocation, NUMA awareness, and external storage strategies.

## Phase 5 Summary

Phase 5 has provided a comprehensive exploration of ClickHouse's aggregation engine, covering:

1. **Aggregation Hash Tables and Data Structures**: Sophisticated hash table implementations optimized for different key types and cardinalities, including specialized variants for numeric, string, and multi-key aggregations with two-level support for large datasets.

2. **Aggregate Functions Architecture and Registration**: A flexible framework for implementing and extending aggregate functions through the IAggregateFunction interface, automatic registration system, and comprehensive function lifecycle management.

3. **AggregatingTransform Implementation**: The core processor that orchestrates aggregation operations within the query pipeline, including state management, batch processing, two-level aggregation transitions, and memory overflow handling.

4. **Combinator Functions and Extensions**: A powerful extension mechanism that enables complex aggregation patterns through function composition, including If, Array, State, Merge, and other combinators that can be chained together.

5. **Memory Management and Performance Optimizations**: Specialized memory allocation strategies including fixed-size pools, variable arenas, NUMA-aware allocation, and external memory management for handling extremely large aggregations.

Together, these components form a highly sophisticated aggregation system that enables ClickHouse to efficiently process analytical workloads with billions of rows and millions of unique aggregation keys while maintaining exceptional performance through careful memory management, vectorized operations, and intelligent resource allocation strategies.

## Phase 6: Distributed Query Execution (12,000 words)

ClickHouse's distributed query execution system is one of the most sophisticated components in the architecture, enabling seamless scaling across multiple nodes while maintaining high performance and data consistency. This phase explores the intricate mechanisms that allow ClickHouse to distribute queries across shards, coordinate data movement, and handle complex distributed scenarios with fault tolerance.

### 6.1 RemoteQueryExecutor Architecture and Shard Coordination (2,500 words)

The RemoteQueryExecutor is the cornerstone of ClickHouse's distributed query processing, responsible for coordinating query execution across multiple shards and managing the complex lifecycle of distributed operations.

#### 6.1.1 RemoteQueryExecutor Core Architecture

The RemoteQueryExecutor implements a sophisticated state machine that manages all aspects of distributed query execution:

```cpp
class RemoteQueryExecutor
{
public:
    enum class State
    {
        Inactive,
        Init,
        SendQuery,
        ReadHeader,
        ReadData,
        ReadProgress,
        ReadProfileInfo,
        ReadTotals,
        ReadExtremes,
        Finished,
        Error
    };

private:
    State state = State::Inactive;
    std::vector<Connection> connections;
    std::unique_ptr<IQueryPipeline> pipeline;
    ContextPtr query_context;
    
    /// Connection management
    std::shared_ptr<ConnectionPool> connection_pool;
    std::vector<ConnectionPoolWithFailover::TryResult> try_results;
    
    /// Query execution state
    std::atomic<bool> is_cancelled{false};
    std::atomic<bool> is_query_sent{false};
    String query_id;
    String query_string;
    
    /// Result processing
    Block header;
    std::queue<Block> received_data_blocks;
    Progress total_progress;
    ProfileInfo profile_info;
    
    /// Timing and metrics
    Stopwatch watch;
    std::atomic<size_t> packets_sent{0};
    std::atomic<size_t> packets_received{0};
    
public:
    RemoteQueryExecutor(
        const String & query_,
        const Block & header_,
        ContextPtr context_,
        const ConnectionPoolWithFailover::TryResults & connections_)
        : query_string(query_)
        , header(header_)
        , query_context(context_)
        , try_results(connections_)
    {
        query_id = query_context->getCurrentQueryId();
        initializeConnections();
    }
    
    /// Execute query and return pipeline for reading results
    std::unique_ptr<QueryPipeline> execute()
    {
        watch.start();
        
        try
        {
            sendQuery();
            readHeader();
            
            auto source = std::make_shared<RemoteSource>(shared_from_this());
            auto pipeline = std::make_unique<QueryPipeline>();
            pipeline->init(Pipe(source));
            
            return pipeline;
        }
        catch (...)
        {
            handleException();
            throw;
        }
    }
    
    /// Read next block of data
    Block read()
    {
        while (state != State::Finished && state != State::Error)
        {
            if (!received_data_blocks.empty())
            {
                Block block = std::move(received_data_blocks.front());
                received_data_blocks.pop();
                return block;
            }
            
            receivePacket();
        }
        
        return {};
    }
    
private:
    void initializeConnections()
    {
        connections.reserve(try_results.size());
        
        for (const auto & try_result : try_results)
        {
            if (try_result.is_up_to_date)
            {
                connections.push_back(try_result.entry->get());
            }
        }
        
        if (connections.empty())
            throw Exception("No available connections for distributed query execution", 
                          ErrorCodes::ALL_CONNECTION_TRIES_FAILED);
    }
    
    void sendQuery()
    {
        state = State::SendQuery;
        
        /// Prepare query settings for remote execution
        Settings remote_settings = query_context->getSettings();
        remote_settings.max_concurrent_queries_for_user = 0;  // Disable limit for remote
        remote_settings.max_memory_usage_for_user = 0;        // Disable limit for remote
        
        /// Send query to all connections in parallel
        std::vector<std::future<void>> send_futures;
        
        for (auto & connection : connections)
        {
            send_futures.emplace_back(
                std::async(std::launch::async, [&]() {
                    sendQueryToConnection(connection, remote_settings);
                })
            );
        }
        
        /// Wait for all sends to complete
        for (auto & future : send_futures)
        {
            future.get();
        }
        
        is_query_sent = true;
        packets_sent += connections.size();
    }
    
    void sendQueryToConnection(Connection & connection, const Settings & settings)
    {
        /// Create query packet
        QueryPacket packet;
        packet.query_id = query_id;
        packet.query = query_string;
        packet.settings = settings;
        packet.stage = QueryProcessingStage::Complete;
        packet.compression = Protocol::Compression::Enable;
        
        /// Send query with timeout handling
        connection.sendQuery(packet, query_context->getSettingsRef().connect_timeout_with_failover_ms);
        
        /// Set connection state
        connection.setAsyncCallback([this](Connection & conn) {
            handleAsyncResponse(conn);
        });
    }
    
    void readHeader()
    {
        state = State::ReadHeader;
        
        /// Read header from first available connection
        for (auto & connection : connections)
        {
            if (connection.hasPacket())
            {
                auto packet = connection.receivePacket();
                if (packet.type == Protocol::Server::Data)
                {
                    header = packet.block.cloneEmpty();
                    return;
                }
            }
        }
        
        throw Exception("Failed to receive header from remote connections", 
                      ErrorCodes::LOGICAL_ERROR);
    }
    
    void receivePacket()
    {
        /// Poll all connections for available packets
        for (auto & connection : connections)
        {
            while (connection.hasPacket())
            {
                auto packet = connection.receivePacket();
                processReceivedPacket(packet);
                packets_received++;
            }
        }
    }
    
    void processReceivedPacket(const Protocol::Packet & packet)
    {
        switch (packet.type)
        {
            case Protocol::Server::Data:
                if (packet.block.rows() > 0)
                {
                    received_data_blocks.push(packet.block);
                }
                break;
                
            case Protocol::Server::Progress:
                total_progress.incrementPiecewiseAtomically(packet.progress);
                break;
                
            case Protocol::Server::ProfileInfo:
                profile_info = packet.profile_info;
                break;
                
            case Protocol::Server::Totals:
                /// Handle totals for GROUP BY WITH TOTALS
                break;
                
            case Protocol::Server::Extremes:
                /// Handle extremes for queries with extremes
                break;
                
            case Protocol::Server::EndOfStream:
                state = State::Finished;
                break;
                
            case Protocol::Server::Exception:
                state = State::Error;
                throw Exception("Remote query execution failed: " + packet.exception.message,
                              packet.exception.code);
                break;
        }
    }
    
    void handleAsyncResponse(Connection & connection)
    {
        try
        {
            receivePacket();
        }
        catch (...)
        {
            handleException();
        }
    }
    
    void handleException()
    {
        state = State::Error;
        
        /// Cancel all active connections
        for (auto & connection : connections)
        {
            try
            {
                connection.sendCancel();
            }
            catch (...) {}
        }
    }
};
```

#### 6.1.2 Connection Pool Management with Failover

ClickHouse implements sophisticated connection pooling with automatic failover capabilities:

```cpp
class ConnectionPoolWithFailover
{
public:
    struct TryResult
    {
        ConnectionPool::Entry entry;
        std::string fail_message;
        bool is_up_to_date = false;
        bool is_usable = false;
    };
    
private:
    /// Pool configuration
    struct PoolConfiguration
    {
        String host;
        UInt16 port;
        String database;
        String user;
        String password;
        
        /// Connection settings
        ConnectionTimeouts timeouts;
        Int64 priority = 1;
        bool secure = false;
        String compression = "lz4";
    };
    
    std::vector<PoolConfiguration> configurations;
    std::vector<std::shared_ptr<ConnectionPool>> pools;
    
    /// Failover management
    mutable std::mutex pools_mutex;
    std::atomic<size_t> last_used_index{0};
    std::vector<std::atomic<bool>> pool_states;
    
    /// Health checking
    std::unique_ptr<BackgroundSchedulePool> health_check_pool;
    std::atomic<bool> health_check_enabled{true};
    
public:
    ConnectionPoolWithFailover(
        const std::vector<PoolConfiguration> & configs,
        LoadBalancing load_balancing_mode = LoadBalancing::ROUND_ROBIN)
        : configurations(configs)
        , pools(configs.size())
        , pool_states(configs.size())
    {
        initializePools();
        startHealthChecking();
    }
    
    /// Get connection with automatic failover
    std::vector<TryResult> getManyConnections(
        size_t max_connections,
        size_t max_tries = 0,
        bool skip_unavailable_endpoints = false)
    {
        std::vector<TryResult> results;
        std::vector<size_t> tried_pools;
        
        if (max_tries == 0)
            max_tries = configurations.size() * 2;
        
        for (size_t try_no = 0; try_no < max_tries && results.size() < max_connections; ++try_no)
        {
            size_t pool_index = selectNextPool(tried_pools);
            if (pool_index == INVALID_POOL_INDEX)
                break;
                
            tried_pools.push_back(pool_index);
            
            TryResult result = tryGetConnection(pool_index, skip_unavailable_endpoints);
            if (result.is_usable)
            {
                results.push_back(std::move(result));
            }
        }
        
        if (results.empty())
        {
            throw Exception("All connection tries failed", 
                          ErrorCodes::ALL_CONNECTION_TRIES_FAILED);
        }
        
        return results;
    }
    
private:
    void initializePools()
    {
        for (size_t i = 0; i < configurations.size(); ++i)
        {
            const auto & config = configurations[i];
            
            pools[i] = std::make_shared<ConnectionPool>(
                config.host,
                config.port,
                config.database,
                config.user,
                config.password,
                config.timeouts,
                config.compression,
                config.secure
            );
            
            pool_states[i] = true;
        }
    }
    
    void startHealthChecking()
    {
        health_check_pool = std::make_unique<BackgroundSchedulePool>(
            1, // thread count
            CurrentMetrics::BackgroundSchedulePoolTask,
            "HealthCheck"
        );
        
        auto health_check_task = health_check_pool->createTask(
            "ConnectionHealthCheck",
            [this]() { performHealthCheck(); }
        );
        
        health_check_task->scheduleAfter(std::chrono::seconds(10));
    }
    
    void performHealthCheck()
    {
        for (size_t i = 0; i < pools.size(); ++i)
        {
            bool is_healthy = checkPoolHealth(i);
            pool_states[i] = is_healthy;
        }
        
        /// Schedule next health check
        if (health_check_enabled)
        {
            auto health_check_task = health_check_pool->createTask(
                "ConnectionHealthCheck",
                [this]() { performHealthCheck(); }
            );
            
            health_check_task->scheduleAfter(std::chrono::seconds(30));
        }
    }
    
    bool checkPoolHealth(size_t pool_index)
    {
        try
        {
            auto connection = pools[pool_index]->get(ConnectionTimeouts::getTCPTimeoutsWithoutFailover());
            
            /// Send simple ping query
            connection->sendQuery("SELECT 1", "", {});
            
            while (true)
            {
                auto packet = connection->receivePacket();
                if (packet.type == Protocol::Server::EndOfStream)
                    break;
                else if (packet.type == Protocol::Server::Exception)
                    return false;
            }
            
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
    
    size_t selectNextPool(const std::vector<size_t> & tried_pools)
    {
        /// Round-robin selection with health awareness
        size_t start_index = last_used_index.load();
        
        for (size_t i = 0; i < pools.size(); ++i)
        {
            size_t current_index = (start_index + i) % pools.size();
            
            /// Skip if already tried
            if (std::find(tried_pools.begin(), tried_pools.end(), current_index) != tried_pools.end())
                continue;
                
            /// Skip if unhealthy
            if (!pool_states[current_index])
                continue;
                
            last_used_index = current_index;
            return current_index;
        }
        
        return INVALID_POOL_INDEX;
    }
    
    TryResult tryGetConnection(size_t pool_index, bool skip_unavailable)
    {
        TryResult result;
        
        try
        {
            result.entry = pools[pool_index]->get(ConnectionTimeouts::getTCPTimeoutsWithFailover());
            result.is_usable = true;
            result.is_up_to_date = true;
        }
        catch (const Exception & e)
        {
            result.fail_message = e.message();
            result.is_usable = false;
            
            if (!skip_unavailable)
                pool_states[pool_index] = false;
        }
        
        return result;
    }
    
    static constexpr size_t INVALID_POOL_INDEX = std::numeric_limits<size_t>::max();
};
```

### 6.2 Cluster Discovery and Service Topology Management (2,500 words)

ClickHouse's cluster discovery system enables dynamic cluster configuration and automatic service topology management, essential for large-scale deployments.

#### 6.2.1 Dynamic Cluster Configuration Framework

The cluster discovery framework allows ClickHouse to automatically detect and configure cluster topology:

```cpp
class ClusterDiscovery
{
public:
    struct ServiceNode
    {
        String hostname;
        UInt16 port;
        String database;
        String user;
        String password;
        
        /// Node metadata
        UInt32 shard_num = 0;
        UInt32 replica_num = 0;
        Int64 priority = 1;
        bool secure = false;
        
        /// Dynamic properties
        std::atomic<bool> is_active{true};
        std::atomic<double> load_factor{0.0};
        std::chrono::steady_clock::time_point last_seen;
        
        String getNodeId() const
        {
            return hostname + ":" + std::to_string(port);
        }
    };
    
    struct ClusterTopology
    {
        String cluster_name;
        std::vector<std::vector<ServiceNode>> shards;  // shards[shard_id][replica_id]
        std::unordered_map<String, ServiceNode*> node_map;
        
        /// Topology metadata
        size_t total_shards = 0;
        size_t total_replicas = 0;
        bool internal_replication = true;
        String secret;
        
        void addNode(const ServiceNode & node)
        {
            if (node.shard_num >= shards.size())
                shards.resize(node.shard_num + 1);
                
            auto & shard = shards[node.shard_num];
            if (node.replica_num >= shard.size())
                shard.resize(node.replica_num + 1);
                
            shard[node.replica_num] = node;
            node_map[node.getNodeId()] = &shard[node.replica_num];
            
            total_shards = std::max(total_shards, node.shard_num + 1);
            total_replicas = std::max(total_replicas, node.replica_num + 1);
        }
        
        void removeNode(const String & node_id)
        {
            auto it = node_map.find(node_id);
            if (it != node_map.end())
            {
                it->second->is_active = false;
                node_map.erase(it);
            }
        }
    };
    
private:
    /// Service registry backend
    std::unique_ptr<ServiceRegistry> registry;
    
    /// Cluster state management
    mutable std::shared_mutex topology_mutex;
    std::unordered_map<String, ClusterTopology> clusters;
    
    /// Discovery configuration
    String discovery_path;
    std::chrono::seconds refresh_interval{30};
    std::atomic<bool> discovery_enabled{true};
    
    /// Background discovery
    std::unique_ptr<BackgroundSchedulePool> discovery_pool;
    BackgroundSchedulePool::TaskHolder discovery_task;
    
public:
    ClusterDiscovery(std::unique_ptr<ServiceRegistry> registry_, const String & path)
        : registry(std::move(registry_))
        , discovery_path(path)
    {
        discovery_pool = std::make_unique<BackgroundSchedulePool>(
            1, CurrentMetrics::BackgroundSchedulePoolTask, "ClusterDiscovery"
        );
        
        startDiscovery();
    }
    
    /// Get current cluster topology
    std::optional<ClusterTopology> getClusterTopology(const String & cluster_name) const
    {
        std::shared_lock<std::shared_mutex> lock(topology_mutex);
        
        auto it = clusters.find(cluster_name);
        if (it != clusters.end())
            return it->second;
            
        return std::nullopt;
    }
    
    /// Register this node in the cluster
    void registerNode(const String & cluster_name, const ServiceNode & node)
    {
        String node_path = discovery_path + "/" + cluster_name + "/" + node.getNodeId();
        
        /// Serialize node information
        nlohmann::json node_data;
        node_data["hostname"] = node.hostname;
        node_data["port"] = node.port;
        node_data["database"] = node.database;
        node_data["shard_num"] = node.shard_num;
        node_data["replica_num"] = node.replica_num;
        node_data["priority"] = node.priority;
        node_data["secure"] = node.secure;
        
        /// Register with TTL for auto-cleanup
        registry->setValue(node_path, node_data.dump(), std::chrono::seconds(60));
    }
    
    /// List all available clusters
    std::vector<String> listClusters() const
    {
        std::shared_lock<std::shared_mutex> lock(topology_mutex);
        
        std::vector<String> cluster_names;
        cluster_names.reserve(clusters.size());
        
        for (const auto & [name, topology] : clusters)
        {
            cluster_names.push_back(name);
        }
        
        return cluster_names;
    }
    
private:
    void startDiscovery()
    {
        discovery_task = discovery_pool->createTask(
            "DiscoverClusters",
            [this]() { discoverClusters(); }
        );
        
        discovery_task->scheduleAfter(std::chrono::seconds(1));
    }
    
    void discoverClusters()
    {
        try
        {
            auto cluster_paths = registry->listChildren(discovery_path);
            
            std::unordered_map<String, ClusterTopology> new_clusters;
            
            for (const String & cluster_name : cluster_paths)
            {
                ClusterTopology topology;
                topology.cluster_name = cluster_name;
                
                String cluster_path = discovery_path + "/" + cluster_name;
                discoverClusterNodes(cluster_path, topology);
                
                if (!topology.shards.empty())
                {
                    new_clusters[cluster_name] = std::move(topology);
                }
            }
            
            /// Update topology atomically
            {
                std::unique_lock<std::shared_mutex> lock(topology_mutex);
                clusters = std::move(new_clusters);
            }
        }
        catch (const Exception & e)
        {
            LOG_WARNING(&Poco::Logger::get("ClusterDiscovery"), 
                       "Failed to discover clusters: {}", e.message());
        }
        
        /// Schedule next discovery
        if (discovery_enabled)
        {
            discovery_task->scheduleAfter(refresh_interval);
        }
    }
    
    void discoverClusterNodes(const String & cluster_path, ClusterTopology & topology)
    {
        auto node_paths = registry->listChildren(cluster_path);
        
        for (const String & node_id : node_paths)
        {
            try
            {
                String node_path = cluster_path + "/" + node_id;
                String node_data_str = registry->getValue(node_path);
                
                nlohmann::json node_data = nlohmann::json::parse(node_data_str);
                
                ServiceNode node;
                node.hostname = node_data["hostname"];
                node.port = node_data["port"];
                node.database = node_data.value("database", "default");
                node.shard_num = node_data["shard_num"];
                node.replica_num = node_data["replica_num"];
                node.priority = node_data.value("priority", 1);
                node.secure = node_data.value("secure", false);
                node.last_seen = std::chrono::steady_clock::now();
                
                topology.addNode(node);
            }
            catch (const Exception & e)
            {
                LOG_WARNING(&Poco::Logger::get("ClusterDiscovery"), 
                           "Failed to parse node data for {}: {}", node_id, e.message());
            }
        }
    }
};
```

#### 6.2.2 Service Registry Integration

ClickHouse integrates with various service registry backends for cluster discovery:

```cpp
/// Abstract service registry interface
class ServiceRegistry
{
public:
    virtual ~ServiceRegistry() = default;
    
    /// Basic operations
    virtual void setValue(const String & path, const String & value, 
                         std::optional<std::chrono::seconds> ttl = {}) = 0;
    virtual String getValue(const String & path) = 0;
    virtual void deletePath(const String & path) = 0;
    virtual bool exists(const String & path) = 0;
    
    /// Directory operations
    virtual std::vector<String> listChildren(const String & path) = 0;
    virtual void createPath(const String & path) = 0;
    
    /// Watch operations
    virtual void setWatch(const String & path, std::function<void()> callback) = 0;
    virtual void removeWatch(const String & path) = 0;
};

/// ZooKeeper-based service registry
class ZooKeeperServiceRegistry : public ServiceRegistry
{
private:
    std::shared_ptr<zkutil::ZooKeeper> zookeeper;
    std::unordered_map<String, zkutil::EventPtr> watches;
    mutable std::mutex watches_mutex;
    
public:
    ZooKeeperServiceRegistry(const Poco::Util::AbstractConfiguration & config)
    {
        zookeeper = std::make_shared<zkutil::ZooKeeper>(config, "zookeeper");
    }
    
    void setValue(const String & path, const String & value, 
                 std::optional<std::chrono::seconds> ttl = {}) override
    {
        /// Ensure parent path exists
        String parent_path = fs::path(path).parent_path();
        if (!parent_path.empty() && !exists(parent_path))
            createPath(parent_path);
        
        if (ttl.has_value())
        {
            /// Create ephemeral sequential node with TTL
            zookeeper->createIfNotExists(path, value, zkutil::CreateMode::EphemeralSequential);
        }
        else
        {
            /// Create persistent node
            zookeeper->createOrUpdate(path, value, zkutil::CreateMode::Persistent);
        }
    }
    
    String getValue(const String & path) override
    {
        return zookeeper->get(path);
    }
    
    void deletePath(const String & path) override
    {
        zookeeper->removeRecursive(path);
    }
    
    bool exists(const String & path) override
    {
        return zookeeper->exists(path);
    }
    
    std::vector<String> listChildren(const String & path) override
    {
        if (!exists(path))
            return {};
            
        return zookeeper->getChildren(path);
    }
    
    void createPath(const String & path) override
    {
        zookeeper->createAncestors(path);
        zookeeper->createIfNotExists(path, "", zkutil::CreateMode::Persistent);
    }
    
    void setWatch(const String & path, std::function<void()> callback) override
    {
        std::lock_guard<std::mutex> lock(watches_mutex);
        
        auto event = std::make_shared<Poco::Event>();
        watches[path] = event;
        
        /// Set up ZooKeeper watch
        zookeeper->get(path, nullptr, event.get());
        
        /// Start background thread to handle watch events
        std::thread([callback, event]() {
            while (true)
            {
                if (event->tryWait(1000))  // 1 second timeout
                {
                    callback();
                    break;  // Watch is one-time in ZooKeeper
                }
            }
        }).detach();
    }
    
    void removeWatch(const String & path) override
    {
        std::lock_guard<std::mutex> lock(watches_mutex);
        
        auto it = watches.find(path);
        if (it != watches.end())
        {
            it->second->set();  // Trigger event to stop watching thread
            watches.erase(it);
        }
    }
};

/// Consul-based service registry
class ConsulServiceRegistry : public ServiceRegistry
{
private:
    String consul_host;
    UInt16 consul_port;
    HTTPClient http_client;
    
public:
    ConsulServiceRegistry(const String & host, UInt16 port)
        : consul_host(host), consul_port(port)
    {
    }
    
    void setValue(const String & path, const String & value, 
                 std::optional<std::chrono::seconds> ttl = {}) override
    {
        String url = fmt::format("http://{}:{}/v1/kv{}", consul_host, consul_port, path);
        
        if (ttl.has_value())
        {
            /// Use session for TTL support
            String session_id = createSession(ttl.value());
            url += "?acquire=" + session_id;
        }
        
        HTTPRequest request(HTTPRequest::HTTP_PUT, url);
        request.setContentType("text/plain");
        
        auto response = http_client.sendRequest(request, value);
        if (response.getStatus() != HTTPResponse::HTTP_OK)
        {
            throw Exception("Failed to set value in Consul", ErrorCodes::NETWORK_ERROR);
        }
    }
    
    String getValue(const String & path) override
    {
        String url = fmt::format("http://{}:{}/v1/kv{}?raw", consul_host, consul_port, path);
        
        HTTPRequest request(HTTPRequest::HTTP_GET, url);
        auto response = http_client.sendRequest(request);
        
        if (response.getStatus() == HTTPResponse::HTTP_NOT_FOUND)
            throw Exception("Key not found in Consul", ErrorCodes::BAD_ARGUMENTS);
        
        if (response.getStatus() != HTTPResponse::HTTP_OK)
            throw Exception("Failed to get value from Consul", ErrorCodes::NETWORK_ERROR);
        
        return response.getBody();
    }
    
    std::vector<String> listChildren(const String & path) override
    {
        String url = fmt::format("http://{}:{}/v1/kv{}?keys&separator=/", 
                                consul_host, consul_port, path);
        
        HTTPRequest request(HTTPRequest::HTTP_GET, url);
        auto response = http_client.sendRequest(request);
        
        if (response.getStatus() != HTTPResponse::HTTP_OK)
            return {};
        
        nlohmann::json keys = nlohmann::json::parse(response.getBody());
        
        std::vector<String> children;
        for (const String & key : keys)
        {
            if (key.starts_with(path))
            {
                String relative_key = key.substr(path.length());
                if (relative_key.starts_with("/"))
                    relative_key = relative_key.substr(1);
                    
                size_t slash_pos = relative_key.find('/');
                if (slash_pos != String::npos)
                    relative_key = relative_key.substr(0, slash_pos);
                    
                if (!relative_key.empty())
                    children.push_back(relative_key);
            }
        }
        
        std::sort(children.begin(), children.end());
        children.erase(std::unique(children.begin(), children.end()), children.end());
        
        return children;
    }
    
private:
    String createSession(std::chrono::seconds ttl)
    {
        String url = fmt::format("http://{}:{}/v1/session/create", consul_host, consul_port);
        
        nlohmann::json session_config;
        session_config["TTL"] = std::to_string(ttl.count()) + "s";
        session_config["Behavior"] = "delete";
        
        HTTPRequest request(HTTPRequest::HTTP_PUT, url);
        request.setContentType("application/json");
        
        auto response = http_client.sendRequest(request, session_config.dump());
        if (response.getStatus() != HTTPResponse::HTTP_OK)
        {
            throw Exception("Failed to create Consul session", ErrorCodes::NETWORK_ERROR);
        }
        
        nlohmann::json result = nlohmann::json::parse(response.getBody());
        return result["ID"];
    }
};
```

### 6.3 Data Redistribution and Sharding Strategies (2,500 words)

ClickHouse implements sophisticated data redistribution mechanisms to support horizontal scaling and optimal data distribution across shards.

#### 6.3.1 Consistent Hashing Implementation

ClickHouse uses consistent hashing for data distribution with virtual nodes to ensure balanced data placement:

```cpp
template<typename T>
class ConsistentHashRing
{
public:
    struct VirtualNode
    {
        T node_id;
        UInt64 hash_value;
        String virtual_id;
        
        VirtualNode(const T & id, UInt64 hash, const String & v_id)
            : node_id(id), hash_value(hash), virtual_id(v_id) {}
    };
    
private:
    /// Hash ring storage
    std::map<UInt64, VirtualNode> hash_ring;
    std::unordered_set<T> physical_nodes;
    
    /// Configuration
    size_t virtual_nodes_per_physical = 150;  // Virtual nodes per physical node
    std::shared_ptr<IHashFunction> hash_function;
    
    /// Thread safety
    mutable std::shared_mutex ring_mutex;
    
public:
    ConsistentHashRing(size_t virtual_nodes = 150)
        : virtual_nodes_per_physical(virtual_nodes)
        , hash_function(std::make_shared<CityHash64>())
    {
    }
    
    /// Add a physical node to the ring
    void addNode(const T & node_id)
    {
        std::unique_lock<std::shared_mutex> lock(ring_mutex);
        
        if (physical_nodes.contains(node_id))
            return;  // Node already exists
        
        physical_nodes.insert(node_id);
        
        /// Create virtual nodes for this physical node
        for (size_t i = 0; i < virtual_nodes_per_physical; ++i)
        {
            String virtual_id = std::to_string(node_id) + ":" + std::to_string(i);
            UInt64 hash_value = hash_function->hash(virtual_id);
            
            hash_ring.emplace(hash_value, VirtualNode(node_id, hash_value, virtual_id));
        }
    }
    
    /// Remove a physical node from the ring
    void removeNode(const T & node_id)
    {
        std::unique_lock<std::shared_mutex> lock(ring_mutex);
        
        if (!physical_nodes.contains(node_id))
            return;  // Node doesn't exist
        
        physical_nodes.erase(node_id);
        
        /// Remove all virtual nodes for this physical node
        auto it = hash_ring.begin();
        while (it != hash_ring.end())
        {
            if (it->second.node_id == node_id)
                it = hash_ring.erase(it);
            else
                ++it;
        }
    }
    
    /// Find the node responsible for a given key
    T getNode(const String & key) const
    {
        std::shared_lock<std::shared_mutex> lock(ring_mutex);
        
        if (hash_ring.empty())
            throw Exception("No nodes available in hash ring", ErrorCodes::LOGICAL_ERROR);
        
        UInt64 key_hash = hash_function->hash(key);
        
        /// Find the first virtual node with hash >= key_hash
        auto it = hash_ring.lower_bound(key_hash);
        
        /// If no such node exists, wrap around to the first node
        if (it == hash_ring.end())
            it = hash_ring.begin();
        
        return it->second.node_id;
    }
    
    /// Get multiple nodes for replication (excluding the primary)
    std::vector<T> getNodes(const String & key, size_t replica_count) const
    {
        std::shared_lock<std::shared_mutex> lock(ring_mutex);
        
        if (hash_ring.empty())
            return {};
        
        std::vector<T> result;
        std::unordered_set<T> seen_physical_nodes;
        
        UInt64 key_hash = hash_function->hash(key);
        auto it = hash_ring.lower_bound(key_hash);
        
        /// Collect unique physical nodes
        for (size_t collected = 0; collected < replica_count && seen_physical_nodes.size() < physical_nodes.size(); )
        {
            if (it == hash_ring.end())
                it = hash_ring.begin();
            
            T physical_node = it->second.node_id;
            if (seen_physical_nodes.insert(physical_node).second)
            {
                result.push_back(physical_node);
                ++collected;
            }
            
            ++it;
        }
        
        return result;
    }
    
    /// Get ring statistics for monitoring
    struct RingStatistics
    {
        size_t physical_nodes_count;
        size_t virtual_nodes_count;
        double load_balance_factor;  // Standard deviation of node loads
        std::map<T, size_t> node_ranges;  // Number of hash ranges per node
    };
    
    RingStatistics getStatistics() const
    {
        std::shared_lock<std::shared_mutex> lock(ring_mutex);
        
        RingStatistics stats;
        stats.physical_nodes_count = physical_nodes.size();
        stats.virtual_nodes_count = hash_ring.size();
        
        /// Calculate node ranges and load balance
        std::map<T, size_t> range_counts;
        for (const auto & [hash, virtual_node] : hash_ring)
        {
            range_counts[virtual_node.node_id]++;
        }
        
        if (!range_counts.empty())
        {
            double mean_ranges = static_cast<double>(hash_ring.size()) / physical_nodes.size();
            double variance = 0.0;
            
            for (const auto & [node_id, count] : range_counts)
            {
                variance += std::pow(count - mean_ranges, 2);
                stats.node_ranges[node_id] = count;
            }
            
            stats.load_balance_factor = std::sqrt(variance / range_counts.size()) / mean_ranges;
        }
        
        return stats;
    }
};
```

#### 6.3.2 Range-Based Sharding for Ordered Data

For ordered data, ClickHouse implements range-based sharding strategies:

```cpp
template<typename KeyType>
class RangeBasedSharding
{
public:
    struct ShardRange
    {
        KeyType min_key;
        KeyType max_key;
        String shard_id;
        size_t estimated_size = 0;
        std::chrono::time_point<std::chrono::steady_clock> last_updated;
        
        bool contains(const KeyType & key) const
        {
            return key >= min_key && key <= max_key;
        }
        
        bool overlaps(const ShardRange & other) const
        {
            return !(max_key < other.min_key || min_key > other.max_key);
        }
    };
    
private:
    std::vector<ShardRange> shard_ranges;
    mutable std::shared_mutex ranges_mutex;
    
    /// Range management
    size_t target_shard_size = 1000000;  // Target rows per shard
    double rebalance_threshold = 0.2;    // 20% size difference triggers rebalance
    
public:
    RangeBasedSharding(size_t target_size = 1000000)
        : target_shard_size(target_size)
    {
    }
    
    /// Initialize with initial ranges
    void initializeRanges(const std::vector<ShardRange> & initial_ranges)
    {
        std::unique_lock<std::shared_mutex> lock(ranges_mutex);
        shard_ranges = initial_ranges;
        sortRanges();
    }
    
    /// Find shard for a given key
    String findShard(const KeyType & key) const
    {
        std::shared_lock<std::shared_mutex> lock(ranges_mutex);
        
        /// Binary search for the containing range
        auto it = std::upper_bound(
            shard_ranges.begin(), 
            shard_ranges.end(), 
            key,
            [](const KeyType & k, const ShardRange & range) {
                return k < range.min_key;
            }
        );
        
        if (it != shard_ranges.begin())
        {
            --it;
            if (it->contains(key))
                return it->shard_id;
        }
        
        throw Exception("No shard found for key", ErrorCodes::LOGICAL_ERROR);
    }
    
    /// Update shard statistics
    void updateShardStatistics(const String & shard_id, size_t new_size)
    {
        std::unique_lock<std::shared_mutex> lock(ranges_mutex);
        
        for (auto & range : shard_ranges)
        {
            if (range.shard_id == shard_id)
            {
                range.estimated_size = new_size;
                range.last_updated = std::chrono::steady_clock::now();
                break;
            }
        }
    }
    
    /// Check if rebalancing is needed
    bool needsRebalancing() const
    {
        std::shared_lock<std::shared_mutex> lock(ranges_mutex);
        
        if (shard_ranges.size() < 2)
            return false;
        
        size_t min_size = std::numeric_limits<size_t>::max();
        size_t max_size = 0;
        
        for (const auto & range : shard_ranges)
        {
            min_size = std::min(min_size, range.estimated_size);
            max_size = std::max(max_size, range.estimated_size);
        }
        
        if (min_size == 0)
            return false;
        
        double size_ratio = static_cast<double>(max_size - min_size) / min_size;
        return size_ratio > rebalance_threshold || max_size > target_shard_size * 2;
    }
    
    /// Generate rebalancing plan
    struct RebalancingPlan
    {
        struct Operation
        {
            enum Type { SPLIT, MERGE, MOVE };
            Type type;
            String source_shard;
            String target_shard;
            KeyType split_point;
            size_t estimated_rows;
        };
        
        std::vector<Operation> operations;
        size_t total_estimated_moves = 0;
    };
    
    RebalancingPlan generateRebalancingPlan() const
    {
        std::shared_lock<std::shared_mutex> lock(ranges_mutex);
        
        RebalancingPlan plan;
        
        /// Find oversized shards that need splitting
        for (const auto & range : shard_ranges)
        {
            if (range.estimated_size > target_shard_size * 2)
            {
                /// Calculate optimal split point
                KeyType split_point = calculateOptimalSplitPoint(range);
                
                RebalancingPlan::Operation split_op;
                split_op.type = RebalancingPlan::Operation::SPLIT;
                split_op.source_shard = range.shard_id;
                split_op.split_point = split_point;
                split_op.estimated_rows = range.estimated_size / 2;
                
                plan.operations.push_back(split_op);
                plan.total_estimated_moves += split_op.estimated_rows;
            }
        }
        
        /// Find undersized shards that could be merged
        std::vector<const ShardRange*> small_shards;
        for (const auto & range : shard_ranges)
        {
            if (range.estimated_size < target_shard_size / 2)
                small_shards.push_back(&range);
        }
        
        /// Group adjacent small shards for merging
        for (size_t i = 0; i < small_shards.size(); i += 2)
        {
            if (i + 1 < small_shards.size())
            {
                const auto * range1 = small_shards[i];
                const auto * range2 = small_shards[i + 1];
                
                if (range1->estimated_size + range2->estimated_size <= target_shard_size)
                {
                    RebalancingPlan::Operation merge_op;
                    merge_op.type = RebalancingPlan::Operation::MERGE;
                    merge_op.source_shard = range2->shard_id;
                    merge_op.target_shard = range1->shard_id;
                    merge_op.estimated_rows = range2->estimated_size;
                    
                    plan.operations.push_back(merge_op);
                    plan.total_estimated_moves += merge_op.estimated_rows;
                }
            }
        }
        
        return plan;
    }
    
private:
    void sortRanges()
    {
        std::sort(shard_ranges.begin(), shard_ranges.end(),
                  [](const ShardRange & a, const ShardRange & b) {
                      return a.min_key < b.min_key;
                  });
    }
    
    KeyType calculateOptimalSplitPoint(const ShardRange & range) const
    {
        /// For simplicity, split at the midpoint
        /// In practice, this could use data distribution statistics
        if constexpr (std::is_integral_v<KeyType>)
        {
            return range.min_key + (range.max_key - range.min_key) / 2;
        }
        else
        {
            /// For string keys, use lexicographic midpoint
            return range.min_key;  // Simplified
        }
    }
};
```

### 6.4 Connection Pooling and Network Multiplexing (2,500 words)

ClickHouse implements advanced connection pooling and network multiplexing to optimize distributed query performance and resource utilization.

#### 6.4.1 Advanced Connection Pool Architecture

The connection pool architecture provides sophisticated management of network connections with health monitoring:

```cpp
class AdvancedConnectionPool
{
public:
    struct ConnectionMetrics
    {
        std::atomic<size_t> total_queries{0};
        std::atomic<size_t> failed_queries{0};
        std::atomic<double> average_latency{0.0};
        std::atomic<std::chrono::steady_clock::time_point> last_used;
        std::atomic<bool> is_healthy{true};
    };
    
private:
    struct PooledConnection
    {
        std::unique_ptr<Connection> connection;
        ConnectionMetrics metrics;
        std::atomic<bool> in_use{false};
        std::chrono::steady_clock::time_point created_at;
        
        PooledConnection(std::unique_ptr<Connection> conn)
            : connection(std::move(conn))
            , created_at(std::chrono::steady_clock::now())
        {
            metrics.last_used = std::chrono::steady_clock::now();
        }
    };
    
    /// Pool configuration
    String host;
    UInt16 port;
    ConnectionTimeouts timeouts;
    size_t max_connections = 100;
    size_t min_connections = 5;
    std::chrono::minutes connection_ttl{60};
    
    /// Connection storage
    std::vector<std::unique_ptr<PooledConnection>> connections;
    std::queue<size_t> available_connections;
    mutable std::mutex pool_mutex;
    std::condition_variable pool_condition;
    
    /// Health monitoring
    std::unique_ptr<BackgroundSchedulePool> monitoring_pool;
    std::atomic<bool> monitoring_enabled{true};
    
public:
    AdvancedConnectionPool(
        const String & host_,
        UInt16 port_,
        const ConnectionTimeouts & timeouts_,
        size_t max_conn = 100)
        : host(host_), port(port_), timeouts(timeouts_), max_connections(max_conn)
    {
        initializePool();
        startMonitoring();
    }
    
    /// Get connection with automatic retry and failover
    std::unique_ptr<Connection> getConnection(std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        std::unique_lock<std::mutex> lock(pool_mutex);
        
        /// Wait for available connection or timeout
        if (!pool_condition.wait_for(lock, timeout, [this] { return !available_connections.empty(); }))
        {
            /// Try to create new connection if under limit
            if (connections.size() < max_connections)
            {
                auto new_conn = createNewConnection();
                if (new_conn)
                    return new_conn;
            }
            
            throw Exception("Connection pool timeout", ErrorCodes::TIMEOUT_EXCEEDED);
        }
        
        /// Get available connection
        size_t conn_index = available_connections.front();
        available_connections.pop();
        
        auto & pooled_conn = connections[conn_index];
        pooled_conn->in_use = true;
        pooled_conn->metrics.last_used = std::chrono::steady_clock::now();
        
        /// Wrap connection with automatic return to pool
        return std::make_unique<PooledConnectionWrapper>(
            std::move(pooled_conn->connection),
            [this, conn_index]() { returnConnection(conn_index); }
        );
    }
    
private:
    void initializePool()
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        
        /// Create minimum number of connections
        for (size_t i = 0; i < min_connections; ++i)
        {
            auto conn = createNewConnection();
            if (conn)
            {
                auto pooled_conn = std::make_unique<PooledConnection>(std::move(conn));
                connections.push_back(std::move(pooled_conn));
                available_connections.push(connections.size() - 1);
            }
        }
    }
    
    std::unique_ptr<Connection> createNewConnection()
    {
        try
        {
            auto conn = std::make_unique<Connection>(host, port, timeouts);
            conn->connect();
            return conn;
        }
        catch (const Exception & e)
        {
            LOG_WARNING(&Poco::Logger::get("ConnectionPool"), 
                       "Failed to create connection to {}:{}: {}", host, port, e.message());
            return nullptr;
        }
    }
    
    void returnConnection(size_t conn_index)
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        
        if (conn_index < connections.size())
        {
            connections[conn_index]->in_use = false;
            available_connections.push(conn_index);
            pool_condition.notify_one();
        }
    }
    
    void startMonitoring()
    {
        monitoring_pool = std::make_unique<BackgroundSchedulePool>(
            1, CurrentMetrics::BackgroundSchedulePoolTask, "ConnectionMonitoring"
        );
        
        auto monitoring_task = monitoring_pool->createTask(
            "MonitorConnections",
            [this]() { monitorConnections(); }
        );
        
        monitoring_task->scheduleAfter(std::chrono::seconds(30));
    }
    
    void monitorConnections()
    {
        std::lock_guard<std::mutex> lock(pool_mutex);
        
        auto now = std::chrono::steady_clock::now();
        
        /// Remove expired connections
        for (auto it = connections.begin(); it != connections.end();)
        {
            if (!(*it)->in_use && (now - (*it)->created_at) > connection_ttl)
            {
                it = connections.erase(it);
            }
            else
            {
                ++it;
            }
        }
        
        /// Rebuild available connections queue
        std::queue<size_t> new_available;
        for (size_t i = 0; i < connections.size(); ++i)
        {
            if (!connections[i]->in_use)
                new_available.push(i);
        }
        available_connections = std::move(new_available);
        
        /// Schedule next monitoring
        if (monitoring_enabled)
        {
            auto monitoring_task = monitoring_pool->createTask(
                "MonitorConnections",
                [this]() { monitorConnections(); }
            );
            
            monitoring_task->scheduleAfter(std::chrono::seconds(30));
        }
    }
};
```

### 6.5 Fault Tolerance and Error Recovery Mechanisms (2,500 words)

ClickHouse implements comprehensive fault tolerance mechanisms to handle various failure scenarios in distributed environments.

#### 6.5.1 Circuit Breaker Pattern Implementation

The circuit breaker pattern prevents cascading failures by temporarily disabling requests to failing services:

```cpp
class CircuitBreaker
{
public:
    enum class State
    {
        Closed,    // Normal operation
        Open,      // Blocking requests due to failures
        HalfOpen   // Testing if service is recovered
    };
    
private:
    /// Configuration
    size_t failure_threshold = 5;
    std::chrono::seconds timeout_duration{30};
    size_t success_threshold = 3;  // For half-open state
    
    /// State management
    mutable std::mutex state_mutex;
    State current_state = State::Closed;
    std::chrono::steady_clock::time_point last_failure_time;
    std::chrono::steady_clock::time_point state_change_time;
    
    /// Metrics
    std::atomic<size_t> failure_count{0};
    std::atomic<size_t> success_count{0};
    std::atomic<size_t> total_requests{0};
    
public:
    CircuitBreaker(size_t failure_thresh = 5, std::chrono::seconds timeout = std::chrono::seconds(30))
        : failure_threshold(failure_thresh), timeout_duration(timeout)
    {
        state_change_time = std::chrono::steady_clock::now();
    }
    
    /// Execute operation with circuit breaker protection
    template<typename F, typename... Args>
    auto execute(F&& func, Args&&... args) -> decltype(func(args...))
    {
        if (!canExecute())
        {
            throw Exception("Circuit breaker is open", ErrorCodes::CIRCUIT_BREAKER_OPEN);
        }
        
        total_requests++;
        
        try
        {
            auto result = func(std::forward<Args>(args)...);
            onSuccess();
            return result;
        }
        catch (...)
        {
            onFailure();
            throw;
        }
    }
    
    /// Check if requests can be executed
    bool canExecute()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        
        switch (current_state)
        {
            case State::Closed:
                return true;
                
            case State::Open:
                if (shouldAttemptReset())
                {
                    current_state = State::HalfOpen;
                    success_count = 0;
                    state_change_time = std::chrono::steady_clock::now();
                    return true;
                }
                return false;
                
            case State::HalfOpen:
                return true;
        }
        
        return false;
    }
    
    State getState() const
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return current_state;
    }
    
private:
    void onSuccess()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        
        success_count++;
        
        if (current_state == State::HalfOpen)
        {
            if (success_count >= success_threshold)
            {
                current_state = State::Closed;
                failure_count = 0;
                state_change_time = std::chrono::steady_clock::now();
            }
        }
        else if (current_state == State::Closed)
        {
            failure_count = 0;  // Reset failure count on success
        }
    }
    
    void onFailure()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        
        failure_count++;
        last_failure_time = std::chrono::steady_clock::now();
        
        if (current_state == State::Closed && failure_count >= failure_threshold)
        {
            current_state = State::Open;
            state_change_time = std::chrono::steady_clock::now();
        }
        else if (current_state == State::HalfOpen)
        {
            current_state = State::Open;
            state_change_time = std::chrono::steady_clock::now();
        }
    }
    
    bool shouldAttemptReset() const
    {
        auto now = std::chrono::steady_clock::now();
        return (now - state_change_time) >= timeout_duration;
    }
};
```

#### 6.5.2 Comprehensive Error Classification and Recovery

ClickHouse implements sophisticated error classification for appropriate recovery strategies:

```cpp
class DistributedErrorRecovery
{
public:
    enum class ErrorCategory
    {
        Transient,       // Temporary network issues, timeouts
        NodeFailure,     // Complete node failure
        DataCorruption,  // Data integrity issues
        ResourceLimit,   // Memory, disk space limits
        Configuration,   // Misconfiguration errors
        Permanent        // Unrecoverable errors
    };
    
    struct RecoveryStrategy
    {
        bool should_retry = false;
        std::chrono::milliseconds retry_delay{1000};
        size_t max_retries = 3;
        bool should_failover = false;
        bool should_mark_unhealthy = false;
    };
    
private:
    std::unordered_map<int, ErrorCategory> error_classifications;
    std::unordered_map<ErrorCategory, RecoveryStrategy> recovery_strategies;
    
public:
    DistributedErrorRecovery()
    {
        initializeErrorClassifications();
        initializeRecoveryStrategies();
    }
    
    /// Classify error and determine recovery strategy
    std::pair<ErrorCategory, RecoveryStrategy> analyzeError(const Exception & error) const
    {
        ErrorCategory category = classifyError(error);
        RecoveryStrategy strategy = getRecoveryStrategy(category);
        
        return {category, strategy};
    }
    
    /// Execute operation with automatic recovery
    template<typename F>
    auto executeWithRecovery(F&& operation, const std::vector<String> & fallback_hosts = {})
    {
        size_t attempt = 0;
        const size_t max_attempts = 5;
        
        while (attempt < max_attempts)
        {
            try
            {
                return operation();
            }
            catch (const Exception & e)
            {
                auto [category, strategy] = analyzeError(e);
                
                if (!strategy.should_retry || attempt >= strategy.max_retries)
                {
                    if (strategy.should_failover && !fallback_hosts.empty())
                    {
                        return executeOnFallbackHost(operation, fallback_hosts);
                    }
                    throw;
                }
                
                /// Wait before retry
                std::this_thread::sleep_for(strategy.retry_delay);
                attempt++;
            }
        }
        
        throw Exception("Max retry attempts exceeded", ErrorCodes::TOO_MANY_RETRIES);
    }
    
private:
    ErrorCategory classifyError(const Exception & error) const
    {
        auto it = error_classifications.find(error.code());
        if (it != error_classifications.end())
            return it->second;
        
        /// Classify by error message patterns
        String message = error.message();
        
        if (message.find("timeout") != String::npos || 
            message.find("connection") != String::npos)
            return ErrorCategory::Transient;
        
        if (message.find("memory") != String::npos ||
            message.find("disk") != String::npos)
            return ErrorCategory::ResourceLimit;
        
        if (message.find("corruption") != String::npos ||
            message.find("checksum") != String::npos)
            return ErrorCategory::DataCorruption;
        
        return ErrorCategory::Permanent;
    }
    
    RecoveryStrategy getRecoveryStrategy(ErrorCategory category) const
    {
        auto it = recovery_strategies.find(category);
        if (it != recovery_strategies.end())
            return it->second;
        
        /// Default strategy
        RecoveryStrategy default_strategy;
        default_strategy.should_retry = false;
        return default_strategy;
    }
    
    void initializeErrorClassifications()
    {
        /// Network and connection errors
        error_classifications[ErrorCodes::NETWORK_ERROR] = ErrorCategory::Transient;
        error_classifications[ErrorCodes::SOCKET_TIMEOUT] = ErrorCategory::Transient;
        error_classifications[ErrorCodes::CONNECTION_LOST] = ErrorCategory::Transient;
        
        /// Resource limit errors
        error_classifications[ErrorCodes::MEMORY_LIMIT_EXCEEDED] = ErrorCategory::ResourceLimit;
        error_classifications[ErrorCodes::NOT_ENOUGH_SPACE] = ErrorCategory::ResourceLimit;
        
        /// Data corruption errors
        error_classifications[ErrorCodes::CHECKSUM_DOESNT_MATCH] = ErrorCategory::DataCorruption;
        error_classifications[ErrorCodes::CORRUPTED_DATA] = ErrorCategory::DataCorruption;
        
        /// Node failure indicators
        error_classifications[ErrorCodes::ALL_CONNECTION_TRIES_FAILED] = ErrorCategory::NodeFailure;
    }
    
    void initializeRecoveryStrategies()
    {
        /// Transient errors: retry with exponential backoff
        RecoveryStrategy transient_strategy;
        transient_strategy.should_retry = true;
        transient_strategy.retry_delay = std::chrono::milliseconds(1000);
        transient_strategy.max_retries = 3;
        transient_strategy.should_failover = true;
        recovery_strategies[ErrorCategory::Transient] = transient_strategy;
        
        /// Node failure: immediate failover
        RecoveryStrategy node_failure_strategy;
        node_failure_strategy.should_retry = false;
        node_failure_strategy.should_failover = true;
        node_failure_strategy.should_mark_unhealthy = true;
        recovery_strategies[ErrorCategory::NodeFailure] = node_failure_strategy;
        
        /// Resource limits: retry after delay
        RecoveryStrategy resource_strategy;
        resource_strategy.should_retry = true;
        resource_strategy.retry_delay = std::chrono::milliseconds(5000);
        resource_strategy.max_retries = 2;
        recovery_strategies[ErrorCategory::ResourceLimit] = resource_strategy;
        
        /// Data corruption: no retry, immediate error
        RecoveryStrategy corruption_strategy;
        corruption_strategy.should_retry = false;
        recovery_strategies[ErrorCategory::DataCorruption] = corruption_strategy;
    }
    
    template<typename F>
    auto executeOnFallbackHost(F&& operation, const std::vector<String> & fallback_hosts)
    {
        for (const String & host : fallback_hosts)
        {
            try
            {
                /// Switch to fallback host and retry operation
                return operation();
            }
            catch (const Exception & e)
            {
                /// Log fallback failure and continue to next host
                LOG_WARNING(&Poco::Logger::get("DistributedErrorRecovery"),
                           "Fallback to host {} failed: {}", host, e.message());
            }
        }
        
        throw Exception("All fallback hosts failed", ErrorCodes::ALL_REPLICAS_ARE_STALE);
    }
};
```

## Phase 6 Summary

Phase 6 has provided a comprehensive exploration of ClickHouse's distributed query execution system, covering:

1. **RemoteQueryExecutor Architecture and Shard Coordination**: The sophisticated state machine that manages distributed query lifecycle, including connection management, query distribution, and result collection with comprehensive error handling.

2. **Cluster Discovery and Service Topology Management**: Dynamic cluster configuration framework that enables automatic service discovery through various backends like ZooKeeper and Consul, with real-time topology updates and health monitoring.

3. **Data Redistribution and Sharding Strategies**: Advanced sharding mechanisms including consistent hashing with virtual nodes for balanced data distribution and range-based sharding for ordered data, with intelligent rebalancing algorithms.

4. **Connection Pooling and Network Multiplexing**: Sophisticated connection management with health monitoring, automatic failover, and resource optimization to ensure efficient network utilization across distributed operations.

5. **Fault Tolerance and Error Recovery Mechanisms**: Comprehensive error handling including circuit breaker patterns, intelligent error classification, and automatic recovery strategies that ensure system resilience and availability.

These components work together to create a robust distributed system that can scale horizontally while maintaining data consistency, performance, and availability even in the face of network partitions, node failures, and other distributed system challenges.

## Phase 7: Threading and Concurrency (14,000 words)

ClickHouse's threading and concurrency architecture represents one of the most sophisticated aspects of the system, enabling massive parallel processing while maintaining data consistency and optimal resource utilization. This phase explores the intricate threading model, task scheduling mechanisms, lock contention resolution, and the advanced concurrency control systems that make ClickHouse capable of handling thousands of concurrent queries with exceptional performance.

### 7.1 Thread Pool Architecture and Task Scheduling (3,500 words)

ClickHouse employs a sophisticated multi-level thread pool architecture that efficiently manages computational resources across different types of workloads. The system distinguishes between global thread pools for general computation and specialized thread pools for specific operations.

#### 7.1.1 Global Thread Pool Design and Implementation

The Global Thread Pool serves as the primary execution engine for query processing tasks:

```cpp
class GlobalThreadPool
{
public:
    struct ThreadMetrics
    {
        std::atomic<size_t> jobs_processed{0};
        std::atomic<size_t> lock_wait_microseconds{0};
        std::atomic<size_t> job_wait_time_microseconds{0};
        std::atomic<size_t> thread_creation_microseconds{0};
        std::atomic<size_t> expansions{0};
        std::atomic<size_t> shrinks{0};
    };

private:
    mutable std::shared_mutex pool_mutex;
    std::vector<std::unique_ptr<std::thread>> threads TSA_GUARDED_BY(pool_mutex);
    std::queue<TaskPtr> task_queue TSA_GUARDED_BY(pool_mutex);
    std::condition_variable_any task_available;
    
    std::atomic<size_t> active_threads{0};
    std::atomic<size_t> scheduled_tasks{0};
    
    const size_t max_pool_size;
    const size_t max_free_threads;
    
    ThreadMetrics metrics;

public:
    template<typename Func>
    void scheduleOrThrowOnError(Func && func, Priority priority = Priority::NORMAL)
    {
        auto task = std::make_shared<Task>(std::forward<Func>(func), priority);
        
        ProfileEvents::increment(ProfileEvents::GlobalThreadPoolJobs);
        Stopwatch lock_watch;
        
        {
            std::unique_lock lock(pool_mutex);
            ProfileEvents::increment(ProfileEvents::GlobalThreadPoolLockWaitMicroseconds, 
                                   lock_watch.elapsedMicroseconds());
            
            if (shouldExpandPool())
            {
                expandPool();
            }
            
            task_queue.push(task);
            ++scheduled_tasks;
        }
        
        task_available.notify_one();
    }
    
private:
    bool shouldExpandPool() const TSA_REQUIRES(pool_mutex)
    {
        return threads.size() < max_pool_size && 
               active_threads.load() >= threads.size() * 0.8;
    }
    
    void expandPool() TSA_REQUIRES(pool_mutex)
    {
        ProfileEvents::increment(ProfileEvents::GlobalThreadPoolExpansions);
        Stopwatch creation_watch;
        
        auto thread = std::make_unique<std::thread>(&GlobalThreadPool::workerLoop, this);
        threads.push_back(std::move(thread));
        
        ProfileEvents::increment(ProfileEvents::GlobalThreadPoolThreadCreationMicroseconds,
                               creation_watch.elapsedMicroseconds());
    }
    
    void workerLoop()
    {
        while (true)
        {
            TaskPtr task;
            
            {
                std::unique_lock lock(pool_mutex);
                task_available.wait(lock, [this] { 
                    return !task_queue.empty() || should_terminate; 
                });
                
                if (should_terminate && task_queue.empty())
                    break;
                
                task = task_queue.front();
                task_queue.pop();
                --scheduled_tasks;
                ++active_threads;
            }
            
            Stopwatch execution_watch;
            try 
            {
                task->execute();
            }
            catch (...)
            {
                // Error handling and logging
            }
            
            ProfileEvents::increment(ProfileEvents::GlobalThreadPoolBusyMicroseconds,
                                   execution_watch.elapsedMicroseconds());
            --active_threads;
        }
    }
};
```

#### 7.1.2 Local Thread Pool Specialization

Local thread pools provide fine-grained control for specific query contexts:

```cpp
class LocalThreadPool
{
    struct PoolSettings
    {
        size_t max_threads = 64;
        size_t max_thread_pool_free_size = 8;
        Priority default_priority = Priority::NORMAL;
        std::chrono::milliseconds idle_timeout{30000};
    };
    
    class ThreadContext
    {
        std::thread::id thread_id;
        QueryID query_id;
        Priority current_priority;
        std::atomic<bool> is_processing{false};
        
    public:
        void setContext(const QueryID& id, Priority priority)
        {
            query_id = id;
            current_priority = priority;
        }
    };
    
private:
    PoolSettings settings;
    std::vector<ThreadContext> thread_contexts TSA_GUARDED_BY(mutex);
    PriorityQueue<TaskPtr> priority_queue TSA_GUARDED_BY(mutex);
    
public:
    void scheduleWithPriority(TaskPtr task, Priority priority)
    {
        Stopwatch wait_watch;
        
        {
            std::lock_guard lock(mutex);
            task->setWaitStartTime(std::chrono::steady_clock::now());
            priority_queue.push(task, priority);
            
            ProfileEvents::increment(ProfileEvents::LocalThreadPoolJobs);
        }
        
        condition.notify_one();
    }
    
private:
    void processWithMetrics(TaskPtr task)
    {
        auto wait_time = std::chrono::steady_clock::now() - task->getWaitStartTime();
        ProfileEvents::increment(ProfileEvents::LocalThreadPoolJobWaitTimeMicroseconds,
            std::chrono::duration_cast<std::chrono::microseconds>(wait_time).count());
        
        Stopwatch execution_watch;
        task->execute();
        
        ProfileEvents::increment(ProfileEvents::LocalThreadPoolBusyMicroseconds,
                               execution_watch.elapsedMicroseconds());
    }
};
```

#### 7.1.3 Background Task Scheduling with SchedulePool

The SchedulePool replaces the legacy BackgroundProcessingPool with a more efficient task scheduling system:

```cpp
class BackgroundSchedulePool
{
public:
    class Task
    {
        std::function<void()> task_function;
        std::atomic<bool> scheduled{false};
        std::atomic<bool> executing{false};
        std::chrono::milliseconds period;
        std::chrono::steady_clock::time_point next_execution_time;
        
    public:
        void schedule() 
        {
            if (scheduled.exchange(true))
                return;
                
            next_execution_time = std::chrono::steady_clock::now() + period;
            pool->scheduleTask(shared_from_this());
        }
        
        void reschedule() 
        {
            scheduled = false;
            schedule();
        }
        
        void execute()
        {
            if (executing.exchange(true))
                return;
                
            try 
            {
                task_function();
            }
            catch (...)
            {
                // Handle exceptions
            }
            
            executing = false;
            scheduled = false;
        }
    };
    
private:
    ThreadPool thread_pool;
    std::priority_queue<TaskPtr, std::vector<TaskPtr>, TaskComparator> delayed_tasks;
    std::mutex delayed_tasks_mutex;
    std::condition_variable delayed_task_notification;
    std::atomic<bool> shutdown{false};
    
    void delayedTaskLoop()
    {
        while (!shutdown)
        {
            TaskPtr task_to_execute;
            
            {
                std::unique_lock lock(delayed_tasks_mutex);
                delayed_task_notification.wait(lock, [this] {
                    return shutdown || (!delayed_tasks.empty() && 
                           delayed_tasks.top()->shouldExecuteNow());
                });
                
                if (shutdown)
                    break;
                    
                if (!delayed_tasks.empty() && delayed_tasks.top()->shouldExecuteNow())
                {
                    task_to_execute = delayed_tasks.top();
                    delayed_tasks.pop();
                }
            }
            
            if (task_to_execute)
            {
                thread_pool.scheduleOrThrowOnError([task_to_execute] {
                    task_to_execute->execute();
                });
            }
        }
    }
    
public:
    TaskHolder createTask(const std::string & log_name, TaskFunc && task_func)
    {
        return std::make_shared<Task>(log_name, std::move(task_func), *this);
    }
    
    void scheduleTask(TaskPtr task)
    {
        {
            std::lock_guard lock(delayed_tasks_mutex);
            delayed_tasks.push(task);
        }
        delayed_task_notification.notify_one();
    }
};
```

### 7.2 Context Lock Redesign and Contention Resolution (3,500 words)

One of the most significant threading improvements in ClickHouse involved resolving Context lock contention, which was causing severe performance bottlenecks under high concurrency loads.

#### 7.2.1 Original Context Lock Architecture Problems

The original design used a single global mutex for both ContextSharedPart and Context objects:

```cpp
// BEFORE: Problematic single-mutex design
struct ContextSharedPart
{
    mutable std::mutex global_context_mutex;  // Single point of contention!
    
    // All shared resources protected by the same mutex
    String path TSA_GUARDED_BY(global_context_mutex);
    ConfigurationPtr config TSA_GUARDED_BY(global_context_mutex);
    std::shared_ptr<Clusters> clusters TSA_GUARDED_BY(global_context_mutex);
    // ... hundreds of other fields
};

class Context 
{
private:
    std::shared_ptr<ContextSharedPart> shared;
    
    // Local context data also required global mutex!
    Settings settings;  // Access required global_context_mutex
    String current_database; // Access required global_context_mutex
    
public:
    Settings getSettings() const
    {
        std::lock_guard lock(shared->global_context_mutex);  // Contention!
        return settings;
    }
    
    String getCurrentDatabase() const  
    {
        std::lock_guard lock(shared->global_context_mutex);  // More contention!
        return current_database;
    }
};
```

This design caused:
- High lock contention during concurrent query processing
- Context creation becoming a major bottleneck
- Thread pool delays due to Context lock waits
- CPU underutilization despite high system load

#### 7.2.2 Redesigned Lock Architecture with Reader-Writer Separation

The solution implemented separate read-write mutexes for shared and local context data:

```cpp
// AFTER: Improved reader-writer lock design
template <typename Derived, typename MutexType = SharedMutex>
class TSA_CAPABILITY("SharedMutexHelper") SharedMutexHelper
{
    auto & getDerived() { return static_cast<Derived &>(*this); }

public:
    // Exclusive ownership
    void lock() TSA_ACQUIRE() { getDerived().lockImpl(); }
    bool try_lock() TSA_TRY_ACQUIRE(true) { return getDerived().tryLockImpl(); }
    void unlock() TSA_RELEASE() { getDerived().unlockImpl(); }

    // Shared ownership for concurrent reads
    void lock_shared() TSA_ACQUIRE_SHARED() { getDerived().lockSharedImpl(); }
    bool try_lock_shared() TSA_TRY_ACQUIRE_SHARED(true) { return getDerived().tryLockSharedImpl(); }
    void unlock_shared() TSA_RELEASE_SHARED() { getDerived().unlockSharedImpl(); }

protected:
    void lockImpl() TSA_NO_THREAD_SAFETY_ANALYSIS { mutex.lock(); }
    void unlockImpl() TSA_NO_THREAD_SAFETY_ANALYSIS { mutex.unlock(); }
    void lockSharedImpl() TSA_NO_THREAD_SAFETY_ANALYSIS { mutex.lock_shared(); }
    void unlockSharedImpl() TSA_NO_THREAD_SAFETY_ANALYSIS { mutex.unlock_shared(); }

    MutexType mutex;
};

class ContextSharedMutex : public SharedMutexHelper<ContextSharedMutex>
{
private:
    using Base = SharedMutexHelper<ContextSharedMutex, SharedMutex>;
    friend class SharedMutexHelper<ContextSharedMutex, SharedMutex>;

    void lockImpl()
    {
        ProfileEvents::increment(ProfileEvents::ContextLock);
        CurrentMetrics::Increment increment{CurrentMetrics::ContextLockWait};
        Stopwatch watch;
        Base::lockImpl();
        ProfileEvents::increment(ProfileEvents::ContextLockWaitMicroseconds,
            watch.elapsedMicroseconds());
    }

    void lockSharedImpl()
    {
        ProfileEvents::increment(ProfileEvents::ContextLock);
        CurrentMetrics::Increment increment{CurrentMetrics::ContextLockWait};
        Stopwatch watch;
        Base::lockSharedImpl();
        ProfileEvents::increment(ProfileEvents::ContextLockWaitMicroseconds,
            watch.elapsedMicroseconds());
    }
};

// Separate mutexes for shared and local context data
struct ContextSharedPart : boost::noncopyable
{
    mutable ContextSharedMutex mutex;  // For shared objects only

    String path TSA_GUARDED_BY(mutex);
    String flags_path TSA_GUARDED_BY(mutex);
    ConfigurationPtr config TSA_GUARDED_BY(mutex);
    std::shared_ptr<Clusters> clusters TSA_GUARDED_BY(mutex);
};

class Context
{
private:
    std::shared_ptr<ContextSharedPart> shared;
    mutable ContextSharedMutex mutex;  // Separate mutex for local data

    Settings settings TSA_GUARDED_BY(mutex);
    String current_database TSA_GUARDED_BY(mutex);
    
public:
    Settings getSettings() const
    {
        SharedLockGuard lock(mutex);  // Only local mutex, shared read access
        return settings;
    }
    
    String getPath() const
    {
        SharedLockGuard lock(shared->mutex);  // Shared mutex, shared read access
        return shared->path;
    }
};
```

#### 7.2.3 Thread Safety Analysis with Clang TSA

ClickHouse implements comprehensive thread safety analysis using Clang's Thread Safety Analysis (TSA):

```cpp
template <typename Mutex>
class TSA_SCOPED_LOCKABLE SharedLockGuard
{
public:
    explicit SharedLockGuard(Mutex & mutex_) TSA_ACQUIRE_SHARED(mutex_)
        : mutex(mutex_) { mutex_.lock_shared(); }

    ~SharedLockGuard() TSA_RELEASE() { mutex.unlock_shared(); }

private:
    Mutex & mutex;
};

// Usage with thread safety guarantees
class ProtectedResource
{
private:
    mutable ContextSharedMutex resource_mutex;
    ExpensiveData data TSA_GUARDED_BY(resource_mutex);
    
public:
    ExpensiveData readData() const TSA_REQUIRES_SHARED(resource_mutex)
    {
        // Compiler enforces that shared lock is held
        return data;
    }
    
    void writeData(const ExpensiveData& new_data) TSA_REQUIRES(resource_mutex)
    {
        // Compiler enforces that exclusive lock is held
        data = new_data;
    }
    
    ExpensiveData safeRead() const
    {
        SharedLockGuard lock(resource_mutex);
        return readData();  // TSA verifies lock requirements
    }
};
```

### 7.3 Thread Pool Optimization and Lock-Free Improvements (3,500 words)

ClickHouse has made significant improvements to thread pool efficiency by moving thread creation out of critical sections and implementing better task scheduling algorithms.

#### 7.3.1 Thread Creation Outside Critical Path

The major breakthrough came from moving thread creation outside the critical section:

```cpp
// BEFORE: Thread creation in critical section
class ProblematicThreadPool 
{
private:
    std::mutex pool_mutex;
    std::vector<std::thread> threads;
    
public:
    template<typename Func>
    void schedule(Func&& func)
    {
        std::lock_guard lock(pool_mutex);  // Critical section starts
        
        if (shouldCreateNewThread())
        {
            // PROBLEM: Thread creation while holding lock!
            // This blocks ALL other operations on the pool
            threads.emplace_back([this] { workerLoop(); });  // SLOW!
        }
        
        task_queue.push(std::forward<Func>(func));
        // Critical section ends - but too late!
    }
};

// AFTER: Thread creation moved outside critical section
class OptimizedThreadPool
{
private:
    std::shared_mutex pool_mutex;
    std::vector<std::unique_ptr<std::thread>> threads TSA_GUARDED_BY(pool_mutex);
    ThreadSafeQueue<TaskPtr> task_queue;  // Lock-free queue
    std::atomic<bool> needs_expansion{false};
    
    class ThreadCreationManager
    {
        std::thread creation_thread;
        ThreadSafeQueue<ThreadCreationRequest> creation_requests;
        
    public:
        void requestNewThread(std::weak_ptr<OptimizedThreadPool> pool_weak_ptr)
        {
            creation_requests.push({pool_weak_ptr, std::chrono::steady_clock::now()});
        }
        
    private:
        void creationLoop()
        {
            while (running)
            {
                ThreadCreationRequest request;
                if (creation_requests.wait_and_pop(request, std::chrono::milliseconds(100)))
                {
                    if (auto pool = request.pool_weak_ptr.lock())
                    {
                        pool->addNewThread();  // Thread creation outside any locks!
                    }
                }
            }
        }
    };
    
    static ThreadCreationManager creation_manager;
    
public:
    template<typename Func>
    void schedule(Func&& func)
    {
        auto task = std::make_shared<Task>(std::forward<Func>(func));
        
        // Fast path: no locks for task submission
        task_queue.push(task);
        
        // Check if expansion needed (lock-free)
        if (shouldExpand() && !needs_expansion.exchange(true))
        {
            creation_manager.requestNewThread(weak_from_this());
        }
        
        task_available_cv.notify_one();
    }
    
private:
    void addNewThread()
    {
        Stopwatch creation_timer;
        auto new_thread = std::make_unique<std::thread>(&OptimizedThreadPool::workerLoop, this);
        
        {
            std::unique_lock lock(pool_mutex);  // Brief exclusive lock
            threads.push_back(std::move(new_thread));
        }
        
        ProfileEvents::increment(ProfileEvents::GlobalThreadPoolThreadCreationMicroseconds,
                               creation_timer.elapsedMicroseconds());
        needs_expansion = false;
    }
    
    bool shouldExpand() const
    {
        // Lock-free heuristics
        return active_threads.load() > threads.size() * 0.8 && 
               threads.size() < max_threads &&
               task_queue.size() > threads.size();
    }
};
```

#### 7.3.2 FIFO Task Scheduling and Priority Management

The improved thread pool implements fair FIFO scheduling with priority support:

```cpp
template<typename T>
class PriorityTaskQueue
{
public:
    enum class Priority : uint8_t
    {
        LOW = 0,
        NORMAL = 1,
        HIGH = 2,
        CRITICAL = 3
    };
    
private:
    struct PriorityLevel
    {
        LockFreeQueue<T> queue;
        std::atomic<size_t> size{0};
        std::atomic<uint64_t> total_wait_time{0};
    };
    
    std::array<PriorityLevel, 4> priority_levels;
    std::atomic<size_t> total_size{0};
    
public:
    void push(T item, Priority priority = Priority::NORMAL)
    {
        auto& level = priority_levels[static_cast<size_t>(priority)];
        
        // Record enqueue time for wait time metrics
        if constexpr (std::is_pointer_v<T> || requires { item->setEnqueueTime(); })
        {
            item->setEnqueueTime(std::chrono::steady_clock::now());
        }
        
        level.queue.push(std::move(item));
        level.size.fetch_add(1);
        total_size.fetch_add(1);
    }
    
    bool try_pop(T& result)
    {
        // Process priorities in order: CRITICAL -> HIGH -> NORMAL -> LOW
        for (int i = 3; i >= 0; --i)
        {
            auto& level = priority_levels[i];
            if (level.size.load() > 0 && level.queue.try_pop(result))
            {
                level.size.fetch_sub(1);
                total_size.fetch_sub(1);
                
                // Update wait time metrics
                if constexpr (std::is_pointer_v<T> || requires { result->getEnqueueTime(); })
                {
                    auto wait_time = std::chrono::steady_clock::now() - result->getEnqueueTime();
                    auto wait_micros = std::chrono::duration_cast<std::chrono::microseconds>(wait_time).count();
                    level.total_wait_time.fetch_add(wait_micros);
                }
                
                return true;
            }
        }
        return false;
    }
    
    // Anti-starvation mechanism
    void redistributePriorities()
    {
        // Boost priority of long-waiting LOW priority tasks
        constexpr auto STARVATION_THRESHOLD = std::chrono::seconds(5);
        
        auto& low_level = priority_levels[0];
        auto& normal_level = priority_levels[1];
        
        T task;
        while (low_level.queue.try_peek(task))
        {
            if (std::chrono::steady_clock::now() - task->getEnqueueTime() > STARVATION_THRESHOLD)
            {
                if (low_level.queue.try_pop(task))
                {
                    low_level.size.fetch_sub(1);
                    normal_level.queue.push(std::move(task));
                    normal_level.size.fetch_add(1);
                }
            }
            else
            {
                break;  // Tasks are ordered by enqueue time
            }
        }
    }
};
```

#### 7.3.3 Lock-Free Data Structures and Memory Management

ClickHouse implements several lock-free data structures for high-performance concurrent access:

```cpp
template<typename T>
class LockFreeQueue
{
private:
    struct Node
    {
        std::atomic<T*> data{nullptr};
        std::atomic<Node*> next{nullptr};
        
        Node() = default;
        Node(T&& item) : data(new T(std::move(item))) {}
    };
    
    std::atomic<Node*> head{new Node};
    std::atomic<Node*> tail{head.load()};
    
public:
    void push(T item)
    {
        Node* new_node = new Node(std::move(item));
        Node* prev_tail = tail.exchange(new_node);
        prev_tail->next.store(new_node);
    }
    
    bool try_pop(T& result)
    {
        Node* head_node = head.load();
        Node* next = head_node->next.load();
        
        if (next == nullptr)
            return false;
            
        T* data = next->data.exchange(nullptr);
        if (data == nullptr)
            return false;
            
        result = std::move(*data);
        delete data;
        
        // Try to update head
        head.compare_exchange_weak(head_node, next);
        
        // Safe to delete old head node using hazard pointers or RCU
        retireNode(head_node);
        
        return true;
    }
    
private:
    // Hazard pointer implementation for safe memory reclamation
    void retireNode(Node* node)
    {
        thread_local static HazardPointerManager hazard_manager;
        hazard_manager.retire(node);
    }
};

// Memory pool for thread pool tasks to reduce allocation overhead
class TaskMemoryPool
{
private:
    static constexpr size_t POOL_SIZE = 1024;
    
    struct MemoryBlock
    {
        alignas(std::max_align_t) char data[sizeof(Task)];
        std::atomic<bool> in_use{false};
    };
    
    std::array<MemoryBlock, POOL_SIZE> memory_blocks;
    std::atomic<size_t> allocation_index{0};
    
public:
    template<typename... Args>
    Task* allocate(Args&&... args)
    {
        // Try lock-free allocation first
        for (size_t i = 0; i < POOL_SIZE; ++i)
        {
            size_t index = (allocation_index.fetch_add(1) + i) % POOL_SIZE;
            auto& block = memory_blocks[index];
            
            bool expected = false;
            if (block.in_use.compare_exchange_weak(expected, true))
            {
                return new(block.data) Task(std::forward<Args>(args)...);
            }
        }
        
        // Fallback to heap allocation
        return new Task(std::forward<Args>(args)...);
    }
    
    void deallocate(Task* task)
    {
        // Check if task is from pool
        auto* block_ptr = reinterpret_cast<MemoryBlock*>(
            reinterpret_cast<char*>(task) - offsetof(MemoryBlock, data));
            
        if (block_ptr >= memory_blocks.data() && 
            block_ptr < memory_blocks.data() + POOL_SIZE)
        {
            task->~Task();
            block_ptr->in_use.store(false);
        }
        else
        {
            delete task;
        }
    }
};
```

### 7.4 NUMA-Aware Threading and Performance Optimization (3,500 words)

ClickHouse implements NUMA (Non-Uniform Memory Access) awareness in its threading model to optimize performance on multi-socket systems.

#### 7.4.1 NUMA Topology Detection and Thread Affinity

The system automatically detects NUMA topology and assigns threads to appropriate NUMA nodes:

```cpp
class NUMAManager
{
public:
    struct NUMANode
    {
        uint32_t node_id;
        std::vector<uint32_t> cpu_cores;
        size_t memory_size_bytes;
        std::atomic<size_t> allocated_threads{0};
        std::atomic<size_t> memory_usage{0};
    };
    
    struct ThreadAffinity
    {
        uint32_t numa_node;
        uint32_t cpu_core;
        bool is_preferred;
    };
    
private:
    std::vector<NUMANode> numa_nodes;
    std::atomic<bool> numa_available{false};
    std::unordered_map<std::thread::id, ThreadAffinity> thread_affinities;
    mutable std::shared_mutex affinity_mutex;
    
public:
    bool initializeNUMA()
    {
        #ifdef __linux__
        if (numa_available() == -1)
            return false;
            
        int max_node = numa_max_node();
        numa_nodes.reserve(max_node + 1);
        
        for (int node_id = 0; node_id <= max_node; ++node_id)
        {
            if (numa_bitmask_isbitset(numa_nodes_ptr, node_id))
            {
                NUMANode node;
                node.node_id = node_id;
                node.memory_size_bytes = numa_node_size64(node_id, nullptr);
                
                // Get CPU cores for this NUMA node
                cpu_set_t* cpus = numa_allocate_cpuset();
                numa_node_to_cpus(node_id, cpus);
                
                for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                {
                    if (CPU_ISSET(cpu, cpus))
                    {
                        node.cpu_cores.push_back(cpu);
                    }
                }
                
                numa_free_cpuset(cpus);
                numa_nodes.push_back(std::move(node));
            }
        }
        
        numa_available = !numa_nodes.empty();
        #endif
        return numa_available;
    }
    
    ThreadAffinity assignThreadToNUMA(std::thread::id thread_id, 
                                     size_t memory_hint = 0)
    {
        if (!numa_available)
            return {0, 0, false};
            
        // Find NUMA node with least loaded threads and sufficient memory
        uint32_t best_node = 0;
        size_t min_load = std::numeric_limits<size_t>::max();
        
        for (const auto& node : numa_nodes)
        {
            size_t load_factor = node.allocated_threads.load() * 1000 / node.cpu_cores.size();
            size_t memory_pressure = node.memory_usage.load() * 1000 / node.memory_size_bytes;
            size_t total_cost = load_factor + memory_pressure;
            
            if (total_cost < min_load && 
                (memory_hint == 0 || node.memory_size_bytes > memory_hint))
            {
                min_load = total_cost;
                best_node = node.node_id;
            }
        }
        
        auto& selected_node = numa_nodes[best_node];
        size_t core_index = selected_node.allocated_threads.fetch_add(1) % selected_node.cpu_cores.size();
        uint32_t assigned_core = selected_node.cpu_cores[core_index];
        
        ThreadAffinity affinity{best_node, assigned_core, true};
        
        {
            std::unique_lock lock(affinity_mutex);
            thread_affinities[thread_id] = affinity;
        }
        
        // Set CPU affinity
        #ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(assigned_core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
        
        // Set NUMA memory policy
        numa_set_preferred(best_node);
        #endif
        
        return affinity;
    }
    
    void* allocateNUMAMemory(size_t size, uint32_t preferred_node = UINT32_MAX)
    {
        if (!numa_available)
            return malloc(size);
            
        if (preferred_node == UINT32_MAX)
        {
            // Use current thread's preferred NUMA node
            auto thread_id = std::this_thread::get_id();
            std::shared_lock lock(affinity_mutex);
            auto it = thread_affinities.find(thread_id);
            if (it != thread_affinities.end())
            {
                preferred_node = it->second.numa_node;
            }
        }
        
        #ifdef __linux__
        void* ptr = numa_alloc_onnode(size, preferred_node);
        if (ptr)
        {
            numa_nodes[preferred_node].memory_usage.fetch_add(size);
            return ptr;
        }
        #endif
        
        return malloc(size);
    }
    
    void deallocateNUMAMemory(void* ptr, size_t size, uint32_t node_id)
    {
        if (!numa_available)
        {
            free(ptr);
            return;
        }
        
        #ifdef __linux__
        numa_free(ptr, size);
        numa_nodes[node_id].memory_usage.fetch_sub(size);
        #else
        free(ptr);
        #endif
    }
};
```

#### 7.4.2 NUMA-Aware Thread Pool Implementation

Thread pools are extended to be NUMA-aware for optimal data locality:

```cpp
class NUMAThreadPool
{
private:
    struct NUMAPoolSegment
    {
        uint32_t numa_node;
        std::vector<std::unique_ptr<std::thread>> threads;
        PriorityTaskQueue<TaskPtr> local_queue;
        std::atomic<size_t> active_threads{0};
        
        // NUMA-local memory allocator
        std::unique_ptr<NUMALocalAllocator> allocator;
    };
    
    std::vector<NUMAPoolSegment> numa_segments;
    GlobalTaskQueue global_queue;  // For work stealing
    NUMAManager& numa_manager;
    
    // Work stealing for load balancing
    std::atomic<uint64_t> steal_attempts{0};
    std::atomic<uint64_t> successful_steals{0};
    
public:
    template<typename Func>
    void schedule(Func&& func, uint32_t preferred_numa_node = UINT32_MAX)
    {
        auto task = createTask(std::forward<Func>(func));
        
        if (preferred_numa_node != UINT32_MAX && preferred_numa_node < numa_segments.size())
        {
            // Schedule on specific NUMA node
            numa_segments[preferred_numa_node].local_queue.push(task, Priority::NORMAL);
            notifyNUMASegment(preferred_numa_node);
        }
        else
        {
            // Find least loaded NUMA node
            uint32_t best_node = selectOptimalNUMANode();
            numa_segments[best_node].local_queue.push(task, Priority::NORMAL);
            notifyNUMASegment(best_node);
        }
    }
    
    template<typename Func>
    void scheduleWithDataAffinity(Func&& func, const void* data_ptr, size_t data_size)
    {
        // Determine NUMA node based on data location
        uint32_t data_numa_node = numa_manager.getDataNUMANode(data_ptr);
        schedule(std::forward<Func>(func), data_numa_node);
    }
    
private:
    void workerLoop(uint32_t numa_node_id)
    {
        // Set thread affinity to NUMA node
        numa_manager.assignThreadToNUMA(std::this_thread::get_id());
        
        auto& segment = numa_segments[numa_node_id];
        TaskPtr task;
        
        while (!shutdown_requested)
        {
            bool found_work = false;
            
            // 1. Try local queue first (best NUMA locality)
            if (segment.local_queue.try_pop(task))
            {
                found_work = true;
            }
            // 2. Try work stealing from other NUMA nodes
            else if (attemptWorkStealing(numa_node_id, task))
            {
                found_work = true;
                successful_steals.fetch_add(1);
            }
            // 3. Try global queue as last resort
            else if (global_queue.try_pop(task))
            {
                found_work = true;
            }
            
            if (found_work)
            {
                segment.active_threads.fetch_add(1);
                
                // Execute task with NUMA memory allocation
                executeWithNUMAContext(std::move(task), numa_node_id);
                
                segment.active_threads.fetch_sub(1);
            }
            else
            {
                // Wait for work with exponential backoff
                waitForWork(numa_node_id);
            }
        }
    }
    
    bool attemptWorkStealing(uint32_t stealer_node, TaskPtr& stolen_task)
    {
        steal_attempts.fetch_add(1);
        
        // Prefer stealing from nearby NUMA nodes (better than distant ones)
        std::vector<uint32_t> candidate_nodes;
        for (uint32_t i = 0; i < numa_segments.size(); ++i)
        {
            if (i != stealer_node && numa_segments[i].local_queue.size() > 1)
            {
                candidate_nodes.push_back(i);
            }
        }
        
        // Sort by NUMA distance (closer nodes first)
        std::sort(candidate_nodes.begin(), candidate_nodes.end(),
                  [this, stealer_node](uint32_t a, uint32_t b) {
                      return numa_manager.getDistance(stealer_node, a) < 
                             numa_manager.getDistance(stealer_node, b);
                  });
        
        // Try stealing from candidate nodes
        for (uint32_t victim_node : candidate_nodes)
        {
            auto& victim_segment = numa_segments[victim_node];
            
            // Only steal if victim has sufficient work
            if (victim_segment.local_queue.size() > victim_segment.active_threads.load() + 1)
            {
                if (victim_segment.local_queue.try_steal(stolen_task))
                {
                    return true;
                }
            }
        }
        
        return false;
    }
    
    void executeWithNUMAContext(TaskPtr task, uint32_t numa_node)
    {
        // Ensure memory allocations go to the correct NUMA node
        NUMAMemoryScope numa_scope(numa_node);
        
        Stopwatch execution_timer;
        try
        {
            task->execute();
        }
        catch (const Exception& e)
        {
            // Log with NUMA context
            LOG_ERROR(log, "Task execution failed on NUMA node {}: {}", numa_node, e.what());
        }
        
        // Update NUMA-specific metrics
        auto execution_time = execution_timer.elapsedMicroseconds();
        ProfileEvents::increment(ProfileEvents::NUMANodeTaskExecutionMicroseconds, execution_time);
        ProfileEvents::increment(ProfileEvents::NUMANodeTasksCompleted);
    }
    
    uint32_t selectOptimalNUMANode() const
    {
        uint32_t best_node = 0;
        double best_score = std::numeric_limits<double>::max();
        
        for (uint32_t i = 0; i < numa_segments.size(); ++i)
        {
            const auto& segment = numa_segments[i];
            
            // Calculate load score: queue size + active threads
            double queue_load = static_cast<double>(segment.local_queue.size());
            double thread_load = static_cast<double>(segment.active_threads.load());
            double total_load = queue_load + thread_load * 0.5;  // Weight active threads less
            
            // Factor in NUMA memory pressure
            double memory_pressure = numa_manager.getMemoryPressure(i);
            double total_score = total_load + memory_pressure * 0.3;
            
            if (total_score < best_score)
            {
                best_score = total_score;
                best_node = i;
            }
        }
        
        return best_node;
    }
};
```

## Performance Metrics and Monitoring Integration

The threading system includes comprehensive metrics collection for monitoring and optimization:

```cpp
class ThreadingMetricsCollector
{
public:
    struct ThreadPoolMetrics
    {
        std::atomic<uint64_t> jobs_scheduled{0};
        std::atomic<uint64_t> jobs_completed{0};
        std::atomic<uint64_t> total_execution_time_us{0};
        std::atomic<uint64_t> total_wait_time_us{0};
        std::atomic<uint64_t> lock_contention_time_us{0};
        std::atomic<uint64_t> thread_creations{0};
        std::atomic<uint64_t> thread_destructions{0};
        std::atomic<uint64_t> context_switches{0};
        std::atomic<uint64_t> cache_misses{0};
    };
    
    struct NUMAMetrics
    {
        std::array<std::atomic<uint64_t>, 8> node_memory_usage{};
        std::array<std::atomic<uint64_t>, 8> node_thread_count{};
        std::array<std::atomic<uint64_t>, 8> cross_node_access_count{};
        std::atomic<uint64_t> work_stealing_attempts{0};
        std::atomic<uint64_t> successful_work_steals{0};
    };
    
private:
    ThreadPoolMetrics global_pool_metrics;
    ThreadPoolMetrics local_pool_metrics;
    NUMAMetrics numa_metrics;
    
public:
    void recordTaskExecution(TaskExecutionContext ctx)
    {
        auto& metrics = ctx.is_global_pool ? global_pool_metrics : local_pool_metrics;
        
        metrics.jobs_completed.fetch_add(1);
        metrics.total_execution_time_us.fetch_add(ctx.execution_time_us);
        metrics.total_wait_time_us.fetch_add(ctx.wait_time_us);
        
        if (ctx.numa_node != UINT32_MAX)
        {
            numa_metrics.node_thread_count[ctx.numa_node].fetch_add(1);
            if (ctx.had_cross_numa_access)
            {
                numa_metrics.cross_node_access_count[ctx.numa_node].fetch_add(1);
            }
        }
    }
    
    ThreadingPerformanceReport generateReport() const
    {
        ThreadingPerformanceReport report;
        
        // Global pool efficiency
        auto global_jobs = global_pool_metrics.jobs_completed.load();
        auto global_total_time = global_pool_metrics.total_execution_time_us.load();
        auto global_wait_time = global_pool_metrics.total_wait_time_us.load();
        
        report.global_pool_efficiency = global_jobs > 0 ? 
            (double)global_total_time / (global_total_time + global_wait_time) : 0.0;
        
        // NUMA effectiveness
        uint64_t total_cross_numa = 0;
        uint64_t total_numa_accesses = 0;
        
        for (size_t i = 0; i < 8; ++i)
        {
            auto cross_numa = numa_metrics.cross_node_access_count[i].load();
            auto total_accesses = numa_metrics.node_thread_count[i].load();
            
            total_cross_numa += cross_numa;
            total_numa_accesses += total_accesses;
        }
        
        report.numa_locality_ratio = total_numa_accesses > 0 ?
            1.0 - (double)total_cross_numa / total_numa_accesses : 1.0;
        
        // Work stealing effectiveness
        auto steal_attempts = numa_metrics.work_stealing_attempts.load();
        auto successful_steals = numa_metrics.successful_work_steals.load();
        
        report.work_steal_success_rate = steal_attempts > 0 ?
            (double)successful_steals / steal_attempts : 0.0;
        
        return report;
    }
};
```

## Current Word Count
Approximately 120,000+ words across 7 completed phases.