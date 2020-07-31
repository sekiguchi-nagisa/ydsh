#include "gtest/gtest.h"

#include "jsonrpc.h"

using namespace ydsh;
using namespace json;

TEST(JSON, type) {
    JSON json;
    ASSERT_TRUE(json.isInvalid());

    json = nullptr;
    ASSERT_TRUE(json.isNull());
    ASSERT_TRUE(ydsh::get<std::nullptr_t>(json) == nullptr);

    json = true;
    ASSERT_TRUE(json.isBool());
    ASSERT_EQ(true, json.asBool());

    json = JSON(12);
    ASSERT_TRUE(json.isLong());
    ASSERT_EQ(12, json.asLong());

    json = JSON(3.14);
    ASSERT_TRUE(json.isDouble());
    ASSERT_EQ(3.14, json.asDouble());

    json = "hello";
    ASSERT_TRUE(json.isString());
    ASSERT_EQ("hello", json.asString());

    json = JSON(array());
    ASSERT_TRUE(json.isArray());
    json.asArray().emplace_back(23);
    json.asArray().emplace_back(true);
    ASSERT_EQ(2, json.size());
    ASSERT_EQ(23, json[0].asLong());
    ASSERT_TRUE(json[1].asBool());

    json = array(
           23, 4.3, "hello", false, nullptr, array(34, 34), std::move(json)
    );
    ASSERT_TRUE(json.isArray());
    ASSERT_EQ(7, json.size());
    ASSERT_EQ(23, json[0].asLong());
    ASSERT_EQ(4.3, json[1].asDouble());
    ASSERT_EQ("hello", json[2].asString());
    ASSERT_EQ(false, json[3].asBool());
    ASSERT_TRUE(json[4].isNull());
    ASSERT_EQ(34, json[5][0].asLong());
    ASSERT_EQ(34, json[5][1].asLong());
    ASSERT_EQ(23, json[6][0].asLong());
    ASSERT_EQ(true, json[6][1].asBool());

    json = JSON(object());
    ASSERT_TRUE(json.isObject());
    json.asObject().emplace("hey", false);
    ASSERT_FALSE(json["hey"].asBool());

    json = {
            {"hello", 45}, {"world", array("false", true)}
    };
    ASSERT_TRUE(json.isObject());
    ASSERT_EQ(2, json.size());
    ASSERT_EQ(45, json["hello"].asLong());
    ASSERT_EQ("false", json["world"][0].asString());
    ASSERT_TRUE(json["world"][1].asBool());

    // Optional<JSON>
    ASSERT_EQ(sizeof(JSON), sizeof(Optional<JSON>));
    Optional<JSON> opt;
    ASSERT_TRUE(opt.isInvalid());
    opt = JSON(23);
    ASSERT_TRUE(opt.isLong());
}

TEST(JSON, serialize) {
    JSON json = {
            {"hello", false},
            {"hoge", array(2, nullptr, true)}
    };
    auto actual = json.serialize();

    const char *expect = R"({"hello":false,"hoge":[2,null,true]})";
    ASSERT_EQ(expect, actual);

    json = 34;
    actual = json.serialize(1);
    ASSERT_EQ("34\n", actual);

    json = {
            {"23",array(JSON())},
            {"hey", array(3, -123, nullptr, object({"3",""}))}
    };
    actual = json.serialize(4);

    expect = R"({
    "23": [],
    "hey": [
        3,
        -123,
        null,
        {
            "3": ""
        }
    ]
}
)";
    ASSERT_EQ(expect, actual);
}

class ParserTest : public ::testing::Test {
protected:
    JSON ret;

    void parse(const char *src) {
        Parser parser(src);
        this->ret = parser();
        if(parser.hasError()) {
            parser.showError();
        }
        ASSERT_FALSE(parser.hasError());
    }

public:
    ParserTest() = default;
};

TEST(InvalidTest, base) {
    Parser parser("hoge");
    auto ret = parser();
    ASSERT_TRUE(parser.hasError());
    ASSERT_TRUE(ret.isInvalid());
}

TEST_F(ParserTest, null1) {
    ASSERT_NO_FATAL_FAILURE(this->parse("null"));
    ASSERT_TRUE(this->ret.isNull());
}

