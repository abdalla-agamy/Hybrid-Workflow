#ifndef DT_HSBP_MINI_JSON_HPP
#define DT_HSBP_MINI_JSON_HPP

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dthsbp {
namespace json {

enum class ValueType {
    Null,
    Bool,
    Number,
    String,
    Array,
    Object
};

class Value;
using ArrayStorage = std::vector<Value>;
using ObjectStorage = std::unordered_map<std::string, Value>;

class Value {
public:
    Value() : type_(ValueType::Null), boolVal_(false), numVal_(0.0) {}
    explicit Value(bool b) : type_(ValueType::Bool), boolVal_(b), numVal_(0.0) {}
    explicit Value(double d) : type_(ValueType::Number), boolVal_(false), numVal_(d) {}
    explicit Value(std::string s)
        : type_(ValueType::String), boolVal_(false), numVal_(0.0), strVal_(std::move(s)) {}
    explicit Value(std::shared_ptr<ArrayStorage> arr)
        : type_(ValueType::Array), boolVal_(false), numVal_(0.0), arrPtr_(std::move(arr)) {}
    explicit Value(std::shared_ptr<ObjectStorage> obj)
        : type_(ValueType::Object), boolVal_(false), numVal_(0.0), objPtr_(std::move(obj)) {}

    ValueType type() const { return type_; }
    bool isNull() const { return type_ == ValueType::Null; }
    bool isBool() const { return type_ == ValueType::Bool; }
    bool isNumber() const { return type_ == ValueType::Number; }
    bool isString() const { return type_ == ValueType::String; }
    bool isArray() const { return type_ == ValueType::Array; }
    bool isObject() const { return type_ == ValueType::Object; }

    bool asBool() const {
        if (!isBool()) throw std::runtime_error("JSON value is not a bool");
        return boolVal_;
    }

    double asDouble() const {
        if (!isNumber()) throw std::runtime_error("JSON value is not a number");
        return numVal_;
    }

    int64_t asInt64() const {
        if (!isNumber()) throw std::runtime_error("JSON value is not a number");
        return static_cast<int64_t>(numVal_);
    }

    int asInt() const {
        return static_cast<int>(asInt64());
    }

    const std::string& asString() const {
        if (!isString()) throw std::runtime_error("JSON value is not a string");
        return strVal_;
    }

    const ArrayStorage& asArray() const {
        if (!isArray() || !arrPtr_) throw std::runtime_error("JSON value is not an array");
        return *arrPtr_;
    }

    const ObjectStorage& asObject() const {
        if (!isObject() || !objPtr_) throw std::runtime_error("JSON value is not an object");
        return *objPtr_;
    }

    bool hasKey(const std::string& key) const {
        if (!isObject() || !objPtr_) return false;
        return objPtr_->find(key) != objPtr_->end();
    }

    const Value& at(const std::string& key) const {
        const ObjectStorage& obj = asObject();
        auto it = obj.find(key);
        if (it == obj.end()) {
            throw std::runtime_error("Missing required JSON key: " + key);
        }
        return it->second;
    }

private:
    ValueType type_;
    bool boolVal_;
    double numVal_;
    std::string strVal_;
    std::shared_ptr<ArrayStorage> arrPtr_;
    std::shared_ptr<ObjectStorage> objPtr_;
};

namespace detail {

class Parser {
public:
    explicit Parser(const std::string& input) : src_(input), pos_(0) {}

    Value parseRoot() {
        skipWs();
        Value val = parseValue();
        skipWs();
        if (pos_ != src_.size()) {
            throw errorAt("Trailing characters after JSON document");
        }
        return val;
    }

private:
    const std::string& src_;
    size_t pos_;

    std::runtime_error errorAt(const std::string& msg) const {
        std::ostringstream oss;
        oss << msg << " at byte offset " << pos_;
        return std::runtime_error(oss.str());
    }

    void skipWs() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const {
        if (pos_ >= src_.size()) throw errorAt("Unexpected end of JSON input");
        return src_[pos_];
    }

    char get() {
        char c = peek();
        ++pos_;
        return c;
    }

    Value parseValue() {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == 't') {
            expectLiteral("true");
            return Value(true);
        }
        if (c == 'f') {
            expectLiteral("false");
            return Value(false);
        }
        if (c == 'n') {
            expectLiteral("null");
            return Value();
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return Value(parseNumber());
        }
        throw errorAt(std::string("Unexpected JSON token '") + c + "'");
    }

    void expectLiteral(const char* lit) {
        for (const char* p = lit; *p != '\0'; ++p) {
            if (get() != *p) throw errorAt(std::string("Expected literal ") + lit);
        }
    }

    std::string parseString() {
        if (get() != '"') throw errorAt("Expected '\"'");
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) throw errorAt("Unterminated escape sequence");
                char esc = src_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) throw errorAt("Incomplete \\u escape");
                        unsigned int cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src_[pos_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
                            else throw errorAt("Invalid hex in \\u escape");
                        }
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default:
                        throw errorAt("Unsupported escape character");
                }
            } else {
                out.push_back(c);
            }
        }
        throw errorAt("Unterminated string");
    }

    double parseNumber() {
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            throw errorAt("Malformed JSON number");
        }
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                throw errorAt("Malformed fraction in JSON number");
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                throw errorAt("Malformed exponent in JSON number");
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        std::string slice = src_.substr(start, pos_ - start);
        char* endPtr = nullptr;
        double val = std::strtod(slice.c_str(), &endPtr);
        if (endPtr == slice.c_str()) throw errorAt("Failed converting JSON number");
        return val;
    }

    Value parseArray() {
        if (get() != '[') throw errorAt("Expected '['");
        auto arr = std::make_shared<ArrayStorage>();
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return Value(arr);
        }
        while (true) {
            arr->push_back(parseValue());
            skipWs();
            char next = get();
            if (next == ']') break;
            if (next != ',') throw errorAt("Expected ',' or ']' in array");
        }
        return Value(arr);
    }

    Value parseObject() {
        if (get() != '{') throw errorAt("Expected '{'");
        auto obj = std::make_shared<ObjectStorage>();
        skipWs();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return Value(obj);
        }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            if (get() != ':') throw errorAt("Expected ':' in object");
            Value val = parseValue();
            (*obj)[std::move(key)] = std::move(val);
            skipWs();
            char next = get();
            if (next == '}') break;
            if (next != ',') throw errorAt("Expected ',' or '}' in object");
        }
        return Value(obj);
    }
};

} // namespace detail

inline Value parseString(const std::string& content) {
    detail::Parser p(content);
    return p.parseRoot();
}

inline Value parseFile(const std::string& filePath) {
    std::ifstream ifs(filePath.c_str(), std::ios::in | std::ios::binary);
    if (!ifs.is_open()) {
        throw std::runtime_error("Cannot open JSON trace file: " + filePath);
    }
    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    return parseString(buffer.str());
}

} // namespace json
} // namespace dthsbp

#endif // DT_HSBP_MINI_JSON_HPP
