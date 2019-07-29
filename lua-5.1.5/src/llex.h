/*
** $Id: llex.h,v 1.58.1.1 2007/12/27 13:02:25 roberto Exp $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include "lobject.h"
#include "lzio.h"


#define FIRST_RESERVED	257

/* maximum length of a reserved word */
#define TOKEN_LEN	(sizeof("function")/sizeof(char))


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*
* * ORDER RESERVED *
* const char *const luaX_tokens [] = {
*     "and", "break", "do", "else", "elseif",
*     "end", "false", "for", "function", "if",
*     "in", "local", "nil", "not", "or", "repeat",
*     "return", "then", "true", "until", "while",
*     "..", "...", "==", ">=", "<=", "~=",
*     "<number>", "<name>", "<string>", "<eof>",
*     NULL
* };
*/
/*
** 可以假设'and', 'break'...等字符串是保留字符串，
** 'and'的reserved值等于1，'break'的reserved值等于2，后面以此类推。
*/
enum RESERVED {
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_NUMBER, TK_NAME, TK_STRING, TK_EOS
};

/* number of reserved words */
#define NUM_RESERVED	(cast(int, TK_WHILE-FIRST_RESERVED+1))


/* array with token `names' */
LUAI_DATA const char *const luaX_tokens [];


typedef union {
  lua_Number r;
  TString *ts;
} SemInfo;  /* semantics information */


typedef struct Token {
  int token;
  SemInfo seminfo;
} Token;

/*
** typedef struct Mbuffer {
**  char *buffer;
**  size_t n;
**  size_t buffsize;
** } Mbuffer;
**
** typedef struct Zio ZIO;
** struct Zio {
**  size_t n;     /* bytes still unread *
**  const char *p;    /* current position in buffer *
**  lua_Reader reader;
**  void* data;     /* additional data *
**  lua_State *L;     /* Lua state (for reader) *
** };
*/
typedef struct LexState {
  int current;  /* 当前的字符 current character (charint) */
  int linenumber;  /* 文件行数 input line counter */
  int lastline;  /* 上一个被consume的字符所在的行数 line of last token `consumed' */
  Token t;  /* 当前的token current token */
  Token lookahead;  /* 预取的token look ahead token */
  struct FuncState *fs;  /* 总是指向当前分析的FuncState `FuncState' is private to the parser */
  struct lua_State *L;
  ZIO *z;  /* input stream */
  Mbuffer *buff;  /* buffer for tokens */
  TString *source;  /* current source name */
  char decpoint;  /* locale decimal point(小数点) */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC void luaX_lookahead (LexState *ls);
LUAI_FUNC void luaX_lexerror (LexState *ls, const char *msg, int token);
LUAI_FUNC void luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