TEST_F(ParserTest, null2) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t\n\tnull  \t   \n\n  \n"));
    ASSERT_TRUE(this->ret.isNull());
}

TEST_F(ParserTest, bool1) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t\n\t false  \t   \n\n  \n"));
    ASSERT_TRUE(this->ret.isBool());
    ASSERT_FALSE(this->ret.asBool());
}

TEST_F(ParserTest, bool2) {
    ASSERT_NO_FATAL_FAILURE(this->parse("true"));
    ASSERT_TRUE(this->ret.isBool());
    ASSERT_TRUE(this->ret.asBool());
}

TEST_F(ParserTest, num1) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t\n\t9876  \t   \n\n  \n"));
    ASSERT_TRUE(this->ret.isLong());
    ASSERT_EQ(9876, this->ret.asLong());
}

TEST_F(ParserTest, num2) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t\n\t-1203  \t   \n\n  \n"));
    ASSERT_TRUE(this->ret.isLong());
    ASSERT_EQ(-1203, this->ret.asLong());
}

TEST_F(ParserTest, num3) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t\n\t-1203e+2  \t   \n\n  \n"));
    ASSERT_TRUE(this->ret.isDouble());
    ASSERT_EQ(-120300, this->ret.asDouble());
}

TEST_F(ParserTest, num4) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t\n\t12.03E-2  \t   \n\n  \n"));
    ASSERT_TRUE(this->ret.isDouble());
    ASSERT_EQ(0.1203, this->ret.asDouble());
}

TEST_F(ParserTest, num5) {
    ASSERT_NO_FATAL_FAILURE(this->parse("\t 0 \n"));
    ASSERT_TRUE(this->ret.isLong());
    ASSERT_EQ(0, this->ret.asLong());
}

TEST_F(ParserTest, string1) {
    const char *text = R"("hello")";
    ASSERT_NO_FATAL_FAILURE(this->parse(text));
    ASSERT_TRUE(this->ret.isString());
    ASSERT_EQ("hello", this->ret.asString());
}

template <unsigned int N>
static std::string str(const char (&v)[N]) {
    return std::string(v, N - 1);
}

TEST_F(ParserTest, string2) {
    const char *text = R"("hello\n\t\b\f\r \u0023\u0000\ud867\uDE3d\u0026")";
    ASSERT_NO_FATAL_FAILURE(this->parse(text));
    ASSERT_TRUE(this->ret.isString());

    ASSERT_NO_FATAL_FAILURE(
            ASSERT_EQ(str("hello\n\t\b\f\r #\0\xf0\xa9\xb8\xbd&"), this->ret.asString()));
}

TEST_F(ParserTest, string3) {
    const char *text = R"("\"\\\/")";
    ASSERT_NO_FATAL_FAILURE(this->parse(text));

    const char *expect = R"("\/)";
    ASSERT_EQ(expect, this->ret.asString());
}

TEST_F(ParserTest, array) {
    const char *text = R"(
        [
           "a" , "b", true
               , null,
              [45
                ,
               54
             ], 3.14
        ])";
    ASSERT_NO_FATAL_FAILURE(this->parse(text));
    ASSERT_TRUE(this->ret.isArray());
    ASSERT_EQ(6, this->ret.size());
    ASSERT_EQ("a", this->ret[0].asString());
    ASSERT_EQ("b", this->ret[1].asString());
    ASSERT_EQ(true, this->ret[2].asBool());
    ASSERT_EQ(true, this->ret[3].isNull());
    ASSERT_EQ(45, this->ret[4][0].asLong());
    ASSERT_EQ(54, this->ret[4][1].asLong());
    ASSERT_EQ(3.14, this->ret[5].asDouble());
}

TEST_F(ParserTest, object) {
    const char *text = R"(
     { "23" :
         [true, false, null], "aaa"
          : null
          ,
           "bbb" : {
                   }
           ,
              "ccc" :[]
        }
)";
    ASSERT_NO_FATAL_FAILURE(this->parse(text));
    ASSERT_TRUE(this->ret.isObject());
    ASSERT_EQ(4, this->ret.size());
    ASSERT_EQ(true, this->ret["23"][0].asBool());
    ASSERT_EQ(false, this->ret["23"][1].asBool());
    ASSERT_EQ(true, this->ret["23"][2].isNull());
    ASSERT_EQ(true, this->ret["aaa"].isNull());
    ASSERT_EQ(true, this->ret["bbb"].asObject().empty());
    ASSERT_EQ(true, this->ret["ccc"].asArray().empty());
    ASSERT_TRUE(this->ret["hohgeo"].isInvalid());
}

