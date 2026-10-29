" syntax/civic.vim — Civic syntax highlighting
" Part of the Civic vim/neovim plugin.
"
" Matches the header-based module system: `#include <std/io>` directives and
" `#header` markers resolved by the compiler itself (no C preprocessor).
" All groups link to standard Vim highlight groups so they adapt to the
" user's color scheme automatically.

if exists("b:current_syntax")
  finish
endif

" ---------------------------------------------------------------------------
" Module directives
"   #include <std/io>     include a stdlib or local header
"   #header io            mark the current file as a header
" ---------------------------------------------------------------------------
syn match    civicInclude
  \ "^#\s*include\s*<[^>]*>"
syn match    civicHeader
  \ "^#\s*header\%(\s\+\w\+\)\="

" ---------------------------------------------------------------------------
" Keywords — control flow and declarations
" ---------------------------------------------------------------------------
syn keyword  civicKeyword
  \ if
  \ else
  \ while
  \ for
  \ return
  \ break
  \ continue

syn keyword  civicKeyword
  \ class
  \ struct
  \ public
  \ private
  \ template
  \ typename

" ---------------------------------------------------------------------------
" Types — primitive and library
" ---------------------------------------------------------------------------
syn keyword  civicType
  \ void
  \ bool
  \ char
  \ int
  \ float
  \ double
  \ const
  \ String
  \ string
  \ vector
  \ unique_ptr
  \ shared_ptr

" ---------------------------------------------------------------------------
" Constants
" ---------------------------------------------------------------------------
syn keyword  civicConstant
  \ true
  \ false

" ---------------------------------------------------------------------------
" Built-in functions
"   print, println   (from <std/io>)
"   assert           (always available)
"   move             (move semantics, like std::move)
"   make_unique, make_shared  (smart pointer construction)
" ---------------------------------------------------------------------------
syn keyword  civicBuiltIn
  \ print
  \ println
  \ assert
  \ move
  \ make_unique
  \ make_shared

" ---------------------------------------------------------------------------
" Comments — line and block (non-nesting, per spec)
" ---------------------------------------------------------------------------
" TODO / FIXME / NOTE / etc. — only inside comments
syn keyword  civicTodo    TODO FIXME NOTE HACK XXX
            \ contained

" Line comments:  // ...
syn match    civicLineComment  "//.*$"
            \ contains=civicTodo
            \ extend

" Block comments:  /* ... */  (non-nesting)
syn region   civicBlockComment  start="/\*"  end="\*/"
            \ contains=civicTodo
            \ extend

" ---------------------------------------------------------------------------
" String literals
" ---------------------------------------------------------------------------
" Double-quoted strings with Civic escape sequences:
"   \\  \"  \n  \r  \t
syn region   civicString          start='"'  end='"'
            \ skip='\\\\.'
            \ contains=civicStringEscape,civicTodo
            \ extend

" Escape sequences inside strings
syn match    civicStringEscape     "\\[\"ntr\\]"
            \ contained

" ---------------------------------------------------------------------------
" Numeric literals
" ---------------------------------------------------------------------------
" Integers
syn match    civicNumber          "\<\d\+\>"
" Floats (digits on both sides of the decimal point, per spec)
syn match    civicNumber          "\<\d\+\.\d\+\>"

" ---------------------------------------------------------------------------
" Operators
" ---------------------------------------------------------------------------
" Multi-character operators (defined first for regex priority)
syn match    civicOperator
  \ "==\|!=\|<=\|>=\|&&\|||"
syn match    civicOperator
  \ "++\|--\|+=\|-=\|\*=\|/=\|%="
" Single-character operators
syn match    civicOperator  "[+\-*/%&=|!<>]"

" ---------------------------------------------------------------------------
" Delimiters / punctuation
" ---------------------------------------------------------------------------
syn match    civicDelimiter    "[{}()\[\];,]"

" ---------------------------------------------------------------------------
" Highlight group linking
" ---------------------------------------------------------------------------
hi def link civicInclude        PreProc
hi def link civicHeader         PreProc
hi def link civicKeyword        Keyword
hi def link civicType           Type
hi def link civicConstant       Constant
hi def link civicBuiltIn        Function
hi def link civicTodo           Todo
hi def link civicLineComment    Comment
hi def link civicBlockComment   Comment
hi def link civicString         String
hi def link civicStringEscape   SpecialChar
hi def link civicNumber         Number
hi def link civicOperator       Operator
hi def link civicDelimiter      Delimiter

let b:current_syntax = "civic"

" vim: set ft=vim et sw=2 sts=2:
