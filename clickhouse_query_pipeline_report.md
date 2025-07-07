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