TEST_F(ParserTest, serialize1) {
    auto expect = JSON {
            {"hiofr", JSON()},
            {"34", object()},
            {"aa", false},
            {"bb", array(nullptr, 3, 3.4, "hey", object({"huga", array()}), JSON())},
            {"ZZ", {
                           {";;;", 234}, {"", object()}
            }}
    }.serialize(3);

    auto actual = Parser(expect.c_str(), expect.size())().serialize(3);

    ASSERT_EQ(expect, actual);

    expect = JSON(34).serialize(3);
    actual = Parser(expect.c_str(), expect.size())().serialize(3);

    ASSERT_EQ(expect, actual);
}

TEST_F(ParserTest, serialize2) {
    // error
    auto actual = toJSON(rpc::Error(-1, "hello")).serialize(3);
    const char *text = R"( { "code" : -1, "message": "hello" })";
    auto expect = Parser(text)().serialize(3);
    ASSERT_EQ(expect, actual);

    actual = toJSON(rpc::Error(-100, "world", array(1, 3, 5))).serialize(3);
    text = R"( { "code" : -100, "message": "world", "data": [1,3,5] })";
    expect = Parser(text)().serialize(3);
    ASSERT_EQ(expect, actual);

    // response
    actual = toJSON(rpc::Response(34, nullptr)).serialize(2);
    text = R"( { "jsonrpc" : "2.0", "id" : 34, "result" : null } )";
    expect = Parser(text)().serialize(2);
    ASSERT_EQ(expect, actual);

    actual = toJSON(rpc::Response(-0.4, 34)).serialize(2);
    text = R"( { "jsonrpc" : "2.0", "id" : -0.4, "result" : 34 } )";
    expect = Parser(text)().serialize(2);
    ASSERT_EQ(expect, actual);

    actual = toJSON(rpc::Response(nullptr, rpc::Error(-100, "world", array(1, 3, 5)))).serialize(2);
    text = R"( { "jsonrpc" : "2.0", "id" : null,
                "error" : { "code": -100, "message" : "world", "data" : [1,3,5]}})";
    expect = Parser(text)().serialize(2);
    ASSERT_EQ(expect, actual);
}


class ValidatorTest : public ::testing::Test {
protected:
    Validator validator;
    bool ret{false};

    void tryValidate(const InterfaceWrapper &wrapper, const char *src) {
        Parser parser(src);
        auto json = parser();
        if(parser.hasError()) {
            parser.showError();
        }
        ASSERT_FALSE(parser.hasError());
        this->ret = wrapper(this->validator, json);
    }

    void validate(const InterfaceWrapper &wrapper, const char *src) {
        this->tryValidate(wrapper, src);
        if(!this->ret) {
            auto message = this->validator.formatError();
            fprintf(stderr, "%s\n", message.c_str());
        }
        ASSERT_TRUE(this->ret);
        ASSERT_EQ("", this->validator.formatError());
    }
};

TEST_F(ValidatorTest, base) {
    constexpr auto iface = createInterface(
            "hoge1",
            field("params", null | array(string)),
            field("id", opt(number))
            );

    const char *text = R"(
    { "params" : [ "hoge", "de" ] }
)";
    ASSERT_NO_FATAL_FAILURE(this->validate(iface, text));

    text = R"(
    {
        "params" : null, "id" : 45
    }
)";
    ASSERT_NO_FATAL_FAILURE(this->validate(iface, text));
}

TEST_F(ValidatorTest, iface) {
    static constexpr auto AAA = createInterface(
            "AAA",
            field("a", number),
            field("b", string),
            field("c", array(any))
            );

    static constexpr auto BBB = createInterface(
            "BBB",
            field("a", object(AAA) | string | any),
            field("b", boolean)
            );

    const char *text = R"(
        {
            "a" : { "a" : 34, "b" : "dewrfw", "c" : null },
            "b" : false
        }
)";
    ASSERT_NO_FATAL_FAILURE(this->validate(BBB, text));
    ASSERT_NO_FATAL_FAILURE(this->validate(voidIface, "null"));
}

