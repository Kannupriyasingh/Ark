#include <Ark/Compiler/Compiler.hpp>

#include <fstream>
#include <chrono>
#include <limits>
#include <picosha2.h>

#include <Ark/Literals.hpp>
#include <Ark/Utils.hpp>
#include <Ark/Builtins/Builtins.hpp>
#include <Ark/Compiler/Macros/Processor.hpp>

namespace Ark
{
    using namespace internal;
    using namespace literals;

    Compiler::Compiler(unsigned debug, const std::vector<std::string>& libenv, uint16_t options) :
        m_parser(debug, options, libenv), m_optimizer(options),
        m_options(options), m_debug(debug)
    {}

    void Compiler::feed(const std::string& code, const std::string& filename)
    {
        m_parser.feed(code, filename);

        MacroProcessor mp(m_debug, m_options);
        mp.feed(m_parser.ast());
        m_optimizer.feed(mp.ast());
    }

    void Compiler::compile()
    {
        pushHeadersPhase1();

        m_code_pages.emplace_back();  // create empty page

        // gather symbols, values, and start to create code segments
        _compile(m_optimizer.ast(), 0);
        // throw an error on undefined symbol uses
        checkForUndefinedSymbol();

        pushHeadersPhase2();

        // start code segments
        for (auto page : m_code_pages)
        {
            m_bytecode.push_back(Instruction::CODE_SEGMENT_START);

            // push number of elements
            pushNumber(static_cast<uint16_t>(page.size() + 1));

            for (auto inst : page)
                m_bytecode.push_back(inst);
            // just in case we got too far, always add a HALT to be sure the
            // VM won't do anything crazy
            m_bytecode.push_back(Instruction::HALT);
        }

        if (!m_code_pages.size())
        {
            m_bytecode.push_back(Instruction::CODE_SEGMENT_START);
            pushNumber(1_u16);
            m_bytecode.push_back(Instruction::HALT);
        }

        constexpr std::size_t header_size = 18;

        // generate a hash of the tables + bytecode
        std::vector<unsigned char> hash(picosha2::k_digest_size);
        picosha2::hash256(m_bytecode.begin() + header_size, m_bytecode.end(), hash);
        m_bytecode.insert(m_bytecode.begin() + header_size, hash.begin(), hash.end());
    }

    void Compiler::saveTo(const std::string& file)
    {
        if (m_debug >= 1)
            std::cout << "Final bytecode size: " << m_bytecode.size() * sizeof(uint8_t) << "B\n";

        std::ofstream output(file, std::ofstream::binary);
        output.write(reinterpret_cast<char*>(&m_bytecode[0]), m_bytecode.size() * sizeof(uint8_t));
        output.close();
    }

    const bytecode_t& Compiler::bytecode() noexcept
    {
        return m_bytecode;
    }

    void Compiler::pushHeadersPhase1() noexcept
    {
        /*
            Generating headers:
                - lang name (to be sure we are executing an ArkScript file)
                    on 4 bytes (ark + padding)
                - version (major: 2 bytes, minor: 2 bytes, patch: 2 bytes)
                - timestamp (8 bytes, unix format)
        */

        m_bytecode.push_back('a');
        m_bytecode.push_back('r');
        m_bytecode.push_back('k');
        m_bytecode.push_back(0_u8);

        // push version
        pushNumber(ARK_VERSION_MAJOR);
        pushNumber(ARK_VERSION_MINOR);
        pushNumber(ARK_VERSION_PATCH);

        // push timestamp
        unsigned long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
        for (char c = 0; c < 8; c++)
        {
            unsigned d = 56 - 8 * c;
            uint8_t b = (timestamp & (0xffULL << d)) >> d;
            m_bytecode.push_back(b);
        }
    }

    void Compiler::pushHeadersPhase2()
    {
        /*
            - symbols table
                + elements
            - values table header
                + elements
         */

        m_bytecode.push_back(Instruction::SYM_TABLE_START);
        // push size
        pushNumber(static_cast<uint16_t>(m_symbols.size()));
        // push elements
        for (auto sym : m_symbols)
        {
            // push the string, null terminated
            std::string s = sym.string();
            for (std::size_t i = 0, size = s.size(); i < size; ++i)
                m_bytecode.push_back(s[i]);
            m_bytecode.push_back(0_u8);
        }

        // values table
        m_bytecode.push_back(Instruction::VAL_TABLE_START);
        // push size
        pushNumber(static_cast<uint16_t>(m_values.size()));
        // push elements (separated with 0x00)
        for (auto val : m_values)
        {
            if (val.type == ValTableElemType::Number)
            {
                m_bytecode.push_back(Instruction::NUMBER_TYPE);
                auto n = std::get<double>(val.value);
                std::string t = std::to_string(n);
                for (std::size_t i = 0, size = t.size(); i < size; ++i)
                    m_bytecode.push_back(t[i]);
            }
            else if (val.type == ValTableElemType::String)
            {
                m_bytecode.push_back(Instruction::STRING_TYPE);
                std::string t = std::get<std::string>(val.value);
                for (std::size_t i = 0, size = t.size(); i < size; ++i)
                    m_bytecode.push_back(t[i]);
            }
            else if (val.type == ValTableElemType::PageAddr)
            {
                m_bytecode.push_back(Instruction::FUNC_TYPE);
                pushNumber(static_cast<uint16_t>(std::get<std::size_t>(val.value)));
            }
            else
                throw CompilationError("trying to put a value in the value table, but the type isn't handled.\nCertainly a logic problem in the compiler source code");

            m_bytecode.push_back(0_u8);
        }
    }

    std::size_t Compiler::countArkObjects(const std::vector<Node>& lst) noexcept
    {
        std::size_t n = 0;
        for (const Node& node : lst)
        {
            if (node.nodeType() != NodeType::GetField)
                n++;
        }
        return n;
    }

    std::optional<std::size_t> Compiler::isOperator(const std::string& name) noexcept
    {
        auto it = std::find(internal::operators.begin(), internal::operators.end(), name);
        if (it != internal::operators.end())
            return std::distance(internal::operators.begin(), it);
        return {};
    }

    std::optional<std::size_t> Compiler::isBuiltin(const std::string& name) noexcept
    {
        auto it = std::find_if(Builtins::builtins.begin(), Builtins::builtins.end(),
                               [&name](const std::pair<std::string, Value>& element) -> bool {
                                   return name == element.first;
                               });
        if (it != Builtins::builtins.end())
            return std::distance(Builtins::builtins.begin(), it);
        return {};
    }

    void Compiler::pushSpecificInstArgc(Instruction inst, uint16_t previous, int p) noexcept
    {
        if (inst == Instruction::LIST)
            pushNumber(previous, page_ptr(p));
        else if (inst == Instruction::APPEND || inst == Instruction::APPEND_IN_PLACE ||
                 inst == Instruction::CONCAT || inst == Instruction::CONCAT_IN_PLACE)
            pushNumber(previous - 1, page_ptr(p));
    }

    bool Compiler::mayBeFromPlugin(const std::string& name) noexcept
    {
        std::string splitted = Utils::splitString(name, ':')[0];
        auto it = std::find_if(m_plugins.begin(), m_plugins.end(),
                               [&splitted](const std::string& plugin) -> bool {
                                   return std::filesystem::path(plugin).stem().string() == splitted;
                               });
        return it != m_plugins.end();
    }

    void Compiler::throwCompilerError(const std::string& message, const Node& node)
    {
        throw CompilationError(makeNodeBasedErrorCtx(message, node));
    }

