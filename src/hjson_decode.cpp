#include "hjson.h"
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>


namespace Hjson {


class CommentInfo {
public:
  bool hasComment;
  int cmStart, cmEnd;
};


class Parser {
public:
  const unsigned char *data;
  size_t dataSize;
  int at;
  unsigned char ch;
  DecoderOptions opt;
};


bool tryParseNumber(Value *pNumber, const char *text, size_t textSize, bool stopAtNext);
static Value _readValue(Parser *p);


static inline void _setComment(Value& val, void (Value::*fp)(const std::string&),
  Parser *p, const CommentInfo& ci)
{
  if (ci.hasComment) {
    (val.*fp)(std::string(p->data + ci.cmStart, p->data + ci.cmEnd));
  }
}


static inline void _setComment(Value& val, void (Value::*fp)(const std::string&),
  Parser *p, const CommentInfo& ciA, const CommentInfo& ciB)
{
  if (ciA.hasComment && ciB.hasComment) {
    (val.*fp)(std::string(p->data + ciA.cmStart, p->data + ciA.cmEnd) +
      std::string(p->data + ciB.cmStart, p->data + ciB.cmEnd));
  } else {
    _setComment(val, fp, p, ciA);
    _setComment(val, fp, p, ciB);
  }
}


// trim from start (in place)
static inline void _ltrim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
}


// trim from end (in place)
static inline void _rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), s.end());
}


// trim from both ends (copy)
static inline std::string _trim(std::string s) {
  _rtrim(s);
  _ltrim(s);
  return s;
}


static bool _next(Parser *p) {
  // get the next character.
  if (p->at < p->dataSize) {
    p->ch = p->data[p->at++];
    return true;
  }

  p->ch = 0;
  ++p->at;

  return false;
}


static bool _prev(Parser *p) {
  // get the previous character.
  if (p->at > 1) {
    p->ch = p->data[p->at-- - 2];
    return true;
  }

  return false;
}


static void _resetAt(Parser *p) {
  p->at = 0;
  _next(p);
}


static bool _isPunctuatorChar(char c) {
  return c == '{' || c == '}' || c == '[' || c == ']' || c == ',' || c == ':';
}


static std::string _errAt(Parser *p, std::string message) {
  int i, col = 0, line = 1;

  for (i = p->at - 1; i > 0 && p->data[i] != '\n'; i--) {
    col++;
  }

  for (; i > 0; i--) {
    if (p->data[i] == '\n') {
      line++;
    }
  }

  size_t samEnd = std::min((size_t)20, p->dataSize - (p->at - col));

  return message + " at line " + std::to_string(line) + "," +
    std::to_string(col) + " >>> " + std::string((char*)p->data + p->at - col, samEnd);
}


static unsigned char _peek(Parser *p, int offs) {
  int pos = p->at + offs;

  if (pos >= 0 && pos < p->dataSize) {
    return p->data[p->at + offs];
  }

  return 0;
}


static unsigned char _escapee(unsigned char c) {
  switch (c)
  {
  case '"':
  case '\'':
  case '\\':
  case '/':
    return c;
  case 'b':
    return '\b';
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  }

  return 0;
}


// Parse a multiline string value.
static std::string _readMLString(Parser *p) {
  std::vector<char> res;
  int triple = 0;

  // we are at ''' +1 - get indent
  int indent = 0;

  for (;;) {
    auto c = _peek(p, -indent - 5);
    if (c == 0 || c == '\n') {
      break;
    }
    indent++;
  }

  auto skipIndent = [&]() {
    auto skip = indent;
    while (p->ch > 0 && p->ch <= ' ' && p->ch != '\n' && skip > 0) {
      skip--;
      _next(p);
    }
  };

  // skip white/to (newline)
  while (p->ch > 0 && p->ch <= ' ' && p->ch != '\n') {
    _next(p);
  }
  if (p->ch == '\n') {
    _next(p);
    skipIndent();
  }

  // When parsing multiline string values, we must look for ' characters.
  bool lastLf = false;
  for (;;) {
    if (p->ch == 0) {
      throw syntax_error(_errAt(p, "Bad multiline string"));
    } else if (p->ch == '\'') {
      triple++;
      _next(p);
      if (triple == 3) {
        auto sres = res.data();
        if (lastLf) {
          return std::string(sres, res.size() - 1); // remove last EOL
        }
        return std::string(sres, res.size());
      }
      continue;
    } else {
      while (triple > 0) {
        res.push_back('\'');
        triple--;
        lastLf = false;
      }
    }
    if (p->ch == '\n') {
      res.push_back('\n');
      lastLf = true;
      _next(p);
      skipIndent();
    } else {
      if (p->ch != '\r') {
        res.push_back(p->ch);
        lastLf = false;
      }
      _next(p);
    }
  }
}


