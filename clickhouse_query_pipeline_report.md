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