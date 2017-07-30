/*
 * Copyright (C) 2015-2017 Nagisa Sekiguchi
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fstream>
#include <sstream>
#include <unordered_set>

#include "directive_parser.h"
#include <misc/fatal.h>

namespace ydsh {
namespace directive {

// ######################
// ##     TypeImpl     ##
// ######################

std::string TypeImpl::getRealName() const {
    if(this->childs.empty()) {
        return this->name;
    }

    std::string str(this->name);
    str += "<";
    unsigned int size = this->childs.size();
    for(unsigned int i = 0; i < size; i++) {
        if(i > 0) {
            str += ",";
        }
        str += this->childs[i]->getRealName();
    }
    str += ">";
    return str;
}

bool TypeImpl::operator==(const TypeImpl &t) const {
    // check name
    if(this->getName() != t.getName()) {
        return false;
    }

    // check child size
    unsigned int size = this->getChilds().size();
    if(size != t.getChilds().size()) {
        return false;
    }

    // check child
    for(unsigned int i = 0; i < size; i++) {
        if(*this->getChilds()[i] != *t.getChilds()[i]) {
            return false;
        }
    }
    return true;
}

std::shared_ptr<TypeImpl> TypeImpl::create(const char *name) {
    return std::make_shared<TypeImpl>(name, 0);
}

std::shared_ptr<TypeImpl> TypeImpl::create(const char *name, const std::shared_ptr<TypeImpl> &child) {
    auto value(std::make_shared<TypeImpl>(name, 1));
    value->childs[0] = child;
    return value;
}

// #####################
// ##     TypeEnv     ##
// #####################

TypeEnv::TypeEnv() {
    this->addType(TypeImpl::create("Int"));
    this->addType(TypeImpl::create("String"));
    this->addType(TypeImpl::create("Boolean"));
}

const Type &TypeEnv::getType(const std::string &name) const {
    auto iter = this->typeMap.find(name);
    if(iter == this->typeMap.end()) {
        fatal("undefined type: %s\n", name.c_str());
    }
    return iter->second;
}

const Type &TypeEnv::getArrayType(const Type &elementType) {
    std::string name("Array<");
    name += elementType->getRealName();
    name += ">";

    if(this->hasType(name)) {
        return this->getType(name);
    }
    return addType(std::move(name), TypeImpl::create("Array", elementType));
}

const Type &TypeEnv::addType(Type &&type) {
    std::string name(type->getRealName());
    return this->addType(std::move(name), std::move(type));
}

const Type &TypeEnv::addType(std::string &&name, Type &&type) {
    auto pair = this->typeMap.insert(std::make_pair(std::move(name), std::move(type)));
    if(!pair.second) {
        fatal("found duplicated type: %s\n", pair.first->first.c_str());
    }
    return pair.first->second;
}

bool TypeEnv::hasType(const std::string &name) const {
    auto iter = this->typeMap.find(name);
    return iter != this->typeMap.end();
}


// #############################
// ##     DirectiveParser     ##
// #############################

static bool isDirective(const std::string &line) {
    std::string prefix("#$test");
    if(line.size() < prefix.size()) {
        return false;
    }
    unsigned int size = prefix.size();
    for(unsigned int i = 0; i < size; i++) {
        if(line[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

#define TRY(expr) \
({ auto v = expr; if(this->hasError()) { return nullptr; } std::forward<decltype(v)>(v); })


bool DirectiveParser::operator()(const char *sourceName, std::istream &input, Directive &d) {
    std::string line;
    unsigned int lineNum = 0;

    while(std::getline(input, line)) {
        lineNum++;

        if(!isDirective(line)) {
            continue;
        }

        // prepare
        const char *src = line.c_str() + 1;
        Lexer lexer(src);
        lexer.setLineNum(lineNum);
        this->lexer = &lexer;
        this->fetchNext();

        auto node = this->parse_toplevel();
        if(this->hasError()) {
            auto &e = *this->getError();
            std::cerr << sourceName << ":" << lexer.getLineNum() << ": [syntax error] " << e.getMessage() << std::endl;
            std::cerr << src << std::endl;
            Token lineToken;
            lineToken.pos = 0;
            lineToken.size = line.size();
            std::cerr << this->lexer->formatLineMarker(lineToken, e.getErrorToken()) << std::endl;
            return false;
        }

        DirectiveInitializer initializer;
        initializer(node, d);
        if(initializer.hasError()) {
            auto &e = initializer.getError();
            std::cerr << sourceName << ":" << lexer.getLineNum() << ": [semantic error] ";
            std::cerr << e.getMessage() << std::endl;

            std::cerr << src << std::endl;
            Token lineToken;
            lineToken.pos = 0;
            lineToken.size = line.size();
            std::cerr << this->lexer->formatLineMarker(lineToken, e.getErrorToken()) << std::endl;
            return false;
        }
        return true;
    }
    return true;
}

#define CUR_KIND() this->curKind

std::unique_ptr<DirectiveNode> DirectiveParser::parse_toplevel() {
    Token token = TRY(this->expect(APPLIED_NAME));
    std::unique_ptr<DirectiveNode> node(new DirectiveNode(token, this->lexer->toName(token)));

    TRY(this->expect(LP));

    bool first = true;
    do {
        if(!first) {
            TRY(this->expect(COMMA));
        } else {
            first = false;
        }
        auto attr = TRY(this->parse_attribute());
        node->append(std::move(attr));
    } while(CUR_KIND() == COMMA);

    TRY(this->expect(RP));

    return node;
}

std::unique_ptr<AttributeNode> DirectiveParser::parse_attribute() {
    Token token = TRY(this->expect(APPLIED_NAME));

    TRY(this->expect(ASSIGN));

    auto value = TRY(this->parse_value());

    return std::unique_ptr<AttributeNode>(new AttributeNode(token, this->lexer->toName(token), std::move(value)));
}

std::unique_ptr<Node> DirectiveParser::parse_value() {
    switch(CUR_KIND()) {
    case INT_LITERAL:
        return this->parse_number();
    case STRING_LITERAL:
        return this->parse_string();
    case TRUE_LITERAL:
    case FALSE_LITERAL:
        return this->parse_boolean();
    case ARRAY_OPEN:
        return this->parse_array();
    default:
        const TokenKind alters[] = {
#define EACH_LA_value(OP) \
    OP(INT_LITERAL) \
    OP(STRING_LITERAL) \
    OP(TRUE_LITERAL) \
    OP(FALSE_LITERAL) \
    OP(ARRAY_OPEN)
#define GEN_LA_value(OP) OP,
                EACH_LA_value(GEN_LA_value)
#undef GEN_LA_value
#undef EACH_LA_value
        };
        this->raiseNoViableAlterError(alters);
        return nullptr;
    }
}

std::unique_ptr<Node> DirectiveParser::parse_number() {
    Token token = TRY(this->expect(INT_LITERAL));
    int status;
    int value = this->lexer->toInt(token, status);
    if(value < 0 || status != 0) {
        std::string str("out of range number: ");
        str += this->lexer->toTokenText(token);
        this->createError(INT_LITERAL, token, "TokenFormat", std::move(str));
        return nullptr;
    }
    return std::unique_ptr<Node>(new NumberNode(token, value));
}

std::unique_ptr<Node> DirectiveParser::parse_string() {
    Token token = TRY(this->expect(STRING_LITERAL));
    return std::unique_ptr<Node>(new StringNode(token, this->lexer->toString(token)));
}

std::unique_ptr<Node> DirectiveParser::parse_boolean() {
    Token token;
    bool value;
    if(CUR_KIND() == TRUE_LITERAL) {
        token = TRY(this->expect(TRUE_LITERAL));
        value = true;
    } else {
        token = TRY(this->expect(FALSE_LITERAL));
        value = false;
    }
    return std::unique_ptr<Node>(new BooleanNode(token, value));
}

std::unique_ptr<Node> DirectiveParser::parse_array() {
    Token token = TRY(this->expect(ARRAY_OPEN));
    std::unique_ptr<ArrayNode> arrayNode(new ArrayNode(token));
    auto value = TRY(this->parse_value());
    arrayNode->appendNode(std::move(value));

    while(CUR_KIND() == COMMA) {
        TRY(this->expect(COMMA));

        value = TRY(this->parse_value());
        arrayNode->appendNode(std::move(value));
    }
    TRY(this->expect(ARRAY_CLOSE));
    return std::move(arrayNode);
}

template <typename T>
T &cast(Node &node) {
    static_assert(std::is_base_of<Node, T>::value, "not derived type");
    if(!T::classof(&node)) {
        fatal("illegal cast\n");
    }
    return static_cast<T&>(node);
}

#undef TRY
#define TRY(expr) \
({ auto v = expr; if(this->hasError()) { return; } std::forward<decltype(v)>(v); })


// ##################################
// ##     DirectiveInitializer     ##
// ##################################

void DirectiveInitializer::operator()(const std::unique_ptr<DirectiveNode> &node, Directive &d) {
    this->error.reset();

    // check name
    if(node->getName() != "test") {
        std::string str("unsupported directive: ");
        str += node->getName();
        this->createError(node->getToken(), std::move(str));
        return;
    }

    this->addHandler("status", this->env.getIntType(), [](Node &node, Directive &d) {
        d.setStatus(cast<NumberNode>(node).getValue());
    });

    this->addHandler("result", this->env.getStringType(), [&](Node &node, Directive &d) {
        d.setResult(this->resolveStatus(cast<StringNode>(node)));
    });

    this->addHandler("params", this->env.getArrayType(this->env.getStringType()), [](Node &node, Directive &d) {
        auto &value = cast<ArrayNode>(node);
        for(auto &e : value.getValues()) {
            d.appendParam(cast<StringNode>(*e).getValue());
        }
    });

    this->addHandler("lineNum", this->env.getIntType(), [](Node &node, Directive &d) {
        d.setLineNum(cast<NumberNode>(node).getValue());
    });

    this->addHandler("ifHaveDBus", this->env.getBooleanType(), [](Node &node, Directive &d) {
        d.setIfHaveDBus(cast<BooleanNode>(node).getValue());
    });

    this->addHandler("errorKind", this->env.getStringType(), [](Node &node, Directive &d) {
        d.setErrorKind(cast<StringNode>(node).getValue());
    });

    std::unordered_set<std::string> foundAttrSet;

    for(auto &e : node->getNodes()) {
        const std::string &attrName = e->getName();
        auto *pair = this->lookupHandler(attrName);
        if(pair == nullptr) {
            std::string str("unsupported attribute: ");
            str += attrName;
            this->createError(e->getToken(), std::move(str));
            return;
        }

        // check duplication
        auto iter = foundAttrSet.find(attrName);
        if(iter != foundAttrSet.end()) {
            std::string str("duplicated attribute: ");
            str += attrName;
            this->createError(e->getToken(), std::move(str));
            return;
        }

        // check type attribute
        TRY(this->checkType(pair->first, *e->getAttrNode()));

        // invoke handler
        (pair->second)(*e->getAttrNode(), d);
        if(this->hasError()) {
            return;
        }

        foundAttrSet.insert(attrName);
    }
}

void DirectiveInitializer::visitDirectiveNode(DirectiveNode &) {
    fatal("unsupported: visitDirectiveNode\n");
}

void DirectiveInitializer::visitAttributeNode(AttributeNode &) {
    fatal("unsupported: visitAttributeNode\n");
}

void DirectiveInitializer::visitNumberNode(NumberNode &node) {
    node.setType(this->env.getIntType());
}

void DirectiveInitializer::visitStringNode(StringNode &node) {
    node.setType(this->env.getStringType());
}

void DirectiveInitializer::visitBooleanNode(BooleanNode &node) {
    node.setType(this->env.getBooleanType());
}

void DirectiveInitializer::visitArrayNode(ArrayNode &node) {
    unsigned int size = node.getValues().size();
    auto type = TRY(this->checkType(*node.getValues()[0]));
    for(unsigned int i = 1; i < size; i++) {
        TRY(this->checkType(type, *node.getValues()[i]));
    }
    node.setType(this->env.getArrayType(type));
}


Type DirectiveInitializer::checkType(Node &node) {
    auto type = node.getType();
    if(type) {
        return type;
    }

    node.accept(*this);
    type = node.getType();

    if(!type) {
        this->createError(node.getToken(), std::string("require type, but is null"));
    }
    return type;
}

Type DirectiveInitializer::checkType(const Type &requiredType, Node &node) {
    Type type = this->checkType(node);
    if(type && *type != *requiredType) {
        std::string str("require: ");
        str += requiredType->getRealName();
        str += ", but is: ";
        str += type->getRealName();
        this->createError(node.getToken(), std::move(str));
        return nullptr;
    }
    return type;
}

void DirectiveInitializer::addHandler(const char *attributeName, const Type &type, AttributeHandler &&handler) {
    auto pair = this->handlerMap.insert(std::make_pair(attributeName, std::make_pair(type, std::move(handler))));
    if(!pair.second) {
        fatal("found duplicated handler: %s\n", attributeName);
    }
}

unsigned int DirectiveInitializer::resolveStatus(const StringNode &node) {
    const struct {
        const char *name;
        unsigned int status;
    } statusTable[] = {
#define _E(K) DS_ERROR_KIND_##K
            {"success",         _E(SUCCESS)},
            {"parse_error",     _E(PARSE_ERROR)},
            {"parse",           _E(PARSE_ERROR)},
            {"type_error",      _E(TYPE_ERROR)},
            {"type",            _E(TYPE_ERROR)},
            {"runtime_error",   _E(RUNTIME_ERROR)},
            {"runtime",         _E(RUNTIME_ERROR)},
            {"throw",           _E(RUNTIME_ERROR)},
            {"assertion_error", _E(ASSERTION_ERROR)},
            {"assert",          _E(ASSERTION_ERROR)},
            {"exit",            _E(EXIT)},
#undef _E
    };

    for(auto &e : statusTable) {
        if(strcasecmp(node.getValue().c_str(), e.name) == 0) {
            return e.status;
        }
    }

    std::vector<std::string> alters;
    for(auto &e : statusTable) {
        alters.emplace_back(e.name);
    }

    std::string message("illegal status, expect for ");
    unsigned int count = 0;
    for(auto &e : alters) {
        if(count++ > 0) {
            message += ", ";
        }
        message += e;
    }
    this->createError(node.getToken(), std::move(message));
    return 0;
}

const std::pair<Type, AttributeHandler> *DirectiveInitializer::lookupHandler(const std::string &name) const {
    auto iter = this->handlerMap.find(name);
    if(iter == this->handlerMap.end()) {
        return nullptr;
    }
    return &iter->second;
}


// #######################
// ##     Directive     ##
// #######################

bool Directive::init(const char *fileName, Directive &d) {
    std::ifstream input(fileName);
    if(!input) {
        fatal("cannot open file: %s\n", fileName);
    }
    return DirectiveParser()(fileName, input, d);
}

bool Directive::init(const char *sourceName, const char *src, Directive &d) {
    std::istringstream input(src);
    return DirectiveParser()(sourceName, input, d);
}

} // namespace directive
} // namespace ydsh