    void Compiler::_compile(const Node& x, int p)
    {
        // register symbols
        if (x.nodeType() == NodeType::Symbol)
            compileSymbol(x, p);
        else if (x.nodeType() == NodeType::GetField)
        {
            std::string name = x.string();
            // 'name' shouldn't be a builtin/operator, we can use it as-is
            uint16_t i = addSymbol(x);

            page(p).emplace_back(Instruction::GET_FIELD);
            pushNumber(i, page_ptr(p));
        }
        // register values
        else if (x.nodeType() == NodeType::String || x.nodeType() == NodeType::Number)
        {
            uint16_t i = addValue(x);

            page(p).emplace_back(Instruction::LOAD_CONST);
            pushNumber(i, page_ptr(p));
        }
        // empty code block should be nil
        else if (x.constList().empty())
        {
            auto it_builtin = isBuiltin("nil");
            page(p).emplace_back(Instruction::BUILTIN);
            pushNumber(static_cast<uint16_t>(it_builtin.value()), page_ptr(p));
        }
        // specific instructions
        else if (auto c0 = x.constList()[0]; c0.nodeType() == NodeType::Symbol && isSpecific(c0.string()).has_value())
            compileSpecific(c0, x, p);
        // registering structures
        else if (x.constList()[0].nodeType() == NodeType::Keyword)
        {
            Keyword n = x.constList()[0].keyword();

            switch (n)
            {
                case Keyword::If:
                    compileIf(x, p);
                    break;

                case Keyword::Set:
                    compileSet(x, p);
                    break;

                case Keyword::Let:
                    [[fallthrough]];
                case Keyword::Mut:
                    compileLetMut(n, x, p);
                    break;

                case Keyword::Fun:
                    compileFunction(x, p);
                    break;

                case Keyword::Begin:
                {
                    for (std::size_t i = 1, size = x.constList().size(); i < size; ++i)
                        _compile(x.constList()[i], p);
                    break;
                }

                case Keyword::While:
                    compileWhile(x, p);
                    break;

                case Keyword::Import:
                    compilePluginImport(x, p);
                    break;

                case Keyword::Quote:
                    compileQuote(x, p);
                    break;

                case Keyword::Del:
                    compileDel(x, p);
                    break;
            }
        }
        else
        {
            // if we are here, we should have a function name
            // push arguments first, then function name, then call it
            handleCalls(x, p);
        }
    }

    void Compiler::compileSymbol(const Node& x, int p)
    {
        std::string name = x.string();

        if (auto it_builtin = isBuiltin(name))
        {
            page(p).emplace_back(Instruction::BUILTIN);
            pushNumber(static_cast<uint16_t>(it_builtin.value()), page_ptr(p));
        }
        else if (auto it_operator = isOperator(name))
            page(p).emplace_back(static_cast<uint8_t>(Instruction::FIRST_OPERATOR + it_operator.value()));
        else  // var-use
        {
            uint16_t i = addSymbol(x);

            page(p).emplace_back(Instruction::LOAD_SYMBOL);
            pushNumber(i, page_ptr(p));
        }
    }

    void Compiler::compileSpecific(const Node& c0, const Node& x, int p)
    {
        std::string name = c0.string();
        Instruction inst = isSpecific(name).value();

        // length of at least 1 since we got a symbol name
        uint16_t argc = countArkObjects(x.constList()) - 1;
        // error, can not use append/concat/pop (and their in place versions) with a <2 length argument list
        if (argc < 2 && inst != Instruction::LIST)
            throw CompilationError("can not use " + name + " with less than 2 arguments");

        // compile arguments in reverse order
        for (uint16_t i = x.constList().size() - 1; i > 0; --i)
        {
            uint16_t j = i;
            while (x.constList()[j].nodeType() == NodeType::GetField)
                --j;
            uint16_t diff = i - j;
            while (j < i)
            {
                _compile(x.constList()[j], p);
                ++j;
            }
            _compile(x.constList()[i], p);
            i -= diff;
        }

        // put inst and number of arguments
        page(p).emplace_back(inst);
        pushSpecificInstArgc(inst, argc, p);
    }

    void Compiler::compileIf(const Node& x, int p)
    {
        // compile condition
        _compile(x.constList()[1], p);
        // jump only if needed to the x.list()[2] part
        page(p).emplace_back(Instruction::POP_JUMP_IF_TRUE);
        std::size_t jump_to_if_pos = page(p).size();
        // absolute address to jump to if condition is true
        pushNumber(0_u16, page_ptr(p));
        // else code
        if (x.constList().size() == 4)  // we have an else clause
            _compile(x.constList()[3], p);
        // when else is finished, jump to end
        page(p).emplace_back(Instruction::JUMP);
        std::size_t jump_to_end_pos = page(p).size();
        pushNumber(0_u16, page_ptr(p));
        // set jump to if pos
        page(p)[jump_to_if_pos] = (static_cast<uint16_t>(page(p).size()) & 0xff00) >> 8;
        page(p)[jump_to_if_pos + 1] = static_cast<uint16_t>(page(p).size()) & 0x00ff;
        // if code
        _compile(x.constList()[2], p);
        // set jump to end pos
        page(p)[jump_to_end_pos] = (static_cast<uint16_t>(page(p).size()) & 0xff00) >> 8;
        page(p)[jump_to_end_pos + 1] = static_cast<uint16_t>(page(p).size()) & 0x00ff;
    }

    void Compiler::compileFunction(const Node& x, int p)
    {
        // capture, if needed
        for (auto it = x.constList()[1].constList().begin(), it_end = x.constList()[1].constList().end(); it != it_end; ++it)
        {
            if (it->nodeType() == NodeType::Capture)
            {
                // first check that the capture is a defined symbol
                if (std::find(m_defined_symbols.begin(), m_defined_symbols.end(), it->string()) == m_defined_symbols.end())
                {
                    // we didn't find it in the defined symbol list, thus we can't capture it
                    throwCompilerError("Can not capture " + it->string() + " because it is referencing an unbound variable.", *it);
                }
                page(p).emplace_back(Instruction::CAPTURE);
                addDefinedSymbol(it->string());
                uint16_t var_id = addSymbol(*it);
                pushNumber(var_id, page_ptr(p));
            }
        }
        // create new page for function body
        m_code_pages.emplace_back();
        std::size_t page_id = m_code_pages.size() - 1;
        // load value on the stack
        page(p).emplace_back(Instruction::LOAD_CONST);
        uint16_t id = addValue(page_id, x);  // save page_id into the constants table as PageAddr
        pushNumber(id, page_ptr(p));
        // pushing arguments from the stack into variables in the new scope
        for (auto it = x.constList()[1].constList().begin(), it_end = x.constList()[1].constList().end(); it != it_end; ++it)
        {
            if (it->nodeType() == NodeType::Symbol)
            {
                page(page_id).emplace_back(Instruction::MUT);
                uint16_t var_id = addSymbol(*it);
                addDefinedSymbol(it->string());
                pushNumber(var_id, page_ptr(page_id));
            }
        }
        // push body of the function
        _compile(x.constList()[2], page_id);
        // return last value on the stack
        page(page_id).emplace_back(Instruction::RET);
    }

    void Compiler::compileLetMut(Keyword n, const Node& x, int p)
    {
        std::string name = x.constList()[1].string();
        uint16_t i = addSymbol(x.constList()[1]);
        addDefinedSymbol(name);

        // put value before symbol id
        putValue(x, p);

        page(p).emplace_back(n == Keyword::Let ? Instruction::LET : Instruction::MUT);
        pushNumber(i, page_ptr(p));
    }

    void Compiler::compileWhile(const Node& x, int p)
    {
        // save current position to jump there at the end of the loop
        std::size_t current = page(p).size();
        // push condition
        _compile(x.constList()[1], p);
        // absolute jump to end of block if condition is false
        page(p).emplace_back(Instruction::POP_JUMP_IF_FALSE);
        std::size_t jump_to_end_pos = page(p).size();
        // absolute address to jump to if condition is false
        pushNumber(0_u16, page_ptr(p));
        // push code to page
        _compile(x.constList()[2], p);
        // loop, jump to the condition
        page(p).emplace_back(Instruction::JUMP);
        // abosolute address
        pushNumber(static_cast<uint16_t>(current), page_ptr(p));
        // set jump to end pos
        page(p)[jump_to_end_pos] = (static_cast<uint16_t>(page(p).size()) & 0xff00) >> 8;
        page(p)[jump_to_end_pos + 1] = static_cast<uint16_t>(page(p).size()) & 0x00ff;
    }

