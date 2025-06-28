#include <unordered_set>

#include "wabt/binary-reader.h"
#include "wabt/error-formatter.h"
#include "wabt/ir.h"
#include "wabt/cast.h"
#include "wabt/feature.h"
#include "wabt/opcode.h"
#include "wabt/common.h"
#include "Utils.h"
#ifndef PARSER_H
#define PARSER_H
inline std::string my_api_key = "yourgeminiapikey";
struct MemoryI {
    wabt::Memory* memory;
    size_t idx;
};

struct TableI {
    wabt::Table* table;
    size_t idx;
};

inline std::string FormatInt32(uint32_t v) {
    if (v <= 9) return std::to_string(v);
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v;
    return oss.str();
}

inline std::string FormatInt64(uint64_t v) {
    if (v <= 9) return std::to_string(v) + "n";
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << v << "n";
    return oss.str();
}

struct UsefulProperties {
    std::vector<wabt::Func*> funcs;
    std::vector<wabt::Import*> imports;
    std::vector<wabt::Export*> exports;
    std::vector<MemoryI> memories;
    std::vector<TableI> tables;
    std::vector<wabt::DataSegment*> dataSegments;
    std::vector<wabt::Global*> globals;
};
enum class LabelType { Block, Loop };
struct LabelInfo {
    std::string name;
    LabelType     type;
    std::size_t   resultCount;
};
enum class TaskType {
    PROCESS_EXPRESSION,
    EXIT_BLOCK,
    //PROCESS_ELSE,
    PROCESS_DO_WHILE_END,
    START_WHILE_LOOP,
    PROCESS_WHILE_BODY,
    PROCESS_WHILE_CONDITION,
    END_WHILE_LOOP,
    PROCESS_EXPRESSION_RANGE,
    START_ELSE,
    END_IF_ELSE,
    PROCESS_ELSE_BRANCH,
    END_ELSE_WITH_VALUE,
    END_THEN_WITH_VALUE,
    END_IF,
    END_BLOCK,
    END_LOOP,
    START_BLOCK,
    END_BLOCK_WITH_VALUE,
    END_DO_WHILE,
    END_FOR,
    END_FOR_WITH_VALUE,
    END_FOR_IN,
    END_FOR_OF,
    END_FOR_IN_WITH_VALUE,
    END_FOR_OF_WITH_VALUE,
    END_FUNCTION,
    END_FUNCTION_WITH_VALUE,
    START_FUNCTION,
    START_IMPORT,
    START_EXPORT,
    START_MEMORY,
    START_TABLE,
    START_DATA_SEGMENT,
    START_GLOBAL,
    START_MODULE,
    START_IMPORTS_OBJECT,
    START_MODULE_OBJECT,
    START_MODULE_OBJECT_IIFE,
    START_MODULE_OBJECT_ESM,
    START_MODULE_OBJECT_ESM_EXPORTS,
    START_MODULE_OBJECT_ESM_IMPORTS,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_EXPORTS,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_IMPORTS,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_IMPORTS_OBJECT,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_IMPORTS_OBJECT_EXPORTS,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_IMPORTS_OBJECT_IMPORTS,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_IMPORTS_OBJECT_IMPORTS_OBJECT,
    START_MODULE_OBJECT_ESM_IMPORTS_OBJECT_IIFE_IMPORTS_OBJECT_IMPORTS_OBJECT_EXPORTS,
    MERGE_IF_REACHABILITY
};

enum class ModuleFormat { IIFE, ESM };

struct Operand {
    std::string js_repr;
    bool is_constant = false;
    wabt::Type type;
    wabt::Const value;
};
struct ImportEntry {
    std::string field;
    std::string js_name;
};
using ModuleImports = std::unordered_map<std::string, std::vector<ImportEntry>>;
struct Task {
    TaskType type;
    const wabt::Expr* expr;
    bool needsLabel;
    bool emitBrace;         
    bool then_branch_ended_reachable;
    Task(TaskType t, const wabt::Expr* e, bool nLbl,
         bool eBrace = true, bool thenBranchEndedReachable = true) noexcept
        : type(t), expr(e), needsLabel(nLbl), emitBrace(eBrace), then_branch_ended_reachable(then_branch_ended_reachable){}
    constexpr Task(TaskType t, const wabt::Expr* e = nullptr) noexcept
        : type{t}, expr{e} {}
};

class Parser{
public:
    Parser();

    std::string GenerateCallJs(uint32_t funcIndex, const std::vector<Operand> &args);

    ~Parser();
    std::string Parse(const char* path, bool AI, ModuleFormat format = ModuleFormat::ESM);

    void CollectLocals(const wabt::Func *func, const std::unordered_set<std::string> &inlNames,
                       std::vector<std::string> &outList);

    void EmitImportsObject(std::ostringstream &out);
private:
    wabt::Errors errors;
    std::unique_ptr<wabt::Module> module;
    wabt::Features features;
    wabt::ReadBinaryOptions options;
    std::unordered_set<uint32_t> problematic_indices;
    std::unordered_map<uint32_t, std::string> function_sources;
    std::unordered_map<uint32_t, std::string> ai_summaries;
    std::unordered_map<uint32_t, std::vector<uint32_t>> call_graph;
    std::unordered_set<uint32_t> inlinable_function_indices;
    std::unordered_set<uint32_t> functions_to_trace;
    std::unordered_set<uint32_t> mutated_globals;
    std::vector<std::string> func_names;
    std::vector<std::string> generated_stub_decls;
    std::unordered_map<uint32_t,std::string> pretty_name;
    const std::unordered_set<std::string> JS_RESERVED_WORDS = {
        "await", "break", "case", "catch", "class", "const", "continue", "debugger",
        "default", "delete", "do", "else", "enum", "export", "extends", "false",
        "finally", "for", "function", "if", "implements", "import", "in",
        "instanceof", "interface", "let", "new", "null", "package", "private",
        "protected", "public", "return", "static", "super", "switch", "this",
        "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield"
    };
    std::string PrettyFuncName(uint32_t func_index);

    uint32_t inline_tmp_id = 0;

    std::vector<std::string> ExtractLocals(const std::string &func_src);

    wabt::Result LoadModule(const char *path, wabt::Errors &errors);
    UsefulProperties ParseModule(wabt::Module* module);

    const wabt::ModuleField *GetModuleField(const wabt::Var &var);


    const wabt::FuncSignature* GetFuncSignature(const wabt::Var& func_var);


    void FindCallsRecursive(const wabt::ExprList& exprs, std::vector<uint32_t>& called_indices);

    void ProcessExprList(wabt::Func *func,
                         uint32_t func_idx, const wabt::ExprList &exprs,
                         std::vector<std::pair<std::string, LabelType>> &labelStack,
                         int &nextLabelId,
                         std::stack<Operand> &js_stack,
                         std::vector<std::string> &js_statements,
                         std::stack<bool> &reachability_stack,
                         const std::unordered_map<int, std::string> &local_names);

    Operand pop_or_throw(std::stack<Operand> &st, std::stack<bool> &reachability_stack, const char *where);

    Operand peek_or_throw(std::stack<Operand> &st, std::stack<bool> &reachability_stack, const char *where);

    void AnalyzeFunctionDependencies(wabt::Func *func, uint32_t func_idx);
    std::string BuildStoreExpression(std::stack<Operand> &js_stack, std::stack<bool> &reachability_stack, const std::string &expr, char div);
    std::vector<Operand> UnstackArgs(std::stack<Operand> &js_stack, std::stack<bool> &reachability_stack, size_t paramCount);
    std::string FillBrackets(const std::string &scope);
    void removeSemicolons(std::vector<std::string>& js_statements);

    bool DoesBlockNeedLabel(const wabt::Block & block);

    bool IsExportedFunc(uint32_t func_idx);


    void IdentifyInlinableFunctions();

    void ProcessFunctionBody(wabt::Func &func, uint32_t realIndex, const wabt::ExprList &initial_exprs,
                             size_t start, size_t end,
                             std::vector<std::pair<std::string, LabelType>> &labelStack,
                             int &nextLabelId,
                             std::stack<Operand> &js_stack,
                             std::vector<std::string> &js_statements,
                             const std::unordered_map<int, std::string> &local_names);

    std::string GetExportValue(wabt::Export *exp);

    std::string ProcessExpr(wabt::Func *func,
                            uint32_t func_idx, const wabt::Expr &expr,
                            std::vector<std::pair<std::string, LabelType>> &labelStack,
                            int &nextLabelId,
                            std::stack<Operand> &js_stack, std::vector<std::string> &js_statements,
                            std::stack<bool> &reachability_stack,
                            const std::unordered_map<int, std::string> &local_names);
    std::string ProcessFunction(wabt::Func* func, uint32_t realIndex);
    std::string ProcessExport(wabt::Export *exp);

    void ProcessImport(wabt::Import *imp, uint32_t import_index, std::ostringstream &prologue);
    std::string ProcessMemory(const MemoryI &mi);
    std::string ProcessTable(const TableI &ti);
    bool TryToProcessAsForLoop(wabt::LoopExpr loopExpr, std::vector<wabt::Expr> exprs
    );
    bool TryProcessAsDoWhileLoop(const wabt::LoopExpr & loop,
                                 std::vector<std::pair<std::string, LabelType>> &labelStack,
                                 int &nextLabelId,
                                 std::stack<std::string> &js_stack,
                                 std::vector<std::string> &js_statements);
    bool TryProcessAsWhileLoop(const wabt::LoopExpr & loop, std::vector<std::pair<std::string, LabelType>> &labelStack, int &nextLabelId, std::stack<
                               Operand> &js_stack, std::vector<std::string> &js_statements, std::stack<bool> &reachability_stack);
    std::tuple<uint32_t, std::string> ProcessSegment(wabt::DataSegment* seg);

    std::string InjectLetDeclarations(const std::string &func_src);
};



#endif //PARSER_H