TEST(ReqTest, parse) {
    using namespace rpc;

    // syntax error
    ByteBuffer buf;
    std::string text = "}{";
    buf.append(text.c_str(), text.size());
    auto msg = MessageParser(std::move(buf))();
    ASSERT_TRUE(is<Error>(msg));
    ASSERT_EQ(rpc::ParseError, get<Error>(msg).code);

    // semantic error
    text = R"({ "hoge" : "de" })";
    buf = ByteBuffer();
    buf.append(text.c_str(), text.size());
    msg = MessageParser(std::move(buf))();
    ASSERT_TRUE(is<Error>(msg));
    ASSERT_EQ(rpc::InvalidRequest, get<Error>(msg).code);

    // request
    text = rpc::Request("AAA", "hey", array(false, true)).toJSON().serialize(0);
    buf = ByteBuffer();
    buf.append(text.c_str(), text.size());
    msg = MessageParser(std::move(buf))();
    ASSERT_TRUE(is<Request>(msg));
    ASSERT_TRUE(get<Request>(msg).isCall());
    auto json = get<Request>(msg).toJSON();
    ASSERT_EQ("AAA", json["id"].asString());
    ASSERT_EQ("hey", json["method"].asString());
    ASSERT_EQ(2, json["params"].size());
    ASSERT_EQ(false, json["params"][0].asBool());
    ASSERT_EQ(true, json["params"][1].asBool());

    text = rpc::Request(1234, "hoge", JSON()).toJSON().serialize(0);
    buf = ByteBuffer();
    buf.append(text.c_str(), text.size());
    msg = MessageParser(std::move(buf))();
    ASSERT_TRUE(is<Request>(msg));
    ASSERT_TRUE(get<Request>(msg).isCall());
    json = get<Request>(msg).toJSON();
    ASSERT_EQ(1234, json["id"].asLong());
    ASSERT_EQ("hoge", json["method"].asString());
    ASSERT_TRUE(json["params"].isInvalid());

    // notification
    text = rpc::Request(JSON(), "world", {{"AAA", 0.23}}).toJSON().serialize(0);
    buf = ByteBuffer();
    buf.append(text.c_str(), text.size());
    msg = MessageParser(std::move(buf))();
    ASSERT_TRUE(is<Request>(msg));
    ASSERT_TRUE(get<Request>(msg).isNotification());
    json = get<Request>(msg).toJSON();
    ASSERT_TRUE(json["id"].isInvalid());
    ASSERT_EQ("world", json["method"].asString());
    ASSERT_EQ(1, json["params"].size());
    ASSERT_EQ(0.23, json["params"]["AAA"].asDouble());

    // invalid request
    text = rpc::Request(true, "world", {{"AAA", 0.23}}).toJSON().serialize(0);
    buf = ByteBuffer();
    buf.append(text.c_str(), text.size());
    msg = MessageParser(std::move(buf))();
    ASSERT_TRUE(is<Error>(msg));
}

struct SingleNullLogger : ydsh::SingletonLogger<SingleNullLogger> {
    SingleNullLogger() : ydsh::SingletonLogger<SingleNullLogger>("") {}
};

struct StringTransport : public rpc::Transport {
    std::string inStr;
    unsigned int cursor{0};
    std::string outStr;

    StringTransport() : rpc::Transport(SingleNullLogger::instance()) {}

    explicit StringTransport(std::string &&text) : rpc::Transport(SingleNullLogger::instance()), inStr(std::move(text)) {}

    int send(unsigned int size, const char *data) override {
        this->outStr.append(data, size);
        return size;
    }

    int recv(unsigned int size, char *data) override {
        unsigned int count = 0;
        for(; this->cursor < this->inStr.size() && count < size; this->cursor++) {
            data[count] = this->inStr[this->cursor];
            count++;
        }
        return count;
    }

    int recvSize() override {
        return this->inStr.size();
    }
};

class RPCTest : public ::testing::Test {
protected:
    StringTransport transport;
    rpc::Handler handler;

    RPCTest() : handler(SingleNullLogger::instance()) {}

    void init(rpc::Request &&req) {
        this->init(req.toJSON().serialize());
    }