    void Compiler::compileSet(const Node& x, int p)
    {
        uint16_t i = addSymbol(x.constList()[1]);

        // put value before symbol id
        putValue(x, p);

        page(p).emplace_back(Instruction::STORE);
        pushNumber(i, page_ptr(p));
    }

    void Compiler::compileQuote(const Node& x, int p)
    {
        // create new page for quoted code
        m_code_pages.emplace_back();
        std::size_t page_id = m_code_pages.size() - 1;
        _compile(x.constList()[1], page_id);
        page(page_id).emplace_back(Instruction::RET);  // return to the last frame

        // call it
        uint16_t id = addValue(page_id, x);  // save page_id into the constants table as PageAddr
        page(p).emplace_back(Instruction::LOAD_CONST);
        pushNumber(id, page_ptr(p));
    }

    void Compiler::compilePluginImport(const Node& x, int p)
    {
        // register plugin path in the constants table
        uint16_t id = addValue(x.constList()[1]);
        // save plugin name to use it later
        m_plugins.push_back(x.constList()[1].string());
        // add plugin instruction + id of the constant refering to the plugin path
        page(p).emplace_back(Instruction::PLUGIN);
        pushNumber(id, page_ptr(p));
    }

    void Compiler::compileDel(const Node& x, int p)
    {
        // get id of symbol to delete
        uint16_t i = addSymbol(x.constList()[1]);

        page(p).emplace_back(Instruction::DEL);
        pushNumber(i, page_ptr(p));
    }

    void Compiler::handleCalls(const Node& x, int p)
    {
        m_temp_pages.emplace_back();
        int proc_page = -static_cast<int>(m_temp_pages.size());
        _compile(x.constList()[0], proc_page);  // storing proc

        // trying to handle chained closure.field.field.field...
        std::size_t n = 1;  // we need it later
        const std::size_t end = x.constList().size();
        while (n < end)
        {
            if (x.constList()[n].nodeType() == NodeType::GetField)
            {
                _compile(x.constList()[n], proc_page);
                n++;
            }
            else
                break;
        }
        std::size_t proc_page_len = m_temp_pages.back().size();

        // we know that operators take only 1 instruction, so if there are more
        // it's a builtin/function
        if (proc_page_len > 1)
        {
            // push arguments on current page
            for (auto exp = x.constList().begin() + n, exp_end = x.constList().end(); exp != exp_end; ++exp)
                _compile(*exp, p);
            // push proc from temp page
            for (auto&& inst : m_temp_pages.back())
                page(p).push_back(inst);
            m_temp_pages.pop_back();

            // call the procedure
            page(p).push_back(Instruction::CALL);
            // number of arguments
            std::size_t args_count = 0;
            for (auto it = x.constList().begin() + 1, it_end = x.constList().end(); it != it_end; ++it)
            {
                if (it->nodeType() != NodeType::GetField &&
                    it->nodeType() != NodeType::Capture)
                    args_count++;
            }
            pushNumber(static_cast<uint16_t>(args_count), page_ptr(p));
        }
        else  // operator
        {
            // retrieve operator
            auto op_inst = m_temp_pages.back()[0];
            m_temp_pages.pop_back();

            // push arguments on current page
            std::size_t exp_count = 0;
            for (std::size_t index = n, size = x.constList().size(); index < size; ++index)
            {
                _compile(x.constList()[index], p);

                if ((index + 1 < size &&
                     x.constList()[index + 1].nodeType() != NodeType::GetField &&
                     x.constList()[index + 1].nodeType() != NodeType::Capture) ||
                    index + 1 == size)
                    exp_count++;

                // in order to be able to handle things like (op A B C D...)
                // which should be transformed into A B op C op D op...
                if (exp_count >= 2)
                    page(p).push_back(op_inst);
            }

            if (exp_count == 1)
                page(p).push_back(op_inst);

            // need to check we didn't push the (op A B C D...) things for operators not supporting it
            if (exp_count > 2)
            {
                switch (op_inst)
                {
                    // authorized instructions
                    case Instruction::ADD:
                    case Instruction::SUB:
                    case Instruction::DIV:
                    case Instruction::MUL:
                    case Instruction::MOD:
                    case Instruction::AND_:
                    case Instruction::OR_:
                        break;

                    default:
                        throwCompilerError(
                            "can not create a chained expression (of length " + std::to_string(exp_count) +
                                ") for operator `" + std::string(internal::operators[static_cast<std::size_t>(op_inst - Instruction::FIRST_OPERATOR)]) +
                                "'. You most likely forgot a `)'.",
                            x);
                }
            }
        }
    }

