/**
 * @file Compiler.hpp
 * @author Alexandre Plateau (lexplt.dev@gmail.com)
 * @brief ArkScript compiler is in charge of transforming the AST into bytecode
 * @version 0.4
 * @date 2020-10-27
 * 
 * @copyright Copyright (c) 2020-2021
 * 
 */

#ifndef ARK_COMPILER_COMPILER_HPP
#define ARK_COMPILER_COMPILER_HPP

#include <vector>
#include <iostream>
#include <string>
#include <cinttypes>
#include <optional>
#include <functional>

#include <Ark/Compiler/Parser.hpp>
#include <Ark/Compiler/Node.hpp>
#include <Ark/Compiler/CValue.hpp>
#include <Ark/Compiler/Optimizer.hpp>
#include <Ark/Compiler/Instructions.hpp>
#include <Ark/Compiler/BytecodeReader.hpp>
#include <Ark/Builtins/Builtins.hpp>
#include <Ark/Utils.hpp>
#include <Ark/Config.hpp>
#include <Ark/Compiler/makeErrorCtx.hpp>

namespace Ark
{
    namespace internal
    {
        /**
         * @brief Category for complex nodes (not including values)
         * 
         */
        enum class NodeCategory
        {
            Store,  ///< This comprises let, mut and set nodes
            If,
            Function,
            FunctionCall,
            While
        };
    }

    class State;

    /**
     * @brief The ArkScript bytecode compiler
     * 
     */
    class ARK_API_EXPORT Compiler
    {
    public:
        /**
         * @brief Construct a new Compiler object
         * 
         * @param debug the debug level
         * @param lib_dir the path to the standard library
         * @param options the compilers options
         */
        Compiler(unsigned debug, const std::string& lib_dir, uint16_t options = DefaultFeatures);

        /**
         * @brief Feed the differents variables with information taken from the given source code file
         * 
         * @param code the code of the file
         * @param filename the name of the file
         */
        void feed(const std::string& code, const std::string& filename = ARK_NO_NAME_FILE);

        /**
         * @brief Start the compilation
         * 
         */
        void compile();

        /**
         * @brief Save generated bytecode to a file
         * 
         * @param file the name of the file where the bytecode will be saved
         */
        void saveTo(const std::string& file);

        /**
         * @brief Return the constructed bytecode object
         * 
         * @return const bytecode_t& 
         */
        const bytecode_t& bytecode() noexcept;

        friend class Ark::State;

    private:
        Parser m_parser;
        Optimizer m_optimizer;
        uint16_t m_options;
        // tables: symbols, values, plugins and codes
        std::vector<internal::Node> m_symbols;
        std::vector<std::string> m_defined_symbols;
        std::vector<std::string> m_plugins;
        std::vector<internal::CValue> m_values;
        std::vector<std::vector<internal::Inst_t>> m_code_pages;
        std::vector<std::vector<internal::Inst_t>> m_temp_pages;  ///< we need temporary code pages for some compilations passes
        std::vector<std::pair<internal::NodeCategory, bool>> m_history;

        bytecode_t m_bytecode;
        unsigned m_debug;  ///< the debug level of the compiler

        /**
         * @brief Push the first headers of the bytecode file
         * 
         */
        void pushHeadersPhase1() noexcept;

        /**
         * @brief Push the other headers, after having compile the file
         * (it's needed because we read the symbol/value tables, populated by compilation)
         * 
         */
        void pushHeadersPhase2();

        /**
         * @brief helper functions to get a temp or finalized code page
         * 
         * @param i page index, if negative, refers to a temporay code page
         * @return std::vector<internal::Inst_t>& 
         */
        inline std::vector<internal::Inst_t>& page(int i) noexcept;

        /**
         * @brief Count the number of "valid" ark objects in a node
         * @details Isn't considered valid a GetField, because we use
         *          this function to count the number of arguments of function calls.
         * 
         * @param lst 
         * @return std::size_t 
         */
        inline std::size_t countArkObjects(const std::vector<internal::Node>& lst) noexcept;

        /**
         * @brief Checking if a symbol is an operator
         * 
         * @param name symbol name
         * @return std::optional<std::size_t> position in the operators' list
         */
        inline std::optional<std::size_t> isOperator(const std::string& name) noexcept;

        /**
         * @brief Checking if a symbol is a builtin
         * 
         * @param name symbol name
         * @return std::optional<std::size_t> position in the builtins' list
         */
        inline std::optional<std::size_t> isBuiltin(const std::string& name) noexcept;

        /**
         * @brief Checking if a symbol may be coming from a plugin
         * 
         * @param name symbol name
         * @return true the symbol may be from a plugin, loaded at runtime
         * @return false 
         */
        inline bool mayBeFromPlugin(const std::string& name) noexcept;

        /**
         * @brief Throw a nice error message
         * 
         * @param message 
         * @param node 
         */
        inline void throwCompilerError(const std::string& message, const internal::Node& node)
        {
            throw CompilationError(internal::makeNodeBasedErrorCtx(message, node));
        }

        /**
         * @brief Check if the value of node can be stored in a distant or near parent
         * @details Use an history (category, terminal?) to check this
         * 
         * @return true 
         * @return false 
         */
        inline bool isCurrentNodeStored();

        /**
         * @brief Check if a node can be terminal
         * @details A node is terminal if it's the only child of its parent, or if it's the last child
         * 
         * @param position position of the node in its parent's list
         * @param parent_size the number of nodes in the parent
         * @return true 
         * @return false 
         */
        inline bool isTerminalNode(std::size_t position, std::size_t parent_size);

        /**
         * @brief Compile a single node recursively
         * 
         * @param x the internal::Node to compile
         * @param parent the parent node holding the current node `x`
         * @param p the current page number we're on
         */
        void _compile(const internal::Node& x, internal::Node* parent, std::size_t pos, int p);

        void compileSymbol(                             const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileSpecific(const internal::Node& c0,  const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileIf(                                 const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileFunction(                           const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileLetMut(internal::Keyword n,         const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileWhile(                              const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileSet(                                const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileQuote(                              const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compilePluginImport(                       const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void compileDel(                                const internal::Node& x, internal::Node* parent, std::size_t pos, int p);
        void handleCalls(                               const internal::Node& x, internal::Node* parent, std::size_t pos, int p);

        /**
         * @brief Register a given node in the symbol table
         * 
         * @param sym 
         * @return std::size_t 
         */
        std::size_t addSymbol(const internal::Node& sym) noexcept;

        /**
         * @brief Register a given node in the value table
         * 
         * @param x 
         * @return std::size_t 
         */
        std::size_t addValue(const internal::Node& x) noexcept;

        /**
         * @brief Register a page id (function reference) in the value table
         * 
         * @param page_id 
         * @return std::size_t 
         */
        std::size_t addValue(std::size_t page_id) noexcept;

        /**
         * @brief Register a symbol as defined, so that later we can throw errors on undefined symbols
         * 
         * @param sym 
         */
        void addDefinedSymbol(const std::string& sym);

        /**
         * @brief Checks for undefined symbols, not present in the defined symbols table
         * 
         */
        void checkForUndefinedSymbol();

        /**
         * @brief Push a number on stack (need 2 bytes)
         * 
         * @param n the number to push
         * @param page the page where it should land, nullptr for current page
         */
        void pushNumber(uint16_t n, std::vector<internal::Inst_t>* page = nullptr) noexcept;
    };

    #include "inline/Compiler.inl"
}

#endif