static void _toUtf8(std::vector<char> &res, uint32_t uIn) {
  if (uIn < 0x80) {
    res.push_back(uIn);
  } else if (uIn < 0x800) {
    res.push_back(0xc0 | ((uIn >> 6) & 0x1f));
    res.push_back(0x80 | (uIn & 0x3f));
  } else if (uIn < 0x10000) {
    res.push_back(0xe0 | ((uIn >> 12) & 0xf));
    res.push_back(0x80 | ((uIn >> 6) & 0x3f));
    res.push_back(0x80 | (uIn & 0x3f));
  } else if (uIn < 0x110000) {
    res.push_back(0xf0 | ((uIn >> 18) & 0x7));
    res.push_back(0x80 | ((uIn >> 12) & 0x3f));
    res.push_back(0x80 | ((uIn >> 6) & 0x3f));
    res.push_back(0x80 | (uIn & 0x3f));
  } else {
    throw std::logic_error("Invalid unicode code point");
  }
}


// Parse a string value.
// callers make sure that (ch === '"' || ch === "'")
// When parsing for string values, we must look for " and \ characters.
static std::string _readString(Parser *p, bool allowML) {
  std::vector<char> res;

  char exitCh = p->ch;
  while (_next(p)) {
    if (p->ch == exitCh) {
      _next(p);
      if (allowML && exitCh == '\'' && p->ch == '\'' && res.size() == 0) {
        // ''' indicates a multiline string
        _next(p);
        return _readMLString(p);
      } else {
        return std::string(res.data(), res.size());
      }
    }
    if (p->ch == '\\') {
      unsigned char ech;
      _next(p);
      if (p->ch == 'u') {
        uint32_t uffff = 0;
        for (int i = 0; i < 4; i++) {
          _next(p);
          unsigned char hex;
          if (p->ch >= '0' && p->ch <= '9') {
            hex = p->ch - '0';
          } else if (p->ch >= 'a' && p->ch <= 'f') {
            hex = p->ch - 'a' + 0xa;
          } else if (p->ch >= 'A' && p->ch <= 'F') {
            hex = p->ch - 'A' + 0xa;
          } else {
            throw syntax_error(_errAt(p, std::string("Bad \\u char ") + (char)p->ch));
          }
          uffff = uffff * 16 + hex;
        }
        _toUtf8(res, uffff);
      } else if ((ech = _escapee(p->ch))) {
        res.push_back(ech);
      } else {
        throw syntax_error(_errAt(p, std::string("Bad escape \\") + (char)p->ch));
      }
    } else if (p->ch == '\n' || p->ch == '\r') {
      throw syntax_error(_errAt(p, "Bad string containing newline"));
    } else {
      res.push_back(p->ch);
    }
  }

  throw syntax_error(_errAt(p, "Bad string"));
}