    void init(std::string &&text) {
        this->transport = StringTransport(std::move(text));
        ASSERT_EQ(0, this->transport.cursor);
    }

    void dispatch() {
        this->transport.dispatch(this->handler);
    }

    const std::string &response() const {
        return this->transport.outStr;
    }

    void assertResponse(rpc::Response &&res) {
        ASSERT_EQ(toJSON(res).serialize(), this->response());
    }

    void parseResponse(JSON &value) {
        Parser parser(this->response().c_str());
        auto ret = parser();
        ASSERT_FALSE(parser.hasError());
        ASSERT_FALSE(ret.isInvalid());
        ASSERT_TRUE(ret.isObject());
        value = std::move(ret);
    }
};

TEST_F(RPCTest, api1) {
    this->transport.call(34, "hoge", {{"value", false}});
    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_EQ("2.0", json["jsonrpc"].asString());
    ASSERT_EQ("hoge", json["method"].asString());
    ASSERT_EQ(34, json["id"].asLong());
    ASSERT_EQ(1, json["params"].size());
    ASSERT_EQ(false, json["params"]["value"].asBool());
}

TEST_F(RPCTest, api2) {
    this->transport.notify("hoge", {{"value", "hey"}});
    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_EQ("2.0", json["jsonrpc"].asString());
    ASSERT_EQ("hoge", json["method"].asString());
    ASSERT_TRUE(json["id"].isInvalid());
    ASSERT_EQ(1, json["params"].size());
    ASSERT_EQ("hey", json["params"]["value"].asString());
}

struct Param1 {
    unsigned int value;
};

static void fromJSON(JSON &&j, Param1 &p) {
    p.value = j["value"].asLong();
}

struct Param2 {
    std::string value;
};

static void fromJSON(JSON &&j, Param2 &p) {
    p.value = std::move(j["value"].asString());
}

struct Context {
    unsigned int nRet{0};
    std::string cRet;
    bool exited{false};
    std::string calledName;

    void init(const Param1 &p) {
        this->calledName = "init";
        this->nRet = p.value;
    }

    rpc::Reply<std::string> put(const Param2 &p) {
        this->calledName = "put";
        this->cRet = p.value;
        if(this->cRet.size() > 5) {
            return rpc::newError(rpc::InternalError, "too long");
        }
        return std::string("hello");
    }

    void exit() {
        this->calledName = "exit";
        this->exited = true;
    }

    rpc::Reply<void> tryExit() {
        this->calledName = "tryExit";
        this->exited = false;
        return rpc::newError(rpc::InternalError, "busy");
    }
};

TEST_F(RPCTest, parse1) {
    this->init("hoger hiur!!");
    this->dispatch();

    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_TRUE(json.asObject().find("id") != json.asObject().end());
    ASSERT_TRUE(json["id"].isNull());
    ASSERT_TRUE(json.asObject().find("error") != json.asObject().end());
    ASSERT_EQ(rpc::ParseError, json["error"]["code"].asLong());
    ASSERT_EQ("Parse error", json["error"]["message"].asString());
    ASSERT_TRUE(json["error"]["data"].isString());
}

TEST_F(RPCTest, parse2) {
    this->init("false");
    this->dispatch();

    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_TRUE(json.asObject().find("id") != json.asObject().end());
    ASSERT_TRUE(json["id"].isNull());
    ASSERT_TRUE(json.asObject().find("error") != json.asObject().end());
    ASSERT_EQ(rpc::InvalidRequest, json["error"]["code"].asLong());
    ASSERT_EQ("Invalid Request", json["error"]["message"].asString());
    ASSERT_TRUE(json["error"]["data"].isString());
}

#define EACH_Param1_FIELD(T, OP) \
    OP(T, value)

#define EACH_Param2_FIELD(T, OP) \
    OP(T, value)

namespace ydsh {
namespace json {

DEFINE_JSON_VALIDATE_INTERFACE(Param1);
DEFINE_JSON_VALIDATE_INTERFACE(Param2);

} // namespace json
} // namespace ydsh

TEST_F(RPCTest, call1) {
    Context ctx;

    this->init(rpc::Request(1, "/put", {{"value", "hello"}}));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);

    this->dispatch();
    ASSERT_EQ("put", ctx.calledName);
    ASSERT_EQ("hello", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_FALSE(ctx.exited);
    ASSERT_NO_FATAL_FAILURE(this->assertResponse(rpc::Response(1, "hello")));
}

TEST_F(RPCTest, call2) {
    Context ctx;

    this->init(rpc::Request(1, "/putdd", {{"value", "hello"}}));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);

    this->dispatch();
    ASSERT_EQ("", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_FALSE(ctx.exited);

    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_EQ(1, json["id"].asLong());
    ASSERT_TRUE(json.asObject().find("error") != json.asObject().end());
    ASSERT_EQ(rpc::MethodNotFound, json["error"]["code"].asLong());
    ASSERT_EQ("undefined method: /putdd", json["error"]["message"].asString());
    ASSERT_TRUE(json["error"]["data"].isInvalid());
}

TEST_F(RPCTest, call3) {
    Context ctx;

    this->init(rpc::Request(1, "/put", {{"value", array(34,43)}}));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);

    this->dispatch();
    ASSERT_EQ("", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_FALSE(ctx.exited);

    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_EQ(1, json["id"].asLong());
    ASSERT_TRUE(json.asObject().find("error") != json.asObject().end());
    ASSERT_EQ(rpc::InvalidParams, json["error"]["code"].asLong());
    ASSERT_TRUE(json["error"]["message"].isString());
    ASSERT_TRUE(json["error"]["data"].isInvalid());
}

TEST_F(RPCTest, call4) {
    Context ctx;

    this->init(rpc::Request(1, "/tryExit", nullptr));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);
    this->handler.bind("/tryExit", &ctx, &Context::tryExit);

    this->dispatch();
    ASSERT_EQ("tryExit", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_FALSE(ctx.exited);

    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_EQ(1, json["id"].asLong());
    ASSERT_TRUE(json.asObject().find("error") != json.asObject().end());
    ASSERT_EQ(rpc::InternalError, json["error"]["code"].asLong());
    ASSERT_EQ("busy", json["error"]["message"].asString());
    ASSERT_TRUE(json["error"]["data"].isInvalid());
}

TEST_F(RPCTest, call5) {
    Context ctx;

    this->init(rpc::Request(1, "/tryExit", {{"de",45}}));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);
    this->handler.bind("/tryExit", &ctx, &Context::tryExit);

    this->dispatch();
    ASSERT_EQ("", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_FALSE(ctx.exited);

    JSON json;
    ASSERT_NO_FATAL_FAILURE(this->parseResponse(json));
    ASSERT_EQ(1, json["id"].asLong());
    ASSERT_TRUE(json.asObject().find("error") != json.asObject().end());
    ASSERT_EQ(rpc::InvalidParams, json["error"]["code"].asLong());
    ASSERT_EQ("requires `void' type", json["error"]["message"].asString());
    ASSERT_TRUE(json["error"]["data"].isInvalid());
}


TEST_F(RPCTest, notify1) {
    Context ctx;

    this->init(rpc::Request("/init", {{"value", 1234}}));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);
    this->handler.bind("/tryExit", &ctx, &Context::tryExit);

    this->dispatch();
    ASSERT_EQ("init", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(1234, ctx.nRet);
    ASSERT_FALSE(ctx.exited);
    ASSERT_EQ("", this->response());
}

TEST_F(RPCTest, notify2) {
    Context ctx;

    this->init(rpc::Request("/inited", {{"value", 1234}}));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);
    this->handler.bind("/tryExit", &ctx, &Context::tryExit);

    this->dispatch();
    ASSERT_EQ("", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_FALSE(ctx.exited);
    ASSERT_EQ("", this->response());
}

TEST_F(RPCTest, notify3) {
    Context ctx;

    this->init(rpc::Request("/exit", nullptr));
    this->handler.bind("/init", &ctx, &Context::init);
    this->handler.bind("/put", &ctx, &Context::put);
    this->handler.bind("/exit", &ctx, &Context::exit);
    this->handler.bind("/tryExit", &ctx, &Context::tryExit);

    this->dispatch();
    ASSERT_EQ("exit", ctx.calledName);
    ASSERT_EQ("", ctx.cRet);
    ASSERT_EQ(0, ctx.nRet);
    ASSERT_TRUE(ctx.exited);
    ASSERT_EQ("", this->response());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}