    void Compiler::putValue(const Node& x, int p)
    {
        // starting at index = 2 because x is a (let|mut|set variable ...) node
        for (std::size_t idx = 2, end = x.constList().size(); idx < end; ++idx)
            _compile(x.constList()[idx], p);
    }

    uint16_t Compiler::addSymbol(const Node& sym)
    {
        // otherwise, add the symbol, and return its id in the table
        auto it = std::find_if(m_symbols.begin(), m_symbols.end(), [&sym](const Node& sym_node) -> bool {
            return sym_node.string() == sym.string();
        });
        if (it == m_symbols.end())
        {
            m_symbols.push_back(sym);
            it = m_symbols.begin() + m_symbols.size() - 1;
        }

        auto distance = std::distance(m_symbols.begin(), it);
        if (distance < std::numeric_limits<uint16_t>::max())
            return static_cast<uint16_t>(distance);
        else
            throwCompilerError("Too many symbols (exceeds 65'536), aborting compilation.", sym);
    }

    uint16_t Compiler::addValue(const Node& x)
    {
        ValTableElem v(x);
        auto it = std::find(m_values.begin(), m_values.end(), v);
        if (it == m_values.end())
        {
            m_values.push_back(v);
            it = m_values.begin() + m_values.size() - 1;
        }

        auto distance = std::distance(m_values.begin(), it);
        if (distance < std::numeric_limits<uint16_t>::max())
            return static_cast<uint16_t>(distance);
        else
            throwCompilerError("Too many values (exceeds 65'536), aborting compilation.", x);
    }

    uint16_t Compiler::addValue(std::size_t page_id, const Node& current)
    {
        ValTableElem v(page_id);
        auto it = std::find(m_values.begin(), m_values.end(), v);
        if (it == m_values.end())
        {
            m_values.push_back(v);
            it = m_values.begin() + m_values.size() - 1;
        }

        auto distance = std::distance(m_values.begin(), it);
        if (distance < std::numeric_limits<uint16_t>::max())
            return static_cast<uint16_t>(distance);
        else
            throwCompilerError("Too many values (exceeds 65'536), aborting compilation.", current);
    }

    void Compiler::addDefinedSymbol(const std::string& sym)
    {
        // otherwise, add the symbol, and return its id in the table
        auto it = std::find(m_defined_symbols.begin(), m_defined_symbols.end(), sym);
        if (it == m_defined_symbols.end())
            m_defined_symbols.push_back(sym);
    }

    void Compiler::checkForUndefinedSymbol()
    {
        for (const Node& sym : m_symbols)
        {
            const std::string& str = sym.string();
            bool is_plugin = mayBeFromPlugin(str);

            auto it = std::find(m_defined_symbols.begin(), m_defined_symbols.end(), str);
            if (it == m_defined_symbols.end() && !is_plugin)
                throwCompilerError("Unbound variable error (variable is used but not defined)", sym);
        }
    }

    void Compiler::pushNumber(uint16_t n, std::vector<uint8_t>* page) noexcept
    {
        if (page == nullptr)
        {
            m_bytecode.push_back((n & 0xff00) >> 8);
            m_bytecode.push_back(n & 0x00ff);
        }
        else
        {
            page->emplace_back((n & 0xff00) >> 8);
            page->emplace_back(n & 0x00ff);
        }
    }
}
