#pragma once
#include "pch.h"
#include "Parser.h"
#include <cmath>
#include <ranges>
#include <sstream>
#include <unordered_set>
#include "wabt/binary-reader-ir.h"
#include "wabt/error-formatter.h"
#include <wabt/opcode.h>

#include "wabt/ir.h"
#include "wabt/ir.h"
#include "wabt/ir.h"


int seh_filter(unsigned int code) {
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_EXECUTE_HANDLER;
    } else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static std::unordered_set<uint32_t> g_inliner_blacklist;
static std::unordered_set<std::string> current_inline_names;

Parser::~Parser() = default;

Parser::Parser() {
    this->features.EnableAll();
    this->problematic_indices = {};
    this->options.features = this->features;
    this->options.log_stream = nullptr;
    this->options.read_debug_names = true;
    this->options.stop_on_first_error = false;
    this->functions_to_trace = {};
}

std::string ConstToJS(const wabt::Const &c) {
    using wabt::Type;
    switch (c.type()) {
        case Type::I32: return std::to_string(c.u32());
        case Type::I64: return std::to_string(c.u64()) + "n";
        case Type::F32: return std::to_string(c.f32_bits()) + " /* f32 */";
        case Type::F64: return std::to_string(c.f64_bits()) + " /* f64 */";
        default: return "0 /* unsupported const */";
    }
}

std::string Parser::GenerateCallJs(uint32_t funcIndex,
                                   const std::vector<Operand> &args) {
    std::ostringstream ss;
    ss << PrettyFuncName(funcIndex) << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        ss << args[i].js_repr;
        if (i + 1 < args.size()) ss << ", ";
    }
    ss << ")";
    return ss.str();
}

template<typename ExprContainer>
static const wabt::Expr *FirstExprPtr(const ExprContainer &exprs) {
    if (exprs.empty())
        return nullptr;

    if constexpr (std::is_pointer_v<decltype(exprs.front())>)
        return exprs.front();
    else if constexpr (std::is_reference_v<decltype(exprs.front())>) {
        return &exprs.front();
    }
    else
        return exprs.front().get();
}

template<typename ExprContainer>
std::string InitExprListToJS(const ExprContainer &exprs,
                             const std::string &gprefix = "global") {
    const wabt::Expr *e = FirstExprPtr(exprs);
    if (!e)
        return "0 /* empty init */";

    if (auto *cexpr = wabt::dyn_cast<wabt::ConstExpr>(e))
        return ConstToJS(cexpr->const_);

    if (auto *gget = wabt::dyn_cast<wabt::GlobalGetExpr>(e))
        return gprefix + std::to_string(gget->var.index());

    return "0 /* complex init not handled */";
}

template<typename InitT>
auto &GetInitExprList(const InitT &init) {
    if constexpr (std::is_same_v<std::decay_t<InitT>, wabt::ExprList>)
        return init;
    else
        return init;
}

using ModuleImports =
std::unordered_map<std::string, std::vector<ImportEntry> >;
static ModuleImports g_module_imports;

static void RegisterImport(const std::string &module,
                           const std::string &field,
                           const std::string &jsName) {
    g_module_imports[module].push_back({field, jsName});
}

static std::string SafeLabelLookup(const std::vector<std::pair<std::string, LabelType> > &ls,
                                   size_t depth) {
    if (depth >= ls.size()) {
        return "";
    }
    return ls[ls.size() - 1 - depth].first;
}

static void cleanDeadJS(std::string &src) {
    std::vector<std::string> keep;
    std::istringstream is(src);
    std::string line;

    std::stack<bool> scope_is_reachable;
    scope_is_reachable.push(true);

    while (std::getline(is, line)) {
        std::string trimmed = Utils::trim_copy(line);

        size_t closing_braces = std::count(line.begin(), line.end(), '}');
        for (size_t i = 0; i < closing_braces; ++i) {
            if (scope_is_reachable.size() > 1) {
                scope_is_reachable.pop();
            }
        }

        if (scope_is_reachable.top()) {
            keep.push_back(line);
        }

        size_t opening_braces = std::count(line.begin(), line.end(), '{');
        for (size_t i = 0; i < opening_braces; ++i) {
            scope_is_reachable.push(scope_is_reachable.top());
        }

        if (scope_is_reachable.top() && (trimmed.rfind("return", 0) == 0 || trimmed.rfind("break", 0) == 0)) {
            scope_is_reachable.top() = false;
        }
    }
    src = Utils::join(keep, "\n");
}

const std::unordered_set<std::string> JS_RESERVED_WORDS = {
    "await", "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "enum", "export", "extends", "false",
    "finally", "for", "function", "if", "implements", "import", "in",
    "instanceof", "interface", "let", "new", "null", "package", "private",
    "protected", "public", "return", "static", "super", "switch", "this",
    "throw", "true", "try", "typeof", "var", "void", "while", "with", "yield"
};

static void parseGeminiAnswer(const std::string &txt,
                              std::unordered_map<uint32_t, std::string> &dst) {
    std::istringstream iss(txt);
    std::string line;
    while (std::getline(iss, line)) {
        Utils::trim(line);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        uint32_t id = std::strtoul(line.substr(0, eq).c_str(), nullptr, 10);
        std::string name = Utils::trim_copy(line.substr(eq + 1));

        if (name.empty() || !std::isalpha((unsigned char) name[0]))
            continue;

        for (char &c: name)
            if (!std::isalnum((unsigned char) c) && c != '_' && c != '$') c = '_';

        if (name.size() > 40) name.resize(40);
        if (JS_RESERVED_WORDS.count(name)) {
            name += "_";
        }
        dst[id] = std::move(name);
    }
}

inline void ReplaceId(std::string &s,
                      const std::string &oldId,
                      const std::string &newId) {
    std::regex pat("\\b" + oldId + "\\b");
    s = std::regex_replace(s, pat, newId);
}

void CleanEmptyBraces(std::vector<std::string> &lines) {
    static const std::regex openRe(R"(\s*\{\s*)");
    static const std::regex closeRe(R"(\s*\}\s*)");

    for (size_t i = 1; i + 1 < lines.size(); ) {
        if (std::regex_match(lines[i - 1], openRe) &&
            std::regex_match(lines[i], closeRe)) {

            std::string next = Utils::trim_copy(lines[i + 1]);
            if (i + 1 == lines.size() || next.rfind("//", 0) == 0) {
                ++i;
                continue;
            }

            lines.erase(lines.begin() + static_cast<long>(i - 1),
                        lines.begin() + static_cast<long>(i + 1));
            if (i) --i;
        } else {
            ++i;
        }
    }
}

std::string Parser::PrettyFuncName(uint32_t index) {
    auto it = pretty_name.find(index);
    if (it != pretty_name.end()) {
        return it->second;
    }
    if (index < module->num_func_imports) {
        wabt::Import* imp = module->imports[index];
        return "imports['" + imp->module_name + "']['" + imp->field_name + "']";
    }
    return "func" + std::to_string(index);
}


std::string Parser::Parse(const char *path, bool AI, ModuleFormat format) {
    wabt::Errors errors;
    if (LoadModule(path, errors) != wabt::Result::Ok) {
        std::cerr << "could not load module: " << path << std::endl;
        std::cerr << wabt::FormatErrorsToString(errors, wabt::Location::Type::Binary);
        return "";
    }

    auto props = ParseModule(module.get());
    std::cout << "Parsing done (ParseModule)" << std::endl;
    IdentifyInlinableFunctions();
    std::ostringstream prologue;
    for (uint32_t i = 0; i < module->imports.size(); ++i) {
        ProcessImport(module->imports[i],
                      i,
                      prologue);
    }

    std::ostringstream out;
    EmitImportsObject(out);
    out << "\n(function(){\n";

    out << R"JS(
//---------------HELPERS----------------
  function i64(b) {
    return BigInt(b);
  }
  function f32(b) {
    var dv = new DataView(new ArrayBuffer(4));
    dv.setUint32(0, b, true);
    return dv.getFloat32(0, true);
  }
  function f64(b) {
    var dv = new DataView(new ArrayBuffer(8));
    dv.setBigUint64(0, BigInt(b), true);
    return dv.getFloat64(0, true);
  }
  function rotl32(x, y) {
    return (x << y) | (x >>> (32 - y));
  }
  function rotr32(x, y) {
    return (x >>> y) | (x << (32 - y));
  }
  function rotl64(x, y) {
    return (x << y) | (x >> (64n - y));
  }
  function rotr64(x, y) {
    return (x >> y) | (x << (64n - y));
  }
  function copysign(x, y) {
    return Math.sign(y) < 0 ? -Math.abs(x) : Math.abs(x);
  }
  function UNHANDLED_OP(op_name, lhs, rhs) {
    console.error("Called unhandled WASM op:", op_name, "with", lhs, "and", rhs);
    return 0;
  }
//---------------HELPERS----------------
)JS";
    out << "\n\n\n";
    if (!props.memories.empty()) {
        out << this->ProcessMemory(props.memories[0]);
        out << "  const memory = memory0Buffer;\n";
    }

    if (!props.tables.empty()) {
        out << this->ProcessTable(props.tables[0]);
        out << "  const table = table0;\n";
    }

    out << "\n";

    out << "  const passiveSegments = [\n";
    for (auto *seg: props.dataSegments) {
        if (seg->kind == wabt::SegmentKind::Passive) {
            std::tuple t = this->ProcessSegment(seg);
            std::string data_string = std::get<1>(t);

            out << "    [ ";
            for (size_t i = 0; i < seg->data.size(); ++i) {
                out << unsigned(seg->data[i]) << (i + 1 < seg->data.size() ? ", " : " ");
            }
            out << " ],\n";
        }
    }
    out << "  ];\n\n";


    for (size_t i = 0; i < props.globals.size(); ++i) {
        auto &g = module->globals[i];
        bool is_mutable = mutated_globals.count(i) || g->mutable_;
        out << (is_mutable ? "let " : "const ")
                << "global" << i << " = "
                << InitExprListToJS(GetInitExprList(g->init_expr))
                << ";\n";
    }
    std::cout << "--- Pass 1: Analyzing dependencies and transpiling functions ---" << std::endl;
    constexpr size_t BATCH_VAR = 60;
    constexpr size_t MAX_VAR_LEN = 30;
    auto renameVariablesWithAI = [&](uint32_t idx,
                                     std::string &func_src,
                                     std::unordered_map<std::string, std::string> &outMap) {
        auto locals = ExtractLocals(func_src);
        size_t sent = 0;
        for (size_t start = 0; start < locals.size(); start += BATCH_VAR) {
            std::ostringstream prompt;
            prompt <<
                    "You are a JavaScript reverse engineer.\n"
                    "For each listed variable give a concise camelCase name "
                    "(≤ " << MAX_VAR_LEN << " characters) that describes its role. "
                    "Return **only** `old=new` per line.\n\n";
            prompt << "/* function func" << idx << " */\n"
                    << func_src << "\n\nVariables:\n";
            for (size_t i = start; i < locals.size() && i < start + BATCH_VAR; ++i) {
                prompt << locals[i] << '\n';
                ++sent;
            }
            if (!sent) return;
            std::string ans = Utils::ask_gemini(my_api_key, prompt.str());
            auto parsed = Utils::parseRenamingAnswer(ans);
            outMap.insert(parsed.begin(), parsed.end());
        }
    };

    for (uint32_t idx = 0; idx < props.funcs.size(); ++idx) {
        wabt::Func *func = props.funcs[idx];
        AnalyzeFunctionDependencies(func, idx);

        std::string func_source = this->ProcessFunction(func, idx);
        cleanDeadJS(func_source);

        std::unordered_map<std::string, std::string> varMap;
        if(AI) {
            renameVariablesWithAI(idx, func_source, varMap);
        }

        for (const auto &[oldId,newId]: varMap)
            ReplaceId(func_source, oldId, newId);

        if (!varMap.empty()) {
            func_source = "/* renamed vars: "
                          + Utils::join_kv(varMap) + " */\n"
                          + func_source;
        }
        function_sources[idx] = func_source;
    }
    constexpr size_t BATCH_SIZE = 60;
    constexpr size_t MIN_LEN = 250;


    if (AI) {
        std::cout << "\n--- Pass 2: Generating AI summaries for key functions ---" << std::endl;
        for (size_t start = 0; start < props.funcs.size(); start += BATCH_SIZE) {
            std::ostringstream prompt;
            prompt
                    << "You're a JS reverse-engineer. "
                    "For every function below, propose a concise camelCase name "
                    "(≤ 40 chars) that describes its purpose. "
                    "Reply *only* with lines of the form `index=name`.\n\n";

            size_t added = 0;
            for (size_t idx = start;
                 idx < props.funcs.size() && added < BATCH_SIZE;
                 ++idx) {
                const std::string &body = function_sources[idx];
                std::string cleaned = body;
                Utils::replace_all_regex(
                    cleaned,
                    R"(\s*let\s+unused\w*(?:\s*,\s*unused\w*)*\s*;\s*)",
                    "");


                if (cleaned.size() < MIN_LEN)
                    continue;

                prompt << "/* func" << idx << " */\n"
                        << cleaned
                        << "\n\n";
                ++added;
            }
            if (!added) continue;


            std::string answer =
                    Utils::ask_gemini_retry(my_api_key, prompt.str());

            parseGeminiAnswer(answer, pretty_name);
        }
    }

    for (wabt::ElemSegment *elem_seg: module->elem_segments) {
        if (elem_seg->kind != wabt::SegmentKind::Active) {
            continue;
        }

        uint32_t offset = 0;
        if (!elem_seg->offset.empty()) {
            if (auto *const_expr = wabt::dyn_cast<wabt::ConstExpr>(&elem_seg->offset.front())) {
                offset = const_expr->const_.u32();
            }
        }

        uint32_t target_table_index = elem_seg->table_var.index();

        for (size_t i = 0; i < elem_seg->elem_exprs.size(); ++i) {
            const wabt::ExprList &expr_list = elem_seg->elem_exprs[i];

            if (expr_list.empty()) {
                continue;
            }

            const wabt::Expr &expr = expr_list.front();

            if (auto *ref_func_expr = wabt::dyn_cast<wabt::RefFuncExpr>(&expr)) {
                uint32_t func_index = ref_func_expr->var.index();
                uint32_t table_pos = offset + i;
                out << "  table" << target_table_index
                        << "[" << table_pos << "] = "
                        << PrettyFuncName(func_index) << ";\n";
            } else if (wabt::dyn_cast<wabt::RefNullExpr>(&expr)) {
            }
        }
    }
    out << "\n";

    if(AI) {
        for (uint32_t idx = 0; idx < props.funcs.size(); ++idx) {
            const std::string &func_src = function_sources[idx];

            if (func_src.length() < 350) {
                continue;
            }

            std::cout << "--> Analyzing function index: " << idx << std::endl;

            std::string prompt =
                    "you are an expert in javascript reverse engineering. your mission is to describe in one sentence compact and technical the goal of the next function.\n\n";

            if (call_graph.count(idx) && !call_graph[idx].empty()) {
                prompt += "CONTEXT : this function call the next functions :\n";
                for (uint32_t called_idx: call_graph[idx]) {
                    if (ai_summaries.count(called_idx)) {
                        prompt += " - " + PrettyFuncName(called_idx) + " (Summary : " + ai_summaries[called_idx] + ")\n";
                    }
                }
                prompt += "\n";
            }

            prompt += "FUNCTION TO STUDY :\n```javascript\n" + func_src + "\n```\n\nYOUR DESCRIPTION :";

            ai_summaries[idx] = Utils::ask_gemini(my_api_key, prompt);
        }
    }

    std::cout << "\n--- Pass 3: Assembling final output file ---" << std::endl;
    for (uint32_t idx = 0; idx < props.funcs.size(); ++idx) {
        std::string declared = function_sources[idx];

        for (const auto &[id, newName]: pretty_name) {
             if(id != idx) {
                Utils::replace_all(declared, "func" + std::to_string(id) + "(", newName + "(");
             }
        }

        auto it = pretty_name.find(idx);
        if (it != pretty_name.end()) {
            const std::string &newName = it->second;
            const std::string oldDecl = "function func" + std::to_string(idx);
            Utils::replace_all(declared, oldDecl, "function " + newName);
            declared = "// func" + std::to_string(idx) + " is " + newName + "\n" + declared;
        }

        std::string final_code = InjectLetDeclarations(declared);
        out << final_code << "\n";

        if (AI && ai_summaries.count(idx)) {
            out << "/*\n * AI Summary: " << ai_summaries[idx] << "\n */\n";
        }
        out << "\n";
    }

    if (format == ModuleFormat::IIFE) {
        out << "  return {\n";
        for (auto *exp: props.exports) {
            out << "    " << exp->name << ": " << GetExportValue(exp) << ",\n";
        }
        out << "  };\n";
        out << "})();";
    } else if (format == ModuleFormat::ESM) {
        out << "\n// --- Exports ---\n";
        for (auto *exp: props.exports) {
             if (exp->kind == wabt::ExternalKind::Func) {
                 out << "export { " << GetExportValue(exp) << " };\n";
             } else {
                out << "export const " << exp->name << " = " << GetExportValue(exp) << ";\n";
             }
        }
    }
    return out.str();
}

void Parser::CollectLocals(const wabt::Func *func,
                           const std::unordered_set<std::string> &inlNames,
                           std::vector<std::string> &outNames) {
    for (size_t i = 0; i < func->GetNumLocals(); ++i)
        outNames.push_back("local" + std::to_string(i));

    outNames.insert(outNames.end(), inlNames.begin(), inlNames.end());

    std::sort(outNames.begin(), outNames.end());
    outNames.erase(std::unique(outNames.begin(), outNames.end()),
                   outNames.end());
}

void Parser::EmitImportsObject(std::ostringstream &out) {
    if (g_module_imports.empty()) {
        out << "const imports = {};\n";
        return;
    }
    out << "const imports = {\n";

    size_t modIdx = 0, modTotal = g_module_imports.size();
    for (const auto &[moduleName, entries]: g_module_imports) {
        out << "  \"" << moduleName << "\": {\n";
        for (size_t i = 0; i < entries.size(); ++i) {
            out << "    \"" << entries[i].field << "\": "
                    << "null /* TODO: Provide import " << entries[i].js_name << " */"
                    << (i + 1 < entries.size() ? "," : "") << "\n";
        }
        out << "  }" << (++modIdx < modTotal ? ",\n" : "\n");
    }
    out << "};\n";
}


std::vector<std::string> Parser::ExtractLocals(const std::string &func_src) {
    std::vector<std::string> names;
    std::regex decl(R"(\blet\s+([^;=]+))");
    std::smatch m;

    auto it = func_src.cbegin();
    while (std::regex_search(it, func_src.cend(), m, decl)) {
        std::string list = m[1];
        std::stringstream ss(list);
        std::string id;
        while (std::getline(ss, id, ',')) {
            Utils::trim(id);
            if (!id.empty()) names.push_back(id);
        }
        it = m.suffix().first;
    }
    return names;
}

wabt::Result Parser::LoadModule(const char* path, wabt::Errors& errors) {
    std::vector<uint8_t> data;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "Cannot open file: " << path << "\n";
        return wabt::Result::Error;
    }
    data.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());

    wabt::Module *raw_module = new wabt::Module();
    wabt::Result result = wabt::ReadBinaryIr(path,
                                             data.data(), data.size(),
                                             options,
                                             &errors,
                                             raw_module);

    if (!wabt::Succeeded(result)) {
        delete raw_module;
        return result;
    }

    module.reset(raw_module);

    uint32_t func_idx_counter = module->num_func_imports;
    for(const auto& func : module->funcs) {
        if(!func->name.empty() && func->name[0] == '$') {
            pretty_name[func_idx_counter] = func->name.substr(1);
        }
        func_idx_counter++;
    }

    for(const auto& exp : module->exports) {
        if(exp->kind == wabt::ExternalKind::Func) {
             if(pretty_name.find(exp->var.index()) == pretty_name.end()) {
                 pretty_name[exp->var.index()] = exp->name;
             }
        }
    }

    return result;
}


UsefulProperties Parser::ParseModule(wabt::Module *module) {
    UsefulProperties properties;

    for (auto func: module->funcs) {
        properties.funcs.push_back(func);
    }

    for (wabt::Import *imp: module->imports) {
        if (imp != nullptr) {
            properties.imports.push_back(imp);
        }
    }

    for (wabt::Export *exp: module->exports) {
        if (exp != nullptr) {
            properties.exports.push_back(exp);
        }
    }

    for (wabt::Memory *mem: module->memories) {
        if (mem != nullptr) {
            auto it = std::find(module->memories.begin(), module->memories.end(), mem);
            if (it != module->memories.end()) {
                size_t idx = std::distance(module->memories.begin(), it);
                properties.memories.push_back(MemoryI(mem, idx));
            }
        }
    }

    for (wabt::Table *tab: module->tables) {
        if (tab != nullptr) {
            auto it = std::find(module->tables.begin(), module->tables.end(), tab);
            if (it != module->tables.end()) {
                size_t idx = std::distance(module->tables.begin(), it);
                properties.tables.push_back(TableI(tab, idx));
            }
        }
    }

    for (wabt::DataSegment *seg: module->data_segments) {
        if (seg != nullptr) {
            properties.dataSegments.push_back(seg);
        }
    }

    for (wabt::Global *global: module->globals) {
        if (global != nullptr) {
            properties.globals.push_back(global);
        }
    }

    return properties;
}


const wabt::FuncSignature *Parser::GetFuncSignature(const wabt::Var &func_var) {
    if (!func_var.is_index()) {
        return nullptr;
    }
    wabt::Index func_index = func_var.index();

    if (func_index < module->num_func_imports) {
        wabt::Import *import = module->imports[func_index];
        if (auto *func_import = wabt::dyn_cast<wabt::FuncImport>(import)) {
            if (func_import->func.decl.has_func_type) {
                wabt::FuncType *func_type = module->GetFuncType(func_import->func.decl.type_var);
                return func_type ? &func_type->sig : nullptr;
            } else {
                return &func_import->func.decl.sig;
            }
        }
    } else {
        wabt::Index internal_index = func_index - module->num_func_imports;
        if (internal_index < module->funcs.size()) {
            wabt::Func *func = module->funcs[internal_index];
            if (func->decl.has_func_type) {
                wabt::FuncType *func_type = module->GetFuncType(func->decl.type_var);
                return func_type ? &func_type->sig : nullptr;
            } else {
                return &func->decl.sig;
            }
        }
    }

    return nullptr;
}

void Parser::FindCallsRecursive(const wabt::ExprList &exprs,
                                std::vector<uint32_t> &called_indices) {
    for (const wabt::Expr &expr_ref: exprs) {
        const wabt::Expr *e = &expr_ref;

        if (auto *call_expr = wabt::dyn_cast<wabt::CallExpr>(e))
            called_indices.push_back(call_expr->var.index());

        if (auto *blk = wabt::dyn_cast<wabt::BlockExpr>(e))
            FindCallsRecursive(blk->block.exprs, called_indices);

        if (auto *loop = wabt::dyn_cast<wabt::LoopExpr>(e))
            FindCallsRecursive(loop->block.exprs, called_indices);

        if (auto *iff = wabt::dyn_cast<wabt::IfExpr>(e)) {
            FindCallsRecursive(iff->true_.exprs, called_indices);
            if (!iff->false_.empty())
                FindCallsRecursive(iff->false_, called_indices);
        }
    }
}

void Parser::ProcessExprList(wabt::Func *func,
                             uint32_t func_idx, const wabt::ExprList &exprs,
                             std::vector<std::pair<std::string, LabelType> > &labelStack,
                             int &nextLabelId,
                             std::stack<Operand> &js_stack,
                             std::vector<std::string> &js_statements,
                             std::stack<bool> &reachability_stack,
                             const std::unordered_map<int, std::string> &local_names) {
    for (const auto &expr: exprs) {
        ProcessExpr(func, func_idx, expr, labelStack, nextLabelId, js_stack, js_statements, reachability_stack,
                    local_names);
    }
}

void PrintOperandStack(const std::stack<Operand> &s) {
    std::stack<Operand> temp = s;
    std::vector<std::string> items;
    while (!temp.empty()) {
        const Operand &op = temp.top();
        if (op.type != wabt::Type::Void && op.type != wabt::Type::Any) {
            items.push_back(op.js_repr + " (" + std::string(op.type.GetName()) + ")");
        } else {
            items.push_back(op.js_repr);
        }
        temp.pop();
    }
    std::cout << "[";
    for (int i = items.size() - 1; i >= 0; --i) {
        std::cout << items[i] << (i > 0 ? ", " : "");
    }
    std::cout << "]";
}

inline void assert_stack(std::stack<Operand> &st,
                         std::size_t need,
                         const char *where) {
    if (st.size() < need) {
        return;
    }
}


inline Operand Parser::pop_or_throw(std::stack<Operand> &st, std::stack<bool>& reachability_stack, const char *where) {
    if (st.empty()) {
        if (!reachability_stack.empty() && !reachability_stack.top()) {
            Operand op;
            op.is_constant = false;
            op.js_repr = "0/*unreachable*/";
            op.type = wabt::Type::I32;
            return op;
        }
        throw std::runtime_error("FATAL POP: Attempted to pop from an empty stack in " + std::string(where));
    }
    Operand v = st.top();
    st.pop();
    return v;
}

inline Operand Parser::peek_or_throw(std::stack<Operand> &st, std::stack<bool>& reachability_stack, const char *where) {
    if (st.empty()) {
        if (!reachability_stack.empty() && !reachability_stack.top()) {
            Operand op;
            op.is_constant = false;
            op.js_repr = "0/*unreachable*/";
            op.type = wabt::Type::I32;
            return op;
        }
        throw std::runtime_error("FATAL PEEK: Attempted to peek from an empty stack in " + std::string(where));
    }
    return st.top();
}


std::string Parser::BuildStoreExpression(std::stack<Operand> &js_stack, std::stack<bool>& reachability_stack,
                                         const std::string &expr,
                                         char div) {
    std::string value = pop_or_throw(js_stack, reachability_stack, "Store(value)").js_repr;
    std::string address = pop_or_throw(js_stack, reachability_stack, "Store(addr)").js_repr;

    return "view" + expr + "[(" + address + ")/" + div + "] = " + value;
}


std::vector<Operand> Parser::UnstackArgs(std::stack<Operand> &js_stack, std::stack<bool>& reachability_stack,
                                         size_t paramCount) {
    std::vector<Operand> args(paramCount);

    for (size_t i = 0; i < paramCount; ++i) {
        args[paramCount - 1 - i] = pop_or_throw(js_stack, reachability_stack, "UnstackArgs");
    }
    return args;
}


std::string Parser::FillBrackets(const std::string &scope) {
    std::string result;
    result.reserve(scope.size() + 16);

    int opened = 0;
    for (char c: scope) {
        if (c == '{') {
            ++opened;
            result += c;
        } else if (c == '}') {
            if (opened == 0) {
                result += '{';
            } else {
                --opened;
            }
            result += c;
        } else {
            result += c;
        }
    }

    if (opened > 0) {
        result.append(opened, '}');
    }
    if (opened < 0) {
        result.insert(0, std::string(-opened, '{'));
        opened = 0;
    }
    return result;
}


void Parser::removeSemicolons(std::vector<std::string> &js_statements) {
    for (std::string &line: js_statements) {
        std::string result;
        bool inString = false;
        char stringChar = 0;

        for (size_t i = 0; i < line.length(); ++i) {
            char c = line[i];

            if ((c == '"' || c == '\'') && (i == 0 || line[i - 1] != '\\')) {
                if (!inString) {
                    inString = true;
                    stringChar = c;
                } else if (c == stringChar) {
                    inString = false;
                }
            }

            if (!inString) {
                if (c == ';') {
                    if (!result.empty() && result.back() == ';') continue;
                }
            }

            result += c;
        }

        while (result.length() >= 2 && result[result.length() - 1] == '\n' && result[result.length() - 2] == ';') {
            size_t semis = 0;
            for (int i = result.length() - 2; i >= 0 && result[i] == ';'; --i) semis++;
            if (semis > 1)
                result.erase(result.length() - 1 - (semis - 1), semis - 1);
            break;
        }

        if (result.empty() || result.back() != '\n') result += '\n';

        line = result;
    }
}

bool Parser::DoesBlockNeedLabel(const wabt::Block &block) {
    if (!block.label.empty()) {
        return true;
    }
    return false;
}

bool Parser::IsExportedFunc(uint32_t func_idx) {
    for (auto *exp: module->exports) {
        if (exp->kind == wabt::ExternalKind::Func && exp->var.index() == func_idx) {
            return true;
        }
    }
    return false;
}

void Parser::IdentifyInlinableFunctions() {
    std::cout << "--- Pass 0: Identifying inlinable functions ---" << std::endl;
    for (uint32_t idx = 0; idx < module->funcs.size(); ++idx) {
        wabt::Func *func = module->funcs[idx];
        uint32_t absolute_func_index = idx + module->num_func_imports;
        if (func->exprs.size() > 25) {
            continue;
        }

        if (IsExportedFunc(absolute_func_index)) {
            continue;
        }

        bool has_complex_control_flow = false;
        for (const auto &expr: func->exprs) {
            if (wabt::isa<wabt::LoopExpr>(&expr) || wabt::isa<wabt::BrTableExpr>(&expr)) {
                has_complex_control_flow = true;
                break;
            }
        }

        if (!has_complex_control_flow) {
            std::cout << "[Inliner] Function " << absolute_func_index
                    << " (internal idx " << idx << ") is a candidate for inlining." << std::endl;
            this->inlinable_function_indices.insert(absolute_func_index);
        }
    }
}

void Parser::AnalyzeFunctionDependencies(wabt::Func *func, uint32_t func_idx) {
    std::vector<uint32_t> called_indices;
    std::stack<const wabt::ExprList *> work_stack;

    work_stack.push(&func->exprs);

    while (!work_stack.empty()) {
        const wabt::ExprList *current_exprs = work_stack.top();
        work_stack.pop();

        for (const wabt::Expr &expr_ref: *current_exprs) {
            const wabt::Expr *e = &expr_ref;

            if (auto *call_expr = wabt::dyn_cast<wabt::CallExpr>(e)) {
                called_indices.push_back(call_expr->var.index());
            } else if (auto *blk = wabt::dyn_cast<wabt::BlockExpr>(e)) {
                work_stack.push(&blk->block.exprs);
            } else if (auto *loop = wabt::dyn_cast<wabt::LoopExpr>(e)) {
                work_stack.push(&loop->block.exprs);
            } else if (auto *iff = wabt::dyn_cast<wabt::IfExpr>(e)) {
                work_stack.push(&iff->true_.exprs);
                if (!iff->false_.empty()) {
                    work_stack.push(&iff->false_);
                }
            }
        }
    }

    this->call_graph[func_idx] = called_indices;
}

void Parser::ProcessFunctionBody(wabt::Func &func, uint32_t realIndex, const wabt::ExprList &initial_exprs,
                                 size_t start, size_t end,
                                 std::vector<std::pair<std::string, LabelType> > &labelStack,
                                 int &nextLabelId,
                                 std::stack<Operand> &js_stack,
                                 std::vector<std::string> &js_statements,
                                 const std::unordered_map<int, std::string> &local_names) {
    std::stack<Task> taskStack;
    std::stack<bool> reachability_stack;
    reachability_stack.push(true);

    auto it = initial_exprs.begin();
    std::advance(it, start);
    std::vector<const wabt::Expr *> temp_exprs;
    for (size_t i = 0; i < (end - start); ++i, ++it) {
        temp_exprs.push_back(&(*it));
    }
    for (auto rit = temp_exprs.rbegin(); rit != temp_exprs.rend(); ++rit) {
        taskStack.push({TaskType::PROCESS_EXPRESSION, *rit, false});
    }
    auto pushStmt = [&](const std::string &s) {
        if (!reachability_stack.empty() && reachability_stack.top())
            js_statements.push_back(s);
    };

    auto pushExit = [&](bool needsLabel, bool emitBrace = true)
    {
        taskStack.emplace(TaskType::EXIT_BLOCK, nullptr, needsLabel, emitBrace);
    };
    while (!taskStack.empty()) {
        Task currentTask = taskStack.top();
        taskStack.pop();

        switch (currentTask.type) {
            case TaskType::EXIT_BLOCK: {
                if (currentTask.emitBrace) {
                    js_statements.push_back("}");
                }
                if (currentTask.needsLabel && !labelStack.empty()) {
                    labelStack.pop_back();
                }
                if (!reachability_stack.empty()) {
                    reachability_stack.pop();
                }
                break;
            }

            case TaskType::START_ELSE: {
                if (!reachability_stack.empty()) {
                    reachability_stack.pop();
                }
                pushStmt("} else {");
                reachability_stack.push(reachability_stack.top());

                auto *ifExpr = wabt::dyn_cast<wabt::IfExpr>(currentTask.expr);
                taskStack.push({TaskType::EXIT_BLOCK, nullptr, false});
                for (auto it_else = ifExpr->false_.rbegin(); it_else != ifExpr->false_.rend(); ++it_else) {
                    taskStack.push({TaskType::PROCESS_EXPRESSION, &(*it_else), false});
                }
                break;
            }

            case TaskType::END_IF_ELSE: {
                if (!reachability_stack.empty()) {
                    reachability_stack.pop();
                }
                break;
            }

            case TaskType::PROCESS_EXPRESSION: {
                if (reachability_stack.empty()) {
                    throw std::runtime_error("FATAL: reachability_stack is empty.");
                }
                if (!reachability_stack.top()) {
                    continue;
                }

                const wabt::Expr &expr = *currentTask.expr;

                if (auto* loopExpr = wabt::dyn_cast<wabt::LoopExpr>(&expr)) {
                    reachability_stack.push(reachability_stack.top());

                    std::string label_name = "loop" + std::to_string(nextLabelId++);
                    labelStack.push_back({label_name, LabelType::Loop});
                    pushStmt(label_name + ": while (true) {");

                    pushExit(true);

                    for (auto it_loop = loopExpr->block.exprs.rbegin(); it_loop != loopExpr->block.exprs.rend(); ++it_loop) {
                        taskStack.emplace(TaskType::PROCESS_EXPRESSION, &(*it_loop), false);
                    }
                    continue;

                } else if (auto *blockExpr = wabt::dyn_cast<wabt::BlockExpr>(&expr)) {
                    reachability_stack.push(reachability_stack.top());

                    bool canFlatten = false;
                    if (blockExpr->block.exprs.empty()) {
                        canFlatten = true;
                    } else if (blockExpr->block.exprs.size() == 1) {
                        const auto& innerExpr = blockExpr->block.exprs.front();
                        if (wabt::isa<wabt::BlockExpr>(&innerExpr) ||
                            wabt::isa<wabt::LoopExpr>(&innerExpr) ||
                            wabt::isa<wabt::IfExpr>(&innerExpr))
                        {
                            canFlatten = true;
                        }
                    }

                    bool hasLabel = !blockExpr->block.label.empty();

                    if (hasLabel) {
                        std::string label_name = blockExpr->block.label;
                        if (!label_name.empty() && label_name[0] == '$') {
                           label_name = label_name.substr(1);
                        }
                        labelStack.push_back({label_name, LabelType::Block});
                        pushStmt(label_name + ":");
                    }

                    if (!canFlatten) {
                        pushStmt("{");
                        pushExit(hasLabel, true);
                    } else {
                        pushExit(hasLabel, false);
                    }

                    for (auto it_block = blockExpr->block.exprs.rbegin(); it_block != blockExpr->block.exprs.rend(); ++it_block) {
                        taskStack.emplace(TaskType::PROCESS_EXPRESSION, &(*it_block), false);
                    }

                    continue;

                } else if (auto *ifExpr = wabt::dyn_cast<wabt::IfExpr>(&expr)) {
                    reachability_stack.push(reachability_stack.top());
                    Operand condOp = pop_or_throw(js_stack, reachability_stack, "if_cond");
                    std::string cond = condOp.js_repr;

                    pushStmt("if (" + cond + ") {");

                    taskStack.push({TaskType::END_IF_ELSE, nullptr, false});

                    if (!ifExpr->false_.empty()) {
                        taskStack.push({TaskType::START_ELSE, &expr, false});
                    } else {
                        taskStack.push({TaskType::EXIT_BLOCK, nullptr, false});
                    }

                    reachability_stack.push(reachability_stack.top());
                    for (auto it_then = ifExpr->true_.exprs.rbegin(); it_then != ifExpr->true_.exprs.rend(); ++it_then) {
                        taskStack.push({TaskType::PROCESS_EXPRESSION, &(*it_then), false});
                    }
                    continue;
                } else if (auto *br = wabt::dyn_cast<wabt::BrExpr>(&expr)) {
                    wabt::Index depth = br->var.index();
                    std::string target_label;
                    std::string jump_keyword = "break";

                    if (depth < labelStack.size()) {
                        auto const& [label, type] = labelStack[labelStack.size() - 1 - depth];
                        target_label = label;
                        if (type == LabelType::Loop) {
                            jump_keyword = "continue";
                        }
                    }

                    pushStmt((target_label.empty() ? "return;" : jump_keyword + " " + target_label + ";"));
                    reachability_stack.top() = false;
                } else if (auto *brIf = wabt::dyn_cast<wabt::BrIfExpr>(&expr)) {
                    std::string cond = pop_or_throw(js_stack, reachability_stack, "br_if").js_repr;
                    wabt::Index depth = brIf->var.index();
                    std::string target_label;
                    std::string jump_keyword = "break";

                    if (depth < labelStack.size()) {
                        auto const& [label, type] = labelStack[labelStack.size() - 1 - depth];
                        target_label = label;
                        if (type == LabelType::Loop) {
                            jump_keyword = "continue";
                        }
                    }

                    pushStmt("if (" + cond + ") " + (target_label.empty() ? "return;" : jump_keyword + " " + target_label + ";"));
                } else if (auto *brTab = wabt::dyn_cast<wabt::BrTableExpr>(currentTask.expr)) {
                    std::string idx = pop_or_throw(js_stack, reachability_stack, "br_table_idx").js_repr;
                    const auto &targets = brTab->targets;

                    pushStmt("switch (" + idx + ") {");
                    for (size_t i = 0; i < targets.size(); ++i) {
                        wabt::Index depth = targets[i].index();
                        std::string target_label;
                        std::string jump_keyword = "break";

                        if (depth < labelStack.size()) {
                            auto const& [label, type] = labelStack[labelStack.size() - 1 - depth];
                            target_label = label;
                            if (type == LabelType::Loop) {
                                jump_keyword = "continue";
                            }
                        }

                        pushStmt(
                            "  case " + std::to_string(i) + ": " + (target_label.empty() ? "return;" : jump_keyword + " " + target_label + ";"));
                    }

                    wabt::Index depth = brTab->default_target.index();
                    std::string target_label;
                    std::string jump_keyword = "break";
                    if (depth < labelStack.size()) {
                        auto const& [label, type] = labelStack[labelStack.size() - 1 - depth];
                        target_label = label;
                        if (type == LabelType::Loop) {
                            jump_keyword = "continue";
                        }
                    }
                    pushStmt("  default: " + (target_label.empty() ? "return;" : jump_keyword + " " + target_label + ";"));
                    pushStmt("}");
                    reachability_stack.top() = false;
                } else {
                    ProcessExpr(&func, realIndex, expr, labelStack, nextLabelId, js_stack, js_statements,
                                reachability_stack, local_names);
                }

                break;
            }
            default:
                break;
        }
    }
}

std::string Parser::GetExportValue(wabt::Export *exp) {
    switch (exp->kind) {
        case wabt::ExternalKind::Func:
            return PrettyFuncName(exp->var.index());
        case wabt::ExternalKind::Memory:
            return "memory";
        case wabt::ExternalKind::Table:
            return "table" + std::to_string(exp->var.index());
        case wabt::ExternalKind::Global:
            return "global" + std::to_string(exp->var.index());
        default:
            return "null";
    }
}

void TraceStack(uint32_t func_idx, const wabt::Location &loc, const char *op_name, const std::stack<Operand> &s,
                const char *phase) {
    std::cout << "\n[Trace F" << func_idx << "][L" << loc.line << "] Op: " << op_name
            << "\n  Stack " << phase << ": ";
    PrintOperandStack(s);
    std::cout << std::endl;
}

std::string Parser::ProcessExpr(wabt::Func *func,
                                uint32_t func_idx, const wabt::Expr &expr,
                                std::vector<std::pair<std::string, LabelType> > &labelStack,
                                int &nextLabelId,
                                std::stack<Operand> &js_stack, std::vector<std::string> &js_statements,
                                std::stack<bool> &reachability_stack,
                                const std::unordered_map<int, std::string> &local_names) {
    bool trace_this_func = functions_to_trace.count(func_idx);

    if (trace_this_func) {
        std::cout << "[OP] " << GetExprTypeName(expr.type()) << " | Stack BEFORE: ";
        PrintOperandStack(js_stack);
        std::cout << std::endl;
    }

    if (reachability_stack.empty() || !reachability_stack.top()) {
        return "";
    }
    auto pushStmt = [&](const std::string &s) {
        if (!reachability_stack.empty() && reachability_stack.top())
            js_statements.push_back(s);
    };
    if (auto *const_expr = wabt::dyn_cast<wabt::ConstExpr>(&expr)) {
        Operand op;
        op.is_constant = true;
        op.value = const_expr->const_;
        op.type = const_expr->const_.type();

        switch (op.type) {
            case wabt::Type::I32: op.js_repr = FormatInt32(op.value.u32()); break;
            case wabt::Type::I64: op.js_repr = FormatInt64(op.value.u64()); break;
            case wabt::Type::F32: op.js_repr = "f32(" + std::to_string(op.value.f32_bits()) + ")"; break;
            case wabt::Type::F64: op.js_repr = "f64(" + std::to_string(op.value.f64_bits()) + ")"; break;
            default: op.js_repr = "/* unknown const */";
        }
        js_stack.push(op);
    } else if (auto *get_expr = wabt::dyn_cast<wabt::LocalGetExpr>(&expr)) {
        std::string var_name = "local" + std::to_string(get_expr->var.index());
        Operand op;
        op.is_constant = false;
        op.js_repr = var_name;
        js_stack.push(op);
    } else if (auto *drop = wabt::dyn_cast<wabt::DropExpr>(&expr)) {
        pop_or_throw(js_stack, reachability_stack, "drop");
        return {};
    } else if (auto *sel = wabt::dyn_cast<wabt::SelectExpr>(&expr)) {
        assert_stack(js_stack, 3, "select");
        std::string cond = pop_or_throw(js_stack, reachability_stack, "select.cond").js_repr;
        std::string v2 = pop_or_throw(js_stack, reachability_stack, "select.v2").js_repr;
        std::string v1 = pop_or_throw(js_stack, reachability_stack, "select.v1").js_repr;
        js_stack.push(static_cast<std::stack<Operand>::value_type>("(" + cond + " ? " + v1 + " : " + v2 + ")"));
        return {};
    } else if (auto *set_expr = wabt::dyn_cast<wabt::LocalSetExpr>(&expr)) {
        std::string var_name = "local" + std::to_string(set_expr->var.index());
        std::string val = pop_or_throw(js_stack, reachability_stack, "local.set").js_repr;
        pushStmt(var_name + " = " + val + ";");
    } else if (auto *get_expr = wabt::dyn_cast<wabt::LocalTeeExpr>(&expr)) {
        std::string var_name = "local" + std::to_string(get_expr->var.index());
        pushStmt(var_name + " = " + peek_or_throw(js_stack, reachability_stack, "local.tee").js_repr + ";");
    } else if (auto *get_expr = wabt::dyn_cast<wabt::GlobalGetExpr>(&expr)) {
        Operand op;
        op.is_constant = false;
        op.js_repr = "global" + std::to_string(get_expr->var.index());
        js_stack.push(op);
    } else if (auto *gset = wabt::dyn_cast<wabt::GlobalSetExpr>(&expr)) {
        mutated_globals.insert(gset->var.index());
        std::string val = pop_or_throw(js_stack, reachability_stack, "global.set").js_repr;
        std::string varName = "global" + std::to_string(gset->var.index());
        pushStmt(varName + " = " + val + ";");
    } else if (auto *get_expr = wabt::dyn_cast<wabt::LoadExpr>(&expr)) {
        wabt::Opcode opcode = get_expr->opcode;
        const auto pushLoad = [&](const char *view, char div, const char *where) {
            std::string addr = pop_or_throw(js_stack, reachability_stack, where).js_repr;
            js_stack.push(
                static_cast<std::stack<Operand>::value_type>(std::string(view) + "_0[(" + addr + ")/" + div + "]"));
        };
        switch (opcode) {
            case wabt::Opcode::I32Load:
            case wabt::Opcode::I32Load8S: case wabt::Opcode::I32Load8U:
            case wabt::Opcode::I32Load16S: case wabt::Opcode::I32Load16U:
                 pushLoad("viewI32", '4', "I32Load"); break;
            case wabt::Opcode::I64Load:
            case wabt::Opcode::I64Load8S: case wabt::Opcode::I64Load8U:
            case wabt::Opcode::I64Load16S: case wabt::Opcode::I64Load16U:
            case wabt::Opcode::I64Load32S: case wabt::Opcode::I64Load32U:
                 pushLoad("viewI64", '8', "I64Load"); break;
            case wabt::Opcode::F32Load: pushLoad("viewF32", '4', "F32Load"); break;
            case wabt::Opcode::F64Load: pushLoad("viewF64", '8', "F64Load"); break;
            default:
                 pushStmt("/* UNHANDLED LOAD OPCODE: " + std::string(opcode.GetName()) + "*/");
                 pop_or_throw(js_stack, reachability_stack, "unhandled_load_addr");
                 js_stack.push(Operand{"0", true, opcode.GetResultType()});
                 break;
        }
    } else if (auto *set_expr = wabt::dyn_cast<wabt::StoreExpr>(&expr)) {
        wabt::Opcode opcode = set_expr->opcode;
        auto pushStore = [&](const char *tag, char div) {
            pushStmt(BuildStoreExpression(js_stack, reachability_stack, std::string(tag) + "_0", div) + ";");
        };
        switch (opcode) {
            case wabt::Opcode::I32Store:
            case wabt::Opcode::I32Store8:
            case wabt::Opcode::I32Store16:
                 pushStore("I32", '4'); break;
            case wabt::Opcode::I64Store:
            case wabt::Opcode::I64Store8:
            case wabt::Opcode::I64Store16:
            case wabt::Opcode::I64Store32:
                 pushStore("I64", '8'); break;
            case wabt::Opcode::F32Store: pushStore("F32", '4'); break;
            case wabt::Opcode::F64Store: pushStore("F64", '8'); break;
            default:
                 pushStmt("/* UNHANDLED STORE OPCODE: " + std::string(opcode.GetName()) + "*/");
                 pop_or_throw(js_stack, reachability_stack, "unhandled_store_val");
                 pop_or_throw(js_stack, reachability_stack, "unhandled_store_addr");
                 break;
        }
    } else if (auto *bin = wabt::dyn_cast<wabt::BinaryExpr>(&expr)) {
        const wabt::Opcode op = bin->opcode;

        assert_stack(js_stack, 2, op.GetName());
        auto rhs = pop_or_throw(js_stack, reachability_stack, op.GetName());
        auto lhs = pop_or_throw(js_stack, reachability_stack, op.GetName());

        if (lhs.is_constant && rhs.is_constant) {
            Operand result_op;
            result_op.is_constant = true;

            auto set_i32 = [&](uint32_t v) { result_op.value.set_u32(v); result_op.type = wabt::Type::I32; result_op.js_repr = std::to_string(static_cast<int32_t>(v)); };
            auto set_i64 = [&](uint64_t v) { result_op.value.set_u64(v); result_op.type = wabt::Type::I64; result_op.js_repr = std::to_string(static_cast<int64_t>(v)) + "n"; };
            auto set_f32 = [&](float v) { result_op.value.set_f32(std::bit_cast<uint32_t>(v)); result_op.type = wabt::Type::F32; result_op.js_repr = std::isnan(v) ? "NaN" : std::to_string(v); };
            auto set_f64 = [&](double v) { result_op.value.set_f64(std::bit_cast<uint64_t>(v)); result_op.type = wabt::Type::F64; result_op.js_repr = std::isnan(v) ? "NaN" : std::to_string(v); };

            switch (op) {
                case wabt::Opcode::I32Add: set_i32(lhs.value.u32() + rhs.value.u32()); break;
                case wabt::Opcode::I32Sub: set_i32(lhs.value.u32() - rhs.value.u32()); break;
                case wabt::Opcode::I32Mul: set_i32(lhs.value.u32() * rhs.value.u32()); break;
                case wabt::Opcode::I32DivS: { int32_t r = rhs.value.u32(); if (!r) { result_op.is_constant = false; break; } set_i32(static_cast<int32_t>(lhs.value.u32()) / r); break; }
                case wabt::Opcode::I32DivU: { uint32_t r = rhs.value.u32(); if (!r) { result_op.is_constant = false; break; } set_i32(lhs.value.u32() / r); break; }
                case wabt::Opcode::I32RemS: { int32_t r = rhs.value.u32(); if (!r) { result_op.is_constant = false; break; } set_i32(static_cast<int32_t>(lhs.value.u32()) % r); break; }
                case wabt::Opcode::I32RemU: { uint32_t r = rhs.value.u32(); if (!r) { result_op.is_constant = false; break; } set_i32(lhs.value.u32() % r); break; }
                case wabt::Opcode::I32And: set_i32(lhs.value.u32() & rhs.value.u32()); break;
                case wabt::Opcode::I32Or: set_i32(lhs.value.u32() | rhs.value.u32()); break;
                case wabt::Opcode::I32Xor: set_i32(lhs.value.u32() ^ rhs.value.u32()); break;
                case wabt::Opcode::I32Shl: set_i32(lhs.value.u32() << (rhs.value.u32() & 31)); break;
                case wabt::Opcode::I32ShrS: set_i32(static_cast<int32_t>(lhs.value.u32()) >> (rhs.value.u32() & 31)); break;
                case wabt::Opcode::I32ShrU: set_i32(lhs.value.u32() >> (rhs.value.u32() & 31)); break;
                case wabt::Opcode::I32Rotl: { uint32_t s = rhs.value.u32() & 31; set_i32((lhs.value.u32() << s) | (lhs.value.u32() >> (32 - s))); break; }
                case wabt::Opcode::I32Rotr: { uint32_t s = rhs.value.u32() & 31; set_i32((lhs.value.u32() >> s) | (lhs.value.u32() << (32 - s))); break; }
                case wabt::Opcode::I32Eq: set_i32(lhs.value.u32() == rhs.value.u32()); break;
                case wabt::Opcode::I32Ne: set_i32(lhs.value.u32() != rhs.value.u32()); break;
                case wabt::Opcode::I32LtS: set_i32(static_cast<int32_t>(lhs.value.u32()) < static_cast<int32_t>(rhs.value.u32())); break;
                case wabt::Opcode::I32LtU: set_i32(lhs.value.u32() < rhs.value.u32()); break;
                case wabt::Opcode::I32GtS: set_i32(static_cast<int32_t>(lhs.value.u32()) > static_cast<int32_t>(rhs.value.u32())); break;
                case wabt::Opcode::I32GtU: set_i32(lhs.value.u32() > rhs.value.u32()); break;
                case wabt::Opcode::I32LeS: set_i32(static_cast<int32_t>(lhs.value.u32()) <= static_cast<int32_t>(rhs.value.u32())); break;
                case wabt::Opcode::I32LeU: set_i32(lhs.value.u32() <= rhs.value.u32()); break;
                case wabt::Opcode::I32GeS: set_i32(static_cast<int32_t>(lhs.value.u32()) >= static_cast<int32_t>(rhs.value.u32())); break;
                case wabt::Opcode::I32GeU: set_i32(lhs.value.u32() >= rhs.value.u32()); break;
                case wabt::Opcode::I64Add: set_i64(lhs.value.u64() + rhs.value.u64()); break;
                case wabt::Opcode::I64Sub: set_i64(lhs.value.u64() - rhs.value.u64()); break;
                case wabt::Opcode::I64Mul: set_i64(lhs.value.u64() * rhs.value.u64()); break;
                case wabt::Opcode::I64DivS: { int64_t r = rhs.value.u64(); if (!r) { result_op.is_constant = false; break; } set_i64(static_cast<int64_t>(lhs.value.u64()) / r); break; }
                case wabt::Opcode::I64DivU: { uint64_t r = rhs.value.u64(); if (!r) { result_op.is_constant = false; break; } set_i64(lhs.value.u64() / r); break; }
                case wabt::Opcode::I64RemS: { int64_t r = rhs.value.u64(); if (!r) { result_op.is_constant = false; break; } set_i64(static_cast<int64_t>(lhs.value.u64()) % r); break; }
                case wabt::Opcode::I64RemU: { uint64_t r = rhs.value.u64(); if (!r) { result_op.is_constant = false; break; } set_i64(lhs.value.u64() % r); break; }
                case wabt::Opcode::I64And: set_i64(lhs.value.u64() & rhs.value.u64()); break;
                case wabt::Opcode::I64Or: set_i64(lhs.value.u64() | rhs.value.u64()); break;
                case wabt::Opcode::I64Xor: set_i64(lhs.value.u64() ^ rhs.value.u64()); break;
                case wabt::Opcode::I64Shl: set_i64(lhs.value.u64() << (rhs.value.u64() & 63)); break;
                case wabt::Opcode::I64ShrS: set_i64(static_cast<int64_t>(lhs.value.u64()) >> (rhs.value.u64() & 63)); break;
                case wabt::Opcode::I64ShrU: set_i64(lhs.value.u64() >> (rhs.value.u64() & 63)); break;
                case wabt::Opcode::I64Rotl: { uint64_t s = rhs.value.u64() & 63; set_i64((lhs.value.u64() << s) | (lhs.value.u64() >> (64 - s))); break; }
                case wabt::Opcode::I64Rotr: { uint64_t s = rhs.value.u64() & 63; set_i64((lhs.value.u64() >> s) | (lhs.value.u64() << (64 - s))); break; }
                case wabt::Opcode::I64Eq: set_i32(lhs.value.u64() == rhs.value.u64()); break;
                case wabt::Opcode::I64Ne: set_i32(lhs.value.u64() != rhs.value.u64()); break;
                case wabt::Opcode::I64LtS: set_i32(static_cast<int64_t>(lhs.value.u64()) < static_cast<int64_t>(rhs.value.u64())); break;
                case wabt::Opcode::I64LtU: set_i32(lhs.value.u64() < rhs.value.u64()); break;
                case wabt::Opcode::I64GtS: set_i32(static_cast<int64_t>(lhs.value.u64()) > static_cast<int64_t>(rhs.value.u64())); break;
                case wabt::Opcode::I64GtU: set_i32(lhs.value.u64() > rhs.value.u64()); break;
                case wabt::Opcode::I64LeS: set_i32(static_cast<int64_t>(lhs.value.u64()) <= static_cast<int64_t>(rhs.value.u64())); break;
                case wabt::Opcode::I64LeU: set_i32(lhs.value.u64() <= rhs.value.u64()); break;
                case wabt::Opcode::I64GeS: set_i32(static_cast<int64_t>(lhs.value.u64()) >= static_cast<int64_t>(rhs.value.u64())); break;
                case wabt::Opcode::I64GeU: set_i32(lhs.value.u64() >= rhs.value.u64()); break;
                case wabt::Opcode::F32Add: set_f32(std::bit_cast<float>(lhs.value.f32_bits()) + std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Sub: set_f32(std::bit_cast<float>(lhs.value.f32_bits()) - std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Mul: set_f32(std::bit_cast<float>(lhs.value.f32_bits()) * std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Div: { float r = std::bit_cast<float>(rhs.value.f32_bits()); if (r == 0.0f) { result_op.is_constant = false; break; } set_f32(std::bit_cast<float>(lhs.value.f32_bits()) / r); break; }
                case wabt::Opcode::F32Min: set_f32(std::min(std::bit_cast<float>(lhs.value.f32_bits()), std::bit_cast<float>(rhs.value.f32_bits()))); break;
                case wabt::Opcode::F32Max: set_f32(std::max(std::bit_cast<float>(lhs.value.f32_bits()), std::bit_cast<float>(rhs.value.f32_bits()))); break;
                case wabt::Opcode::F32Copysign: set_f32(std::copysign(std::bit_cast<float>(lhs.value.f32_bits()), std::bit_cast<float>(rhs.value.f32_bits()))); break;
                case wabt::Opcode::F32Eq: set_i32(std::bit_cast<float>(lhs.value.f32_bits()) == std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Ne: set_i32(std::bit_cast<float>(lhs.value.f32_bits()) != std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Lt: set_i32(std::bit_cast<float>(lhs.value.f32_bits()) < std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Le: set_i32(std::bit_cast<float>(lhs.value.f32_bits()) <= std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Gt: set_i32(std::bit_cast<float>(lhs.value.f32_bits()) > std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F32Ge: set_i32(std::bit_cast<float>(lhs.value.f32_bits()) >= std::bit_cast<float>(rhs.value.f32_bits())); break;
                case wabt::Opcode::F64Add: set_f64(std::bit_cast<double>(lhs.value.f64_bits()) + std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Sub: set_f64(std::bit_cast<double>(lhs.value.f64_bits()) - std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Mul: set_f64(std::bit_cast<double>(lhs.value.f64_bits()) * std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Div: { double r = std::bit_cast<double>(rhs.value.f64_bits()); if (r == 0.0) { result_op.is_constant = false; break; } set_f64(std::bit_cast<double>(lhs.value.f64_bits()) / r); break; }
                case wabt::Opcode::F64Min: set_f64(std::min(std::bit_cast<double>(lhs.value.f64_bits()), std::bit_cast<double>(rhs.value.f64_bits()))); break;
                case wabt::Opcode::F64Max: set_f64(std::max(std::bit_cast<double>(lhs.value.f64_bits()), std::bit_cast<double>(rhs.value.f64_bits()))); break;
                case wabt::Opcode::F64Copysign: set_f64(std::copysign(std::bit_cast<double>(lhs.value.f64_bits()), std::bit_cast<double>(rhs.value.f64_bits()))); break;
                case wabt::Opcode::F64Eq: set_i32(std::bit_cast<double>(lhs.value.f64_bits()) == std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Ne: set_i32(std::bit_cast<double>(lhs.value.f64_bits()) != std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Lt: set_i32(std::bit_cast<double>(lhs.value.f64_bits()) < std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Le: set_i32(std::bit_cast<double>(lhs.value.f64_bits()) <= std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Gt: set_i32(std::bit_cast<double>(lhs.value.f64_bits()) > std::bit_cast<double>(rhs.value.f64_bits())); break;
                case wabt::Opcode::F64Ge: set_i32(std::bit_cast<double>(lhs.value.f64_bits()) >= std::bit_cast<double>(rhs.value.f64_bits())); break;
                default: result_op.is_constant = false; break;
            }

            if (result_op.is_constant) {
                js_stack.push(result_op);
                return {};
            }
        }

        Operand result_op;
        result_op.is_constant = false;
        result_op.type = op.GetResultType();

        switch (op) {
            case wabt::Opcode::I32Add: case wabt::Opcode::I64Add: case wabt::Opcode::F32Add: case wabt::Opcode::F64Add: result_op.js_repr = "(" + lhs.js_repr + " + " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Sub: case wabt::Opcode::I64Sub: case wabt::Opcode::F32Sub: case wabt::Opcode::F64Sub: result_op.js_repr = "(" + lhs.js_repr + " - " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Mul: case wabt::Opcode::I64Mul: case wabt::Opcode::F32Mul: case wabt::Opcode::F64Mul: result_op.js_repr = "(" + lhs.js_repr + " * " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32DivS: case wabt::Opcode::I64DivS: case wabt::Opcode::F32Div: case wabt::Opcode::F64Div: result_op.js_repr = "(" + lhs.js_repr + " / " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32DivU: result_op.js_repr = "((" + lhs.js_repr + " >>> 0) / (" + rhs.js_repr + " >>> 0))"; break;
            case wabt::Opcode::I64DivU: result_op.js_repr = "(" + lhs.js_repr + " / " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32RemS: case wabt::Opcode::I64RemS: result_op.js_repr = "(" + lhs.js_repr + " % " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32RemU: result_op.js_repr = "((" + lhs.js_repr + " >>> 0) % (" + rhs.js_repr + " >>> 0))"; break;
            case wabt::Opcode::I64RemU: result_op.js_repr = "(" + lhs.js_repr + " % " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Eq: case wabt::Opcode::I64Eq: case wabt::Opcode::F32Eq: case wabt::Opcode::F64Eq: result_op.js_repr = "(" + lhs.js_repr + " === " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Ne: case wabt::Opcode::I64Ne: case wabt::Opcode::F32Ne: case wabt::Opcode::F64Ne: result_op.js_repr = "(" + lhs.js_repr + " !== " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32LtS: case wabt::Opcode::I64LtS: case wabt::Opcode::F32Lt: case wabt::Opcode::F64Lt: result_op.js_repr = "(" + lhs.js_repr + " < " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32LtU: result_op.js_repr = "((" + lhs.js_repr + " >>> 0) < (" + rhs.js_repr + " >>> 0))"; break;
            case wabt::Opcode::I64LtU: result_op.js_repr = "(" + lhs.js_repr + " < " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32LeS: case wabt::Opcode::I64LeS: case wabt::Opcode::F32Le: case wabt::Opcode::F64Le: result_op.js_repr = "(" + lhs.js_repr + " <= " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32LeU: result_op.js_repr = "((" + lhs.js_repr + " >>> 0) <= (" + rhs.js_repr + " >>> 0))"; break;
            case wabt::Opcode::I64LeU: result_op.js_repr = "(" + lhs.js_repr + " <= " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32GtS: case wabt::Opcode::I64GtS: case wabt::Opcode::F32Gt: case wabt::Opcode::F64Gt: result_op.js_repr = "(" + lhs.js_repr + " > " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32GtU: result_op.js_repr = "((" + lhs.js_repr + " >>> 0) > (" + rhs.js_repr + " >>> 0))"; break;
            case wabt::Opcode::I64GtU: result_op.js_repr = "(" + lhs.js_repr + " > " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32GeS: case wabt::Opcode::I64GeS: case wabt::Opcode::F32Ge: case wabt::Opcode::F64Ge: result_op.js_repr = "(" + lhs.js_repr + " >= " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32GeU: result_op.js_repr = "((" + lhs.js_repr + " >>> 0) >= (" + rhs.js_repr + " >>> 0))"; break;
            case wabt::Opcode::I64GeU: result_op.js_repr = "(" + lhs.js_repr + " >= " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32And: case wabt::Opcode::I64And: result_op.js_repr = "(" + lhs.js_repr + " & " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Or: case wabt::Opcode::I64Or: result_op.js_repr = "(" + lhs.js_repr + " | " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Xor: case wabt::Opcode::I64Xor: result_op.js_repr = "(" + lhs.js_repr + " ^ " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Shl: case wabt::Opcode::I64Shl: result_op.js_repr = "(" + lhs.js_repr + " << " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32ShrS: case wabt::Opcode::I64ShrS: result_op.js_repr = "(" + lhs.js_repr + " >> " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32ShrU: result_op.js_repr = "(" + lhs.js_repr + " >>> " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I64ShrU: result_op.js_repr = "(" + lhs.js_repr + " >> " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Rotl: result_op.js_repr = "rotl32(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I32Rotr: result_op.js_repr = "rotr32(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I64Rotl: result_op.js_repr = "rotl64(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            case wabt::Opcode::I64Rotr: result_op.js_repr = "rotr64(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            case wabt::Opcode::F32Min: case wabt::Opcode::F64Min: result_op.js_repr = "Math.min(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            case wabt::Opcode::F32Max: case wabt::Opcode::F64Max: result_op.js_repr = "Math.max(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            case wabt::Opcode::F32Copysign: case wabt::Opcode::F64Copysign: result_op.js_repr = "copysign(" + lhs.js_repr + ", " + rhs.js_repr + ")"; break;
            default: pushStmt("/* UNHANDLED_BINARY_OP: " + std::string(op.GetName()) + " */"); result_op.js_repr = "UNHANDLED_OP(\"" + std::string(op.GetName()) + "\", " + lhs.js_repr + ", " + rhs. js_repr + ")"; break;
        }

        js_stack.push(result_op);
        return {};
    } else if (auto *callExpr = wabt::dyn_cast<wabt::CallExpr>(&expr)) {
        wabt::Index func_index = callExpr->var.index();

        const wabt::FuncSignature *sig = GetFuncSignature(callExpr->var);
        if (!sig) {
            throw std::runtime_error(
                "FATAL: Signature not found for call expression to func " + std::to_string(callExpr->var.index()));
        }
        size_t paramCount = sig->GetNumParams();
        std::vector<Operand> args = UnstackArgs(js_stack, reachability_stack, paramCount);

        std::string funcName = PrettyFuncName(func_index);

        std::vector<std::string> argStrs;
        for (auto &op: args) argStrs.push_back(op.js_repr);
        std::string callJs = funcName + "(" + Utils::join(argStrs, ", ") + ")";

        if (sig->GetNumResults() > 0) {
            Operand result;
            result.is_constant = false;
            result.type = sig->GetResultType(0);
            result.js_repr = callJs;
            js_stack.push(result);
        } else {
            pushStmt(callJs + ";");
        }
        return "";
    } else if (auto *call_indirectExpr = wabt::dyn_cast<wabt::CallIndirectExpr>(&expr)) {
        wabt::FuncType *func_type = module->GetFuncType(call_indirectExpr->decl.type_var);
        if (!func_type) {
            throw std::runtime_error("Function type not found for call_indirect");
        }

        const wabt::FuncSignature *sig = &func_type->sig;
        uint32_t table_idx = call_indirectExpr->table.index();
        size_t paramCount = sig->GetNumParams();

        std::string func_index_in_table = pop_or_throw(js_stack, reachability_stack, "call_indirect index").js_repr;
        std::vector<Operand> args = UnstackArgs(js_stack, reachability_stack, paramCount);
        std::vector<std::string> arg_strings;
        for (const auto &arg: args) { arg_strings.push_back(arg.js_repr); }

        std::string callJs = "table" + std::to_string(table_idx) + "[" + func_index_in_table + "](" + Utils::join(arg_strings, ", ") + ")";

        if (sig->GetNumResults() > 0) {
            Operand result_op;
            result_op.js_repr = callJs;
            result_op.type = sig->GetResultType(0);
            js_stack.push(result_op);
        } else {
            pushStmt(callJs + ";");
        }
        return "";
    } else if (wabt::isa<wabt::ReturnExpr>(&expr)) {
        if (!js_stack.empty())
            pushStmt("return " + pop_or_throw(js_stack, reachability_stack, "return").js_repr + ";");
        else
            pushStmt("return;");
        if (!reachability_stack.empty())
            reachability_stack.top() = false;
    } else if (wabt::isa<wabt::UnreachableExpr>(&expr)) {
        pushStmt("throw new Error('Unreachable code reached');");
        if (!reachability_stack.empty()) {
            reachability_stack.top() = false;
        }
    }


    if (trace_this_func) {
        const char *final_op_name = wabt::GetExprTypeName(expr.type());
        if (auto *bin = wabt::dyn_cast<wabt::BinaryExpr>(&expr)) final_op_name = bin->opcode.GetName();
        else if (auto *un = wabt::dyn_cast<wabt::UnaryExpr>(&expr)) final_op_name = un->opcode.GetName();

        TraceStack(func_idx, expr.loc, final_op_name, js_stack, "AFTER");
    }
    return {};
}


// CORRECTION: Added try-catch block for robust error handling
std::string Parser::ProcessFunction(wabt::Func *func, uint32_t realIndex) {
    try {
        std::cout << "processing function func" << realIndex
                << " with " << func->exprs.size() << " expressions.\n";

        if (problematic_indices.count(realIndex))
            return "function func" + std::to_string(realIndex) +
                   "() { /* SKIPPED */ }\n";

        std::stack<Operand> js_stack;
        std::vector<std::pair<std::string, LabelType> > labelStack;
        std::vector<std::string> js_statements;
        int nextLabelId = 0;

        std::unordered_map<int, std::string> local_names;
        for (const auto &[name,binding]: func->bindings)
            if (!name.empty() && name[0] == '$')
                local_names[binding.index] = name.substr(1);

        ProcessFunctionBody(*func, realIndex, func->exprs,
                            0, func->exprs.size(),
                            labelStack, nextLabelId,
                            js_stack, js_statements,
                            local_names);

        CleanEmptyBraces(js_statements);
        std::ostringstream src;

        std::vector<std::string> params;
        for (size_t i = 0; i < func->GetNumParams(); ++i) {
            params.push_back("local" + std::to_string(i));
        }

        std::string func_name = "func" + std::to_string(realIndex);
        src << "function " << func_name
                << "(" << Utils::join(params, ", ") << ") {\n";

        std::vector<std::string> locals_to_declare;
        std::unordered_set<std::string> declared_params(params.begin(), params.end());
        for (size_t i = 0; i < func->GetNumLocals(); ++i) {
            std::string local_name = "local" + std::to_string(i);
            if (declared_params.find(local_name) == declared_params.end()) {
                 locals_to_declare.push_back(local_name);
            }
        }

        if (!locals_to_declare.empty()) {
            src << "  let " << Utils::join(locals_to_declare, ", ") << ";\n";
        }

        for (const auto &l: js_statements) src << "  " << l << '\n';

        if (!func->decl.sig.result_types.empty()) {
            std::stack<bool> final_reachability;
            final_reachability.push(true);
            if(!js_stack.empty()) {
                 src << "  return " << pop_or_throw(js_stack, final_reachability, "final_return").js_repr << ";\n";
            }
        }
        src << "}\n";
        return src.str();

    } catch (const std::runtime_error& e) {
        std::cerr << "Error processing function func" << realIndex << ": " << e.what() << std::endl;
        std::string func_name = "func" + std::to_string(realIndex);
        std::string error_body = "function " + func_name + "() { /* ERROR: " + std::string(e.what()) + " */ }";
        return error_body;
    }
}


std::string Parser::ProcessExport(wabt::Export *exp) {
    std::string name = exp->name;
    switch (exp->kind) {
        case wabt::ExternalKind::Func: {
            uint32_t idx = exp->var.index();
            return name + ":" + PrettyFuncName(idx);
        }
        case wabt::ExternalKind::Memory:
            return name + ": memory";
        case wabt::ExternalKind::Table: {
            uint32_t idx = exp->var.index();
            return name + ": table" + std::to_string(idx);
        }
        case wabt::ExternalKind::Global:
            return name + ": global" + std::to_string(exp->var.index());
        default: break;
    }
    std::cerr << "Unknown export kind: " << static_cast<int>(exp->kind) << "\n";
    return "";
}

void Parser::ProcessImport(wabt::Import *imp,
                           uint32_t import_index,
                           std::ostringstream &prologue) {
    std::string jsName = "imp_" + std::to_string(import_index);
    RegisterImport(imp->module_name, imp->field_name, jsName);
}

std::string Parser::ProcessMemory(const MemoryI &mi) {
    std::string idx = std::to_string(mi.idx);

    for (auto *imp: this->module->imports) {
        if (auto *memImp = wabt::dyn_cast<wabt::MemoryImport>(imp)) {
            if (&memImp->memory == mi.memory) {
                return "  const memory" + idx + "Buffer = imports[\"" + imp->module_name + "\"][\"" + imp->field_name +
                       "\"];\n"
                       + "  const viewI32_" + idx + " = new Int32Array(memory" + idx + "Buffer);\n"
                       + "  const viewI64_" + idx + " = new BigInt64Array(memory" + idx + "Buffer);\n"
                       + "  const viewF32_" + idx + " = new Float32Array(memory" + idx + "Buffer);\n"
                       + "  const viewF64_" + idx + " = new Float64Array(memory" + idx + "Buffer);\n";
            }
        }
    }

    auto initial = mi.memory->page_limits.initial;
    std::string js = "  const memory" + idx + "Buffer = new ArrayBuffer(" + std::to_string(initial) + " * 65536);\n";
    js += "  const viewI32_" + idx + " = new Int32Array(memory" + idx + "Buffer);\n";
    js += "  const viewI64_" + idx + " = new BigInt64Array(memory" + idx + "Buffer);\n";
    js += "  const viewF32_" + idx + " = new Float32Array(memory" + idx + "Buffer);\n";
    js += "  const viewF64_" + idx + " = new Float64Array(memory" + idx + "Buffer);\n";

    return js;
}

std::string Parser::ProcessTable(const TableI &ti) {
    std::string idx_str = std::to_string(ti.idx);
    for (auto *imp: this->module->imports) {
        if (auto *tabImp = wabt::dyn_cast<wabt::TableImport>(imp)) {
            if (&tabImp->table == ti.table) {
                return "  const table" + idx_str + " = imports[\"" + tabImp->module_name + "\"][\"" + tabImp->field_name
                       + "\"];\n";
            }
        }
    }

    std::string internalTable = "  const table" + idx_str + " = new Array("
                                + std::to_string(ti.table->elem_limits.initial)
                                + ").fill(null);\n";
    return internalTable;
}

bool Parser::TryToProcessAsForLoop(wabt::LoopExpr loopExpr, std::vector<wabt::Expr> exprs) {
    return false;
}


bool Parser::TryProcessAsDoWhileLoop(const wabt::LoopExpr &loop,
                                     std::vector<std::pair<std::string, LabelType> > &labelStack,
                                     int &nextLabelId,
                                     std::stack<std::string> &js_stack,
                                     std::vector<std::string> &js_statements) {
    return false;
}

std::tuple<uint32_t, std::string> Parser::ProcessSegment(wabt::DataSegment *seg) {
    if (seg->kind == wabt::SegmentKind::Active) {
        uint32_t offset = 0;

        auto iter = seg->offset.begin();
        if (iter != seg->offset.end()) {
            auto &expr = *iter;
            auto *const_expr = wabt::dyn_cast<wabt::ConstExpr>(&expr);

            if (!const_expr || const_expr->const_.type() != wabt::Type::I32) {
                std::cerr << "Unsupported data segment offset expr" << std::endl;
                return std::tuple(0, "");
            }

            offset = const_expr->const_.u32();
        }

        const auto &bytes = seg->data;

        std::string data_string(bytes.begin(), bytes.end());

        return std::tuple(offset, data_string);
    } else if (seg->kind == wabt::SegmentKind::Passive) {
        const auto &bytes = seg->data;

        std::string data_string(bytes.begin(), bytes.end());

        return std::tuple(-1, data_string);
    }
    return std::tuple(0, "");
}

std::string Parser::InjectLetDeclarations(const std::string& func_src) {
    size_t brace_pos = func_src.find('{');
    if (brace_pos == std::string::npos) return func_src;

    std::string header = func_src.substr(0, brace_pos + 1);
    std::string body = func_src.substr(brace_pos + 1);
    if (!body.empty() && body.back() == '}') {
        body.pop_back();
    }

    body = std::regex_replace(body, std::regex(R"(\s*let\s+[a-zA-Z0-9_,\s]+;\s*\n)"), "");

    std::regex id_regex(R"(\b([a-zA-Z_$][a-zA-Z0-9_$]*)\b)");
    auto words_begin = std::sregex_iterator(body.begin(), body.end(), id_regex);
    auto words_end = std::sregex_iterator();

    std::unordered_set<std::string> ids;
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        ids.insert((*i).str());
    }

    std::smatch match;
    std::regex sig_regex(R"(function\s+([a-zA-Z0-9_$]+)\s*\(([^)]*)\))");
    std::unordered_set<std::string> params;
    std::string func_name_in_sig;

    if(std::regex_search(header, match, sig_regex)) {
        func_name_in_sig = match[1].str();
        std::string param_list = match[2].str();
        std::stringstream ss(param_list);
        std::string param;
        while(std::getline(ss, param, ',')) {
            Utils::trim(param);
            if(!param.empty()) params.insert(param);
        }
    }

    std::vector<std::string> locals_to_declare;
    for (const auto& id : ids) {
        if (params.count(id) || JS_RESERVED_WORDS.count(id) || id == func_name_in_sig) {
            continue;
        }

        bool is_known = false;
        for(const auto& [func_id, p_name] : this->pretty_name) {
            if (p_name == id) {
                is_known = true;
                break;
            }
        }
        if (is_known) continue;

        if (id.rfind("func", 0) == 0 && isdigit(id[4])) continue;
        if (id.rfind("global", 0) == 0) continue;
        if (id.rfind("table", 0) == 0) continue;
        if (id.rfind("memory", 0) == 0) continue;
        if (id.rfind("view", 0) == 0) continue;
        if (id == "imports") continue;

        locals_to_declare.push_back(id);
    }

    std::sort(locals_to_declare.begin(), locals_to_declare.end());

    std::ostringstream final_func;
    final_func << header << "\n";
    if (!locals_to_declare.empty()) {
        final_func << "  let " << Utils::join(locals_to_declare, ", ") << ";\n";
    }
    final_func << body << "\n}\n";

    return final_func.str();
}