// quotes for keys are optional in Hjson
// unless they include {}[],: or whitespace.
static std::string _readKeyname(Parser *p) {
  if (p->ch == '"' || p->ch == '\'') {
    return _readString(p, false);
  }

  std::vector<char> name;
  auto start = p->at;
  int space = -1;
  for (;;) {
    if (p->ch == ':') {
      if (name.empty()) {
        throw syntax_error(_errAt(p, "Found ':' but no key name (for an empty key name use quotes)"));
      } else if (space >= 0 && space != name.size()) {
        p->at = start + space;
        throw syntax_error(_errAt(p, "Found whitespace in your key name (use quotes to include)"));
      }
      return std::string(name.data(), name.size());
    } else if (p->ch <= ' ') {
      if (p->ch == 0) {
        throw syntax_error(_errAt(p, "Found EOF while looking for a key name (check your syntax)"));
      }
      if (space < 0) {
        space = (int)name.size();
      }
    } else {
      if (_isPunctuatorChar(p->ch)) {
        throw syntax_error(_errAt(p, std::string("Found '") + (char)p->ch + std::string(
          "' where a key name was expected (check your syntax or use quotes if the key name includes {}[],: or whitespace)")));
      }
      name.push_back(p->ch);
    }
    _next(p);
  }
}


static CommentInfo _white(Parser *p) {
  CommentInfo ci = {
    false,
    p->at - 1,
    0
  };

  while (p->ch > 0) {
    // Skip whitespace.
    while (p->ch > 0 && p->ch <= ' ') {
      _next(p);
    }
    // Hjson allows comments
    if (p->ch == '#' || (p->ch == '/' && _peek(p, 0) == '/')) {
      if (p->opt.comments) {
        ci.hasComment = true;
      }
      while (p->ch > 0 && p->ch != '\n') {
        _next(p);
      }
    } else if (p->ch == '/' && _peek(p, 0) == '*') {
      if (p->opt.comments) {
        ci.hasComment = true;
      }
      _next(p);
      _next(p);
      while (p->ch > 0 && !(p->ch == '*' && _peek(p, 0) == '/')) {
        _next(p);
      }
      if (p->ch > 0) {
        _next(p);
        _next(p);
      }
    } else {
      break;
    }
  }

  ci.cmEnd = p->at - 1;

  return ci;
}


static CommentInfo _getCommentAfter(Parser *p) {
  CommentInfo ci = {
    false,
    p->at - 1,
    0
  };

  while (p->ch > 0) {
    // Skip whitespace, but only until EOL.
    while (p->ch > 0 && p->ch <= ' ' && p->ch != '\n') {
      _next(p);
    }
    // Hjson allows comments
    if (p->ch == '#' || (p->ch == '/' && _peek(p, 0) == '/')) {
      if (p->opt.comments) {
        ci.hasComment = true;
      }
      while (p->ch > 0 && p->ch != '\n') {
        _next(p);
      }
    } else if (p->ch == '/' && _peek(p, 0) == '*') {
      if (p->opt.comments) {
        ci.hasComment = true;
      }
      _next(p);
      _next(p);
      while (p->ch > 0 && !(p->ch == '*' && _peek(p, 0) == '/')) {
        _next(p);
      }
      if (p->ch > 0) {
        _next(p);
        _next(p);
      }
    } else {
      break;
    }
  }

  ci.cmEnd = p->at - 1;

  return ci;
}


// Hjson strings can be quoteless
// returns string, true, false, or null.
static Value _readTfnns(Parser *p) {
  if (_isPunctuatorChar(p->ch)) {
    throw syntax_error(_errAt(p, std::string("Found a punctuator character '") +
      (char)p->ch + std::string("' when expecting a quoteless string (check your syntax)")));
  }
  auto chf = p->ch;
  std::vector<char> value;
  value.push_back(p->ch);

  for (;;) {
    _next(p);
    bool isEol = (p->ch == '\r' || p->ch == '\n' || p->ch == 0);
    if (isEol ||
      p->ch == ',' || p->ch == '}' || p->ch == ']' ||
      p->ch == '#' ||
      (p->ch == '/' && (_peek(p, 0) == '/' || _peek(p, 0) == '*')))
    {
      auto trimmed = _trim(std::string(value.data(), value.size()));

      switch (chf) {
      case 'f':
        if (trimmed == "false") {
          return false;
        }
        break;
      case 'n':
        if (trimmed == "null") {
          return Value(Value::Type::Null);
        }
        break;
      case 't':
        if (trimmed == "true") {
          return true;
        }
        break;
      default:
        if (chf == '-' || (chf >= '0' && chf <= '9')) {
          Value number;
          if (tryParseNumber(&number, trimmed.c_str(), trimmed.size(), false)) {
            return number;
          }
        }
      }
      if (isEol) {
        return trimmed;
      }
    }
    value.push_back(p->ch);
  }
}


// Parse an array value.
// assuming ch == '['
static Value _readArray(Parser *p) {
  Value array(Hjson::Value::Type::Vector);

  // Skip '['.
  _next(p);
  auto ciBefore = _white(p);

  if (p->ch == ']') {
    _setComment(array, &Value::set_comment_inside, p, ciBefore);
    _next(p);
    return array; // empty array
  }

  CommentInfo ciExtra = {};

  while (p->ch > 0) {
    auto elem = _readValue(p);
    _setComment(elem, &Value::set_comment_before, p, ciBefore, ciExtra);
    auto ciAfter = _white(p);
    // in Hjson the comma is optional and trailing commas are allowed
    if (p->ch == ',') {
      _next(p);
      // It is unlikely that someone writes a comment after the value but
      // before the comma, so we include any such comment in "comment_after".
      ciExtra = _white(p);
    } else {
      ciExtra = {};
    }
    if (p->ch == ']') {
      auto existingAfter = elem.get_comment_after();
      _setComment(elem, &Value::set_comment_after, p, ciAfter, ciExtra);
      if (!existingAfter.empty()) {
        elem.set_comment_after(existingAfter + elem.get_comment_after());
      }
      array.push_back(elem);
      _next(p);
      return array;
    }
    array.push_back(elem);
    ciBefore = ciAfter;
  }

  throw syntax_error(_errAt(p, "End of input while parsing an array (did you forget a closing ']'?)"));
}


// Parse an object value.
static Value _readObject(Parser *p, bool withoutBraces) {
  Value object(Hjson::Value::Type::Map);

  if (!withoutBraces) {
    // assuming ch == '{'
    _next(p);
  }

  auto ciBefore = _white(p);

  if (p->ch == '}' && !withoutBraces) {
    _setComment(object, &Value::set_comment_inside, p, ciBefore);
    _next(p);
    return object; // empty object
  }

  CommentInfo ciExtra = {};

  while (p->ch > 0) {
    auto key = _readKeyname(p);
    auto ciKey = _white(p);
    if (p->ch != ':') {
      throw syntax_error(_errAt(p, std::string(
        "Expected ':' instead of '") + (char)(p->ch) + "'"));
    }
    _next(p);
    // duplicate keys overwrite the previous value
    auto elem = _readValue(p);
    _setComment(elem, &Value::set_comment_key, p, ciKey);
    if (!elem.get_comment_before().empty()) {
      elem.set_comment_key(elem.get_comment_key() +
        elem.get_comment_before());
    }
    _setComment(elem, &Value::set_comment_before, p, ciBefore, ciExtra);
    auto ciAfter = _white(p);
    // in Hjson the comma is optional and trailing commas are allowed
    if (p->ch == ',') {
      _next(p);
      // It is unlikely that someone writes a comment after the value but
      // before the comma, so we include any such comment in "comment_after".
      ciExtra = _white(p);
    } else {
      ciExtra = {};
    }
    if (p->ch == '}' && !withoutBraces) {
      _setComment(elem, &Value::set_comment_after, p, ciAfter, ciExtra);
      object[key].assign_with_comments(elem);
      _next(p);
      return object;
    }
    object[key].assign_with_comments(elem);
    ciBefore = ciAfter;
  }

  if (withoutBraces) {
    if (object.empty()) {
      _setComment(object, &Value::set_comment_inside, p, ciBefore);
    } else {
      auto elem = object[object.size() - 1];
      _setComment(elem, &Value::set_comment_after, p, ciBefore, ciExtra);
      object[object.size() - 1].assign_with_comments(elem);
    }

    return object;
  }
  throw syntax_error(_errAt(p, "End of input while parsing an object (did you forget a closing '}'?)"));
}


// Parse a Hjson value. It could be an object, an array, a string, a number or a word.
static Value _readValue(Parser *p) {
  Hjson::Value ret;

  auto ciBefore = _white(p);

  switch (p->ch) {
  case '{':
    ret = _readObject(p, false);
    break;
  case '[':
    ret = _readArray(p);
    break;
  case '"':
  case '\'':
    ret = _readString(p, true);
    break;
  default:
    ret = _readTfnns(p);
    // Make sure that any comment will include preceding whitespace.
    if (p->ch == '#' || p->ch == '/') {
      while (_prev(p) && std::isspace(p->ch)) {}
      _next(p);
    }
    break;
  }

  auto ciAfter = _getCommentAfter(p);

  _setComment(ret, &Value::set_comment_before, p, ciBefore);
  _setComment(ret, &Value::set_comment_after, p, ciAfter);

  return ret;
}


static Value _hasTrailing(Parser *p, CommentInfo *ci) {
  *ci = _white(p);
  return p->ch > 0;
}


// Braces for the root object are optional
static Value _rootValue(Parser *p) {
  Value ret;
  std::string errMsg;
  CommentInfo ciExtra;

  auto ciBefore = _white(p);

  switch (p->ch) {
  case '{':
    ret = _readObject(p, false);
    if (_hasTrailing(p, &ciExtra)) {
      throw syntax_error(_errAt(p, "Syntax error, found trailing characters"));
    }
    break;
  case '[':
    ret = _readArray(p);
    if (_hasTrailing(p, &ciExtra)) {
      throw syntax_error(_errAt(p, "Syntax error, found trailing characters"));
    }
    break;
  }

  if (!ret.defined()) {
    // assume we have a root object without braces
    try {
      ret = _readObject(p, true);
      if (_hasTrailing(p, &ciExtra)) {
        // Syntax error, or maybe a single JSON value.
        ret = Value();
      }
    } catch(syntax_error e) {
      errMsg = std::string(e.what());
    }
  }

  if (!ret.defined()) {
    // test if we are dealing with a single JSON value instead (true/false/null/num/"")
    _resetAt(p);
    ret = _readValue(p);
    if (_hasTrailing(p, &ciExtra)) {
      // Syntax error.
      ret = Value();
    }
  }

  if (ret.defined()) {
    _setComment(ret, &Value::set_comment_before, p, ciBefore);
    auto existingAfter = ret.get_comment_after();
    _setComment(ret, &Value::set_comment_after, p, ciExtra);
    if (!existingAfter.empty()) {
      ret.set_comment_after(existingAfter + ret.get_comment_after());
    }
    return ret;
  }

  if (!errMsg.empty()) {
    throw syntax_error(errMsg);
  }

  throw syntax_error(_errAt(p, "Syntax error, found trailing characters"));
}


// Unmarshal parses the Hjson-encoded data and returns a tree of Values.
//
// Unmarshal uses the inverse of the encodings that Marshal uses.
//
Value Unmarshal(const char *data, size_t dataSize, DecoderOptions options) {
  Parser parser = {
    (const unsigned char*) data,
    dataSize,
    0,
    ' ',
    options
  };

  _resetAt(&parser);
  return _rootValue(&parser);
}


Value Unmarshal(const char *data, DecoderOptions options) {
  if (!data) {
    return Value();
  }

  return Unmarshal(data, std::strlen(data), options);
}


Value Unmarshal(const std::string &data, DecoderOptions options) {
  return Unmarshal(data.c_str(), data.size(), options);
}


Value UnmarshalFromFile(const std::string &path, DecoderOptions options) {
  std::ifstream infile(path, std::ifstream::ate | std::ifstream::binary);
  if (!infile.is_open()) {
    throw file_error("Could not open file '" + path + "' for reading");
  }
  std::string inStr;
  inStr.resize(infile.tellg());
  infile.seekg(0, std::ios::beg);
  infile.read(&inStr[0], inStr.size());
  infile.close();

  return Unmarshal(inStr, options);
}


}
