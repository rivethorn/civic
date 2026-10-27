" indent/civic.vim — Civic indentation
" Part of the Civic vim/neovim plugin
"
" Delegates to Vim's built-in cindent for C-like brace-and-parenthesis
" indentation.  The cinoptions values below produce:
"   - 1 shiftwidth inside braces
"   - statement continuation at 0 (not indented)
"   - else/while matching the if/else keyword
"   - no extra indent for namespace-like constructs

if exists("b:did_indent")
  finish
endif

let b:did_indent = 1

" Use cindent — the most reliable indentation engine for C-like syntax
setlocal cindent

" cinoptions tuned for Civic:
"   >s  — statement after '(' is not shifted further
"   e-s — 'else' aligns with matching 'if'
"   n-s — no extra indent for namespace-like constructs
"   {1s — 1 shiftwidth for brace contents
"   +0  — no continuation line indentation
setlocal cinoptions=>s,e-s,n-s

" Use standard indent keys with cindent
setlocal indentexpr=
setlocal indentkeys=0{,0},0),:,!^F,o,O,0=

" vim: set ft=vim et sw=2 sts=2:
