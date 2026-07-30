#include <catch2/catch_test_macros.hpp>
#include <string>

#include "sql/lexer.h"

using cardinal::lex;
using cardinal::Token;
using cardinal::TokenKind;

namespace {

// One token per word: keywords and symbols by name, others as kind:text.
std::string show(const std::string& sql) {
    std::string out;
    for (const Token& t : lex(sql)) {
        if (t.kind == TokenKind::End) break;
        if (!out.empty()) out += ' ';
        switch (t.kind) {
            case TokenKind::Identifier:
            case TokenKind::Integer:
            case TokenKind::Decimal:
            case TokenKind::String:
            case TokenKind::Error:
                out += std::string(cardinal::token_kind_name(t.kind)) + ":" + t.text;
                break;
            default:
                out += cardinal::token_kind_name(t.kind);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("lexer: table of input and tokens") {
    struct Case {
        const char* sql;
        const char* tokens;
    };
    const Case cases[] = {
        {"", ""},
        {"   \n\t ", ""},
        {"SELECT * FROM users", "SELECT * FROM identifier:users"},
        {"select Name from Users", "SELECT identifier:Name FROM identifier:Users"},
        {"SeLeCt", "SELECT"},
        {"42 007 3.14 0.5", "integer:42 integer:007 decimal:3.14 decimal:0.5"},
        {"'hello' '' 'it''s' 'a b'", "string:hello string: string:it's string:a b"},
        {"+ - * / %", "+ - * / %"},
        {"= <> != < <= > >=", "= <> <> < <= > >="},
        {"( ) , . ;", "( ) , . ;"},
        {"t.a", "identifier:t . identifier:a"},
        {"1.a", "integer:1 . identifier:a"},
        {"a<=b", "identifier:a <= identifier:b"},
        {"x_1 _y", "identifier:x_1 identifier:_y"},
        {"selection", "identifier:selection"},
        {"CREATE TABLE t (a INT, b DOUBLE, c TEXT, d BOOL)",
         "CREATE TABLE identifier:t ( identifier:a INT , identifier:b DOUBLE , identifier:c TEXT , "
         "identifier:d BOOL )"},
        {"INSERT INTO t VALUES (1, NULL, TRUE, false)",
         "INSERT INTO identifier:t VALUES ( integer:1 , NULL , TRUE , FALSE )"},
        {"SELECT a FROM t WHERE a BETWEEN 1 AND 5 OR NOT a IN (7) AND b IS NOT NULL "
         "ORDER BY a DESC, b ASC LIMIT 10;",
         "SELECT identifier:a FROM identifier:t WHERE identifier:a BETWEEN integer:1 AND integer:5 "
         "OR NOT identifier:a IN ( integer:7 ) AND identifier:b IS NOT NULL ORDER BY "
         "identifier:a DESC , identifier:b ASC LIMIT integer:10 ;"},
        {"EXPLAIN SELECT 1", "EXPLAIN SELECT integer:1"},
    };
    for (const Case& c : cases) {
        INFO(c.sql);
        REQUIRE(show(c.sql) == c.tokens);
    }
}

TEST_CASE("lexer: keeps identifier case") {
    auto tokens = lex("Users");
    REQUIRE(tokens[0].text == "Users");
}

TEST_CASE("lexer: line and column") {
    auto tokens = lex("SELECT a\n  FROM t");
    REQUIRE(tokens.size() == 5);  // SELECT a FROM t End
    REQUIRE((tokens[0].line == 1 && tokens[0].column == 1));
    REQUIRE((tokens[1].line == 1 && tokens[1].column == 8));
    REQUIRE((tokens[2].line == 2 && tokens[2].column == 3));
    REQUIRE((tokens[3].line == 2 && tokens[3].column == 8));
    REQUIRE((tokens[4].kind == TokenKind::End && tokens[4].line == 2 && tokens[4].column == 9));
}

TEST_CASE("lexer: unterminated string") {
    auto tokens = lex("SELECT 'oops");
    REQUIRE(tokens.size() == 3);
    REQUIRE(tokens[1].kind == TokenKind::Error);
    REQUIRE(tokens[1].text == "unterminated string");
    // Points at the opening quote.
    REQUIRE((tokens[1].line == 1 && tokens[1].column == 8));
    REQUIRE(tokens[2].kind == TokenKind::End);
}

TEST_CASE("lexer: unexpected character stops lexing") {
    auto tokens = lex("a @ b");
    REQUIRE(tokens.size() == 3);
    REQUIRE(tokens[1].kind == TokenKind::Error);
    REQUIRE(tokens[1].text == "unexpected character '@'");
    REQUIRE(tokens[1].column == 3);
    REQUIRE(show("a ! b") == "identifier:a error:unexpected character '!'");
}
